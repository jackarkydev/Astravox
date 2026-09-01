// SPDX-License-Identifier: GPL-3.0-only

// TapeEchoWidget.hpp — panel widgets and the module's GUI layer.
//
// NOT a standalone header: #included from inside TapeEcho.cpp, since
// everything here needs the complete `TapeEcho` type plus the panel
// constants, knob bases and ParamQuantity declared above it.
//
// Everything below runs on the GUI thread. It may read audio-thread state,
// but must never write anything process() also writes.

// Rate NUDGE — the Rate field means whatever the knob currently means: the
// nudge in percent while synced, the plain 0..1 taper position otherwise.
// Defined out of line since these read TapeEcho's published state. Legal to
// read audio-thread state here — the prohibition is only on process() writing
// the param, so typed entry still patch-saves, undoes and automates correctly.
std::string TapeEchoModeQuantity::getDisplayValueString() {
    // dynamic_cast is nullptr for browser-preview widgets (Code Standards) — guard it.
    TapeEcho* m = dynamic_cast<TapeEcho*>(module);
    if (!m || !m->sync.snapModeOn || labels.empty())
        return SwitchQuantity::getDisplayValueString();
    // While synced, show only what's actually playing — the parked mode is
    // unchanged underneath (setDisplayValueString() is deliberately not
    // overridden, so typing still sets it) but isn't worth restating on every
    // hover. It reappears, pointer and tooltip together, the moment sync
    // disengages.
    int snapped = clamp((int)std::round(m->sync.snapModePos), 0, (int)labels.size() - 1);
    return labels[snapped];
}

float TapeEchoRateQuantity::getDisplayValue() {
    TapeEcho* m = dynamic_cast<TapeEcho*>(module);
    // Achieved while sync is running (the requested trim can clamp near a
    // taper end stop). Falls back to the requested value when sync isn't
    // running — only reached by automation or a patch inspector then.
    if (m && m->sync.nudgeOn)
        return m->sync.nudgePct;
    return (ParamQuantity::getValue() - 0.5f) * 2.f * CLOCK_NUDGE_MAX_PCT;
}

void TapeEchoRateQuantity::setDisplayValue(float displayValue) {
    float pct = clamp(displayValue, -CLOCK_NUDGE_MAX_PCT, CLOCK_NUDGE_MAX_PCT);
    setImmediateValue(0.5f + 0.5f * pct / CLOCK_NUDGE_MAX_PCT);
}

std::string TapeEchoRateQuantity::getDisplayValueString() {
    float pct = getDisplayValue();
    // Number only — Rack appends getUnit() itself. Dead centre reads as
    // "0.00", not "+0.00" (on the grid is a state, not a trim direction);
    // everything else carries an explicit sign.
    if (std::fabs(pct) < 0.005f)
        return "0.00";
    return string::f("%+.2f", pct);
}

void TapeEchoRateQuantity::setDisplayValueString(std::string s) {
    // Parsed here rather than Quantity's sscanf, since that also tries to read
    // an SI prefix off the suffix. Convention: a bare number is positive
    // ("3" means +3%); strtof also accepts a leading "+" and trailing "%", so
    // a value copied out of the field pastes back unchanged.
    const char* c = s.c_str();
    char* end = nullptr;
    float v = std::strtof(c, &end);
    if (end == c)
        return;     // unparseable — leave the nudge where it is
    setDisplayValue(v);
}

std::string TapeEchoRateQuantity::getUnit() {
    return "%";
}


// VU meter face — overlays the silkscreen meter face. The SVG owns frame,
// ticks, glow, sheen, pivot dot, LED housing outline; this widget only paints
// the rotating needle line. Pivot at panel (150, 73).
struct TapeEchoVUMeter : TransparentWidget {
    TapeEcho* module = nullptr;
    Vec pivot;          // in box-local px
    float arcRad;       // needle length in px
    float minAngle;     // needle angle at norm=0
    float maxAngle;     // needle angle at norm=1

    TapeEchoVUMeter() {
        // Box spans the full bezel-outer extent, not just the glass, so the
        // bezel glow below has room to draw without clipping; pivot/scrim are
        // shifted by the same (4,4) margin to match.
        pivot    = Vec(34.f, 39.f);                          // panel (150,73) - box(116,34)
        arcRad   = 28.f;                                     // reaches the outer tick ring
        minAngle = -float(M_PI) * 0.5f - 0.72f;              // ≈ -41° from up (left)
        maxAngle = -float(M_PI) * 0.5f + 0.80f;              // ≈ +46° from up (right purple)
    }

