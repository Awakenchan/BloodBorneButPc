// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/keycap_art.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#pragma GCC diagnostic pop

#ifdef _WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_keycap_font[] = {
#embed BB_KEYCAP_FONT_PATH
};
#pragma GCC diagnostic pop
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_keycap_font\n"
    ".global bb_keycap_font\n"
    "bb_keycap_font:\n"
    ".incbin \"" BB_KEYCAP_FONT_PATH "\"\n"
    ".previous\n");
extern "C" const unsigned char bb_keycap_font[];
#endif

namespace Keycap {

std::string Spec::Id() const {
    std::string id(1, char('0' + kind));
    for (const auto& label : labels) {
        id += '|';
        id += label;
    }
    return id + '|' + std::to_string(button);
}

namespace {

constexpr int S = 4;

struct Color {
    float r, g, b, a;
};
constexpr Color Rgb(std::uint32_t rgb, float a = 1.0f) {
    return {float((rgb >> 16) & 0xff) / 255.0f, float((rgb >> 8) & 0xff) / 255.0f,
            float(rgb & 0xff) / 255.0f, a};
}
constexpr Color kFace = Rgb(0x2f2f2f), kRimTop = Rgb(0x7a7a7a), kRimSide = Rgb(0x4a4a4a),
                kRimDark = Rgb(0x1e1e1e), kUnderRim = Rgb(0x262626), kShadow = Rgb(0, 0.42f),
                kInk = Rgb(0xc3a171), kOutline = Rgb(0x8a8a8a);

struct Canvas {
    int size, n;
    float scale, ox, oy;
    std::vector<float> px;

    Canvas(int size_, float scale_, float ox_, float oy_)
        : size(size_), n(size_ * S), scale(scale_), ox(ox_), oy(oy_), px(std::size_t(n) * n * 4, 0.0f) {}

