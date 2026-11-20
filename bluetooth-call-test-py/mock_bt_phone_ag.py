#!/usr/bin/env python3
"""
Mock Mobile Phone (Bluetooth HFP-AG + Device Manager + Battery + AVRCP Media Listener)
with both an interactive CLI (stdin) and a GTK3 GUI shell.

Default target: nokia-argon (02:00:34:40:12:1E) or pass --mac XX:XX:XX:XX:XX:XX
Pass --no-gui to run in terminal-only mode.

Interactive CLI commands (work simultaneously with the GUI):
  status                 - Show connection, call, volume, and battery status
  scan on|off            - Start/stop Bluetooth discovery
  pair [MAC]             - Pair and trust headset
  unpair [MAC]           - Remove (unpair) headset
  connect [MAC]          - Connect to headset (HFP + A2DP/AVRCP)
  disconnect [MAC]       - Disconnect headset
  battery [MAC]          - Query headset battery (BlueZ Battery1 + HFP AT cache)
  ring [number]          - Mock incoming call (+CIEV callsetup=1, RING, +CLIP)
  answer                 - Answer call from phone side (+CIEV call=1, callsetup=0, open SCO)
  hangup                 - End/cancel call from phone side (+CIEV call=0, callsetup=0, close SCO)
  sco on|off             - Manually open or close BTPROTO_SCO voice link
  vgs <0-15>             - Push speaker volume (+VGS) to headset
  vgm <0-15>             - Push mic volume (+VGM) to headset
  quit                   - Exit
"""

import argparse
import ctypes
import ctypes.util
import datetime
import fcntl
import glob
import math
import os
import socket
import struct
import subprocess
import sys
import time
import wave
import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib

HFP_AG_UUID = "0000111f-0000-1000-8000-00805f9b34fb"
HFP_HF_UUID = "0000111e-0000-1000-8000-00805f9b34fb"
HSP_AG_UUID = "00001112-0000-1000-8000-00805f9b34fb"
HSP_HS_UUID = "00001108-0000-1000-8000-00805f9b34fb"
PROFILE_PATH = "/org/mockphone/HfpAgProfile"
PROFILE_HSP_PATH = "/org/mockphone/HspAgProfile"

AF_BLUETOOTH = 31
SOCK_SEQPACKET = 5
BTPROTO_SCO = 2
BTPROTO_RFCOMM = 3
_LIBC = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)

PROFILE_XML = """
<node>
  <interface name='org.bluez.Profile1'>
    <method name='Release'/>
    <method name='NewConnection'>
      <arg type='o' name='device' direction='in'/>
      <arg type='h' name='fd' direction='in'/>
      <arg type='a{sv}' name='fd_properties' direction='in'/>
    </method>
    <method name='RequestDisconnection'>
      <arg type='o' name='device' direction='in'/>
    </method>
  </interface>
  <interface name='org.bluez.Agent1'>
    <method name='Release'/>
    <method name='AuthorizeService'>
      <arg type='o' name='device' direction='in'/>
      <arg type='s' name='uuid' direction='in'/>
    </method>
    <method name='RequestConfirmation'>
      <arg type='o' name='device' direction='in'/>
      <arg type='u' name='passkey' direction='in'/>
    </method>
    <method name='RequestAuthorization'>
      <arg type='o' name='device' direction='in'/>
    </method>
    <method name='Cancel'/>
  </interface>
</node>
"""

CIND_DEF = (
    '+CIND: ("service",(0,1)),("call",(0,1)),("callsetup",(0,3)),'
    '("callheld",(0,2)),("signal",(0,5)),("roam",(0,1)),("battchg",(0,5))'
)

# Linux input event codes for AVRCP / headset buttons
KEY_NAMES = {
    113: "KEY_MUTE",
    114: "KEY_VOLUMEDOWN",
    115: "KEY_VOLUMEUP",
    163: "KEY_NEXTSONG",
    164: "KEY_PLAYPAUSE",
    165: "KEY_PREVIOUSSONG",
    166: "KEY_STOPCD",
    168: "KEY_REWIND",
    200: "KEY_PLAYCD",
    201: "KEY_PAUSECD",
    207: "KEY_PLAY",
    208: "KEY_FASTFORWARD",
    226: "KEY_MEDIA",
    445: "KEY_PICKUP_PHONE",
    446: "KEY_HANGUP_PHONE",
}


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_RINGTONE_FILE = os.path.join(SCRIPT_DIR, "ringtone.wav")
DEFAULT_VOICE_FILE = os.path.join(SCRIPT_DIR, "voice.wav")


def load_pcm_8k(path: str) -> bytes:
    """Load an audio file into 8000 Hz mono S16_LE PCM bytes (native WAV or via ffmpeg)."""
    if path.lower().endswith(".wav"):
        try:
            with wave.open(path, "rb") as w:
                if w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == 8000:
                    return w.readframes(w.getnframes())
        except Exception:
            pass
    res = subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            path,
            "-f",
            "s16le",
            "-acodec",
            "pcm_s16le",
            "-ac",
            "1",
            "-ar",
            "8000",
            "-",
        ],
        capture_output=True,
    )
    if res.returncode == 0 and res.stdout:
        return res.stdout
    raise RuntimeError(f"Could not decode '{path}' as 8kHz mono S16_LE PCM")


