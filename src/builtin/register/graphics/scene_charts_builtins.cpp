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
#include <keel/hash_map.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/builtin/datafun.hpp>
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
    // Trim whitespace for full color name check (e.g. 'green', 'blue', 'black').
    keel::usize lo = 0;
    keel::usize hi = s.size();
    while (lo < hi && (s[lo] == ' ' || s[lo] == '\t')) ++lo;
    while (hi > lo && (s[hi - 1] == ' ' || s[hi - 1] == '\t')) --hi;
    const keel::StringRef trimmed = s.substr(lo, hi - lo);
    if (trimmed.size() > 1 && isColorCharOrName(trimmed)) {
        out.color = keel::String::from(trimmed, mr);
        out.valid = true;
        return out;
    }
    keel::usize i = lo;
    while (i < hi) {
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
    const NodeId preAx = (leadingAx != kInvalidNodeId) ? leadingAx : scene.currentAxesId();
    const bool wasHoldOn = (preAx != kInvalidNodeId) && scene.isHoldOn(preAx);
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
            const bool isPropName = ((args.size() - idx) % 2 == 0) && (lineSchema.findExactNoCase(tok) != nullptr);
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
            } else if (xArg.numel() > 1 && yArg.numel() == 1) {
                // plot(vec, scalar) -> N 1-point lines (matches reference behavior).
                const Value ys = Value::scalar(yArg.elemAsDouble(0), mr);
                for (keel::usize k = 0; k < xArg.numel(); ++k)
                    addOneLine(Value::scalar(xArg.elemAsDouble(k), mr), ys);
            } else if (yArg.numel() > 1 && xArg.numel() == 1) {
                const Value xs = Value::scalar(xArg.elemAsDouble(0), mr);
                for (keel::usize k = 0; k < yArg.numel(); ++k)
                    addOneLine(xs, Value::scalar(yArg.elemAsDouble(k), mr));
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

    if (!wasHoldOn) {
        if (xScale) scene.setProperty(axId, "XScale", Value::fromString(xScale, mr), mr);
        if (yScale) scene.setProperty(axId, "YScale", Value::fromString(yScale, mr), mr);
    }
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
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, true);

    const ClassSchema &lineSchema = schemaFor(ClassId::Line);
    keel::Array<NodeId> created(mr);
    keel::usize nvStart = args.size();
    while (idx + 2 < args.size() && isPlottableCoord(args[idx]) && isPlottableCoord(args[idx + 1]) && isPlottableCoord(args[idx + 2])) {
        const Value &xa = args[idx];
        const Value &ya = args[idx + 1];
        const Value &za = args[idx + 2];
        idx += 3;
        ParsedLineSpec ls;
        if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
            const keel::String tok = args[idx].toString();
            if (!(((args.size() - idx) % 2 == 0) && lineSchema.findExactNoCase(tok) != nullptr)) {
                ParsedLineSpec cand = parseLineSpec(tok, mr);
                if (cand.valid) { ls = keel::move(cand); ++idx; }
            }
        }
        auto addOne3D = [&](const Value &xv, const Value &yv, const Value &zv) {
            const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
            if (GraphicsNode *ln = scene.findNode(lid)) {
                ln->props.put(keel::String::from("XData", mr), xv);
                ln->props.put(keel::String::from("YData", mr), yv);
                ln->props.put(keel::String::from("ZData", mr), zv);
            }
            applyLineSpecToNode(scene, lid, ls, mr);
            created.push_back(lid);
        };
        const bool xMat = (xa.dims().rows() > 1 && xa.dims().cols() > 1);
        const bool yMat = (ya.dims().rows() > 1 && ya.dims().cols() > 1);
        const bool zMat = (za.dims().rows() > 1 && za.dims().cols() > 1);
        if (xMat || yMat || zMat) {
            const keel::usize nCols = xMat ? xa.dims().cols() : (yMat ? ya.dims().cols() : za.dims().cols());
            const Value xFlat = xMat ? Value() : flattenToRowVec(xa, mr);
            const Value yFlat = yMat ? Value() : flattenToRowVec(ya, mr);
            const Value zFlat = zMat ? Value() : flattenToRowVec(za, mr);
            for (keel::usize c = 0; c < nCols; ++c) {
                const Value xv = xMat ? extractColAsRowVec(xa, c < xa.dims().cols() ? c : xa.dims().cols() - 1, mr) : xFlat;
                const Value yv = yMat ? extractColAsRowVec(ya, c < ya.dims().cols() ? c : ya.dims().cols() - 1, mr) : yFlat;
                const Value zv = zMat ? extractColAsRowVec(za, c < za.dims().cols() ? c : za.dims().cols() - 1, mr) : zFlat;
                addOne3D(xv, yv, zv);
            }
        } else {
            addOne3D(flattenToRowVec(xa, mr), flattenToRowVec(ya, mr), flattenToRowVec(za, mr));
        }
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
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, is3D);

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
            } else if (!(((args.size() - idx) % 2 == 0) && scSchema.findExactNoCase(tok) != nullptr)) {
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
        if (((args.size() - idx) % 2 == 0) && scSchema.findExactNoCase(tok) != nullptr)
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
        if (((args.size() - idx) % 2 == 0) && barSchema.findExactNoCase(tok) != nullptr)
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
                 bool isMesh, bool isPcolor, bool clearOnReplace, bool withContour = false)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const bool is3DSurf = !isPcolor;
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, clearOnReplace, is3DSurf);

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
    if (isPcolor && !zd.isEmpty()) {
        cd = zd;
        zd = Value::matrix(zd.dims().rows(), zd.dims().cols(), ValueType::DOUBLE, mr);
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

    NodeId cid = kInvalidNodeId;
    if (withContour && !zd.isEmpty()) {
        cid = scene.createChartNode(ClassId::Contour, axId, false);
        if (GraphicsNode *cn = scene.findNode(cid)) {
            if (!xd.isEmpty()) cn->props.put(keel::String::from("XData", mr), xd);
            if (!yd.isEmpty()) cn->props.put(keel::String::from("YData", mr), yd);
            cn->props.put(keel::String::from("ZData", mr), zd);
            cn->props.put(keel::String::from("ZLocation", mr), Value::fromString("zmin", mr));
            // Compute auto nice LevelList & LevelStep matching contourCore.
            keel::f64 zmin = keel::Limits<keel::f64>::infinity();
            keel::f64 zmax = -keel::Limits<keel::f64>::infinity();
            for (keel::usize i = 0; i < zd.numel(); ++i) {
                const keel::f64 v = zd.elemAsDouble(i);
                if (keel::is_finite(v)) { zmin = keel::min(zmin, v); zmax = keel::max(zmax, v); }
            }
            if (keel::is_finite(zmin) && keel::is_finite(zmax) && zmax > zmin) {
                const keel::f64 raw = (zmax - zmin) / 12.0;
                const keel::f64 p10 = keel::pow(10.0, keel::floor(keel::log10(raw)));
                const keel::f64 frac = raw / p10;
                const keel::f64 mult = (frac <= 1.0 + 1e-12) ? 1.0
                                     : (frac <= 2.0 + 1e-12) ? 2.0
                                     : (frac <= 5.0 + 1e-12) ? 5.0
                                                             : 10.0;
                const keel::f64 step = mult * p10;
                keel::f64 first = keel::ceil(zmin / step - 1e-12) * step;
                if (zmin == 0.0 && keel::abs(first) <= 1e-12 * step)
                    first = step;
                const keel::f64 last = keel::floor(zmax / step + 1e-12) * step;
                keel::Array<keel::f64> lv(mr);
                for (keel::i32 k = 0; k < 256; ++k) {
                    const keel::f64 val = first + keel::as<keel::f64>(k) * step;
                    if (val > last + 1e-9 * step) break;
                    lv.push_back(keel::abs(val) < 1e-12 * step ? 0.0 : val);
                }
                if (!lv.empty()) {
                    Value ll = Value::matrix(1, lv.size(), ValueType::DOUBLE, mr);
                    for (keel::usize k = 0; k < lv.size(); ++k) ll.doubleDataMut()[k] = lv[k];
                    cn->props.put(keel::String::from("LevelList", mr), keel::move(ll));
                    cn->props.put(keel::String::from("LevelStep", mr), Value::scalar(step, mr));
                }
            }
        }
    }

    if (!isPcolor && clearOnReplace) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
        if (!scene.isHoldOn(axId)) {
            scene.setProperty(axId, "XGrid", Value::fromString("on", mr), mr);
            scene.setProperty(axId, "YGrid", Value::fromString("on", mr), mr);
            scene.setProperty(axId, "ZGrid", Value::fromString("on", mr), mr);
        }
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    if (nargout >= 1) {
        if (cid != kInvalidNodeId) {
            const NodeId pair[2] = {sid, cid};
            outs[0] = scene.makeHandleColumn(keel::ArrayRef<NodeId>(pair, 2), mr);
        } else {
            outs[0] = scene.makeHandleValue(sid, mr);
        }
    } else {
        outs[0] = Value();
    }
}

void surf_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ surfaceCore(a, no, o, c, false, false, true, false); }
void mesh_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ surfaceCore(a, no, o, c, true, false, true, false); }
void surfc_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ surfaceCore(a, no, o, c, false, false, true, true); }
void meshc_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ surfaceCore(a, no, o, c, true, false, true, true); }
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
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, clearOnReplace, is3D);

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
                    && schemaFor(ClassId::Patch).findExactNoCase(args[idx + 1].toString()) != nullptr);
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
                if (!(((args.size() - idx) % 2 == 0) && pSchema.findExactNoCase(tok) != nullptr)) {
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
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, is3D);

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
        if (((args.size() - idx) % 2 == 0) && qSchema.findExactNoCase(tok) != nullptr)
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

