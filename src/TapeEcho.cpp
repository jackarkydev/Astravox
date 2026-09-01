// SPDX-License-Identifier: GPL-3.0-only

#include "plugin.hpp"
#include "widgets.hpp"
#include "SpringConvolver.hpp"
#include "SpringLevelEnvelope.hpp"
#include <dsp/resampler.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

// Flush denormals to zero on the current (audio) thread. ARM scalar FP does not
// flush subnormals by default, and Rack's denormal-flush is x86/SSE-only — so on
// Apple Silicon, filter states that decay into the subnormal range get processed
// 10-100× slower, causing intermittent CPU-spike "clicks" (worse at high
// Intensity, partly hidden by a larger buffer). Set once per audio thread; cheap.
static inline void enableFlushToZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    if (!(fpcr & (1ULL << 24))) {                 // FZ bit
        fpcr |= (1ULL << 24);
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
    }
#elif defined(__x86_64__) || defined(__i386__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

static inline float xorshiftFloat(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return ((state & 0xFFFFFF) / float(0x800000)) - 1.0f;
}

// Repeat Rate taper — measured knob→tape-speed law. Not linear: dead zones at
// both travel ends, roughly geometric through the middle. Span 2.58:1, not
// the 3.33:1 the manual's spec implies.
static constexpr int TAPER_N = 11;
static constexpr float TAPER_SPEED[TAPER_N] = {
    0.3876f, 0.3890f, 0.4147f, 0.4597f, 0.5137f, 0.5834f,
    0.6600f, 0.7655f, 0.8630f, 0.9864f, 1.0000f,
};

// The three reference knob positions (CCW / detent / CW), in measured
// tapeSpeed units.
static constexpr float SPEED_SLOW = TAPER_SPEED[0];    // 0.3876 — fully CCW
static constexpr float SPEED_MED  = TAPER_SPEED[5];    // 0.5834 — centre detent
static constexpr float SPEED_FAST = TAPER_SPEED[10];   // 1.0000 — fully CW

// Knob position (0..1) → tape speed, piecewise-linear across the measured ticks.
// Speed is interpolated rather than delay because speed is what the motor servo
// actually controls; delay is the derived quantity.
static inline float taperSpeed(float rate) {
    float x = rate < 0.f ? 0.f : (rate > 1.f ? 1.f : rate);
    x *= float(TAPER_N - 1);
    int i = int(x);
    if (i >= TAPER_N - 1) return TAPER_SPEED[TAPER_N - 1];
    float f = x - float(i);
    return TAPER_SPEED[i] + f * (TAPER_SPEED[i + 1] - TAPER_SPEED[i]);
}

// Inverse of taperSpeed() — the knob position that produces a given tape
// speed. Exact because TAPER_SPEED is monotonic and piecewise linear.
static inline float taperKnobPos(float speed) {
    if (speed <= TAPER_SPEED[0])            return 0.f;
    if (speed >= TAPER_SPEED[TAPER_N - 1])  return 1.f;
    for (int i = 0; i < TAPER_N - 1; i++) {
        float a = TAPER_SPEED[i], b = TAPER_SPEED[i + 1];
        if (speed <= b) {
            float f = (b > a) ? (speed - a) / (b - a) : 0.f;
            return (float(i) + f) / float(TAPER_N - 1);
        }
    }
    return 1.f;
}

// Nearest musical multiple (sync mode only), chosen in log space so ratio
// distance decides, not linear distance. kMax caps the choice to what fits
// the delay buffer.
static inline float nearestMusicalMultiple(float k, float kMax) {
    static const float SET[] = {0.25f, 0.5f, 0.75f, 1.f, 1.5f, 2.f, 3.f, 4.f, 6.f, 8.f};
    float best = SET[0], bestD = 1e9f;
    const float lk = std::log(std::max(1e-4f, k));
    for (int i = 0; i < 10; i++) {
        if (SET[i] > kMax && i > 0) continue;
        float d = std::fabs(std::log(SET[i]) - lk);
        if (d < bestD) { bestD = d; best = SET[i]; }
    }
    return best;
}

// Gang, CV half — applies the same proportional scaling the knob gesture uses
// to the Intensity CV, so a patched LFO produces a repeating auto-twist
// instead of just modulating feedback. Never writes a param (CV is summed at
// read time, per VCV convention).
static inline float gangApplyCv(float f0, float h0, float h) {
    if (h >= h0) {
        float d = 1.f - h0;
        return (d > 1e-6f) ? f0 + (h - h0) / d * (1.f - f0) : f0;
    }
    return (h0 > 1e-6f) ? f0 * (h / h0) : f0;
}

// Clock-division metadata, at file scope (ODR-use workaround for a
// subscripted static constexpr class member under C++11).
static constexpr int CLOCK_N_DIV = 10;
// Division family. The tempo-drift fallback prefers the nearest reachable
// division of the SAME family, so 1/8 falls back to 1/16 rather than to a
// dotted value.
enum DivClass { DIV_STRAIGHT, DIV_DOTTED, DIV_TRIPLET };
static constexpr DivClass CLOCK_DIV_CLASS[CLOCK_N_DIV] = {
    DIV_DOTTED, DIV_STRAIGHT, DIV_DOTTED,   DIV_TRIPLET, DIV_STRAIGHT,
    DIV_DOTTED, DIV_TRIPLET,  DIV_STRAIGHT, DIV_TRIPLET, DIV_STRAIGHT,
};
// Clock multiplier applied to the incoming clock rate (xN speeds up, /N slows
// down). Also fixes clocks that emit more than one pulse per beat, which
// would otherwise read as the wrong tempo.
static constexpr int CLOCK_N_MULT = 5;
static constexpr float CLOCK_MULTS[CLOCK_N_MULT] = { 4.f, 2.f, 1.f, 0.5f, 0.25f };
static const char* const CLOCK_MULT_NAMES[CLOCK_N_MULT] = {
    "x4", "x2", "x1", "/2", "/4",
};
static constexpr int CLOCK_MULT_DEFAULT = 2;   // x1

static const char* const CLOCK_DIV_NAMES[CLOCK_N_DIV] = {
    "1/4 dotted", "1/4", "1/8 dotted", "1/4 triplet", "1/8",
    "1/16 dotted", "1/8 triplet", "1/16", "1/16 triplet", "1/32",
};

// Rate NUDGE. While sync is engaged, the Rate knob becomes a bounded
// bidirectional trim (±6%) around the synced tape speed, centred on 0.5 —
// sized so full deflection still stays nearer the chosen division than any
// neighbour. Percent, not milliseconds, so the feel is tempo-invariant.
static constexpr float CLOCK_NUDGE_MAX_PCT = 6.f;

// The Rate knob's typed/tooltip value follows what the knob currently means:
// the nudge (in percent) while synced, the plain 0..1 taper position
// otherwise. Distinct from TapeEchoRateKnob's own indicator-angle override,
// which only affects the pointer, not the tooltip.

// Validating JSON readers — jansson's raw accessors silently return a
// wrong-type default instead of failing, and narrowing an out-of-range/NaN
// double before clamping is undefined behaviour. These validate type,
// finiteness, and range before ever casting. Patch files are untrusted input.
static inline float jsonReal(json_t* o, const char* key, float def, float lo, float hi) {
    json_t* j = json_object_get(o, key);
    if (!j || !json_is_number(j)) return def;
    double v = json_number_value(j);
    if (!std::isfinite(v)) return def;
    return (float)((v < (double)lo) ? (double)lo : (v > (double)hi) ? (double)hi : v);
}

static inline int jsonInt(json_t* o, const char* key, int def, int lo, int hi) {
    json_t* j = json_object_get(o, key);
    if (!j || !json_is_number(j)) return def;
    double v = json_number_value(j);
    if (!std::isfinite(v)) return def;
    if (v < (double)lo) return lo;
    if (v > (double)hi) return hi;
    return (int)v;
}

static inline bool jsonBool(json_t* o, const char* key, bool def) {
    json_t* j = json_object_get(o, key);
    return json_is_boolean(j) ? json_boolean_value(j) : def;
}

struct TapeEchoRateQuantity : ParamQuantity {
    float getDisplayValue() override;
    void setDisplayValue(float displayValue) override;
    std::string getDisplayValueString() override;
    void setDisplayValueString(std::string s) override;
    std::string getUnit() override;
};

// While clock sync snaps the mode, the panel pointer follows the synced mode
// but MODE_PARAM stays where the user parked it — untouched, so a patch save
// captures the parked value exactly like any other param, with no separate
// bookkeeping needed. The tooltip shows only what's actually playing while
// synced; the parked value isn't restated there, since disengaging sync
// (unpatching the clock) is the deliberate act that brings it back — both to
// the pointer and to the tooltip.
struct TapeEchoModeQuantity : SwitchQuantity {
    std::string getDisplayValueString() override;
};

// Feedback-loop calibration helpers, derived from hardware self-oscillation
// measurements.

// Loop hysteresis: once self-oscillation is established, Intensity is boosted
// by this offset so the loop holds on as the knob is reduced, collapsing only
// near the measured falling-unity point. Per-speed offsets, measured from
// hardware; linearly interpolated.
static inline float hystOffset(float tapeSpeed) {
    if (tapeSpeed <= SPEED_SLOW) return 0.490f;
    if (tapeSpeed >= SPEED_FAST) return 0.094f;
    if (tapeSpeed <  SPEED_MED)
        return 0.490f + (0.296f - 0.490f) * (tapeSpeed - SPEED_SLOW) / (SPEED_MED - SPEED_SLOW);
    return 0.296f + (0.094f - 0.296f) * (tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED);
}

// Per-mode loop-gain trim. Mode 11 (all heads summed) is the only mode whose
// measured self-oscillation behavior differs — its multi-head phase
// cancellation suppresses it through slow/medium speed, only ramping in
// between medium and fast.
static inline float modeTrim(int mode, float tapeSpeed) {
    if (mode != 11) return 1.f;
    if (tapeSpeed <= SPEED_MED)  return 0.665f;
    if (tapeSpeed >= SPEED_FAST) return 1.f;
    return 0.665f + (1.f - 0.665f) * (tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED);
}

// Intensity loop-gain magnitude correction, ramping in dB from
// INTENSITY_TAPER_LOW_ANCHOR up to a per-speed target that lands exactly on
// that speed's measured self-oscillation onset knob, then holds. Monotonic by
// construction, so no volume dip is possible. Targets are a log-linear fit of
// the module's own measured gain curve, confirmed by ear.
static constexpr float INTENSITY_TAPER_LOW_ANCHOR = 0.4f;   // matches the knob's own default position — free null below this
static constexpr float INTENSITY_TAPER_ONSET[3] = { 0.823f, 0.796f, 0.761f };  // slow/med/fast, measured
static constexpr float INTENSITY_TAPER_DB[3]    = { 1.554f, 1.083f, 0.34f };  // slow/med/fast, see comment above

static inline float intensityTaper(float knob, float tapeSpeed) {
    float onset, dB;
    if (tapeSpeed <= SPEED_SLOW) {
        onset = INTENSITY_TAPER_ONSET[0]; dB = INTENSITY_TAPER_DB[0];
    } else if (tapeSpeed >= SPEED_FAST) {
        onset = INTENSITY_TAPER_ONSET[2]; dB = INTENSITY_TAPER_DB[2];
    } else if (tapeSpeed < SPEED_MED) {
        float t = (tapeSpeed - SPEED_SLOW) / (SPEED_MED - SPEED_SLOW);
        onset = INTENSITY_TAPER_ONSET[0] + t * (INTENSITY_TAPER_ONSET[1] - INTENSITY_TAPER_ONSET[0]);
        dB    = INTENSITY_TAPER_DB[0]    + t * (INTENSITY_TAPER_DB[1]    - INTENSITY_TAPER_DB[0]);
    } else {
        float t = (tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED);
        onset = INTENSITY_TAPER_ONSET[1] + t * (INTENSITY_TAPER_ONSET[2] - INTENSITY_TAPER_ONSET[1]);
        dB    = INTENSITY_TAPER_DB[1]    + t * (INTENSITY_TAPER_DB[2]    - INTENSITY_TAPER_DB[1]);
    }
    float rampT = (knob - INTENSITY_TAPER_LOW_ANCHOR) / std::max(0.01f, onset - INTENSITY_TAPER_LOW_ANCHOR);
    rampT = std::max(0.f, std::min(1.f, rampT));
    return std::pow(10.f, (rampT * dB) / 20.f);
}

#include "TapeEchoHysteresis.hpp"

#include "TapeEchoSaturation.hpp"

// VU pin + peak-LED thresholds, calibrated against the recordIn tap point.
// VU_PIN sits below recordIn's measured ceiling so a hot, driven hit can
// genuinely pin the needle. PEAK_LED_THRESHOLD matches the RE-201 service
// manual's factory trim points, sitting near the meter's 0VU rather than up
// near pin.
static constexpr float VU_PIN             = 6.2f;
static constexpr float PEAK_LED_THRESHOLD = 4.5f;
// Cosmetic needle skew — a pure display warp on the VU needle angle
// (TapeEchoVUMeter::draw()); doesn't affect vu.level, VU_PIN, or
// PEAK_LED_THRESHOLD. <1 pushes the needle right; 1.0 = linear.
static constexpr float VU_NEEDLE_DISPLAY_GAMMA = 0.75f;

#include "TapeEchoFilters.hpp"

#include "TapeEchoSpring.hpp"

#include "TapeEchoWowFlutter.hpp"

// =============================================================================

// Drive tilt presets: scale the Input drive knob's character across three axes
// (drive-mapping base, volume-compensation exponent, HF rolloff floor).
// Volume swing is held at ±6 dB across all three presets (Gentle/Moderate/
// Aggressive); only drive amount and HF rolloff differ.
static constexpr float TILT_DRIVE_BASE[3] = { 1.5945f,     3.189f,      6.378f  };
static constexpr float TILT_COMP_EXP[3]   = { -0.485654f,  0.402309f,  0.625903f };
static constexpr float TILT_HF_FLOOR[3]   = { 7000.f,      3000.f,      1500.f };

// Tape-echo swell fix. Part 1: input-keyed feedback-decay taper exponent
// (applied to min(drive,1); 1.0 = linear-in-drive). Part 2: asymmetric slew on
// the ECHO output's drive makeup — slow to follow Input DOWN (so the makeup can't
// spike the still-hot delayed repeats), fast to follow Input UP.
static constexpr float INPUT_FB_DECAY_EXP     = 1.0f;
static constexpr float ECHO_MAKEUP_DOWN_TAU   = 1.0f;    // s — slow (Input down)
static constexpr float ECHO_MAKEUP_UP_TAU     = 0.02f;   // s — fast (Input up)

// res/spring_noise.wav is 24-bit PCM at 96 kHz, stored scaled UP by 1024 so it
// uses the full sample range. The capture peaks at -63.7 dBFS, so stored at its
// natural level it would waste ~64 dB of headroom -- at 24-bit that still leaves
// only ~80 dB of usable range, no better than the 16-bit-plus-scaling it
// replaced. Scaled, the round trip measures 130.7 dB SNR.
//
// This constant undoes that scaling at load, restoring the exact level
// MACHINE_NOISE_OUTPUT_GAIN (10.09f, explicitly flagged as not self-calibrating)
// is calibrated against. A power of two, so the round trip is exact apart from
// quantisation -- no calibration constant moves.
//
// The IRs need no such trick: halving their sample rate to 48 kHz freed enough
// bytes to store them 24-bit, which has ample headroom at their natural peaks,
// so they load unscaled at the peak 0.5 that SPRING_CONV_OUTPUT_GAIN (13.8f,
// tuned by ear) assumes.
//
// INVARIANT, if the noise file is ever recut: its length must stay an exact
// multiple of 1600 samples (one 60 Hz cycle at 96 kHz). The capture carries
// mains hum only 14.1 dB below its total RMS, and all three readers wrap on
// length, so a non-multiple phase-jumps that hum audibly once per loop. The
// current file is 768000 samples = 480 cycles exactly. Its rate cannot change
// either: it is read one element per engine sample with no resampling, so a
// different rate would shift both its spectrum and its loop period.
static constexpr float SPRING_NOISE_FILE_GAIN = 1.f / 1024.f;

struct TapeEcho : Module {
    enum ParamId {
        REPEAT_RATE_PARAM,
        INTENSITY_PARAM,
        ECHO_VOLUME_PARAM,
        REVERB_VOLUME_PARAM,
        BASS_PARAM,
        TREBLE_PARAM,
        MODE_PARAM,
        MOTOR_STOP_PARAM,
        REVERSE_PARAM,
        INPUT_LEVEL_PARAM,
        OUTPUT_PAD_PARAM,            // H/M/L drive switch (0=L, 1=M, 2=H)
        // Clock-sync Rate nudge lives on its own param, not REPEAT_RATE_PARAM,
        // so it always starts centred and leaves the user's parked Rate
        // untouched for the sync-disengage fallback. Appended last, never inserted.
        RATE_NUDGE_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        IN_INPUT,
        // Deprecated (gate input dropped, POWER toggle is sole control). Slot
        // kept at its original index so saved patches don't misroute cables.
        MOTOR_STOP_INPUT_DEPRECATED,
        REVERSE_INPUT,
        FB_RETURN_INPUT,
        RATE_CV_INPUT,               // Stage 9: knob CV jacks
        INTENSITY_CV_INPUT,
        ECHO_VOLUME_CV_INPUT,
        REVERB_VOLUME_CV_INPUT,
        MODE_INPUT,                  // Stage 9: 1V/step mode CV
        CLOCK_INPUT,                 // Stage 9: tempo-sync
        TAPE_AGE_INPUT,              // Stage 9: re-added CV (reverses dc3b8be)
        INPUTS_LEN
    };
    enum OutputId {
        OUT_OUTPUT,
        OUT_H1_OUTPUT,
        OUT_H2_OUTPUT,
        OUT_H3_OUTPUT,
        FB_SEND_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LIGHTS_LEN
    };

    // Delay buffer (D2: 4s, sized in onSampleRateChange)
    std::vector<float> delayBuf;
    int writeIdx = 0;

    // tapeSpeed smoothing — critically-damped 2nd-order step response (two
    // cascaded one-pole stages), fit to the measured Repeat Rate speed-change
    // transient. tauUp/tauDown asymmetry matches the measured direction split.
    struct CriticallyDamped {
        float state1 = 1.0f, state2 = 1.0f;
        float coefUp = 0.0f, coefDown = 0.0f;

        float tauUp = 0.0917f, tauDown = 0.1319f;

        void onSampleRateChange(float sr) {
            coefUp   = 1.0f - std::exp(-1.0f / (tauUp   * sr));
            coefDown = 1.0f - std::exp(-1.0f / (tauDown * sr));
        }
        void instant(float v) { state1 = state2 = v; }
        float process(float target) {
            float c1 = (target > state1) ? coefUp : coefDown;
            state1 += c1 * (target - state1);
            float c2 = (state1 > state2) ? coefUp : coefDown;
            state2 += c2 * (state1 - state2);
            return state2;
        }
    };
    CriticallyDamped tapeSpeedSmoother;
    float tapeSpeedTarget = 1.0f;
    float tapeSpeed = 1.0f;

// Motor Stop glide. CriticallyDamped above is fit to the Repeat Rate
// knob's live speed-change transient and extrapolated to also drive Motor
// Stop's spin-down/re-engage. SinglePole instead reproduces the plain
// single-exponential glide measured directly from an actual motor
// stop/start event — used for that specific transition instead, since an
// A/B by ear preferred the directly-measured glide over the extrapolated one.
    struct SinglePole {
        float state = 1.0f;
        float coefUp = 0.0f, coefDown = 0.0f;
        float tauUp = 0.2151f, tauDown = 0.2751f;  // measured single-pole taus

        void onSampleRateChange(float sr) {
            coefUp   = 1.0f - std::exp(-1.0f / (tauUp   * sr));
            coefDown = 1.0f - std::exp(-1.0f / (tauDown * sr));
        }
        void instant(float v) { state = v; }
        float process(float target) {
            float c = (target > state) ? coefUp : coefDown;
            state += c * (target - state);
            return state;
        }
    };
    SinglePole motorStopSmootherMeasured;
    bool motorStopGlideActive = false;

    // Mode-switch tap crossfade
    float tapGainTarget[3] = {0.f, 0.f, 0.f};
    float tapGain[3]       = {0.f, 0.f, 0.f};
    float tapGainTau  = 0.020f;
    float tapGainCoef = 0.0f;


    // Reverb output gate — ramps spring output and noise floor toward 0 in
    // echo-only modes, back to 1 in reverb-active modes. τ ≈ 80 ms.
    float reverbActiveGate     = 1.0f;
    float reverbActiveGateTau  = 0.080f;
    float reverbActiveGateCoef = 0.0f;

    // Head-tap spacing, measured: ratio 1 : 1.998 : 2.967 across the three
    // heads. Absolute scale anchored so head 1 at full speed = 63.51 ms.
    const float TAP_FRACTIONS[3]  = {0.141420f, 0.282618f, 0.419664f};
    const float BASE_LOOP_SECONDS = 0.4491f;

