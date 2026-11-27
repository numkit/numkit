// src/builtin/register/graphics/scene_axes_builtins.cpp
//
// Phase 2 Model-First Handle Graphics builtins:
//   axes, gca, polaraxes, tiledlayout, nexttile, subplot, cla, hold, ishold,
//   yyaxis, linkaxes, title, subtitle, xlabel, ylabel, zlabel, text, legend,
//   colorbar, xline, yline, xlim, ylim, zlim, clim, caxis, xticks, yticks,
//   zticks, xticklabels, yticklabels, zticklabels, xscale, yscale, zscale,
//   grid, box, view, colormap, rlim, thetalim, rticks, thetaticks, axis.

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/core/engine.hpp>
#include <numkit/graphics/graphics_handle.hpp>
#include <numkit/graphics/graphics_scene.hpp>
#include <numkit/graphics/property_schema.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::gfx::detail {

namespace {

inline bool extractSceneHandle(const Value &v, NodeId &idOut) noexcept
{
    keel::i32 id = -1;
    bool inScene = false;
    if (!graphicsHandleAt(v, 0, id, inScene) || !inScene)
        return false;
    idOut = id;
    return true;
}

inline bool isAxesLike(const GraphicsScene &scene, NodeId id) noexcept
{
    const GraphicsNode *n = scene.findNode(id);
    return n && (n->classId == ClassId::Axes || n->classId == ClassId::PolarAxes);
}

inline bool isLabelOwner(const GraphicsScene &scene, NodeId id) noexcept
{
    const GraphicsNode *n = scene.findNode(id);
    return n && (n->classId == ClassId::Axes || n->classId == ClassId::PolarAxes
                 || n->classId == ClassId::TiledChartLayout || n->classId == ClassId::ColorBar
                 || n->classId == ClassId::HeatmapChart || n->classId == ClassId::Legend);
}

void applyNV(GraphicsScene &scene, NodeId id,
             keel::ArrayRef<Value> args, keel::usize startIdx,
             keel::Allocator *mr)
{
    for (keel::usize i = startIdx; i < args.size();) {
        if (args[i].isStruct()) {
            const auto fields = args[i].fieldNamesInOrder(mr);
            for (const auto &fn : fields)
                scene.setProperty(id, fn, args[i].field(fn), mr);
            ++i;
            continue;
        }
        if (i + 1 >= args.size()) {
            throw Error("Graphics property Name-Value arguments must appear in pairs.",
                        0, 0, "set", "", "numkit:graphics:InvalidNameValue");
        }
        if (!args[i].isChar() && !args[i].isString()) {
            throw Error("Property name must be a character vector or string.",
                        0, 0, "set", "", "numkit:graphics:InvalidPropName");
        }
        scene.setProperty(id, args[i].toString(), args[i + 1], mr);
        i += 2;
    }
}

// Find Parent NV override if specified in args[startIdx..].
NodeId findParentOverride(keel::ArrayRef<Value> args, keel::usize startIdx) noexcept
{
    for (keel::usize i = startIdx; i + 1 < args.size(); i += 2) {
        if ((args[i].isChar() || args[i].isString())
            && asciiEqualNoCase(args[i].toString(), "Parent")) {
            NodeId pid = kInvalidNodeId;
            if (extractSceneHandle(args[i + 1], pid))
                return pid;
        }
    }
    return kInvalidNodeId;
}

void applyNVSkippingParent(GraphicsScene &scene, NodeId id,
                           keel::ArrayRef<Value> args, keel::usize startIdx,
                           keel::Allocator *mr)
{
    for (keel::usize i = startIdx; i < args.size();) {
        if (args[i].isStruct()) {
            const auto fields = args[i].fieldNamesInOrder(mr);
            for (const auto &fn : fields) {
                if (!asciiEqualNoCase(fn, "Parent"))
                    scene.setProperty(id, fn, args[i].field(fn), mr);
            }
            ++i;
            continue;
        }
        if (i + 1 >= args.size()) {
            throw Error("Graphics property Name-Value arguments must appear in pairs.",
                        0, 0, "set", "", "numkit:graphics:InvalidNameValue");
        }
        if (!args[i].isChar() && !args[i].isString()) {
            throw Error("Property name must be a character vector or string.",
                        0, 0, "set", "", "numkit:graphics:InvalidPropName");
        }
        if (!asciiEqualNoCase(args[i].toString(), "Parent"))
            scene.setProperty(id, args[i].toString(), args[i + 1], mr);
        i += 2;
    }
}

keel::String joinTextArg(const Value &v, keel::Allocator *mr)
{
    if (v.isChar() || v.isString())
        return v.toString();
    if (v.isCell()) {
        keel::String out(mr);
        for (keel::usize i = 0; i < v.numel(); ++i) {
            if (i > 0) out.push_back('\n');
            out.append(v.cellAt(i).toString());
        }
        return out;
    }
    if (v.isNumeric() && v.numel() == 1) {
        char buf[40];
        return keel::String::from(keel::format_f64_general_to(buf, v.elemAsDouble(0), 6), mr);
    }
    return keel::String(mr);
}

// Resolve optional leading target Axes (returns target axId and first remaining arg index).
NodeId resolveLeadingAxes(GraphicsScene &scene, keel::ArrayRef<Value> args, keel::usize &nextIdx)
{
    nextIdx = 0;
    if (!args.empty()) {
        NodeId id = kInvalidNodeId;
        if (extractSceneHandle(args[0], id) && isAxesLike(scene, id)) {
            nextIdx = 1;
            return id;
        }
    }
    return scene.ensureCurrentAxes();
}

// ── Axes / Layout builtins ──────────────────────────────────────────────

void axes_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId axId = kInvalidNodeId;
    keel::usize nvStart = 0;
    if (!args.empty()) {
        NodeId existing = kInvalidNodeId;
        if (extractSceneHandle(args[0], existing)) {
            const GraphicsNode *n = scene.findNode(existing);
            if (!n)
                throw Error("axes: invalid or deleted graphics handle.", 0, 0, "axes", "", "numkit:graphics:InvalidHandle");
            if (n->classId == ClassId::Axes || n->classId == ClassId::PolarAxes) {
                scene.setCurrentAxes(existing);
                axId = existing;
                nvStart = 1;
            } else if (n->classId == ClassId::Figure || n->classId == ClassId::TiledChartLayout) {
                axId = scene.createAxes(existing);
                nvStart = 1;
            } else {
                throw Error("axes: handle must be an Axes, PolarAxes, Figure, or TiledChartLayout.",
                            0, 0, "axes", "", "numkit:graphics:InvalidParent");
            }
        }
    }
    if (axId == kInvalidNodeId) {
        const NodeId parentOv = findParentOverride(args, 0);
        axId = scene.createAxes(parentOv);
        applyNVSkippingParent(scene, axId, args, 0, mr);
    } else if (nvStart < args.size()) {
        applyNV(scene, axId, args, nvStart, mr);
    }