    void Blend(int x, int y, Color c, float coverage) {
        const float a = c.a * std::min(coverage, 1.0f);
        if (a <= 0.0f) {
            return;
        }
        float* p = &px[(std::size_t(y) * n + x) * 4];
        p[0] = c.r * a + p[0] * (1.0f - a);
        p[1] = c.g * a + p[1] * (1.0f - a);
        p[2] = c.b * a + p[2] * (1.0f - a);
        p[3] = a + p[3] * (1.0f - a);
    }
};

thread_local float t_samples = S;

template <class Coverage>
void Paint(Canvas& c, Color color, Coverage coverage) {
    t_samples = S * c.scale;
    for (int y = 0; y < c.n; ++y) {
        for (int x = 0; x < c.n; ++x) {
            const float cover = coverage(((float(x) + 0.5f) / S - c.ox) / c.scale,
                                         ((float(y) + 0.5f) / S - c.oy) / c.scale);
            if (cover > 0.0f) {
                c.Blend(x, y, color, cover);
            }
        }
    }
}

struct Box {
    float x0, y0, x1, y1, r;
};

float Distance(const Box& b, float x, float y) {
    const float hx = (b.x1 - b.x0) / 2 - b.r, hy = (b.y1 - b.y0) / 2 - b.r;
    const float qx = std::abs(x - (b.x0 + b.x1) / 2) - hx, qy = std::abs(y - (b.y0 + b.y1) / 2) - hy;
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - b.r;
}

float Inside(const Box& b, float x, float y) {
    return std::clamp(0.5f - Distance(b, x, y) * t_samples, 0.0f, 1.0f);
}

Box Inset(Box b, float left, float top, float right, float bottom) {
    b.x0 += left;
    b.y0 += top;
    b.x1 -= right;
    b.y1 -= bottom;
    b.r = std::max(b.r - std::max(left, top), 0.5f);
    return b;
}

Box Offset(Box b, float dx, float dy) {
    b.x0 += dx;
    b.x1 += dx;
    b.y0 += dy;
    b.y1 += dy;
    return b;
}

void DrawCap(Canvas& c, const Box& cap) {
    const Box shadow = Offset(Inset(cap, 0.3f, 0.0f, 0.3f, 0.0f), 0.0f, 1.1f);
    Paint(c, kShadow, [&](float x, float y) { return Inside(shadow, x, y); });
    Paint(c, kRimDark, [&](float x, float y) { return Inside(cap, x, y); });
    const Box side = Inset(cap, 0.0f, 0.0f, 0.8f, 1.0f);
    Paint(c, kRimSide, [&](float x, float y) { return Inside(side, x, y); });
    Paint(c, kRimTop, [&](float x, float y) { return y < cap.y0 + 1.0f ? Inside(side, x, y) : 0.0f; });
    const Box under = Inset(cap, 0.9f, 1.0f, 0.9f, 1.0f);
    Paint(c, kUnderRim, [&](float x, float y) { return Inside(under, x, y); });
    const Box face = Inset(cap, 0.9f, 1.8f, 0.9f, 1.0f);
    Paint(c, kFace, [&](float x, float y) { return Inside(face, x, y); });
}

void DrawArrow(Canvas& c, float cx, float cy, float size, int dir, Color color) {
    const float head = size * 0.5f, half_head = size * 0.45f, half_shaft = size * 0.14f;
    Paint(c, color, [&](float x, float y) {
        float u = x - cx, v = y - cy;
        switch (dir) {
        case 1: std::swap(u, v); v = -v; break;
        case 2: v = -v; break;
        case 3: std::swap(u, v); break;
        default: break;
        }
        v += size / 2;
        if (v < 0.0f || v > size) {
            return 0.0f;
        }
        if (v <= head) {
            return std::abs(u) <= half_head * v / head ? 1.0f : 0.0f;
        }
        return std::abs(u) <= half_shaft ? 1.0f : 0.0f;
    });
}

int ArrowDir(const std::string& label) {
    return label == "Up" ? 0 : label == "Right" ? 1 : label == "Down" ? 2 : label == "Left" ? 3 : -1;
}

std::vector<int> Utf8(const std::string& s) {
    std::vector<int> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const int len = b < 0x80 ? 1 : b >> 5 == 6 ? 2 : b >> 4 == 14 ? 3 : b >> 3 == 30 ? 4 : 1;
        int cp = len == 1 ? b : b & (0x7f >> len);
        for (int k = 1; k < len && i + k < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

struct Font {
    stbtt_fontinfo info{};
    bool ok = false;
    float cap_height = 1.0f;

    Font() {
        ok = stbtt_InitFont(&info, bb_keycap_font, stbtt_GetFontOffsetForIndex(bb_keycap_font, 0)) != 0;
        int x0, y0, x1, y1;
        if (ok && stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1)) {
            cap_height = float(y1);
        }
    }
};

const Font& GetFont() {
    static const Font font;
    return font;
}

void DrawText(Canvas& c, const std::string& text, float cx, float cy, float width, float cap, Color color = kInk) {
    const Font& font = GetFont();
    const auto cps = Utf8(text);
    if (!font.ok || cps.empty()) {
        return;
    }
    float pen = 0.0f, left = 1e9f, right = -1e9f, top = -1e9f, bottom = 1e9f;
    bool capitals = false;
    std::vector<float> at(cps.size());
    for (std::size_t i = 0; i < cps.size(); ++i) {
        int advance, bearing, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&font.info, cps[i], &advance, &bearing);
        at[i] = pen;
        if (stbtt_GetCodepointBox(&font.info, cps[i], &x0, &y0, &x1, &y1)) {
            left = std::min(left, pen + float(x0));
            right = std::max(right, pen + float(x1));
            bottom = std::min(bottom, float(y0));
            top = std::max(top, float(y1));
        }
        capitals |= (cps[i] >= 'A' && cps[i] <= 'Z') || (cps[i] >= '0' && cps[i] <= '9');
        pen += float(advance);
        if (i + 1 < cps.size()) {
            pen += float(stbtt_GetCodepointKernAdvance(&font.info, cps[i], cps[i + 1]));
        }
    }
    if (right <= left) {
        return;
    }
    const float scale = std::min(cap / font.cap_height, width / (right - left));
    const float baseline = capitals ? cy + font.cap_height * scale / 2 : cy + (top + bottom) / 2 * scale;
    const float origin = cx - (left + right) / 2 * scale;

    const int n = c.n;
    std::vector<float> ink(std::size_t(n) * n, 0.0f);
    const float hs = scale * c.scale * S;
    std::vector<unsigned char> glyph;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const float gx = (c.ox + (origin + at[i] * scale) * c.scale) * S;
        const float gy = (c.oy + baseline * c.scale) * S;
        const float fx = std::floor(gx), fy = std::floor(gy);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&font.info, cps[i], hs, hs, gx - fx, gy - fy, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) {
            continue;
        }
        glyph.assign(std::size_t(w) * h, 0);
        stbtt_MakeCodepointBitmapSubpixel(&font.info, glyph.data(), w, h, w, hs, hs, gx - fx, gy - fy, cps[i]);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int px = int(fx) + x0 + x, py = int(fy) + y0 + y;
                if (px >= 0 && px < n && py >= 0 && py < n) {
                    float& v = ink[std::size_t(py) * n + px];
                    v = std::min(1.0f, v + float(glyph[y * w + x]) / 255.0f);
                }
            }
        }
    }
    for (int y = 0; y < n; ++y) {
        float* row = &ink[std::size_t(y) * n];
        for (int x = n - 1; x > 0; --x) {
            row[x] = std::max(row[x], row[x - 1]);
        }
    }
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            c.Blend(x, y, color, ink[std::size_t(y) * n + x]);
        }
    }
}

