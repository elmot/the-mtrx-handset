---
name: qcom-dts-migration
description: >-
  Use when analyzing downstream Qualcomm Android/KaiOS (Linux 3.10) Device Tree
  files and migrating audio, codec, LPASS, SPMI PMIC, clock, regulator, or pinctrl
  DTS fragments to mainline Linux / Nura OS (postmarketOS) for MSM8905/MSM8909
  and PM8909 (nokia-argon).
---

# Qualcomm Downstream-to-Mainline DTS Migration (`msm8909` / `pm8909` / `nokia-argon`)

Use this skill whenever extracting hardware parameters from the downstream HMD/KaiOS 3.10 kernel (`ASKSAP/android_kernel_nokia_8110` / `Nokia_8110_4G_OSS`) and porting them to the mainline Nura OS `v26.06` kernel (`linux-postmarketos-qcom-msm8909`).

## 1. Downstream (Linux 3.10) -> Mainline (Linux 6.x) Audio Binding Map

Downstream Qualcomm 3.10 kernels combine the digital and analog codec halves into a single monolithic driver (`msm8x16_wcd`), whereas mainline Linux splits them into two separate ASoC codec drivers plus an LPASS CPU DAI driver and an ASoC machine driver:

| Component | Downstream 3.10 Binding (`msm8909`) | Mainline 6.x Binding (`qcom-msm8909` / `pm8909`) | Mainline Driver Source |
| :--- | :--- | :--- | :--- |
| **Sound Card (Machine)** | `qcom,msm8x16-audio-codec` | `qcom,apq8016-sbc-sndcard` (LPASS direct) or `qcom,msm8916-qdsp6-sndcard` (QDSP6) | `sound/soc/qcom/apq8016_sbc.c` |
| **LPASS CPU DAI (MI2S)** | `qcom,msm-dai-mi2s` / `qcom,msm-pri-mi2s` | `qcom,apq8016-lpass-cpu` (`lpass@7708000`) | `sound/soc/qcom/lpass-apq8016.c` |
| **In-SoC Digital Codec** | Part of `qcom,msm8x16_wcd_codec` (`0x0771c000`) | `qcom,msm8916-wcd-digital-codec` (`codec@771c000`) | `sound/soc/codecs/msm8916-wcd-digital.c` |
| **In-PMIC Analog Codec** | Part of `qcom,msm8x16_wcd_codec` (SPMI `0xf000`, `0xf100`) | `qcom,pm8916-wcd-analog-codec` (`codec@f000` under `pmic@0`) | `sound/soc/codecs/msm8916-wcd-analog.c` |
| **LPASS IOMUX / CSR Syscon** | Inline `0x07702000` / `0x07701000` | `qcom,lpass-cpu-iomux` (`syscon`) | Referenced via `qcom,lpass-ultaudio-iomux` / `qcom,lpass-codec-spkr` |

## 2. Property-by-Property Translation Rules

### A. PM8909 Analog Codec (`codec@f000` under `&spmi_bus` -> `pmic@0`)
1. **Check Sibling `reg` Format in `pm8909.dtsi`:**
   - In mainline `pm8916.dtsi` / `pm8909.dtsi`, `pmic@0` has `#address-cells = <1>; #size-cells = <0>;`, so the codec uses `reg = <0xf000>;` (covering both `0xf000` and `0xf100` peripherals internally via `regmap`).
2. **Regulator Supply Names:**
   - Downstream uses `cdc-vdd-io-supply`, `cdc-vdd-tx-rx-cx-supply`, `cdc-vdd-mic-bias-supply`, `cdc-vdd-pa-supply`.
   - Mainline `msm8916-wcd-analog.c` strictly requires these three supply properties:
     - `vdd-cdc-io-supply` (map to PM8909 LDO supplying 1.8V, typically `&pm8909_l5`)
     - `vdd-cdc-tx-rx-cx-supply` (map to PM8909 SMPS/LDO supplying 1.8V, typically `&pm8909_s2` or `&pm8909_l5`)
     - `vdd-micbias-supply` (map to PM8909 LDO supplying ~2.7–2.85V, typically `&pm8909_l13`)
