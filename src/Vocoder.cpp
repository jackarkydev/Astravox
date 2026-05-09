#include "plugin.hpp"
#include "widgets.hpp"
#include <cmath>

struct SVFilter {
    float lp = 0.f, bp = 0.f;

    void process(float in, float cutoffHz, float res, float sr) {
        float f = clamp(2.0f * std::sin(float(M_PI) * clamp(cutoffHz, 20.f, 20000.f) / (2.0f * sr)), 0.f, 1.f);
        float q = clamp(1.0f - res, 0.02f, 1.0f);
        for (int i = 0; i < 2; i++) {
            lp += f * bp;
            float hp = in - lp - q * bp;
            bp += f * hp;
        }
        if (!std::isfinite(lp) || !std::isfinite(bp)) { lp = 0.f; bp = 0.f; }
    }

    float hp(float in, float q_damp) { return in - lp - q_damp * bp; }
    void reset() { lp = 0.f; bp = 0.f; }
};

struct EnvFollower {
    float value = 0.f;

    float process(float in, float sampleTime) {
        float attackTime  = 0.001f;
        float releaseTime = 0.040f;
        float target = std::fabs(in);
        if (target > value)
            value += (target - value) * (1.f - std::exp(-sampleTime / attackTime));
        else
            value += (target - value) * (1.f - std::exp(-sampleTime / releaseTime));
        return value;
    }

    // Overload: caller passes rectified target; explicit attack + release times
    float process(float target, float sampleTime, float attackTime, float releaseTime) {
        if (target > value)
            value += (target - value) * (1.f - std::exp(-sampleTime / std::max(attackTime, 1e-6f)));
        else
            value += (target - value) * (1.f - std::exp(-sampleTime / releaseTime));
        return value;
    }

    void reset() { value = 0.f; }
};

struct DecayEnvelope {
    float value = 0.f;

    void trigger() { value = 1.f; }

    float process(float sampleTime, float decayTime) {
        value *= std::exp(-sampleTime / std::max(decayTime, 0.001f));
        if (value < 1e-5f) value = 0.f;
        return value;
    }
};

struct Vocoder : Module {
    static constexpr int NUM_BANDS = 16;

    enum ParamId {
        VCO_FREQ,
        VCO_WAVE_BTN,
        PULSE_WIDTH,
        CARRIER_MIX,
        GAIN_LEVEL,
        SPECTRAL_SHIFT,
        RESONANCE,
        SHIFT_LFO_RATE,
        SHIFT_LFO_AMT,
        BAND_LEVEL_FIRST,
        BAND_LEVEL_LAST = BAND_LEVEL_FIRST + 15,
        NOISE_BTN,
        MODE,
        DECAY,
        VOLUME,
        VCA_MODE_BTN,
        TRIGGER_PARAM,
        FREEZE_BTN,
        PROG_TRIG_EG,
        ATTACK_BTN,
        PARAMS_LEN
    };

    enum InputId {
        PROGRAM_INPUT,
        CARRIER_INPUT,
        VCA_CV_INPUT,
        VOCT_INPUT,
        VCO_PWM_INPUT,
        LFO_RATE_INPUT,
        TRIGGER_INPUT,
        SHIFT_INPUT,
        FREEZE_INPUT,
        BAND_VCACV_FIRST,
        BAND_VCACV_LAST = BAND_VCACV_FIRST + 15,
        INPUTS_LEN
    };

    enum OutputId {
        VCA_OUTPUT,
        LFO_OUTPUT,
        EG_OUTPUT,
        PROG_ENVF_OUTPUT,
        BAND_ENVF_FIRST,
        BAND_ENVF_LAST = BAND_ENVF_FIRST + 15,
        EVEN_OUTPUT,
        ODD_OUTPUT,
        OUTPUTS_LEN
    };

    enum LightId {
        TRIGGER_LIGHT,
        SHIFT_LFO_RATE_LIGHT,
        VCA_ON_LIGHT,
        VCA_EG_LIGHT,
        EXPANDER_LIGHT,
        PROG_TRIG_EG_LIGHT,
        PROG_TRIG_EG_OFF_LIGHT,
        ATTACK_FAST_LIGHT,
        ATTACK_SLOW_LIGHT,
        VCO_SAW_LIGHT,
        VCO_PULSE_LIGHT,
        NOISE_WHITE_LIGHT,
        NOISE_PINK_LIGHT,
        FREEZE_LIGHT,  // embedded in FREEZE_BTN latch
        VOCODER_LIGHT,
        FILTERBANK_LIGHT,
        BAND_LIGHT_FIRST,
        BAND_LIGHT_LAST = BAND_LIGHT_FIRST + NUM_BANDS - 1,
        LIGHTS_LEN
    };

    float phase[16] = {};
    float lfoPhase = 0.f;
    dsp::MinBlepGenerator<16, 32, float> sawMinBlep[16];
    dsp::MinBlepGenerator<16, 32, float> pulseMinBlep[16];
    SVFilter analysisFilters[NUM_BANDS];
    SVFilter synthFilters[NUM_BANDS];
    float analysisOut[NUM_BANDS] = {};
    EnvFollower bandFollowers[NUM_BANDS];
    EnvFollower progFollower;
    float envF[NUM_BANDS] = {};
    bool band0BP  = false;
    bool band15BP = false;
    float progEnvF = 0.f;
    DecayEnvelope eg;
    dsp::SchmittTrigger triggerIn;
    dsp::BooleanTrigger triggerBtn;
    dsp::SchmittTrigger progTrigger;
    int vcaMode    = 0;  // 0 = EG, 1 = ON
    int bandCvMode = 0;  // 0 = replace envelope, 1 = sum with envelope
    dsp::SchmittTrigger vcaModeTrigger;
    int progTrigEg = 1;  // 0 = OFF (manual trigger only), 1 = ON (auto from program)
    dsp::SchmittTrigger progTrigEgBtnTrigger;
    int attackMode = 0;  // 0 = FAST (0ms), 1 = SLOW (40ms)
    dsp::SchmittTrigger attackBtnTrigger;
    int vcoWave = 0;  // 0 = SAW, 1 = PULSE
    dsp::SchmittTrigger vcoWaveTrigger;
    int noiseType = 0;  // 0 = WHITE, 1 = PINK
    dsp::SchmittTrigger noiseBtnTrigger;
    int filterBankMode = 0;  // 0 = VOCODER, 1 = FILTER BANK
    dsp::SchmittTrigger modeBtnTrigger;
    float pinkB0=0, pinkB1=0, pinkB2=0, pinkB3=0, pinkB4=0, pinkB5=0, pinkB6=0;
    bool frozen = false;
    float frozenEnv[NUM_BANDS] = {};
    dsp::SchmittTrigger freezeGateTrigger;
    bool hfBypass = false;
    SVFilter hfFilter;
    int lfoTarget = 0;  // 0 = Spectrum, 1 = PWM, 2 = Both
    int lfoSync      = 0;  // 0 = Free-running, 1 = Reset on trigger

    enum SpacingMode { LIN = 0, MEL = 1, BARK = 2 };
    int spacingMode = LIN;
    float bandHz[NUM_BANDS];
    bool bandHzDirty = true;

    struct BandLevelQuantity : ParamQuantity {
        int bandIndex = 0;
        std::string getDisplayValueString() override {
            Vocoder* m = dynamic_cast<Vocoder*>(module);
            std::string levelStr = string::f("%.3f", getValue());
            if (m) {
                float hz = m->bandHz[bandIndex];
                std::string hzStr = hz >= 1000.f
                    ? string::f("%.2f kHz", hz / 1000.f)
                    : string::f("%.0f Hz", hz);
                return levelStr + " | " + hzStr;
            }
            return levelStr;
        }
    };