    void draw(const DrawArgs& args) override {
        float norm = 0.f;
        if (module) norm = clamp(module->vu.level / VU_PIN, 0.f, 1.f);
        else        norm = 0.20f;   // visible needle in browser preview

        // Meter "power" — follows tapeSpeed so the meter darkens and the needle
        // fades when the motor stops, then fades back up when power resumes.
        // TAPER_SPEED[0] = slowest measured speed; matches the audio-path motorGain ramp.
        float meterPower = 1.f;
        if (module) meterPower = clamp(module->tapeSpeed / TAPER_SPEED[0], 0.f, 1.f);

        // Display-only skew (see VU_NEEDLE_DISPLAY_GAMMA) — needle angle only,
        // doesn't feed back into norm/vu.level for anything else.
        float displayNorm = std::pow(norm, VU_NEEDLE_DISPLAY_GAMMA);
        float angle = rescale(displayNorm, 0.f, 1.f, minAngle, maxAngle);
        float tipX  = pivot.x + std::cos(angle) * arcRad;
        float tipY  = pivot.y + std::sin(angle) * arcRad;

        NVGcontext* vg = args.vg;

        // Warm backlight bleed into the bezel — same warm tone as the needle
        // halo, spilling past the frame like the real unit's lamp. Boxed to
        // the glass rect with a transparent inner color so only the margin
        // ring picks up the tint, not the glass/labels themselves.
        unsigned char bezelA = (unsigned char)(70.f * meterPower);
        if (bezelA > 0) {
            NVGpaint bezelGlow = nvgBoxGradient(vg, 4.f, 4.f, 60.f, 38.f, 2.f, 8.f,
                nvgRGBA(255, 230, 180, 0), nvgRGBA(255, 230, 180, bezelA));
            nvgBeginPath(vg);
            nvgRect(vg, 0.f, 0.f, 68.f, 46.f);
            nvgFillPaint(vg, bezelGlow);
            nvgFill(vg);
        }

        // Dark scrim — overlays the SVG glow/sheen/ticks to simulate the lights
        // going out. Fully opaque at meterPower=0, invisible at meterPower=1.
        // Covers just the glass (box-local (4,4)-(64,42)), not the wider bezel
        // margin the widget's box now also spans.
        if (meterPower < 0.999f) {
            unsigned char scrimA = (unsigned char)((1.f - meterPower) * 205.f);
            nvgBeginPath(vg);
            nvgRect(vg, 4.f, 4.f, 60.f, 38.f);
            nvgFillColor(vg, nvgRGBA(0, 0, 0, scrimA));
            nvgFill(vg);
        }

        // Soft warm halo behind the needle — alpha scales with meter power.
        unsigned char haloA = (unsigned char)(52.f * meterPower);
        if (haloA > 0) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, pivot.x, pivot.y);
            nvgLineTo(vg, tipX, tipY);
            nvgStrokeColor(vg, nvgRGBA(255, 230, 180, haloA));
            nvgStrokeWidth(vg, 3.6f);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);
        }
        // Cream needle core — fades to invisible when motor is fully stopped.
        unsigned char coreA = (unsigned char)(255.f * meterPower);
        if (coreA > 0) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, pivot.x, pivot.y);
            nvgLineTo(vg, tipX, tipY);
            nvgStrokeColor(vg, nvgRGBA(188, 180, 161, coreA));
            nvgStrokeWidth(vg, 1.4f);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);
        }
    }
};

// Peak LED — paints just the lit interior + halo of the housing at panel (126,68).
// The SVG already draws the white r=3.5 housing outline; this widget overlays it.
struct TapeEchoPeakLED : TransparentWidget {
    TapeEcho* module = nullptr;

    void draw(const DrawArgs& args) override {
        NVGcontext* vg = args.vg;
        float r  = box.size.x * 0.5f;
        float cx = r, cy = r;
        float lit = module ? clamp(module->vu.peakLed, 0.f, 1.f) : 0.f;
        // Fade with motor power so the LED dims when the meter "powers down".
        float meterPower = module ? clamp(module->tapeSpeed / TAPER_SPEED[0], 0.f, 1.f) : 1.f;
        lit *= meterPower;

        // Halo (extends well beyond the housing).
        if (lit > 0.02f) {
            NVGpaint halo = nvgRadialGradient(vg, cx, cy, r * 0.5f, r * 3.2f,
                nvgRGBA(0xff, 0x40, 0x30, (unsigned char)(lit * 160)),
                nvgRGBA(0xff, 0x40, 0x30, 0));
            nvgBeginPath(vg);
            nvgRect(vg, cx - r * 3.2f, cy - r * 3.2f, r * 6.4f, r * 6.4f);
            nvgFillPaint(vg, halo);
            nvgFill(vg);
        }

        // Lit interior — sits inside the SVG housing outline.
        NVGcolor off = nvgRGB(0x28, 0x10, 0x10);
        NVGcolor on  = nvgRGB(0xff, 0x50, 0x30);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r * 0.82f);
        nvgFillColor(vg, nvgLerpRGBA(off, on, lit));
        nvgFill(vg);

