// SPDX-License-Identifier: GPL-3.0-only

// TapeEchoSaturation.hpp — the oversampled nonlinear stages.
//
// NOT a standalone header: #included from inside TapeEcho.cpp, relying on
// plugin.hpp and TapeEchoHysteresis.hpp above it (Hyst::fastTanh must
// already be visible). Governing rule: any hard nonlinearity gets an
// oversampling boundary (8x normally, 4x in Eco mode) — OversampledStage
// owns that boilerplate so each stage isn't a verbatim copy.

// Measured input-preamp drive character. y = tanh(driveScale·x) / tanh(driveScale),
// x normalized to VCV ±5V = ±1 model units. Applied only to the tape-bound
// signal — a separate, preceding nonlinearity from the Chowdhury
// tape-hysteresis saturator (SatPath/Hyst above), which models the tape itself.
static constexpr float INPUT_DRIVE_SCALE = 0.9302f;
// Derived with the same function the stage uses (Hyst::fastTanh), so the
// approximation's error cancels instead of landing as a gain offset.
static const     float INPUT_DRIVE_RECIP = 1.f / Hyst::fastTanh(INPUT_DRIVE_SCALE);

// The oversampling boundary every hard nonlinearity needs, factored out so
// each stage doesn't carry its own verbatim copy. `run()` is a template so
// the shaping expression inlines like a hand-written loop.
struct OversampledStage {
    rack::dsp::Upsampler<8, 8> up8;
    rack::dsp::Decimator<8, 8> dec8;
    rack::dsp::Upsampler<4, 8> up4;
    rack::dsp::Decimator<4, 8> dec4;

    void reset() {
        up8.reset(); dec8.reset();
        up4.reset(); dec4.reset();
    }

    // Eco toggle: reset only the set about to become active, avoiding a click
    // without disturbing the set left behind. RT-safe, callable from process().
    void resetForOversampleChange(bool ecoNowActive) {
        if (ecoNowActive) { up4.reset(); dec4.reset(); }
        else              { up8.reset(); dec8.reset(); }
    }

    template <typename Shape>
    float run(float in, bool ecoMode, Shape shape) {
        if (ecoMode) {
            float buf[4];
            up4.process(in, buf);
            for (int i = 0; i < 4; ++i) buf[i] = shape(buf[i]);
            return dec4.process(buf);
        } else {
            float buf[8];
            up8.process(in, buf);
            for (int i = 0; i < 8; ++i) buf[i] = shape(buf[i]);
            return dec8.process(buf);
        }
    }
};

// Bundles hysteresis state with upsampler/decimator pairs for one saturation path.
struct SatPath : OversampledStage {
    Hyst::State state;

    float inScale   = 0.2f;   // 1/5 — brings VCV ±5 V into ±1 model units
    float outGain   = 2.5f;   // unity small-signal gain = 5 * (3a/M_s)
    float clampLim  = 1.5f;   // eco-mode magnetization clamp

    // Asymmetric warmth: a level-tracked DC bias shifts the signal onto an
    // asymmetric part of the hysteresis curve, adding even-harmonic warmth and
    // taming harsh upper odd harmonics. DC removed downstream by the write-path blocker.
    float biasEnv   = 0.f;
    float asymBias  = 0.20f;  // even-harmonic warmth amount (0 = symmetric)
    static constexpr float BIAS_ENV_TAU = 0.015f;  // s — bias envelope follower

    void cook(float drive, float sat, float width) {
        state.cook(drive, sat, width);
        // unity small-signal gain: outGain = 1 / (inScale * M_s/(3*a)) = 15*a/M_s
        outGain = 15.f * state.a / state.M_s;
    }

    void resetFilters() { OversampledStage::reset(); }
    // biasEnv is recursive state — must be cleared alongside the solver, or a
    // stray NaN poisons every subsequent sample silently.
    void resetState() { state.reset(); biasEnv = 0.f; }
    void reset()      { resetFilters(); resetState(); }

    float process(float in, bool ecoMode, float T8, float T4) {
        float H = in * inScale;
        if (ecoMode) {
            const float envCoef = T4 / BIAS_ENV_TAU;
            float buf[4];
            up4.process(H, buf);
            for (int i = 0; i < 4; ++i) {
                biasEnv += envCoef * (std::fabs(buf[i]) - biasEnv);
                float M = Hyst::processSample(buf[i] + asymBias * biasEnv, T4, state);
                M = std::max(-clampLim, std::min(clampLim, M));
                state.M_n1 = M;
                buf[i] = M;
            }
            return dec4.process(buf) * outGain;
        } else {
            const float envCoef = T8 / BIAS_ENV_TAU;
            float buf[8];
            up8.process(H, buf);
            for (int i = 0; i < 8; ++i) {
                biasEnv += envCoef * (std::fabs(buf[i]) - biasEnv);
                buf[i] = Hyst::processSample(buf[i] + asymBias * biasEnv, T8, state);
            }
            return dec8.process(buf) * outGain;
        }
    }
};

// Oversampled input-preamp drive stage. Plain measured tanh, no
// hysteresis/memory (unlike SatPath/Hyst above, which models the tape itself)
// — but still a hard nonlinearity, so it aliases without oversampling just
// like Chowdhury does: any nonlinear stage needs an oversampling boundary,
// not just the hysteresis model.
struct InputDriveStage : OversampledStage {

    float process(float in, bool ecoMode) {
        float x = in * 0.2f;  // VCV ±5V -> ±1 model units (SatPath::inScale convention)
        return run(x, ecoMode, [](float v) {
            return Hyst::fastTanh(INPUT_DRIVE_SCALE * v) * INPUT_DRIVE_RECIP;
        }) * 5.f;
    }
};

// Oversampled high-drive wet-path clip — same class of hard nonlinearity as
// InputDriveStage. `k` varies only at knob-rate, safe to treat as constant
// across one oversampled block.
struct WetDriveClip : OversampledStage {

    float process(float in, float k, bool ecoMode) {
        return run(in, ecoMode, [k](float v) {
            return (5.f / k) * Hyst::fastTanh(v * k * 0.2f);
        });
    }
};

// Oversampled output soft-clip — same class of nonlinearity as InputDriveStage/
// WetDriveClip. All three instances sit right before setVoltage.
struct OutputClipStage : OversampledStage {

    float process(float in, float ceiling, bool ecoMode) {
        return run(in, ecoMode, [ceiling](float v) {
            return ceiling * Hyst::fastTanh(v / ceiling);
        });
    }
};