void DrawLabel(Canvas& c, const Box& cap, const std::string& label, float cap_px) {
    DrawCap(c, cap);
    const float cx = (cap.x0 + cap.x1) / 2, cy = (cap.y0 + cap.y1) / 2 + 0.3f;
    if (const int dir = ArrowDir(label); dir >= 0) {
        DrawArrow(c, cx, cy, cap_px * 1.05f, dir, kInk);
        return;
    }
    const float w = cap.x1 - cap.x0;
    DrawText(c, label, cx, cy, w - std::min(5.0f, w * 0.2f), cap_px);
}

void DrawMouse(Canvas& c, int button) {
    const Box body{7.0f, 2.0f, 25.0f, 29.0f, 8.5f};
    const Box shadow = Offset(body, 0.0f, 1.1f);
    Paint(c, kShadow, [&](float x, float y) { return Inside(shadow, x, y); });
    Paint(c, kOutline, [&](float x, float y) { return Inside(body, x, y); });
    const Box face = Inset(body, 1.3f, 1.3f, 1.3f, 1.3f);
    Paint(c, kFace, [&](float x, float y) { return Inside(face, x, y); });
    const float cx = 16.0f, split = 13.0f;
    if (button == 1 || button == 3) {
        Paint(c, kInk, [&](float x, float y) {
            const bool side = button == 1 ? x < cx - 0.6f : x > cx + 0.6f;
            return side && y < split - 0.6f ? Inside(face, x, y) : 0.0f;
        });
    }
    Paint(c, kOutline, [&](float x, float y) {
        const bool seam = (std::abs(x - cx) <= 0.6f && y < split) || std::abs(y - split) <= 0.6f;
        return seam ? Inside(face, x, y) : 0.0f;
    });
    const bool wheel = button == 2 || button == 6 || button == 7;
    const Box wheel_box{cx - 2.0f, 4.5f, cx + 2.0f, 11.5f, 2.0f};
    Paint(c, wheel ? kInk : kOutline, [&](float x, float y) { return Inside(wheel_box, x, y); });
    if (button == 6 || button == 7) {
        DrawArrow(c, 28.5f, 8.0f, 6.5f, button == 6 ? 0 : 2, kInk);
    } else if (button == 0) {
        DrawArrow(c, 3.5f, 17.0f, 6.0f, 3, kInk);
        DrawArrow(c, 28.5f, 17.0f, 6.0f, 1, kInk);
    }
}

constexpr Box kRoundCap{3.7f, 3.2f, 28.3f, 27.8f, 12.3f};
constexpr Box kWideCap{0.5f, 6.0f, 31.5f, 26.0f, 4.0f};
constexpr Color kPadFace[] = {Rgb(0x5bbf3f), Rgb(0xe2423a), Rgb(0x3a7bf0), Rgb(0xf1c232)};
constexpr const char* kPadNames[] = {"A",  "B",  "X",  "Y",  "LB", "RB", "LT",  "RT",
                                     "LS", "RS", "",   "",   "",   "",   "",    "",
                                     "",   "P1", "P2", "P3", "P4", "Pad", "Home"};
