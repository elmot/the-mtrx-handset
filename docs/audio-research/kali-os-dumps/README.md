# Dumps captured earlier on stock KaiOS (`msm8909-skub-snd-card`) via `params.sh`:
- `call-earpiece.txt`, `call-loud.txt`, and `call-ext-earpiece.txt` all used:
  - **Voice Uplink:** `DEC1 MUX = ADC2`, `ADC2 MUX = INP3` (**`AMIC3`**, the top/rear internal microphone). Stock firmware never used `AMIC1` in any captured call, and even in `call-ext-earpiece.txt` (call with headphones on `HPHL`/`HPHR`), uplink remained on `AMIC3` (`INP3`) rather than `AMIC2` (`INP2`).
- `playback-ext-earpiece.txt` / `call-ext-earpiece.txt`: `HPHL Impedance = 35`, `HPHR Impedance = 30`, `HPHL = Switch`, `HPHR = Switch`, `RDAC2 MUX = RX2`.
- Stock downstream DTS (`msm8905-qrd-skub.dtsi`) maps `AMIC1` ("Handset Mic"), `AMIC2` ("Headset Mic"), and `AMIC3` ("Secondary Mic").
