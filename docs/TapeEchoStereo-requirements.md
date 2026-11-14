# Stereo Tape Echo — Requirements (Draft)

*Status: draft from the requirements brainstorm. Items marked **[OPEN]** still need a decision; items marked **[TBD]** need a value from prototyping or measurement.*

Companion to the mono [Tape Echo](TapeEcho.md). Requirement IDs (`ST-xxx`) are stable references for design, implementation and test.

---

## 1. Overview

A new module, **Stereo Tape Echo**, modeling a two-track tape echo: **one tape loop, one transport, one head block, two recording tracks (L and R)**. It keeps the mono module's measured RE-201 character and adds the behaviors and artifacts specific to a stereo tape machine, plus modern stereo routing.

### Goals
- Sound like one physical stereo machine by default: shared transport, shared tape, correlated wobble.
- Offer a continuous path from "one machine" to "two machines" (Transport Coupling) for width.
- Model stereo-specific tape artifacts (azimuth, crosstalk, edge damage, track skew) and tie their severity to Tape Age.
- Add stereo routing (Dual / Ping-Pong / Cross), per-head panning, a stereo spring reverb, a fourth head and selectable head-spacing ratios.
- With stereo features neutral, reproduce the mono module (regression anchor).

### Non-goals
- Changing the mono Tape Echo. (Backporting head ratios / H4 to the mono module is a possible future item, out of scope here.)
- Polyphonic / multi-channel-cable stereo. All stereo I/O uses separate L/R jacks.
- New CV inputs beyond those the mono module already has (for this release).

---

## 2. Architecture

| ID | Requirement |
|---|---|
| ST-001 | The module SHALL be a new plugin module with its own slug; existing Tape Echo patches are unaffected. |
| ST-002 | The model SHALL have one tape loop with two tracks (L, R), one head block shared by both tracks, and one transport. |
| ST-003 | Repeat Rate, Rate nudge, Motor Stop, Reverse, Gang and tempo sync SHALL act on the shared transport (both tracks together). |
| ST-004 | Each track SHALL have its own record path (hysteresis/saturation), playback path, feedback loop and per-pass bandwidth loss. |
| ST-005 | The tape defect layout (Tape Age model) SHALL be a single layout shared by both tracks, drawn once per instance and saved with the patch, as in the mono module. Per-track severity is defined in §3. |
| ST-006 | DSP shared with the mono module SHOULD be factored into a reusable per-track engine so fixes apply to both modules. |

### 2.1 Transport Coupling

| ID | Requirement |
|---|---|
| ST-010 | A **Transport Coupling** panel control SHALL blend continuously from 0 = one transport (both tracks share identical wow/flutter) to 1 = fully independent transports (each track has its own, uncorrelated wow/flutter). |
| ST-011 | Transport Coupling SHALL NOT change mean tape speed per track; delay times stay locked between tracks. Only the speed *modulation* decorrelates. **[OPEN]** whether slow per-track speed drift (not just wow/flutter) is also decoupled at high settings. |
| ST-012 | At Transport Coupling = 0 with Stereo alignment = Aligned and Tape Age = 0, wow/flutter SHALL be sample-identical on both tracks. |

---

## 3. Stereo tape artifacts

All artifacts have a baseline set by the **Stereo alignment** preset (context menu) and are worsened by **Tape Age**, the same way the Tape Age preset/CV works today. One knob (Tape Age) therefore degrades both the tape and its stereo image.

| ID | Requirement |
|---|---|
| ST-020 | **Stereo alignment** context-menu preset: *Aligned* (default), *Slightly off*, *Misaligned*. It sets the baseline for ST-021–ST-024. |
| ST-021 | **Head azimuth error:** a small inter-track time offset and per-track HF loss from head misalignment. Baseline from Stereo alignment, increasing with Tape Age. Range **[TBD]**. |
| ST-022 | **Inter-track crosstalk:** each track bleeds into the other. Target ≈ −40 dB at Aligned/Tape Age 0, rising with Tape Age **[TBD, validate]**. Crosstalk SHOULD be HF-weighted as on real tape. |
| ST-023 | **Edge damage:** each defect in the shared layout SHALL carry a per-track severity bias (stored with the patch), so a dropout can hit one track harder than the other. Edge bias deepens with Tape Age. Dropout floor stays ≈ −20 dB as in the mono module. |
| ST-024 | **Track skew:** small per-track differences in flutter even at Transport Coupling = 0 (one motor, slightly uneven tape path). Zero at Aligned/Tape Age 0, increasing with misalignment and Tape Age. |
| ST-025 | Machine noise floor (when enabled) SHALL be independent per track. |