    // Clock-sync "true grid" fractions — an exact 1 : 2 : 3, deliberately not
    // this unit's measured geometry (h3/h1 ≈ 2.9675), so a grid-locked head 1
    // doesn't leave head 3 drifting early as feedback compounds it. Only in
    // force while clock snap is active.
    const float GRID_FRACTIONS[3] = {0.141420f, 0.282840f, 0.424260f};

    // Live tap fractions — slewed between TAP_FRACTIONS and GRID_FRACTIONS so
    // engaging/disengaging sync glides instead of clicking the read pointer.
    float tapFrac[3]     = {0.141420f, 0.282618f, 0.419664f};
    float tapFracTau     = 0.060f;
    float tapFracCoef    = 0.0f;
    // Derived from the measured taper so the two can never drift apart.
    static constexpr float TAPE_SPEED_MIN   = TAPER_SPEED[0];
    static constexpr float TAPE_SPEED_RANGE = TAPER_SPEED[TAPER_N - 1] - TAPER_SPEED[0];
    const float ANTI_DENORMAL_DC  = 1e-20f;

    // Tape Age loop-noise injection gain, quadratic in tapeAge (values shown
    // are at full age). Always present when aged; boosted when the machine
    // noise floor is on, as masking compensation against that floor.
    static constexpr float LOOP_NOISE_GAIN       = 6.0f;
    static constexpr float LOOP_NOISE_MASK_BOOST = 5.0f;   // when the floor is on
    // Read position for the above. Deliberately never reset — see OutputArtifacts.
    int loopNoisePos = 0;

    HeadEQ headEQ[3];
    // Forward-only twin of headEQ, feeding the feedback path instead of the
    // listener-facing output — needs its own filter state since headEQ[i]
    // can't process two signals per sample without corrupting its memory.
    HeadEQ headEQFwd[3];

    // Head-EQ coefficient cache. interpolateCoefs() is expensive and runs six
    // times per sample, so it's gated on change-detection rather than a time
    // divider. Must be invalidated on sample-rate change.
    static constexpr float EQ_RECOOK_EPS = 1e-4f;
    float eqLastSpeed[3] = {-1.f, -1.f, -1.f};
    float eqLastHf[3]    = {-1.f, -1.f, -1.f};
    void invalidateHeadEqCache() {
        for (int i = 0; i < 3; i++) { eqLastSpeed[i] = -1.f; eqLastHf[i] = -1.f; }
    }

    // Inter-head crosstalk — small-gain leakage from the other heads at their
    // nominal-tap positions. Gains are measured (mean across 3 speeds).
    struct Crosstalk {
        float adjacentGain    = 0.0296f;  // -30.6 dB (heads 0↔1, 1↔2)
        float nonAdjacentGain = 0.0139f;  // -37.1 dB (heads 0↔2)
    };
    Crosstalk crosstalk;

    ShelfFilter wetBass, wetTreble;
    float lastBass = 0.f, lastTreble = 0.f;
    // Forward-only twin, mirrors wetBass/wetTreble's coefficients (same knob
    // values) but processes fwdWetSum — the signal that feeds the feedback
    // loop — with its own independent filter state.
    ShelfFilter fwdWetBass, fwdWetTreble;

    // Spring reverb (Stage 5)
    SpringReverb spring;
    // Cached mode lookup for reverb activation per Mode setting
    bool reverbActive = false;
    // Input-RMS estimator (single-pole envelope follower) for drive scaling
    float inEnv      = 0.f;
    float inEnvCoef  = 0.f;
    static constexpr float IN_ENV_TAU = 0.050f;

    // RE-201 feedback gain ceiling: at max Intensity the loop gain slightly
    // exceeds 1.0 so the signal can build into self-oscillation; saturation
    // bounds the amplitude. Sized so the emulated unity crossing lands on
    // hardware's measured crossing. Topology has since evolved but the
    // crossing still lands close to target; the residual gap at Fast was
    // deliberately accepted, not an open item.
    const float FEEDBACK_GAIN_MAX = 2.10f;

    // "Scary" runaway at the top of the Intensity knob — past SCARY_THRESH the
    // loop gain is pushed beyond the measured unity crossing so it slams into
    // saturation. A deliberate modular liberty above the calibrated range.
    static constexpr float SCARY_THRESH   = 0.85f;  // knob pos where runaway begins
    static constexpr float SCARY_MAX_MULT = 2.0f;   // extra fbGain mult at knob = 1.0
    // Swell the runaway in gradually (slow up) but release it promptly (fast down),
    // so entering the scary zone builds up dramatically instead of snapping to full.
    // Asymmetric one-pole; decouples swell TIME from ultimate wildness. Tune by ear.
    float scarySlew = 1.f;                              // current slewed boost (1.0 = none)
    float scarySlewUpCoef = 0.f, scarySlewDownCoef = 0.f;
    static constexpr float SCARY_SLEW_UP_TAU   = 0.8f;  // s — slow swell up
    static constexpr float SCARY_SLEW_DOWN_TAU = 0.2f;  // s — prompt release

    // Loop-hysteresis state. loopEnv is a slow envelope follower of the
    // feedback-tap level; "hotness" derived from it gates hystOffset() so only
    // an established self-oscillation holds on as Intensity is reduced.
    float loopEnv        = 0.f;
    float loopEnvAtkCoef = 0.f;
    float loopEnvRelCoef = 0.f;
    static constexpr float LOOP_ENV_ATK_TAU = 0.08f;  // s — heats quickly as the loop builds
    static constexpr float LOOP_ENV_REL_TAU = 0.60f;  // s — cools slowly so collapse fades, not snaps
    static constexpr float LOOP_ENV_LO      = 0.5f;   // V
    static constexpr float LOOP_ENV_HI      = 3.0f;   // V

    // `fbLevel` feeding the loopEnv tracker is pre-conditioned with a short
    // RMS-style smoothing stage rather than a raw instantaneous sample, so a
    // signal's crest factor doesn't skew how quiet the loop appears.
    float fbRmsZ = 0.f;
    float fbRmsCoef = 0.f;
    static constexpr float FB_RMS_TAU = 0.004f;  // 4ms

    // Asymmetric-slewed drive makeup for the ECHO output only. Lags
    // driveComp on the way DOWN so an Input pull-down can't spike the still-hot
    // delayed repeats; snaps on the way UP. Converges to driveComp in steady state
    // (echo output then bit-identical to the old /driveComp). Init 1.0 = unity.
    float echoMakeupSlew    = 1.f;
    float echoMakeupDownCoef = 0.f;
    float echoMakeupUpCoef   = 0.f;

    // Write-path loop high-pass — one 1-pole HPF in the feedback loop, corner
    // driven by loop `hotness`: ~12 Hz for echoes (full bass, blocks the
    // saturator's DC bias), rising toward ~80 Hz as self-oscillation
    // establishes (tames sub-bass hum). State/coefficients live in TapeWriteStage.
    static constexpr float WRITE_HPF_TAU_LO = 0.0133f;  // s (~12 Hz)
    static constexpr float WRITE_HPF_TAU_HI = 0.0020f;  // s (~80 Hz)

    // Per-pass tape/head bandwidth loss in the feedback loop. Real tape + heads
    // shed HF and LF every pass, so repeats darken/thin toward the midrange.
    // Corners are measurement-derived and speed-coupled (scale with tapeSpeed
    // so the loop peak tracks the measured self-osc shift). Covers both the
    // feedback path and a structurally identical first-pass twin for
    // tapeInputDrive — shared coefficients, independent filter memory.
    struct LoopLoss {
        // LP → HP → sub-HP, one instance per path. The shared coefficients are
        // passed in rather than held, because both paths use the same ones.
        struct Chain {
            float lpZ = 0.f, hpZ = 0.f, subHpZ = 0.f;
            void resetState() { lpZ = hpZ = subHpZ = 0.f; }
            float process(float x, float lpCoef, float hpCoef, float subHpCoef) {
                lpZ += lpCoef * (x - lpZ);
                float hp = lpZ - hpZ;
                hpZ += hpCoef * hp;
                float subHp = hp - subHpZ;
                subHpZ += subHpCoef * subHp;
                return subHp;
            }
        };
        Chain fb;      // the feedback path (gen2+)
        Chain input;   // first-pass twin (gen1), its own filter memory

        float lpCoef = 0.f, hpCoef = 0.f, subHpCoef = 0.f;        // shared by both paths
        float airWidenHz = 0.f;   // air-widen speed taper
        float hpExtraHz  = 0.f;   // Fast-only HP extra (retired to a no-op)

        // Filter memory only — see TapeEcho::resetFilterState().
        void resetFilterState() { fb.resetState(); input.resetState(); }

        struct Result { float feedback, firstPass; };

        Result process(float loopFeedback, float tapeInputDrive, float tapeSpeed,
                       float loopLevelDb, float sr) {
            // The feedback tap does not run through its own saturator — an
            // amplitude-dependent saturation curve there caused the per-pass
            // decay rate to slow over the course of a decay, where hardware
            // holds an almost constant rate throughout. `FB_LOOP_LOSS_BASE` is
            // a flat loss instead, sized to match hardware's measured per-pass
            // decay rate, then relieved (less loss) as the loop quiets via
            // `lossReliefDb`/`fbLoopLoss` below, since a flat cut alone decays
            // faster than hardware except at one specific speed/level point.
            float lossReliefDb = std::min(FB_LOOP_LOSS_RELIEF_MAX_DB,
                                           FB_LOOP_LOSS_RELIEF_DB_PER_DB * std::max(0.f, -loopLevelDb))
                                + fbLoopLossSpeedRelief(tapeSpeed);
            float fbLoopLoss = std::min(1.f, FB_LOOP_LOSS_BASE * std::pow(10.f, lossReliefDb / 20.f));
            float fbSat = loopFeedback * fbLoopLoss;
            // Flat extra cut (see comment above LOOP_LOSS_EXTRA_DB) —
            // added alongside fbLoopLoss rather than folded into FB_LOOP_LOSS_BASE,
            // which is shared, already-validated state.
            float loopLossExtraGainLin = std::pow(10.f, -LOOP_LOSS_EXTRA_DB / 20.f);
            fbSat *= loopLossExtraGainLin;
            // Per-pass tape/head bandwidth loss: shed a little HF then LF from the
            // feedback each loop so repeats darken/thin toward midrange (authentic tape
            // echo) and in-loop tone boosts can't compound into hiss/boom. Always on.
            // Speed-coupled: both corners scale with tape speed so the loop peak tracks
            // the measured ~360→570 Hz self-osc shift.
            float loopBwScale = 1.f + LOOP_BW_SPEED_SCALE
                              * clamp((tapeSpeed - TAPE_SPEED_MIN) / TAPE_SPEED_RANGE, 0.f, 1.f);
            // Air-widen speed taper, Slow/Med/Fast anchors interpolated —
            // same idiom as `inputFbBoost`/`modeTrim` elsewhere in this file.
            if (tapeSpeed <= SPEED_SLOW) {
                airWidenHz = LOOP_LP_AIR_WIDEN_HZ_SLOW;
            } else if (tapeSpeed >= SPEED_FAST) {
                airWidenHz = LOOP_LP_AIR_WIDEN_HZ_FAST;
            } else if (tapeSpeed < SPEED_MED) {
                float t = (tapeSpeed - SPEED_SLOW) / (SPEED_MED - SPEED_SLOW);
                airWidenHz = LOOP_LP_AIR_WIDEN_HZ_SLOW
                           + (LOOP_LP_AIR_WIDEN_HZ_MED - LOOP_LP_AIR_WIDEN_HZ_SLOW) * t;
            } else {
                float t = (tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED);
                airWidenHz = LOOP_LP_AIR_WIDEN_HZ_MED
                           + (LOOP_LP_AIR_WIDEN_HZ_FAST - LOOP_LP_AIR_WIDEN_HZ_MED) * t;
            }
            // Real hardware's repeat tail brightens as it decays. Widen the LP
            // corner as `loopEnv` drops below a "loud" reference, so a loud
            // repeat 1 is unchanged and only quieter repeats gain HF back.
            float loopBrighten = std::min(LOOP_BRIGHTEN_MAX_HZ, LOOP_BRIGHTEN_HZ_PER_DB * std::max(0.f, -loopLevelDb));
            lpCoef = 1.f - std::exp(-2.f * float(M_PI) * (LOOP_LP_HZ_SLOW * loopBwScale + loopBrighten + airWidenHz) / sr);
            // Fast-only low-mid trim: zero at Slow and Med, ramping in only
            // between Med and Fast. CURRENTLY A NO-OP — LOOP_HP_EXTRA_HZ_FAST
            // was retired to 0 (documented at its definition), so this whole
            // ramp evaluates to zero. Kept for the shape, not live.
            hpExtraHz = (tapeSpeed <= SPEED_MED) ? 0.f
                      : LOOP_HP_EXTRA_HZ_FAST
                        * ((tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED));
            hpCoef = 1.f - std::exp(-2.f * float(M_PI) * (LOOP_HP_HZ_SLOW * loopBwScale + hpExtraHz) / sr);
            subHpCoef = 1.f - std::exp(-2.f * float(M_PI) * (LOOP_SUBHP_HZ_SLOW * loopBwScale) / sr);
            // HF loss (one-pole LP) → LF loss (one-pole HP) → sub-bass shape
            // correction (a cascaded extra HP). Both paths run this SAME three-stage
            // chain off the SAME coefficients and differ only in filter memory.
            fbSat = fb.process(fbSat, lpCoef, hpCoef, subHpCoef);
            // Same per-pass loss (fbLoopLoss level cut, then LP-then-HP
            // bandwidth narrowing) applied to tapeInputDrive, with its own
            // filter memory so it doesn't disturb fbSat's state — this covers
            // gen1 (formed before any feedback exists), which would otherwise
            // skip the per-pass loss entirely.
            float inputPassLoss = tapeInputDrive * fbLoopLoss * loopLossExtraGainLin;
            float firstPass = input.process(inputPassLoss, lpCoef, hpCoef, subHpCoef);
            return { fbSat, firstPass };
        }
    };
    LoopLoss loopLoss;
    static constexpr float LOOP_LP_HZ_SLOW     = 800.f;   // HF loss corner @ slow
    static constexpr float LOOP_HP_HZ_SLOW     = 150.f;   // LF loss corner @ slow
    static constexpr float LOOP_BW_SPEED_SCALE = 0.583f;  // ×(1+scale·t): 570/360−1

    // Air-widen speed taper: an extra HF-loss corner beyond LOOP_LP_HZ_SLOW,
    // targeting a 10-16kHz band the module sheds faster than hardware per
    // pass. Widens `loopLpCoef` directly. Slow is measurement-derived; Med/Fast
    // are by-ear.
    static constexpr float LOOP_LP_AIR_WIDEN_HZ_SLOW = 1000.f;
    static constexpr float LOOP_LP_AIR_WIDEN_HZ_MED  = 5000.f;
    static constexpr float LOOP_LP_AIR_WIDEN_HZ_FAST = 7000.f;
    // (state: loopLoss.airWidenHz)

    // Fast-only extra loop HIGH-pass — retired to a no-op (0.f). Tried values
    // over-cut decay on low-mid content; the target measurement itself is suspect.
    static constexpr float LOOP_HP_EXTRA_HZ_FAST = 0.f;     // retired; was 65/130/263
    // (state: loopLoss.hpExtraHz)

    // Per-pass timing jitter target, in microseconds RMS (see WowFlutter's
    // jitter block). Med/Fast fit against decorrelation measured at
    // 2.6-4.1kHz, where the audible "spring" character sits; Slow is by-ear.
    static constexpr float FLUTTER_JITTER_US_SLOW = 16.6f;
    static constexpr float FLUTTER_JITTER_US_MED  = 26.3f;
    static constexpr float FLUTTER_JITTER_US_FAST = 16.2f;

    // gen1 mirrors fbSat's per-pass loss pipeline onto tapeInputDrive with
    // independent filter memory — real tape applies the same loss to a first
    // recording as to a regenerated repeat.
    // (state: loopLoss.inputLpZ / loopLoss.inputHpZ)

    // Loop-loss correction, fit from bass and mid/upper-mid sweeps against
    // hardware. A small flat extra cut (LOOP_LOSS_EXTRA_DB) plus a refit
    // sub-bass HP corner, applied alongside fbLoopLoss rather than folded into
    // FB_LOOP_LOSS_BASE. Slow-only data, extrapolated to Med/Fast.
    // (state: loopLoss.subHpZ / loopLoss.subHpCoef / loopLoss.inputSubHpZ)
    static constexpr float LOOP_SUBHP_HZ_SLOW = 40.f;
    static constexpr float LOOP_LOSS_EXTRA_DB = 1.5f;

    // Busy-material presence deficit vs hardware, two bands (midrange
    // 800-2300Hz, ~250Hz). A parametric EQ correction, sized from real capture
    // data, rather than retuning writeSat's shared, load-bearing saturation
    // params.
    //
    // IMPORTANT, load-bearing: must NOT sit inside the feedback path — an
    // earlier version applied it to the compounding loop signal directly and
    // caused a self-oscillation regression. Applied to `wet` (the audible
    // delay-buffer tap) instead, so it colors every repeat without compounding
    // loop gain.
    Biquad midColor;
    static constexpr float MIDRANGE_BOOST_FC_HZ        = 1450.f;
    static constexpr float MIDRANGE_BOOST_GAIN_DB_SLOWMED = 6.0f;  // flat through Slow AND Med
    static constexpr float MIDRANGE_BOOST_GAIN_DB_FAST    = -2.0f; // was +4.0f — Fast needs a cut, not a smaller boost
    static constexpr float MIDRANGE_BOOST_Q            = 1.0f;

    Biquad lowColor;
    static constexpr float LOW_COLOR_BOOST_FC_HZ   = 250.f;
    static constexpr float LOW_COLOR_BOOST_GAIN_DB = 4.5f;
    static constexpr float LOW_COLOR_BOOST_Q       = 1.5f;

    // First-pass HF shelf — a proper RBJ high-shelf (not a highpass-add-back
    // form, which phase-cancels through the transition band) applied only to
    // the one-time first echo. Currently zeroed at all three speeds (Med/Fast
    // made an unrelated modulation issue worse; Slow's measurement is stale) —
    // fit constants kept since the underlying measurement stands.
    Biquad firstPassShelf;
    float firstPassShelfCosW0 = 0.f, firstPassShelfSinW0 = 0.f;   // fc is fixed — cached
    static constexpr float FIRST_PASS_SHELF_FC_HZ = 2721.f;
    static constexpr float FIRST_PASS_SHELF_S     = 0.678f;       // shelf slope
    static constexpr float FIRST_PASS_SHELF_GAIN_DB_SLOW = 0.f;   // stale data — needs re-capture
    static constexpr float FIRST_PASS_SHELF_GAIN_DB_MED  = 0.f;   // was 6.75f (fitted) — retired, see comment above
    static constexpr float FIRST_PASS_SHELF_GAIN_DB_FAST = 0.f;   // gap real but marginal

    // 4-10kHz brightness deficit vs hardware — several additive-stage attempts
    // were tried and removed since the actual gap was at 10-16kHz, fixed by
    // widening `loopLpCoef`'s corner directly instead (see process()).

    // Level-dependent brightening of the LP corner above as the loop quiets,
    // sized against a measured gap around 500Hz.
    static constexpr float LOOP_BRIGHTEN_REF_V     = LOOP_ENV_HI;  // "loud" reference, volts
    static constexpr float LOOP_BRIGHTEN_HZ_PER_DB = 28.f;
    static constexpr float LOOP_BRIGHTEN_MAX_HZ    = 1500.f;

    // Flat per-pass feedback-loop loss, matched to hardware's measured average
    // decay rate, plus a relief term that reduces the loss as the loop quiets.
    static constexpr float FB_LOOP_LOSS_BASE          = 0.5565f;
    static constexpr float FB_LOOP_LOSS_RELIEF_DB_PER_DB = 0.108f;  // relief (dB less loss) per dB the loop sits below reference
    static constexpr float FB_LOOP_LOSS_RELIEF_MAX_DB    = 3.6f;    // cap — never fully removes the loss

    // Additional per-speed relief: echo tails run audibly shorter than
    // hardware's at Med/Fast specifically. Ramped zero at Slow, rising
    // between Med and Fast.
    static constexpr float FB_LOOP_LOSS_MED_RELIEF_DB  = 0.7f;
    static constexpr float FB_LOOP_LOSS_FAST_RELIEF_DB = 3.6f;  // was 1.8f

    static inline float fbLoopLossSpeedRelief(float tapeSpeed) {
        if (tapeSpeed <= SPEED_SLOW) return 0.f;
        if (tapeSpeed >= SPEED_FAST) return FB_LOOP_LOSS_FAST_RELIEF_DB;
        if (tapeSpeed < SPEED_MED)
            return FB_LOOP_LOSS_MED_RELIEF_DB * (tapeSpeed - SPEED_SLOW) / (SPEED_MED - SPEED_SLOW);
        return FB_LOOP_LOSS_MED_RELIEF_DB + (FB_LOOP_LOSS_FAST_RELIEF_DB - FB_LOOP_LOSS_MED_RELIEF_DB)
                                           * (tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED);
    }

