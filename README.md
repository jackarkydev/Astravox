# Astravox

A vocal processing suite for VCV Rack 2, focused on spectral processing and traditional vocoding.

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

## Installation

Download the latest `.vcvplugin` file from the [Releases](../../releases) page and drag it onto the VCV Rack window, or place it in your Rack plugins folder manually.

**Requirements:** VCV Rack 2.x

---

## Building from Source

Requires the [VCV Rack 2 SDK](https://vcvrack.com/manual/Building).

```bash
git clone https://github.com/JackArkyDev/Astravox.git
cd Astravox
make
make install    # installs to your Rack user folder
```

---

## License

Copyright (C) 2026 Jack Arky

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.

See [LICENSE](LICENSE) for the full license text.
