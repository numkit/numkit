// src/builtin/register/graphics/scene_builtins.cpp
//
// Phase 1 Model-First Handle Graphics builtins & BuiltinClass registrations:
//   - BuiltinClass: matlab.ui.Root (16 props), matlab.ui.Figure (68 props),
//                   matlab.graphics.theme.GraphicsTheme (2 props),
//                   matlab.graphics.GraphicsPlaceholder
//   - Builtins: figure, gcf, groot, gobjects, clf, close, allchild, ancestor,
//               copyobj, reset, plus scene-dispatching set/get/findobj/findall/
//               isvalid/isgraphics/ishandle with fallback to legacy FigureManager
//               for chart/axes handles until Phases 2-3 migrate them.

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>

#include <numkit/core/engine.hpp>
#include <numkit/graphics/figure_manager.hpp>
#include <numkit/graphics/graphics_handle.hpp>
#include <numkit/graphics/graphics_scene.hpp>
#include <numkit/graphics/property_schema.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/object.hpp>
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

bool valuesEqualForFind(const Value &a, const Value &b)
{
    if ((a.isChar() || a.isString()) && (b.isChar() || b.isString()))
        return a.toString() == b.toString();
    if (a.isNumeric() && b.isNumeric()) {
        if (a.dims() != b.dims()) return false;
        for (keel::usize i = 0; i < a.numel(); ++i) {
            if (a.elemAsDouble(i) != b.elemAsDouble(i))
                return false;
        }
        return true;
    }
    if (a.isLogical() && b.isLogical()) {
        if (a.dims() != b.dims()) return false;
        for (keel::usize i = 0; i < a.numel(); ++i) {
            if (a.logicalData()[i] != b.logicalData()[i])
                return false;
        }
        return true;
    }
    if (a.isObject() && b.isObject()) {
        return graphicsHandleIdOf(a) >= 0 && graphicsHandleIdOf(a) == graphicsHandleIdOf(b);
    }
    return false;
}

bool matchSimpleRegex(keel::StringRef text, keel::StringRef pat) noexcept
{
    // Lightweight substring / anchor matcher (^prefix, suffix$, plain substring).
    if (pat.empty()) return true;
    const bool anchorStart = (pat.front() == '^');
    const bool anchorEnd = (pat.back() == '$');
    keel::StringRef core = pat;
    if (anchorStart) core = core.substr(1);
    if (anchorEnd && !core.empty()) core = core.substr(0, core.size() - 1);
    if (anchorStart && anchorEnd)
        return text == core;
    if (anchorStart)
        return text.size() >= core.size() && text.substr(0, core.size()) == core;
    if (anchorEnd)
        return text.size() >= core.size() && text.substr(text.size() - core.size()) == core;
    return text.find(core) != keel::kInvalidIndex;
}

void applyNameValuePairs(GraphicsScene &scene, NodeId id,
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

void collectSubtree(const GraphicsScene &scene, NodeId root, bool includeHidden,
                    keel::i32 maxDepth, keel::i32 depth, keel::Array<NodeId> &out)
{
    if (!scene.findNode(root))
        return;
    if (includeHidden || root == kRootNodeId || scene.isHandleVisible(root))
        out.push_back(root);
    if (maxDepth >= 0 && depth >= maxDepth)
        return;
    auto kids = includeHidden ? scene.allChildren(root) : scene.visibleChildren(root);
    for (NodeId cid : kids)
        collectSubtree(scene, cid, includeHidden, maxDepth, depth + 1, out);
}

// ── BuiltinClass hooks for Scene classes ────────────────────────────────

bool scenePropGet(const Value &self, keel::StringRef name, Value &out, CallContext &ctx)
{
    NodeId id = kInvalidNodeId;
    if (!extractSceneHandle(self, id))
        return false;
    if (id == kPlaceholderNodeId) {
        throw Error("Unrecognized property for class 'matlab.graphics.GraphicsPlaceholder'.",
                    0, 0, "get", "", "numkit:graphics:UnrecognizedProperty");
    }
    auto &scene = coreEngine(ctx).graphicsScene();
    out = scene.getProperty(id, name, ctx.engine->allocator());
    return true;
}

bool scenePropSet(Value &self, keel::StringRef name, const Value &val, CallContext &ctx)
{
    NodeId id = kInvalidNodeId;
    if (!extractSceneHandle(self, id))
        return false;
    if (id == kPlaceholderNodeId) {
        throw Error("Cannot set property on 'matlab.graphics.GraphicsPlaceholder'.",
                    0, 0, "set", "", "numkit:graphics:UnrecognizedProperty");
    }
    auto &scene = coreEngine(ctx).graphicsScene();
    scene.setProperty(id, name, val, ctx.engine->allocator());
    return true;
}

void sceneEqOp(Value &, keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.size() < 2) {
        outs[0] = Value::logicalScalar(false, mr);
        return;
    }
    const Value &a = args[0];
    const Value &b = args[1];
    auto elemId = [](const Value &v, keel::usize idx, keel::i32 &idOut, bool &inSc) -> bool {
        if (v.isObject() && v.objectCount() > 0)
            return graphicsHandleAt(v, idx, idOut, inSc);
        return false;
    };
    const keel::usize na = a.isObject() ? a.objectCount() : a.numel();
    const keel::usize nb = b.isObject() ? b.objectCount() : b.numel();
    if (na == 0 || nb == 0) {
        outs[0] = Value::matrix(0, 0, ValueType::LOGICAL, mr);
        return;
    }
    const keel::usize n = (na == 1) ? nb : na;
    const Dims dims = (na == 1) ? b.dims() : a.dims();
    Value res = Value::matrix(dims.rows(), dims.cols(), ValueType::LOGICAL, mr);
    keel::u8 *dst = res.logicalDataMut();
    for (keel::usize i = 0; i < n; ++i) {
        keel::i32 ida = -1, idb = -1;
        bool sca = false, scb = false;
        const bool ha = elemId(a, na == 1 ? 0 : i, ida, sca);
        const bool hb = elemId(b, nb == 1 ? 0 : i, idb, scb);
        dst[i] = (ha && hb && sca == scb && ida == idb && ida >= 0) ? 1u : 0u;
    }
    outs[0] = keel::move(res);
}

