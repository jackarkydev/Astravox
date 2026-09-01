// SPDX-License-Identifier: GPL-3.0-only

// TapeEchoHysteresis.hpp — the Chowdhury magnetic hysteresis model, adapted
// from jatinchowdhury18/AnalogTapeModel (GPLv3). NOT a standalone header:
// #included from inside TapeEcho.cpp, relying on plugin.hpp above it.
// `fastTanh` lives here since the saturation stages need it visible too, so
// this header must be included before them.

// =============================================================================
// Hysteresis model — adapted from jatinchowdhury18/AnalogTapeModel (GPLv3).
// =============================================================================
namespace Hyst {

// All state and computation in float — tanhf is 2-3× faster than tanh on ARM,
// and 6 significant digits is more than sufficient for 8× oversampled audio.
static constexpr float ALPHA        = 1.6e-3f;
static constexpr float ONE_THIRD    = 1.0f / 3.0f;
static constexpr float NEG2_OVER_15 = -2.0f / 15.0f;

// Padé [6/7] rational approximation for tanh — replaces tanhf() in the inner loop.
// Max error < 2e-5 for |x| < 4.5; saturates to ±1 beyond that.
// Uses only multiply/add/divide — ~5-8 cycles vs ~30 for tanhf on ARM.
static inline float fastTanh(float x) {
    if (x >  4.5f) return  1.f;
    if (x < -4.5f) return -1.f;
    const float x2 = x * x;
    return x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)))
             / (135135.f + x2 * (62370.f + x2 * (3150.f + x2 * 28.f)));
}

struct State {
    // solver history (last_dMdt = previous-sample dMdt, needed for trapezoidal step)
    float M_n1 = 0.f, H_n1 = 0.f, H_d_n1 = 0.f, last_dMdt = 0.f;

    // model parameters (set by cook())
    float M_s = 1.25f, a = 0.2080f, c = 0.6971f, k = 0.47875f;
    float nc, M_s_oa, M_s_oa_talpha, M_s_oa_tc, M_s_oa_tc_talpha;
    float M_s_oaSq_tc_talpha, M_s_oaSq_tc_talphaSq;
    float upperLim = 20.f;

    // per-sample temporaries (written by hysteresisFunc, read by hysteresisFuncPrime)
    float Q, M_diff, L_prime, kap1, f1Denom, f1, f2, f3;
    float coth, oneOverQ, oneOverQSq, oneOverQCubed, cothSq;
    float oneOverF1Denom, oneOverF3;
    bool nearZero;

    void cook(float drive, float sat, float width) {
        M_s = 0.5f + 1.5f * (1.f - sat);
        a   = M_s / (0.01f + 6.f * drive);
        c   = std::sqrt(std::max(0.f, 1.f - width)) - 0.01f;
        k   = 0.47875f;
        nc                     = 1.f - c;
        M_s_oa                 = M_s / a;
        M_s_oa_talpha          = ALPHA * M_s_oa;
        M_s_oa_tc              = c * M_s_oa;
        M_s_oa_tc_talpha       = ALPHA * M_s_oa_tc;
        M_s_oaSq_tc_talpha     = M_s_oa_tc_talpha / a;
        M_s_oaSq_tc_talphaSq   = ALPHA * M_s_oaSq_tc_talpha;
        upperLim               = 20.f;
    }

    void reset() { M_n1 = H_n1 = H_d_n1 = last_dMdt = 0.f; }
};

static inline float langevin(State& hp) {
    return hp.nearZero ? hp.Q * ONE_THIRD : hp.coth - hp.oneOverQ;
}
static inline float langevinD(State& hp) {
    return hp.nearZero ? ONE_THIRD : hp.oneOverQSq - hp.cothSq + 1.f;
}
static inline float langevinD2(State& hp) {
    return hp.nearZero ? NEG2_OVER_15 * hp.Q
                       : 2.f * hp.coth * (hp.cothSq - 1.f) - 2.f * hp.oneOverQCubed;
}

static inline float deriv(float x_n, float x_n1, float x_d_n1, float T) {
    constexpr float dA = 0.75f;
    return ((1.f + dA) / T) * (x_n - x_n1) - dA * x_d_n1;
}

