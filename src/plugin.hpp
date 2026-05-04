#pragma once
#include <rack.hpp>

using namespace rack;

// Declare each module Model — add one line per module
extern Plugin* pluginInstance;

extern Model* modelVocoder;

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