    outs[0] = (nargout >= 1) ? scene.makeHandleValue(axId, mr) : Value();
}

void gca_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 1, "gca");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    NodeId figId = kInvalidNodeId;
    if (args.size() == 1) {
        if (!extractSceneHandle(args[0], figId))
            throw Error("gca: argument must be a valid Figure handle.", 0, 0, "gca", "", "numkit:graphics:InvalidFigureHandle");
    }
    const NodeId axId = scene.ensureCurrentAxes(figId);
    outs[0] = scene.makeHandleValue(axId, mr);
}

void polaraxes_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId paxId = kInvalidNodeId;
    keel::usize nvStart = 0;
    if (!args.empty()) {
        NodeId existing = kInvalidNodeId;
        if (extractSceneHandle(args[0], existing)) {
            const GraphicsNode *n = scene.findNode(existing);
            if (!n)
                throw Error("polaraxes: invalid or deleted graphics handle.", 0, 0, "polaraxes", "", "numkit:graphics:InvalidHandle");
            if (n->classId == ClassId::PolarAxes) {
                scene.setCurrentAxes(existing);
                paxId = existing;
                nvStart = 1;
            } else if (n->classId == ClassId::Figure || n->classId == ClassId::TiledChartLayout) {
                paxId = scene.createPolarAxes(existing);
                nvStart = 1;
            }
        }
    }
    if (paxId == kInvalidNodeId) {
        const NodeId parentOv = findParentOverride(args, 0);
        paxId = scene.createPolarAxes(parentOv);
        applyNVSkippingParent(scene, paxId, args, 0, mr);
    } else if (nvStart < args.size()) {
        applyNV(scene, paxId, args, nvStart, mr);
    }

    outs[0] = (nargout >= 1) ? scene.makeHandleValue(paxId, mr) : Value();
}

void tiledlayout_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                           keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId figId = kInvalidNodeId;
    keel::usize idx = 0;
    if (!args.empty()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[0], h)) {
            figId = h;
            idx = 1;
        }
    }

    keel::i32 rows = 1;
    keel::i32 cols = 1;
    keel::String arrangement = keel::String::from("fixed", mr);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "flow") || asciiEqualNoCase(tok, "vertical") || asciiEqualNoCase(tok, "horizontal")) {
            arrangement = tok.clone(mr);
            ++idx;
        }
    } else if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
        rows = keel::max<keel::i32>(1, keel::as<keel::i32>(args[idx].toScalar()));
        cols = keel::max<keel::i32>(1, keel::as<keel::i32>(args[idx + 1].toScalar()));
        idx += 2;
    }

    const NodeId tlId = scene.createTiledLayout(figId, rows, cols);
    if (GraphicsNode *tl = scene.findNode(tlId))
        tl->props.put(keel::String::from("TileArrangement", mr), Value::fromString(arrangement, mr));
    if (idx < args.size())
        applyNV(scene, tlId, args, idx, mr);

    outs[0] = (nargout >= 1) ? scene.makeHandleValue(tlId, mr) : Value();
}

void nexttile_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId tlId = kInvalidNodeId;
    keel::usize idx = 0;
    if (!args.empty()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[0], h)) {
            if (const GraphicsNode *n = scene.findNode(h); n && n->classId == ClassId::TiledChartLayout) {
                tlId = h;
                idx = 1;
            }
        }
    }
    if (tlId == kInvalidNodeId) {
        const NodeId figId = scene.ensureCurrentFigure();
        if (const GraphicsNode *fn = scene.findNode(figId)) {
            for (NodeId cid : fn->children) {
                if (const GraphicsNode *c = scene.findNode(cid); c && c->classId == ClassId::TiledChartLayout) {
                    tlId = cid;
                    break;
                }
            }
        }
        if (tlId == kInvalidNodeId)
            tlId = scene.createTiledLayout(figId, 1, 1);
    }

    keel::i32 tileNum = 0;
    keel::i32 rSpan = 1;
    keel::i32 cSpan = 1;
    if (idx < args.size() && args[idx].isNumeric()) {
        if (args[idx].numel() == 1) {
            tileNum = keel::max<keel::i32>(1, keel::as<keel::i32>(args[idx].elemAsDouble(0)));
            ++idx;
            if (idx < args.size() && args[idx].isNumeric() && args[idx].numel() >= 2) {
                rSpan = keel::max<keel::i32>(1, keel::as<keel::i32>(args[idx].elemAsDouble(0)));
                cSpan = keel::max<keel::i32>(1, keel::as<keel::i32>(args[idx].elemAsDouble(1)));
            }
        } else if (args[idx].numel() >= 2) {
            rSpan = keel::max<keel::i32>(1, keel::as<keel::i32>(args[idx].elemAsDouble(0)));
            cSpan = keel::max<keel::i32>(1, keel::as<keel::i32>(args[idx].elemAsDouble(1)));
        }
    }

    const NodeId axId = scene.nextTile(tlId, tileNum, rSpan, cSpan);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(axId, mr) : Value();
}

void subplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    if (args.empty()) {
        const NodeId axId = scene.ensureCurrentAxes();
        outs[0] = (nargout >= 1) ? scene.makeHandleValue(axId, mr) : Value();
        return;
    }

    // subplot(ax)
    if (NodeId existing = kInvalidNodeId; extractSceneHandle(args[0], existing) && isAxesLike(scene, existing)) {
        scene.setCurrentAxes(existing);
        outs[0] = (nargout >= 1) ? scene.makeHandleValue(existing, mr) : Value();
        return;
    }

    // subplot('Position', [l b w h], ...)
    if ((args[0].isChar() || args[0].isString()) && asciiEqualNoCase(args[0].toString(), "Position")) {
        const NodeId axId = scene.createAxes();
        applyNV(scene, axId, args, 0, mr);
        outs[0] = (nargout >= 1) ? scene.makeHandleValue(axId, mr) : Value();
        return;
    }

    auto parseIntStr = [](const Value &v, keel::i32 &outVal) -> bool {
        if (!v.isChar() && !v.isString())
            return false;
        const keel::String s = v.toString();
        if (s.empty())
            return false;
        keel::i32 acc = 0;
        for (keel::usize i = 0; i < s.size(); ++i) {
            const char ch = s[i];
            if (ch < '0' || ch > '9')
                return false;
            acc = acc * 10 + keel::as<keel::i32>(ch - '0');
        }
        outVal = acc;
        return true;
    };

    keel::i32 m = 1, n = 1;
    keel::Array<keel::i32> pList(mr);
    keel::usize nvStart = args.size();
    keel::i32 strCode = 0;
    keel::i32 sM = 0, sN = 0, sP = 0;
    if (args.size() >= 3 && args[0].isNumeric() && args[1].isNumeric() && args[2].isNumeric() && !args[2].isEmpty()) {
        m = keel::as<keel::i32>(args[0].toScalar());
        n = keel::as<keel::i32>(args[1].toScalar());
        pList.reserve(args[2].numel());
        for (keel::usize k = 0; k < args[2].numel(); ++k)
            pList.push_back(keel::as<keel::i32>(args[2].elemAsDouble(k)));
        nvStart = 3;
    } else if (args.size() >= 3 && parseIntStr(args[0], sM) && parseIntStr(args[1], sN) && parseIntStr(args[2], sP)) {
        m = sM;
        n = sN;
        pList.push_back(sP);
        nvStart = 3;
    } else if (args[0].isNumeric() && args[0].numel() == 1) {
        const keel::i32 code = keel::as<keel::i32>(args[0].toScalar());
        m = code / 100;
        n = (code / 10) % 10;
        pList.push_back(code % 10);
        nvStart = 1;
    } else if (parseIntStr(args[0], strCode) && strCode >= 111 && strCode <= 999) {
        m = strCode / 100;
        n = (strCode / 10) % 10;
        pList.push_back(strCode % 10);
        nvStart = 1;
    } else {
        throw Error("subplot: expected (m, n, p), 3-digit code, 'Position', or Axes handle.",
                    0, 0, "subplot", "", "numkit:graphics:InvalidSubplot");
    }

    // Consume optional standalone flag ('replace' / 'align') after (m, n, p).
    bool forceReplace = false;
    if (nvStart < args.size() && (args[nvStart].isChar() || args[nvStart].isString())) {
        const keel::String tok = args[nvStart].toString();
        if (asciiEqualNoCase(tok, "replace")) {
            forceReplace = true;
            ++nvStart;
        } else if (asciiEqualNoCase(tok, "align")) {
            ++nvStart;
        }
    }

    const NodeId axId = scene.createOrActivateSubplot(kInvalidNodeId, m, n, pList.as_ref());
    if (forceReplace)
        scene.clearAxes(axId, true);
    if (nvStart < args.size())
        applyNV(scene, axId, args, nvStart, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(axId, mr) : Value();
}

void cla_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    bool doReset = false;
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        if (asciiEqualNoCase(args[idx].toString(), "reset"))
            doReset = true;
    }
    scene.clearAxes(axId, doReset);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(axId, mr) : Value();
}

void hold_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    keel::String mode = keel::String::from("toggle");
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString()))
        mode = args[idx].toString();

    scene.setHoldMode(axId, mode);
    outs[0] = Value();
}

void ishold_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 1, "ishold");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    outs[0] = Value::logicalScalar(scene.isHoldOn(axId), mr);
}

void yyaxis_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "yyaxis");
    auto &scene = coreEngine(ctx).graphicsScene();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    if (idx >= args.size() || (!args[idx].isChar() && !args[idx].isString())) {
        throw Error("yyaxis: specify 'left' or 'right'.", 0, 0, "yyaxis", "", "numkit:graphics:InvalidArgument");
    }
    const keel::String side = args[idx].toString();
    bool isRight = false;
    if (asciiEqualNoCase(side, "right"))
        isRight = true;
    else if (!asciiEqualNoCase(side, "left"))
        throw Error("yyaxis: side must be 'left' or 'right'.", 0, 0, "yyaxis", "", "numkit:graphics:InvalidOption");

    scene.setYYAxisSide(axId, isRight);
    outs[0] = Value();
}

void linkaxes_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "linkaxes");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::Array<NodeId> ids(mr);
    if (args[0].isObject()) {
        for (keel::usize i = 0; i < args[0].objectCount(); ++i) {
            keel::i32 id = -1;
            bool inSc = false;
            if (graphicsHandleAt(args[0], i, id, inSc) && inSc)
                ids.push_back(id);
        }
    }
    const keel::String mode = (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                                  ? args[1].toString() : keel::String::from("xy", mr);
    scene.linkAxes(ids.as_ref(), mode);
    outs[0] = Value();
}

// ── Labels / Text / Legend / ColorBar / ConstantLine ────────────────────

