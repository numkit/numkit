/// @file figure_render_state.hpp
/// @ingroup group_graphics
///
/// Ephemeral render-state DTOs projected from GraphicsScene for software
/// figure export (SVG / EPS / PDF / RGB raster).
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/ops/decimate.hpp>

namespace numkit {

/// @addtogroup group_graphics
/// @{

namespace figdetail {

keel::usize countFlatElements(keel::StringRef j);
keel::Array<keel::f64> parseFlatDoubles(const keel::String &j);
keel::String serializeFlatDoubles(keel::ArrayRef<keel::f64> v);
keel::String serializeFlatDoubles(const keel::Array<keel::f64> &v);
void appendDouble(keel::String &out, keel::f64 v);
void appendInt(keel::String &out, keel::i32 v);
void appendUsize(keel::String &out, keel::usize v);

} // namespace figdetail

struct DatasetInfo
{
    keel::String xJson;
    keel::String yJson;
    keel::String zJson;
    keel::String sizeJson;
    keel::String colorJson;
    keel::String type;
    keel::String label;
    keel::String style;
    keel::f64 lineWidth = 0.0;
    keel::f64 markerSize = 0.0;
    bool visible = true;
    bool handleVisible = true;
    keel::String yside;

    keel::String eJson;
    keel::String eNegJson;
    keel::String ePosJson;

    keel::String uJson;
    keel::String vJson;

    keel::Array<keel::u8> zQuantized;
    keel::f64 cminOrig = 0.0;
    keel::f64 cmaxOrig = 1.0;
    bool colorScaleBaked = false;

    bool downsampled = false;
    keel::usize originalRows = 0;
    keel::usize originalCols = 0;

    keel::Array<keel::f64> xRaw;
    keel::Array<keel::f64> yRaw;
    bool seriesDownsampled = false;
    keel::usize seriesN = 0;
    keel::f64 sxLo = 0.0;
    keel::f64 sxHi = 0.0;
    keel::f64 syLo = 0.0;
    keel::f64 syHi = 0.0;
    mutable keel::Array<DecimatedSeries> seriesPyramid;

    mutable keel::Array<keel::Array<keel::u8>> lodLevels;
    mutable keel::Array<keel::Pair<keel::usize, keel::usize>> lodDims;

    keel::String vertexColorsJson;

    keel::Array<keel::f64> animatedX;
    keel::Array<keel::f64> animatedY;
    bool isAnimated = false;

    keel::Array<keel::u8> rgbBytes;
    keel::String rgbJson;

    DatasetInfo clone(keel::Allocator *alloc = nullptr) const;
};

struct AxesState
{
    keel::Array<DatasetInfo> datasets;
    keel::String title;
    keel::String subtitle;
    keel::String xlabel;
    keel::String ylabel;
    keel::String xlimJson;
    keel::String ylimJson;
    keel::String rlimJson;
    keel::String thetalimJson;
    keel::String climJson;
    keel::String colormapName;
    keel::String customColormapJson;
    bool polar = false;
    bool holdOn = false;
    bool gridMajor = false;
    bool gridMinor = false;
    bool gridUserTouched = false;
    keel::Array<keel::String> legendLabels;
    keel::String legendLocation;
    bool legendBoxOn = true;
    bool legendHidden = false;
    keel::String colorbarLocation;

    keel::String xscale = keel::String::from("linear");
    keel::String yscale = keel::String::from("linear");
    keel::String colorScale = keel::String::from("linear");
    keel::String axisMode;
    keel::String xTicksJson;
    keel::String yTicksJson;
    keel::String zTicksJson;
    bool xTicksAuto = true;
    bool yTicksAuto = true;
    bool zTicksAuto = true;
    keel::String xTickLabelsJson;
    keel::String yTickLabelsJson;
    keel::String zTickLabelsJson;

    keel::f64 fontSize = 10.0;
    keel::String fontWeight = keel::String::from("normal");
    keel::String fontName = keel::String::from("Helvetica");
    keel::f64 lineWidth = 0.5;
    keel::String xTickFormat;
    keel::String yTickFormat;
    keel::String zTickFormat;
    keel::f64 xTickAngle = 0.0;
    keel::f64 yTickAngle = 0.0;
    bool axisVisible = true;
    bool boxOn = true;
    keel::i32 animatedDatasetIdx = -1;
    keel::String xDir = keel::String::from("normal");
    keel::String yDir = keel::String::from("normal");

    bool yyEnabled = false;
    keel::String activeYside = keel::String::from("left");
    keel::String ylabel2;
    keel::String ylim2Json;
    keel::String yscale2 = keel::String::from("linear");

    keel::String viewJson;
    keel::String zlabel;
    keel::String zlimJson;

    keel::String lightingMode;
    keel::String materialPreset;
    keel::String camlightPos;

    keel::String rotate3dMode;
    keel::String pan3dMode;
    keel::String zoom3dMode;

    keel::String thetaDir = keel::String::from("counterclockwise");
    keel::String thetaZeroLocation = keel::String::from("right");
    keel::String thetaticksJson;
    keel::String rticksJson;
    keel::String thetaticklabelsJson;
    keel::String rticklabelsJson;

    keel::i32 subplotIndex = 0;
    keel::i32 subplotRows = 0;
    keel::i32 subplotCols = 0;

    bool hasPosition = false;
    keel::f64 posLeft = 0.0;
    keel::f64 posBottom = 0.0;
    keel::f64 posWidth = 0.0;
    keel::f64 posHeight = 0.0;

    AxesState clone(keel::Allocator *alloc = nullptr) const;
};

struct FigureState
{
    keel::i32 id = 1;
    bool modified = false;
    keel::i32 subplotRows = 0;
    keel::i32 subplotCols = 0;
    keel::f64 figWidth = 0.0;
    keel::f64 figHeight = 0.0;
    keel::String bgColorHex;

    keel::Array<AxesState> axes;
    keel::i32 currentAxes = 0;
    keel::String linkMode;
    keel::String superTitle;

    struct Annotation {
        keel::String type;
        keel::f64 x1 = 0.0;
        keel::f64 y1 = 0.0;
        keel::f64 x2 = 0.0;
        keel::f64 y2 = 0.0;
        keel::String text;
        keel::String colorHex;
        keel::String faceHex;
        keel::f64 fontSize = 10.0;

        Annotation clone(keel::Allocator *alloc = nullptr) const
        {
            Annotation cp;
            cp.type = type.clone(alloc);
            cp.x1 = x1; cp.y1 = y1; cp.x2 = x2; cp.y2 = y2;
            cp.text = text.clone(alloc);
            cp.colorHex = colorHex.clone(alloc);
            cp.faceHex = faceHex.clone(alloc);
            cp.fontSize = fontSize;
            return cp;
        }
    };
    keel::Array<Annotation> annotations;

    AxesState &cur()
    {
        if (axes.empty())
            axes.push_back(AxesState{});
        return axes[keel::as<keel::usize>(currentAxes)];
    }

    FigureState clone(keel::Allocator *alloc = nullptr) const;
};

/// @}

} // namespace numkit