void sceneNeOp(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    sceneEqOp(self, args, nargout, outs, ctx);
    if (outs[0].isLogical()) {
        keel::u8 *p = outs[0].logicalDataMut();
        for (keel::usize i = 0; i < outs[0].numel(); ++i)
            p[i] = p[i] ? 0u : 1u;
    }
}

keel::String sceneDispText(Engine &engine, const Value &self)
{
    if (self.objectCount() != 1)
        return {};
    NodeId id = kInvalidNodeId;
    if (!extractSceneHandle(self, id))
        return {};
    if (id == kPlaceholderNodeId)
        return keel::String::from("  GraphicsPlaceholder with no properties.\n");
    const auto &scene = engine.graphicsScene();
    const GraphicsNode *n = scene.findNode(id);
    if (!n)
        return keel::String::from("  handle to deleted graphics object\n");
    const ClassSchema &s = schemaFor(n->classId);
    if (n->classId == ClassId::Figure) {
        const Value numV = scene.getProperty(id, "Number");
        const Value nameV = scene.getProperty(id, "Name");
        const Value posV = scene.getProperty(id, "Position");
        const Value visV = scene.getProperty(id, "Visible");
        keel::String numStr = numV.isEmpty() ? keel::String::from("[]")
                                             : keel::format("{}", keel::as<keel::i32>(numV.toScalar()));
        return keel::format(
            "  Figure ({}) with properties:\n\n"
            "      Number: {}\n"
            "        Name: '{}'\n"
            "       Color: [3x1 double]\n"
            "    Position: [{:.4g} {:.4g} {:.4g} {:.4g}]\n"
            "     Visible: '{}'\n",
            numStr, numStr, nameV.toString(),
            posV.elemAsDouble(0), posV.elemAsDouble(1),
            posV.elemAsDouble(2), posV.elemAsDouble(3),
            visV.toString());
    }
    if (n->classId == ClassId::Root) {
        const Value szV = scene.getProperty(id, "ScreenSize");
        return keel::format(
            "  Graphics Root with properties:\n\n"
            "      CurrentFigure: [{}]\n"
            "         ScreenSize: [{:.4g} {:.4g} {:.4g} {:.4g}]\n"
            "  ShowHiddenHandles: '{}'\n",
            scene.currentFigureId() != kInvalidNodeId ? "1x1 Figure" : "0x0 GraphicsPlaceholder",
            szV.elemAsDouble(0), szV.elemAsDouble(1), szV.elemAsDouble(2), szV.elemAsDouble(3),
            scene.getProperty(id, "ShowHiddenHandles").toString());
    }
    if (n->classId == ClassId::Theme) {
        return keel::format(
            "  GraphicsTheme with properties:\n\n"
            "              Name: '{}'\n"
            "    BaseColorStyle: '{}'\n",
            scene.getProperty(id, "Name").toString(),
            scene.getProperty(id, "BaseColorStyle").toString());
    }
    if (n->classId == ClassId::Axes) {
        const Value xl = scene.getProperty(id, "XLim");
        const Value yl = scene.getProperty(id, "YLim");
        return keel::format(
            "  Axes with properties:\n\n"
            "             XLim: [{:.4g} {:.4g}]\n"
            "             YLim: [{:.4g} {:.4g}]\n"
            "           XScale: '{}'\n"
            "           YScale: '{}'\n"
            "         NextPlot: '{}'\n",
            xl.elemAsDouble(0), xl.elemAsDouble(1),
            yl.elemAsDouble(0), yl.elemAsDouble(1),
            scene.getProperty(id, "XScale").toString(),
            scene.getProperty(id, "YScale").toString(),
            scene.getProperty(id, "NextPlot").toString());
    }
    if (n->classId == ClassId::Text) {
        return keel::format(
            "  Text ('{}') with properties:\n\n"
            "      String: '{}'\n"
            "    FontSize: {:.4g}\n",
            scene.getProperty(id, "String").toString(),
            scene.getProperty(id, "String").toString(),
            scene.getProperty(id, "FontSize").toScalar());
    }
    return keel::format("  {} with properties\n", s.className);
}

