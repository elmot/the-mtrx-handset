# Nokia 8110 4G (`nokia-argon`) Python + Slint UI Architecture

## 1. Display: Slint(Skia Software renderer) over Linux KMS 

- **UI Toolkit**: `py3-slint` built for `armv7` (`musl`)

## 2. Hardware Buttons & Flip/Slider (`evdev`)

__TBD correct__
Per `qcom-msm8905-nokia-argon.dts`, the phone's physical inputs are exposed as Linux `/dev/input/event*` devices:
- **`gpio-keys`**:
  - `KEY_UP`, `KEY_BACKSPACE`
  - `EV_SW`: `SW_LID` (GPIO 92, flip/slider closed) and `SW_KEYPAD_SLIDE` (GPIO 36, flip/slider open)
- **`gpio-matrix-keypad`**:
  - Standard keys: `KEY_0`–`KEY_9`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_ENTER`
  - High keycodes (`> 255`): `KEY_PICKUP_PHONE` (445), `KEY_NUMERIC_STAR` (522), `KEY_NUMERIC_POUND` (523), `KEY_KBD_LCD_MENU1` (698), `KEY_KBD_LCD_MENU2` (699)
- **PMIC power key**: `KEY_POWER`

## 3. Bluetooth Events (`org.bluez` over System D-Bus)
__TBD correct__
`device-nokia-argon` includes `bluez`. In Python, `py3-dbus-next` connects to the system D-Bus (`org.bluez`) in the same background `asyncio` loop:
- Reads initial state via `org.freedesktop.DBus.ObjectManager.GetManagedObjects`.
- Subscribes to `InterfacesAdded`, `InterfacesRemoved`, and `org.freedesktop.DBus.Properties.PropertiesChanged` on `/org/bluez/...` to receive live adapter power/discovery state and device connect/disconnect/RSSI events.

__TBD correct__
## 4. Prepared `pmaports` Test Package: `main/argon-ui-tests`

Location: `pmaports-bananaphone/main/argon-ui-tests/`

It provides a single `240x320` Slint + Python test app (`/usr/bin/argon-ui-test`) and `/etc/cage-ui/50-argon-ui-tests.sh` covering all 3 smoke tests:
1. **Slint UI rendering**: Draws a `240x320` dark UI with live tick indicator and status cards.
2. **Key & Flip detection**: Reads `/dev/input/event*` via `py3-evdev` and displays the latest button press/release and `SW_LID`/`SW_KEYPAD_SLIDE` state.
3. **Bluetooth event detection**: Listens to `org.bluez` on the system D-Bus via `py3-dbus-next` and displays live adapter/device events.

### Build & Sideload
```bash
# After py3-slint is built:
pmbootstrap build argon-ui-tests --arch=armv7
pmbootstrap sideload argon-ui-tests
```

### Headless CLI Verification (over SSH, without Cage)
```bash
argon-ui-test --headless
```
