#pragma once
#include <rack.hpp>

using namespace rack;

// Declare each module Model — add one line per module
extern Plugin* pluginInstance;

extern Model* modelVocoder;
extern Model* modelTapeEcho;

// ── Vocoder ↔ VocoderExpander shared message struct ──────────────────────────

static const int MAX_BANDS = 24;

struct VocoderExpanderMessage {
	bool  connected           = false;
	int   numBands            = 16;                  // 16 or 24
	float bandEnv[MAX_BANDS]  = {};                  // core → expander: raw envelope CVs
	float bandHz[MAX_BANDS]   = {};                  // core → expander: computed center freqs
	bool  bandMuted[MAX_BANDS] = {};                 // core → expander: mute states
	bool  globalFrozen        = false;
	// Expander → core (filled by expander's process())
	float bandCvIn[MAX_BANDS]    = {};               // per-band synthesis VCA override
	bool  bandFreezeIn[MAX_BANDS] = {};              // per-band selective freeze
	bool  expanderPresent        = false;            // expander sets true each frame
};

// ── BandMeterLight — 6-segment teal→purple LED column ────────────────────────
// One instance per band, driven by a single float brightness (0–1).
// Segments fill bottom-up; color gradient: teal (low) → purple (high).
// box.size matches SVG column: 8px wide × 72px tall (6 × 12px segments).
static const int NUM_METER_SEGS = 6;

struct BandMeterLight : ModuleLightWidget {
	BandMeterLight() {
		box.size = Vec(8.f, 72.f);   // px — matches SVG ghost-outline column exactly
		addBaseColor(nvgRGBf(1.f, 1.f, 1.f));   // white base → color.a = brightness
	}

	void drawLight(const DrawArgs& args) override {
		float brightness = color.a;   // 0–1 from module's light value
		float segH  = box.size.y / NUM_METER_SEGS;   // 12px per segment
		float inset = 0.75f;   // inset on each edge — SVG stroke outline shows through

		for (int s = 0; s < NUM_METER_SEGS; s++) {
			float t = (float)s / (NUM_METER_SEGS - 1);   // 0=bottom teal, 1=top purple

			// Teal: rgb(93,202,165)   Purple: rgb(139,124,248)
			float r = 0.365f + t * 0.180f;
			float g = 0.792f - t * 0.306f;
			float b = 0.647f + t * 0.326f;

			// Fraction of this segment that is lit; smooth partial fill on top segment
			float litF  = brightness * NUM_METER_SEGS;
			float alpha = clamp(litF - s, 0.f, 1.f);

			// Ghost (unlit) base + lit fill combined
			float a = 0.05f + alpha * 0.95f;

			float yTop = box.size.y - (s + 1) * segH + inset;

			nvgBeginPath(args.vg);
			nvgRect(args.vg, inset, yTop, box.size.x - inset * 2.f, segH - inset * 2.f);
			nvgFillColor(args.vg, nvgRGBAf(r, g, b, a));
			nvgFill(args.vg);
		}
	}

	void drawHalo(const DrawArgs& args) override {}   // suppress circular halo
};

// ── Tape Echo widgets ─────────────────────────────────────────────────────
// TapeEcho uses pixels throughout. These widgets are sized in pixels, not
// millimetres, so they layout 1:1 with the panel SVG.

// ── Knob rendering (shared) ────────────────────────────────────────────
// Adaptation of the stock VCV RoundBlackKnob with an added silver apron and
// an indexing arrow. Geometry is proportional to `r`, so every knob size and
// the mode rotary share one look.
inline void drawStockKnobSvg(NVGcontext* vg, float cx, float cy, float diameter, float angle) {
    static const char* kBg = "res/ComponentLibrary/RoundBlackKnob_bg.svg";
    static const char* kFg = "res/ComponentLibrary/RoundBlackKnob.svg";
    std::shared_ptr<window::Svg> bg = window::Svg::load(asset::system(kBg));
    std::shared_ptr<window::Svg> fg = window::Svg::load(asset::system(kFg));
    if (!bg || !fg || !bg->handle || !fg->handle) return;
    float src = bg->getSize().x;
    if (src <= 0.f) return;
    const float k = diameter / src;

    // Static layer — bevel gradient + the wedges the shading depends on.
    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgScale(vg, k, k);
    nvgTranslate(vg, -src * 0.5f, -src * 0.5f);
    window::svgDraw(vg, bg->handle);
    nvgRestore(vg);

    // Rotating layer — body + pointer.
    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgRotate(vg, angle);
    nvgScale(vg, k, k);
    nvgTranslate(vg, -src * 0.5f, -src * 0.5f);
    window::svgDraw(vg, fg->handle);
    nvgRestore(vg);
}

