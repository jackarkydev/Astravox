// SPDX-License-Identifier: GPL-3.0-only

// TapeEchoFilters.hpp — per-head EQ, and the biquad/shelf primitives.
//
// NOT a standalone header: #included from inside TapeEcho.cpp, relying on
// plugin.hpp above it. The head EQ is speed-coupled — Repeat Rate moves the
// head bump and HF rolloff together with the delay time.
//
// `Biquad` is the shared transposed-DF2 base for midColor/lowColor/
// firstPassShelf. Deliberately not rack::dsp::TBiquadFilter (Direct Form I) —
// same transfer function, different last-bit rounding.

// =============================================================================
// Per-head EQ: head-bump peaking biquad + HF rolloff, speed-coupled
// =============================================================================
struct HeadEQ {
    struct BiquadCoefs { float b0, b1, b2, a1, a2; };
    struct DetentParams { float bumpFreq, bumpGain, bumpQ, hfCutoff; };

    DetentParams slow, med, fast;
    BiquadCoefs peakSlow, peakMed, peakFast;
    BiquadCoefs hfSlow,   hfMed,   hfFast;
    BiquadCoefs peakNow,  hfNow;

    float peakZ1 = 0.f, peakZ2 = 0.f;
    float hfZ1   = 0.f;

    void resetState() { peakZ1 = peakZ2 = hfZ1 = 0.f; }

    void computeCoefs(float sr, const DetentParams& p, BiquadCoefs& peak, BiquadCoefs& hf) {
        float A     = std::pow(10.f, p.bumpGain / 40.f);
        float w0    = 2.f * float(M_PI) * p.bumpFreq / sr;
        float cosw  = std::cos(w0);
        float alpha = std::sin(w0) / (2.f * p.bumpQ);
        float inv   = 1.f / (1.f + alpha / A);
        peak.b0 =  (1.f + alpha * A) * inv;
        peak.b1 =  (-2.f * cosw)     * inv;
        peak.b2 =  (1.f - alpha * A) * inv;
        peak.a1 =  (-2.f * cosw)     * inv;
        peak.a2 =  (1.f - alpha / A) * inv;

        float k    = std::tan(float(M_PI) * p.hfCutoff / sr);
        float inv2 = 1.f / (1.f + k);
        hf.b0 =  k * inv2;
        hf.b1 =  k * inv2;
        hf.b2 =  0.f;
        hf.a1 = (k - 1.f) * inv2;
        hf.a2 =  0.f;
    }

    void onSampleRateChange(float sr) {
        computeCoefs(sr, slow, peakSlow, hfSlow);
        computeCoefs(sr, med,  peakMed,  hfMed);
        computeCoefs(sr, fast, peakFast, hfFast);
        peakNow = peakSlow;
        hfNow   = hfSlow;
    }

    void interpolateCoefs(float sr, float tapeSpeed, float hfScale = 1.f) {
        // Session 3 head EQ was captured at the same three physical knob
        // positions as previous sessions — anchor to their measured tape speeds.
        static const float logSlow = std::log(SPEED_SLOW);
        static const float logMed  = std::log(SPEED_MED);
        static const float logFast = std::log(SPEED_FAST);
        float logSpeed = std::log(std::max(0.1f, tapeSpeed));
        float t;
        const BiquadCoefs* pa; const BiquadCoefs* pb;
        const DetentParams* da; const DetentParams* db;
        if (logSpeed <= logMed) {
            t = (logSpeed - logSlow) / (logMed - logSlow);
            pa = &peakSlow; pb = &peakMed;
            da = &slow;     db = &med;
        } else {
            t = (logSpeed - logMed) / (logFast - logMed);
            pa = &peakMed; pb = &peakFast;
            da = &med;     db = &fast;
        }
        t = std::max(0.f, std::min(1.f, t));
        auto lerp = [t](float a, float b) { return a + t * (b - a); };
        // Head bump (peaking biquad): interp precomputed coefs — unaffected by age.
        peakNow.b0 = lerp(pa->b0, pb->b0); peakNow.b1 = lerp(pa->b1, pb->b1);
        peakNow.b2 = lerp(pa->b2, pb->b2); peakNow.a1 = lerp(pa->a1, pb->a1);
        peakNow.a2 = lerp(pa->a2, pb->a2);
        // HF rolloff: interpolate cutoff in log-space, apply age scaling,
        // then derive 1-pole LP biquad. Lets Stage 8A scale cutoff at runtime.
        float logCutoff = std::log(da->hfCutoff)
                        + t * (std::log(db->hfCutoff) - std::log(da->hfCutoff));
        float cutoff    = std::exp(logCutoff) * hfScale;
        cutoff          = std::max(20.f, std::min(0.45f * sr, cutoff));
        float k         = std::tan(float(M_PI) * cutoff / sr);
        float inv2      = 1.f / (1.f + k);
        hfNow.b0 = k * inv2;
        hfNow.b1 = k * inv2;
        hfNow.b2 = 0.f;
        hfNow.a1 = (k - 1.f) * inv2;
        hfNow.a2 = 0.f;
    }

