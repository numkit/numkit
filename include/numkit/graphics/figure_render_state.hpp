/// @file figure_render_state.hpp
/// @ingroup group_graphics
///
/// Strongly-typed ephemeral render-state DTOs projected directly from
/// GraphicsScene for software figure export (SVG / EPS / PDF / RGB raster).
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/string.h>

namespace numkit {

/// @addtogroup group_graphics
/// @{

struct RenderRgb {
    keel::u8 r = 0u;
    keel::u8 g = 0u;
    keel::u8 b = 0u;

    constexpr RenderRgb() noexcept = default;
    constexpr RenderRgb(keel::u8 rr, keel::u8 gg, keel::u8 bb) noexcept
        : r(rr), g(gg), b(bb) {}
};

enum class RenderChartKind : keel::u8 {
    Line = 0,
    Stairs,
    Stem,
    Scatter,
    Bar,
    Histogram,
    Area,
    ErrorBar,
    Quiver,
    Polygon,
    Surface,
    Mesh,
    Waterfall,
    Plot3,
    Contour,
    ImageIndexed,
    ImageRgb,
    Text,
};

enum class RenderLineStyle : keel::u8 {
    Solid = 0,
    Dashed,
    Dotted,
    DashDot,
    None,
};

enum class RenderHAlign : keel::u8 {
    Left = 0,
    Center,
    Right,
};

struct RenderDataset {
    RenderChartKind kind = RenderChartKind::Line;
    bool visible = true;
    bool handleVisible = true;
    bool rightYAxis = false;

    keel::String label;   // DisplayName (chart) or String (Text)

    // Typed numeric coordinate buffers (1-D vectors, or row-major zRows x zCols grid for z)
    keel::Array<keel::f64> x;
    keel::Array<keel::f64> y;
    keel::Array<keel::f64> z;
    keel::usize zRows = 0;
    keel::usize zCols = 0;

    // Optional chart-specific auxiliary numeric vectors
    keel::Array<keel::f64> u;       // Quiver UData
    keel::Array<keel::f64> v;       // Quiver VData
    keel::Array<keel::f64> eNeg;    // ErrorBar YNegativeDelta
    keel::Array<keel::f64> ePos;    // ErrorBar YPositiveDelta
    keel::Array<keel::f64> sizes;   // Scatter SizeData

    // Typed line & marker style
    RenderRgb color{0x00, 0x72, 0xbd};
    bool hasColor = false;
    RenderLineStyle lineStyle = RenderLineStyle::Solid;
    keel::f64 lineWidth = 0.0;      // points (0 => default)

    keel::String marker;            // "", "o", "+", "*", ".", "x", "s", "d", ...
    keel::f64 markerSize = 0.0;     // points (0 => default)
    RenderRgb markerFaceColor{0, 0, 0};
    bool hasMarkerFaceColor = false;
    bool markerFaceAuto = false;
    RenderRgb markerEdgeColor{0, 0, 0};
    bool hasMarkerEdgeColor = false;
    bool markerEdgeNone = false;

    // Histogram / Patch / Area face & edge styling
    RenderRgb faceColor{0x00, 0x72, 0xbd};
    bool hasFaceColor = false;
    RenderRgb edgeColor{0xd9, 0xd9, 0xd9};
    bool hasEdgeColor = false;
    keel::f64 faceAlpha = 1.0;
    keel::f64 binWidth = 0.0;

    // Text annotation properties (kind == RenderChartKind::Text)
    keel::f64 fontSize = 10.0;
    RenderHAlign halign = RenderHAlign::Left;

    // Raster image data (ImageIndexed: col-major 0..254, 255=NaN; ImageRgb: row-major RGBA)
    keel::Array<keel::u8> imagePixels;
    keel::usize imageRows = 0;
    keel::usize imageCols = 0;
    keel::f64 cmin = 0.0;
    keel::f64 cmax = 1.0;
};

struct RenderAxes {
    keel::Array<RenderDataset> datasets;

    // Normalized inner plot rectangle [left, bottom, width, height] in [0, 1]
    bool hasPosition = false;
    keel::f64 posLeft = 0.13;
    keel::f64 posBottom = 0.11;
    keel::f64 posWidth = 0.775;
    keel::f64 posHeight = 0.815;

    bool polar = false;
    bool axisVisible = true;
    bool boxOn = false;
    bool gridMajor = false;
    bool gridMinor = false;

    keel::String title;
    keel::String subtitle;
    keel::String xlabel;
    keel::String ylabel;
    keel::String ylabel2;
    keel::String zlabel;

    // Typed explicit/scene limits
    bool hasXLim = false;  keel::f64 xLo = 0.0, xHi = 1.0;
    bool hasYLim = false;  keel::f64 yLo = 0.0, yHi = 1.0;
    bool hasY2Lim = false; keel::f64 y2Lo = 0.0, y2Hi = 1.0;
    bool hasZLim = false;  keel::f64 zLo = 0.0, zHi = 1.0;
    bool hasRLim = false;  keel::f64 rLo = 0.0, rHi = 1.0;
    bool hasCLim = false;  keel::f64 cLo = 0.0, cHi = 1.0;

    // Typed ticks & labels (empty => auto-generated)
    keel::Array<keel::f64> xTicks;
    keel::Array<keel::f64> yTicks;
    keel::Array<keel::f64> rTicks;
    keel::Array<keel::f64> thetaTicks;
    keel::Array<keel::String> xTickLabels;
    keel::Array<keel::String> yTickLabels;
    keel::Array<keel::String> thetaTickLabels;
    keel::String xTickFormat;
    keel::String yTickFormat;

    // Axis scale, direction, aspect ratio, dual Y
    bool xLog = false;
    bool yLog = false;
    bool y2Log = false;
    bool xReverse = false;
    bool yReverse = false;
    bool equalAspect = false;
    bool yyEnabled = false;

    // Polar configuration
    bool thetaClockwise = false;
    keel::String thetaZeroLocation; // "right", "top", "left", "bottom"

    // 3-D camera view & colormap / colorbar
    keel::f64 viewAz = -37.5;
    keel::f64 viewEl = 30.0;
    keel::String colormapName;
    keel::Array<RenderRgb> customColormap;
    keel::String colorbarLocation;

    // Typography & axis line width
    keel::f64 fontSize = 10.0;
    bool fontBold = false;
    keel::String fontName;
    keel::f64 lineWidth = 0.5;

    // Legend
    keel::Array<keel::String> legendLabels;
    keel::String legendLocation;
    bool legendBoxOn = true;
    bool legendVisible = false;
};

struct RenderAnnotation {
    keel::String type;   // "line", "arrow", "textarrow", "rectangle", "textbox", "ellipse"
    keel::f64 x1 = 0.0;
    keel::f64 y1 = 0.0;
    keel::f64 x2 = 0.0;
    keel::f64 y2 = 0.0;
    keel::String text;
    RenderRgb color{33, 33, 33};
    RenderRgb faceColor{255, 255, 255};
    bool hasFaceColor = false;
    keel::f64 fontSize = 10.0;
};

struct FigureState {
    keel::i32 id = 1;
    keel::f64 figWidth = 560.0;
    keel::f64 figHeight = 420.0;
    RenderRgb bgColor{255, 255, 255};
    keel::String superTitle;
    keel::Array<RenderAxes> axes;
    keel::Array<RenderAnnotation> annotations;
};

/// @}

} // namespace numkit
