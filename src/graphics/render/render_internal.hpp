/// @file render_internal.hpp
/// Internal display-list and helper declarations shared across the figure renderer modules.
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/font.h>
#include <keel/format.h>
#include <keel/ref.h>
#include <keel/static_array.h>
#include <keel/string.h>
#include <numkit/graphics/figure_render.hpp>

#include <initializer_list>

namespace numkit::figrender::detail {

struct Rect {
    keel::f64 x0 = 0.0;
    keel::f64 y0 = 0.0;
    keel::f64 x1 = 0.0;
    keel::f64 y1 = 0.0;
};

enum class TextVAlign : keel::u8 {
    Baseline = 0,
    Top,
    Middle,
};

struct Cmd {
    enum class Kind : keel::u8 { Line = 0, Poly, Text, Mark, Image };
    Kind kind = Kind::Line;

    keel::Array<keel::f64> xs;
    keel::Array<keel::f64> ys;
    RenderRgb color{};
    RenderRgb fillColor{};
    keel::f64 width = 1.0;          // stroke width or marker diameter (px)
    keel::f64 strokeWidth = 1.4;    // marker outline width (px)
    bool fill = false;              // Poly: filled; Mark: filled face
    bool noEdge = false;            // Mark: MarkerEdgeColor == 'none'
    RenderLineStyle lineStyle = RenderLineStyle::Solid;

    keel::String text;              // Text UTF-8 payload, or Mark glyph selector
    keel::f64 fontSize = 12.0;      // Text size (px)
    keel::f64 x = 0.0;              // Text baseline anchor / Mark center
    keel::f64 y = 0.0;
    bool rot90 = false;             // Text rotated 90 deg CCW
    bool bold = false;
    keel::String fontFamily;

    // Image payload (indexed column-major 0..254 + 255=NaN, or direct row-major RGBA)
    keel::Array<keel::u8> img;
    keel::i32 imgW = 0;
    keel::i32 imgH = 0;
    keel::f64 qLo = 0.0;
    keel::f64 qHi = 1.0;
    keel::f64 climLo = 0.0;
    keel::f64 climHi = 1.0;
    keel::String cmap;
    keel::Array<RenderRgb> customCmap;
    bool rgbDirect = false;
};

inline keel::String rgbHex(RenderRgb c)
{
    return keel::format("#{:02x}{:02x}{:02x}",
                        keel::as<keel::u32>(c.r),
                        keel::as<keel::u32>(c.g),
                        keel::as<keel::u32>(c.b));
}

// ── Palette helpers (render_palette.cpp) ─────────────────────────────
const keel::StaticArray<RenderRgb, 8> &seriesPalette() noexcept;
RenderRgb sampleColormap(keel::f64 t, keel::StringRef cmapName,
                         keel::ArrayRef<RenderRgb> customCmap) noexcept;
keel::Array<keel::u8> imageCmdRgb(const Cmd &c);

// ── Typography & TeX helpers (render_text.cpp) ───────────────────────
const keel::font::FontFace *getPlotFontFace(bool bold, bool &syntheticBold);
keel::String decodeTexToUtf8(keel::StringRef src);
keel::f64 measureTextWidthPx(keel::StringRef utf8Text, keel::f64 fontSizePx, bool bold);
keel::f64 fontCapHeightPx(keel::f64 fontSizePx, bool bold);
const keel::u8 *font5x7(char c) noexcept;

// ── Display-list builder (render_display.cpp) ────────────────────────
keel::Array<Cmd> buildFigureDisplayList(const FigureState &fig, keel::i32 W, keel::i32 H);

} // namespace numkit::figrender::detail
