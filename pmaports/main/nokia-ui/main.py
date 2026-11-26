#!/usr/bin/env python3
"""Nokia 8110 4G Slint KMS/DRM UI driven by nokia-bt-hfp and Linux evdev."""

from __future__ import annotations

import ctypes
from datetime import timedelta
import errno
import fcntl
import glob
import os
import queue
import re
import select
import socket
import struct
import subprocess
import threading
import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib
import slint

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULT_BT_NAME = "THE MTRX HANDSET"
BLUEZ_AGENT_PATH = "/org/nokia/BtAgent"
INACTIVITY_TIMEOUT_SEC = 20
CALL_ENDED_HOLD_MS = 1500
PAIRING_BANNER_MS = 3000
RING_VIBRATE_INTERVAL_MS = 2000

BACKLIGHT_BRIGHTNESS_PATH = "/sys/class/backlight/backlight/brightness"
BACKLIGHT_MAX_PATH = "/sys/class/backlight/backlight/max_brightness"
KBD_LED_PATH = "/sys/class/leds/white:kbd_backlight/brightness"
DEFAULT_BACKLIGHT_LEVEL = 14
VIBRATOR_BY_PATH = (
    "/dev/input/by-path/"
    "platform-200f000.spmi-platform-200f000.spmi:pmic@1:vibrator@c000-event"
)

# Linux input / ioctl definitions (generic asm-generic ABI: ARM + x86_64)
_IOC_WRITE = 1
_IOC_READ = 2


def _ioc(direction: int, ioc_type: int, nr: int, size: int) -> int:
    return (direction << 30) | (size << 16) | (ioc_type << 8) | nr


def _eviocgname(length: int) -> int:
    return _ioc(_IOC_READ, ord("E"), 0x06, length)


def _eviocgsw(length: int) -> int:
    return _ioc(_IOC_READ, ord("E"), 0x1B, length)


# struct input_event uses __kernel_ulong_t sec/usec -> 'L' matches both 32-bit armv7 and 64-bit
INPUT_EVENT_STRUCT = struct.Struct("LLHHi")

EV_KEY = 0x01
EV_SW = 0x05
EV_FF = 0x15

SW_KEYPAD_SLIDE = 0x0A
SW_MAX = 0x10

KEY_ESC = 1
KEY_BACKSPACE = 14
KEY_UP = 103
KEY_DOWN = 108
KEY_POWER = 116  # Intentionally unbound for now (mapping TBD)
KEY_MENU = 139
KEY_F13 = 183
KEY_PICKUP_PHONE = 445
KEY_KBD_LCD_MENU1 = 696
KEY_KBD_LCD_MENU2 = 697

# Map both hwdb-remapped and raw DTS keycodes so physical keys work either way
EVDEV_KEY_MAP = {
    KEY_MENU: "soft_left",
    KEY_KBD_LCD_MENU1: "soft_left",
    KEY_ESC: "soft_right",
    KEY_KBD_LCD_MENU2: "soft_right",
    KEY_F13: "pickup",
    KEY_PICKUP_PHONE: "pickup",
    KEY_BACKSPACE: "hangup",
    KEY_UP: "vol_up",
    KEY_DOWN: "vol_down",
}

EVDEV_TARGET_NAMES = {"keypad", "gpio-keys", "pm8941_resin"}


# Force-feedback structs matching <linux/input.h> ABI on both 32-bit and 64-bit Linux
class _FFTrigger(ctypes.Structure):
    _fields_ = [("button", ctypes.c_uint16), ("interval", ctypes.c_uint16)]


class _FFReplay(ctypes.Structure):
    _fields_ = [("length", ctypes.c_uint16), ("delay", ctypes.c_uint16)]


class _FFEnvelope(ctypes.Structure):
    _fields_ = [
        ("attack_length", ctypes.c_uint16),
        ("attack_level", ctypes.c_uint16),
        ("fade_length", ctypes.c_uint16),
        ("fade_level", ctypes.c_uint16),
    ]


class _FFPeriodicEffect(ctypes.Structure):
    _fields_ = [
        ("waveform", ctypes.c_uint16),
        ("period", ctypes.c_uint16),
        ("magnitude", ctypes.c_int16),
        ("offset", ctypes.c_int16),
        ("phase", ctypes.c_uint16),
        ("envelope", _FFEnvelope),
        ("custom_len", ctypes.c_uint32),
        ("custom_data", ctypes.c_void_p),
    ]


class _FFRumbleEffect(ctypes.Structure):
    _fields_ = [
        ("strong_magnitude", ctypes.c_uint16),
        ("weak_magnitude", ctypes.c_uint16),
    ]


class _FFUnion(ctypes.Union):
    _fields_ = [
        ("periodic", _FFPeriodicEffect),
        ("rumble", _FFRumbleEffect),
    ]


class _FFEffect(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_uint16),
        ("id", ctypes.c_int16),
        ("direction", ctypes.c_uint16),
        ("trigger", _FFTrigger),
        ("replay", _FFReplay),
        ("u", _FFUnion),
    ]


