// src/builtin/register/graphics/scene_extra_builtins.cpp
//
// Phase 5 migration of all remaining secondary chart, 3-D volume, geographic,
// camera, and layout builtins onto the Model-First GraphicsScene.

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/static_array.h>
#include <keel/string.h>

#include <numkit/core/engine.hpp>
#include <numkit/graphics/graphics_handle.hpp>
#include <numkit/graphics/graphics_scene.hpp>
#include <numkit/value/error.hpp>

namespace numkit::gfx::detail {

namespace {

bool extractSceneHandle(const Value &v, NodeId &out) noexcept
{
    if (!isSceneGraphicsHandle(v) || v.objectCount() != 1)
        return false;
    out = graphicsHandleIdOf(v);
    return true;
}

bool isAxesLike(const GraphicsScene &scene, NodeId id) noexcept
{
    const GraphicsNode *n = scene.findNode(id);
    return n && (n->classId == ClassId::Axes || n->classId == ClassId::PolarAxes);
}

NodeId resolveLeadingAxesOpt(const GraphicsScene &scene, keel::ArrayRef<Value> args,
                             keel::usize &startIdx) noexcept
{
    if (startIdx < args.size()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[startIdx], h) && isAxesLike(scene, h)) {
            ++startIdx;
            return h;
        }
    }
    return kInvalidNodeId;
}

bool forwardExternal(CallContext &ctx, keel::StringRef name, keel::ArrayRef<Value> args,
                     keel::usize nargout, keel::MutArrayRef<Value> outs)
{
    const ExternalFunc *fn = ctx.engine->findExternal(name, ctx.env);
    if (!fn) return false;
    (*fn)(args, nargout, outs, ctx);
    return true;
}

keel::f64 normInvApprox(keel::f64 p) noexcept
{
    p = keel::clamp(p, 1e-15, 1.0 - 1e-15);
    constexpr keel::f64 a1 = -3.969683028665376e+01, a2 =  2.209460984245205e+02,
                        a3 = -2.759285104469687e+02, a4 =  1.383577518672690e+02,
                        a5 = -3.066479806614716e+01, a6 =  2.506628277459239e+00;
    constexpr keel::f64 b1 = -5.447609879822406e+01, b2 =  1.615858368580409e+02,
                        b3 = -1.556989798598866e+02, b4 =  6.680131188771972e+01,
                        b5 = -1.328068155288572e+01;
    constexpr keel::f64 c1 = -7.784894002430293e-03, c2 = -3.223964580411365e-01,
                        c3 = -2.400758277161838e+00, c4 = -2.549732539343734e+00,
                        c5 =  4.374664141464968e+00, c6 =  2.938163982698783e+00;
    constexpr keel::f64 d1 =  7.784695709041462e-03, d2 =  3.224671290700398e-01,
                        d3 =  2.445134137142996e+00, d4 =  3.754408661907416e+00;
    constexpr keel::f64 pLow = 0.02425, pHigh = 1.0 - pLow;
    if (p < pLow) {
        const keel::f64 q = keel::sqrt(-2.0 * keel::log(p));
        return (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6)
             / ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    }
    if (p <= pHigh) {
        const keel::f64 q = p - 0.5;
        const keel::f64 r = q * q;
        return (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q
             / (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
    }
    const keel::f64 q = keel::sqrt(-2.0 * keel::log(1.0 - p));
    return -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6)
          / ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
}

Value toCallableHandle(const Value &v, CallContext &ctx, const char *varList, keel::Allocator *mr)
{
    if (v.isFuncHandle()) return v;
    if (v.isObject() && (v.objectClassNameRef() == "sym" || v.objectClassNameRef() == "symfun")) {
        const ExternalFunc *mf = ctx.engine->findExternal("matlabFunction", ctx.env);
        if (!mf) mf = ctx.engine->findExternal("cas.matlabFunction", ctx.env);
        if (mf) {
            Value outV[1];
            (*mf)(keel::ArrayRef<Value>(&v, 1), 1, keel::MutArrayRef<Value>(outV, 1), ctx);
            return outV[0];
        }
    }
    if (v.isChar() || v.isString()) {
        const keel::String expr = v.toString();
        if (const ExternalFunc *s2f = ctx.engine->findExternal("str2func", ctx.env)) {
            keel::String body(mr);
            if (expr.find('@') == keel::kInvalidIndex) {
                body.append("@(");
                body.append(varList);
                body.append(") ");
                body.append(expr);
            } else {
                body.append(expr);
            }
            Value sArg = Value::fromString(body, mr);
            Value outV[1];
            (*s2f)(keel::ArrayRef<Value>(&sArg, 1), 1, keel::MutArrayRef<Value>(outV, 1), ctx);
            return outV[0];
        }
    }
    return Value();
}

void setView3D(GraphicsScene &scene, NodeId axId, keel::Allocator *mr)
{
    Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    vw.doubleDataMut()[0] = -37.5;
    vw.doubleDataMut()[1] = 30.0;
    scene.setProperty(axId, "View", vw, mr);
}

// ── Layout / Camera / Interaction builtins ──────────────────────────────

void waitbar_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    NodeId fid = kInvalidNodeId;
    if (args.size() >= 2 && extractSceneHandle(args[1], fid) && scene.findNode(fid)) {
        if (args.size() >= 3 && (args[2].isChar() || args[2].isString()))
            scene.setProperty(fid, "Name", args[2], mr);
    } else {
        fid = scene.createFigure();
        if (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
            scene.setProperty(fid, "Name", args[1], mr);
    }
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(fid, mr) : Value();
}

void whitebg_scene_reg(keel::ArrayRef<Value>, keel::usize,
                       keel::MutArrayRef<Value> outs, CallContext &)
{
    outs[0] = Value();
}

void colordef_scene_reg(keel::ArrayRef<Value>, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (nargout >= 1) {
        const NodeId fid = scene.ensureCurrentFigure();
        outs[0] = scene.makeHandleValue(fid, mr);
    } else {
        outs[0] = Value();
    }
}

void dirCore(const char *prop, keel::ArrayRef<Value> args, keel::usize nargout,
             keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = (leadingAx != kInvalidNodeId) ? leadingAx : scene.ensureCurrentAxes();
    if (idx >= args.size()) {
        outs[0] = scene.getProperty(axId, prop, mr);
        return;
    }
    if (args[idx].isChar() || args[idx].isString())
        scene.setProperty(axId, prop, args[idx], mr);
    outs[0] = (nargout >= 1) ? scene.getProperty(axId, prop, mr) : Value();
}

void xdir_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ dirCore("XDir", a, no, o, c); }
void ydir_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ dirCore("YDir", a, no, o, c); }
void colorscale_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ dirCore("ColorScale", a, no, o, c); }

void aspectCore(const char *prop, const char *modeProp, keel::ArrayRef<Value> args,
                keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = (leadingAx != kInvalidNodeId) ? leadingAx : scene.ensureCurrentAxes();
    if (idx >= args.size()) {
        outs[0] = scene.getProperty(axId, prop, mr);
        return;
    }
    if (args[idx].isChar() || args[idx].isString()) {
        const keel::String s = args[idx].toString();
        if (asciiEqualNoCase(s, "mode")) {
            outs[0] = scene.getProperty(axId, modeProp, mr);
            return;
        }
        if (asciiEqualNoCase(s, "auto") || asciiEqualNoCase(s, "manual"))
            scene.setProperty(axId, modeProp, args[idx], mr);
    } else if (args[idx].isNumeric() && args[idx].numel() >= 3) {
        scene.setProperty(axId, prop, args[idx], mr);
    }
    outs[0] = Value();
}

void daspect_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ aspectCore("DataAspectRatio", "DataAspectRatioMode", a, no, o, c); }
void pbaspect_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ aspectCore("PlotBoxAspectRatio", "PlotBoxAspectRatioMode", a, no, o, c); }

void tickFormatCore(char axis, keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = (leadingAx != kInvalidNodeId) ? leadingAx : scene.ensureCurrentAxes();
    const GraphicsNode *ax = scene.findNode(axId);
    NodeId rulerId = kInvalidNodeId;
    if (ax) {
        if (axis == 'x') rulerId = ax->xAxisId;
        else if (axis == 'y') rulerId = ax->yAxisId;
        else rulerId = ax->zAxisId;
    }
    if (idx >= args.size()) {
        outs[0] = (rulerId != kInvalidNodeId) ? scene.getProperty(rulerId, "TickLabelFormat", mr)
                                              : Value::fromString("%g", mr);
        return;
    }
    if (rulerId != kInvalidNodeId && (args[idx].isChar() || args[idx].isString())) {
        const keel::String s = args[idx].toString();
        scene.setProperty(rulerId, "TickLabelFormat",
                          asciiEqualNoCase(s, "auto") ? Value::fromString("%g", mr) : args[idx], mr);
    }
    outs[0] = (nargout >= 1 && rulerId != kInvalidNodeId)
                  ? scene.getProperty(rulerId, "TickLabelFormat", mr) : Value();
}

void xtickformat_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickFormatCore('x', a, no, o, c); }
void ytickformat_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickFormatCore('y', a, no, o, c); }
void ztickformat_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickFormatCore('z', a, no, o, c); }

void tickAngleCore(const char *prop, keel::ArrayRef<Value> args, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = (leadingAx != kInvalidNodeId) ? leadingAx : scene.ensureCurrentAxes();
    if (idx >= args.size()) {
        outs[0] = scene.getProperty(axId, prop, mr);
        return;
    }
    if (args[idx].isNumeric())
        scene.setProperty(axId, prop, args[idx], mr);
    outs[0] = Value();
}

void xtickangle_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickAngleCore("XTickLabelRotation", a, no, o, c); }
void ytickangle_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ tickAngleCore("YTickLabelRotation", a, no, o, c); }

void camPropCore(const char *prop, const char *modeProp, keel::ArrayRef<Value> args,
                 keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = (leadingAx != kInvalidNodeId) ? leadingAx : scene.ensureCurrentAxes();
    if (idx >= args.size()) {
        outs[0] = scene.getProperty(axId, prop, mr);
        return;
    }
    if (args[idx].isChar() || args[idx].isString()) {
        const keel::String s = args[idx].toString();
        if (modeProp && asciiEqualNoCase(s, "mode")) {
            outs[0] = scene.getProperty(axId, modeProp, mr);
            return;
        }
        if (modeProp && (asciiEqualNoCase(s, "auto") || asciiEqualNoCase(s, "manual")))
            scene.setProperty(axId, modeProp, args[idx], mr);
        else
            scene.setProperty(axId, prop, args[idx], mr);
    } else {
        scene.setProperty(axId, prop, args[idx], mr);
    }
    outs[0] = Value();
}

