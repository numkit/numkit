// src/builtin/register/graphics/scene_charts_builtins.cpp
//
// Phase 3 Model-First Handle Graphics chart & annotation builtins:
//   plot, plot3, line, semilogx, semilogy, loglog, scatter, scatter3,
//   bar, barh, surf, mesh, surfc, meshc, pcolor, surface, patch, fill, fill3,
//   quiver, quiver3, contour, contourf, contour3, stem, stem3, errorbar,
//   histogram, area, image, imagesc, stairs, animatedline, addpoints,
//   getpoints, clearpoints, annotation, polarplot, polarscatter, fplot.

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

NodeId resolveLeadingAxesOpt(GraphicsScene &scene, keel::ArrayRef<Value> args, keel::usize &nextIdx)
{
    nextIdx = 0;
    if (!args.empty()) {
        NodeId id = kInvalidNodeId;
        if (extractSceneHandle(args[0], id) && isAxesLike(scene, id)) {
            nextIdx = 1;
            return id;
        }
    }
    return kInvalidNodeId;
}

inline bool isPlottableCoord(const Value &v) noexcept
{
    if (v.isNumeric() || v.isLogical())
        return true;
    if (v.isObject() && !isSceneGraphicsHandle(v)) {
        const keel::StringRef cn = v.objectClassNameRef();
        return cn == "datetime" || cn == "duration" || cn == "categorical";
    }
    return false;
}

inline bool isColorCharOrName(keel::StringRef s) noexcept
{
    return asciiEqualNoCase(s, "r") || asciiEqualNoCase(s, "red")
        || asciiEqualNoCase(s, "g") || asciiEqualNoCase(s, "green")
        || asciiEqualNoCase(s, "b") || asciiEqualNoCase(s, "blue")
        || asciiEqualNoCase(s, "c") || asciiEqualNoCase(s, "cyan")
        || asciiEqualNoCase(s, "m") || asciiEqualNoCase(s, "magenta")
        || asciiEqualNoCase(s, "y") || asciiEqualNoCase(s, "yellow")
        || asciiEqualNoCase(s, "k") || asciiEqualNoCase(s, "black")
        || asciiEqualNoCase(s, "w") || asciiEqualNoCase(s, "white");
}

// Parse MATLAB LineSpec (e.g. 'r--o', 'k:', 'b o-', '.'). Returns true iff every non-space char is a valid linespec token.
struct ParsedLineSpec {
    keel::String color;
    keel::String lineStyle;
    keel::String marker;
    bool valid = false;
};

ParsedLineSpec parseLineSpec(keel::StringRef s, keel::Allocator *mr)
{
    ParsedLineSpec out;
    if (s.empty() || s.size() > 16)
        return out;
    keel::usize i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (i + 1 < s.size() && c == '-' && s[i + 1] == '-') {
            if (!out.lineStyle.empty()) return ParsedLineSpec{};
            out.lineStyle = keel::String::from("--", mr);
            i += 2; continue;
        }
        if (i + 1 < s.size() && c == '-' && s[i + 1] == '.') {
            if (!out.lineStyle.empty()) return ParsedLineSpec{};
            out.lineStyle = keel::String::from("-.", mr);
            i += 2; continue;
        }
        if (c == '-' || c == ':') {
            if (!out.lineStyle.empty()) return ParsedLineSpec{};
            const char b[2] = {c, '\0'};
            out.lineStyle = keel::String::from(b, mr);
            ++i; continue;
        }
        if (c == 'r' || c == 'g' || c == 'b' || c == 'c' || c == 'm' || c == 'y' || c == 'k' || c == 'w') {
            if (!out.color.empty()) return ParsedLineSpec{};
            const char b[2] = {c, '\0'};
            out.color = keel::String::from(b, mr);
            ++i; continue;
        }
        if (c == 'o' || c == '+' || c == '*' || c == '.' || c == 'x' || c == '_' || c == '|'
            || c == 's' || c == 'd' || c == '^' || c == 'v' || c == '>' || c == '<' || c == 'p' || c == 'h') {
            if (!out.marker.empty()) return ParsedLineSpec{};
            const char b[2] = {c, '\0'};
            out.marker = keel::String::from(b, mr);
            ++i; continue;
        }
        return ParsedLineSpec{};
    }
    out.valid = (!out.color.empty() || !out.lineStyle.empty() || !out.marker.empty());
    return out;
}

void applyLineSpecToNode(GraphicsScene &scene, NodeId id, const ParsedLineSpec &ls, keel::Allocator *mr)
{
    if (!ls.valid)
        return;
    if (!ls.color.empty() && scene.hasProperty(id, "Color"))
        scene.setProperty(id, "Color", Value::fromString(ls.color, mr), mr);
    if (!ls.lineStyle.empty() && scene.hasProperty(id, "LineStyle"))
        scene.setProperty(id, "LineStyle", Value::fromString(ls.lineStyle, mr), mr);
    else if (ls.lineStyle.empty() && !ls.marker.empty() && scene.hasProperty(id, "LineStyle"))
        scene.setProperty(id, "LineStyle", Value::fromString("none", mr), mr);
    if (!ls.marker.empty() && scene.hasProperty(id, "Marker"))
        scene.setProperty(id, "Marker", Value::fromString(ls.marker, mr), mr);
}

