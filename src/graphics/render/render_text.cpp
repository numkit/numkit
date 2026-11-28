// src/graphics/render/render_text.cpp
//
// Thread-safe TrueType font cache (via keel::font), UTF-8 text measurement,
// TeX-to-UTF-8 symbol decoder, and built-in 5x7 ASCII fallback font.

#include "render/render_internal.hpp"

#include <keel/fs.h>
#include <keel/sync.h>
#include <keel/utf8.h>

namespace numkit::figrender::detail {

namespace {

struct PlotFontCache {
    keel::Mutex mu;
    bool initialized = false;
    keel::Array<keel::u8> regBytes;
    keel::Array<keel::u8> boldBytes;
    keel::font::FontFace regFace;
    keel::font::FontFace boldFace;

    void ensureLoaded()
    {
        keel::LockGuard lock(mu);
        if (initialized)
            return;
        initialized = true;

        struct FontPair {
            const char *regPath;
            const char *boldPath;
        };
        static const FontPair kCandidates[] = {
            {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"},
            {"/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
             "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf"},
            {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
             "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"},
            {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
            {"/usr/share/fonts/TTF/DejaVuSans.ttf",
             "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"},
            {"/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
             "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf"},
            {"/System/Library/Fonts/Supplemental/Arial.ttf",
             "/System/Library/Fonts/Supplemental/Arial Bold.ttf"},
            {"/Library/Fonts/Arial.ttf",
             "/Library/Fonts/Arial Bold.ttf"},
            {"C:/Windows/Fonts/arial.ttf",
             "C:/Windows/Fonts/arialbd.ttf"},
        };

        for (const FontPair &cand : kCandidates) {
            auto r = keel::fs_read_file(cand.regPath);
            if (!r.is_ok() || r->empty())
                continue;
            keel::Array<keel::u8> bytes = keel::move(*r);
            keel::font::FontFace face;
            if (!face.load(bytes.as_ref()))
                continue;
            regBytes = keel::move(bytes);
            regFace.load(regBytes.as_ref());

            if (cand.boldPath) {
                auto rb = keel::fs_read_file(cand.boldPath);
                if (rb.is_ok() && !rb->empty()) {
                    keel::Array<keel::u8> bbytes = keel::move(*rb);
                    keel::font::FontFace bface;
                    if (bface.load(bbytes.as_ref())) {
                        boldBytes = keel::move(bbytes);
                        boldFace.load(boldBytes.as_ref());
                    }
                }
            }
            break;
        }
    }
};

PlotFontCache &plotFontCache()
{
    static PlotFontCache cache;
    return cache;
}

} // namespace

const keel::font::FontFace *getPlotFontFace(bool bold, bool &syntheticBold)
{
    PlotFontCache &cache = plotFontCache();
    cache.ensureLoaded();
    if (bold && cache.boldFace.is_valid()) {
        syntheticBold = false;
        return &cache.boldFace;
    }
    if (cache.regFace.is_valid()) {
        syntheticBold = bold;
        return &cache.regFace;
    }
    syntheticBold = false;
    return nullptr;
}

keel::String decodeTexToUtf8(keel::StringRef src)
{
    if (src.find('\\') == keel::kInvalidIndex && src.find('^') == keel::kInvalidIndex)
        return keel::String::from(src);

    struct TexEntry {
        const char *name;
        const char *utf8;
    };
    static const TexEntry kTexSymbols[] = {
        {"rightarrow", "\xE2\x86\x92"},
        {"leftarrow",  "\xE2\x86\x90"},
        {"varepsilon", "\xCE\xB5"},
        {"vartheta",   "\xCF\x91"},
        {"varsigma",   "\xCF\x82"},
        {"epsilon",    "\xCE\xB5"},
        {"upsilon",    "\xCF\x85"},
        {"Upsilon",    "\xCE\xA5"},
        {"approx",     "\xE2\x89\x88"},
        {"lambda",     "\xCE\xBB"},
        {"Lambda",     "\xCE\x9B"},
        {"varphi",     "\xCF\x86"},
        {"propto",     "\xE2\x88\x9D"},
        {"partial",    "\xE2\x88\x82"},
        {"alpha",      "\xCE\xB1"},
        {"gamma",      "\xCE\xB3"},
        {"Gamma",      "\xCE\x93"},
        {"delta",      "\xCE\xB4"},
        {"Delta",      "\xCE\x94"},
        {"theta",      "\xCE\xB8"},
        {"Theta",      "\xCE\x98"},
        {"kappa",      "\xCE\xBA"},
        {"sigma",      "\xCF\x83"},
        {"Sigma",      "\xCE\xA3"},
        {"omega",      "\xCF\x89"},
        {"Omega",      "\xCE\xA9"},
        {"times",      "\xC3\x97"},
        {"infty",      "\xE2\x88\x9E"},
        {"nabla",      "\xE2\x88\x87"},
        {"equiv",      "\xE2\x89\xA1"},
        {"beta",       "\xCE\xB2"},
        {"zeta",       "\xCE\xB6"},
        {"iota",       "\xCE\xB9"},
        {"circ",       "\xC2\xB0"},
        {"cdot",       "\xC2\xB7"},
        {"sqrt",       "\xE2\x88\x9A"},
        {"eta",        "\xCE\xB7"},
        {"rho",        "\xCF\x81"},
        {"tau",        "\xCF\x84"},
        {"phi",        "\xCF\x86"},
        {"Phi",        "\xCE\xA6"},
        {"chi",        "\xCF\x87"},
        {"psi",        "\xCF\x88"},
        {"Psi",        "\xCE\xA8"},
        {"deg",        "\xC2\xB0"},
        {"div",        "\xC3\xB7"},
        {"leq",        "\xE2\x89\xA4"},
        {"geq",        "\xE2\x89\xA5"},
        {"neq",        "\xE2\x89\xA0"},
        {"mu",         "\xCE\xBC"},
        {"nu",         "\xCE\xBD"},
        {"xi",         "\xCE\xBE"},
        {"Xi",         "\xCE\x9E"},
        {"pi",         "\xCF\x80"},
        {"Pi",         "\xCE\xA0"},
        {"pm",         "\xC2\xB1"},
        {"le",         "\xE2\x89\xA4"},
        {"ge",         "\xE2\x89\xA5"},
        {"ne",         "\xE2\x89\xA0"},
    };

    keel::String out;
    out.reserve(src.size());
    keel::usize i = 0;
    while (i < src.size()) {
        if (src[i] == '^') {
            if (i + 6 <= src.size() && src.substr(i, 6) == "^\\circ") {
                out.append("\xC2\xB0");
                i += 6;
                continue;
            }
            if (i + 8 <= src.size() && src.substr(i, 8) == "^{\\circ}") {
                out.append("\xC2\xB0");
                i += 8;
                continue;
            }
        }
        if (src[i] == '\\' && i + 1 < src.size()) {
            keel::usize end = i + 1;
            while (end < src.size() && keel::is_ascii_alpha(src[end]))
                ++end;
            if (end > i + 1) {
                const keel::StringRef cmd = src.substr(i + 1, end - (i + 1));
                bool matched = false;
                for (const TexEntry &e : kTexSymbols) {
                    if (cmd == e.name) {
                        out.append(e.utf8);
                        i = end;
                        matched = true;
                        break;
                    }
                }
                if (matched)
                    continue;
            }
        }
        out.push_back(src[i]);
        ++i;
    }
    return out;
}

keel::f64 measureTextWidthPx(keel::StringRef utf8Text, keel::f64 fontSizePx, bool bold)
{
    if (utf8Text.empty())
        return 0.0;
    bool synthBold = false;
    const keel::font::FontFace *face = getPlotFontFace(bold, synthBold);
    if (!face)
        return keel::as<keel::f64>(keel::utf8_codepoint_count(utf8Text)) * 0.6 * fontSizePx;

    const keel::f32 sz = keel::as<keel::f32>(fontSizePx);
    keel::f64 width = 0.0;
    keel::u32 prevGid = 0;
    bool hasPrev = false;
    keel::usize off = 0;
    while (off < utf8Text.size()) {
        const keel::u32 cp = keel::utf8_decode_next(utf8Text, off);
        const keel::u32 gid = face->glyph_index(cp);
        if (hasPrev)
            width += keel::as<keel::f64>(face->kerning_px(prevGid, gid, sz, keel::font::Hinting::Light));
        width += keel::as<keel::f64>(face->advance_width_px(gid, sz, keel::font::Hinting::Light, synthBold));
        prevGid = gid;
        hasPrev = true;
    }
    return width;
}

keel::f64 fontCapHeightPx(keel::f64 fontSizePx, bool bold)
{
    bool synthBold = false;
    const keel::font::FontFace *face = getPlotFontFace(bold, synthBold);
    if (face && face->units_per_em() > 0 && face->cap_height() > 0) {
        return fontSizePx * keel::as<keel::f64>(face->cap_height())
               / keel::as<keel::f64>(face->units_per_em());
    }
    return fontSizePx * 0.72;
}

// Public-domain 5x7 column-encoded bitmap font (ASCII 0x20..0x7E) used only
// when no system TrueType font is available.
const keel::u8 *font5x7(char c) noexcept
{
    static const keel::u8 kFont[][5] = {
        {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
        {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
        {0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},{0x00,0x1C,0x22,0x41,0x00},
        {0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
        {0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},
        {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
        {0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},{0x18,0x14,0x12,0x7F,0x10},
        {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},
        {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},
        {0x00,0x40,0x34,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
        {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},{0x3E,0x41,0x5D,0x59,0x4E},
        {0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
        {0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
        {0x3E,0x41,0x41,0x51,0x73},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
        {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
        {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
        {0x26,0x49,0x49,0x49,0x32},{0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},
        {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
        {0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43},{0x00,0x7F,0x41,0x41,0x41},
        {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x41,0x7F},{0x04,0x02,0x01,0x02,0x04},
        {0x40,0x40,0x40,0x40,0x40},{0x00,0x03,0x07,0x08,0x00},{0x20,0x54,0x54,0x78,0x40},
        {0x7F,0x28,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x28},{0x38,0x44,0x44,0x28,0x7F},
        {0x38,0x54,0x54,0x54,0x18},{0x00,0x08,0x7E,0x09,0x02},{0x18,0xA4,0xA4,0x9C,0x78},
        {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x40,0x3D,0x00},
        {0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x78,0x04,0x78},
        {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0xFC,0x18,0x24,0x24,0x18},
        {0x18,0x24,0x24,0x18,0xFC},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x24},
        {0x04,0x04,0x3F,0x44,0x24},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
        {0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x4C,0x90,0x90,0x90,0x7C},
        {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x77,0x00,0x00},
        {0x00,0x41,0x36,0x08,0x00},{0x02,0x01,0x02,0x04,0x02}
    };
    static_assert(sizeof(kFont) / sizeof(kFont[0]) == 95,
                  "font5x7 must cover ASCII 0x20..0x7E");
    if (c < 0x20 || c > 0x7E)
        c = '?';
    return kFont[c - 0x20];
}

} // namespace numkit::figrender::detail