    float process(float in) {
        float y = peakNow.b0 * in + peakZ1;
        peakZ1  = peakNow.b1 * in - peakNow.a1 * y + peakZ2;
        peakZ2  = peakNow.b2 * in - peakNow.a2 * y;
        float x = y;
        y       = hfNow.b0 * x + hfZ1;
        hfZ1    = hfNow.b1 * x - hfNow.a1 * y;
        return y;
    }
};

// Global wet output EQ: 2nd-order RBJ shelving (Bass / Treble). Transposed
// direct-form II biquad, shared by every stage needing this topology.
struct Biquad {
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float z1 = 0.f, z2 = 0.f;

    // Clears filter memory without touching the coefficients — the coefficients
    // are derived from knob state and stay valid across a DSP reset.
    void resetState() { z1 = z2 = 0.f; }

    float process(float in) {
        float y = b0 * in + z1;
        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;
        return y;
    }
};

struct ShelfFilter : Biquad {

    void setShelf(float sr, float freq, float gainDb, bool isHigh) {
        float A    = std::pow(10.f, gainDb / 40.f);
        float w0   = 2.f * float(M_PI) * freq / sr;
        float cosw = std::cos(w0);
        float sqA  = std::sqrt(A);
        float alpha = std::sin(w0) / 2.f * (sqA + 1.f / sqA);
        float raw_a0, raw_a1, raw_a2;
        if (!isHigh) {
            b0      =    A * ((A+1.f) - (A-1.f)*cosw + 2.f*sqA*alpha);
            b1      = 2.f*A * ((A-1.f) - (A+1.f)*cosw);
            b2      =    A * ((A+1.f) - (A-1.f)*cosw - 2.f*sqA*alpha);
            raw_a0  =       (A+1.f) + (A-1.f)*cosw + 2.f*sqA*alpha;
            raw_a1  = -2.f*((A-1.f) + (A+1.f)*cosw);
            raw_a2  =       (A+1.f) + (A-1.f)*cosw - 2.f*sqA*alpha;
        } else {
            b0      =    A * ((A+1.f) + (A-1.f)*cosw + 2.f*sqA*alpha);
            b1      =-2.f*A * ((A-1.f) + (A+1.f)*cosw);
            b2      =    A * ((A+1.f) + (A-1.f)*cosw - 2.f*sqA*alpha);
            raw_a0  =       (A+1.f) - (A-1.f)*cosw + 2.f*sqA*alpha;
            raw_a1  =  2.f*((A-1.f) - (A+1.f)*cosw);
            raw_a2  =       (A+1.f) - (A-1.f)*cosw - 2.f*sqA*alpha;
        }
        float inv = 1.f / raw_a0;
        b0 *= inv; b1 *= inv; b2 *= inv;
        a1  = raw_a1 * inv; a2 = raw_a2 * inv;
    }

};