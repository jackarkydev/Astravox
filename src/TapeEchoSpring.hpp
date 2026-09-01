// SPDX-License-Identifier: GPL-3.0-only

// TapeEchoSpring.hpp — the spring reverb: partitioned convolution over the six
// measured drive-level IRs, plus the process-wide resampled-IR cache.
//
// NOT a standalone header: #included from inside TapeEcho.cpp, relying on
// plugin.hpp, SpringConvolver.hpp and SpringLevelEnvelope.hpp above it.
//
// Two things here are load-bearing and easy to undo by accident:
//  - The IR cache is keyed by rate and shared by shared_ptr across instances,
//    since resampling six IRs is too costly to pay per instance.
//  - Reclamation is epoch-guarded, not a plain pointer swap + deferred free —
//    a reader on the audio thread can be mid-read when the GUI thread
//    rebuilds the cache (`make -C tools/regress stress` exercises this).
//
// A dispersion-allpass + two-path FDN approach was tried and retired: a small
// FDN produces sparse modes that ring tonally, while the real RE-201 spring is
// a dense, noise-like wash. Pure IR convolution matched the target by ear.
//
// Engine: uniformly-partitioned FFT convolution of six conditioned IRs
// (48kHz, 7s, equal-energy normalized). The input envelope morphs across
// L1(soft/long) → L6(hard/tight); IRs resample to engine SR on SR change.
// Latency = one partition (10.7ms @ 96k), inaudible on a parallel wet send.

// Minimal WAV reader — float32 / int16 / int24 PCM mono. Returns true on success.
// Used once at module construction; not RT-safe. If `outSR` is non-null it
// receives the file's declared sample rate.
static bool loadWavMono(const std::string& path, std::vector<float>& out, uint32_t* outSR = nullptr) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    auto rdU32 = [](FILE* fh) -> uint32_t {
        uint8_t b[4]; if (std::fread(b, 1, 4, fh) != 4) return 0;
        return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
    };
    auto rdU16 = [](FILE* fh) -> uint16_t {
        uint8_t b[2]; if (std::fread(b, 1, 2, fh) != 2) return 0;
        return (uint16_t)b[0] | ((uint16_t)b[1]<<8);
    };

    // Every size below is bounded against the real file length BEFORE
    // anything is allocated — otherwise a corrupt `data` header declaring a
    // huge size could throw bad_alloc uncaught out of the constructor.
    std::fseek(f, 0, SEEK_END);
    const long fileLen = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fileLen < 12) { std::fclose(f); return false; }

    char riff[4]; if (std::fread(riff, 1, 4, f) != 4 || std::memcmp(riff, "RIFF", 4)) { std::fclose(f); return false; }
    rdU32(f); // file size
    char wave[4]; if (std::fread(wave, 1, 4, f) != 4 || std::memcmp(wave, "WAVE", 4)) { std::fclose(f); return false; }

    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    std::vector<uint8_t> dataBytes;
    while (!std::feof(f)) {
        char id[4]; if (std::fread(id, 1, 4, f) != 4) break;
        uint32_t sz = rdU32(f);
        // A chunk cannot be longer than what remains of the file.
        const long here = std::ftell(f);
        if (here < 0 || (uint64_t)sz > (uint64_t)(fileLen - here)) { std::fclose(f); return false; }
        // RIFF pads odd-length chunks to even; not skipping it desynchronised the
        // walk on any odd-sized LIST/INFO chunk before `data`.
        const long advance = (long)sz + (sz & 1);
        if (!std::memcmp(id, "fmt ", 4)) {
            if (sz < 16) { std::fclose(f); return false; }   // was read unconditionally
            audioFormat   = rdU16(f);
            numChannels   = rdU16(f);
            sampleRate    = rdU32(f);
            rdU32(f); // byte rate
            rdU16(f); // block align
            bitsPerSample = rdU16(f);
            std::fseek(f, here + advance, SEEK_SET);
        } else if (!std::memcmp(id, "data", 4)) {
            dataBytes.resize(sz);
            if (std::fread(dataBytes.data(), 1, sz, f) != sz) { std::fclose(f); return false; }
            break;
        } else {
            std::fseek(f, here + advance, SEEK_SET);
        }
    }
    std::fclose(f);
    if (numChannels == 0 || dataBytes.empty()) return false;
    if (outSR) *outSR = sampleRate;

    // Reject the format before sizing `out`, so a rejected file doesn't
    // allocate up to 4x its declared chunk size first.
    const bool supported = (audioFormat == 3 && bitsPerSample == 32)
                        || (audioFormat == 1 && (bitsPerSample == 16 || bitsPerSample == 24));
    if (!supported) return false;

    int bytesPerSample = bitsPerSample / 8;
    int frameBytes = bytesPerSample * numChannels;
    if (frameBytes == 0) return false;
    size_t frameCount = dataBytes.size() / (size_t)frameBytes;
    // Bounded rather than trusted: ~350s at 96kHz is absurdly generous for an
    // impulse response, keeping the worst case at 128 MiB instead of a
    // multi-gigabyte allocation.
    static const size_t MAX_FRAMES = 1u << 25;   // 33,554,432
    if (frameCount == 0 || frameCount > MAX_FRAMES) return false;
    int numFrames = (int)frameCount;
    out.assign(numFrames, 0.f);

    const uint8_t* p = dataBytes.data();
    if (audioFormat == 3 && bitsPerSample == 32) { // IEEE float
        for (int i = 0; i < numFrames; i++) {
            float s; std::memcpy(&s, p + (size_t)i * (size_t)frameBytes, 4); // first channel
            out[i] = s;
        }
    } else if (audioFormat == 1 && bitsPerSample == 16) { // PCM 16
        for (int i = 0; i < numFrames; i++) {
            int16_t s; std::memcpy(&s, p + (size_t)i * (size_t)frameBytes, 2);
            out[i] = (float)s / 32768.f;
        }
    } else if (audioFormat == 1 && bitsPerSample == 24) { // PCM 24
        for (int i = 0; i < numFrames; i++) {
            const uint8_t* q = p + (size_t)i * (size_t)frameBytes;
            int32_t s = (int32_t)q[0] | ((int32_t)q[1] << 8) | ((int32_t)q[2] << 16);
            if (s & 0x800000) s |= 0xFF000000;
            out[i] = (float)s / 8388608.f;
        }
    } else {
        return false;
    }
    return true;
}

