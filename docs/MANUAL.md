# Astravox Vocoder — User Manual

A 16-band phase vocoder for VCV Rack 2, inspired by the Moog Spectravox, with some additions to improve vocoding behavior. The module combines a program (modulator) signal with a carrier signal — the spectral envelope of the program is extracted and imposed on the carrier to produce the classic vocoder effect. It can also operate as a standalone 16-band filter bank.

---

## Signal Flow Overview

```
PROGRAM ──► Analysis Filter Bank ──► Envelope Followers ──┐
                                                           ▼
CARRIER / VCO / Noise ──► Synthesis Filter Bank ──► Band VCAs ──► Mix ──► VCA ──► OUT
```

---

## Inputs

| Jack | Description |
|------|-------------|
| **PROGRAM** | Modulator signal (typically voice). Drives the 16 analysis filters and envelope followers. |
| **CARRIER** | External carrier. If unpatched, the internal VCO is used. |
| **V/OCT** | 1V/oct pitch input for the internal VCO. Polyphonic — up to 16 channels are averaged. |
| **PWM** | Pulse width modulation CV for the internal VCO (active in PULSE wave mode). |
| **LFO RATE** | CV input that scales the LFO rate exponentially. |
| **TRIGGER** | Gate/trigger input — fires the EG on rising edge. |
| **FORMANT** | CV input for spectral shift (±5V = full range with knob centered). |
| **FREEZE** | Gate input — holds envelope followers at their current values while high (≥2V). |
| **VCA CV** | Final output level CV (0–5V = 0–100%). |
| **Band CV (×16)** | Per-band VCA CV inputs. Behavior set by **Band CV mode** in the context menu. |

---

## Outputs

| Jack | Description |
|------|-------------|
| **OUT** | Main mixed audio output. |
| **LFO OUT** | LFO signal (triangle wave, 0–5V). |
| **EG OUT** | Envelope generator output (0–8V). |
| **PROG ENV F** | Program envelope follower output (0–8V). Reflects overall program level. |
| **Band ENV (×16)** | Per-band envelope follower outputs (0–8V each). |
| **EVEN / ODD** | Alternate-band mix outputs. Even-numbered bands to EVEN, odd-numbered to ODD. Useful for stereo spreading or parallel processing. |

---

## Controls

### Carrier Section

**PITCH** — Internal VCO frequency, centered at middle C. Tracks V/OCT input.

**WAVE** (button) — Toggles the internal VCO between SAW and PULSE waveforms.

**PULSE WIDTH** — Controls the duty cycle of the PULSE waveform (1–50%). Modulatable via the PWM input.

**CARRIER MIX** — Crossfades between the external CARRIER input (or internal VCO) and the internal noise source. Fully left = carrier only; fully right = noise only.

**NOISE** (button) — Selects the noise type: **WHITE** (spectrally flat) or **PINK** (1/f, warmer and less harsh — useful for more natural-sounding breath and texture).

**PROGRAM LEVEL** — Input gain for the program signal, with soft saturation.

---

### Band Controls

**Band Sliders (×16)** — Set the output level for each synthesis band.
- Range: 0 (muted) to 1.5 (6dB boost), unity at 1.0.
- **Double-click** — Reset to unity (1.0).
- **Cmd+double-click** — Mute the band (set to 0).
- **Hover** — Tooltip displays the current level and the band's center frequency (e.g. `1.000 | 250 Hz`). The frequency reflects the active band spacing mode.

**Band LEDs (×16)** — Indicate signal activity per band. Brightness reflects both the envelope follower level and the slider position — a muted slider silences the LED regardless of signal.

---

### Spectral Shift + LFO

**FORMANT** — Shifts the synthesis filter bank up or down in frequency relative to the analysis bank. Center position = no shift. Modulatable via the FORMANT CV input.

**LFO RATE** — Sets the LFO speed. The LED blinks at the LFO rate (flashes solid at high rates). Modulatable via LFO RATE CV input.

**LFO DEPTH** — Amount of LFO modulation applied to the selected target (see LFO target in context menu).

---

### Envelope & EG

**DECAY** — Controls envelope follower release time and EG decay time. Range: ~1ms (left) to ~5 seconds (right), logarithmically scaled.

**TRIGGER** (button) — Manually fires the EG.

**PROG TRIG** (button) — Enables automatic EG triggering from the program signal. When ON, the EG fires on signal onset — threshold-based detection that re-arms after the signal drops below the lower threshold during a pause. **PROGRAM LEVEL acts as the sensitivity control** — lower settings require a louder signal to trigger. Best suited for rhythmic sources or speech with clear pauses; for continuous vocoding use VCA ON mode instead.

**ATTACK** (button) — Toggles envelope follower attack time: FAST (0ms, instantaneous) or SLOW (40ms).

---

### Output Section

**RESONANCE** — Unified Q control for all 16 synthesis filters. Higher values increase band selectivity and add resonant character.

**MODE** (button) — Switches between VOCODER mode (synthesis bands gated by program envelope) and FILTER BANK mode (synthesis bands always open, band sliders act as a static EQ).

**VCA MODE** (button) — Switches the output VCA between EG-controlled (EG) and always-on (ON).