// Build standard 2-by-M contour matrix [level, x1..xk; count, y1..yk] via marching squares + edge chaining.
Value computeContourMatrix(const Value &xd, const Value &yd, const Value &zd,
                           const Value &levels, keel::Allocator *mr)
{
    if (zd.isEmpty() || levels.isEmpty())
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    const keel::usize R = zd.dims().rows();
    const keel::usize C = zd.dims().cols();
    if (R < 2 || C < 2)
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);

    const bool xIsGrid = (xd.dims().rows() == R && xd.dims().cols() == C);
    const bool yIsGrid = (yd.dims().rows() == R && yd.dims().cols() == C);
    auto xAt = [&](keel::usize r, keel::usize c) -> keel::f64 {
        if (xIsGrid) return xd.elemAsDouble(r + c * R);
        if (xd.numel() == C) return xd.elemAsDouble(c);
        return keel::as<keel::f64>(c + 1);
    };
    auto yAt = [&](keel::usize r, keel::usize c) -> keel::f64 {
        if (yIsGrid) return yd.elemAsDouble(r + c * R);
        if (yd.numel() == R) return yd.elemAsDouble(r);
        return keel::as<keel::f64>(r + 1);
    };
    auto zAt = [&](keel::usize r, keel::usize c) -> keel::f64 {
        return zd.elemAsDouble(r + c * R);
    };

    struct Pt { keel::f64 x; keel::f64 y; };
    struct Seg { keel::u64 k0; keel::u64 k1; Pt p0; Pt p1; bool used; };

    // Edge key uniquely identifies a grid edge shared by at most 2 cells:
    // dir 0 = horizontal edge between (r, c) and (r, c+1);
    // dir 1 = vertical edge between (r, c) and (r+1, c).
    auto makeEdgeKey = [](keel::usize r, keel::usize c, keel::u64 dir) -> keel::u64 {
        return ((keel::as<keel::u64>(r) << 33) | (keel::as<keel::u64>(c) << 1) | dir);
    };
    auto interpT = [](keel::f64 va, keel::f64 vb, keel::f64 L) -> keel::f64 {
        const keel::f64 d = vb - va;
        if (keel::abs(d) < 1e-30) return 0.5;
        return keel::clamp((L - va) / d, 0.0, 1.0);
    };

    keel::Array<keel::f64> row0(mr);
    keel::Array<keel::f64> row1(mr);

    for (keel::usize li = 0; li < levels.numel(); ++li) {
        const keel::f64 L = levels.elemAsDouble(li);
        if (!keel::is_finite(L))
            continue;

        keel::Array<Seg> segs(mr);
        keel::HashMap<keel::u64, keel::i64> outEdge(mr);
        keel::HashMap<keel::u64, keel::i64> inEdge(mr);

        for (keel::usize r = 0; r + 1 < R; ++r) {
            for (keel::usize c = 0; c + 1 < C; ++c) {
                const keel::f64 vTL = zAt(r, c);
                const keel::f64 vTR = zAt(r, c + 1);
                const keel::f64 vBR = zAt(r + 1, c + 1);
                const keel::f64 vBL = zAt(r + 1, c);
                if (!keel::is_finite(vTL) || !keel::is_finite(vTR)
                    || !keel::is_finite(vBR) || !keel::is_finite(vBL))
                    continue;
                keel::i32 code = 0;
                if (vTL >= L) code |= 1;
                if (vTR >= L) code |= 2;
                if (vBR >= L) code |= 4;
                if (vBL >= L) code |= 8;
                if (code == 0 || code == 15)
                    continue;

                auto edgePt = [&](keel::i32 e, keel::u64 &keyOut) -> Pt {
                    // 0=Top(TL-TR), 1=Right(TR-BR), 2=Bottom(BL-BR), 3=Left(TL-BL)
                    if (e == 0) {
                        keyOut = makeEdgeKey(r, c, 0);
                        const keel::f64 t = interpT(vTL, vTR, L);
                        return {xAt(r, c) + t * (xAt(r, c + 1) - xAt(r, c)),
                                yAt(r, c) + t * (yAt(r, c + 1) - yAt(r, c))};
                    }
                    if (e == 1) {
                        keyOut = makeEdgeKey(r, c + 1, 1);
                        const keel::f64 t = interpT(vTR, vBR, L);
                        return {xAt(r, c + 1) + t * (xAt(r + 1, c + 1) - xAt(r, c + 1)),
                                yAt(r, c + 1) + t * (yAt(r + 1, c + 1) - yAt(r, c + 1))};
                    }
                    if (e == 2) {
                        keyOut = makeEdgeKey(r + 1, c, 0);
                        const keel::f64 t = interpT(vBL, vBR, L);
                        return {xAt(r + 1, c) + t * (xAt(r + 1, c + 1) - xAt(r + 1, c)),
                                yAt(r + 1, c) + t * (yAt(r + 1, c + 1) - yAt(r + 1, c))};
                    }
                    keyOut = makeEdgeKey(r, c, 1);
                    const keel::f64 t = interpT(vTL, vBL, L);
                    return {xAt(r, c) + t * (xAt(r + 1, c) - xAt(r, c)),
                            yAt(r, c) + t * (yAt(r + 1, c) - yAt(r, c))};
                };

                // Directed segments with higher values (>= L) on the LEFT (CCW around highs).
                keel::i32 ePairs[4] = {-1, -1, -1, -1};
                keel::i32 nPairs = 0;
                switch (code) {
                case 1:  ePairs[0] = 0; ePairs[1] = 3; nPairs = 1; break;
                case 14: ePairs[0] = 3; ePairs[1] = 0; nPairs = 1; break;
                case 2:  ePairs[0] = 1; ePairs[1] = 0; nPairs = 1; break;
                case 13: ePairs[0] = 0; ePairs[1] = 1; nPairs = 1; break;
                case 3:  ePairs[0] = 1; ePairs[1] = 3; nPairs = 1; break;
                case 12: ePairs[0] = 3; ePairs[1] = 1; nPairs = 1; break;
                case 4:  ePairs[0] = 2; ePairs[1] = 1; nPairs = 1; break;
                case 11: ePairs[0] = 1; ePairs[1] = 2; nPairs = 1; break;
                case 6:  ePairs[0] = 2; ePairs[1] = 0; nPairs = 1; break;
                case 9:  ePairs[0] = 0; ePairs[1] = 2; nPairs = 1; break;
                case 7:  ePairs[0] = 2; ePairs[1] = 3; nPairs = 1; break;
                case 8:  ePairs[0] = 3; ePairs[1] = 2; nPairs = 1; break;
                case 5: {
                    const keel::f64 ctr = 0.25 * (vTL + vTR + vBR + vBL);
                    if (ctr >= L) { ePairs[0] = 2; ePairs[1] = 3; ePairs[2] = 0; ePairs[3] = 1; }
                    else          { ePairs[0] = 0; ePairs[1] = 3; ePairs[2] = 2; ePairs[3] = 1; }
                    nPairs = 2;
                    break;
                }
                case 10: {
                    const keel::f64 ctr = 0.25 * (vTL + vTR + vBR + vBL);
                    if (ctr >= L) { ePairs[0] = 3; ePairs[1] = 0; ePairs[2] = 1; ePairs[3] = 2; }
                    else          { ePairs[0] = 3; ePairs[1] = 2; ePairs[2] = 1; ePairs[3] = 0; }
                    nPairs = 2;
                    break;
                }
                default: break;
                }
                for (keel::i32 s = 0; s < nPairs; ++s) {
                    keel::u64 k0 = 0, k1 = 0;
                    const Pt p0 = edgePt(ePairs[2 * s], k0);
                    const Pt p1 = edgePt(ePairs[2 * s + 1], k1);
                    const keel::i64 idxS = keel::as<keel::i64>(segs.size());
                    segs.push_back(Seg{k0, k1, p0, p1, false});
                    outEdge.put(k0, idxS);
                    inEdge.put(k1, idxS);
                }
            }
        }

        // Pass 1: open polylines starting at entry boundary edges (no incoming segment at k0);
        // Pass 2: closed loops.
        for (keel::i32 pass = 0; pass < 2; ++pass) {
            for (keel::usize si = 0; si < segs.size(); ++si) {
                if (segs[si].used)
                    continue;
                if (pass == 0 && inEdge.contains(segs[si].k0))
                    continue;

                keel::Array<Pt> poly(mr);
                segs[si].used = true;
                const keel::u64 startKey = segs[si].k0;
                poly.push_back(segs[si].p0);
                poly.push_back(segs[si].p1);
                keel::u64 curKey = segs[si].k1;
                for (;;) {
                    const keel::i64 *nxt = outEdge.get(curKey);
                    if (!nxt || segs[keel::as<keel::usize>(*nxt)].used)
                        break;
                    Seg &ns = segs[keel::as<keel::usize>(*nxt)];
                    ns.used = true;
                    poly.push_back(ns.p1);
                    curKey = ns.k1;
                    if (curKey == startKey)
                        break;
                }
                if (poly.size() >= 2) {
                    row0.push_back(L);
                    row1.push_back(keel::as<keel::f64>(poly.size()));
                    for (const Pt &pt : poly) {
                        row0.push_back(pt.x);
                        row1.push_back(pt.y);
                    }
                }
            }
        }
    }

    if (row0.empty())
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    const keel::usize cols = row0.size();
    Value out = Value::matrix(2, cols, ValueType::DOUBLE, mr);
    keel::f64 *dst = out.doubleDataMut();
    for (keel::usize c = 0; c < cols; ++c) {
        dst[2 * c]     = row0[c];
        dst[2 * c + 1] = row1[c];
    }
    return out;
}