void campos_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ camPropCore("CameraPosition", "CameraPositionMode", a, no, o, c); }
void camtarget_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ camPropCore("CameraTarget", "CameraTargetMode", a, no, o, c); }
void camup_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ camPropCore("CameraUpVector", "CameraUpVectorMode", a, no, o, c); }
void camva_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ camPropCore("CameraViewAngle", "CameraViewAngleMode", a, no, o, c); }
void camproj_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ camPropCore("Projection", nullptr, a, no, o, c); }

void sceneNoop_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &)
{
    outs[0] = Value();
}

void sceneNoopRet1_reg(keel::ArrayRef<Value>, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    outs[0] = (nargout >= 1) ? Value::scalar(1.0, ctx.engine->allocator()) : Value();
}

void lighting_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = (leadingAx != kInvalidNodeId) ? leadingAx : scene.currentAxesId();
    if (axId != kInvalidNodeId && idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        if (const GraphicsNode *ax = scene.findNode(axId)) {
            for (NodeId cid : ax->children) {
                if (const GraphicsNode *ch = scene.findNode(cid);
                    ch && (ch->classId == ClassId::Surface || ch->classId == ClassId::Patch)) {
                    scene.setProperty(cid, "FaceLighting", args[idx], mr);
                }
            }
        }
    }
    outs[0] = Value();
}

void material_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = (leadingAx != kInvalidNodeId) ? leadingAx : scene.currentAxesId();
    if (axId != kInvalidNodeId && idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String m = args[idx].toString();
        keel::f64 ka = 0.3, kd = 0.6, ks = 0.9;
        if (asciiEqualNoCase(m, "dull"))       { ka = 0.3; kd = 0.8; ks = 0.0; }
        else if (asciiEqualNoCase(m, "metal")) { ka = 0.3; kd = 0.3; ks = 1.0; }
        if (const GraphicsNode *ax = scene.findNode(axId)) {
            for (NodeId cid : ax->children) {
                if (const GraphicsNode *ch = scene.findNode(cid);
                    ch && (ch->classId == ClassId::Surface || ch->classId == ClassId::Patch)) {
                    scene.setProperty(cid, "AmbientStrength", Value::scalar(ka, mr), mr);
                    scene.setProperty(cid, "DiffuseStrength", Value::scalar(kd, mr), mr);
                    scene.setProperty(cid, "SpecularStrength", Value::scalar(ks, mr), mr);
                }
            }
        }
    }
    outs[0] = Value();
}

void surfl_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    forwardExternal(ctx, "surf", args, nargout, outs);
    Value lt[1] = {Value::fromString("gouraud", ctx.engine->allocator())};
    Value dummy[1];
    lighting_scene_reg(keel::ArrayRef<Value>(lt, 1), 0, keel::MutArrayRef<Value>(dummy, 1), ctx);
}

// ── Statistical & 2D/3D Extra Charts ────────────────────────────────────

void cdfplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty() || args[0].isEmpty()) { outs[0] = Value(); return; }
    keel::Array<keel::f64> xs(mr);
    xs.reserve(args[0].numel());
    for (keel::usize i = 0; i < args[0].numel(); ++i) {
        const keel::f64 v = args[0].elemAsDouble(i);
        if (keel::is_finite(v)) xs.push_back(v);
    }
    if (xs.empty()) { outs[0] = Value(); return; }
    keel::sort(xs);
    const keel::usize n = xs.size();
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const NodeId sid = scene.createChartNode(ClassId::Stair, axId, true);
    Value xv = Value::matrix(1, n, ValueType::DOUBLE, mr);
    Value yv = Value::matrix(1, n, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < n; ++i) {
        xv.doubleDataMut()[i] = xs[i];
        yv.doubleDataMut()[i] = keel::as<keel::f64>(i + 1) / keel::as<keel::f64>(n);
    }
    if (GraphicsNode *sn = scene.findNode(sid)) {
        sn->props.put(keel::String::from("XData", mr), keel::move(xv));
        sn->props.put(keel::String::from("YData", mr), keel::move(yv));
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(sid, mr) : Value();
}

void qqplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty() || args[0].isEmpty()) { outs[0] = Value(); return; }
    keel::Array<keel::f64> xs(mr);
    for (keel::usize i = 0; i < args[0].numel(); ++i) {
        const keel::f64 v = args[0].elemAsDouble(i);
        if (keel::is_finite(v)) xs.push_back(v);
    }
    if (xs.size() < 2) { outs[0] = Value(); return; }
    keel::sort(xs);
    const keel::usize n = xs.size();
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const NodeId dataId = scene.createChartNode(ClassId::Line, axId, true);
    const NodeId refId = scene.createChartNode(ClassId::Line, axId, false);
    Value xv = Value::matrix(1, n, ValueType::DOUBLE, mr);
    Value yv = Value::matrix(1, n, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < n; ++i) {
        xv.doubleDataMut()[i] = normInvApprox((keel::as<keel::f64>(i) + 0.5) / keel::as<keel::f64>(n));
        yv.doubleDataMut()[i] = xs[i];
    }
    const keel::f64 q1 = xs[keel::as<keel::usize>(keel::as<keel::f64>(n - 1) * 0.25)];
    const keel::f64 q3 = xs[keel::as<keel::usize>(keel::as<keel::f64>(n - 1) * 0.75)];
    const keel::f64 t1 = normInvApprox(0.25), t3 = normInvApprox(0.75);
    const keel::f64 slope = (q3 - q1) / (t3 - t1);
    const keel::f64 ic = q1 - slope * t1;
    const keel::f64 tlo = xv.doubleData()[0], thi = xv.doubleData()[n - 1];
    Value rx = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    Value ry = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    rx.doubleDataMut()[0] = tlo; rx.doubleDataMut()[1] = thi;
    ry.doubleDataMut()[0] = ic + slope * tlo; ry.doubleDataMut()[1] = ic + slope * thi;
    if (GraphicsNode *dn = scene.findNode(dataId)) {
        dn->props.put(keel::String::from("XData", mr), keel::move(xv));
        dn->props.put(keel::String::from("YData", mr), keel::move(yv));
        dn->props.put(keel::String::from("LineStyle", mr), Value::fromString("none", mr));
        dn->props.put(keel::String::from("Marker", mr), Value::fromString("+", mr));
    }
    if (GraphicsNode *rn = scene.findNode(refId)) {
        rn->props.put(keel::String::from("XData", mr), keel::move(rx));
        rn->props.put(keel::String::from("YData", mr), keel::move(ry));
        rn->props.put(keel::String::from("LineStyle", mr), Value::fromString("-.", mr));
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    if (nargout >= 1) {
        const NodeId h[2] = {dataId, refId};
        outs[0] = scene.makeHandleColumn(keel::ArrayRef<NodeId>(h, 2), mr);
    } else {
        outs[0] = Value();
    }
}

void pareto_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty() || args[0].isEmpty()) { outs[0] = Value(); return; }
    keel::Array<keel::f64> ys(mr);
    for (keel::usize i = 0; i < args[0].numel(); ++i) {
        const keel::f64 v = args[0].elemAsDouble(i);
        if (keel::is_finite(v)) ys.push_back(v);
    }
    if (ys.empty()) { outs[0] = Value(); return; }
    keel::sort(ys.begin(), ys.end(), [](keel::f64 a, keel::f64 b) { return a > b; });
    keel::f64 total = 0.0;
    for (keel::f64 v : ys) total += v;
    if (total == 0.0) total = 1.0;

    const keel::usize n = ys.size();
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const NodeId bid = scene.createChartNode(ClassId::Bar, axId, true);
    const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
    Value xv = Value::matrix(1, n, ValueType::DOUBLE, mr);
    Value by = Value::matrix(1, n, ValueType::DOUBLE, mr);
    Value ly = Value::matrix(1, n, ValueType::DOUBLE, mr);
    keel::f64 cum = 0.0;
    for (keel::usize i = 0; i < n; ++i) {
        cum += ys[i];
        xv.doubleDataMut()[i] = keel::as<keel::f64>(i + 1);
        by.doubleDataMut()[i] = ys[i];
        ly.doubleDataMut()[i] = 100.0 * cum / total;
    }
    if (GraphicsNode *bn = scene.findNode(bid)) {
        bn->props.put(keel::String::from("XData", mr), xv);
        bn->props.put(keel::String::from("YData", mr), keel::move(by));
    }
    if (GraphicsNode *ln = scene.findNode(lid)) {
        ln->props.put(keel::String::from("XData", mr), keel::move(xv));
        ln->props.put(keel::String::from("YData", mr), keel::move(ly));
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    if (nargout >= 1) {
        const NodeId h[2] = {bid, lid};
        outs[0] = scene.makeHandleColumn(keel::ArrayRef<NodeId>(h, 2), mr);
    } else {
        outs[0] = Value();
    }
}

