/// @file render_internal.hpp
/// Internal display-list and helper declarations shared across the figure renderer modules.
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/font.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/ref.h>
#include <keel/static_array.h>
#include <keel/string.h>
#include <numkit/graphics/figure_render.hpp>

#include <initializer_list>

namespace numkit::figrender::detail {

inline constexpr RenderRgb kAxisCol(0x26, 0x26, 0x26);
inline constexpr RenderRgb kGridCol(0xe2, 0xe2, 0xe2);
inline constexpr RenderRgb kMinorGridCol(0xf0, 0xf0, 0xf0);
inline constexpr RenderRgb kTextCol(0x18, 0x18, 0x18);

struct Rect {
    keel::f64 x0 = 0.0;
    keel::f64 y0 = 0.0;
    keel::f64 x1 = 0.0;
    keel::f64 y1 = 0.0;
};

struct AxisRange {
    keel::f64 lo = 0.0;
    keel::f64 hi = 1.0;
    bool log = false;

    keel::f64 map(keel::f64 v, keel::f64 p0, keel::f64 p1) const noexcept
    {
        keel::f64 t = 0.0;
        if (log) {
            const keel::f64 a = keel::log10(lo);
            const keel::f64 b = keel::log10(hi);
            t = (keel::log10(v) - a) / (b - a);
        } else {
            t = (v - lo) / (hi - lo);
        }
        return p0 + t * (p1 - p0);
    }
};

struct PlotTransform {
    Rect plot;
    AxisRange xr;
    AxisRange yr;
    AxisRange yr2;
    keel::f64 px0 = 0.0, px1 = 0.0;
    keel::f64 py0 = 0.0, py1 = 0.0;

