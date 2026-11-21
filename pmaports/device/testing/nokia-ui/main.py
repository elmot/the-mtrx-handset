#!/usr/bin/env python3
from datetime import timedelta
import os
import socket
import slint

HERE = os.path.dirname(os.path.abspath(__file__))
app = slint.load_file(os.path.join(HERE, "main.slint"))
win = app.Demo()

xdg = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
sock_path = os.path.join(xdg, "nokia-bt-hfp.sock")


def poll_hfp_once():
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            s.connect(sock_path)
            s.sendall(b"status\n")
            data = s.recv(512).decode("utf-8", errors="replace").strip()
        kv = dict(item.split("=", 1) for item in data.split() if "=" in item)
        win.bt_connected = kv.get("connected") == "1"
        call = kv.get("call") == "1"
        callsetup = kv.get("callsetup", "0")
        caller = kv.get("caller", "-")
        if call:
            win.call_state = "active"
        elif callsetup == "1":
            win.call_state = "incoming"
        else:
            win.call_state = "idle"
        win.caller_id = "" if caller == "-" else caller
    except OSError:
        pass


timer = slint.Timer()
timer.start(slint.TimerMode.Repeated, timedelta(milliseconds=300), poll_hfp_once)
poll_hfp_once()
win.run()
