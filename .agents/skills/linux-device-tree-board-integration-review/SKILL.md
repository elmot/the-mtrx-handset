---
name: linux-device-tree-board-integration-review
description: >-
  Use when reviewing or authoring Linux Device Tree (.dts / .dtsi) changes for a
  specific target board and kernel release. Enforces strict boundary checks on
  SoC vs. PMIC vs. board layering, address/size cells, interrupt specifiers,
  regulator supplies, pin control states, status overrides, and driver binding
  alignment.
---

# Linux Device Tree Board Integration Review

Use this skill when authoring, migrating, or reviewing Device Tree Source (`.dts` and `.dtsi`) changes for a target board and kernel tree.

## 1. Context & Evidence Boundaries
Before modifying or approving any `.dts` / `.dtsi` node:
1. **Separate Observed Facts from Assumptions:**
   - Every register offset (`reg`), interrupt specifier (`interrupts`), GPIO pin number, regulator voltage range, and clock ID must cite its authoritative source:
     - Target mainline driver source (`sound/soc/...`, `drivers/clk/...`, `Documentation/devicetree/bindings/...`)
     - Vendor/downstream board DTS (`arch/arm/boot/dts/qcom/...`)
     - Runtime target inspection (`/sys/firmware/devicetree/base`, `dmesg`, `/sys/kernel/debug/devices_deferred`).
2. **Track Uncertainties Explicitly:**
   - If a property in downstream vendor DTS has no direct mainline equivalent or its hardware wiring is ambiguous (e.g., optional external speaker PA GPIO or MBHC switch polarity), flag it explicitly rather than guessing silently.

## 2. Hierarchy & Layering Rules (`.dtsi` vs. `.dts`)
Maintain strict separation across Device Tree files:
- **SoC `.dtsi` (e.g., `qcom-msm8909.dtsi`):**
  - Defines memory-mapped SoC blocks (`reg`), internal SoC interrupts, clocks, resets, `#sound-dai-cells`, and default SoC pinmux groups (`&tlmm`).
  - Hardware blocks that depend on board-specific wiring or external regulators must default to `status = "disabled";` in the `.dtsi`.
- **PMIC `.dtsi` (e.g., `pm8909.dtsi`):**
  - Defines SPMI sub-nodes under the appropriate USID slave (`pmic@0`, `pmic@1`), SPMI reg offsets, internal PMIC interrupts, and `#sound-dai-cells`.
  - Leave board-dependent sub-nodes `status = "disabled";` until enabled and supplied by the board `.dts`.
- **Board `.dts` (e.g., `qcom-msm8905-nokia-argon.dts`):**
  - Overrides `status = "okay";` via label references (`&lpass`, `&lpass_codec`, `&pm8909_codec`).
  - Supplies board-specific regulator phandles (`vdd-*-supply`), board pin control states (`pinctrl-0`, `pinctrl-1`), GPIO assignments, and top-level board glue nodes (`sound` card node and `audio-routing`).

## 3. Technical Validation Checklist
Check every added or modified node against these 7 criteria:

1. **Address & Size Cells (`#address-cells`, `#size-cells`):**
   - Verify the parent bus's `#address-cells` and `#size-cells` match the child `reg` tuple length (e.g., `/soc` uses `<1>/<1>`, while SPMI sub-nodes under `pmic@0` use `#address-cells = <1>; #size-cells = <0>;` or `<1>/<1>` depending on the PMIC node definition—always inspect sibling nodes in the same file!).
2. **Compatible String & Driver Binding Alignment:**
   - Confirm that the exact `compatible` string exists in `of_device_id` within the target kernel tree and that every property name (`clock-names`, `interrupt-names`, `*-supply`, `#sound-dai-cells`) matches what the driver's `probe()` function parses.
3. **Interrupt Specifiers & Polarity:**
   - For GIC interrupts (`&intc`): `<GIC_SPI irq_num IRQ_TYPE_*>`.
   - For SPMI PMIC interrupts (`&spmi_bus`): `<USID periph_id irq_index IRQ_TYPE_*>`, matching sibling nodes in `pm8909.dtsi`.
4. **Regulator Supply Completeness:**
   - Every `devm_regulator_get()` or `devm_regulator_bulk_get()` in the target driver must have a matching `<name>-supply = <&reg_phandle>;` in the DT node, and that regulator must be defined and operable.
5. **Clocks & `clock-names`:**
   - Every clock requested by `devm_clk_get(dev, "<name>")` must be present in `clocks` and `clock-names` in the exact order/name expected by the driver, and the clock ID macro must be implemented in the SoC clock controller driver (`gcc-msm8909.c`).
6. **Pin Control (`pinctrl-names`, `pinctrl-0`, `pinctrl-1`):**
   - Active (`"default"`) and sleep (`"sleep"`) states must reference valid pinmux child nodes in `&tlmm` with valid pin names (`"gpio59"`..`"gpio64"`) and function names (`"cdc_pdm0"`, `"pri_mi2s"`, etc.) supported by the pinctrl driver (`pinctrl-msm8909.c`).
7. **Status Overrides & Orphans:**
   - Ensure no node referenced by a DAI link (`cpu { sound-dai = <&...>; }`, `codec { sound-dai = <&...>; }`) is left with `status = "disabled";`, as ASoC card registration will fail or defer indefinitely.