void contourCore(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx, bool filled, bool is3D)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, is3D);

    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == 4) break;
    }

    Value xd, yd, zd;
    bool hasExplicitN = false;
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
        if (nums[1]->numel() == 1) {
            hasExplicitN = true;
            nLevels = keel::max<keel::usize>(1, keel::as<keel::usize>(nums[1]->toScalar()));
        } else {
            explicitLevels = flattenToRowVec(*nums[1], mr);
        }
    } else if (nums.size() >= 3) {
        xd = ensure2DDouble(*nums[0], mr);
        yd = ensure2DDouble(*nums[1], mr);
        zd = ensure2DDouble(*nums[2], mr);
        if (nums.size() >= 4) {
            if (nums[3]->numel() == 1) {
                hasExplicitN = true;
                nLevels = keel::max<keel::usize>(1, keel::as<keel::usize>(nums[3]->toScalar()));
            } else {
                explicitLevels = flattenToRowVec(*nums[3], mr);
            }
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

    // Compute LevelList & LevelStep across Z range.
    keel::f64 autoLevelStep = 0.0;
    if (explicitLevels.isEmpty() && !zd.isEmpty()) {
        keel::f64 zmin = keel::Limits<keel::f64>::infinity();
        keel::f64 zmax = -keel::Limits<keel::f64>::infinity();
        for (keel::usize i = 0; i < zd.numel(); ++i) {
            const keel::f64 v = zd.elemAsDouble(i);
            if (keel::is_finite(v)) { zmin = keel::min(zmin, v); zmax = keel::max(zmax, v); }
        }
        if (keel::is_finite(zmin) && keel::is_finite(zmax) && zmax > zmin) {
            const keel::f64 span = zmax - zmin;
            if (hasExplicitN) {
                const keel::f64 dz = span / keel::as<keel::f64>(nLevels + 1);
                autoLevelStep = dz;
                const keel::usize count = filled ? (nLevels + 1) : nLevels;
                explicitLevels = Value::matrix(1, count, ValueType::DOUBLE, mr);
                for (keel::usize k = 0; k < count; ++k) {
                    const keel::usize m = filled ? k : (k + 1);
                    explicitLevels.doubleDataMut()[k] = zmin + dz * keel::as<keel::f64>(m);
                }
            } else {
                // Auto nice step: ceil_to_1_2_5(span / 12)
                const keel::f64 raw = span / 12.0;
                const keel::f64 p10 = keel::pow(10.0, keel::floor(keel::log10(raw)));
                const keel::f64 frac = raw / p10;
                const keel::f64 mult = (frac <= 1.0 + 1e-12) ? 1.0
                                     : (frac <= 2.0 + 1e-12) ? 2.0
                                     : (frac <= 5.0 + 1e-12) ? 5.0
                                                             : 10.0;
                const keel::f64 step = mult * p10;
                autoLevelStep = step;
                keel::f64 first = keel::ceil(zmin / step - 1e-12) * step;
                if (zmin == 0.0 && keel::abs(first) <= 1e-12 * step)
                    first = step;
                const keel::f64 last = keel::floor(zmax / step + 1e-12) * step;
                keel::Array<keel::f64> lv(mr);
                for (keel::i32 k = 0; k < 256; ++k) {
                    const keel::f64 val = first + keel::as<keel::f64>(k) * step;
                    if (val > last + 1e-9 * step)
                        break;
                    lv.push_back(keel::abs(val) < 1e-12 * step ? 0.0 : val);
                }
                if (!lv.empty()) {
                    explicitLevels = Value::matrix(1, lv.size(), ValueType::DOUBLE, mr);
                    for (keel::usize k = 0; k < lv.size(); ++k)
                        explicitLevels.doubleDataMut()[k] = lv[k];
                }
            }
        }
    }

    const NodeId cid = scene.createChartNode(ClassId::Contour, axId, false);
    if (GraphicsNode *cn = scene.findNode(cid)) {
        if (!xd.isEmpty()) cn->props.put(keel::String::from("XData", mr), xd);
        if (!yd.isEmpty()) cn->props.put(keel::String::from("YData", mr), yd);
        if (!zd.isEmpty()) cn->props.put(keel::String::from("ZData", mr), zd);
        if (!explicitLevels.isEmpty()) cn->props.put(keel::String::from("LevelList", mr), explicitLevels);
        if (autoLevelStep > 0.0) cn->props.put(keel::String::from("LevelStep", mr), Value::scalar(autoLevelStep, mr));
        if (filled)
            cn->props.put(keel::String::from("FaceColor", mr), Value::fromString("flat", mr));
        if (nargout >= 2 && !zd.isEmpty() && !explicitLevels.isEmpty()) {
            cn->props.put(keel::String::from("ContourMatrix", mr),
                          computeContourMatrix(xd, yd, zd, explicitLevels, mr));
        }
    }
    // Optional LineSpec token after Z/levels (e.g. contour3(X, Y, Z, 10, 'k', 'LineWidth', 1)).
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx].toString();
        const ClassSchema &cSchema = schemaFor(ClassId::Contour);
        const bool isPropName = ((args.size() - idx) % 2 == 0)
                                && (cSchema.findExactNoCase(tok) != nullptr || asciiEqualNoCase(tok, "LineColor"));
        if (!isPropName) {
            const ParsedLineSpec ls = parseLineSpec(tok, mr);
            if (ls.valid) {
                if (!ls.color.empty())
                    scene.setProperty(cid, "EdgeColor", Value::fromString(ls.color, mr), mr);
                if (!ls.lineStyle.empty())
                    scene.setProperty(cid, "LineStyle", Value::fromString(ls.lineStyle, mr), mr);
                ++idx;
            }
        }
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
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, is3D);

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
        if (((args.size() - idx) % 2 == 0) && sSchema.findExactNoCase(tok) != nullptr) break;
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
        if (!(((args.size() - idx) % 2 == 0) && stSchema.findExactNoCase(tok) != nullptr)) {
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
        if (!(((args.size() - idx) % 2 == 0) && eSchema.findExactNoCase(tok) != nullptr)) {
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
    while (idx < args.size() && (args[idx].isNumeric() || args[idx].isLogical())) {
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

    // Trailing [cmin cmax] CLim vector after NV pairs (e.g. imagesc(A, 'EraseMode', 'none', [-1 1])).
    keel::usize nvEnd = args.size();
    if (scaled && climArg.isEmpty() && nvEnd > idx && ((nvEnd - idx) % 2 == 1)
        && args[nvEnd - 1].isNumeric() && args[nvEnd - 1].numel() == 2) {
        climArg = args[nvEnd - 1];
        --nvEnd;
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
    if (idx < nvEnd)
        applyChartNV(scene, iid, args.slice(0, nvEnd), idx, mr);
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
        if (!(((args.size() - idx) % 2 == 0) && lSchema.findExactNoCase(tok) != nullptr)) {
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

    if (idx >= args.size())
        throw Error("fplot: first argument must be a function handle or symbolic expression.",
                    0, 0, "fplot", "", "numkit:graphics:InvalidArgument");
    Value fn;
    bool zeroArgFn = false;
    if (args[idx].isFuncHandle()) {
        fn = args[idx++];
    } else if (args[idx].isObject()
               && (args[idx].objectClassNameRef() == "sym" || args[idx].objectClassNameRef() == "symfun")) {
        const ExternalFunc *mf = ctx.engine->findExternal("matlabFunction", ctx.env);
        if (!mf) mf = ctx.engine->findExternal("cas.matlabFunction", ctx.env);
        if (!mf) {
            throw Error("fplot: symbolic conversion requires matlabFunction.",
                        0, 0, "fplot", "", "numkit:graphics:InvalidArgument");
        }
        // Check if expression has 0 symbolic variables (constant sym).
        if (const ExternalFunc *svf = ctx.engine->findExternal("symvar", ctx.env)) {
            Value svOut[1];
            (*svf)(keel::ArrayRef<Value>(&args[idx], 1), 1, keel::MutArrayRef<Value>(svOut, 1), ctx);
            if (svOut[0].isEmpty())
                zeroArgFn = true;
        }
        Value mfOut[1];
        (*mf)(keel::ArrayRef<Value>(&args[idx], 1), 1, keel::MutArrayRef<Value>(mfOut, 1), ctx);
        fn = keel::move(mfOut[0]);
        ++idx;
    } else if (args[idx].isChar() || args[idx].isString()) {
        const keel::String expr = args[idx++].toString();
        const ExternalFunc *s2f = ctx.engine->findExternal("str2func", ctx.env);
        if (s2f) {
            keel::String body(mr);
            if (expr.find('@') == keel::kInvalidIndex && expr.find('(') != keel::kInvalidIndex) {
                body.append("@(x) ");
                body.append(expr);
            } else {
                body.append(expr);
            }
            Value sArg = Value::fromString(body, mr);
            Value sOut[1];
            (*s2f)(keel::ArrayRef<Value>(&sArg, 1), 1, keel::MutArrayRef<Value>(sOut, 1), ctx);
            fn = keel::move(sOut[0]);
        }
    }
    if (!fn.isFuncHandle())
        throw Error("fplot: first argument must be a function handle.", 0, 0, "fplot", "", "numkit:graphics:InvalidArgument");

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
        const Value yRes = zeroArgFn
            ? ctx.engine->callFunctionHandle(fn, keel::ArrayRef<Value>())
            : ctx.engine->callFunctionHandle(fn, keel::ArrayRef<Value>(argArr, 1));
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
        if (!(((args.size() - idx) % 2 == 0) && lSchema.findExactNoCase(tok) != nullptr)) {
            ParsedLineSpec ls = parseLineSpec(tok, mr);
            if (ls.valid) { applyLineSpecToNode(scene, lid, ls, mr); ++idx; }
        }
    }
    if (idx < args.size())
        applyChartNV(scene, lid, args, idx, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

// ── pie / pie3 ──────────────────────────────────────────────────────────

void pie_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, false);

    if (idx >= args.size() || !args[idx].isNumeric()) {
        outs[0] = Value();
        return;
    }
    const Value &xVal = args[idx++];
    const keel::usize n = xVal.numel();
    keel::Array<keel::f64> vals(mr);
    vals.reserve(n);
    keel::f64 total = 0.0;
    for (keel::usize i = 0; i < n; ++i) {
        const keel::f64 v = xVal.elemAsDouble(i);
        vals.push_back(v > 0.0 && keel::is_finite(v) ? v : 0.0);
        if (v > 0.0 && keel::is_finite(v))
            total += v;
    }
    const keel::f64 denom = (total > 1.0) ? total : 1.0;

    keel::Array<keel::f64> explode(n, 0.0, mr);
    keel::Array<keel::String> customLabels(mr);
    while (idx < args.size()) {
        if (args[idx].isNumeric() || args[idx].isLogical()) {
            for (keel::usize i = 0; i < n && i < args[idx].numel(); ++i)
                explode[i] = args[idx].elemAsDouble(i) != 0.0 ? 0.1 : 0.0;
        } else if (args[idx].isCell()) {
            for (keel::usize i = 0; i < args[idx].numel(); ++i)
                customLabels.push_back(args[idx].cellAt(i).toString());
        } else if (args[idx].isString()) {
            for (keel::usize i = 0; i < args[idx].numel(); ++i)
                customLabels.push_back(args[idx].elemAt(i).toString());
        }
        ++idx;
    }

    constexpr keel::f64 kPi = 3.14159265358979323846;
    keel::f64 theta0 = 0.5 * kPi;
    keel::Array<NodeId> created(mr);
    keel::usize labelIdx = 0;

    for (keel::usize i = 0; i < n; ++i) {
        if (vals[i] <= 0.0) {
            ++labelIdx;
            continue;
        }
        const keel::f64 frac = vals[i] / denom;
        const keel::f64 dTheta = 2.0 * kPi * frac;
        const keel::f64 theta1 = theta0 + dTheta;
        const keel::f64 mid = 0.5 * (theta0 + theta1);
        const keel::f64 offX = explode[i] * keel::cos(mid);
        const keel::f64 offY = explode[i] * keel::sin(mid);

        // Build sector polygon (center + arc points + closing center), matching R2025b pie vertex count (max(1, ceil(100*frac)) + 3).
        const keel::usize nSteps = keel::max<keel::usize>(1, keel::as<keel::usize>(keel::ceil(frac * 100.0)));
        const keel::usize nPts = nSteps + 3;
        Value xv = Value::matrix(nPts, 1, ValueType::DOUBLE, mr);
        Value yv = Value::matrix(nPts, 1, ValueType::DOUBLE, mr);
        xv.doubleDataMut()[0] = offX;
        yv.doubleDataMut()[0] = offY;
        for (keel::usize k = 0; k <= nSteps; ++k) {
            const keel::f64 ang = theta0 + dTheta * keel::as<keel::f64>(k) / keel::as<keel::f64>(nSteps);
            xv.doubleDataMut()[k + 1] = offX + keel::cos(ang);
            yv.doubleDataMut()[k + 1] = offY + keel::sin(ang);
        }
        xv.doubleDataMut()[nPts - 1] = offX;
        yv.doubleDataMut()[nPts - 1] = offY;

        Value cd = Value::matrix(nPts, 1, ValueType::DOUBLE, mr);
        for (keel::usize k = 0; k < nPts; ++k)
            cd.doubleDataMut()[k] = keel::as<keel::f64>(i + 1);
        const NodeId pid = scene.createChartNode(ClassId::Patch, axId, false);
        if (GraphicsNode *pn = scene.findNode(pid)) {
            pn->props.put(keel::String::from("XData", mr), keel::move(xv));
            pn->props.put(keel::String::from("YData", mr), keel::move(yv));
            pn->props.put(keel::String::from("CData", mr), keel::move(cd));
            pn->props.put(keel::String::from("FaceColor", mr), Value::fromString("flat", mr));
        }
        scene.syncPatchVerticesAndData(pid, "XData", mr);
        created.push_back(pid);

        keel::String lbl = (labelIdx < customLabels.size())
            ? customLabels[labelIdx].clone(mr)
            : keel::format("{}%", keel::as<keel::i32>(keel::round(100.0 * vals[i] / (total > 0.0 ? total : 1.0))));
        const NodeId tid = scene.createText(axId, offX + 1.2 * keel::cos(mid), offY + 1.2 * keel::sin(mid), 0.0, lbl);
        created.push_back(tid);

        theta0 = theta1;
        ++labelIdx;
    }

    Value lim = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    lim.doubleDataMut()[0] = -1.2; lim.doubleDataMut()[1] = 1.2;
    scene.setProperty(axId, "XLim", lim, mr);
    scene.setProperty(axId, "YLim", lim, mr);
    Value zlim = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    zlim.doubleDataMut()[0] = -1.0; zlim.doubleDataMut()[1] = 1.0;
    scene.setProperty(axId, "ZLim", zlim, mr);
    scene.setProperty(axId, "Box", Value::fromString("off", mr), mr);
    scene.setProperty(axId, "Visible", Value::fromString("off", mr), mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

// ── slice ───────────────────────────────────────────────────────────────

void slice_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, true);

    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == 7) break;
    }

    const Value *xGrid = nullptr;
    const Value *yGrid = nullptr;
    const Value *zGrid = nullptr;
    const Value *vVol = nullptr;
    const Value *sxVal = nullptr;
    const Value *syVal = nullptr;
    const Value *szVal = nullptr;
    if (nums.size() >= 7) {
        xGrid = nums[0]; yGrid = nums[1]; zGrid = nums[2]; vVol = nums[3];
        sxVal = nums[4]; syVal = nums[5]; szVal = nums[6];
    } else if (nums.size() >= 4) {
        vVol = nums[0]; sxVal = nums[1]; syVal = nums[2]; szVal = nums[3];
    } else {
        outs[0] = Value();
        return;
    }

    const keel::usize ny = vVol->dims().rows();
    const keel::usize nx = vVol->dims().cols();
    const keel::usize nz = (vVol->dims().ndim() >= 3) ? vVol->dims().dim(2) : 1;
    if (ny == 0 || nx == 0 || nz == 0) {
        outs[0] = Value();
        return;
    }

    // Extract 1-D coordinate vectors xv(nx), yv(ny), zv(nz) from 3-D meshgrid or 1-D defaults.
    keel::Array<keel::f64> xv(nx, 0.0, mr), yv(ny, 0.0, mr), zv(nz, 0.0, mr);
    for (keel::usize c = 0; c < nx; ++c)
        xv[c] = (xGrid && xGrid->numel() >= ny * nx) ? xGrid->elemAsDouble(c * ny) : keel::as<keel::f64>(c + 1);
    for (keel::usize r = 0; r < ny; ++r)
        yv[r] = (yGrid && yGrid->numel() >= ny) ? yGrid->elemAsDouble(r) : keel::as<keel::f64>(r + 1);
    for (keel::usize k = 0; k < nz; ++k)
        zv[k] = (zGrid && zGrid->numel() >= ny * nx * nz) ? zGrid->elemAsDouble(k * ny * nx) : keel::as<keel::f64>(k + 1);

    auto findBracket = [](const keel::Array<keel::f64> &axis, keel::f64 val,
                          keel::usize &i0, keel::usize &i1, keel::f64 &t) -> bool {
        const keel::usize m = axis.size();
        if (m == 0) return false;
        if (m == 1) { i0 = i1 = 0; t = 0.0; return keel::abs(val - axis[0]) <= 1e-9; }
        const bool asc = axis[m - 1] >= axis[0];
        const keel::f64 lo = asc ? axis[0] : axis[m - 1];
        const keel::f64 hi = asc ? axis[m - 1] : axis[0];
        if (val < lo - 1e-12 || val > hi + 1e-12)
            return false;
        for (keel::usize i = 0; i + 1 < m; ++i) {
            const keel::f64 a = axis[i], b = axis[i + 1];
            const bool inside = asc ? (val >= a - 1e-12 && val <= b + 1e-12)
                                    : (val <= a + 1e-12 && val >= b - 1e-12);
            if (inside) {
                i0 = i; i1 = i + 1;
                const keel::f64 d = b - a;
                t = (keel::abs(d) > 1e-30) ? (val - a) / d : 0.0;
                if (t < 0.0) t = 0.0;
                if (t > 1.0) t = 1.0;
                return true;
            }
        }
        return false;
    };

    auto vAt = [&](keel::usize r, keel::usize c, keel::usize k) -> keel::f64 {
        return vVol->elemAsDouble(r + c * ny + k * ny * nx);
    };

    keel::Array<NodeId> created(mr);
    auto emitSurface = [&](Value xd, Value yd, Value zd, Value cd) {
        const NodeId sid = scene.createChartNode(ClassId::Surface, axId, false);
        if (GraphicsNode *sn = scene.findNode(sid)) {
            sn->props.put(keel::String::from("XData", mr), keel::move(xd));
            sn->props.put(keel::String::from("YData", mr), keel::move(yd));
            sn->props.put(keel::String::from("ZData", mr), keel::move(zd));
            sn->props.put(keel::String::from("CData", mr), keel::move(cd));
        }
        created.push_back(sid);
    };

    // Constant-X slices: ny x nz surfaces
    if (sxVal && !sxVal->isEmpty()) {
        for (keel::usize si = 0; si < sxVal->numel(); ++si) {
            const keel::f64 x0 = sxVal->elemAsDouble(si);
            keel::usize c0 = 0, c1 = 0;
            keel::f64 t = 0.0;
            const bool ok = findBracket(xv, x0, c0, c1, t);
            Value xd = Value::matrix(ny, nz, ValueType::DOUBLE, mr);
            Value yd = Value::matrix(ny, nz, ValueType::DOUBLE, mr);
            Value zd = Value::matrix(ny, nz, ValueType::DOUBLE, mr);
            Value cd = Value::matrix(ny, nz, ValueType::DOUBLE, mr);
            for (keel::usize k = 0; k < nz; ++k) {
                for (keel::usize r = 0; r < ny; ++r) {
                    const keel::usize o = r + k * ny;
                    xd.doubleDataMut()[o] = x0;
                    yd.doubleDataMut()[o] = yv[r];
                    zd.doubleDataMut()[o] = zv[k];
                    cd.doubleDataMut()[o] = ok ? ((1.0 - t) * vAt(r, c0, k) + t * vAt(r, c1, k)) : keel::kNaN;
                }
            }
            emitSurface(keel::move(xd), keel::move(yd), keel::move(zd), keel::move(cd));
        }
    }
    // Constant-Y slices: nx x nz surfaces
    if (syVal && !syVal->isEmpty()) {
        for (keel::usize si = 0; si < syVal->numel(); ++si) {
            const keel::f64 y0 = syVal->elemAsDouble(si);
            keel::usize r0 = 0, r1 = 0;
            keel::f64 t = 0.0;
            const bool ok = findBracket(yv, y0, r0, r1, t);
            Value xd = Value::matrix(nx, nz, ValueType::DOUBLE, mr);
            Value yd = Value::matrix(nx, nz, ValueType::DOUBLE, mr);
            Value zd = Value::matrix(nx, nz, ValueType::DOUBLE, mr);
            Value cd = Value::matrix(nx, nz, ValueType::DOUBLE, mr);
            for (keel::usize k = 0; k < nz; ++k) {
                for (keel::usize c = 0; c < nx; ++c) {
                    const keel::usize o = c + k * nx;
                    xd.doubleDataMut()[o] = xv[c];
                    yd.doubleDataMut()[o] = y0;
                    zd.doubleDataMut()[o] = zv[k];
                    cd.doubleDataMut()[o] = ok ? ((1.0 - t) * vAt(r0, c, k) + t * vAt(r1, c, k)) : keel::kNaN;
                }
            }
            emitSurface(keel::move(xd), keel::move(yd), keel::move(zd), keel::move(cd));
        }
    }
    // Constant-Z slices: ny x nx surfaces
    if (szVal && !szVal->isEmpty()) {
        for (keel::usize si = 0; si < szVal->numel(); ++si) {
            const keel::f64 z0 = szVal->elemAsDouble(si);
            keel::usize k0 = 0, k1 = 0;
            keel::f64 t = 0.0;
            const bool ok = findBracket(zv, z0, k0, k1, t);
            Value xd = Value::matrix(ny, nx, ValueType::DOUBLE, mr);
            Value yd = Value::matrix(ny, nx, ValueType::DOUBLE, mr);
            Value zd = Value::matrix(ny, nx, ValueType::DOUBLE, mr);
            Value cd = Value::matrix(ny, nx, ValueType::DOUBLE, mr);
            for (keel::usize c = 0; c < nx; ++c) {
                for (keel::usize r = 0; r < ny; ++r) {
                    const keel::usize o = r + c * ny;
                    xd.doubleDataMut()[o] = xv[c];
                    yd.doubleDataMut()[o] = yv[r];
                    zd.doubleDataMut()[o] = z0;
                    cd.doubleDataMut()[o] = ok ? ((1.0 - t) * vAt(r, c, k0) + t * vAt(r, c, k1)) : keel::kNaN;
                }
            }
            emitSurface(keel::move(xd), keel::move(yd), keel::move(zd), keel::move(cd));
        }
    }

    Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
    scene.setProperty(axId, "View", vw, mr);
    if (!scene.isHoldOn(axId)) {
        scene.setProperty(axId, "XGrid", Value::fromString("on", mr), mr);
        scene.setProperty(axId, "YGrid", Value::fromString("on", mr), mr);
        scene.setProperty(axId, "ZGrid", Value::fromString("on", mr), mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

// ── light / camlight ────────────────────────────────────────────────────

void light_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, false, false);
    const NodeId lid = scene.createChartNode(ClassId::Light, axId, false);
    if (idx < args.size())
        applyChartNV(scene, lid, args, idx, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

void camlight_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, false, false);
    const NodeId lid = scene.createChartNode(ClassId::Light, axId, false);
    keel::f64 px = 1.0, py = 0.0, pz = 1.0;
    keel::String style = keel::String::from("infinite", mr);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String tok = args[idx++].toString();
        if (asciiEqualNoCase(tok, "left"))           { px = -1.0; py = 0.5; pz = 1.0; }
        else if (asciiEqualNoCase(tok, "right"))     { px =  1.0; py = 0.5; pz = 1.0; }
        else if (asciiEqualNoCase(tok, "headlight")) { px =  0.0; py = 0.0; pz = 1.0; }
    } else if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
        constexpr keel::f64 kDeg = 3.14159265358979323846 / 180.0;
        const keel::f64 az = args[idx++].toScalar() * kDeg;
        const keel::f64 el = args[idx++].toScalar() * kDeg;
        px = keel::cos(el) * keel::sin(az);
        py = -keel::cos(el) * keel::cos(az);
        pz = keel::sin(el);
    }
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        const keel::String st = args[idx++].toString();
        if (asciiEqualNoCase(st, "local") || asciiEqualNoCase(st, "infinite"))
            style = st.clone(mr);
    }
    Value pos = Value::matrix(1, 3, ValueType::DOUBLE, mr);
    pos.doubleDataMut()[0] = px; pos.doubleDataMut()[1] = py; pos.doubleDataMut()[2] = pz;
    scene.setProperty(lid, "Position", keel::move(pos), mr);
    scene.setProperty(lid, "Style", Value::fromString(style, mr), mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

// ── boxchart ────────────────────────────────────────────────────────────

void boxchart_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, false);

    Value xData, yData;
    bool hasX = false;
    if (idx + 1 < args.size() && !args[idx].isStruct()
        && isPlottableCoord(args[idx + 1])) {
        xData = args[idx++];
        yData = ensure2DDouble(args[idx++], mr);
        hasX = true;
    } else if (idx < args.size() && isPlottableCoord(args[idx])) {
        yData = ensure2DDouble(args[idx++], mr);
    }

    const NodeId bid = scene.createChartNode(ClassId::BoxChart, axId, true);
    if (GraphicsNode *bn = scene.findNode(bid)) {
        if (hasX) {
            bn->props.put(keel::String::from("XData", mr), xData);
            bn->props.put(keel::String::from("XDataMode", mr), Value::fromString("manual", mr));
        }
        if (!yData.isEmpty())
            bn->props.put(keel::String::from("YData", mr), yData);
    }
    if (idx < args.size())
        applyChartNV(scene, bid, args, idx, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(bid, mr) : Value();
}