void histfit_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty() || args[0].isEmpty()) { outs[0] = Value(); return; }
    keel::Array<keel::f64> xs(mr);
    for (keel::usize i = 0; i < args[0].numel(); ++i) {
        const keel::f64 v = args[0].elemAsDouble(i);
        if (keel::is_finite(v)) xs.push_back(v);
    }
    if (xs.size() < 2) { outs[0] = Value(); return; }
    keel::usize nbins = 10;
    if (args.size() >= 2 && args[1].isNumeric() && args[1].numel() == 1)
        nbins = keel::max<keel::usize>(1, keel::as<keel::usize>(args[1].toScalar()));

    keel::f64 mn = xs[0], mx = xs[0], mean = 0.0;
    for (keel::f64 v : xs) { mn = keel::min(mn, v); mx = keel::max(mx, v); mean += v; }
    mean /= keel::as<keel::f64>(xs.size());
    keel::f64 var = 0.0;
    for (keel::f64 v : xs) var += (v - mean) * (v - mean);
    var /= keel::as<keel::f64>(xs.size() - 1);
    const keel::f64 sd = (var > 0.0) ? keel::sqrt(var) : 1.0;
    keel::f64 bw = (mx - mn) / keel::as<keel::f64>(nbins);
    if (!(bw > 0.0)) bw = 1.0;

    Value bx = Value::matrix(1, nbins, ValueType::DOUBLE, mr);
    Value by = Value::matrix(1, nbins, ValueType::DOUBLE, mr);
    for (keel::usize b = 0; b < nbins; ++b) {
        bx.doubleDataMut()[b] = mn + bw * (keel::as<keel::f64>(b) + 0.5);
        by.doubleDataMut()[b] = 0.0;
    }
    for (keel::f64 v : xs) {
        keel::i64 b = keel::as<keel::i64>(keel::floor((v - mn) / bw));
        if (b < 0) b = 0;
        if (keel::as<keel::usize>(b) >= nbins) b = keel::as<keel::i64>(nbins - 1);
        by.doubleDataMut()[keel::as<keel::usize>(b)] += 1.0;
    }
    constexpr keel::usize kG = 100;
    Value fx = Value::matrix(1, kG, ValueType::DOUBLE, mr);
    Value fy = Value::matrix(1, kG, ValueType::DOUBLE, mr);
    const keel::f64 scale = keel::as<keel::f64>(xs.size()) * bw;
    for (keel::usize g = 0; g < kG; ++g) {
        const keel::f64 xg = mn + (mx - mn) * keel::as<keel::f64>(g) / keel::as<keel::f64>(kG - 1);
        const keel::f64 z = (xg - mean) / sd;
        fx.doubleDataMut()[g] = xg;
        fy.doubleDataMut()[g] = scale * keel::exp(-0.5 * z * z) / (sd * keel::kSqrt2Pi);
    }

    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const NodeId bid = scene.createChartNode(ClassId::Bar, axId, true);
    const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
    if (GraphicsNode *bn = scene.findNode(bid)) {
        bn->props.put(keel::String::from("XData", mr), keel::move(bx));
        bn->props.put(keel::String::from("YData", mr), keel::move(by));
    }
    if (GraphicsNode *ln = scene.findNode(lid)) {
        ln->props.put(keel::String::from("XData", mr), keel::move(fx));
        ln->props.put(keel::String::from("YData", mr), keel::move(fy));
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    if (nargout >= 1) {
        const NodeId h[2] = {bid, lid};
        outs[0] = scene.makeHandleColumn(keel::ArrayRef<NodeId>(h, 2), mr);
    } else {
        outs[0] = Value();
    }
}

void spy_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty()) { outs[0] = Value(); return; }
    const Value &M = args[0];
    const keel::usize R = M.dims().rows(), C = M.dims().cols();
    keel::Array<keel::f64> xs(mr), ys(mr);
    for (keel::usize c = 0; c < C; ++c) {
        for (keel::usize r = 0; r < R; ++r) {
            const keel::f64 v = M.elemAsDouble(r + c * R);
            if (v != 0.0 && keel::is_finite(v)) {
                xs.push_back(keel::as<keel::f64>(c + 1));
                ys.push_back(keel::as<keel::f64>(r + 1));
            }
        }
    }
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
    Value xv = Value::matrix(1, xs.size(), ValueType::DOUBLE, mr);
    Value yv = Value::matrix(1, ys.size(), ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < xs.size(); ++i) {
        xv.doubleDataMut()[i] = xs[i];
        yv.doubleDataMut()[i] = ys[i];
    }
    if (GraphicsNode *ln = scene.findNode(lid)) {
        ln->props.put(keel::String::from("XData", mr), keel::move(xv));
        ln->props.put(keel::String::from("YData", mr), keel::move(yv));
        ln->props.put(keel::String::from("LineStyle", mr), Value::fromString("none", mr));
        ln->props.put(keel::String::from("Marker", mr), Value::fromString(".", mr));
    }
    scene.setProperty(axId, "YDir", Value::fromString("reverse", mr), mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = Value();
}

void violinplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty() || args[0].isEmpty()) { outs[0] = Value(); return; }
    const Value &M = args[0];
    const keel::usize R = keel::max<keel::usize>(1, M.dims().rows());
    const keel::usize C = keel::max<keel::usize>(1, M.dims().cols());
    const bool single = (R == 1 || C == 1);
    const keel::usize nV = single ? 1 : C;
    const keel::usize nPer = single ? M.numel() : R;

    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    keel::Array<NodeId> created(mr);
    constexpr keel::usize kKdePts = 50;
    for (keel::usize bi = 0; bi < nV; ++bi) {
        keel::Array<keel::f64> col(mr);
        for (keel::usize r = 0; r < nPer; ++r) {
            const keel::f64 v = single ? M.elemAsDouble(r) : M.elemAsDouble(r + bi * R);
            if (keel::is_finite(v)) col.push_back(v);
        }
        if (col.size() < 2) continue;
        keel::f64 mean = 0.0;
        for (keel::f64 v : col) mean += v;
        mean /= keel::as<keel::f64>(col.size());
        keel::f64 var = 0.0;
        for (keel::f64 v : col) var += (v - mean) * (v - mean);
        var /= keel::as<keel::f64>(col.size() - 1);
        const keel::f64 sd = keel::sqrt(var);
        if (!(sd > 0.0)) continue;
        const keel::f64 bw = 1.06 * sd * keel::pow(keel::as<keel::f64>(col.size()), -0.2);
        keel::sort(col);
        const keel::f64 mn = col.front(), mx = col.back();
        keel::Array<keel::f64> ys(kKdePts, 0.0, mr), den(kKdePts, 0.0, mr);
        keel::f64 maxD = 0.0;
        for (keel::usize g = 0; g < kKdePts; ++g) {
            const keel::f64 y = mn + (mx - mn) * keel::as<keel::f64>(g) / keel::as<keel::f64>(kKdePts - 1);
            keel::f64 s = 0.0;
            for (keel::f64 v : col) {
                const keel::f64 t = (y - v) / bw;
                s += keel::exp(-0.5 * t * t);
            }
            s /= (keel::as<keel::f64>(col.size()) * bw * keel::kSqrt2Pi);
            ys[g] = y; den[g] = s;
            if (s > maxD) maxD = s;
        }
        if (!(maxD > 0.0)) continue;
        const keel::f64 cx = keel::as<keel::f64>(bi + 1);
        const keel::f64 scale = 0.4 / maxD;
        Value px = Value::matrix(2 * kKdePts, 1, ValueType::DOUBLE, mr);
        Value py = Value::matrix(2 * kKdePts, 1, ValueType::DOUBLE, mr);
        for (keel::usize g = 0; g < kKdePts; ++g) {
            px.doubleDataMut()[g] = cx + den[g] * scale;
            py.doubleDataMut()[g] = ys[g];
            px.doubleDataMut()[kKdePts + g] = cx - den[kKdePts - 1 - g] * scale;
            py.doubleDataMut()[kKdePts + g] = ys[kKdePts - 1 - g];
        }
        const NodeId pid = scene.createChartNode(ClassId::Patch, axId, true);
        if (GraphicsNode *pn = scene.findNode(pid)) {
            pn->props.put(keel::String::from("XData", mr), keel::move(px));
            pn->props.put(keel::String::from("YData", mr), keel::move(py));
        }
        scene.syncPatchVerticesAndData(pid, "XData", mr);
        created.push_back(pid);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void bar3_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, true);
    if (idx >= args.size() || !args[idx].isNumeric()) { outs[0] = Value(); return; }
    const Value &Z = args[idx];
    const keel::usize R = Z.dims().rows(), C = Z.dims().cols();
    keel::Array<NodeId> created(mr);
    for (keel::usize c = 0; c < C; ++c) {
        const NodeId sid = scene.createChartNode(ClassId::Surface, axId, false);
        Value sx = Value::matrix(R, 2, ValueType::DOUBLE, mr);
        Value sy = Value::matrix(R, 2, ValueType::DOUBLE, mr);
        Value sz = Value::matrix(R, 2, ValueType::DOUBLE, mr);
        const keel::f64 xc = keel::as<keel::f64>(c + 1);
        for (keel::usize r = 0; r < R; ++r) {
            const keel::f64 zv = Z.elemAsDouble(r + c * R);
            sx.doubleDataMut()[r] = xc - 0.4; sx.doubleDataMut()[r + R] = xc + 0.4;
            sy.doubleDataMut()[r] = keel::as<keel::f64>(r + 1); sy.doubleDataMut()[r + R] = keel::as<keel::f64>(r + 1);
            sz.doubleDataMut()[r] = zv; sz.doubleDataMut()[r + R] = zv;
        }
        if (GraphicsNode *sn = scene.findNode(sid)) {
            sn->props.put(keel::String::from("XData", mr), keel::move(sx));
            sn->props.put(keel::String::from("YData", mr), keel::move(sy));
            sn->props.put(keel::String::from("ZData", mr), keel::move(sz));
        }
        created.push_back(sid);
    }
    setView3D(scene, axId, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void geoForwardScene(const char *target, keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.size() < 2) {
        forwardExternal(ctx, target, args, nargout, outs);
        return;
    }
    keel::Array<Value> swapped(mr);
    swapped.reserve(args.size());
    swapped.push_back(args[1]); // lon -> X
    swapped.push_back(args[0]); // lat -> Y
    for (keel::usize i = 2; i < args.size(); ++i) swapped.push_back(args[i]);
    forwardExternal(ctx, target, swapped.as_ref(), nargout, outs);
}

void geoplot_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ geoForwardScene("plot", a, no, o, c); }
void geoscatter_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ geoForwardScene("scatter", a, no, o, c); }
void geobubble_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ geoForwardScene("scatter", a, no, o, c); }

void geolimits_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    keel::usize start = (!args.empty() && args[0].isObject()) ? 1 : 0;
    const keel::usize rem = args.size() - start;
    if (rem == 0) {
        Value yOut[1], xOut[1];
        forwardExternal(ctx, "ylim", {}, 1, keel::MutArrayRef<Value>(yOut, 1));
        forwardExternal(ctx, "xlim", {}, 1, keel::MutArrayRef<Value>(xOut, 1));
        outs[0] = keel::move(yOut[0]);
        if (nargout >= 2 && outs.size() >= 2) outs[1] = keel::move(xOut[0]);
        return;
    }
    Value tmp[1];
    forwardExternal(ctx, "ylim", keel::ArrayRef<Value>(&args[start], 1), 0, keel::MutArrayRef<Value>(tmp, 1));
    if (rem >= 2)
        forwardExternal(ctx, "xlim", keel::ArrayRef<Value>(&args[start + 1], 1), 0, keel::MutArrayRef<Value>(tmp, 1));
    outs[0] = Value();
}

void geobasemap_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    keel::usize start = (!args.empty() && args[0].isObject()) ? 1 : 0;
    if (start < args.size() && (args[start].isChar() || args[start].isString())) {
        outs[0] = Value();
        return;
    }
    outs[0] = (nargout >= 1) ? Value::fromString("streets", ctx.engine->allocator()) : Value();
}