// Stock knob + a custom silver apron around it. Panel ticks start exactly at
// `r`; the apron laps ~a quarter of the tick length at every size.
inline void drawTapeEchoKnob(NVGcontext* vg, float cx, float cy, float r, float angle) {
    const float rApron = r + 1.2f;
    const float rCap   = rApron * 0.80f;   // stock's inner-face ratio (11.28/14.17)

    // Contact shadow.
    NVGpaint sh = nvgRadialGradient(vg, cx, cy + r * 0.08f, rApron * 0.90f, rApron * 1.16f,
        nvgRGBA(0, 0, 0, 100), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy + r * 0.08f, rApron * 1.16f);
    nvgFillPaint(vg, sh); nvgFill(vg);

    // ── Apron: static polished silver ──
    // Base ramp: sky above, ground below.
    NVGpaint base = nvgLinearGradient(vg, cx, cy - rApron, cx, cy + rApron,
        nvgRGB(244, 245, 246), nvgRGB(142, 144, 148));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, rApron);
    nvgFillPaint(vg, base); nvgFill(vg);

    // Horizon — the dark band that makes it read as a mirror rather than paint.
    NVGpaint horizonIn = nvgLinearGradient(vg, cx, cy - rApron * 0.34f, cx, cy + rApron * 0.04f,
        nvgRGBA(28, 30, 34, 0), nvgRGBA(28, 30, 34, 104));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, rApron);
    nvgFillPaint(vg, horizonIn); nvgFill(vg);

    // Ground bounce — brightens again below the horizon.
    NVGpaint bounce = nvgLinearGradient(vg, cx, cy + rApron * 0.04f, cx, cy + rApron * 0.46f,
        nvgRGBA(28, 30, 34, 104), nvgRGBA(255, 255, 255, 132));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, rApron);
    nvgFillPaint(vg, bounce); nvgFill(vg);

    // Lower rim rolls off again.
    NVGpaint rimRoll = nvgLinearGradient(vg, cx, cy + rApron * 0.52f, cx, cy + rApron,
        nvgRGBA(255, 255, 255, 40), nvgRGBA(0, 0, 0, 64));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, rApron);
    nvgFillPaint(vg, rimRoll); nvgFill(vg);

    // Specular, upper left.
    NVGpaint spec = nvgRadialGradient(vg, cx - rApron * 0.40f, cy - rApron * 0.48f,
        rApron * 0.03f, rApron * 0.80f,
        nvgRGBA(255, 255, 255, 140), nvgRGBA(255, 255, 255, 0));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, rApron);
    nvgFillPaint(vg, spec); nvgFill(vg);

    nvgBeginPath(vg); nvgCircle(vg, cx, cy, rApron);
    nvgStrokeColor(vg, nvgRGB(22, 21, 20));
    nvgStrokeWidth(vg, std::max(0.6f, r * 0.05f));
    nvgStroke(vg);

    // ── Arrow: the only part of the apron that rotates. ──
    // Sits WHOLLY INSIDE the apron ring, clear of both edges: the ring spans
    // rCap (0.80) to rApron (1.00), and the stock knob's own pointer tick
    // reaches all the way to rCap, so the arrow's base has to start above that
    // or the two markings run together. Tip stops short of the rim stroke
    // (which is centred on rApron and so reaches ~0.977 inward at large sizes).
    const float tip   = rApron * 0.960f;
    const float base_ = rApron * 0.845f;
    const float half_ = rApron * 0.085f;
    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgRotate(vg, angle);
    nvgBeginPath(vg);
    nvgMoveTo(vg,  0.f,    -tip);
    nvgLineTo(vg,  half_,  -base_);
    nvgLineTo(vg, -half_,  -base_);
    nvgClosePath(vg);
    nvgFillColor(vg, nvgRGB(22, 21, 20)); nvgFill(vg);
    nvgRestore(vg);

    // Seat shadow where the knob body meets the apron.
    NVGpaint seat = nvgRadialGradient(vg, cx, cy, rCap * 0.96f, rCap * 1.16f,
        nvgRGBA(0, 0, 0, 95), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, rCap * 1.16f);
    nvgFillPaint(vg, seat); nvgFill(vg);

    // ── The stock knob itself ──
    drawStockKnobSvg(vg, cx, cy, rCap * 2.f, angle);
}