    float bufSizeFloat = 0.f;

    // Quiet-pass expansion. The Chowdhury hysteresis saturator (`writeSat`)
    // runs every pass on the combined record signal and was compressing loud
    // vs. quiet hits' echoes toward each other more than the real hardware
    // does, for low-pitched (kick-like) content specifically. A raw bypass of
    // writeSat isn't viable on its own — it's also the loop's primary
    // gain-limiting nonlinearity, not just its saturation character — so the
    // fix is two-part: (1) a level-matched bypass, using two slow envelope
    // followers tracking |recordIn| and |writeSat's output| to isolate
    // saturation SHAPE from static level; (2) an extra cut on quiet material
    // only, on the working hypothesis that real tape lets a loud pass
    // overpower a quiet one more than the saturation curve reproduces.
    //
    // Neither part is pitch-aware on its own, and applying this to
    // non-bass content overshoots badly (measured against real captured
    // audio). Gated by a real-time bass/broadband ratio: a one-pole low-pass
    // at WRITE_BASS_LP_HZ tracked by its own slow envelope, divided by the
    // broadband envelope — 300 Hz cleanly separates bass-heavy from
    // high-pitched percussive content in measured captures. Gate ramps 0->1
    // across WRITE_BASS_GATE_LO..HI so high-pitched content passes through
    // the real, unmodified `writeSatOut` and bass content gets the full
    // bypass+expand treatment, with a smooth crossfade (not a hard switch)
    // in between.
    static constexpr float WRITE_ENV_COEF = 3e-5f;  // ~0.5-1s tracking window at typical SRs
    static constexpr float QUIET_EXPAND_DB_PER_DB = 0.09f;
    static constexpr float QUIET_EXPAND_MAX_DB    = 4.f;
    static constexpr float WRITE_BASS_LP_HZ   = 300.f;
    static constexpr float WRITE_BASS_GATE_LO = 0.65f;
    static constexpr float WRITE_BASS_GATE_HI = 0.85f;

    // The tape write stage: saturator, the two bypass/expand envelope followers,
    // the bass-ratio gate's low-pass, and the loop high-pass — one object owning
    // the whole write path's state, so no lifecycle path has to remember its
    // pieces by hand.
    struct TapeWriteStage {
        SatPath sat;
        float inEnv = 0.f, satEnvOut = 0.f, bassLp = 0.f, bassEnv = 0.f;
        float dcZ      = 0.f;
        float dcCoefLo = 0.f;   // ~12 Hz — full echo bass (+ DC block)
        float dcCoefHi = 0.f;   // ~80 Hz — self-osc bass tightening
        float bassLpCoef = 0.f; // ~300 Hz — the bass-ratio gate's detector LP

        void prepare(float sr) {
            auto coef = [&](float tau) { return 1.0f - std::exp(-1.0f / (tau * sr)); };
            dcCoefLo = coef(WRITE_HPF_TAU_LO);
            dcCoefHi = coef(WRITE_HPF_TAU_HI);
            // Was recomputed per sample from args.sampleRate. Same expression, same
            // sample rate, so hoisting it here is bit-exact — it just stops paying
            // for one std::exp on every sample of every channel.
            bassLpCoef = 1.f - std::exp(-2.f * float(M_PI) * WRITE_BASS_LP_HZ / sr);
        }

        // Filter memory only — see TapeEcho::resetFilterState().
        void resetFilterState() { dcZ = 0.f; }

        void reset() {
            sat.reset();
            inEnv = satEnvOut = bassLp = bassEnv = 0.f;
            resetFilterState();
        }

        // Saturate the combined record signal, then the loop high-pass (corner
        // interpolated by loop `hotness`, ~12 Hz for echoes rising to ~80 Hz as
        // self-oscillation establishes). Dropout gain and per-pass level loss
        // are applied at the buffer write instead, not here.
        float process(float recordIn, bool ecoMode, float T8, float T4,
                      float loopLevelDb, float hotness) {
            float writeSatOut = sat.process(recordIn, ecoMode, T8, T4);
            inEnv     += WRITE_ENV_COEF * (std::fabs(recordIn)    - inEnv);
            satEnvOut += WRITE_ENV_COEF * (std::fabs(writeSatOut) - satEnvOut);
            bassLp  += bassLpCoef * (recordIn - bassLp);
            bassEnv += WRITE_ENV_COEF * (std::fabs(bassLp) - bassEnv);
            float compGain = inEnv > 1e-4f ? clamp(satEnvOut / inEnv, 0.1f, 2.f) : 1.f;
            float expandDb = std::min(QUIET_EXPAND_MAX_DB,
                                       QUIET_EXPAND_DB_PER_DB * std::max(0.f, -loopLevelDb));
            float quietExpandOut = recordIn * compGain * std::pow(10.f, -expandDb / 20.f);
            float bassRatio = inEnv > 1e-4f ? clamp(bassEnv / inEnv, 0.f, 1.f) : 0.f;
            float bassGate = clamp((bassRatio - WRITE_BASS_GATE_LO)
                                    / (WRITE_BASS_GATE_HI - WRITE_BASS_GATE_LO), 0.f, 1.f);
            float writeOut = bassGate * quietExpandOut + (1.f - bassGate) * writeSatOut;
            float writeHpfCoef = dcCoefLo + hotness * (dcCoefHi - dcCoefLo);
            float writeAC   = writeOut - dcZ;
            dcZ       += writeHpfCoef * writeAC;
            return writeAC;
        }
    };
    TapeWriteStage tapeWrite;

    // Module-wide frozen flag (covers all stochastic subsystems). TEST-ONLY,
    // NOT SHIPPING — pins RNG/dropouts so the regression harness nulls against
    // real changes instead of masking them under modulation. Removing it is
    // safe but requires every scenario using it to drop the key and recapture.
    bool frozenDynamics = false;


    // Machine self-noise floor on/off. By-ear A/B found this noise doesn't
    // track Reverb Volume and only mildly tracks Echo Volume — general
    // output-stage self-noise, not spring-specific. Flat, always-on floor
    // that still tapers with the Output pad switch.
    bool machineNoiseFloor = true;

    // Motor Stop is always latched (the momentary/latching toggle was removed
    // from the context menu). `reverseMomentary` is a separate setting.
    bool motorStopLatched   = false;
    dsp::SchmittTrigger motorStopButtonTrig;

    // Reverse playback (VCV-only). Continuous signed tape velocity: +1 forward
    // unity, 0 stopped, -1 reverse unity, smoothing through zero on
    // engage/disengage. Walking-tap is active whenever velocity isn't at full
    // forward unity, crossfading to the normal delay-line read once it
    // settles. Two independently-wrapping phases, offset ~half a loop apart,
    // crossfaded with a raised-cosine window to mask the wrap click.
    bool reverseMomentary = false;
    bool reverseLatched   = false;
    dsp::SchmittTrigger reverseButtonTrig;
    dsp::SchmittTrigger reverseGateTrig;
    float tapeVelocityTau  = 0.400f;
    float tapeVelocityCoef = 0.f;
    float tapeVelocity     = 1.f;
    // Reverse loop length is slewed, not stepped — a step would move every
    // read position at once, same reason forward's tapFrac transition slews.
    //
    // The reverse tape transport: walking-tap crossfade weight, loop length,
    // and the two half-loop-offset read phases. Owns its own state so
    // resetDspState(), the sample-rate snap, and the coefficient recompute all
    // agree in one place. The per-head read itself stays in the delay line's job.
    struct ReverseTransport {
        float loopLenSlew = 0.f;
        float loopLenTau  = 0.060f;
        float loopLenCoef = 0.f;
        float fadeTau     = 0.080f;
        float fadeCoef    = 0.f;
        float fade        = 0.f;   // walking-tap crossfade weight, GUI-visible
        float phaseA      = 0.f;
        float phaseB      = 0.f;
        bool  prevActive  = false;

        void prepare(float sr) {
            auto coef = [&](float tau) { return 1.0f - std::exp(-1.0f / (tau * sr)); };
            fadeCoef    = coef(fadeTau);
            loopLenCoef = coef(loopLenTau);
        }

        // Sample-domain state, not time constants: meaningless against a new
        // buffer, so re-seeded rather than rescaled on a sample-rate change.
        void reset() {
            loopLenSlew = phaseA = phaseB = 0.f;   // < 1.f forces the snap branch
            prevActive  = false;                   // makes walkEngaging true, re-seeding
        }

        struct Result { bool useWalking; float fadeA, fadeB; };

        Result process(bool reverseActive, bool frozenDynamics,
                       float tapeVelocity, float tapeSpeed, float delaySamples,
                       const float tapFrac[3], const float tapGain[3],
                       float bufSizeFloat, float sr,
                       bool clockSnapOn, float divisionFrac, float beatSec) {
            // Walking-tap advance. growth = 1 − tapeVelocity (pitch-neutral):
            // +1 → growth 0 (stationary, forward unity), 0 → growth 1 (paused),
            // −1 → growth 2 (unity reverse). Crossfades to the delay-line once
            // velocity settles back to ≈+1.
            bool walkingTargetActive = reverseActive || tapeVelocity < 0.95f;
            float walkingFadeTarget  = walkingTargetActive ? 1.f : 0.f;
            if (frozenDynamics) {
                fade = walkingFadeTarget;
            } else {
                fade += fadeCoef * (walkingFadeTarget - fade);
            }
            bool useWalking = fade > 1e-4f;
            // Loop/chunk length the reverse delay-amount wraps within, tied to
            // tapeSpeed via delaySamples. Uses the *ratio* between an active
            // head's tap fraction and head1's (the anchor) as a multiplier on
            // delaySamples, rather than the raw fraction — mode 1 alone
            // reproduces the previous baseline; modes reading further-out
            // heads get a proportionally longer loop, never shorter.
            float tapGainSum = tapGain[0] + tapGain[1] + tapGain[2];
            float avgTapFrac = (tapGainSum > 0.001f)
                ? (tapGain[0]*tapFrac[0] + tapGain[1]*tapFrac[1] + tapGain[2]*tapFrac[2]) / tapGainSum
                : tapFrac[0];
            float loopLenMul = avgTapFrac / tapFrac[0];
            float loopLenRaw = std::max(256.f, delaySamples * loopLenMul);

            // While synced, quantize the reverse grain period to the nearest
            // musical multiple of the running division so it keeps the
            // free-running grain length while still landing on the grid.
            // Handoffs alternate every loopLen/(2·growth): loopLen = 4·grain
            // at full reverse. Sized off steady-state growth so the engage
            // glide can't modulate the loop length while it settles.
            //
            // BUFFER SAFETY, load-bearing: readDelayHermite WRAPS rather than
            // clamping, so an overrun silently reads newer material with no
            // click to give it away (has happened before, near-miss on the
            // buffer size) — must stay caught by arithmetic, not by ear.
            const float deepestExtra = (tapFrac[2] - tapFrac[0]) * delaySamples;
            const float loopLenCap   = std::max(256.f, bufSizeFloat * 0.94f - deepestExtra);

            float grainSamples = 0.f;
            bool  gridLocked   = false;
            if (clockSnapOn) {
                // The division lookup and effectiveBeat() belong to the clock
                // subsystem, so they are resolved at the call site and arrive here
                // as two plain floats.
                const float divSamples = divisionFrac * beatSec * sr;
                if (divSamples > 16.f) {
                    // The invariant is grain length relative to echo spacing,
                    // not absolute duration — quantizing that ratio (rather
                    // than a fixed grain duration, which blurred repeats into
                    // a wash) makes synced match free-running by construction.
                    const float kRaw = loopLenRaw / (4.f * divSamples);
                    const float kMax = loopLenCap / (4.f * divSamples);
                    grainSamples = nearestMusicalMultiple(kRaw, kMax) * divSamples;
                    loopLenRaw   = 4.f * grainSamples;
                    gridLocked   = true;
                }
            }
            // Also bounds the free-running case, where a Motor-Stop-clamped tapeSpeed
            // can inflate delaySamples ~10x.
            if (loopLenRaw > loopLenCap) {
                loopLenRaw = loopLenCap;
                // Nothing reads grainSamples after this — deliberate. Keeps it
                // consistent with the capped loopLen for any future phase-lock
                // attempt that needs a counter wrapping at grainSamples. Do not
                // remove; it documents an invariant, not live code.
                if (gridLocked) grainSamples = 0.25f * loopLenRaw;   // loopLen = 4 x grain
            }
            const bool walkEngaging = useWalking && !prevActive;
            if (walkEngaging || loopLenSlew < 1.f) loopLenSlew = loopLenRaw;
            else loopLenSlew += loopLenCoef * (loopLenRaw - loopLenSlew);
            float loopLen = std::max(256.f, loopLenSlew);

            if (walkEngaging) {
                phaseA = tapFrac[0] * delaySamples;
                if (phaseA >= loopLen) phaseA = std::fmod(phaseA, loopLen);
                phaseB = std::fmod(phaseA + 0.5f * loopLen, loopLen);
            }
            prevActive = useWalking;
            if (useWalking) {
                float growth = 1.f - tapeVelocity;
                // Motor Stop halts the reverse tape too (tapeSpeed is kept out
                // of this path otherwise, so the Rate knob can't pitch reverse
                // — this gate only ever engages below the slowest detent). The
                // spring is untouched, since it's mechanically independent of
                // the tape motor on real hardware.
                growth *= clamp(tapeSpeed / SPEED_SLOW, 0.f, 1.f);

                // Phase alignment (locking to the clock) was tried twice and
                // removed both times — numerically unsound. Only the PERIOD
                // quantization above is kept; PHASE stays unanchored, same as
                // free-running always was. A correct phase lock needs a
                // dedicated sample counter re-anchored on whole clock pulses;
                // do not bolt it on again without that.
                phaseA += growth;
                if (phaseA >= loopLen)      phaseA -= loopLen;
                else if (phaseA < 0.f)      phaseA += loopLen;
                phaseB += growth;
                if (phaseB >= loopLen)      phaseB -= loopLen;
                else if (phaseB < 0.f)      phaseB += loopLen;
            }
            // Boundary-only crossfade. A full-loop raised-cosine blend of A/B
            // was tried and rejected — it smeared each reversed tap into its
            // neighbor instead of presenting distinct repeats. Each phase is
            // now flat for most of its active half, ramping only at handoffs —
            // an "alternating buffers" splice via two wrapping phases. B
            // offset exactly half a loop from A keeps fadeA+fadeB=1 exactly
            // through the handoff.
            //
            // The handoff window is an ABSOLUTE time, not a fraction of
            // loopLen — tying it to loopLen means every change to grain
            // length silently retunes tap distinctness, which reintroduces
            // the same smearing a longer grain would otherwise avoid.
            constexpr float XFADE_SEC  = 0.038f; // absolute handoff window
            constexpr float XFADE_FRAC = 0.05f;  // upper bound, for very short loops
            auto trapFade = [](float phase, float loopLen, float xfadeLen) -> float {
                float halfLen = 0.5f * loopLen;
                if (phase < xfadeLen)
                    return 0.5f - 0.5f * std::cos(float(M_PI) * phase / xfadeLen);
                if (phase < halfLen)
                    return 1.f;
                if (phase < halfLen + xfadeLen)
                    return 0.5f + 0.5f * std::cos(float(M_PI) * (phase - halfLen) / xfadeLen);
                return 0.f;
            };
            float xfadeLen = std::min(XFADE_SEC * sr, XFADE_FRAC * loopLen);
            float fadeA = trapFade(phaseA, loopLen, xfadeLen);
            float fadeB = trapFade(phaseB, loopLen, xfadeLen);
            return { useWalking, fadeA, fadeB };
        }
    };
    ReverseTransport reverse;

    // "Reverb follows Reverse": crossfades the spring's output toward a
    // time-reversed-IR counterpart as Reverse engages. Gated off in Eco mode
    // (roughly doubles the spring's CPU while active).
    bool reverbFollowsReverse = false;
    // Target reverse-reverb swell length, published for the widget to
    // rebuild against — the GUI thread owns the rebuild since it runs FFTs
    // and can never be called from process().
    float reversedSwellTarget = 0.f;
    static constexpr float REV_SWELL_FREE_SEC = 1.5f;   // un-synced fallback
    // A swell of exactly one division is wrong: convolution is causal, so the
    // swell peaks a swell-length later, colliding with the next grid hit at
    // one division. Wanted length is a musical number of divisions instead.
    static constexpr float REV_SWELL_TARGET_SEC = 1.0f;
    // GANG — Intensity and the rate axis linked as one performance gesture.
    // The linkage itself is GUI-thread state inside TapeEchoIntensityKnob; no
    // param is ever written from process().
    bool gangEnabled   = false;
    bool gangMomentary = false;
    // While synced, a Gang gesture suspends sync for its duration so the
    // twist gets the full tape-speed range instead of the ±6% nudge. Safe
    // only because spring-back is forced while synced.
    bool gangSyncOverride = false;
    // Anchors for that widened mapping (state: sync.gangAnchorPos /
    // sync.gangAnchorNudge / sync.gangOverridePrev), so the handover stays
    // continuous even when a nudge was already dialled in.

    // Wow/flutter RNG state (moved to module level; reset to seed when frozen)
    static constexpr uint32_t FLUTTER_RNG_SEED = 12345;
    uint32_t flutterRngState = FLUTTER_RNG_SEED;

    // Hysteresis saturation (write path). The feedback path previously ran
    // through its own SatPath too — removed, see the fbSat comment in process().
    InputDriveStage inputDrive;
    WetDriveClip wetDriveClip;
    OutputClipStage dryClipStage, wetClipStage, finalClipStage;
    bool ecoMode = false;

    // Defeat the dry path for effects-send use (output = wet/reverb only).
    bool dryDefeat = false;
    // Tone EQ placement: true = inside the feedback loop (per-repeat coloration
    // compounds, RE-201-faithful); false = one-shot output EQ (legacy). Null at
    // centered tone (0 dB shelf is a bit-exact identity).
    bool toneInLoop = true;
    // Two insert-point modes for where FB_RETURN re-enters: false (default) =
    // in-loop/before-delay, compounds on every future repeat (real dub
    // technique); true = output-only/after-delay, isolated from the loop.
    bool fbInsertPostLoop = false;
    // fbReturnBlend: within whichever mode is selected above, default (off)
    // replaces the target signal with the processed return; on, sums the
    // return with the unprocessed target instead (straight sum, no gain
    // compensation) — for layering an external effect on top rather than
    // substituting for it.
    bool fbReturnBlend = false;
    // Input-drives-feedback (always on). Above noon, Input boosts the loop gain so
    // pushing Input up drives a given Intensity toward self-oscillation (full = wild,
    // normal = busy repeats); once the loop is hot the driveComp output compression
    // relaxes so the driven wildness reads loud instead of being level-compensated.
    static constexpr float INPUT_FB_BOOST_MAX = 1.8f;  // loop-gain mult at full Input

    // Drive tilt mode: scales Input drive knob's saturation range, volume
    // swing, and HF rolloff together. 0=Gentle, 1=Moderate (default), 2=Aggressive.
    // See TILT_* tables above the class for per-mode values.
    int driveTiltMode = 1;

    // VU meter + Peak LED state (UI-visible; updated each sample, read each
    // frame). Peak follower (fast attack, slow release) so the needle reaches
    // transients rather than averaging them down.
    struct VuBallistics {
        float level          = 0.f;   // peak-follower on |recordIn|
        float peakLed        = 0.f;   // 1.0 on peak hit, decays with ~150 ms τ
        float attackCoef     = 0.f;
        float releaseCoef    = 0.f;
        float peakDecayCoef  = 0.f;

        // VU peak follower: 2 ms attack, 250 ms release. Peak LED decay: 150 ms.
        void prepare(float sr) {
            auto coef = [&](float tau) { return 1.0f - std::exp(-1.0f / (tau * sr)); };
            attackCoef    = coef(0.002f);
            releaseCoef   = coef(0.250f);
            peakDecayCoef = coef(0.150f);
        }

        void reset() { level = peakLed = 0.f; }

        void process(float x) {
            float drivenAbs = std::fabs(x);
            float coef = (drivenAbs > level) ? attackCoef : releaseCoef;
            level += coef * (drivenAbs - level);
            if (drivenAbs > PEAK_LED_THRESHOLD) peakLed = 1.f;
            else                                peakLed -= peakDecayCoef * peakLed;
        }
    };
    VuBallistics vu;

    // Oversampled-rate time constants (recomputed on SR change)
    float T_os8 = 1.f / (48000.f * 8.f);
    float T_os4 = 1.f / (48000.f * 4.f);

    // Eco mode toggle tracking — detect changes to reset idle AA filters (avoids click)
    bool ecoModePrev = false;

    WowFlutter wow;