class MockPhoneAG:
    def __init__(
        self,
        default_mac: str,
        register_ag: bool = True,
        ringtone_path: str = DEFAULT_RINGTONE_FILE,
        voice_path: str = DEFAULT_VOICE_FILE,
    ):
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        self.loop = GLib.MainLoop()
        self.default_mac = default_mac.upper()
        self.register_ag = register_ag

        # UI callbacks
        self.on_state_changed = None
        self.on_log_message = None

        # HFP state
        self.rfcomm_fd = -1
        self.rfcomm_watch = 0
        self.sco_fd = -1
        self.sco_watch = 0
        self.peer_mac = ""
        self.dev_path = ""
        self.rx_buf = b""
        self.cmer_enabled = False
        self.clip_enabled = False
        self.bia_enabled = True
        self.scanning = False
        self.wp_paused_by_us = False

        # Call indicators: service=1, call=0, callsetup=0, callheld=0, signal=5, roam=0, battchg=5
        self.call = 0
        self.callsetup = 0
        self.callheld = 0
        self.caller_number = "+15550199"
        self.ring_timer = 0
        self.last_event = "Idle"

        # Audio / battery levels
        self.vgs = 12
        self.vgm = 12
        self.headset_battery = None  # 0..100%
        self.dbus_battery = None

        # In-band ringtone, partner voice playback, and phone mic recording
        self.ringtone_path = ringtone_path or DEFAULT_RINGTONE_FILE
        self.voice_path = voice_path or DEFAULT_VOICE_FILE
        self.ringtone_pcm = b""
        self.voice_pcm = b""
        self.ring_pos = 0
        self.voice_pos = 0
        self.rec_wav = None
        self.rec_path = ""
        self.rec_bytes = 0
        self.last_rec_path = ""
        self.set_ringtone_file(self.ringtone_path)
        self.set_voice_file(self.voice_path)

        # AVRCP uinput watchers: path -> (fd, watch_id, name)
        self.evdev_watchers = {}

        try:
            self.bus.call_sync(
                "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                "StopDiscovery",
                None,
                None,
                Gio.DBusCallFlags.NONE,
                2000,
                None,
            )
            self.scanning = False
        except Exception:
            pass

        if self.register_ag:
            self._register_profile()
        self._register_agent()
        self._subscribe_dbus_signals()
        self._scan_avrcp_inputs()
        GLib.timeout_add_seconds(3, self._scan_avrcp_inputs)

    def log(self, msg: str):
        print(msg, flush=True)
        if self.on_log_message:
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            self.on_log_message(f"[{ts}] {msg.strip()}")

    def _notify_ui(self, event_summary: str = ""):
        if event_summary:
            self.last_event = event_summary
        if self.on_state_changed:
            self.on_state_changed()

    def _pause_wireplumber(self):
        res = subprocess.run(
            ["systemctl", "--user", "is-active", "wireplumber.service"],
            capture_output=True,
            text=True,
        )
        if res.returncode == 0 and res.stdout.strip() == "active":
            self.log("[AG] Stopping host wireplumber.service so it releases HFP-AG...")
            subprocess.run(["systemctl", "--user", "stop", "wireplumber.service"])
            self.wp_paused_by_us = True

    def _restore_wireplumber(self):
        if self.wp_paused_by_us:
            self.log("[AG] Restarting host wireplumber.service...")
            subprocess.run(["systemctl", "--user", "start", "wireplumber.service"])
            self.wp_paused_by_us = False

    # -------------------- BlueZ D-Bus Helpers --------------------

    def _mac_to_path(self, mac: str = "") -> str:
        mac = (mac or self.peer_mac or self.default_mac).strip().upper()
        return f"/org/bluez/hci0/dev_{mac.replace(':', '_')}"

    def _get_adapter_mac_str(self) -> str:
        res = self.bus.call_sync(
            "org.bluez",
            "/org/bluez/hci0",
            "org.freedesktop.DBus.Properties",
            "Get",
            GLib.Variant("(ss)", ("org.bluez.Adapter1", "Address")),
            GLib.VariantType("(v)"),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        )
        return res.unpack()[0].upper()

    def get_device_props(self, mac: str = "") -> dict:
        path = self._mac_to_path(mac)
        try:
            res = self.bus.call_sync(
                "org.bluez",
                path,
                "org.freedesktop.DBus.Properties",
                "GetAll",
                GLib.Variant("(s)", ("org.bluez.Device1",)),
                GLib.VariantType("(a{sv})"),
                Gio.DBusCallFlags.NONE,
                1500,
                None,
            )
            return res.unpack()[0]
        except Exception:
            return {}

    def _register_profile(self):
        node_info = Gio.DBusNodeInfo.new_for_xml(PROFILE_XML)
        for path in (PROFILE_PATH, PROFILE_HSP_PATH):
            self.bus.register_object(
                path,
                node_info.interfaces[0],
                self._on_profile_method,
                None,
                None,
            )
        profiles = [
            (
                PROFILE_PATH,
                HFP_AG_UUID,
                {
                    "Name": GLib.Variant("s", "Hands-Free Audio Gateway"),
                    "AutoConnect": GLib.Variant("b", True),
                    "Version": GLib.Variant("q", 0x0107),
                    "Features": GLib.Variant("q", 0x03EF),
                },
            ),
            (
                PROFILE_HSP_PATH,
                HSP_AG_UUID,
                {
                    "Name": GLib.Variant("s", "Headset Audio Gateway"),
                    "AutoConnect": GLib.Variant("b", True),
                },
            ),
        ]
        for attempt in range(2):
            try:
                for p_path, uuid, opts in profiles:
                    self.bus.call_sync(
                        "org.bluez",
                        "/org/bluez",
                        "org.bluez.ProfileManager1",
                        "RegisterProfile",
                        GLib.Variant("(osa{sv})", (p_path, uuid, opts)),
                        None,
                        Gio.DBusCallFlags.NONE,
                        5000,
                        None,
                    )
                self.log(f"[AG] Registered HFP-AG ({HFP_AG_UUID}) & HSP-AG ({HSP_AG_UUID})")
                return
            except Exception as e:
                if attempt == 0 and ("AlreadyExists" in str(e) or "NotPermitted" in str(e)):
                    self.log("[AG] WirePlumber is holding HFP-AG/HSP-AG; pausing wireplumber.service...")
                    self._pause_wireplumber()
                    continue
                self.log(
                    f"[AG] WARNING: RegisterProfile failed ({e}). "
                    "If WirePlumber owns HFP-AG, stop wireplumber or use --coexist."
                )

    def _register_agent(self):
        node_info = Gio.DBusNodeInfo.new_for_xml(PROFILE_XML)
        self.bus.register_object(
            PROFILE_PATH,
            node_info.interfaces[1],
            self._on_agent_method,
            None,
            None,
        )
        try:
            self.bus.call_sync(
                "org.bluez",
                "/org/bluez",
                "org.bluez.AgentManager1",
                "RegisterAgent",
                GLib.Variant("(os)", (PROFILE_PATH, "NoInputNoOutput")),
                None,
                Gio.DBusCallFlags.NONE,
                3000,
                None,
            )
            self.bus.call_sync(
                "org.bluez",
                "/org/bluez",
                "org.bluez.AgentManager1",
                "RequestDefaultAgent",
                GLib.Variant("(o)", (PROFILE_PATH,)),
                None,
                Gio.DBusCallFlags.NONE,
                3000,
                None,
            )
            self.log("[AG] Registered NoInputNoOutput pairing agent on host")
        except Exception:
            pass

    def _on_agent_method(
        self, _conn, _sender, _path, _iface, method, params, invocation
    ):
        self.log(f"[AG] Agent1.{method} auto-accepted: {params.unpack()}")
        invocation.return_value(None)

    def _subscribe_dbus_signals(self):
        self.bus.signal_subscribe(
            "org.bluez",
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            None,
            None,
            Gio.DBusSignalFlags.NONE,
            self._on_properties_changed,
        )

    def _on_properties_changed(
        self, _conn, _sender, path, _iface, _signal, params
    ):
        iface_name, changed, _invalid = params.unpack()
        if iface_name == "org.bluez.Battery1" and "Percentage" in changed:
            self.dbus_battery = int(changed["Percentage"])
            self.headset_battery = self.dbus_battery
            self.log(f"[EVENT] Headset Battery1 update ({path}): {self.headset_battery}%")
            self._notify_ui(f"Battery: {self.headset_battery}%")
        elif iface_name in ("org.bluez.MediaControl1", "org.bluez.MediaPlayer1"):
            self.log(f"[EVENT] Media property change ({iface_name}): {changed}")
            self._notify_ui(f"Media: {changed}")
        elif iface_name == "org.bluez.Device1":
            for k in ("Connected", "Paired", "Trusted", "RSSI"):
                if k in changed:
                    self.log(f"[EVENT] Device {path} {k} = {changed[k]}")
            self._notify_ui()

    # -------------------- HFP-AG Profile & AT Engine --------------------

    def _attach_rfcomm_fd(self, fd: int, mac: str, dev_path: str = ""):
        self._close_rfcomm()
        self.rfcomm_fd = fd
        self.peer_mac = mac.upper()
        self.default_mac = self.peer_mac
        self.dev_path = dev_path or self._mac_to_path(self.peer_mac)

        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

        self.rfcomm_watch = GLib.unix_fd_add_full(
            GLib.PRIORITY_DEFAULT,
            fd,
            GLib.IOCondition.IN | GLib.IOCondition.HUP | GLib.IOCondition.ERR,
            self._on_rfcomm_io,
        )
        self.log(f"[EVENT] Headset connected to HFP-AG: {self.peer_mac} (fd={fd})")
        self._notify_ui(f"HFP RFCOMM connected ({self.peer_mac})")

    def _on_profile_method(
        self, _conn, _sender, _path, _iface, method, params, invocation
    ):
        if method == "Release":
            self.log("[AG] Profile released by BlueZ")
            self._close_rfcomm()
            invocation.return_value(None)
        elif method == "RequestDisconnection":
            (dev_path,) = params.unpack()
            self.log(f"[AG] RequestDisconnection({dev_path})")
            self._close_rfcomm()
            invocation.return_value(None)
        elif method == "NewConnection":
            msg = invocation.get_message()
            fd_list = msg.get_unix_fd_list()
            dev_path, fd_idx, _props = params.unpack()
            fd = fd_list.get(fd_idx)
            mac = dev_path.split("dev_")[-1].replace("_", ":")
            self._attach_rfcomm_fd(fd, mac, dev_path)
            invocation.return_value(None)

    def _send_at(self, line: str):
        if self.rfcomm_fd < 0:
            return
        wire = f"\r\n{line}\r\n".encode("utf-8")
        try:
            os.write(self.rfcomm_fd, wire)
            self.log(f"  [AG TX] {line}")
        except OSError as e:
            self.log(f"  [AG TX ERR] {e}")

    def _send_ciev(self, index: int, value: int):
        if self.cmer_enabled and self.bia_enabled:
            self._send_at(f"+CIEV: {index},{value}")

    def _on_rfcomm_io(self, fd, cond):
        if cond & (GLib.IOCondition.HUP | GLib.IOCondition.ERR):
            self.log(f"[EVENT] Headset {self.peer_mac} HFP RFCOMM disconnected")
            self._close_rfcomm()
            return False

        try:
            chunk = os.read(fd, 1024)
        except BlockingIOError:
            return True
        except OSError:
            self._close_rfcomm()
            return False

        if not chunk:
            self.log(f"[EVENT] Headset {self.peer_mac} closed RFCOMM")
            self._close_rfcomm()
            return False

        self.rx_buf += chunk
        while b"\r" in self.rx_buf or b"\n" in self.rx_buf:
            idx_r = self.rx_buf.find(b"\r")
            idx_n = self.rx_buf.find(b"\n")
            candidates = [i for i in (idx_r, idx_n) if i >= 0]
            cut = min(candidates)
            line = self.rx_buf[:cut].decode("utf-8", errors="replace").strip()
            self.rx_buf = self.rx_buf[cut + 1 :]
            if line:
                self._handle_at_cmd(line)
        return True

    def _handle_at_cmd(self, cmd: str):
        self.log(f"  [HF RX] {cmd}")
        u = cmd.upper()

        if u.startswith("AT+BRSF="):
            hf_feat = int(u.split("=", 1)[1] or "0")
            self.log(f"  [AG] Headset features = 0x{hf_feat:x}")
            self._send_at("+BRSF: 1007")
            self._send_at("OK")
        elif u.startswith("AT+BAC="):
            self._send_at("OK")
        elif u == "AT+CIND=?":
            self._send_at(CIND_DEF)
            self._send_at("OK")
        elif u == "AT+CIND?":
            self._send_at(
                f"+CIND: 1,{self.call},{self.callsetup},{self.callheld},5,0,5"
            )
            self._send_at("OK")
        elif u.startswith("AT+CMER="):
            self.cmer_enabled = True
            self._send_at("OK")
            self.log(f"[EVENT] HFP Service Level Connection (SLC) ready with {self.peer_mac}")
            self._notify_ui("HFP SLC Ready")
        elif u == "AT+CHLD=?":
            self._send_at("+CHLD: (0,1,2,3)")
            self._send_at("OK")
        elif u.startswith("AT+CLIP="):
            self.clip_enabled = u.endswith("1")
            self._send_at("OK")
        elif u.startswith("AT+CMEE="):
            self._send_at("OK")
        elif u.startswith("AT+BIA="):
            self._send_at("OK")
        elif u.startswith("AT+BIND="):
            if "=?" in u:
                self._send_at("+BIND: (1,2)")
            elif u.endswith("?"):
                self._send_at("+BIND: 1,1")
                self._send_at("+BIND: 2,1")
            self._send_at("OK")
        elif u.startswith("AT+BIEV="):
            parts = u.split("=", 1)[1].split(",")
            if len(parts) == 2 and parts[0].strip() == "2":
                self.headset_battery = int(parts[1].strip())
                self.log(f"[EVENT] Headset battery (AT+BIEV): {self.headset_battery}%")
                self._notify_ui(f"Battery (BIEV): {self.headset_battery}%")
            self._send_at("OK")
        elif u.startswith("AT+XAPL="):
            self._send_at("+XAPL=iPhone,7")
            self._send_at("OK")
        elif u.startswith("AT+IPHONEACCEV="):
            tokens = [t.strip() for t in u.split("=", 1)[1].split(",")]
            if len(tokens) >= 3:
                pairs = tokens[1:]
                for i in range(0, len(pairs) - 1, 2):
                    if pairs[i] == "1":
                        self.headset_battery = (int(pairs[i + 1]) + 1) * 10
                        self.log(
                            f"[EVENT] Headset battery (AT+IPHONEACCEV): {self.headset_battery}%"
                        )
                        self._notify_ui(f"Battery (Apple): {self.headset_battery}%")
            self._send_at("OK")
        elif u.startswith("AT+VGS="):
            self.vgs = int(u.split("=", 1)[1])
            self.log(f"[EVENT] Headset changed Speaker Volume (VGS) -> {self.vgs}/15")
            self._send_at("OK")
            self._notify_ui(f"Headset VGS -> {self.vgs}/15")
        elif u.startswith("AT+VGM="):
            self.vgm = int(u.split("=", 1)[1])
            self.log(f"[EVENT] Headset changed Mic Gain (VGM) -> {self.vgm}/15")
            self._send_at("OK")
            self._notify_ui(f"Headset VGM -> {self.vgm}/15")
        elif u.startswith("AT+BCS="):
            self._send_at("OK")
            self.open_sco()
        elif u == "AT+BCC":
            self._send_at("OK")
            self.open_sco()
        elif u == "ATA":
            self.log("[EVENT] >>> HEADSET PICKED UP CALL (ATA) <<<")
            self._send_at("OK")
            self._notify_ui("Headset Answered Call (ATA)")
            self.answer_call()
        elif u == "AT+CHUP":
            self.log("[EVENT] >>> HEADSET HUNG UP / REJECTED CALL (AT+CHUP) <<<")
            self._send_at("OK")
            self._notify_ui("Headset Hung Up (AT+CHUP)")
            self.hangup_call()
        elif u.startswith("ATD"):
            dialed = cmd[3:].rstrip(";")
            self.log(f"[EVENT] >>> HEADSET DIALED NUMBER: {dialed} <<<")
            self._send_at("OK")
            self.caller_number = dialed
            self._send_ciev(3, 2)
            self.callsetup = 2
            self._notify_ui(f"Headset Dialed {dialed}")
            self.open_sco()
            GLib.timeout_add_seconds(2, self._complete_outgoing_call)
        elif u == "AT+BLDN":
            self.log(f"[EVENT] >>> HEADSET REDIALED LAST NUMBER ({self.caller_number}) <<<")
            self._send_at("OK")
            self._send_ciev(3, 2)
            self.callsetup = 2
            self._notify_ui(f"Headset Redialed {self.caller_number}")
            self.open_sco()
            GLib.timeout_add_seconds(2, self._complete_outgoing_call)
        elif u.startswith("AT+CLCC"):
            if self.call == 1:
                self._send_at(f'+CLCC: 1,1,0,0,0,"{self.caller_number}",129')
            elif self.callsetup == 1:
                self._send_at(f'+CLCC: 1,1,4,0,0,"{self.caller_number}",129')
            self._send_at("OK")
        elif u.startswith("AT+COPS"):
            self._send_at('+COPS: 0,0,"MockPhone"')
            self._send_at("OK")
        elif u == "AT+CNUM":
            self._send_at('+CNUM: ,"15550100",129,7,4')
            self._send_at("OK")
        elif u.startswith("AT+VTS="):
            dtmf = cmd.split("=", 1)[1]
            self.log(f"[EVENT] Headset sent DTMF tone: {dtmf}")
            self._send_at("OK")
            self._notify_ui(f"DTMF: {dtmf}")
        elif u.startswith("AT+NREC="):
            self.log(f"[EVENT] Headset requested EC/NR off: {cmd}")
            self._send_at("OK")
        elif u.startswith("AT+BVRA="):
            self.log(f"[EVENT] Headset toggled Voice Assistant (BVRA): {cmd}")
            self._send_at("OK")
            self._notify_ui(f"Voice Assistant: {cmd}")
        elif u == "AT+CKPD=200":
            # HSP Headset button press (pickup / hangup toggle)
            self.log("[EVENT] >>> HEADSET BUTTON PRESSED (AT+CKPD=200) <<<")
            self._send_at("OK")
            if self.callsetup == 1:
                self._notify_ui("Headset Answered Call (CKPD)")
                self.answer_call()
            elif self.call == 1:
                self._notify_ui("Headset Hung Up (CKPD)")
                self.hangup_call()
            else:
                self._notify_ui("Headset Button Pressed (CKPD=200)")
        else:
            self._send_at("OK")

    def _complete_outgoing_call(self):
        if self.callsetup == 2:
            self.call = 1
            self.callsetup = 0
            self._send_ciev(2, 1)
            self._send_ciev(3, 0)
            self.log("[AG] Outgoing call connected (call=1)")
            self._notify_ui(f"In Call ({self.caller_number})")
        return False

    # -------------------- Call Control & Ringing --------------------

    def set_ringtone_file(self, path: str):
        self.ringtone_path = path
        if not os.path.exists(path):
            self.ringtone_pcm = b""
            self.log(f"[AUDIO] Ringtone file not found: {path} (add 8kHz mono 16-bit WAV)")
            return
        try:
            self.ringtone_pcm = load_pcm_8k(path)
            self.ring_pos = 0
            self.log(f"[AUDIO] Loaded ringtone '{path}' ({len(self.ringtone_pcm)/16000:.1f}s @ 8kHz)")
        except Exception as e:
            self.ringtone_pcm = b""
            self.log(f"[AUDIO] Failed to load ringtone '{path}': {e}")

    def set_voice_file(self, path: str):
        self.voice_path = path
        if not os.path.exists(path):
            self.voice_pcm = b""
            self.log(f"[AUDIO] Partner voice file not found: {path} (add 8kHz mono 16-bit WAV)")
            return
        try:
            self.voice_pcm = load_pcm_8k(path)
            self.voice_pos = 0
            self.log(f"[AUDIO] Loaded partner voice '{path}' ({len(self.voice_pcm)/16000:.1f}s @ 8kHz)")
        except Exception as e:
            self.voice_pcm = b""
            self.log(f"[AUDIO] Failed to load voice '{path}': {e}")

    def _start_recording(self):
        self._stop_recording()
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        self.rec_path = f"/tmp/mock_phone_rec_{ts}.wav"
        try:
            w = wave.open(self.rec_path, "wb")
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(8000)
            self.rec_wav = w
            self.rec_bytes = 0
            self.log(f"[REC] Recording phone mic response -> {self.rec_path}")
        except Exception as e:
            self.log(f"[REC] Failed to open {self.rec_path}: {e}")
            self.rec_wav = None

    def _stop_recording(self):
        if self.rec_wav is not None:
            try:
                self.rec_wav.close()
            except Exception:
                pass
            self.rec_wav = None
            dur = self.rec_bytes / 16000.0
            self.last_rec_path = self.rec_path
            self.log(f"[REC] Saved {dur:.2f}s ({self.rec_bytes} B) -> {self.last_rec_path}")
            self._notify_ui(f"Recorded {dur:.1f}s -> {os.path.basename(self.last_rec_path)}")

    def play_last_recording(self):
        if not self.last_rec_path or not os.path.exists(self.last_rec_path):
            self.log("[REC] No recording saved yet")
            return
        self.log(f"[REC] Playing {self.last_rec_path}...")
        for player in ("pw-play", "aplay"):
            try:
                subprocess.Popen([player, self.last_rec_path])
                return
            except FileNotFoundError:
                continue
        self.log("[REC] Neither pw-play nor aplay found")

    def start_ringing(self, number: str = "+15550199"):
        if self.rfcomm_fd < 0:
            self.log("[AG] Not connected to a headset over HFP")
            return
        self.caller_number = number or "+15550199"
        self.ring_pos = 0
        self._send_at("+BSIR: 1")
        if self.callsetup != 1:
            self.callsetup = 1
            self._send_ciev(3, 1)
        self._send_ring_pulse()
        if not self.ring_timer:
            self.ring_timer = GLib.timeout_add_seconds(3, self._send_ring_pulse)
        self.open_sco()
        self.log(f"[AG] Mocking incoming call from {self.caller_number} (in-band ringtone active)...")
        self._notify_ui(f"Ringing ({self.caller_number})")

    def _send_ring_pulse(self):
        if self.rfcomm_fd < 0 or self.callsetup != 1:
            self.ring_timer = 0
            return False
        self._send_at("RING")
        if self.clip_enabled:
            self._send_at(f'+CLIP: "{self.caller_number}",129')
        return True

    def answer_call(self):
        if self.ring_timer:
            GLib.source_remove(self.ring_timer)
            self.ring_timer = 0
        prev_setup = self.callsetup
        self.call = 1
        self.callsetup = 0
        self.voice_pos = 0
        self._send_ciev(2, 1)
        if prev_setup != 0:
            self._send_ciev(3, 0)
        self._start_recording()
        self.open_sco()
        self._notify_ui(f"Call Active ({self.caller_number})")

    def hangup_call(self):
        if self.ring_timer:
            GLib.source_remove(self.ring_timer)
            self.ring_timer = 0
        if self.call == 1:
            self.call = 0
            self._send_ciev(2, 0)
        if self.callsetup != 0:
            self.callsetup = 0
            self._send_ciev(3, 0)
        self.close_sco()
        self._notify_ui("Call Ended")

    def set_vgs(self, level: int):
        self.vgs = max(0, min(15, int(level)))
        self._send_at(f"+VGS: {self.vgs}")
        self._notify_ui(f"Phone VGS -> {self.vgs}/15")

    def set_vgm(self, level: int):
        self.vgm = max(0, min(15, int(level)))
        self._send_at(f"+VGM: {self.vgm}")
        self._notify_ui(f"Phone VGM -> {self.vgm}/15")

    # -------------------- SCO Voice Socket --------------------

    def open_sco(self):
        if self.sco_fd >= 0:
            return
        mac = (self.peer_mac or self.default_mac).strip().upper()
        if not mac:
            self.log("[SCO] No peer MAC")
            return
        fd = -1
        try:
            local_mac = self._get_adapter_mac_str()
            local_bdaddr = bytes(int(x, 16) for x in reversed(local_mac.split(":")))
            remote_bdaddr = bytes(int(x, 16) for x in reversed(mac.split(":")))

            # Use libc socket/bind/connect directly with struct sockaddr_sco (sa_family_t + bdaddr_t)
            # because Python 3.14's built-in AF_BLUETOOTH BTPROTO_SCO getsockaddrarg parser is broken.
            fd = _LIBC.socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO)
            if fd < 0:
                raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))

            local_addr = struct.pack("<H6s", AF_BLUETOOTH, local_bdaddr)
            if _LIBC.bind(fd, local_addr, len(local_addr)) < 0:
                raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))

            remote_addr = struct.pack("<H6s", AF_BLUETOOTH, remote_bdaddr)
            if _LIBC.connect(fd, remote_addr, len(remote_addr)) < 0:
                raise OSError(ctypes.get_errno(), os.strerror(ctypes.get_errno()))

            flags = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
            self.sco_fd = fd
            self.sco_watch = GLib.unix_fd_add_full(
                GLib.PRIORITY_DEFAULT,
                self.sco_fd,
                GLib.IOCondition.IN | GLib.IOCondition.HUP | GLib.IOCondition.ERR,
                self._on_sco_io,
            )
            self.log(f"[SCO] Voice eSCO link OPEN with {mac} (fd={self.sco_fd})")
            self._notify_ui("eSCO Audio Open")
        except Exception as e:
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
            self.log(f"[SCO] Failed to open SCO to {mac}: {e}")

    def _next_sco_tx_chunk(self, nbytes: int) -> bytes:
        """Return `nbytes` of 8kHz S16_LE PCM for the current call state (ringtone or partner voice)."""
        if self.callsetup == 1 and self.ringtone_pcm:
            pcm = self.ringtone_pcm
            pos = self.ring_pos
        elif self.call == 1 and self.voice_pcm:
            pcm = self.voice_pcm
            pos = self.voice_pos
        else:
            return b"\x00" * nbytes

        out = bytearray()
        plen = len(pcm)
        while len(out) < nbytes:
            take = min(nbytes - len(out), plen - pos)
            out += pcm[pos : pos + take]
            pos = (pos + take) % plen

        if self.callsetup == 1:
            self.ring_pos = pos
        else:
            self.voice_pos = pos
        return bytes(out)

    def _on_sco_io(self, fd, cond):
        if cond & (GLib.IOCondition.HUP | GLib.IOCondition.ERR):
            self.log("[EVENT] SCO voice link closed by headset")
            self.close_sco()
            return False
        if cond & GLib.IOCondition.IN:
            try:
                data = os.read(fd, 512)
                if not data:
                    self.close_sco()
                    return False
                if self.call == 1 and self.rec_wav is not None:
                    self.rec_wav.writeframesraw(data)
                    self.rec_bytes += len(data)
                # Clock TX PCM directly off incoming eSCO packet cadence
                tx = self._next_sco_tx_chunk(len(data))
                try:
                    os.write(fd, tx)
                except BlockingIOError:
                    pass
            except BlockingIOError:
                pass
            except OSError:
                self.close_sco()
                return False
        return True

    def close_sco(self):
        self._stop_recording()
        if self.sco_watch:
            GLib.source_remove(self.sco_watch)
            self.sco_watch = 0
        if self.sco_fd >= 0:
            try:
                os.close(self.sco_fd)
            except OSError:
                pass
            self.sco_fd = -1
            self.log("[SCO] Voice link closed")
            self._notify_ui()

    def _close_rfcomm(self):
        self.close_sco()
        if self.ring_timer:
            GLib.source_remove(self.ring_timer)
            self.ring_timer = 0
        if self.rfcomm_watch:
            GLib.source_remove(self.rfcomm_watch)
            self.rfcomm_watch = 0
        if self.rfcomm_fd >= 0:
            try:
                os.close(self.rfcomm_fd)
            except OSError:
                pass
            self.rfcomm_fd = -1
        self.cmer_enabled = False
        self.call = 0
        self.callsetup = 0
        self._notify_ui("HFP Disconnected")

    # -------------------- BlueZ Pair / Connect / Battery --------------------

    def set_discovery(self, enable: bool):
        method = "StartDiscovery" if enable else "StopDiscovery"
        try:
            self.bus.call_sync(
                "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                method,
                None,
                None,
                Gio.DBusCallFlags.NONE,
                5000,
                None,
            )
            self.scanning = enable
            self.log(f"[BT] {method} OK")
            self._notify_ui(f"Discovery {'ON' if enable else 'OFF'}")
        except Exception as e:
            self.log(f"[BT] {method} failed: {e}")

    def pair_device(self, mac: str = ""):
        if mac:
            self.default_mac = mac.strip().upper()
        path = self._mac_to_path(mac)
        if self.scanning:
            self.set_discovery(False)
        self.log(f"[BT] Pairing {path} (async)...")

        def on_paired(conn, res):
            try:
                conn.call_finish(res)
                conn.call_sync(
                    "org.bluez",
                    path,
                    "org.freedesktop.DBus.Properties",
                    "Set",
                    GLib.Variant(
                        "(ssv)", ("org.bluez.Device1", "Trusted", GLib.Variant("b", True))
                    ),
                    None,
                    Gio.DBusCallFlags.NONE,
                    3000,
                    None,
                )
                self.log(f"[BT] Paired and trusted {path}")
                self._notify_ui("Paired & Trusted")
            except Exception as e:
                self.log(f"[BT] Pair {path} failed: {e}")

        self.bus.call(
            "org.bluez",
            path,
            "org.bluez.Device1",
            "Pair",
            None,
            None,
            Gio.DBusCallFlags.NONE,
            30000,
            None,
            on_paired,
        )

    def unpair_device(self, mac: str = ""):
        if mac:
            self.default_mac = mac.strip().upper()
        path = self._mac_to_path(mac)
        self._close_rfcomm()
        try:
            self.bus.call_sync(
                "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                "RemoveDevice",
                GLib.Variant("(o)", (path,)),
                None,
                Gio.DBusCallFlags.NONE,
                5000,
                None,
            )
            self.log(f"[BT] Removed (unpaired) {path}")
            self._notify_ui("Unpaired")
        except Exception as e:
            self.log(f"[BT] RemoveDevice {path} failed: {e}")

    def _find_hfp_rfcomm_channels(self, mac: str) -> list:
        """Find the headset's Handsfree (0x111e) / Headset (0x1108) RFCOMM channel via sdptool."""
        channels = []
        try:
            # Query 0x111e specifically (not `browse`, which walks GATT records and drops the link)
            res = subprocess.run(
                ["sdptool", "records", mac],
                capture_output=True,
                text=True,
                timeout=6,
            )
            in_hfp = False
            for line in res.stdout.splitlines():
                s = line.strip()
                if s.startswith("Service Name:") or s.startswith("Service RecHandle:"):
                    in_hfp = ("Handsfree" in s or "Headset" in s)
                elif '"Handsfree" (0x111e)' in s or '"Headset" (0x1108)' in s:
                    in_hfp = True
                elif in_hfp and s.startswith("Channel:"):
                    ch = int(s.split("Channel:")[1].strip().split()[0])
                    if ch not in channels:
                        channels.append(ch)
                    in_hfp = False
        except Exception:
            pass
        for ch in (7, 1, 2, 3, 4, 5, 6, 8, 9):
            if ch not in channels:
                channels.append(ch)
        return channels

    def _ensure_trusted_on_both(self, mac: str) -> bool:
        """Ensure device is Paired and Trusted on host; returns True if ready."""
        path = self._mac_to_path(mac)
        props = self.get_device_props(mac)
        if not props.get("Paired", False):
            return False
        if not props.get("Trusted", False):
            try:
                self.bus.call_sync(
                    "org.bluez",
                    path,
                    "org.freedesktop.DBus.Properties",
                    "Set",
                    GLib.Variant(
                        "(ssv)", ("org.bluez.Device1", "Trusted", GLib.Variant("b", True))
                    ),
                    None,
                    Gio.DBusCallFlags.NONE,
                    2000,
                    None,
                )
            except Exception:
                pass
        return True

    def _direct_rfcomm_connect(self, mac: str, allow_repair: bool = True):
        """Connect directly to the headset's HFP-HF RFCOMM channel."""
        if self.rfcomm_fd >= 0:
            return
        mac = (mac or self.default_mac).strip().upper()
        local_mac = self._get_adapter_mac_str()

        local_bdaddr = bytes(int(x, 16) for x in reversed(local_mac.split(":")))
        remote_bdaddr = bytes(int(x, 16) for x in reversed(mac.split(":")))

        # Try channel 7 first without running sdptool (BlueZ default HFP-HF channel is 7),
        # then fall back to sdptool records if channel 7 returns ECONNREFUSED (111).
        channels = [7]
        queried_sdp = False
        idx = 0
        saw_reset = False

        while idx < len(channels):
            ch = channels[idx]
            idx += 1
            for attempt in range(3):
                fd = -1
                try:
                    SOCK_STREAM = 1
                    fd = _LIBC.socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM)
                    if fd < 0:
                        break
                    local_rc = struct.pack("<H6sBx", AF_BLUETOOTH, local_bdaddr, 0)
                    if _LIBC.bind(fd, local_rc, len(local_rc)) < 0:
                        os.close(fd)
                        break
                    remote_rc = struct.pack("<H6sBx", AF_BLUETOOTH, remote_bdaddr, ch)
                    if _LIBC.connect(fd, remote_rc, len(remote_rc)) == 0:
                        self.log(f"[AG] Direct RFCOMM connected to {mac} on channel {ch}")
                        self._attach_rfcomm_fd(fd, mac)
                        return
                    err = ctypes.get_errno()
                    os.close(fd)
                    if err in (112, 16, 114, 11):  # EHOSTDOWN / EBUSY / EALREADY / EAGAIN -> retry paging
                        time.sleep(0.5)
                        continue
                    if err == 111 and not queried_sdp:
                        queried_sdp = True
                        for sdp_ch in self._find_hfp_rfcomm_channels(mac):
                            if sdp_ch not in channels:
                                channels.append(sdp_ch)
                    elif err in (104, 13, 52):  # ECONNRESET / EACCES / EBADE
                        saw_reset = True
                        self.log(f"[AG] RFCOMM channel {ch} rejected by peer (errno={err}: {os.strerror(err)})")
                    elif err != 111:
                        self.log(f"[AG] RFCOMM channel {ch} connect errno={err} ({os.strerror(err)})")
                    break
                except Exception:
                    if fd >= 0:
                        try:
                            os.close(fd)
                        except OSError:
                            pass
                    break
            if saw_reset:
                break

        if saw_reset and allow_repair:
            self.log(
                f"[BT] Peer {mac} reset RFCOMM (stale link key or untrusted after reflash); "
                "re-pairing automatically..."
            )
            self._auto_repair_and_connect(mac)
            return

        self.log(
            f"[AG] Could not open HFP RFCOMM to {mac}. "
            "If the headset was reflashed, click 'Unpair' then 'Pair & Trust'."
        )

    def _auto_repair_and_connect(self, mac: str):
        """Remove stale bonding on host, wait until discovery finds the device object, pair + trust, and connect."""
        path = self._mac_to_path(mac)
        local_mac = self._get_adapter_mac_str()

        # If target is nokia-argon reachable over USB-eth (172.16.42.1), also remove any half-paired
        # host entry on the phone so Pair doesn't fail with AuthenticationFailed.
        subprocess.run(
            [
                "ssh",
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=2",
                "elmot@172.16.42.1",
                f"bluetoothctl trust {local_mac} >/dev/null 2>&1 || true",
            ],
            capture_output=True,
        )

        try:
            self.bus.call_sync(
                "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                "RemoveDevice",
                GLib.Variant("(o)", (path,)),
                None,
                Gio.DBusCallFlags.NONE,
                5000,
                None,
            )
            self.log(f"[BT] Removed stale pairing for {mac}; scanning until discovered...")
        except Exception:
            pass

        subprocess.run(
            [
                "ssh",
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=2",
                "elmot@172.16.42.1",
                f"bluetoothctl remove {local_mac} >/dev/null 2>&1 || true",
            ],
            capture_output=True,
        )

        self.set_discovery(True)
        attempts = [0]

        def poll_discovered():
            attempts[0] += 1
            props = self.get_device_props(mac)
            if not props and attempts[0] < 10:
                return True  # Keep scanning (up to 10s)
            self.set_discovery(False)

            def do_pair_after_scan_stops():
                try:
                    self.bus.call_sync(
                        "org.bluez",
                        path,
                        "org.bluez.Device1",
                        "Pair",
                        None,
                        None,
                        Gio.DBusCallFlags.NONE,
                        20000,
                        None,
                    )
                    self.bus.call_sync(
                        "org.bluez",
                        path,
                        "org.freedesktop.DBus.Properties",
                        "Set",
                        GLib.Variant(
                            "(ssv)", ("org.bluez.Device1", "Trusted", GLib.Variant("b", True))
                        ),
                        None,
                        Gio.DBusCallFlags.NONE,
                        3000,
                        None,
                    )
                    self.log(f"[BT] Re-paired and trusted {mac}; connecting HFP RFCOMM...")
                except Exception as e:
                    self.log(f"[BT] Auto-repair note: {e}")
                self._direct_rfcomm_connect(mac, allow_repair=False)
                return False

            # Give HCI controller 1s after StopDiscovery before initiating Pair
            GLib.timeout_add_seconds(1, do_pair_after_scan_stops)
            return False

        GLib.timeout_add_seconds(1, poll_discovered)

    def connect_device(self, mac: str = ""):
        if mac:
            self.default_mac = mac.strip().upper()
        target_mac = (mac or self.default_mac).strip().upper()
        path = self._mac_to_path(target_mac)
        # Always call StopDiscovery on the adapter because an external process (like a GUI settings panel)
        # may have left BlueZ inquiry scanning active, which causes br-connection-busy (36).
        try:
            self.bus.call_sync(
                "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                "StopDiscovery",
                None,
                None,
                Gio.DBusCallFlags.NONE,
                2000,
                None,
            )
        except Exception:
            pass
        self.scanning = False

        props = self.get_device_props(target_mac)
        uuids = [u.lower() for u in props.get("UUIDs", [])]
        self.log(
            f"[BT] Target {target_mac} Paired={props.get('Paired', False)} "
            f"Connected={props.get('Connected', False)} HFP-HF_UUID_advertised={HFP_HF_UUID in uuids}"
        )
        if not props.get("Paired", False):
            self.log(
                f"[BT] {target_mac} is not paired yet; discovering and pairing automatically..."
            )
            self._auto_repair_and_connect(target_mac)
            return

        if not props.get("Trusted", False):
            try:
                self.bus.call_sync(
                    "org.bluez",
                    path,
                    "org.freedesktop.DBus.Properties",
                    "Set",
                    GLib.Variant(
                        "(ssv)", ("org.bluez.Device1", "Trusted", GLib.Variant("b", True))
                    ),
                    None,
                    Gio.DBusCallFlags.NONE,
                    2000,
                    None,
                )
            except Exception:
                pass

        self.log(f"[BT] Connecting HFP ({HFP_HF_UUID}) to {path}...")
        self._direct_rfcomm_connect(target_mac)
        if self.rfcomm_fd >= 0:
            self.get_battery(target_mac)
            self._notify_ui("Connected HFP")

    def disconnect_device(self, mac: str = ""):
        if mac:
            self.default_mac = mac.strip().upper()
        path = self._mac_to_path(mac)
        self._close_rfcomm()

        def on_disconnected(conn, res):
            try:
                conn.call_finish(res)
                self.log(f"[BT] Disconnected {path}")
                self._notify_ui("Disconnected")
            except Exception as e:
                self.log(f"[BT] Disconnect {path} failed: {e}")

        self.bus.call(
            "org.bluez",
            path,
            "org.bluez.Device1",
            "Disconnect",
            None,
            None,
            Gio.DBusCallFlags.NONE,
            10000,
            None,
            on_disconnected,
        )

    def get_battery(self, mac: str = ""):
        path = self._mac_to_path(mac)
        dbus_pct = None
        try:
            res = self.bus.call_sync(
                "org.bluez",
                path,
                "org.freedesktop.DBus.Properties",
                "Get",
                GLib.Variant("(ss)", ("org.bluez.Battery1", "Percentage")),
                GLib.VariantType("(v)"),
                Gio.DBusCallFlags.NONE,
                2000,
                None,
            )
            dbus_pct = int(res.unpack()[0])
            self.dbus_battery = dbus_pct
            if self.headset_battery is None:
                self.headset_battery = dbus_pct
        except Exception:
            pass
        self.log(
            f"[BATTERY] {path}: BlueZ Battery1={dbus_pct}% | HFP AT reported={self.headset_battery}%"
        )
        self._notify_ui()
        return self.headset_battery if self.headset_battery is not None else dbus_pct

    # -------------------- AVRCP / Media Button Evdev Listener --------------------

    def _scan_avrcp_inputs(self):
        for sys_name in glob.glob("/sys/class/input/event*/device/name"):
            try:
                name = open(sys_name, "r", encoding="utf-8").read().strip()
            except OSError:
                continue
            ev_name = sys_name.split("/")[4]
            dev_node = f"/dev/input/{ev_name}"
            if dev_node in self.evdev_watchers:
                continue
            if (
                "AVRCP" in name.upper()
                or ":" in name
                or "nokia" in name.lower()
                or (self.peer_mac and self.peer_mac.lower() in name.lower())
            ):
                try:
                    fd = os.open(dev_node, os.O_RDONLY | os.O_NONBLOCK)
                    wid = GLib.unix_fd_add_full(
                        GLib.PRIORITY_DEFAULT,
                        fd,
                        GLib.IOCondition.IN | GLib.IOCondition.HUP | GLib.IOCondition.ERR,
                        self._on_evdev_io,
                    )
                    self.evdev_watchers[dev_node] = (fd, wid, name)
                    self.log(f"[AVRCP] Watching media buttons on {dev_node} ('{name}')")
                except (PermissionError, OSError):
                    pass
        return True

    def _on_evdev_io(self, fd, cond):
        if cond & (GLib.IOCondition.HUP | GLib.IOCondition.ERR):
            for k, (efd, _wid, name) in list(self.evdev_watchers.items()):
                if efd == fd:
                    os.close(efd)
                    del self.evdev_watchers[k]
                    self.log(f"[AVRCP] Input device {k} ('{name}') removed")
            return False

        ev_size = struct.calcsize("llHHi")
        try:
            data = os.read(fd, ev_size * 16)
        except OSError:
            return True

        for offset in range(0, len(data), ev_size):
            _sec, _usec, ev_type, code, value = struct.unpack(
                "llHHi", data[offset : offset + ev_size]
            )
            if ev_type == 1:  # EV_KEY
                key_str = KEY_NAMES.get(code, f"KEY_{code}")
                state = {0: "RELEASED", 1: "PRESSED", 2: "REPEAT"}.get(value, str(value))
                self.log(f"[EVENT] >>> HEADSET MEDIA BUTTON: {key_str} ({code}) {state} <<<")
                self._notify_ui(f"Media Button: {key_str} {state}")
        return True

    # -------------------- Interactive CLI --------------------

    def execute_command(self, line: str) -> bool:
        parts = line.strip().split()
        if not parts:
            return True
        cmd = parts[0].lower()
        arg = parts[1] if len(parts) > 1 else ""

        if cmd == "status":
            self.log(
                f"peer={self.peer_mac or self.default_mac} rfcomm={self.rfcomm_fd >= 0} "
                f"slc={self.cmer_enabled} sco={self.sco_fd >= 0} call={self.call} "
                f"callsetup={self.callsetup} vgs={self.vgs} vgm={self.vgm} "
                f"battery={self.headset_battery}%"
            )
        elif cmd == "scan":
            self.set_discovery(arg.lower() == "on")
        elif cmd == "pair":
            self.pair_device(arg)
        elif cmd == "unpair":
            self.unpair_device(arg)
        elif cmd == "connect":
            self.connect_device(arg)
        elif cmd == "disconnect":
            self.disconnect_device(arg)
        elif cmd == "battery":
            self.get_battery(arg)
        elif cmd == "ring":
            self.start_ringing(arg or "+15550199")
        elif cmd == "answer":
            self.answer_call()
        elif cmd == "hangup":
            self.hangup_call()
        elif cmd == "sco":
            if arg.lower() == "on":
                self.open_sco()
            else:
                self.close_sco()
        elif cmd == "vgs" and arg:
            self.set_vgs(int(arg))
        elif cmd == "vgm" and arg:
            self.set_vgm(int(arg))
        elif cmd == "voice" and arg:
            self.set_voice_file(arg)
        elif cmd == "ringtone" and arg:
            self.set_ringtone_file(arg)
        elif cmd == "playrec":
            self.play_last_recording()
        elif cmd in ("quit", "exit"):
            self._close_rfcomm()
            self._restore_wireplumber()
            self.loop.quit()
            return False
        else:
            self.log(
                "Commands: status | scan on|off | pair [MAC] | unpair [MAC] | "
                "connect [MAC] | disconnect [MAC] | battery [MAC] | "
                "ring [number] | answer | hangup | sco on|off | vgs <0-15> | vgm <0-15> | quit"
            )
        return True

    def handle_stdin(self, _fd, _cond):
        line = sys.stdin.readline()
        if not line:
            self.loop.quit()
            return False
        return self.execute_command(line)