void registerOneSceneClass(Engine &engine, ClassId cid)
{
    const ClassSchema &schema = schemaFor(cid);
    BuiltinClass cls;
    cls.name = keel::String::from(schema.className);
    cls.isHandle = true;
    cls.heterogeneousGroup = keel::String::from("graphics");
    cls.superclasses.push_back(keel::String::from("matlab.graphics.Graphics"));
    cls.superclasses.push_back(keel::String::from("matlab.mixin.Handle"));
    cls.superclasses.push_back(keel::String::from("handle"));

    for (keel::usize i = 0; i < schema.propCount; ++i) {
        if (schema.props[i].isHidden())
            continue;
        cls.propNames.push_back(keel::String::from(schema.props[i].name));
        PropMeta pm;
        pm.name = keel::String::from(schema.props[i].name);
        pm.getAccess = keel::String::from("public");
        pm.setAccess = keel::String::from(schema.props[i].isReadOnly() ? "private" : "public");
        cls.propMeta.push_back(keel::move(pm));
    }

    cls.propGet = &scenePropGet;
    cls.propSet = &scenePropSet;
    cls.ops.put(keel::String::from("eq"), &sceneEqOp);
    cls.ops.put(keel::String::from("ne"), &sceneNeOp);
    cls.dispText = [&engine](const Value &self) -> keel::String {
        return sceneDispText(engine, self);
    };
    cls.construct = [cid](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
        auto &scene = coreEngine(ctx).graphicsScene();
        auto *mr = ctx.engine->allocator();
        if (cid == ClassId::Root)
            return scene.makeHandleValue(kRootNodeId, mr);
        if (cid == ClassId::Placeholder)
            return scene.makeHandleValue(kPlaceholderNodeId, mr);
        if (cid == ClassId::Figure) {
            const NodeId fid = scene.createFigure();
            applyNameValuePairs(scene, fid, args, 0, mr);
            if (const GraphicsNode *fn = scene.findNode(fid); fn && fn->figureNumber > 0)
                ctx.engine->figureManager().setFigure(fn->figureNumber);
            return scene.makeHandleValue(fid, mr);
        }
        if (cid == ClassId::Axes) {
            const NodeId axId = scene.createAxes();
            applyNameValuePairs(scene, axId, args, 0, mr);
            return scene.makeHandleValue(axId, mr);
        }
        if (cid == ClassId::PolarAxes) {
            const NodeId paxId = scene.createPolarAxes();
            applyNameValuePairs(scene, paxId, args, 0, mr);
            return scene.makeHandleValue(paxId, mr);
        }
        if (cid == ClassId::TiledChartLayout) {
            const NodeId tlId = scene.createTiledLayout(kInvalidNodeId, 1, 1);
            applyNameValuePairs(scene, tlId, args, 0, mr);
            return scene.makeHandleValue(tlId, mr);
        }
        if (cid == ClassId::Text) {
            const NodeId tid = scene.createText(kInvalidNodeId, 0.0, 0.0, 0.0, {});
            applyNameValuePairs(scene, tid, args, 0, mr);
            return scene.makeHandleValue(tid, mr);
        }
        if (cid == ClassId::Legend) {
            const NodeId lgId = scene.createOrUpdateLegend(scene.ensureCurrentAxes(), {});
            applyNameValuePairs(scene, lgId, args, 0, mr);
            return scene.makeHandleValue(lgId, mr);
        }
        if (cid == ClassId::ColorBar) {
            const NodeId cbId = scene.createOrUpdateColorBar(scene.ensureCurrentAxes());
            applyNameValuePairs(scene, cbId, args, 0, mr);
            return scene.makeHandleValue(cbId, mr);
        }
        if (cid == ClassId::ConstantLine) {
            const NodeId clId = scene.createConstantLine(kInvalidNodeId, 'x', 0.0, {});
            applyNameValuePairs(scene, clId, args, 0, mr);
            return scene.makeHandleValue(clId, mr);
        }
        if (GraphicsScene::isChartClass(cid) || cid == ClassId::TextBox) {
            // Zero-arg default construction (e.g. during object-array preallocation/growth
            // such as `h = zeros(1,5); h(1) = patch(...)`) must yield a GraphicsPlaceholder
            // without spawning ghost live nodes in the current Axes.
            if (args.empty())
                return scene.makeEmptyPlaceholder(mr);
            const NodeId id = scene.createChartNode(cid, kInvalidNodeId, cid != ClassId::TextBox);
            if (cid == ClassId::Line) {
                Value def = Value::matrix(1, 2, ValueType::DOUBLE, mr);
                def.doubleDataMut()[0] = 0.0;
                def.doubleDataMut()[1] = 1.0;
                if (GraphicsNode *ln = scene.findNode(id)) {
                    ln->props.put(keel::String::from("XData", mr), def);
                    ln->props.put(keel::String::from("YData", mr), def);
                }
            }
            applyNameValuePairs(scene, id, args, 0, mr);
            if (const GraphicsNode *n = scene.findNode(id); n && GraphicsScene::isChartClass(cid))
                scene.recomputeAxesAutoLimits(n->parentId, mr);
            return scene.makeHandleValue(id, mr);
        }
        return scene.makeEmptyPlaceholder(mr);
    };
    engine.registerClass(keel::move(cls));
}

// ── Scene Builtins ──────────────────────────────────────────────────────

void figure_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto &fm = ctx.engine->figureManager();
    auto *mr = ctx.engine->allocator();

    NodeId fid = kInvalidNodeId;
    keel::usize nvStart = 0;

    if (args.empty() || args[0].isChar() || args[0].isString() || args[0].isStruct()) {
        fid = scene.createFigure();
        nvStart = 0;
    } else if (NodeId existingId = kInvalidNodeId; extractSceneHandle(args[0], existingId)) {
        const GraphicsNode *fn = scene.findNode(existingId);
        if (!fn || fn->classId != ClassId::Figure)
            throw Error("figure: handle must be a valid Figure object.",
                        0, 0, "figure", "", "numkit:graphics:InvalidFigureHandle");
        scene.setCurrentFigure(existingId);
        fid = existingId;
        nvStart = 1;
    } else if (args[0].isNumeric() && !args[0].isComplex() && args[0].numel() == 1) {
        const keel::f64 raw = args[0].elemAsDouble(0);
        if (!keel::is_finite(raw) || raw < 1.0 || raw != keel::floor(raw)) {
            throw Error("figure: figure number must be a positive integer.",
                        0, 0, "figure", "", "numkit:graphics:InvalidFigureNumber");
        }
        fid = scene.createOrActivateFigure(keel::as<keel::i32>(raw));
        nvStart = 1;
    } else {
        throw Error("figure: first argument must be a Figure handle, positive integer, or Name-Value pair.",
                    0, 0, "figure", "", "numkit:graphics:InvalidArgument");
    }

    if (nvStart < args.size())
        applyNameValuePairs(scene, fid, args, nvStart, mr);

    if (const GraphicsNode *fn = scene.findNode(fid); fn && fn->figureNumber > 0)
        fm.setFigure(fn->figureNumber);

    outs[0] = (nargout >= 1) ? scene.makeHandleValue(fid, mr) : Value();
}

void gcf_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 0, "gcf");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    const NodeId fid = scene.ensureCurrentFigure();
    if (const GraphicsNode *fn = scene.findNode(fid); fn && fn->figureNumber > 0)
        ctx.engine->figureManager().setFigure(fn->figureNumber);
    outs[0] = scene.makeHandleValue(fid, mr);
}

void groot_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 0, "groot");
    outs[0] = coreEngine(ctx).graphicsScene().makeHandleValue(kRootNodeId, ctx.engine->allocator());
}