static_assert(sizeof(kPadNames) / sizeof(kPadNames[0]) == PadGuide + 1);

bool PadWide(int button) {
    return (button >= PadLB && button <= PadRT) || button >= PadP1;
}

void DrawFrame(Canvas& c, const Box& box, float line) {
    Paint(c, kInk, [&](float x, float y) { return Inside(box, x, y); });
    const Box inside = Inset(box, line, line, line, line);
    Paint(c, kFace, [&](float x, float y) { return Inside(inside, x, y); });
}

void DrawPad(Canvas& c, int button) {
    button = std::clamp(button, 0, int(PadGuide));
    if (PadWide(button)) {
        DrawLabel(c, kWideCap, kPadNames[button], button <= PadRT ? 12.5f : 10.0f);
        return;
    }
    DrawCap(c, kRoundCap);
    const float cx = 16.0f, cy = 15.8f;
    switch (button) {
    case PadA:
    case PadB:
    case PadX:
    case PadY:
        DrawText(c, kPadNames[button], cx, cy, 20.0f, 13.0f, kPadFace[button]);
        break;
    case PadLS:
    case PadRS:
        DrawText(c, kPadNames[button], cx, cy, 19.0f, 11.5f);
        break;
    case PadView:
        DrawFrame(c, {9.0f, 9.5f, 18.5f, 17.0f, 1.5f}, 1.5f);
        DrawFrame(c, {13.5f, 14.0f, 23.0f, 21.5f, 1.5f}, 1.5f);
        break;
    case PadMenu:
        for (const float y : {11.0f, 15.5f, 20.0f}) {
            const Box line{10.0f, y - 0.9f, 22.0f, y + 0.9f, 0.9f};
            Paint(c, kInk, [&](float px, float py) { return Inside(line, px, py); });
        }
        break;
    case PadShare: {
        DrawArrow(c, cx, 13.0f, 9.0f, 0, kInk);
        const Box tray[] = {{10.0f, 15.5f, 11.8f, 21.5f, 0.6f}, {20.2f, 15.5f, 22.0f, 21.5f, 0.6f},
                            {10.0f, 19.7f, 22.0f, 21.5f, 0.6f}};
        for (const Box& b : tray) {
            Paint(c, kInk, [&](float x, float y) { return Inside(b, x, y); });
        }
        break;
    }
    default: {
        constexpr int kDir[] = {0, 2, 3, 1};
        DrawArrow(c, cx, cy, 12.0f, kDir[std::clamp(button - int(PadUp), 0, 3)], kInk);
        break;
    }
    }
}

void Resolve(const Canvas& c, std::uint8_t* rgba) {
    for (int y = 0; y < c.size; ++y) {
        for (int x = 0; x < c.size; ++x) {
            for (int ch = 0; ch < 4; ++ch) {
                float sum = 0.0f;
                for (int sy = 0; sy < S; ++sy) {
                    for (int sx = 0; sx < S; ++sx) {
                        sum += c.px[((std::size_t(y) * S + sy) * c.n + x * S + sx) * 4 + ch];
                    }
                }
                const float v = std::clamp(sum / (S * S), 0.0f, 1.0f);
                rgba[(y * c.size + x) * 4 + ch] = std::uint8_t(std::lround(v * 255.0f));
            }
        }
    }
}