// Spring IR data, shared across every TapeEcho instance in the process. The
// six bundled IRs are 48kHz, so any other engine rate resamples all six —
// costly enough (measured 1.4s at 44.1kHz) that it must be paid once per rate
// per process rather than per instance. Immutable once built, handed out by
// shared_ptr.
struct SpringIrSet {
    std::vector<float> ir[SpringConvolver::NUM_LEVELS];
    int len = 0;   // common length; 0 means "unusable, spring stays silent"
};

// Both caches are cold-path only, never touched from process(), so a plain
// mutex is the right tool. The per-rate cache is capped and MRU-ordered —
// only one rate is ever active, so a cap of 2 costs little while making the
// common switch-there-and-back free.
static const size_t                          SPRING_IR_CACHE_MAX = 2;
static std::mutex                            g_springIrMutex;
static std::shared_ptr<const SpringIrSet>    g_springIrSource;
static std::vector<std::pair<int, std::shared_ptr<const SpringIrSet>>> g_springIrByRate;

// Loads the six 48 kHz source IRs once per process. `loader` does the actual
// per-file read; it is a callback so this stays independent of asset/plugin
// details. Returns a set with len == 0 if any file is missing or the lengths
// disagree, which is the existing "spring stays silent" fallback path.
template <typename Loader>
static std::shared_ptr<const SpringIrSet> springIrSource(Loader loader) {
    std::lock_guard<std::mutex> lock(g_springIrMutex);
    if (g_springIrSource) return g_springIrSource;

    auto set = std::make_shared<SpringIrSet>();
    bool ok = loader(set->ir);
    if (ok) {
        int n = (int)set->ir[0].size();
        for (int i = 1; i < SpringConvolver::NUM_LEVELS; i++)
            if ((int)set->ir[i].size() != n) { ok = false; break; }
        if (ok && n > 0) set->len = n;
    }
    g_springIrSource = set;
    return g_springIrSource;
}