void gobjects_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    keel::usize r = 1, c = 1;
    if (args.size() == 1 && args[0].isNumeric()) {
        if (args[0].numel() == 1) {
            r = c = keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(0)));
        } else if (args[0].numel() >= 2) {
            r = keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(0)));
            c = keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(1)));
        }
    } else if (args.size() >= 2 && args[0].isNumeric() && args[1].isNumeric()) {
        r = keel::as<keel::usize>(keel::max(0.0, args[0].elemAsDouble(0)));
        c = keel::as<keel::usize>(keel::max(0.0, args[1].elemAsDouble(0)));
    }
    auto *res = mr ? mr : keel::default_allocator();
    const keel::usize n = r * c;
    keel::Array<keel::Arc<ObjectState>> states(res);
    states.reserve(n);
    for (keel::usize i = 0; i < n; ++i) {
        auto payload = keel::make_arc_in<GraphicsHandlePayload>(res);
        payload->handleId = kPlaceholderNodeId;
        payload->inScene = true;
        auto st = keel::make_arc_in<ObjectState>(res, res);
        st->native = keel::move(payload);
        states.push_back(keel::move(st));
    }
    outs[0] = Value::objectArray("matlab.graphics.GraphicsPlaceholder",
                                 Dims(r, c), keel::move(states),
                                 /*isHandle=*/true, res);
}

void clf_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto &fm = ctx.engine->figureManager();
    auto *mr = ctx.engine->allocator();

    NodeId targetFig = kInvalidNodeId;
    bool doReset = false;
    keel::usize idx = 0;
    if (idx < args.size()) {
        if (NodeId hid = kInvalidNodeId; extractSceneHandle(args[idx], hid)) {
            targetFig = hid;
            ++idx;
        } else if (args[idx].isNumeric() && args[idx].numel() == 1) {
            targetFig = scene.findFigureByNumber(keel::as<keel::i32>(args[idx].elemAsDouble(0)));
            ++idx;
        }
    }
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        if (asciiEqualNoCase(args[idx].toString(), "reset"))
            doReset = true;
    }
    if (targetFig == kInvalidNodeId)
        targetFig = scene.ensureCurrentFigure();

    scene.clearFigure(targetFig, doReset);
    if (const GraphicsNode *fn = scene.findNode(targetFig); fn && fn->figureNumber > 0) {
        fm.invalidateHandles(fn->figureNumber, -1, true, false);
        if (auto *fs = fm.figureOf(fn->figureNumber)) {
            fs->superTitle.clear();
            fs->axes.clear();
            fs->axes.push_back(AxesState{});
            fs->currentAxes = 0;
            fs->subplotRows = 0;
            fs->subplotCols = 0;
            fs->modified = true;
        }
        fm.emitModified();
    }
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(targetFig, mr) : Value();
}

void close_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto &fm = ctx.engine->figureManager();
    auto *mr = ctx.engine->allocator();

    if (args.empty()) {
        const NodeId cur = scene.currentFigureId();
        if (cur != kInvalidNodeId) {
            if (const GraphicsNode *fn = scene.findNode(cur); fn && fn->figureNumber > 0)
                fm.closeFigureNotify(fn->figureNumber);
            scene.closeFigure(cur, false);
        } else {
            fm.closeCurrentNotify();
        }
        outs[0] = (nargout >= 1) ? Value::scalar(1.0, mr) : Value();
        return;
    }

    if (args[0].isChar() || args[0].isString()) {
        const keel::String tok = args[0].toString();
        if (asciiEqualNoCase(tok, "all")) {
            bool hidden = false;
            bool force = false;
            for (keel::usize i = 1; i < args.size(); ++i) {
                if (!args[i].isChar() && !args[i].isString()) continue;
                const keel::String opt = args[i].toString();
                if (asciiEqualNoCase(opt, "hidden")) hidden = true;
                if (asciiEqualNoCase(opt, "force"))  force = true;
            }
            scene.closeAllFigures(hidden, force);
            fm.closeAllNotify();
            outs[0] = (nargout >= 1) ? Value::scalar(1.0, mr) : Value();
            return;
        }
        // Close figure by Name.
        auto figs = scene.allChildren(kRootNodeId, mr);
        for (NodeId fid : figs) {
            const Value nv = scene.getProperty(fid, "Name", mr);
            if (nv.toString() == tok) {
                if (const GraphicsNode *fn = scene.findNode(fid); fn && fn->figureNumber > 0)
                    fm.closeFigureNotify(fn->figureNumber);
                scene.closeFigure(fid, false);
            }
        }
        outs[0] = (nargout >= 1) ? Value::scalar(1.0, mr) : Value();
        return;
    }

    if (args[0].isObject()) {
        for (keel::usize k = 0; k < args[0].objectCount(); ++k) {
            keel::i32 id = -1;
            bool inSc = false;
            if (!graphicsHandleAt(args[0], k, id, inSc))
                continue;
            if (inSc) {
                if (id == kPlaceholderNodeId) continue;
                const GraphicsNode *fn = scene.findNode(id);
                if (!fn)
                    throw Error("Invalid or deleted object.", 0, 0, "close", "", "numkit:graphics:InvalidHandle");
                if (fn->classId == ClassId::Figure) {
                    if (fn->figureNumber > 0)
                        fm.closeFigureNotify(fn->figureNumber);
                    scene.closeFigure(id, false);
                }
            } else if (auto *rec = fm.findHandle(id)) {
                if (rec->type == "figure")
                    fm.closeFigureNotify(rec->figureId);
                else if (rec->type == "waitbar")
                    rec->deleted = true;
            }
        }
        outs[0] = (nargout >= 1) ? Value::scalar(1.0, mr) : Value();
        return;
    }

    if (args[0].isNumeric()) {
        for (keel::usize k = 0; k < args[0].numel(); ++k) {
            const keel::i32 num = keel::as<keel::i32>(args[0].elemAsDouble(k));
            const NodeId fid = scene.findFigureByNumber(num);
            if (fid != kInvalidNodeId)
                scene.closeFigure(fid, false);
            fm.closeFigureNotify(num);
        }
    }
    outs[0] = (nargout >= 1) ? Value::scalar(1.0, mr) : Value();
}