    // Tape Age — multi-axis "wear" parameter. Right-click preset sets the
    // baseline; TAPE_AGE_INPUT overrides it when connected (0-10V → 0-1).
    // Drives HF rolloff, hysteresis drive shift, wow/flutter depth, and
    // (above 0.5) sporadic dropouts.
    float tapeAge       = 0.0f;   // smoothed toward tapeAgeTarget
    float tapeAgeTarget = 0.0f;
    float tapeAgeCoef   = 0.0f;
    float tapeAgeTau    = 0.050f;
    float tapeAgePreset = 0.0f;

    // Clock-sync. Engaged by patching CLOCK_INPUT — deliberately no menu
    // toggle, so the cable is an unambiguous statement of intent. Unplugging
    // disengages. Measures beat period between rising edges, EMA-smoothed;
    // sync then targets the active head's tap time.
    //
    // ClockFollower owns only "what tempo is the clock running at" — the
    // multiplier/division are separate persisted settings, and sync
    // resolution policy lives elsewhere.
    struct ClockFollower {
        dsp::SchmittTrigger trig;
        int   samplesSinceEdge = 0;
        float beatPeriod       = 0.5f;  // seconds (initialized to 120 BPM)
        bool  hasPeriod        = false;
    // Finding #7: one measured interval that disagrees wildly with the established
    // period is ambiguous — it is either a transport gap or a real tempo change,
    // and a single interval cannot tell them apart. Remembering it lets the NEXT
    // interval decide.
        float outlierMeasured  = 0.f;
        // The rate `samplesSinceEdge` was counted at. `beatPeriod` is in SECONDS
        // and so survives a rate change untouched (D9); the COUNT does not.
        float countedAtSr      = 0.f;

        // Rescale the in-flight interval into the new rate's samples. Without
        // this, an interval measured across a rate switch divides an old-rate
        // count by the new rate and can slip inside the outlier-rejection
        // band undetected. Rescaled rather than reset, since the clock is
        // still running and `beatPeriod` is still valid.
        void onSampleRateChange(float sr) {
            if (countedAtSr > 0.f && sr > 0.f && samplesSinceEdge > 0)
                samplesSinceEdge = (int)((double)samplesSinceEdge * (double)sr / countedAtSr);
            countedAtSr = sr;
        }

        // NOT reset on a sample-rate change, and `samplesSinceEdge` is a sample
        // COUNT — so the first interval measured across an SR switch divides an
        // old-rate count by the new rate and comes out wrong. It is self-limiting:
        // the sanity filter and the outlier-rejection logic below reject or
        // ignore it, and tracking recovers within a beat or two. Left as-is
        // because changing it is a behaviour change, not a refactor — and no
        // regression scenario patches a clock across an SR switch, so a null
        // test cannot see this either way.
        void process(bool connected, float voltage, float sr) {
            // CLOCK sync: track beat period between rising edges; smooth with
            // EMA so live BPM sweeps are jitter-free. τ ≈ 4 beats.
            // The counter used to increment unconditionally and
            // reset only on a rising edge, which had two consequences:
            //   - with nothing patched it was never reset at all, so it overflowed
            //     (signed, UB) after ~12.4 h at 48 kHz, ~3.1 h at 192 kHz;
            //   - after unplug/replug the first interval smoothed into a stale
            //     beatPeriodSmoothed instead of re-seeding.
            // Clearing clockHasPeriod on disconnect makes a replug re-seed from its
            // first interval, which is what the existing `!clockHasPeriod` branch
            // below already does correctly. The cap keeps the counter bounded while a
            // clock is patched but idle, and makes any gap of 4 s or more read as
            // `measured >= 4.f`, which the sanity filter already rejects.
            if (!connected) {
                samplesSinceEdge = 0;
                hasPeriod = false;
                outlierMeasured = 0.f;
            } else if (samplesSinceEdge < (int)(4.f * sr)) {
                samplesSinceEdge++;
            }
            countedAtSr = sr;
            if (trig.process(clamp(voltage, -10.f, 10.f))) {
                float measured = (float)samplesSinceEdge / sr;
                samplesSinceEdge = 0;
                // Reject pathological intervals — keeps the smoother stable on
                // patches that wobble between clock sources.
                if (measured > 0.02f && measured < 4.f) {
                    if (!hasPeriod) {
                        beatPeriod = measured;
                        hasPeriod = true;
                        outlierMeasured = 0.f;
                    } else {
                        // A single interval can't distinguish a transport gap from a
                        // real tempo change, so an out-of-band one is ignored and
                        // remembered; if the NEXT interval agrees, that's a genuine
                        // tempo change and the period re-seeds. The band is deliberately
                        // wide so swung clocks (2:1 swing = ratios 1.33/0.67) keep
                        // smoothing normally.
                        const float ratio = measured / beatPeriod;
                        if (ratio > 0.6f && ratio < 1.7f) {
                            float alpha = 1.f - std::exp(-measured / (4.f * beatPeriod));
                            beatPeriod += alpha * (measured - beatPeriod);
                            outlierMeasured = 0.f;
                        } else if (outlierMeasured > 0.f
                                   && measured > outlierMeasured * 0.9f
                                   && measured < outlierMeasured * 1.1f) {
                            beatPeriod   = measured;   // confirmed tempo change
                            outlierMeasured = 0.f;
                        } else {
                            outlierMeasured = measured;   // ignore once, wait for confirmation
                        }
                    }
                }
            }
        }
    };
    ClockFollower clock;
    // Divisions descend by delay length: index 0 = longest, last = shortest,
    // as a multiplier of one beat period. Sync targets the active head's tap
    // time, not the loop length. Spans the machine's musically useful window
    // (h1 63.5-163.9ms, h2 127.0-327.7ms, h3 190.5-491.6ms); unreachable
    // entries are filtered per-BPM.
    static constexpr int CLOCK_N_DIVISIONS = CLOCK_N_DIV;
    const float CLOCK_DIVISIONS[CLOCK_N_DIVISIONS] = {
        1.5f,        // dotted 1/4
        1.f,         // 1/4
        0.75f,       // dotted 1/8
        2.f/3.f,     // 1/4 triplet
        0.5f,        // 1/8
        0.375f,      // dotted 1/16
        1.f/3.f,     // 1/8 triplet
        0.25f,       // 1/16
        1.f/6.f,     // 1/16 triplet
        0.125f,      // 1/32
    };
    // Sync resolution state — everything resolveClockSync() owns: display
    // fields, the effective division, and Gang anchors, grouped so they can't
    // drift apart. NOT here: clockMultIdx/clockDivisionIdx, which are
    // persisted user settings, not resolved state.
    struct SyncState {
        // Display-only knob snap — the Rate widget draws its indicator from
        // this instead of the param value, which is never written from the
        // audio thread (see the race note in resolveClockSync()).
        float snapKnobPos   = 0.5f;
        bool  snapDisplayOn = false;
        int   effectiveIdx  = 4;    // which division is actually sounding
        float snapModePos   = 1.f;  // display-only mode-switch position
        bool  snapModeOn    = false;// only single-head parked modes animate
        // Nudge, published for TapeEchoRateQuantity. `Pct` is the achieved
        // trim, not the requested one (the requested nudge can clamp near a
        // taper end stop). `On` gates the percent display.
        float nudgePct = 0.f;
        bool  nudgeOn  = false;
        // Gang anchors, captured in the AUDIO domain on the rising edge of
        // gangSyncOverride — the widget publishes only that bool.
        float gangAnchorPos    = 0.5f;
        float gangAnchorNudge  = 0.5f;
        bool  gangOverridePrev = false;   // rising-edge detect for that capture
    };
    SyncState sync;

    // The division is an absolute menu selection, not a position spread
    // across the Rate knob. `requested` is what the user picked and is never
    // overwritten; `effective` differs only while tempo drift has pushed the
    // request out of reach.
    int   clockMultIdx        = CLOCK_MULT_DEFAULT;   // persisted
    int   clockDivisionIdx    = 4;      // requested (4 = "1/8"), persisted
    // (resolved state: sync.effectiveIdx / sync.snapModePos / sync.snapModeOn)
    // (published nudge state: sync.nudgePct / sync.nudgeOn)
    // Slow drift LFO (Stage 8A Pass B) — "warped tape" wobble that's much slower
    // and deeper than wow/flutter. Integrated as varispeed (per CLAUDE.md convention).
    float driftPhase       = 0.0f;
    float driftRate        = 0.18f;          // Hz
    float driftPosition[3] = {0.f, 0.f, 0.f};
    static constexpr float DRIFT_LEAK_TAU = 5.0f;  // seconds

    // Output-stage artifacts: machine self-noise floor + tape hiss. Not part
    // of the dry/wet signal — summed in after both output clips, so neither
    // path's limiter ducks them.
    //
    // DELIBERATELY HAS NO reset(). `tapeHissPos`/`springNoisePos`/
    // `hissLfoPhase` are read positions into a looping noise WAV — resetting
    // them would make every reset replay identical noise. `hissActivityEnv`
    // self-corrects every sample and can't latch. Do not "fix" this.
    struct OutputArtifacts {
        // Tape hiss read position into spring.noiseFloor.
        int tapeHissPos = 0;
        // Spring mechanical/preamp noise read position into spring.noiseFloor.
        // Independent of the tape-hiss read position so the
        // two noise textures don't correlate.
        int springNoisePos = 0;
        // Subtle volume LFO on the hiss — emulates physical contact variations.
        // Rate is dictated by the Repeat Rate knob; depth scales with tape age.
        float hissLfoPhase = 0.0f;
        // Input-activity gate: hiss only audible while there's a live signal feed.
        // Fast attack (snaps up), slow release (~2 s) so the hiss tail fades after
        // the input goes quiet rather than droning on indefinitely.
        float hissActivityEnv         = 0.0f;
        float hissActivityReleaseCoef = 0.0f;

        // Machine self-noise floor. By-ear A/B found it doesn't track Reverb
        // Volume and only mildly tracks Echo Volume — general output-stage
        // self-noise, always present. Modeled as a flat floor, added
        // post-drive-divide so Input drive doesn't attenuate it. Not gated by
        // motorGain; still gated by the Output pad switch and the
        // `machineNoiseFloor` context-menu toggle.
        //
        // `res/spring_noise.wav` is a real hardware capture; the gain constant
        // isn't self-calibrating — re-measure if the source WAV is replaced.
        static constexpr float MACHINE_NOISE_OUTPUT_GAIN = 10.09f;

        float process(const std::vector<float>& noiseFloor, bool noiseFloorEnabled,
                      float tapeAge, float rate, float in, float sr,
                      float motorGain, float inputLevel, float inputEnv, float drive) {
            // Tape hiss — read the noise-floor sample at an independent position
            // from the spring reverb. Quadratic ramp, gated by motorGain.
            float hissSample = 0.f;
            if (tapeAge > 0.001f && !noiseFloor.empty()) {
                hissSample = noiseFloor[tapeHissPos];
                tapeHissPos = (tapeHissPos + 1) % (int)noiseFloor.size();
            }
            // Volume LFO: speed follows Repeat Rate (long-period "warped tape"
            // breathing — 3–12 s period); depth scales with age, kept subtle.
            float hissLfoHz = 0.08f + 0.25f * rate;
            hissLfoPhase += hissLfoHz / sr;
            if (hissLfoPhase >= 1.f) hissLfoPhase -= 1.f;
            float hissLfoMod = 1.f + 0.15f * tapeAge * std::sin(2.f * float(M_PI) * hissLfoPhase);
            // Activity gate — snap up when input present; slow release after silence.
            if (std::fabs(in) > 0.01f) hissActivityEnv = 1.f;
            hissActivityEnv -= hissActivityReleaseCoef * hissActivityEnv;
            // This output-side term is now a small playback-electronics
            // residue — most age character is injected into the loop at the
            // record head instead. Activity-gated so it mutes during the echo tail.
            float hissLevel  = 0.85f * tapeAge * tapeAge * hissLfoMod * hissActivityEnv;

            float artifact = 0.f;

            if (noiseFloorEnabled && !noiseFloor.empty()) {
                float noiseSample = noiseFloor[springNoisePos];
                springNoisePos = (springNoisePos + 1) % (int)noiseFloor.size();
                artifact += noiseSample * MACHINE_NOISE_OUTPUT_GAIN;
            }

            // Hiss: added post-compensation so its level isn't 36× swung by the
            // inverse-compensation curve. Base level is cut 25% from previous Pass C.
            // Drive-coupled treatment:
            //   - Constant noticeability at the bottom of the knob
            //   - Softer at the top, blended into a signal-envelope-tracking
            //     "compressed" character so it varies with content rather than
            //     droning — but never goes fully transparent.
            float knobNorm    = inputLevel * 0.5f;  // 0..1
            float hissDriveAtt = 1.f - 0.5f * knobNorm;                       // 1.0 → 0.5
            // Signal-envelope-tracking blend: only kicks in above midpoint.
            // inputEnv tracks post-drive signal level — divide back out so the
            // envelope responds to raw input level, not driven amplitude.
            float modBlend     = std::max(knobNorm - 0.5f, 0.f) * 2.f;        // 0 below mid → 1 at top
            float rawEnvApprox = inputEnv / std::max(drive, 0.05f);
            float sigEnvNorm   = std::min(1.f, rawEnvApprox * 4.f);
            float hissModulator = (1.f - modBlend) + modBlend * (0.3f + 0.7f * sigEnvNorm);
            float finalHissLevel = 0.75f * hissLevel * hissDriveAtt * hissModulator;
            artifact += hissSample * finalHissLevel * motorGain;
            return artifact;
        }
    };
    OutputArtifacts artifacts;

    // Drive-coupled output filter state.
    //   - lowDriveLPState: 1-pole HF rolloff that engages below the midpoint
    //     of the Input drive knob, for a "pillowy" character at low drive.
    //   - lowDriveLPStateWet: same filter on the wet path (split output stage).
    //     Separate state so dry+wet sum is bit-identical to the old single
    //     filter (the rolloff is linear, so superposition holds).
    float lowDriveLPState    = 0.0f;
    float lowDriveLPStateWet = 0.0f;

    // Split output soft-clip ceilings — dry and wet clip independently
    // (unity-slope `C*tanh(x/C)`) so a loud dry can't duck the wet through a
    // shared limiter. DRY: tight headroom protection. WET: subtler, so
    // self-osc breathes. FINAL: gentle safety on the sum. By-ear tunable.
    static constexpr float OUT_CLIP_DRY   = 5.0f;
    static constexpr float OUT_CLIP_WET   = 9.0f;   // self-osc ceiling — scary but tamed (7→12→9)
    static constexpr float OUT_CLIP_FINAL = 11.0f;  // ~±9V self-osc peak; focused howl reads scary at lower level

    // Reverb Volume taper exponent (by-ear): >1 stretches the low half of the knob
    // so reverb ramps in gradually instead of dominating past ~9 o'clock.
    static constexpr float REVERB_TAPER_EXP = 1.8f;

    // Tape-stutter generator (Stage 8A Pass C) — chewed-tape artifacts at
    // Thrashed+Dumpster. Tape briefly loses contact and re-engages at a
    // different spot: random tap-read offset + amplitude dip. Does NOT touch
    // tapeVelocity so the walking-tap/reverse path stays inactive.
    struct TapeStutter {
        float envelope     = 0.0f;
        float decayCoef    = 0.0f;
        float skipSamples  = 0.0f;   // signed tap-read offset during event
        float dipDepth     = 0.7f;   // per-event amp-dip depth (0..1)
        uint32_t rng       = 5432;
        float threshold    = 0.7f;
    };
    TapeStutter stutter;

    // Dropout generator (active above tapeAge >= 0.5).
    struct DropoutGen {
        float envelope        = 0.0f;
        float decayCoef       = 0.0f;
        // `envelope = 1.f` outright would click. A real oxide dropout has a
        // fast but finite onset — `attacking` runs that ramp before decay
        // takes over.
        bool  attacking       = false;
        float attackCoef      = 0.0f;
        uint32_t rng          = 99;
        float baseRatePerSec  = 2.5f;
        float threshold       = 0.5f;
        // Depth is drawn per event (a fixed depth reads as sample playback,
        // not tape). Floor stays a ghost (-6 dB); ceiling opens with tape age.
        float depth           = DEPTH_MIN;
        static constexpr float DEPTH_MIN      = 0.50f;  // -6 dB — always a ghost, never a hole
        static constexpr float DEPTH_CEIL_LO  = 0.65f;  // -9 dB  — ceiling just above threshold
        static constexpr float DEPTH_CEIL_HI  = 0.90f;  // -20 dB — ceiling at Dumpster

        // Defects live at fixed positions on the loop (expressed as a
        // multiple of the delay, so it tracks Rate/CV/Gang/Motor Stop for
        // free), recurring once per revolution rather than firing at random.
        // Severity is fixed per defect and the age-scaled ceiling opens over
        // it, so aging makes the same bad spots worse instead of relocating
        // them. Defect count is drawn from the seed too, so a saved patch
        // reloads the identical tape.
        static constexpr int   MAX_DEFECTS       = 16;
        static constexpr int   MIN_DEFECTS       = 6;
        static constexpr float LOOP_REVS_OF_BASE = 20.5f;
        float    loopPhase = 0.f;               // 0..1 around the tape loop
        int      activeDefects = MIN_DEFECTS;
        float    defectPos[MAX_DEFECTS] = {};
        float    defectSev[MAX_DEFECTS] = {};
        uint32_t defectSeed = 0x5EEDu;          // saved in JSON: a patch reloads the same tape

        void buildDefects() {
            uint32_t v = defectSeed ? defectSeed : 1u;
            auto nx = [&v]() { v ^= v << 13; v ^= v >> 17; v ^= v << 5; return v; };
            activeDefects = MIN_DEFECTS + int(nx() % uint32_t(MAX_DEFECTS - MIN_DEFECTS + 1));
            for (int i = 0; i < MAX_DEFECTS; ++i) {
                defectPos[i] = float(nx() % 100000u) * 1e-5f;
                defectSev[i] = float(nx() % 100000u) * 1e-5f;
            }
        }
    };
    DropoutGen dropouts;