# -------------------- GTK3 GUI Shell --------------------


def build_gtk_window(ag: MockPhoneAG):
    gi.require_version("Gtk", "3.0")
    from gi.repository import Gtk, Pango

    win = Gtk.Window(title="Mock Mobile Phone (Bluetooth HFP-AG)")
    win.set_default_size(640, 620)
    win.set_border_width(10)

    def on_destroy(_w):
        ag._close_rfcomm()
        ag._restore_wireplumber()
        if ag.loop.is_running():
            ag.loop.quit()

    win.connect("destroy", on_destroy)

    vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
    win.add(vbox)

    # 1. Target Device & Connection Bar
    frame_dev = Gtk.Frame(label=" 1. Bluetooth Headset Connection & Pairing ")
    box_dev = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
    box_dev.set_border_width(8)
    frame_dev.add(box_dev)
    vbox.pack_start(frame_dev, False, False, 0)

    row_mac = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
    box_dev.pack_start(row_mac, False, False, 0)
    row_mac.pack_start(Gtk.Label(label="Headset MAC:"), False, False, 0)
    entry_mac = Gtk.Entry()
    entry_mac.set_text(ag.default_mac)
    entry_mac.set_width_chars(19)
    row_mac.pack_start(entry_mac, False, False, 0)

    btn_scan = Gtk.ToggleButton(label="Scan Discovery")
    row_mac.pack_start(btn_scan, False, False, 0)

    btn_battery = Gtk.Button(label="Refresh Battery")
    row_mac.pack_end(btn_battery, False, False, 0)

    row_btns = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
    box_dev.pack_start(row_btns, False, False, 0)
    btn_connect = Gtk.Button(label="Connect")
    btn_disconnect = Gtk.Button(label="Disconnect")
    btn_pair = Gtk.Button(label="Pair & Trust")
    btn_unpair = Gtk.Button(label="Unpair (Remove)")
    for b in (btn_connect, btn_disconnect, btn_pair, btn_unpair):
        row_btns.pack_start(b, True, True, 0)

    # 2. Live Telemetry / Status Grid
    frame_stat = Gtk.Frame(label=" 2. Headset Telemetry & State ")
    grid_stat = Gtk.Grid(column_spacing=16, row_spacing=6)
    grid_stat.set_border_width(8)
    frame_stat.add(grid_stat)
    vbox.pack_start(frame_stat, False, False, 0)

    lbl_bt_state = Gtk.Label(label="Disconnected", xalign=0)
    lbl_hfp_state = Gtk.Label(label="RFCOMM: Off | SLC: No | eSCO: Closed", xalign=0)
    lbl_call_state = Gtk.Label(label="Idle (call=0, callsetup=0)", xalign=0)
    lbl_battery = Gtk.Label(label="N/A", xalign=0)
    lbl_last_event = Gtk.Label(label="Idle", xalign=0)

    rows = [
        ("BlueZ Device:", lbl_bt_state),
        ("HFP / Audio Link:", lbl_hfp_state),
        ("Call Status:", lbl_call_state),
        ("Headset Battery:", lbl_battery),
        ("Last Headset Event:", lbl_last_event),
    ]
    for idx, (title, widget) in enumerate(rows):
        t_lbl = Gtk.Label(label=title, xalign=0)
        t_lbl.set_markup(f"<b>{title}</b>")
        grid_stat.attach(t_lbl, 0, idx, 1, 1)
        grid_stat.attach(widget, 1, idx, 1, 1)

    # 3. Call & SCO Simulator
    frame_call = Gtk.Frame(label=" 3. Mock Call & Voice Audio (SCO) Control ")
    box_call = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
    box_call.set_border_width(8)
    frame_call.add(box_call)
    vbox.pack_start(frame_call, False, False, 0)

    row_num = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
    box_call.pack_start(row_num, False, False, 0)
    row_num.pack_start(Gtk.Label(label="Caller ID (+CLIP):"), False, False, 0)
    entry_num = Gtk.Entry()
    entry_num.set_text(ag.caller_number)
    row_num.pack_start(entry_num, True, True, 0)

    row_voice = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
    box_call.pack_start(row_voice, False, False, 0)
    row_voice.pack_start(Gtk.Label(label="Partner Voice File:"), False, False, 0)
    entry_voice = Gtk.Entry()
    entry_voice.set_placeholder_text("(Built-in synthesized voice; or pick any .wav/.mp3/.ogg)")
    if ag.voice_path:
        entry_voice.set_text(ag.voice_path)
    row_voice.pack_start(entry_voice, True, True, 0)
    btn_browse_voice = Gtk.Button(label="Browse...")
    btn_play_rec = Gtk.Button(label="Play Last Recording")
    row_voice.pack_start(btn_browse_voice, False, False, 0)
    row_voice.pack_start(btn_play_rec, False, False, 0)

    def on_browse_voice(_b):
        dlg = Gtk.FileChooserDialog(
            title="Choose Partner Voice Audio File",
            parent=win,
            action=Gtk.FileChooserAction.OPEN,
        )
        dlg.add_buttons(
            Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL, Gtk.STOCK_OPEN, Gtk.ResponseType.OK
        )
        if dlg.run() == Gtk.ResponseType.OK:
            path = dlg.get_filename()
            entry_voice.set_text(path)
            ag.set_voice_file(path)
        dlg.destroy()

    btn_browse_voice.connect("clicked", on_browse_voice)
    entry_voice.connect("activate", lambda w: ag.set_voice_file(w.get_text().strip()))
    btn_play_rec.connect("clicked", lambda _b: ag.play_last_recording())

    row_call_btns = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
    box_call.pack_start(row_call_btns, False, False, 0)
    btn_ring = Gtk.Button(label="Mock Incoming Ring")
    btn_answer = Gtk.Button(label="Answer Call (Phone)")
    btn_hangup = Gtk.Button(label="Hangup / End Call")
    btn_sco_on = Gtk.Button(label="Open SCO Audio")
    btn_sco_off = Gtk.Button(label="Close SCO Audio")
    for b in (btn_ring, btn_answer, btn_hangup, btn_sco_on, btn_sco_off):
        row_call_btns.pack_start(b, True, True, 0)

    # 4. Volume Sliders (VGS & VGM)
    frame_vol = Gtk.Frame(label=" 4. HFP Volume Synchronization (0..15) ")
    grid_vol = Gtk.Grid(column_spacing=12, row_spacing=4)
    grid_vol.set_border_width(8)
    frame_vol.add(grid_vol)
    vbox.pack_start(frame_vol, False, False, 0)

    adj_vgs = Gtk.Adjustment(value=ag.vgs, lower=0, upper=15, step_increment=1, page_increment=2)
    scale_vgs = Gtk.Scale(orientation=Gtk.Orientation.HORIZONTAL, adjustment=adj_vgs)
    scale_vgs.set_digits(0)
    scale_vgs.set_hexpand(True)

    adj_vgm = Gtk.Adjustment(value=ag.vgm, lower=0, upper=15, step_increment=1, page_increment=2)
    scale_vgm = Gtk.Scale(orientation=Gtk.Orientation.HORIZONTAL, adjustment=adj_vgm)
    scale_vgm.set_digits(0)
    scale_vgm.set_hexpand(True)

    grid_vol.attach(Gtk.Label(label="Speaker Gain (VGS):", xalign=0), 0, 0, 1, 1)
    grid_vol.attach(scale_vgs, 1, 0, 1, 1)
    grid_vol.attach(Gtk.Label(label="Mic Gain (VGM):", xalign=0), 0, 1, 1, 1)
    grid_vol.attach(scale_vgm, 1, 1, 1, 1)

    # 5. Live AT / AVRCP Log + Interactive Command Bar
    frame_log = Gtk.Frame(label=" 5. Live HFP AT / AVRCP Event Log & Command Shell ")
    box_log = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
    box_log.set_border_width(8)
    frame_log.add(box_log)
    vbox.pack_start(frame_log, True, True, 0)

    scrolled = Gtk.ScrolledWindow()
    scrolled.set_vexpand(True)
    textview = Gtk.TextView()
    textview.set_editable(False)
    textview.set_cursor_visible(False)
    textview.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
    textview.override_font(Pango.FontDescription("Monospace 9"))
    scrolled.add(textview)
    box_log.pack_start(scrolled, True, True, 0)
    textbuf = textview.get_buffer()

    row_cmd = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
    box_log.pack_start(row_cmd, False, False, 0)
    row_cmd.pack_start(Gtk.Label(label="Command:"), False, False, 0)
    entry_cmd = Gtk.Entry()
    entry_cmd.set_placeholder_text("Type any CLI command (e.g. ring +380501234567, vgs 14, status)...")
    row_cmd.pack_start(entry_cmd, True, True, 0)
    btn_send_cmd = Gtk.Button(label="Run")
    row_cmd.pack_start(btn_send_cmd, False, False, 0)

    updating_sliders = False
    updating_scan_btn = False

    def refresh_ui():
        nonlocal updating_sliders, updating_scan_btn
        mac = entry_mac.get_text().strip().upper() or ag.default_mac
        props = ag.get_device_props(mac)
        name = props.get("Alias") or props.get("Name") or mac
        paired = props.get("Paired", False)
        connected = props.get("Connected", False)
        rssi = props.get("RSSI")
        rssi_str = f" | RSSI: {rssi} dBm" if rssi is not None else ""
        lbl_bt_state.set_text(
            f"{name} ({mac}) — Connected: {'Yes' if connected else 'No'} | Paired: {'Yes' if paired else 'No'}{rssi_str}"
        )

        lbl_hfp_state.set_text(
            f"RFCOMM: {'Connected' if ag.rfcomm_fd >= 0 else 'Off'} | "
            f"SLC: {'Ready' if ag.cmer_enabled else 'No'} | "
            f"eSCO Voice: {'OPEN' if ag.sco_fd >= 0 else 'Closed'}"
        )

        call_desc = "Idle"
        if ag.call == 1:
            call_desc = f"IN CALL with {ag.caller_number}"
        elif ag.callsetup == 1:
            call_desc = f"INCOMING RING from {ag.caller_number}"
        elif ag.callsetup == 2:
            call_desc = f"OUTGOING DIAL to {ag.caller_number}"
        lbl_call_state.set_text(f"{call_desc} (call={ag.call}, callsetup={ag.callsetup})")

        bat_str = (
            f"{ag.headset_battery}%"
            if ag.headset_battery is not None
            else (f"{ag.dbus_battery}%" if ag.dbus_battery is not None else "Not reported by headset")
        )
        lbl_battery.set_text(bat_str)
        lbl_last_event.set_text(ag.last_event)

        updating_sliders = True
        adj_vgs.set_value(ag.vgs)
        adj_vgm.set_value(ag.vgm)
        updating_sliders = False

        if btn_scan.get_active() != ag.scanning:
            updating_scan_btn = True
            btn_scan.set_active(ag.scanning)
            updating_scan_btn = False

    def append_log(line: str):
        end_iter = textbuf.get_end_iter()
        textbuf.insert(end_iter, line + "\n")
        mark = textbuf.create_mark(None, textbuf.get_end_iter(), False)
        textview.scroll_to_mark(mark, 0.0, True, 0.0, 1.0)

    ag.on_state_changed = lambda: GLib.idle_add(refresh_ui)
    ag.on_log_message = lambda msg: GLib.idle_add(append_log, msg)

    # Wire up signals
    def on_scan_toggled(b):
        if not updating_scan_btn:
            ag.set_discovery(b.get_active())

    btn_scan.connect("toggled", on_scan_toggled)
    btn_battery.connect("clicked", lambda _b: ag.get_battery(entry_mac.get_text()))
    btn_connect.connect("clicked", lambda _b: ag.connect_device(entry_mac.get_text()))
    btn_disconnect.connect("clicked", lambda _b: ag.disconnect_device(entry_mac.get_text()))
    btn_pair.connect("clicked", lambda _b: ag.pair_device(entry_mac.get_text()))
    btn_unpair.connect("clicked", lambda _b: ag.unpair_device(entry_mac.get_text()))

    btn_ring.connect("clicked", lambda _b: ag.start_ringing(entry_num.get_text().strip()))
    btn_answer.connect("clicked", lambda _b: ag.answer_call())
    btn_hangup.connect("clicked", lambda _b: ag.hangup_call())
    btn_sco_on.connect("clicked", lambda _b: ag.open_sco())
    btn_sco_off.connect("clicked", lambda _b: ag.close_sco())

    def on_vgs_changed(scale):
        if not updating_sliders:
            ag.set_vgs(int(scale.get_value()))

    def on_vgm_changed(scale):
        if not updating_sliders:
            ag.set_vgm(int(scale.get_value()))

    scale_vgs.connect("value-changed", on_vgs_changed)
    scale_vgm.connect("value-changed", on_vgm_changed)

    def run_cmd_entry(_w):
        text = entry_cmd.get_text().strip()
        if text:
            entry_cmd.set_text("")
            ag.log(f"> {text}")
            ag.execute_command(text)

    entry_cmd.connect("activate", run_cmd_entry)
    btn_send_cmd.connect("clicked", run_cmd_entry)

    refresh_ui()
    win.show_all()
    return win