---

## 4. Heads

### 4.1 Head set

| ID | Requirement |
|---|---|
| ST-030 | Four playback heads: H1–H3 as in the mono module, plus **H4**. |
| ST-031 | **Mode** SHALL be unchanged from the mono module: same 12 positions, same tap chart, selecting among H1–H3 (and reverb). H4 is not part of Mode. |
| ST-032 | **H4 level:** continuous panel trimpot, default 0. With H4 level at 0 the module behaves as a three-head machine. |
| ST-033 | **H4 routing** (context menu): *Output only* (default) — H4 feeds the output mix only; *Output + loop* — H4 also feeds the feedback loop, scaled by its level. |
| ST-034 | With *Output + loop*, the loop-gain normalization (head-averaging mix law) SHALL include H4's contribution so turning H4 up does not by itself push the loop into runaway. Self-oscillation onset with H4 in the loop is untuned and SHALL be documented as a liberty; existing clip/limiter safety SHALL still hold. |
| ST-035 | H4 SHALL never be the tempo-sync anchor head. |

### 4.2 Head-spacing ratios

| ID | Requirement |
|---|---|
| ST-040 | **Head ratio** context-menu item with presets (H1 : H2 : H3 : H4): |

| Preset | Ratio | Notes |
|---|---|---|
| Classic *(default)* | 1 : 2 : 3 : 4 | Measured RE-201 geometry (H4 extends the even spacing). |
| Dotted | 1 : 1.5 : 2 : 2.5 | Dotted and straight repeats interlock. |
| Fibonacci | 1 : 1.5 : 2.5 : 4 | Rational near-golden; stays on the sync grid. |
| Golden | 1 : 1.618 : 2.618 : 4.236 | Irrational; repeats never coincide, smoother multi-head feedback. |
| √2 | 1 : 1.414 : 2 : 2.828 | H2 off-grid, H3 an octave of H1. |

| ID | Requirement |
|---|---|
| ST-041 | H1's position SHALL be fixed across presets; H2–H4 move. All presets SHALL fit the existing 8 s delay buffer at the slowest speed. |
| ST-042 | Only Classic is measured; other presets SHALL be documented as modern liberties (self-oscillation onset per Mode not re-measured). |
| ST-043 | Tempo sync (nearest-tap anchoring, division reachability, "Tap N" display) SHALL work for every preset. With irrational ratios only the anchor head is guaranteed on-grid. |
| ST-044 | **Ratio change behavior** (context menu): *Glide* (default) — heads move physically over a glide time **[TBD, ≈0.3–1 s]**, producing a tape-style pitch swoop and ending on the new positions; *Crossfade* — click-free crossfade, no pitch artifact. |
| ST-045 | Ratio changes from patch load, preset recall or initialization SHALL be instant (no glide or crossfade). |

### 4.3 Head placement in the stereo field

| ID | Requirement |
|---|---|
| ST-050 | **Per-head pan** trimpots for H1–H4, default centre. Pan applies to each head's contribution to the main output mix only. |
| ST-051 | Pan law **[TBD]** (equal-power suggested). |
| ST-052 | **Head stagger:** panel trimpot that offsets the R-track head positions relative to L (staggered-head stereo layout). Range **[TBD, ≈0–20 ms]**, default 0. |

---

## 5. Stereo routing and feedback