    TapeEcho() {
        // A fresh module gets a fresh tape. Saved patches restore the seed from JSON
        // and rebuild the identical defect table, so a patch always sounds the same.
        dropouts.defectSeed = rack::random::u32();
        dropouts.buildDefects();
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        configParam(REPEAT_RATE_PARAM,   0.f, 1.f, 0.5f, "Repeat Rate");
        configParam<TapeEchoRateQuantity>(RATE_NUDGE_PARAM, 0.f, 1.f, 0.5f, "Rate nudge");
        configParam(INTENSITY_PARAM,     0.f, 1.f, 0.4f, "Intensity");
        configParam(ECHO_VOLUME_PARAM,   0.f, 1.f, 0.7f, "Echo Volume");
        configParam(REVERB_VOLUME_PARAM, 0.f, 1.f, 0.0f, "Reverb Volume");
        configParam(BASS_PARAM,   -1.f, 1.f, 0.f, "Bass");
        configParam(TREBLE_PARAM, -1.f, 1.f, 0.f, "Treble");
        // Mode rotary: 12 positions, 0 = Reverb only, 1..11 = echo/reverb
        // variants matching the panel labels. Default 4 = Tap 2+3, matching
        // MODE_GAINS in decodeMode(), the source of truth these labels must agree with.
        configSwitch<TapeEchoModeQuantity>(MODE_PARAM, 0.f, 11.f, 4.f, "Mode", {
            "0 — Reverb only", "1 — Tap 1",   "2 — Tap 2",   "3 — Tap 3",
            "4 — Tap 2+3",     "5 — Tap 1 + Reverb",         "6 — Tap 2 + Reverb",
            "7 — Tap 3 + Reverb",             "8 — Tap 1+2 + Reverb",
            "9 — Tap 2+3 + Reverb",           "10 — Tap 1+3 + Reverb",
            "11 — Swell (all + Reverb)",
        });
        configButton(MOTOR_STOP_PARAM, "Motor stop");
        configButton(REVERSE_PARAM,    "Reverse");
        // Input drive: exponential mapping 0.125×..8× across knob 0..2,
        // default 1.0 at midpoint = unity.
        configParam(INPUT_LEVEL_PARAM, 0.f, 2.f, 1.f, "Input drive");

        // H/M/L output pad. 0=L, 1=M, 2=H. Default H (0 dB), matching both the
        // 2.f default and PAD_OUT_GAIN[2] = 1.f. dB values transcribed from that table.
        configSwitch(OUTPUT_PAD_PARAM, 0.f, 2.f, 2.f, "Output pad", {
            "L (-12 dB)", "M (-6 dB)", "H (0 dB)",
        });

        configInput(IN_INPUT,         "Audio");
        configInput(REVERSE_INPUT,    "Reverse gate");
        configInput(FB_RETURN_INPUT,   "Feedback return");
        configInput(RATE_CV_INPUT,           "Repeat Rate CV");
        configInput(INTENSITY_CV_INPUT,      "Intensity CV");
        configInput(ECHO_VOLUME_CV_INPUT,    "Echo Volume CV");
        configInput(REVERB_VOLUME_CV_INPUT,  "Reverb Volume CV");
        configInput(MODE_INPUT,              "Mode CV (1V/step)");
        configInput(CLOCK_INPUT,             "Clock");
        configInput(TAPE_AGE_INPUT,          "Tape Age CV");
        configOutput(OUT_OUTPUT,    "Mix");
        configOutput(OUT_H1_OUTPUT, "Head 1");
        configOutput(OUT_H2_OUTPUT, "Head 2");
        configOutput(OUT_H3_OUTPUT, "Head 3");
        configOutput(FB_SEND_OUTPUT, "Feedback send");

        // Bypass routes the dry input to the main mix (Rack's default
        // silences everything otherwise). Head outputs and feedback send are
        // deliberately not routed — not dry paths.
        configBypass(IN_INPUT, OUT_OUTPUT);

        // Bake hysteresis parameters (drive=1.0, sat=0.7, width=0.5) — moves
        // the saturation knee from ~±5V to ~±3.8V so typical signals see
        // visible compression on the wet path.
        tapeWrite.sat.cook(1.f, 0.7f, 0.5f);

        // Compile-time fallback head EQ, overwritten below by loadMeasuredParams()
        // when the measured params file is present
        headEQ[0].slow = {80.f,  3.f, 1.f, 4500.f};
        headEQ[0].med  = {110.f, 3.f, 1.f, 6000.f};
        headEQ[0].fast = {150.f, 3.f, 1.f, 8000.f};
        headEQ[1].slow = {80.f,  3.f, 1.f, 4400.f};
        headEQ[1].med  = {110.f, 3.f, 1.f, 5800.f};
        headEQ[1].fast = {150.f, 3.f, 1.f, 7800.f};
        headEQ[2].slow = {80.f,  3.f, 1.f, 4300.f};
        headEQ[2].med  = {110.f, 3.f, 1.f, 5600.f};
        headEQ[2].fast = {150.f, 3.f, 1.f, 7600.f};

        loadMeasuredParams();

        // Load spring noise floor WAV (one-shot, RT-safe at construction).
        // Fall back to silence if missing — module still works, just no hum.
        std::string noisePath = asset::plugin(pluginInstance, "res/spring_noise.wav");
        if (!loadWavMono(noisePath, spring.noiseFloor)) {
            WARN("TapeEcho: failed to load %s — spring noise floor will be silent",
                 noisePath.c_str());
            spring.noiseFloor.assign(96000, 0.f);
        } else {
            for (float& v : spring.noiseFloor) v *= SPRING_NOISE_FILE_GAIN;
        }

        // Load the six 48 kHz spring IRs for the convolution reverb.
        // onSampleRateChange() resamples to the engine SR. If any fails to
        // load, the spring stays silent but the module still runs. Read from
        // disk once per process (the loader only runs on the first call).
        try {
        spring.irSource = springIrSource(
            [](std::vector<float>* dst) {
                bool ok = true;
                for (int i = 0; i < SpringReverb::NUM_LEVELS; i++) {
                    std::string irPath = asset::plugin(pluginInstance,
                        string::f("res/spring_ir_L%d.wav", i + 1));
                    uint32_t irSR = 0;
                    if (!loadWavMono(irPath, dst[i], &irSR)) {
                        WARN("TapeEcho: failed to load %s — spring reverb will be silent",
                             irPath.c_str());
                        dst[i].clear();
                        ok = false;
                    } else if (irSR != (uint32_t)SpringReverb::SOURCE_SR) {
                        // Reject, don't just warn: the resampling math keys off
                        // the compile-time SOURCE_SR, so a mismatched file rings
                        // out at the wrong length — a silent wrong-audio failure.
                        WARN("TapeEcho: %s is %u Hz, expected %d Hz — rejected; spring reverb "
                             "will be silent", irPath.c_str(), irSR, SpringReverb::SOURCE_SR);
                        dst[i].clear();
                        ok = false;
                    }
                }
                return ok;
            });
        }
        catch (const std::exception& e) {
            WARN("TapeEcho: spring IR load failed (%s) — spring reverb will be silent", e.what());
            spring.irSource.reset();
        }

        onSampleRateChange();
    }

    // Clears every piece of recursive state the feedback loop can latch,
    // without touching parameters or persisted settings. Needed because a
    // NaN reaching `recordIn` round-trips through the loop permanently —
    // `std::max` doesn't discard NaN, only `std::fmax`/Rack's clamp do.
    //
    // Filter memory only — no delay line, no envelopes, no transport. Split
    // out because onSampleRateChange() needs exactly this: it recomputes
    // biquad coefficients from the new rate but must not run them against
    // stale z1/z2 memory from the old filter.
    void resetFilterState() {
        for (int i = 0; i < 3; i++) { headEQ[i].resetState(); headEQFwd[i].resetState(); }
        wetBass.resetState();    wetTreble.resetState();
        fwdWetBass.resetState(); fwdWetTreble.resetState();
        midColor.resetState();
        lowColor.resetState();
        firstPassShelf.resetState();
        tapeWrite.resetFilterState();
        loopLoss.resetFilterState();
    }

    void resetDspState() {
        std::fill(delayBuf.begin(), delayBuf.end(), 0.f);
        writeIdx = 0;

        tapeWrite.reset();
        inputDrive.reset();
        wetDriveClip.reset();
        dryClipStage.reset();
        wetClipStage.reset();
        finalClipStage.reset();
        spring.resetState();          // written long ago; had zero call sites until now

        resetFilterState();

        fbRmsZ = loopEnv = inEnv = 0.f;
        vu.reset();

        reverse.reset();
    }

    // Rack calls this for right-click -> Initialize. Rack restores PARAMS
    // itself; everything persisted outside params is ours to restore here.
    //
    // KEEP THIS LIST IN SYNC WITH dataToJson()/dataFromJson().
    //
    // Separated from the DSP half so dataFromJson() can restore defaults
    // without cutting the delay tail — Rack calls it on preset-open and
    // paste, not only patch load.
    void resetSettings() {
        frozenDynamics       = false;
        machineNoiseFloor    = true;
        ecoMode              = false;
        motorStopLatched     = false;   // POWER on — the motor runs
        reverseMomentary     = false;
        reverseLatched       = false;
        reverbFollowsReverse = false;
        gangEnabled          = false;
        gangMomentary        = false;
        tapeAgePreset        = 0.0f;
        driveTiltMode        = 1;
        clockDivisionIdx     = 4;                    // "1/8"
        clockMultIdx         = CLOCK_MULT_DEFAULT;   // x1
        dryDefeat            = false;
        toneInLoop           = true;
        fbReturnBlend        = false;
        fbInsertPostLoop     = false;
        sync.effectiveIdx    = clockDivisionIdx;   // never persisted; derived
    }

    void onReset() override {
        resetSettings();
        tapeWrite.sat.state.reset();
        resetDspState();
    }

    void loadMeasuredParams() {
        std::string path = asset::plugin(pluginInstance, "res/tape_echo_params.json");
        FILE* fh = fopen(path.c_str(), "r");
        if (!fh) return;
        json_error_t err;
        json_t* root = json_loadf(fh, 0, &err);
        fclose(fh);
        if (!root) return;

        auto getf = [](json_t* obj, const char* key, float def) -> float {
            json_t* j = json_object_get(obj, key);
            // json_is_number, not json_is_real: jansson types 100 and 100.0
            // differently, so requiring a decimal point would silently discard
            // a round measurement value. Finiteness only, not a range — these
            // are measured constants, and inventing bounds risks discarding a
            // legitimate one.
            if (!j || !json_is_number(j)) return def;
            double v = json_number_value(j);
            return std::isfinite(v) ? (float)v : def;
        };

        json_t* s1 = json_object_get(root, "session1");
        if (s1 && json_is_object(s1)) {
            wow.capstanRate        = getf(s1, "capstanRate",        wow.capstanRate);
            wow.capstanDepth       = getf(s1, "capstanDepth",       wow.capstanDepth);
            wow.loopRate           = getf(s1, "loopRate",           wow.loopRate);
            wow.loopDepth          = getf(s1, "loopDepth",          wow.loopDepth);
            wow.noiseLPCutoff      = getf(s1, "noiseLPCutoff",      wow.noiseLPCutoff);
            wow.stochasticDepth    = getf(s1, "stochasticDepth",    wow.stochasticDepth);
            wow.perHeadCorrelation = getf(s1, "perHeadCorrelation", wow.perHeadCorrelation);
        }
        json_t* s3 = json_object_get(root, "session3");
        if (s3 && json_is_object(s3)) {
            const char* headKeys[]  = {"head1", "head2", "head3"};
            const char* speedKeys[] = {"slow",  "med",   "fast"};
            HeadEQ::DetentParams* detents[3][3] = {
                {&headEQ[0].slow, &headEQ[0].med, &headEQ[0].fast},
                {&headEQ[1].slow, &headEQ[1].med, &headEQ[1].fast},
                {&headEQ[2].slow, &headEQ[2].med, &headEQ[2].fast},
            };
            for (int i = 0; i < 3; i++) {
                json_t* hJ = json_object_get(s3, headKeys[i]);
                if (!hJ || !json_is_object(hJ)) continue;
                for (int s = 0; s < 3; s++) {
                    json_t* spJ = json_object_get(hJ, speedKeys[s]);
                    if (!spJ || !json_is_object(spJ)) continue;
                    detents[i][s]->bumpFreq = getf(spJ, "bumpFreq", detents[i][s]->bumpFreq);
                    detents[i][s]->bumpGain = getf(spJ, "bumpGain", detents[i][s]->bumpGain);
                    detents[i][s]->bumpQ    = getf(spJ, "bumpQ",    detents[i][s]->bumpQ);
                    detents[i][s]->hfCutoff = getf(spJ, "hfCutoff", detents[i][s]->hfCutoff);
                }
            }
        }
        // Rate-knob speed-change transient — drives CriticallyDamped's
        // cascaded one-pole stages.
        json_t* s8 = json_object_get(root, "session8");
        if (s8 && json_is_object(s8)) {
            tapeSpeedSmoother.tauUp   = getf(s8, "tauUp_s",   tapeSpeedSmoother.tauUp);
            tapeSpeedSmoother.tauDown = getf(s8, "tauDown_s", tapeSpeedSmoother.tauDown);
        }
        // Motor tau, measured directly for the Motor Stop event; feeds
        // motorStopSmootherMeasured (see SinglePole's struct comment).
        json_t* s5 = json_object_get(root, "session5");
        if (s5 && json_is_object(s5)) {
            motorStopSmootherMeasured.tauUp   = getf(s5, "tauUp_s",   motorStopSmootherMeasured.tauUp);
            motorStopSmootherMeasured.tauDown = getf(s5, "tauDown_s", motorStopSmootherMeasured.tauDown);
        }
        // Stage 5b convolution spring. Optional overrides only — the IRs
        // themselves carry all the voicing (equal-energy normalized in
        // condition_ir_bank.py). The retired FDN's session4.dispersion / .damping
        // keys are now dead; the parser simply ignores them (D6).
        json_t* sc = json_object_get(root, "spring_conv");
        if (sc && json_is_object(sc)) {
            // Finding #15(b): type-checked but not range-checked, and this becomes
            // the FFT length and every convolution buffer's dimension.
            //   0     -> K = (irLength - 1) / 0, and outputFIFO.assign(0) leaves the
            //            FIFO EMPTY while ready stays true, so process() indexes an
            //            empty vector every sample;
            //   -1024 -> (size_t)6 * K * 2 * N turns negative into ~1.8e19 and throws
            //            length_error out of the constructor;
            //   1000  -> reaches new RealFFT(1000); pffft's only guard is an assert,
            //            compiled out under NDEBUG.
            // Dormant today (tape_echo_params.json has no spring_conv key) but the
            // file is regenerated by the measurement tooling and taken verbatim.
            json_t* pj = json_object_get(sc, "partition_size");
            if (pj && json_is_integer(pj)) {
                const int ps = (int)json_integer_value(pj);
                if (ps >= 64 && ps <= 8192 && (ps & (ps - 1)) == 0)
                    spring.partitionSize = ps;
                else
                    WARN("TapeEcho: spring_conv.partition_size %d rejected — must be a power "
                         "of two in [64, 8192]; keeping %d", ps, spring.partitionSize);
            }
            spring.driveAmount = getf(sc, "drive_amount", spring.driveAmount);
        }
        json_decref(root);
    }

    void onSampleRateChange() override {
        float sr = APP->engine->getSampleRate();
        // 8 s buffer: reverse grains need to be long enough to hear at fast
        // divisions (loopLen is 4x a grain); a shorter buffer would cap grain
        // length and flatten the per-mode cadence variation. ~3 MB at 96 kHz.
        int newSize = (int)(8.0f * sr) + 16;
        if (newSize != (int)delayBuf.size()) {
            delayBuf.assign(newSize, 0.0f);
            writeIdx = 0;
        }
        bufSizeFloat = (float)newSize;

        // The reverse transport is sample-domain (buffer positions, not time
        // constants), so it's reset rather than rescaled — its magnitudes
        // belong to the old sample rate and are meaningless against the new
        // buffer, which delayBuf.assign() has just zeroed anyway.
        reverse.reset();

        // The clock's in-flight interval IS rescaled rather than reset — a
        // different case, see ClockFollower::onSampleRateChange().
        clock.onSampleRateChange(sr);

        auto coef = [&](float tau) {
            return 1.0f - std::exp(-1.0f / (tau * sr));
        };
        tapeSpeedSmoother.onSampleRateChange(sr);
        motorStopSmootherMeasured.onSampleRateChange(sr);
        tapeVelocityCoef = coef(tapeVelocityTau);
        reverse.prepare(sr);
        tapGainCoef  = coef(tapGainTau);
        tapFracCoef  = coef(tapFracTau);
        reverbActiveGateCoef = coef(reverbActiveGateTau);
        loopEnvAtkCoef = coef(LOOP_ENV_ATK_TAU);
        loopEnvRelCoef = coef(LOOP_ENV_REL_TAU);
        fbRmsCoef      = coef(FB_RMS_TAU);
        tapeWrite.prepare(sr);
        // loopLpCoef / loopHpCoef are speed-coupled — recomputed per-sample in process().
        scarySlewUpCoef   = coef(SCARY_SLEW_UP_TAU);
        scarySlewDownCoef = coef(SCARY_SLEW_DOWN_TAU);
        echoMakeupDownCoef = coef(ECHO_MAKEUP_DOWN_TAU);
        echoMakeupUpCoef   = coef(ECHO_MAKEUP_UP_TAU);
        wow.onSampleRateChange(sr);

        // The head-EQ coefficient cache keys on tapeSpeed, not sr, so a rate
        // change under a stationary knob needs an explicit invalidation.
        invalidateHeadEqCache();

        // Midrange color boost coefficients depend on tapeSpeed and are
        // recomputed per-sample in process() instead, same reasoning as
        // loopLpCoef/loopHpCoef being recomputed there rather than here.
        {
            float A = std::pow(10.f, LOW_COLOR_BOOST_GAIN_DB / 40.f);
            float w0 = 2.f * float(M_PI) * LOW_COLOR_BOOST_FC_HZ / sr;
            float alpha = std::sin(w0) / (2.f * LOW_COLOR_BOOST_Q);
            float cosw0 = std::cos(w0);
            float a0 = 1.f + alpha / A;
            lowColor.b0 = (1.f + alpha * A) / a0;
            lowColor.b1 = (-2.f * cosw0) / a0;
            lowColor.b2 = (1.f - alpha * A) / a0;
            lowColor.a1 = (-2.f * cosw0) / a0;
            lowColor.a2 = (1.f - alpha / A) / a0;
        }

        // First-pass HF shelf — only sin/cos(w0) can be cached here, since the
        // shelf gain is speed-coupled and the remaining RBJ terms depend on it.
        {
            float w0 = 2.f * float(M_PI) * FIRST_PASS_SHELF_FC_HZ / sr;
            firstPassShelfCosW0 = std::cos(w0);
            firstPassShelfSinW0 = std::sin(w0);
        }

        // Oversampled-rate timestep (D9: τ in seconds, recomputed here)
        T_os8 = 1.f / (sr * 8.f);
        T_os4 = 1.f / (sr * 4.f);

        // Reset AA filter history on SR change (hysteresis state stays — D9)
        tapeWrite.sat.resetFilters();
        inputDrive.reset();
        wetDriveClip.reset();
        dryClipStage.reset();
        wetClipStage.reset();
        finalClipStage.reset();

        for (int i = 0; i < 3; i++) {
            headEQ[i].onSampleRateChange(sr);
            // Keep the forward-only twin's DetentParams in lockstep with headEQ[i].
            headEQFwd[i].slow = headEQ[i].slow;
            headEQFwd[i].med  = headEQ[i].med;
            headEQFwd[i].fast = headEQ[i].fast;
            headEQFwd[i].onSampleRateChange(sr);
        }
        wetBass.setShelf(sr, 100.f, 0.f, false);
        wetTreble.setShelf(sr, 5000.f, 0.f, true);
        fwdWetBass.setShelf(sr, 100.f, 0.f, false);
        fwdWetTreble.setShelf(sr, 5000.f, 0.f, true);
        // The shelves above were just flattened to 0 dB, so the
        // change-detection cache guarding them must be flattened too — left
        // stale, Bass/Treble would go silently dead after a rate change.
        lastBass = lastTreble = 0.f;
        // The coefficients above were just rebuilt for the new rate, so the
        // z-state they act on is stale by definition.
        resetFilterState();

        spring.onSampleRateChange(sr);
        // convReversed is prepared lazily (see SpringReverb's member comment),
        // so only re-prepare here if the feature is actually enabled — a
        // patch that never uses it never pays for it.
        if (reverbFollowsReverse && !ecoMode)
            spring.prepareReversedNow((int)(REV_SWELL_FREE_SEC * sr));
        inEnvCoef = coef(IN_ENV_TAU);
        inEnv = 0.f;

        tapeAgeCoef       = coef(tapeAgeTau);
        dropouts.decayCoef = coef(0.250f);
        // Dropout onset. 1.2 ms — fast enough to still read as a defect
        // rather than a fade, slow enough that the step is no longer a click.
        dropouts.attackCoef = coef(0.0012f);
        // stutter.decayCoef is set per-event (random duration); seed with a safe default.
        stutter.decayCoef  = coef(0.080f);
        // Hiss activity-gate release τ (snap on; slow release after input stops).
        artifacts.hissActivityReleaseCoef = coef(2.0f);

        vu.prepare(sr);
    }

    // Safe by construction, not by caller proof — this function WRAPS rather
    // than clamps, and other code (loopLenCap) relies on that guarantee.
    // idx0's index reduction, the additive correction, and the float-rounding
    // edge case at the wrap boundary must all actually enforce it, or an
    // out-of-bounds read gets fed back into the delay line through the
    // feedback loop. The fast path is unchanged, so ordinary operation is
    // bit-identical; fmod only runs if a tap was more than one buffer out.
    float readDelayHermite(float delaySamples) {
        const int bufSizeInt = (int)bufSizeFloat;

        float readPos = writeIdx - delaySamples;
        if (readPos < 0.f)            readPos += bufSizeFloat;
        else if (readPos >= bufSizeFloat) readPos -= bufSizeFloat;

        if (readPos < 0.f || readPos >= bufSizeFloat) {
            readPos = std::fmod(readPos, bufSizeFloat);
            if (readPos < 0.f) readPos += bufSizeFloat;
        }

        int idx0 = (int)readPos;
        if (idx0 >= bufSizeInt) idx0 -= bufSizeInt;   // closes the rounding case
        float frac = readPos - idx0;

        int idxm1 = (idx0 - 1 + bufSizeInt) % bufSizeInt;
        int idx1  = (idx0 + 1) % bufSizeInt;
        int idx2  = (idx0 + 2) % bufSizeInt;

        float ym1 = delayBuf[idxm1];
        float y0  = delayBuf[idx0];
        float y1  = delayBuf[idx1];
        float y2  = delayBuf[idx2];

        float m0 = (y1 - ym1) * 0.5f;
        float m1 = (y2 - y0)  * 0.5f;

        float frac2 = frac * frac;
        float frac3 = frac2 * frac;
        float h00 =  2.f * frac3 - 3.f * frac2 + 1.f;
        float h10 =        frac3 - 2.f * frac2 + frac;
        float h01 = -2.f * frac3 + 3.f * frac2;
        float h11 =        frac3 -       frac2;

        return h00 * y0 + h10 * m0 + h01 * y1 + h11 * m1;
    }