namespace bc7 {

constexpr int kW2[4] = {0, 21, 43, 64};
constexpr int kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const int* Weights(int bits) {
    return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4;
}
int Interp(int a, int b, int w) {
    return ((64 - w) * a + w * b + 32) >> 6;
}
int Expand(int v, int bits) {
    v <<= 8 - bits;
    return v | (v >> bits);
}

struct Block {
    int px[16][4];
};

struct LineSpec {
    int c0, c1;
    int index_bits;
    int endpoint_bits;
    bool pbits;
};

struct Line {
    int stored[2][4] = {};
    int pbit[2] = {};
    int value[2][4] = {};
    int index[16] = {};
    long long err = LLONG_MAX;
};

void Quantize(const LineSpec& s, const float e[4], int stored[4], int& pbit, int value[4]) {
    if (s.pbits) {
        float best = 1e30f;
        for (int p = 0; p < 2; ++p) {
            int st[4] = {}, val[4] = {};
            float err = 0.0f;
            for (int c = s.c0; c < s.c1; ++c) {
                st[c] = std::clamp(int(std::lround((e[c] - float(p)) / 2.0f)), 0, 127);
                val[c] = st[c] * 2 + p;
                err += (float(val[c]) - e[c]) * (float(val[c]) - e[c]);
            }
            if (err < best) {
                best = err;
                pbit = p;
                std::memcpy(stored, st, sizeof(st));
                std::memcpy(value, val, sizeof(val));
            }
        }
        return;
    }
    const int top = (1 << s.endpoint_bits) - 1;
    for (int c = s.c0; c < s.c1; ++c) {
        const int guess = std::clamp(int(std::lround(e[c] * float(top) / 255.0f)), 0, top);
        int best_v = guess;
        float best = 1e30f;
        for (int v = std::max(guess - 1, 0); v <= std::min(guess + 1, top); ++v) {
            const float d = std::abs(float(Expand(v, s.endpoint_bits)) - e[c]);
            if (d < best) {
                best = d;
                best_v = v;
            }
        }
        stored[c] = best_v;
        value[c] = Expand(best_v, s.endpoint_bits);
    }
}

void Assign(const LineSpec& s, const Block& b, Line& line) {
    const int* w = Weights(s.index_bits);
    const int n = 1 << s.index_bits;
    line.err = 0;
    for (int i = 0; i < 16; ++i) {
        long long best = LLONG_MAX;
        for (int k = 0; k < n; ++k) {
            long long e = 0;
            for (int c = s.c0; c < s.c1; ++c) {
                const int d = Interp(line.value[0][c], line.value[1][c], w[k]) - b.px[i][c];
                e += d * d;
            }
            if (e < best) {
                best = e;
                line.index[i] = k;
            }
        }
        line.err += best;
    }
}

Line Fit(const LineSpec& s, const Block& b) {
    float mean[4] = {};
    for (int i = 0; i < 16; ++i) {
        for (int c = s.c0; c < s.c1; ++c) {
            mean[c] += float(b.px[i][c]) / 16.0f;
        }
    }
    float cov[4][4] = {};
    for (int i = 0; i < 16; ++i) {
        for (int c = s.c0; c < s.c1; ++c) {
            for (int d = s.c0; d < s.c1; ++d) {
                cov[c][d] += (float(b.px[i][c]) - mean[c]) * (float(b.px[i][d]) - mean[d]);
            }
        }
    }
    float axis[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (int it = 0; it < 12; ++it) {
        float next[4] = {};
        float len = 0.0f;
        for (int c = s.c0; c < s.c1; ++c) {
            for (int d = s.c0; d < s.c1; ++d) {
                next[c] += cov[c][d] * axis[d];
            }
            len += next[c] * next[c];
        }
        if (len < 1e-12f) {
            break;
        }
        len = std::sqrt(len);
        for (int c = s.c0; c < s.c1; ++c) {
            axis[c] = next[c] / len;
        }
    }
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; ++i) {
        float t = 0.0f;
        for (int c = s.c0; c < s.c1; ++c) {
            t += (float(b.px[i][c]) - mean[c]) * axis[c];
        }
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    float e[2][4] = {};
    for (int c = s.c0; c < s.c1; ++c) {
        e[0][c] = std::clamp(mean[c] + axis[c] * lo, 0.0f, 255.0f);
        e[1][c] = std::clamp(mean[c] + axis[c] * hi, 0.0f, 255.0f);
    }

    Line best;
    const int* w = Weights(s.index_bits);
    for (int iter = 0; iter < 6; ++iter) {
        Line line;
        Quantize(s, e[0], line.stored[0], line.pbit[0], line.value[0]);
        Quantize(s, e[1], line.stored[1], line.pbit[1], line.value[1]);
        Assign(s, b, line);
        if (line.err < best.err) {
            best = line;
        }
        if (best.err == 0) {
            break;
        }
        float aa = 0.0f, ab = 0.0f, bb = 0.0f, d0[4] = {}, d1[4] = {};
        for (int i = 0; i < 16; ++i) {
            const float t = float(w[line.index[i]]) / 64.0f;
            aa += (1.0f - t) * (1.0f - t);
            ab += (1.0f - t) * t;
            bb += t * t;
            for (int c = s.c0; c < s.c1; ++c) {
                d0[c] += (1.0f - t) * float(b.px[i][c]);
                d1[c] += t * float(b.px[i][c]);
            }
        }
        const float det = aa * bb - ab * ab;
        if (std::abs(det) < 1e-6f) {
            break;
        }
        for (int c = s.c0; c < s.c1; ++c) {
            e[0][c] = std::clamp((bb * d0[c] - ab * d1[c]) / det, 0.0f, 255.0f);
            e[1][c] = std::clamp((aa * d1[c] - ab * d0[c]) / det, 0.0f, 255.0f);
        }
    }
    return best;
}

void Anchor(Line& line, int index_bits) {
    const int n = 1 << index_bits;
    if (line.index[0] < n / 2) {
        return;
    }
    for (int k = 0; k < 4; ++k) {
        std::swap(line.stored[0][k], line.stored[1][k]);
        std::swap(line.value[0][k], line.value[1][k]);
    }
    std::swap(line.pbit[0], line.pbit[1]);
    for (int& i : line.index) {
        i = n - 1 - i;
    }
}

struct Bits {
    std::uint64_t w[2] = {};
    int pos = 0;
    void Put(std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i, ++pos) {
            if ((v >> i) & 1) {
                w[pos >> 6] |= 1ull << (pos & 63);
            }
        }
    }
};

struct Encoded {
    long long err = LLONG_MAX;
    Bits bits;
};

Encoded Mode6(const Block& b) {
    const LineSpec s{0, 4, 4, 7, true};
    Line l = Fit(s, b);
    Anchor(l, 4);
    Encoded out;
    out.err = l.err;
    out.bits.Put(1u << 6, 7);
    for (int c = 0; c < 4; ++c) {
        for (int k = 0; k < 2; ++k) {
            out.bits.Put(l.stored[k][c], 7);
        }
    }
    out.bits.Put(l.pbit[0], 1);
    out.bits.Put(l.pbit[1], 1);
    for (int i = 0; i < 16; ++i) {
        out.bits.Put(l.index[i], i == 0 ? 3 : 4);
    }
    return out;
}

Encoded Mode5(const Block& b) {
    const LineSpec cs{0, 3, 2, 7, false}, as{3, 4, 2, 8, false};
    Line lc = Fit(cs, b), la = Fit(as, b);
    Anchor(lc, 2);
    Anchor(la, 2);
    Encoded out;
    out.err = lc.err + la.err;
    out.bits.Put(1u << 5, 6);
    out.bits.Put(0, 2);
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 2; ++k) {
            out.bits.Put(lc.stored[k][c], 7);
        }
    }
    for (int k = 0; k < 2; ++k) {
        out.bits.Put(la.stored[k][3], 8);
    }
    for (int i = 0; i < 16; ++i) {
        out.bits.Put(lc.index[i], i == 0 ? 1 : 2);
    }
    for (int i = 0; i < 16; ++i) {
        out.bits.Put(la.index[i], i == 0 ? 1 : 2);
    }
    return out;
}

