# Nokia 8110 4G (`nokia-argon`) Python + Slint UI Architecture

## 1. Display: Slint (Skia Software renderer) over Linux KMS

- **UI Toolkit**: `main/py3-slint/APKBUILD` builds `py3-slint` for armv7 (musl) with `--features backend-linuxkms-libinput,renderer-skia`.
- **Launch**: `nokia-ui.initd` launches `/usr/share/nokia-ui/main.py` after `nokia-bt-hfp` with `SLINT_BACKEND="linuxkms-skia-software"`.
- **Display Power**: `nokia-ui/main.py` controls `/sys/class/backlight/backlight/brightness` directly for wake/sleep and a 20 s screen inactivity timeout while the slider is closed.

## 2. Hardware Buttons, Slider & Vibrator (`evdev`)

Per `qcom-msm8905-nokia-argon.dts`, physical inputs are exposed as Linux `/dev/input/event*` devices. On `nokia-argon`, `nokia-ui/main.py` reads `keypad`, `gpio-keys`, and `pm8941_resin` directly via `evdev` (and sets `MainWindow.use-slint-keys = false` so Slint `FocusScope` does not double-dispatch keys; desktop preview leaves `use-slint-keys = true`):

- **`gpio-keys`**:
  - `KEY_UP` (GPIO 90) &rarr; Volume Up (`vol_up`)
  - `KEY_BACKSPACE` (GPIO 98) &rarr; Hang Up / Back (`hangup`)
  - `EV_SW`: `SW_KEYPAD_SLIDE` (GPIO 92, `GPIO_ACTIVE_HIGH` — `1` = slider open, `0` = slider closed)
  - Secondary flip sensor (GPIO 36) is ignored
- **`gpio-matrix-keypad` (`keypad`)**:
  - High keycodes (`> 255` in DTS, remapped by `61-nokia-argon-keypad.hwdb`; `main.py` accepts both raw and remapped codes):
    - `KEY_PICKUP_PHONE` (445) / `KEY_F13` (183) &rarr; Green Call (`pickup`)
    - `KEY_KBD_LCD_MENU1` (696) / `KEY_MENU` (139) &rarr; Left Softkey (`soft_left`)
    - `KEY_KBD_LCD_MENU2` (697) / `KEY_ESC` (1) &rarr; Right Softkey (`soft_right`)
- **PMIC inputs**:
  - `pm8941_resin`: `KEY_DOWN` &rarr; Volume Down (`vol_down`)
  - `pm8941_pwrkey`: `KEY_POWER` &rarr; Mapping TBD (`on_power_short` / `on_power_long` hooks ready in `main.py`)
  - `pm8909_vib`: PM8909 haptic vibrator (`/dev/input/by-path/platform-200f000.spmi-platform-200f000.spmi:pmic@1:vibrator@c000-event`) driven via `EV_FF` / `FF_RUMBLE` for incoming call alerts.

## 3. Bluetooth & HFP Events (`nokia-bt-hfp` + `org.bluez` over System D-Bus)

`device-nokia-argon` includes `bluez` and `nokia-bt-hfp` (advertising BlueZ alias `"THE MTRX HANDSET"`):

- **UNIX Control Socket** (`$XDG_RUNTIME_DIR/nokia-bt-hfp.sock`, `0666`):
  - `status`, `bt-info` (initial sync of HFP, audio, slider, and BlueZ bonding/device name)
  - Call & audio control: `answer`, `hangup`, `redial`, `dial <num>`, `earpiece`, `speaker`, `headphones`, `vgs <0-15>`, `vgm <0-15>`, `mute`, `unmute`
  - BlueZ pairing control: `pair-start` (120 s discoverable/pairable window), `pair-stop`, `unpair` (`RemoveDevice` on all bonded peers)
- **System D-Bus Signals (`org.nokia.BtHfp` on `/org/nokia/BtHfpProfile`)**:
  - `CallState(connected: bool, state: str, caller_id: str)` (`"idle"`, `"incoming"`, `"active"`; emitted immediately on RFCOMM connect/disconnect, SLC ready, `+CIEV`, `+CLIP`, and `RING`)
  - `AudioState(output: str, vgs: int32, vgm: int32, muted: bool)` (emitted on sink toggles, 3.5 mm jack insert/remove, and local/remote `+VGS`/`+VGM`)
  - `SliderState(open: bool)` (emitted on `SW_KEYPAD_SLIDE` change; `nokia-bt-hfp` also drives `white:kbd_backlight` and instant `ATA`/`AT+CHUP` in C)
  - `BondState(bonded: bool, device_name: str, device_addr: str, pairing_active: bool)`
  - `PairingSuccess(device_name: str, device_addr: str)`
- `nokia-ui/main.py` subscribes to `org.nokia.BtHfp` on the system bus via `dbus-monitor --system` in a background thread and drains a `queue.SimpleQueue` every 50 ms using `slint.Timer`.