struct TapeEchoKnobBase : Knob {
    float radius;
    TapeEchoKnobBase(float radiusPx) {
        minAngle = -0.83f * M_PI;
        maxAngle =  0.83f * M_PI;
        box.size = Vec(radiusPx * 2.f, radiusPx * 2.f);
        radius = radiusPx;
    }
    // Value the INDICATOR is drawn from. Defaults to the param, but a subclass
    // can override so the graphic shows a quantized position while the
    // underlying param stays continuous (TapeEcho's clock-snap Rate knob).
    // Display-only: never write params from the audio thread — see the
    // visual-snap race documented in TapeEcho.cpp's clock-sync block.
    virtual float getDisplayValue() {
        return getParamQuantity() ? getParamQuantity()->getValue() : 0.f;
    }
    void draw(const DrawArgs& args) override {
        float cx = box.size.x / 2.f, cy = box.size.y / 2.f, r = radius;
        float angle = 0.f;
        if (getParamQuantity())
            angle = rescale(getDisplayValue(),
                            getParamQuantity()->getMinValue(),
                            getParamQuantity()->getMaxValue(),
                            minAngle, maxAngle);
        drawTapeEchoKnob(args.vg, cx, cy, r, angle);
    }
};
struct TapeEchoSmallKnob : TapeEchoKnobBase { TapeEchoSmallKnob() : TapeEchoKnobBase(9.f) {} };
struct TapeEchoLargeKnob : TapeEchoKnobBase { TapeEchoLargeKnob() : TapeEchoKnobBase(14.f) {} };

// Mode rotary — same knob face as the rest of the panel, at a larger size.
// 12 positions evenly spaced 30° apart. Mode 0 = 6 o'clock (REVERB ONLY),
// modes 1..11 sweep counterclockwise up through 12 o'clock and back down to
// 5 o'clock. Drag wraps around 360°: dragging past mode 11 wraps to mode 0
// and vice versa.
struct TapeEchoModeKnob : Knob {
    static constexpr int N_POS = 12;
    static constexpr float STEP_RAD = float(M_PI) / 6.f;   // 30° per step
    // Pixels of vertical mouse travel per step. Lower = faster knob, higher = finer.
    static constexpr float PIXELS_PER_STEP = 30.f;
    float dragAccum = 0.f;                                 // continuous accumulator [0, N_POS)