// Returns the source IRs resampled to `isr`, building them on first request for
// that rate. At the source rate the source set is handed back unchanged, which is
// what the old code expressed by leaving irResampled empty.
static std::shared_ptr<const SpringIrSet> springIrForRate(
        int isr, int sourceSr, const std::shared_ptr<const SpringIrSet>& src) {
    if (!src || src->len <= 0) return src;
    if (isr == sourceSr) return src;

    std::lock_guard<std::mutex> lock(g_springIrMutex);
    for (size_t i = 0; i < g_springIrByRate.size(); i++) {
        if (g_springIrByRate[i].first == isr) {
            auto hit = g_springIrByRate[i];
            g_springIrByRate.erase(g_springIrByRate.begin() + i);
            g_springIrByRate.insert(g_springIrByRate.begin(), hit);   // MRU
            return hit.second;
        }
    }

    // Resample 96k → engine SR with Speex. All six use an identical converter
    // config, so their resampler group delay matches, preserving phase
    // coherence for the spectral morph. Off the audio thread, so alloc is fine.
    const int srcLen = src->len;
    const int outCap = (int)((int64_t)srcLen * isr / sourceSr) + 8;
    auto out = std::make_shared<SpringIrSet>();
    int irLen = 0;
    for (int i = 0; i < SpringConvolver::NUM_LEVELS; i++) {
        out->ir[i].assign(outCap, 0.f);
        rack::dsp::SampleRateConverter<1> rs;
        rs.setQuality(SPEEX_RESAMPLER_QUALITY_MAX);
        rs.setRates(sourceSr, isr);
        int inFrames = srcLen, outFrames = outCap;
        rs.process(src->ir[i].data(), 1, &inFrames, out->ir[i].data(), 1, &outFrames);
        if (outFrames > irLen) irLen = outFrames;   // common length = longest
    }
    // Force one common length (identical in practice) so every level partitions
    // into the same K; zero-pad any short tail.
    for (int i = 0; i < SpringConvolver::NUM_LEVELS; i++)
        out->ir[i].resize(irLen, 0.f);
    out->len = irLen;

    g_springIrByRate.insert(g_springIrByRate.begin(), std::make_pair(isr, out));
    if (g_springIrByRate.size() > SPRING_IR_CACHE_MAX)
        g_springIrByRate.resize(SPRING_IR_CACHE_MAX);
    return out;
}

struct SpringReverb {
    static constexpr int NUM_LEVELS = SpringConvolver::NUM_LEVELS;  // 6
    // Bundled IR sample rate. Dropped 96k -> 48k when the assets were shrunk:
    // everything above 24 kHz is ultrasonic (probed 14-18 dB below the audible
    // band) and a 48 kHz engine -- the common case -- was already discarding it
    // by resampling at load. Now those users get the IRs natively via the
    // `isr == sourceSr` early-out, with the band-limiting done offline at higher
    // quality; 96 kHz engines upsample instead, which Speex handles in either
    // direction. Halving the rate also bought back the bytes to store 24-bit
    // instead of 16-bit, which is audibly better on the tails.
    static constexpr int SOURCE_SR  = 48000;  // bundled IR sample rate

    SpringConvolver     conv;
    // Time-reversed counterpart, for the "Reverb follows Reverse" toggle — a
    // genuine reverse-reverb swell needs the impulse response itself
    // time-flipped, since reversing the excitation in real time is non-causal.
    // Prepared lazily (not alongside conv), since most patches never touch
    // this toggle and preparing it unconditionally roughly doubles the
    // spring's IR-spectra memory. Once prepared, it stays prepared.
    //
    // The reversed convolver is PUBLISHED to the audio thread by atomic
    // pointer and never mutated in place — a by-value member the GUI thread
    // rebuilds is not safe while process() can be inside processReversed() on
    // the same instance. The `walkingFade < 0.01f` gate at the widget call
    // site does not protect against this race (it exists to avoid chopping an
    // audible swell) — a by-value rebuild produces a real use-after-free.
    //
    // Reclamation is one rebuild deep: the outgoing instance is retired rather
    // than freed, destroyed only at the next rebuild, since a retired
    // instance outlives any possible use by many milliseconds.
    std::unique_ptr<SpringConvolver> convReversedActive;
    std::atomic<SpringConvolver*>    convReversedPub{nullptr};