// ── histogram2 ──────────────────────────────────────────────────────────

void histogram2_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, true);

    Value xVec, yVec;
    if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
        xVec = flattenToRowVec(args[idx++], mr);
        yVec = flattenToRowVec(args[idx++], mr);
    }

    keel::usize nbx = 0, nby = 0;
    Value xEdges, yEdges;
    if (idx < args.size() && args[idx].isNumeric()) {
        if (idx + 1 < args.size() && args[idx + 1].isNumeric()) {
            xEdges = flattenToRowVec(args[idx++], mr);
            yEdges = flattenToRowVec(args[idx++], mr);
        } else if (args[idx].numel() == 1) {
            nbx = nby = keel::max<keel::usize>(1, keel::as<keel::usize>(args[idx++].toScalar()));
        } else if (args[idx].numel() == 2) {
            nbx = keel::max<keel::usize>(1, keel::as<keel::usize>(args[idx].elemAsDouble(0)));
            nby = keel::max<keel::usize>(1, keel::as<keel::usize>(args[idx].elemAsDouble(1)));
            ++idx;
        }
    }

    const NodeId hid = scene.createChartNode(ClassId::Histogram2, axId, true);
    if (idx < args.size())
        applyChartNV(scene, hid, args, idx, mr);

    const keel::usize n = keel::min(xVec.numel(), yVec.numel());
    keel::f64 xlo = keel::Limits<keel::f64>::infinity(), xhi = -keel::Limits<keel::f64>::infinity();
    keel::f64 ylo = keel::Limits<keel::f64>::infinity(), yhi = -keel::Limits<keel::f64>::infinity();
    for (keel::usize i = 0; i < n; ++i) {
        const keel::f64 xv = xVec.elemAsDouble(i), yv = yVec.elemAsDouble(i);
        if (keel::is_finite(xv) && keel::is_finite(yv)) {
            xlo = keel::min(xlo, xv); xhi = keel::max(xhi, xv);
            ylo = keel::min(ylo, yv); yhi = keel::max(yhi, yv);
        }
    }
    if (!keel::is_finite(xlo) || !keel::is_finite(xhi)) { xlo = 0.0; xhi = 1.0; }
    if (!keel::is_finite(ylo) || !keel::is_finite(yhi)) { ylo = 0.0; yhi = 1.0; }
    if (xhi <= xlo) { xlo -= 0.5; xhi += 0.5; }
    if (yhi <= ylo) { ylo -= 0.5; yhi += 0.5; }

    auto makeUniformEdges = [&](keel::f64 lo, keel::f64 hi, keel::usize nb) -> Value {
        Value e = Value::matrix(1, nb + 1, ValueType::DOUBLE, mr);
        const keel::f64 dx = (hi - lo) / keel::as<keel::f64>(nb);
        for (keel::usize k = 0; k <= nb; ++k)
            e.doubleDataMut()[k] = (k == nb) ? hi : (lo + dx * keel::as<keel::f64>(k));
        return e;
    };

    if (xEdges.isEmpty()) {
        if (nbx == 0) nbx = keel::max<keel::usize>(1, keel::min<keel::usize>(20, keel::as<keel::usize>(keel::ceil(keel::sqrt(keel::as<keel::f64>(keel::max<keel::usize>(1, n)))))));
        xEdges = makeUniformEdges(xlo, xhi, nbx);
    } else {
        nbx = (xEdges.numel() >= 2) ? (xEdges.numel() - 1) : 1;
    }
    if (yEdges.isEmpty()) {
        if (nby == 0) nby = keel::max<keel::usize>(1, keel::min<keel::usize>(20, keel::as<keel::usize>(keel::ceil(keel::sqrt(keel::as<keel::f64>(keel::max<keel::usize>(1, n)))))));
        yEdges = makeUniformEdges(ylo, yhi, nby);
    } else {
        nby = (yEdges.numel() >= 2) ? (yEdges.numel() - 1) : 1;
    }

    // Bin counts (nbx x nby) — MATLAB Histogram2.Values is length(XBinEdges)-1 by length(YBinEdges)-1
    Value counts = Value::matrix(nbx, nby, ValueType::DOUBLE, mr);
    for (keel::usize k = 0; k < nbx * nby; ++k) counts.doubleDataMut()[k] = 0.0;
    keel::f64 totalCount = 0.0;
    auto findBin = [](const Value &edges, keel::f64 v) -> keel::i64 {
        const keel::usize nb = edges.numel() - 1;
        if (v < edges.elemAsDouble(0) || v > edges.elemAsDouble(nb))
            return -1;
        for (keel::usize b = 0; b < nb; ++b) {
            const keel::f64 e0 = edges.elemAsDouble(b), e1 = edges.elemAsDouble(b + 1);
            if (b + 1 == nb ? (v >= e0 && v <= e1) : (v >= e0 && v < e1))
                return keel::as<keel::i64>(b);
        }
        return -1;
    };
    for (keel::usize i = 0; i < n; ++i) {
        const keel::f64 xv = xVec.elemAsDouble(i), yv = yVec.elemAsDouble(i);
        if (!keel::is_finite(xv) || !keel::is_finite(yv)) continue;
        const keel::i64 bx = findBin(xEdges, xv);
        const keel::i64 by = findBin(yEdges, yv);
        if (bx >= 0 && by >= 0) {
            counts.doubleDataMut()[keel::as<keel::usize>(bx) + keel::as<keel::usize>(by) * nbx] += 1.0;
            totalCount += 1.0;
        }
    }

    // Apply Normalization ('count'|'probability'|'pdf'|'countdensity'|'cumcount'|'cdf')
    const keel::String norm = scene.getProperty(hid, "Normalization", mr).toString();
    Value vals = Value::matrix(nbx, nby, ValueType::DOUBLE, mr);
    for (keel::usize by = 0; by < nby; ++by) {
        const keel::f64 dy = yEdges.elemAsDouble(by + 1) - yEdges.elemAsDouble(by);
        for (keel::usize bx = 0; bx < nbx; ++bx) {
            const keel::f64 dx = xEdges.elemAsDouble(bx + 1) - xEdges.elemAsDouble(bx);
            const keel::usize idx2 = bx + by * nbx;
            const keel::f64 c = counts.doubleData()[idx2];
            if (asciiEqualNoCase(norm, "pdf")) {
                const keel::f64 area = dx * dy;
                vals.doubleDataMut()[idx2] = (totalCount > 0.0 && area > 0.0) ? c / (totalCount * area) : 0.0;
            } else if (asciiEqualNoCase(norm, "probability")) {
                vals.doubleDataMut()[idx2] = (totalCount > 0.0) ? c / totalCount : 0.0;
            } else if (asciiEqualNoCase(norm, "countdensity")) {
                const keel::f64 area = dx * dy;
                vals.doubleDataMut()[idx2] = (area > 0.0) ? c / area : 0.0;
            } else {
                vals.doubleDataMut()[idx2] = c;
            }
        }
    }

    Value dataMat = Value::matrix(n, 2, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < n; ++i) {
        dataMat.doubleDataMut()[i] = xVec.elemAsDouble(i);
        dataMat.doubleDataMut()[i + n] = yVec.elemAsDouble(i);
    }
    Value numBinsV = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    numBinsV.doubleDataMut()[0] = keel::as<keel::f64>(nbx);
    numBinsV.doubleDataMut()[1] = keel::as<keel::f64>(nby);
    Value binW = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    binW.doubleDataMut()[0] = xEdges.elemAsDouble(1) - xEdges.elemAsDouble(0);
    binW.doubleDataMut()[1] = yEdges.elemAsDouble(1) - yEdges.elemAsDouble(0);
    Value xLim = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    xLim.doubleDataMut()[0] = xEdges.elemAsDouble(0);
    xLim.doubleDataMut()[1] = xEdges.elemAsDouble(nbx);
    Value yLim = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    yLim.doubleDataMut()[0] = yEdges.elemAsDouble(0);
    yLim.doubleDataMut()[1] = yEdges.elemAsDouble(nby);

    if (GraphicsNode *hn = scene.findNode(hid)) {
        hn->props.put(keel::String::from("Data", mr), keel::move(dataMat));
        hn->props.put(keel::String::from("XBinEdges", mr), keel::move(xEdges));
        hn->props.put(keel::String::from("YBinEdges", mr), keel::move(yEdges));
        hn->props.put(keel::String::from("BinCounts", mr), keel::move(counts));
        hn->props.put(keel::String::from("Values", mr), keel::move(vals));
        hn->props.put(keel::String::from("NumBins", mr), keel::move(numBinsV));
        hn->props.put(keel::String::from("BinWidth", mr), keel::move(binW));
        hn->props.put(keel::String::from("XBinLimits", mr), keel::move(xLim));
        hn->props.put(keel::String::from("YBinLimits", mr), keel::move(yLim));
    }

    const keel::String ds = scene.getProperty(hid, "DisplayStyle", mr).toString();
    if (asciiEqualNoCase(ds, "tile")) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = 0.0; vw.doubleDataMut()[1] = 90.0;
        scene.setProperty(axId, "View", vw, mr);
        scene.setProperty(axId, "Box", Value::fromString("on", mr), mr);
    } else {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
        if (!scene.isHoldOn(axId)) {
            scene.setProperty(axId, "XGrid", Value::fromString("on", mr), mr);
            scene.setProperty(axId, "YGrid", Value::fromString("on", mr), mr);
            scene.setProperty(axId, "ZGrid", Value::fromString("on", mr), mr);
        }
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(hid, mr) : Value();
}

