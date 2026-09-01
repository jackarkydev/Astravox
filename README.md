# Astravox

A processing suite for VCV Rack 2: a 16-band channel vocoder for spectral and vocal work, and a three-head tape echo modeled on 1970s tape delay hardware.

**Developer:** Jack Arky
**License:** GPL-3.0-only

---

## Modules

### VOCODER — 42HP

A 16-band channel vocoder with per-band level faders, an internal carrier oscillator, and extensive CV connectivity. Designed as a dedicated vocal processing instrument.

**Highlights:**
- 16-band Chamberlin state-variable filter banks (analysis + synthesis)
- Per-band level faders with live LED meters showing spectral energy
- Internal VCO: SAW / PULSE with PWM, automatically normalled to carrier input
- FORMANT control — shifts synthesis bands relative to analysis bands for vowel shaping
- FREEZE — latches all 16 envelope followers at their current values
- 16 per-band CV outputs (envelope follower signals, 0–8V)
- 16 per-band CV inputs with selectable Replace or Sum mode
- EVEN / ODD mix outputs for stereo spreading
- Context menu: band spacing (LIN / MEL / BARK), LFO target, Band CV mode

For full documentation, right-click the module in VCV Rack and select **Manual**.

---

### TAPE ECHO — 20HP

A three-head tape echo and reverb combination modeled on the classic tape-based echo/reverb units of the 1970s. Goes beyond a clean delay: tape hysteresis and saturation, wow & flutter, self-oscillating feedback, and a "Tape Age" wear model reproduce the specific imperfections of a physical tape loop.

**Highlights:**
- 3 fixed tape heads (12-position Mode switch selects taps + reverb)
- Tape hysteresis/saturation feedback loop with measured self-oscillation
- Convolution-based spring reverb, with an optional reverse-follows-Reverse swell
- Tape Age wear model — per-instance random defect layout, HF rolloff, wow/flutter, and dropouts above 50% wear
- Clock sync (patch CLOCK to engage) with note-value divisions and a ±6% Rate nudge trim
- Gang — links Intensity and the rate axis as one performance gesture
- Individual H1/H2/H3 head outputs, plus a feedback-loop Send/Return insert point
- Context menu: Eco mode, machine noise floor, Tape Age presets, Drive tilt, Send/Return routing

For full documentation, right-click the module in VCV Rack and select **Manual**.

---

## Installation

Download the latest `.vcvplugin` file from the [Releases](../../releases) page.

**Requirements:** VCV Rack 2.x

---

## Building from Source

Requires the [VCV Rack 2 SDK](https://vcvrack.com/manual/Building).

```bash
git clone https://github.com/JackArkyDev/Astravox.git
cd Astravox
export RACK_DIR=/path/to/Rack-SDK
make
make install    # installs to your Rack user folder
```

---

## Third-Party Credits

This plugin incorporates the following third-party code and assets:

- **[AnalogTapeModel](https://github.com/jatinchowdhury18/AnalogTapeModel)** by Jatin Chowdhury
  — the magnetic hysteresis model used by Tape Echo's tape saturation is adapted from this
  project. Licensed under GPL-3.0.
- **[Inter](https://github.com/rsms/inter)** by The Inter Project Authors — used as a UI
  typeface. Licensed under the SIL Open Font License 1.1; full license text in
  [`res/fonts/OFL.txt`](res/fonts/OFL.txt).

---

## License

Copyright (C) 2026 Jack Arky

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.

See [LICENSE](LICENSE) for the full license text.