Encoded Mode4(const Block& b, int index_mode) {
    const int color_bits = index_mode ? 3 : 2, alpha_bits = index_mode ? 2 : 3;
    const LineSpec cs{0, 3, color_bits, 5, false}, as{3, 4, alpha_bits, 6, false};
    Line lc = Fit(cs, b), la = Fit(as, b);
    Anchor(lc, color_bits);
    Anchor(la, alpha_bits);
    Encoded out;
    out.err = lc.err + la.err;
    out.bits.Put(1u << 4, 5);
    out.bits.Put(0, 2);
    out.bits.Put(index_mode, 1);
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 2; ++k) {
            out.bits.Put(lc.stored[k][c], 5);
        }
    }
    for (int k = 0; k < 2; ++k) {
        out.bits.Put(la.stored[k][3], 6);
    }
    const Line& two = index_mode ? la : lc;
    const Line& three = index_mode ? lc : la;
    for (int i = 0; i < 16; ++i) {
        out.bits.Put(two.index[i], i == 0 ? 1 : 2);
    }
    for (int i = 0; i < 16; ++i) {
        out.bits.Put(three.index[i], i == 0 ? 2 : 3);
    }
    return out;
}

void EncodeBlock(const Block& b, std::uint8_t out[16]) {
    Encoded best = Mode6(b);
    for (Encoded e : {Mode5(b), Mode4(b, 0), Mode4(b, 1)}) {
        if (e.err < best.err) {
            best = e;
        }
    }
    std::memcpy(out, best.bits.w, 16);
}

}

}