    // Clock-sync resolution — decides, given a measured tempo, which head
    // sync should use, what the tape speed becomes, and what the panel
    // should show. Writes tapeSpeedTarget and `sync`; returns the mode audio
    // should run. A member function (not a nested stage struct) since it
    // calls other non-static members.
    int resolveClockSync(int mode, float rate, float nudgeCv,
                         bool clockSnapActive, bool motorStopActive) {
        // Display-only publishes are staged in locals and written ONCE, after the
        // branch. Clearing the flags up front and setting them again ~80 lines
        // later leaves a window in which the GUI thread — drawing at ~60 Hz
        // against a ~20 us audio block — can sample sync.snapDisplayOn as false
        // and draw the parked param for a single frame. That was the Rate and
        // Mode knobs visibly "twitching" back to their parked positions several
        // times a second. Positions are written before flags, so a flag does not
        // go true ahead of the value it refers to.
        bool  snapOn      = false;
        bool  snapModeOn  = false;
        float snapKnobPos = sync.snapKnobPos;
        float snapModePos = sync.snapModePos;
        bool  snapNudgeOn  = false;
        float snapNudgePct = 0.f;
        // Which mode the AUDIO runs. Sync overriding the head has to reach the
        // tap gains, not just the tape speed and the pointer — otherwise the
        // head it selected is silent and only the parked tap sounds, making two
        // different divisions that share a tape speed audibly identical.
        int   audibleMode = mode;
        // Capture the Gang anchors here, not in the widget. The widget
        // publishes only the bool; anchoring in the audio domain is what lets the
        // anchor include RATE_CV exactly as the live value does, which a
        // GUI-thread capture cannot see. tapeSpeedTarget still holds the previous
        // sample at this point — i.e. the speed running when the knob was
        // grabbed, which is precisely what the mapping must pass through.
        if (gangSyncOverride && !sync.gangOverridePrev) {
            sync.gangAnchorNudge = nudgeCv;
            sync.gangAnchorPos   = taperKnobPos(tapeSpeedTarget);
        }
        sync.gangOverridePrev = gangSyncOverride;
        if (clockSnapActive && mode != 0) {
            // Sync overrides the HEAD, not the mode — the division is an
            // absolute selection from the context menu, and sync picks the
            // head that can reach it with tape speed nearest the centre
            // detent (syncHeadFor). reverbOn passes through from the parked
            // mode untouched.
            float refGains[3] = {0.f, 0.f, 0.f};
            bool  refReverb   = false;
            decodeMode(mode, refGains, refReverb);
            int nActive = 0;
            for (int i = 0; i < 3; i++) if (refGains[i] > 0.5f) nActive++;
            const bool singleHead = (nActive == 1);

            const int req  = clamp(clockDivisionIdx, 0, CLOCK_N_DIVISIONS - 1);
            int       eff  = -1;
            int       head = syncHeadFor(req, mode);
            if (head >= 0) {
                eff = req;
            } else {
                // Tempo drift pushed the request out of reach. Run the nearest
                // reachable division, preferring the same family. clockDivisionIdx
                // is never overwritten, so the request returns once tempo does.
                float bestScore = 0.f;
                for (int pass = 0; pass < 2 && eff < 0; pass++) {
                    for (int i = 0; i < CLOCK_N_DIVISIONS; i++) {
                        if (pass == 0 && CLOCK_DIV_CLASS[i] != CLOCK_DIV_CLASS[req])
                            continue;
                        int h = syncHeadFor(i, mode);
                        if (h < 0) continue;
                        float score = std::fabs(std::log(CLOCK_DIVISIONS[i]
                                                         / CLOCK_DIVISIONS[req]));
                        if (eff < 0 || score < bestScore) {
                            eff = i; head = h; bestScore = score;
                        }
                    }
                }
            }
            if (eff < 0) {
                // Nothing reachable at any division — disengage rather than
                // hold an off-grid delay with no indication. Re-evaluated
                // every sample, so sync returns the moment anything reaches.
                tapeSpeedTarget = taperSpeed(rate);
            } else {
                sync.effectiveIdx = eff;

                // Speed from the LIVE tapFrac[] (not GRID_FRACTIONS) so the tap time
                // stays correct while the 60 ms measured->grid slew completes.
                const float targetTapSec = CLOCK_DIVISIONS[eff] * effectiveBeat();
                const float frac = std::max(1e-4f, tapFrac[head]);
                float ts = frac * BASE_LOOP_SECONDS / std::max(1e-4f, targetTapSec);

                // Rate NUDGE (±6%). Applied here, after the head and division
                // are chosen — never before, since feeding a nudged speed into
                // syncHeadFor() could flip which head is anchored mid-gesture.
                const float gridTs = clamp(ts, TAPE_SPEED_MIN,
                                           TAPE_SPEED_MIN + TAPE_SPEED_RANGE);
                const float nudgeReq = (nudgeCv - 0.5f) * 2.f * CLOCK_NUDGE_MAX_PCT;
                if (gangSyncOverride) {
                    // Gang while synced: the nudge knob temporarily spans the
                    // full speed range instead of ±6%, anchored so the position
                    // it was grabbed at still means the speed it meant then —
                    // keeps one knob meaning one thing for the whole gesture.
                    const float n  = nudgeCv;
                    const float n0 = sync.gangAnchorNudge;
                    const float a  = sync.gangAnchorPos;
                    float pos;
                    if (n >= n0) {
                        float d = 1.f - n0;
                        pos = (d > 1e-6f) ? a + (n - n0) / d * (1.f - a) : a;
                    } else {
                        pos = (n0 > 1e-6f) ? a * (n / n0) : a;
                    }
                    tapeSpeedTarget = taperSpeed(clamp(pos, 0.f, 1.f));
                } else {
                    tapeSpeedTarget = clamp(ts * (1.f + nudgeReq * 0.01f),
                                            TAPE_SPEED_MIN,
                                            TAPE_SPEED_MIN + TAPE_SPEED_RANGE);
                }
                // Achieved, not requested — a grid speed near a taper end
                // stop can swallow part of the trim.
                snapNudgePct = (gridTs > 1e-6f)
                               ? (tapeSpeedTarget / gridTs - 1.f) * 100.f
                               : 0.f;
                snapNudgeOn  = true;

                // Display-only publishes. Params are never written from the
                // audio thread — that races with GUI drag deltas. Do NOT
                // reintroduce a param-write version.
                // Effective mode = the head sync chose, in the parked reverb
                // family. Multi-head parked modes are never overridden.
                const int effMode = (refReverb ? 5 : 1) + head;
                if (singleHead) audibleMode = effMode;
                snapKnobPos = taperKnobPos(tapeSpeedTarget);
                snapModePos = (float)effMode;
                snapOn      = true;
                snapModeOn  = singleHead;
            }
        } else {
            // Measured taper, not a linear ramp — reproduces this unit's dead
            // zones at both travel ends.
            tapeSpeedTarget = taperSpeed(rate);
        }
        // POWER off overrides the SPEED only, after the sync branch has run,
        // so POWER behaves identically whether or not sync is engaged — sync
        // itself does not disengage when the motor stops.
        if (motorStopActive)
            tapeSpeedTarget = 0.f;
        sync.snapKnobPos   = snapKnobPos;
        sync.snapModePos   = snapModePos;
        sync.nudgePct      = snapNudgePct;
        sync.snapDisplayOn = snapOn;
        sync.snapModeOn    = snapModeOn;
        sync.nudgeOn       = snapNudgeOn;

        return audibleMode;
    }

    void decodeMode(int mode, float* gainTargets, bool& reverbOn) {
        // Mode switch: 0 = reverb-only (6 o'clock); 1..4 = echo-only variants;
        // 5..11 = echo + reverb variants (11 = "Swell" — all taps + reverb).
        // Index N matches the panel text label N around the dial; mode 0 sits
        // at the bottom "REVERB ONLY" position.
        static const float MODE_GAINS[12][3] = {
            {0.f, 0.f, 0.f},  // 0: Reverb only
            {1.f, 0.f, 0.f},  // 1: Tap1
            {0.f, 1.f, 0.f},  // 2: Tap2
            {0.f, 0.f, 1.f},  // 3: Tap3
            {0.f, 1.f, 1.f},  // 4: Tap2+3
            {1.f, 0.f, 0.f},  // 5: Tap1          + Reverb
            {0.f, 1.f, 0.f},  // 6: Tap2          + Reverb
            {0.f, 0.f, 1.f},  // 7: Tap3          + Reverb
            {1.f, 1.f, 0.f},  // 8: Tap1+2        + Reverb
            {0.f, 1.f, 1.f},  // 9: Tap2+3        + Reverb
            {1.f, 0.f, 1.f},  // 10: Tap1+3       + Reverb
            {1.f, 1.f, 1.f},  // 11: Swell (all   + Reverb)
        };
        if (mode >= 0 && mode <= 11) {
            gainTargets[0] = MODE_GAINS[mode][0];
            gainTargets[1] = MODE_GAINS[mode][1];
            gainTargets[2] = MODE_GAINS[mode][2];
        } else {
            gainTargets[0] = gainTargets[1] = gainTargets[2] = 0.f;
        }
        reverbOn = (mode == 0) || (mode >= 5 && mode <= 11);
    }

    // Beat period after the clock multiplier — the raw measured pulse period
    // stays untouched so changing the multiplier takes effect instantly
    // rather than re-converging.
    float effectiveBeat() const {
        return clock.beatPeriod / CLOCK_MULTS[clamp(clockMultIdx, 0, CLOCK_N_MULT - 1)];
    }

    // Which head sync would use for division `divIdx` at the current tempo
    // and parked mode — or -1 if no head can reach it.
    //
    // Single-head parked modes let sync choose: the head whose required tape
    // speed sits nearest SPEED_MED in log space (the tone model interpolates
    // in log-speed, and SPEED_MED is the centre detent). Ties go to the lower
    // head. Multi-head parked modes keep the user's mode and reference the
    // lowest active tap.
    //
    // Selection uses GRID_FRACTIONS (the enforced 1:2:3) rather than the live
    // slewing tapFrac[], so the choice cannot flicker mid-slew. process() still
    // computes the final speed from tapFrac[] so the tap time stays correct
    // while the slew completes.
    //
    // Shared by process() and the context menu — the menu greys out whatever
    // this returns -1 for.
    int syncHeadFor(int divIdx, int mode) {
        if (mode == 0 || !clock.hasPeriod) return -1;
        if (divIdx < 0 || divIdx >= CLOCK_N_DIVISIONS) return -1;
        float gains[3] = {0.f, 0.f, 0.f};
        bool  rv = false;
        decodeMode(mode, gains, rv);
        int nActive = 0;
        for (int i = 0; i < 3; i++) if (gains[i] > 0.5f) nActive++;
        if (!nActive) return -1;

        const float d    = CLOCK_DIVISIONS[divIdx] * effectiveBeat();
        const float sMin = TAPE_SPEED_MIN;
        const float sMax = TAPE_SPEED_MIN + TAPE_SPEED_RANGE;
        auto headSpeed = [&](int h) {
            return GRID_FRACTIONS[h] * BASE_LOOP_SECONDS / std::max(1e-4f, d);
        };
        // Candidate heads. A single-head parked mode lets sync pick ANY head —
        // the override swaps which tap sounds. A multi-head mode keeps the user's
        // tap set, so the reference must be one of the ACTIVE heads — but ANY of
        // them will do, because GRID_FRACTIONS enforces an exact 1:2:3 and
        // gridding one active head grids the rest. Referencing only the lowest
        // active tap (the original rule) was an arbitrary restriction that left
        // modes 8/10/11 with as few as ZERO reachable divisions at some tempos.
        //
        // Consequence, for the manual: in a multi-head mode the chosen division
        // describes whichever tap sync referenced, NOT necessarily the first one.
        bool cand[3];
        if (nActive == 1) {
            cand[0] = cand[1] = cand[2] = true;
        } else {
            for (int i = 0; i < 3; i++) cand[i] = (gains[i] > 0.5f);
        }
        int best = -1; float bestDist = 0.f;
        for (int h = 0; h < 3; h++) {
            if (!cand[h]) continue;
            float sp = headSpeed(h);
            if (sp < sMin || sp > sMax) continue;
            float dist = std::fabs(std::log(sp / SPEED_MED));
            if (best < 0 || dist < bestDist) { best = h; bestDist = dist; }
        }
        return best;
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "tapeDefectSeed",     json_integer((json_int_t)dropouts.defectSeed));
        json_object_set_new(rootJ, "frozenDynamics",     json_boolean(frozenDynamics));
        json_object_set_new(rootJ, "machineNoiseFloor",  json_boolean(machineNoiseFloor));
        json_object_set_new(rootJ, "ecoMode",            json_boolean(ecoMode));
        json_object_set_new(rootJ, "motorStopLatched",   json_boolean(motorStopLatched));
        json_object_set_new(rootJ, "reverseMomentary",   json_boolean(reverseMomentary));
        json_object_set_new(rootJ, "reverseLatched",     json_boolean(reverseLatched));
        json_object_set_new(rootJ, "reverbFollowsReverse", json_boolean(reverbFollowsReverse));
        json_object_set_new(rootJ, "gangEnabled",       json_boolean(gangEnabled));
        json_object_set_new(rootJ, "gangMomentary",     json_boolean(gangMomentary));
        json_object_set_new(rootJ, "tapeAgePreset",      json_real(tapeAgePreset));
        json_object_set_new(rootJ, "driveTiltMode",      json_integer(driveTiltMode));
        json_object_set_new(rootJ, "clockDivisionIdx",  json_integer(clockDivisionIdx));
        json_object_set_new(rootJ, "clockMultIdx",      json_integer(clockMultIdx));
        json_object_set_new(rootJ, "dryDefeat",          json_boolean(dryDefeat));
        json_object_set_new(rootJ, "toneInLoop",         json_boolean(toneInLoop));
        json_object_set_new(rootJ, "fbReturnBlend",      json_boolean(fbReturnBlend));
        json_object_set_new(rootJ, "fbInsertPostLoop",   json_boolean(fbInsertPostLoop));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        // Defaults first, JSON overlaid on top. Rack calls this on a LIVE,
        // already-initialised module for preset-open and paste, so without
        // resetting first, an older preset would keep whatever the previous
        // patch had set for any key it predates. Only settings are reset
        // here; DSP state and the delay tail are left alone.
        resetSettings();

        // Tape identity seed. Any value is legal except 0, which would give
        // buildDefects() a degenerate xorshift, so it's folded to a usable
        // seed rather than rejected.
        {
            json_t* seedJ = json_object_get(rootJ, "tapeDefectSeed");
            if (seedJ && json_is_integer(seedJ)) {
                uint32_t sd = (uint32_t)(json_integer_value(seedJ) & 0xffffffffu);
                dropouts.defectSeed = sd ? sd : 0x5EEDu;
                dropouts.buildDefects();
            }
        }

        frozenDynamics       = jsonBool(rootJ, "frozenDynamics",       frozenDynamics);
        frozenDynamics       = jsonBool(rootJ, "frozen",               frozenDynamics);  // legacy key
        machineNoiseFloor    = jsonBool(rootJ, "machineNoiseFloor",    machineNoiseFloor);
        ecoMode              = jsonBool(rootJ, "ecoMode",              ecoMode);
        motorStopLatched     = jsonBool(rootJ, "motorStopLatched",     motorStopLatched);
        reverseMomentary     = jsonBool(rootJ, "reverseMomentary",     reverseMomentary);
        reverseLatched       = jsonBool(rootJ, "reverseLatched",       reverseLatched);
        reverbFollowsReverse = jsonBool(rootJ, "reverbFollowsReverse", reverbFollowsReverse);
        gangEnabled          = jsonBool(rootJ, "gangEnabled",          gangEnabled);
        gangMomentary        = jsonBool(rootJ, "gangMomentary",        gangMomentary);
        dryDefeat            = jsonBool(rootJ, "dryDefeat",            dryDefeat);
        toneInLoop           = jsonBool(rootJ, "toneInLoop",           toneInLoop);
        fbReturnBlend        = jsonBool(rootJ, "fbReturnBlend",        fbReturnBlend);
        fbInsertPostLoop     = jsonBool(rootJ, "fbInsertPostLoop",     fbInsertPostLoop);

        // .vcv patches are shared between users — genuine untrusted input, so
        // range is enforced inside the reader, not left to each call site.
        tapeAgePreset    = jsonReal(rootJ, "tapeAgePreset",    tapeAgePreset,  0.f,  1.f);
        driveTiltMode    = jsonInt (rootJ, "driveTiltMode",    driveTiltMode,  0, 2);
        clockMultIdx     = jsonInt (rootJ, "clockMultIdx",     clockMultIdx,   0, CLOCK_N_MULT - 1);
        clockDivisionIdx = jsonInt (rootJ, "clockDivisionIdx", clockDivisionIdx,
                                    0, CLOCK_N_DIVISIONS - 1);
        // Derived, never persisted.
        sync.effectiveIdx = clockDivisionIdx;