void labelHelper(const char *slot, keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId targetId = kInvalidNodeId;
    keel::usize idx = 0;
    if (!args.empty()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[0], h) && isLabelOwner(scene, h)) {
            targetId = h;
            idx = 1;
        }
    }
    if (targetId == kInvalidNodeId) {
        // If the figure's most recent child is a HeatmapChart (e.g. heatmap(...); title('...')),
        // target it directly instead of creating a new overlapping Axes.
        const NodeId figId = scene.currentFigureId();
        if (const GraphicsNode *fn = scene.findNode(figId); fn && !fn->children.empty()) {
            const NodeId topId = fn->children.back();
            if (const GraphicsNode *tn = scene.findNode(topId); tn && tn->classId == ClassId::HeatmapChart)
                targetId = topId;
        }
        if (targetId == kInvalidNodeId)
            targetId = scene.ensureCurrentAxes();
    }

    const keel::String txt = (idx < args.size()) ? joinTextArg(args[idx], mr) : keel::String(mr);
    const NodeId textId = scene.setLabelText(targetId, slot, txt);
    keel::usize nvStart = idx + 1;
    NodeId subId = kInvalidNodeId;
    // title(titleText, subtitleText, Name, Value) two-string form.
    if (asciiEqualNoCase(slot, "title") && nvStart < args.size()
        && (args[nvStart].isChar() || args[nvStart].isString() || args[nvStart].isCell())) {
        const ClassSchema &txtSchema = schemaFor(ClassId::Text);
        const bool isPropName = ((args.size() - nvStart) % 2 == 0)
                                && (args[nvStart].isChar() || args[nvStart].isString())
                                && txtSchema.findExactNoCase(args[nvStart].toString()) != nullptr;
        if (!isPropName) {
            subId = scene.setLabelText(targetId, "subtitle", joinTextArg(args[nvStart], mr));
            ++nvStart;
        }
    }
    if (textId != kInvalidNodeId && nvStart < args.size())
        applyNV(scene, textId, args, nvStart, mr);

    outs[0] = (nargout >= 1) ? scene.makeHandleValue(textId, mr) : Value();
    if (nargout >= 2 && outs.size() >= 2)
        outs[1] = scene.makeHandleValue(subId, mr);
}

void sgtitle_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId figId = kInvalidNodeId;
    keel::usize idx = 0;
    if (!args.empty()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[0], h)) {
            if (const GraphicsNode *n = scene.findNode(h);
                n && (n->classId == ClassId::Figure || n->classId == ClassId::TiledChartLayout)) {
                figId = h;
                idx = 1;
            }
        }
    }
    if (figId == kInvalidNodeId)
        figId = scene.ensureCurrentFigure();

    const keel::String txt = (idx < args.size()) ? joinTextArg(args[idx], mr) : keel::String(mr);
    NodeId textId = kInvalidNodeId;
    if (GraphicsNode *fn = scene.findNode(figId)) {
        if (fn->titleId == kInvalidNodeId || scene.findNode(fn->titleId) == nullptr) {
            textId = scene.createText(figId, 0.5, 0.96, 0.0, txt);
            if (GraphicsNode *fn2 = scene.findNode(figId))
                fn2->titleId = textId;
            if (GraphicsNode *tn = scene.findNode(textId))
                tn->props.put(keel::String::from("HandleVisibility", mr), Value::fromString("off", mr));
        } else {
            textId = fn->titleId;
            scene.setProperty(textId, "String", Value::fromString(txt, mr), mr);
        }
    }
    if (textId != kInvalidNodeId && idx + 1 < args.size())
        applyNV(scene, textId, args, idx + 1, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(textId, mr) : Value();
}

void title_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ labelHelper("title", a, no, o, c); }
void subtitle_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ labelHelper("subtitle", a, no, o, c); }
void xlabel_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ labelHelper("xlabel", a, no, o, c); }
void ylabel_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ labelHelper("ylabel", a, no, o, c); }
void zlabel_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ labelHelper("zlabel", a, no, o, c); }

void text_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);

    if (idx + 2 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
        const Value &xv = args[idx];
        const Value &yv = args[idx + 1];
        const Value *zv = nullptr;
        const Value *sv = nullptr;
        if (idx + 3 < args.size() && args[idx + 2].isNumeric()) {
            zv = &args[idx + 2];
            sv = &args[idx + 3];
            idx += 4;
        } else {
            sv = &args[idx + 2];
            idx += 3;
        }

        const keel::usize n = keel::max<keel::usize>(1, keel::max(xv.numel(), yv.numel()));
        auto elemText = [&](const Value &sVal, keel::usize k) -> keel::String {
            if (n == 1)
                return joinTextArg(sVal, mr);
            if (sVal.isCell()) {
                if (sVal.numel() == 0) return keel::String(mr);
                return sVal.cellAt(k < sVal.numel() ? k : sVal.numel() - 1).toString();
            }
            if (sVal.isString() && sVal.numel() > 1) {
                return sVal.stringElem(k < sVal.numel() ? k : sVal.numel() - 1, mr);
            }
            if (sVal.isChar() && sVal.dims().rows() > 1) {
                const keel::usize nr = sVal.dims().rows();
                const keel::usize nc = sVal.dims().cols();
                const keel::usize r = (k < nr) ? k : (nr - 1);
                keel::String row(mr);
                row.reserve(nc);
                for (keel::usize c = 0; c < nc; ++c)
                    row.push_back(static_cast<char>(sVal.charData()[r + c * nr] & 0x7Fu));
                while (!row.empty() && row[row.size() - 1] == ' ')
                    row.pop_back();
                return row;
            }
            return joinTextArg(sVal, mr);
        };

        keel::Array<NodeId> created(mr);
        created.reserve(n);
        for (keel::usize k = 0; k < n; ++k) {
            const keel::f64 x = xv.isEmpty() ? 0.0 : xv.elemAsDouble(k < xv.numel() ? k : xv.numel() - 1);
            const keel::f64 y = yv.isEmpty() ? 0.0 : yv.elemAsDouble(k < yv.numel() ? k : yv.numel() - 1);
            const keel::f64 z = (zv && !zv->isEmpty()) ? zv->elemAsDouble(k < zv->numel() ? k : zv->numel() - 1) : 0.0;
            const NodeId tid = scene.createText(axId, x, y, z, elemText(*sv, k));
            if (idx < args.size())
                applyNV(scene, tid, args, idx, mr);
            created.push_back(tid);
        }
        outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
        return;
    }

    const NodeId tid = scene.createText(axId, 0.0, 0.0, 0.0, {});
    if (idx < args.size())
        applyNV(scene, tid, args, idx, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(tid, mr) : Value();
}