void Draw(const Spec& spec, std::uint8_t* rgba, int size) {
    const std::string first = spec.labels.empty() ? std::string() : spec.labels[0];
    const bool square = spec.kind == Spec::Pad ? !PadWide(spec.button)
                                               : spec.kind == Spec::Key && (Utf8(first).size() <= 2 || ArrowDir(first) >= 0);
    float scale = float(size) / kSize, origin = 0.0f;
    if (size != kSize && square) {
        scale = float(size) / 27.0f;
        origin = -2.5f * scale;
    }
    Canvas c(size, scale, origin, origin);
    switch (spec.kind) {
    case Spec::Mouse:
        DrawMouse(c, spec.button);
        break;
    case Spec::Pad:
        DrawPad(c, spec.button);
        break;
    case Spec::Keys:
        if (spec.labels.size() >= 4) {
            DrawLabel(c, {10.75f, 0.5f, 21.25f, 14.5f, 2.5f}, spec.labels[0], 7.5f);
            DrawLabel(c, {0.0f, 16.0f, 10.5f, 30.0f, 2.5f}, spec.labels[1], 7.5f);
            DrawLabel(c, {10.75f, 16.0f, 21.25f, 30.0f, 2.5f}, spec.labels[2], 7.5f);
            DrawLabel(c, {21.5f, 16.0f, 32.0f, 30.0f, 2.5f}, spec.labels[3], 7.5f);
        } else if (spec.labels.size() >= 2) {
            DrawLabel(c, {0.0f, 6.0f, 15.5f, 26.0f, 3.0f}, spec.labels[0], 9.5f);
            DrawLabel(c, {16.5f, 6.0f, 32.0f, 26.0f, 3.0f}, spec.labels[1], 9.5f);
        }
        break;
    case Spec::Key:
    default: {
        const std::string label = spec.labels.empty() ? std::string() : spec.labels[0];
        const std::size_t glyphs = Utf8(label).size();
        if (glyphs <= 2 || ArrowDir(label) >= 0) {
            DrawLabel(c, {3.0f, 2.5f, 29.0f, 28.0f, 4.5f}, label, glyphs <= 1 ? 13.0f : 11.0f);
        } else {
            DrawLabel(c, {0.5f, 6.0f, 31.5f, 26.0f, 4.0f}, label, 10.0f);
        }
        break;
    }
    }
    Resolve(c, rgba);
}

void EncodeBlocks(const std::uint8_t* rgba, int width, int height, std::uint8_t* out) {
    for (int by = 0; by < height / 4; ++by) {
        for (int bx = 0; bx < width / 4; ++bx) {
            bc7::Block block;
            for (int p = 0; p < 16; ++p) {
                const int x = bx * 4 + p % 4, y = by * 4 + p / 4;
                for (int ch = 0; ch < 4; ++ch) {
                    block.px[p][ch] = rgba[(y * width + x) * 4 + ch];
                }
            }
            bc7::EncodeBlock(block, out);
            out += 16;
        }
    }
}

void Encode(const std::uint8_t rgba[kSize * kSize * 4], std::uint8_t out[kBytes]) {
    for (int i = 0; i < 64; ++i) {
        int bx = 0, by = 0;
        for (int k = 0; k < 3; ++k) {
            bx |= ((i >> (2 * k)) & 1) << k;
            by |= ((i >> (2 * k + 1)) & 1) << k;
        }
        bc7::Block block;
        for (int p = 0; p < 16; ++p) {
            const int x = bx * 4 + p % 4, y = by * 4 + p / 4;
            for (int ch = 0; ch < 4; ++ch) {
                block.px[p][ch] = rgba[(y * kSize + x) * 4 + ch];
            }
        }
        bc7::EncodeBlock(block, out + i * 16);
    }
}

}