    Vocoder() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        configParam(VCO_FREQ,       -54.f,  36.f, 0.f,   "VCO Frequency");
        configButton(VCO_WAVE_BTN, "VCO Wave");
        configParam(PULSE_WIDTH,      0.f,   1.f, 0.5f,  "Pulse Width");
        configParam(CARRIER_MIX,      0.f,   1.f, 0.f,   "Carrier Mix");
        configParam(GAIN_LEVEL,       0.f,   3.f, 1.f,   "Gain Level");
        configParam(SPECTRAL_SHIFT,   0.f,   1.f, 0.5f,  "Spectral Shift");
        configParam(RESONANCE,        0.f,   1.f, 0.f,   "Resonance");
        configParam(SHIFT_LFO_RATE,   0.f,   1.f, 0.5f,  "Shift LFO Rate");
        configParam(SHIFT_LFO_AMT,    0.f,   1.f, 0.f,   "Shift LFO Amount");
        for (int i = 0; i < NUM_BANDS; i++) {
            auto* q = configParam<BandLevelQuantity>(BAND_LEVEL_FIRST + i, 0.f, 1.5f, 1.0f,
                "Band " + std::to_string(i + 1) + " Level");
            q->bandIndex = i;
        }
        configButton(NOISE_BTN, "Noise type");
        configButton(MODE, "Mode");
        configParam(DECAY,    0.f, 1.f, 0.3f,  "Decay");
        configParam(VOLUME,   0.f, 1.f, 0.75f, "Volume");
        configButton(VCA_MODE_BTN, "VCA mode");
        configButton(TRIGGER_PARAM, "Trigger");
        configButton(FREEZE_BTN, "Freeze");
        configButton(PROG_TRIG_EG, "Prog Trig EG");
        configButton(ATTACK_BTN, "Attack mode");
        configLight(PROG_TRIG_EG_LIGHT, "Prog Trig EG");
        configLight(ATTACK_FAST_LIGHT,  "Attack Fast");
        configLight(ATTACK_SLOW_LIGHT,  "Attack Slow");
        configLight(VCO_SAW_LIGHT,      "SAW");
        configLight(VCO_PULSE_LIGHT,    "PULSE");
        configLight(NOISE_WHITE_LIGHT,  "White noise");
        configLight(NOISE_PINK_LIGHT,   "Pink noise");
        configLight(FREEZE_LIGHT,       "Freeze active");
        configLight(VOCODER_LIGHT,      "Vocoder mode");
        configLight(FILTERBANK_LIGHT,   "Filter bank mode");
        configLight(VCA_ON_LIGHT,       "VCA On mode");
        configLight(VCA_EG_LIGHT,       "EG mode");
        configLight(EXPANDER_LIGHT,         "Expander");
        configLight(TRIGGER_LIGHT,          "Trigger active");

        configLight(SHIFT_LFO_RATE_LIGHT,   "Shift / LFO rate");
        configLight(PROG_TRIG_EG_OFF_LIGHT, "Prog trig EG off");
        for (int i = 0; i < NUM_BANDS; i++)
            configLight(BAND_LIGHT_FIRST + i, "Band " + std::to_string(i + 1) + " level");

        configInput(PROGRAM_INPUT,   "Program");
        configInput(CARRIER_INPUT,   "Carrier");
        configInput(VCA_CV_INPUT,    "VCA CV (–5V = silence, 0V = unity, +5V = no effect)");
        configInput(VOCT_INPUT,      "V/Oct");
        configInput(VCO_PWM_INPUT,   "PWM");
        configInput(LFO_RATE_INPUT,  "LFO Rate");
        configInput(TRIGGER_INPUT,   "Trigger");
        configInput(SHIFT_INPUT,     "Formant");
        configInput(FREEZE_INPUT,    "Freeze");
        for (int i = 0; i < NUM_BANDS; i++)
            configInput(BAND_VCACV_FIRST + i, "Band " + std::to_string(i + 1) + " VCA");

