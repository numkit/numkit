// src/builtin/register/graphics/graphics_library.cpp
//
// Engine-coupled install hub for the Model-First GraphicsScene graphics
// service, handle accessors (set/get/findobj/findall/isgraphics/ishandle/isvalid),
// figure export, and miscellaneous utility builtins.

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>

#include <numkit/core/engine.hpp>
#include <numkit/graphics/graphics_scene.hpp>
#include <numkit/graphics/library.hpp>
#include <numkit/value/error.hpp>

namespace numkit::gfx::detail {
void registerSceneGraphics(Engine &engine);
bool trySceneGet(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs, CallContext &ctx);
bool trySceneSet(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx);
bool trySceneIsValid(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs, CallContext &ctx);
bool trySceneIsGraphics(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs, CallContext &ctx);
bool trySceneIsHandle(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs, CallContext &ctx);
bool trySceneFind(keel::ArrayRef<Value> args, bool includeHidden,
                  keel::MutArrayRef<Value> outs, CallContext &ctx);
} // namespace numkit::gfx::detail

namespace numkit {

void registerFigureExport(Engine &engine);

namespace {

void set_reg(keel::ArrayRef<Value> args, keel::usize nargout,
             keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (gfx::detail::trySceneSet(args, nargout, outs, ctx))
        return;
    if (args.empty()) {
        outs[0] = Value();
        return;
    }
    throw Error("Invalid or deleted object.", 0, 0, "set", "", "numkit:graphics:InvalidHandle");
}

void get_reg(keel::ArrayRef<Value> args, keel::usize,
             keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (gfx::detail::trySceneGet(args, outs, ctx))
        return;
    if (args.empty()) {
        outs[0] = Value();
        return;
    }
    throw Error("Invalid or deleted object.", 0, 0, "get", "", "numkit:graphics:InvalidHandle");
}

void findobj_reg(keel::ArrayRef<Value> args, keel::usize,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (gfx::detail::trySceneFind(args, /*includeHidden=*/false, outs, ctx))
        return;
    outs[0] = coreEngine(ctx).graphicsScene().makeHandleColumn({}, ctx.engine->allocator());
}

void findall_reg(keel::ArrayRef<Value> args, keel::usize,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (gfx::detail::trySceneFind(args, /*includeHidden=*/true, outs, ctx))
        return;
    outs[0] = coreEngine(ctx).graphicsScene().makeHandleColumn({}, ctx.engine->allocator());
}

void isgraphics_reg(keel::ArrayRef<Value> args, keel::usize,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (gfx::detail::trySceneIsGraphics(args, outs, ctx))
        return;
    outs[0] = args.empty() ? Value() : Value::logicalScalar(false, ctx.engine->allocator());
}

void ishandle_reg(keel::ArrayRef<Value> args, keel::usize,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (gfx::detail::trySceneIsHandle(args, outs, ctx))
        return;
    outs[0] = args.empty() ? Value() : Value::logicalScalar(false, ctx.engine->allocator());
}

void isvalid_reg(keel::ArrayRef<Value> args, keel::usize,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (gfx::detail::trySceneIsValid(args, outs, ctx))
        return;
    outs[0] = args.empty() ? Value() : Value::logicalScalar(false, ctx.engine->allocator());
}

void graphicsNoop_reg(keel::ArrayRef<Value>, keel::usize nargout,
                      keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    outs[0] = (nargout >= 1) ? Value::scalar(1.0, ctx.engine->allocator()) : Value();
}

void uicontrol_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    const gfx::NodeId fid = scene.ensureCurrentFigure();
    const gfx::NodeId id = scene.createChartNode(gfx::ClassId::TextBox, fid, false);
    for (keel::usize i = 0; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString()) continue;
        const keel::String nm = args[i].toString();
        if (scene.hasProperty(id, nm))
            scene.setProperty(id, nm, args[i + 1], mr);
    }
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(id, mr) : Value();
}

void videoWriter_reg(keel::ArrayRef<Value> args, keel::usize,
                     keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    Value s = Value::structure(mr);
    const keel::String fn = (!args.empty() && (args[0].isChar() || args[0].isString()))
                                ? args[0].toString() : keel::String::from("output.avi");
    s.setFieldAll("Filename", Value::fromString(fn, mr));
    s.setFieldAll("FrameRate", Value::scalar(30.0, mr));
    s.setFieldAll("Quality", Value::scalar(75.0, mr));
    outs[0] = keel::move(s);
}

void open_reg(keel::ArrayRef<Value>, keel::usize nargout,
              keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    outs[0] = (nargout >= 1) ? Value::structure(ctx.engine->allocator()) : Value();
}

void writeVideo_reg(keel::ArrayRef<Value>, keel::usize,
                    keel::MutArrayRef<Value> outs, CallContext &)
{
    outs[0] = Value();
}

void graphStub_reg(keel::ArrayRef<Value> args, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    Value s = Value::structure(mr);
    if (!args.empty()) s.setFieldAll("Adj", args[0]);
    outs[0] = keel::move(s);
}

void listfonts_reg(keel::ArrayRef<Value>, keel::usize,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    Value c = Value::cell(6, 1, mr);
    c.cellAt(0) = Value::fromString("Helvetica", mr);
    c.cellAt(1) = Value::fromString("Arial", mr);
    c.cellAt(2) = Value::fromString("Times", mr);
    c.cellAt(3) = Value::fromString("Times New Roman", mr);
    c.cellAt(4) = Value::fromString("Courier", mr);
    c.cellAt(5) = Value::fromString("Courier New", mr);
    outs[0] = keel::move(c);
}

struct GfxRegEntry {
    const char *name;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
};

constexpr GfxRegEntry kHandleAccessorFns[] = {
    {"set",        &set_reg},
    {"get",        &get_reg},
    {"findobj",    &findobj_reg},
    {"findall",    &findall_reg},
    {"isgraphics", &isgraphics_reg},
    {"ishandle",   &ishandle_reg},
    {"isvalid",    &isvalid_reg},
};

constexpr GfxRegEntry kMiscGfxFns[] = {
    {"shg",         &graphicsNoop_reg},
    {"refreshdata", &graphicsNoop_reg},
    {"datetick",    &graphicsNoop_reg},
    {"uicontrol",   &uicontrol_reg},
    {"VideoWriter", &videoWriter_reg},
    {"open",        &open_reg},
    {"writeVideo",  &writeVideo_reg},
    {"digraph",     &graphStub_reg},
    {"graph",       &graphStub_reg},
    {"plotmatrix",  &graphicsNoop_reg},
    {"makedatatip", &graphicsNoop_reg},
    {"sound",       &graphicsNoop_reg},
    {"soundsc",     &graphicsNoop_reg},
    {"zplane",      &graphicsNoop_reg},
    {"listfonts",   &listfonts_reg},
};

} // anonymous namespace

void GraphicsLibrary::install(Engine &engine)
{
    gfx::detail::registerSceneGraphics(engine);
    for (const auto &entry : kHandleAccessorFns)
        engine.registerFunction("", entry.name, entry.fn);
    registerFigureExport(engine);
    for (const auto &entry : kMiscGfxFns)
        engine.registerFunction("", entry.name, entry.fn);
}

} // namespace numkit