void bubblechart_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ forwardExternal(c, "scatter", a, no, o); }
void bubblechart3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ forwardExternal(c, "scatter3", a, no, o); }
void swarmchart3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ forwardExternal(c, "scatter3", a, no, o); }

void swarmchart_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.size() < 2) { outs[0] = Value(); return; }
    const Value &xv = args[0], &yv = args[1];
    const keel::usize n = keel::min(xv.numel(), yv.numel());
    Value xJit = Value::matrix(1, n, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < n; ++i) xJit.doubleDataMut()[i] = xv.elemAsDouble(i);
    struct Bucket { keel::i64 key; keel::Array<keel::usize> idxs; };
    keel::Array<Bucket> buckets(mr);
    for (keel::usize i = 0; i < n; ++i) {
        const keel::i64 key = keel::round_i64(xv.elemAsDouble(i));
        keel::usize bi = buckets.size();
        for (keel::usize b = 0; b < buckets.size(); ++b) {
            if (buckets[b].key == key) { bi = b; break; }
        }
        if (bi == buckets.size()) {
            Bucket nb{key, keel::Array<keel::usize>(mr)};
            nb.idxs.push_back(i);
            buckets.push_back(keel::move(nb));
        } else {
            buckets[bi].idxs.push_back(i);
        }
    }
    for (const auto &bk : buckets) {
        const keel::usize m = bk.idxs.size();
        if (m <= 1) continue;
        for (keel::usize j = 0; j < m; ++j) {
            const keel::f64 t = -0.35 + 0.70 * keel::as<keel::f64>(j) / keel::as<keel::f64>(m - 1);
            xJit.doubleDataMut()[bk.idxs[j]] += t;
        }
    }
    keel::Array<Value> proxied(mr);
    proxied.push_back(keel::move(xJit));
    proxied.push_back(yv);
    for (keel::usize i = 2; i < args.size(); ++i) proxied.push_back(args[i]);
    forwardExternal(ctx, "scatter", proxied.as_ref(), nargout, outs);
}

void confusionchart_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                              keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty()) { outs[0] = Value(); return; }
    keel::Array<Value> hmArgs(mr);
    if (args.size() >= 2 && (args[1].isCell() || args[1].isString())) {
        hmArgs.push_back(args[1]);
        hmArgs.push_back(args[1]);
        hmArgs.push_back(args[0]);
    } else {
        hmArgs.push_back(args[0]);
    }
    Value outH[1];
    forwardExternal(ctx, "heatmap", hmArgs.as_ref(), 1, keel::MutArrayRef<Value>(outH, 1));
    NodeId hmid = kInvalidNodeId;
    if (extractSceneHandle(outH[0], hmid)) {
        scene.setProperty(hmid, "XLabel", Value::fromString("Predicted Class", mr), mr);
        scene.setProperty(hmid, "YLabel", Value::fromString("True Class", mr), mr);
    }
    outs[0] = (nargout >= 1) ? keel::move(outH[0]) : Value();
}

void feather_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty()) { outs[0] = Value(); return; }
    const Value &U = args[0];
    const Value *V = (args.size() >= 2 && args[1].isNumeric()) ? &args[1] : nullptr;
    const keel::usize n = V ? keel::min(U.numel(), V->numel()) : U.numel();
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    keel::Array<NodeId> created(mr);
    for (keel::usize i = 0; i < n; ++i) {
        const keel::f64 u = U.elemAsDouble(i);
        const keel::f64 v = V ? V->elemAsDouble(i) : (U.isComplex() ? U.complexData()[i].imag() : 0.0);
        const keel::f64 x0 = keel::as<keel::f64>(i + 1);
        const NodeId lid = scene.createChartNode(ClassId::Line, axId, i == 0);
        Value xv = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        Value yv = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        xv.doubleDataMut()[0] = x0; xv.doubleDataMut()[1] = x0 + u;
        yv.doubleDataMut()[0] = 0.0; yv.doubleDataMut()[1] = v;
        if (GraphicsNode *ln = scene.findNode(lid)) {
            ln->props.put(keel::String::from("XData", mr), keel::move(xv));
            ln->props.put(keel::String::from("YData", mr), keel::move(yv));
        }
        created.push_back(lid);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void ezplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty()) { outs[0] = Value(); return; }
    const Value fh = toCallableHandle(args[0], ctx, "x", mr);
    keel::f64 lo = -2.0 * keel::kPi, hi = 2.0 * keel::kPi;
    if (args.size() >= 2 && args[1].isNumeric() && args[1].numel() >= 2) {
        lo = args[1].elemAsDouble(0);
        hi = args[1].elemAsDouble(1);
    }
    constexpr keel::usize kN = 200;
    Value xv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    Value yv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < kN; ++i) {
        const keel::f64 x = lo + (hi - lo) * keel::as<keel::f64>(i) / keel::as<keel::f64>(kN - 1);
        xv.doubleDataMut()[i] = x;
        keel::f64 y = keel::kNaN;
        if (fh.isFuncHandle()) {
            try {
                const Value xArg[1] = {Value::scalar(x, mr)};
                const Value r = ctx.engine->callFunctionHandle(fh, keel::ArrayRef<Value>(xArg, 1));
                if (r.isNumeric() && !r.isEmpty()) y = r.elemAsDouble(0);
            } catch (...) {}
        }
        yv.doubleDataMut()[i] = y;
    }
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
    if (GraphicsNode *ln = scene.findNode(lid)) {
        ln->props.put(keel::String::from("XData", mr), keel::move(xv));
        ln->props.put(keel::String::from("YData", mr), keel::move(yv));
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

void fSurfCore(const char *targetBuiltin, keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.empty()) { outs[0] = Value(); return; }
    const Value fh = toCallableHandle(args[0], ctx, "x,y", mr);
    keel::f64 xa = -5.0, xb = 5.0, ya = -5.0, yb = 5.0;
    if (args.size() >= 2 && args[1].isNumeric()) {
        if (args[1].numel() >= 4) {
            xa = args[1].elemAsDouble(0); xb = args[1].elemAsDouble(1);
            ya = args[1].elemAsDouble(2); yb = args[1].elemAsDouble(3);
        } else if (args[1].numel() >= 2) {
            xa = ya = args[1].elemAsDouble(0);
            xb = yb = args[1].elemAsDouble(1);
        }
    }
    constexpr keel::usize kN = 30;
    Value Xv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    Value Yv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    Value Zv = Value::matrix(kN, kN, ValueType::DOUBLE, mr);
    for (keel::usize j = 0; j < kN; ++j)
        Xv.doubleDataMut()[j] = xa + (xb - xa) * keel::as<keel::f64>(j) / keel::as<keel::f64>(kN - 1);
    for (keel::usize i = 0; i < kN; ++i)
        Yv.doubleDataMut()[i] = ya + (yb - ya) * keel::as<keel::f64>(i) / keel::as<keel::f64>(kN - 1);
    for (keel::usize i = 0; i < kN; ++i) {
        for (keel::usize j = 0; j < kN; ++j) {
            keel::f64 z = keel::kNaN;
            if (fh.isFuncHandle()) {
                try {
                    const Value xy[2] = {Value::scalar(Xv.doubleData()[j], mr),
                                         Value::scalar(Yv.doubleData()[i], mr)};
                    const Value r = ctx.engine->callFunctionHandle(fh, keel::ArrayRef<Value>(xy, 2));
                    if (r.isNumeric() && !r.isEmpty()) z = r.elemAsDouble(0);
                } catch (...) {}
            }
            Zv.doubleDataMut()[i + j * kN] = z;
        }
    }
    Value fwd[3] = {keel::move(Xv), keel::move(Yv), keel::move(Zv)};
    forwardExternal(ctx, targetBuiltin, keel::ArrayRef<Value>(fwd, 3), nargout, outs);
}

void fsurf_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fSurfCore("surf", a, no, o, c); }
void fmesh_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fSurfCore("mesh", a, no, o, c); }
void fcontour_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fSurfCore("contour", a, no, o, c); }
void ezsurf_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fSurfCore("surf", a, no, o, c); }
void ezsurfc_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fSurfCore("surfc", a, no, o, c); }
void ezmesh_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fSurfCore("mesh", a, no, o, c); }
void ezcontour_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fSurfCore("contour", a, no, o, c); }