void allchild_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "allchild");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId id = kInvalidNodeId;
    if (args[0].isNumeric() && args[0].numel() == 1 && args[0].elemAsDouble(0) == 0.0) {
        id = kRootNodeId;
    } else if (!extractSceneHandle(args[0], id)) {
        outs[0] = scene.makeEmptyPlaceholder(mr);
        return;
    }
    auto kids = scene.allChildren(id, mr);
    outs[0] = scene.makeHandleColumn(kids.as_ref(), mr);
}

void ancestor_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "ancestor");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId id = kInvalidNodeId;
    if (!extractSceneHandle(args[0], id)) {
        outs[0] = scene.makeEmptyPlaceholder(mr);
        return;
    }
    keel::Array<keel::String> types(mr);
    if (args[1].isChar() || args[1].isString()) {
        types.push_back(args[1].toString());
    } else if (args[1].isCell()) {
        for (keel::usize i = 0; i < args[1].numel(); ++i)
            types.push_back(args[1].cellAt(i).toString());
    } else {
        throw Error("ancestor: second argument must be a type name or cell array of type names.",
                    0, 0, "ancestor", "", "numkit:graphics:InvalidArgument");
    }
    bool toplevel = false;
    if (args.size() == 3 && (args[2].isChar() || args[2].isString())) {
        if (asciiEqualNoCase(args[2].toString(), "toplevel"))
            toplevel = true;
    }
    const NodeId hit = scene.ancestor(id, types.as_ref(), toplevel);
    outs[0] = scene.makeHandleValue(hit, mr);
}

void copyobj_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 2, "copyobj");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    NodeId parentId = kInvalidNodeId;
    if (args[1].isNumeric() && args[1].numel() == 1 && args[1].elemAsDouble(0) == 0.0) {
        parentId = kRootNodeId;
    } else if (!extractSceneHandle(args[1], parentId)) {
        throw Error("copyobj: second argument must be a valid parent graphics handle.",
                    0, 0, "copyobj", "", "numkit:graphics:InvalidParent");
    }

    if (args[0].isObject() && args[0].objectCount() > 1) {
        keel::Array<NodeId> created(mr);
        created.reserve(args[0].objectCount());
        for (keel::usize k = 0; k < args[0].objectCount(); ++k) {
            keel::i32 srcId = -1;
            bool inSc = false;
            if (!graphicsHandleAt(args[0], k, srcId, inSc) || !inSc)
                throw Error("copyobj: invalid source handle.", 0, 0, "copyobj", "", "numkit:graphics:InvalidHandle");
            created.push_back(scene.copyNode(srcId, parentId));
        }
        outs[0] = scene.makeHandleColumn(created.as_ref(), mr);
        return;
    }
    NodeId srcId = kInvalidNodeId;
    if (!extractSceneHandle(args[0], srcId))
        throw Error("copyobj: first argument must be a valid graphics handle.",
                    0, 0, "copyobj", "", "numkit:graphics:InvalidHandle");
    outs[0] = scene.makeHandleValue(scene.copyNode(srcId, parentId), mr);
}

void reset_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "reset");
    auto &scene = coreEngine(ctx).graphicsScene();
    if (args[0].isObject() && args[0].objectCount() > 0) {
        for (keel::usize k = 0; k < args[0].objectCount(); ++k) {
            keel::i32 id = -1;
            bool inSc = false;
            if (graphicsHandleAt(args[0], k, id, inSc) && inSc && id != kPlaceholderNodeId)
                scene.resetNode(id);
        }
    }
    outs[0] = Value();
}

} // anonymous namespace

// ── Exported hooks called by graphics_library.cpp ───────────────────────

bool trySceneGet(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        return false;
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    // Numeric 0 == groot
    if (args[0].isNumeric() && args[0].numel() == 1 && args[0].elemAsDouble(0) == 0.0) {
        if (args.size() == 1) {
            outs[0] = scene.getAllPropertiesStruct(kRootNodeId, mr);
            return true;
        }
        if (args[1].isChar() || args[1].isString()) {
            outs[0] = scene.getProperty(kRootNodeId, args[1].toString(), mr);
            return true;
        }
        if (args[1].isCell()) {
            const keel::usize np = args[1].numel();
            Value res = Value::cell(1, np, mr);
            for (keel::usize j = 0; j < np; ++j)
                res.cellAt(j) = scene.getProperty(kRootNodeId, args[1].cellAt(j).toString(), mr);
            outs[0] = keel::move(res);
            return true;
        }
        return false;
    }

    if (!isSceneGraphicsHandle(args[0]))
        return false;

    const keel::usize nObjs = args[0].objectCount();
    if (nObjs == 1) {
        const NodeId id = graphicsHandleIdOf(args[0]);
        if (args.size() == 1) {
            outs[0] = scene.getAllPropertiesStruct(id, mr);
            return true;
        }
        if (args[1].isChar() || args[1].isString()) {
            outs[0] = scene.getProperty(id, args[1].toString(), mr);
            return true;
        }
        if (args[1].isCell()) {
            const keel::usize np = args[1].numel();
            Value res = Value::cell(1, np, mr);
            for (keel::usize j = 0; j < np; ++j)
                res.cellAt(j) = scene.getProperty(id, args[1].cellAt(j).toString(), mr);
            outs[0] = keel::move(res);
            return true;
        }
        throw Error("get: property name must be text or a cell array of property names.",
                    0, 0, "get", "", "numkit:graphics:InvalidPropName");
    }

    // Vector handle array: get(hArr, 'Prop') -> N x 1 cell; get(hArr, {'P1','P2'}) -> N x M cell.
    if (args.size() == 1) {
        throw Error("get: single-argument struct form requires a scalar handle.",
                    0, 0, "get", "", "numkit:graphics:NonScalarHandle");
    }
    if (args[1].isChar() || args[1].isString()) {
        const keel::String prop = args[1].toString();
        Value res = Value::cell(nObjs, 1, mr);
        for (keel::usize i = 0; i < nObjs; ++i) {
            keel::i32 id = -1;
            bool inSc = false;
            if (!graphicsHandleAt(args[0], i, id, inSc) || !inSc)
                throw Error("Invalid or deleted object.", 0, 0, "get", "", "numkit:graphics:InvalidHandle");
            res.cellAt(i) = scene.getProperty(id, prop, mr);
        }
        outs[0] = keel::move(res);
        return true;
    }
    if (args[1].isCell()) {
        const keel::usize np = args[1].numel();
        Value res = Value::cell(nObjs, np, mr);
        for (keel::usize j = 0; j < np; ++j) {
            const keel::String prop = args[1].cellAt(j).toString();
            for (keel::usize i = 0; i < nObjs; ++i) {
                keel::i32 id = -1;
                bool inSc = false;
                if (!graphicsHandleAt(args[0], i, id, inSc) || !inSc)
                    throw Error("Invalid or deleted object.", 0, 0, "get", "", "numkit:graphics:InvalidHandle");
                res.cellAt(i + j * nObjs) = scene.getProperty(id, prop, mr);
            }
        }
        outs[0] = keel::move(res);
        return true;
    }
    return false;
}

