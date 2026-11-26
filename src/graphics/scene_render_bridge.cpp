// src/graphics/scene_render_bridge.cpp
//
// Projects a live GraphicsScene Figure subtree into an ephemeral FigureState
// consumed by figrender (SVG / PDF / EPS / RGB export). This keeps the
// Model-First GraphicsScene as the single source of truth with zero live
// mirroring into legacy FigureManager.

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/graphics/figure_manager.hpp>
#include <numkit/graphics/graphics_scene.hpp>

namespace numkit::gfx {

namespace {

keel::String serializeVec1D(const Value &v)
{
    if (!v.isNumeric() || v.isEmpty())
        return keel::String::from("[]");
    keel::Array<keel::f64> arr;
    arr.resize(v.numel());
    for (keel::usize i = 0; i < v.numel(); ++i)
        arr[i] = v.elemAsDouble(i);
    return figdetail::serializeFlatDoubles(arr);
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

keel::String serializeMat2D(const Value &m)
{
    if (!m.isNumeric() || m.isEmpty())
        return keel::String::from("[]");
    const keel::usize R = m.dims().rows();
    const keel::usize C = m.dims().cols();
    keel::String out = keel::String::from("[");
    char buf[40];
    for (keel::usize r = 0; r < R; ++r) {
        if (r) out.push_back(',');
        out.push_back('[');
        for (keel::usize c = 0; c < C; ++c) {
            if (c) out.push_back(',');
            const keel::f64 val = m.elemAsDouble(r + c * R);
            if (keel::is_finite(val)) out.append(keel::format_f64_general_to(buf, val, 12));
            else out.append("null");
        }
        out.push_back(']');
    }
    out.push_back(']');
    return out;
}

keel::String rgbToHexStr(const Value &v)
{
    if (!v.isNumeric() || v.numel() < 3)
        return keel::String{};
    auto c255 = [](keel::f64 d) -> keel::i32 {
        keel::i32 i = keel::as<keel::i32>(d * 255.0 + 0.5);
        if (i < 0) i = 0;
        if (i > 255) i = 255;
        return i;
    };
    return keel::format("#{:02x}{:02x}{:02x}",
                        c255(v.elemAsDouble(0)),
                        c255(v.elemAsDouble(1)),
                        c255(v.elemAsDouble(2)));
}

keel::String buildStyleKV(const GraphicsScene &scene, NodeId cid, keel::Allocator *mr)
{
    keel::String out;
    auto addKV = [&](const char *k, keel::StringRef val) {
        if (val.empty() || val == "none") return;
        if (!out.empty()) out.push_back(';');
        out.append(k);
        out.push_back('=');
        out.append(val);
    };
    if (scene.hasProperty(cid, "Color")) {
        const Value cv = scene.getProperty(cid, "Color", mr);
        if (cv.isNumeric() && cv.numel() >= 3) addKV("color", rgbToHexStr(cv));
    } else if (scene.hasProperty(cid, "FaceColor")) {
        const Value fc = scene.getProperty(cid, "FaceColor", mr);
        if (fc.isNumeric() && fc.numel() >= 3) addKV("color", rgbToHexStr(fc));
    }
    if (scene.hasProperty(cid, "LineStyle")) {
        const keel::String ls = scene.getProperty(cid, "LineStyle", mr).toString();
        if (!ls.empty()) addKV("lineStyle", ls);
    }
    if (scene.hasProperty(cid, "Marker")) {
        const keel::String mk = scene.getProperty(cid, "Marker", mr).toString();
        if (!mk.empty() && mk != "none") addKV("marker", mk);
    }
    return out;
}

void projectOneAxes(const GraphicsScene &scene, NodeId axId, AxesState &dst, keel::Allocator *mr)
{
    const GraphicsNode *ax = scene.findNode(axId);
    if (!ax)
        return;

    dst.polar = (ax->classId == ClassId::PolarAxes);
    dst.subplotIndex = ax->subplotIdx > 0 ? ax->subplotIdx : 1;

    if (ax->titleId != kInvalidNodeId && scene.findNode(ax->titleId))
        dst.title = scene.getProperty(ax->titleId, "String", mr).toString();
    if (ax->subtitleId != kInvalidNodeId && scene.findNode(ax->subtitleId))
        dst.subtitle = scene.getProperty(ax->subtitleId, "String", mr).toString();

    if (!dst.polar) {
        if (ax->xLabelId != kInvalidNodeId && scene.findNode(ax->xLabelId))
            dst.xlabel = scene.getProperty(ax->xLabelId, "String", mr).toString();
        if (ax->yLabelId != kInvalidNodeId && scene.findNode(ax->yLabelId))
            dst.ylabel = scene.getProperty(ax->yLabelId, "String", mr).toString();
        if (ax->yLabelRightId != kInvalidNodeId && scene.findNode(ax->yLabelRightId)) {
            dst.ylabel2 = scene.getProperty(ax->yLabelRightId, "String", mr).toString();
            dst.yyEnabled = true;
            dst.activeYside = keel::String::from(ax->yyRightActive ? "right" : "left");
        }
        if (ax->zLabelId != kInvalidNodeId && scene.findNode(ax->zLabelId))
            dst.zlabel = scene.getProperty(ax->zLabelId, "String", mr).toString();

        dst.xscale = scene.getProperty(axId, "XScale", mr).toString();
        dst.yscale = scene.getProperty(axId, "YScale", mr).toString();
        dst.gridMajor = asciiEqualNoCase(scene.getProperty(axId, "XGrid", mr).toString(), "on")
                        || asciiEqualNoCase(scene.getProperty(axId, "YGrid", mr).toString(), "on");
        dst.gridMinor = asciiEqualNoCase(scene.getProperty(axId, "XMinorGrid", mr).toString(), "on");
        dst.fontSize = scene.getProperty(axId, "FontSize", mr).toScalar();
        dst.fontWeight = scene.getProperty(axId, "FontWeight", mr).toString();
        dst.fontName = scene.getProperty(axId, "FontName", mr).toString();
        dst.lineWidth = scene.getProperty(axId, "LineWidth", mr).toScalar();

        if (asciiEqualNoCase(scene.getProperty(axId, "XLimMode", mr).toString(), "manual"))
            dst.xlimJson = serializeVec1D(scene.getProperty(axId, "XLim", mr));
        if (asciiEqualNoCase(scene.getProperty(axId, "YLimMode", mr).toString(), "manual"))
            dst.ylimJson = serializeVec1D(scene.getProperty(axId, "YLim", mr));
        if (asciiEqualNoCase(scene.getProperty(axId, "ZLimMode", mr).toString(), "manual"))
            dst.zlimJson = serializeVec1D(scene.getProperty(axId, "ZLim", mr));
    }

    // Walk children in creation order (reverse of newest-first ax->children).
    for (keel::usize i = ax->children.size(); i > 0; --i) {
        const NodeId cid = ax->children[i - 1];
        const GraphicsNode *ch = scene.findNode(cid);
        if (!ch) continue;

        DatasetInfo ds;
        ds.visible = !asciiEqualNoCase(scene.getProperty(cid, "Visible", mr).toString(), "off");
        if (scene.hasProperty(cid, "DisplayName"))
            ds.label = scene.getProperty(cid, "DisplayName", mr).toString();
        if (scene.hasProperty(cid, "LineWidth"))
            ds.lineWidth = scene.getProperty(cid, "LineWidth", mr).toScalar();
        if (scene.hasProperty(cid, "MarkerSize"))
            ds.markerSize = scene.getProperty(cid, "MarkerSize", mr).toScalar();
        if (ch->rulerAxis == 'Y')
            ds.yside = keel::String::from("right");
        ds.style = buildStyleKV(scene, cid, mr);

        switch (ch->classId) {
        case ClassId::Line: {
            ds.type = keel::String::from("line");
            const Value xd = scene.getProperty(cid, "XData", mr);
            const Value yd = scene.getProperty(cid, "YData", mr);
            const Value zd = scene.getProperty(cid, "ZData", mr);
            ds.xRaw = extractVec1D(xd);
            ds.yRaw = extractVec1D(yd);
            ds.xJson = serializeVec1D(xd);
            ds.yJson = serializeVec1D(yd);
            if (!zd.isEmpty()) {
                ds.type = keel::String::from("plot3");
                ds.zJson = serializeVec1D(zd);
            }
            break;
        }
        case ClassId::Scatter: {
            ds.type = keel::String::from("scatter");
            const Value xd = scene.getProperty(cid, "XData", mr);
            const Value yd = scene.getProperty(cid, "YData", mr);
            ds.xRaw = extractVec1D(xd);
            ds.yRaw = extractVec1D(yd);
            ds.xJson = serializeVec1D(xd);
            ds.yJson = serializeVec1D(yd);
            break;
        }
        case ClassId::Bar: {
            ds.type = keel::String::from("bar");
            const Value xd = scene.getProperty(cid, "XData", mr);
            const Value yd = scene.getProperty(cid, "YData", mr);
            ds.xRaw = extractVec1D(xd);
            ds.yRaw = extractVec1D(yd);
            ds.xJson = serializeVec1D(xd);
            ds.yJson = serializeVec1D(yd);
            break;
        }
        case ClassId::Surface: {
            ds.type = keel::String::from("surf");
            ds.xJson = serializeVec1D(scene.getProperty(cid, "XData", mr));
            ds.yJson = serializeVec1D(scene.getProperty(cid, "YData", mr));
            ds.zJson = serializeMat2D(scene.getProperty(cid, "ZData", mr));
            break;
        }
        case ClassId::Patch: {
            ds.type = keel::String::from("polygon");
            ds.xJson = serializeVec1D(scene.getProperty(cid, "XData", mr));
            ds.yJson = serializeVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::Quiver: {
            ds.type = keel::String::from("quiver");
            const Value xd = scene.getProperty(cid, "XData", mr);
            const Value yd = scene.getProperty(cid, "YData", mr);
            const Value ud = scene.getProperty(cid, "UData", mr);
            const Value vd = scene.getProperty(cid, "VData", mr);
            ds.xRaw = extractVec1D(xd);
            ds.yRaw = extractVec1D(yd);
            ds.xJson = serializeVec1D(xd);
            ds.yJson = serializeVec1D(yd);
            ds.uJson = serializeVec1D(ud);
            ds.vJson = serializeVec1D(vd);
            break;
        }
        case ClassId::Contour: {
            ds.type = keel::String::from("contour");
            ds.xJson = serializeVec1D(scene.getProperty(cid, "XData", mr));
            ds.yJson = serializeVec1D(scene.getProperty(cid, "YData", mr));
            ds.zJson = serializeMat2D(scene.getProperty(cid, "ZData", mr));
            break;
        }
        case ClassId::Stem: {
            ds.type = keel::String::from("stem");
            ds.xJson = serializeVec1D(scene.getProperty(cid, "XData", mr));
            ds.yJson = serializeVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::ErrorBar: {
            ds.type = keel::String::from("errorbar");
            ds.xJson = serializeVec1D(scene.getProperty(cid, "XData", mr));
            ds.yJson = serializeVec1D(scene.getProperty(cid, "YData", mr));
            ds.eNegJson = serializeVec1D(scene.getProperty(cid, "YNegativeDelta", mr));
            ds.ePosJson = serializeVec1D(scene.getProperty(cid, "YPositiveDelta", mr));
            break;
        }
        case ClassId::Histogram: {
            ds.type = keel::String::from("histogram");
            const Value edges = scene.getProperty(cid, "BinEdges", mr);
            const Value vals = scene.getProperty(cid, "Values", mr);
            const keel::usize nb = vals.numel();
            keel::Array<keel::f64> centers;
            centers.resize(nb);
            for (keel::usize k = 0; k < nb && k + 1 < edges.numel(); ++k)
                centers[k] = 0.5 * (edges.elemAsDouble(k) + edges.elemAsDouble(k + 1));
            ds.xRaw = extractVec1D(edges);
            ds.yRaw = extractVec1D(vals);
            ds.xJson = figdetail::serializeFlatDoubles(centers);
            ds.yJson = serializeVec1D(vals);
            ds.zJson = serializeVec1D(edges);
            break;
        }
        case ClassId::Area: {
            ds.type = keel::String::from("area");
            ds.xJson = serializeVec1D(scene.getProperty(cid, "XData", mr));
            ds.yJson = serializeVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::Image: {
            const Value cd = scene.getProperty(cid, "CData", mr);
            const keel::usize rows = cd.dims().rows();
            const keel::usize cols = cd.dims().cols();
            ds.originalRows = rows;
            ds.originalCols = cols;
            ds.xJson = serializeVec1D(scene.getProperty(cid, "XData", mr));
            ds.yJson = serializeVec1D(scene.getProperty(cid, "YData", mr));
            if (cd.dims().ndim() >= 3 && cd.dims().dim(2) == 3 && rows > 0 && cols > 0) {
                ds.type = keel::String::from("image-rgb");
                const keel::usize plane = rows * cols;
                const bool isU8 = (cd.type() == ValueType::UINT8);
                ds.rgbBytes.resize(plane * 4u, 255u);
                for (keel::usize r = 0; r < rows; ++r) {
                    for (keel::usize c = 0; c < cols; ++c) {
                        const keel::usize srcBase = r + c * rows;
                        const keel::usize dstBase = (r * cols + c) * 4u;
                        for (keel::usize ch = 0; ch < 3; ++ch) {
                            keel::f64 v = cd.elemAsDouble(srcBase + ch * plane);
                            if (!isU8) v *= 255.0;
                            if (!keel::is_finite(v) || v < 0.0) v = 0.0;
                            if (v > 255.0) v = 255.0;
                            ds.rgbBytes[dstBase + ch] = keel::as<keel::u8>(keel::round_i64(v));
                        }
                        ds.rgbBytes[dstBase + 3u] = 255u;
                    }
                }
            } else {
                ds.type = keel::String::from("imagesc");
                ds.zJson = serializeMat2D(cd);
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
                ds.cminOrig = lo;
                ds.cmaxOrig = hi;
                const keel::f64 span = (hi > lo) ? (hi - lo) : 1.0;
                ds.zQuantized.resize(n, 255u);
                for (keel::usize k = 0; k < n; ++k) {
                    const keel::f64 v = cd.elemAsDouble(k);
                    if (!keel::is_finite(v)) {
                        ds.zQuantized[k] = 255u;
                    } else {
                        keel::f64 t = (v - lo) / span;
                        if (t < 0.0) t = 0.0;
                        if (t > 1.0) t = 1.0;
                        ds.zQuantized[k] = keel::as<keel::u8>(keel::round_i64(t * 254.0));
                    }
                }
            }
            break;
        }
        case ClassId::Stair: {
            ds.type = keel::String::from("stairs");
            ds.xJson = serializeVec1D(scene.getProperty(cid, "XData", mr));
            ds.yJson = serializeVec1D(scene.getProperty(cid, "YData", mr));
            break;
        }
        case ClassId::AnimatedLine: {
            ds.type = keel::String::from("line");
            ds.xRaw = ch->animX.clone();
            ds.yRaw = ch->animY.clone();
            ds.xJson = figdetail::serializeFlatDoubles(ch->animX);
            ds.yJson = figdetail::serializeFlatDoubles(ch->animY);
            break;
        }
        case ClassId::Text: {
            ds.type = keel::String::from("text");
            const Value pos = scene.getProperty(cid, "Position", mr);
            if (pos.numel() >= 2) {
                keel::Array<keel::f64> xv(1, pos.elemAsDouble(0));
                keel::Array<keel::f64> yv(1, pos.elemAsDouble(1));
                ds.xJson = figdetail::serializeFlatDoubles(xv);
                ds.yJson = figdetail::serializeFlatDoubles(yv);
            }
            ds.label = scene.getProperty(cid, "String", mr).toString();
            break;
        }
        default:
            continue;
        }
        dst.datasets.push_back(keel::move(ds));
    }
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
    out.axes.clear();

    // Collect direct Axes/PolarAxes and TiledChartLayout children in creation order.
    keel::Array<NodeId> axList(res);
    for (keel::usize i = fn->children.size(); i > 0; --i) {
        const NodeId cid = fn->children[i - 1];
        const GraphicsNode *ch = scene.findNode(cid);
        if (!ch) continue;
        if (ch->classId == ClassId::Axes || ch->classId == ClassId::PolarAxes) {
            if (ch->subplotRows > out.subplotRows) out.subplotRows = ch->subplotRows;
            if (ch->subplotCols > out.subplotCols) out.subplotCols = ch->subplotCols;
            axList.push_back(cid);
        } else if (ch->classId == ClassId::TiledChartLayout) {
            const Value gs = scene.getProperty(cid, "GridSize", res);
            if (gs.numel() >= 2) {
                out.subplotRows = keel::as<keel::i32>(gs.elemAsDouble(0));
                out.subplotCols = keel::as<keel::i32>(gs.elemAsDouble(1));
            }
            for (keel::usize j = ch->children.size(); j > 0; --j) {
                const NodeId tcid = ch->children[j - 1];
                if (const GraphicsNode *tc = scene.findNode(tcid);
                    tc && (tc->classId == ClassId::Axes || tc->classId == ClassId::PolarAxes)) {
                    axList.push_back(tcid);
                }
            }
        }
    }

    if (axList.empty()) {
        out.axes.push_back(AxesState{});
        out.currentAxes = 0;
        return true;
    }

    for (keel::usize k = 0; k < axList.size(); ++k) {
        AxesState st;
        projectOneAxes(scene, axList[k], st, res);
        if (axList[k] == fn->currentAxesId)
            out.currentAxes = keel::as<keel::i32>(k);
        out.axes.push_back(keel::move(st));
    }
    return true;
}

} // namespace numkit::gfx
