#pragma once
#include "plugin.hpp"

// Custom port widget modelled on the PJ301M SVG — drawn entirely in NanoVG.
// Matches the AUDIO-8 port aesthetic with dark inner rings.
struct AstravoxPort : app::PortWidget {
    AstravoxPort() { box.size = Vec(23.7f, 23.7f); }
    void draw(const DrawArgs& args) override {
        NVGcontext* vg = args.vg;
        float cx = box.size.x / 2.f;
        float r  = cx;
        // Outer metallic ring
        NVGpaint p = nvgLinearGradient(vg, cx, cx - r, cx, cx + r,
            nvgRGB(0x82,0x81,0x81), nvgRGB(0x57,0x57,0x57));
        nvgBeginPath(vg); nvgCircle(vg, cx, cx, r * 0.937f);
        nvgFillPaint(vg, p); nvgFill(vg);
        // Light gray body
        nvgBeginPath(vg); nvgCircle(vg, cx, cx, r * 0.881f);
        nvgFillColor(vg, nvgRGB(0xE0,0xE0,0xE0)); nvgFill(vg);
        // Horizontal slot bevel
        p = nvgLinearGradient(vg, cx, cx - 1.35f, cx, cx + 1.35f,
            nvgRGB(0x80,0x80,0x80), nvgRGB(0xFF,0xFB,0xFD));
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 1.25f, cx - 1.35f, box.size.x - 2.5f, 2.7f, 1.0f);
        nvgFillPaint(vg, p); nvgFill(vg);
        // Dark gradient collar
        p = nvgLinearGradient(vg, cx - r, cx, cx + r, cx,
            nvgRGB(0xB0,0xAE,0xAE), nvgRGB(0x61,0x61,0x61));
        nvgBeginPath(vg); nvgCircle(vg, cx, cx, r * 0.738f);
        nvgFillPaint(vg, p); nvgFill(vg);
        // Very dark inner
        nvgBeginPath(vg); nvgCircle(vg, cx, cx, r * 0.689f);
        nvgFillColor(vg, nvgRGB(0x1F,0x1F,0x1F)); nvgFill(vg);
        // Dark gradient ring
        p = nvgLinearGradient(vg, cx, cx + r * 0.644f, cx, cx - r * 0.644f,
            nvgRGB(0x54,0x51,0x51), nvgRGB(0x3F,0x3F,0x3F));
        nvgBeginPath(vg); nvgCircle(vg, cx, cx, r * 0.644f);
        nvgFillPaint(vg, p); nvgFill(vg);
        // Dark gray inner body
        nvgBeginPath(vg); nvgCircle(vg, cx, cx, r * 0.574f);
        nvgFillColor(vg, nvgRGB(0x3F,0x3F,0x3F)); nvgFill(vg);
        // Inner gradient ring
        p = nvgLinearGradient(vg, cx, cx + r * 0.465f, cx, cx - r * 0.465f,
            nvgRGB(0x54,0x51,0x51), nvgRGB(0x30,0x30,0x30));
        nvgBeginPath(vg); nvgCircle(vg, cx, cx, r * 0.465f);
        nvgFillPaint(vg, p); nvgFill(vg);
        // Black center hole
        nvgBeginPath(vg); nvgCircle(vg, cx, cx, r * 0.411f);
        nvgFillColor(vg, nvgRGB(0,0,0)); nvgFill(vg);
        app::PortWidget::draw(args);
    }
};
