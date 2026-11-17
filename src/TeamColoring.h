// RWX team colouring: PlayerTeam's ten-colour table and the three per-pixel dyes of
// gameFramework/graphics/TeamColoring.java. Header-only so the headless harness can assert on the maths.
#pragma once
#include <cstdint>

// CustomUnitConfigParser.java:1318-1338. pureGreen is the default, teamColorsUseHue:true is a shortcut for
// hueAdd, and naming both keys is a ConfigParseException. hueNew exists in the ColorMode enum but no dye
// below reads it, so it falls in the pureGreen else-branch exactly like the Java does.
enum TeamColourMode { kTCModePureGreen = 0, kTCModeHueAdd = 1, kTCModeHueShift = 2, kTCModeDisabled = 3 };

// PlayerTeam.java:1338 -- the list loadTeamColorSettings() falls back to. Index 0 is the art's own green,
// so the green team draws the untouched texture; 5 is the white the neutral/unknown teams sit on.
static const uint32_t kTeamColors[10] = {
    0xFF00FF00u, 0xFFD02013u, 0xFF0463F3u, 0xFFFFFF40u, 0xFF00FFFFu,
    0xFFD0F8F7u, 0xFF000000u, 0xFFFF00EAu, 0xFFFF7F18u, 0xFF9368C4u,
};

// PlayerTeam.getDefaultTeamColorIndex():1800-1818: teamId %% 10, with teamId -1/-2 (neutral, unknown) on the
// white slot. A team that is assigned an explicit colour index wins over this (:1792-1797).
inline int teamColorIndex(int teamId) { return teamId < 0 ? 5 : teamId % 10; }

inline uint32_t teamColourArgb(uint32_t color, int mode, uint32_t teamColor) {
    if (mode == kTCModeDisabled) return color;
    int a = int((color >> 24) & 0xFF), r = int((color >> 16) & 0xFF);
    int g = int((color >> 8) & 0xFF), b = int(color & 0xFF);
    int tr = int((teamColor >> 16) & 0xFF), tg = int((teamColor >> 8) & 0xFF), tb = int(teamColor & 0xFF);
    if (mode == kTCModeHueAdd) {                                  // TeamColoring.hueAdd():40-52
        if (a <= 0) return color;
        auto cl = [](float v) { return int(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        return (uint32_t(a) << 24) | (uint32_t(cl(r + tr * 0.15f)) << 16)
             | (uint32_t(cl(g + tg * 0.15f)) << 8) | uint32_t(cl(b + tb * 0.15f));
    }
    if (mode == kTCModeHueShift) {                                // TeamColoring.hueShift():24-38
        if (a == 0) return (r > 0 || g > 0 || b > 0) ? 0u : color;
        float minV = float(r < g ? (r < b ? r : b) : (g < b ? g : b));
        float d1 = float(r > g ? r - g : g - r), d2 = float(g > b ? g - b : b - g);
        float d3 = float(b > r ? b - r : r - b);
        float maxDiff = d1 > d2 ? (d1 > d3 ? d1 : d3) : (d2 > d3 ? d2 : d3);
        if (maxDiff <= 15.0f) return color;
        float amount = maxDiff / 255.0f;
        auto cl = [](float v) { return int(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        return (uint32_t(a) << 24) | (uint32_t(cl(minV + tr * amount)) << 16)
             | (uint32_t(cl(minV + tg * amount)) << 8) | uint32_t(cl(minV + tb * amount));
    }
    // TeamColoring.pureGreen():5-23: only pixels whose red and blue already agree -- the art's greens and
    // greys -- are dyed, and a grey where green equals red is left alone.
    if (a == 0) return 0u;
    if (g <= 0 || r != b) return color;
    if (r == 0) {
        return (uint32_t(a) << 24) | (uint32_t((tr * g) >> 8) << 16)
             | (uint32_t((tg * g) >> 8) << 8) | uint32_t((tb * g) >> 8);
    }
    if (g != r) {
        float amount = (g * 0.003921569f) - (r * 0.003921569f);
        auto cl = [](float v) { return int(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        return (uint32_t(a) << 24) | (uint32_t(cl(r + tr * amount)) << 16)
             | (uint32_t(cl(r + tg * amount)) << 8) | uint32_t(cl(r + tb * amount));
    }
    return color;
}
