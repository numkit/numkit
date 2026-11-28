// src/graphics/scene_render_bridge.cpp
//
// Projects a live GraphicsScene Figure subtree directly into strongly-typed
// ephemeral render-state DTOs (zero JSON serialization or KV string encoding).

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/graphics/figure_render_state.hpp>
#include <numkit/graphics/graphics_scene.hpp>

namespace numkit::gfx {

namespace {

bool extractRgb(const Value &v, RenderRgb &out) noexcept
{
    if (!v.isNumeric() || v.numel() < 3)
        return false;
    auto c255 = [](keel::f64 d) -> keel::u8 {
        if (!keel::is_finite(d) || d <= 0.0) return 0u;
        if (d >= 1.0) return 255u;
        return keel::as<keel::u8>(keel::round_i64(d * 255.0));
    };
    out = RenderRgb(c255(v.elemAsDouble(0)),
                    c255(v.elemAsDouble(1)),
                    c255(v.elemAsDouble(2)));
    return true;
}

keel::Array<keel::f64> extractVec1D(const Value &v)
{
    keel::Array<keel::f64> arr;
    if (!v.isNumeric() || v.isEmpty())
        return arr;
    arr.resize(v.numel());
    for (keel::usize i = 0; i < v.numel(); ++i)
        arr[i] = v.elemAsDouble(i);
    return arr;
}

void extractMat2DRowMajor(const Value &m, keel::Array<keel::f64> &out,
                          keel::usize &rows, keel::usize &cols)
{
    out.clear();
    rows = 0;
    cols = 0;
    if (!m.isNumeric() || m.isEmpty())
        return;
    rows = m.dims().rows();
    cols = m.dims().cols();
    if (rows == 0 || cols == 0)
        return;
    out.resize(rows * cols);
    for (keel::usize r = 0; r < rows; ++r) {
        for (keel::usize c = 0; c < cols; ++c)
            out[r * cols + c] = m.elemAsDouble(r + c * rows);
    }
}

RenderLineStyle parseLineStyleStr(keel::StringRef ls) noexcept
{
    if (ls == "--") return RenderLineStyle::Dashed;
    if (ls == ":")  return RenderLineStyle::Dotted;
    if (ls == "-.") return RenderLineStyle::DashDot;
    if (asciiEqualNoCase(ls, "none")) return RenderLineStyle::None;
    return RenderLineStyle::Solid;
}

keel::Array<keel::String> extractStringList(const Value &v)
{
    keel::Array<keel::String> out;
    if (v.isEmpty())
        return out;
    if (v.isCell()) {
        out.reserve(v.numel());
        for (keel::usize i = 0; i < v.numel(); ++i)
            out.push_back(v.cellAt(i).toString());
    } else if (v.isChar() || v.isString()) {
        out.push_back(v.toString());
    }
    return out;
}

void extractLimitPair(const GraphicsScene &scene, NodeId id, const char *prop,
                      keel::Allocator *mr, bool &hasOut, keel::f64 &loOut, keel::f64 &hiOut)
{
    if (!scene.hasProperty(id, prop))
        return;
    const Value lv = scene.getProperty(id, prop, mr);
    if (!lv.isNumeric() || lv.numel() < 2)
        return;
    const keel::f64 lo = lv.elemAsDouble(0);
    const keel::f64 hi = lv.elemAsDouble(1);
    if (keel::is_finite(lo) && keel::is_finite(hi) && hi > lo) {
        hasOut = true;
        loOut = lo;
        hiOut = hi;
    }
}

void projectDatasetStyle(const GraphicsScene &scene, NodeId cid,
                         RenderDataset &ds, keel::Allocator *mr)
{
    RenderRgb col;
    if (scene.hasProperty(cid, "Color") && extractRgb(scene.getProperty(cid, "Color", mr), col)) {
        ds.color = col;
        ds.hasColor = true;
    } else if (scene.hasProperty(cid, "FaceColor") && extractRgb(scene.getProperty(cid, "FaceColor", mr), col)) {
        ds.color = col;
        ds.hasColor = true;
    } else if (scene.hasProperty(cid, "CData")) {
        const Value cd = scene.getProperty(cid, "CData", mr);
        if (cd.isNumeric() && cd.numel() == 3 && extractRgb(cd, col)) {
            ds.color = col;
            ds.hasColor = true;
        } else if (scene.hasProperty(cid, "MarkerEdgeColor")
                   && extractRgb(scene.getProperty(cid, "MarkerEdgeColor", mr), col)) {
            ds.color = col;
            ds.hasColor = true;
        }
    }

    if (scene.hasProperty(cid, "LineStyle")) {
        const keel::String ls = scene.getProperty(cid, "LineStyle", mr).toString();
        if (!ls.empty())
            ds.lineStyle = parseLineStyleStr(ls);
    }

    if (scene.hasProperty(cid, "Marker")) {
        const keel::String mk = scene.getProperty(cid, "Marker", mr).toString();
        if (!mk.empty() && !asciiEqualNoCase(mk, "none"))
            ds.marker = mk.clone();
    }

    if (scene.hasProperty(cid, "MarkerFaceColor")) {
        const Value mfc = scene.getProperty(cid, "MarkerFaceColor", mr);
        if (extractRgb(mfc, col)) {
            ds.markerFaceColor = col;
            ds.hasMarkerFaceColor = true;
        } else if (mfc.isChar() || mfc.isString()) {
            if (asciiEqualNoCase(mfc.toString(), "auto"))
                ds.markerFaceAuto = true;
        }
    }

    if (scene.hasProperty(cid, "MarkerEdgeColor")) {
        const Value mec = scene.getProperty(cid, "MarkerEdgeColor", mr);
        if (extractRgb(mec, col)) {
            ds.markerEdgeColor = col;
            ds.hasMarkerEdgeColor = true;
        } else if (mec.isChar() || mec.isString()) {
            if (asciiEqualNoCase(mec.toString(), "none"))
                ds.markerEdgeNone = true;
        }
    }

    if (scene.hasProperty(cid, "FaceColor") && extractRgb(scene.getProperty(cid, "FaceColor", mr), col)) {
        ds.faceColor = col;
        ds.hasFaceColor = true;
    }
    if (scene.hasProperty(cid, "EdgeColor") && extractRgb(scene.getProperty(cid, "EdgeColor", mr), col)) {
        ds.edgeColor = col;
        ds.hasEdgeColor = true;
    }
    if (scene.hasProperty(cid, "FaceAlpha")) {
        const Value fa = scene.getProperty(cid, "FaceAlpha", mr);
        if (fa.isScalar())
            ds.faceAlpha = fa.toScalar();
    }
    if (scene.hasProperty(cid, "BinWidth")) {
        const Value bw = scene.getProperty(cid, "BinWidth", mr);
        if (bw.isScalar())
            ds.binWidth = bw.toScalar();
    }
}

void projectColormap(const GraphicsScene &scene, NodeId ownerId,
                     RenderAxes &dst, keel::Allocator *mr)
{
    if (!scene.hasProperty(ownerId, "Colormap"))
        return;
    const Value cm = scene.getProperty(ownerId, "Colormap", mr);
    if (cm.isNumeric() && cm.dims().cols() == 3 && cm.dims().rows() >= 2) {
        const keel::usize rows = cm.dims().rows();
        dst.customColormap.clear();
        dst.customColormap.reserve(rows);
        auto c255 = [](keel::f64 d) -> keel::u8 {
            if (!keel::is_finite(d) || d <= 0.0) return 0u;
            if (d >= 1.0) return 255u;
            return keel::as<keel::u8>(keel::round_i64(d * 255.0));
        };
        for (keel::usize r = 0; r < rows; ++r) {
            dst.customColormap.push_back(RenderRgb(
                c255(cm.elemAsDouble(r + 0 * rows)),
                c255(cm.elemAsDouble(r + 1 * rows)),
                c255(cm.elemAsDouble(r + 2 * rows))));
        }
    }
}

void projectOneAxes(const GraphicsScene &scene, NodeId axId, RenderAxes &dst, keel::Allocator *mr)
{
    const GraphicsNode *ax = scene.findNode(axId);
    if (!ax)
        return;

    dst.polar = (ax->classId == ClassId::PolarAxes);

    if (scene.hasProperty(axId, "Position")) {
        const Value pos = scene.getProperty(axId, "Position", mr);
        if (pos.isNumeric() && pos.numel() >= 4) {
            const keel::f64 l = pos.elemAsDouble(0);
            const keel::f64 b = pos.elemAsDouble(1);
            const keel::f64 w = pos.elemAsDouble(2);
            const keel::f64 h = pos.elemAsDouble(3);
            if (w > 0.0 && h > 0.0 && w <= 1.5 && h <= 1.5 && l >= -0.2 && b >= -0.2) {
                dst.hasPosition = true;
                dst.posLeft = l;
                dst.posBottom = b;
                dst.posWidth = w;
                dst.posHeight = h;
            }
        }
    }

    if (ax->titleId != kInvalidNodeId && scene.findNode(ax->titleId))
        dst.title = scene.getProperty(ax->titleId, "String", mr).toString();
    if (ax->subtitleId != kInvalidNodeId && scene.findNode(ax->subtitleId))
        dst.subtitle = scene.getProperty(ax->subtitleId, "String", mr).toString();

    dst.axisVisible = !asciiEqualNoCase(scene.getProperty(axId, "Visible", mr).toString(), "off");
    dst.fontSize = scene.getProperty(axId, "FontSize", mr).toScalar();
    dst.fontBold = asciiEqualNoCase(scene.getProperty(axId, "FontWeight", mr).toString(), "bold");
    dst.fontName = scene.getProperty(axId, "FontName", mr).toString();
    dst.lineWidth = scene.getProperty(axId, "LineWidth", mr).toScalar();

    if (dst.polar) {
        extractLimitPair(scene, axId, "RLim", mr, dst.hasRLim, dst.rLo, dst.rHi);
        if (scene.hasProperty(axId, "RTickMode")
            && asciiEqualNoCase(scene.getProperty(axId, "RTickMode", mr).toString(), "manual")) {
            dst.rTicks = extractVec1D(scene.getProperty(axId, "RTick", mr));
        }
        if (scene.hasProperty(axId, "ThetaTickMode")
            && asciiEqualNoCase(scene.getProperty(axId, "ThetaTickMode", mr).toString(), "manual")) {
            dst.thetaTicks = extractVec1D(scene.getProperty(axId, "ThetaTick", mr));
        }
        if (scene.hasProperty(axId, "ThetaTickLabelMode")
            && asciiEqualNoCase(scene.getProperty(axId, "ThetaTickLabelMode", mr).toString(), "manual")) {
            dst.thetaTickLabels = extractStringList(scene.getProperty(axId, "ThetaTickLabel", mr));
        }
        if (scene.hasProperty(axId, "ThetaDir"))
            dst.thetaClockwise = asciiEqualNoCase(scene.getProperty(axId, "ThetaDir", mr).toString(), "clockwise");
        if (scene.hasProperty(axId, "ThetaZeroLocation")) {
            dst.thetaZeroLocation = scene.getProperty(axId, "ThetaZeroLocation", mr).toString();
            dst.thetaZeroLocation.to_ascii_lower_in_place();
        }
    } else {
        if (ax->xLabelId != kInvalidNodeId && scene.findNode(ax->xLabelId))
            dst.xlabel = scene.getProperty(ax->xLabelId, "String", mr).toString();
        if (ax->yLabelId != kInvalidNodeId && scene.findNode(ax->yLabelId))
            dst.ylabel = scene.getProperty(ax->yLabelId, "String", mr).toString();
        if (ax->yLabelRightId != kInvalidNodeId && scene.findNode(ax->yLabelRightId)) {
            dst.ylabel2 = scene.getProperty(ax->yLabelRightId, "String", mr).toString();
            dst.yyEnabled = true;
        }
        if (ax->yAxisRightId != kInvalidNodeId && scene.findNode(ax->yAxisRightId)) {
            dst.yyEnabled = true;
            extractLimitPair(scene, ax->yAxisRightId, "Limits", mr, dst.hasY2Lim, dst.y2Lo, dst.y2Hi);
            if (scene.hasProperty(ax->yAxisRightId, "Scale"))
                dst.y2Log = asciiEqualNoCase(scene.getProperty(ax->yAxisRightId, "Scale", mr).toString(), "log");
        }
        if (ax->zLabelId != kInvalidNodeId && scene.findNode(ax->zLabelId))
            dst.zlabel = scene.getProperty(ax->zLabelId, "String", mr).toString();

        dst.xLog = asciiEqualNoCase(scene.getProperty(axId, "XScale", mr).toString(), "log");
        dst.yLog = asciiEqualNoCase(scene.getProperty(axId, "YScale", mr).toString(), "log");
        dst.xReverse = asciiEqualNoCase(scene.getProperty(axId, "XDir", mr).toString(), "reverse");
        dst.yReverse = asciiEqualNoCase(scene.getProperty(axId, "YDir", mr).toString(), "reverse");
        dst.boxOn = !asciiEqualNoCase(scene.getProperty(axId, "Box", mr).toString(), "off");
        dst.gridMajor = asciiEqualNoCase(scene.getProperty(axId, "XGrid", mr).toString(), "on")
                        || asciiEqualNoCase(scene.getProperty(axId, "YGrid", mr).toString(), "on");
        dst.gridMinor = asciiEqualNoCase(scene.getProperty(axId, "XMinorGrid", mr).toString(), "on")
                        || asciiEqualNoCase(scene.getProperty(axId, "YMinorGrid", mr).toString(), "on");

        if (asciiEqualNoCase(scene.getProperty(axId, "DataAspectRatioMode", mr).toString(), "manual")) {
            const Value dar = scene.getProperty(axId, "DataAspectRatio", mr);
            if (dar.isNumeric() && dar.numel() >= 2
                && keel::abs(dar.elemAsDouble(0) - dar.elemAsDouble(1)) < 1e-9) {
                dst.equalAspect = true;
            }
        }

        extractLimitPair(scene, axId, "XLim", mr, dst.hasXLim, dst.xLo, dst.xHi);
        extractLimitPair(scene, axId, "YLim", mr, dst.hasYLim, dst.yLo, dst.yHi);
        if (asciiEqualNoCase(scene.getProperty(axId, "ZLimMode", mr).toString(), "manual"))
            extractLimitPair(scene, axId, "ZLim", mr, dst.hasZLim, dst.zLo, dst.zHi);
        if (scene.hasProperty(axId, "CLimMode")
            && asciiEqualNoCase(scene.getProperty(axId, "CLimMode", mr).toString(), "manual")) {
            extractLimitPair(scene, axId, "CLim", mr, dst.hasCLim, dst.cLo, dst.cHi);
        }

        if (asciiEqualNoCase(scene.getProperty(axId, "XTickMode", mr).toString(), "manual"))
            dst.xTicks = extractVec1D(scene.getProperty(axId, "XTick", mr));
        if (asciiEqualNoCase(scene.getProperty(axId, "YTickMode", mr).toString(), "manual"))
            dst.yTicks = extractVec1D(scene.getProperty(axId, "YTick", mr));
        if (asciiEqualNoCase(scene.getProperty(axId, "XTickLabelMode", mr).toString(), "manual"))
            dst.xTickLabels = extractStringList(scene.getProperty(axId, "XTickLabel", mr));
        if (asciiEqualNoCase(scene.getProperty(axId, "YTickLabelMode", mr).toString(), "manual"))
            dst.yTickLabels = extractStringList(scene.getProperty(axId, "YTickLabel", mr));

        if (ax->xAxisId != kInvalidNodeId && scene.findNode(ax->xAxisId)
            && scene.hasProperty(ax->xAxisId, "TickLabelFormat")) {
            const keel::String fmt = scene.getProperty(ax->xAxisId, "TickLabelFormat", mr).toString();
            if (!fmt.empty() && fmt != "%g")
                dst.xTickFormat = fmt.clone();
        }
        if (ax->yAxisId != kInvalidNodeId && scene.findNode(ax->yAxisId)
            && scene.hasProperty(ax->yAxisId, "TickLabelFormat")) {
            const keel::String fmt = scene.getProperty(ax->yAxisId, "TickLabelFormat", mr).toString();
            if (!fmt.empty() && fmt != "%g")
                dst.yTickFormat = fmt.clone();
        }

        if (scene.hasProperty(axId, "View")) {
            const Value vv = scene.getProperty(axId, "View", mr);
            if (vv.isNumeric() && vv.numel() >= 2) {
                dst.viewAz = vv.elemAsDouble(0);
                dst.viewEl = vv.elemAsDouble(1);
            }
        }
        projectColormap(scene, axId, dst, mr);
    }

    // Project ColorBar (if attached to this axes)
    if (ax->colorBarId != kInvalidNodeId && scene.findNode(ax->colorBarId)) {
        const bool cbVis = !asciiEqualNoCase(scene.getProperty(ax->colorBarId, "Visible", mr).toString(), "off");
        if (cbVis) {
            dst.colorbarLocation = scene.getProperty(ax->colorBarId, "Location", mr).toString();
            dst.colorbarLocation.to_ascii_lower_in_place();
            if (dst.colorbarLocation.empty())
                dst.colorbarLocation = keel::String::from("eastoutside");
        }
    }

    // Walk children in creation order (oldest-first).
    for (keel::usize i = 0; i < ax->children.size(); ++i) {
        const NodeId cid = ax->children[i];
        const GraphicsNode *ch = scene.findNode(cid);
        if (!ch) continue;

        RenderDataset ds;
        ds.visible = !asciiEqualNoCase(scene.getProperty(cid, "Visible", mr).toString(), "off");
        if (scene.hasProperty(cid, "HandleVisibility"))
            ds.handleVisible = !asciiEqualNoCase(scene.getProperty(cid, "HandleVisibility", mr).toString(), "off");
        if (scene.hasProperty(cid, "DisplayName"))
            ds.label = scene.getProperty(cid, "DisplayName", mr).toString();
        if (scene.hasProperty(cid, "LineWidth"))
            ds.lineWidth = scene.getProperty(cid, "LineWidth", mr).toScalar();
        if (scene.hasProperty(cid, "MarkerSize"))
            ds.markerSize = scene.getProperty(cid, "MarkerSize", mr).toScalar();
        if (ch->rulerAxis == 'Y')
            ds.rightYAxis = true;
        projectDatasetStyle(scene, cid, ds, mr);

        switch (ch->classId) {
        case ClassId::Line:
        case ClassId::FunctionLine:
        case ClassId::ParameterizedFunctionLine: {
            ds.kind = RenderChartKind::Line;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            const Value zd = scene.hasProperty(cid, "ZData") ? scene.getProperty(cid, "ZData", mr) : Value();
            if (!zd.isEmpty()) {
                ds.kind = RenderChartKind::Plot3;
                ds.z = extractVec1D(zd);
            }
            break;
        }
        case ClassId::ConstantLine: {
            ds.kind = RenderChartKind::Line;
            const keel::f64 val = scene.hasProperty(cid, "Value")
                                      ? scene.getProperty(cid, "Value", mr).toScalar()
                                      : 0.0;
            const keel::String ia = scene.hasProperty(cid, "InterceptAxis")
                                        ? scene.getProperty(cid, "InterceptAxis", mr).toString()
                                        : keel::String::from("x");
            if (asciiEqualNoCase(ia, "y")) {
                ds.x.push_back(dst.hasXLim ? dst.xLo : 0.0);
                ds.x.push_back(dst.hasXLim ? dst.xHi : 1.0);
                ds.y.push_back(val);
                ds.y.push_back(val);
            } else {
                ds.x.push_back(val);
                ds.x.push_back(val);
                ds.y.push_back(dst.hasYLim ? dst.yLo : 0.0);
                ds.y.push_back(dst.hasYLim ? dst.yHi : 1.0);
            }
            break;
        }
        case ClassId::Scatter: {
            ds.kind = RenderChartKind::Scatter;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            if (scene.hasProperty(cid, "SizeData"))
                ds.sizes = extractVec1D(scene.getProperty(cid, "SizeData", mr));
            break;
        }
        case ClassId::Bar: {
            ds.kind = RenderChartKind::Bar;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::Surface: {
            ds.kind = RenderChartKind::Surface;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            extractMat2DRowMajor(scene.getProperty(cid, "ZData", mr), ds.z, ds.zRows, ds.zCols);
            break;
        }
        case ClassId::Patch: {
            ds.kind = RenderChartKind::Polygon;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::Quiver: {
            ds.kind = RenderChartKind::Quiver;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            ds.u = extractVec1D(scene.getProperty(cid, "UData", mr));
            ds.v = extractVec1D(scene.getProperty(cid, "VData", mr));
            break;
        }
        case ClassId::Contour: {
            ds.kind = RenderChartKind::Contour;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            extractMat2DRowMajor(scene.getProperty(cid, "ZData", mr), ds.z, ds.zRows, ds.zCols);
            if (scene.hasProperty(cid, "LevelList"))
                ds.levels = extractVec1D(scene.getProperty(cid, "LevelList", mr));
            break;
        }
        case ClassId::Stem: {
            ds.kind = RenderChartKind::Stem;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::ErrorBar: {
            ds.kind = RenderChartKind::ErrorBar;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            ds.eNeg = extractVec1D(scene.getProperty(cid, "YNegativeDelta", mr));
            ds.ePos = extractVec1D(scene.getProperty(cid, "YPositiveDelta", mr));
            break;
        }
        case ClassId::Histogram: {
            ds.kind = RenderChartKind::Histogram;
            const Value edges = scene.getProperty(cid, "BinEdges", mr);
            const Value vals = scene.getProperty(cid, "Values", mr);
            const keel::usize nb = vals.numel();
            ds.x.resize(nb);
            for (keel::usize k = 0; k < nb && k + 1 < edges.numel(); ++k)
                ds.x[k] = 0.5 * (edges.elemAsDouble(k) + edges.elemAsDouble(k + 1));
            ds.y = extractVec1D(vals);
            if (!(ds.binWidth > 0.0) && edges.numel() >= 2)
                ds.binWidth = keel::abs(edges.elemAsDouble(1) - edges.elemAsDouble(0));
            break;
        }
        case ClassId::Area: {
            ds.kind = RenderChartKind::Area;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::Image: {
            const Value cd = scene.getProperty(cid, "CData", mr);
            const keel::usize rows = cd.dims().rows();
            const keel::usize cols = cd.dims().cols();
            ds.imageRows = rows;
            ds.imageCols = cols;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            if (cd.dims().ndim() >= 3 && cd.dims().dim(2) == 3 && rows > 0 && cols > 0) {
                ds.kind = RenderChartKind::ImageRgb;
                const keel::usize plane = rows * cols;
                const bool isU8 = (cd.type() == ValueType::UINT8);
                ds.imagePixels.resize(plane * 4u, 255u);
                for (keel::usize r = 0; r < rows; ++r) {
                    for (keel::usize c = 0; c < cols; ++c) {
                        const keel::usize srcBase = r + c * rows;
                        const keel::usize dstBase = (r * cols + c) * 4u;
                        for (keel::usize chIdx = 0; chIdx < 3; ++chIdx) {
                            keel::f64 v = cd.elemAsDouble(srcBase + chIdx * plane);
                            if (!isU8) v *= 255.0;
                            if (!keel::is_finite(v) || v < 0.0) v = 0.0;
                            if (v > 255.0) v = 255.0;
                            ds.imagePixels[dstBase + chIdx] = keel::as<keel::u8>(keel::round_i64(v));
                        }
                        ds.imagePixels[dstBase + 3u] = 255u;
                    }
                }
            } else {
                ds.kind = RenderChartKind::ImageIndexed;
                const keel::usize n = rows * cols;
                keel::f64 lo = keel::Limits<keel::f64>::infinity();
                keel::f64 hi = -keel::Limits<keel::f64>::infinity();
                for (keel::usize k = 0; k < n; ++k) {
                    const keel::f64 v = cd.elemAsDouble(k);
                    if (keel::is_finite(v)) {
                        if (v < lo) lo = v;
                        if (v > hi) hi = v;
                    }
                }
                if (!keel::is_finite(lo) || !keel::is_finite(hi)) {
                    lo = 0.0;
                    hi = 1.0;
                }
                ds.cmin = lo;
                ds.cmax = hi;
                const keel::f64 span = (hi > lo) ? (hi - lo) : 1.0;
                ds.imagePixels.resize(n, 255u);
                for (keel::usize k = 0; k < n; ++k) {
                    const keel::f64 v = cd.elemAsDouble(k);
                    if (!keel::is_finite(v)) {
                        ds.imagePixels[k] = 255u;
                    } else {
                        keel::f64 t = (v - lo) / span;
                        if (t < 0.0) t = 0.0;
                        if (t > 1.0) t = 1.0;
                        ds.imagePixels[k] = keel::as<keel::u8>(keel::round_i64(t * 254.0));
                    }
                }
            }
            break;
        }
        case ClassId::Stair: {
            ds.kind = RenderChartKind::Stairs;
            ds.x = extractVec1D(scene.getProperty(cid, "XData", mr));
            ds.y = extractVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::AnimatedLine: {
            ds.kind = RenderChartKind::Line;
            ds.x = ch->animX.clone();
            ds.y = ch->animY.clone();
            break;
        }
        case ClassId::Text: {
            ds.kind = RenderChartKind::Text;
            const Value pos = scene.getProperty(cid, "Position", mr);
            if (pos.numel() >= 2) {
                ds.x.push_back(pos.elemAsDouble(0));
                ds.y.push_back(pos.elemAsDouble(1));
            }
            ds.label = scene.getProperty(cid, "String", mr).toString();
            if (scene.hasProperty(cid, "FontSize"))
                ds.fontSize = scene.getProperty(cid, "FontSize", mr).toScalar();
            if (scene.hasProperty(cid, "HorizontalAlignment")) {
                const keel::String ha = scene.getProperty(cid, "HorizontalAlignment", mr).toString();
                if (asciiEqualNoCase(ha, "center")) ds.halign = RenderHAlign::Center;
                else if (asciiEqualNoCase(ha, "right")) ds.halign = RenderHAlign::Right;
                else ds.halign = RenderHAlign::Left;
            }
            if (!ds.hasColor)
                ds.color = RenderRgb(0, 0, 0);
            break;
        }
        default:
            continue;
        }
        dst.datasets.push_back(keel::move(ds));
    }

    // Project Legend (if attached to this axes).
    if (ax->legendId != kInvalidNodeId && scene.findNode(ax->legendId)) {
        const NodeId lgId = ax->legendId;
        dst.legendVisible = !asciiEqualNoCase(scene.getProperty(lgId, "Visible", mr).toString(), "off");
        dst.legendBoxOn = !asciiEqualNoCase(scene.getProperty(lgId, "Box", mr).toString(), "off");
        dst.legendLocation = scene.getProperty(lgId, "Location", mr).toString();
        dst.legendLocation.to_ascii_lower_in_place();

        keel::Array<keel::String> explicitLabels = extractStringList(scene.getProperty(lgId, "String", mr));
        for (keel::usize k = 0; k < explicitLabels.size(); ++k) {
            if (!explicitLabels[k].empty())
                dst.legendLabels.push_back(keel::move(explicitLabels[k]));
        }
        // Auto-collect DisplayName from visible, handle-visible chart children.
        if (dst.legendLabels.empty()) {
            for (keel::usize d = 0; d < dst.datasets.size(); ++d) {
                const RenderDataset &ds = dst.datasets[d];
                if (!ds.visible || !ds.handleVisible || ds.kind == RenderChartKind::Text
                    || ds.kind == RenderChartKind::ImageIndexed || ds.kind == RenderChartKind::ImageRgb)
                    continue;
                if (!ds.label.empty())
                    dst.legendLabels.push_back(ds.label.clone());
            }
        }
    }
}

void projectFigureAnnotation(const GraphicsScene &scene, NodeId cid,
                             FigureState &out, keel::Allocator *mr)
{
    const GraphicsNode *ch = scene.findNode(cid);
    if (!ch || ch->classId != ClassId::TextBox)
        return;
    if (asciiEqualNoCase(scene.getProperty(cid, "Visible", mr).toString(), "off"))
        return;
    const Value pos = scene.getProperty(cid, "Position", mr);
    if (!pos.isNumeric() || pos.numel() < 4)
        return;
    RenderAnnotation an;
    an.type = keel::String::from("textbox");
    an.x1 = pos.elemAsDouble(0);
    an.y1 = pos.elemAsDouble(1);
    an.x2 = an.x1 + pos.elemAsDouble(2);
    an.y2 = an.y1 + pos.elemAsDouble(3);
    if (scene.hasProperty(cid, "String"))
        an.text = scene.getProperty(cid, "String", mr).toString();
    if (scene.hasProperty(cid, "FontSize"))
        an.fontSize = scene.getProperty(cid, "FontSize", mr).toScalar();
    if (scene.hasProperty(cid, "EdgeColor"))
        extractRgb(scene.getProperty(cid, "EdgeColor", mr), an.color);
    if (scene.hasProperty(cid, "BackgroundColor"))
        an.hasFaceColor = extractRgb(scene.getProperty(cid, "BackgroundColor", mr), an.faceColor);
    out.annotations.push_back(keel::move(an));
}

} // anonymous namespace

bool projectSceneFigureToRenderState(const GraphicsScene &scene, NodeId figId,
                                     FigureState &out, keel::Allocator *mr)
{
    const GraphicsNode *fn = scene.findNode(figId);
    if (!fn || fn->classId != ClassId::Figure)
        return false;

    auto *res = mr ? mr : scene.allocator();
    out = FigureState{};
    out.id = fn->figureNumber > 0 ? fn->figureNumber : 1;

    if (scene.hasProperty(figId, "Position")) {
        const Value fpos = scene.getProperty(figId, "Position", res);
        if (fpos.isNumeric() && fpos.numel() >= 4) {
            const keel::f64 fw = fpos.elemAsDouble(2);
            const keel::f64 fh = fpos.elemAsDouble(3);
            if (fw > 0.0 && fh > 0.0) {
                out.figWidth = fw;
                out.figHeight = fh;
            }
        }
    }
    if (scene.hasProperty(figId, "Color"))
        extractRgb(scene.getProperty(figId, "Color", res), out.bgColor);
    if (fn->titleId != kInvalidNodeId && scene.findNode(fn->titleId))
        out.superTitle = scene.getProperty(fn->titleId, "String", res).toString();

    keel::Array<NodeId> axList(res);
    for (keel::usize i = 0; i < fn->children.size(); ++i) {
        const NodeId cid = fn->children[i];
        const GraphicsNode *ch = scene.findNode(cid);
        if (!ch) continue;
        if (ch->classId == ClassId::Axes || ch->classId == ClassId::PolarAxes) {
            axList.push_back(cid);
        } else if (ch->classId == ClassId::TiledChartLayout) {
            if (out.superTitle.empty() && ch->titleId != kInvalidNodeId && scene.findNode(ch->titleId))
                out.superTitle = scene.getProperty(ch->titleId, "String", res).toString();
            for (keel::usize j = 0; j < ch->children.size(); ++j) {
                const NodeId tcid = ch->children[j];
                if (const GraphicsNode *tc = scene.findNode(tcid);
                    tc && (tc->classId == ClassId::Axes || tc->classId == ClassId::PolarAxes)) {
                    axList.push_back(tcid);
                }
            }
        } else if (ch->classId == ClassId::TextBox) {
            projectFigureAnnotation(scene, cid, out, res);
        }
    }

    if (axList.empty()) {
        out.axes.push_back(RenderAxes{});
        return true;
    }

    for (keel::usize k = 0; k < axList.size(); ++k) {
        RenderAxes st;
        projectOneAxes(scene, axList[k], st, res);
        if (st.customColormap.empty())
            projectColormap(scene, figId, st, res);
        out.axes.push_back(keel::move(st));
    }
    return true;
}

} // namespace numkit::gfx