void streamCore(bool autoSlice, keel::ArrayRef<Value> args, keel::usize nargout,
                keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.size() < 4) { outs[0] = Value(); return; }
    const Value &Xv = args[0], &Yv = args[1], &Uv = args[2], &Vv = args[3];
    const keel::usize Ry = Uv.dims().rows(), Cx = Uv.dims().cols();
    if (Cx < 2 || Ry < 2 || Vv.dims().rows() != Ry || Vv.dims().cols() != Cx) { outs[0] = Value(); return; }

    keel::Array<keel::f64> Xs(Cx, 0.0, mr), Ys(Ry, 0.0, mr);
    for (keel::usize c = 0; c < Cx; ++c)
        Xs[c] = (Xv.numel() == Ry * Cx) ? Xv.elemAsDouble(c * Ry) : (c < Xv.numel() ? Xv.elemAsDouble(c) : keel::as<keel::f64>(c + 1));
    for (keel::usize r = 0; r < Ry; ++r)
        Ys[r] = (r < Yv.numel()) ? Yv.elemAsDouble(r) : keel::as<keel::f64>(r + 1);
    const keel::f64 xMin = keel::min(Xs.front(), Xs.back()), xMax = keel::max(Xs.front(), Xs.back());
    const keel::f64 yMin = keel::min(Ys.front(), Ys.back()), yMax = keel::max(Ys.front(), Ys.back());
    const keel::f64 h = keel::min(keel::abs(xMax - xMin) / keel::as<keel::f64>(Cx - 1),
                                  keel::abs(yMax - yMin) / keel::as<keel::f64>(Ry - 1)) * 0.4;

    auto sampleField = [&](keel::f64 x, keel::f64 y, keel::f64 &u, keel::f64 &v) -> bool {
        if (x < xMin || x > xMax || y < yMin || y > yMax) return false;
        keel::usize c0 = 0;
        while (c0 + 1 < Cx && Xs[c0 + 1] <= x) ++c0;
        if (c0 + 1 >= Cx) c0 = Cx - 2;
        keel::usize r0 = 0;
        while (r0 + 1 < Ry && Ys[r0 + 1] <= y) ++r0;
        if (r0 + 1 >= Ry) r0 = Ry - 2;
        const keel::f64 tx = (Xs[c0 + 1] != Xs[c0]) ? (x - Xs[c0]) / (Xs[c0 + 1] - Xs[c0]) : 0.0;
        const keel::f64 ty = (Ys[r0 + 1] != Ys[r0]) ? (y - Ys[r0]) / (Ys[r0 + 1] - Ys[r0]) : 0.0;
        const keel::f64 u00 = Uv.elemAsDouble(r0 + c0 * Ry), u01 = Uv.elemAsDouble(r0 + (c0 + 1) * Ry);
        const keel::f64 u10 = Uv.elemAsDouble(r0 + 1 + c0 * Ry), u11 = Uv.elemAsDouble(r0 + 1 + (c0 + 1) * Ry);
        const keel::f64 v00 = Vv.elemAsDouble(r0 + c0 * Ry), v01 = Vv.elemAsDouble(r0 + (c0 + 1) * Ry);
        const keel::f64 v10 = Vv.elemAsDouble(r0 + 1 + c0 * Ry), v11 = Vv.elemAsDouble(r0 + 1 + (c0 + 1) * Ry);
        if (!keel::is_finite(u00) || !keel::is_finite(u01) || !keel::is_finite(u10) || !keel::is_finite(u11)
            || !keel::is_finite(v00) || !keel::is_finite(v01) || !keel::is_finite(v10) || !keel::is_finite(v11))
            return false;
        u = (u00 * (1.0 - tx) + u01 * tx) * (1.0 - ty) + (u10 * (1.0 - tx) + u11 * tx) * ty;
        v = (v00 * (1.0 - tx) + v01 * tx) * (1.0 - ty) + (v10 * (1.0 - tx) + v11 * tx) * ty;
        return true;
    };

    keel::Array<keel::Pair<keel::f64, keel::f64>> seeds(mr);
    if (autoSlice) {
        for (keel::i32 j = 0; j < 5; ++j)
            for (keel::i32 i = 0; i < 5; ++i)
                seeds.emplace_back(xMin + (xMax - xMin) * (keel::as<keel::f64>(i) + 0.5) / 5.0,
                                   yMin + (yMax - yMin) * (keel::as<keel::f64>(j) + 0.5) / 5.0);
    } else if (args.size() >= 6) {
        const keel::usize nS = keel::min(args[4].numel(), args[5].numel());
        for (keel::usize k = 0; k < nS; ++k)
            seeds.emplace_back(args[4].elemAsDouble(k), args[5].elemAsDouble(k));
    }

    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    keel::Array<NodeId> created(mr);
    for (const auto &sd : seeds) {
        keel::Array<keel::f64> px(mr), py(mr);
        keel::f64 x = sd.first, y = sd.second;
        for (keel::i32 step = 0; step < 400; ++step) {
            keel::f64 u = 0.0, v = 0.0;
            if (!sampleField(x, y, u, v)) break;
            if (keel::hypot(u, v) < 1e-9) break;
            px.push_back(x); py.push_back(y);
            keel::f64 k2u, k2v, k3u, k3v, k4u, k4v;
            if (!sampleField(x + 0.5 * h * u, y + 0.5 * h * v, k2u, k2v)) break;
            if (!sampleField(x + 0.5 * h * k2u, y + 0.5 * h * k2v, k3u, k3v)) break;
            if (!sampleField(x + h * k3u, y + h * k3v, k4u, k4v)) break;
            x += h * (u + 2.0 * k2u + 2.0 * k3u + k4u) / 6.0;
            y += h * (v + 2.0 * k2v + 2.0 * k3v + k4v) / 6.0;
        }
        if (px.empty()) continue;
        const NodeId lid = scene.createChartNode(ClassId::Line, axId, created.empty());
        Value xv = Value::matrix(1, px.size(), ValueType::DOUBLE, mr);
        Value yv = Value::matrix(1, py.size(), ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < px.size(); ++i) {
            xv.doubleDataMut()[i] = px[i];
            yv.doubleDataMut()[i] = py[i];
        }
        if (GraphicsNode *ln = scene.findNode(lid)) {
            ln->props.put(keel::String::from("XData", mr), keel::move(xv));
            ln->props.put(keel::String::from("YData", mr), keel::move(yv));
        }
        created.push_back(lid);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void streamline_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ streamCore(false, a, no, o, c); }
void streamslice_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ streamCore(true, a, no, o, c); }

void triplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.size() < 3) { outs[0] = Value(); return; }
    const Value &TRI = args[0], &xv = args[1], &yv = args[2];
    const keel::usize M = TRI.dims().rows(), N = xv.numel();
    if (yv.numel() != N || TRI.dims().cols() != 3 || M == 0) { outs[0] = Value(); return; }

    keel::Array<keel::f64> px(mr), py(mr);
    for (keel::usize i = 0; i < M; ++i) {
        const keel::usize a = keel::as<keel::usize>(TRI.elemAsDouble(i)) - 1;
        const keel::usize b = keel::as<keel::usize>(TRI.elemAsDouble(i + M)) - 1;
        const keel::usize c = keel::as<keel::usize>(TRI.elemAsDouble(i + 2 * M)) - 1;
        if (a >= N || b >= N || c >= N) continue;
        if (!px.empty()) { px.push_back(keel::kNaN); py.push_back(keel::kNaN); }
        px.push_back(xv.elemAsDouble(a)); py.push_back(yv.elemAsDouble(a));
        px.push_back(xv.elemAsDouble(b)); py.push_back(yv.elemAsDouble(b));
        px.push_back(xv.elemAsDouble(c)); py.push_back(yv.elemAsDouble(c));
        px.push_back(xv.elemAsDouble(a)); py.push_back(yv.elemAsDouble(a));
    }
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
    Value xd = Value::matrix(1, px.size(), ValueType::DOUBLE, mr);
    Value yd = Value::matrix(1, py.size(), ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < px.size(); ++i) {
        xd.doubleDataMut()[i] = px[i];
        yd.doubleDataMut()[i] = py[i];
    }
    if (GraphicsNode *ln = scene.findNode(lid)) {
        ln->props.put(keel::String::from("XData", mr), keel::move(xd));
        ln->props.put(keel::String::from("YData", mr), keel::move(yd));
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

void voronoi_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.size() < 2) { outs[0] = Value(); return; }
    const Value &xv = args[0], &yv = args[1];
    const keel::usize n = xv.numel();
    if (yv.numel() != n || n < 3) { outs[0] = Value(); return; }

    keel::Array<keel::f64> X(n, 0.0, mr), Y(n, 0.0, mr);
    for (keel::usize i = 0; i < n; ++i) { X[i] = xv.elemAsDouble(i); Y[i] = yv.elemAsDouble(i); }
    auto sa2 = [&](keel::usize a, keel::usize b, keel::usize c) {
        return (X[b] - X[a]) * (Y[c] - Y[a]) - (Y[b] - Y[a]) * (X[c] - X[a]);
    };
    auto inC = [&](keel::usize a, keel::usize b, keel::usize c, keel::usize p) {
        const keel::f64 ax = X[a] - X[p], ay = Y[a] - Y[p];
        const keel::f64 bx = X[b] - X[p], by = Y[b] - Y[p];
        const keel::f64 cx = X[c] - X[p], cy = Y[c] - Y[p];
        return ax * (by * (cx * cx + cy * cy) - cy * (bx * bx + by * by))
             - ay * (bx * (cx * cx + cy * cy) - cx * (bx * bx + by * by))
             + (ax * ax + ay * ay) * (bx * cy - cx * by);
    };
    keel::Array<keel::StaticArray<keel::usize, 3>> tris(mr);
    for (keel::usize a = 0; a < n; ++a)
        for (keel::usize b = a + 1; b < n; ++b)
            for (keel::usize c = b + 1; c < n; ++c) {
                const keel::f64 s = sa2(a, b, c);
                if (keel::abs(s) < 1e-15) continue;
                keel::usize va = a, vb = b, vc = c;
                if (s < 0.0) keel::swap(vb, vc);
                bool ok = true;
                for (keel::usize p = 0; p < n; ++p) {
                    if (p == va || p == vb || p == vc) continue;
                    if (inC(va, vb, vc, p) > 1e-12) { ok = false; break; }
                }
                if (ok) tris.push_back({va, vb, vc});
            }
    struct CC { keel::f64 x, y; };
    keel::Array<CC> ccs(tris.size(), CC{0.0, 0.0}, mr);
    for (keel::usize t = 0; t < tris.size(); ++t) {
        const auto &T = tris[t];
        const keel::f64 ax = X[T[0]], ay = Y[T[0]], bx = X[T[1]], by = Y[T[1]], cx = X[T[2]], cy = Y[T[2]];
        const keel::f64 d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
        if (keel::abs(d) < 1e-15) continue;
        const keel::f64 a2 = ax * ax + ay * ay, b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
        ccs[t] = {(a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d,
                  (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d};
    }

    struct EdgeTri { keel::usize u, v, t; };
    keel::Array<EdgeTri> et(mr);
    for (keel::usize t = 0; t < tris.size(); ++t) {
        for (keel::i32 e = 0; e < 3; ++e) {
            keel::usize u = tris[t][e], v = tris[t][(e + 1) % 3];
            if (u > v) keel::swap(u, v);
            et.push_back(EdgeTri{u, v, t});
        }
    }
    keel::sort(et.begin(), et.end(), [](const EdgeTri &a, const EdgeTri &b) {
        return a.u != b.u ? a.u < b.u : (a.v != b.v ? a.v < b.v : a.t < b.t);
    });

    keel::f64 xMin = X[0], xMax = X[0], yMin = Y[0], yMax = Y[0];
    for (keel::usize i = 1; i < n; ++i) {
        xMin = keel::min(xMin, X[i]); xMax = keel::max(xMax, X[i]);
        yMin = keel::min(yMin, Y[i]); yMax = keel::max(yMax, Y[i]);
    }
    const keel::f64 xPad = keel::max(0.1, (xMax - xMin) * 0.25);
    const keel::f64 yPad = keel::max(0.1, (yMax - yMin) * 0.25);
    const keel::f64 far = (xMax - xMin + 2.0 * xPad) + (yMax - yMin + 2.0 * yPad);

    keel::Array<keel::f64> vx(mr), vy(mr);
    auto emitSeg = [&](keel::f64 xa, keel::f64 ya, keel::f64 xb, keel::f64 yb) {
        if (!vx.empty()) { vx.push_back(keel::kNaN); vy.push_back(keel::kNaN); }
        vx.push_back(xa); vx.push_back(xb);
        vy.push_back(ya); vy.push_back(yb);
    };
    for (keel::usize i = 0; i < et.size();) {
        keel::usize j = i + 1;
        while (j < et.size() && et[j].u == et[i].u && et[j].v == et[i].v) ++j;
        if (j - i == 2) {
            emitSeg(ccs[et[i].t].x, ccs[et[i].t].y, ccs[et[i + 1].t].x, ccs[et[i + 1].t].y);
        } else if (j - i == 1) {
            const keel::usize u = et[i].u, v = et[i].v, t = et[i].t;
            const auto &T = tris[t];
            const keel::usize third = (T[0] != u && T[0] != v) ? T[0] : ((T[1] != u && T[1] != v) ? T[1] : T[2]);
            keel::f64 nx = -(Y[v] - Y[u]), ny = (X[v] - X[u]);
            if (nx * (X[third] - 0.5 * (X[u] + X[v])) + ny * (Y[third] - 0.5 * (Y[u] + Y[v])) > 0.0) {
                nx = -nx; ny = -ny;
            }
            const keel::f64 nl = keel::hypot(nx, ny);
            if (nl > 1e-15) emitSeg(ccs[t].x, ccs[t].y, ccs[t].x + nx / nl * far, ccs[t].y + ny / nl * far);
        }
        i = j;
    }

    if (nargout >= 2) {
        const keel::usize nSeg = (vx.size() + 1) / 3;
        Value ox = Value::matrix(2, nSeg, ValueType::DOUBLE, mr);
        Value oy = Value::matrix(2, nSeg, ValueType::DOUBLE, mr);
        for (keel::usize s = 0; s < nSeg; ++s) {
            ox.doubleDataMut()[0 + 2 * s] = vx[3 * s];
            ox.doubleDataMut()[1 + 2 * s] = vx[3 * s + 1];
            oy.doubleDataMut()[0 + 2 * s] = vy[3 * s];
            oy.doubleDataMut()[1 + 2 * s] = vy[3 * s + 1];
        }
        outs[0] = keel::move(ox);
        outs[1] = keel::move(oy);
        return;
    }

    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const NodeId ptId = scene.createChartNode(ClassId::Line, axId, true);
    const NodeId edId = scene.createChartNode(ClassId::Line, axId, false);
    if (GraphicsNode *pn = scene.findNode(ptId)) {
        pn->props.put(keel::String::from("XData", mr), xv);
        pn->props.put(keel::String::from("YData", mr), yv);
        pn->props.put(keel::String::from("LineStyle", mr), Value::fromString("none", mr));
        pn->props.put(keel::String::from("Marker", mr), Value::fromString(".", mr));
    }
    Value ex = Value::matrix(1, vx.size(), ValueType::DOUBLE, mr);
    Value ey = Value::matrix(1, vy.size(), ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < vx.size(); ++i) {
        ex.doubleDataMut()[i] = vx[i];
        ey.doubleDataMut()[i] = vy[i];
    }
    if (GraphicsNode *en = scene.findNode(edId)) {
        en->props.put(keel::String::from("XData", mr), keel::move(ex));
        en->props.put(keel::String::from("YData", mr), keel::move(ey));
    }
    Value xl = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    xl.doubleDataMut()[0] = xMin - xPad; xl.doubleDataMut()[1] = xMax + xPad;
    Value yl = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    yl.doubleDataMut()[0] = yMin - yPad; yl.doubleDataMut()[1] = yMax + yPad;
    scene.setProperty(axId, "XLim", keel::move(xl), mr);
    scene.setProperty(axId, "YLim", keel::move(yl), mr);
    if (nargout >= 1) {
        const NodeId h[2] = {ptId, edId};
        outs[0] = scene.makeHandleColumn(keel::ArrayRef<NodeId>(h, 2), mr);
    } else {
        outs[0] = Value();
    }
}

void parallelplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                            keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty() || !args[0].isNumeric()) { outs[0] = Value(); return; }
    const Value &T = args[0];
    const keel::usize R = T.dims().rows(), C = T.dims().cols();
    if (R == 0 || C == 0) { outs[0] = Value(); return; }
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    Value xv = Value::matrix(1, C, ValueType::DOUBLE, mr);
    for (keel::usize c = 0; c < C; ++c) xv.doubleDataMut()[c] = keel::as<keel::f64>(c + 1);
    keel::Array<NodeId> created(mr);
    for (keel::usize r = 0; r < R; ++r) {
        const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
        Value yv = Value::matrix(1, C, ValueType::DOUBLE, mr);
        for (keel::usize c = 0; c < C; ++c) yv.doubleDataMut()[c] = T.elemAsDouble(r + c * R);
        if (GraphicsNode *ln = scene.findNode(lid)) {
            ln->props.put(keel::String::from("XData", mr), xv);
            ln->props.put(keel::String::from("YData", mr), keel::move(yv));
        }
        created.push_back(lid);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

// ── 3-D Volume / Vector Field Builtins (isosurface / coneplot / streamtube) ──

void isosurface_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    const Value *V = nullptr, *X = nullptr, *Y = nullptr, *Z = nullptr;
    keel::f64 iso = 0.0;
    bool isoSet = false;
    if (args.size() >= 5 && args[3].dims().ndim() >= 3) {
        X = &args[0]; Y = &args[1]; Z = &args[2]; V = &args[3]; iso = args[4].toScalar(); isoSet = true;
    } else if (args.size() >= 2 && args[0].dims().ndim() >= 3) {
        V = &args[0]; iso = args[1].toScalar(); isoSet = true;
    } else if (!args.empty() && args[0].dims().ndim() >= 3) {
        V = &args[0];
    }
    if (!V) { outs[0] = Value(); return; }
    const keel::usize M = V->dims().rows(), N = V->dims().cols(), P = V->dims().pages();
    if (M < 2 || N < 2 || P < 2) { outs[0] = Value(); return; }

    keel::Array<keel::f64> Xs(N, 0.0, mr), Ys(M, 0.0, mr), Zs(P, 0.0, mr);
    for (keel::usize j = 0; j < N; ++j) Xs[j] = (X && X->numel() >= M * N * P) ? X->elemAsDouble(j * M) : ((X && j < X->numel()) ? X->elemAsDouble(j) : keel::as<keel::f64>(j + 1));
    for (keel::usize i = 0; i < M; ++i) Ys[i] = (Y && i < Y->numel()) ? Y->elemAsDouble(i) : keel::as<keel::f64>(i + 1);
    for (keel::usize k = 0; k < P; ++k) Zs[k] = (Z && Z->numel() >= M * N * P) ? Z->elemAsDouble(k * M * N) : ((Z && k < Z->numel()) ? Z->elemAsDouble(k) : keel::as<keel::f64>(k + 1));

    auto vAt = [&](keel::usize i, keel::usize j, keel::usize k) { return V->elemAsDouble(k * M * N + j * M + i); };
    if (!isoSet) {
        keel::f64 sum = 0.0; keel::usize cnt = 0;
        for (keel::usize i = 0; i < M * N * P; ++i) {
            const keel::f64 v = V->elemAsDouble(i);
            if (keel::is_finite(v)) { sum += v; ++cnt; }
        }
        iso = cnt > 0 ? sum / keel::as<keel::f64>(cnt) : 0.0;
    }

    // Collect surface crossing triangles via cube-face marching (compact, watertight-friendly).
    keel::Array<keel::StaticArray<keel::f64, 3>> verts(mr);
    keel::Array<keel::StaticArray<keel::usize, 3>> tris(mr);
    static constexpr keel::i32 kEdgePairs[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };
    static constexpr keel::i32 kCDX[8] = {0,1,1,0,0,1,1,0};
    static constexpr keel::i32 kCDY[8] = {0,0,1,1,0,0,1,1};
    static constexpr keel::i32 kCDZ[8] = {0,0,0,0,1,1,1,1};
    for (keel::usize k = 0; k + 1 < P; ++k) {
        for (keel::usize j = 0; j + 1 < N; ++j) {
            for (keel::usize i = 0; i + 1 < M; ++i) {
                keel::f64 cv[8] = {
                    vAt(i,j,k), vAt(i,j+1,k), vAt(i+1,j+1,k), vAt(i+1,j,k),
                    vAt(i,j,k+1), vAt(i,j+1,k+1), vAt(i+1,j+1,k+1), vAt(i+1,j,k+1)
                };
                keel::i32 below = 0;
                bool ok = true;
                for (keel::i32 c = 0; c < 8; ++c) {
                    if (!keel::is_finite(cv[c])) { ok = false; break; }
                    if (cv[c] < iso) ++below;
                }
                if (!ok || below == 0 || below == 8) continue;
                keel::StaticArray<keel::f64, 3> pts[12];
                keel::usize nPts = 0;
                for (keel::i32 e = 0; e < 12; ++e) {
                    const keel::i32 a = kEdgePairs[e][0], b = kEdgePairs[e][1];
                    if ((cv[a] < iso) == (cv[b] < iso)) continue;
                    const keel::f64 t = (keel::abs(cv[b] - cv[a]) > 1e-15) ? (iso - cv[a]) / (cv[b] - cv[a]) : 0.5;
                    const keel::f64 ax = Xs[j + keel::as<keel::usize>(kCDX[a])], bx = Xs[j + keel::as<keel::usize>(kCDX[b])];
                    const keel::f64 ay = Ys[i + keel::as<keel::usize>(kCDY[a])], by = Ys[i + keel::as<keel::usize>(kCDY[b])];
                    const keel::f64 az = Zs[k + keel::as<keel::usize>(kCDZ[a])], bz = Zs[k + keel::as<keel::usize>(kCDZ[b])];
                    pts[nPts++] = {ax + t * (bx - ax), ay + t * (by - ay), az + t * (bz - az)};
                }
                if (nPts < 3) continue;
                const keel::usize base = verts.size();
                for (keel::usize p = 0; p < nPts; ++p) verts.push_back(pts[p]);
                for (keel::usize p = 1; p + 1 < nPts; ++p)
                    tris.push_back({base + 1, base + p + 1, base + p + 2});
            }
        }
    }

    Value vMat = Value::matrix(verts.size(), 3, ValueType::DOUBLE, mr);
    for (keel::usize v = 0; v < verts.size(); ++v) {
        vMat.doubleDataMut()[v] = verts[v][0];
        vMat.doubleDataMut()[v + verts.size()] = verts[v][1];
        vMat.doubleDataMut()[v + 2 * verts.size()] = verts[v][2];
    }
    Value fMat = Value::matrix(tris.size(), 3, ValueType::DOUBLE, mr);
    for (keel::usize t = 0; t < tris.size(); ++t) {
        fMat.doubleDataMut()[t] = keel::as<keel::f64>(tris[t][0]);
        fMat.doubleDataMut()[t + tris.size()] = keel::as<keel::f64>(tris[t][1]);
        fMat.doubleDataMut()[t + 2 * tris.size()] = keel::as<keel::f64>(tris[t][2]);
    }

    if (nargout >= 2) {
        outs[0] = keel::move(fMat);
        outs[1] = keel::move(vMat);
        return;
    }
    if (nargout == 1) {
        Value s = Value::structure(mr);
        s.field("faces") = keel::move(fMat);
        s.field("vertices") = keel::move(vMat);
        outs[0] = keel::move(s);
        return;
    }

    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true, true);
    const NodeId pid = scene.createChartNode(ClassId::Patch, axId, false);
    if (GraphicsNode *pn = scene.findNode(pid)) {
        pn->props.put(keel::String::from("Vertices", mr), keel::move(vMat));
        pn->props.put(keel::String::from("Faces", mr), keel::move(fMat));
    }
    scene.syncPatchVerticesAndData(pid, "Vertices", mr);
    setView3D(scene, axId, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = Value();
}

void coneplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true, true);
    const NodeId pid = scene.createChartNode(ClassId::Patch, axId, false);
    if (args.size() >= 3) {
        const Value &U = (args.size() >= 6) ? args[3] : args[0];
        const keel::usize n = keel::min<keel::usize>(U.numel(), 64);
        Value verts = Value::matrix(n * 3, 3, ValueType::DOUBLE, mr);
        Value faces = Value::matrix(n, 3, ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < n; ++i) {
            const keel::f64 x = keel::as<keel::f64>(i + 1);
            const keel::usize b = 3 * i;
            verts.doubleDataMut()[b] = x; verts.doubleDataMut()[b + 3 * n] = 0.0; verts.doubleDataMut()[b + 6 * n] = 0.0;
            verts.doubleDataMut()[b + 1] = x + 0.5; verts.doubleDataMut()[b + 1 + 3 * n] = 0.2; verts.doubleDataMut()[b + 1 + 6 * n] = 0.0;
            verts.doubleDataMut()[b + 2] = x + 0.5; verts.doubleDataMut()[b + 2 + 3 * n] = -0.2; verts.doubleDataMut()[b + 2 + 6 * n] = 0.2;
            faces.doubleDataMut()[i] = keel::as<keel::f64>(b + 1);
            faces.doubleDataMut()[i + n] = keel::as<keel::f64>(b + 2);
            faces.doubleDataMut()[i + 2 * n] = keel::as<keel::f64>(b + 3);
        }
        if (GraphicsNode *pn = scene.findNode(pid)) {
            pn->props.put(keel::String::from("Vertices", mr), keel::move(verts));
            pn->props.put(keel::String::from("Faces", mr), keel::move(faces));
        }
        scene.syncPatchVerticesAndData(pid, "Vertices", mr);
    }
    setView3D(scene, axId, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(pid, mr) : Value();
}

void streamtube_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true, true);
    const NodeId sid = scene.createChartNode(ClassId::Surface, axId, false);
    (void)args;
    setView3D(scene, axId, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(sid, mr) : Value();
}

// ── montage & Polar secondary builtins ──────────────────────────────────

void montage_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty()) { outs[0] = Value(); return; }
    auto *mr = ctx.engine->allocator();

    keel::i32 reqRows = 0, reqCols = 0;
    keel::usize borderR = 0, borderC = 0;
    keel::f64 bgVal = 0.0;
    const Value *indicesArg = nullptr;
    const Value *rangeArg = nullptr;
    for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString()) continue;
        const keel::String k = args[i].toString();
        const Value &v = args[i + 1];
        if (asciiEqualNoCase(k, "Size") && v.isNumeric() && v.numel() >= 2) {
            const keel::f64 r = v.elemAsDouble(0), c = v.elemAsDouble(1);
            if (keel::is_finite(r) && r > 0.0) reqRows = keel::as<keel::i32>(r);
            if (keel::is_finite(c) && c > 0.0) reqCols = keel::as<keel::i32>(c);
        } else if (asciiEqualNoCase(k, "BorderSize") && v.isNumeric() && v.numel() >= 1) {
            const keel::f64 r = v.elemAsDouble(0);
            const keel::f64 c = (v.numel() >= 2) ? v.elemAsDouble(1) : r;
            if (keel::is_finite(r) && r >= 0.0) borderR = keel::as<keel::usize>(r);
            if (keel::is_finite(c) && c >= 0.0) borderC = keel::as<keel::usize>(c);
        } else if (asciiEqualNoCase(k, "BackgroundColor") && v.isNumeric() && v.numel() >= 1) {
            bgVal = v.elemAsDouble(0);
        } else if (asciiEqualNoCase(k, "Indices")) {
            indicesArg = &v;
        } else if (asciiEqualNoCase(k, "DisplayRange")) {
            rangeArg = &v;
        }
    }

    keel::Array<Value> frames(mr);
    const Value &in = args[0];
    if (in.isCell()) {
        for (keel::usize i = 0; i < in.numel(); ++i) {
            const Value &el = in.cellAt(i);
            if (el.isChar() || el.isString()) {
                Value outV[1];
                if (forwardExternal(ctx, "imread", keel::ArrayRef<Value>(&el, 1), 1, keel::MutArrayRef<Value>(outV, 1)))
                    frames.push_back(keel::move(outV[0]));
            } else if (!el.isEmpty()) {
                frames.push_back(el);
            }
        }
    } else if (in.isNumeric() || in.isLogical()) {
        const auto &d = in.dims();
        const keel::usize nd = d.ndim();
        const keel::usize H = d.rows(), W = d.cols();
        if (H == 0 || W == 0) { outs[0] = Value(); return; }
        if (nd <= 2) {
            frames.push_back(in);
        } else if (nd == 3) {
            const keel::usize N = d.dim(2), plane = H * W;
            for (keel::usize f = 0; f < N; ++f) {
                Value slice = Value::matrix(H, W, ValueType::DOUBLE, mr);
                for (keel::usize p = 0; p < plane; ++p)
                    slice.doubleDataMut()[p] = in.elemAsDouble(f * plane + p);
                frames.push_back(keel::move(slice));
            }
        } else {
            const keel::usize C = d.dim(2), N = d.dim(3), plane = H * W, frameElems = plane * C;
            for (keel::usize f = 0; f < N; ++f) {
                Value slice = (C == 1) ? Value::matrix(H, W, ValueType::DOUBLE, mr)
                                       : Value::matrix3d(H, W, C, ValueType::DOUBLE, mr);
                for (keel::usize p = 0; p < frameElems; ++p)
                    slice.doubleDataMut()[p] = in.elemAsDouble(f * frameElems + p);
                frames.push_back(keel::move(slice));
            }
        }
    } else {
        outs[0] = Value();
        return;
    }

    keel::Array<keel::usize> sel(mr);
    if (indicesArg && indicesArg->isNumeric() && !indicesArg->isEmpty()) {
        for (keel::usize i = 0; i < indicesArg->numel(); ++i) {
            const keel::i64 ii = keel::as<keel::i64>(indicesArg->elemAsDouble(i));
            if (ii >= 1 && keel::as<keel::usize>(ii) <= frames.size())
                sel.push_back(keel::as<keel::usize>(ii - 1));
        }
    } else {
        for (keel::usize i = 0; i < frames.size(); ++i) sel.push_back(i);
    }
    if (sel.empty()) { outs[0] = Value(); return; }

    const keel::usize nFrames = sel.size();
    keel::usize tileH = 1, tileW = 1, tileC = 1;
    for (keel::usize k = 0; k < nFrames; ++k) {
        const auto &fd = frames[sel[k]].dims();
        tileH = keel::max(tileH, fd.rows());
        tileW = keel::max(tileW, fd.cols());
        if (fd.ndim() >= 3 && fd.dim(2) == 3) tileC = 3;
    }
    keel::usize gridR = 0, gridC = 0;
    if (reqRows > 0 && reqCols > 0) {
        gridR = keel::as<keel::usize>(reqRows); gridC = keel::as<keel::usize>(reqCols);
    } else if (reqRows > 0) {
        gridR = keel::as<keel::usize>(reqRows); gridC = (nFrames + gridR - 1) / gridR;
    } else if (reqCols > 0) {
        gridC = keel::as<keel::usize>(reqCols); gridR = (nFrames + gridC - 1) / gridC;
    } else {
        gridC = keel::max<keel::usize>(1, keel::as<keel::usize>(keel::ceil(keel::sqrt(keel::as<keel::f64>(nFrames)))));
        gridR = (nFrames + gridC - 1) / gridC;
    }

    const keel::usize cellH = tileH + 2 * borderR, cellW = tileW + 2 * borderC;
    const keel::usize outH = gridR * cellH, outW = gridC * cellW;
    Value mosaic = (tileC == 3) ? Value::matrix3d(outH, outW, 3, ValueType::DOUBLE, mr)
                                : Value::matrix(outH, outW, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < outH * outW * tileC; ++i) mosaic.doubleDataMut()[i] = bgVal;
    const keel::usize outPlane = outH * outW;
    for (keel::usize k = 0; k < nFrames && k < gridR * gridC; ++k) {
        const keel::usize gr = k / gridC, gcIdx = k % gridC;
        const keel::usize r0 = gr * cellH + borderR, c0 = gcIdx * cellW + borderC;
        const Value &fr = frames[sel[k]];
        const keel::usize fh = keel::min(fr.dims().rows(), tileH), fw = keel::min(fr.dims().cols(), tileW);
        const keel::usize fc = (fr.dims().ndim() >= 3) ? fr.dims().dim(2) : 1;
        const keel::usize fplane = fr.dims().rows() * fr.dims().cols();
        for (keel::usize ch = 0; ch < tileC; ++ch) {
            const keel::usize srcCh = (ch < fc) ? ch : 0;
            for (keel::usize c = 0; c < fw; ++c) {
                for (keel::usize r = 0; r < fh; ++r) {
                    mosaic.doubleDataMut()[ch * outPlane + (c0 + c) * outH + (r0 + r)] =
                        fr.elemAsDouble(srcCh * fplane + c * fr.dims().rows() + r);
                }
            }
        }
    }
    keel::Array<Value> showArgs(mr);
    showArgs.push_back(keel::move(mosaic));
    if (rangeArg) showArgs.push_back(*rangeArg);
    forwardExternal(ctx, "imshow", showArgs.as_ref(), nargout, outs);
}

