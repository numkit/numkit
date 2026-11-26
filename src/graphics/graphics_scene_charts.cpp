// src/graphics/graphics_scene_charts.cpp
//
// Phase 3 GraphicsScene operations: Chart primitive creation, ColorOrder cycling,
// live XData/YData/ZData/CData -> Axes auto-limits & nice-ticks recomputation,
// Bar XEndPoints/YEndPoints, Histogram binning & normalizations, Patch
// Vertices/Faces <-> X/Y/ZData sync, and AnimatedLine point buffers.

#include <keel/algo.h>
#include <keel/core.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/builtin/datafun.hpp>
#include <numkit/graphics/graphics_scene.hpp>
#include <numkit/value/error.hpp>

namespace numkit::gfx {

namespace {

void scanNumericBounds(const Value &v, keel::f64 &lo, keel::f64 &hi, bool &any) noexcept
{
    if (!v.isNumeric() || v.isEmpty())
        return;
    const keel::usize n = v.numel();
    for (keel::usize i = 0; i < n; ++i) {
        const keel::f64 x = v.elemAsDouble(i);
        if (keel::is_finite(x)) {
            if (!any) {
                lo = hi = x;
                any = true;
            } else {
                if (x < lo) lo = x;
                if (x > hi) hi = x;
            }
        }
    }
}

void computeNiceLimitRange(keel::f64 dmin, keel::f64 dmax, keel::StringRef method,
                           bool isLog, keel::f64 &loOut, keel::f64 &hiOut) noexcept
{
    if (dmax <= dmin) {
        if (dmin == 0.0) {
            loOut = -1.0;
            hiOut = 1.0;
        } else {
            const keel::f64 pad = keel::max(1.0, keel::abs(dmin) * 0.1);
            loOut = dmin - pad;
            hiOut = dmin + pad;
        }
        return;
    }
    if (asciiEqualNoCase(method, "tight")) {
        loOut = dmin;
        hiOut = dmax;
        return;
    }
    if (isLog && dmin > 0.0 && dmax > dmin) {
        loOut = keel::pow(10.0, keel::floor(keel::log10(dmin)));
        hiOut = keel::pow(10.0, keel::ceil(keel::log10(dmax)));
        if (hiOut <= loOut) hiOut = loOut * 10.0;
        return;
    }
    const keel::f64 span = dmax - dmin;
    if (asciiEqualNoCase(method, "padded")) {
        const keel::f64 pad = span * 0.07;
        loOut = dmin - pad;
        hiOut = dmax + pad;
        return;
    }
    // Default 'tickaligned': snap outer bounds to clean nice tick step.
    const keel::f64 p10 = keel::pow(10.0, keel::floor(keel::log10(span)));
    const keel::f64 s = span / p10;
    const keel::f64 step = (s <= 1.5) ? 0.2 * p10
                         : (s <= 3.0) ? 0.5 * p10
                         : (s <= 7.0 || s >= 9.5) ? 1.0 * p10
                                                  : 2.0 * p10;
    loOut = keel::floor(dmin / step + 1e-12) * step;
    hiOut = keel::ceil(dmax / step - 1e-12) * step;
    if (hiOut <= loOut) {
        loOut = dmin;
        hiOut = dmax;
    }
}

} // anonymous namespace

bool GraphicsScene::isChartClass(ClassId cls) noexcept
{
    switch (cls) {
    case ClassId::Line:
    case ClassId::Scatter:
    case ClassId::Bar:
    case ClassId::Surface:
    case ClassId::Patch:
    case ClassId::Quiver:
    case ClassId::Contour:
    case ClassId::Stem:
    case ClassId::ErrorBar:
    case ClassId::Histogram:
    case ClassId::Area:
    case ClassId::Image:
    case ClassId::Stair:
    case ClassId::AnimatedLine:
        return true;
    default:
        return false;
    }
}

NodeId GraphicsScene::prepareAxesForPlot(NodeId axId, bool resetIfReplace, bool is3D)
{
    const NodeId target = (axId != kInvalidNodeId && findNode(axId) != nullptr)
                              ? axId : ensureCurrentAxes();
    if (resetIfReplace && !isHoldOn(target)) {
        const GraphicsNode *axCur = findNode(target);
        if (axCur && axCur->classId == ClassId::Axes && axCur->yAxisRightId != kInvalidNodeId) {
            // Dual-Y axes: only clear children on the active side so the opposite side persists.
            const char activeSide = axCur->yyRightActive ? 'Y' : '\0';
            keel::Array<NodeId> kids = axCur->children.clone(mr_);
            for (NodeId cid : kids) {
                if (const GraphicsNode *cn = findNode(cid); cn && cn->rulerAxis == activeSide)
                    deleteNode(cid, true);
            }
        } else {
            clearAxes(target, false);
            if (GraphicsNode *ax = findNode(target)) {
                ax->props.remove("XLim");
                ax->props.remove("YLim");
                ax->props.remove("ZLim");
                ax->props.remove("CLim");
                ax->props.put(keel::String::from("XLimMode", mr_), Value::fromString("auto", mr_));
                ax->props.put(keel::String::from("YLimMode", mr_), Value::fromString("auto", mr_));
                ax->props.put(keel::String::from("ZLimMode", mr_), Value::fromString("auto", mr_));
                ax->props.put(keel::String::from("CLimMode", mr_), Value::fromString("auto", mr_));
                ax->props.put(keel::String::from("XTickMode", mr_), Value::fromString("auto", mr_));
                ax->props.put(keel::String::from("YTickMode", mr_), Value::fromString("auto", mr_));
                ax->props.put(keel::String::from("ZTickMode", mr_), Value::fromString("auto", mr_));
                if (ax->classId == ClassId::Axes) {
                    ax->props.put(keel::String::from("Box", mr_),
                                  Value::fromString(is3D ? "off" : "on", mr_));
                }
            }
        }
    }
    return target;
}

Value GraphicsScene::nextSeriesColor(NodeId axId, keel::i32 &seriesIdxOut, keel::Allocator *mr)
{
    auto *res = mr ? mr : mr_;
    GraphicsNode *ax = findNode(axId);
    if (!ax || (ax->classId != ClassId::Axes && ax->classId != ClassId::PolarAxes)) {
        seriesIdxOut = 1;
        Value rgb = Value::matrix(1, 3, ValueType::DOUBLE, res);
        rgb.doubleDataMut()[0] = 0.066;
        rgb.doubleDataMut()[1] = 0.443;
        rgb.doubleDataMut()[2] = 0.745;
        return rgb;
    }
    const Value co = getProperty(axId, "ColorOrder", res);
    const Value idxV = getProperty(axId, "ColorOrderIndex", res);
    const Value nsiV = getProperty(axId, "NextSeriesIndex", res);
    const keel::i32 idx = keel::max<keel::i32>(1, keel::as<keel::i32>(idxV.toScalar()));
    const keel::i32 nsi = keel::max<keel::i32>(1, keel::as<keel::i32>(nsiV.toScalar()));
    seriesIdxOut = nsi;

    const keel::usize nRows = (co.isNumeric() && co.dims().cols() == 3 && co.dims().rows() > 0)
                                  ? co.dims().rows() : 0;
    Value rgb = Value::matrix(1, 3, ValueType::DOUBLE, res);
    if (nRows > 0) {
        const keel::usize row = keel::as<keel::usize>(idx - 1) % nRows;
        rgb.doubleDataMut()[0] = co.elemAsDouble(row);
        rgb.doubleDataMut()[1] = co.elemAsDouble(row + nRows);
        rgb.doubleDataMut()[2] = co.elemAsDouble(row + 2 * nRows);
        const keel::i32 nextIdx = (keel::as<keel::usize>(idx) % nRows) + 1;
        ax->props.put(keel::String::from("ColorOrderIndex", mr_), Value::scalar(keel::as<keel::f64>(nextIdx), mr_));
    } else {
        rgb.doubleDataMut()[0] = 0.066;
        rgb.doubleDataMut()[1] = 0.443;
        rgb.doubleDataMut()[2] = 0.745;
    }
    ax->props.put(keel::String::from("NextSeriesIndex", mr_), Value::scalar(keel::as<keel::f64>(nsi + 1), mr_));
    return rgb;
}

NodeId GraphicsScene::createChartNode(ClassId cls, NodeId parentId, bool consumeSeriesColor)
{
    NodeId actualParent = parentId;
    if (cls == ClassId::TextBox) {
        if (actualParent == kInvalidNodeId)
            actualParent = ensureCurrentFigure();
    } else {
        if (actualParent == kInvalidNodeId)
            actualParent = ensureCurrentAxes();
    }

    const NodeId id = allocateNode(cls, actualParent);
    if (const GraphicsNode *pn = findNode(actualParent);
        pn && pn->classId == ClassId::Axes && pn->yyRightActive) {
        if (GraphicsNode *n = findNode(id))
            n->rulerAxis = 'Y';
    }
    if (consumeSeriesColor && cls != ClassId::TextBox && cls != ClassId::Image
        && cls != ClassId::Surface && cls != ClassId::Contour && cls != ClassId::Patch) {
        keel::i32 sIdx = 1;
        Value rgb = nextSeriesColor(actualParent, sIdx, mr_);
        if (GraphicsNode *n = findNode(id)) {
            if (schemaFor(cls).findExact("SeriesIndex") != nullptr)
                n->props.put(keel::String::from("SeriesIndex", mr_), Value::scalar(keel::as<keel::f64>(sIdx), mr_));
            if (cls == ClassId::Bar || cls == ClassId::Area || cls == ClassId::Histogram) {
                if (!n->props.contains("FaceColor"))
                    n->props.put(keel::String::from("FaceColor", mr_), rgb);
            } else if (cls == ClassId::Scatter) {
                if (!n->props.contains("CData"))
                    n->props.put(keel::String::from("CData", mr_), rgb);
            } else if (schemaFor(cls).findExact("Color") != nullptr) {
                if (!n->props.contains("Color"))
                    n->props.put(keel::String::from("Color", mr_), rgb);
            }
        }
    }
    return id;
}

void GraphicsScene::recomputeAxesAutoLimits(NodeId axId, keel::Allocator *mr)
{
    if (inSyncGuard_)
        return;
    auto *res = mr ? mr : mr_;
    GraphicsNode *ax = findNode(axId);
    if (!ax || (ax->classId != ClassId::Axes && ax->classId != ClassId::PolarAxes))
        return;

    if (ax->classId == ClassId::PolarAxes) {
        const Value rMode = getProperty(axId, "RLimMode", res);
        if (!asciiEqualNoCase(rMode.toString(), "auto"))
            return;
        keel::f64 rmin = 0.0, rmax = 0.0;
        bool anyR = false;
        for (NodeId cid : ax->children) {
            const GraphicsNode *ch = findNode(cid);
            if (!ch || !isChartClass(ch->classId)) continue;
            if (asciiEqualNoCase(getProperty(cid, "Visible", res).toString(), "off")) continue;
            if (const Value *rv = ch->props.get("YData"))
                scanNumericBounds(*rv, rmin, rmax, anyR);
        }
        if (anyR) {
            keel::f64 lo = 0.0, hi = 1.0;
            computeNiceLimitRange(keel::min(0.0, rmin), rmax, "tickaligned", false, lo, hi);
            Value rl = Value::matrix(1, 2, ValueType::DOUBLE, res);
            rl.doubleDataMut()[0] = keel::max(0.0, lo);
            rl.doubleDataMut()[1] = keel::max(rl.doubleDataMut()[0] + 1e-6, hi);
            ax->props.put(keel::String::from("RLim", mr_), keel::move(rl));
            ax->version = generation_ + 1;
        }
        return;
    }

    // Cartesian Axes
    keel::f64 xmin = 0.0, xmax = 0.0; bool anyX = false;
    keel::f64 ymin = 0.0, ymax = 0.0; bool anyY = false;
    keel::f64 zmin = 0.0, zmax = 0.0; bool anyZ = false;
    keel::f64 cmin = 0.0, cmax = 0.0; bool anyC = false;
    bool hasBaseZero = false;
    keel::f64 baseVal = 0.0;
    bool hasImage = false;
    bool hasNonImage = false;
    const Value viewV = getProperty(axId, "View", res);
    const bool isView3D = (viewV.isNumeric() && viewV.numel() >= 2 && keel::abs(viewV.elemAsDouble(1) - 90.0) > 1e-6);

    for (NodeId cid : ax->children) {
        const GraphicsNode *ch = findNode(cid);
        if (!ch || !isChartClass(ch->classId))
            continue;
        if (asciiEqualNoCase(getProperty(cid, "Visible", res).toString(), "off"))
            continue;

        if (ch->classId == ClassId::Histogram) {
            hasNonImage = true;
            if (const Value *be = ch->props.get("BinEdges"))
                scanNumericBounds(*be, xmin, xmax, anyX);
            if (const Value *vals = ch->props.get("Values")) {
                scanNumericBounds(*vals, ymin, ymax, anyY);
                hasBaseZero = true;
            }
            continue;
        }
        if (ch->classId == ClassId::Image) {
            hasImage = true;
            const Value *cd = ch->props.get("CData");
            const Value *xd = ch->props.get("XData");
            const Value *yd = ch->props.get("YData");
            const keel::usize cols = (cd && !cd->isEmpty()) ? keel::max<keel::usize>(1, cd->dims().cols()) : 1;
            const keel::usize rows = (cd && !cd->isEmpty()) ? keel::max<keel::usize>(1, cd->dims().rows()) : 1;
            if (xd && !xd->isEmpty()) {
                keel::f64 x0 = xd->elemAsDouble(0);
                keel::f64 x1 = xd->elemAsDouble(xd->numel() - 1);
                if (x1 < x0) keel::swap(x0, x1);
                const keel::f64 dx = (cols > 1) ? (x1 - x0) / keel::as<keel::f64>(cols - 1) : 1.0;
                const keel::f64 lo = x0 - 0.5 * dx;
                const keel::f64 hi = x1 + 0.5 * dx;
                if (!anyX) { xmin = lo; xmax = hi; anyX = true; }
                else { xmin = keel::min(xmin, lo); xmax = keel::max(xmax, hi); }
            } else if (cd && !cd->isEmpty()) {
                const keel::f64 lo = 0.5, hi = keel::as<keel::f64>(cols) + 0.5;
                if (!anyX) { xmin = lo; xmax = hi; anyX = true; }
                else { xmin = keel::min(xmin, lo); xmax = keel::max(xmax, hi); }
            }
            if (yd && !yd->isEmpty()) {
                keel::f64 y0 = yd->elemAsDouble(0);
                keel::f64 y1 = yd->elemAsDouble(yd->numel() - 1);
                if (y1 < y0) keel::swap(y0, y1);
                const keel::f64 dy = (rows > 1) ? (y1 - y0) / keel::as<keel::f64>(rows - 1) : 1.0;
                const keel::f64 lo = y0 - 0.5 * dy;
                const keel::f64 hi = y1 + 0.5 * dy;
                if (!anyY) { ymin = lo; ymax = hi; anyY = true; }
                else { ymin = keel::min(ymin, lo); ymax = keel::max(ymax, hi); }
            } else if (cd && !cd->isEmpty()) {
                const keel::f64 lo = 0.5, hi = keel::as<keel::f64>(rows) + 0.5;
                if (!anyY) { ymin = lo; ymax = hi; anyY = true; }
                else { ymin = keel::min(ymin, lo); ymax = keel::max(ymax, hi); }
            }
            if (cd) scanNumericBounds(*cd, cmin, cmax, anyC);
            continue;
        }
        hasNonImage = true;
        if (ch->classId == ClassId::AnimatedLine) {
            for (keel::f64 v : ch->animX) {
                if (keel::is_finite(v)) {
                    if (!anyX) { xmin = xmax = v; anyX = true; }
                    else { xmin = keel::min(xmin, v); xmax = keel::max(xmax, v); }
                }
            }
            for (keel::f64 v : ch->animY) {
                if (keel::is_finite(v)) {
                    if (!anyY) { ymin = ymax = v; anyY = true; }
                    else { ymin = keel::min(ymin, v); ymax = keel::max(ymax, v); }
                }
            }
            if (ch->animHasZ) {
                for (keel::f64 v : ch->animZ) {
                    if (keel::is_finite(v)) {
                        if (!anyZ) { zmin = zmax = v; anyZ = true; }
                        else { zmin = keel::min(zmin, v); zmax = keel::max(zmax, v); }
                    }
                }
            }
            continue;
        }

        if (const Value *xd = ch->props.get("XData"))
            scanNumericBounds(*xd, xmin, xmax, anyX);
        if (const Value *yd = ch->props.get("YData"))
            scanNumericBounds(*yd, ymin, ymax, anyY);
        if (const Value *zd = ch->props.get("ZData")) {
            if (ch->classId == ClassId::Contour) {
                scanNumericBounds(*zd, cmin, cmax, anyC);
                if (isView3D)
                    scanNumericBounds(*zd, zmin, zmax, anyZ);
            } else {
                scanNumericBounds(*zd, zmin, zmax, anyZ);
            }
        }
        if (const Value *cd = ch->props.get("CData")) {
            if (ch->classId == ClassId::Surface || ch->classId == ClassId::Contour
                || (ch->classId == ClassId::Scatter && cd->numel() != 3)) {
                scanNumericBounds(*cd, cmin, cmax, anyC);
            }
        }
        if (ch->classId == ClassId::Bar || ch->classId == ClassId::Area || ch->classId == ClassId::Stem) {
            hasBaseZero = true;
            baseVal = getProperty(cid, "BaseValue", res).toScalar();
        }
        if (ch->classId == ClassId::ErrorBar) {
            const Value *yd = ch->props.get("YData");
            const Value *yp = ch->props.get("YPositiveDelta");
            const Value *yn = ch->props.get("YNegativeDelta");
            if (yd && yp && yd->numel() == yp->numel()) {
                for (keel::usize i = 0; i < yd->numel(); ++i) {
                    const keel::f64 hi = yd->elemAsDouble(i) + keel::abs(yp->elemAsDouble(i));
                    if (keel::is_finite(hi)) ymax = anyY ? keel::max(ymax, hi) : hi;
                }
            }
            if (yd && yn && yd->numel() == yn->numel()) {
                for (keel::usize i = 0; i < yd->numel(); ++i) {
                    const keel::f64 lo = yd->elemAsDouble(i) - keel::abs(yn->elemAsDouble(i));
                    if (keel::is_finite(lo)) ymin = anyY ? keel::min(ymin, lo) : lo;
                }
            }
        }
        if (ch->classId == ClassId::Patch) {
            if (const Value *verts = ch->props.get("Vertices"); verts && verts->isNumeric() && !verts->isEmpty()) {
                const keel::usize nr = verts->dims().rows();
                const keel::usize nc = verts->dims().cols();
                for (keel::usize r = 0; r < nr; ++r) {
                    if (nc >= 1) {
                        const keel::f64 vx = verts->elemAsDouble(r);
                        if (keel::is_finite(vx)) { if (!anyX) { xmin = xmax = vx; anyX = true; } else { xmin = keel::min(xmin, vx); xmax = keel::max(xmax, vx); } }
                    }
                    if (nc >= 2) {
                        const keel::f64 vy = verts->elemAsDouble(r + nr);
                        if (keel::is_finite(vy)) { if (!anyY) { ymin = ymax = vy; anyY = true; } else { ymin = keel::min(ymin, vy); ymax = keel::max(ymax, vy); } }
                    }
                    if (nc >= 3) {
                        const keel::f64 vz = verts->elemAsDouble(r + 2 * nr);
                        if (keel::is_finite(vz)) { if (!anyZ) { zmin = zmax = vz; anyZ = true; } else { zmin = keel::min(zmin, vz); zmax = keel::max(zmax, vz); } }
                    }
                }
            }
        }
    }

    if (hasBaseZero && anyY) {
        if (ymin > baseVal) ymin = baseVal;
        if (ymax < baseVal) ymax = baseVal;
    }

    auto applyAxis = [&](char axisChar, const char *limP, const char *modeP,
                         const char *methP, const char *sclP,
                         keel::f64 dmin, keel::f64 dmax, bool any) {
        if (!any)
            return;
        if (!asciiEqualNoCase(getProperty(axId, modeP, res).toString(), "auto"))
            return;
        const keel::String meth = methP ? getProperty(axId, methP, res).toString()
                                        : keel::String::from("tight", res);
        const bool isLog = sclP && asciiEqualNoCase(getProperty(axId, sclP, res).toString(), "log");
        keel::f64 lo = 0.0, hi = 1.0;
        computeNiceLimitRange(dmin, dmax, meth, isLog, lo, hi);
        Value lv = Value::matrix(1, 2, ValueType::DOUBLE, res);
        lv.doubleDataMut()[0] = lo;
        lv.doubleDataMut()[1] = hi;
        ax->props.put(keel::String::from(limP, mr_), lv);
        // Sync to owned ruler WITHOUT flipping LimitsMode to 'manual'.
        const NodeId rid = (axisChar == 'x') ? ax->xAxisId
                         : (axisChar == 'y' ? (ax->yyRightActive && ax->yAxisRightId != kInvalidNodeId ? ax->yAxisRightId : ax->yAxisId)
                                            : (axisChar == 'z' ? ax->zAxisId : kInvalidNodeId));
        if (GraphicsNode *rn = findNode(rid))
            rn->props.put(keel::String::from("Limits", mr_), lv);
        if (axisChar == 'x' || axisChar == 'y' || axisChar == 'z')
            recomputeAutoTicksForAxis(axId, axisChar, res);
        if (axisChar == 'c' && ax->colorBarId != kInvalidNodeId) {
            if (GraphicsNode *cb = findNode(ax->colorBarId))
                cb->props.put(keel::String::from("Limits", mr_), lv);
        }
    };

    const bool imageOnlyTight = (hasImage && !hasNonImage);
    applyAxis('x', "XLim", "XLimMode", imageOnlyTight ? nullptr : "XLimitMethod", "XScale", xmin, xmax, anyX);
    applyAxis('y', "YLim", "YLimMode", imageOnlyTight ? nullptr : "YLimitMethod", "YScale", ymin, ymax, anyY);
    if (anyZ) {
        applyAxis('z', "ZLim", "ZLimMode", "ZLimitMethod", "ZScale", zmin, zmax, true);
    } else if ((anyX || anyY) && asciiEqualNoCase(getProperty(axId, "ZLimMode", res).toString(), "auto")) {
        // 2-D plots default auto ZLim to [-1, 1] (matches reference engine).
        applyAxis('z', "ZLim", "ZLimMode", nullptr, "ZScale", -1.0, 1.0, true);
    }
    applyAxis('c', "CLim", "CLimMode", nullptr, nullptr, cmin, cmax, anyC);
    ax->version = generation_ + 1;
}

void GraphicsScene::recomputeBarEndPoints(NodeId axId, keel::Allocator *mr)
{
    auto *res = mr ? mr : mr_;
    const GraphicsNode *ax = findNode(axId);
    if (!ax || ax->classId != ClassId::Axes)
        return;

    // Collect Bar children in creation order (reverse of newest-first ax->children).
    keel::Array<NodeId> bars(res);
    for (keel::usize i = ax->children.size(); i > 0; --i) {
        const NodeId cid = ax->children[i - 1];
        if (const GraphicsNode *cn = findNode(cid); cn && cn->classId == ClassId::Bar)
            bars.push_back(cid);
    }
    if (bars.empty())
        return;

    const keel::usize M = bars.size();
    const keel::f64 groupWidth = (M == 1) ? 0.0
                                          : keel::min(0.8, keel::as<keel::f64>(M) / (keel::as<keel::f64>(M) + 1.5));

    for (keel::usize k = 0; k < M; ++k) {
        GraphicsNode *bn = findNode(bars[k]);
        if (!bn) continue;
        const Value *xd = bn->props.get("XData");
        const Value *yd = bn->props.get("YData");
        if (!xd || !yd || xd->isEmpty()) continue;

        const keel::usize n = xd->numel();
        const keel::String layout = getProperty(bars[k], "BarLayout", res).toString();
        const bool isGrouped = (M > 1) && asciiEqualNoCase(layout, "grouped");

        // Compute typical x spacing for grouped offset.
        keel::f64 dx = 1.0;
        if (n >= 2) {
            keel::f64 minStep = 1e300;
            for (keel::usize j = 1; j < n; ++j) {
                const keel::f64 d = keel::abs(xd->elemAsDouble(j) - xd->elemAsDouble(j - 1));
                if (d > 0.0 && d < minStep) minStep = d;
            }
            if (minStep < 1e299) dx = minStep;
        }
        const keel::f64 offset = isGrouped
            ? ((-groupWidth * 0.5 + (keel::as<keel::f64>(k) + 0.5) * (groupWidth / keel::as<keel::f64>(M))) * dx)
            : 0.0;

        Value xep = Value::matrix(1, n, ValueType::DOUBLE, res);
        Value yep = Value::matrix(1, n, ValueType::DOUBLE, res);
        for (keel::usize j = 0; j < n; ++j) {
            xep.doubleDataMut()[j] = xd->elemAsDouble(j) + offset;
            yep.doubleDataMut()[j] = (j < yd->numel()) ? yd->elemAsDouble(j) : 0.0;
        }
        bn->props.put(keel::String::from("XEndPoints", mr_), keel::move(xep));
        bn->props.put(keel::String::from("YEndPoints", mr_), keel::move(yep));
    }
}

void GraphicsScene::recomputeHistogramBins(NodeId histId, keel::Allocator *mr)
{
    auto *res = mr ? mr : mr_;
    GraphicsNode *hn = findNode(histId);
    if (!hn || hn->classId != ClassId::Histogram)
        return;

    const Value *dataV = hn->props.get("Data");
    if (!dataV || !dataV->isNumeric() || dataV->isEmpty()) {
        Value eV = Value::matrix(1, 2, ValueType::DOUBLE, res);
        eV.doubleDataMut()[0] = 0.0; eV.doubleDataMut()[1] = 1.0;
        Value vV = Value::matrix(1, 1, ValueType::DOUBLE, res);
        vV.doubleDataMut()[0] = 0.0;
        hn->props.put(keel::String::from("BinEdges", mr_), eV);
        hn->props.put(keel::String::from("Values", mr_), vV);
        hn->props.put(keel::String::from("NumBins", mr_), Value::scalar(1.0, mr_));
        hn->props.put(keel::String::from("BinWidth", mr_), Value::scalar(1.0, mr_));
        hn->props.put(keel::String::from("BinLimits", mr_), eV);
        hn->version = generation_ + 1;
        return;
    }

    // Build HistBinSpec from current Histogram properties (single source of truth with histcounts).
    numkit::builtin::HistBinSpec spec;
    const keel::String bm = getProperty(histId, "BinMethod", res).toString();
    if (asciiEqualNoCase(bm, "scott"))         spec.method = numkit::builtin::HistBinMethod::Scott;
    else if (asciiEqualNoCase(bm, "fd"))       spec.method = numkit::builtin::HistBinMethod::Fd;
    else if (asciiEqualNoCase(bm, "integers")) spec.method = numkit::builtin::HistBinMethod::Integers;
    else if (asciiEqualNoCase(bm, "sturges"))  spec.method = numkit::builtin::HistBinMethod::Sturges;
    else if (asciiEqualNoCase(bm, "sqrt"))     spec.method = numkit::builtin::HistBinMethod::Sqrt;
    else                                       spec.method = numkit::builtin::HistBinMethod::Auto;

    if (const Value *nbV = hn->props.get("NumBins"); nbV && nbV->isNumeric() && nbV->toScalar() >= 1.0
        && hn->props.contains("_NumBinsExplicit")) {
        spec.hasNumBins = true;
        spec.numBins = keel::floor(nbV->toScalar());
    }
    if (const Value *bwV = hn->props.get("BinWidth"); bwV && bwV->isNumeric() && bwV->toScalar() > 0.0
        && hn->props.contains("_BinWidthExplicit")) {
        spec.hasBinWidth = true;
        spec.binWidth = bwV->toScalar();
        spec.hasNumBins = false;
    }
    if (const Value *blV = hn->props.get("BinLimits"); blV && blV->isNumeric() && blV->numel() >= 2
        && asciiEqualNoCase(getProperty(histId, "BinLimitsMode", res).toString(), "manual")) {
        spec.hasBinLimits = true;
        spec.limLo = blV->elemAsDouble(0);
        spec.limHi = blV->elemAsDouble(1);
    }

    Value edgesVal;
    const Value *explicitEdges = hn->props.get("BinEdges");
    if (explicitEdges && explicitEdges->isNumeric() && explicitEdges->numel() >= 2
        && hn->props.contains("_EdgesExplicit")) {
        edgesVal = *explicitEdges;
    } else {
        edgesVal = numkit::builtin::histcountsAutoEdges(*dataV, spec, res);
    }

    const keel::String normStr = getProperty(histId, "Normalization", res).toString();
    numkit::builtin::HistNorm norm = numkit::builtin::HistNorm::Count;
    bool isPercentage = false;
    if (asciiEqualNoCase(normStr, "probability"))        norm = numkit::builtin::HistNorm::Probability;
    else if (asciiEqualNoCase(normStr, "percentage"))  { norm = numkit::builtin::HistNorm::Probability; isPercentage = true; }
    else if (asciiEqualNoCase(normStr, "countdensity"))  norm = numkit::builtin::HistNorm::CountDensity;
    else if (asciiEqualNoCase(normStr, "pdf"))           norm = numkit::builtin::HistNorm::Pdf;
    else if (asciiEqualNoCase(normStr, "cumcount"))      norm = numkit::builtin::HistNorm::CumCount;
    else if (asciiEqualNoCase(normStr, "cdf"))           norm = numkit::builtin::HistNorm::Cdf;

    Value valsVal = numkit::builtin::histcounts(*dataV, edgesVal, norm, res);
    if (isPercentage && valsVal.isNumeric()) {
        Value pct = Value::matrix(valsVal.dims().rows(), valsVal.dims().cols(), ValueType::DOUBLE, res);
        for (keel::usize i = 0; i < valsVal.numel(); ++i)
            pct.doubleDataMut()[i] = valsVal.elemAsDouble(i) * 100.0;
        valsVal = keel::move(pct);
    }

    const keel::usize ne = edgesVal.numel();
    const keel::usize nb = (ne >= 2) ? (ne - 1) : 0;
    Value blV = Value::matrix(1, 2, ValueType::DOUBLE, res);
    blV.doubleDataMut()[0] = (ne >= 1) ? edgesVal.elemAsDouble(0) : 0.0;
    blV.doubleDataMut()[1] = (ne >= 1) ? edgesVal.elemAsDouble(ne - 1) : 1.0;

    hn->props.put(keel::String::from("BinEdges", mr_), edgesVal);
    hn->props.put(keel::String::from("Values", mr_), valsVal);
    hn->props.put(keel::String::from("NumBins", mr_), Value::scalar(keel::as<keel::f64>(nb), mr_));
    hn->props.put(keel::String::from("BinWidth", mr_),
                  Value::scalar((ne >= 2) ? (edgesVal.elemAsDouble(1) - edgesVal.elemAsDouble(0)) : 1.0, mr_));
    hn->props.put(keel::String::from("BinLimits", mr_), keel::move(blV));
    hn->version = generation_ + 1;
}

void GraphicsScene::syncPatchVerticesAndData(NodeId patchId, keel::StringRef changedProp, keel::Allocator *mr)
{
    if (inSyncGuard_) return;
    auto *res = mr ? mr : mr_;
    GraphicsNode *pn = findNode(patchId);
    if (!pn || pn->classId != ClassId::Patch)
        return;

    inSyncGuard_ = true;
    if (changedProp == "XData" || changedProp == "YData" || changedProp == "ZData") {
        const Value *xd = pn->props.get("XData");
        const Value *yd = pn->props.get("YData");
        const Value *zd = pn->props.get("ZData");
        if (xd && yd && xd->isNumeric() && yd->isNumeric() && xd->numel() == yd->numel() && xd->numel() > 0) {
            const keel::usize n = xd->numel();
            const bool hasZ = zd && zd->isNumeric() && zd->numel() == n;
            const keel::usize cols = hasZ ? 3 : 2;
            Value verts = Value::matrix(n, cols, ValueType::DOUBLE, res);
            for (keel::usize i = 0; i < n; ++i) {
                verts.doubleDataMut()[i] = xd->elemAsDouble(i);
                verts.doubleDataMut()[i + n] = yd->elemAsDouble(i);
                if (hasZ) verts.doubleDataMut()[i + 2 * n] = zd->elemAsDouble(i);
            }
            const keel::usize nFaces = xd->dims().cols();
            const keel::usize nPerFace = xd->dims().rows();
            const keel::usize fRows = (nPerFace == 1) ? 1 : nFaces;
            const keel::usize fCols = (nPerFace == 1) ? nFaces : nPerFace;
            Value faces = Value::matrix(fRows, fCols, ValueType::DOUBLE, res);
            for (keel::usize r = 0; r < fRows; ++r) {
                for (keel::usize c = 0; c < fCols; ++c)
                    faces.doubleDataMut()[r + c * fRows] = keel::as<keel::f64>(r * fCols + c + 1);
            }
            pn->props.put(keel::String::from("Vertices", mr_), keel::move(verts));
            pn->props.put(keel::String::from("Faces", mr_), keel::move(faces));
        }
    } else if (changedProp == "Vertices" || changedProp == "Faces") {
        const Value *verts = pn->props.get("Vertices");
        const Value *faces = pn->props.get("Faces");
        if (verts && faces && verts->isNumeric() && faces->isNumeric()
            && verts->dims().rows() > 0 && verts->dims().cols() >= 2 && !faces->isEmpty()) {
            const keel::usize vr = verts->dims().rows();
            const keel::usize vc = verts->dims().cols();
            const keel::usize fr = faces->dims().rows();
            const keel::usize fc = faces->dims().cols();
            Value xd = Value::matrix(fc, fr, ValueType::DOUBLE, res);
            Value yd = Value::matrix(fc, fr, ValueType::DOUBLE, res);
            Value zd = (vc >= 3) ? Value::matrix(fc, fr, ValueType::DOUBLE, res)
                                 : Value::matrix(0, 0, ValueType::DOUBLE, res);
            for (keel::usize f = 0; f < fr; ++f) {
                for (keel::usize k = 0; k < fc; ++k) {
                    const keel::f64 rawIdx = faces->elemAsDouble(f + k * fr);
                    keel::f64 xv = keel::kNaN, yv = keel::kNaN, zv = keel::kNaN;
                    if (keel::is_finite(rawIdx) && rawIdx >= 1.0 && rawIdx <= keel::as<keel::f64>(vr)) {
                        const keel::usize vi = keel::as<keel::usize>(rawIdx) - 1;
                        xv = verts->elemAsDouble(vi);
                        yv = verts->elemAsDouble(vi + vr);
                        if (vc >= 3) zv = verts->elemAsDouble(vi + 2 * vr);
                    }
                    xd.doubleDataMut()[k + f * fc] = xv;
                    yd.doubleDataMut()[k + f * fc] = yv;
                    if (vc >= 3) zd.doubleDataMut()[k + f * fc] = zv;
                }
            }
            pn->props.put(keel::String::from("XData", mr_), keel::move(xd));
            pn->props.put(keel::String::from("YData", mr_), keel::move(yd));
            if (vc >= 3)
                pn->props.put(keel::String::from("ZData", mr_), keel::move(zd));
        }
    }
    inSyncGuard_ = false;
}

void GraphicsScene::animatedLineAddPoints(NodeId alId, const Value &x, const Value &y,
                                          const Value *z, keel::Allocator *mr)
{
    auto *res = mr ? mr : mr_;
    GraphicsNode *al = findNode(alId);
    if (!al || al->classId != ClassId::AnimatedLine)
        throw Error("addpoints: first argument must be an AnimatedLine handle.",
                    0, 0, "addpoints", "", "numkit:graphics:InvalidHandle");
    const keel::usize n = keel::min(x.numel(), y.numel());
    const bool hasZ = (z != nullptr && z->isNumeric() && z->numel() >= n);
    if (hasZ) al->animHasZ = true;
    for (keel::usize i = 0; i < n; ++i) {
        al->animX.push_back(x.elemAsDouble(i));
        al->animY.push_back(y.elemAsDouble(i));
        al->animZ.push_back(hasZ ? z->elemAsDouble(i) : 0.0);
    }
    // Enforce MaximumNumPoints (if finite).
    const Value maxP = getProperty(alId, "MaximumNumPoints", res);
    if (maxP.isNumeric() && keel::is_finite(maxP.toScalar()) && maxP.toScalar() >= 1.0) {
        const keel::usize cap = keel::as<keel::usize>(maxP.toScalar());
        if (al->animX.size() > cap) {
            const keel::usize drop = al->animX.size() - cap;
            keel::Array<keel::f64> nx(mr_), ny(mr_), nz(mr_);
            nx.reserve(cap); ny.reserve(cap); nz.reserve(cap);
            for (keel::usize i = drop; i < al->animX.size(); ++i) {
                nx.push_back(al->animX[i]);
                ny.push_back(al->animY[i]);
                nz.push_back(al->animZ[i]);
            }
            al->animX = keel::move(nx);
            al->animY = keel::move(ny);
            al->animZ = keel::move(nz);
        }
    }
    al->version = generation_ + 1;
    emitEvent(SceneEventKind::PropertyChanged, alId, kInvalidNodeId, "Points");
    recomputeAxesAutoLimits(al->parentId, res);
}

void GraphicsScene::animatedLineClearPoints(NodeId alId)
{
    GraphicsNode *al = findNode(alId);
    if (!al || al->classId != ClassId::AnimatedLine)
        throw Error("clearpoints: argument must be an AnimatedLine handle.",
                    0, 0, "clearpoints", "", "numkit:graphics:InvalidHandle");
    al->animX.clear();
    al->animY.clear();
    al->animZ.clear();
    al->animHasZ = false;
    al->version = generation_ + 1;
    emitEvent(SceneEventKind::PropertyChanged, alId, kInvalidNodeId, "Points");
    recomputeAxesAutoLimits(al->parentId, mr_);
}

void GraphicsScene::animatedLineGetPoints(NodeId alId, Value &xOut, Value &yOut, Value &zOut,
                                          bool wantZ, keel::Allocator *mr) const
{
    auto *res = mr ? mr : mr_;
    const GraphicsNode *al = findNode(alId);
    if (!al || al->classId != ClassId::AnimatedLine)
        throw Error("getpoints: argument must be an AnimatedLine handle.",
                    0, 0, "getpoints", "", "numkit:graphics:InvalidHandle");
    const keel::usize n = al->animX.size();
    xOut = Value::matrix(1, n, ValueType::DOUBLE, res);
    yOut = Value::matrix(1, n, ValueType::DOUBLE, res);
    for (keel::usize i = 0; i < n; ++i) {
        xOut.doubleDataMut()[i] = al->animX[i];
        yOut.doubleDataMut()[i] = al->animY[i];
    }
    if (wantZ) {
        zOut = Value::matrix(1, n, ValueType::DOUBLE, res);
        for (keel::usize i = 0; i < n; ++i)
            zOut.doubleDataMut()[i] = al->animZ[i];
    }
}

} // namespace numkit::gfx