// ── heatmap ─────────────────────────────────────────────────────────────

void heatmap_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;

    NodeId parentId = kInvalidNodeId;
    Value savedPos;
    keel::i32 spR = 0, spC = 0, spI = 0;

    if (!args.empty()) {
        NodeId h = kInvalidNodeId;
        if (extractSceneHandle(args[0], h)) {
            if (const GraphicsNode *n = scene.findNode(h);
                n && (n->classId == ClassId::Figure || n->classId == ClassId::TiledChartLayout)) {
                parentId = h;
                idx = 1;
            }
        }
    }
    if (parentId == kInvalidNodeId) {
        const NodeId curAx = scene.currentAxesId();
        if (const GraphicsNode *cn = scene.findNode(curAx);
            cn && cn->classId == ClassId::Axes && cn->children.empty()) {
            // Replace an empty Cartesian Axes (e.g. subplot(3,2,4)) with HeatmapChart in place.
            parentId = cn->parentId;
            savedPos = scene.getProperty(curAx, "Position", mr);
            spR = cn->subplotRows; spC = cn->subplotCols; spI = cn->subplotIdx;
            scene.deleteNode(curAx);
        } else {
            parentId = scene.ensureCurrentFigure();
        }
    }

    Value xData, yData, cData;
    if (idx + 2 < args.size() && !args[idx].isStruct() && args[idx + 2].isNumeric()) {
        xData = args[idx++];
        yData = args[idx++];
        cData = ensure2DDouble(args[idx++], mr);
    } else if (idx < args.size() && args[idx].isNumeric()) {
        cData = ensure2DDouble(args[idx++], mr);
    }

    const NodeId hmid = scene.createChartNode(ClassId::HeatmapChart, parentId, false);
    if (GraphicsNode *hm = scene.findNode(hmid)) {
        hm->subplotRows = spR; hm->subplotCols = spC; hm->subplotIdx = spI;
        if (savedPos.numel() == 4)
            hm->props.put(keel::String::from("Position", mr), savedPos);
        if (!xData.isEmpty()) {
            hm->props.put(keel::String::from("XData", mr), xData);
            hm->props.put(keel::String::from("XDisplayData", mr), xData);
            if (xData.isCell() || xData.isString() || xData.isChar())
                hm->props.put(keel::String::from("XDisplayLabels", mr), xData);
        }
        if (!yData.isEmpty()) {
            hm->props.put(keel::String::from("YData", mr), yData);
            hm->props.put(keel::String::from("YDisplayData", mr), yData);
            if (yData.isCell() || yData.isString() || yData.isChar())
                hm->props.put(keel::String::from("YDisplayLabels", mr), yData);
        }
        if (!cData.isEmpty()) {
            hm->props.put(keel::String::from("ColorData", mr), cData);
            hm->props.put(keel::String::from("ColorDisplayData", mr), cData);
            keel::f64 clo = keel::Limits<keel::f64>::infinity(), chi = -keel::Limits<keel::f64>::infinity();
            for (keel::usize i = 0; i < cData.numel(); ++i) {
                const keel::f64 v = cData.elemAsDouble(i);
                if (keel::is_finite(v)) { clo = keel::min(clo, v); chi = keel::max(chi, v); }
            }
            if (keel::is_finite(clo) && keel::is_finite(chi) && chi > clo) {
                Value cl = Value::matrix(1, 2, ValueType::DOUBLE, mr);
                cl.doubleDataMut()[0] = clo; cl.doubleDataMut()[1] = chi;
                hm->props.put(keel::String::from("ColorLimits", mr), keel::move(cl));
            }
        }
    }
    if (idx < args.size())
        applyChartNV(scene, hmid, args, idx, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(hmid, mr) : Value();
}

// ── imshow / hist / waterfall / ribbon / comet / comet3 / ezplot3 / fplot3 / gscatter / boxplot / probplot / rectangle ──