void legend_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);

    // Special single-keyword forms: legend('off'|'hide'|'show'|'toggle'|'boxoff'|'boxon')
    if (idx + 1 == args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String cmd = args[idx].toString();
        const GraphicsNode *ax = scene.findNode(axId);
        const NodeId existingLg = ax ? ax->legendId : kInvalidNodeId;
        if (asciiEqualNoCase(cmd, "off")) {
            if (existingLg != kInvalidNodeId && scene.findNode(existingLg))
                scene.deleteNode(existingLg);
            outs[0] = Value();
            return;
        }
        if (asciiEqualNoCase(cmd, "hide")) {
            if (existingLg != kInvalidNodeId && scene.findNode(existingLg))
                scene.setProperty(existingLg, "Visible", Value::fromString("off", mr), mr);
            outs[0] = Value();
            return;
        }
        if (asciiEqualNoCase(cmd, "show")) {
            const NodeId lgId = scene.createOrUpdateLegend(axId, {});
            scene.setProperty(lgId, "Visible", Value::fromString("on", mr), mr);
            outs[0] = (nargout >= 1) ? scene.makeHandleValue(lgId, mr) : Value();
            return;
        }
        if (asciiEqualNoCase(cmd, "toggle")) {
            const NodeId lgId = scene.createOrUpdateLegend(axId, {});
            const bool isOn = asciiEqualNoCase(scene.getProperty(lgId, "Visible", mr).toString(), "on");
            scene.setProperty(lgId, "Visible", Value::fromString(isOn ? "off" : "on", mr), mr);
            outs[0] = (nargout >= 1) ? scene.makeHandleValue(lgId, mr) : Value();
            return;
        }
        if (asciiEqualNoCase(cmd, "boxoff") || asciiEqualNoCase(cmd, "boxon")) {
            const NodeId lgId = scene.createOrUpdateLegend(axId, {});
            scene.setProperty(lgId, "Box", Value::fromString(asciiEqualNoCase(cmd, "boxon") ? "on" : "off", mr), mr);
            outs[0] = (nargout >= 1) ? scene.makeHandleValue(lgId, mr) : Value();
            return;
        }
    }

    // Separate label strings from trailing Legend Name-Value pairs.
    const ClassSchema &lgSchema = schemaFor(ClassId::Legend);
    keel::Array<keel::String> labels(mr);
    keel::usize nvStart = args.size();
    while (idx < args.size()) {
        if (args[idx].isCell()) {
            for (keel::usize k = 0; k < args[idx].numel(); ++k)
                labels.push_back(args[idx].cellAt(k).toString());
            ++idx;
            nvStart = idx;
            break;
        }
        if (args[idx].isChar() || args[idx].isString()) {
            const keel::String tok = args[idx].toString();
            // If an even number of args remain and tok is an exact Legend property name, start NV pairs here.
            if (((args.size() - idx) % 2 == 0) && lgSchema.findExactNoCase(tok) != nullptr) {
                nvStart = idx;
                break;
            }
            labels.push_back(tok.clone(mr));
            ++idx;
            continue;
        }
        if (args[idx].isStruct()) {
            nvStart = idx;
            break;
        }
        ++idx;
    }

    const NodeId lgId = scene.createOrUpdateLegend(axId, labels.as_ref());
    if (nvStart < args.size())
        applyNV(scene, lgId, args, nvStart, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lgId, mr) : Value();
}

void colorbar_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);

    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "off") || asciiEqualNoCase(tok, "hide")) {
            if (const GraphicsNode *ax = scene.findNode(axId); ax && ax->colorBarId != kInvalidNodeId) {
                if (scene.findNode(ax->colorBarId))
                    scene.deleteNode(ax->colorBarId);
            }
            outs[0] = Value();
            return;
        }
    }

    const NodeId cbId = scene.createOrUpdateColorBar(axId);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        // Positional Location argument (e.g. colorbar('southoutside', ...))
        if ((args.size() - idx) % 2 == 1) {
            scene.setProperty(cbId, "Location", args[idx], mr);
            ++idx;
        }
    }
    if (idx < args.size())
        applyNV(scene, cbId, args, idx, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(cbId, mr) : Value();
}

void constantLineHelper(char axisChar, keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    if (idx >= args.size()) {
        throw Error("xline/yline: numeric intercept value is required.",
                    0, 0, axisChar == 'x' ? "xline" : "yline", "", "numkit:graphics:InvalidArgument");
    }
    const bool okCoord = args[idx].isNumeric() || args[idx].isLogical()
        || (args[idx].isObject() && !isSceneGraphicsHandle(args[idx])
            && (args[idx].objectClassNameRef() == "datetime" || args[idx].objectClassNameRef() == "duration"));
    if (!okCoord) {
        throw Error("xline/yline: numeric intercept value is required.",
                    0, 0, axisChar == 'x' ? "xline" : "yline", "", "numkit:graphics:InvalidArgument");
    }
    const Value &vals = args[idx++];

    keel::String lineSpec(mr);
    keel::String label(mr);
    const ClassSchema &clSchema = schemaFor(ClassId::ConstantLine);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (!((args.size() - idx) % 2 == 0 && clSchema.findExactNoCase(tok) != nullptr)) {
            lineSpec = tok.clone(mr);
            ++idx;
            if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
                const keel::String tok2 = args[idx].toString();
                if (!((args.size() - idx) % 2 == 0 && clSchema.findExactNoCase(tok2) != nullptr)) {
                    label = tok2.clone(mr);
                    ++idx;
                }
            }
        }
    }

    keel::Array<NodeId> created(mr);
    created.reserve(vals.numel());
    for (keel::usize k = 0; k < vals.numel(); ++k) {
        const NodeId clId = scene.createConstantLine(axId, axisChar, vals.elemAsDouble(k), label);
        if (!lineSpec.empty()) {
            // Simple linespec parsing: '--', '-.', ':', '-' and single-letter color.
            keel::String ls(mr);
            if (lineSpec.find("--") != keel::kInvalidIndex) ls = keel::String::from("--", mr);
            else if (lineSpec.find("-.") != keel::kInvalidIndex) ls = keel::String::from("-.", mr);
            else if (lineSpec.find(':') != keel::kInvalidIndex) ls = keel::String::from(":", mr);
            else if (lineSpec.find('-') != keel::kInvalidIndex) ls = keel::String::from("-", mr);
            if (!ls.empty())
                scene.setProperty(clId, "LineStyle", Value::fromString(ls, mr), mr);
            for (keel::usize c = 0; c < lineSpec.size(); ++c) {
                const char ch = lineSpec[c];
                if (ch == 'r' || ch == 'g' || ch == 'b' || ch == 'c' || ch == 'm'
                    || ch == 'y' || ch == 'k' || ch == 'w') {
                    const char cs[2] = {ch, '\0'};
                    scene.setProperty(clId, "Color", Value::fromString(cs, mr), mr);
                    break;
                }
            }
        }
        if (idx < args.size())
            applyNV(scene, clId, args, idx, mr);
        created.push_back(clId);
    }

    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void xline_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ constantLineHelper('x', a, no, o, c); }
