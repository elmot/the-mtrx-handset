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
MAIN_SLINT = NOKIA_UI_DIR / "main.slint"


class DesktopUiHarness:
    """Drives nokia-ui's MainWindow with PoC state-machine transitions."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.app = slint.load_file(str(MAIN_SLINT))
        self.ScreenId = self.app.ScreenId
        self.win = self.app.MainWindow()
        self.cmd_queue: queue.SimpleQueue[str] = queue.SimpleQueue()
        self.prev_screen = self.ScreenId.home

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

        # Single-shot timers for CALL_ENDED (1.5s) and PAIRING timeout (120s)
        self.call_ended_timer = slint.Timer()
        self.pairing_timer = slint.Timer()
        self.poll_timer = slint.Timer()

        # Bind Slint callbacks
        self.win.soft_left = self.on_soft_left
        self.win.soft_right = self.on_soft_right
        self.win.pickup_pressed = self.on_pickup
        self.win.hangup_pressed = self.on_hangup
        self.win.vol_up = lambda: self.adjust_volume(+5)
        self.win.vol_down = lambda: self.adjust_volume(-5)

    def set_screen(self, name: str) -> None:
        screen = getattr(self.ScreenId, name, None)
        if screen is None:
            valid = [s for s in dir(self.ScreenId) if not s.startswith("_")]
            raise ValueError(f"Unknown screen '{name}'. Valid: {', '.join(valid)}")
        if self.win.active_screen != self.ScreenId.power_off:
            self.prev_screen = self.win.active_screen
        self.win.active_screen = screen
        print(f"[ui] screen -> {name}")

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
            print("[ui] pairing timed out (120s)")

    def on_soft_left(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.home:
            if self.win.paired_device_name:
                self.set_screen("unpair")
            else:
                self._start_pairing()
        elif cur == self.ScreenId.incoming_call:
            print("[hfp] ATA (answer)")
            self.set_screen("active_call")
        elif cur == self.ScreenId.active_call:
            self.win.speaker_mode = not self.win.speaker_mode
            sink = "SPEAKER" if self.win.speaker_mode else "EARPIECE"
            print(f"[audio] sink -> {sink}")
        elif cur == self.ScreenId.unpair:
            self.set_screen("home")
        elif cur == self.ScreenId.pairing and self.win.pairing_timed_out:
            self._start_pairing()
        elif cur == self.ScreenId.power_off:
            print("[sys] power off confirmed -> exiting desktop harness")
            slint.quit_event_loop()

    def on_soft_right(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.home:
            self.set_screen("power_off")
        elif cur == self.ScreenId.incoming_call:
            print("[hfp] AT+CHUP (reject)")
            self.set_screen("home")
        elif cur == self.ScreenId.active_call:
            print("[hfp] AT+CHUP (end)")
            self._enter_call_ended()
        elif cur == self.ScreenId.unpair:
            print(f"[bt] unpairing '{self.win.paired_device_name}'")
            self.win.paired_device_name = ""
            self.win.bt_connected = False
            self._start_pairing()
        elif cur == self.ScreenId.pairing:
            self.pairing_timer.stop()
            self.set_screen("home")
        elif cur == self.ScreenId.power_off:
            self.win.active_screen = self.prev_screen
            print(f"[ui] power off cancelled -> {self.prev_screen}")

    def on_pickup(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.home:
            print("[hfp] ATD; (redial/test call)")
            self.set_screen("active_call")
        elif cur == self.ScreenId.incoming_call:
            print("[hfp] ATA (answer)")
            self.set_screen("active_call")

    def on_hangup(self) -> None:
        cur = self.win.active_screen
        if cur == self.ScreenId.incoming_call:
            print("[hfp] AT+CHUP (reject)")
            self.set_screen("home")
        elif cur == self.ScreenId.active_call:
            print("[hfp] AT+CHUP (end)")
            self._enter_call_ended()
        elif cur in (self.ScreenId.unpair, self.ScreenId.pairing, self.ScreenId.power_off):
            self.pairing_timer.stop()
            self.set_screen("home")

    def adjust_volume(self, delta: int) -> None:
        if self.win.active_screen == self.ScreenId.active_call:
            self.win.volume_percent = max(0, min(100, self.win.volume_percent + delta))
            print(f"[audio] volume -> {self.win.volume_percent}%")

    def handle_command(self, line: str) -> None:
        parts = line.strip().split()
        if not parts:
            return
        cmd = parts[0].lower()
        if cmd in ("quit", "q", "exit"):
            slint.quit_event_loop()
        elif cmd == "ring":
            self.win.caller_id = " ".join(parts[1:]) if len(parts) > 1 else "+46 70 123 4567"
            self.win.ring_silenced = False
            self.set_screen("incoming_call")
        elif cmd == "silence":
            if self.win.active_screen == self.ScreenId.incoming_call:
                self.win.ring_silenced = True
                print("[ui] ring silenced")
        elif cmd == "end":
            if self.win.active_screen == self.ScreenId.active_call:
                self._enter_call_ended()
            elif self.win.active_screen == self.ScreenId.incoming_call:
                self.set_screen("home")
        elif cmd == "slider":
            self.win.slider_open = not self.win.slider_open
            print(f"[hw] slider_open -> {self.win.slider_open}")
            if self.win.slider_open:
                if self.win.active_screen == self.ScreenId.sleep:
                    self.set_screen("home")
                elif self.win.active_screen == self.ScreenId.incoming_call:
                    self.set_screen("active_call")
            else:
                if self.win.active_screen == self.ScreenId.active_call:
                    self._enter_call_ended()
        elif cmd == "bt":
            self.win.bt_connected = not self.win.bt_connected
            print(f"[bt] connected -> {self.win.bt_connected}")
        elif cmd == "pair_ok":
            name = " ".join(parts[1:]) if len(parts) > 1 else "Pixel 8 Pro"
            self.win.paired_device_name = name
            self.win.bt_connected = True
            self.pairing_timer.stop()
            self.set_screen("home")
        elif cmd == "timeout":
            self._on_pairing_timeout()
        elif cmd == "power_short":
            if self.win.active_screen == self.ScreenId.incoming_call:
                self.win.ring_silenced = True
            elif self.win.active_screen == self.ScreenId.home:
                self.set_screen("sleep")
            elif self.win.active_screen == self.ScreenId.sleep:
                self.set_screen("home")
        elif cmd == "power_long":
            self.set_screen("power_off")
        elif cmd == "screen" and len(parts) == 2:
            try:
                self.set_screen(parts[1])
            except ValueError as exc:
                print(f"[err] {exc}")
        elif cmd == "help":
            print(
                "Commands: ring [caller] | silence | end | slider | bt | "
                "pair_ok [name] | timeout | power_short | power_long | "
                "screen <name> | quit"
            )
        else:
            print(f"[err] unknown command '{line.strip()}' (type 'help')")

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
        print(f"Loaded {MAIN_SLINT}")
        print(
            "Keys: Left=SoftLeft | Right=SoftRight | Enter=PickUp | "
            "Backspace/Esc=HangUp | Up/Down=Vol+/-"
        )
        self.win.run()
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
