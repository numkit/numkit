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

#include <numkit/graphics/figure_render_state.hpp>
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
    auto addKV = [&](const char *k, keel::StringRef val, bool allowNone = false) {
        if (val.empty() || (!allowNone && val == "none")) return;
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
    } else if (scene.hasProperty(cid, "CData")) {
        const Value cd = scene.getProperty(cid, "CData", mr);
        if (cd.isNumeric() && cd.numel() == 3) {
            addKV("color", rgbToHexStr(cd));
        } else if (scene.hasProperty(cid, "MarkerEdgeColor")) {
            const Value mec = scene.getProperty(cid, "MarkerEdgeColor", mr);
            if (mec.isNumeric() && mec.numel() >= 3) addKV("color", rgbToHexStr(mec));
        }
    }
    if (scene.hasProperty(cid, "LineStyle")) {
        const keel::String ls = scene.getProperty(cid, "LineStyle", mr).toString();
        if (!ls.empty()) addKV("lineStyle", ls, /*allowNone=*/true);
    }
    if (scene.hasProperty(cid, "Marker")) {
        const keel::String mk = scene.getProperty(cid, "Marker", mr).toString();
        if (!mk.empty() && mk != "none") addKV("marker", mk);
    }
    if (scene.hasProperty(cid, "MarkerFaceColor")) {
        const Value mfc = scene.getProperty(cid, "MarkerFaceColor", mr);
        if (mfc.isNumeric() && mfc.numel() >= 3) {
            addKV("markerFaceColor", rgbToHexStr(mfc));
        } else if (mfc.isChar() || mfc.isString()) {
            const keel::String mfcs = mfc.toString();
            if (asciiEqualNoCase(mfcs, "auto"))
                addKV("markerFaceColor", "auto");
        }
    }
    if (scene.hasProperty(cid, "MarkerEdgeColor")) {
        const Value mec = scene.getProperty(cid, "MarkerEdgeColor", mr);
        if (mec.isNumeric() && mec.numel() >= 3) {
            addKV("markerEdgeColor", rgbToHexStr(mec));
        } else if (mec.isChar() || mec.isString()) {
            const keel::String mecs = mec.toString();
            if (asciiEqualNoCase(mecs, "none"))
                addKV("markerEdgeColor", "none", /*allowNone=*/true);
        }
    }
    if (const GraphicsNode *nd = scene.findNode(cid); nd && nd->classId == ClassId::Text) {
        if (scene.hasProperty(cid, "FontSize")) {
            char fbuf[32];
            addKV("fontSize", keel::format_f64_general_to(fbuf, scene.getProperty(cid, "FontSize", mr).toScalar(), 6));
        }
        if (scene.hasProperty(cid, "HorizontalAlignment")) {
            keel::String ha = scene.getProperty(cid, "HorizontalAlignment", mr).toString();
            ha.to_ascii_lower_in_place();
            addKV("halign", ha);
        }
    }
    return out;
}

keel::String serializeTickLabels(const Value &v)
{
    if (v.isEmpty())
        return keel::String{};
    keel::String out = keel::String::from("[");
    if (v.isCell()) {
        for (keel::usize i = 0; i < v.numel(); ++i) {
            if (i) out.push_back(',');
            out.push_back('"');
            out.append(v.cellAt(i).toString());
            out.push_back('"');
        }
    } else if (v.isChar() || v.isString()) {
        out.push_back('"');
        out.append(v.toString());
        out.push_back('"');
    }
    out.push_back(']');
    return out;
}

