// src/graphics/figure_render_state.cpp
//
// Out-of-line helpers and clone methods for the ephemeral render-state DTOs
// used by scene_render_bridge and figure_render (SVG/PDF/EPS/RGB export).

#include <keel/math.h>
#include <keel/number.h>
#include <numkit/graphics/figure_render_state.hpp>

namespace numkit {

namespace figdetail {

keel::usize countFlatElements(keel::StringRef j)
{
    keel::usize commas = 0;
    bool any = false;
    for (char c : j) {
        if (c == ',') commas++;
        else if (c != '[' && c != ']' && c != ' ' && c != '\t' && c != '\n') any = true;
    }
    return any ? commas + 1 : 0;
}

keel::Array<keel::f64> parseFlatDoubles(const keel::String &j)
{
    keel::Array<keel::f64> out;
    const char *p = j.c_str();
    const char *end = p + j.size();
    while (p < end && *p != '[') ++p;
    if (p < end) ++p;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '"' || *p == '\t' || *p == '\n')) ++p;
        if (p >= end || *p == ']') break;
        if ((p[0] == 'n' || p[0] == 'N') && (p + 4 <= end) &&
            (p[1] == 'u') && (p[2] == 'l') && (p[3] == 'l')) {
            out.push_back(keel::kNaN);
            p += 4;
            continue;
        }
        const char *tokStart = p;
        while (p < end && *p != ' ' && *p != ',' && *p != ']' && *p != '"' && *p != '\t' && *p != '\n') ++p;
        auto parsed = keel::parse_f64(keel::StringRef(tokStart, keel::as<keel::usize>(p - tokStart)));
        if (!parsed.is_ok()) continue;
        out.push_back(parsed.value());
    }
    return out;
}

keel::String serializeFlatDoubles(keel::ArrayRef<keel::f64> v)
{
    keel::String out;
    out.push_back('[');
    char buf[64];
    for (keel::usize i = 0; i < v.size(); ++i) {
        if (i) out.push_back(',');
        const keel::f64 x = v[i];
        if (keel::is_nan(x)) out.append("null");
        else if (keel::is_inf(x)) out.append(x > 0 ? "\"Inf\"" : "\"-Inf\"");
        else {
            const keel::usize n = keel::format_f64(buf, sizeof(buf), x);
            if (n > 0) out.append(keel::StringRef(buf, n));
        }
    }
    out.push_back(']');
    return out;
}

keel::String serializeFlatDoubles(const keel::Array<keel::f64> &v)
{
    return serializeFlatDoubles(v.as_ref());
}

void appendDouble(keel::String &out, keel::f64 v)
{
    char buf[64];
    const keel::usize n = keel::format_f64_general(buf, sizeof(buf), v, 6);
    if (n > 0) out.append(keel::StringRef(buf, n));
}

void appendInt(keel::String &out, keel::i32 v)
{
    char buf[32];
    const keel::usize n = keel::format_i64(buf, sizeof(buf), v);
    if (n > 0) out.append(keel::StringRef(buf, n));
}

void appendUsize(keel::String &out, keel::usize v)
{
    char buf[32];
    const keel::usize n = keel::format_u64(buf, sizeof(buf), keel::as<keel::u64>(v));
    if (n > 0) out.append(keel::StringRef(buf, n));
}

} // namespace figdetail

DatasetInfo DatasetInfo::clone(keel::Allocator *alloc) const
{
    DatasetInfo cp;
    cp.xJson = xJson.clone(alloc);
    cp.yJson = yJson.clone(alloc);
    cp.zJson = zJson.clone(alloc);
    cp.sizeJson = sizeJson.clone(alloc);
    cp.colorJson = colorJson.clone(alloc);
    cp.type = type.clone(alloc);
    cp.label = label.clone(alloc);
    cp.style = style.clone(alloc);
    cp.lineWidth = lineWidth;
    cp.markerSize = markerSize;
    cp.visible = visible;
    cp.yside = yside.clone(alloc);
    cp.eJson = eJson.clone(alloc);
    cp.eNegJson = eNegJson.clone(alloc);
    cp.ePosJson = ePosJson.clone(alloc);
    cp.uJson = uJson.clone(alloc);
    cp.vJson = vJson.clone(alloc);
    cp.zQuantized = zQuantized.clone(alloc);
    cp.cminOrig = cminOrig;
    cp.cmaxOrig = cmaxOrig;
    cp.colorScaleBaked = colorScaleBaked;
    cp.downsampled = downsampled;
    cp.originalRows = originalRows;
    cp.originalCols = originalCols;
    cp.xRaw = xRaw.clone(alloc);
    cp.yRaw = yRaw.clone(alloc);
    cp.seriesDownsampled = seriesDownsampled;
    cp.seriesN = seriesN;
    cp.sxLo = sxLo; cp.sxHi = sxHi; cp.syLo = syLo; cp.syHi = syHi;
    cp.seriesPyramid = seriesPyramid.clone(alloc);
    cp.lodLevels = lodLevels.clone(alloc);
    cp.lodDims = lodDims.clone(alloc);
    cp.vertexColorsJson = vertexColorsJson.clone(alloc);
    cp.animatedX = animatedX.clone(alloc);
    cp.animatedY = animatedY.clone(alloc);
    cp.isAnimated = isAnimated;
    cp.rgbBytes = rgbBytes.clone(alloc);
    cp.rgbJson = rgbJson.clone(alloc);
    return cp;
}