    TapeEchoModeKnob() {
        // Visual sweep covers all 12 positions over ~330° (11 steps × 30°).
        // The remaining 30° gap between mode 11 and mode 0 is crossed by the
        // wrap logic in onDragMove, so the knob can rotate a full 360°.
        minAngle = float(M_PI);                            // mode 0 = down (6 o'clock)
        maxAngle = float(M_PI) + 11.f * STEP_RAD;          // mode 11 = 5 o'clock
        box.size = Vec(54.f, 54.f);
        smooth = false;
        snap   = false;                                    // we snap manually in onDragMove
    }
    void onDragStart(const event::DragStart& e) override {
        if (e.button != GLFW_MOUSE_BUTTON_LEFT) return;
        if (getParamQuantity()) dragAccum = getParamQuantity()->getValue();
        Knob::onDragStart(e);
    }
    void onDragMove(const event::DragMove& e) override {
        if (!getParamQuantity()) return;
        // Drag up → increase value; standard Rack convention is -y on screen = up.
        dragAccum += -e.mouseDelta.y / PIXELS_PER_STEP;
        // Wrap into [0, N_POS).
        dragAccum = std::fmod(dragAccum, float(N_POS));
        if (dragAccum < 0.f) dragAccum += float(N_POS);
        int snapped = ((int)std::round(dragAccum)) % N_POS;
        getParamQuantity()->setValue((float)snapped);
        ParamWidget::onDragMove(e);
    }
    // Value the POINTER is drawn from. Defaults to the param, but a subclass
    // can override so the switch shows the head sync selected while the
    // underlying param stays where the user parked it. Display-only: never
    // write params from the audio thread.
    virtual float getDisplayValue() {
        return getParamQuantity() ? getParamQuantity()->getValue() : 4.f;
    }
    void draw(const DrawArgs& args) override {
        float cx = box.size.x / 2.f, cy = box.size.y / 2.f, r = 27.f;
        // Mode N points at nvgRotate angle (π + N·π/6) — direct, no rescale needed.
        float value = getDisplayValue();
        float angle = float(M_PI) + value * STEP_RAD;
        // Same three-part RE-201 stack as every other knob, at the larger mode
        // size. The chickenhead it replaced was the odd one out on the panel;
        // snapping, wrap and drag behaviour above are unchanged, so the pointer
        // still lands exactly on a mode detent.
        drawTapeEchoKnob(args.vg, cx, cy, r, angle);
    }
};

// Circular toggle switch — r=9 px. The pair (POWER / REVERSE). Momentary
// (one pulse per click), but draws its engaged-state from a latched-state
// pointer rather than the param value, since the param is only high during
// the click itself. Falls back to the param value in momentary mode.
struct TapeEchoToggle : Switch {
    const bool* litStatePtr      = nullptr;   // persisted latched state (drives visual)
    const bool* momentaryFlagPtr = nullptr;   // if non-null AND *==true → use param value
    bool invertDisplay = false;
    float radius = 9.f;
    TapeEchoToggle() {
        box.size = Vec(radius * 2.f, radius * 2.f);
        momentary = true;   // each click = one 0→1→0 pulse on the param
    }
    void draw(const DrawArgs& args) override {
        float cx = box.size.x / 2.f, cy = box.size.y / 2.f, r = radius;
        bool useParamValue = !litStatePtr ||
                             (momentaryFlagPtr && *momentaryFlagPtr);
        bool engaged;
        if (useParamValue) {
            engaged = getParamQuantity() && getParamQuantity()->getValue() > 0.5f;
        } else {
            engaged = *litStatePtr;
        }
        if (invertDisplay) engaged = !engaged;
        NVGcontext* vg = args.vg;
        // Outer body
        nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
        nvgFillColor(vg, nvgRGB(20, 20, 25)); nvgFill(vg);
        nvgStrokeColor(vg, nvgRGB(190, 190, 200)); nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
        // Inner cap with engagement glow
        float capR = r * 0.78f;
        NVGpaint grad = nvgRadialGradient(vg,
            cx - capR*0.3f, cy - capR*0.4f, capR*0.05f, capR*1.4f,
            engaged ? nvgRGB(170, 140, 220) : nvgRGB(120, 120, 130),
            engaged ? nvgRGB(95, 70, 160)   : nvgRGB(45, 45, 55));
        nvgBeginPath(vg); nvgCircle(vg, cx, cy, capR);
        nvgFillPaint(vg, grad); nvgFill(vg);
        // Top highlight
        nvgBeginPath(vg);
        nvgEllipse(vg, cx, cy - capR*0.45f, capR*0.55f, capR*0.18f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, engaged ? 100 : 55));
        nvgFill(vg);
    }
};