void yline_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ constantLineHelper('y', a, no, o, c); }

// ── Limits / Ticks / Scale / Grid / Box / View / Colormap / Axis ────────

void limHelper(const char *limProp, const char *modeProp, const char *methodProp,
               keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);

    // Query form: xlim / xlim(ax)
    if (idx >= args.size()) {
        outs[0] = scene.getProperty(axId, limProp, mr);
        return;
    }

    if (args[idx].isChar() || args[idx].isString()) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "mode")) {
            outs[0] = scene.getProperty(axId, modeProp, mr);
            return;
        }
        if (methodProp && asciiEqualNoCase(tok, "method")) {
            outs[0] = scene.getProperty(axId, methodProp, mr);
            return;
        }
        if (asciiEqualNoCase(tok, "auto") || asciiEqualNoCase(tok, "manual")) {
            scene.setProperty(axId, modeProp, args[idx], mr);
            outs[0] = (nargout >= 1) ? scene.getProperty(axId, limProp, mr) : Value();
            return;
        }
        if (methodProp && (asciiEqualNoCase(tok, "tight") || asciiEqualNoCase(tok, "padded")
                           || asciiEqualNoCase(tok, "tickaligned"))) {
            scene.setProperty(axId, methodProp, args[idx], mr);
            outs[0] = Value();
            return;
        }
    }

    scene.setProperty(axId, limProp, args[idx], mr);
    outs[0] = (nargout >= 1) ? scene.getProperty(axId, limProp, mr) : Value();
}

void xlim_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ limHelper("XLim", "XLimMode", "XLimitMethod", a, no, o, c); }
void ylim_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ limHelper("YLim", "YLimMode", "YLimitMethod", a, no, o, c); }
void zlim_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ limHelper("ZLim", "ZLimMode", "ZLimitMethod", a, no, o, c); }
void clim_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ limHelper("CLim", "CLimMode", nullptr, a, no, o, c); }
void rlim_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ limHelper("RLim", "RLimMode", nullptr, a, no, o, c); }
void thetalim_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ limHelper("ThetaLim", "ThetaLimMode", nullptr, a, no, o, c); }

void tickHelper(const char *valProp, const char *modeProp,
                keel::ArrayRef<Value> args, keel::usize,
                keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    if (idx >= args.size()) {
        outs[0] = scene.getProperty(axId, valProp, mr);
        return;
    }
    if (args[idx].isChar() || args[idx].isString()) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "mode")) {
            outs[0] = scene.getProperty(axId, modeProp, mr);
            return;
        }
        if (asciiEqualNoCase(tok, "auto") || asciiEqualNoCase(tok, "manual")) {
            scene.setProperty(axId, modeProp, args[idx], mr);
            outs[0] = Value();
            return;
        }
    }
    scene.setProperty(axId, valProp, args[idx], mr);
    outs[0] = Value();
}

void xticks_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickHelper("XTick", "XTickMode", a, no, o, c); }
void yticks_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickHelper("YTick", "YTickMode", a, no, o, c); }
void zticks_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickHelper("ZTick", "ZTickMode", a, no, o, c); }
void rticks_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickHelper("RTick", "RTickMode", a, no, o, c); }
void thetaticks_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickHelper("ThetaTick", "ThetaTickMode", a, no, o, c); }
void xticklabels_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickHelper("XTickLabel", "XTickLabelMode", a, no, o, c); }
void yticklabels_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickHelper("YTickLabel", "YTickLabelMode", a, no, o, c); }
void zticklabels_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickHelper("ZTickLabel", "ZTickLabelMode", a, no, o, c); }

void scaleHelper(const char *prop, keel::ArrayRef<Value> args,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    if (idx >= args.size()) {
        outs[0] = scene.getProperty(axId, prop, mr);
        return;
    }
    scene.setProperty(axId, prop, args[idx], mr);
    outs[0] = Value();
}

void xscale_scene_reg(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, CallContext &c)
{ scaleHelper("XScale", a, o, c); }
void yscale_scene_reg(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, CallContext &c)
{ scaleHelper("YScale", a, o, c); }
void zscale_scene_reg(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, CallContext &c)
{ scaleHelper("ZScale", a, o, c); }

void grid_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    const GraphicsNode *ax = scene.findNode(axId);
    const bool isPolar = (ax && ax->classId == ClassId::PolarAxes);

    keel::String mode = keel::String::from("toggle", mr);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString()))
        mode = args[idx].toString();

    if (asciiEqualNoCase(mode, "minor")) {
        if (isPolar) {
            const bool cur = asciiEqualNoCase(scene.getProperty(axId, "RMinorGrid", mr).toString(), "on");
            const Value nv = Value::fromString(cur ? "off" : "on", mr);
            scene.setProperty(axId, "RMinorGrid", nv, mr);
            scene.setProperty(axId, "ThetaMinorGrid", nv, mr);
        } else {
            const bool cur = asciiEqualNoCase(scene.getProperty(axId, "XMinorGrid", mr).toString(), "on");
            const Value nv = Value::fromString(cur ? "off" : "on", mr);
            scene.setProperty(axId, "XMinorGrid", nv, mr);
            scene.setProperty(axId, "YMinorGrid", nv, mr);
            scene.setProperty(axId, "ZMinorGrid", nv, mr);
        }
    } else {
        const char *probeProp = isPolar ? "RGrid" : "XGrid";
        const bool cur = asciiEqualNoCase(scene.getProperty(axId, probeProp, mr).toString(), "on");
        const bool wantOn = asciiEqualNoCase(mode, "on") ? true
                          : (asciiEqualNoCase(mode, "off") ? false : !cur);
        const Value nv = Value::fromString(wantOn ? "on" : "off", mr);
        if (isPolar) {
            scene.setProperty(axId, "RGrid", nv, mr);
            scene.setProperty(axId, "ThetaGrid", nv, mr);
        } else {
            scene.setProperty(axId, "XGrid", nv, mr);
            scene.setProperty(axId, "YGrid", nv, mr);
            scene.setProperty(axId, "ZGrid", nv, mr);
        }
    }
    outs[0] = Value();
}