**VOLUME** — Master output level.

**FREEZE** (button/input) — Freezes all 16 envelope followers at their current values. Useful for capturing and holding a vowel or spectral snapshot. Also controllable via the FREEZE gate input.

---

## Context Menu Options

Right-click the module panel to access:

**Sibilance** — Blends a high-frequency component of the program signal directly into the output, above the top synthesis band. Improves intelligibility of sibilants and fricatives.

**Band 1: LP → BP** — Changes band 1 from a low-pass to a band-pass filter, giving it a tighter character consistent with the other bands.

**Band 16: HP → BP** — Changes band 16 from a high-pass to a band-pass filter.

**Band spacing** — Sets the frequency layout of the 16 analysis and synthesis bands:
- **LIN** — Linear spacing.
- **MEL** — Mel scale (perceptually uniform pitch spacing).
- **BARK** — Bark scale (models critical bands of human hearing; generally best for voice intelligibility).

**Band CV mode** — Determines how the per-band CV inputs interact with the envelope followers:
- **Replace envelope (default)** — Band CV directly sets the VCA level, overriding the envelope follower.
- **Sum with envelope** — Band CV is added to the envelope follower value.

**LFO target** — Selects what the LFO modulates:
- **Spectrum (default)** — LFO modulates the Spectral Shift.
- **PWM** — LFO modulates the VCO pulse width (active in PULSE wave mode).
- **Both** — LFO modulates both simultaneously.

**LFO sync** — **Free (default):** LFO runs continuously. **Reset on trigger:** LFO phase resets to zero on each trigger.

**Reset all faders to unity** — Returns all 16 band sliders to 1.0.

**Set all faders to zero** — Sets all 16 band sliders to 0.

---

## Tips

- **Vocal intelligibility:** Start with BARK band spacing, enable Sibilance, and set DECAY to a medium-fast value. The program signal should be clean and relatively loud going in.
- **Freeze as an instrument:** Patch a sequencer into FORMANT CV and freeze a vowel — the shift will sweep the frozen formants across the spectrum while new notes track via V/OCT.
- **Filter bank mode:** With MODE set to FILTER BANK, the module becomes a 16-band parametric filter with per-band CV control. Route audio into CARRIER and patch per-band CVs to shape the spectrum.
- **EVEN/ODD outputs:** Feed these into a stereo panner or reverb send at different widths for a spacious stereo image without duplicating the main output.
- **Polyphonic chord vocoding:** Route a polyphonic chord into CARRIER IN with a vocal signal on PROGRAM IN. Each voice is independently shaped by the same spectral envelope — the chord sings along with the voice.
- **Rhythmic gating:** Set VCA MODE to EG, patch a clock into TRIGGER IN, and set DECAY to taste. The vocoded output gates rhythmically with each trigger.

---

## Troubleshooting

**No output sound:**
- Check that PROGRAM IN is receiving audio and PROGRAM LEVEL is > 0.
- Verify CARRIER IN or VCO is connected and active.
- Check that VOLUME is > 0 and at least one band fader is > 0.

**Output is too quiet:**
- Increase PROGRAM LEVEL to boost input sensitivity.
- Raise VOLUME and ensure band faders are lifted (0.75–1.0 is typical for vocals).
- Check VCA MODE — EG mode silences output once the EG has decayed.

**Vocoded output doesn't track the modulator:**
- Confirm PROGRAM IN has sufficient level (adjust PROGRAM LEVEL).
- Check ATTACK mode (FAST for tight tracking, SLOW for smooth).
- Verify DECAY is not too long (try a short-to-medium setting, around 20–40ms).

**Carrier signal is clipping:**
- Reduce external CARRIER IN level or dial back VOLUME.
- Lower individual band faders if certain frequencies are too loud.

**Frozen spectrum not updating:**
- Ensure FREEZE button is off (LED should be dark).
- Check FREEZE IN — if patched, must be low (<2V) to release.

---

## Technical Specifications

| Parameter | Value |
|---|---|
| **Filter Banks** | 16-band Chamberlin state-variable |
| **Filter Type** | Band 1: LP, Bands 2–15: BP, Band 16: HP |
| **Frequency Range** | 100–8000 Hz (default LIN mode; MEL/BARK also available) |
| **Envelope Follower Attack** | 0ms (FAST) or 40ms (SLOW) |
| **Envelope Follower Release** | 1ms–5 seconds (logarithmically scaled by DECAY) |
| **LFO Waveform** | Triangle, 0.05 Hz – 500 Hz |
| **Polyphony** | Carrier: up to 16 voices; Program: mono analysis |
| **Output Voltage** | VCA OUT: ±5V nominal, EG OUT: 0–8V, LFO OUT: 0–5V, Band OUT: 0–8V |
| **Panel Size** | 42HP × 128.5mm (3U standard) |

---

## Expander Modules (Future)

A dedicated **+8 Band Expander** will extend the core module to 24 total bands with expanded routing and CV capabilities.

Longer term, the Astravox suite plans tribute expanders inspired by classic hardware vocoders — the Sennheiser VSM-201, Roland SVC-350, Moog 16-band, and EMS Vocoder 5000 — each bringing the character and band configuration of the original instrument.