        configOutput(VCA_OUTPUT,       "VCA Out");
        configOutput(LFO_OUTPUT,       "LFO Out");
        configOutput(EG_OUTPUT,        "EG Out");
        configOutput(PROG_ENVF_OUTPUT, "Prog Env F");
        for (int i = 0; i < NUM_BANDS; i++)
            configOutput(BAND_ENVF_FIRST + i, "Band " + std::to_string(i + 1) + " Out");
        configOutput(EVEN_OUTPUT, "Even bands");
        configOutput(ODD_OUTPUT,  "Odd bands");
    }

    void computeBandHz() {
        if (spacingMode == LIN) {
            static constexpr float BASE[NUM_BANDS] = {
                100.f, 135.f, 180.f, 240.f, 320.f, 430.f, 575.f, 770.f,
                1030.f, 1380.f, 1850.f, 2480.f, 3320.f, 4440.f, 5950.f, 8000.f
            };
            for (int i = 0; i < NUM_BANDS; i++) bandHz[i] = BASE[i];
        } else if (spacingMode == MEL) {
            auto toMel   = [](float f) { return 2595.f * std::log10(1.f + f / 700.f); };
            auto fromMel = [](float m) { return 700.f * (std::pow(10.f, m / 2595.f) - 1.f); };
            float melLow  = toMel(100.f);
            float melHigh = toMel(8000.f);
            for (int i = 0; i < NUM_BANDS; i++) {
                float t = (float)i / (NUM_BANDS - 1);
                bandHz[i] = fromMel(melLow + t * (melHigh - melLow));
            }
        } else { // BARK
            static constexpr float BARK[NUM_BANDS] = {
                50.f, 150.f, 250.f, 350.f, 450.f, 570.f, 700.f, 840.f,
                1000.f, 1170.f, 1370.f, 1600.f, 1850.f, 2150.f, 2500.f, 2900.f
            };
            for (int i = 0; i < NUM_BANDS; i++) bandHz[i] = BARK[i];
        }
        bandHzDirty = false;
    }

    void process(const ProcessArgs& args) override {
        if (bandHzDirty) computeBandHz();
        // ── Shift LFO ─────────────────────────────────────────────────────────
        float lfoRate = 0.05f * std::pow(10000.f, params[SHIFT_LFO_RATE].getValue());
        if (inputs[LFO_RATE_INPUT].isConnected())
            lfoRate *= dsp::exp2_taylor5(inputs[LFO_RATE_INPUT].getVoltage());
        lfoPhase += lfoRate * args.sampleTime;
        if (lfoPhase >= 1.f) lfoPhase -= 1.f;
        float lfoOut = 1.f - 4.f * std::fabs(lfoPhase - 0.5f);
        outputs[LFO_OUTPUT].setVoltage((lfoOut + 1.f) * 2.5f);
        lights[SHIFT_LFO_RATE_LIGHT].setBrightness(
            params[SHIFT_LFO_RATE].getValue() < 0.6f ? lfoOut * 0.5f + 0.5f : 1.0f);

        // ── VCO ───────────────────────────────────────────────────────────────
        int vcoChannels = std::max(inputs[VOCT_INPUT].getChannels(), 1);
        float basePitch = params[VCO_FREQ].getValue() / 12.f;
        float pwKnob = rescale(params[PULSE_WIDTH].getValue(), 0.f, 1.f, 0.01f, 0.50f);
        float duty = inputs[VCO_PWM_INPUT].isConnected()
            ? clamp(pwKnob + rescale(inputs[VCO_PWM_INPUT].getVoltage(), -5.f, 5.f, -0.495f, 0.495f), 0.01f, 0.50f)
            : pwKnob;
        if (lfoTarget >= 1)
            duty = clamp(duty + lfoOut * params[SHIFT_LFO_AMT].getValue() * 0.245f, 0.01f, 0.50f);
        if (vcoWaveTrigger.process(params[VCO_WAVE_BTN].getValue()))
            vcoWave = 1 - vcoWave;
        lights[VCO_SAW_LIGHT].setBrightness(vcoWave == 0 ? 1.f : 0.f);
        lights[VCO_PULSE_LIGHT].setBrightness(vcoWave == 1 ? 1.f : 0.f);
        float vco_sum = 0.f;
        for (int c = 0; c < vcoChannels; c++) {
            float pitch = basePitch + inputs[VOCT_INPUT].getVoltage(c);
            float freq = clamp(261.626f * std::pow(2.f, pitch), 0.1f, 20000.f);
            float deltaPhase = freq * args.sampleTime;
            if (phase[c] + deltaPhase >= 1.f) {
                float frac = (1.f - phase[c]) / deltaPhase;
                sawMinBlep[c].insertDiscontinuity(frac - 1.f, -2.f);
                pulseMinBlep[c].insertDiscontinuity(frac - 1.f, 2.f);
            }
            if (phase[c] < duty && phase[c] + deltaPhase >= duty) {
                float frac = (duty - phase[c]) / deltaPhase;
                pulseMinBlep[c].insertDiscontinuity(frac - 1.f, -2.f);
            }
            float saw   = (phase[c] * 2.f - 1.f) * 5.f + sawMinBlep[c].process() * 5.f;
            float pulse = ((phase[c] < duty) ? 5.f : -5.f) + pulseMinBlep[c].process() * 5.f;
            phase[c] += deltaPhase;
            if (phase[c] >= 1.f)
                phase[c] -= 1.f;
            float vco_c = (vcoWave == 0) ? saw : pulse;
            vco_sum += vco_c;
        }
        float vco_out = vco_sum / vcoChannels;

        // ── Noise ─────────────────────────────────────────────────────────────
        if (noiseBtnTrigger.process(params[NOISE_BTN].getValue()))
            noiseType = 1 - noiseType;
        lights[NOISE_WHITE_LIGHT].setBrightness(noiseType == 0 ? 1.f : 0.f);
        lights[NOISE_PINK_LIGHT].setBrightness(noiseType == 1 ? 1.f : 0.f);
        float white = rack::random::uniform() * 2.f - 1.f;
        float noise_out;
        if (noiseType == 0) {
            noise_out = white * 5.f;
        } else {
            pinkB0 = 0.99886f * pinkB0 + white * 0.0555179f;
            pinkB1 = 0.99332f * pinkB1 + white * 0.0750759f;
            pinkB2 = 0.96900f * pinkB2 + white * 0.1538520f;
            pinkB3 = 0.86650f * pinkB3 + white * 0.3104856f;
            pinkB4 = 0.55000f * pinkB4 + white * 0.5329522f;
            pinkB5 = -0.7616f * pinkB5 - white * 0.0168980f;
            noise_out = (pinkB0+pinkB1+pinkB2+pinkB3+pinkB4+pinkB5+pinkB6+white*0.5362f) * 0.11f * 5.f;
            pinkB6 = white * 0.115926f;
        }

        // ── Carrier Mix ───────────────────────────────────────────────────────
        float carrier_left;
        if (inputs[CARRIER_INPUT].isConnected()) {
            int carrierCh = inputs[CARRIER_INPUT].getChannels();
            float carrierSum = 0.f;
            for (int c = 0; c < carrierCh; c++)
                carrierSum += inputs[CARRIER_INPUT].getVoltage(c);
            carrier_left = carrierSum / carrierCh;
        } else {
            carrier_left = vco_out;
        }
        float carrier_out = crossfade(carrier_left, noise_out, params[CARRIER_MIX].getValue());

        // ── Analysis filter bank ──────────────────────────────────────────────
        float programSignal = inputs[PROGRAM_INPUT].getVoltage();
        float gainLevel = params[GAIN_LEVEL].getValue();
        float gainApplied = gainLevel * 0.5f;
        float programScaled = programSignal * gainApplied;
        float satLevel = 10.0f / (1.0f + gainLevel * 0.5f);
        float programAmped = std::tanh(programScaled / satLevel) * satLevel;
        progEnvF = clamp(progFollower.process(programAmped, args.sampleTime) * 8.f, 0.f, 8.f);
        outputs[PROG_ENVF_OUTPUT].setVoltage(progEnvF);

        for (int i = 0; i < NUM_BANDS; i++)
            analysisFilters[i].process(programAmped, bandHz[i], 0.5f, args.sampleRate);
        analysisOut[0] = band0BP  ? analysisFilters[0].bp : analysisFilters[0].lp;
        for (int i = 1; i < NUM_BANDS - 1; i++)
            analysisOut[i] = analysisFilters[i].bp;
        analysisOut[NUM_BANDS - 1] = band15BP ? analysisFilters[NUM_BANDS - 1].bp
                                               : analysisFilters[NUM_BANDS - 1].hp(programAmped, 0.5f);

        frozen = params[FREEZE_BTN].getValue() > 0.f;
        if (inputs[FREEZE_INPUT].isConnected())
            frozen = (inputs[FREEZE_INPUT].getVoltage() >= 2.0f);
        lights[FREEZE_LIGHT].setBrightness(frozen ? 1.f : 0.f);

        float attackTime  = (attackMode == 0) ? 0.f : 0.040f;
        float releaseTime = 0.001f * std::pow(5000.f, params[DECAY].getValue());
        for (int i = 0; i < NUM_BANDS; i++) {
            float target = std::fabs(analysisOut[i]);
            if (attackTime == 0.f)
                bandFollowers[i].value = (target > bandFollowers[i].value)
                    ? target
                    : bandFollowers[i].value + (target - bandFollowers[i].value)
                      * (1.f - std::exp(-args.sampleTime / releaseTime));
            else
                bandFollowers[i].process(target, args.sampleTime, attackTime, releaseTime);
            envF[i] = clamp(bandFollowers[i].value * 8.f, 0.f, 8.f);
        }
        if (!frozen) {
            for (int i = 0; i < NUM_BANDS; i++)
                frozenEnv[i] = envF[i];
        }
        float* activeEnv = frozen ? frozenEnv : envF;
        for (int i = 0; i < NUM_BANDS; i++)
            outputs[BAND_ENVF_FIRST + i].setVoltage(activeEnv[i]);

        // ── Synthesis filter bank ─────────────────────────────────────────────
        float shiftKnob = params[SPECTRAL_SHIFT].getValue();
        float shiftLfo  = (lfoTarget != 1) ? lfoOut * params[SHIFT_LFO_AMT].getValue() * 0.5f : 0.f;
        float shiftCv   = inputs[SHIFT_INPUT].isConnected()
            ? inputs[SHIFT_INPUT].getVoltage() / 10.f
            : 0.f;
        float shiftNorm = clamp(shiftKnob + shiftLfo + shiftCv, 0.f, 1.f);
        float shift_mult = 0.25f * std::pow(16.f, shiftNorm);
        float res_norm   = params[RESONANCE].getValue();
        float Q          = 0.7f * std::pow(15.f / 0.7f, res_norm);
        float q_damp     = clamp(1.0f / Q, 0.02f, 1.0f);
        float synth_res  = 1.0f - q_damp;

        for (int i = 0; i < NUM_BANDS; i++) {
            float filterIn = carrier_out;
            synthFilters[i].process(filterIn, bandHz[i] * shift_mult, synth_res, args.sampleRate);
        }

        float synthOut[NUM_BANDS];
        synthOut[0] = band0BP  ? synthFilters[0].bp : synthFilters[0].lp;
        for (int i = 1; i < NUM_BANDS - 1; i++)
            synthOut[i] = synthFilters[i].bp;
        synthOut[NUM_BANDS - 1] = band15BP ? synthFilters[NUM_BANDS - 1].bp
                                           : synthFilters[NUM_BANDS - 1].hp(carrier_out, q_damp);

        // ── EG ────────────────────────────────────────────────────────────────
        if (triggerIn.process(inputs[TRIGGER_INPUT].getVoltage())) { eg.trigger(); if (lfoSync) lfoPhase = 0.f; }
        if (triggerBtn.process(params[TRIGGER_PARAM].getValue()))  { eg.trigger(); if (lfoSync) lfoPhase = 0.f; }
        if (progTrigEgBtnTrigger.process(params[PROG_TRIG_EG].getValue()))
            progTrigEg = 1 - progTrigEg;
        lights[PROG_TRIG_EG_LIGHT].setBrightness(    progTrigEg ? 1.f : 0.f);
        lights[PROG_TRIG_EG_OFF_LIGHT].setBrightness(progTrigEg ? 0.f : 1.f);
        if (vcaMode == 0 && progTrigEg && progTrigger.process(progEnvF, 2.f, 6.f)) eg.trigger();
        float egValue = eg.process(args.sampleTime, releaseTime);
        if (attackBtnTrigger.process(params[ATTACK_BTN].getValue()))
            attackMode = 1 - attackMode;
        lights[ATTACK_FAST_LIGHT].setBrightness(attackMode == 0 ? 1.f : 0.f);
        lights[ATTACK_SLOW_LIGHT].setBrightness(attackMode == 1 ? 1.f : 0.f);
        outputs[EG_OUTPUT].setVoltage(egValue * 8.f);
        lights[TRIGGER_LIGHT].setBrightness(egValue > 0.01f ? egValue : 0.f);

        // ── VCA mode ──────────────────────────────────────────────────────────
        if (vcaModeTrigger.process(params[VCA_MODE_BTN].getValue()))
            vcaMode = 1 - vcaMode;
        lights[VCA_EG_LIGHT].setBrightness(vcaMode == 0 ? 1.f : 0.f);
        lights[VCA_ON_LIGHT].setBrightness(vcaMode == 1 ? 1.f : 0.f);
        bool expanderConnected = rightExpander.module != nullptr &&
            rightExpander.module->model->slug == "Astravox-VocoderExpander";
        lights[EXPANDER_LIGHT].setBrightness(expanderConnected ? 1.f : 0.f);
        bool egMode = (vcaMode == 0);
        float vcaLevel = egMode ? egValue : 1.f;

        // ── Band level attenuation + VCA CV ───────────────────────────────────
        if (modeBtnTrigger.process(params[MODE].getValue()))
            filterBankMode = 1 - filterBankMode;
        lights[VOCODER_LIGHT].setBrightness(filterBankMode == 0 ? 1.f : 0.f);
        lights[FILTERBANK_LIGHT].setBrightness(filterBankMode == 1 ? 1.f : 0.f);
        bool vocoderMode = (filterBankMode == 0);
        float leveledOut[NUM_BANDS];

        float evenSum = 0.f;
        float oddSum  = 0.f;
        for (int i = 0; i < NUM_BANDS; i++) {
            float level = params[BAND_LEVEL_FIRST + i].getValue();


            float vcaCv;
            if (inputs[BAND_VCACV_FIRST + i].isConnected()) {
                float cv = inputs[BAND_VCACV_FIRST + i].getVoltage() / 8.f;
                if (bandCvMode == 0)
                    vcaCv = clamp(cv, 0.f, 1.f);
                else
                    vcaCv = clamp(activeEnv[i] / 8.f + cv, 0.f, 1.f);
            } else if (vocoderMode)
                vcaCv = activeEnv[i] / 8.f;
            else
                vcaCv = 1.f;

            leveledOut[i] = synthOut[i] * level * vcaCv;
            float ledGain = vocoderMode ? vcaCv : vcaLevel;
            lights[BAND_LIGHT_FIRST + i].setBrightness(clamp(ledGain * level, 0.f, 1.f));
            if ((i % 2) == 0)  // i=0 → band 1 (odd-numbered); index parity is inverted
                oddSum  += leveledOut[i];
            else
                evenSum += leveledOut[i];
        }

        float volume = params[VOLUME].getValue() * 1.7783f;
        float mixedOut = 0.f;
        for (int i = 0; i < NUM_BANDS; i++)
            mixedOut += leveledOut[i];
        float audioOut = mixedOut * 0.1f * vcaLevel * volume;
        if (hfBypass) {
            float hfCutoff = bandHz[NUM_BANDS - 1] * (band15BP ? 1.0f : 1.5f);
            hfFilter.process(programAmped, hfCutoff, 0.f, args.sampleRate);
            audioOut += hfFilter.hp(programAmped, 1.f) * 0.25f * vcaLevel * volume;
        }
        float vcaCv = inputs[VCA_CV_INPUT].isConnected()
            ? clamp(inputs[VCA_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f)
            : 1.f;
        outputs[VCA_OUTPUT].setVoltage(audioOut * vcaCv);
        outputs[EVEN_OUTPUT].setVoltage(clamp(evenSum * 0.1f * vcaLevel * volume * vcaCv, -12.f, 12.f));
        outputs[ODD_OUTPUT].setVoltage( clamp(oddSum  * 0.1f * vcaLevel * volume * vcaCv, -12.f, 12.f));
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_t* phaseJ = json_array();
        for (int c = 0; c < 16; c++)
            json_array_append_new(phaseJ, json_real(phase[c]));
        json_object_set_new(rootJ, "phase", phaseJ);
        json_object_set_new(rootJ, "lfoPhase", json_real(lfoPhase));
        json_object_set_new(rootJ, "egValue",  json_real(eg.value));
        json_t* fArr = json_array();
        for (int i = 0; i < NUM_BANDS; i++)
            json_array_append_new(fArr, json_real(bandFollowers[i].value));
        json_object_set_new(rootJ, "bandFollowers", fArr);
        json_object_set_new(rootJ, "progFollower", json_real(progFollower.value));
        json_object_set_new(rootJ, "band0BP",     json_boolean(band0BP));
        json_object_set_new(rootJ, "band15BP",    json_boolean(band15BP));
        json_object_set_new(rootJ, "spacingMode", json_integer(spacingMode));
        json_object_set_new(rootJ, "vcaMode",     json_integer(vcaMode));
        json_object_set_new(rootJ, "progTrigEg",  json_integer(progTrigEg));
        json_object_set_new(rootJ, "attackMode",  json_integer(attackMode));
        json_object_set_new(rootJ, "vcoWave",     json_integer(vcoWave));
        json_object_set_new(rootJ, "noiseType",      json_integer(noiseType));
        json_object_set_new(rootJ, "filterBankMode", json_integer(filterBankMode));
        json_object_set_new(rootJ, "hfBypass",    json_boolean(hfBypass));
        json_object_set_new(rootJ, "bandCvMode", json_integer(bandCvMode));
        json_object_set_new(rootJ, "lfoTarget",  json_integer(lfoTarget));
        json_object_set_new(rootJ, "lfoSync",       json_integer(lfoSync));
        json_t* feJ = json_array();
        for (int i = 0; i < NUM_BANDS; i++)
            json_array_append_new(feJ, json_real(frozenEnv[i]));
        json_object_set_new(rootJ, "frozenEnv", feJ);
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* j;
        json_t* phaseJ = json_object_get(rootJ, "phase");
        if (phaseJ && json_is_array(phaseJ))
            for (int c = 0; c < 16; c++) {
                json_t* v = json_array_get(phaseJ, c);
                if (v) phase[c] = json_real_value(v);
            }
        j = json_object_get(rootJ, "lfoPhase"); if (j) lfoPhase       = json_real_value(j);
        j = json_object_get(rootJ, "egValue");  if (j) eg.value       = json_real_value(j);
        j = json_object_get(rootJ, "bandFollowers");
        if (j) for (int i = 0; i < NUM_BANDS; i++) {
            json_t* v = json_array_get(j, i);
            if (v) bandFollowers[i].value = json_real_value(v);
        }
        j = json_object_get(rootJ, "progFollower"); if (j) progFollower.value = json_real_value(j);
        j = json_object_get(rootJ, "band0BP");     if (j) band0BP     = json_boolean_value(j);
        j = json_object_get(rootJ, "band15BP");    if (j) band15BP    = json_boolean_value(j);
        j = json_object_get(rootJ, "spacingMode"); if (j) { spacingMode = json_integer_value(j); bandHzDirty = true; }
        j = json_object_get(rootJ, "vcaMode");     if (j) vcaMode    = json_integer_value(j);
        j = json_object_get(rootJ, "progTrigEg"); if (j) progTrigEg = json_integer_value(j);
        j = json_object_get(rootJ, "attackMode");  if (j) attackMode = json_integer_value(j);
        j = json_object_get(rootJ, "vcoWave");     if (j) vcoWave    = json_integer_value(j);
        j = json_object_get(rootJ, "noiseType");      if (j) noiseType      = json_integer_value(j);
        j = json_object_get(rootJ, "filterBankMode"); if (j) filterBankMode = json_integer_value(j);
        j = json_object_get(rootJ, "hfBypass");    if (j) hfBypass    = json_boolean_value(j);
        j = json_object_get(rootJ, "bandCvMode"); if (j) bandCvMode = json_integer_value(j);
        j = json_object_get(rootJ, "lfoTarget");  if (j) lfoTarget  = json_integer_value(j);
        j = json_object_get(rootJ, "lfoSync");      if (j) lfoSync      = json_integer_value(j);
        json_t* feJ = json_object_get(rootJ, "frozenEnv");
        if (feJ) for (int i = 0; i < NUM_BANDS; i++) {
            json_t* v = json_array_get(feJ, i);
            if (v) frozenEnv[i] = json_real_value(v);
        }
    }
};