def main():
    parser = argparse.ArgumentParser(
        description="Mock Mobile Phone (Bluetooth HFP-AG) with CLI + GTK3 GUI"
    )
    parser.add_argument(
        "--mac",
        default="02:00:34:40:12:1E",
        help="Default Bluetooth Headset MAC address (default: nokia-argon 02:00:34:40:12:1E)",
    )
    parser.add_argument(
        "--coexist",
        action="store_true",
        help="Skip registering HFP-AG profile (use if PipeWire/WirePlumber owns HFP-AG)",
    )
    parser.add_argument(
        "--no-gui",
        action="store_true",
        help="Run in terminal-only mode without opening the GTK3 window",
    )
    parser.add_argument(
        "--ringtone",
        default=DEFAULT_RINGTONE_FILE,
        help=f"Audio file to stream as in-band ringtone (default: {DEFAULT_RINGTONE_FILE})",
    )
    parser.add_argument(
        "--voice",
        default=DEFAULT_VOICE_FILE,
        help=f"Audio file to stream as call partner voice (default: {DEFAULT_VOICE_FILE})",
    )
    args = parser.parse_args()

    ag = MockPhoneAG(
        default_mac=args.mac,
        register_ag=not args.coexist,
        ringtone_path=args.ringtone,
        voice_path=args.voice,
    )
    GLib.unix_fd_add_full(
        GLib.PRIORITY_DEFAULT, sys.stdin.fileno(), GLib.IOCondition.IN, ag.handle_stdin
    )

    if not args.no_gui and (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        try:
            build_gtk_window(ag)
            ag.log("[GUI] GTK3 shell started (stdin CLI remains active simultaneously)")
        except Exception as e:
            ag.log(f"[GUI] Falling back to CLI-only mode ({e})")

    ag.log(f"[MockPhone] Ready (target={args.mac}). Type 'ring', 'status', 'connect', etc.")
    try:
        ag.loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        ag._close_rfcomm()
        ag._restore_wireplumber()


if __name__ == "__main__":
    main()
