// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <dsp/common.hpp>
#include <cmath>
#include <algorithm>

// Envelope follower + dB-calibrated level-morph mapping, plus an optional
// input drive, feeding SpringConvolver's `morph` parameter. Pipeline: input
// -> envelope follower -> dBFS -> piecewise-linear map through six measured
// calibration points -> one-pole smoothing -> morph position in [0, 5].
struct SpringLevelEnvelope {
    static constexpr int NUM_LEVELS = 6;

    // Measured drive levels (dBFS) for L1..L6 — see condition_ir_bank.py's
    // sweep_params / res/tape_echo_spring_irs.json. Level i calibrates to
    // morph position i (L1 -> 0 .. L6 -> 5).
    static constexpr float CALIBRATION_DBFS[NUM_LEVELS] = {
        -30.0f, -20.0f, -12.0f, -6.0f, -3.0f, -0.1f
    };

    static constexpr float ENV_ATK_TAU = 0.005f;  // s — fast attack, catches transients
    static constexpr float ENV_REL_TAU = 0.150f;  // s — slower release, avoids chatter
    // Asymmetric morph smoothing. Fast up so character tightens instantly on
    // a hit; slow down so it relaxes over the tail — prevents an unphysical
    // reverb bloom on a fast hi->low input drop. By-ear tunable.
    static constexpr float MORPH_UP_TAU   = 0.030f;  // s — fast tighten
    static constexpr float MORPH_DOWN_TAU = 1.500f;  // s — slow relax

    float env         = 0.f;  // linear amplitude envelope
    float envAtkCoef  = 0.f;
    float envRelCoef  = 0.f;

    float morph         = 0.f;  // smoothed morph position — feed to SpringConvolver::process()
    float morphUpCoef   = 0.f;
    float morphDownCoef = 0.f;

    void onSampleRateChange(float sr) {
        auto coef = [sr](float tau) -> float {
            return 1.f - std::exp(-1.f / (tau * sr));
        };
        envAtkCoef    = coef(ENV_ATK_TAU);
        envRelCoef    = coef(ENV_REL_TAU);
        morphUpCoef   = coef(MORPH_UP_TAU);
        morphDownCoef = coef(MORPH_DOWN_TAU);
    }

    // Converts a dBFS level to a morph position via piecewise-linear
    // interpolation through the six calibration points. Clamps (flat) beyond
    // the L1/L6 endpoints — including for -inf (true silence).
    static float dbToMorph(float db) {
        if (db <= CALIBRATION_DBFS[0])
            return 0.f;
        if (db >= CALIBRATION_DBFS[NUM_LEVELS - 1])
            return (float)(NUM_LEVELS - 1);
        for (int i = 0; i < NUM_LEVELS - 1; i++) {
            if (db <= CALIBRATION_DBFS[i + 1]) {
                float lo = CALIBRATION_DBFS[i];
                float hi = CALIBRATION_DBFS[i + 1];
                float frac = (db - lo) / (hi - lo);
                return (float)i + frac;
            }
        }
        return (float)(NUM_LEVELS - 1);
    }

    // RT-safe. Feed the (pre-drive) spring input signal each sample; returns
    // the smoothed morph position to pass to SpringConvolver::process().
    float process(float in) {
        float target = std::fabs(in);
        env += (target > env ? envAtkCoef : envRelCoef) * (target - env);

        float db = rack::dsp::amplitudeToDb(env);
        float targetMorph = dbToMorph(db);
        float mc = (targetMorph > morph) ? morphUpCoef : morphDownCoef;
        morph += mc * (targetMorph - morph);
        return morph;
    }

    void reset() {
        env = 0.f;
        morph = 0.f;
    }
};

// Optional light input drive: self-normalizing tanh soft-clip
// (tanh(g*x)/g -> x for small x, so it's unity gain at low levels and only
// engages as the signal approaches the knee). `amount` in [0,1]; 0 bypasses
// entirely (identity, no tanh call). g ranges 1x (amount=0+) .. 3x (amount=1).
inline float springDrive(float in, float amount) {
    if (amount <= 0.f)
        return in;
    float g = 1.f + amount * 2.f;
    return std::tanh(g * in) / g;
}