void applyChartNV(GraphicsScene &scene, NodeId id,
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

void applyChartNVToAll(GraphicsScene &scene, keel::ArrayRef<NodeId> ids,
                       keel::ArrayRef<Value> args, keel::usize startIdx,
                       keel::Allocator *mr)
{
    for (NodeId id : ids)
        applyChartNV(scene, id, args, startIdx, mr);
}

Value makeRange1N(keel::usize n, keel::Allocator *mr)
{
    Value out = Value::matrix(1, n, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < n; ++i)
        out.doubleDataMut()[i] = keel::as<keel::f64>(i + 1);
    return out;
}

Value extractRowAsRowVec(const Value &m, keel::usize row, keel::Allocator *mr)
{
    const keel::usize nr = m.dims().rows();
    const keel::usize nc = m.dims().cols();
    Value out = Value::matrix(1, nc, ValueType::DOUBLE, mr);
    for (keel::usize c = 0; c < nc; ++c)
        out.doubleDataMut()[c] = m.elemAsDouble(row + c * nr);
    return out;
}

Value extractColAsRowVec(const Value &m, keel::usize col, keel::Allocator *mr)
{
    const keel::usize nr = m.dims().rows();
    Value out = Value::matrix(1, nr, ValueType::DOUBLE, mr);
    for (keel::usize r = 0; r < nr; ++r)
        out.doubleDataMut()[r] = m.elemAsDouble(r + col * nr);
    return out;
}

Value flattenToRowVec(const Value &v, keel::Allocator *mr)
{
    const keel::usize n = v.numel();
    Value out = Value::matrix(1, n, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < n; ++i)
        out.doubleDataMut()[i] = v.elemAsDouble(i);
    return out;
}

Value ensure2DDouble(const Value &v, keel::Allocator *mr)
{
    if (v.type() == ValueType::DOUBLE && !v.isComplex())
        return v;
    const keel::usize r = v.dims().rows();
    const keel::usize c = v.dims().cols();
    Value out = Value::matrix(r, c, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < r * c; ++i)
        out.doubleDataMut()[i] = v.elemAsDouble(i);
    return out;
}

// ── plot / plot3 / semilogx / semilogy / loglog / line ──────────────────

void plotCore(keel::ArrayRef<Value> args, keel::usize nargout,
              keel::MutArrayRef<Value> outs, CallContext &ctx,
              const char *xScale, const char *yScale, bool clearOnReplace)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, clearOnReplace);

    const ClassSchema &lineSchema = schemaFor(ClassId::Line);
    keel::Array<NodeId> created(mr);
    keel::usize nvStart = args.size();

    while (idx < args.size()) {
        if (args[idx].isStruct()) {
            nvStart = idx;
            break;
        }
        if (args[idx].isChar() || args[idx].isString()) {
            nvStart = idx;
            break;
        }
        if (!isPlottableCoord(args[idx])) {
            ++idx;
            continue;
        }
        const Value &a0 = args[idx++];
        const Value *yPtr = nullptr;
        if (idx < args.size() && isPlottableCoord(args[idx])) {
            yPtr = &args[idx++];
        }
        ParsedLineSpec ls;
        if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
            const keel::String tok = args[idx].toString();
            const bool isPropName = ((args.size() - idx) % 2 == 0) && (lineSchema.findExact(tok) != nullptr);
            if (!isPropName) {
                ParsedLineSpec cand = parseLineSpec(tok, mr);
                if (cand.valid) {
                    ls = keel::move(cand);
                    ++idx;
                }
            }
        }

        auto addOneLine = [&](const Value &xv, const Value &yv) {
            const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
            if (GraphicsNode *ln = scene.findNode(lid)) {
                ln->props.put(keel::String::from("XData", mr), xv);
                ln->props.put(keel::String::from("YData", mr), yv);
            }
            applyLineSpecToNode(scene, lid, ls, mr);
            created.push_back(lid);
        };

        if (yPtr == nullptr) {
            // plot(Y) — complex vector plots real vs imag; matrix plots each column vs 1:rows.
            if (a0.isComplex()) {
                const keel::usize n = a0.numel();
                Value xv = Value::matrix(1, n, ValueType::DOUBLE, mr);
                Value yv = Value::matrix(1, n, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < n; ++i) {
                    xv.doubleDataMut()[i] = a0.complexData()[i].re;
                    yv.doubleDataMut()[i] = a0.complexData()[i].im;
                }
                addOneLine(xv, yv);
            } else if (a0.dims().rows() > 1 && a0.dims().cols() > 1) {
                const Value xv = makeRange1N(a0.dims().rows(), mr);
                for (keel::usize c = 0; c < a0.dims().cols(); ++c)
                    addOneLine(xv, extractColAsRowVec(a0, c, mr));
            } else {
                const Value yv = flattenToRowVec(a0, mr);
                const Value xv = makeRange1N(yv.numel(), mr);
                addOneLine(xv, yv);
            }
        } else {
            const Value &xArg = a0;
            const Value &yArg = *yPtr;
            const bool xVec = (xArg.dims().rows() == 1 || xArg.dims().cols() == 1);
            const bool yMat = (yArg.dims().rows() > 1 && yArg.dims().cols() > 1);
            if (xVec && yMat) {
                const Value xv = flattenToRowVec(xArg, mr);
                if (yArg.dims().rows() == xv.numel()) {
                    for (keel::usize c = 0; c < yArg.dims().cols(); ++c)
                        addOneLine(xv, extractColAsRowVec(yArg, c, mr));
                } else if (yArg.dims().cols() == xv.numel()) {
                    for (keel::usize r = 0; r < yArg.dims().rows(); ++r)
                        addOneLine(xv, extractRowAsRowVec(yArg, r, mr));
                } else {
                    addOneLine(xv, flattenToRowVec(yArg, mr));
                }
            } else if (!xVec && !yMat && (xArg.dims().rows() > 1 && xArg.dims().cols() > 1)) {
                const Value yv = flattenToRowVec(yArg, mr);
                if (xArg.dims().rows() == yv.numel()) {
                    for (keel::usize c = 0; c < xArg.dims().cols(); ++c)
                        addOneLine(extractColAsRowVec(xArg, c, mr), yv);
                } else {
                    for (keel::usize r = 0; r < xArg.dims().rows(); ++r)
                        addOneLine(extractRowAsRowVec(xArg, r, mr), yv);
                }
            } else if (!xVec && yMat && xArg.dims() == yArg.dims()) {
                for (keel::usize c = 0; c < yArg.dims().cols(); ++c)
                    addOneLine(extractColAsRowVec(xArg, c, mr), extractColAsRowVec(yArg, c, mr));
            } else {
                addOneLine(flattenToRowVec(xArg, mr), flattenToRowVec(yArg, mr));
            }
        }
    }

    if (created.empty() && !clearOnReplace) {
        // Low-level line('XData', ..., 'YData', ...) form
        const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
        if (GraphicsNode *ln = scene.findNode(lid)) {
            Value def = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            def.doubleDataMut()[0] = 0.0; def.doubleDataMut()[1] = 1.0;
            ln->props.put(keel::String::from("XData", mr), def);
            ln->props.put(keel::String::from("YData", mr), def);
        }
        created.push_back(lid);
    }

    if (nvStart < args.size() && !created.empty())
        applyChartNVToAll(scene, created.as_ref(), args, nvStart, mr);

    if (xScale) scene.setProperty(axId, "XScale", Value::fromString(xScale, mr), mr);
    if (yScale) scene.setProperty(axId, "YScale", Value::fromString(yScale, mr), mr);
    scene.recomputeAxesAutoLimits(axId, mr);

    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void plot_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ plotCore(a, no, o, c, nullptr, nullptr, true); }
void semilogx_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ plotCore(a, no, o, c, "log", nullptr, true); }
void semilogy_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ plotCore(a, no, o, c, nullptr, "log", true); }
void loglog_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ plotCore(a, no, o, c, "log", "log", true); }

void line_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, false);

    // Check 3-arg numeric form: line(x, y, z, NV...)
    if (idx + 2 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric() && args[idx + 2].isNumeric()) {
        const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
        if (GraphicsNode *ln = scene.findNode(lid)) {
            ln->props.put(keel::String::from("XData", mr), flattenToRowVec(args[idx], mr));
            ln->props.put(keel::String::from("YData", mr), flattenToRowVec(args[idx + 1], mr));
            ln->props.put(keel::String::from("ZData", mr), flattenToRowVec(args[idx + 2], mr));
        }
        if (idx + 3 < args.size())
            applyChartNV(scene, lid, args, idx + 3, mr);
        scene.recomputeAxesAutoLimits(axId, mr);
        outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
        return;
    }
    plotCore(args, nargout, outs, ctx, nullptr, nullptr, false);
}

