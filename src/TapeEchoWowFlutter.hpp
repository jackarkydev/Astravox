// SPDX-License-Identifier: GPL-3.0-only

// TapeEchoWowFlutter.hpp — the stochastic wow/flutter model.
//
// NOT a standalone header: #included from inside TapeEcho.cpp, relying on
// plugin.hpp above it and the file-scope xorshiftFloat() helper being visible.
// DRIFT_LEAK_TAU below is intentionally duplicated (separate scope) by
// TapeEcho's own tape-age drift code — not a constant to consolidate.
//
// Unit convention: capstanDepth/loopDepth/stochasticDepth are fractional
// tape-speed deviation (dimensionless), which must be integrated in time
// (varispeed model) to become a delay-line modulation — treating them as a
// direct fractional delay-time deviation produces runaway FM sidebands.
// `perHeadOffset[i]` is therefore an additive time offset in seconds.
struct WowFlutter {
    float capstanPhase = 0.0f;
    float capstanRate  = 1.5f;     // Hz
    float capstanDepth = 0.0005f;  // fractional speed-dev amplitude
    float loopPhase    = 0.0f;
    float loopRate     = 0.4f;     // Hz
    float loopDepth    = 0.001f;   // fractional speed-dev amplitude

    float noiseLPCutoff      = 25.0f;    // Hz (PSD bandwidth of stochastic)
    float stochasticDepth    = 0.0005f;  // scales LP-filtered xorshift RMS
    float perHeadCorrelation = 0.85f;

    // "Stretched tape" — a very slow speed wander, distinct from capstan/loop
    // wow (a healthy machine's behaviour, present at Mint): stretch is a
    // defect, so it scales on tapeAge directly rather than ageScale, which
    // would leave it audible on a fresh tape. Rate chosen against
    // DRIFT_LEAK_TAU so it isn't eaten by the leak.
    float stretchRate  = 0.07f;     // Hz
    float stretchDepth = 0.0022f;   // fractional speed dev at FULL age (backed off ~12.5%)
    float stretchPhase = 0.0f;

    // Leaky integrator state: per-head accumulated position offset (seconds).
    // Drift leak τ bounds DC; chosen far below loop rate (0.4 Hz) so wow is preserved.
    float positionDrift[3] = {0.0f, 0.0f, 0.0f};
    static constexpr float DRIFT_LEAK_TAU = 5.0f;  // seconds

    struct Biquad { float z1=0, z2=0, b0=0, b1=0, b2=0, a1=0, a2=0; };
    Biquad commonFilter;
    Biquad uncommonFilter[3];

    // High-frequency per-pass JITTER, added alongside the measured wow model
    // above, which is left untouched.
    //
    // ** UNITS: this one is SECONDS OF POSITION, applied directly — NOT a
    //    fractional speed deviation, and must NOT be integrated. ** The
    //    opposite convention from capstan/loop/stochasticDepth above.
    //
    // Why it exists: models the hardware's measured pass-to-pass decorrelation
    // (which makes real repeats blur into a "wash"), which stochasticDepth's
    // 41Hz cutoff can't reach — genuine flutter energy extends to ~1kHz.
    //
    // Spectrum: HP ~40Hz then two one-pole LPs at 8Hz/150Hz, approximating the
    // measured speed-deviation shape. Common-mode across heads. Shape is the
    // weaker part of this fit; magnitude is sized from the per-pass
    // correlation measurement, which is trusted more than the floor-corrected
    // band ratios used for shape.
    float jitterLp1Z = 0.f, jitterLp2Z = 0.f, jitterHpZ = 0.f;
    float jitterLp1Coef = 0.f, jitterLp2Coef = 0.f, jitterHpCoef = 0.f;
    float jitterNormGain = 0.f;      // makes the shaped noise unit-RMS
    static constexpr float JITTER_HP_HZ  = 40.f;
    static constexpr float JITTER_LP1_HZ = 8.f;
    static constexpr float JITTER_LP2_HZ = 150.f;