void imshow_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        outs[0] = Value();
        return;
    }

    keel::usize idx = 0;
    NodeId parentAx = kInvalidNodeId;
    Value imgArg = args[idx++];
    if (imgArg.isChar() || imgArg.isString()) {
        // imshow(filename, ...) — load via imread if available
        if (const ExternalFunc *ir = ctx.engine->findExternal("imread", ctx.env)) {
            Value outV[1];
            (*ir)(keel::ArrayRef<Value>(&imgArg, 1), 1, keel::MutArrayRef<Value>(outV, 1), ctx);
            imgArg = keel::move(outV[0]);
        }
    }

    bool autoScale = false;
    Value climArg;
    Value cmapArg;
    if (idx < args.size()) {
        if (args[idx].isEmpty() && args[idx].isNumeric()) {
            autoScale = true;
            ++idx;
        } else if (args[idx].isNumeric() && args[idx].numel() == 2) {
            climArg = args[idx++];
        } else if (args[idx].isNumeric() && args[idx].dims().cols() == 3 && args[idx].dims().rows() > 1) {
            cmapArg = args[idx++];
        }
    }
    // Scan optional NV pairs for Parent / DisplayRange / Colormap
    keel::Array<keel::usize> imgNVIdx(mr);
    for (keel::usize i = idx; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString())
            break;
        const keel::String nm = args[i].toString();
        if (asciiEqualNoCase(nm, "Parent")) {
            NodeId h = kInvalidNodeId;
            if (extractSceneHandle(args[i + 1], h) && isAxesLike(scene, h))
                parentAx = h;
        } else if (asciiEqualNoCase(nm, "DisplayRange")) {
            if (args[i + 1].isEmpty()) autoScale = true;
            else if (args[i + 1].isNumeric() && args[i + 1].numel() == 2) climArg = args[i + 1];
        } else if (asciiEqualNoCase(nm, "Colormap")) {
            cmapArg = args[i + 1];
        } else if (asciiEqualNoCase(nm, "InitialMagnification") || asciiEqualNoCase(nm, "Border")
                   || asciiEqualNoCase(nm, "Interpolation") || asciiEqualNoCase(nm, "Reduce")) {
            // Accepted display options (no-op in scene DOM).
        } else if (schemaFor(ClassId::Image).findExactNoCase(nm) != nullptr) {
            imgNVIdx.push_back(i);
        }
    }

    const NodeId axId = scene.prepareAxesForPlot(parentAx, true);
    const NodeId iid = scene.createChartNode(ClassId::Image, axId, false);
    if (GraphicsNode *in = scene.findNode(iid)) {
        if (!imgArg.isEmpty()) {
            in->props.put(keel::String::from("CData", mr), imgArg);
            Value defX = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            defX.doubleDataMut()[0] = 1.0;
            defX.doubleDataMut()[1] = keel::as<keel::f64>(keel::max<keel::usize>(1, imgArg.dims().cols()));
            Value defY = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            defY.doubleDataMut()[0] = 1.0;
            defY.doubleDataMut()[1] = keel::as<keel::f64>(keel::max<keel::usize>(1, imgArg.dims().rows()));
            in->props.put(keel::String::from("XData", mr), keel::move(defX));
            in->props.put(keel::String::from("YData", mr), keel::move(defY));
        }
        in->props.put(keel::String::from("CDataMapping", mr),
                      Value::fromString(cmapArg.isEmpty() ? "scaled" : "direct", mr));
    }
    for (keel::usize i : imgNVIdx)
        scene.setProperty(iid, args[i].toString(), args[i + 1], mr);

    scene.setProperty(axId, "YDir", Value::fromString("reverse", mr), mr);
    scene.setProperty(axId, "Visible", Value::fromString("off", mr), mr);
    if (!cmapArg.isEmpty())
        scene.setProperty(axId, "Colormap", cmapArg, mr);

    if (!climArg.isEmpty()) {
        scene.setProperty(axId, "CLim", climArg, mr);
    } else if (autoScale && !imgArg.isEmpty()) {
        keel::f64 lo = keel::Limits<keel::f64>::infinity();
        keel::f64 hi = -keel::Limits<keel::f64>::infinity();
        for (keel::usize k = 0; k < imgArg.numel(); ++k) {
            const keel::f64 v = imgArg.elemAsDouble(k);
            if (keel::is_finite(v)) { lo = keel::min(lo, v); hi = keel::max(hi, v); }
        }
        if (keel::is_finite(lo) && keel::is_finite(hi)) {
            if (hi <= lo) { lo -= 0.5; hi += 0.5; }
            Value cl = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            cl.doubleDataMut()[0] = lo; cl.doubleDataMut()[1] = hi;
            scene.setProperty(axId, "CLim", keel::move(cl), mr);
        }
    } else if (cmapArg.isEmpty() && !imgArg.isEmpty() && imgArg.dims().ndim() <= 2) {
        Value cl = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        if (imgArg.type() == ValueType::UINT8) { cl.doubleDataMut()[0] = 0.0; cl.doubleDataMut()[1] = 255.0; }
        else if (imgArg.type() == ValueType::UINT16) { cl.doubleDataMut()[0] = 0.0; cl.doubleDataMut()[1] = 65535.0; }
        else { cl.doubleDataMut()[0] = 0.0; cl.doubleDataMut()[1] = 1.0; }
        scene.setProperty(axId, "CLim", keel::move(cl), mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(iid, mr) : Value();
}

void hist_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        outs[0] = Value();
        return;
    }

    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    if (idx >= args.size()) {
        outs[0] = Value();
        return;
    }
    const Value &data = args[idx++];
    Value centers;
    Value counts;
    keel::f64 bw = 1.0;

    if (idx < args.size() && args[idx].isNumeric() && args[idx].numel() > 1) {
        const keel::usize bins = args[idx].numel();
        centers = Value::matrix(1, bins, ValueType::DOUBLE, mr);
        counts = Value::matrix(1, bins, ValueType::DOUBLE, mr);
        for (keel::usize b = 0; b < bins; ++b)
            centers.doubleDataMut()[b] = args[idx].elemAsDouble(b);
        const keel::f64 *cd = centers.doubleData();
        bw = (bins >= 2) ? keel::abs(cd[1] - cd[0]) : 1.0;
        if (!(bw > 0.0) || !keel::is_finite(bw)) bw = 1.0;
        for (keel::usize i = 0; i < data.numel(); ++i) {
            const keel::f64 v = data.elemAsDouble(i);
            if (keel::is_nan(v)) continue;
            keel::usize b = 0;
            while (b + 1 < bins && v > 0.5 * (cd[b] + cd[b + 1]))
                ++b;
            counts.doubleDataMut()[b] += 1.0;
        }
    } else {
        const keel::i32 bins = (idx < args.size() && args[idx].isNumeric() && !args[idx].isEmpty())
            ? keel::max<keel::i32>(1, keel::as<keel::i32>(args[idx].toScalar()))
            : 10;
        keel::f64 mn = keel::Limits<keel::f64>::infinity();
        keel::f64 mx = -keel::Limits<keel::f64>::infinity();
        for (keel::usize i = 0; i < data.numel(); ++i) {
            const keel::f64 v = data.elemAsDouble(i);
            if (keel::is_finite(v)) { mn = keel::min(mn, v); mx = keel::max(mx, v); }
        }
        if (!keel::is_finite(mn)) { mn = 0.0; mx = 1.0; }
        bw = (mx - mn) / keel::as<keel::f64>(bins);
        if (!(bw > 0.0) || !keel::is_finite(bw)) bw = 1.0;
        centers = Value::matrix(1, keel::as<keel::usize>(bins), ValueType::DOUBLE, mr);
        counts = Value::matrix(1, keel::as<keel::usize>(bins), ValueType::DOUBLE, mr);
        for (keel::i32 b = 0; b < bins; ++b)
            centers.doubleDataMut()[keel::as<keel::usize>(b)] = mn + bw * (keel::as<keel::f64>(b) + 0.5);
        for (keel::usize i = 0; i < data.numel(); ++i) {
            const keel::f64 v = data.elemAsDouble(i);
            if (!keel::is_finite(v)) continue;
            keel::i32 b = keel::as<keel::i32>(keel::floor((v - mn) / bw));
            if (b >= bins) b = bins - 1;
            if (b < 0) b = 0;
            counts.doubleDataMut()[keel::as<keel::usize>(b)] += 1.0;
        }
    }

    if (nargout >= 1) {
        outs[0] = keel::move(counts);
        if (nargout >= 2 && outs.size() >= 2)
            outs[1] = keel::move(centers);
        return;
    }

    // nargout == 0: render as Patch bar histogram (matches reference `hist` scene object type).
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);
    const NodeId pid = scene.createChartNode(ClassId::Patch, axId, true);
    const keel::usize M = centers.numel();
    if (M > 0) {
        const keel::usize nVerts = 5 * M + 1;
        Value verts = Value::matrix(nVerts, 2, ValueType::DOUBLE, mr);
        Value faces = Value::matrix(M, 4, ValueType::DOUBLE, mr);
        keel::f64 *vd = verts.doubleDataMut();
        keel::f64 *fd = faces.doubleDataMut();
        const keel::f64 *cd = centers.doubleData();
        const keel::f64 *yd = counts.doubleData();
        const keel::f64 halfW = 0.5 * bw;
        for (keel::usize k = 0; k < M; ++k) {
            const keel::f64 xL = cd[k] - halfW;
            const keel::f64 xR = cd[k] + halfW;
            const keel::f64 h = yd[k];
            const keel::usize base = 5 * k;
            vd[base + 0] = xL; vd[base + 0 + nVerts] = 0.0;
            vd[base + 1] = xL; vd[base + 1 + nVerts] = 0.0;
            vd[base + 2] = xL; vd[base + 2 + nVerts] = h;
            vd[base + 3] = xR; vd[base + 3 + nVerts] = h;
            vd[base + 4] = xR; vd[base + 4 + nVerts] = 0.0;
            fd[k + 0 * M] = keel::as<keel::f64>(base + 2);
            fd[k + 1 * M] = keel::as<keel::f64>(base + 3);
            fd[k + 2 * M] = keel::as<keel::f64>(base + 4);
            fd[k + 3 * M] = keel::as<keel::f64>(base + 5);
        }
        vd[5 * M] = cd[M - 1] + halfW;
        vd[5 * M + nVerts] = 0.0;
        if (GraphicsNode *pn = scene.findNode(pid)) {
            pn->props.put(keel::String::from("Vertices", mr), keel::move(verts));
            pn->props.put(keel::String::from("Faces", mr), keel::move(faces));
        }
        scene.syncPatchVerticesAndData(pid, "Vertices", mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = Value();
}

void waterfall_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, true);

    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == 4) break;
    }
    Value xd, yd, zd;
    if (nums.size() == 1 || nums.size() == 2) {
        zd = ensure2DDouble(*nums[0], mr);
        xd = makeRange1N(zd.dims().cols(), mr);
        yd = makeRange1N(zd.dims().rows(), mr);
    } else if (nums.size() >= 3) {
        xd = ensure2DDouble(*nums[0], mr);
        yd = ensure2DDouble(*nums[1], mr);
        zd = ensure2DDouble(*nums[2], mr);
    }

    const NodeId pid = scene.createChartNode(ClassId::Patch, axId, false);
    if (!zd.isEmpty()) {
        const keel::usize R = zd.dims().rows();
        const keel::usize C = zd.dims().cols();
        const bool xGrid = (xd.dims().rows() == R && xd.dims().cols() == C);
        const bool yGrid = (yd.dims().rows() == R && yd.dims().cols() == C);
        keel::f64 zBase = keel::Limits<keel::f64>::infinity();
        for (keel::usize i = 0; i < zd.numel(); ++i) {
            const keel::f64 v = zd.elemAsDouble(i);
            if (keel::is_finite(v)) zBase = keel::min(zBase, v);
        }
        if (!keel::is_finite(zBase)) zBase = 0.0;

        // Each row becomes a curtain polygon of (C + 5) vertices (matches reference waterfall patch).
        const keel::usize ptsPerRow = C + 5;
        const keel::usize totalV = R * ptsPerRow;
        Value verts = Value::matrix(totalV, 3, ValueType::DOUBLE, mr);
        Value faces = Value::matrix(R, ptsPerRow, ValueType::DOUBLE, mr);
        keel::f64 *vd = verts.doubleDataMut();
        keel::f64 *fd = faces.doubleDataMut();
        for (keel::usize r = 0; r < R; ++r) {
            auto xOf = [&](keel::usize c) { return xGrid ? xd.elemAsDouble(r + c * R) : (c < xd.numel() ? xd.elemAsDouble(c) : keel::as<keel::f64>(c + 1)); };
            auto yOf = [&](keel::usize c) { return yGrid ? yd.elemAsDouble(r + c * R) : (r < yd.numel() ? yd.elemAsDouble(r) : keel::as<keel::f64>(r + 1)); };
            auto setV = [&](keel::usize localIdx, keel::f64 x, keel::f64 y, keel::f64 z) {
                const keel::usize gi = r * ptsPerRow + localIdx;
                vd[gi]              = x;
                vd[gi + totalV]     = y;
                vd[gi + 2 * totalV] = z;
                fd[r + localIdx * R] = keel::as<keel::f64>(gi + 1);
            };
            setV(0, xOf(0), yOf(0), zBase);
            setV(1, xOf(0), yOf(0), zd.elemAsDouble(r));
            for (keel::usize c = 0; c < C; ++c)
                setV(c + 2, xOf(c), yOf(c), zd.elemAsDouble(r + c * R));
            setV(C + 2, xOf(C - 1), yOf(C - 1), zd.elemAsDouble(r + (C - 1) * R));
            setV(C + 3, xOf(C - 1), yOf(C - 1), zBase);
            setV(C + 4, xOf(C - 1), yOf(C - 1), zBase);
        }
        if (GraphicsNode *pn = scene.findNode(pid)) {
            pn->props.put(keel::String::from("Vertices", mr), keel::move(verts));
            pn->props.put(keel::String::from("Faces", mr), keel::move(faces));
        }
        scene.syncPatchVerticesAndData(pid, "Vertices", mr);
    }
    if (idx < args.size())
        applyChartNV(scene, pid, args, idx, mr);
    Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
    scene.setProperty(axId, "View", vw, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(pid, mr) : Value();
}