struct BandSlider : VCVSlider {
    void onDoubleClick(const event::DoubleClick& e) override {
        if (APP->window->getMods() & RACK_MOD_CTRL) {
            getParamQuantity()->setValue(0.f);
            e.consume(this);
        } else {
            VCVSlider::onDoubleClick(e);
        }
    }
};

// Band x-positions (mm) — shared by widget constructor and panel draw
static constexpr float kBX[16] = {
    34.08f, 43.71f, 53.35f, 62.98f, 72.61f, 82.25f,
    92.41f, 102.05f, 111.69f, 121.91f, 130.96f, 140.59f,
    150.74f, 160.38f, 170.02f, 179.65f
};

struct VocoderPanelDraw : Widget {
    std::shared_ptr<Font> font;
    std::shared_ptr<Font> fontLight;

    void draw(const DrawArgs& args) override;
};

struct VocoderWidget : ModuleWidget {
    VocoderWidget(Vocoder* module) {

        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Vocoder.svg")));

        addChild(createWidget<ScrewBlack>(Vec(0, 0)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewBlack>(Vec(0, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Knobs
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 15.00f,  27.71f)), module, Vocoder::VCO_FREQ));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 15.00f,  56.47f)), module, Vocoder::PULSE_WIDTH));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 15.00f,  89.05f)), module, Vocoder::CARRIER_MIX));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 68.65f,  98.18f)), module, Vocoder::SPECTRAL_SHIFT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 68.65f, 112.93f)), module, Vocoder::RESONANCE));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 96.10f,  98.18f)), module, Vocoder::SHIFT_LFO_RATE));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 96.10f, 112.93f)), module, Vocoder::SHIFT_LFO_AMT));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec( 42.36f, 112.93f)), module, Vocoder::DECAY));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(197.96f,  27.71f)), module, Vocoder::VOLUME));
        addParam(createParamCentered<Trimpot>(       mm2px(Vec(197.96f,  80.04f)), module, Vocoder::GAIN_LEVEL));

        // Band level faders
        for (int i = 0; i < Vocoder::NUM_BANDS; i++)
            addParam(createParamCentered<BandSlider>(mm2px(Vec(kBX[i], 45.92f)), module, Vocoder::BAND_LEVEL_FIRST + i));

        // VCO WAVE button + LEDs
        addParam(createParamCentered<VCVButton>(             mm2px(Vec(15.00f, 41.71f)), module, Vocoder::VCO_WAVE_BTN));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec( 6.50f, 42.06f)), module, Vocoder::VCO_SAW_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(23.50f, 42.06f)), module, Vocoder::VCO_PULSE_LIGHT));

        // NOISE button + LEDs
        addParam(createParamCentered<VCVButton>(             mm2px(Vec(15.00f, 76.15f)), module, Vocoder::NOISE_BTN));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec( 6.50f, 76.54f)), module, Vocoder::NOISE_WHITE_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(23.50f, 76.54f)), module, Vocoder::NOISE_PINK_LIGHT));

        // ATTACK button + LEDs
        addParam(createParamCentered<VCVButton>(             mm2px(Vec(42.69f,  96.98f)), module, Vocoder::ATTACK_BTN));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(34.20f,  97.77f)), module, Vocoder::ATTACK_FAST_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(50.98f,  97.77f)), module, Vocoder::ATTACK_SLOW_LIGHT));

        // VCA MODE button
        addParam(createParamCentered<VCVButton>(mm2px(Vec(197.96f, 41.33f)), module, Vocoder::VCA_MODE_BTN));

        // FREEZE latch button with embedded LED
        addParam(createLightParamCentered<VCVLightLatch<SmallLight<GreenLight>>>(mm2px(Vec(197.96f, 58.64f)), module, Vocoder::FREEZE_BTN, Vocoder::FREEZE_LIGHT));

        // PROG TRIG EG button + ON/OFF LEDs
        addParam(createParamCentered<VCVButton>(             mm2px(Vec(15.00f, 106.75f)), module, Vocoder::PROG_TRIG_EG));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec( 6.50f, 106.62f)), module, Vocoder::PROG_TRIG_EG_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(  mm2px(Vec(23.50f, 106.62f)), module, Vocoder::PROG_TRIG_EG_OFF_LIGHT));

        // TRIGGER button
        addParam(createLightParamCentered<VCVLightButton<SmallLight<GreenLight>>>(mm2px(Vec(15.00f, 118.00f)), module, Vocoder::TRIGGER_PARAM, Vocoder::TRIGGER_LIGHT));

        // MODE button + LEDs
        addParam(createParamCentered<VCVButton>(             mm2px(Vec(147.62f, 94.00f)), module, Vocoder::MODE));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(138.79f, 94.39f)), module, Vocoder::VOCODER_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(156.61f, 94.39f)), module, Vocoder::FILTERBANK_LIGHT));

        // Inputs
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(197.96f,  91.43f)), module, Vocoder::PROGRAM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(118.01f, 107.35f)), module, Vocoder::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(132.24f, 107.35f)), module, Vocoder::CARRIER_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(146.52f, 107.35f)), module, Vocoder::SHIFT_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(160.54f, 107.35f)), module, Vocoder::VCA_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(118.01f, 117.67f)), module, Vocoder::VCO_PWM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(132.24f, 117.67f)), module, Vocoder::TRIGGER_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(146.52f, 117.67f)), module, Vocoder::LFO_RATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(160.54f, 117.67f)), module, Vocoder::FREEZE_INPUT));

        // Band VCA CV inputs
        for (int i = 0; i < Vocoder::NUM_BANDS; i++)
            addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kBX[i], 68.45f)), module, Vocoder::BAND_VCACV_FIRST + i));

        // Outputs
        addOutput(createOutputCentered<AstravoxPort>(mm2px(Vec(205.65f, 117.67f)), module, Vocoder::VCA_OUTPUT));
        addOutput(createOutputCentered<AstravoxPort>(mm2px(Vec(176.17f, 107.35f)), module, Vocoder::EG_OUTPUT));
        addOutput(createOutputCentered<AstravoxPort>(mm2px(Vec(176.19f, 117.67f)), module, Vocoder::LFO_OUTPUT));
        addOutput(createOutputCentered<AstravoxPort>(mm2px(Vec(191.05f, 117.67f)), module, Vocoder::PROG_ENVF_OUTPUT));
        addOutput(createOutputCentered<AstravoxPort>(mm2px(Vec(191.05f, 107.35f)), module, Vocoder::EVEN_OUTPUT));
        addOutput(createOutputCentered<AstravoxPort>(mm2px(Vec(205.65f, 107.35f)), module, Vocoder::ODD_OUTPUT));

        // Band envelope follower outputs
        for (int i = 0; i < Vocoder::NUM_BANDS; i++)
            addOutput(createOutputCentered<AstravoxPort>(mm2px(Vec(kBX[i], 79.47f)), module, Vocoder::BAND_ENVF_FIRST + i));

        // Band LED meters
        for (int i = 0; i < Vocoder::NUM_BANDS; i++)
            addChild(createLightCentered<BandMeterLight>(
                mm2px(Vec(kBX[i], 20.00f)), module, Vocoder::BAND_LIGHT_FIRST + i));

        // VCA MODE indicator LEDs
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(189.46f, 42.10f)), module, Vocoder::VCA_ON_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(206.46f, 42.10f)), module, Vocoder::VCA_EG_LIGHT));

        // Expander LED
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(202.81f,  8.12f)), module, Vocoder::EXPANDER_LIGHT));

        // LFO rate blink LED
        addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(104.43f, 94.96f)), module, Vocoder::SHIFT_LFO_RATE_LIGHT));

        // Static panel labels — cached in a framebuffer (drawn last, on top)
        auto* fbw = new rack::FramebufferWidget;
        fbw->box = box;
        auto* pd = new VocoderPanelDraw;
        pd->box = box;
        pd->font      = APP->window->loadFont(asset::plugin(pluginInstance, "res/AVPosterGothic-Regular.otf"));
        pd->fontLight = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Inter-Light.otf"));
        fbw->addChild(pd);
        addChild(fbw);
    }

    void appendContextMenu(Menu* menu) override {
        Vocoder* m = dynamic_cast<Vocoder*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Sibilance", "", &m->hfBypass));
        menu->addChild(createBoolPtrMenuItem("Band 1: LP → BP", "", &m->band0BP));
        menu->addChild(createBoolPtrMenuItem("Band 16: HP → BP", "", &m->band15BP));

        // Band spacing submenu
        struct SpacingItem : MenuItem {
            Vocoder* module;
            int mode;
            void onAction(const event::Action& e) override {
                module->spacingMode = mode;
                module->bandHzDirty = true;
            }
            void step() override {
                rightText = (module->spacingMode == mode) ? "✔" : "";
                MenuItem::step();
            }
        };
        struct SpacingMenu : MenuItem {
            Vocoder* module;
            Menu* createChildMenu() override {
                Menu* menu = new Menu;
                auto add = [&](const char* label, int mode) {
                    auto* item = new SpacingItem;
                    item->text = label;
                    item->module = module;
                    item->mode = mode;
                    menu->addChild(item);
                };
                add("LIN", Vocoder::LIN);
                add("MEL", Vocoder::MEL);
                add("BARK", Vocoder::BARK);
                return menu;
            }
        };
        auto* sm = new SpacingMenu;
        sm->text = "Band spacing";
        sm->module = m;
        sm->rightText = RIGHT_ARROW;
        menu->addChild(sm);

        // Band CV mode submenu
        struct BandCvModeItem : MenuItem {
            Vocoder* module;
            int mode;
            void onAction(const event::Action& e) override {
                module->bandCvMode = mode;
            }
            void step() override {
                rightText = (module->bandCvMode == mode) ? "✔" : "";
                MenuItem::step();
            }
        };
        struct BandCvMenu : MenuItem {
            Vocoder* module;
            Menu* createChildMenu() override {
                Menu* menu = new Menu;
                auto add = [&](const char* label, int mode) {
                    auto* item = new BandCvModeItem;
                    item->text = label;
                    item->module = module;
                    item->mode = mode;
                    menu->addChild(item);
                };
                add("Replace envelope (default)", 0);
                add("Sum with envelope", 1);
                return menu;
            }
        };
        auto* bcm = new BandCvMenu;
        bcm->text = "Band CV mode";
        bcm->module = m;
        bcm->rightText = RIGHT_ARROW;
        menu->addChild(bcm);

        // LFO target submenu
        struct LfoTargetItem : MenuItem {
            Vocoder* module;
            int mode;
            void onAction(const event::Action& e) override {
                module->lfoTarget = mode;
            }
            void step() override {
                rightText = (module->lfoTarget == mode) ? "✔" : "";
                MenuItem::step();
            }
        };
        struct LfoTargetMenu : MenuItem {
            Vocoder* module;
            Menu* createChildMenu() override {
                Menu* menu = new Menu;
                auto add = [&](const char* label, int mode) {
                    auto* item = new LfoTargetItem;
                    item->text = label;
                    item->module = module;
                    item->mode = mode;
                    menu->addChild(item);
                };
                add("Spectrum (default)", 0);
                add("PWM", 1);
                add("Both", 2);
                return menu;
            }
        };
        auto* ltm = new LfoTargetMenu;
        ltm->text = "LFO target";
        ltm->module = m;
        ltm->rightText = RIGHT_ARROW;
        menu->addChild(ltm);

        // LFO sync submenu
        struct LfoSyncItem : MenuItem {
            Vocoder* module;
            int mode;
            void onAction(const event::Action& e) override {
                module->lfoSync = mode;
            }
            void step() override {
                rightText = (module->lfoSync == mode) ? "✔" : "";
                MenuItem::step();
            }
        };
        struct LfoSyncMenu : MenuItem {
            Vocoder* module;
            Menu* createChildMenu() override {
                Menu* menu = new Menu;
                auto add = [&](const char* label, int mode) {
                    auto* item = new LfoSyncItem;
                    item->text = label;
                    item->module = module;
                    item->mode = mode;
                    menu->addChild(item);
                };
                add("Free (default)", 0);
                add("Reset on trigger", 1);
                return menu;
            }
        };
        auto* lsm = new LfoSyncMenu;
        lsm->text = "LFO sync";
        lsm->module = m;
        lsm->rightText = RIGHT_ARROW;
        menu->addChild(lsm);

        // Reset faders
        struct ResetFadersItem : MenuItem {
            Vocoder* module;
            void onAction(const event::Action& e) override {
                for (int i = 0; i < Vocoder::NUM_BANDS; i++)
                    module->params[Vocoder::BAND_LEVEL_FIRST + i].setValue(1.0f);
            }
        };
        auto* rf = new ResetFadersItem;
        rf->text = "Reset all faders to unity";
        rf->module = m;
        menu->addChild(rf);

        // Zero all faders
        struct ZeroFadersItem : MenuItem {
            Vocoder* module;
            void onAction(const event::Action& e) override {
                for (int i = 0; i < Vocoder::NUM_BANDS; i++)
                    module->params[Vocoder::BAND_LEVEL_FIRST + i].setValue(0.0f);
            }
        };
        auto* zf = new ZeroFadersItem;
        zf->text = "Set all faders to zero";
        zf->module = m;
        menu->addChild(zf);
    }

};