void box_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    const bool cur = asciiEqualNoCase(scene.getProperty(axId, "Box", mr).toString(), "on");
    bool wantOn = !cur;
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "on")) wantOn = true;
        else if (asciiEqualNoCase(tok, "off")) wantOn = false;
    }
    scene.setProperty(axId, "Box", Value::fromString(wantOn ? "on" : "off", mr), mr);
    outs[0] = Value();
}

void view_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);
    if (idx >= args.size()) {
        const Value vw = scene.getProperty(axId, "View", mr);
        if (nargout >= 2) {
            outs[0] = Value::scalar(vw.elemAsDouble(0), mr);
            outs[1] = Value::scalar(vw.elemAsDouble(1), mr);
        } else {
            outs[0] = vw;
        }
        return;
    }

    keel::f64 az = 0.0, el = 90.0;
    if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
        az = args[idx].toScalar();
        el = args[idx + 1].toScalar();
    } else if (args[idx].isNumeric()) {
        if (args[idx].numel() == 1) {
            const keel::i32 dim = keel::as<keel::i32>(args[idx].toScalar());
            if (dim == 3) { az = -37.5; el = 30.0; }
            else { az = 0.0; el = 90.0; }
        } else if (args[idx].numel() == 2) {
            az = args[idx].elemAsDouble(0);
            el = args[idx].elemAsDouble(1);
        } else if (args[idx].numel() >= 3) {
            const keel::f64 x = args[idx].elemAsDouble(0);
            const keel::f64 y = args[idx].elemAsDouble(1);
            const keel::f64 z = args[idx].elemAsDouble(2);
            const keel::f64 rxy = keel::sqrt(x * x + y * y);
            az = keel::atan2(x, -y) * (180.0 / keel::kPi);
            el = keel::atan2(z, rxy) * (180.0 / keel::kPi);
        }
    }

    // Reference engine: 3-D views are not supported on axes with dual Y-axes (yyaxis).
    if (const GraphicsNode *axn = scene.findNode(axId);
        axn && axn->yAxisRightId != kInvalidNodeId && el != 90.0) {
        outs[0] = (nargout >= 1) ? scene.getProperty(axId, "View", mr) : Value();
        return;
    }

    Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    vw.doubleDataMut()[0] = az;
    vw.doubleDataMut()[1] = el;
    scene.setProperty(axId, "View", vw, mr);
    outs[0] = (nargout >= 1) ? vw : Value();
}

void colormap_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId targetId = kInvalidNodeId;
    keel::usize idx = 0;
    if (!args.empty()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[0], h)) {
            targetId = h;
            idx = 1;
        }
    }
    if (targetId == kInvalidNodeId)
        targetId = scene.ensureCurrentFigure();

    if (idx >= args.size()) {
        outs[0] = scene.getProperty(targetId, "Colormap", mr);
        return;
    }

    Value cmap;
    if (args[idx].isNumeric()) {
        cmap = args[idx];
    } else if (args[idx].isChar() || args[idx].isString()) {
        const keel::String nm = args[idx].toString();
        if (asciiEqualNoCase(nm, "default") || asciiEqualNoCase(nm, "parula")) {
            cmap = makeDefaultParulaColormap(mr);
        } else if (const ExternalFunc *cf = ctx.engine->findExternal(nm, ctx.env)) {
            Value tmpOut[1];
            (*cf)({}, 1, keel::MutArrayRef<Value>(tmpOut, 1), ctx);
            if (tmpOut[0].isNumeric() && tmpOut[0].dims().cols() == 3)
                cmap = keel::move(tmpOut[0]);
            else
                cmap = makeDefaultParulaColormap(mr);
        } else {
            cmap = makeDefaultParulaColormap(mr);
        }
    } else {
        throw Error("colormap: expected a colormap name or N-by-3 RGB matrix.",
                    0, 0, "colormap", "", "numkit:graphics:InvalidColormap");
    }

    scene.setProperty(targetId, "Colormap", cmap, mr);
    if (const GraphicsNode *tn = scene.findNode(targetId); tn && tn->classId == ClassId::Figure) {
        if (tn->currentAxesId != kInvalidNodeId && scene.findNode(tn->currentAxesId))
            scene.setProperty(tn->currentAxesId, "Colormap", cmap, mr);
    }
    outs[0] = (nargout >= 1) ? cmap : Value();
}