    keel::f64 X(keel::f64 v) const noexcept { return xr.map(v, px0, px1); }
    keel::f64 Y(keel::f64 v) const noexcept { return yr.map(v, py0, py1); }
    keel::f64 Y2(keel::f64 v) const noexcept { return yr2.map(v, py0, py1); }
    keel::f64 Ys(keel::f64 v, bool rightY) const noexcept { return rightY ? Y2(v) : Y(v); }
    const AxisRange &yRange(bool rightY) const noexcept { return rightY ? yr2 : yr; }
    bool markerInPlot(keel::f64 mx, keel::f64 my) const noexcept
    {
        return mx >= plot.x0 - 2.0 && mx <= plot.x1 + 2.0
            && my >= plot.y0 - 2.0 && my <= plot.y1 + 2.0;
    }
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

inline bool is3dDataset(const RenderDataset &ds) noexcept
{
    return ds.kind == RenderChartKind::Surface
        || ds.kind == RenderChartKind::Mesh
        || ds.kind == RenderChartKind::Waterfall
        || (ds.kind == RenderChartKind::Plot3 && !ds.z.empty());
}

inline char canonicalMarkerGlyph(keel::StringRef k) noexcept
{
    if (k.empty()) return 'o';
    if (k == "s" || k == "square")    return 's';
    if (k == "d" || k == "diamond")   return 'd';
    if (k == "p" || k == "pentagram") return 'p';
    if (k == "h" || k == "hexagram")  return 'h';
    const char c = k[0];
    if (c == 'o' || c == '+' || c == 'x' || c == '*' || c == '.'
        || c == '^' || c == 'v' || c == '>' || c == '<')
        return c;
    return 'o';
}

/// Generates screen-space polygon vertices (y-down) for polygonal markers
/// ('s', 'd', '^', 'v', '>', '<', 'p', 'h'). Returns vertex count (0 if non-polygon).
inline keel::i32 buildMarkerPolygon(char glyph, keel::f64 cx, keel::f64 cy, keel::f64 r,
                                    keel::f64 outX[12], keel::f64 outY[12]) noexcept
{
    switch (glyph) {
    case 's': {
        const keel::f64 s = r * 0.92;
        outX[0] = cx - s; outY[0] = cy - s;
        outX[1] = cx + s; outY[1] = cy - s;
        outX[2] = cx + s; outY[2] = cy + s;
        outX[3] = cx - s; outY[3] = cy + s;
        return 4;
    }
    case 'd': {
        const keel::f64 rx = r * 0.85, ry = r * 1.15;
        outX[0] = cx;      outY[0] = cy - ry;
        outX[1] = cx + rx; outY[1] = cy;
        outX[2] = cx;      outY[2] = cy + ry;
        outX[3] = cx - rx; outY[3] = cy;
        return 4;
    }
    case '^': {
        const keel::f64 w = r * 1.05, top = r * 1.05, bot = r * 0.72;
        outX[0] = cx;     outY[0] = cy - top;
        outX[1] = cx + w; outY[1] = cy + bot;
        outX[2] = cx - w; outY[2] = cy + bot;
        return 3;
    }
    case 'v': {
        const keel::f64 w = r * 1.05, top = r * 0.72, bot = r * 1.05;
        outX[0] = cx - w; outY[0] = cy - top;
        outX[1] = cx + w; outY[1] = cy - top;
        outX[2] = cx;     outY[2] = cy + bot;
        return 3;
    }
    case '>': {
        const keel::f64 h = r * 1.05, tip = r * 1.05, back = r * 0.72;
        outX[0] = cx - back; outY[0] = cy - h;
        outX[1] = cx + tip;  outY[1] = cy;
        outX[2] = cx - back; outY[2] = cy + h;
        return 3;
    }
    case '<': {
        const keel::f64 h = r * 1.05, tip = r * 1.05, back = r * 0.72;
        outX[0] = cx + back; outY[0] = cy - h;
        outX[1] = cx + back; outY[1] = cy + h;
        outX[2] = cx - tip;  outY[2] = cy;
        return 3;
    }
    case 'p': {
        const keel::f64 rOut = r * 1.15, rIn = r * 0.48;
        for (keel::i32 i = 0; i < 10; ++i) {
            const keel::f64 ang = -keel::kPi * 0.5 + keel::as<keel::f64>(i) * (keel::kPi / 5.0);
            const keel::f64 rad = (i % 2 == 0) ? rOut : rIn;
            outX[i] = cx + rad * keel::cos(ang);
            outY[i] = cy + rad * keel::sin(ang);
        }
        return 10;
    }
    case 'h': {
        const keel::f64 rOut = r * 1.15, rIn = r * 0.62;
        for (keel::i32 i = 0; i < 12; ++i) {
            const keel::f64 ang = -keel::kPi * 0.5 + keel::as<keel::f64>(i) * (keel::kPi / 6.0);
            const keel::f64 rad = (i % 2 == 0) ? rOut : rIn;
            outX[i] = cx + rad * keel::cos(ang);
            outY[i] = cy + rad * keel::sin(ang);
        }
        return 12;
    }
    default:
        return 0;
    }
}

/// Computes auto-scale multiplier for a Quiver dataset (1.0 if disabled).
keel::f64 computeQuiverScale(const RenderDataset &ds);

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

// ── Display-list primitives (render_display.cpp) ─────────────────────
void addLine(keel::Array<Cmd> &out, keel::ArrayRef<keel::f64> xs,
             keel::ArrayRef<keel::f64> ys, RenderRgb c, keel::f64 w,
             RenderLineStyle ls = RenderLineStyle::Solid);
void addLine(keel::Array<Cmd> &out, std::initializer_list<keel::f64> xs,
             std::initializer_list<keel::f64> ys, RenderRgb c, keel::f64 w,
             RenderLineStyle ls = RenderLineStyle::Solid);
void addClippedLine(keel::Array<Cmd> &out, keel::ArrayRef<keel::f64> xs,
                    keel::ArrayRef<keel::f64> ys, const Rect &clip, RenderRgb c,
                    keel::f64 w, RenderLineStyle ls);
void addClippedSeg(keel::Array<Cmd> &out, keel::f64 x0, keel::f64 y0,
                   keel::f64 x1, keel::f64 y1, const Rect &clip, RenderRgb c,
                   keel::f64 w, RenderLineStyle ls = RenderLineStyle::Solid);
void addPoly(keel::Array<Cmd> &out, keel::ArrayRef<keel::f64> xs,
             keel::ArrayRef<keel::f64> ys, RenderRgb c, bool fill);
void addPoly(keel::Array<Cmd> &out, std::initializer_list<keel::f64> xs,
             std::initializer_list<keel::f64> ys, RenderRgb c, bool fill);
void addMarker(keel::Array<Cmd> &out, keel::f64 x, keel::f64 y, keel::f64 size,
               RenderRgb edgeColor, keel::StringRef kind, bool hasFill = false,
               RenderRgb fillColor = RenderRgb(0, 0, 0), bool noEdge = false,
               keel::f64 strokeW = 1.4);
keel::f64 addAlignedText(keel::Array<Cmd> &out, keel::StringRef rawText,
                         keel::f64 anchorX, keel::f64 anchorY,
                         keel::f64 fontSizePx, RenderRgb color,
                         RenderHAlign halign = RenderHAlign::Left,
                         TextVAlign valign = TextVAlign::Baseline,
                         bool rot90 = false, bool bold = false,
                         keel::StringRef family = "");
void renderTitleAndSubtitle(const RenderAxes &ax, const Rect &plot,
                            keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out);
void renderLegend(const RenderAxes &ax, const Rect &plot,
                  keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out);

// ── Specialized renderers ────────────────────────────────────────────
void render2dSeries(const RenderAxes &ax, const PlotTransform &tf,
                    keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out);
void renderPolarAxes(const RenderAxes &ax, const Rect &plot,
                     keel::f64 uiScale, keel::f64 ptToPx, keel::Array<Cmd> &out);
void render3dContent(const RenderAxes &ax, const Rect &plot,
                     keel::f64 uiScale, keel::Array<Cmd> &out);

// ── Top-level display-list entry point ───────────────────────────────
keel::Array<Cmd> buildFigureDisplayList(const FigureState &fig, keel::i32 W, keel::i32 H);

} // namespace numkit::figrender::detail
