#!/usr/bin/env python3
from datetime import timedelta
import os
import queue
import re
import socket
import subprocess
import threading
import slint

HERE = os.path.dirname(os.path.abspath(__file__))
app = slint.load_file(os.path.join(HERE, "main.slint"))
win = app.Demo()
ev_queue = queue.SimpleQueue()


def read_initial_status():
    xdg = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    sock_path = os.path.join(xdg, "nokia-bt-hfp.sock")
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(0.2)
            s.connect(sock_path)
            s.sendall(b"status\n")
            data = s.recv(512).decode("utf-8", errors="replace").strip()
        kv = dict(item.split("=", 1) for item in data.split() if "=" in item)
        connected = kv.get("connected") == "1"
        call = kv.get("call") == "1"
        callsetup = kv.get("callsetup", "0")
        caller = kv.get("caller", "-")
        state = "active" if call else ("incoming" if callsetup == "1" else "idle")
        ev_queue.put((connected, state, "" if caller == "-" else caller))
    except OSError:
        pass


def start_dbus_listener():
    proc = subprocess.Popen(
        ["dbus-monitor", "--system", "type='signal',interface='org.nokia.BtHfp'"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    vals = []
    for line in proc.stdout:
        s = line.strip()
        if s.startswith("boolean "):
            vals = [s.split()[1] == "true"]
        elif s.startswith("string ") and vals:
            vals.append(s.split('"', 2)[1])
            if len(vals) == 3:
                ev_queue.put(tuple(vals))
                vals = []


def drain_events():
    while not ev_queue.empty():
        connected, state, caller = ev_queue.get_nowait()
        win.bt_connected = connected
        win.call_state = state
        win.caller_id = caller


read_initial_status()
threading.Thread(target=start_dbus_listener, daemon=True).start()
timer = slint.Timer()
timer.start(slint.TimerMode.Repeated, timedelta(milliseconds=50), drain_events)
drain_events()
win.run()