void projectOneAxes(const GraphicsScene &scene, NodeId axId, AxesState &dst, keel::Allocator *mr)
{
    const GraphicsNode *ax = scene.findNode(axId);
    if (!ax)
        return;

    dst.polar = (ax->classId == ClassId::PolarAxes);
    dst.subplotIndex = ax->subplotIdx;
    dst.subplotRows = ax->subplotRows;
    dst.subplotCols = ax->subplotCols;

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
        dst.xDir = scene.getProperty(axId, "XDir", mr).toString();
        dst.yDir = scene.getProperty(axId, "YDir", mr).toString();
        dst.boxOn = !asciiEqualNoCase(scene.getProperty(axId, "Box", mr).toString(), "off");
        dst.gridMajor = asciiEqualNoCase(scene.getProperty(axId, "XGrid", mr).toString(), "on")
                        || asciiEqualNoCase(scene.getProperty(axId, "YGrid", mr).toString(), "on");
        dst.gridMinor = asciiEqualNoCase(scene.getProperty(axId, "XMinorGrid", mr).toString(), "on")
                        || asciiEqualNoCase(scene.getProperty(axId, "YMinorGrid", mr).toString(), "on");
        if (dst.gridMajor || dst.gridMinor)
            dst.gridUserTouched = true;
        dst.fontSize = scene.getProperty(axId, "FontSize", mr).toScalar();
        dst.fontWeight = scene.getProperty(axId, "FontWeight", mr).toString();
        dst.fontName = scene.getProperty(axId, "FontName", mr).toString();
        dst.lineWidth = scene.getProperty(axId, "LineWidth", mr).toScalar();

        if (asciiEqualNoCase(scene.getProperty(axId, "DataAspectRatioMode", mr).toString(), "manual")) {
            const Value dar = scene.getProperty(axId, "DataAspectRatio", mr);
            if (dar.isNumeric() && dar.numel() >= 2
                && keel::abs(dar.elemAsDouble(0) - dar.elemAsDouble(1)) < 1e-9) {
                dst.axisMode = keel::String::from("equal");
            }
        }

        auto forwardLim = [&](const char *prop, keel::String &outJson) {
            const Value lv = scene.getProperty(axId, prop, mr);
            if (lv.isNumeric() && lv.numel() == 2) {
                const keel::f64 lo = lv.elemAsDouble(0);
                const keel::f64 hi = lv.elemAsDouble(1);
                if (keel::is_finite(lo) && keel::is_finite(hi) && hi > lo)
                    outJson = serializeVec1D(lv);
            }
        };
        forwardLim("XLim", dst.xlimJson);
        forwardLim("YLim", dst.ylimJson);
        if (asciiEqualNoCase(scene.getProperty(axId, "ZLimMode", mr).toString(), "manual"))
            forwardLim("ZLim", dst.zlimJson);

        if (asciiEqualNoCase(scene.getProperty(axId, "XTickMode", mr).toString(), "manual")) {
            dst.xTicksJson = serializeVec1D(scene.getProperty(axId, "XTick", mr));
            dst.xTicksAuto = false;
        }
        if (asciiEqualNoCase(scene.getProperty(axId, "YTickMode", mr).toString(), "manual")) {
            dst.yTicksJson = serializeVec1D(scene.getProperty(axId, "YTick", mr));
            dst.yTicksAuto = false;
        }
        if (asciiEqualNoCase(scene.getProperty(axId, "XTickLabelMode", mr).toString(), "manual"))
            dst.xTickLabelsJson = serializeTickLabels(scene.getProperty(axId, "XTickLabel", mr));
        if (asciiEqualNoCase(scene.getProperty(axId, "YTickLabelMode", mr).toString(), "manual"))
            dst.yTickLabelsJson = serializeTickLabels(scene.getProperty(axId, "YTickLabel", mr));
    }

    // Walk children in creation order (ax->children is stored oldest-first).
    for (keel::usize i = 0; i < ax->children.size(); ++i) {
        const NodeId cid = ax->children[i];
        const GraphicsNode *ch = scene.findNode(cid);
        if (!ch) continue;

        DatasetInfo ds;
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

    // Project Legend (if attached to this axes).
    if (ax->legendId != kInvalidNodeId && scene.findNode(ax->legendId)) {
        const NodeId lgId = ax->legendId;
        dst.legendHidden = asciiEqualNoCase(scene.getProperty(lgId, "Visible", mr).toString(), "off");
        dst.legendBoxOn = !asciiEqualNoCase(scene.getProperty(lgId, "Box", mr).toString(), "off");
        dst.legendLocation = scene.getProperty(lgId, "Location", mr).toString();
        dst.legendLocation.to_ascii_lower_in_place();

        const Value lgStr = scene.getProperty(lgId, "String", mr);
        if (lgStr.isCell() && !lgStr.isEmpty()) {
            for (keel::usize k = 0; k < lgStr.numel(); ++k) {
                keel::String lbl = lgStr.cellAt(k).toString();
                if (!lbl.empty())
                    dst.legendLabels.push_back(keel::move(lbl));
            }
        } else if ((lgStr.isChar() || lgStr.isString()) && !lgStr.isEmpty()) {
            keel::String lbl = lgStr.toString();
            if (!lbl.empty())
                dst.legendLabels.push_back(keel::move(lbl));
        }
        // Auto-collect DisplayName from visible chart children when legend() was
        // called with no explicit label strings (e.g. `legend('Location','northeast')`).
        if (dst.legendLabels.empty()) {
            for (keel::usize d = 0; d < dst.datasets.size(); ++d) {
                const DatasetInfo &ds = dst.datasets[d];
                if (!ds.visible || !ds.handleVisible || ds.type == "text"
                    || ds.type == "imagesc" || ds.type == "image-rgb")
                    continue;
                if (!ds.label.empty())
                    dst.legendLabels.push_back(ds.label.clone());
            }
        }
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
    if (scene.hasProperty(figId, "Color")) {
        const Value fcol = scene.getProperty(figId, "Color", res);
        if (fcol.isNumeric() && fcol.numel() >= 3)
            out.bgColorHex = rgbToHexStr(fcol);
    }

    // Collect direct Axes/PolarAxes and TiledChartLayout children in creation order.
    keel::Array<NodeId> axList(res);
    for (keel::usize i = 0; i < fn->children.size(); ++i) {
        const NodeId cid = fn->children[i];
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
            for (keel::usize j = 0; j < ch->children.size(); ++j) {
                const NodeId tcid = ch->children[j];
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
