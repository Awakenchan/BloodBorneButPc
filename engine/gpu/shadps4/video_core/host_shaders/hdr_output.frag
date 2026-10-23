// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: HDR output. The game's SDR picture (letterboxed into a swapchain-sized sRGB-encoded
// image) and the settings menu's layer (premultiplied alpha, sRGB encoded) become the HDR
// swapchain's signal: HDR10 (BT.2020 primaries, PQ) or scRGB (linear BT.709, 1.0 = 80 nits).
// SDR white is shown at paper_white. Native mode (FLAG_NATIVE): the frame's gain map
// (hdr_native.comp: how much brighter the game's own scene is where its tonemap compressed the
// highlights; 1 for midtones and UI) brightens the highlights. Expand mode, and native frames
// without a scene (menus, loading, movies): boost expands the SDR picture's highlights towards
// the peak.
// bbport: in native mode the game's UI (where the gain map is negative, and all of a frame
// without a scene) has its own white (ui_white times paper white) and colours: ui_vivid of the
// way from sRGB's (exact) to a wide-gamut SDR monitor's (the values taken as DCI-P3), which is
// how most players have seen it - next to that, exact sRGB looks washed out.

#version 450

layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D game;    // the letterboxed SDR frame, swapchain-sized
layout (binding = 1) uniform sampler2D overlay; // the menu layer (FLAG_OVERLAY)
layout (binding = 2) uniform sampler2D gain;    // the frame's gain map (FLAG_NATIVE), filtered

layout (push_constant) uniform HdrSettings {
    float paper_white; // nits of SDR white
    float peak;        // nits the display reaches
    float boost;       // 0: SDR white at paper white .. 1: SDR white at the peak
    uint flags;
    vec2 gain_offset;  // the frame's rectangle in the output: its top left corner, pixels,
    vec2 gain_scale;   // and 1 / its size: output pixels -> gain map coordinates
    float ui_white;    // native mode: the UI's white, times paper white
    float ui_vivid;    // native mode: 0 the UI in sRGB's colours .. 1 in DCI-P3's
} hdr;

const uint FLAG_SCRGB = 1u;   // scRGB output, else HDR10
const uint FLAG_OVERLAY = 2u; // the menu layer has something drawn
const uint FLAG_NATIVE = 4u;  // the gain map belongs to this frame
const uint FLAG_DEBUG = 8u;   // BB_HDR_DEBUG: the gain as a heat map
const uint FLAG_UI_FRAME = 16u; // native mode, a frame without a scene: all of it is UI
const uint FLAG_COMPARE = 32u;  // the left half as SDR (at paper white), for comparison

// Linear brightness (SDR white = 1) above which the highlights are expanded: high, so large
// bright areas (fog, sky) stay near paper white and only lamps, fire and sparks reach the peak.
const float KNEE = 0.8;

// BT.709 -> BT.2020 primaries (columns).
const mat3 BT709_TO_BT2020 = mat3(
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956);

// DCI-P3 (D65) -> BT.709 primaries (columns): BT.709 values outside [0, 1] are the wider gamut,
// which the BT.2020 and scRGB encodes below both carry.
const mat3 P3_TO_BT709 = mat3(
    1.2249, -0.0421, -0.0196,
    -0.2249, 1.0421, -0.0786,
    0.0, 0.0, 1.0983);

// The game's picture is made for SDR displays, which show it with a 2.2 power curve. The
// piecewise sRGB curve (what Windows uses for SDR content in HDR) lifts the shadows: dark
// scenes turn grey and foggy. Bloodborne is mostly shadows, so 2.2 it is.
vec3 SdrToLinear(vec3 c) {
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

// SMPTE ST 2084 inverse EOTF; y: luminance / 10000 nits.
vec3 PqEncode(vec3 y) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 ym = pow(clamp(y, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), vec3(m2));
}

// Inverse tone map of the brightest channel (hue and saturation kept): unchanged up to KNEE,
// then a curve with the same slope there that reaches `white` at SDR white.
vec3 ExpandHighlights(vec3 rgb, float white) {
    float m = max(rgb.r, max(rgb.g, rgb.b));
    if (white <= 1.0 || m <= KNEE) {
        return rgb;
    }
    float t = (m - KNEE) / (1.0 - KNEE);
    float expanded = m + (white - 1.0) * t * t;
    return rgb * (expanded / m);
}

