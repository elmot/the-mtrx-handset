# Nokia 8110 4G (`nokia-argon`) Audio Hardware & Mainline Bring-Up Findings

**Target:** Nokia 8110 4G (`nokia-argon`, Qualcomm MSM8905/MSM8909 + PM8909 PMIC)  
**OS / Kernel:** Nura OS `v26.06` (`linux-postmarketos-qcom-msm8916`, Linux `6.12.1-msm8916`, `armv7`)  
**Constraint:** Pure Device Tree + ALSA mixer configuration (zero C code changes).

---

## 1. Hardware Audio Architecture

The Nokia 8110 4G uses the **PM8909** internal analog codec (`qcom,pm8916-wcd-analog-codec`) paired with the **MSM8909** LPASS digital codec (`qcom,msm8916-wcd-digital-codec`) over a 6-pin PDM bus (`gpio59`–`gpio64`, `cdc_pdm0`) and Hexagon ADSP (`qcom,apr-v2` / QDSP6 AFE/ASM/Routing):

| Physical Endpoint | Codec Path | Mainline Status | Verification Result |
|---|---|---|---|
| **Loudspeaker** (rear grille) | `PRI_MI2S_RX` &rarr; `RX3` &rarr; `SPK_OUT` | **Working** | Audibly confirmed (loud, clear 659 Hz tone) |
| **Earpiece** (front top receiver) | `PRI_MI2S_RX` &rarr; `RX1` &rarr; `EAR` | **Working** | Audibly confirmed (clear 523 Hz tone) |
| **3.5mm Stereo Headphones** | `PRI_MI2S_RX` &rarr; `RX1` (`HPHL`) / `RX2` (`HPHR`) | **Working** | Audibly confirmed true stereo (isolated Left 440 Hz / Right 880 Hz) |
| **Primary Mic** (bottom slider) | `AMIC1` &rarr; `ADC1` &rarr; `DEC1` &rarr; `TERT_MI2S_TX` | **Working** | Peak `32768` (`0.0 dBFS`), RMS `−39.4 dBFS` on claps (headset unplugged) |
| **Secondary Mic** (top/rear ambient) | `AMIC3` &rarr; `ADC2 (INP3)` &rarr; `DEC1` &rarr; `TERT_MI2S_TX` | **Working** | Peak `22208` (`−3.4 dBFS`), RMS `−41.2 dBFS` on claps (headset unplugged) |
| **3.5mm Headset Mic** | `AMIC2` &rarr; `ADC2 (INP2)` &rarr; `DEC1` &rarr; `TERT_MI2S_TX` | **Closed / Silent** | Jack insert detected (`/dev/input/event5`, `0x0014`), but capture remains at noise floor (`−73 dBFS`) |

---

## 2. Key Device Tree & Clocking Findings

Full patch: [`docs/nokia-argon-audio-dts.patch`](nokia-argon-audio-dts.patch) (modifying `qcom-msm8909.dtsi` and `qcom-msm8905-nokia-argon.dts`).

### 2.1 Hexagon DSP Clocking (Pure-DTS Workaround for AVS 2.7/2.8)
On MSM8909 (`pm8909` / `lpass_codec`), `aplay` initially failed with `hw_ptr: 0` / `I/O error` because `q6afe` requires explicit v2 IBIT/MCLK clock enablement via `&q6afecc`:
- **`lpass_codec: audio-codec@771c000`** enables:
  - `LPASS_CLK_ID_PRI_MI2S_IBIT` at `1536000 Hz`
  - `LPASS_CLK_ID_INTERNAL_DIGITAL_CODEC_CORE` at `9600000 Hz`
- **`lpass_ter_clk: audio-codec@771c400`** is added as a pure clock-consumer node (`qcom,msm8916-wcd-digital-codec` at `0x771c400`) to trigger `clk_prepare_enable()` on `LPASS_CLK_ID_TER_MI2S_IBIT` (`1536000 Hz`) without C driver changes. *(Note: this is a pure-DTS workaround that registers a second ASoC digital codec component.)*

