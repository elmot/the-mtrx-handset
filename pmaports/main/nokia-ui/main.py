#!/usr/bin/env python3
"""Nokia 8110 4G Slint KMS/DRM UI driven by nokia-bt-hfp."""

from datetime import timedelta
import glob
import os
import queue
import socket
import subprocess
import threading
import slint

HERE = os.path.dirname(os.path.abspath(__file__))


def _hfp_sock_path() -> str:
    xdg = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    return os.path.join(xdg, "nokia-bt-hfp.sock")


def send_hfp_cmd(cmd: str) -> str:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(0.3)
            s.connect(_hfp_sock_path())
            s.sendall((cmd + "\n").encode("utf-8"))
            return s.recv(512).decode("utf-8", errors="replace").strip()
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


class NokiaUi:
    def __init__(self) -> None:
        self.app = slint.load_file(os.path.join(HERE, "main.slint"))
        self.ScreenId = getattr(self.app, "ScreenId", None)
        self.win = self.app.MainWindow()
        self.ev_queue: queue.SimpleQueue[tuple[bool, str, str]] = queue.SimpleQueue()
        self.prev_screen = self._screen_val("home")

        self.call_ended_timer = slint.Timer()
        self.pairing_timer = slint.Timer()
        self.poll_timer = slint.Timer()
        self.battery_timer = slint.Timer()

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

    def _enter_call_ended(self) -> None:
        self.set_screen("call_ended")
        self.call_ended_timer.start(
            slint.TimerMode.SingleShot,
            timedelta(milliseconds=1500),
            self._after_call_ended,
        )

    def _after_call_ended(self) -> None:
        if self._is_screen("call_ended"):
            self.set_screen("home" if self.win.slider_open else "sleep")

    def _start_pairing(self) -> None:
        self.win.pairing_timed_out = False
        self.set_screen("pairing")
        self.pairing_timer.start(
            slint.TimerMode.SingleShot,
            timedelta(seconds=120),
            self._on_pairing_timeout,
        )

    def _on_pairing_timeout(self) -> None:
        if self._is_screen("pairing"):
            self.win.pairing_timed_out = True

    def on_soft_left(self) -> None:
        if self._is_screen("home"):
            if self.win.paired_device_name:
                self.set_screen("unpair")
            else:
                self._start_pairing()
        elif self._is_screen("incoming_call"):
            send_hfp_cmd("answer")
        elif self._is_screen("active_call"):
            self.win.speaker_mode = not self.win.speaker_mode
            send_hfp_cmd("speaker" if self.win.speaker_mode else "earpiece")
        elif self._is_screen("unpair"):
            self.set_screen("home")
        elif self._is_screen("pairing") and self.win.pairing_timed_out:
            self._start_pairing()
        elif self._is_screen("power_off"):
            subprocess.Popen(["poweroff"])

    def on_soft_right(self) -> None:
        if self._is_screen("home"):
            self.set_screen("power_off")
        elif self._is_screen("incoming_call"):
            send_hfp_cmd("hangup")
            self.set_screen("home")
        elif self._is_screen("active_call"):
            send_hfp_cmd("hangup")
            self._enter_call_ended()
        elif self._is_screen("unpair"):
            self.win.paired_device_name = ""
            self.win.bt_connected = False
            self._start_pairing()
        elif self._is_screen("pairing"):
            self.pairing_timer.stop()
            self.set_screen("home")
        elif self._is_screen("power_off"):
            self.win.active_screen = self.prev_screen

    def on_pickup(self) -> None:
        if self._is_screen("home"):
            send_hfp_cmd("redial")
        elif self._is_screen("incoming_call"):
            send_hfp_cmd("answer")

    def on_hangup(self) -> None:
        if self._is_screen("incoming_call"):
            send_hfp_cmd("hangup")
            self.set_screen("home")
        elif self._is_screen("active_call"):
            send_hfp_cmd("hangup")
            self._enter_call_ended()
        elif (
            self._is_screen("unpair")
            or self._is_screen("pairing")
            or self._is_screen("power_off")
        ):
            self.pairing_timer.stop()
            self.set_screen("home")

    def adjust_volume(self, delta: int) -> None:
        if self._is_screen("active_call"):
            pct = max(0, min(100, self.win.volume_percent + delta))
            self.win.volume_percent = pct
            vgs = round(pct * 15 / 100)
            send_hfp_cmd(f"vgs {vgs}")

    def read_initial_status(self) -> None:
        data = send_hfp_cmd("status")
        if not data:
            return
        kv = dict(item.split("=", 1) for item in data.split() if "=" in item)
        connected = kv.get("connected") == "1"
        call = kv.get("call") == "1"
        callsetup = kv.get("callsetup", "0")
        caller = kv.get("caller", "-")
        peer = kv.get("peer", "-")
        if peer and peer != "-":
            self.win.paired_device_name = peer
        if "output" in kv:
            self.win.speaker_mode = kv["output"] == "speaker"
        if "vgs" in kv:
            try:
                vgs = max(0, min(15, int(kv["vgs"])))
                self.win.volume_percent = round(vgs * 100 / 15)
            except ValueError:
                pass
        state = "active" if call else ("incoming" if callsetup == "1" else "idle")
        self.ev_queue.put((connected, state, "" if caller == "-" else caller))

    def _dbus_listener(self) -> None:
        try:
            proc = subprocess.Popen(
                ["dbus-monitor", "--system", "type='signal',interface='org.nokia.BtHfp'"],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            )
        except OSError:
            return
        if not proc.stdout:
            return
        vals: list = []
        for line in proc.stdout:
            s = line.strip()
            if s.startswith("boolean "):
                vals = [s.split()[1] == "true"]
            elif s.startswith("string ") and vals:
                parts = s.split('"', 2)
                if len(parts) >= 2:
                    vals.append(parts[1])
                if len(vals) == 3:
                    self.ev_queue.put((vals[0], vals[1], vals[2]))
                    vals = []

    def _drain_events(self) -> None:
        while not self.ev_queue.empty():
            connected, state, caller = self.ev_queue.get_nowait()
            self.win.bt_connected = connected
            self.win.caller_id = caller
            if state == "incoming":
                self.win.ring_silenced = False
                self.set_screen("incoming_call")
            elif state == "active":
                self.set_screen("active_call")
            elif state == "idle":
                if self._is_screen("active_call"):
                    self._enter_call_ended()
                elif self._is_screen("incoming_call"):
                    self.set_screen("home")

    def _refresh_battery(self) -> None:
        pct = read_battery_percent()
        if pct is not None:
            self.win.battery_percent = pct

    def run(self) -> None:
        self._refresh_battery()
        self.read_initial_status()
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