void ribbon_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, true);

    keel::Array<const Value *> nums(mr);
    while (idx < args.size() && args[idx].isNumeric()) {
        nums.push_back(&args[idx++]);
        if (nums.size() == 3) break;
    }
    Value xv, zv;
    keel::f64 width = 0.75;
    if (nums.size() == 1) {
        zv = ensure2DDouble(*nums[0], mr);
    } else if (nums.size() == 2) {
        if (nums[1]->numel() == 1) {
            zv = ensure2DDouble(*nums[0], mr);
            width = nums[1]->toScalar();
        } else {
            xv = ensure2DDouble(*nums[0], mr);
            zv = ensure2DDouble(*nums[1], mr);
        }
    } else if (nums.size() >= 3) {
        xv = ensure2DDouble(*nums[0], mr);
        zv = ensure2DDouble(*nums[1], mr);
        width = nums[2]->toScalar();
    }

    keel::Array<NodeId> created(mr);
    if (!zv.isEmpty()) {
        const keel::usize R = zv.dims().rows();
        const keel::usize C = zv.dims().cols();
        const keel::f64 halfW = 0.5 * width;
        for (keel::usize c = 0; c < C; ++c) {
            const NodeId sid = scene.createChartNode(ClassId::Surface, axId, false);
            Value rx = Value::matrix(R, 2, ValueType::DOUBLE, mr);
            Value ry = Value::matrix(R, 2, ValueType::DOUBLE, mr);
            Value rz = Value::matrix(R, 2, ValueType::DOUBLE, mr);
            Value rc = Value::matrix(R, 2, ValueType::DOUBLE, mr);
            const keel::f64 cx = keel::as<keel::f64>(c + 1);
            for (keel::usize r = 0; r < R; ++r) {
                const keel::f64 yVal = (!xv.isEmpty() && r < xv.numel()) ? xv.elemAsDouble(r) : keel::as<keel::f64>(r + 1);
                const keel::f64 zVal = zv.elemAsDouble(r + c * R);
                rx.doubleDataMut()[r]     = cx - halfW;
                rx.doubleDataMut()[r + R] = cx + halfW;
                ry.doubleDataMut()[r]     = yVal;
                ry.doubleDataMut()[r + R] = yVal;
                rz.doubleDataMut()[r]     = zVal;
                rz.doubleDataMut()[r + R] = zVal;
                rc.doubleDataMut()[r]     = cx;
                rc.doubleDataMut()[r + R] = cx;
            }
            if (GraphicsNode *sn = scene.findNode(sid)) {
                sn->props.put(keel::String::from("XData", mr), keel::move(rx));
                sn->props.put(keel::String::from("YData", mr), keel::move(ry));
                sn->props.put(keel::String::from("ZData", mr), keel::move(rz));
                sn->props.put(keel::String::from("CData", mr), keel::move(rc));
            }
            created.push_back(sid);
        }
    }
    if (idx < args.size() && !created.empty())
        applyChartNVToAll(scene, created.as_ref(), args, idx, mr);
    Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
    scene.setProperty(axId, "View", vw, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void cometCore(keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> outs, CallContext &ctx, bool is3D)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, is3D);

    Value xv, yv, zv;
    if (!is3D) {
        if (idx + 1 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric()) {
            xv = flattenToRowVec(args[idx++], mr);
            yv = flattenToRowVec(args[idx++], mr);
        } else if (idx < args.size() && args[idx].isNumeric()) {
            yv = flattenToRowVec(args[idx++], mr);
            xv = makeRange1N(yv.numel(), mr);
        }
    } else if (idx + 2 < args.size() && args[idx].isNumeric() && args[idx + 1].isNumeric() && args[idx + 2].isNumeric()) {
        xv = flattenToRowVec(args[idx++], mr);
        yv = flattenToRowVec(args[idx++], mr);
        zv = flattenToRowVec(args[idx++], mr);
    }

    // Reference comet/comet3 creates 3 children: 1 head Line (1 point at end) + 2 AnimatedLine (body, tail).
    const NodeId headId = scene.createChartNode(ClassId::Line, axId, true);
    const NodeId bodyId = scene.createChartNode(ClassId::AnimatedLine, axId, true);
    const NodeId tailId = scene.createChartNode(ClassId::AnimatedLine, axId, true);
    if (!xv.isEmpty() && !yv.isEmpty()) {
        const keel::usize n = keel::min(xv.numel(), yv.numel());
        if (GraphicsNode *hn = scene.findNode(headId); hn && n > 0) {
            hn->props.put(keel::String::from("XData", mr), Value::scalar(xv.elemAsDouble(n - 1), mr));
            hn->props.put(keel::String::from("YData", mr), Value::scalar(yv.elemAsDouble(n - 1), mr));
            if (is3D && !zv.isEmpty())
                hn->props.put(keel::String::from("ZData", mr), Value::scalar(zv.elemAsDouble(keel::min(n, zv.numel()) - 1), mr));
            hn->props.put(keel::String::from("Marker", mr), Value::fromString("o", mr));
        }
        const Value *zPtr = (is3D && !zv.isEmpty()) ? &zv : nullptr;
        scene.animatedLineAddPoints(bodyId, xv, yv, zPtr, mr);
        scene.animatedLineAddPoints(tailId, xv, yv, zPtr, mr);
    }
    if (is3D) {
        Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
        scene.setProperty(axId, "View", vw, mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = Value();
}

void comet_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ cometCore(a, no, o, c, false); }
void comet3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ cometCore(a, no, o, c, true); }

Value toParamHandle(const Value &v, CallContext &ctx, keel::Allocator *mr)
{
    if (v.isFuncHandle())
        return v;
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
                body.append("@(t) ");
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

void fplot3Core(keel::ArrayRef<Value> args, keel::usize nargout,
                keel::MutArrayRef<Value> outs, CallContext &ctx, bool isEz)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true, true);
    if (idx + 2 >= args.size()) {
        outs[0] = Value();
        return;
    }
    const Value fx = toParamHandle(args[idx++], ctx, mr);
    const Value fy = toParamHandle(args[idx++], ctx, mr);
    const Value fz = toParamHandle(args[idx++], ctx, mr);
    if (!fx.isFuncHandle() || !fy.isFuncHandle() || !fz.isFuncHandle()) {
        outs[0] = Value();
        return;
    }
    keel::f64 lo = isEz ? 0.0 : -5.0;
    keel::f64 hi = isEz ? (2.0 * keel::kPi) : 5.0;
    if (idx < args.size() && args[idx].isNumeric() && args[idx].numel() >= 2) {
        lo = args[idx].elemAsDouble(0);
        hi = args[idx].elemAsDouble(1);
        ++idx;
    }
    // Optional 'animate' flag on ezplot3
    bool animate = false;
    if (isEz && idx < args.size() && (args[idx].isChar() || args[idx].isString())
        && asciiEqualNoCase(args[idx].toString(), "animate")) {
        animate = true;
        ++idx;
    }

    const keel::usize kN = isEz ? 300 : 200;
    Value xv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    Value yv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    Value zv = Value::matrix(1, kN, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < kN; ++i) {
        const keel::f64 t = lo + (hi - lo) * keel::as<keel::f64>(i) / keel::as<keel::f64>(kN - 1);
        const Value tArr[1] = {Value::scalar(t, mr)};
        const Value rx = ctx.engine->callFunctionHandle(fx, keel::ArrayRef<Value>(tArr, 1));
        const Value ry = ctx.engine->callFunctionHandle(fy, keel::ArrayRef<Value>(tArr, 1));
        const Value rz = ctx.engine->callFunctionHandle(fz, keel::ArrayRef<Value>(tArr, 1));
        xv.doubleDataMut()[i] = rx.isNumeric() && !rx.isEmpty() ? rx.elemAsDouble(0) : keel::kNaN;
        yv.doubleDataMut()[i] = ry.isNumeric() && !ry.isEmpty() ? ry.elemAsDouble(0) : keel::kNaN;
        zv.doubleDataMut()[i] = rz.isNumeric() && !rz.isEmpty() ? rz.elemAsDouble(0) : keel::kNaN;
    }

    const keel::f64 xLast = xv.doubleData()[kN - 1];
    const keel::f64 yLast = yv.doubleData()[kN - 1];
    const keel::f64 zLast = zv.doubleData()[kN - 1];

    const ClassId lineCid = isEz ? ClassId::Line : ClassId::ParameterizedFunctionLine;
    const NodeId lid = scene.createChartNode(lineCid, axId, true);
    if (GraphicsNode *ln = scene.findNode(lid)) {
        ln->props.put(keel::String::from("XData", mr), keel::move(xv));
        ln->props.put(keel::String::from("YData", mr), keel::move(yv));
        ln->props.put(keel::String::from("ZData", mr), keel::move(zv));
    }
    if (animate) {
        // ezplot3(..., 'animate') appends a 1-point marker head line at the final point.
        const NodeId headId = scene.createChartNode(ClassId::Line, axId, false);
        if (GraphicsNode *hn = scene.findNode(headId)) {
            hn->props.put(keel::String::from("XData", mr), Value::scalar(xLast, mr));
            hn->props.put(keel::String::from("YData", mr), Value::scalar(yLast, mr));
            hn->props.put(keel::String::from("ZData", mr), Value::scalar(zLast, mr));
            hn->props.put(keel::String::from("Marker", mr), Value::fromString("o", mr));
            hn->props.put(keel::String::from("LineStyle", mr), Value::fromString("none", mr));
        }
    }
    if (idx < args.size()) {
        const ClassSchema &lSchema = schemaFor(ClassId::Line);
        if ((args[idx].isChar() || args[idx].isString())
            && !(((args.size() - idx) % 2 == 0) && lSchema.findExactNoCase(args[idx].toString()) != nullptr)) {
            const ParsedLineSpec ls = parseLineSpec(args[idx].toString(), mr);
            if (ls.valid) { applyLineSpecToNode(scene, lid, ls, mr); ++idx; }
        }
        if (idx < args.size())
            applyChartNV(scene, lid, args, idx, mr);
    }
    Value vw = Value::matrix(1, 2, ValueType::DOUBLE, mr);
    vw.doubleDataMut()[0] = -37.5; vw.doubleDataMut()[1] = 30.0;
    scene.setProperty(axId, "View", vw, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(lid, mr) : Value();
}

void fplot3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fplot3Core(a, no, o, c, false); }
void ezplot3_scene_reg(keel::ArrayRef<Value> a, keel::usize no, keel::MutArrayRef<Value> o, CallContext &c)
{ fplot3Core(a, no, o, c, true); }