void axis_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId axId = resolveLeadingAxes(scene, args, idx);

    auto getLimVec = [&]() -> Value {
        const Value xl = scene.getProperty(axId, "XLim", mr);
        const Value yl = scene.getProperty(axId, "YLim", mr);
        Value v = Value::matrix(1, 4, ValueType::DOUBLE, mr);
        v.doubleDataMut()[0] = xl.elemAsDouble(0);
        v.doubleDataMut()[1] = xl.elemAsDouble(1);
        v.doubleDataMut()[2] = yl.elemAsDouble(0);
        v.doubleDataMut()[3] = yl.elemAsDouble(1);
        return v;
    };

    if (idx >= args.size()) {
        outs[0] = getLimVec();
        return;
    }

    for (keel::usize i = idx; i < args.size(); ++i) {
        if (args[i].isNumeric() && args[i].numel() >= 4) {
            Value xl = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            xl.doubleDataMut()[0] = args[i].elemAsDouble(0);
            xl.doubleDataMut()[1] = args[i].elemAsDouble(1);
            Value yl = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            yl.doubleDataMut()[0] = args[i].elemAsDouble(2);
            yl.doubleDataMut()[1] = args[i].elemAsDouble(3);
            scene.setProperty(axId, "XLim", xl, mr);
            scene.setProperty(axId, "YLim", yl, mr);
            if (args[i].numel() >= 6) {
                Value zl = Value::matrix(1, 2, ValueType::DOUBLE, mr);
                zl.doubleDataMut()[0] = args[i].elemAsDouble(4);
                zl.doubleDataMut()[1] = args[i].elemAsDouble(5);
                scene.setProperty(axId, "ZLim", zl, mr);
            }
            continue;
        }
        if (!args[i].isChar() && !args[i].isString())
            continue;
        const keel::String opt = args[i].toString();
        if (asciiEqualNoCase(opt, "equal")) {
            Value dar = Value::matrix(1, 3, ValueType::DOUBLE, mr);
            dar.doubleDataMut()[0] = 1.0; dar.doubleDataMut()[1] = 1.0; dar.doubleDataMut()[2] = 1.0;
            scene.setProperty(axId, "DataAspectRatio", dar, mr);
        } else if (asciiEqualNoCase(opt, "square")) {
            Value pbar = Value::matrix(1, 3, ValueType::DOUBLE, mr);
            pbar.doubleDataMut()[0] = 1.0; pbar.doubleDataMut()[1] = 1.0; pbar.doubleDataMut()[2] = 1.0;
            scene.setProperty(axId, "PlotBoxAspectRatio", pbar, mr);
        } else if (asciiEqualNoCase(opt, "image")) {
            Value dar = Value::matrix(1, 3, ValueType::DOUBLE, mr);
            dar.doubleDataMut()[0] = 1.0; dar.doubleDataMut()[1] = 1.0; dar.doubleDataMut()[2] = 1.0;
            scene.setProperty(axId, "DataAspectRatio", dar, mr);
            scene.setProperty(axId, "XLimitMethod", Value::fromString("tight", mr), mr);
            scene.setProperty(axId, "YLimitMethod", Value::fromString("tight", mr), mr);
        } else if (asciiEqualNoCase(opt, "normal")) {
            scene.setProperty(axId, "DataAspectRatioMode", Value::fromString("auto", mr), mr);
            scene.setProperty(axId, "PlotBoxAspectRatioMode", Value::fromString("auto", mr), mr);
        } else if (asciiEqualNoCase(opt, "tight") || asciiEqualNoCase(opt, "padded") || asciiEqualNoCase(opt, "tickaligned")) {
            scene.setProperty(axId, "XLimitMethod", args[i], mr);
            scene.setProperty(axId, "YLimitMethod", args[i], mr);
            scene.setProperty(axId, "ZLimitMethod", args[i], mr);
        } else if (asciiEqualNoCase(opt, "auto") || asciiEqualNoCase(opt, "manual")) {
            scene.setProperty(axId, "XLimMode", args[i], mr);
            scene.setProperty(axId, "YLimMode", args[i], mr);
            scene.setProperty(axId, "ZLimMode", args[i], mr);
        } else if (asciiEqualNoCase(opt, "ij")) {
            scene.setProperty(axId, "YDir", Value::fromString("reverse", mr), mr);
        } else if (asciiEqualNoCase(opt, "xy")) {
            scene.setProperty(axId, "YDir", Value::fromString("normal", mr), mr);
        } else if (asciiEqualNoCase(opt, "off") || asciiEqualNoCase(opt, "on")) {
            scene.setProperty(axId, "Visible", Value::fromString(asciiEqualNoCase(opt, "on") ? "on" : "off", mr), mr);
        }
    }
    outs[0] = (nargout >= 1) ? getLimVec() : Value();
}

} // anonymous namespace

void registerSceneAxesBuiltins(Engine &engine)
{
    auto regBoth = [&](const char *sub, const char *name,
                       void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &)) {
        engine.registerFunction("", name, ExternalFunc(fn));
        keel::SmallString<64> ns("graphics.");
        ns.append(sub);
        engine.registerFunction(ns, name, ExternalFunc(fn));
    };

    regBoth("layout", "axes",        &axes_scene_reg);
    regBoth("layout", "gca",         &gca_scene_reg);
    regBoth("polar",  "polaraxes",   &polaraxes_scene_reg);
    regBoth("layout", "tiledlayout", &tiledlayout_scene_reg);
    regBoth("layout", "nexttile",    &nexttile_scene_reg);
    regBoth("layout", "subplot",     &subplot_scene_reg);
    regBoth("layout", "cla",         &cla_scene_reg);
    regBoth("layout", "hold",        &hold_scene_reg);
    regBoth("layout", "ishold",      &ishold_scene_reg);
    regBoth("layout", "yyaxis",      &yyaxis_scene_reg);
    regBoth("layout", "linkaxes",    &linkaxes_scene_reg);

    regBoth("layout", "title",       &title_scene_reg);
    regBoth("layout", "subtitle",    &subtitle_scene_reg);
    regBoth("layout", "sgtitle",     &sgtitle_scene_reg);
    regBoth("layout", "xlabel",      &xlabel_scene_reg);
    regBoth("layout", "ylabel",      &ylabel_scene_reg);
    regBoth("layout", "zlabel",      &zlabel_scene_reg);
    regBoth("layout", "text",        &text_scene_reg);
    regBoth("layout", "legend",      &legend_scene_reg);
    regBoth("layout", "colorbar",    &colorbar_scene_reg);
    regBoth("line",   "xline",       &xline_scene_reg);
    regBoth("line",   "yline",       &yline_scene_reg);

    regBoth("layout", "xlim",        &xlim_scene_reg);
    regBoth("layout", "ylim",        &ylim_scene_reg);
    regBoth("layout", "zlim",        &zlim_scene_reg);
    regBoth("layout", "clim",        &clim_scene_reg);
    regBoth("layout", "caxis",       &clim_scene_reg);
    regBoth("polar",  "rlim",        &rlim_scene_reg);
    regBoth("polar",  "thetalim",    &thetalim_scene_reg);

    regBoth("layout", "xticks",      &xticks_scene_reg);
    regBoth("layout", "yticks",      &yticks_scene_reg);
    regBoth("layout", "zticks",      &zticks_scene_reg);
    regBoth("polar",  "rticks",      &rticks_scene_reg);
    regBoth("polar",  "thetaticks",  &thetaticks_scene_reg);
    regBoth("layout", "xticklabels", &xticklabels_scene_reg);
    regBoth("layout", "yticklabels", &yticklabels_scene_reg);
    regBoth("layout", "zticklabels", &zticklabels_scene_reg);

    regBoth("layout", "xscale",      &xscale_scene_reg);
    regBoth("layout", "yscale",      &yscale_scene_reg);
    regBoth("layout", "zscale",      &zscale_scene_reg);
    regBoth("layout", "grid",        &grid_scene_reg);
    regBoth("layout", "box",         &box_scene_reg);
    regBoth("layout", "view",        &view_scene_reg);
    regBoth("layout", "colormap",    &colormap_scene_reg);
    regBoth("layout", "axis",        &axis_scene_reg);
}

} // namespace numkit::gfx::detail
