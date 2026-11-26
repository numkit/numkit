// include/numkit/graphics/graphics_handle.hpp
//
// Graphics-handle object payload — the Value-side half of the handle
// registry (bugs/closed/graphics/plot-family-no-return-value.md).
//
// Layering: the registry (id → HandleRecord) lives in FigureManager
// (figure layer, no Engine). This header carries the NativePayload that
// a handle Value holds: just the registry id. It depends only on the
// value layer, so three consumers can share it without layering knots:
//   • the graphics plot bodies (core-free, e.g. close(figureHandle)),
//   • the bundle install hub (graphics_library.cpp — accessor layer),
//   • the builtin layer (general_reg.cpp — unified delete dispatch).
#pragma once

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <numkit/value/object.hpp>
#include <numkit/value/value.hpp>

namespace numkit {

// NativePayload carrying the GraphicsScene (inScene=true) or legacy FigureManager handle id.
struct GraphicsHandlePayload : public NativePayload
{
    keel::i32 handleId = 0;
    keel::i32 numericValue = 0;
    bool inScene = false;
    keel::String className;
    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        auto *res = alloc ? alloc : keel::default_allocator();
        auto p = keel::make_arc_in<GraphicsHandlePayload>(res);
        p->handleId = handleId;
        p->numericValue = numericValue;
        p->inScene = inScene;
        p->className = className.clone(res);
        return p;
    }
    keel::f64 asDouble() const override { return keel::as<keel::f64>(numericValue != 0 ? numericValue : handleId); }
    keel::StringRef concreteClassName() const noexcept override { return className.as_ref(); }
};

// Registry id of a graphics-handle Value, or -1 when `v` is not one.
// Payload-based (not class-name-based): class(h) reports the MATLAB
// class (matlab.graphics.chart.primitive.Line, …), so the class name is
// NOT a discriminator — the payload is.
inline keel::i32 graphicsHandleIdOf(const Value &v)
{
    if (!v.isObject() || v.objectCount() == 0)
        return -1;
    auto *st = v.objectStateConst();
    if (!st || !st->native)
        return -1;
    auto *p = dynamic_cast<const GraphicsHandlePayload *>(st->native.get());
    return p ? p->handleId : -1;
}

inline bool isSceneGraphicsHandle(const Value &v)
{
    if (!v.isObject() || v.objectCount() == 0)
        return false;
    auto *st = v.objectStateConst();
    if (!st || !st->native)
        return false;
    auto *p = dynamic_cast<const GraphicsHandlePayload *>(st->native.get());
    return p && p->inScene;
}

inline bool graphicsHandleAt(const Value &v, keel::usize idx,
                             keel::i32 &idOut, bool &inSceneOut)
{
    if (!v.isObject() || idx >= v.objectCount())
        return false;
    const ObjectState *st = v.objectStateAt(idx);
    if (!st || !st->native)
        return false;
    auto *p = dynamic_cast<const GraphicsHandlePayload *>(st->native.get());
    if (!p)
        return false;
    idOut = p->handleId;
    inSceneOut = p->inScene;
    return true;
}

// Handle object Value wrapping registry id `id` with MATLAB class name
// `className` (that string is what class()/isa() report).
inline Value makeGraphicsHandleValue(keel::StringRef className, keel::i32 id,
                                     keel::Allocator *mr = nullptr)
{
    auto *res = mr ? mr : keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(res, res);
    auto payload = keel::make_arc_in<GraphicsHandlePayload>(res);
    payload->handleId = id;
    payload->className = keel::String::from(className, res);
    st->native = keel::move(payload);
    return Value::object(className, keel::move(st), /*isHandle=*/true, res);
}

inline Value makeGraphicsHandleValue(keel::StringRef className, keel::i32 id,
                                     keel::i32 numericVal,
                                     keel::Allocator *mr = nullptr)
{
    auto *res = mr ? mr : keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(res, res);
    auto payload = keel::make_arc_in<GraphicsHandlePayload>(res);
    payload->handleId = id;
    payload->numericValue = numericVal;
    payload->className = keel::String::from(className, res);
    st->native = keel::move(payload);
    return Value::object(className, keel::move(st), /*isHandle=*/true, res);
}

} // namespace numkit