| ID | Requirement |
|---|---|
| ST-060 | **Routing** 3-position panel switch: *Dual*, *Ping-Pong*, *Cross*. |
| ST-061 | **Dual:** L input records on the L track, R on R; each track feeds back into itself. |
| ST-062 | **Ping-Pong:** the input is summed as 0.5·(L+R) and recorded on the **L track only**; L playback feeds the R record path, R playback feeds the L record path. The 0.5 sum law makes a normalled mono input drive the tape exactly as in Dual. |
| ST-063 | **Cross:** true stereo input (as Dual); the **Cross** panel knob blends feedback continuously from 0 (L→L, R→R) to 1 (L→R, R→L). Cross = 1 is stereo-preserving ping-pong. **[OPEN]** whether the Cross knob does anything in Dual / Ping-Pong. |
| ST-064 | Switching Routing while audio is running SHALL crossfade the routing change (no clicks). |
| ST-065 | **Feedback Balance:** one bipolar panel knob, centre = equal. It scales the L and R loop gains in opposite directions using a dB-symmetric law, so gL·gR is constant: the Ping-Pong round-trip gain (and its self-oscillation onset) is unchanged by Balance, while the decay differs per side. Range **[TBD, e.g. ±6 dB]**. |
| ST-066 | Intensity, Input Drive and the self-oscillation behavior (swell-in, hold-on, prompt release) SHALL apply per track as in the mono module. Self-oscillation onset in Ping-Pong / Cross (where the loop period is a round trip) SHALL be validated and tuned. |

---

## 6. Spring reverb