        // Hot spot
        if (lit > 0.10f) {
            nvgBeginPath(vg);
            nvgCircle(vg, cx - r * 0.20f, cy - r * 0.22f, r * 0.32f);
            nvgFillColor(vg, nvgRGBA(0xff, 0xc0, 0xa0, (unsigned char)(lit * 220)));
            nvgFill(vg);
        }
    }
};

// Rate knob with a display-only clock-snap indicator — while sync is active
// the graphic jumps between reachable divisions while the param stays
// continuous, without the module ever writing the param from the audio thread.
struct TapeEchoRateKnob : TapeEchoLargeKnob {
    TapeEcho* mod = nullptr;
    float getDisplayValue() override {
        if (mod && mod->sync.snapDisplayOn) return mod->sync.snapKnobPos;
        return TapeEchoLargeKnob::getDisplayValue();
    }
};
// NOTE: the override above is currently unreachable — this knob is hidden
// under exactly the condition that triggers it, since the nudge knob takes
// its place while synced. Retained, not deleted, since swapping it back is a
// visibility decision, not a rewrite.

// The Rate NUDGE knob. Stacked on the Rate knob at the same spot; exactly one
// of the two is visible, swapped by sync state in step(). Its centre is the
// grid, so while synced the knob sits at 12 o'clock whenever locked.
struct TapeEchoNudgeKnob : TapeEchoLargeKnob {};

// GANG — Intensity is the handle, the rate axis follows. The whole linkage
// lives here, in step(), on the GUI thread, and works by observing the handle
// rather than hooking a setter — so no param is written from process(), and
// drag/typed-entry/MIDI-mapping all link identically.
//
// The follower is whichever param the rate axis currently is — Rate when
// free, the nudge when synced. Grabbing the follower directly moves only the
// follower, so the nudge stays a precision timing trim with Gang armed.
//
// Bypass: hold RACK_MOD_CTRL while dragging Intensity to reposition it
// without moving the follower.
static constexpr float GANG_RETURN_TAU = 0.18f;   // spring-back time constant, s

struct TapeEchoIntensityKnob : TapeEchoLargeKnob {
    TapeEcho* mod = nullptr;
    bool   anchored = false;
    float  h0 = 0.f, f0 = 0.f;          // the gesture's anchor — its return point
    float  lastH = 0.f, lastF = 0.f;
    int    lastFollowerId = -1;
    bool   returning = false;
    double lastTime  = 0.0;

    bool syncOverride = false;    // this widget owns mod->gangSyncOverride