bool trySceneSet(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        return false;
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::Array<NodeId> targets(mr);
    if (args[0].isNumeric() && args[0].numel() == 1 && args[0].elemAsDouble(0) == 0.0) {
        targets.push_back(kRootNodeId);
    } else if (isSceneGraphicsHandle(args[0])) {
        const keel::usize nObjs = args[0].objectCount();
        targets.reserve(nObjs);
        for (keel::usize i = 0; i < nObjs; ++i) {
            keel::i32 id = -1;
            bool inSc = false;
            if (!graphicsHandleAt(args[0], i, id, inSc) || !inSc)
                throw Error("Invalid or deleted object.", 0, 0, "set", "", "numkit:graphics:InvalidHandle");
            targets.push_back(id);
        }
    } else {
        return false;
    }

    if (args.size() == 1) {
        if (targets.size() != 1)
            throw Error("set: query form requires a scalar handle.", 0, 0, "set", "", "numkit:graphics:NonScalarHandle");
        Value spec = scene.getSettableSpec(targets[0], {}, false, mr);
        if (nargout == 0 && spec.isStruct()) {
            const auto fields = spec.fieldNamesInOrder(mr);
            keel::String buf(mr);
            for (const auto &fn : fields) {
                buf.append("    ");
                buf.append(fn);
                buf.push_back('\n');
            }
            coreEngine(ctx).outputText(buf);
            outs[0] = Value();
        } else {
            outs[0] = keel::move(spec);
        }
        return true;
    }
    if (args.size() == 2 && (args[1].isChar() || args[1].isString())) {
        if (targets.size() != 1)
            throw Error("set: query form requires a scalar handle.", 0, 0, "set", "", "numkit:graphics:NonScalarHandle");
        Value spec = scene.getSettableSpec(targets[0], args[1].toString(), true, mr);
        if (nargout == 0 && spec.isCell()) {
            keel::String buf(mr);
            for (keel::usize i = 0; i < spec.numel(); ++i) {
                buf.append("    ");
                buf.append(spec.cellAt(i).toString());
                buf.push_back('\n');
            }
            coreEngine(ctx).outputText(buf);
            outs[0] = Value();
        } else {
            outs[0] = keel::move(spec);
        }
        return true;
    }
    // Cell property names + cell values: set(hArr, {'P1','P2'}, valsCell)
    if (args.size() == 3 && args[1].isCell() && args[2].isCell()) {
        const keel::usize np = args[1].numel();
        const keel::usize nh = targets.size();
        const keel::usize vr = args[2].dims().rows();
        const keel::usize vc = args[2].dims().cols();
        if (vr != nh || vc != np) {
            throw Error("set: cell array of values must match [numHandles x numProperties].",
                        0, 0, "set", "", "numkit:graphics:CellSizeMismatch");
        }
        for (keel::usize j = 0; j < np; ++j) {
            const keel::String prop = args[1].cellAt(j).toString();
            for (keel::usize i = 0; i < nh; ++i)
                scene.setProperty(targets[i], prop, args[2].cellAt(i + j * vr), mr);
        }
        outs[0] = Value();
        return true;
    }

    for (NodeId id : targets)
        applyNameValuePairs(scene, id, args, 1, mr);
    outs[0] = Value();
    return true;
}

bool trySceneIsValid(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty() || !args[0].isObject())
        return false;
    auto *mr = ctx.engine->allocator();
    const keel::usize n = args[0].objectCount();
    if (n == 0) {
        if (args[0].objectClassNameRef() == "matlab.graphics.GraphicsPlaceholder") {
            outs[0] = Value::matrix(args[0].dims().rows(), args[0].dims().cols(), ValueType::LOGICAL, mr);
            return true;
        }
        return false;
    }
    if (!isSceneGraphicsHandle(args[0]))
        return false;
    const auto &scene = coreEngine(ctx).graphicsScene();
    Value res = Value::matrix(args[0].dims().rows(), args[0].dims().cols(), ValueType::LOGICAL, mr);
    keel::u8 *dst = res.logicalDataMut();
    for (keel::usize i = 0; i < n; ++i) {
        keel::i32 id = -1;
        bool inSc = false;
        if (graphicsHandleAt(args[0], i, id, inSc) && inSc)
            dst[i] = scene.isValidNode(id) ? 1u : 0u;
        else
            dst[i] = 0u;
    }
    outs[0] = keel::move(res);
    return true;
}

