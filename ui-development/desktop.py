#!/usr/bin/env python3
"""Desktop preview & state-machine harness for nokia-ui Slint screens."""

from __future__ import annotations

import argparse
from datetime import timedelta
from pathlib import Path
import queue
import sys
import threading
import slint

REPO_ROOT = Path(__file__).resolve().parent.parent
NOKIA_UI_DIR = REPO_ROOT / "pmaports" / "main" / "nokia-ui"
SIMULATOR_SLINT = Path(__file__).resolve().parent / "simulator.slint"
DEFAULT_PAIRED_DEVICE = "Pixel 8 Pro"


class DesktopUiHarness:
    """Drives nokia-ui's MainView and OS Event Simulator in a single Slint window."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.app = slint.load_file(
            str(SIMULATOR_SLINT),
            include_paths=[str(NOKIA_UI_DIR)],
        )
        self.ScreenId = self.app.ScreenId
        self.win = self.app.DesktopDevWindow()
        self.cmd_queue: queue.SimpleQueue[str] = queue.SimpleQueue()
        self.prev_screen = self.ScreenId.home
        self.default_paired_device = args.paired_device or DEFAULT_PAIRED_DEVICE
        self.default_caller = args.caller

        # Single-shot timers for CALL_ENDED (1.5s) and PAIRING timeout (120s)
        self.call_ended_timer = slint.Timer()
        self.pairing_timer = slint.Timer()
        self.poll_timer = slint.Timer()

        # Apply initial state from CLI arguments
        self.win.bt_connected = args.bt_connected
        self.win.paired_device_name = args.paired_device
        self.win.caller_id = args.caller
        self.win.battery_percent = max(0, min(100, args.battery))
        self.win.slider_open = not args.slider_closed
        self.win.speaker_mode = args.speaker
        self.win.volume_percent = max(0, min(100, args.volume))
        self.win.pairing_timed_out = args.pairing_timeout
        self.set_screen(args.screen)

        # Bind real MainView keyboard / softkey callbacks
        self.win.soft_left = self.on_soft_left
        self.win.soft_right = self.on_soft_right
        self.win.pickup_pressed = self.on_pickup
        self.win.hangup_pressed = self.on_hangup
        self.win.vol_up = lambda: self.adjust_volume(+5)
        self.win.vol_down = lambda: self.adjust_volume(-5)

        # Bind OS Event Simulator panel callbacks
        self.win.bt_connect = self.on_bt_connect
        self.win.bt_disconnect = self.on_bt_disconnect
        self.win.bt_pair = self.on_bt_pair
        self.win.bt_unpair = self.on_bt_unpair
        self.win.bt_pair_timeout = self.on_bt_pair_timeout
        self.win.sim_vol_up = lambda: self.adjust_volume(+5, allow_any_screen=True)
        self.win.sim_vol_down = lambda: self.adjust_volume(-5, allow_any_screen=True)
        self.win.slider_open_ev = lambda: self.on_slider_set(True)
        self.win.slider_closed_ev = lambda: self.on_slider_set(False)
        self.win.power_pressed = self.on_power_pressed
        self.win.power_long_pressed = self.on_power_long_pressed
        self.win.incoming_call = self.on_incoming_call
        self.win.remote_end_call = self.on_remote_end_call

    def _screen_name(self, screen: object) -> str:
        for attr in dir(self.ScreenId):
            if not attr.startswith("_") and getattr(self.ScreenId, attr) == screen:
                return attr
        return str(screen)

    def _log(self, msg: str) -> None:
        print(msg)
        self.win.last_event = msg

    def set_screen(self, name: str) -> None:
        screen = getattr(self.ScreenId, name, None)
        if screen is None:
            valid = [s for s in dir(self.ScreenId) if not s.startswith("_")]
            raise ValueError(f"Unknown screen '{name}'. Valid: {', '.join(valid)}")
        if self.win.active_screen != self.ScreenId.power_off:
            self.prev_screen = self.win.active_screen
        self.win.active_screen = screen
        self.win.active_screen_name = name
        self._log(f"[ui] screen -> {name}")

    def _enter_call_ended(self) -> None:
        self.set_screen("call_ended")
        self.call_ended_timer.start(
            slint.TimerMode.SingleShot,
            timedelta(milliseconds=1500),
            self._after_call_ended,
        )

    def _after_call_ended(self) -> None:
        if self.win.active_screen == self.ScreenId.call_ended:
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
        if self.win.active_screen == self.ScreenId.pairing:
            self.win.pairing_timed_out = True
            self._log("[bt] pairing timed out (120s)")

    # --- Real UI softkey & keypad handlers ---

    def on_soft_left(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.home:
            if self.win.paired_device_name:
                self.set_screen("unpair")
            else:
                self._start_pairing()
        elif cur == self.ScreenId.incoming_call:
            self._log("[hfp] ATA (answer)")
            self.set_screen("active_call")
        elif cur == self.ScreenId.active_call:
            self.win.speaker_mode = not self.win.speaker_mode
            sink = "SPEAKER" if self.win.speaker_mode else "EARPIECE"
            self._log(f"[audio] sink -> {sink}")
        elif cur == self.ScreenId.unpair:
            self.set_screen("home")
        elif cur == self.ScreenId.pairing and self.win.pairing_timed_out:
            self._start_pairing()
        elif cur == self.ScreenId.power_off:
            self._log("[sys] power off confirmed -> exiting desktop harness")
            slint.quit_event_loop()

    def on_soft_right(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.home:
            self.set_screen("power_off")
        elif cur == self.ScreenId.incoming_call:
            self._log("[hfp] AT+CHUP (reject)")
            self.set_screen("home")
        elif cur == self.ScreenId.active_call:
            self._log("[hfp] AT+CHUP (end)")
            self._enter_call_ended()
        elif cur == self.ScreenId.unpair:
            self._log(f"[bt] unpairing '{self.win.paired_device_name}'")
            self.win.paired_device_name = ""
            self.win.bt_connected = False
            self._start_pairing()
        elif cur == self.ScreenId.pairing:
            self.pairing_timer.stop()
            self.set_screen("home")
        elif cur == self.ScreenId.power_off:
            prev_name = self._screen_name(self.prev_screen)
            self.win.active_screen = self.prev_screen
            self.win.active_screen_name = prev_name
            self._log(f"[ui] power off cancelled -> {prev_name}")

    def on_pickup(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.home:
            self._log("[hfp] ATD; (redial/test call)")
            self.set_screen("active_call")
        elif cur == self.ScreenId.incoming_call:
            self._log("[hfp] ATA (answer)")
            self.set_screen("active_call")

    def on_hangup(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.incoming_call:
            self._log("[hfp] AT+CHUP (reject)")
            self.set_screen("home")
        elif cur == self.ScreenId.active_call:
            self._log("[hfp] AT+CHUP (end)")
            self._enter_call_ended()
        elif cur in (self.ScreenId.unpair, self.ScreenId.pairing, self.ScreenId.power_off):
            self.pairing_timer.stop()
            self.set_screen("home")

    # --- Simulated OS event handlers ---

    def on_bt_connect(self) -> None:
        if not self.win.paired_device_name:
            self.win.paired_device_name = self.default_paired_device
        self.win.bt_connected = True
        if self.win.active_screen == self.ScreenId.pairing:
            self.pairing_timer.stop()
            self.set_screen("home")
        self._log(f"[bt] connected ({self.win.paired_device_name})")

    def on_bt_disconnect(self) -> None:
        self.win.bt_connected = False
        if self.win.active_screen == self.ScreenId.active_call:
            self._enter_call_ended()
        elif self.win.active_screen == self.ScreenId.incoming_call:
            self.set_screen("home")
        self._log("[bt] disconnected")

    def on_bt_pair(self, name: str | None = None) -> None:
        device_name = name or self.default_paired_device
        self.win.paired_device_name = device_name
        self.win.bt_connected = True
        self.win.pairing_timed_out = False
        self.pairing_timer.stop()
        if self.win.active_screen in (self.ScreenId.pairing, self.ScreenId.unpair):
            self.set_screen("home")
        self._log(f"[bt] paired & connected '{device_name}'")

    def on_bt_unpair(self) -> None:
        prev = self.win.paired_device_name or "device"
        self.win.paired_device_name = ""
        self.win.bt_connected = False
        if self.win.active_screen == self.ScreenId.active_call:
            self._enter_call_ended()
        elif self.win.active_screen in (self.ScreenId.incoming_call, self.ScreenId.unpair):
            self.set_screen("home")
        self._log(f"[bt] unpaired '{prev}'")

    def on_bt_pair_timeout(self) -> None:
        self._on_pairing_timeout()

    def adjust_volume(self, delta: int, *, allow_any_screen: bool = False) -> None:
        if allow_any_screen or self.win.active_screen == self.ScreenId.active_call:
            self.win.volume_percent = max(0, min(100, self.win.volume_percent + delta))
            self._log(f"[audio] volume -> {self.win.volume_percent}%")

    def on_slider_set(self, is_open: bool) -> None:
        self.win.slider_open = is_open
        state_str = "OPEN" if is_open else "CLOSED"
        if is_open:
            if self.win.active_screen == self.ScreenId.sleep:
                self.set_screen("home")
            elif self.win.active_screen == self.ScreenId.incoming_call:
                self.set_screen("active_call")
        else:
            if self.win.active_screen == self.ScreenId.active_call:
                self._enter_call_ended()
        self._log(f"[hw] slider -> {state_str}")

    def on_power_pressed(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.incoming_call:
            self.win.ring_silenced = True
            self._log("[hw] power pressed -> ring silenced")
        elif cur == self.ScreenId.home:
            self.set_screen("sleep")
            self._log("[hw] power pressed -> sleep")
        elif cur == self.ScreenId.sleep:
            self.set_screen("home")
            self._log("[hw] power pressed -> wake")
        else:
            self._log(f"[hw] power pressed (no-op on {self._screen_name(cur)})")

    def on_power_long_pressed(self) -> None:
        self.set_screen("power_off")
        self._log("[hw] power long pressed -> power_off")

    def on_incoming_call(self, caller: str | None = None) -> None:
        self.win.caller_id = caller or self.default_caller
        self.win.ring_silenced = False
        self.set_screen("incoming_call")
        self._log(f"[hfp] incoming call ({self.win.caller_id})")

    def on_remote_end_call(self) -> None:
        if self.win.active_screen == self.ScreenId.active_call:
            self._enter_call_ended()
            self._log("[hfp] remote end -> call_ended")
        elif self.win.active_screen == self.ScreenId.incoming_call:
            self.set_screen("home")
            self._log("[hfp] remote cancel -> home")
        else:
            self._log("[hfp] remote end (no active call)")

    # --- Optional stdin CLI (kept for scripting / terminal use) ---

    def handle_command(self, line: str) -> None:
        parts = line.strip().split()
        if not parts:
            return
        cmd = parts[0].lower()
        if cmd in ("quit", "q", "exit"):
            slint.quit_event_loop()
        elif cmd == "ring":
            caller = " ".join(parts[1:]) if len(parts) > 1 else None
            self.on_incoming_call(caller)
        elif cmd == "silence":
            if self.win.active_screen == self.ScreenId.incoming_call:
                self.win.ring_silenced = True
                self._log("[ui] ring silenced")
        elif cmd == "end":
            self.on_remote_end_call()
        elif cmd == "slider":
            self.on_slider_set(not self.win.slider_open)
        elif cmd == "slider_open":
            self.on_slider_set(True)
        elif cmd == "slider_closed":
            self.on_slider_set(False)
        elif cmd == "bt":
            if self.win.bt_connected:
                self.on_bt_disconnect()
            else:
                self.on_bt_connect()
        elif cmd == "bt_connect":
            self.on_bt_connect()
        elif cmd == "bt_disconnect":
            self.on_bt_disconnect()
        elif cmd in ("pair", "pair_ok"):
            name = " ".join(parts[1:]) if len(parts) > 1 else None
            self.on_bt_pair(name)
        elif cmd == "unpair":
            self.on_bt_unpair()
        elif cmd == "timeout":
            self.on_bt_pair_timeout()
        elif cmd == "vol_up":
            self.adjust_volume(+5, allow_any_screen=True)
        elif cmd == "vol_down":
            self.adjust_volume(-5, allow_any_screen=True)
        elif cmd == "power_short":
            self.on_power_pressed()
        elif cmd == "power_long":
            self.on_power_long_pressed()
        elif cmd == "screen" and len(parts) == 2:
            try:
                self.set_screen(parts[1])
            except ValueError as exc:
                self._log(f"[err] {exc}")
        elif cmd == "help":
            print(
                "Commands: ring [caller] | silence | end | slider | slider_open | "
                "slider_closed | bt | bt_connect | bt_disconnect | pair [name] | "
                "unpair | timeout | vol_up | vol_down | power_short | power_long | "
                "screen <name> | quit"
            )
        else:
            self._log(f"[err] unknown command '{line.strip()}' (type 'help')")

    def _poll_commands(self) -> None:
        while not self.cmd_queue.empty():
            self.handle_command(self.cmd_queue.get_nowait())

    def _stdin_worker(self) -> None:
        if not sys.stdin or not sys.stdin.isatty():
            return
        print("Interactive CLI ready. Type 'help' for simulated hardware commands.")
        for line in sys.stdin:
            self.cmd_queue.put(line)

    def run(self) -> int:
        threading.Thread(target=self._stdin_worker, daemon=True).start()
        self.poll_timer.start(
            slint.TimerMode.Repeated,
            timedelta(milliseconds=50),
            self._poll_commands,
        )
        print(f"Loaded Desktop Workbench: {SIMULATOR_SLINT} (UI from {NOKIA_UI_DIR})")
        print(
            "Keys: Left=SoftLeft | Right=SoftRight | Enter=PickUp | "
            "Backspace/Esc=HangUp | Up/Down=Vol+/-"
        )
        try:
            self.win.run()
        except KeyboardInterrupt:
            pass
        return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run nokia-ui Slint screens on desktop for development."
    )
    p.add_argument(
        "--screen",
        default="home",
        choices=[
            "home",
            "sleep",
            "incoming_call",
            "active_call",
            "call_ended",
            "unpair",
            "pairing",
            "power_off",
        ],
        help="Initial screen to display (default: home)",
    )
    p.add_argument("--bt-connected", action="store_true", help="Start with BT connected")
    p.add_argument("--caller", default="+46 70 123 4567", help="Simulated caller ID")
    p.add_argument(
        "--paired-device",
        default="Pixel 8 Pro",
        help="Simulated paired device name (pass '' for unbonded)",
    )
    p.add_argument("--battery", type=int, default=85, help="Battery percent (0-100)")
    p.add_argument("--slider-closed", action="store_true", help="Start with slider closed")
    p.add_argument("--speaker", action="store_true", help="Start active call on speaker")
    p.add_argument("--volume", type=int, default=65, help="Initial call volume (0-100)")
    p.add_argument(
        "--pairing-timeout",
        action="store_true",
        help="Start pairing screen in timed-out state",
    )
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    harness = DesktopUiHarness(args)
    return harness.run()


if __name__ == "__main__":
    raise SystemExit(main())