    int followerId() const {
        // A synced Gang gesture still drives the NUDGE param, just with a
        // wider mapping to speed while it runs.
        return (mod && mod->sync.nudgeOn) ? TapeEcho::RATE_NUDGE_PARAM
                                          : TapeEcho::REPEAT_RATE_PARAM;
    }
    // Narrows the mapping back to ±6% — nothing to restore, since the
    // override only ever changed how process() reads the nudge param.
    void endSyncOverride() {
        if (!mod || !syncOverride) return;
        mod->gangSyncOverride = false;
        syncOverride = false;
    }
    // Anchor this gesture at the moment of the grab — capturing it once and
    // persisting across gestures would make spring-back return to wherever
    // the relationship was first established, not to where this grab started.
    void captureAnchor() {
        if (!mod) return;
        ParamQuantity* hq = getParamQuantity();
        if (!hq) return;
        const int fid = followerId();
        ParamQuantity* fq = mod->paramQuantities[fid];
        if (!fq) return;
        h0 = lastH = hq->getValue();
        f0 = lastF = fq->getValue();
        lastFollowerId = fid;
        anchored = true;
    }
    void onDragStart(const DragStartEvent& e) override {
        returning = false;      // grabbing it again cancels a spring-back
        // Bypass gesture (RACK_MOD_CTRL held at grab): never engages Gang, so
        // it must not widen the nudge mapping either.
        bool bypass = (APP->window->getMods() & RACK_MOD_MASK) == RACK_MOD_CTRL;
        // Synced + armed: widen the nudge knob's mapping for this gesture so
        // the twist gets the full speed range. Only a bool crosses threads;
        // process() captures the anchors itself on the rising edge.
        if (!bypass && mod && mod->gangEnabled && mod->sync.nudgeOn && !syncOverride) {
            mod->gangSyncOverride = true;
            syncOverride = true;
        }
        captureAnchor();
        TapeEchoLargeKnob::onDragStart(e);
    }
    void onDragEnd(const DragEndEvent& e) override {
        TapeEchoLargeKnob::onDragEnd(e);
        // syncOverride forces the return regardless of the menu — see the menu
        // item, which shows ticked and greyed while synced for exactly this.
        if (mod && mod->gangEnabled && anchored && (mod->gangMomentary || syncOverride))
            returning = true;
        else
            endSyncOverride();  // nothing will bring it back otherwise
    }
    void step() override {
        TapeEchoLargeKnob::step();
        double now = system::getTime();
        double dt  = now - lastTime;
        if (dt < 0.0)  dt = 0.0;        // first frame / clock step
        if (dt > 0.1)  dt = 0.1;        // don't let a stalled GUI jump the glide
        lastTime = now;
        if (!mod) return;
        ParamQuantity* hq = getParamQuantity();
        if (!hq) return;
        const int fid = followerId();
        ParamQuantity* fq = mod->paramQuantities[fid];
        if (!fq) return;
        float h = hq->getValue();
        float f = fq->getValue();

        if (!mod->gangEnabled) {
            // Disarmed mid-gesture — hand the speed back, or sync stays
            // suspended with nothing left to resume it.
            endSyncOverride();
            anchored = false; returning = false;
            lastH = h; lastF = f; lastFollowerId = fid;
            return;
        }

        if (returning) {
            // Both ends glide home together. setImmediateValue, not setValue —
            // this computes both sides itself and must not re-enter the
            // observer below.
            float k  = std::exp(-(float)dt / GANG_RETURN_TAU);
            float nh = h0 + (h - h0) * k;
            float nf = f0 + (f - f0) * k;
            bool landed = (std::fabs(nh - h0) < 1e-4f && std::fabs(nf - f0) < 1e-4f);
            if (landed) { nh = h0; nf = f0; returning = false; }
            hq->setImmediateValue(nh);
            fq->setImmediateValue(nf);
            lastH = nh; lastF = nf; lastFollowerId = fid;
            // The glide returned the nudge param to its anchor, and the widened
            // mapping passes through that anchor by construction — so the tape is
            // already exactly where the narrow mapping puts it. Continuous.
            if (landed) endSyncOverride();
            return;
        }

        // Bypass: hold RACK_MOD_CTRL to move Intensity without engaging Gang.
        // Forces the "follower moved independently" escape hatch below every
        // frame it's held, so releasing the modifier re-anchors at the
        // current position with no jump. Checked live every frame so it can
        // be pressed/released mid-gesture without breaking the drag.
        if ((APP->window->getMods() & RACK_MOD_MASK) == RACK_MOD_CTRL) {
            anchored = false;
            lastH = h; lastF = f; lastFollowerId = fid;
            return;
        }

        // (Re)anchor when the pairing is new, when sync moved the rate axis to
        // the other param, or when the user moved the follower directly — that
        // last one is the escape hatch for re-zeroing the relationship.
        if (!anchored || fid != lastFollowerId || std::fabs(f - lastF) > 1e-6f) {
            h0 = h; f0 = f; anchored = true;
            lastH = h; lastF = f; lastFollowerId = fid;
            return;
        }
        if (std::fabs(h - lastH) < 1e-9f)
            return;                     // handle hasn't moved — nothing to do

        // Proportional scaling: each side maps its own remaining travel onto
        // the gesture, so both arrive at their ends together. Sweeping up and
        // back within one gesture retraces exactly; across separate grabs it
        // does not, by design (each grab re-anchors).
        const float hMin = hq->getMinValue(), hMax = hq->getMaxValue();
        const float fMin = fq->getMinValue(), fMax = fq->getMaxValue();
        float nf;
        if (h >= h0) {
            float d = hMax - h0;
            nf = (d > 1e-6f) ? f0 + (h - h0) / d * (fMax - f0) : f0;
        } else {
            float d = h0 - hMin;
            nf = (d > 1e-6f) ? f0 - (h0 - h) / d * (f0 - fMin) : f0;
        }
        nf = clamp(nf, fMin, fMax);
        fq->setImmediateValue(nf);
        lastH = h; lastF = nf;
    }
};

// Mode pointer follows the head sync selected, while MODE_PARAM stays
// where the user parked it. Display-only — see the race note in the clock-sync
// block. Only single-head parked modes set clockSnapModeOn.
struct TapeEchoSyncModeKnob : TapeEchoModeKnob {
    TapeEcho* mod = nullptr;
    float getDisplayValue() override {
        if (mod && mod->sync.snapModeOn) return mod->sync.snapModePos;
        return TapeEchoModeKnob::getDisplayValue();
    }
};