FF_RUMBLE = 0x50
EVIOCSFF = _ioc(_IOC_WRITE, ord("E"), 0x80, ctypes.sizeof(_FFEffect))
EVIOCRMFF = _ioc(_IOC_WRITE, ord("E"), 0x81, ctypes.sizeof(ctypes.c_int))


def _hfp_sock_path() -> str:
    xdg = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(xdg, "nokia-bt-hfp.sock")


def send_hfp_cmd(cmd: str) -> str:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            s.connect(_hfp_sock_path())
            s.sendall((cmd + "\n").encode("utf-8"))
            chunks: list[bytes] = []
            while True:
                data = s.recv(1024)
                if not data:
                    break
                chunks.append(data)
            return b"".join(chunks).decode("utf-8", errors="replace").strip()
    except OSError:
        return ""


def read_battery_percent() -> int | None:
    for path in sorted(glob.glob("/sys/class/power_supply/*/capacity")):
        try:
            with open(path, "r", encoding="utf-8") as f:
                val = int(f.read().strip())
            return max(0, min(100, val))
        except (OSError, ValueError):
            continue
    return None


def _read_evdev_name(fd: int) -> str:
    buf = bytearray(256)
    try:
        fcntl.ioctl(fd, _eviocgname(len(buf)), buf)
        return buf.split(b"\x00", 1)[0].decode("utf-8", errors="replace").strip()
    except OSError:
        return ""


def _read_evdev_switch(fd: int, sw_code: int) -> bool | None:
    nbytes = (SW_MAX // 8) + 2
    buf = bytearray(nbytes)
    try:
        fcntl.ioctl(fd, _eviocgsw(nbytes), buf)
        return bool((buf[sw_code // 8] >> (sw_code % 8)) & 1)
    except OSError:
        return None


class DisplayBacklight:
    """Controls the LCD panel backlight sysfs brightness."""

    def __init__(self) -> None:
        self.available = os.path.exists(BACKLIGHT_BRIGHTNESS_PATH)
        self.active_level = DEFAULT_BACKLIGHT_LEVEL
        self.is_on = True
        if self.available:
            max_val = self._read_int(BACKLIGHT_MAX_PATH)
            cur_val = self._read_int(BACKLIGHT_BRIGHTNESS_PATH)
            if cur_val and cur_val > 0:
                self.active_level = cur_val
            if max_val and self.active_level > max_val:
                self.active_level = max_val

    @staticmethod
    def _read_int(path: str) -> int | None:
        try:
            with open(path, "r", encoding="utf-8") as f:
                return int(f.read().strip())
        except (OSError, ValueError):
            return None

    def set_on(self, on: bool) -> None:
        self.is_on = on
        if not self.available:
            return
        level = self.active_level if on else 0
        try:
            with open(BACKLIGHT_BRIGHTNESS_PATH, "w", encoding="utf-8") as f:
                f.write(f"{level}\n")
        except OSError:
            pass


class VibratorRinger:
    """Drives the PM8909 haptic vibrator during incoming calls."""

    def __init__(self) -> None:
        self.fd: int = -1
        self.effect_id: int = -1
        self._open_device()

    def _open_device(self) -> None:
        candidates: list[str] = []
        if os.path.exists(VIBRATOR_BY_PATH):
            candidates.append(VIBRATOR_BY_PATH)
        else:
            for path in sorted(glob.glob("/dev/input/event*")):
                try:
                    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
                    name = _read_evdev_name(fd).lower()
                    os.close(fd)
                    if "vib" in name:
                        candidates.append(path)
                except OSError:
                    continue

        for path in candidates:
            try:
                fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
            except OSError:
                continue
            eff = _FFEffect()
            eff.type = FF_RUMBLE
            eff.id = -1
            eff.replay.length = 600
            eff.replay.delay = 0
            eff.u.rumble.strong_magnitude = 0x5000
            eff.u.rumble.weak_magnitude = 0x5000
            try:
                fcntl.ioctl(fd, EVIOCSFF, eff)
                self.fd = fd
                self.effect_id = eff.id
                return
            except OSError:
                os.close(fd)

    def pulse(self) -> None:
        if self.fd < 0 or self.effect_id < 0:
            return
        try:
            os.write(self.fd, INPUT_EVENT_STRUCT.pack(0, 0, EV_FF, self.effect_id, 1))
        except OSError:
            pass

    def stop(self) -> None:
        if self.fd < 0 or self.effect_id < 0:
            return
        try:
            os.write(self.fd, INPUT_EVENT_STRUCT.pack(0, 0, EV_FF, self.effect_id, 0))
        except OSError:
            pass


class EvdevInputReader:
    """Reads Nokia 8110 4G physical keys and slider switch directly from evdev."""

    def __init__(self, out_queue: queue.SimpleQueue[tuple[str, object]]) -> None:
        self.out_queue = out_queue
        self.fds: list[int] = []
        self.initial_slider_open: bool | None = None

    def open_devices(self) -> int:
        opened = 0
        for path in sorted(glob.glob("/dev/input/event*")):
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            except OSError:
                continue
            name = _read_evdev_name(fd)
            if name in EVDEV_TARGET_NAMES:
                self.fds.append(fd)
                opened += 1
                if name == "gpio-keys":
                    sw = _read_evdev_switch(fd, SW_KEYPAD_SLIDE)
                    if sw is not None:
                        self.initial_slider_open = sw
            else:
                os.close(fd)
        return opened

    def start(self) -> bool:
        if self.open_devices() == 0:
            return False
        threading.Thread(target=self._loop, daemon=True).start()
        return True

    def _loop(self) -> None:
        ev_size = INPUT_EVENT_STRUCT.size
        while self.fds:
            try:
                ready, _, _ = select.select(self.fds, [], [], 1.0)
            except OSError:
                break
            for fd in ready:
                while True:
                    try:
                        raw = os.read(fd, ev_size * 16)
                    except OSError as exc:
                        if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                            break
                        self.fds.remove(fd)
                        try:
                            os.close(fd)
                        except OSError:
                            pass
                        break
                    if not raw:
                        break
                    for off in range(0, len(raw) - (len(raw) % ev_size), ev_size):
                        _, _, ev_type, ev_code, ev_val = INPUT_EVENT_STRUCT.unpack_from(
                            raw, off
                        )
                        if ev_type == EV_KEY and ev_val == 1:
                            action = EVDEV_KEY_MAP.get(ev_code)
                            if action:
                                self.out_queue.put(("key", action))
                        elif ev_type == EV_SW and ev_code == SW_KEYPAD_SLIDE:
                            self.out_queue.put(("slider", bool(ev_val)))


# Expected argument signatures for org.nokia.BtHfp D-Bus signals
_DBUS_SIGNAL_SPECS: dict[str, tuple[str, ...]] = {
    "CallState": ("bool", "str", "str"),
    "AudioState": ("str", "int", "int", "bool"),
}
_MEMBER_RE = re.compile(r"member=([A-Za-z0-9_]+)")


def set_kbd_backlight(on: bool) -> None:
    try:
        with open(KBD_LED_PATH, "w", encoding="utf-8") as f:
            f.write("1\n" if on else "0\n")
    except OSError:
        pass


class BluezAgent(dbus.service.Object):
    """Auto-accepting NoInputNoOutput BlueZ pairing/authorization agent."""

    def __init__(self, bus: dbus.SystemBus, path: str) -> None:
        super().__init__(bus, path)
        self._bus = bus

    def _trust_device(self, device_path: str) -> None:
        if not device_path:
            return
        try:
            proxy = self._bus.get_object("org.bluez", device_path)
            props = dbus.Interface(proxy, "org.freedesktop.DBus.Properties")
            props.Set("org.bluez.Device1", "Trusted", dbus.Boolean(True))
        except dbus.exceptions.DBusException:
            pass

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self) -> None:
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self) -> None:
        pass

    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, device: str, uuid: str) -> None:
        self._trust_device(str(device))

    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, device: str, passkey: int) -> None:
        self._trust_device(str(device))

    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, device: str) -> None:
        self._trust_device(str(device))