// Debug view colours: blue (a little gain) .. cyan .. green .. yellow .. red (the peak).
vec3 Heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0),
                      1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 game_srgb = texelFetch(game, pixel, 0).rgb;
    float peak_white = max(hdr.peak / hdr.paper_white, 1.0);
    float white = max(mix(1.0, peak_white, clamp(hdr.boost, 0.0, 1.0)), 1.0);
    vec3 sdr_linear = SdrToLinear(game_srgb);
    vec3 linear;
    float applied = 1.0; // the brightest channel's gain (debug view)
    bool ui = false;
    float ui_amount = (hdr.flags & FLAG_UI_FRAME) != 0u ? 1.0 : 0.0; // filtered: soft UI edges
    if ((hdr.flags & FLAG_NATIVE) != 0u) {
        // Outside the frame (letterbox bars) the map is not sampled: gain 1.
        vec2 uv = (gl_FragCoord.xy - hdr.gain_offset) * hdr.gain_scale;
        if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
            float g = textureLod(gain, uv, 0.0).r;
            ui = g < 0.0;
            ui_amount = clamp(-g, 0.0, 1.0);
            applied = max(abs(g), 1.0);
        }
        // The map keeps the brightest channel within the peak; filtering and the letterbox
        // blit can step over it by a little.
        linear = min(sdr_linear * applied, vec3(peak_white));
    } else {
        linear = ExpandHighlights(sdr_linear, white);
        float m = max(sdr_linear.r, max(sdr_linear.g, sdr_linear.b));
        applied = m > 0.0 ? max(linear.r, max(linear.g, linear.b)) / m : 1.0;
    }
    if (ui_amount > 0.0) {
        linear *= mix(1.0, hdr.ui_white, ui_amount);
        linear = mix(linear, P3_TO_BT709 * linear, ui_amount * clamp(hdr.ui_vivid, 0.0, 1.0));
        linear = min(linear, vec3(peak_white));
    }
    if ((hdr.flags & FLAG_DEBUG) != 0u) {
        // The picture in grey at a third of paper white, the gain over it from blue (a little)
        // to red (the peak), UI that native mode keeps at paper white in magenta. Top left
        // corner: green while native mode has this frame's gain map, orange while it falls back
        // to expand (or in expand mode).
        float y = dot(sdr_linear, vec3(0.2126, 0.7152, 0.0722));
        vec3 shown = vec3(y * 0.33);
        if (applied > 1.001) {
            shown = mix(shown, Heat(log2(applied) / max(log2(peak_white), 0.1)), 0.8);
        }
        if (ui) {
            shown = mix(shown, vec3(1.0, 0.0, 1.0), 0.5);
        }
        if (gl_FragCoord.x < 48.0 && gl_FragCoord.y < 48.0) {
            shown = (hdr.flags & FLAG_NATIVE) != 0u ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.5, 0.0);
        }
        linear = shown;
    }
    if ((hdr.flags & FLAG_COMPARE) != 0u && (hdr.flags & FLAG_DEBUG) == 0u) {
        // The left half as an SDR display shows the game (at paper white, sRGB's colours), a
        // thin grey line between the two.
        const float middle = 0.5 * float(textureSize(game, 0).x);
        if (gl_FragCoord.x < middle) {
            linear = sdr_linear;
        }
        if (abs(gl_FragCoord.x - middle) < 1.5) {
            linear = vec3(0.5);
        }
    }
    if ((hdr.flags & FLAG_OVERLAY) != 0u) {
        // The menu over the frame as the SDR path draws it (blended in sRGB), at paper white:
        // the menu is not expanded.
        vec4 menu = texelFetch(overlay, pixel, 0);
        if (menu.a > 0.0) {
            vec3 sdr = SdrToLinear(clamp(menu.rgb + game_srgb * (1.0 - menu.a), 0.0, 1.0));
            linear = mix(linear, sdr, clamp(menu.a * 4.0, 0.0, 1.0));
        }
    }
    if ((hdr.flags & FLAG_SCRGB) != 0u) {
        color = vec4(linear * (hdr.paper_white / 80.0), 1.0);
    } else {
        vec3 nits = max(BT709_TO_BT2020 * linear, vec3(0.0)) * hdr.paper_white;
        color = vec4(PqEncode(nits / 10000.0), 1.0);
    }
}
