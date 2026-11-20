# `mock_bt_phone_ag.py` — Mock Mobile Phone (Bluetooth HFP-AG)

A test utility that emulates a **mobile phone (Bluetooth HFP-AG / HSP-AG + AVRCP media button listener)** to test Bluetooth headsets (including `nokia-argon` running `nokia-bt-hfp`). It pairs/connects, checks headset battery, mocks incoming calls with in-band ringtone (`ringtone.wav`), plays call-partner audio (`voice.wav`), records the headset's microphone response to `/tmp/mock_phone_rec_*.wav`, and logs pickup/hangup, volume, and media-button events.

## GUI & CLI
- **GTK3 GUI** opens automatically when `DISPLAY`/`WAYLAND_DISPLAY` is set, with buttons, live telemetry, volume sliders, file picker, and an embedded log/command box.
- **Interactive CLI** (`stdin`) works simultaneously alongside the GUI, or standalone with `--no-gui`.

```bash
./mock_bt_phone_ag.py --mac 02:00:34:40:12:1E
./mock_bt_phone_ag.py --no-gui --mac 02:00:34:40:12:1E
```

Place optional 8 kHz mono 16-bit PCM WAV files `ringtone.wav` and `voice.wav` in `scripts/` (other formats are auto-converted via `ffmpeg`).

## Interactive Commands
| Command | Action |
| :--- | :--- |
| `status` | Print connection, HFP/SCO, call, volume, and battery state |
| `scan on\|off` | Start or stop Bluetooth discovery |
| `pair [MAC]` / `unpair [MAC]` | Pair + trust, or remove device from BlueZ |
| `connect [MAC]` / `disconnect [MAC]` | Connect or disconnect HFP RFCOMM & BlueZ device |
| `battery [MAC]` | Query BlueZ `Battery1` and HFP AT battery level |
| `ring [number]` | Mock incoming call (`+CIEV`, `RING`, `+CLIP`, in-band `ringtone.wav`) |
| `answer` / `hangup` | Answer (plays `voice.wav` & records mic) or end call |
| `sco on\|off` | Manually open or close the `BTPROTO_SCO` voice link |
| `vgs <0-15>` / `vgm <0-15>` | Push speaker (`+VGS`) or mic (`+VGM`) gain to headset |
| `ringtone <file>` / `voice <file>` | Load custom ringtone or call-partner audio file |
| `playrec` | Play back the most recent recorded mic `.wav` |
| `quit` | Disconnect, restore WirePlumber, and exit |

## OS Dependencies

### Linux (Native — Supported)
- **Debian / Ubuntu**:
  ```bash
  sudo apt install python3 python3-gi python3-gi-cairo gir1.2-gtk-3.0 \
      bluez ffmpeg pipewire-bin alsa-utils
  ```
- **Arch / Fedora**: install equivalent `python-gobject`, `gtk3`, `bluez` / `bluez-utils`, `ffmpeg`, `pipewire` / `alsa-utils`.
- **Permissions**: add your user to the `input` group (optional, for `/dev/input/event*` AVRCP media button capture).

### Windows (Not Supported Natively)
- Native Windows is **not supported** because this tool uses Linux-only **BlueZ D-Bus**, raw **`AF_BLUETOOTH` (`BTPROTO_RFCOMM` / `BTPROTO_SCO`)** kernel sockets, and **`/dev/input/event*`**.
- **Workaround on Windows**: run inside a **Linux VM** (VirtualBox/VMware) or **WSL2** with a USB Bluetooth dongle attached via [`usbipd-win`](https://github.com/dorssel/usbipd-win) and a Linux kernel with BlueZ (`bluetoothd`) + `WSLg` for the GTK3 GUI.