class BluezManager:
    """Hosts org.bluez.Agent1 and manages Adapter1/Device1 pairing on system D-Bus."""

    def __init__(self, out_queue: queue.SimpleQueue[tuple[str, object]]) -> None:
        self.out_queue = out_queue
        self.bus: dbus.SystemBus | None = None
        self.loop: GLib.MainLoop | None = None
        self.agent: BluezAgent | None = None
        self.local_name: str = DEFAULT_BT_NAME
        self.bonded: bool = False
        self.bonded_path: str = ""
        self.bonded_name: str = ""
        self.bonded_addr: str = ""
        self.pairing_active: bool = False

    def start(self) -> None:
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        dbus.mainloop.glib.threads_init()
        self.bus = dbus.SystemBus()
        self.loop = GLib.MainLoop()
        self.agent = BluezAgent(self.bus, BLUEZ_AGENT_PATH)

        self.bus.add_signal_receiver(
            self._on_properties_changed,
            signal_name="PropertiesChanged",
            dbus_interface="org.freedesktop.DBus.Properties",
            bus_name="org.bluez",
            path_keyword="path",
        )
        self.bus.add_signal_receiver(
            self._on_interfaces_removed,
            signal_name="InterfacesRemoved",
            dbus_interface="org.freedesktop.DBus.ObjectManager",
            bus_name="org.bluez",
        )
        self.bus.add_signal_receiver(
            self._on_name_owner_changed,
            signal_name="NameOwnerChanged",
            dbus_interface="org.freedesktop.DBus",
            arg0="org.bluez",
        )

        self._register_agent()
        self._scan_devices()
        self.loop.run()

    def _emit_bond_state(self) -> None:
        self.out_queue.put(
            (
                "BluezBondState",
                (
                    self.bonded,
                    self.bonded_name,
                    self.bonded_addr,
                    self.pairing_active,
                    self.local_name,
                ),
            )
        )

    def _register_agent(self) -> None:
        if self.bus is None:
            return
        try:
            obj = self.bus.get_object("org.bluez", "/org/bluez")
            mgr = dbus.Interface(obj, "org.bluez.AgentManager1")
            mgr.RegisterAgent(BLUEZ_AGENT_PATH, "NoInputNoOutput")
            mgr.RequestDefaultAgent(BLUEZ_AGENT_PATH)
        except dbus.exceptions.DBusException:
            pass

    def _set_adapter_prop(self, prop: str, val: object) -> bool:
        if self.bus is None:
            return False
        try:
            obj = self.bus.get_object("org.bluez", "/org/bluez/hci0")
            props = dbus.Interface(obj, "org.freedesktop.DBus.Properties")
            props.Set("org.bluez.Adapter1", prop, val)
            return True
        except dbus.exceptions.DBusException:
            return False

    def _trust_device(self, dev_path: str) -> None:
        if self.bus is None or not dev_path:
            return
        try:
            obj = self.bus.get_object("org.bluez", dev_path)
            props = dbus.Interface(obj, "org.freedesktop.DBus.Properties")
            props.Set("org.bluez.Device1", "Trusted", dbus.Boolean(True))
        except dbus.exceptions.DBusException:
            pass

    @staticmethod
    def _mac_from_path(dev_path: str) -> str:
        idx = dev_path.find("dev_")
        if idx < 0:
            return ""
        return dev_path[idx + 4 :].replace("_", ":")

    def _fetch_device_info(self, dev_path: str) -> tuple[str, str]:
        addr = self._mac_from_path(dev_path)
        name = ""
        if self.bus is not None:
            try:
                obj = self.bus.get_object("org.bluez", dev_path)
                props = dbus.Interface(obj, "org.freedesktop.DBus.Properties")
                d = props.GetAll("org.bluez.Device1")
                if "Address" in d and str(d["Address"]):
                    addr = str(d["Address"])
                if "Alias" in d and str(d["Alias"]):
                    name = str(d["Alias"])
                elif "Name" in d and str(d["Name"]):
                    name = str(d["Name"])
            except dbus.exceptions.DBusException:
                pass
        return (name or addr, addr)

    def _scan_devices(self) -> None:
        if self.bus is None:
            return
        try:
            obj = self.bus.get_object("org.bluez", "/")
            om = dbus.Interface(obj, "org.freedesktop.DBus.ObjectManager")
            managed = om.GetManagedObjects()
        except dbus.exceptions.DBusException:
            return

        found_bonded = False
        found_path = ""
        found_name = ""
        found_addr = ""

        for path, ifaces in managed.items():
            p = str(path)
            if p == "/org/bluez/hci0":
                aprops = ifaces.get("org.bluez.Adapter1")
                if aprops:
                    if "Alias" in aprops and str(aprops["Alias"]):
                        self.local_name = str(aprops["Alias"])
                    if "Discoverable" in aprops:
                        self.pairing_active = bool(aprops["Discoverable"])
            elif "/org/bluez/hci0/dev_" in p:
                dprops = ifaces.get("org.bluez.Device1")
                if dprops and bool(dprops.get("Paired", False)):
                    if not bool(dprops.get("Trusted", False)):
                        self._trust_device(p)
                    if not found_bonded:
                        found_bonded = True
                        found_path = p
                        found_addr = str(dprops.get("Address", "")) or self._mac_from_path(p)
                        found_name = (
                            str(dprops.get("Alias", ""))
                            or str(dprops.get("Name", ""))
                            or found_addr
                        )

        self.bonded = found_bonded
        self.bonded_path = found_path
        self.bonded_name = found_name
        self.bonded_addr = found_addr
        self._emit_bond_state()

    def _on_name_owner_changed(self, _name: str, _old: str, new_owner: str) -> None:
        if new_owner:
            self._register_agent()
            self._scan_devices()
        else:
            self.pairing_active = False
            self._emit_bond_state()

    def _on_properties_changed(
        self, iface: str, changed: dict, _invalidated: list, path: str = ""
    ) -> None:
        if iface == "org.bluez.Adapter1" and path == "/org/bluez/hci0":
            notify = False
            if "Discoverable" in changed:
                disc = bool(changed["Discoverable"])
                self.pairing_active = disc
                if not disc:
                    self._set_adapter_prop("Pairable", dbus.Boolean(False))
                notify = True
            if "Alias" in changed and str(changed["Alias"]):
                self.local_name = str(changed["Alias"])
                notify = True
            if notify:
                self._emit_bond_state()
        elif iface == "org.bluez.Device1" and "/org/bluez/hci0/dev_" in path:
            if "Paired" in changed:
                if bool(changed["Paired"]):
                    self._trust_device(path)
                    dev_name, dev_addr = self._fetch_device_info(path)
                    self.bonded = True
                    self.bonded_path = path
                    self.bonded_name = dev_name
                    self.bonded_addr = dev_addr
                    self._do_stop_pairing()
                    self.out_queue.put(("BluezPairingSuccess", (dev_name, dev_addr)))
                    self._emit_bond_state()
                else:
                    self._scan_devices()
            elif self.bonded and path == self.bonded_path:
                if "Alias" in changed and str(changed["Alias"]):
                    self.bonded_name = str(changed["Alias"])
                    self._emit_bond_state()
                elif "Name" in changed and str(changed["Name"]):
                    self.bonded_name = str(changed["Name"])
                    self._emit_bond_state()

    def _on_interfaces_removed(self, obj_path: str, _ifaces: list) -> None:
        if "/org/bluez/hci0/dev_" in str(obj_path):
            self._scan_devices()

    def _do_start_pairing(self) -> bool:
        ok = (
            self._set_adapter_prop("Pairable", dbus.Boolean(True))
            and self._set_adapter_prop("Discoverable", dbus.Boolean(True))
        )
        if ok:
            self.pairing_active = True
            self._emit_bond_state()
        else:
            self.pairing_active = False
            self.out_queue.put(("BluezPairFailed", None))
        return False

    def _do_stop_pairing(self) -> bool:
        self._set_adapter_prop("Discoverable", dbus.Boolean(False))
        self._set_adapter_prop("Pairable", dbus.Boolean(False))
        self.pairing_active = False
        self._emit_bond_state()
        return False

    def _do_unpair_all(self) -> bool:
        if self.bus is None:
            return False
        try:
            obj = self.bus.get_object("org.bluez", "/")
            om = dbus.Interface(obj, "org.freedesktop.DBus.ObjectManager")
            managed = om.GetManagedObjects()
            adapter = dbus.Interface(
                self.bus.get_object("org.bluez", "/org/bluez/hci0"),
                "org.bluez.Adapter1",
            )
            for path in list(managed.keys()):
                if "/org/bluez/hci0/dev_" in str(path):
                    try:
                        adapter.RemoveDevice(path)
                    except dbus.exceptions.DBusException:
                        pass
        except dbus.exceptions.DBusException:
            pass
        self.bonded = False
        self.bonded_path = ""
        self.bonded_name = ""
        self.bonded_addr = ""
        self._emit_bond_state()
        return False

    def start_pairing(self) -> None:
        GLib.idle_add(self._do_start_pairing)

    def stop_pairing(self) -> None:
        GLib.idle_add(self._do_stop_pairing)

    def unpair_all(self) -> None:
        GLib.idle_add(self._do_unpair_all)