    // Reclamation guard. `revEpoch` is advanced by the audio thread: +1 on
    // entry to processReversed(), +1 on exit, so odd means "a reader is
    // inside". A retired instance is destroyed only once the epoch proves no
    // reader can still hold it — simply deferring by one rebuild was NOT
    // enough; ThreadSanitizer showed two rebuilds completing inside a single
    // processReversed() call, freeing an instance still in use.
    //
    // The seq_cst fences are the Dekker pairing that makes this sound —
    // without them the writer could see an even epoch while a reader already
    // held the old pointer.
    struct RetiredConv {
        std::unique_ptr<SpringConvolver> conv;
        uint64_t guard;
    };
    std::vector<RetiredConv> convReversedRetired;   // GUI thread only
    std::atomic<uint64_t>    revEpoch{0};
    int   lastIrLen      = 0;   // common IR length conv was last prepared with — reused by prepareReversedNow()
    int   reversedLen    = 0;   // swell length convReversed was last built at

    // True once a reversed convolver is published.
    bool reversedReady() const {
        return convReversedPub.load(std::memory_order_acquire) != nullptr;
    }

    // GUI thread only. Publishes `next` (possibly null) and rotates the retired
    // slot. Store first, so the audio thread moves onto the new instance before
    // anything is destroyed.
    void publishReversed(std::unique_ptr<SpringConvolver> next) {
        SpringConvolver* raw = next.get();
        convReversedPub.store(raw, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint64_t guard = revEpoch.load(std::memory_order_relaxed);

        if (convReversedActive) {
            RetiredConv r;
            r.conv  = std::move(convReversedActive);
            r.guard = guard;
            convReversedRetired.push_back(std::move(r));
        }
        convReversedActive = std::move(next);
        reclaimRetiredReversed();
    }

    // GUI thread only. Destroys retired convolvers that no reader can hold:
    //   - now >= guard + 4: two complete reader calls have finished since the
    //     publish, so any reader that was inside at `guard` has long exited;
    //   - now == guard and even: no reader was inside when we published and none
    //     has entered since. This is the ordinary case when the feature is off or
    //     the engine is not running, and it keeps the list from growing.
    void reclaimRetiredReversed() {
        const uint64_t now = revEpoch.load(std::memory_order_acquire);
        for (size_t i = convReversedRetired.size(); i-- > 0; ) {
            const uint64_t g = convReversedRetired[i].guard;
            if (now >= g + 4 || (now == g && (now % 2) == 0))
                convReversedRetired.erase(convReversedRetired.begin() + i);
        }
    }
    SpringLevelEnvelope levelEnv;
    // Last morph value computed by process() this sample, reused by
    // processReversed() so both convolvers blend the same drive-level
    // character — levelEnv is a stateful smoother and must only be
    // advanced once per sample.
    float lastMorph = 0.f;

    // Bundled 48kHz source IRs (L1..L6) and the working set at the engine
    // rate — shared process-wide, immutable once built. `irSetMutex` guards
    // the two handles since onSampleRateChange() and prepareReversedNow() can
    // touch them from different threads; neither is touched in process().
    std::shared_ptr<const SpringIrSet> irSource;
    std::shared_ptr<const SpringIrSet> irActive;
    mutable std::mutex irSetMutex;

    std::shared_ptr<const SpringIrSet> activeIrs() const {
        std::lock_guard<std::mutex> lock(irSetMutex);
        return irActive;
    }

    // P locked at 1024: 10.7ms latency @ 96k, absorbed transparently since the
    // spring is a parallel wet send. JSON-overridable via spring_conv.partition_size.
    int   partitionSize = 1024;
    // Optional subtle pre-convolution soft-clip (springDrive). 0 = bypass —
    // the level-dependent character is already carried by the morph.
    float driveAmount = 0.f;

    // false while onSampleRateChange() resamples + rebuilds the spectra —
    // defence-in-depth, not a lock; process() returns silence until ready.
    bool ready = false;

    // Mechanical noise floor: a looped hardware capture of real spring tank
    // self-noise, not synthesized. Read directly in the main process().
    std::vector<float> noiseFloor;

    // Resample (if needed) and (re)build all six levels' partition spectra for
    // the engine SR. NOT RT-safe — call only from construction / SR change.
    void onSampleRateChange(float sr) {
        ready = false;
        levelEnv.onSampleRateChange(sr);

        const int srcLen = irSource ? irSource->len : 0;
        // Invalidate the reversed convolver on every sample-rate change,
        // before any early return — `reversedReady` deliberately stays true
        // after the toggle is switched off, so without this a toggle-off,
        // SR-change, toggle-on sequence would convolve a stale-rate reversed
        // IR (2.18x too long, an audibly slow reverse reverb). Gap between
        // invalidation and rebuild is silence, not stale audio.
        publishReversed(nullptr);

        if (srcLen <= 0) return;  // IRs missing → silent spring

        // Built once per rate for the whole process, not once per instance.
        auto set = springIrForRate((int)std::lround(sr), SOURCE_SR, irSource);
        if (!set || set->len <= 0) return;
        {
            std::lock_guard<std::mutex> lock(irSetMutex);
            irActive = set;
        }

        std::array<const float*, NUM_LEVELS> irs{};
        for (int i = 0; i < NUM_LEVELS; i++)
            irs[i] = set->ir[i].data();
        const int irLen = set->len;

        conv.prepare(irs, irLen, partitionSize);
        lastIrLen = irLen;
        ready = true;
        // convReversed is NOT prepared here — see its member comment. Callers
        // that want it prepared call prepareReversedNow() themselves after
        // this returns.
    }

    // Builds convReversed from time-flipped copies of the same IR data conv
    // was most recently prepared from. NOT RT-safe (allocates, runs FFTs) —
    // UI-thread only, never from process(). `swellLen` is the musical
    // parameter: convolution is causal, so a reversed IR of length T peaks T
    // late. Flipping the whole 7s bundled IR would put every swell's peak ~7s
    // after its input; taking the first swellLen samples instead keeps the
    // loudest part and lands the peak swellLen after the input.
    void prepareReversedNow(int swellLen) {
        if (!ready) { publishReversed(nullptr); return; }
        int useLen = swellLen;
        if (useLen > lastIrLen) useLen = lastIrLen;
        if (useLen < 256)       useLen = std::min(lastIrLen, 256);
        if (useLen <= 0) { publishReversed(nullptr); return; }

        // Local handle: the set stays alive for this call even if
        // onSampleRateChange() publishes a new one underneath us.
        auto set = activeIrs();
        if (!set || set->len <= 0) { publishReversed(nullptr); return; }
        std::array<const float*, NUM_LEVELS> irs{};
        for (int i = 0; i < NUM_LEVELS; i++)
            irs[i] = set->ir[i].data();

        std::vector<float> irsReversedStorage[NUM_LEVELS];
        std::array<const float*, NUM_LEVELS> irsReversed{};
        for (int i = 0; i < NUM_LEVELS; i++) {
            irsReversedStorage[i].assign(irs[i], irs[i] + useLen);
            std::reverse(irsReversedStorage[i].begin(), irsReversedStorage[i].end());
            irsReversed[i] = irsReversedStorage[i].data();
        }
        // Build into a fresh instance so the one the audio thread may
        // currently be inside is never touched mid-convolution.
        std::unique_ptr<SpringConvolver> fresh(new SpringConvolver);
        fresh->prepare(irsReversed, useLen, partitionSize);
        reversedLen = useLen;
        publishReversed(std::move(fresh));
    }

    // One sample. Pure, fully deterministic — no stochastic state (the
    // mechanical noise floor is frozen-gated in the main process(), not here).
    float process(float morphIn, float convIn) {
        if (!ready)
            return 0.f;
        // morphIn drives the level->character morph; convIn is the
        // drive-compensated excitation, so the tail rings at a fixed level.
        lastMorph     = levelEnv.process(morphIn);
        float driven  = springDrive(convIn, driveAmount);
        return conv.process(driven, lastMorph);
    }

    // The reversed-IR counterpart. Must be called after process() in the same
    // sample (reuses lastMorph so both convolvers share one drive-level
    // character). Returns silence if prepareReversedNow() hasn't run yet.
    float processReversed(float convIn) {
        // Announce that a reader is inside BEFORE loading the pointer, with the
        // fence that stops the load being hoisted above the announcement.
        const uint64_t e = revEpoch.load(std::memory_order_relaxed);
        revEpoch.store(e + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        SpringConvolver* c = convReversedPub.load(std::memory_order_relaxed);
        float out = 0.f;
        if (ready && c) {
            float driven = springDrive(convIn, driveAmount);
            out = c->process(driven, lastMorph);
        }

        revEpoch.store(e + 2, std::memory_order_release);
        return out;
    }

    void resetState() {
        conv.reset();
        if (SpringConvolver* c = convReversedPub.load(std::memory_order_acquire))
            c->reset();   // clears delay-line state only; no allocation
        levelEnv.reset();
    }
};