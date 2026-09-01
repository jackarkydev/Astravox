# Astravox Changelog

## 2.1.0

### TAPE ECHO — new module

- Three-head tape delay and spring reverb, 20HP. Physical modeling based on a Roland RE-201 Space Echo tape delay unit and other hardware from that era.
- Twelve-position Mode selector switching which heads reach the output and whether the spring reverb is in circuit.
- Magnetic hysteresis saturation and per-pass bandwidth loss modeled through the record/playback cycle, with measured self-oscillation onset — including the hysteresis that holds an established oscillation below the level that started it.
- Stochastic wow and flutter. Each instance is given its own tape defect layout when created, saved with the patch.
- Convolution spring reverb from captured impulse responses, morphing across six excitation levels.
- Independent H1/H2/H3 head outputs alongside the main mix, for stereo and multi-tap patching.
- Feedback send/return insert, switchable between in-loop and output-only.
- Tempo sync with per-head anchoring, a rate nudge around the grid, and a Gang gesture linking Intensity to tape speed.
- Reverse, motor stop, Tape Age presets and CV, and a switchable output pad.

### Documentation

- Each module now has its own manual: [Vocoder](docs/Vocoder.md) and [Tape Echo](docs/TapeEcho.md), linked individually from the VCV Library. `docs/MANUAL.md` remains as an index.

### Under the hood

- Bundled audio reduced from 34 MB to 8 MB with no change to the modeling: impulse responses moved to 48 kHz/24-bit (everything above 24 kHz was ultrasonic, and a 48 kHz engine was already discarding it at load), and the noise floor capture stored 24-bit and trimmed to a loop length that is an exact multiple of its mains-hum period. Fixes a latent thump at the noise loop's wrap point.

## 2.0.3

- Windows: fix plugin failing to load due to MinGW runtime mismatch. Build now cross-compiles from Linux with static libgcc and win32 thread model.

## 2.0.2

- VOCODER: panel label and silkscreen polish.

## 2.0.1

- Build fixes only, no functional changes: restored the mac-x64 build, fixed the macOS SDK download, and corrected a Windows build that produced a crashing plugin.

## 2.0.0

### VOCODER
- Initial implementation: 16-band channel vocoder
- Per-band level faders with LED display
- Internal carrier oscillator (SAW/PULSE, bandlimited by default)
- FORMANT, GLIDE, MIX, FREEZE controls
- BANDS POLY out (16ch), MUTES POLY in (16ch)
- Context menu: band spacing, follower mode, carrier quality, preset saving