AxesState AxesState::clone(keel::Allocator *alloc) const
{
    AxesState cp;
    cp.datasets = datasets.clone(alloc);
    cp.title = title.clone(alloc);
    cp.subtitle = subtitle.clone(alloc);
    cp.xlabel = xlabel.clone(alloc);
    cp.ylabel = ylabel.clone(alloc);
    cp.xlimJson = xlimJson.clone(alloc);
    cp.ylimJson = ylimJson.clone(alloc);
    cp.rlimJson = rlimJson.clone(alloc);
    cp.thetalimJson = thetalimJson.clone(alloc);
    cp.climJson = climJson.clone(alloc);
    cp.colormapName = colormapName.clone(alloc);
    cp.customColormapJson = customColormapJson.clone(alloc);
    cp.polar = polar;
    cp.holdOn = holdOn;
    cp.gridMajor = gridMajor;
    cp.gridMinor = gridMinor;
    cp.gridUserTouched = gridUserTouched;
    cp.legendLabels = legendLabels.clone(alloc);
    cp.legendLocation = legendLocation.clone(alloc);
    cp.legendBoxOn = legendBoxOn;
    cp.legendHidden = legendHidden;
    cp.colorbarLocation = colorbarLocation.clone(alloc);
    cp.xscale = xscale.clone(alloc);
    cp.yscale = yscale.clone(alloc);
    cp.colorScale = colorScale.clone(alloc);
    cp.axisMode = axisMode.clone(alloc);
    cp.xTicksJson = xTicksJson.clone(alloc);
    cp.yTicksJson = yTicksJson.clone(alloc);
    cp.zTicksJson = zTicksJson.clone(alloc);
    cp.xTicksAuto = xTicksAuto;
    cp.yTicksAuto = yTicksAuto;
    cp.zTicksAuto = zTicksAuto;
    cp.xTickLabelsJson = xTickLabelsJson.clone(alloc);
    cp.yTickLabelsJson = yTickLabelsJson.clone(alloc);
    cp.zTickLabelsJson = zTickLabelsJson.clone(alloc);
    cp.fontSize = fontSize;
    cp.fontWeight = fontWeight.clone(alloc);
    cp.fontName = fontName.clone(alloc);
    cp.lineWidth = lineWidth;
    cp.xTickFormat = xTickFormat.clone(alloc);
    cp.yTickFormat = yTickFormat.clone(alloc);
    cp.zTickFormat = zTickFormat.clone(alloc);
    cp.xTickAngle = xTickAngle;
    cp.yTickAngle = yTickAngle;
    cp.axisVisible = axisVisible;
    cp.boxOn = boxOn;
    cp.animatedDatasetIdx = animatedDatasetIdx;
    cp.xDir = xDir.clone(alloc);
    cp.yDir = yDir.clone(alloc);
    cp.yyEnabled = yyEnabled;
    cp.activeYside = activeYside.clone(alloc);
    cp.ylabel2 = ylabel2.clone(alloc);
    cp.ylim2Json = ylim2Json.clone(alloc);
    cp.yscale2 = yscale2.clone(alloc);
    cp.viewJson = viewJson.clone(alloc);
    cp.zlabel = zlabel.clone(alloc);
    cp.zlimJson = zlimJson.clone(alloc);
    cp.lightingMode = lightingMode.clone(alloc);
    cp.materialPreset = materialPreset.clone(alloc);
    cp.camlightPos = camlightPos.clone(alloc);
    cp.rotate3dMode = rotate3dMode.clone(alloc);
    cp.pan3dMode = pan3dMode.clone(alloc);
    cp.zoom3dMode = zoom3dMode.clone(alloc);
    cp.thetaDir = thetaDir.clone(alloc);
    cp.thetaZeroLocation = thetaZeroLocation.clone(alloc);
    cp.thetaticksJson = thetaticksJson.clone(alloc);
    cp.rticksJson = rticksJson.clone(alloc);
    cp.thetaticklabelsJson = thetaticklabelsJson.clone(alloc);
    cp.rticklabelsJson = rticklabelsJson.clone(alloc);
    cp.subplotIndex = subplotIndex;
    cp.subplotRows = subplotRows;
    cp.subplotCols = subplotCols;
    return cp;
}

FigureState FigureState::clone(keel::Allocator *alloc) const
{
    FigureState cp;
    cp.id = id;
    cp.modified = modified;
    cp.subplotRows = subplotRows;
    cp.subplotCols = subplotCols;
    cp.axes = axes.clone(alloc);
    cp.currentAxes = currentAxes;
    cp.linkMode = linkMode.clone(alloc);
    cp.superTitle = superTitle.clone(alloc);
    cp.annotations = annotations.clone(alloc);
    return cp;
}

} // namespace numkit