struct TapeEchoWidget : ModuleWidget {
    TapeEchoWidget(TapeEcho* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/TapeEcho.svg")));

        // 4 corner screws
        addChild(createWidget<ScrewBlack>(Vec(0.f, 0.f)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - RACK_GRID_WIDTH, 0.f)));
        addChild(createWidget<ScrewBlack>(Vec(0.f, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewBlack>(Vec(box.size.x - RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Top row — REVERSE / POWER toggle switches + paired red LEDs.
        // Positions lifted from res/TapeEcho_design.svg.
        // Both toggles are momentary (one pulse per click); the DSP's edge-
        // detector toggles the persisted *Latched bool. Visual state reads
        // from that bool (or the param value when in momentary-mode context).
        {
            auto* reverseToggle = createParamCentered<TapeEchoToggle>(Vec(66.f, 41.f), module, TapeEcho::REVERSE_PARAM);
            reverseToggle->litStatePtr      = module ? &module->reverseLatched   : nullptr;
            reverseToggle->momentaryFlagPtr = module ? &module->reverseMomentary : nullptr;
            addParam(reverseToggle);

            auto* reverseLED = new TapeEchoRedLED();
            reverseLED->trackedModule    = module;
            reverseLED->trackedParamId   = TapeEcho::REVERSE_PARAM;
            reverseLED->litStatePtr      = module ? &module->reverseLatched   : nullptr;
            reverseLED->momentaryFlagPtr = module ? &module->reverseMomentary : nullptr;
            reverseLED->box.pos = Vec(37.f - 4.f, 41.f - 4.f);
            addChild(reverseLED);
        }
        {
            auto* powerToggle = createParamCentered<TapeEchoToggle>(Vec(234.f, 41.f), module, TapeEcho::MOTOR_STOP_PARAM);
            powerToggle->litStatePtr      = module ? &module->motorStopLatched   : nullptr;
            powerToggle->invertDisplay    = true;     // POWER lit when motor running (latched == false)
            addParam(powerToggle);

            auto* powerLED = new TapeEchoRedLED();
            powerLED->trackedModule    = module;
            powerLED->trackedParamId   = TapeEcho::MOTOR_STOP_PARAM;
            powerLED->litStatePtr      = module ? &module->motorStopLatched   : nullptr;
            powerLED->invertDisplay    = true;
            powerLED->box.pos = Vec(263.f - 4.f, 41.f - 4.f);
            addChild(powerLED);
        }

        // BASS / TREBLE (small knobs, r=9) + INPUT (large, r=14) + IN jack
        addParam(createParamCentered<TapeEchoSmallKnob>(Vec(215.f, 88.f), module, TapeEcho::BASS_PARAM));
        addParam(createParamCentered<TapeEchoSmallKnob>(Vec(263.f, 88.f), module, TapeEcho::TREBLE_PARAM));
        addParam(createParamCentered<TapeEchoLargeKnob>(Vec(78.f, 88.f), module, TapeEcho::INPUT_LEVEL_PARAM));
        addInput(createInputCentered<PJ301MPort>(Vec(37.f, 88.f), module, TapeEcho::IN_INPUT));

        // VU meter (overlays SVG silkscreen). Box spans the full bezel-outer
        // extent (116..184 x 34..80, not just the glass 120..180 x 38..76) so
        // the bezel glow has room to draw without being clipped at the
        // widget's own box edge.
        {
            auto* vu = new TapeEchoVUMeter();
            vu->module    = module;
            vu->box.pos   = Vec(116.f, 34.f);
            vu->box.size  = Vec(68.f, 46.f);
            addChild(vu);
        }
        // Peak LED — SVG housing at (126, 68) r=3.5, in the meter's lower-left
        // corner. Widget covers that diameter.
        {
            auto* led = new TapeEchoPeakLED();
            led->module    = module;
            led->box.pos   = Vec(126.f - 3.5f, 68.f - 3.5f);
            led->box.size  = Vec(7.f, 7.f);
            addChild(led);
        }

        // Main row of knobs (RATE / INTENSITY / ECHO / REVERB) + paired CV jacks
        {
            auto* rk = createParamCentered<TapeEchoRateKnob>(Vec(78.f,  144.f), module, TapeEcho::REPEAT_RATE_PARAM);
            rk->mod = module;
            addParam(rk);
            rateKnob = rk;
            // Same position — see TapeEchoNudgeKnob. Added second so it draws
            // and hit-tests above the Rate knob when both are momentarily
            // visible; step() keeps exactly one of them shown.
            auto* nk = createParamCentered<TapeEchoNudgeKnob>(Vec(78.f,  144.f), module, TapeEcho::RATE_NUDGE_PARAM);
            nk->visible = false;
            addParam(nk);
            nudgeKnob = nk;
        }
        {
            auto* ik = createParamCentered<TapeEchoIntensityKnob>(Vec(126.f, 144.f), module, TapeEcho::INTENSITY_PARAM);
            ik->mod = module;
            addParam(ik);
        }
        addParam(createParamCentered<TapeEchoLargeKnob>(Vec(174.f, 144.f), module, TapeEcho::ECHO_VOLUME_PARAM));
        addParam(createParamCentered<TapeEchoLargeKnob>(Vec(222.f, 144.f), module, TapeEcho::REVERB_VOLUME_PARAM));
        addInput(createInputCentered<PJ301MPort>(Vec(78.f,  175.f), module, TapeEcho::RATE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(126.f, 175.f), module, TapeEcho::INTENSITY_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(174.f, 175.f), module, TapeEcho::ECHO_VOLUME_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(222.f, 175.f), module, TapeEcho::REVERB_VOLUME_CV_INPUT));

        // MODE rotary
        {
            auto* mk = createParamCentered<TapeEchoSyncModeKnob>(Vec(217.5f, 265.4f), module, TapeEcho::MODE_PARAM);
            mk->mod = module;
            addParam(mk);
        }

        // Feedback insert + utility ports (bottom-left cluster). Top row at y=273,
        // bottom row at y=305 (top row moved up 2 px to clear label collisions).
        // Outputs use AstravoxPort (custom NanoVG port) for visual distinction.
        addOutput(createOutputCentered<AstravoxPort>(Vec(37.5f,  273.f), module, TapeEcho::FB_SEND_OUTPUT));
        addInput (createInputCentered<PJ301MPort> (Vec(37.5f,  305.f), module, TapeEcho::FB_RETURN_INPUT));
        addInput (createInputCentered<PJ301MPort> (Vec(82.5f,  273.f), module, TapeEcho::REVERSE_INPUT));
        addInput (createInputCentered<PJ301MPort> (Vec(82.5f,  305.f), module, TapeEcho::TAPE_AGE_INPUT));
        addInput (createInputCentered<PJ301MPort> (Vec(127.5f, 273.f), module, TapeEcho::MODE_INPUT));
        addInput (createInputCentered<PJ301MPort> (Vec(127.5f, 305.f), module, TapeEcho::CLOCK_INPUT));

        // Bottom row — head outs + H/M/L switch + main OUT (all AstravoxPort).
        addOutput(createOutputCentered<AstravoxPort>(Vec(37.5f,  353.f), module, TapeEcho::OUT_H1_OUTPUT));
        addOutput(createOutputCentered<AstravoxPort>(Vec(82.5f,  353.f), module, TapeEcho::OUT_H2_OUTPUT));
        addOutput(createOutputCentered<AstravoxPort>(Vec(127.5f, 353.f), module, TapeEcho::OUT_H3_OUTPUT));
        {
            auto* hml = createParam<TapeEchoHMLSwitch>(Vec(197.f, 346.5f), module, TapeEcho::OUTPUT_PAD_PARAM);
            addParam(hml);
        }
        addOutput(createOutputCentered<AstravoxPort>(Vec(263.f, 353.f), module, TapeEcho::OUT_OUTPUT));
    }

    ParamWidget* rateKnob  = nullptr;
    ParamWidget* nudgeKnob = nullptr;

    // Swap the Rate knob for the nudge knob while sync is running. GUI thread,
    // reading an audio-thread-published flag — no param is written from either
    // side, which is what keeps this clear of any param-write race.
    void step() override {
        auto* m = dynamic_cast<TapeEcho*>(module);
        bool synced = m && m->sync.snapDisplayOn;
        if (rateKnob && nudgeKnob && rateKnob->isVisible() == synced) {
            rateKnob->visible  = !synced;
            nudgeKnob->visible = synced;
        }
        // Keep the reverse-reverb swell matched to the current division. Must
        // happen here, on the GUI thread — prepareReversedNow() allocates and
        // runs FFTs, so process() can never do it. Gated by a 5% deadband and
        // walkingFade < 0.01 (silent reversed path) so a rebuild never chops
        // an audible swell. `!m->ecoMode` matters: without it a wandering
        // clock could trigger a discarded rebuild at up to 60Hz in Eco mode.
        if (m && m->reverbFollowsReverse && !m->ecoMode && m->spring.ready
            && m->reverse.fade < 0.01f) {
            const int target = (int)m->reversedSwellTarget;
            const int have   = m->spring.reversedLen;
            if (target > 256 &&
                (!m->spring.reversedReady() || std::abs(target - have) > have / 20))
                m->spring.prepareReversedNow(target);
        }
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        TapeEcho* m = dynamic_cast<TapeEcho*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Eco mode (4× saturation oversample)", "", &m->ecoMode));
        menu->addChild(createBoolPtrMenuItem("Machine noise floor",                 "", &m->machineNoiseFloor));
        menu->addChild(createBoolPtrMenuItem("Reverse is momentary",                "", &m->reverseMomentary));
        // Gang. Intensity is the handle and the rate axis follows — never the
        // other way round, since the Rate knob already means two things.
        menu->addChild(createBoolPtrMenuItem("Gang Intensity \u2192 Rate",             "", &m->gangEnabled));
        // Spring-back is not optional while synced — shown ticked and greyed
        // rather than hidden, so the forcing is visible, not mysterious.
        {
            const bool syncedNow = m->sync.snapDisplayOn;
            auto* gm = createCheckMenuItem("Gang springs back on release", "",
                [m]() { return m->gangMomentary || m->sync.snapDisplayOn; },
                [m]() { m->gangMomentary = !m->gangMomentary; });
            gm->disabled = syncedNow;
            menu->addChild(gm);
        }
        // Not a plain createBoolPtrMenuItem — turning this on needs to also
        // lazily build convReversed the first time, on this UI-thread click,
        // never inside process().
        menu->addChild(createCheckMenuItem("Reverb follows Reverse (off in Eco mode)", "",
            [m]() { return m->reverbFollowsReverse; },
            [m]() {
                m->reverbFollowsReverse = !m->reverbFollowsReverse;
                if (m->reverbFollowsReverse && !m->ecoMode && !m->spring.reversedReady())
                    m->spring.prepareReversedNow(
                        (int)(m->reversedSwellTarget > 256.f ? m->reversedSwellTarget
                                                            : TapeEcho::REV_SWELL_FREE_SEC * APP->engine->getSampleRate()));
            }));
        // Absolute note-value selection. Unreachable divisions are greyed
        // with the reason, rather than hidden, since the reachable window
        // slides with tempo.
        menu->addChild(createSubmenuItem("Tempo Sync",
            CLOCK_DIV_NAMES[clamp(m->clockDivisionIdx, 0, TapeEcho::CLOCK_N_DIVISIONS - 1)],
            [m](Menu* sub) {
                const int curMode = clamp((int)std::round(m->params[TapeEcho::MODE_PARAM].getValue()), 0, 11);
                if (!m->clock.hasPeriod || !m->inputs[TapeEcho::CLOCK_INPUT].isConnected()) {
                    sub->addChild(createMenuLabel("Patch a clock to enable"));
                }
                // Source BPM leads, since that's the stable real-world fact —
                // it doesn't change when the multiplier does. The resulting
                // sync tempo follows in parentheses.
                const float bpm    = (m->effectiveBeat() > 1e-4f) ? 60.f / m->effectiveBeat() : 0.f;
                const float rawBpm = (m->clock.beatPeriod > 1e-4f) ? 60.f / m->clock.beatPeriod : 0.f;
                const int   mi     = clamp(m->clockMultIdx, 0, CLOCK_N_MULT - 1);
                if (bpm > 0.f) {
                    sub->addChild(createMenuLabel(
                        (mi == CLOCK_MULT_DEFAULT)
                            ? string::f("Clock: %.1f BPM", rawBpm)
                            : string::f("Clock: %.1f BPM  (%s = %.1f BPM)", rawBpm,
                                        CLOCK_MULT_NAMES[mi], bpm)));
                }
                // One conditional line, shown only when what you hear isn't
                // what you picked — otherwise the fallback substitution is invisible.
                {
                    int nReach = 0;
                    for (int i = 0; i < TapeEcho::CLOCK_N_DIVISIONS; i++)
                        if (m->syncHeadFor(i, curMode) >= 0) nReach++;
                    const int req = clamp(m->clockDivisionIdx, 0, TapeEcho::CLOCK_N_DIVISIONS - 1);
                    const int eff = clamp(m->sync.effectiveIdx, 0, TapeEcho::CLOCK_N_DIVISIONS - 1);
                    if (nReach == 0)
                        sub->addChild(createMenuLabel(
                            string::f("Sync off — nothing reachable at %s", CLOCK_MULT_NAMES[mi])));
                    else if (eff != req)
                        sub->addChild(createMenuLabel(
                            string::f("Playing %s — %s unreachable",
                                      CLOCK_DIV_NAMES[eff], CLOCK_DIV_NAMES[req])));
                }
                sub->addChild(createSubmenuItem("Clock multiplier", CLOCK_MULT_NAMES[mi],
                    [m](Menu* mm) {
                        for (int k = 0; k < CLOCK_N_MULT; k++)
                            mm->addChild(createCheckMenuItem(CLOCK_MULT_NAMES[k], "",
                                [m, k]() { return m->clockMultIdx == k; },
                                [m, k]() { m->clockMultIdx = k; }));
                    }));
                sub->addChild(new MenuSeparator);
                for (int i = 0; i < TapeEcho::CLOCK_N_DIVISIONS; i++) {
                    const int head = m->syncHeadFor(i, curMode);
                    std::string right;
                    if (head < 0) {
                        // Long divisions fall off the bottom of the speed
                        // range, short ones off the top.
                        const float d = m->CLOCK_DIVISIONS[i] * m->effectiveBeat();
                        const float maxReach = m->GRID_FRACTIONS[2] * m->BASE_LOOP_SECONDS
                                               / m->TAPE_SPEED_MIN;
                        if (bpm > 0.f)
                            right = (d > maxReach) ? string::f("too long at %.0f BPM", bpm)
                                                   : string::f("too short at %.0f BPM", bpm);
                        else
                            right = "no clock";
                    } else {
                        right = string::f("Tap %d", head + 1);
                    }
                    // `disabled` already blocks the click, so this re-check
                    // only covers the stale-snapshot case: a Rack menu built
                    // on open never refreshes if the mode changes via
                    // MODE_INPUT while it's up.
                    MenuItem* it = createCheckMenuItem(CLOCK_DIV_NAMES[i], right,
                        [m, i]() { return m->clockDivisionIdx == i; },
                        [m, i]() {
                            const int md = clamp((int)std::round(
                                m->params[TapeEcho::MODE_PARAM].getValue()), 0, 11);
                            if (m->syncHeadFor(i, md) >= 0) m->clockDivisionIdx = i;
                        });
                    if (head < 0) it->disabled = true;
                    sub->addChild(it);
                }
            }));
        menu->addChild(createBoolPtrMenuItem("Output: wet only (effects send)",     "", &m->dryDefeat));
        menu->addChild(createBoolPtrMenuItem("Tone in feedback loop (per-repeat)",  "", &m->toneInLoop));
        menu->addChild(createSubmenuItem("Send/Return", "", [m](Menu* sub) {
            sub->addChild(createMenuLabel("Insert point"));
            sub->addChild(createCheckMenuItem("In-loop (compounds)", "",
                [m]() { return !m->fbInsertPostLoop; },
                [m]() { m->fbInsertPostLoop = false; }));
            sub->addChild(createCheckMenuItem("Output only (isolated)", "",
                [m]() { return m->fbInsertPostLoop; },
                [m]() { m->fbInsertPostLoop = true; }));
            sub->addChild(new MenuSeparator);
            sub->addChild(createMenuLabel("Return mode"));
            sub->addChild(createCheckMenuItem("Replace", "",
                [m]() { return !m->fbReturnBlend; },
                [m]() { m->fbReturnBlend = false; }));
            sub->addChild(createCheckMenuItem("Sum", "",
                [m]() { return m->fbReturnBlend; },
                [m]() { m->fbReturnBlend = true; }));
        }));

        // Tape Age preset (baseline when no CV is patched)
        struct AgePreset { const char* name; float value; };
        static const AgePreset presets[] = {
            {"Mint",          0.00f},
            {"Slightly aged", 0.15f},
            {"Lightly worn",  0.30f},
            {"Worn",          0.50f},
            {"Well-worn",     0.60f},
            {"Thrashed",      0.80f},
            {"Dumpster",      1.00f},
        };
        menu->addChild(createSubmenuItem("Tape age preset", "", [m](Menu* sub) {
            for (const auto& p : presets) {
                float v = p.value;
                sub->addChild(createCheckMenuItem(p.name, "",
                    [m, v]() { return std::fabs(m->tapeAgePreset - v) < 1e-4f; },
                    [m, v]() { m->tapeAgePreset = v; }));
            }
        }));


        // Drive tilt — scales the Input drive knob's saturation range,
        // volume swing, and HF rolloff together.
        struct TiltOption { const char* name; int value; };
        static const TiltOption tiltOptions[] = {
            {"Gentle",     0},
            {"Moderate",   1},
            {"Aggressive", 2},
        };
        menu->addChild(createSubmenuItem("Drive tilt", "", [m](Menu* sub) {
            for (const auto& p : tiltOptions) {
                int v = p.value;
                sub->addChild(createCheckMenuItem(p.name, "",
                    [m, v]() { return m->driveTiltMode == v; },
                    [m, v]() { m->driveTiltMode = v; }));
            }
        }));
    }
};