class DbusMonitorParser:
    """Incremental parser for `dbus-monitor --system` org.nokia.BtHfp signals."""

    def __init__(self) -> None:
        self._member: str | None = None
        self._args: list[object] = []

    def feed_line(self, line: str) -> tuple[str, tuple[object, ...]] | None:
        s = line.strip()
        if s.startswith("signal "):
            m = _MEMBER_RE.search(s)
            self._member = m.group(1) if m and m.group(1) in _DBUS_SIGNAL_SPECS else None
            self._args = []
            return None

        if not self._member:
            return None

        spec = _DBUS_SIGNAL_SPECS[self._member]
        idx = len(self._args)
        if idx >= len(spec):
            self._member = None
            self._args = []
            return None

        expected = spec[idx]
        parsed: object | None = None
        if expected == "bool" and s.startswith("boolean "):
            parsed = s.split(None, 1)[1] == "true"
        elif expected == "int" and (s.startswith("int32 ") or s.startswith("uint32 ")):
            try:
                parsed = int(s.split(None, 1)[1])
            except ValueError:
                parsed = None
        elif expected == "str" and s.startswith("string "):
            q1 = s.find('"')
            q2 = s.rfind('"')
            if q1 >= 0 and q2 > q1:
                parsed = s[q1 + 1 : q2]
            else:
                parsed = ""

        if parsed is None:
            return None

        self._args.append(parsed)
        if len(self._args) == len(spec):
            ev = (self._member, tuple(self._args))
            self._member = None
            self._args = []
            return ev
        return None