void plot3_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    const ClassSchema &lineSchema = schemaFor(ClassId::Line);
    keel::Array<NodeId> created(mr);
    keel::usize nvStart = args.size();
    while (idx + 2 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric() && args[idx + 2].isNumeric()) {
        const Value xv = flattenToRowVec(args[idx], mr);
        const Value yv = flattenToRowVec(args[idx + 1], mr);
        const Value zv = flattenToRowVec(args[idx + 2], mr);
        idx += 3;
        ParsedLineSpec ls;
        if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
            const keel::String tok = args[idx].toString();
            if (!(((args.size() - idx) % 2 == 0) && lineSchema.findExact(tok) != nullptr)) {
                ParsedLineSpec cand = parseLineSpec(tok, mr);
                if (cand.valid) { ls = keel::move(cand); ++idx; }
            }
        }
        const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
        if (GraphicsNode *ln = scene.findNode(lid)) {
            ln->props.put(keel::String::from("XData", mr), xv);
            ln->props.put(keel::String::from("YData", mr), yv);
            ln->props.put(keel::String::from("ZData", mr), zv);
        }
        applyLineSpecToNode(scene, lid, ls, mr);
        created.push_back(lid);
    }
    if (idx < args.size()) nvStart = idx;
    if (nvStart < args.size() && !created.empty())
        applyChartNVToAll(scene, created.as_ref(), args, nvStart, mr);

    Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
    scene.setProperty(axId, "View", vw, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

// ── scatter / scatter3 ──────────────────────────────────────────────────

void scatterCore(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx, bool is3D)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    const NodeId sid = scene.createChartNode(ClassId::Scatter, axId, true);
    if (idx + (is3D ? 2 : 1) < args.size()) {
        if (GraphicsNode *sn = scene.findNode(sid)) {
            sn->props.put(keel::String::from("XData", mr), flattenToRowVec(args[idx++], mr));
            sn->props.put(keel::String::from("YData", mr), flattenToRowVec(args[idx++], mr));
            if (is3D)
                sn->props.put(keel::String::from("ZData", mr), flattenToRowVec(args[idx++], mr));
        }
    }
    // Optional SizeData (numeric or empty [])
    if (idx < args.size() && args[idx].isNumeric()) {
        if (!args[idx].isEmpty())
            scene.setProperty(sid, "SizeData", flattenToRowVec(args[idx], mr), mr);
        ++idx;
    }
    // Optional Color C (numeric vector/matrix or color string)
    const ClassSchema &scSchema = schemaFor(ClassId::Scatter);
    if (idx < args.size()) {
        if (args[idx].isNumeric()) {
            if (!args[idx].isEmpty())
                scene.setProperty(sid, "CData", ensure2DDouble(args[idx], mr), mr);
            ++idx;
        } else if (args[idx].isChar() || args[idx].isString()) {
            const keel::String tok = args[idx].toString();
            if (asciiEqualNoCase(tok, "filled")) {
                scene.setProperty(sid, "MarkerFaceColor", Value::fromString("flat", mr), mr);
                ++idx;
            } else if (!(((args.size() - idx) % 2 == 0) && scSchema.findExact(tok) != nullptr)) {
                ParsedLineSpec ls = parseLineSpec(tok, mr);
                if (ls.valid) {
                    if (!ls.color.empty()) {
                        Value rgb = parseColorSpec(Value::fromString(ls.color, mr), false, false, "CData", mr);
                        scene.setProperty(sid, "CData", rgb, mr);
                    }
                    if (!ls.marker.empty())
                        scene.setProperty(sid, "Marker", Value::fromString(ls.marker, mr), mr);
                    ++idx;
                }
            }
        }
    }
    // Check trailing 'filled' or marker token before NV pairs
    while (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "filled")) {
            scene.setProperty(sid, "MarkerFaceColor", Value::fromString("flat", mr), mr);
            ++idx;
            continue;
        }
        if (((args.size() - idx) % 2 == 0) && scSchema.findExact(tok) != nullptr)
            break;
        ParsedLineSpec ls = parseLineSpec(tok, mr);
        if (ls.valid && !ls.marker.empty()) {
            scene.setProperty(sid, "Marker", Value::fromString(ls.marker, mr), mr);
            ++idx;
            continue;
        }
        break;
    }

    if (idx < args.size())
        applyChartNV(scene, sid, args, idx, mr);
    if (is3D) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(sid, mr) : Value();
}

void scatter_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ scatterCore(a, no, o, c, false); }
void scatter3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ scatterCore(a, no, o, c, true); }

// ── bar / barh ──────────────────────────────────────────────────────────

void barCore(keel::ArrayRef<Value> args, keel::usize nargout,
             keel::MutArrayRef<Value> outs, CallContext &ctx, bool horizontal)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    Value xArg, yArg;
    bool hasX = false;
    keel::f64 width = 0.8;
    keel::String layout = keel::String::from("grouped", mr);
    keel::String colorSpec(mr);

    if (idx < args.size() && isPlottableCoord(args[idx])) {
        if (idx + 1 < args.size() && isPlottableCoord(args[idx + 1]) && args[idx + 1].numel() > 1) {
            xArg = args[idx++];
            yArg = args[idx++];
            hasX = true;
        } else {
            yArg = args[idx++];
        }
    }
    if (idx < args.size() && args[idx].isNumeric() && args[idx].numel() == 1) {
        width = args[idx++].toScalar();
    }
    const ClassSchema &barSchema = schemaFor(ClassId::Bar);
    while (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "grouped") || asciiEqualNoCase(tok, "stacked")
            || asciiEqualNoCase(tok, "hist") || asciiEqualNoCase(tok, "histc")) {
            if (asciiEqualNoCase(tok, "grouped") || asciiEqualNoCase(tok, "stacked"))
                layout = tok.clone(mr);
            else
                width = 1.0;
            ++idx;
            continue;
        }
        if (((args.size() - idx) % 2 == 0) && barSchema.findExact(tok) != nullptr)
            break;
        if (isColorCharOrName(tok)) {
            colorSpec = tok.clone(mr);
            ++idx;
            continue;
        }
        break;
    }

    keel::Array<NodeId> created(mr);
    auto addBar = [&](const Value &xv, const Value &yv) {
        const NodeId bid = scene.createChartNode(ClassId::Bar, axId, true);
        if (GraphicsNode *bn = scene.findNode(bid)) {
            bn->props.put(keel::String::from("XData", mr), xv);
            bn->props.put(keel::String::from("YData", mr), yv);
            bn->props.put(keel::String::from("BarWidth", mr), Value::scalar(width, mr));
            bn->props.put(keel::String::from("BarLayout", mr), Value::fromString(layout, mr));
            bn->props.put(keel::String::from("Horizontal", mr), Value::fromString(horizontal ? "on" : "off", mr));
        }
        if (!colorSpec.empty())
            scene.setProperty(bid, "FaceColor", Value::fromString(colorSpec, mr), mr);
        created.push_back(bid);
    };

    if (!yArg.isEmpty()) {
        const bool yMat = (yArg.dims().rows() > 1 && yArg.dims().cols() > 1);
        if (!yMat) {
            const Value yv = flattenToRowVec(yArg, mr);
            const Value xv = hasX ? flattenToRowVec(xArg, mr) : makeRange1N(yv.numel(), mr);
            addBar(xv, yv);
        } else {
            const Value xv = hasX ? flattenToRowVec(xArg, mr) : makeRange1N(yArg.dims().rows(), mr);
            if (hasX && yArg.dims().cols() == xv.numel() && yArg.dims().rows() != xv.numel()) {
                for (keel::usize r = 0; r < yArg.dims().rows(); ++r)
                    addBar(xv, extractRowAsRowVec(yArg, r, mr));
            } else {
                for (keel::usize c = 0; c < yArg.dims().cols(); ++c)
                    addBar(xv, extractColAsRowVec(yArg, c, mr));
            }
        }
    }

    if (idx < args.size() && !created.empty())
        applyChartNVToAll(scene, created.as_ref(), args, idx, mr);

    scene.recomputeBarEndPoints(axId, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void bar_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ barCore(a, no, o, c, false); }
void barh_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ barCore(a, no, o, c, true); }

// ── surf / mesh / surfc / meshc / pcolor / surface ──────────────────────