        // A saved patch loaded with the toggle already on needs convReversed
        // actually built — it's lazy (see SpringReverb), so restoring the
        // bool alone isn't enough. Must come after reverbFollowsReverse is read.
        if (reverbFollowsReverse && !ecoMode)
            spring.prepareReversedNow(
                (int)(REV_SWELL_FREE_SEC * APP->engine->getSampleRate()));
    }

    void process(const ProcessArgs& args) override {
        // Denormal flush-to-zero (see helper above) — prevents subnormal CPU
        // spikes during sustained feedback. Effectively free after sample 1.
        enableFlushToZero();

        // Knob CVs (Stage 9b): ±5 V → 100 % knob travel, summed with knob
        // value and clamped to each param's native range.
        auto applyKnobCV = [&](int paramId, int inputId) -> float {
            float v = params[paramId].getValue();
            if (inputs[inputId].isConnected()) {
                ParamQuantity* pq = paramQuantities[paramId];
                float span = pq->getMaxValue() - pq->getMinValue();
                float cv   = clamp(inputs[inputId].getVoltage(), -5.f, 5.f);
                v = clamp(v + (cv / 5.f) * span, pq->getMinValue(), pq->getMaxValue());
            }
            return v;
        };
        float rate      = applyKnobCV(REPEAT_RATE_PARAM,   RATE_CV_INPUT);
        // RATE_CV drives the NUDGE while synced, rather than being silently
        // dead there — same jack, same ±5V = full travel convention.
        float nudgeCv   = applyKnobCV(RATE_NUDGE_PARAM,    RATE_CV_INPUT);
        float intensity = applyKnobCV(INTENSITY_PARAM,     INTENSITY_CV_INPUT);
        // Gang, CV half. Applied here, once, so both rate axes see the same
        // ganged value. While synced this deliberately uses the narrow ±6%
        // mapping — the widened range belongs to a deliberate grab only.
        if (gangEnabled) {
            const float gangH0 = params[INTENSITY_PARAM].getValue();
            rate    = gangApplyCv(rate,    gangH0, intensity);
            nudgeCv = gangApplyCv(nudgeCv, gangH0, intensity);
        }
        float echoVol   = applyKnobCV(ECHO_VOLUME_PARAM,   ECHO_VOLUME_CV_INPUT);
        float reverbVol = applyKnobCV(REVERB_VOLUME_PARAM, REVERB_VOLUME_CV_INPUT);
        // Taper the low end of Reverb Volume — linear response makes
        // everything past ~9 o'clock heavily reverberant.
        reverbVol = std::pow(reverbVol, REVERB_TAPER_EXP);
        float bass      = params[BASS_PARAM].getValue();
        float treble    = params[TREBLE_PARAM].getValue();
        // MODE CV: 1 V per step, summed with knob, snapped, clamped to 0..11.
        int modeCvSteps = 0;
        if (inputs[MODE_INPUT].isConnected())
            // Clamp the float before the cast — (int)std::round(NaN) or of a
            // huge float is undefined behaviour, too late to catch after the cast.
            modeCvSteps = (int)std::round(clamp(inputs[MODE_INPUT].getVoltage(), -20.f, 20.f));
        int mode = clamp(
            (int)std::round(params[MODE_PARAM].getValue()) + modeCvSteps,
            0, 11);

        // CLOCK follower — measure and smooth the beat period. Everything the
        // sync resolution below reads about tempo comes from here.
        clock.process(inputs[CLOCK_INPUT].isConnected(),
                      inputs[CLOCK_INPUT].getVoltage(), args.sampleRate);
        bool clockSnapActive = inputs[CLOCK_INPUT].isConnected() && clock.hasPeriod;

        // While sync is active the taps run on an EXACT 1:2:3 grid rather
        // than this unit's measured spacing, so every active head lands on the
        // beat grid together (see GRID_FRACTIONS). Slewed, not switched — the
        // h3 move is 1.1% (2-5 ms of read position) and stepping it clicks.
        for (int i = 0; i < 3; i++) {
            float target = clockSnapActive ? GRID_FRACTIONS[i] : TAP_FRACTIONS[i];
            if (frozenDynamics) tapFrac[i] = target;
            else                tapFrac[i] += tapFracCoef * (target - tapFrac[i]);
        }

        // OUTPUT_PAD (H/M/L) — a true output pad: pure level trim on the final
        // mix, with no effect on saturation drive or feedback loop gain (so the
        // the self-oscillation calibration holds at every setting).
        //   H (2):  0 dB (full output)
        //   M (1): -6 dB
        //   L (0): -12 dB
        int padMode = clamp((int)std::round(params[OUTPUT_PAD_PARAM].getValue()),
                            0, 2);
        const float PAD_OUT_GAIN[3] = { 0.2511886f, 0.5011872f, 1.f };  // L, M, H
        float padOutGain = PAD_OUT_GAIN[padMode];

        // Motor-stop: POWER toggle drives tapeSpeedTarget → 0
        // through the spin-down curve. (Gate input dropped in Stage 9b — the
        // panel toggle is the sole control.)
        float btnV  = params[MOTOR_STOP_PARAM].getValue();
        bool buttonRising = motorStopButtonTrig.process(btnV);
        if (buttonRising)
            motorStopLatched = !motorStopLatched;
        bool motorStopActive = motorStopLatched;

        // Clock-sync resolution — division to head, the ±6% nudge, the Gang
        // anchors, the resulting tape speed, and the staged display publish.
        int audibleMode = resolveClockSync(mode, rate, nudgeCv,
                                           clockSnapActive, motorStopActive);

        if (frozenDynamics) {
            tapeSpeedSmoother.instant(tapeSpeedTarget);
            motorStopSmootherMeasured.instant(tapeSpeedTarget);
            tapeSpeed = tapeSpeedTarget;
            motorStopGlideActive = false;
        } else {
            float tsCrit = tapeSpeedSmoother.process(tapeSpeedTarget);
            // Motor Stop glide: uses a directly-measured single-pole glide
            // for this event, rather than the extrapolated S-curve used for
            // normal Rate-knob turns. Seeded from the current position on the
            // rising edge; cleared once the re-engage glide actually settles.
            if (motorStopActive && !motorStopGlideActive)
                motorStopSmootherMeasured.instant(tapeSpeed);
            if (motorStopActive) motorStopGlideActive = true;

            if (motorStopGlideActive) {
                tapeSpeed = motorStopSmootherMeasured.process(tapeSpeedTarget);
                if (!motorStopActive && std::abs(tapeSpeed - tapeSpeedTarget) < 1e-4f)
                    motorStopGlideActive = false;
            } else {
                tapeSpeed = tsCrit;
            }
        }

        // Reverse playback: button OR gate, momentary/latching.
        float rBtnV  = params[REVERSE_PARAM].getValue();
        float rGateV = clamp(inputs[REVERSE_INPUT].getVoltage(), -10.f, 10.f);
        bool reverseRising  = reverseButtonTrig.process(rBtnV);
        reverseGateTrig.process(rGateV);
        bool reverseGateHi  = reverseGateTrig.isHigh();
        bool reverseHeld    = rBtnV >= 0.5f;
        if (reverseMomentary) {
            reverseLatched = false;
        } else if (reverseRising) {
            reverseLatched = !reverseLatched;
        }
        bool reverseActive = reverseGateHi
            || (reverseMomentary ? reverseHeld : reverseLatched);

        // Velocity target: +1 forward unity, −1 reverse unity. Single smoother
        // glides through 0 (paused) on both engage and disengage.
        float tapeVelocityTarget = reverseActive ? -1.f : 1.f;
        if (frozenDynamics) {
            tapeVelocity = tapeVelocityTarget;
        } else {
            tapeVelocity += tapeVelocityCoef * (tapeVelocityTarget - tapeVelocity);
        }

        // Tape-stutter events (Thrashed+Dumpster) — worn tape briefly loses
        // contact and re-engages at a slightly different spot: a brief offset
        // on the tap-read positions plus a masking amplitude dip.
        float retriggerCeiling = 0.05f
            + 0.25f * ((xorshiftFloat(stutter.rng) + 1.f) * 0.5f);
        if (!frozenDynamics && tapeAge > stutter.threshold
                && stutter.envelope < retriggerCeiling) {
            float age01 = (tapeAge - stutter.threshold) / (1.f - stutter.threshold);
            float prob  = age01 * age01 * 1.2f / args.sampleRate;  // up to ~1.2/sec
            float r     = (xorshiftFloat(stutter.rng) + 1.f) * 0.5f;
            if (r < prob) {
                stutter.envelope = 1.f;
                // Random signed sample-offset: ±40..±500 samples
                // (~0.8–10 ms at 48 k). Forward and backward skips equally likely.
                float r2 = xorshiftFloat(stutter.rng);  // [-1..1]
                float magNorm = (xorshiftFloat(stutter.rng) + 1.f) * 0.5f;
                float magSamples = 40.f + 460.f * magNorm * magNorm;
                stutter.skipSamples = (r2 < 0.f ? -1.f : 1.f) * magSamples;
                // Per-event duration: 25–150 ms, biased short.
                float r3 = (xorshiftFloat(stutter.rng) + 1.f) * 0.5f;
                float durationSec = 0.025f + 0.125f * r3 * r3;
                stutter.decayCoef = 1.f - std::exp(-1.f / (durationSec * args.sampleRate));
                // Per-event dip depth: 0.4..0.9 — some events are barely
                // attenuated, others nearly dropouts.
                float r4 = (xorshiftFloat(stutter.rng) + 1.f) * 0.5f;
                stutter.dipDepth = 0.4f + 0.5f * r4;
            }
        }
        stutter.envelope -= stutter.decayCoef * stutter.envelope;

        // audibleMode, not mode — sync's head override must reach the tap
        // gains. tapGain slews via tapGainCoef, so a head change crossfades.
        decodeMode(audibleMode, tapGainTarget, reverbActive);
        for (int i = 0; i < 3; i++)
            tapGain[i] += tapGainCoef * (tapGainTarget[i] - tapGain[i]);

        // A non-finite value here round-trips into delayBuf permanently.
        // clamp is fmax(fmin(x, b), a), so it discards NaN too.
        float in = clamp(inputs[IN_INPUT].getVoltage(), -10.f, 10.f);
        // Input drive: exponential 1/base..base across knob 0..2.
        //   knob 0   → drive 1/base (very clean, signal still passes)
        //   knob 1   → drive 1      (unity gain through dry path)
        //   knob 2   → drive base   (heavily saturated tape, "limited at top")
        // base is set by the Drive tilt preset: 4 / 8 (default) / 16.
        const float tiltBase   = TILT_DRIVE_BASE[driveTiltMode];
        const float tiltExp    = TILT_COMP_EXP[driveTiltMode];
        const float tiltHFFlr  = TILT_HF_FLOOR[driveTiltMode];
        float drive = std::pow(tiltBase, params[INPUT_LEVEL_PARAM].getValue() - 1.f);
        in *= drive;

        // Measured input-preamp drive character — applied only to the signal
        // feeding the tape, not to `in` itself, so the dry path and makeup
        // gain stay untouched. A separate, preceding nonlinearity from the
        // Chowdhury tape-hysteresis modeling below; oversampled to avoid aliasing.
        float tapeInputDrive = inputDrive.process(in, ecoMode);

        // Tape Age: TAPE_AGE_INPUT (when connected) replaces the right-click
        // preset baseline — same 0–10 V → 0–1 scaling as the original Stage 8A CV.
        tapeAgeTarget = inputs[TAPE_AGE_INPUT].isConnected()
            ? clamp(inputs[TAPE_AGE_INPUT].getVoltage() / 10.f, 0.f, 1.f)
            : tapeAgePreset;
        if (frozenDynamics) {
            tapeAge = tapeAgeTarget;
        } else {
            tapeAge += tapeAgeCoef * (tapeAgeTarget - tapeAge);
        }

        // (Hysteresis drive scaling removed in Pass A — outGain rescales to preserve
        // small-signal gain, making the change inaudible on most material.)

        // Eco mode toggle: reset the newly-activated AA filters to avoid a click.
        // reset() uses memset only — RT-safe, no allocation.
        if (ecoMode != ecoModePrev) {
            tapeWrite.sat.resetForOversampleChange(ecoMode);
            inputDrive.resetForOversampleChange(ecoMode);
            wetDriveClip.resetForOversampleChange(ecoMode);
            dryClipStage.resetForOversampleChange(ecoMode);
            wetClipStage.resetForOversampleChange(ecoMode);
            finalClipStage.resetForOversampleChange(ecoMode);
            ecoModePrev = ecoMode;
        }

        // Wow/flutter
        if (frozenDynamics) flutterRngState = FLUTTER_RNG_SEED;
        float perHeadOffset[3];
        // Overall age-wobble scale — makes the top presets read as wobbly
        // rather than purely dropout-driven. Scales capstan/loop wow, the
        // stochastic term and the jitter target together. By-ear tunable.
        float ageScale = 1.f + 10.4f * tapeAge;
        // "Mashed up" bumps — CUBIC in age, so it is nearly inert through
        // the middle of the range and only bites at the top. Multiplies the
        // irregular components only; see WowFlutter::process. By-ear tunable.
        float bumpScale = 1.f + 4.0f * tapeAge * tapeAge * tapeAge;
        // Pronounced wow — QUADRATIC, so it is well established by the upper
        // end of the range rather than waiting for the very top like
        // bumpScale. Periodic terms only. By-ear tunable.
        float wowScale = 1.f + 2.0f * tapeAge * tapeAge;
        // Jitter target — same 3-point Slow/Med/Fast anchor idiom as
        // loopLpAirWidenHz, converted from microseconds to seconds.
        float jitterUs;
        if (tapeSpeed <= SPEED_SLOW) {
            jitterUs = FLUTTER_JITTER_US_SLOW;
        } else if (tapeSpeed >= SPEED_FAST) {
            jitterUs = FLUTTER_JITTER_US_FAST;
        } else if (tapeSpeed < SPEED_MED) {
            float t = (tapeSpeed - SPEED_SLOW) / (SPEED_MED - SPEED_SLOW);
            jitterUs = FLUTTER_JITTER_US_SLOW + (FLUTTER_JITTER_US_MED - FLUTTER_JITTER_US_SLOW) * t;
        } else {
            float t = (tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED);
            jitterUs = FLUTTER_JITTER_US_MED + (FLUTTER_JITTER_US_FAST - FLUTTER_JITTER_US_MED) * t;
        }
        wow.process(args.sampleRate, frozenDynamics, flutterRngState, perHeadOffset, ageScale,
                    jitterUs * 1e-6f, tapeAge, bumpScale, wowScale);

        // Slow drift (warped-tape wobble) — independent of wow/flutter, scales
        // quadratically with age. Integrated as varispeed: position offset is
        // the integral of speed deviation.
        //
        // The integrator keeps LEAKING while gated off and the offset is
        // always applied, so it decays continuously to zero over
        // DRIFT_LEAK_TAU instead of jumping when the age gate crosses —
        // gating only the drive term avoids a one-sample tap-position jump.
        if (!frozenDynamics) {
            // Bumped to ~1.8% wobble at full age, with age^1.5 curve so warble
            // arrives earlier in the gradient (no longer "only at Dumpster").
            const bool aged = tapeAge > 0.001f;
            float driftSpeed = 0.f;
            if (aged) {
                float driftDepth = 0.018f * tapeAge * std::sqrt(tapeAge);
                driftPhase += driftRate / args.sampleRate;
                if (driftPhase >= 1.f) driftPhase -= 1.f;
                driftSpeed = driftDepth * std::sin(2.f * float(M_PI) * driftPhase);
            }
            float dt   = 1.f / args.sampleRate;
            float leak = dt / DRIFT_LEAK_TAU;
            for (int i = 0; i < 3; i++) {
                driftPosition[i] += -driftSpeed * dt - driftPosition[i] * leak;
                perHeadOffset[i] += driftPosition[i];
            }
        } else {
            driftPosition[0] = driftPosition[1] = driftPosition[2] = 0.f;
        }

        // Read delay taps with per-head EQ (speed-coupled bump + HF rolloff).
        // Clamp tapeSpeed at 0.1 in the delay denominator so motor-stop (target → 0)
        // doesn't blow past the delay buffer. Matches the clamp in HeadEQ::interpolateCoefs.
        float delaySamples = BASE_LOOP_SECONDS * args.sampleRate / std::max(0.1f, tapeSpeed);

        // Reverse tape transport — walking-tap fade, loop length and grain
        // period, the two read phases, and the handoff crossfade weights.
        const int eIdx = clamp(sync.effectiveIdx, 0, CLOCK_N_DIVISIONS - 1);
        ReverseTransport::Result rev =
            reverse.process(reverseActive, frozenDynamics, tapeVelocity, tapeSpeed,
                            delaySamples, tapFrac, tapGain, bufSizeFloat, args.sampleRate,
                            sync.snapDisplayOn, CLOCK_DIVISIONS[eIdx], effectiveBeat());
        const bool  useWalking = rev.useWalking;
        const float fadeA      = rev.fadeA;
        const float fadeB      = rev.fadeB;

        // Pre-compute per-head tap positions and their delay reads, so each
        // head's crosstalk contribution can sample any other head's position
        // without recomputing reads. Stutter offset is envelope-scaled, so it
        // ramps in/out with the event and is exactly zero when inactive.
        float stutterOffset = stutter.envelope * stutter.skipSamples;

        float fwdRead[3];
        float walkRead[3];
        for (int i = 0; i < 3; i++) {
            float perHeadSec = perHeadOffset[i] * args.sampleRate;
            float fwdTap     = tapFrac[i] * delaySamples + perHeadSec + stutterOffset;
            fwdRead[i] = readDelayHermite(fwdTap);
            if (useWalking) {
                float headOffset = (tapFrac[i] - tapFrac[0]) * delaySamples
                                    + perHeadSec + stutterOffset;
                float walkTapA = reverse.phaseA + headOffset;
                float walkTapB = reverse.phaseB + headOffset;
                walkRead[i] = fadeA * readDelayHermite(walkTapA)
                            + fadeB * readDelayHermite(walkTapB);
            }
        }

        float wetSum = 0.0f;
        // Forward-only sum, decoupled from reverse, feeds the loop
        // regeneration (feedbackTap) instead of wetSum. With Reverse engaged
        // and Intensity active, the feedback loop writing back the
        // REVERSE-crossfaded signal would get read in reverse AGAIN on the
        // next pass — a fundamentally different, never-measured feedback
        // topology, since the loop-gain and hysteresis tuning throughout this
        // file all assume plain forward regeneration. Reverse changes what's
        // monitored, not what regenerates — see headEQFwd/fwdWetBass/
        // fwdWetTreble member comments for why a fully independent filter
        // chain was needed rather than just swapping which sum feeds
        // feedbackTap.
        float fwdWetSum = 0.0f;
        float headPostEQ[3] = {0.f, 0.f, 0.f};
        for (int i = 0; i < 3; i++) {
            // Self-read + small-gain crosstalk from the other two heads.
            float fwdSample = fwdRead[i];
            float walkSample = useWalking ? walkRead[i] : 0.f;
            for (int j = 0; j < 3; j++) {
                if (j == i) continue;
                float gain = (std::abs(i - j) == 1) ? crosstalk.adjacentGain
                                                    : crosstalk.nonAdjacentGain;
                fwdSample += gain * fwdRead[j];
                if (useWalking) walkSample += gain * walkRead[j];
            }

            float sample = useWalking
                ? reverse.fade * walkSample + (1.f - reverse.fade) * fwdSample
                : fwdSample;

            // Tape Age: -80% HF cutoff at full age. Both heads' coefficients
            // come from identical inputs, so one guard serves the pair.
            const float hfScale = 1.f - 0.8f * tapeAge;
            if (std::fabs(tapeSpeed - eqLastSpeed[i]) > EQ_RECOOK_EPS
             || std::fabs(hfScale  - eqLastHf[i])    > EQ_RECOOK_EPS) {
                headEQ[i].interpolateCoefs(args.sampleRate, tapeSpeed, hfScale);
                headEQFwd[i].interpolateCoefs(args.sampleRate, tapeSpeed, hfScale);
                eqLastSpeed[i] = tapeSpeed;
                eqLastHf[i]    = hfScale;
            }
            sample = headEQ[i].process(sample);
            headPostEQ[i] = sample;
            wetSum += sample * tapGain[i];

            // Forward-only twin — coefficients cooked alongside headEQ[i]
            // above, independent filter memory, fed by fwdSample
            // (pre-reverse-blend, post-crosstalk).
            fwdWetSum += headEQFwd[i].process(fwdSample) * tapGain[i];
        }
        // Same `motorGain` the wet path uses, so POWER off fades the head
        // outputs along with the main mix. They still deliberately bypass the
        // Output Pad and final clip stages — raw taps, not a second mix output.
        float motorGain = clamp(tapeSpeed / TAPE_SPEED_MIN, 0.f, 1.f);
        outputs[OUT_H1_OUTPUT].setVoltage(headPostEQ[0] * motorGain);
        outputs[OUT_H2_OUTPUT].setVoltage(headPostEQ[1] * motorGain);
        outputs[OUT_H3_OUTPUT].setVoltage(headPostEQ[2] * motorGain);

        // Tone corners/ranges from the RE-201 service manual: Bass ±10dB @
        // 100Hz, Treble ±10dB @ 5kHz — milder than an earlier ±12/200Hz, since
        // per-repeat compounding inside the feedback loop makes ±10 plenty.
        if (bass != lastBass) {
            wetBass.setShelf(args.sampleRate, 100.f, bass * 10.f, false);
            fwdWetBass.setShelf(args.sampleRate, 100.f, bass * 10.f, false);
            lastBass = bass;
        }
        if (treble != lastTreble) {
            wetTreble.setShelf(args.sampleRate, 5000.f, treble * 10.f, true);
            fwdWetTreble.setShelf(args.sampleRate, 5000.f, treble * 10.f, true);
            lastTreble = treble;
        }
        // Tone (Bass/Treble) applied once to the summed playback taps. When
        // toneInLoop is set, this same toned signal feeds the feedback tap
        // below, so coloration compounds per pass. When cleared, tone is a
        // one-shot output EQ (legacy).
        float wetToned = wetTreble.process(wetBass.process(wetSum));
        float wet = wetToned;
        // Color boost, two bands — applied here, on the audible tap only,
        // since `wet` never reaches `feedbackTap` (that comes from
        // `fwdWetToned`/`fwdWetSum` below), so this can't affect loop gain.
        // Midrange band's gain is speed-coupled, recomputed here per-sample.
        {
            float gainDb;
            if (tapeSpeed <= SPEED_MED) {
                gainDb = MIDRANGE_BOOST_GAIN_DB_SLOWMED;
            } else {
                float t = clamp((tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED), 0.f, 1.f);
                gainDb = MIDRANGE_BOOST_GAIN_DB_SLOWMED
                       + (MIDRANGE_BOOST_GAIN_DB_FAST - MIDRANGE_BOOST_GAIN_DB_SLOWMED) * t;
            }
            float A = std::pow(10.f, gainDb / 40.f);
            float w0 = 2.f * float(M_PI) * MIDRANGE_BOOST_FC_HZ / args.sampleRate;
            float alpha = std::sin(w0) / (2.f * MIDRANGE_BOOST_Q);
            float cosw0 = std::cos(w0);
            float a0 = 1.f + alpha / A;
            midColor.b0 = (1.f + alpha * A) / a0;
            midColor.b1 = (-2.f * cosw0) / a0;
            midColor.b2 = (1.f - alpha * A) / a0;
            midColor.a1 = (-2.f * cosw0) / a0;
            midColor.a2 = (1.f - alpha / A) / a0;
        }
        wet = lowColor.process(midColor.process(wet));
        // Forward-only counterpart, same shelf coefficients, independent
        // filter state — feeds feedbackTap below instead of wetToned.
        float fwdWetToned = fwdWetTreble.process(fwdWetBass.process(fwdWetSum));
        float activeCount = std::max(tapGain[0] + tapGain[1] + tapGain[2], 0.001f);
        // Output-only insert mode: FB_RETURN, if patched and fbInsertPostLoop
        // is set, substitutes here instead of at the regenerative feedback
        // tap below — isolated, not fed back into the tape. Default replaces
        // the tap-echo signal; fbReturnBlend sums it instead.
        //
        // Gain staging: `feedbackTap` below is already divided by activeCount,
        // but `wet` here isn't yet (that happens later at echoWetOut).
        // Multiplying fbReturn by activeCount here cancels that second
        // division so it doesn't get halved again in multi-head modes.
        if (inputs[FB_RETURN_INPUT].isConnected() && fbInsertPostLoop) {
            float fbReturn = clamp(inputs[FB_RETURN_INPUT].getVoltage(), -10.f, 10.f) * activeCount;
            wet = fbReturnBlend ? (wet + fbReturn) : fbReturn;
        }

        // High-drive extra soft-clip on the wet path. Blends in only above the
        // knob's midpoint, full strength at the top — output-amp-style
        // compression on top of the Chowdhury saturation. Oversampled to
        // avoid aliasing, same as InputDriveStage.
        float topClipAmount = clamp((drive - 1.f) / (tiltBase - 1.f), 0.f, 1.f);
        if (topClipAmount > 0.001f) {
            float k = 1.f + topClipAmount * 1.5f;     // 1.0 → 2.5
            float clipped = wetDriveClip.process(wet, k, ecoMode);
            wet = wet + topClipAmount * (clipped - wet);
        }

        // Tape-age dropouts: loop-positioned defects above tapeAge=0.5,
        // ~250ms decay, applied at the delay-buffer write so the dropout
        // persists across echo repeats.
        if (!frozenDynamics && tapeAge > dropouts.threshold) {
            float age01 = (tapeAge - dropouts.threshold) / (1.f - dropouts.threshold);
            // Advance the loop by the same tape motion that drives the delay,
            // so the period inherits wow/flutter/drift/Rate/CV/Gang/Motor Stop
            // automatically, without its own separate randomness.
            float prevPhase = dropouts.loopPhase;
            dropouts.loopPhase += tapeSpeed * args.sampleTime
                                / (DropoutGen::LOOP_REVS_OF_BASE * BASE_LOOP_SECONDS);
            bool wrapped = dropouts.loopPhase >= 1.f;
            if (wrapped) dropouts.loopPhase -= 1.f;
            // Fire on any defect the loop crossed this sample. The window is one
            // sample wide, so at most one fires in practice; the loop is written to
            // cope with a wrap landing between two of them.
            for (int i = 0; i < dropouts.activeDefects; ++i) {
                float pos = dropouts.defectPos[i];
                bool hit = wrapped ? (pos > prevPhase || pos <= dropouts.loopPhase)
                                   : (pos > prevPhase && pos <= dropouts.loopPhase);
                if (!hit) continue;
                dropouts.attacking = true;
                // Depth: the defect's own fixed severity, mapped into the
                // age-scaled window, so a bad spot deepens with age rather
                // than being redrawn each time.
                float ceil = DropoutGen::DEPTH_CEIL_LO
                           + age01 * (DropoutGen::DEPTH_CEIL_HI - DropoutGen::DEPTH_CEIL_LO);
                dropouts.depth = DropoutGen::DEPTH_MIN
                               + dropouts.defectSev[i] * (ceil - DropoutGen::DEPTH_MIN);
                break;
            }
        }
        if (dropouts.attacking) {
            dropouts.envelope += dropouts.attackCoef * (1.f - dropouts.envelope);
            if (dropouts.envelope > 0.99f) dropouts.attacking = false;
        } else {
            dropouts.envelope -= dropouts.decayCoef * dropouts.envelope;
        }
        float dropoutGain = 1.f - dropouts.depth * dropouts.envelope;

        // Stutter amp dip masks the read-position transitions. Per-event depth
        // (0.4..0.9) varies the attenuation each time — some chews are subtle,
        // others nearly dropouts.
        float stutterGain = 1.f - stutter.dipDepth * stutter.envelope;

        // Feedback path: saturate the feedback tap, then mix with input for
        // write. FB_SEND always emits feedbackTap regardless of mode or
        // connection state. fbInsertPostLoop selects where FB_RETURN
        // re-enters: false (default) = in-loop/compounding, right here; true
        // = output-only, handled below at `wet` instead.
        //
        // Reads the forward-only sum, not the reverse-blended one — the
        // regenerative loop stays forward-only regardless of Reverse; only
        // the listener-facing `wet` reflects the reverse crossfade.
        float feedbackTap  = (toneInLoop ? fwdWetToned : fwdWetSum) / activeCount;
        outputs[FB_SEND_OUTPUT].setVoltage(feedbackTap);
        float loopFeedback = feedbackTap;
        if (inputs[FB_RETURN_INPUT].isConnected() && !fbInsertPostLoop) {
            // Clamp to ±10V — protects the hysteresis saturator from
            // Inf/NaN if an upstream module misbehaves.
            float fbReturn = clamp(inputs[FB_RETURN_INPUT].getVoltage(), -10.f, 10.f);
            loopFeedback = fbReturnBlend ? (feedbackTap + fbReturn) : fbReturn;
        }
        // Loop hysteresis + per-mode trim. Track established-loop hotness
        // from `loopFeedback`, since that's what's actually about to hit the
        // record head and regenerate. Boosts effective Intensity while hot
        // (bistable: rising-unity up, falling-unity down), and trims the base
        // gain for the all-heads/slow mode hardware can't oscillate.
        // fbLevel is pre-conditioned through a short mean-square
        // (RMS-style) smoothing stage before reaching loopEnv's tracker, so
        // it reflects true signal power rather than a single instantaneous
        // sample's crest-factor-sensitive magnitude. See comment
        // above FB_RMS_TAU.
        float fbLevelSq = loopFeedback * loopFeedback;
        fbRmsZ += fbRmsCoef * (fbLevelSq - fbRmsZ);
        float fbLevel = std::sqrt(std::max(fbRmsZ, 0.f));
        loopEnv += (fbLevel > loopEnv ? loopEnvAtkCoef : loopEnvRelCoef) * (fbLevel - loopEnv);
        float ht = clamp((loopEnv - LOOP_ENV_LO) / (LOOP_ENV_HI - LOOP_ENV_LO), 0.f, 1.f);
        float hotness = ht * ht * (3.f - 2.f * ht);   // smoothstep
        // Distance below the "loud" reference, in dB — shared by the
        // loss-relief calc just below and the brightening calc further
        // down.
        float loopLevelDb = 20.f * std::log10(std::max(loopEnv, 1e-6f) / LOOP_BRIGHTEN_REF_V);
        // Per-pass loop loss — level cut plus HF/LF/sub-bass
        // bandwidth narrowing, applied to the feedback path and, with independent
        // filter memory, to the first pass.
        LoopLoss::Result loss = loopLoss.process(loopFeedback, tapeInputDrive,
                                                 tapeSpeed, loopLevelDb, args.sampleRate);
        float fbSat = loss.feedback;
        float tapeInputDriveFirstPassLoss = loss.firstPass;
        // First-pass HF shelf (see comment above FIRST_PASS_SHELF_FC_HZ).
        // Speed-coupled gain, same 3-point Slow/Med/Fast anchor idiom as
        // loopLpAirWidenHz above, so it is recomputed per-sample alongside it.
        {
            float gDb;
            if (tapeSpeed <= SPEED_SLOW) {
                gDb = FIRST_PASS_SHELF_GAIN_DB_SLOW;
            } else if (tapeSpeed >= SPEED_FAST) {
                gDb = FIRST_PASS_SHELF_GAIN_DB_FAST;
            } else if (tapeSpeed < SPEED_MED) {
                float t = (tapeSpeed - SPEED_SLOW) / (SPEED_MED - SPEED_SLOW);
                gDb = FIRST_PASS_SHELF_GAIN_DB_SLOW
                    + (FIRST_PASS_SHELF_GAIN_DB_MED - FIRST_PASS_SHELF_GAIN_DB_SLOW) * t;
            } else {
                float t = (tapeSpeed - SPEED_MED) / (SPEED_FAST - SPEED_MED);
                gDb = FIRST_PASS_SHELF_GAIN_DB_MED
                    + (FIRST_PASS_SHELF_GAIN_DB_FAST - FIRST_PASS_SHELF_GAIN_DB_MED) * t;
            }
            // RBJ high-shelf. At gDb = 0 this gives b == a exactly (identity),
            // so speeds with no correction are bit-unchanged.
            float A     = std::pow(10.f, gDb / 40.f);
            float c     = firstPassShelfCosW0;
            float alpha = firstPassShelfSinW0 * 0.5f
                        * std::sqrt(std::max((A + 1.f / A) * (1.f / FIRST_PASS_SHELF_S - 1.f) + 2.f, 1e-9f));
            float sa    = 2.f * std::sqrt(A) * alpha;
            float a0    = (A + 1.f) - (A - 1.f) * c + sa;
            float b0    = A * ((A + 1.f) + (A - 1.f) * c + sa) / a0;
            float b1    = -2.f * A * ((A - 1.f) + (A + 1.f) * c) / a0;
            float b2    = A * ((A + 1.f) + (A - 1.f) * c - sa) / a0;
            float a1    = 2.f * ((A - 1.f) - (A + 1.f) * c) / a0;
            float a2    = ((A + 1.f) - (A - 1.f) * c - sa) / a0;
            firstPassShelf.b0 = b0; firstPassShelf.b1 = b1; firstPassShelf.b2 = b2;
            firstPassShelf.a1 = a1; firstPassShelf.a2 = a2;
            tapeInputDriveFirstPassLoss = firstPassShelf.process(tapeInputDriveFirstPassLoss);
        }
        // Reverse-release safeguard: while the walking tap fades out after
        // reverse disengages, the buffer can hold a self-sustaining loop that
        // Intensity alone can't break. Multiply feedback gain by (1 -
        // walkingFade) during release to break it as walking winds down.
        float reverseFbGate = reverseActive ? 1.f : (1.f - reverse.fade);
        // Clamp to 1.0 so the top of the knob is unchanged vs. the static
        // model; the hysteresis boost acts purely in the sub-unity hold band.
        float hystContribution = hotness * hystOffset(tapeSpeed);
        float effIntensity = std::min(intensity + hystContribution, 1.f);
        // Input-keyed feedback decay. Below noon (drive < 1) the loop gain
        // tapers with Input, so pulling Input down dissolves self-oscillation
        // like a partial Intensity drop. At noon and above the factor is 1.0.
        float inputFbDecay = std::pow(std::min(drive, 1.f), INPUT_FB_DECAY_EXP);
        // Input-drives-feedback: above noon, boost loop gain so pushing Input
        // up drives a given Intensity toward self-oscillation.
        float inputAboveNoon = clamp(params[INPUT_LEVEL_PARAM].getValue() - 1.f, 0.f, 1.f);
        float inputFbBoost = 1.f + inputAboveNoon * (INPUT_FB_BOOST_MAX - 1.f);
        // Keyed on the raw `intensity` knob, not effIntensity, so this can't
        // be contaminated by the loop-heating hystOffset boost.
        float fbGain = effIntensity * FEEDBACK_GAIN_MAX * modeTrim(mode, tapeSpeed) * reverseFbGate * inputFbDecay * inputFbBoost * intensityTaper(intensity, tapeSpeed);
        // "Scary" runaway. Keyed on the raw knob (not effIntensity) so it
        // tracks the physical top of travel; squared ramp above SCARY_THRESH.
        float scary = clamp((intensity - SCARY_THRESH) / (1.f - SCARY_THRESH), 0.f, 1.f);
        float scaryTarget = 1.f + scary * scary * (SCARY_MAX_MULT - 1.f);
        scarySlew += ((scaryTarget > scarySlew) ? scarySlewUpCoef : scarySlewDownCoef)
                     * (scaryTarget - scarySlew);
        fbGain *= scarySlew;
        float recordIn = tapeInputDriveFirstPassLoss + fbGain * fbSat;

        // VU meter + Peak LED: read `recordIn` — input plus feedback, the
        // actual signal hitting the record head, not the pre-feedback-loop
        // input. This lets the meter show self-oscillation building/pinning
        // with no live input, matching the real unit's playback-tap meter.
        // When motor is stopped, freeze both values so the meter "powers
        // down" — needle and peak LED hold, widget paints a dark scrim.
        float meterGain = clamp(tapeSpeed / TAPE_SPEED_MIN, 0.f, 1.f);
        if (meterGain > 1e-3f) vu.process(recordIn);

        // Tape Age loop noise — recorded onto the tape, so it passes the
        // saturator, head EQ age rolloff, and the whole feedback loop,
        // compounding per generation. Makes an aged echo dull-and-present
        // rather than clean-and-absent. Keyed to tapeAge only, not gated by
        // `machineNoiseFloor`. Third independent read position into the same
        // noise WAV, deliberately never reset (see OutputArtifacts).
        // Quadratic in age so Mint and the early presets stay clean.
        float loopNoise = 0.f;
        if (tapeAge > 0.001f && !spring.noiseFloor.empty()) {
            float lnGain = LOOP_NOISE_GAIN;
            if (machineNoiseFloor) lnGain *= LOOP_NOISE_MASK_BOOST;
            loopNoise = spring.noiseFloor[loopNoisePos]
                      * (lnGain * tapeAge * tapeAge);
            loopNoisePos = (loopNoisePos + 1) % (int)spring.noiseFloor.size();
        }

        // Write path — saturation, bypass/expand, and the loop high-pass.
        float writeOut = tapeWrite.process(recordIn + loopNoise, ecoMode, T_os8, T_os4,
                                           loopLevelDb, hotness);
        // A stronger per-repeat level loss compounds too fast at high age,
        // dropping the echo tail below the noise floor. This keeps some real
        // level loss while leaving the echo above the floor.
        float levelLoss = 1.f - 0.08f * tapeAge;
        // The loop's one write point — the one place a non-finite value can
        // be caught before it becomes permanent. Not a silent clamp: if this
        // fires, only a full reset is trustworthy.
        float writeSample = writeOut * dropoutGain * stutterGain * levelLoss + ANTI_DENORMAL_DC;
        if (!std::isfinite(writeSample)) {
            resetDspState();
            writeSample = 0.f;
        }
        delayBuf[writeIdx] = writeSample;
        writeIdx = (writeIdx + 1) % delayBuf.size();

        // Spring reverb path. Modes 1..4 are echo-only: spring receives no
        // input and the output gate ramps down over τ ≈ 80ms. Mode 0 + 5..11
        // leave both input and gate at full.
        float inAbs = std::fabs(in) * 0.2f;  // VCV ±5V → 0..1 scale
        inEnv += inEnvCoef * (inAbs - inEnv);

        // Smooth output gate toward 1.0 when reverbActive, else toward 0.0.
        float reverbActiveTarget = reverbActive ? 1.f : 0.f;
        reverbActiveGate += reverbActiveGateCoef * (reverbActiveTarget - reverbActiveGate);

        // Drive compensation factor. Computed here since the spring's conv
        // excitation is pre-compensated below so the reverb tail can't swell
        // on Input-knob moves; the dry path divides by it at the output stage.
        float driveComp = std::pow(drive, tiltExp);
        // Asymmetric-slewed makeup for the tape ECHO — slow to follow
        // driveComp down (so the still-hot delayed repeats aren't spiked),
        // fast up. Converges to driveComp when the knob is still.
        echoMakeupSlew += ((driveComp < echoMakeupSlew) ? echoMakeupDownCoef
                                                        : echoMakeupUpCoef)
                          * (driveComp - echoMakeupSlew);

        float springOut = 0.f;
        if (reverbVol > 0.001f) {
            // The RE-201's spring reverb is a parallel effect: fed only by the
            // dry input, never the echo repeats. In echo-only modes it feeds
            // zero so the spring decays naturally during the gate ramp. 0.2
            // feed matches the calibrated Mode-0 (dry-only) level.
            float springIn = reverbActive ? 0.2f * in : 0.f;
            // Feed the conv a drive-compensated excitation so the reverb tail
            // rings out at a fixed level regardless of later Input-knob moves;
            // the morph still tracks the uncompensated springIn.
            springOut = spring.process(springIn, springIn / driveComp);
            // Convolution output → VCV voltage scale, tuned by ear. The
            // conditioned IRs are equal-energy normalized to peak 0.5, so this
            // is this stage's own broadband gain.
            constexpr float SPRING_CONV_OUTPUT_GAIN = 13.8f;
            springOut *= SPRING_CONV_OUTPUT_GAIN;

            // "Reverb follows Reverse". Gated off in Eco mode; runs
            // continuously whenever enabled so convReversed's FDL is already
            // warmed up by the time Reverse engages. Crossfades on the same
            // walkingFade the tape path uses.
            //
            // Swell = one division while synced, so it resolves on the beat,
            // same principle as the reverse tape grain length. Free-running
            // falls back to a fixed length.
            if (sync.snapDisplayOn) {
                const float divSec =
                    CLOCK_DIVISIONS[clamp(sync.effectiveIdx, 0, CLOCK_N_DIVISIONS - 1)]
                    * effectiveBeat();
                // Nearest musical multiple of the division to a ~1s reverb
                // time, so the swell is long enough to be a spring while
                // still peaking on a grid point.
                const float mult = (divSec > 1e-4f)
                    ? nearestMusicalMultiple(REV_SWELL_TARGET_SEC / divSec, 8.f) : 1.f;
                reversedSwellTarget = mult * divSec * args.sampleRate;
            } else {
                reversedSwellTarget = REV_SWELL_FREE_SEC * args.sampleRate;
            }

            if (reverbFollowsReverse && !ecoMode && spring.reversedReady()) {
                float springOutRev = spring.processReversed(springIn / driveComp) * SPRING_CONV_OUTPUT_GAIN;
                springOut = reverse.fade * springOutRev + (1.f - reverse.fade) * springOut;
            }
        }

        // Motor-stop fade: below the normal slowest detent, scale wet + spring
        // toward silence so motor-stop fades out (and back in on release) rather
        // than just stalling on reverse-swept echoes. Unity in normal range.
        // motorGain is computed above, at the head-output writes.

        // Split output stage: keep dry and wet separate so they can clip
        // independently downstream. The intervening drive-compensation and
        // low-drive rolloff are linear, so applying them per-path and summing is
        // bit-identical to the old single-signal chain.
        float dryOut = dryDefeat ? 0.f : in;
        // Split wet compensation. The tape ECHO wet gets the drive-comp makeup
        // via the asymmetric-slewed echoMakeupSlew, so an Input pull-down
        // can't spike the still-hot delayed repeats. The SPRING wet doesn't
        // divide here — it was compensated at excitation instead.
        // Input-drives-feedback Part 2: once the loop is hot, relax the
        // driveComp output compression so driven self-osc reads loud.
        float effMakeup    = echoMakeupSlew + hotness * (1.f - echoMakeupSlew);
        // Head MIXING law, measured: the RE-201 averages its playback heads
        // rather than summing them (1/N, matching -6.0/-9.5 dB measured).
        // Physically the Mode selector switches heads onto a shared bus.
        // Done here rather than at wetSum so feedbackTap stays untouched.
        float echoWetOut   = (wet * echoVol) / (effMakeup * activeCount);
        float springWetOut = springOut * reverbVol * reverbActiveGate;
        float wetOut = (echoWetOut + springWetOut) * motorGain;

        // Partial inverse compensation on the dry path: residue =
        // drive^(1-tiltExp). Knob still tracks "drive amount" cleanly; the
        // residue is the volume swell riding along.
        dryOut /= driveComp;

        // Low-drive HF rolloff. One-pole LP, transparent at midpoint, closes
        // to tiltHFFlr Hz at the bottom — adds a "polite/pillowy" character
        // at low drive. Applied to dry and wet with separate states.
        float lowDriveAmt = clamp((1.f - drive) / (1.f - 1.f / tiltBase), 0.f, 1.f);
        if (lowDriveAmt > 0.001f) {
            float hfCutoff = 20000.f - lowDriveAmt * (20000.f - tiltHFFlr);
            float a = 1.f - std::exp(-2.f * float(M_PI) * hfCutoff / args.sampleRate);
            lowDriveLPState    += a * (dryOut - lowDriveLPState);    dryOut = lowDriveLPState;
            lowDriveLPStateWet += a * (wetOut - lowDriveLPStateWet); wetOut = lowDriveLPStateWet;
        } else {
            lowDriveLPState = dryOut;  lowDriveLPStateWet = wetOut;  // keep in sync (no pop)
        }


        // Output-stage artifacts (spring noise + hiss) — summed in after both
        // output clips, so neither path's limiter ducks them.
        float artifact = artifacts.process(spring.noiseFloor,
                                           !frozenDynamics && machineNoiseFloor,
                                           tapeAge, rate, in, args.sampleRate,
                                           motorGain, params[INPUT_LEVEL_PARAM].getValue(),
                                           inEnv, drive);

        // Split output soft-clip. Dry and wet clip independently
        // (unity-slope `C*tanh(x/C)`) so a loud dry can't duck the wet, and
        // self-oscillation keeps its warm character. Artifacts are added
        // post-clip, then a gentle final safety soft-bounds the sum.
        dryOut = dryClipStage.process(dryOut, OUT_CLIP_DRY, ecoMode);
        wetOut = wetClipStage.process(wetOut, OUT_CLIP_WET, ecoMode);
        float out = dryOut + wetOut + artifact;
        out = finalClipStage.process(out, OUT_CLIP_FINAL, ecoMode);

        // Output pad: pure final-output level trim (H 0 dB / M -6 dB / L -12 dB).
        out *= padOutGain;

        outputs[OUT_OUTPUT].setVoltage(out);
    }
};

// Panel widgets and the GUI layer live in their own file.
// Out-of-class definitions for the three static constexpr members that are
// ODR-used (std::min/std::max bind by const T&). C++11/14 require a
// definition somewhere; -O1+ folds the reference away and links without
// these, but any -O0/sanitizer build fails to link without them.
constexpr float TapeEcho::QUIET_EXPAND_MAX_DB;
constexpr float TapeEcho::LOOP_BRIGHTEN_MAX_HZ;
constexpr float TapeEcho::FB_LOOP_LOSS_RELIEF_MAX_DB;

#include "TapeEchoWidget.hpp"

Model* modelTapeEcho = createModel<TapeEcho, TapeEchoWidget>("TapeEcho");