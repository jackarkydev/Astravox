// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <dsp/fft.hpp>
#include <array>
#include <vector>
#include <algorithm>
#include <cstring>

// Uniformly-partitioned overlap-save FFT convolver (Gardner-style UPOLS),
// morphing across NUM_LEVELS measured spring IRs. Morph blends the two
// neighboring levels' partition sets via pffft's scaling argument as an exact
// linear crossfade, within a single convolver runtime — no added latency.
// RT-safe: buffers sized in prepare(); process() only does FFTs/MACs/index math.
struct SpringConvolver {
    static constexpr int NUM_LEVELS = 6;

    int P = 0;   // partition size / hop (samples)
    int N = 0;   // FFT size = 2*P
    int K = 0;   // number of IR partitions (same for every level — IRs share length)

    rack::dsp::RealFFT* fft = nullptr;

    // NUM_LEVELS * K * 2N: forward-FFT'd, zero-padded IR partitions, one set per level.
    // Indexed as irSpectra[(level*K + k) * 2N ...].
    std::vector<float> irSpectra;
    std::vector<float> fdl;         // K * 2N: frequency-domain delay line (ring buffer)
    int fdlWriteIndex = 0;          // index of the newest FDL slot

    std::vector<float> inputWindow; // N: sliding [prev P | curr P] time-domain window
    int inputAccumCount = 0;        // samples written into the "curr P" half so far

    std::vector<float> freqAccum;   // 2N: scratch accumulator for zconvolve_accumulate
    std::vector<float> timeAccum;   // N: scratch inverse-FFT output

    std::vector<float> outputFIFO;  // P: this block's output samples, read out one per sample
    int outputReadIndex = 0;

    float currentMorph = 0.f;       // latched once per block in blockStep()

    bool ready = false;

    ~SpringConvolver() {
        delete fft;
    }

    // Loads NUM_LEVELS mono IRs (all must share the same length) and precomputes
    // every level's partition spectra. NOT real-time safe — call only from
    // module construction / onSampleRateChange().
    void prepare(const std::array<const float*, NUM_LEVELS>& irs, int irLength, int partitionSize) {
        delete fft;
        fft = nullptr;
        ready = false;

        P = partitionSize;
        N = 2 * P;
        K = (irLength + P - 1) / P;
        if (K < 1) K = 1;

        fft = new rack::dsp::RealFFT(N);

        irSpectra.assign((size_t)NUM_LEVELS * K * 2 * N, 0.f);
        fdl.assign((size_t)K * 2 * N, 0.f);
        inputWindow.assign(N, 0.f);
        freqAccum.assign(2 * N, 0.f);
        timeAccum.assign(N, 0.f);
        outputFIFO.assign(P, 0.f);

        std::vector<float> partitionTime(N, 0.f);
        for (int level = 0; level < NUM_LEVELS; level++) {
            const float* ir = irs[level];
            for (int k = 0; k < K; k++) {
                std::fill(partitionTime.begin(), partitionTime.end(), 0.f);
                int start = k * P;
                int count = std::min(P, irLength - start);
                if (count > 0)
                    std::memcpy(partitionTime.data(), ir + start, (size_t)count * sizeof(float));
                float* dst = &irSpectra[((size_t)level * K + k) * 2 * N];
                fft->rfftUnordered(partitionTime.data(), dst);
            }
        }

        fdlWriteIndex = 0;
        inputAccumCount = 0;
        outputReadIndex = 0;
        currentMorph = 0.f;
        ready = true;
    }

    // Clears all runtime state (ring buffers, sliding window) without re-running
    // the IR partitioning. Not RT-safe if called mid-stream on another thread,
    // but contains no allocation.
    void reset() {
        std::fill(fdl.begin(), fdl.end(), 0.f);
        std::fill(inputWindow.begin(), inputWindow.end(), 0.f);
        std::fill(outputFIFO.begin(), outputFIFO.end(), 0.f);
        fdlWriteIndex = 0;
        inputAccumCount = 0;
        outputReadIndex = 0;
    }

    // RT-safe: no allocation, only FFTs/MACs/index math. `morph` selects/
    // blends the two nearest IR levels (0=L1..5=L6), sampled once per block —
    // should already be smoothed upstream (see SpringLevelEnvelope).
    //
    // Read-before-refill ordering matters: the old block's last output sample
    // must be read before blockStep() overwrites outputFIFO with the new block.
    float process(float in, float morph) {
        if (!ready)
            return 0.f;

        float out = outputFIFO[outputReadIndex];

        currentMorph = morph;
        inputWindow[P + inputAccumCount] = in;
        inputAccumCount++;
        outputReadIndex++;

        if (inputAccumCount >= P) {
            inputAccumCount = 0;
            blockStep();
        }
        if (outputReadIndex >= P)
            outputReadIndex = 0;

        return out;
    }

private:
    void blockStep() {
        // Forward-FFT the full N-sample window (prev P | just-completed curr P)
        // into the newest FDL slot. One FDL, shared by every level.
        fdlWriteIndex++;
        if (fdlWriteIndex >= K)
            fdlWriteIndex = 0;
        fft->rfftUnordered(inputWindow.data(), &fdl[(size_t)fdlWriteIndex * 2 * N]);

        // Slide the window: curr P becomes prev P for the next block.
        std::memmove(inputWindow.data(), inputWindow.data() + P, (size_t)P * sizeof(float));

        // (min/max rather than std::clamp — the plugin builds with -std=c++11.)
        float m = std::min(std::max(currentMorph, 0.f), (float)(NUM_LEVELS - 1));
        int i0 = std::min(std::max((int)m, 0), NUM_LEVELS - 2);
        int i1 = i0 + 1;
        float frac = m - (float)i0;

        // Accumulate partition products, blended across the two nearest levels:
        // freqAccum = sum_k FDL[m-k] * (IR[i0][k]*(1-frac) + IR[i1][k]*frac).
        std::fill(freqAccum.begin(), freqAccum.end(), 0.f);
        for (int k = 0; k < K; k++) {
            int srcIndex = fdlWriteIndex - k;
            if (srcIndex < 0)
                srcIndex += K;
            const float* fdlSpec = &fdl[(size_t)srcIndex * 2 * N];

            pffft_zconvolve_accumulate(
                fft->setup, fdlSpec,
                &irSpectra[((size_t)i0 * K + k) * 2 * N],
                freqAccum.data(), 1.f - frac);

            if (frac > 0.f) {
                pffft_zconvolve_accumulate(
                    fft->setup, fdlSpec,
                    &irSpectra[((size_t)i1 * K + k) * 2 * N],
                    freqAccum.data(), frac);
            }
        }

        // Inverse FFT (unscaled: IFFT(FFT(x)) = N*x per pffft convention).
        fft->irfftUnordered(freqAccum.data(), timeAccum.data());
        float scale = 1.f / (float)N;
        for (int i = 0; i < N; i++)
            timeAccum[i] *= scale;

        // Overlap-save: the last P samples are the valid linear-convolution block.
        std::memcpy(outputFIFO.data(), timeAccum.data() + P, (size_t)P * sizeof(float));
    }
};