| ID | Requirement |
|---|---|
| ST-070 | One spring tank, fed from 0.5·(L+R) of the input, in parallel with the tape path (as in the mono module), gated by Mode. |
| ST-071 | Stereo structure: **A** = input ⊛ measured IR (today's spring), **D** = input ⊛ synthesized decorrelated IR. Outputs: L = g·A + w·D, R = g·A − w·D, where w = **Spring Width** (panel knob, 0–1) and **g = 1/√(1+w²)** (constant per-side loudness). |
| ST-072 | At Spring Width = 0 the spring output SHALL equal the mono module's spring on both sides (within float rounding). |
| ST-073 | L+R SHALL contain no D component at any width (mono sum = measured spring, scaled by g). |
| ST-074 | **Reverb Pan** trimpot, applied after the width stage as a balance control. Default centre. |
| ST-075 | A second set of six IRs (one per drive level L1–L6) for D SHALL be generated offline by a script in `tools/` and shipped in `res/` (≈ +6 MB). The same process and seed SHALL be applied to all six levels so the level morph stays consistent. Reversed D IRs are built at load time, as the existing reversed set is. |
| ST-076 | D synthesis method: **combination** — small time-stretch (≈±1–3%, "a second spring of slightly different length") for the early part (≈ first 100 ms, preserving chirp/drip), crossfading to velvet-noise and/or STFT phase-randomized decorrelation for the tail. Development order: prototype time-stretch and velvet-noise first, then the combination; pick by ear subject to ST-077. A virtual-pickup-position model is a possible later refinement. |
| ST-077 | D acceptance criteria: interchannel correlation |ρ| < 0.2 over the tail at width = 1; per-octave T60 within ≈10% of A; long-term spectrum within ±1 dB of A; no new tonal peaks relative to A (listening test plus spectral-peak check). |
| ST-078 | *Reverb follows Reverse* SHALL use the reversed A and D sets and act on both sides together. |
| ST-079 | In Eco mode the D path SHALL be disabled (spring forced mono, w = 0). |

---

## 7. Features carried over from the mono module

| ID | Requirement |
|---|---|
| ST-080 | Carried over unchanged in behavior, acting on both tracks: Repeat Rate / Rate nudge, Intensity, Input Drive, Echo Volume, Reverb Volume, Bass/Treble, Mode, Reverse (button + gate), Motor Stop, Output Pad, Gang (+ spring-back), Tempo Sync (division, multiplier), Tape Age preset + CV, Drive tilt, Eco mode, Machine noise floor, Wet only, Tone in feedback loop, Reverse is momentary, Send/Return insert point and return mode. |
| ST-081 | All existing CV inputs carried over as single (shared) inputs. No new CV inputs in this release. |

---

## 8. Panel and I/O

| ID | Requirement |
|---|---|
| ST-090 | Single wide panel (no expander). Width **[TBD from mockup, est. 32–36HP]**. |
| ST-091 | Inputs: IN L, IN R (R normalled to L). |
| ST-092 | Outputs: OUT L, OUT R. **[OPEN]** whether OUT L carries a mono sum when OUT R is unpatched. |
| ST-093 | Head outputs: H1–H4, each with L and R jacks (8 jacks). Each carries that head's tape-track signal (pre-pan, delay-only, unpadded), matching the mono module's head-output semantics. |
| ST-094 | FB SEND L/R and FB RETURN L/R. **[OPEN]** normalling when only FB RETURN L is patched (feed both tracks vs. leave R internal). |
| ST-095 | CV/gate/clock inputs: as mono (Reverse, Rate CV, Intensity CV, Echo Volume CV, Reverb Volume CV, Mode CV, Clock, Tape Age CV). |
| ST-096 | New panel controls: Routing switch, Cross, Feedback Balance, Transport Coupling, Spring Width (knobs); per-head pan ×4, H4 level, Head stagger, Reverb Pan (trimpots). |
| ST-097 | New context-menu items: Head ratio, Ratio change behavior, H4 routing, Stereo alignment. |

Jack count: 24 (vs. 15 on the mono module).

---

## 9. Non-functional requirements

| ID | Requirement |
|---|---|
| ST-100 | **Mono regression match:** with IN L only (normalled), Routing = Dual, Transport Coupling = 0, Stereo alignment = Aligned, Tape Age = 0, Head stagger = 0, pans centre, Feedback Balance centre, H4 level = 0, Spring Width = 0, Head ratio = Classic, Machine noise floor off and a fixed defect-layout seed, OUT L and OUT R SHALL each match the mono module's OUT within a tolerance **[TBD]**. |
| ST-101 | **Mono compatibility:** at default settings, L+R SHALL NOT show audible comb filtering; spring reverb mono sum per ST-073. |
| ST-102 | **CPU:** target ≤ 2.2× the mono module at default settings, Eco off **[validate after profiling]**. Eco mode SHALL reduce saturation oversampling on both tracks and disable the spring D path. |
| ST-103 | **Memory:** D IR set shared process-wide via the existing IR cache; per-instance FFT spectra growth to be measured. |
| ST-104 | Thread safety of the IR cache (epoch-guarded reclamation) SHALL extend to the D set; `make -C tools/regress stress` SHALL cover it. |
| ST-105 | Patch state (defect layout incl. per-track edge bias, all menu options) SHALL save and restore; parameters SHALL only ever be appended to preserve patch compatibility across versions. |

---

## 10. Open items

1. ST-011 — Does high Transport Coupling also decouple slow speed drift?
2. ST-063 — Does the Cross knob have a role in Dual / Ping-Pong?
3. ST-092 — OUT L mono-sum when OUT R unpatched?
4. ST-094 — FB RETURN normalling.
5. Values: azimuth range, crosstalk curve, head stagger range, glide time, pan law, Feedback Balance range, regression tolerance, panel width.

---

## 11. Market references considered

- **Strymon Volante:** four heads with per-head level/feedback/pan; head-spacing morph (Even, Triplet, Golden, Silver). → per-head pan, H4, head-ratio presets.
- **Make Noise Mimeophon:** Skew (opposite L/R offset), Halo, ping-pong with summed input. → head stagger, summed Ping-Pong.
- **Soundtoys EchoBoy:** Single / Dual / Ping-Pong / Rhythm; Ping-Pong sums the input. → Routing switch.
- **Boss RE-202:** stereo Space Echo, 4th virtual head, independent stereo reverb, New/Aged tape. → H4 and stereo-spring precedent.
- **UAD Galaxy Tape Echo:** separate echo and reverb pan. → Reverb Pan.
- **Chase Bliss Generation Loss MKII:** "malfunctioning stereo image" tied to its Failure control. → stereo artifacts driven by Tape Age.