void compass_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty()) { outs[0] = Value(); return; }
    const Value &U = args[0];
    const Value *V = (args.size() >= 2 && args[1].isNumeric()) ? &args[1] : nullptr;
    const keel::usize n = V ? keel::min(U.numel(), V->numel()) : U.numel();
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    keel::Array<NodeId> created(mr);
    for (keel::usize i = 0; i < n; ++i) {
        const keel::f64 u = U.elemAsDouble(i);
        const keel::f64 v = V ? V->elemAsDouble(i) : (U.isComplex() ? U.complexData()[i].imag() : 0.0);
        const NodeId lid = scene.createChartNode(ClassId::Line, axId, i == 0);
        Value xv = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        Value yv = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        xv.doubleDataMut()[0] = 0.0; xv.doubleDataMut()[1] = u;
        yv.doubleDataMut()[0] = 0.0; yv.doubleDataMut()[1] = v;
        if (GraphicsNode *ln = scene.findNode(lid)) {
            ln->props.put(keel::String::from("XData", mr), keel::move(xv));
            ln->props.put(keel::String::from("YData", mr), keel::move(yv));
        }
        created.push_back(lid);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void polar_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ forwardExternal(c, "polarplot", a, no, o); }

void polarbubblechart_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ forwardExternal(c, "polarscatter", a, no, o); }

void polarhistogram_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                              keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty() || !args[0].isNumeric()) { outs[0] = Value(); return; }
    keel::usize nbins = 20;
    if (args.size() >= 2 && args[1].isNumeric() && args[1].numel() == 1)
        nbins = keel::max<keel::usize>(1, keel::as<keel::usize>(args[1].toScalar()));
    const keel::f64 twoPi = 2.0 * keel::kPi;
    keel::Array<keel::f64> counts(nbins, 0.0, mr);
    for (keel::usize i = 0; i < args[0].numel(); ++i) {
        keel::f64 th = keel::fmod(args[0].elemAsDouble(i), twoPi);
        if (th < 0.0) th += twoPi;
        keel::usize b = keel::as<keel::usize>(keel::floor(th / twoPi * keel::as<keel::f64>(nbins)));
        if (b >= nbins) b = nbins - 1;
        counts[b] += 1.0;
    }
    const NodeId curAx = scene.currentAxesId();
    NodeId paxId = kInvalidNodeId;
    if (const GraphicsNode *cn = scene.findNode(curAx); cn && cn->classId == ClassId::PolarAxes)
        paxId = curAx;
    else
        paxId = scene.createPolarAxes();
    const NodeId lid = scene.createChartNode(ClassId::Line, paxId, true);
    Value tv = Value::matrix(1, nbins, ValueType::DOUBLE, mr);
    Value rv = Value::matrix(1, nbins, ValueType::DOUBLE, mr);
    for (keel::usize b = 0; b < nbins; ++b) {
        tv.doubleDataMut()[b] = (keel::as<keel::f64>(b) + 0.5) * twoPi / keel::as<keel::f64>(nbins);
        rv.doubleDataMut()[b] = counts[b];
    }
    if (GraphicsNode *ln = scene.findNode(lid)) {
        ln->props.put(keel::String::from("XData", mr), keel::move(tv));
        ln->props.put(keel::String::from("YData", mr), keel::move(rv));
    }
    scene.recomputeAxesAutoLimits(paxId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

void polarPropCore(const char *prop, keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    NodeId paxId = (leadingAx != kInvalidNodeId) ? leadingAx : scene.currentAxesId();
    if (const GraphicsNode *cn = scene.findNode(paxId); !cn || cn->classId != ClassId::PolarAxes)
        paxId = scene.createPolarAxes();
    if (idx >= args.size()) {
        outs[0] = scene.getProperty(paxId, prop, mr);
        return;
    }
    scene.setProperty(paxId, prop, args[idx], mr);
    outs[0] = (nargout >= 1) ? scene.getProperty(paxId, prop, mr) : Value();
}

void thetadir_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ polarPropCore("ThetaDir", a, no, o, c); }
void thetazero_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ polarPropCore("ThetaZeroLocation", a, no, o, c); }
void thetaticklabels_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ polarPropCore("ThetaTickLabel", a, no, o, c); }
void rticklabels_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ polarPropCore("RTickLabel", a, no, o, c); }

} // anonymous namespace

void registerSceneExtraBuiltins(Engine &engine)
{
    auto regBoth = [&](const char *sub, const char *name,
                       void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &)) {
        engine.registerFunction("", name, ExternalFunc(fn));
        keel::SmallString<64> ns("graphics.");
        ns.append(sub);
        engine.registerFunction(ns, name, ExternalFunc(fn));
    };

    // Layout / Camera / Interaction
    regBoth("layout", "waitbar",      &waitbar_scene_reg);
    regBoth("layout", "whitebg",      &whitebg_scene_reg);
    regBoth("layout", "colordef",     &colordef_scene_reg);
    regBoth("layout", "xdir",         &xdir_scene_reg);
    regBoth("layout", "ydir",         &ydir_scene_reg);
    regBoth("layout", "colorscale",   &colorscale_scene_reg);
    regBoth("layout", "pan3d",        &sceneNoop_reg);
    regBoth("layout", "zoom3d",       &sceneNoop_reg);
    regBoth("layout", "daspect",      &daspect_scene_reg);
    regBoth("layout", "pbaspect",     &pbaspect_scene_reg);
    regBoth("layout", "linkprop",     &sceneNoopRet1_reg);
    regBoth("layout", "linkdata",     &sceneNoopRet1_reg);
    regBoth("layout", "drawnow",      &sceneNoop_reg);
    regBoth("layout", "camzoom",      &sceneNoop_reg);
    regBoth("layout", "camorbit",     &sceneNoop_reg);
    regBoth("layout", "campan",       &sceneNoop_reg);
    regBoth("layout", "camroll",      &sceneNoop_reg);
    regBoth("layout", "camdolly",     &sceneNoop_reg);
    regBoth("layout", "camva",        &camva_scene_reg);
    regBoth("layout", "camtarget",    &camtarget_scene_reg);
    regBoth("layout", "campos",       &campos_scene_reg);
    regBoth("layout", "camup",        &camup_scene_reg);
    regBoth("layout", "camproj",      &camproj_scene_reg);
    regBoth("layout", "xtickformat",  &xtickformat_scene_reg);
    regBoth("layout", "ytickformat",  &ytickformat_scene_reg);
    regBoth("layout", "ztickformat",  &ztickformat_scene_reg);
    regBoth("layout", "xtickangle",   &xtickangle_scene_reg);
    regBoth("layout", "ytickangle",   &ytickangle_scene_reg);

    // Statistical & 2D/3D extra charts
    regBoth("bar",     "cdfplot",        &cdfplot_scene_reg);
    regBoth("bar",     "qqplot",         &qqplot_scene_reg);
    regBoth("bar",     "pareto",         &pareto_scene_reg);
    regBoth("bar",     "histfit",        &histfit_scene_reg);
    regBoth("bar",     "spy",            &spy_scene_reg);
    regBoth("bar",     "violinplot",     &violinplot_scene_reg);
    regBoth("bar",     "bar3",           &bar3_scene_reg);
    regBoth("bar",     "geoscatter",     &geoscatter_scene_reg);
    regBoth("bar",     "geobubble",      &geobubble_scene_reg);
    regBoth("bar",     "geolimits",      &geolimits_scene_reg);
    regBoth("bar",     "geobasemap",     &geobasemap_scene_reg);
    regBoth("bar",     "bubblechart",    &bubblechart_scene_reg);
    regBoth("bar",     "bubblechart3",   &bubblechart3_scene_reg);
    regBoth("bar",     "swarmchart",     &swarmchart_scene_reg);
    regBoth("bar",     "swarmchart3",    &swarmchart3_scene_reg);
    regBoth("bar",     "confusionchart", &confusionchart_scene_reg);

    // Line / Function / Flow / Geometry charts
    regBoth("line",    "feather",        &feather_scene_reg);
    regBoth("line",    "ezplot",         &ezplot_scene_reg);
    regBoth("line",    "ezsurf",         &ezsurf_scene_reg);
    regBoth("line",    "ezsurfc",        &ezsurfc_scene_reg);
    regBoth("line",    "ezmesh",         &ezmesh_scene_reg);
    regBoth("line",    "ezcontour",      &ezcontour_scene_reg);
    regBoth("line",    "fcontour",       &fcontour_scene_reg);
    regBoth("line",    "fsurf",          &fsurf_scene_reg);
    regBoth("line",    "fmesh",          &fmesh_scene_reg);
    regBoth("line",    "streamline",     &streamline_scene_reg);
    regBoth("line",    "streamslice",    &streamslice_scene_reg);
    regBoth("line",    "geoplot",        &geoplot_scene_reg);
    regBoth("line",    "triplot",        &triplot_scene_reg);
    regBoth("line",    "voronoi",        &voronoi_scene_reg);
    regBoth("line",    "parallelplot",   &parallelplot_scene_reg);

    // Surface / Volume / Lighting
    regBoth("surface", "isosurface",     &isosurface_scene_reg);
    regBoth("surface", "coneplot",       &coneplot_scene_reg);
    regBoth("surface", "streamtube",     &streamtube_scene_reg);
    regBoth("surface", "lighting",       &lighting_scene_reg);
    regBoth("surface", "material",       &material_scene_reg);
    regBoth("surface", "surfl",          &surfl_scene_reg);

    // Image & Polar secondary builtins
    regBoth("image",   "montage",           &montage_scene_reg);
    regBoth("polar",   "compass",           &compass_scene_reg);
    regBoth("polar",   "polar",             &polar_scene_reg);
    regBoth("polar",   "polarhistogram",    &polarhistogram_scene_reg);
    regBoth("polar",   "polarbubblechart",  &polarbubblechart_scene_reg);
    regBoth("polar",   "rose",              &polarhistogram_scene_reg);
    regBoth("polar",   "thetadir",          &thetadir_scene_reg);
    regBoth("polar",   "thetazero",         &thetazero_scene_reg);
    regBoth("polar",   "thetazerolocation", &thetazero_scene_reg);
    regBoth("polar",   "thetaticklabels",   &thetaticklabels_scene_reg);
    regBoth("polar",   "rticklabels",       &rticklabels_scene_reg);
}

} // namespace numkit::gfx::detail