class NokiaUi:
    def __init__(self) -> None:
        self.app = slint.load_file(os.path.join(HERE, "main.slint"))
        self.ScreenId = getattr(self.app, "ScreenId", None)
        self.win = self.app.MainWindow()
        self.ev_queue: queue.SimpleQueue[tuple[str, object]] = queue.SimpleQueue()
        self.prev_screen = self._screen_val("home")

        self.backlight = DisplayBacklight()
        self.ringer = VibratorRinger()
        self.evdev = EvdevInputReader(self.ev_queue)
        self.bluez = BluezManager(self.ev_queue)
        self._init_sync_done = False
        self._init_sync_attempts = 0
        self._awaiting_pairing_active = False

        self.call_ended_timer = slint.Timer()
        self.inactivity_timer = slint.Timer()
        self.ring_timer = slint.Timer()
        self.banner_timer = slint.Timer()
        self.init_sync_timer = slint.Timer()
        self.poll_timer = slint.Timer()
        self.battery_timer = slint.Timer()

        self.win.local_bt_name = DEFAULT_BT_NAME
        self.win.soft_left = self.on_soft_left
        self.win.soft_right = self.on_soft_right
        self.win.pickup_pressed = self.on_pickup
        self.win.hangup_pressed = self.on_hangup
        self.win.vol_up = lambda: self.adjust_volume(+7)
        self.win.vol_down = lambda: self.adjust_volume(-7)

    def _screen_val(self, name: str):
        if self.ScreenId is not None:
            return getattr(self.ScreenId, name)
        return name

    def _is_screen(self, name: str) -> bool:
        return self.win.active_screen == self._screen_val(name)

    def set_screen(self, name: str) -> None:
        if not self._is_screen("power_off"):
            self.prev_screen = self.win.active_screen
        self.win.active_screen = self._screen_val(name)

    def _wake_display(self) -> None:
        self.backlight.set_on(True)
        if self._is_screen("sleep"):
            self.set_screen("home")

    def _sleep_display(self) -> None:
        self.inactivity_timer.stop()
        self.backlight.set_on(False)
        if not self._is_screen("active_call") and not self._is_screen("incoming_call"):
            self.set_screen("sleep")

    def _is_active_pairing(self) -> bool:
        return self._is_screen("pairing") and not self.win.pairing_timed_out

    def _schedule_inactivity_sleep(self) -> None:
        if (
            self.win.slider_open
            or self._is_screen("incoming_call")
            or self._is_screen("active_call")
            or self._is_active_pairing()
        ):
            self.inactivity_timer.stop()
            return
        self.inactivity_timer.start(
            slint.TimerMode.SingleShot,
            timedelta(seconds=INACTIVITY_TIMEOUT_SEC),
            self._on_screen_inactivity_timeout,
        )

    def _on_screen_inactivity_timeout(self) -> None:
        if (
            not self.win.slider_open
            and not self._is_screen("incoming_call")
            and not self._is_screen("active_call")
            and not self._is_active_pairing()
        ):
            self._sleep_display()

    def _start_ringer(self) -> None:
        if self.win.ring_silenced:
            return
        self.ringer.pulse()
        self.ring_timer.start(
            slint.TimerMode.Repeated,
            timedelta(milliseconds=RING_VIBRATE_INTERVAL_MS),
            self._on_ring_tick,
        )

    def _on_ring_tick(self) -> None:
        if self._is_screen("incoming_call") and not self.win.ring_silenced:
            self.ringer.pulse()
        else:
            self._stop_ringer()

    def _stop_ringer(self) -> None:
        self.ring_timer.stop()
        self.ringer.stop()

    def _flash_banner(self, text: str) -> None:
        self.win.status_banner = text
        self.banner_timer.start(
            slint.TimerMode.SingleShot,
            timedelta(milliseconds=PAIRING_BANNER_MS),
            self._clear_banner,
        )

    def _clear_banner(self) -> None:
        self.win.status_banner = ""

    def _enter_call_ended(self) -> None:
        self._stop_ringer()
        self.inactivity_timer.stop()
        self._wake_display()
        self.set_screen("call_ended")
        self.call_ended_timer.start(
            slint.TimerMode.SingleShot,
            timedelta(milliseconds=CALL_ENDED_HOLD_MS),
            self._after_call_ended,
        )

    def _after_call_ended(self) -> None:
        if self._is_screen("call_ended"):
            if self.win.slider_open:
                self.set_screen("home")
            else:
                self._sleep_display()

    def _start_pairing(self) -> None:
        self.win.pairing_timed_out = False
        self.set_screen("pairing")
        self._awaiting_pairing_active = True
        self.inactivity_timer.stop()
        self.bluez.start_pairing()

    def _stop_pairing(self) -> None:
        self._awaiting_pairing_active = False
        self.bluez.stop_pairing()
        self._schedule_inactivity_sleep()

    def _on_user_activity(self) -> bool:
        """Wake display if asleep; returns True if the key was consumed solely to wake."""
        was_sleeping = (not self.backlight.is_on) or self._is_screen("sleep")
        if was_sleeping:
            self._wake_display()
        self._schedule_inactivity_sleep()
        return was_sleeping

    def on_soft_left(self) -> None:
        if self._on_user_activity():
            return
        if self._is_screen("home"):
            if self.win.paired_device_name:
                self.set_screen("unpair")
            else:
                self._start_pairing()
        elif self._is_screen("incoming_call"):
            self._stop_ringer()
            send_hfp_cmd("answer")
        elif self._is_screen("active_call"):
            self.win.speaker_mode = not self.win.speaker_mode
            self.win.audio_sink_label = "SPEAKER" if self.win.speaker_mode else "EARPIECE"
            send_hfp_cmd("speaker" if self.win.speaker_mode else "earpiece")
        elif self._is_screen("unpair"):
            self.set_screen("home")
        elif self._is_screen("pairing") and self.win.pairing_timed_out:
            self._start_pairing()
        elif self._is_screen("power_off"):
            subprocess.Popen(["poweroff"])

    def on_soft_right(self) -> None:
        if self._on_user_activity():
            return
        if self._is_screen("home"):
            self.set_screen("power_off")
        elif self._is_screen("incoming_call"):
            self._stop_ringer()
            send_hfp_cmd("hangup")
            self.set_screen("home")
            self._schedule_inactivity_sleep()
        elif self._is_screen("active_call"):
            send_hfp_cmd("hangup")
            self._enter_call_ended()
        elif self._is_screen("unpair"):
            self.bluez.unpair_all()
            self.win.paired_device_name = ""
            self.win.bt_connected = False
            self._start_pairing()
        elif self._is_screen("pairing"):
            self._stop_pairing()
            self.set_screen("home")
        elif self._is_screen("power_off"):
            self.win.active_screen = self.prev_screen

    def on_pickup(self) -> None:
        if self._on_user_activity():
            return
        if self._is_screen("home"):
            send_hfp_cmd("redial")
        elif self._is_screen("incoming_call"):
            self._stop_ringer()
            send_hfp_cmd("answer")

    def on_hangup(self) -> None:
        if self._on_user_activity():
            return
        if self._is_screen("incoming_call"):
            self._stop_ringer()
            send_hfp_cmd("hangup")
            self.set_screen("home")
            self._schedule_inactivity_sleep()
        elif self._is_screen("active_call"):
            send_hfp_cmd("hangup")
            self._enter_call_ended()
        elif (
            self._is_screen("unpair")
            or self._is_screen("pairing")
            or self._is_screen("power_off")
        ):
            if self._is_screen("pairing"):
                self._stop_pairing()
            self.set_screen("home")

    def on_power_short(self) -> None:
        """Hook ready for KEY_POWER short press once power-key behavior is finalized."""
        if self._is_screen("incoming_call"):
            self.win.ring_silenced = True
            self._stop_ringer()
        elif self._is_screen("sleep") or not self.backlight.is_on:
            self._wake_display()
            self._schedule_inactivity_sleep()
        elif self._is_screen("home") or self._is_screen("active_call"):
            self._sleep_display()

    def on_power_long(self) -> None:
        """Hook ready for KEY_POWER long press (>=3s) once power-key behavior is finalized."""
        self._wake_display()
        self.set_screen("power_off")

    def on_slider_changed(self, is_open: bool) -> None:
        self.win.slider_open = is_open
        set_kbd_backlight(is_open)
        if is_open:
            self.inactivity_timer.stop()
            self._wake_display()
            if self._is_screen("incoming_call"):
                self._stop_ringer()
                send_hfp_cmd("answer")
        else:
            if self._is_screen("active_call"):
                send_hfp_cmd("hangup")
                self._enter_call_ended()
            self._schedule_inactivity_sleep()

    def adjust_volume(self, delta: int) -> None:
        if self._is_screen("active_call"):
            if not self.backlight.is_on:
                self._wake_display()
            pct = max(0, min(100, self.win.volume_percent + delta))
            self.win.volume_percent = pct
            vgs = round(pct * 15 / 100)
            send_hfp_cmd(f"vgs {vgs}")

    def _apply_audio_state(self, output: str, vgs: int) -> None:
        out_lower = output.lower()
        self.win.speaker_mode = out_lower == "speaker"
        if out_lower == "headphones":
            self.win.audio_sink_label = "HEADPHONES"
        elif out_lower == "speaker":
            self.win.audio_sink_label = "SPEAKER"
        else:
            self.win.audio_sink_label = "EARPIECE"
        vgs_clamped = max(0, min(15, vgs))
        self.win.volume_percent = round(vgs_clamped * 100 / 15)

    def read_initial_status(self) -> bool:
        data = send_hfp_cmd("status")
        if not data:
            return False
        kv = dict(item.split("=", 1) for item in data.split() if "=" in item)
        connected = kv.get("connected") == "1"
        call = kv.get("call") == "1"
        callsetup = kv.get("callsetup", "0")
        caller = kv.get("caller", "-")
        vgs = 12
        if "vgs" in kv:
            try:
                vgs = int(kv["vgs"])
            except ValueError:
                pass
        self._apply_audio_state(kv.get("output", "earpiece"), vgs)

        state = "active" if call else ("incoming" if callsetup == "1" else "idle")
        self.ev_queue.put(("CallState", (connected, state, "" if caller == "-" else caller)))
        self._init_sync_done = True
        return True

    def _retry_initial_status(self) -> None:
        self._init_sync_attempts += 1
        if self.read_initial_status() or self._init_sync_attempts >= 20:
            self.init_sync_timer.stop()

    def _dbus_listener(self) -> None:
        try:
            proc = subprocess.Popen(
                [
                    "dbus-monitor",
                    "--system",
                    "type='signal',interface='org.nokia.BtHfp'",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            )
        except OSError:
            return
        if not proc.stdout:
            return
        parser = DbusMonitorParser()
        for line in proc.stdout:
            ev = parser.feed_line(line)
            if ev is not None:
                self.ev_queue.put(ev)

    def _handle_call_state(self, connected: bool, state: str, caller: str) -> None:
        self.win.bt_connected = connected
        self.win.caller_id = caller
        if state == "incoming":
            if self._is_screen("pairing"):
                self._stop_pairing()
            was_incoming = self._is_screen("incoming_call")
            self.inactivity_timer.stop()
            self._wake_display()
            if not was_incoming:
                self.win.ring_silenced = False
                self.set_screen("incoming_call")
                self._start_ringer()
        elif state == "active":
            if self._is_screen("pairing"):
                self._stop_pairing()
            self._stop_ringer()
            self.inactivity_timer.stop()
            self._wake_display()
            self.set_screen("active_call")
        elif state == "idle":
            self._stop_ringer()
            if self._is_screen("active_call"):
                self._enter_call_ended()
            elif self._is_screen("incoming_call"):
                self.set_screen("home")
                self._schedule_inactivity_sleep()

    def _handle_bond_state(
        self, bonded: bool, dev_name: str, dev_addr: str, pairing_active: bool
    ) -> None:
        if bonded:
            self.win.paired_device_name = dev_name or dev_addr
            if self._is_screen("pairing"):
                self._awaiting_pairing_active = False
                self.win.pairing_timed_out = False
                self.set_screen("home")
                self._schedule_inactivity_sleep()
        else:
            self.win.paired_device_name = ""
            self.win.bt_connected = False
            if self._is_screen("unpair") and not pairing_active:
                self.set_screen("home")
                self._schedule_inactivity_sleep()

        if pairing_active:
            self._awaiting_pairing_active = False
            self.win.pairing_timed_out = False
            if not self._is_screen("pairing"):
                self.set_screen("pairing")
            self._wake_display()
            self.inactivity_timer.stop()
        elif (
            self._is_screen("pairing")
            and not bonded
            and not self._awaiting_pairing_active
        ):
            self.win.pairing_timed_out = True
            self._schedule_inactivity_sleep()

    def _drain_events(self) -> None:
        while not self.ev_queue.empty():
            kind, payload = self.ev_queue.get_nowait()
            if kind == "key":
                if payload == "soft_left":
                    self.on_soft_left()
                elif payload == "soft_right":
                    self.on_soft_right()
                elif payload == "pickup":
                    self.on_pickup()
                elif payload == "hangup":
                    self.on_hangup()
                elif payload == "vol_up":
                    self.adjust_volume(+7)
                elif payload == "vol_down":
                    self.adjust_volume(-7)
            elif kind == "slider":
                is_open = payload[0] if isinstance(payload, tuple) else bool(payload)
                self.on_slider_changed(bool(is_open))
            elif kind == "CallState":
                connected, state, caller = payload  # type: ignore[misc]
                self._handle_call_state(bool(connected), str(state), str(caller))
            elif kind == "AudioState":
                output, vgs, _vgm, _muted = payload  # type: ignore[misc]
                self._apply_audio_state(str(output), int(vgs))
            elif kind == "BluezBondState":
                bonded, dev_name, dev_addr, pairing_active, local_name = payload  # type: ignore[misc]
                if local_name:
                    self.win.local_bt_name = str(local_name)
                self._handle_bond_state(
                    bool(bonded),
                    str(dev_name),
                    str(dev_addr),
                    bool(pairing_active),
                )
            elif kind == "BluezPairingSuccess":
                dev_name, dev_addr = payload  # type: ignore[misc]
                label = str(dev_name) or str(dev_addr)
                self._awaiting_pairing_active = False
                self.win.pairing_timed_out = False
                self.win.paired_device_name = label
                self._wake_display()
                self.set_screen("home")
                self._flash_banner(f"PAIRED: {label}")
                self._schedule_inactivity_sleep()
            elif kind == "BluezPairFailed":
                self._awaiting_pairing_active = False
                if self._is_screen("pairing"):
                    self.win.pairing_timed_out = True
                    self._schedule_inactivity_sleep()

    def _refresh_battery(self) -> None:
        pct = read_battery_percent()
        if pct is not None:
            self.win.battery_percent = pct

    def run(self) -> None:
        self._refresh_battery()
        if self.evdev.start():
            # Hardware evdev owns all physical keys; prevent double triggers via Slint FocusScope
            self.win.use_slint_keys = False
            if self.evdev.initial_slider_open is not None:
                self.win.slider_open = self.evdev.initial_slider_open
        set_kbd_backlight(bool(self.win.slider_open))
        if not self.read_initial_status():
            self.init_sync_timer.start(
                slint.TimerMode.Repeated,
                timedelta(milliseconds=500),
                self._retry_initial_status,
            )
        self._schedule_inactivity_sleep()
        self.bluez.start()
        threading.Thread(target=self._dbus_listener, daemon=True).start()
        self.poll_timer.start(
            slint.TimerMode.Repeated, timedelta(milliseconds=50), self._drain_events
        )
        self.battery_timer.start(
            slint.TimerMode.Repeated, timedelta(seconds=30), self._refresh_battery
        )
        self._drain_events()
        self.win.run()


if __name__ == "__main__":
    NokiaUi().run()
