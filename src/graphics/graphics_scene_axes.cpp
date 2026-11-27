// src/graphics/graphics_scene_axes.cpp
//
// Phase 2 GraphicsScene operations: Axes, NumericRuler, PolarAxes,
// TiledChartLayout, Text, Legend, ColorBar, ConstantLine, nice auto-ticks,
// yyaxis, subplot, tiledlayout/nexttile, hold/cla, and linkaxes.

#include <keel/algo.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/graphics/graphics_scene.hpp>
#include <numkit/value/error.hpp>

namespace numkit::gfx {

namespace {

keel::f64 niceTickStep(keel::f64 span, keel::i32 targetTicks) noexcept
{
    if (!(span > 0.0) || !keel::is_finite(span))
        return 0.2;
    const keel::f64 rawStep = span / keel::as<keel::f64>(keel::max<keel::i32>(targetTicks, 2));
    const keel::f64 mag = keel::pow(10.0, keel::floor(keel::log10(rawStep)));
    const keel::f64 norm = rawStep / mag;
    keel::f64 nice = 1.0;
    if (norm <= 1.5)      nice = 1.0;
    else if (norm <= 3.0) nice = 2.0;
    else if (norm <= 7.0) nice = 5.0;
    else                  nice = 10.0;
    return nice * mag;
}

void buildNiceTicks(keel::f64 lo, keel::f64 hi, bool isLog,
                    Value &ticksOut, Value &labelsOut, keel::Allocator *mr)
{
    keel::Array<keel::f64> tv(mr);
    if (isLog && lo > 0.0 && hi > lo) {
        const keel::i32 p0 = keel::as<keel::i32>(keel::ceil(keel::log10(lo) - 1e-12));
        const keel::i32 p1 = keel::as<keel::i32>(keel::floor(keel::log10(hi) + 1e-12));
        for (keel::i32 p = p0; p <= p1 && tv.size() < 32; ++p)
            tv.push_back(keel::pow(10.0, keel::as<keel::f64>(p)));
    } else if (hi > lo && keel::is_finite(lo) && keel::is_finite(hi)) {
        const keel::f64 step = niceTickStep(hi - lo, 5);
        const keel::f64 start = keel::ceil(lo / step - 1e-9) * step;
        for (keel::f64 v = start; v <= hi + step * 1e-9 && tv.size() < 64; v += step) {
            const keel::f64 snapped = (keel::abs(v) < step * 1e-10) ? 0.0 : v;
            tv.push_back(snapped);
        }
    }
    ticksOut = Value::matrix(1, tv.size(), ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < tv.size(); ++i)
        ticksOut.doubleDataMut()[i] = tv[i];

    labelsOut = Value::cell(tv.size(), 1, mr);
    for (keel::usize i = 0; i < tv.size(); ++i)
        labelsOut.cellAt(i) = Value::fromString(keel::format("{:.6g}", tv[i]), mr);
}

Value makeVec4Local(keel::f64 a, keel::f64 b, keel::f64 c, keel::f64 d, keel::Allocator *mr)
{
    Value out = Value::matrix(1, 4, ValueType::DOUBLE, mr);
    out.doubleDataMut()[0] = a;
    out.doubleDataMut()[1] = b;
    out.doubleDataMut()[2] = c;
    out.doubleDataMut()[3] = d;
    return out;
}

Value makeVec3Local(keel::f64 a, keel::f64 b, keel::f64 c, keel::Allocator *mr)
{
    Value out = Value::matrix(1, 3, ValueType::DOUBLE, mr);
    out.doubleDataMut()[0] = a;
    out.doubleDataMut()[1] = b;
    out.doubleDataMut()[2] = c;
    return out;
}

} // anonymous namespace

NodeId GraphicsScene::containingFigure(NodeId id) const noexcept
{
    NodeId cur = id;
    while (cur != kInvalidNodeId) {
        const GraphicsNode *n = findNode(cur);
        if (!n)
            return kInvalidNodeId;
        if (n->classId == ClassId::Figure)
            return cur;
        if (n->ownerAxesId != kInvalidNodeId)
            cur = n->ownerAxesId;
        else
            cur = n->parentId;
    }
    return kInvalidNodeId;
}

NodeId GraphicsScene::createAxes(NodeId parentId)
{
    NodeId actualParent = parentId;
    if (actualParent == kInvalidNodeId)
        actualParent = ensureCurrentFigure();
    const GraphicsNode *pn = findNode(actualParent);
    if (!pn || (pn->classId != ClassId::Figure && pn->classId != ClassId::TiledChartLayout)) {
        throw Error("axes: Parent must be a Figure or TiledChartLayout.",
                    0, 0, "axes", "", "numkit:graphics:InvalidParent");
    }

    const NodeId axId = allocateNode(ClassId::Axes, actualParent);
    // Owned sub-objects (not in ax.Children — matching MATLAB).
    const NodeId xRuler = allocateNode(ClassId::NumericRuler, kInvalidNodeId);
    const NodeId yRuler = allocateNode(ClassId::NumericRuler, kInvalidNodeId);
    const NodeId zRuler = allocateNode(ClassId::NumericRuler, kInvalidNodeId);
    const NodeId titleId = allocateNode(ClassId::Text, kInvalidNodeId);
    const NodeId subId   = allocateNode(ClassId::Text, kInvalidNodeId);
    const NodeId xlId    = allocateNode(ClassId::Text, kInvalidNodeId);
    const NodeId ylId    = allocateNode(ClassId::Text, kInvalidNodeId);
    const NodeId zlId    = allocateNode(ClassId::Text, kInvalidNodeId);

    auto initRuler = [&](NodeId rid, char ch, NodeId lblId) {
        if (GraphicsNode *rn = findNode(rid)) {
            rn->ownerAxesId = axId;
            rn->rulerAxis = ch;
            rn->xLabelId = lblId; // store ruler's Label Text id here
        }
    };
    initRuler(xRuler, 'x', xlId);
    initRuler(yRuler, 'y', ylId);
    initRuler(zRuler, 'z', zlId);

    auto initLabel = [&](NodeId tid, keel::f64 mult, bool bold) {
        if (GraphicsNode *tn = findNode(tid)) {
            tn->ownerAxesId = axId;
            tn->parentId = axId;
            tn->props.put(keel::String::from("FontSize", mr_), Value::scalar(10.0 * mult, mr_));
            if (bold)
                tn->props.put(keel::String::from("FontWeight", mr_), Value::fromString("bold", mr_));
        }
    };
    initLabel(titleId, 1.1, true);
    initLabel(subId,   1.0, false);
    initLabel(xlId,    1.1, false);
    initLabel(ylId,    1.1, false);
    initLabel(zlId,    1.1, false);

    if (GraphicsNode *ax = findNode(axId)) {
        ax->xAxisId = xRuler;
        ax->yAxisId = yRuler;
        ax->zAxisId = zRuler;
        ax->titleId = titleId;
        ax->subtitleId = subId;
        ax->xLabelId = xlId;
        ax->yLabelId = ylId;
        ax->zLabelId = zlId;
    }

    setCurrentAxes(axId);
    return axId;
}

NodeId GraphicsScene::createPolarAxes(NodeId parentId)
{
    NodeId actualParent = parentId;
    if (actualParent == kInvalidNodeId)
        actualParent = ensureCurrentFigure();
    const GraphicsNode *pn = findNode(actualParent);
    if (!pn || (pn->classId != ClassId::Figure && pn->classId != ClassId::TiledChartLayout)) {
        throw Error("polaraxes: Parent must be a Figure or TiledChartLayout.",
                    0, 0, "polaraxes", "", "numkit:graphics:InvalidParent");
    }

    const NodeId paxId   = allocateNode(ClassId::PolarAxes, actualParent);
    const NodeId tRuler  = allocateNode(ClassId::NumericRuler, kInvalidNodeId);
    const NodeId rRuler  = allocateNode(ClassId::NumericRuler, kInvalidNodeId);
    const NodeId titleId = allocateNode(ClassId::Text, kInvalidNodeId);
    const NodeId subId   = allocateNode(ClassId::Text, kInvalidNodeId);
    if (GraphicsNode *rn = findNode(tRuler)) { rn->ownerAxesId = paxId; rn->rulerAxis = 't'; }
    if (GraphicsNode *rn = findNode(rRuler)) { rn->ownerAxesId = paxId; rn->rulerAxis = 'r'; }
    if (GraphicsNode *tn = findNode(titleId)) {
        tn->ownerAxesId = paxId;
        tn->parentId = paxId;
        tn->props.put(keel::String::from("FontSize", mr_), Value::scalar(11.0, mr_));
        tn->props.put(keel::String::from("FontWeight", mr_), Value::fromString("bold", mr_));
    }
    if (GraphicsNode *sn = findNode(subId)) {
        sn->ownerAxesId = paxId;
        sn->parentId = paxId;
    }
    if (GraphicsNode *pax = findNode(paxId)) {
        pax->xAxisId = tRuler; // ThetaAxis
        pax->yAxisId = rRuler; // RAxis
        pax->titleId = titleId;
        pax->subtitleId = subId;
    }
    setCurrentAxes(paxId);
    return paxId;
}

NodeId GraphicsScene::currentAxesId(NodeId figId) const noexcept
{
    const NodeId fid = (figId != kInvalidNodeId) ? figId : currentFigureId_;
    const GraphicsNode *fn = findNode(fid);
    if (!fn || fn->classId != ClassId::Figure)
        return kInvalidNodeId;
    if (findNode(fn->currentAxesId) != nullptr)
        return fn->currentAxesId;
    return kInvalidNodeId;
}

NodeId GraphicsScene::ensureCurrentAxes(NodeId figId)
{
    const NodeId fid = (figId != kInvalidNodeId) ? figId : ensureCurrentFigure();
    const NodeId existing = currentAxesId(fid);
    if (existing != kInvalidNodeId)
        return existing;
    return createAxes(fid);
}

void GraphicsScene::setCurrentAxes(NodeId axId)
{
    const GraphicsNode *ax = findNode(axId);
    if (!ax || (ax->classId != ClassId::Axes && ax->classId != ClassId::PolarAxes)) {
        throw Error("CurrentAxes must be a valid Axes or PolarAxes handle.",
                    0, 0, "set", "", "numkit:graphics:InvalidAxesHandle");
    }
    const NodeId figId = containingFigure(axId);
    if (GraphicsNode *fn = findNode(figId)) {
        if (fn->currentAxesId != axId) {
            fn->currentAxesId = axId;
            fn->version = generation_ + 1;
            emitEvent(SceneEventKind::CurrentChanged, figId, axId, "CurrentAxes");
        }
        setCurrentFigure(figId);
    }
}

void GraphicsScene::updateCurrentAxesAfterRemoval(NodeId figId)
{
    GraphicsNode *fn = findNode(figId);
    if (!fn || fn->classId != ClassId::Figure)
        return;
    if (findNode(fn->currentAxesId) != nullptr)
        return;
    NodeId nextAx = kInvalidNodeId;
    for (NodeId cid : fn->children) {
        const GraphicsNode *c = findNode(cid);
        if (!c) continue;
        if (c->classId == ClassId::Axes || c->classId == ClassId::PolarAxes) {
            nextAx = cid;
            break;
        }
        if (c->classId == ClassId::TiledChartLayout) {
            for (NodeId tcid : c->children) {
                const GraphicsNode *tc = findNode(tcid);
                if (tc && (tc->classId == ClassId::Axes || tc->classId == ClassId::PolarAxes)) {
                    nextAx = tcid;
                    break;
                }
            }
            if (nextAx != kInvalidNodeId) break;
        }
    }
    fn->currentAxesId = nextAx;
    emitEvent(SceneEventKind::CurrentChanged, figId, nextAx, "CurrentAxes");
}

void GraphicsScene::clearAxes(NodeId axId, bool resetProps)
{
    GraphicsNode *ax = findNode(axId);
    if (!ax || (ax->classId != ClassId::Axes && ax->classId != ClassId::PolarAxes))
        return;
    keel::Array<NodeId> childCopy = ax->children.clone(mr_);
    for (NodeId cid : childCopy)
        deleteNode(cid, /*force=*/true);
    if (GraphicsNode *a2 = findNode(axId)) {
        a2->props.put(keel::String::from("ColorOrderIndex", mr_), Value::scalar(1.0, mr_));
        a2->props.put(keel::String::from("LineStyleOrderIndex", mr_), Value::scalar(1.0, mr_));
        a2->props.put(keel::String::from("NextSeriesIndex", mr_), Value::scalar(1.0, mr_));
        a2->version = generation_ + 1;
    }
    if (resetProps) {
        resetNode(axId);
        // Also clear label strings on reset.
        if (const GraphicsNode *a3 = findNode(axId)) {
            for (NodeId lid : {a3->titleId, a3->subtitleId, a3->xLabelId, a3->yLabelId, a3->zLabelId}) {
                if (GraphicsNode *ln = findNode(lid))
                    ln->props.remove("String");
            }
        }
    }
}

void GraphicsScene::setHoldMode(NodeId axId, keel::StringRef mode)
{
    const GraphicsNode *ax = findNode(axId);
    if (!ax || (ax->classId != ClassId::Axes && ax->classId != ClassId::PolarAxes))
        return;
    const bool currentlyOn = isHoldOn(axId);
    keel::String nextPlot;
    if (asciiEqualNoCase(mode, "on") || asciiEqualNoCase(mode, "all")) {
        nextPlot = keel::String::from("add", mr_);
    } else if (asciiEqualNoCase(mode, "off")) {
        nextPlot = keel::String::from("replace", mr_);
    } else {
        // toggle
        nextPlot = keel::String::from(currentlyOn ? "replace" : "add", mr_);
    }
    setProperty(axId, "NextPlot", Value::fromString(nextPlot, mr_), mr_);
}

bool GraphicsScene::isHoldOn(NodeId axId) const noexcept
{
    const GraphicsNode *ax = findNode(axId);
    if (!ax || (ax->classId != ClassId::Axes && ax->classId != ClassId::PolarAxes))
        return false;
    const Value np = getProperty(axId, "NextPlot", mr_);
    const keel::String s = np.toString();
    return asciiEqualNoCase(s, "add") || asciiEqualNoCase(s, "replacechildren");
}

void GraphicsScene::setYYAxisSide(NodeId axId, bool rightSide)
{
    GraphicsNode *ax = findNode(axId);
    if (!ax || ax->classId != ClassId::Axes)
        return;
    if (ax->yAxisRightId == kInvalidNodeId) {
        const NodeId rRuler = allocateNode(ClassId::NumericRuler, kInvalidNodeId);
        const NodeId rLbl   = allocateNode(ClassId::Text, kInvalidNodeId);
        if (GraphicsNode *rn = findNode(rRuler)) {
            rn->ownerAxesId = axId;
            rn->rulerAxis = 'Y';
            rn->xLabelId = rLbl;
        }
        if (GraphicsNode *ln = findNode(rLbl)) {
            ln->ownerAxesId = axId;
            ln->parentId = axId;
            ln->props.put(keel::String::from("FontSize", mr_), Value::scalar(11.0, mr_));
        }
        if (GraphicsNode *ax2 = findNode(axId)) {
            ax2->yAxisRightId = rRuler;
            ax2->yLabelRightId = rLbl;
        }
    }
    if (GraphicsNode *ax3 = findNode(axId)) {
        ax3->yyRightActive = rightSide;
        ax3->version = generation_ + 1;
        emitEvent(SceneEventKind::PropertyChanged, axId, kInvalidNodeId, "YAxis");
    }
}

NodeId GraphicsScene::createTiledLayout(NodeId figId, keel::i32 rows, keel::i32 cols)
{
    const NodeId fid = (figId != kInvalidNodeId) ? figId : ensureCurrentFigure();
    clearFigure(fid, false);
    const NodeId tlId = allocateNode(ClassId::TiledChartLayout, fid);
    const NodeId tId  = allocateNode(ClassId::Text, kInvalidNodeId);
    const NodeId sId  = allocateNode(ClassId::Text, kInvalidNodeId);
    const NodeId xlId = allocateNode(ClassId::Text, kInvalidNodeId);
    const NodeId ylId = allocateNode(ClassId::Text, kInvalidNodeId);

    for (NodeId lid : {tId, sId, xlId, ylId}) {
        if (GraphicsNode *ln = findNode(lid)) {
            ln->ownerAxesId = tlId;
            ln->parentId = tlId;
        }
    }
    if (GraphicsNode *tl = findNode(tlId)) {
        tl->titleId = tId;
        tl->subtitleId = sId;
        tl->xLabelId = xlId;
        tl->yLabelId = ylId;
        tl->nextTileIdx = 1;
        Value gs = Value::matrix(1, 2, ValueType::DOUBLE, mr_);
        gs.doubleDataMut()[0] = keel::as<keel::f64>(keel::max<keel::i32>(rows, 1));
        gs.doubleDataMut()[1] = keel::as<keel::f64>(keel::max<keel::i32>(cols, 1));
        tl->props.put(keel::String::from("GridSize", mr_), keel::move(gs));
    }
    return tlId;
}

NodeId GraphicsScene::nextTile(NodeId tlId, keel::i32 tileNum, keel::i32 rowSpan, keel::i32 colSpan)
{
    GraphicsNode *tl = findNode(tlId);
    if (!tl || tl->classId != ClassId::TiledChartLayout) {
        throw Error("nexttile: requires a valid TiledChartLayout.",
                    0, 0, "nexttile", "", "numkit:graphics:InvalidTiledLayout");
    }
    const keel::i32 chosen = (tileNum > 0) ? tileNum : tl->nextTileIdx;
    tl->nextTileIdx = chosen + 1;

    const Value gs = getProperty(tlId, "GridSize", mr_);
    const keel::i32 rows = keel::max<keel::i32>(1, keel::as<keel::i32>(gs.elemAsDouble(0)));
    const keel::i32 cols = keel::max<keel::i32>(1, keel::as<keel::i32>(gs.elemAsDouble(1)));
    const keel::i32 r0 = (chosen - 1) / cols;
    const keel::i32 c0 = (chosen - 1) % cols;

    const NodeId axId = createAxes(tlId);
    if (GraphicsNode *ax = findNode(axId)) {
        const keel::f64 cellW = 0.84 / keel::as<keel::f64>(cols);
        const keel::f64 cellH = 0.84 / keel::as<keel::f64>(rows);
        const keel::f64 x = 0.08 + keel::as<keel::f64>(c0) * cellW;
        const keel::f64 y = 0.92 - keel::as<keel::f64>(r0 + rowSpan) * cellH;
        const Value pos = makeVec4Local(x, keel::max(0.05, y), cellW * colSpan * 0.9, cellH * rowSpan * 0.88, mr_);
        ax->props.put(keel::String::from("Position", mr_), pos);
        ax->props.put(keel::String::from("InnerPosition", mr_), pos);
        ax->props.put(keel::String::from("OuterPosition", mr_), pos);
        Value lo = Value::structure(mr_);
        lo.setField(0, "Tile", Value::scalar(keel::as<keel::f64>(chosen), mr_));
        Value sp = Value::matrix(1, 2, ValueType::DOUBLE, mr_);
        sp.doubleDataMut()[0] = keel::as<keel::f64>(rowSpan);
        sp.doubleDataMut()[1] = keel::as<keel::f64>(colSpan);
        lo.setField(0, "TileSpan", keel::move(sp));
        ax->props.put(keel::String::from("Layout", mr_), keel::move(lo));
    }
    return axId;
}

NodeId GraphicsScene::createOrActivateSubplot(NodeId figId, keel::i32 m, keel::i32 n, keel::i32 p)
{
    if (m < 1 || n < 1 || p < 1 || p > m * n) {
        throw Error("subplot: index out of range for m-by-n grid.",
                    0, 0, "subplot", "", "numkit:graphics:InvalidSubplot");
    }
    const NodeId fid = (figId != kInvalidNodeId) ? figId : ensureCurrentFigure();
    // Reuse existing axes at exact (m, n, p) slot if present.
    if (const GraphicsNode *fn = findNode(fid)) {
        for (NodeId cid : fn->children) {
            const GraphicsNode *c = findNode(cid);
            if (c && (c->classId == ClassId::Axes || c->classId == ClassId::PolarAxes)
                && c->subplotRows == m && c->subplotCols == n && c->subplotIdx == p) {
                setCurrentAxes(cid);
                return cid;
            }
        }
    }

    const NodeId axId = createAxes(fid);
    if (GraphicsNode *ax = findNode(axId)) {
        ax->subplotRows = m;
        ax->subplotCols = n;
        ax->subplotIdx = p;
        const keel::i32 row = (p - 1) / n;
        const keel::i32 col = (p - 1) % n;
        const keel::f64 w = 0.775 / keel::as<keel::f64>(n);
        const keel::f64 h = 0.815 / keel::as<keel::f64>(m);
        const keel::f64 left = 0.13 + keel::as<keel::f64>(col) * (0.86 / keel::as<keel::f64>(n));
        const keel::f64 bottom = 0.925 - keel::as<keel::f64>(row + 1) * (0.86 / keel::as<keel::f64>(m));
        const Value pos = makeVec4Local(left, keel::max(0.05, bottom), w * 0.92, h * 0.88, mr_);
        ax->props.put(keel::String::from("Position", mr_), pos);
        ax->props.put(keel::String::from("InnerPosition", mr_), pos);
        ax->props.put(keel::String::from("OuterPosition", mr_), pos);
    }
    return axId;
}

NodeId GraphicsScene::createText(NodeId parentId, keel::f64 x, keel::f64 y, keel::f64 z, keel::StringRef str)
{
    const NodeId pid = (parentId != kInvalidNodeId) ? parentId : ensureCurrentAxes();
    const NodeId tid = allocateNode(ClassId::Text, pid);
    if (GraphicsNode *tn = findNode(tid)) {
        tn->props.put(keel::String::from("Position", mr_), makeVec3Local(x, y, z, mr_));
        tn->props.put(keel::String::from("String", mr_), Value::fromString(str, mr_));
    }
    return tid;
}

NodeId GraphicsScene::setLabelText(NodeId ownerId, keel::StringRef slot, keel::StringRef str)
{
    const GraphicsNode *ow = findNode(ownerId);
    if (!ow)
        return kInvalidNodeId;
    NodeId targetId = kInvalidNodeId;
    if (ow->classId == ClassId::HeatmapChart) {
        if (asciiEqualNoCase(slot, "title"))       setProperty(ownerId, "Title", Value::fromString(str, mr_), mr_);
        else if (asciiEqualNoCase(slot, "xlabel")) setProperty(ownerId, "XLabel", Value::fromString(str, mr_), mr_);
        else if (asciiEqualNoCase(slot, "ylabel")) setProperty(ownerId, "YLabel", Value::fromString(str, mr_), mr_);
        return ownerId;
    }
    if (ow->classId == ClassId::ColorBar) {
        if (asciiEqualNoCase(slot, "ylabel") || asciiEqualNoCase(slot, "xlabel")
            || asciiEqualNoCase(slot, "title")) {
            targetId = ow->xLabelId;
        }
    } else if (asciiEqualNoCase(slot, "title"))    targetId = ow->titleId;
    else if (asciiEqualNoCase(slot, "subtitle")) targetId = ow->subtitleId;
    else if (asciiEqualNoCase(slot, "xlabel"))   targetId = ow->xLabelId;
    else if (asciiEqualNoCase(slot, "ylabel"))   targetId = (ow->yyRightActive && ow->yLabelRightId != kInvalidNodeId) ? ow->yLabelRightId : ow->yLabelId;
    else if (asciiEqualNoCase(slot, "zlabel"))   targetId = ow->zLabelId;
    if (targetId == kInvalidNodeId)
        return kInvalidNodeId;
    setProperty(targetId, "String", Value::fromString(str, mr_), mr_);
    return targetId;
}

NodeId GraphicsScene::createOrUpdateLegend(NodeId axId, keel::ArrayRef<keel::String> labels)
{
    GraphicsNode *ax = findNode(axId);
    if (!ax || (ax->classId != ClassId::Axes && ax->classId != ClassId::PolarAxes))
        throw Error("legend: requires a valid Axes or PolarAxes.", 0, 0, "legend", "", "numkit:graphics:InvalidAxesHandle");

    const NodeId figId = containingFigure(axId);
    NodeId lgId = ax->legendId;
    if (findNode(lgId) == nullptr) {
        lgId = allocateNode(ClassId::Legend, figId);
        const NodeId tId = allocateNode(ClassId::Text, kInvalidNodeId);
        if (GraphicsNode *tn = findNode(tId)) {
            tn->ownerAxesId = lgId;
            tn->parentId = lgId;
        }
        if (GraphicsNode *ln = findNode(lgId)) {
            ln->ownerAxesId = axId;
            ln->titleId = tId;
        }
        if (GraphicsNode *ax2 = findNode(axId))
            ax2->legendId = lgId;
    }
    if (!labels.empty()) {
        Value c = Value::cell(1, labels.size(), mr_);
        for (keel::usize i = 0; i < labels.size(); ++i)
            c.cellAt(i) = Value::fromString(labels[i], mr_);
        setProperty(lgId, "String", keel::move(c), mr_);
        // Mirror legend labels onto the axes's chart children's DisplayName in creation order.
        if (const GraphicsNode *axCur = findNode(axId)) {
            keel::usize li = 0;
            for (keel::usize ci = axCur->children.size(); ci > 0 && li < labels.size(); --ci) {
                const NodeId cid = axCur->children[ci - 1];
                const GraphicsNode *ch = findNode(cid);
                if (!ch || ch->classId == ClassId::Text || ch->classId == ClassId::Image)
                    continue;
                if (hasProperty(cid, "DisplayName")) {
                    setProperty(cid, "DisplayName", Value::fromString(labels[li], mr_), mr_);
                    ++li;
                }
            }
        }
    }
    return lgId;
}

NodeId GraphicsScene::createOrUpdateColorBar(NodeId axId)
{
    GraphicsNode *ax = findNode(axId);
    if (!ax || (ax->classId != ClassId::Axes && ax->classId != ClassId::PolarAxes))
        throw Error("colorbar: requires a valid Axes or PolarAxes.", 0, 0, "colorbar", "", "numkit:graphics:InvalidAxesHandle");

    const NodeId figId = containingFigure(axId);
    NodeId cbId = ax->colorBarId;
    if (findNode(cbId) == nullptr) {
        cbId = allocateNode(ClassId::ColorBar, figId);
        const NodeId lblId = allocateNode(ClassId::Text, kInvalidNodeId);
        if (GraphicsNode *ln = findNode(lblId)) {
            ln->ownerAxesId = cbId;
            ln->parentId = cbId;
        }
        if (GraphicsNode *cb = findNode(cbId)) {
            cb->ownerAxesId = axId;
            cb->xLabelId = lblId;
        }
        if (GraphicsNode *ax2 = findNode(axId))
            ax2->colorBarId = cbId;
    }
    const Value clim = getProperty(axId, "CLim", mr_);
    if (clim.numel() == 2) {
        if (GraphicsNode *cb = findNode(cbId))
            cb->props.put(keel::String::from("Limits", mr_), clim);
    }
    return cbId;
}

NodeId GraphicsScene::createConstantLine(NodeId axId, char axisChar, keel::f64 value, keel::StringRef label)
{
    const NodeId targetAx = (axId != kInvalidNodeId) ? axId : ensureCurrentAxes();
    const NodeId clId = allocateNode(ClassId::ConstantLine, targetAx);
    if (GraphicsNode *cl = findNode(clId)) {
        const char axStr[2] = {(axisChar == 'y' || axisChar == 'Y') ? 'y' : 'x', '\0'};
        cl->props.put(keel::String::from("InterceptAxis", mr_), Value::fromString(axStr, mr_));
        cl->props.put(keel::String::from("Value", mr_), Value::scalar(value, mr_));
        if (!label.empty())
            cl->props.put(keel::String::from("Label", mr_), Value::fromString(label, mr_));
    }
    return clId;
}

void GraphicsScene::linkAxes(keel::ArrayRef<NodeId> axIds, keel::StringRef mode)
{
    const bool lx = asciiEqualNoCase(mode, "x") || asciiEqualNoCase(mode, "xy");
    const bool ly = asciiEqualNoCase(mode, "y") || asciiEqualNoCase(mode, "xy");
    const bool isOff = asciiEqualNoCase(mode, "off");
    if (!lx && !ly && !isOff) {
        throw Error("linkaxes: option must be 'x', 'y', 'xy', or 'off'.",
                    0, 0, "linkaxes", "", "numkit:graphics:InvalidOption");
    }

    // Remove these axes from any existing link groups.
    for (auto &grp : linkGroups_) {
        for (keel::usize i = grp.axes.size(); i > 0; --i) {
            const NodeId id = grp.axes[i - 1];
            bool match = false;
            for (NodeId q : axIds) {
                if (q == id) { match = true; break; }
            }
            if (match)
                grp.axes.erase(i - 1);
        }
    }

    if (isOff || axIds.size() < 2)
        return;

    AxisLinkGroup grp;
    for (NodeId id : axIds) {
        if (const GraphicsNode *n = findNode(id); n && n->classId == ClassId::Axes)
            grp.axes.push_back(id);
    }
    if (grp.axes.size() < 2)
        return;
    grp.linkX = lx;
    grp.linkY = ly;

    // Synchronize initial limits to the union across linked axes (MATLAB linkaxes behavior).
    auto syncUnion = [&](bool isX) {
        keel::f64 lo = keel::Limits<keel::f64>::infinity();
        keel::f64 hi = -keel::Limits<keel::f64>::infinity();
        const char *prop = isX ? "XLim" : "YLim";
        for (NodeId id : grp.axes) {
            const Value lv = getProperty(id, prop, mr_);
            if (lv.numel() >= 2) {
                lo = keel::min(lo, lv.elemAsDouble(0));
                hi = keel::max(hi, lv.elemAsDouble(1));
            }
        }
        if (keel::is_finite(lo) && keel::is_finite(hi) && hi > lo) {
            Value lim = Value::matrix(1, 2, ValueType::DOUBLE, mr_);
            lim.doubleDataMut()[0] = lo;
            lim.doubleDataMut()[1] = hi;
            inSyncGuard_ = true;
            for (NodeId id : grp.axes)
                setProperty(id, prop, lim, mr_);
            inSyncGuard_ = false;
        }
    };
    if (lx) syncUnion(true);
    if (ly) syncUnion(false);

    linkGroups_.push_back(keel::move(grp));
}

void GraphicsScene::recomputeAutoTicksForAxis(NodeId axId, char axisChar, keel::Allocator *mr)
{
    auto *res = mr ? mr : mr_;
    GraphicsNode *ax = findNode(axId);
    if (!ax || ax->classId != ClassId::Axes)
        return;

    const char *limProp = (axisChar == 'x') ? "XLim" : (axisChar == 'y' ? "YLim" : "ZLim");
    const char *sclProp = (axisChar == 'x') ? "XScale" : (axisChar == 'y' ? "YScale" : "ZScale");
    const char *tkProp  = (axisChar == 'x') ? "XTick" : (axisChar == 'y' ? "YTick" : "ZTick");
    const char *tkmProp = (axisChar == 'x') ? "XTickMode" : (axisChar == 'y' ? "YTickMode" : "ZTickMode");
    const char *tlProp  = (axisChar == 'x') ? "XTickLabel" : (axisChar == 'y' ? "YTickLabel" : "ZTickLabel");
    const char *tlmProp = (axisChar == 'x') ? "XTickLabelMode" : (axisChar == 'y' ? "YTickLabelMode" : "ZTickLabelMode");

    const Value tkm = getProperty(axId, tkmProp, res);
    if (!asciiEqualNoCase(tkm.toString(), "auto"))
        return;

    const Value lim = getProperty(axId, limProp, res);
    if (lim.numel() < 2)
        return;
    const Value scl = getProperty(axId, sclProp, res);
    const bool isLog = asciiEqualNoCase(scl.toString(), "log");

    Value ticks, labels;
    buildNiceTicks(lim.elemAsDouble(0), lim.elemAsDouble(1), isLog, ticks, labels, res);

    const bool routeRight = (axisChar == 'y' && ax->yyRightActive && ax->yAxisRightId != kInvalidNodeId);
    const Value tlm = getProperty(axId, tlmProp, res);
    const bool labelsAuto = asciiEqualNoCase(tlm.toString(), "auto");
    if (!routeRight) {
        ax->props.put(keel::String::from(tkProp, mr_), ticks);
        if (labelsAuto)
            ax->props.put(keel::String::from(tlProp, mr_), labels);
    }

    // Sync ticks to owned ruler
    const NodeId rid = (axisChar == 'x') ? ax->xAxisId
                     : (axisChar == 'y' ? (routeRight ? ax->yAxisRightId : ax->yAxisId)
                                        : ax->zAxisId);
    if (GraphicsNode *rn = findNode(rid)) {
        rn->props.put(keel::String::from("TickValues", mr_), ticks);
        if (labelsAuto)
            rn->props.put(keel::String::from("TickLabels", mr_), labels);
    }
}

void GraphicsScene::syncAxesToRuler(NodeId axId, char axisChar, keel::StringRef axProp,
                                    const Value &val, keel::Allocator *mr)
{
    if (inSyncGuard_) return;
    const GraphicsNode *ax = findNode(axId);
    if (!ax || ax->classId != ClassId::Axes)
        return;
    const NodeId rid = (axisChar == 'x') ? ax->xAxisId
                     : (axisChar == 'y' ? (ax->yyRightActive && ax->yAxisRightId != kInvalidNodeId ? ax->yAxisRightId : ax->yAxisId)
                                        : ax->zAxisId);
    GraphicsNode *rn = findNode(rid);
    if (!rn)
        return;

    const char *rProp = nullptr;
    const char *rMode = nullptr;
    if (axProp == "XLim" || axProp == "YLim" || axProp == "ZLim") { rProp = "Limits"; rMode = "LimitsMode"; }
    else if (axProp == "XLimMode" || axProp == "YLimMode" || axProp == "ZLimMode") { rProp = "LimitsMode"; }
    else if (axProp == "XScale" || axProp == "YScale" || axProp == "ZScale") { rProp = "Scale"; }
    else if (axProp == "XDir" || axProp == "YDir" || axProp == "ZDir") { rProp = "Direction"; }
    else if (axProp == "XColor" || axProp == "YColor" || axProp == "ZColor") { rProp = "Color"; rMode = "ColorMode"; }
    else if (axProp == "XTick" || axProp == "YTick" || axProp == "ZTick") { rProp = "TickValues"; rMode = "TickValuesMode"; }
    else if (axProp == "XTickMode" || axProp == "YTickMode" || axProp == "ZTickMode") { rProp = "TickValuesMode"; }
    else if (axProp == "XTickLabel" || axProp == "YTickLabel" || axProp == "ZTickLabel") { rProp = "TickLabels"; rMode = "TickLabelsMode"; }
    else if (axProp == "XTickLabelMode" || axProp == "YTickLabelMode" || axProp == "ZTickLabelMode") { rProp = "TickLabelsMode"; }
    else if (axProp == "XTickLabelRotation" || axProp == "YTickLabelRotation" || axProp == "ZTickLabelRotation") { rProp = "TickLabelRotation"; rMode = "TickLabelRotationMode"; }
    else if (axProp == "XMinorTick" || axProp == "YMinorTick" || axProp == "ZMinorTick") { rProp = "MinorTick"; }

    if (rProp) {
        rn->props.put(keel::String::from(rProp, mr_), val);
        if (rMode)
            rn->props.put(keel::String::from(rMode, mr_), Value::fromString("manual", mr ? mr : mr_));
        rn->version = generation_ + 1;
    }
}

void GraphicsScene::syncRulerToAxes(NodeId rulerId, keel::StringRef rulerProp,
                                    const Value &val, keel::Allocator *mr)
{
    if (inSyncGuard_) return;
    const GraphicsNode *rn = findNode(rulerId);
    if (!rn || rn->classId != ClassId::NumericRuler || rn->ownerAxesId == kInvalidNodeId)
        return;
    const NodeId axId = rn->ownerAxesId;
    GraphicsNode *ax = findNode(axId);
    if (!ax || ax->classId != ClassId::Axes)
        return;
    // Only push back to Axes scalar properties if this ruler is the active side.
    if (rn->rulerAxis == 'y' && ax->yyRightActive) return;
    if (rn->rulerAxis == 'Y' && !ax->yyRightActive) return;

    const char pfx = (rn->rulerAxis == 'x') ? 'X' : ((rn->rulerAxis == 'z') ? 'Z' : 'Y');
    keel::String axProp;
    keel::String axMode;
    if (rulerProp == "Limits")               { axProp.push_back(pfx); axProp.append("Lim"); axMode.push_back(pfx); axMode.append("LimMode"); }
    else if (rulerProp == "LimitsMode")      { axProp.push_back(pfx); axProp.append("LimMode"); }
    else if (rulerProp == "Scale")           { axProp.push_back(pfx); axProp.append("Scale"); }
    else if (rulerProp == "Direction")       { axProp.push_back(pfx); axProp.append("Dir"); }
    else if (rulerProp == "Color")           { axProp.push_back(pfx); axProp.append("Color"); axMode.push_back(pfx); axMode.append("ColorMode"); }
    else if (rulerProp == "TickValues")      { axProp.push_back(pfx); axProp.append("Tick"); axMode.push_back(pfx); axMode.append("TickMode"); }
    else if (rulerProp == "TickValuesMode")  { axProp.push_back(pfx); axProp.append("TickMode"); }
    else if (rulerProp == "TickLabels")      { axProp.push_back(pfx); axProp.append("TickLabel"); axMode.push_back(pfx); axMode.append("TickLabelMode"); }
    else if (rulerProp == "TickLabelsMode")  { axProp.push_back(pfx); axProp.append("TickLabelMode"); }
    else if (rulerProp == "TickLabelRotation") { axProp.push_back(pfx); axProp.append("TickLabelRotation"); axMode.push_back(pfx); axMode.append("TickLabelRotationMode"); }
    else if (rulerProp == "MinorTick")       { axProp.push_back(pfx); axProp.append("MinorTick"); }
    else return;

    ax->props.put(keel::String::from(axProp, mr_), val);
    if (!axMode.empty())
        ax->props.put(keel::String::from(axMode, mr_), Value::fromString("manual", mr ? mr : mr_));
    ax->version = generation_ + 1;

    if (rulerProp == "Limits" || rulerProp == "Scale")
        recomputeAutoTicksForAxis(axId, (pfx == 'X') ? 'x' : (pfx == 'Z' ? 'z' : 'y'), mr);
    if (rulerProp == "Limits" && (pfx == 'X' || pfx == 'Y'))
        propagateLinkedLimits(axId, pfx == 'X', val, mr);
}

void GraphicsScene::propagateLinkedLimits(NodeId srcAxId, bool isX, const Value &limVal, keel::Allocator *mr)
{
    if (inSyncGuard_) return;
    const char *prop = isX ? "XLim" : "YLim";
    inSyncGuard_ = true;
    for (const auto &grp : linkGroups_) {
        if ((isX && !grp.linkX) || (!isX && !grp.linkY))
            continue;
        bool inGroup = false;
        for (NodeId id : grp.axes) {
            if (id == srcAxId) { inGroup = true; break; }
        }
        if (!inGroup) continue;
        for (NodeId id : grp.axes) {
            if (id == srcAxId || findNode(id) == nullptr) continue;
            if (GraphicsNode *peer = findNode(id)) {
                peer->props.put(keel::String::from(prop, mr_), limVal);
                peer->props.put(keel::String::from(isX ? "XLimMode" : "YLimMode", mr_),
                                Value::fromString("manual", mr ? mr : mr_));
                peer->version = generation_ + 1;
                recomputeAutoTicksForAxis(id, isX ? 'x' : 'y', mr);
                emitEvent(SceneEventKind::PropertyChanged, id, kInvalidNodeId, prop);
            }
        }
    }
    inSyncGuard_ = false;
}

} // namespace numkit::gfx