bool trySceneIsGraphics(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        return false;
    auto *mr = ctx.engine->allocator();
    if (args[0].isObject() && args[0].objectCount() == 0
        && args[0].objectClassNameRef() == "matlab.graphics.GraphicsPlaceholder") {
        outs[0] = Value::matrix(args[0].dims().rows(), args[0].dims().cols(), ValueType::LOGICAL, mr);
        return true;
    }
    if (!isSceneGraphicsHandle(args[0]))
        return false;

    const auto &scene = coreEngine(ctx).graphicsScene();
    const bool checkType = (args.size() >= 2 && (args[1].isChar() || args[1].isString()));
    const keel::String wantType = checkType ? args[1].toString() : keel::String{};
    const keel::usize n = args[0].objectCount();
    Value res = Value::matrix(args[0].dims().rows(), args[0].dims().cols(), ValueType::LOGICAL, mr);
    keel::u8 *dst = res.logicalDataMut();
    for (keel::usize i = 0; i < n; ++i) {
        keel::i32 id = -1;
        bool inSc = false;
        if (!graphicsHandleAt(args[0], i, id, inSc) || !inSc || id == kPlaceholderNodeId) {
            dst[i] = 0u;
            continue;
        }
        const GraphicsNode *node = scene.findNode(id);
        if (!node) {
            dst[i] = 0u;
            continue;
        }
        if (!checkType) {
            dst[i] = 1u;
        } else {
            const ClassSchema &s = schemaFor(node->classId);
            dst[i] = (asciiEqualNoCase(s.typeName, wantType)
                      || asciiEqualNoCase(s.className, wantType)
                      || asciiEqualNoCase(wantType, "graphics")) ? 1u : 0u;
        }
    }
    outs[0] = keel::move(res);
    return true;
}

bool trySceneIsHandle(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        return false;
    if (args[0].isNumeric() && args[0].numel() == 1 && args[0].elemAsDouble(0) == 0.0) {
        outs[0] = Value::logicalScalar(true, ctx.engine->allocator());
        return true;
    }
    return trySceneIsGraphics(args, outs, ctx);
}

bool trySceneFind(keel::ArrayRef<Value> args, bool includeHidden,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::Array<NodeId> roots(mr);
    keel::usize idx = 0;
    if (!args.empty()) {
        if (args[0].isNumeric() && args[0].numel() == 1 && args[0].elemAsDouble(0) == 0.0) {
            roots.push_back(kRootNodeId);
            idx = 1;
        } else if (isSceneGraphicsHandle(args[0])) {
            for (keel::usize i = 0; i < args[0].objectCount(); ++i) {
                keel::i32 id = -1;
                bool inSc = false;
                if (graphicsHandleAt(args[0], i, id, inSc) && inSc && scene.findNode(id))
                    roots.push_back(id);
            }
            idx = 1;
        } else if (graphicsHandleIdOf(args[0]) >= 0) {
            // Legacy FigureManager handle — let legacy findobj handle it.
            return false;
        }
    }
    if (roots.empty()) {
        // Default root scope: only claim if caller didn't pass legacy-only chart type.
        roots.push_back(kRootNodeId);
    }

    // Parse options (-depth, -flat, -not, -and, -or, -xor, -property, -isa, -class, -regexp, Prop/Val).
    enum class TermKind : keel::u8 { PropEq, PropRegexp, HasProp, IsClass };
    struct Term {
        TermKind kind = TermKind::PropEq;
        keel::String name;
        Value want;
        bool negated = false;
        keel::u8 combineOp = 0; // 0=AND, 1=OR, 2=XOR
    };

    keel::i32 maxDepth = -1;
    keel::Array<Term> terms(mr);
    bool nextNeg = false;
    keel::u8 nextOp = 0;

    while (idx < args.size()) {
        if (!args[idx].isChar() && !args[idx].isString()) {
            ++idx;
            continue;
        }
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "-flat")) {
            maxDepth = 0;
            ++idx;
            continue;
        }
        if (asciiEqualNoCase(tok, "-depth") && idx + 1 < args.size()) {
            maxDepth = keel::as<keel::i32>(args[idx + 1].toScalar());
            idx += 2;
            continue;
        }
        if (asciiEqualNoCase(tok, "-not")) { nextNeg = !nextNeg; ++idx; continue; }
        if (asciiEqualNoCase(tok, "-and")) { nextOp = 0; ++idx; continue; }
        if (asciiEqualNoCase(tok, "-or"))  { nextOp = 1; ++idx; continue; }
        if (asciiEqualNoCase(tok, "-xor")) { nextOp = 2; ++idx; continue; }
        if (asciiEqualNoCase(tok, "-property") && idx + 1 < args.size()) {
            Term t;
            t.kind = TermKind::HasProp;
            t.name = args[idx + 1].toString();
            t.negated = nextNeg;
            t.combineOp = nextOp;
            terms.push_back(keel::move(t));
            nextNeg = false; nextOp = 0;
            idx += 2;
            continue;
        }
        if ((asciiEqualNoCase(tok, "-isa") || asciiEqualNoCase(tok, "-class")) && idx + 1 < args.size()) {
            Term t;
            t.kind = TermKind::IsClass;
            t.name = args[idx + 1].toString();
            t.negated = nextNeg;
            t.combineOp = nextOp;
            terms.push_back(keel::move(t));
            nextNeg = false; nextOp = 0;
            idx += 2;
            continue;
        }
        if (asciiEqualNoCase(tok, "-regexp") && idx + 2 < args.size()) {
            Term t;
            t.kind = TermKind::PropRegexp;
            t.name = args[idx + 1].toString();
            t.want = args[idx + 2];
            t.negated = nextNeg;
            t.combineOp = nextOp;
            terms.push_back(keel::move(t));
            nextNeg = false; nextOp = 0;
            idx += 3;
            continue;
        }
        if (idx + 1 < args.size()) {
            // All Phase 1-3 object types live in GraphicsScene now; only defer unknown legacy types.
            if (asciiEqualNoCase(tok, "Type") && (args[idx + 1].isChar() || args[idx + 1].isString())) {
                const keel::String wantT = args[idx + 1].toString();
                if (schemaByTypeName(wantT) == nullptr && !asciiEqualNoCase(wantT, "theme"))
                    return false;
            }
            Term t;
            t.kind = TermKind::PropEq;
            t.name = tok.clone(mr);
            t.want = args[idx + 1];
            t.negated = nextNeg;
            t.combineOp = nextOp;
            terms.push_back(keel::move(t));
            nextNeg = false; nextOp = 0;
            idx += 2;
            continue;
        }
        ++idx;
    }

    keel::Array<NodeId> candidates(mr);
    for (NodeId r : roots)
        collectSubtree(scene, r, includeHidden, maxDepth, 0, candidates);

    keel::Array<NodeId> hits(mr);
    for (NodeId nid : candidates) {
        const GraphicsNode *n = scene.findNode(nid);
        if (!n) continue;
        bool acc = true;
        for (keel::usize ti = 0; ti < terms.size(); ++ti) {
            const Term &t = terms[ti];
            bool ok = false;
            switch (t.kind) {
            case TermKind::HasProp:
                ok = scene.hasProperty(nid, t.name);
                break;
            case TermKind::IsClass: {
                const ClassSchema &s = schemaFor(n->classId);
                ok = asciiEqualNoCase(s.className, t.name)
                     || asciiEqualNoCase(s.typeName, t.name)
                     || asciiEqualNoCase(t.name, "matlab.graphics.Graphics")
                     || asciiEqualNoCase(t.name, "handle");
                break;
            }
            case TermKind::PropEq:
                if (scene.hasProperty(nid, t.name)) {
                    const Value actual = scene.getProperty(nid, t.name, mr);
                    ok = valuesEqualForFind(actual, t.want);
                }
                break;
            case TermKind::PropRegexp:
                if (scene.hasProperty(nid, t.name)) {
                    const Value actual = scene.getProperty(nid, t.name, mr);
                    if (actual.isChar() || actual.isString())
                        ok = matchSimpleRegex(actual.toString(), t.want.toString());
                }
                break;
            }
            if (t.negated) ok = !ok;
            if (ti == 0) acc = ok;
            else if (t.combineOp == 0) acc = acc && ok;
            else if (t.combineOp == 1) acc = acc || ok;
            else acc = (acc != ok);
        }
        if (acc)
            hits.push_back(nid);
    }

    // If no hits and there are legacy FigureManager handles while caller searched without explicit scene root,
    // allow fallback only when no explicit scene handle was passed.
    if (hits.empty() && (args.empty() || !isSceneGraphicsHandle(args[0]))
        && !ctx.engine->figureManager().handles().empty()) {
        return false;
    }

    outs[0] = scene.makeHandleColumn(hits.as_ref(), mr);
    return true;
}