// Small red LED, drawn at the SVG housing position (r=4 px).
// Reads brightness from a paired latched-state bool (the same pointer the toggle
// uses). Falls back to the param value when in momentary mode (matches the
// toggle's visual semantics).
struct TapeEchoRedLED : Widget {
    const bool* litStatePtr      = nullptr;
    const bool* momentaryFlagPtr = nullptr;
    engine::Module* trackedModule = nullptr;
    int trackedParamId = -1;
    bool invertDisplay = false;
    float radius = 4.f;
    TapeEchoRedLED() {
        box.size = Vec(radius * 2.f, radius * 2.f);
    }
    void draw(const DrawArgs& args) override {
        bool useParamValue = !litStatePtr ||
                             (momentaryFlagPtr && *momentaryFlagPtr);
        float brightness = 0.f;
        if (useParamValue) {
            if (trackedModule && trackedParamId >= 0) {
                float v = trackedModule->params[trackedParamId].getValue();
                if (invertDisplay) v = 1.f - v;
                brightness = clamp(v, 0.f, 1.f);
            } else if (!trackedModule) {
                brightness = 0.5f;  // browser preview
            }
        } else {
            bool engaged = *litStatePtr;
            if (invertDisplay) engaged = !engaged;
            brightness = engaged ? 1.f : 0.f;
        }
        if (brightness < 0.001f) return;
        NVGcontext* vg = args.vg;
        float cx = box.size.x / 2.f, cy = box.size.y / 2.f;
        // Outer red glow
        NVGpaint glow = nvgRadialGradient(vg, cx, cy, 0.f, radius * 2.2f,
            nvgRGBA(255, 50, 50, (unsigned char)(brightness * 180)),
            nvgRGBA(255, 50, 50, 0));
        nvgBeginPath(vg);
        nvgRect(vg, -radius * 2.f, -radius * 2.f,
                box.size.x + radius * 4.f, box.size.y + radius * 4.f);
        nvgFillPaint(vg, glow); nvgFill(vg);
        // Lit interior
        nvgBeginPath(vg); nvgCircle(vg, cx, cy, radius * 0.80f);
        nvgFillColor(vg, nvgRGBA(255, 70, 70, (unsigned char)(brightness * 255)));
        nvgFill(vg);
        // Hot spot
        nvgBeginPath(vg); nvgCircle(vg, cx - radius * 0.18f, cy - radius * 0.22f, radius * 0.35f);
        nvgFillColor(vg, nvgRGBA(255, 200, 200, (unsigned char)(brightness * 220)));
        nvgFill(vg);
    }
};

// Horizontal three-position H/M/L switch.
// Param values: 0=L, 1=M, 2=H. Click cycles L→M→H→L. Position indicator slides.
struct TapeEchoHMLSwitch : Switch {
    TapeEchoHMLSwitch() {
        box.size = Vec(30.f, 15.f);
    }
    void onDragStart(const event::DragStart& e) override {
        if (e.button != GLFW_MOUSE_BUTTON_LEFT) return;
        if (!getParamQuantity()) return;
        int idx = clamp((int)std::round(getParamQuantity()->getValue()), 0, 2);
        idx = (idx + 1) % 3;
        getParamQuantity()->setValue((float)idx);
    }
    void draw(const DrawArgs& args) override {
        int idx = 1;
        if (getParamQuantity()) idx = clamp((int)std::round(getParamQuantity()->getValue()), 0, 2);
        NVGcontext* vg = args.vg;
        // Track
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f, 2.f);
        nvgFillColor(vg, nvgRGB(25, 25, 30)); nvgFill(vg);
        // Three slot markers
        for (int i = 0; i < 3; i++) {
            float x = 5.f + i * 10.f;
            nvgBeginPath(vg);
            nvgCircle(vg, x, box.size.y / 2.f, 1.5f);
            nvgFillColor(vg, nvgRGB(70, 70, 75)); nvgFill(vg);
        }
        // Position indicator — bright cap at selected slot. Silkscreen reads
        // "H M L" left→right, so the cap mirrors the value to match the letters:
        // H (idx 2) draws at the left slot (x=5), M (1) centre, L (0) right (x=25).
        float capX = 5.f + (2 - idx) * 10.f;
        float capY = box.size.y / 2.f;
        NVGpaint grad = nvgRadialGradient(vg, capX - 1.5f, capY - 2.f, 0.5f, 6.f,
            nvgRGB(230, 225, 210), nvgRGB(140, 135, 120));
        nvgBeginPath(vg); nvgCircle(vg, capX, capY, 4.f);
        nvgFillPaint(vg, grad); nvgFill(vg);
        nvgStrokeColor(vg, nvgRGB(40, 40, 40)); nvgStrokeWidth(vg, 0.5f); nvgStroke(vg);
    }
};
