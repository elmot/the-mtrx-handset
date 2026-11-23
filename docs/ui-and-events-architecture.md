# Nokia 8110 4G (`nokia-argon`) Python + Slint UI Architecture

## 1. Display: Slint(Skia Software renderer) over Linux KMS 

- **UI Toolkit**: `main/py3-slint/APKBUILD` builds `py3-slint` for armv7 (musl) with `--features backend-linuxkms-noseat,renderer-skia`.
- **Launch** `nokia-ui.initd` launches `/usr/share/nokia-ui/main.py` with `SLINT_BACKEND="linuxkms-skia-software"`.

## 2. Hardware Buttons & Flip/Slider (`evdev`)

Per `qcom-msm8905-nokia-argon.dts`, the phone's physical inputs are exposed as Linux `/dev/input/event*` devices:
- **`gpio-keys`**:
  - `KEY_UP` (GPIO 90), `KEY_BACKSPACE` (GPIO 98)
  - `EV_SW`: `SW_KEYPAD_SLIDE` (GPIO 92, `GPIO_ACTIVE_HIGH` — `1` = slider open, `0` = slider closed)
  - the secondary flip sensor (flip fully open) on GPIO 36 is ignored

- **`gpio-matrix-keypad`**:
  - Standard keys: `KEY_0`–`KEY_9`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_ENTER`
  - High keycodes (`> 255` in DTS, remapped by `device-nokia-argon`'s `61-nokia-argon-keypad.hwdb` so XKB/libinput/Slint receive them):
    - `KEY_PICKUP_PHONE` (445) &rarr; `KEY_F13` (`f13`, 183 — Green Call key)
    - `KEY_NUMERIC_STAR` (522) &rarr; `KEY_KPASTERISK` (`kpasterisk`, 55 — `*` key)
    - `KEY_NUMERIC_POUND` (523) &rarr; `KEY_F14` (`f14`, 184 — `#` key)
    - `KEY_KBD_LCD_MENU1` (696) &rarr; `KEY_MENU` (`menu`, 139 — Left Softkey)
    - `KEY_KBD_LCD_MENU2` (697) &rarr; `KEY_ESC` (`esc`, 1 — Right Softkey)
- **PMIC inputs**:
  - `pm8941_pwrkey`: `KEY_POWER` (Power / Hang Up key)
  - `pm8941_resin`: `KEY_DOWN` (D-pad Down)

## 3. Bluetooth & HFP Events (`nokia-bt-hfp` + `org.bluez` over System D-Bus)

`device-nokia-argon` includes `bluez` and `nokia-bt-hfp`:
- **Call & HFP Connection State **:
  - `nokia-bt-hfp` exposes a UNIX control socket (`$XDG_RUNTIME_DIR/nokia-bt-hfp.sock`, `0666`) queried on startup via `status` (`connected`, `call`, `callsetup`, `caller`).
  - On RFCOMM connect/disconnect and HFP call state changes, `nokia-bt-hfp` emits `org.nokia.BtHfp.CallState(connected: bool, state: str, caller_id: str)` (`"idle"`, `"incoming"`, `"active"`) on the system D-Bus.
  - `nokia-ui/main.py` reads initial state from the UNIX socket, subscribes to `org.nokia.BtHfp` signals on the system bus via `dbus-monitor --system` in a background thread, and polls a `queue.SimpleQueue` every 50 ms using `slint.Timer`.
- **Pairing & Adapter Management (`org.bluez`, _TBD_)**:
  - BlueZ pairing/unpairing (`RemoveDevice`, `Discoverable`, `Pairable`, `GetManagedObjects`, and `PropertiesChanged` on `/org/bluez/...`) to be driven over the same system D-Bus (e.g., via `dbus-next` or `bluetoothctl` / D-Bus calls)