void surfaceCore(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx,
                 bool isMesh, bool isPcolor, bool clearOnReplace)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, clearOnReplace);

    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == 4) break;
    }

    Value xd, yd, zd, cd;
    if (nums.size() == 1) {
        zd = ensure2DDouble(*nums[0], mr);
        cd = zd;
        xd = makeRange1N(zd.dims().cols(), mr);
        yd = makeRange1N(zd.dims().rows(), mr);
    } else if (nums.size() == 2) {
        zd = ensure2DDouble(*nums[0], mr);
        cd = ensure2DDouble(*nums[1], mr);
        xd = makeRange1N(zd.dims().cols(), mr);
        yd = makeRange1N(zd.dims().rows(), mr);
    } else if (nums.size() >= 3) {
        xd = ensure2DDouble(*nums[0], mr);
        yd = ensure2DDouble(*nums[1], mr);
        zd = ensure2DDouble(*nums[2], mr);
        cd = (nums.size() >= 4) ? ensure2DDouble(*nums[3], mr) : zd;
    }

    const NodeId sid = scene.createChartNode(ClassId::Surface, axId, false);
    if (GraphicsNode *sn = scene.findNode(sid)) {
        if (!xd.isEmpty()) sn->props.put(keel::String::from("XData", mr), xd);
        if (!yd.isEmpty()) sn->props.put(keel::String::from("YData", mr), yd);
        if (!zd.isEmpty()) sn->props.put(keel::String::from("ZData", mr), zd);
        if (!cd.isEmpty()) sn->props.put(keel::String::from("CData", mr), cd);
        if (isMesh) {
            Value w = Value::matrix(1, 3, ValueType::DOUBLE, mr);
            w.doubleDataMut()[0] = 1.0; w.doubleDataMut()[1] = 1.0; w.doubleDataMut()[2] = 1.0;
            sn->props.put(keel::String::from("FaceColor", mr), keel::move(w));
            sn->props.put(keel::String::from("EdgeColor", mr), Value::fromString("flat", mr));
        }
    }
    if (idx < args.size())
        applyChartNV(scene, sid, args, idx, mr);

    if (!isPcolor && clearOnReplace) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(sid, mr) : Value();
}

void surf_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ surfaceCore(a, no, o, c, false, false, true); }
void mesh_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ surfaceCore(a, no, o, c, true, false, true); }
void pcolor_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ surfaceCore(a, no, o, c, false, true, true); }
void surface_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ surfaceCore(a, no, o, c, false, false, false); }

// ── patch / fill / fill3 ────────────────────────────────────────────────

void patchCore(keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx, bool is3D, bool clearOnReplace)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, clearOnReplace);

    const NodeId pid = scene.createChartNode(ClassId::Patch, axId, false);
    if (idx < args.size() && isPlottableCoord(args[idx])) {
        const Value xd = ensure2DDouble(args[idx++], mr);
        Value yd, zd;
        if (idx < args.size() && isPlottableCoord(args[idx]))
            yd = ensure2DDouble(args[idx++], mr);
        if (is3D && idx < args.size() && isPlottableCoord(args[idx]))
            zd = ensure2DDouble(args[idx++], mr);
        else if (!is3D && idx + 1 < args.size() && isPlottableCoord(args[idx])
                 && (args[idx + 1].isChar() || args[idx + 1].isString() || args[idx + 1].isNumeric())) {
            // patch(X, Y, Z, C): distinguish Z from 3-element RGB triplet C followed by NV pairs
            const bool looksLikeRgbBeforeNV = (args[idx].numel() == 3 && xd.numel() != 3)
                || (args[idx].numel() == 3 && ((args.size() - idx - 1) % 2 == 0)
                    && (args[idx + 1].isChar() || args[idx + 1].isString())
                    && schemaFor(ClassId::Patch).findExact(args[idx + 1].toString()) != nullptr);
            if (!looksLikeRgbBeforeNV && args[idx].numel() == xd.numel())
                zd = ensure2DDouble(args[idx++], mr);
        }
        if (GraphicsNode *pn = scene.findNode(pid)) {
            pn->props.put(keel::String::from("XData", mr), xd);
            if (!yd.isEmpty()) pn->props.put(keel::String::from("YData", mr), yd);
            if (!zd.isEmpty()) pn->props.put(keel::String::from("ZData", mr), zd);
        }
        scene.syncPatchVerticesAndData(pid, "XData", mr);

        // Optional color C
        const ClassSchema &pSchema = schemaFor(ClassId::Patch);
        if (idx < args.size()) {
            if (args[idx].isChar() || args[idx].isString()) {
                const keel::String tok = args[idx].toString();
                if (!(((args.size() - idx) % 2 == 0) && pSchema.findExact(tok) != nullptr)) {
                    scene.setProperty(pid, "FaceColor", args[idx++], mr);
                }
            } else if (args[idx].isNumeric()) {
                if (args[idx].numel() == 3)
                    scene.setProperty(pid, "FaceColor", args[idx++], mr);
                else
                    scene.setProperty(pid, "CData", ensure2DDouble(args[idx++], mr), mr);
            }
        }
    }
    if (idx < args.size())
        applyChartNV(scene, pid, args, idx, mr);
    if (is3D) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(pid, mr) : Value();
}

void patch_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ patchCore(a, no, o, c, false, false); }
void fill_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ patchCore(a, no, o, c, false, true); }
void fill3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ patchCore(a, no, o, c, true, true); }

// ── quiver / quiver3 ────────────────────────────────────────────────────

void quiverCore(keel::ArrayRef<Value> args, keel::usize nargout,
                keel::MutArrayRef<Value> outs, CallContext &ctx, bool is3D)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == (is3D ? 6u : 4u)) break;
    }

    Value xd, yd, zd, ud, vd, wd;
    if (!is3D) {
        if (nums.size() == 2) {
            ud = ensure2DDouble(*nums[0], mr);
            vd = ensure2DDouble(*nums[1], mr);
            xd = makeRange1N(ud.numel(), mr);
            yd = makeRange1N(ud.numel(), mr);
        } else if (nums.size() >= 4) {
            xd = ensure2DDouble(*nums[0], mr);
            yd = ensure2DDouble(*nums[1], mr);
            ud = ensure2DDouble(*nums[2], mr);
            vd = ensure2DDouble(*nums[3], mr);
        }
    } else if (nums.size() >= 6) {
        xd = ensure2DDouble(*nums[0], mr);
        yd = ensure2DDouble(*nums[1], mr);
        zd = ensure2DDouble(*nums[2], mr);
        ud = ensure2DDouble(*nums[3], mr);
        vd = ensure2DDouble(*nums[4], mr);
        wd = ensure2DDouble(*nums[5], mr);
    }

    const NodeId qid = scene.createChartNode(ClassId::Quiver, axId, true);
    if (GraphicsNode *qn = scene.findNode(qid)) {
        if (!xd.isEmpty()) qn->props.put(keel::String::from("XData", mr), xd);
        if (!yd.isEmpty()) qn->props.put(keel::String::from("YData", mr), yd);
        if (!zd.isEmpty()) qn->props.put(keel::String::from("ZData", mr), zd);
        if (!ud.isEmpty()) qn->props.put(keel::String::from("UData", mr), ud);
        if (!vd.isEmpty()) qn->props.put(keel::String::from("VData", mr), vd);
        if (!wd.isEmpty()) qn->props.put(keel::String::from("WData", mr), wd);
    }
    // Optional numeric scale argument (scale == 0 disables AutoScale)
    if (idx < args.size() && args[idx].isNumeric() && args[idx].numel() == 1) {
        const keel::f64 sc = args[idx++].toScalar();
        if (sc == 0.0) {
            scene.setProperty(qid, "AutoScale", Value::fromString("off", mr), mr);
        } else {
            scene.setProperty(qid, "AutoScale", Value::fromString("on", mr), mr);
            scene.setProperty(qid, "AutoScaleFactor", Value::scalar(sc, mr), mr);
        }
    }
    // Optional 'filled' and/or LineSpec before Name-Value pairs
    const ClassSchema &qSchema = schemaFor(ClassId::Quiver);
    while (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "filled")) {
            scene.setProperty(qid, "MarkerFaceColor", scene.getProperty(qid, "Color", mr), mr);
            ++idx;
            continue;
        }
        if (((args.size() - idx) % 2 == 0) && qSchema.findExact(tok) != nullptr)
            break;
        ParsedLineSpec ls = parseLineSpec(tok, mr);
        if (ls.valid) {
            applyLineSpecToNode(scene, qid, ls, mr);
            ++idx;
            continue;
        }
        break;
    }
    if (idx < args.size())
        applyChartNV(scene, qid, args, idx, mr);
    if (is3D) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(qid, mr) : Value();
}