static inline float hysteresisFunc(float M, float H, float H_d, State& hp) {
    hp.Q             = (H + M * ALPHA) / hp.a;
    hp.oneOverQ      = 1.f / hp.Q;
    hp.oneOverQSq    = hp.oneOverQ  * hp.oneOverQ;
    hp.oneOverQCubed = hp.oneOverQ  * hp.oneOverQSq;
    hp.coth          = 1.f / fastTanh(hp.Q);
    hp.nearZero      = (hp.Q > -0.001f && hp.Q < 0.001f);
    hp.cothSq        = hp.coth * hp.coth;
    hp.M_diff        = langevin(hp) * hp.M_s - M;

    const float delta   = (H_d >= 0.f) ? 1.f : -1.f;
    const int sgnDelta  = (delta     > 0.f) ? 1 : -1;
    const int sgnMdiff  = (hp.M_diff > 0.f) ? 1 : (hp.M_diff < 0.f) ? -1 : 0;
    hp.kap1 = hp.nc * (float)(sgnDelta == sgnMdiff ? 1 : 0);

    hp.L_prime        = langevinD(hp);
    hp.f1Denom        = hp.nc * delta * hp.k - ALPHA * hp.M_diff;
    hp.oneOverF1Denom = 1.f / hp.f1Denom;
    hp.f1             = hp.kap1 * hp.M_diff * hp.oneOverF1Denom;
    hp.f2             = hp.L_prime * hp.M_s_oa_tc;
    hp.f3             = 1.f - hp.L_prime * hp.M_s_oa_tc_talpha;
    hp.oneOverF3      = 1.f / hp.f3;

    return H_d * (hp.f1 + hp.f2) * hp.oneOverF3;
}

static inline float hysteresisFuncPrime(float H_d, float dMdt, State& hp) {
    const float L_prime2 = langevinD2(hp);
    const float M_diff2  = hp.L_prime * hp.M_s_oa_talpha - 1.f;

    float f1_p = M_diff2 * hp.oneOverF1Denom;
    f1_p += hp.M_diff * ALPHA * M_diff2 * (hp.oneOverF1Denom * hp.oneOverF1Denom);
    f1_p *= hp.kap1;
    const float f2_p = L_prime2 * hp.M_s_oaSq_tc_talpha;
    const float f3_p = L_prime2 * (-hp.M_s_oaSq_tc_talphaSq);

    return (H_d * (f1_p + f2_p) - dMdt * f3_p) * hp.oneOverF3;
}

// Newton-Raphson solver (4 iterations) — trapezoidal integration with the Chowdhury
// under-relaxation factor (Talpha = T/1.9 instead of T/2). last_dMdt is the converged
// dMdt of the previous sample (reused from the last NR iteration — no extra call).
static float processSample(float H, float T, State& hp) {
    const float Talpha = T * (1.f / 1.9f);
    float H_d = deriv(H, hp.H_n1, hp.H_d_n1, T);

    // Idle gate: when H and H_d are essentially zero, dMdt ≡ 0 in the
    // Chowdhury formulation, so the NR loop would converge to M_n1 in one
    // iteration anyway — skip it. Post-signal M_n1 drain (τ=100ms, sub-audio)
    // lets residual DC drain instead of circulating through feedback forever.
    hp.M_n1 *= (1.f - T * 10.f);
    if (std::abs(H) < 1e-12f && std::abs(H_d) < 1e-12f) {
        hp.H_n1      = H;
        hp.H_d_n1    = H_d;
        hp.last_dMdt = 0.f;
        return hp.M_n1;
    }

    float M = hp.M_n1;
    float dMdt = 0.f;
    for (int i = 0; i < 4; ++i) {
        dMdt              = hysteresisFunc(M, H, H_d, hp);
        float dMdtPrime   = hysteresisFuncPrime(H_d, dMdt, hp);
        // The Newton denominator has no natural floor, so flooring it bounds
        // the step instead of relying on the guard below to clean up after.
        float den         = 1.f - Talpha * dMdtPrime;
        if (std::fabs(den) < 1e-6f) den = std::copysign(1e-6f, den);
        float deltaNR     = (M - hp.M_n1 - Talpha * (dMdt + hp.last_dMdt)) / den;
        M -= deltaNR;
        if (std::abs(deltaNR) < 1e-5f) break;
    }

    // Must be symmetric, not one-sided: a large finite negative M would pass
    // an asymmetric check through and let hysteresisFunc's f1Denom cross zero,
    // diverging instead of recovering.
    if (!std::isfinite(M) || std::fabs(M) > hp.upperLim) { M = 0.f; H_d = 0.f; dMdt = 0.f; }

    hp.M_n1      = M;
    hp.H_n1      = H;
    hp.H_d_n1    = H_d;
    hp.last_dMdt = dMdt;
    return M;
}

} // namespace Hyst