    void onSampleRateChange(float sr) {
        float wc   = 2.0f * M_PI * noiseLPCutoff / sr;
        float k    = std::tan(wc / 2.0f);
        float k2   = k * k;
        float norm = 1.0f / (1.0f + std::sqrt(2.0f) * k + k2);
        float b0   = k2 * norm;
        float b1   = 2.0f * b0;
        float b2   = b0;
        float a1   = 2.0f * (k2 - 1.0f) * norm;
        float a2   = (1.0f - std::sqrt(2.0f) * k + k2) * norm;
        auto setCoefs = [&](Biquad& f) {
            f.b0 = b0; f.b1 = b1; f.b2 = b2; f.a1 = a1; f.a2 = a2;
        };
        setCoefs(commonFilter);
        for (int i = 0; i < 3; i++) setCoefs(uncommonFilter[i]);

        // Jitter: HP then LP, normalised to unit RMS numerically (rather than
        // derived analytically) so the caller's target can be given directly
        // in seconds. Runs here, not in process() — off the audio thread.
        jitterLp1Coef = 1.f - std::exp(-2.f * float(M_PI) * JITTER_LP1_HZ / sr);
        jitterLp2Coef = 1.f - std::exp(-2.f * float(M_PI) * JITTER_LP2_HZ / sr);
        jitterHpCoef  = 1.f - std::exp(-2.f * float(M_PI) * JITTER_HP_HZ  / sr);
        {
            uint32_t probe = 0x1234567u;
            float lp1 = 0.f, lp2 = 0.f, hp = 0.f, acc = 0.f;
            const int N = 1 << 16;
            for (int i = 0; i < N; i++) {
                float w = xorshiftFloat(probe);
                hp  += jitterHpCoef * (w - hp);
                float band = w - hp;                    // HP output
                lp1 += jitterLp1Coef * (band - lp1);
                lp2 += jitterLp2Coef * (lp1  - lp2);
                if (i > (N >> 3)) acc += lp2 * lp2;     // skip settling
            }
            float rms = std::sqrt(acc / float(N - (N >> 3)));
            jitterNormGain = rms > 1e-12f ? 1.f / rms : 0.f;
        }
        jitterLp1Z = jitterLp2Z = jitterHpZ = 0.f;
    }

    // `bumpScale` multiplies only the irregular components (stochastic +
    // jitter), not the periodic capstan/loop sinusoids — worn-tape damage
    // should read as irregular mashed-up bumps, not a detuned pitch.
    void process(float sr, bool frozen, uint32_t& rngState, float perHeadOffset[3],
                 float ageScale = 1.0f, float jitterRmsSec = 0.0f,
                 float ageNorm = 0.0f, float bumpScale = 1.0f,
                 float wowScale = 1.0f) {
        if (frozen) {
            // Preserves the "frozen = deterministic" contract by taking an
            // early return, silencing the rng path entirely.
            perHeadOffset[0] = perHeadOffset[1] = perHeadOffset[2] = 0.0f;
            positionDrift[0] = positionDrift[1] = positionDrift[2] = 0.0f;
            jitterLp1Z = jitterLp2Z = jitterHpZ = 0.f;  // frozen = deterministic
            return;
        }
        capstanPhase += capstanRate / sr;
        if (capstanPhase >= 1.0f) capstanPhase -= 1.0f;
        loopPhase += loopRate / sr;
        if (loopPhase >= 1.0f) loopPhase -= 1.0f;
        stretchPhase += stretchRate / sr;
        if (stretchPhase >= 1.0f) stretchPhase -= 1.0f;
        // `wowScale` lifts only the periodic capstan/loop sinusoids, kept
        // separate from `bumpScale` so the two read as different kinds of damage.
        float periodicSpeed = ageScale * wowScale * (
              capstanDepth * std::sin(2.0f * M_PI * capstanPhase)
            + loopDepth    * std::sin(2.0f * M_PI * loopPhase))
            // Stretch rides OUTSIDE the ageScale product on purpose — see the
            // member comment. Zero at Mint, quadratic in age.
            + stretchDepth * ageNorm * ageNorm
              * std::sin(2.0f * M_PI * stretchPhase);

        auto biquad = [](Biquad& f, float in) {
            float out = f.b0 * in + f.z1;
            f.z1 = f.b1 * in - f.a1 * out + f.z2;
            f.z2 = f.b2 * in - f.a2 * out;
            return out;
        };
        float commonNoise = biquad(commonFilter, xorshiftFloat(rngState));

        float c  = perHeadCorrelation;
        float ic = std::sqrt(std::max(0.0f, 1.0f - c * c));
        float dt = 1.0f / sr;
        float leakCoef = dt / DRIFT_LEAK_TAU;
        for (int i = 0; i < 3; i++) {
            float uncommonNoise = biquad(uncommonFilter[i], xorshiftFloat(rngState));
            float headStochastic = c * commonNoise + ic * uncommonNoise;
            // Fractional tape-speed deviation for this head (dimensionless).
            float speedDev = periodicSpeed
                           + ageScale * bumpScale * stochasticDepth * headStochastic;
            // Varispeed integrator: faster tape → shorter delay, so
            // d(positionDrift)/dt = −speedDev. Leak keeps DC bounded.
            positionDrift[i] += -speedDev * dt - positionDrift[i] * leakCoef;
            perHeadOffset[i] = positionDrift[i];  // seconds, additive
        }
        // Jitter — added directly in seconds, NOT integrated (see the units
        // note above). Common-mode across heads.
        {
            float w = xorshiftFloat(rngState);
            jitterHpZ += jitterHpCoef * (w - jitterHpZ);
            float band = w - jitterHpZ;
            jitterLp1Z += jitterLp1Coef * (band      - jitterLp1Z);
            jitterLp2Z += jitterLp2Coef * (jitterLp1Z - jitterLp2Z);
            float j = jitterLp2Z * jitterNormGain * jitterRmsSec * ageScale * bumpScale;
            for (int i = 0; i < 3; i++) perHeadOffset[i] += j;
        }
    }
};