void quiver_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ quiverCore(a, no, o, c, false); }
void quiver3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ quiverCore(a, no, o, c, true); }

// ── contour / contourf / contour3 ───────────────────────────────────────

void contourCore(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx, bool filled, bool is3D)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == 4) break;
    }

    Value xd, yd, zd;
    keel::usize nLevels = 7;
    Value explicitLevels;
    if (nums.size() == 1) {
        zd = ensure2DDouble(*nums[0], mr);
        xd = makeRange1N(zd.dims().cols(), mr);
        yd = makeRange1N(zd.dims().rows(), mr);
    } else if (nums.size() == 2) {
        zd = ensure2DDouble(*nums[0], mr);
        xd = makeRange1N(zd.dims().cols(), mr);
        yd = makeRange1N(zd.dims().rows(), mr);
        if (nums[1]->numel() == 1) nLevels = keel::max<keel::usize>(1, keel::as<keel::usize>(nums[1]->toScalar()));
        else explicitLevels = flattenToRowVec(*nums[1], mr);
    } else if (nums.size() >= 3) {
        xd = ensure2DDouble(*nums[0], mr);
        yd = ensure2DDouble(*nums[1], mr);
        zd = ensure2DDouble(*nums[2], mr);
        if (nums.size() >= 4) {
            if (nums[3]->numel() == 1) nLevels = keel::max<keel::usize>(1, keel::as<keel::usize>(nums[3]->toScalar()));
            else explicitLevels = flattenToRowVec(*nums[3], mr);
        }
    }

    // Deduplicate & sort explicit contour levels (e.g. contour(X,Y,Z,[v v]) -> single level [v]).
    if (!explicitLevels.isEmpty() && explicitLevels.numel() > 1) {
        keel::Array<keel::f64> lv(mr);
        lv.reserve(explicitLevels.numel());
        for (keel::usize i = 0; i < explicitLevels.numel(); ++i)
            lv.push_back(explicitLevels.elemAsDouble(i));
        keel::sort(lv);
        keel::Array<keel::f64> uniq(mr);
        for (keel::f64 v : lv) {
            if (uniq.empty() || uniq.back() != v)
                uniq.push_back(v);
        }
        if (uniq.size() != explicitLevels.numel()) {
            Value ul = Value::matrix(1, uniq.size(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < uniq.size(); ++i)
                ul.doubleDataMut()[i] = uniq[i];
            explicitLevels = keel::move(ul);
        }
    }

    // Compute default LevelList across Z range.
    if (explicitLevels.isEmpty() && !zd.isEmpty()) {
        keel::f64 zmin = 1e300, zmax = -1e300;
        for (keel::usize i = 0; i < zd.numel(); ++i) {
            const keel::f64 v = zd.elemAsDouble(i);
            if (keel::is_finite(v)) { zmin = keel::min(zmin, v); zmax = keel::max(zmax, v); }
        }
        if (zmax > zmin) {
            explicitLevels = Value::matrix(1, nLevels, ValueType::DOUBLE, mr);
            for (keel::usize k = 0; k < nLevels; ++k)
                explicitLevels.doubleDataMut()[k] = zmin + (zmax - zmin) * keel::as<keel::f64>(k + 1) / keel::as<keel::f64>(nLevels + 1);
        }
    }

    const NodeId cid = scene.createChartNode(ClassId::Contour, axId, false);
    if (GraphicsNode *cn = scene.findNode(cid)) {
        if (!xd.isEmpty()) cn->props.put(keel::String::from("XData", mr), xd);
        if (!yd.isEmpty()) cn->props.put(keel::String::from("YData", mr), yd);
        if (!zd.isEmpty()) cn->props.put(keel::String::from("ZData", mr), zd);
        if (!explicitLevels.isEmpty()) cn->props.put(keel::String::from("LevelList", mr), explicitLevels);
        if (filled) cn->props.put(keel::String::from("Fill", mr), Value::fromString("on", mr));
    }
    if (idx < args.size())
        applyChartNV(scene, cid, args, idx, mr);
    if (is3D) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);

    if (nargout >= 2) {
        outs[0] = scene.getProperty(cid, "ContourMatrix", mr);
        outs[1] = scene.makeHandleValue(cid, mr);
    } else if (nargout == 1) {
        outs[0] = scene.makeHandleValue(cid, mr);
    } else {
        outs[0] = Value();
    }
}

void contour_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ contourCore(a, no, o, c, false, false); }
void contourf_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ contourCore(a, no, o, c, true, false); }
void contour3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ contourCore(a, no, o, c, false, true); }

// ── stem / stem3 / area / stairs / errorbar ─────────────────────────────

void stemCore(keel::ArrayRef<Value> args, keel::usize nargout,
              keel::MutArrayRef<Value> outs, CallContext &ctx, bool is3D)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    Value xd, yd, zd;
    if (!is3D) {
        if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
            xd = flattenToRowVec(args[idx++], mr);
            yd = flattenToRowVec(args[idx++], mr);
        } else if (idx < args.size() && args[idx].isNumeric()) {
            yd = flattenToRowVec(args[idx++], mr);
            xd = makeRange1N(yd.numel(), mr);
        }
    } else if (idx + 2 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric() && args[idx + 2].isNumeric()) {
        xd = flattenToRowVec(args[idx++], mr);
        yd = flattenToRowVec(args[idx++], mr);
        zd = flattenToRowVec(args[idx++], mr);
    }

    const NodeId stId = scene.createChartNode(ClassId::Stem, axId, true);
    if (GraphicsNode *sn = scene.findNode(stId)) {
        if (!xd.isEmpty()) sn->props.put(keel::String::from("XData", mr), xd);
        if (!yd.isEmpty()) sn->props.put(keel::String::from("YData", mr), yd);
        if (!zd.isEmpty()) sn->props.put(keel::String::from("ZData", mr), zd);
    }
    const ClassSchema &sSchema = schemaFor(ClassId::Stem);
    while (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (asciiEqualNoCase(tok, "filled")) {
            scene.setProperty(stId, "MarkerFaceColor", scene.getProperty(stId, "Color", mr), mr);
            ++idx;
            continue;
        }
        if (((args.size() - idx) % 2 == 0) && sSchema.findExact(tok) != nullptr) break;
        ParsedLineSpec ls = parseLineSpec(tok, mr);
        if (ls.valid) { applyLineSpecToNode(scene, stId, ls, mr); ++idx; continue; }
        break;
    }
    if (idx < args.size())
        applyChartNV(scene, stId, args, idx, mr);
    if (is3D) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(stId, mr) : Value();
}

void stem_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ stemCore(a, no, o, c, false); }
void stem3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ stemCore(a, no, o, c, true); }