3. **Interrupts (`interrupt-names` in `msm8916-wcd-analog.c`):**
   - Required `interrupt-names` (14 interrupts across SPMI peripheral `0xf0` and `0xf1`):
     - `"cdc_spk_cnp_int"`, `"cdc_spk_clip_int"`, `"cdc_spk_ocp_int"`, `"mbhc_ins_rem_det1"`, `"mbhc_but_rel_det"`, `"mbhc_but_press_det"`, `"mbhc_ins_rem_det"`, `"mbhc_switch_int"`, `"cdc_ear_ocp_int"`, `"cdc_hphr_ocp_int"`, `"cdc_hphl_ocp_int"`, `"cdc_rx_pb_short_int"`, `"cdc_tx_pb_short_int"`, `"cdc_pa_ocp_int"` (verify exact list against `sound/soc/codecs/msm8916-wcd-analog.c` in target kernel tree).
4. **Clock:**
   - `clocks = <&lpass_codec>;` (or `<&gcc GCC_CODEC_DIGCODEC_CLK>`), `clock-names = "mclk";`.
5. **MBHC & Micbias Properties:**
   - Translate downstream `qcom,msm-mbhc-hphl-swh` -> `qcom,mbhc-vthreshold-low` / `qcom,mbhc-vthreshold-high`, `qcom,hphl-jack-type-normally-open` (boolean), `qcom,gnd-jack-type-normally-open` (boolean), `qcom,micbias1-ext-cap`, `qcom,micbias2-ext-cap`, `qcom,micbias-lvl`.

### B. SoC Digital Codec (`codec@771c000` in `qcom-msm8909.dtsi`)
1. **Registers & Clocks:**
   - `reg = <0x0771c000 0x400>;`
   - `clocks = <&gcc GCC_ULTAUDIO_AHBFABRIC_IXFABRIC_CLK>, <&gcc GCC_CODEC_DIGCODEC_CLK>;`
   - `clock-names = "ahbix-clk", "mclk";`
   - `#sound-dai-cells = <1>;`
   - `#clock-cells = <0>;` (if exporting `mclk` to the analog codec).

### C. Pin Control (`&tlmm` in `qcom-msm8909.dtsi`)
- Verify the pin function name in `drivers/pinctrl/qcom/pinctrl-msm8909.c`:
  - Look up the pin group covering GPIO 59–64 (`cdc_pdm0` or `pdm`) and MI2S pins if needed.
  - Attach `pinctrl-names = "default", "sleep";` to the `lpass` or `sound` node as expected by the driver.

### D. Sound Card & `audio-routing` (`qcom-msm8905-nokia-argon.dts`)
- Mainline `msm8916-wcd-analog` widget names differ slightly from downstream 3.10:
  - Downstream `"MIC BIAS Internal1"` -> Mainline `"MIC BIAS Internal1"` / `"AMIC1"`
  - Check exact `snd_soc_dapm_widget` and `snd_soc_dapm_route` tables in `sound/soc/codecs/msm8916-wcd-analog.c` and `msm8916-wcd-digital.c` before writing `audio-routing` in `qcom-msm8905-nokia-argon.dts`!

## 3. Pre-Flash DTB Verification Procedure

Never flash a modified DTS without running the offline DTB verification step:
1. Run the helper script [validate_dtb.sh](./scripts/validate_dtb.sh) against the compiled `.dtb` file.
2. Confirm:
   - `dtc` compiles the `.dtb` with zero syntax or phandle errors.
   - Decompiled `.dtb` contains `/soc/lpass@7708000`, `/soc/codec@771c000`, `/soc/spmi@200f000/pmic@0/codec@f000`, and `/sound` with `status = "okay"`.
   - No unresolved phandles (`0xffffffff`) exist on `clocks`, `*-supply`, `pinctrl-*`, or `sound-dai`.