void gscatter_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    if (args.size() < 2) {
        outs[0] = Value();
        return;
    }
    const NodeId axId = scene.prepareAxesForPlot(kInvalidNodeId, true);
    const Value &X = args[0];
    const Value &Y = args[1];
    const keel::usize N = keel::min(X.numel(), Y.numel());
    if (N == 0) {
        outs[0] = Value();
        return;
    }

    struct GroupKey {
        keel::String label;
        keel::f64 numVal = 0.0;
        bool isNumeric = false;
    };
    keel::Array<GroupKey> groupKeys(mr);
    keel::Array<keel::usize> groupIdx(N, 0, mr);
    if (args.size() >= 3 && !args[2].isEmpty()) {
        const Value &G = args[2];
        const bool gNum = G.isNumeric() || G.isLogical();
        for (keel::usize i = 0; i < N; ++i) {
            const keel::usize giPos = (i < G.numel()) ? i : (G.numel() - 1);
            keel::String key(mr);
            keel::f64 nv = 0.0;
            if (G.isCell()) key = G.cellAt(giPos).toString();
            else if (G.isString()) key = G.stringElem(giPos, mr);
            else { nv = G.elemAsDouble(giPos); key = keel::format("{:.6g}", nv); }
            keel::usize found = groupKeys.size();
            for (keel::usize j = 0; j < groupKeys.size(); ++j) {
                if (gNum ? (groupKeys[j].numVal == nv) : (groupKeys[j].label == key)) {
                    found = j;
                    break;
                }
            }
            if (found == groupKeys.size())
                groupKeys.push_back(GroupKey{keel::move(key), nv, gNum});
            groupIdx[i] = found;
        }
    } else {
        groupKeys.push_back(GroupKey{keel::String::from("1", mr), 1.0, true});
    }

    // Reference gscatter orders unique groups ascending (numeric or lexicographic).
    keel::Array<keel::usize> order(groupKeys.size(), mr);
    for (keel::usize i = 0; i < groupKeys.size(); ++i) order[i] = i;
    keel::sort(order.begin(), order.end(), [&](keel::usize a, keel::usize b) {
        if (groupKeys[a].isNumeric && groupKeys[b].isNumeric)
            return groupKeys[a].numVal < groupKeys[b].numVal;
        return groupKeys[a].label < groupKeys[b].label;
    });

    keel::Array<keel::String> groupNames(mr);
    keel::Array<NodeId> created(mr);
    for (keel::usize oi = 0; oi < order.size(); ++oi) {
        const keel::usize gi = order[oi];
        keel::Array<keel::f64> gx(mr), gy(mr);
        for (keel::usize i = 0; i < N; ++i) {
            if (groupIdx[i] == gi) {
                gx.push_back(X.elemAsDouble(i));
                gy.push_back(Y.elemAsDouble(i));
            }
        }
        const NodeId lid = scene.createChartNode(ClassId::Line, axId, true);
        Value xv = Value::matrix(1, gx.size(), ValueType::DOUBLE, mr);
        Value yv = Value::matrix(1, gy.size(), ValueType::DOUBLE, mr);
        for (keel::usize k = 0; k < gx.size(); ++k) {
            xv.doubleDataMut()[k] = gx[k];
            yv.doubleDataMut()[k] = gy[k];
        }
        if (GraphicsNode *ln = scene.findNode(lid)) {
            ln->props.put(keel::String::from("XData", mr), keel::move(xv));
            ln->props.put(keel::String::from("YData", mr), keel::move(yv));
            ln->props.put(keel::String::from("LineStyle", mr), Value::fromString("none", mr));
            ln->props.put(keel::String::from("Marker", mr), Value::fromString(".", mr));
            ln->props.put(keel::String::from("DisplayName", mr), Value::fromString(groupKeys[gi].label, mr));
        }
        groupNames.push_back(groupKeys[gi].label.clone(mr));
        created.push_back(lid);
    }
    scene.createOrUpdateLegend(axId, groupNames.as_ref());
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void boxplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);
    if (idx >= args.size()) {
        outs[0] = Value();
        return;
    }
    const Value &X = args[idx++];
    keel::Array<keel::String> labels(mr);
    keel::Array<keel::Array<keel::f64>> groups(mr);

    if (idx < args.size() && (args[idx].isCell() || args[idx].isString() || args[idx].isNumeric())
        && args[idx].numel() == X.numel() && X.numel() > 1) {
        const Value &G = args[idx++];
        for (keel::usize i = 0; i < X.numel(); ++i) {
            keel::String key(mr);
            if (G.isCell()) key = G.cellAt(i).toString();
            else if (G.isString()) key = G.stringElem(i, mr);
            else key = keel::format("{:.6g}", G.elemAsDouble(i));
            keel::usize gi = labels.size();
            for (keel::usize j = 0; j < labels.size(); ++j) {
                if (labels[j] == key) { gi = j; break; }
            }
            if (gi == labels.size()) {
                labels.push_back(keel::move(key));
                groups.push_back(keel::Array<keel::f64>(mr));
            }
            const keel::f64 v = X.elemAsDouble(i);
            if (keel::is_finite(v))
                groups[gi].push_back(v);
        }
    } else if (X.dims().rows() > 1 && X.dims().cols() > 1) {
        for (keel::usize c = 0; c < X.dims().cols(); ++c) {
            labels.push_back(keel::format("{}", c + 1));
            keel::Array<keel::f64> col(mr);
            for (keel::usize r = 0; r < X.dims().rows(); ++r) {
                const keel::f64 v = X.elemAsDouble(r + c * X.dims().rows());
                if (keel::is_finite(v)) col.push_back(v);
            }
            groups.push_back(keel::move(col));
        }
    } else {
        labels.push_back(keel::String::from("1", mr));
        keel::Array<keel::f64> col(mr);
        for (keel::usize i = 0; i < X.numel(); ++i) {
            const keel::f64 v = X.elemAsDouble(i);
            if (keel::is_finite(v)) col.push_back(v);
        }
        groups.push_back(keel::move(col));
    }

    // Reference boxplot groups all box/whisker/median primitives under a single hggroup child of Axes.
    const NodeId grpId = scene.createChartNode(ClassId::Group, axId, false);
    if (GraphicsNode *gn = scene.findNode(grpId))
        gn->props.put(keel::String::from("Tag", mr), Value::fromString("boxplot", mr));
    keel::Array<NodeId> created(mr);
    for (keel::usize g = 0; g < groups.size(); ++g) {
        keel::Array<keel::f64> &vals = groups[g];
        if (vals.empty()) continue;
        keel::sort(vals);
        auto pct = [&](keel::f64 p) -> keel::f64 {
            const keel::f64 pos = p * keel::as<keel::f64>(vals.size() - 1);
            const keel::usize lo = keel::as<keel::usize>(keel::floor(pos));
            const keel::usize hi = keel::min(lo + 1, vals.size() - 1);
            return vals[lo] + (pos - keel::as<keel::f64>(lo)) * (vals[hi] - vals[lo]);
        };
        const keel::f64 q1 = pct(0.25), med = pct(0.5), q3 = pct(0.75);
        const keel::f64 cx = keel::as<keel::f64>(g + 1);
        const keel::f64 hw = 0.25;
        const NodeId boxId = scene.createChartNode(ClassId::Line, grpId, false);
        Value bx = Value::matrix(1, 5, ValueType::DOUBLE, mr);
        Value by = Value::matrix(1, 5, ValueType::DOUBLE, mr);
        bx.doubleDataMut()[0] = cx - hw; by.doubleDataMut()[0] = q1;
        bx.doubleDataMut()[1] = cx + hw; by.doubleDataMut()[1] = q1;
        bx.doubleDataMut()[2] = cx + hw; by.doubleDataMut()[2] = q3;
        bx.doubleDataMut()[3] = cx - hw; by.doubleDataMut()[3] = q3;
        bx.doubleDataMut()[4] = cx - hw; by.doubleDataMut()[4] = q1;
        if (GraphicsNode *bn = scene.findNode(boxId)) {
            bn->props.put(keel::String::from("XData", mr), keel::move(bx));
            bn->props.put(keel::String::from("YData", mr), keel::move(by));
            bn->props.put(keel::String::from("Tag", mr), Value::fromString("Box", mr));
        }
        const NodeId medId = scene.createChartNode(ClassId::Line, grpId, false);
        Value mx = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        Value my = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        mx.doubleDataMut()[0] = cx - hw; my.doubleDataMut()[0] = med;
        mx.doubleDataMut()[1] = cx + hw; my.doubleDataMut()[1] = med;
        if (GraphicsNode *mn = scene.findNode(medId)) {
            mn->props.put(keel::String::from("XData", mr), keel::move(mx));
            mn->props.put(keel::String::from("YData", mr), keel::move(my));
            mn->props.put(keel::String::from("Tag", mr), Value::fromString("Median", mr));
        }
        created.push_back(boxId);
        created.push_back(medId);
    }
    if (!labels.empty()) {
        Value xtk = Value::matrix(1, labels.size(), ValueType::DOUBLE, mr);
        Value xtl = Value::cell(1, labels.size(), mr);
        for (keel::usize i = 0; i < labels.size(); ++i) {
            xtk.doubleDataMut()[i] = keel::as<keel::f64>(i + 1);
            xtl.cellAt(i) = Value::fromString(labels[i], mr);
        }
        scene.setProperty(axId, "XTick", keel::move(xtk), mr);
        scene.setProperty(axId, "XTickLabel", keel::move(xtl), mr);
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleColumn(created.as_ref(), mr) : Value();
}

void probplot_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString()))
        ++idx; // distribution name (default 'normal')
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, true);
    keel::Array<keel::f64> ys(mr);
    if (idx < args.size() && args[idx].isNumeric()) {
        for (keel::usize i = 0; i < args[idx].numel(); ++i) {
            const keel::f64 v = args[idx].elemAsDouble(i);
            if (keel::is_finite(v)) ys.push_back(v);
        }
    }
    keel::sort(ys);
    const keel::usize n = ys.size();
    // Acklam rational approximation of standard normal inverse CDF (relative error < 1.15e-9).
    auto normInv = [](keel::f64 p) -> keel::f64 {
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
    };

    // Reference probplot emits FunctionLine (reference fit) first, then data Line (N points) second.
    const NodeId refLine = scene.createChartNode(ClassId::FunctionLine, axId, false);
    const NodeId dataLine = scene.createChartNode(ClassId::Line, axId, true);
    if (n > 0) {
        Value xv = Value::matrix(1, n, ValueType::DOUBLE, mr);
        Value yv = Value::matrix(1, n, ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < n; ++i) {
            const keel::f64 p = (keel::as<keel::f64>(i) + 0.5) / keel::as<keel::f64>(n);
            xv.doubleDataMut()[i] = ys[i];
            yv.doubleDataMut()[i] = normInv(p);
        }
        if (GraphicsNode *dn = scene.findNode(dataLine)) {
            dn->props.put(keel::String::from("XData", mr), keel::move(xv));
            dn->props.put(keel::String::from("YData", mr), keel::move(yv));
            dn->props.put(keel::String::from("LineStyle", mr), Value::fromString("none", mr));
            dn->props.put(keel::String::from("Marker", mr), Value::fromString("+", mr));
        }
        auto qAt = [&](keel::f64 p) -> keel::f64 {
            const keel::f64 pos = p * keel::as<keel::f64>(n) - 0.5;
            if (pos <= 0.0) return ys[0];
            if (pos >= keel::as<keel::f64>(n - 1)) return ys[n - 1];
            const keel::usize lo = keel::as<keel::usize>(keel::floor(pos));
            const keel::usize hi = keel::min(lo + 1, n - 1);
            return ys[lo] + (pos - keel::as<keel::f64>(lo)) * (ys[hi] - ys[lo]);
        };
        const keel::f64 q25 = qAt(0.25), q75 = qAt(0.75);
        const keel::f64 z25 = normInv(0.25), z75 = normInv(0.75);
        const keel::f64 slope = (keel::abs(q75 - q25) > 1e-15) ? (z75 - z25) / (q75 - q25) : 0.0;
        const keel::f64 intercept = z25 - slope * q25;
        constexpr keel::usize kRefN = 45;
        Value rx = Value::matrix(1, kRefN, ValueType::DOUBLE, mr);
        Value ry = Value::matrix(1, kRefN, ValueType::DOUBLE, mr);
        for (keel::usize k = 0; k < kRefN; ++k) {
            const keel::f64 xk = ys[0] + (ys[n - 1] - ys[0]) * keel::as<keel::f64>(k) / keel::as<keel::f64>(kRefN - 1);
            rx.doubleDataMut()[k] = xk;
            ry.doubleDataMut()[k] = intercept + slope * xk;
        }
        if (GraphicsNode *rn = scene.findNode(refLine)) {
            rn->props.put(keel::String::from("XData", mr), keel::move(rx));
            rn->props.put(keel::String::from("YData", mr), keel::move(ry));
            rn->props.put(keel::String::from("LineStyle", mr), Value::fromString("--", mr));
        }
    }
    scene.recomputeAxesAutoLimits(axId, mr);
    if (nargout >= 1) {
        const NodeId pair[2] = {dataLine, refLine};
        outs[0] = scene.makeHandleColumn(keel::ArrayRef<NodeId>(pair, 2), mr);
    } else {
        outs[0] = Value();
    }
}

void rectangle_scene_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                         keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    keel::usize idx = 0;
    const NodeId leadingAx = resolveLeadingAxesOpt(scene, args, idx);
    const NodeId axId = scene.prepareAxesForPlot(leadingAx, false);
    const NodeId rid = scene.createChartNode(ClassId::Rectangle, axId, false);
    if (idx < args.size())
        applyChartNV(scene, rid, args, idx, mr);
    scene.recomputeAxesAutoLimits(axId, mr);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(rid, mr) : Value();
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
    regBoth("line",    "fplot3",       &fplot3_scene_reg);
    regBoth("line",    "ezplot3",      &ezplot3_scene_reg);
    regBoth("line",    "comet",        &comet_scene_reg);
    regBoth("line",    "comet3",       &comet3_scene_reg);
    regBoth("line",    "ribbon",       &ribbon_scene_reg);
    regBoth("line",    "animatedline", &animatedline_scene_reg);
    regBoth("line",    "addpoints",    &addpoints_scene_reg);
    regBoth("line",    "getpoints",    &getpoints_scene_reg);
    regBoth("line",    "clearpoints",  &clearpoints_scene_reg);

    regBoth("bar",     "scatter",      &scatter_scene_reg);
    regBoth("bar",     "scatter3",     &scatter3_scene_reg);
    regBoth("bar",     "gscatter",     &gscatter_scene_reg);
    regBoth("bar",     "bar",          &bar_scene_reg);
    regBoth("bar",     "barh",         &barh_scene_reg);
    regBoth("bar",     "hist",         &hist_scene_reg);
    regBoth("bar",     "histogram",    &histogram_scene_reg);
    regBoth("bar",     "histogram2",   &histogram2_scene_reg);
    regBoth("bar",     "boxchart",     &boxchart_scene_reg);
    regBoth("bar",     "boxplot",      &boxplot_scene_reg);
    regBoth("bar",     "probplot",     &probplot_scene_reg);
    regBoth("bar",     "pie",          &pie_scene_reg);
    regBoth("bar",     "pie3",         &pie_scene_reg);
    regBoth("bar",     "patch",        &patch_scene_reg);
    regBoth("bar",     "fill",         &fill_scene_reg);
    regBoth("bar",     "fill3",        &fill3_scene_reg);
    regBoth("bar",     "rectangle",    &rectangle_scene_reg);

    regBoth("surface", "surf",         &surf_scene_reg);
    regBoth("surface", "mesh",         &mesh_scene_reg);
    regBoth("surface", "surfc",        &surfc_scene_reg);
    regBoth("surface", "meshc",        &meshc_scene_reg);
    regBoth("surface", "waterfall",    &waterfall_scene_reg);
    regBoth("surface", "pcolor",       &pcolor_scene_reg);
    regBoth("surface", "surface",      &surface_scene_reg);
    regBoth("surface", "slice",        &slice_scene_reg);
    regBoth("surface", "quiver",       &quiver_scene_reg);
    regBoth("surface", "quiver3",      &quiver3_scene_reg);
    regBoth("surface", "light",        &light_scene_reg);
    regBoth("surface", "camlight",     &camlight_scene_reg);

    regBoth("contour", "contour",      &contour_scene_reg);
    regBoth("contour", "contourf",     &contourf_scene_reg);
    regBoth("contour", "contour3",     &contour3_scene_reg);

    regBoth("image",   "image",        &image_scene_reg);
    regBoth("image",   "imagesc",      &imagesc_scene_reg);
    regBoth("image",   "imshow",       &imshow_scene_reg);
    regBoth("image",   "heatmap",      &heatmap_scene_reg);

    regBoth("polar",   "polarplot",    &polarplot_scene_reg);
    regBoth("polar",   "polarscatter", &polarscatter_scene_reg);

    regBoth("layout",  "annotation",   &annotation_scene_reg);
}

} // namespace numkit::gfx::detail
