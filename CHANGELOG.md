# Astravox Changelog

## 2.0.0 (in development)

### VOCODER
- Initial implementation: 16-band channel vocoder
- Per-band level faders with LED display
- Internal carrier oscillator (SAW/PULSE, bandlimited by default)
- FORMANT, GLIDE, MIX, FREEZE controls
- BANDS POLY out (16ch), MUTES POLY in (16ch)
- Context menu: band spacing, follower mode, carrier quality, preset saving

### VOCODER EXPANDER
- Initial implementation: extends core to 24 Bark bands
- 8 additional fader strips
- Per-band CV I/O and freeze gates for all 24 bands
- Phoneme detection: VOICED, UNVOICED, ONSET