void areaStairCore(ClassId cls, keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    Value xArg, yArg;
    bool hasX = false;
    bool hasBaseValue = false;
    keel::f64 baseValue = 0.0;
    if (idx + 1 < args.size() && isPlottableCoord(args[idx]) && isPlottableCoord(args[idx + 1])) {
        if (cls == ClassId::Area && args[idx + 1].isNumeric() && args[idx + 1].numel() == 1
            && args[idx].numel() > 1 && (idx + 2 >= args.size() || !isPlottableCoord(args[idx + 2]))) {
            // area(Y, basevalue, ...)
            yArg = args[idx++];
            baseValue = args[idx++].toScalar();
            hasBaseValue = true;
        } else {
            xArg = args[idx++];
            yArg = args[idx++];
            hasX = true;
            if (cls == ClassId::Area && idx < args.size() && args[idx].isNumeric() && args[idx].numel() == 1) {
                // area(X, Y, basevalue, ...)
                baseValue = args[idx++].toScalar();
                hasBaseValue = true;
            }
        }
    } else if (idx < args.size() && isPlottableCoord(args[idx])) {
        yArg = args[idx++];
    }

    ParsedLineSpec ls;
    if (cls == ClassId::Stair && idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        const ClassSchema &stSchema = schemaFor(ClassId::Stair);
        if (!(((args.size() - idx) % 2 == 0) && stSchema.findExact(tok) != nullptr)) {
            ParsedLineSpec cand = parseLineSpec(tok, mr);
            if (cand.valid) {
                ls = keel::move(cand);
                ++idx;
            }
        }
    }

    keel::Array<NodeId> created(mr);
    if (!yArg.isEmpty()) {
        const bool yMat = (yArg.dims().rows() > 1 && yArg.dims().cols() > 1);
        if (!yMat) {
            const Value yv = flattenToRowVec(yArg, mr);
            const Value xv = hasX ? flattenToRowVec(xArg, mr) : makeRange1N(yv.numel(), mr);
            const NodeId id = scene.createChartNode(cls, axId, true);
            if (GraphicsNode *n = scene.findNode(id)) {
                n->props.put(keel::String::from("XData", mr), xv);
                n->props.put(keel::String::from("YData", mr), yv);
                if (hasBaseValue)
                    n->props.put(keel::String::from("BaseValue", mr), Value::scalar(baseValue, mr));
            }
            if (ls.valid)
                applyLineSpecToNode(scene, id, ls, mr);
            created.push_back(id);
        } else {
            const Value xv = hasX ? flattenToRowVec(xArg, mr) : makeRange1N(yArg.dims().rows(), mr);
            for (keel::usize c = 0; c < yArg.dims().cols(); ++c) {
                const NodeId id = scene.createChartNode(cls, axId, true);
                if (GraphicsNode *n = scene.findNode(id)) {
                    n->props.put(keel::String::from("XData", mr), xv);
                    n->props.put(keel::String::from("YData", mr), extractColAsRowVec(yArg, c, mr));
                    if (hasBaseValue)
                        n->props.put(keel::String::from("BaseValue", mr), Value::scalar(baseValue, mr));
                }
                if (ls.valid)
                    applyLineSpecToNode(scene, id, ls, mr);
                created.push_back(id);
            }
        }
    }
    if (idx < args.size() && !created.empty())
        applyChartNVToAll(scene, created.as_ref(), args, idx, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void area_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ areaStairCore(ClassId::Area, a, no, o, c); }
void stairs_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ areaStairCore(ClassId::Stair, a, no, o, c); }

void errorbar_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == 6) break;
    }

    Value xd, yd, yneg, ypos, xneg, xpos;
    if (nums.size() == 2) {
        yd = flattenToRowVec(*nums[0], mr);
        xd = makeRange1N(yd.numel(), mr);
        yneg = ypos = flattenToRowVec(*nums[1], mr);
    } else if (nums.size() == 3) {
        xd = flattenToRowVec(*nums[0], mr);
        yd = flattenToRowVec(*nums[1], mr);
        yneg = ypos = flattenToRowVec(*nums[2], mr);
    } else if (nums.size() == 4) {
        xd = flattenToRowVec(*nums[0], mr);
        yd = flattenToRowVec(*nums[1], mr);
        yneg = flattenToRowVec(*nums[2], mr);
        ypos = flattenToRowVec(*nums[3], mr);
    } else if (nums.size() >= 6) {
        xd = flattenToRowVec(*nums[0], mr);
        yd = flattenToRowVec(*nums[1], mr);
        yneg = flattenToRowVec(*nums[2], mr);
        ypos = flattenToRowVec(*nums[3], mr);
        xneg = flattenToRowVec(*nums[4], mr);
        xpos = flattenToRowVec(*nums[5], mr);
    }

    const NodeId eid = scene.createChartNode(ClassId::ErrorBar, axId, true);
    if (GraphicsNode *en = scene.findNode(eid)) {
        if (!xd.isEmpty()) en->props.put(keel::String::from("XData", mr), xd);
        if (!yd.isEmpty()) en->props.put(keel::String::from("YData", mr), yd);
        if (!yneg.isEmpty()) en->props.put(keel::String::from("YNegativeDelta", mr), yneg);
        if (!ypos.isEmpty()) en->props.put(keel::String::from("YPositiveDelta", mr), ypos);
        if (!xneg.isEmpty()) en->props.put(keel::String::from("XNegativeDelta", mr), xneg);
        if (!xpos.isEmpty()) en->props.put(keel::String::from("XPositiveDelta", mr), xpos);
    }
    const ClassSchema &eSchema = schemaFor(ClassId::ErrorBar);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (!(((args.size() - idx) % 2 == 0) && eSchema.findExact(tok) != nullptr)) {
            ParsedLineSpec ls = parseLineSpec(tok, mr);
            if (ls.valid) { applyLineSpecToNode(scene, eid, ls, mr); ++idx; }
        }
    }
    if (idx < args.size())
        applyChartNV(scene, eid, args, idx, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(eid, mr) : Value();
}

// ── histogram ───────────────────────────────────────────────────────────

void histogram_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    const NodeId hid = scene.createChartNode(ClassId::Histogram, axId, true);
    if (idx < args.size() && args[idx].isNumeric()) {
        if (GraphicsNode *hn = scene.findNode(hid))
            hn->props.put(keel::String::from("Data", mr), flattenToRowVec(args[idx++], mr));
    }
    if (idx < args.size() && args[idx].isNumeric()) {
        if (args[idx].numel() == 1) {
            if (GraphicsNode *hn = scene.findNode(hid)) {
                hn->props.put(keel::String::from("NumBins", mr), Value::scalar(args[idx].toScalar(), mr));
                hn->props.put(keel::String::from("_NumBinsExplicit", mr), Value::logicalScalar(true, mr));
            }
        } else if (args[idx].numel() >= 2) {
            if (GraphicsNode *hn = scene.findNode(hid)) {
                hn->props.put(keel::String::from("BinEdges", mr), flattenToRowVec(args[idx], mr));
                hn->props.put(keel::String::from("_EdgesExplicit", mr), Value::logicalScalar(true, mr));
            }
        }
        ++idx;
    }

    if (idx < args.size())
        applyChartNV(scene, hid, args, idx, mr);
    scene.recomputeHistogramBins(hid, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(hid, mr) : Value();
}

// ── image / imagesc ─────────────────────────────────────────────────────

