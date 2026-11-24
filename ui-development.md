# Nokia 8110 4G (`nokia-argon`) UI Development Guide

This guide details how to develop, preview, debug, and deploy the user interface for the Nokia 8110 4G (HMD `nokia-argon`) running THE-MTRX-HEADSET.

The Nokia UI is built with [Slint](https://slint.dev/) and driven by Python on top of the Linux DRM/KMS backend (`linuxkms-skia-software`). 
For rapid iteration without touching hardware, the repository includes a desktop simulation harness that emulates phone hardware events, telephony, and Bluetooth states.

---

## 1. Architecture & File Layout

The user interface code and its desktop tooling are organized across two main directories:

- **Production UI Package (`pmaports/main/nokia-ui/`)**:
  - Contains all production code packaged into the Alpine/postmarketOS `.apk`.
  - [`main.py`](pmaports/main/nokia-ui/main.py): Production Python service running on the phone. Communicates with Bluetooth HFP daemon (`nokia-bt-hfp`), handles keypad events, monitors battery levels, and transitions screens.
  - [`main.slint`](pmaports/main/nokia-ui/main.slint) & [`main_view.slint`](pmaports/main/nokia-ui/main_view.slint): Top-level Slint containers targeting the 240×320 QVGA display.
  - [`screen_*.slint`](pmaports/main/nokia-ui/): Individual screen components (`screen_home.slint`, `screen_incoming_call.slint`, `screen_active_call.slint`, `screen_call_ended.slint`, `screen_pairing.slint`, `screen_unpair.slint`, `screen_power_off.slint`).
  - [`ui_common.slint`](pmaports/main/nokia-ui/ui_common.slint): Shared design tokens, colors, status bar, and softkey bar components.
  - [`APKBUILD`](pmaports/main/nokia-ui/APKBUILD): Alpine packaging definition for `nokia-ui`.
  - [`nokia-ui.initd`](pmaports/main/nokia-ui/nokia-ui.initd): OpenRC service script installed on the device.

- **Desktop Simulator & Harness (`ui-development/`)**:
  - [`desktop.py`](ui-development/desktop.py): Desktop development harness and state machine. Loads production screens and provides mock OS/Bluetooth/telephony events.
  - [`simulator.slint`](ui-development/simulator.slint): Desktop development window combining the real 240×320 `MainView` with an interactive OS Event Simulator control panel.

> [!NOTE]
> All UI styling, layout, and logic changes intended for the phone must be made in **[`pmaports/main/nokia-ui/`](pmaports/main/nokia-ui/)**. The files in `ui-development/` import components directly from `pmaports/main/nokia-ui/`.

---

## 2. Tools & Environment Setup

### Required Tools

- **Python 3.10+**
- **Slint Python Module**: `slint>=1.19.0`
- **Python IDE**: PyCharm, VS Code, or your preferred IDE.

Activate the repository virtual environment (or set up one):

```bash
# Using the repository activate script:
source activate.sh

# Or create/activate your own venv:
python3 -m venv venv
source venv/bin/activate
pip install "slint>=1.19.0"
```

### Recommended Tools

- **Slint Viewer (`slint-viewer`)**:
  Standalone preview tool with live reloading support.
  ```bash
  cargo install slint-viewer
  ```
- **Slint LSP Server (`slint-lsp`)**:
  Language Server Protocol binary providing autocompletion, real-time error checking, and code navigation for `.slint` files.
  ```bash
  cargo install slint-lsp
  ```
- **PyCharm + Slint Plugin** (or **VS Code + Slint Extension**):
  - Provides full syntax highlighting, code completion, and integrated live component previews alongside your editor.
  - In PyCharm / IntelliJ: Install the **Slint** plugin from JetBrains Marketplace (*Settings &rarr; Plugins*).

---

## 3. The Desktop Simulator

Running the desktop development module opens a dual-panel interface:

1. **Left Panel (Phone Screen)**:
   - Pixel-accurate 240×320 display rendering the real production `MainView` Slint component.
   - Shows status bar (battery percentage, Bluetooth connectivity, slider state), screen contents, and softkey actions.
2. **Right Panel (OS Event Simulator)**:
   - **Direct Screen Switcher**: Jump instantly to any screen (`home`, `sleep`, `incoming_call`, `active_call`, `call_ended`, `unpair`, `pairing`, `power_off`).
   - **Bluetooth Controls**: Toggle connected/disconnected state, simulate pairing mode, test pairing timeout, or trigger unpair.
   - **Telephony Controls**: Simulate incoming calls, answer, reject, or remote end call.
   - **Hardware Controls**: Toggle slider open/closed, adjust volume up/down, simulate power button short press and 2-second long press.
   - **Status & Event Log**: Live readout of active screen, volume, battery level, speakerphone mode, and recent event logs.

---

## 4. Development Scenarios

### Scenario 1: Open UI and Test

To launch the desktop simulator with default settings:

```bash
source activate.sh
python3 ui-development/desktop.py
```
__todo__ vscode launch, pycharm run config

#### Keyboard Controls in Simulator
The simulator window accepts direct keyboard input to emulate phone keypad buttons:
- **Left Softkey**: `F1` or keypad left
- **Right Softkey**: `F2` or keypad right
- **Call / Pickup**: `Enter`
- **Hangup / Back**: `Escape`
- **Volume Up / Down**: `+` / `-`

#### Launching into Specific States
You can pass command-line arguments to `desktop.py` to test specific scenarios immediately without clicking through the UI:

```bash
# Start directly on incoming call screen with a custom caller ID:
python3 ui-development/desktop.py --screen incoming_call --caller "+1 (555) 234-5678"

# Start on active call with speakerphone enabled and volume at 80%:
python3 ui-development/desktop.py --screen active_call --speaker --volume 80

# Start in pairing mode with pairing already timed out:
python3 ui-development/desktop.py --screen pairing --pairing-timeout

# Start with slider closed and low battery:
python3 ui-development/desktop.py --slider-closed --battery 15
```

---

### Scenario 2: Change UI Files and Test on the Fly

When tweaking UI layouts, typography, padding, or colors:

1. **Live Preview with `slint-viewer` (Hot-Reloading)**:
__todo__ vscode launch, pycharm run config
   Launch `slint-viewer` pointing to the Slint files in `pmaports/main/nokia-ui`:
   ```bash
   slint-viewer -I pmaports/main/nokia-ui pmaports/main/nokia-ui/main_view.slint --auto-reload
   ```
   Every time you save a `.slint` file (e.g. [`screen_incoming_call.slint`](pmaports/main/nokia-ui/screen_incoming_call.slint) or [`ui_common.slint`](pmaports/main/nokia-ui/ui_common.slint)), the preview window refreshes immediately without restarting.

2. **Using IDE Preview**:
   - In PyCharm or VS Code with the Slint plugin installed, open any `.slint` file in [`pmaports/main/nokia-ui/`](pmaports/main/nokia-ui/).
   - Click the **Preview** icon in the editor toolbar to view the component rendered live side-by-side with your code.

3. **Verifying in the Desktop Simulator**:
__todo__ update on the fly?
   After updating the `.slint` file, re-run `python3 ui-development/desktop.py` to ensure interactions, callbacks, and state transitions behave correctly.

---

### Scenario 3: Debug the Python Part

To debug the state machine, timers, callbacks, and event processing in Python:

__todo__ vscode launch, pycharm run config
1. **Configure Your IDE Run/Debug Configuration**:
   - **Script Path**: `ui-development/desktop.py`
   - **Working Directory**: Project root
   - **Python Interpreter**: Virtual environment (`./venv/bin/python3`)
2. **Set Breakpoints**:
   - Open [`ui-development/desktop.py`](ui-development/desktop.py).
   - Set breakpoints in key event handlers:
     - `on_incoming_call()`: Simulating incoming calls and caller ID propagation.
     - `on_pickup()` / `on_hangup()`: Call lifecycle transitions.
     - `on_slider_set()`: Automatic answer / hangup when the slider opens or closes.
     - `adjust_volume()`: Volume clamping and UI synchronization.
     - `_after_call_ended()` / `_pairing_timeout()`: Timer expiration callbacks.
3. **Run with Debugger**:
   - Click **Debug** in PyCharm / VS Code.
   - Trigger the relevant action using the simulator buttons or keypad shortcuts to hit your breakpoint, inspect variables, and step through execution.

> [!TIP]
> If you are modifying the production logic in [`pmaports/main/nokia-ui/main.py`](pmaports/main/nokia-ui/main.py), note that `desktop.py` is an architectural mirror of `main.py` tailored for desktop mock events. Keep callback signatures and screen naming consistent between both files.

---

### Scenario 4: Deploy Temporarily to Mobile via SSH

__todo__ vscode launch, pycharm run config
For fast on-device verification without waiting for a full package rebuild, deploy modified files directly over SSH (USB networking default IP: `172.16.42.1`).

1. **Stop the Running UI Service on Phone**:
   ```bash
   ssh morpheus@172.16.42.1 "sudo rc-service nokia-ui stop"
   ```

2. **Copy Updated Python and Slint Files**:
   Copy your modified files directly into the phone's production directory (`/usr/share/nokia-ui/`):
   ```bash
   scp pmaports/main/nokia-ui/*.py pmaports/main/nokia-ui/*.slint morpheus@172.16.42.1:/usr/share/nokia-ui/
   ```

3. **Option A: Run Interactively to Inspect Logs**:
   Launch `main.py` directly from an SSH terminal to see standard output, exceptions, and D-Bus logs:
   ```bash
   ssh -t morpheus@172.16.42.1 "export SLINT_BACKEND=linuxkms-skia-software; export XDG_RUNTIME_DIR=/run/user/10000; sudo python3 /usr/share/nokia-ui/main.py"
   ```

4. **Option B: Restart the Background Service**:
   ```bash
   ssh morpheus@172.16.42.1 "sudo rc-service nokia-ui restart"
   ```
   Check service status:
   ```bash
   ssh morpheus@172.16.42.1 "sudo rc-service nokia-ui status"
   ```

---

### Scenario 5: Build APK and Deploy Permanently

__todo__ vscode launch, pycharm build config
Once your UI changes are complete and tested, package them into an Alpine package (`.apk`) using `pmbootstrap`.

1. **Update [`pmaports/main/nokia-ui/APKBUILD`](pmaports/main/nokia-ui/APKBUILD)**:
   - If you added new `.slint` or asset files, add them to `source=`.
   - Increment `pkgrel` (e.g., `pkgrel=11` &rarr; `pkgrel=12`) so `apk` recognizes the update over previous builds.

2. **Update Package Checksums**:
   ```bash
   source activate.sh
   pmbootstrap checksum nokia-ui
   ```

3. **Build the Package for `armv7`**:
   ```bash
   pmbootstrap build nokia-ui --arch=armv7 --force
   ```

4. **Sideload onto Connected Phone**:
   Install the freshly built package onto the running device without reflashing:
   ```bash
   pmbootstrap sideload nokia-ui
   ```

5. **Verify Installation**:
   Restart the service on the phone to load the new binary package:
   ```bash
   ssh morpheus@172.16.42.1 "sudo rc-service nokia-ui restart"
   ```