void VocoderPanelDraw::draw(const DrawArgs& args) {
        if (!font) return;

        NVGcontext* vg = args.vg;
        const float PX = mm2px(Vec(1.f, 0.f)).x;
        const NVGcolor WH = nvgRGB(0xff, 0xff, 0xff);
        const NVGcolor TC = nvgRGB(0xcc, 0xcc, 0xcc);
        const NVGcolor BG = nvgRGB(0x3a, 0x3d, 0x43);
        const NVGcolor DV = nvgRGB(0xe8, 0xe6, 0xde);
        const NVGcolor TK = nvgRGB(0x73, 0x76, 0x7f);
        const NVGcolor PU = nvgRGB(0x8b, 0x7c, 0xf8);
        const int MID = NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE;
        const int LFT = NVG_ALIGN_LEFT   | NVG_ALIGN_BASELINE;

        auto txt = [&](float x, float y, const char* s, float sz, NVGcolor c, int align = NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE) {
            nvgFontFaceId(vg, font->handle);
            nvgFontSize(vg, sz * PX);
            nvgTextAlign(vg, align);
            nvgFillColor(vg, c);
            nvgText(vg, x * PX, y * PX, s, nullptr);
        };

        auto drawBox = [&](float x, float y, float w, float h, float r) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x * PX, y * PX, w * PX, h * PX, r * PX);
            nvgStrokeColor(vg, DV);
            nvgStrokeWidth(vg, 0.25f * PX);
            nvgStroke(vg);
        };

        // Section divider boxes (drawn over SVG, under text)
        // FILTER, LFO, TRIGGERS, and FREEZE boxes and badge fills are now in panel SVG
        drawBox(110.3f,  100.2f,  59.9f, 21.6f, 1.5f);  // patchbay inputs

        // Knob dot markers
        // Angular constants — NanoVG 0=right, CW+. Rack knob 0=up → NanoVG = -π/2 + Rack angle.
        // RoundBlackKnob: minAngle = ±0.83π → NanoVG start = -1.33π, range = 1.66π
        // Trimpot:        minAngle = ±0.75π → NanoVG start = -1.25π, range = 1.50π
        const float kMinA = -float(M_PI) * 1.33f;
        const float kRng  =  float(M_PI) * 1.66f;
        const float tMinA = -float(M_PI) * 1.25f;
        const float tRng  =  float(M_PI) * 1.50f;

        // skipCenter omits the i==n/2 dot (12 o'clock) where labels sit above the knob
        auto knobDot = [&](float cx, float cy, float r, float dotR, int n, float minA, float rA, bool skipCenter) {
            nvgFillColor(vg, TK);
            for (int i = 0; i < n; i++) {
                if (skipCenter && i == n / 2) continue;
                float a = minA + i * rA / (n - 1);
                nvgBeginPath(vg);
                nvgCircle(vg, (cx + r * std::cos(a)) * PX, (cy + r * std::sin(a)) * PX, dotR * PX);
                nvgFill(vg);
            }
        };

        // RoundBlackKnob dots — body r ≈ 4.75mm, dots placed at r=5.85mm
        knobDot( 15.00f,  27.71f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // VCO_FREQ
        knobDot( 15.00f,  56.47f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // PULSE_WIDTH
        knobDot( 15.00f,  89.05f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // CARRIER_MIX
        knobDot( 68.65f,  98.18f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // SPECTRAL_SHIFT
        knobDot( 68.65f, 112.93f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // RESONANCE
        knobDot( 96.10f,  98.18f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // SHIFT_LFO_RATE
        knobDot( 96.10f, 112.93f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // SHIFT_LFO_AMT
        knobDot( 42.36f, 112.93f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // DECAY
        knobDot(197.96f,  27.71f, 5.85f, 0.35f, 7, kMinA, kRng, true);   // VOLUME
        // Trimpot dots — body r ≈ 2.5mm, dots at r=3.4mm; keep 12 o'clock (label clears)
        knobDot(197.96f,  80.04f, 3.40f, 0.25f, 5, tMinA, tRng, false);  // GAIN_LEVEL

        // Slider guide lines — one line spanning the gap between each adjacent pair of faders
        // Omits the outer edges of bands 0 and 15 (too close to divider borders)
        // VCVSlider: 19.843px wide → half-width ≈ 3.36mm; handle centre y: max=34.95, mid=45.92, min=56.89mm
        {
            const float sHW     = 19.84260f / (2.f * PX);  // half widget width in mm
            const float sGap    = 0.2f;                     // gap from widget edge (mm)
            const float sYs[3]  = { 34.95f, 45.92f, 56.89f };

            nvgStrokeColor(vg, TK);
            nvgStrokeWidth(vg, 0.5f * PX);
            for (int i = 0; i < Vocoder::NUM_BANDS - 1; i++) {
                float x0 = (kBX[i]     + sHW + sGap) * PX;
                float x1 = (kBX[i + 1] - sHW - sGap) * PX;
                for (float y : sYs) {
                    nvgBeginPath(vg);
                    nvgMoveTo(vg, x0, y * PX);
                    nvgLineTo(vg, x1, y * PX);
                    nvgStroke(vg);
                }
            }
        }

        // Section badge labels
        txt( 24.21f,  18.26f, "VCO",      2.30f, WH, MID);
        txt(207.84f,  18.26f, "VCA",      2.30f, WH, MID);
        txt( 22.77f,  67.37f, "CARRIER",  2.30f, WH, MID);
        txt( 54.78f,  87.73f, "EG",       2.30f, WH, MID);
        txt( 79.5f,   87.8f,  "FILTER",   2.30f, WH, MID);
        txt(107.9f,   87.8f,  "LFO",      2.30f, WH, MID);
        txt( 23.3f,   98.5f,  "TRIGGERS", 2.30f, WH, MID);

        // Title / branding

        // VOCODER title — top-left, larger, left-aligned
        txt(4.5f, 13.0f, "VOCODER", 5.5f, DV, LFT);

        // AV glyph stamp — top-left, brand purple, ~5mm tall
        {
            const float gH  = 4.0f * PX;     // glyph height
            const float gCx = 13.5f * PX;     // center x — left of panel, above VCO box
            const float gTy = 3.5f * PX;    // apex y — bottom lands ~1mm above VCO box top
            const float halfW   = gH * (15.84f / 39.f);
            const float crossY  = gTy + 0.60f * gH;
            const float crossW  = halfW * 0.55f;
            const float sw      = gH * (3.0f / 39.f);
            const float dotR    = gH * (3.4f / 39.f);
            const float dotY    = gTy - dotR - gH * (6.0f / 39.f);

            nvgSave(vg);
            nvgStrokeColor(vg, nvgRGBA(0xe8, 0xe6, 0xde, 160));
            nvgStrokeWidth(vg, sw);
            nvgLineCap(vg, NVG_ROUND);
            nvgLineJoin(vg, NVG_MITER);

            // Left leg: apex → bottom-left
            nvgBeginPath(vg);
            nvgMoveTo(vg, gCx,           gTy);
            nvgLineTo(vg, gCx - halfW,   gTy + gH);
            nvgStroke(vg);

            // Right leg: apex → bottom-right
            nvgBeginPath(vg);
            nvgMoveTo(vg, gCx,           gTy);
            nvgLineTo(vg, gCx + halfW,   gTy + gH);
            nvgStroke(vg);

            // Crossbar
            nvgBeginPath(vg);
            nvgMoveTo(vg, gCx - crossW,  crossY);
            nvgLineTo(vg, gCx + crossW,  crossY);
            nvgStroke(vg);

            // Dot above apex
            nvgBeginPath(vg);
            nvgCircle(vg, gCx, dotY, dotR);
            nvgFillColor(vg, PU);
            nvgFill(vg);

            // Scored lines flanking the glyph at crossbar height — symmetric
            const float scoreGap = 1.5f * PX;
            const float scoreLen = 7.4f * PX;
            nvgStrokeColor(vg, BG);
            nvgStrokeWidth(vg, 0.35f * PX);
            nvgLineCap(vg, NVG_BUTT);
            nvgBeginPath(vg);
            nvgMoveTo(vg, gCx - halfW - scoreGap - scoreLen + 1.5f * PX, crossY);
            nvgLineTo(vg, gCx - halfW - scoreGap,                         crossY);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, gCx + halfW + scoreGap,            crossY);
            nvgLineTo(vg, gCx + halfW + scoreGap + scoreLen, crossY);
            nvgStroke(vg);

            nvgRestore(vg);
        }

        // Bottom wordmark — Inter Light + scored lines
        if (fontLight) {
            const float wmX  = 107.14f * PX;
            const float wmY  = 126.60f * PX;
            const float wmSz = 4.2f * PX;
            const float spacing = 0.20f * wmSz;

            // Measure text width and A-advance to place dot
            float fullBounds[4], aBounds[4];
            nvgSave(vg);
            nvgFontFaceId(vg, fontLight->handle);
            nvgFontSize(vg, wmSz);
            nvgTextLetterSpacing(vg, spacing);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
            nvgTextBounds(vg, 0, 0, "ASTRAVOX", nullptr, fullBounds);
            nvgTextBounds(vg, 0, 0, "A",        nullptr, aBounds);
            nvgRestore(vg);

            const float totalW  = fullBounds[2] - fullBounds[0];
            const float aAdv    = aBounds[2];           // advance of "A" incl. spacing
            const float leftEdge = wmX - totalW * 0.5f;
            const float dotX    = leftEdge + aAdv * 0.5f;
            const float capH    = wmSz * 0.72f;
            const float dotR    = wmSz * 0.12f;
            const float dotY    = wmY - capH - dotR - wmSz * 0.08f;

            const float lineY   = wmY - capH * 0.5f;
            const float lineXL1 =   5.0f * PX;
            const float lineXL2 =  93.0f * PX;
            const float lineXR1 = 121.0f * PX;
            const float lineXR2 = 208.0f * PX;

            // Scored lines
            nvgSave(vg);
            nvgStrokeColor(vg, BG);
            nvgStrokeWidth(vg, 0.35f * PX);
            nvgBeginPath(vg);
            nvgMoveTo(vg, lineXL1, lineY);
            nvgLineTo(vg, lineXL2, lineY);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgMoveTo(vg, lineXR1, lineY);
            nvgLineTo(vg, lineXR2, lineY);
            nvgStroke(vg);
            nvgRestore(vg);

            // ASTRAVOX text
            nvgSave(vg);
            nvgFontFaceId(vg, fontLight->handle);
            nvgFontSize(vg, wmSz);
            nvgTextAlign(vg, MID);
            nvgFillColor(vg, TK);
            nvgTextLetterSpacing(vg, spacing);
            nvgText(vg, wmX, wmY, "ASTRAVOX", nullptr);
            nvgRestore(vg);

            // Purple dot above the A
            nvgBeginPath(vg);
            nvgCircle(vg, dotX, dotY, dotR);
            nvgFillColor(vg, PU);
            nvgFill(vg);
        }

        // Left column — oscillator knob labels
        txt(15.00f,  21.71f, "PITCH",       2.87f, WH, MID);
        txt(15.00f,  50.47f, "PULSE WIDTH", 2.87f, WH, MID);
        txt(15.00f,  83.05f, "MIX",         2.87f, WH, MID);

        // Left column — WAVE button labels
        txt(15.00f, 37.21f, "WAVE",  2.58f, WH, MID);
        txt( 6.50f, 40.56f, "SAW",   1.72f, WH, MID);
        txt(23.50f, 40.56f, "PULSE", 1.72f, WH, MID);

        // Left column — NOISE button labels
        txt(15.00f, 71.65f, "NOISE", 2.87f, WH, MID);
        txt( 6.50f, 75.04f, "WHITE", 1.72f, WH, MID);
        txt(23.50f, 75.04f, "PINK",  1.72f, WH, MID);

        // Left column — PROG TRIG EG + TRIGGER labels
        txt(15.00f, 102.25f, "PROG TRIG EG", 2.29f, WH, MID);
        txt(15.00f, 113.50f, "TRIGGER",      2.29f, WH, MID);

        // Filter section knob labels
        txt(68.79f,  92.46f, "FORMANT",   2.87f, WH, MID);
        txt(68.79f, 107.07f, "RESONANCE", 2.87f, WH, MID);
        txt(96.13f,  92.46f, "RATE",  2.87f, WH, MID);
        txt(96.37f, 107.07f, "DEPTH", 2.87f, WH, MID);

        // Envelope section labels
        txt(42.40f,  92.64f, "ATTACK", 2.87f, WH, MID);
        txt(34.10f,  96.12f, "FAST",   1.90f, WH, MID);
        txt(50.93f,  96.12f, "SLOW",   1.90f, WH, MID);
        txt(42.57f, 107.06f, "DECAY",  2.87f, WH, MID);

        // Right column — VCA MODE labels
        txt(197.96f, 36.57f, "VCA MODE", 2.58f, WH, MID);
        txt(189.46f, 40.10f, "ON",       1.90f, WH, MID);
        txt(206.46f, 40.10f, "EG",       1.90f, WH, MID);

        // Right column — FREEZE / VOLUME / PROGRAM LEVEL
        txt(197.96f, 54.11f, "FREEZE",  2.87f, WH, MID);
        txt(197.96f, 21.75f, "VOLUME",  2.87f, WH, MID);
        txt(197.96f, 72.29f, "PROGRAM", 2.87f, WH, MID);
        txt(197.96f, 74.76f, "LEVEL",   2.87f, WH, MID);

        // Expander label
        txt(202.79f,  6.12f, "EXPANDER", 2.29f, WH, MID);

        // Port labels — right patchbay area
        txt(197.96f,  87.00f, "PROGRAM IN", 2.50f, WH, MID);
        txt(175.96f, 103.00f, "EG",          2.50f, WH, MID);
        txt(175.75f, 113.25f, "LFO",        2.50f, WH, MID);
        txt(190.54f, 113.25f, "PROG ENV F", 2.50f, WH, MID);
        txt(191.21f, 103.00f, "EVEN",       2.50f, WH, MID);
        txt(205.47f, 103.00f, "ODD",        2.50f, WH, MID);
        txt(205.50f, 113.25f, "MIX",        2.50f, WH, MID);

        // MODE button labels
        txt(147.62f, 89.37f, "MODE",        2.87f, WH, MID);
        txt(138.79f, 92.59f, "VOCODER",     1.90f, WH, MID);
        txt(156.61f, 92.59f, "FILTER BANK", 1.90f, WH, MID);

        // Port labels — bottom patchbay row
        txt(117.88f, 103.00f, "V/OCT",    2.50f, WH, MID);
        txt(132.01f, 103.00f, "CARRIER",  2.50f, WH, MID);
        txt(146.20f, 103.00f, "FORMANT",  2.50f, WH, MID);
        txt(160.57f, 103.00f, "VCA CV",   2.50f, WH, MID);
        txt(117.83f, 113.25f, "PWM",      2.50f, WH, MID);
        txt(132.07f, 113.25f, "TRIGGER",  2.50f, WH, MID);
        txt(146.12f, 113.25f, "LFO RATE", 2.50f, WH, MID);
        txt(160.34f, 113.25f, "FREEZE",   2.50f, WH, MID);

        // Band numbers
        for (int i = 0; i < Vocoder::NUM_BANDS; i++) {
            char s[3];
            snprintf(s, sizeof(s), "%d", i + 1);
            txt(kBX[i], 60.50f, s, 1.90f, TC, MID);
        }

        // Band VCA CV port labels
        for (int i = 0; i < Vocoder::NUM_BANDS; i++) {
            char s[8];
            snprintf(s, sizeof(s), "VCA %d", i + 1);
            txt(kBX[i], 63.75f, s, 2.30f, WH, MID);
        }

        // Band ENV F output label — single centered label over the row
        txt(kBX[0] + (kBX[Vocoder::NUM_BANDS - 1] - kBX[0]) * 0.5f, 74.75f, "OUT", 2.30f, WH, MID);
}

Model* modelVocoder = createModel<Vocoder, VocoderWidget>("Vocoder");