### 2.2 Analog Codec (`&pm8909_codec`) & Sound Card (`&sound`)
- `&pm8909_codec`: `qcom,micbias-lvl = <2800>`, `qcom,hphl-jack-type-normally-open`, `qcom,micbias1-ext-cap`, `qcom,micbias2-ext-cap`, and MBHC button thresholds `<75 150 237 450 500>`.
- `&q6afedai`: `PRIMARY_MI2S_RX` (`dai@16`) and `TERTIARY_MI2S_TX` (`dai@20`), both with `qcom,sd-lines = <0 1>`.
- `&sound`: `qcom,msm8916-qdsp6-sndcard`, pinctrl `cdc_pdm_default` / `cdc_pdm_sleep` (`gpio59`–`gpio64`), frontends `MultiMedia1`–`MultiMedia3` + `MultiMedia5` (compress), and `audio-routing` connecting `AMIC1`/`AMIC3` to `MIC BIAS External1` and `AMIC2` to `MIC BIAS Internal2`.

---

## 3. Stock KaiOS Capture Analysis (`bananaphone-audio-settings/`)

Dumps captured on stock KaiOS (`msm8909-skub-snd-card`) via `params.sh`:
- `call-earpiece.txt`, `call-loud.txt`, and `call-ext-earpiece.txt` all use:
  - **Voice Uplink:** `DEC1 MUX = ADC2`, `ADC2 MUX = INP3` (**`AMIC3`**, the top/rear internal microphone). Stock firmware never used `AMIC1` in any captured call, and even in `call-ext-earpiece.txt` (call with headphones on `HPHL`/`HPHR`), uplink remained on `AMIC3` (`INP3`) rather than `AMIC2` (`INP2`).
- `playback-ext-earpiece.txt` / `call-ext-earpiece.txt`: `HPHL Impedance = 35`, `HPHR Impedance = 30`, `HPHL = Switch`, `HPHR = Switch`, `RDAC2 MUX = RX2`.
- Stock downstream DTS (`msm8905-qrd-skub.dtsi`) maps `AMIC1` ("Handset Mic"), `AMIC2` ("Headset Mic"), and `AMIC3` ("Secondary Mic").

---

## 4. Verified Mainline ALSA Mixer Routes

All routes use `card 0` (`msm8916`), playback on `hw:0,0` (`MultiMedia1`), capture on `hw:0,1` (`MultiMedia2`).

### Outputs (`scripts/test_outputs.sh`)
- **Loudspeaker (`SPK_OUT`):**
  ```sh
  PRI_MI2S_RX Audio Mixer MultiMedia1 = 1
  RX3 MIX1 INP1 = RX1
  RX3 Digital Volume = 84
  SPK DAC Switch = 1
  Speaker Boost = ENABLE
  ```
- **Earpiece (`EAR`):**
  ```sh
  PRI_MI2S_RX Audio Mixer MultiMedia1 = 1
  RX1 MIX1 INP1 = RX1
  RDAC2 MUX = RX1
  RX1 Digital Volume = 84
  EAR PA Gain = POS_1P5_DB
  EAR_S = 1
  ```
- **3.5mm Stereo Headphones (`HPHL` / `HPHR`):**
  ```sh
  PRI_MI2S_RX Audio Mixer MultiMedia1 = 1
  RX1 MIX1 INP1 = RX1
  RX2 MIX1 INP1 = RX2
  RDAC2 MUX = RX2
  RX1 Digital Volume = 84
  RX2 Digital Volume = 84
  HPHL = 1
  HPHR = 1
  ```

### Microphones (`scripts/test_mics.sh`)
- **Primary Internal Mic (`AMIC1`, bottom slider):**
  ```sh
  MultiMedia2 Mixer TERT_MI2S_TX = 1
  CIC1 MUX = AMIC
  DEC1 MUX = ADC1
  ADC1 Volume = 6
  TX1 Digital Volume = 84
  TX1 HPF Switch = 1
  ```
- **Secondary Internal Mic (`AMIC3`, top/rear ambient):**
  ```sh
  MultiMedia2 Mixer TERT_MI2S_TX = 1
  CIC1 MUX = AMIC
  ADC2 MUX = INP3
  DEC1 MUX = ADC2
  ADC2 Volume = 6      # Note: AMIC3 uses ADC2 Volume, not ADC3 Volume
  TX1 Digital Volume = 84
  TX1 HPF Switch = 1
  ```
- **Userspace Note:** Calling `pactl` when no PulseAudio/PipeWire daemon is running autospawns PulseAudio and locks `/dev/snd/pcmC0D1c` (`arecord: Resource busy`). Test scripts guard `pactl` behind `pgrep -x pulseaudio || pgrep -x pipewire-pulse`.