void registerSceneAxesBuiltins(Engine &engine);
void registerSceneChartsBuiltins(Engine &engine);

void registerSceneGraphics(Engine &engine)
{
    registerOneSceneClass(engine, ClassId::Root);
    registerOneSceneClass(engine, ClassId::Figure);
    registerOneSceneClass(engine, ClassId::Theme);
    registerOneSceneClass(engine, ClassId::Placeholder);
    registerOneSceneClass(engine, ClassId::Axes);
    registerOneSceneClass(engine, ClassId::NumericRuler);
    registerOneSceneClass(engine, ClassId::PolarAxes);
    registerOneSceneClass(engine, ClassId::TiledChartLayout);
    registerOneSceneClass(engine, ClassId::Text);
    registerOneSceneClass(engine, ClassId::Legend);
    registerOneSceneClass(engine, ClassId::ColorBar);
    registerOneSceneClass(engine, ClassId::ConstantLine);
    registerOneSceneClass(engine, ClassId::Line);
    registerOneSceneClass(engine, ClassId::Scatter);
    registerOneSceneClass(engine, ClassId::Bar);
    registerOneSceneClass(engine, ClassId::Surface);
    registerOneSceneClass(engine, ClassId::Patch);
    registerOneSceneClass(engine, ClassId::Quiver);
    registerOneSceneClass(engine, ClassId::Contour);
    registerOneSceneClass(engine, ClassId::Stem);
    registerOneSceneClass(engine, ClassId::ErrorBar);
    registerOneSceneClass(engine, ClassId::Histogram);
    registerOneSceneClass(engine, ClassId::Area);
    registerOneSceneClass(engine, ClassId::Image);
    registerOneSceneClass(engine, ClassId::Stair);
    registerOneSceneClass(engine, ClassId::AnimatedLine);
    registerOneSceneClass(engine, ClassId::TextBox);
    registerOneSceneClass(engine, ClassId::Light);
    registerOneSceneClass(engine, ClassId::BoxChart);
    registerOneSceneClass(engine, ClassId::Histogram2);
    registerOneSceneClass(engine, ClassId::HeatmapChart);
    registerOneSceneClass(engine, ClassId::Rectangle);
    registerOneSceneClass(engine, ClassId::Group);
    registerOneSceneClass(engine, ClassId::FunctionLine);
    registerOneSceneClass(engine, ClassId::ParameterizedFunctionLine);

    auto regBoth = [&](const char *sub, const char *name,
                       void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &)) {
        engine.registerFunction("", name, ExternalFunc(fn));
        keel::SmallString<64> ns("graphics.");
        ns.append(sub);
        engine.registerFunction(ns, name, ExternalFunc(fn));
    };

    regBoth("layout", "figure",   &figure_scene_reg);
    regBoth("layout", "gcf",      &gcf_scene_reg);
    regBoth("layout", "clf",      &clf_scene_reg);
    regBoth("layout", "close",    &close_scene_reg);
    regBoth("handle", "groot",    &groot_scene_reg);
    regBoth("handle", "gobjects", &gobjects_scene_reg);
    regBoth("handle", "allchild", &allchild_scene_reg);
    regBoth("handle", "ancestor", &ancestor_scene_reg);
    regBoth("handle", "copyobj",  &copyobj_scene_reg);
    regBoth("handle", "reset",    &reset_scene_reg);

    registerSceneAxesBuiltins(engine);
    registerSceneChartsBuiltins(engine);
}

} // namespace numkit::gfx::detail