void imageCore(keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, CallContext &ctx, bool scaled)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, scaled);

    Value xd, yd, cd;
    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == 4) break;
    }

    Value climArg;
    if (nums.size() == 1) {
        cd = *nums[0];
    } else if (nums.size() == 2) {
        cd = *nums[0];
        if (scaled && nums[1]->numel() == 2) climArg = *nums[1];
    } else if (nums.size() >= 3) {
        xd = flattenToRowVec(*nums[0], mr);
        yd = flattenToRowVec(*nums[1], mr);
        cd = *nums[2];
        if (scaled && nums.size() >= 4 && nums[3]->numel() == 2) climArg = *nums[3];
    }

    const NodeId iid = scene.createChartNode(ClassId::Image, axId, false);
    if (GraphicsNode *in = scene.findNode(iid)) {
        if (!cd.isEmpty()) {
            in->props.put(keel::String::from("CData", mr), cd);
            if (xd.isEmpty()) {
                Value defX = Value::matrix(1, 2, ValueType::DOUBLE, mr);
                defX.doubleDataMut()[0] = 1.0;
                defX.doubleDataMut()[1] = keel::as<keel::f64>(keel::max<keel::usize>(1, cd.dims().cols()));
                xd = keel::move(defX);
            }
            if (yd.isEmpty()) {
                Value defY = Value::matrix(1, 2, ValueType::DOUBLE, mr);
                defY.doubleDataMut()[0] = 1.0;
                defY.doubleDataMut()[1] = keel::as<keel::f64>(keel::max<keel::usize>(1, cd.dims().rows()));
                yd = keel::move(defY);
            }
        }
        if (!xd.isEmpty()) in->props.put(keel::String::from("XData", mr), xd);
        if (!yd.isEmpty()) in->props.put(keel::String::from("YData", mr), yd);
        in->props.put(keel::String::from("CDataMapping", mr),
                      Value::fromString(scaled ? "scaled" : "direct", mr));
    }
    if (idx < args.size())
        applyChartNV(scene, iid, args, idx, mr);
    if (scaled) {
        scene.setProperty(axId, "YDir", Value::fromString("reverse", mr), mr);
        if (!climArg.isEmpty())
            scene.setProperty(axId, "CLim", climArg, mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(iid, mr) : Value();
}

void image_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ imageCore(a, no, o, c, false); }
void imagesc_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ imageCore(a, no, o, c, true); }

// ── animatedline / addpoints / getpoints / clearpoints ──────────────────

void animatedline_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                            keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, false);

    const NodeId alId = scene.createChartNode(ClassId::AnimatedLine, axId, true);
    if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
        const Value &x = args[idx++];
        const Value &y = args[idx++];
        const Value *z = nullptr;
        if (idx < args.size() && args[idx].isNumeric())
            z = &args[idx++];
        scene.animatedLineAddPoints(alId, x, y, z, mr);
    }
    if (idx < args.size())
        applyChartNV(scene, alId, args, idx, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(alId, mr) : Value();
}

void addpoints_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                         keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, 4, "addpoints");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    NodeId alId = kInvalidNodeId;
    if (!extractSceneHandle(args[0], alId))
        throw Error("addpoints: first argument must be an AnimatedLine handle.", 0, 0, "addpoints", "", "numkit:graphics:InvalidHandle");
    const Value *z = (args.size() >= 4) ? &args[3] : nullptr;
    scene.animatedLineAddPoints(alId, args[1], args[2], z, mr);
    outs[0] = Value();
}

void getpoints_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "getpoints");
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    NodeId alId = kInvalidNodeId;
    if (!extractSceneHandle(args[0], alId))
        throw Error("getpoints: argument must be an AnimatedLine handle.", 0, 0, "getpoints", "", "numkit:graphics:InvalidHandle");
    Value x, y, z;
    scene.animatedLineGetPoints(alId, x, y, z, nargout >= 3, mr);
    outs[0] = keel::move(x);
    if (nargout >= 2) outs[1] = keel::move(y);
    if (nargout >= 3) outs[2] = keel::move(z);
}

void clearpoints_scene_reg(keel::ArrayRef<Value> args, keel::usize,
                           keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "clearpoints");
    auto &scene = coreEngine(ctx).graphicsScene();
    NodeId alId = kInvalidNodeId;
    if (!extractSceneHandle(args[0], alId))
        throw Error("clearpoints: argument must be an AnimatedLine handle.", 0, 0, "clearpoints", "", "numkit:graphics:InvalidHandle");
    scene.animatedLineClearPoints(alId);
    outs[0] = Value();
}

// ── annotation (TextBox) ────────────────────────────────────────────────

void annotation_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
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
    if (figId == kInvalidNodeId)
        figId = scene.ensureCurrentFigure();

    // Skip shape type token ('textbox', 'rectangle', 'ellipse', 'line', 'arrow', ...)
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString()))
        ++idx;

    const NodeId tid = scene.createChartNode(ClassId::TextBox, figId, false);
    if (idx < args.size() && args[idx].isNumeric()) {
        if (args[idx].numel() == 4)
            scene.setProperty(tid, "Position", args[idx++], mr);
        else if (idx + 1 < args.size() && args[idx + 1].isNumeric()
                 && args[idx].numel() == 2 && args[idx + 1].numel() == 2) {
            Value p = Value::matrix(1, 4, ValueType::DOUBLE, mr);
            p.doubleDataMut()[0] = args[idx].elemAsDouble(0);
            p.doubleDataMut()[1] = args[idx + 1].elemAsDouble(0);
            p.doubleDataMut()[2] = args[idx].elemAsDouble(1) - args[idx].elemAsDouble(0);
            p.doubleDataMut()[3] = args[idx + 1].elemAsDouble(1) - args[idx + 1].elemAsDouble(0);
            scene.setProperty(tid, "Position", keel::move(p), mr);
            idx += 2;
        }
    }
    if (idx < args.size())
        applyChartNV(scene, tid, args, idx, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(tid, mr) : Value();
}

// ── polarplot / polarscatter / fplot ────────────────────────────────────

void polarplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();

    keel::usize idx = 0;
    NodeId paxId = kInvalidNodeId;
    if (!args.empty()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[0], h)) {
            if (const GraphicsNode *n = scene.findNode(h); n && n->classId == ClassId::PolarAxes) {
                paxId = h;
                idx = 1;
            }
        }
    }
    if (paxId == kInvalidNodeId) {
        const NodeId cur = scene.currentAxesId();
        if (const GraphicsNode *cn = scene.findNode(cur); cn && cn->classId == ClassId::PolarAxes) {
            paxId = cur;
        } else if (cn && cn->classId == ClassId::Axes && cn->children.empty()) {
            // Replace an empty Cartesian Axes (e.g. from subplot(1,2,2)) with PolarAxes in place.
            const NodeId parId = cn->parentId;
            const Value savedPos = scene.getProperty(cur, "Position", mr);
            const keel::i32 spR = cn->subplotRows;
            const keel::i32 spC = cn->subplotCols;
            const keel::i32 spI = cn->subplotIdx;
            scene.deleteNode(cur);
            paxId = scene.createPolarAxes(parId);
            if (GraphicsNode *pn = scene.findNode(paxId)) {
                pn->subplotRows = spR;
                pn->subplotCols = spC;
                pn->subplotIdx = spI;
                if (savedPos.numel() == 4)
                    pn->props.put(keel::String::from("Position", mr), savedPos);
            }
        } else {
            paxId = scene.createPolarAxes();
        }
    }
    scene.prepareAxesForPlot(paxId, true);

    Value th, rh;
    if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
        th = flattenToRowVec(args[idx++], mr);
        rh = flattenToRowVec(args[idx++], mr);
    } else if (idx < args.size() && args[idx].isNumeric()) {
        rh = flattenToRowVec(args[idx++], mr);
        th = makeRange1N(rh.numel(), mr);
    }

    const NodeId lid = scene.createChartNode(ClassId::Line, paxId, true);
    if (GraphicsNode *ln = scene.findNode(lid)) {
        if (!th.isEmpty()) ln->props.put(keel::String::from("XData", mr), th);
        if (!rh.isEmpty()) ln->props.put(keel::String::from("YData", mr), rh);
    }
    const ClassSchema &lSchema = schemaFor(ClassId::Line);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (!(((args.size() - idx) % 2 == 0) && lSchema.findExact(tok) != nullptr)) {
            ParsedLineSpec ls = parseLineSpec(tok, mr);
            if (ls.valid) { applyLineSpecToNode(scene, lid, ls, mr); ++idx; }
        }
    }
    if (idx < args.size())
        applyChartNV(scene, lid, args, idx, mr);
    scene.recomputeAxesAutoLimits(paxId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

void polarscatter_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                            keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    NodeId paxId = kInvalidNodeId;
    if (!args.empty()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[0], h)) {
            if (const GraphicsNode *n = scene.findNode(h); n && n->classId == ClassId::PolarAxes) {
                paxId = h;
                idx = 1;
            }
        }
    }
    if (paxId == kInvalidNodeId) {
        const NodeId cur = scene.currentAxesId();
        if (const GraphicsNode *cn = scene.findNode(cur); cn && cn->classId == ClassId::PolarAxes) {
            paxId = cur;
        } else if (cn && cn->classId == ClassId::Axes && cn->children.empty()) {
            // Replace an empty Cartesian Axes (e.g. from subplot(1,2,2)) with PolarAxes in place.
            const NodeId parId = cn->parentId;
            const Value savedPos = scene.getProperty(cur, "Position", mr);
            const keel::i32 spR = cn->subplotRows;
            const keel::i32 spC = cn->subplotCols;
            const keel::i32 spI = cn->subplotIdx;
            scene.deleteNode(cur);
            paxId = scene.createPolarAxes(parId);
            if (GraphicsNode *pn = scene.findNode(paxId)) {
                pn->subplotRows = spR;
                pn->subplotCols = spC;
                pn->subplotIdx = spI;
                if (savedPos.numel() == 4)
                    pn->props.put(keel::String::from("Position", mr), savedPos);
            }
        } else {
            paxId = scene.createPolarAxes();
        }
    }
    scene.prepareAxesForPlot(paxId, true);

    const NodeId sid = scene.createChartNode(ClassId::Scatter, paxId, true);
    if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
        if (GraphicsNode *sn = scene.findNode(sid)) {
            sn->props.put(keel::String::from("XData", mr), flattenToRowVec(args[idx++], mr));
            sn->props.put(keel::String::from("YData", mr), flattenToRowVec(args[idx++], mr));
        }
    }
    if (idx < args.size())
        applyChartNV(scene, sid, args, idx, mr);
    scene.recomputeAxesAutoLimits(paxId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(sid, mr) : Value();
}

void fplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);

    if (idx >= args.size() || !args[idx].isFuncHandle())
        throw Error("fplot: first argument must be a function handle.", 0, 0, "fplot", "", "numkit:graphics:InvalidArgument");
    const Value &fn = args[idx++];

    keel::f64 lo = -5.0, hi = 5.0;
    if (idx < args.size() && args[idx].isNumeric() && args[idx].numel() >= 2) {
        lo = args[idx].elemAsDouble(0);
        hi = args[idx].elemAsDouble(1);
        ++idx;
    }

    constexpr keel::usize kN = 101;
    Value xv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    Value yv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < kN; ++i) {
        const keel::f64 x = lo + (hi - lo) * keel::as<keel::f64>(i) / keel::as<keel::f64>(kN - 1);
        xv.doubleDataMut()[i] = x;
        const Value argArr[1] = {Value::scalar(x, mr)};
        const Value yRes = ctx.engine->callFunctionHandle(fn, keel::ArrayRef<Value>(argArr, 1));
        yv.doubleDataMut()[i] = yRes.isNumeric() && !yRes.isEmpty() ? yRes.elemAsDouble(0) : keel::kNaN;
    }

    const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
    if (GraphicsNode *ln = scene.findNode(lid)) {
        ln->props.put(keel::String::from("XData", mr), keel::move(xv));
        ln->props.put(keel::String::from("YData", mr), keel::move(yv));
    }
    const ClassSchema &lSchema = schemaFor(ClassId::Line);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        if (!(((args.size() - idx) % 2 == 0) && lSchema.findExact(tok) != nullptr)) {
            ParsedLineSpec ls = parseLineSpec(tok, mr);
            if (ls.valid) { applyLineSpecToNode(scene, lid, ls, mr); ++idx; }
        }
    }
    if (idx < args.size())
        applyChartNV(scene, lid, args, idx, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

} // anonymous namespace

void registerSceneChartsBuiltins(Engine &engine)
{
    auto regBoth = [&](const char *sub, const char *name,
                       void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &)) {
        engine.registerFunction("", name, ExternalFunc(fn));
        keel::SmallString<64> ns("graphics.");
        ns.append(sub);
        engine.registerFunction(ns, name, ExternalFunc(fn));
    };

    regBoth("line",    "plot",         &plot_scene_reg);
    regBoth("line",    "plot3",        &plot3_scene_reg);
    regBoth("line",    "line",         &line_scene_reg);
    regBoth("line",    "semilogx",     &semilogx_scene_reg);
    regBoth("line",    "semilogy",     &semilogy_scene_reg);
    regBoth("line",    "loglog",       &loglog_scene_reg);
    regBoth("line",    "stem",         &stem_scene_reg);
    regBoth("line",    "stem3",        &stem3_scene_reg);
    regBoth("line",    "stairs",       &stairs_scene_reg);
    regBoth("line",    "area",         &area_scene_reg);
    regBoth("line",    "errorbar",     &errorbar_scene_reg);
    regBoth("line",    "fplot",        &fplot_scene_reg);
    regBoth("line",    "animatedline", &animatedline_scene_reg);
    regBoth("line",    "addpoints",    &addpoints_scene_reg);
    regBoth("line",    "getpoints",    &getpoints_scene_reg);
    regBoth("line",    "clearpoints",  &clearpoints_scene_reg);

    regBoth("bar",     "scatter",      &scatter_scene_reg);
    regBoth("bar",     "scatter3",     &scatter3_scene_reg);
    regBoth("bar",     "bar",          &bar_scene_reg);
    regBoth("bar",     "barh",         &barh_scene_reg);
    regBoth("bar",     "histogram",    &histogram_scene_reg);
    regBoth("bar",     "patch",        &patch_scene_reg);
    regBoth("bar",     "fill",         &fill_scene_reg);
    regBoth("bar",     "fill3",        &fill3_scene_reg);

    regBoth("surface", "surf",         &surf_scene_reg);
    regBoth("surface", "mesh",         &mesh_scene_reg);
    regBoth("surface", "surfc",        &surf_scene_reg);
    regBoth("surface", "meshc",        &mesh_scene_reg);
    regBoth("surface", "pcolor",       &pcolor_scene_reg);
    regBoth("surface", "surface",      &surface_scene_reg);
    regBoth("surface", "quiver",       &quiver_scene_reg);
    regBoth("surface", "quiver3",      &quiver3_scene_reg);

    regBoth("contour", "contour",      &contour_scene_reg);
    regBoth("contour", "contourf",     &contourf_scene_reg);
    regBoth("contour", "contour3",     &contour3_scene_reg);

    regBoth("image",   "image",        &image_scene_reg);
    regBoth("image",   "imagesc",      &imagesc_scene_reg);

    regBoth("polar",   "polarplot",    &polarplot_scene_reg);
    regBoth("polar",   "polarscatter", &polarscatter_scene_reg);

    regBoth("layout",  "annotation",   &annotation_scene_reg);
}

} // namespace numkit::gfx::detail
