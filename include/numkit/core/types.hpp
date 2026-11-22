// include/types.hpp
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/function.h>
#include <keel/hash_map.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/sys/plugin.hpp>
#include <numkit/core/ast.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/value/error.hpp>   // numkit::Error (L0) — re-exported here for existing users
#include <numkit/value/value.hpp>


namespace numkit {

// ============================================================
// Timer types (monotonic nanoseconds from keel::monotonic_ns())
// ============================================================
using TimePoint = keel::u64;

// ============================================================
// Function types
// ============================================================
using BinaryOpFunc = keel::Function<Value(const Value &, const Value &)>;
using UnaryOpFunc = keel::Function<Value(const Value &)>;

// ============================================================
// Control flow signals
// ============================================================
struct BreakSignal
{};
struct ContinueSignal
{};
struct ReturnSignal
{};

enum class FlowSignal : keel::u8 { NONE = 0, BREAK, CONTINUE, RETURN };

// ============================================================
// Reserved names — classified for precise use at call sites.
// ============================================================
//
// Numeric/logical constants served by `constantsEnv_` (parent of the
// base workspace). Always defined at runtime. Assigning to one creates
// a shadow local; `clear name` removes the shadow.
// (`true`/`false`/`nan`/`NaN`/`inf`/`Inf`/`eps` are built-in functions,
// not constants — they support argument forms.)
[[nodiscard]] constexpr bool isBuiltinConstant(keel::StringRef name) noexcept
{
    return name == "pi" || name == "i" || name == "j";
}

// Runtime pseudo-variables set by the VM / display pipeline, not by
// user code. `nargin` / `nargout` are loaded on function entry; `ans`
// is set by DISPLAY for bare expressions; `end` is the magic keyword
// used in indexing. None of them live in any environment.
[[nodiscard]] constexpr bool isPseudoVar(keel::StringRef name) noexcept
{
    return name == "ans" || name == "nargin" || name == "nargout" || name == "end";
}

// Union of the above — "any name the compiler / debugger treats
// specially and must not mistake for a plain user variable". Most
// filter sites want this broad set; the narrower ones are for places
// where we specifically need "true constant" semantics (e.g. deciding
// whether to LOAD_CONST from `constantsEnv_`).
[[nodiscard]] constexpr bool isBuiltinName(keel::StringRef name) noexcept
{
    return isBuiltinConstant(name) || isPseudoVar(name);
}

// numkit::Error (runtime error with source location) moved to an L0 header:
// <nk/value/error.hpp>, included above. Kept out of this heavy core header
// so value/ops/toolboxes can throw it without pulling in the engine.

// ============================================================
// User-defined function descriptor
// ============================================================
struct UserFunction
{
    keel::String name;
    keel::Array<keel::String> params;
    keel::Array<keel::String> returns;
    keel::Arc<const ASTNode> body;
    keel::Arc<Environment> closureEnv;
    // Non-owning pointer to enclosing lexical Environment for non-escaped nested
    // functions (breaks self-cycle `heapEnv -> nestedFuncs_ -> UserFunction -> heapEnv`).
    // Resolved against active lexical environments on escape/invocation.
    Environment *rawClosureEnv = nullptr;
    mutable keel::i8 usesNarginNargout = -1;
    // classdef methods/constructors only: the class that declares this body.
    // Drives the member-access execution context (pushed while the body runs).
    // Empty for ordinary functions.
    keel::String ownerClass;
    // m-file provenance — set by the m-file resolver (file functions),
    // captured at creation for anonymous functions and at registration for
    // classdef methods. When originKnown the call machinery pushes a
    // script-origin frame for the body's duration, so mfilename() reports
    // the captured file and sibling lookups see its directory. A KNOWN but
    // EMPTY origin (function born at the command window) pushes an empty
    // entry — masking the caller's file, matching the reference engine's
    // capture-at-creation semantics. Not known (in-script defs, closures)
    // → no push, the body runs under the caller's origin.
    bool originKnown = false;
    keel::String sourceFs;
    keel::String sourceDir;
    keel::String sourceFile;
    keel::usize numUpvalueCaptures = 0;

    UserFunction clone(keel::Allocator *alloc = nullptr) const
    {
        UserFunction cp;
        cp.name = name.clone(alloc);
        cp.params = params.clone(alloc);
        cp.returns = returns.clone(alloc);
        cp.body = body;
        cp.closureEnv = closureEnv;
        cp.rawClosureEnv = rawClosureEnv;
        cp.usesNarginNargout = usesNarginNargout;
        cp.ownerClass = ownerClass.clone(alloc);
        cp.originKnown = originKnown;
        cp.sourceFs = sourceFs.clone(alloc);
        cp.sourceDir = sourceDir.clone(alloc);
        cp.sourceFile = sourceFile.clone(alloc);
        cp.numUpvalueCaptures = numUpvalueCaptures;
        return cp;
    }
};

// Shared mutable box for nested-function lexical variables (Upvalues).
// Backed by a handle ObjectState (`isHandle = true`), so copying the Value
// across VM registers and closure captures shares the exact same cell.
struct UpvaluePayload final : NativePayload
{
    Value value;
    keel::Arc<Environment> globalEnv;
    keel::Arc<keel::HashMap<keel::String, Value>> persistentBucket;
    keel::String backingName;

    explicit UpvaluePayload(Value v = Value()) : value(keel::move(v)) {}

    Value read() const
    {
        if (globalEnv) {
            if (Value *gv = globalEnv->get(backingName))
                return *gv;
            return Value::Empty;
        }
        if (persistentBucket) {
            if (const Value *pv = persistentBucket->get(backingName))
                return *pv;
            return Value::Empty;
        }
        return value;
    }

    void write(Value v)
    {
        if (globalEnv) {
            if (v.isDeleted() || v.isUnset())
                globalEnv->remove(backingName);
            else
                globalEnv->set(backingName, keel::move(v));
            return;
        }
        if (persistentBucket) {
            if (v.isDeleted() || v.isUnset())
                persistentBucket->remove(backingName);
            else
                persistentBucket->put(backingName.clone(), keel::move(v));
            return;
        }
        value = keel::move(v);
    }

    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        auto *a = alloc ? alloc : keel::default_allocator();
        auto cp = keel::make_arc_in<UpvaluePayload>(a, value);
        cp->globalEnv = globalEnv;
        cp->persistentBucket = persistentBucket;
        cp->backingName = backingName.clone(a);
        return cp;
    }
};

inline Value makeUpvalueBox(Value init = Value(), keel::Allocator *mr = nullptr)
{
    auto *res = mr ? mr : keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(res, res);
    st->native = keel::make_arc_in<UpvaluePayload>(res, keel::move(init));
    return Value::object("__upvalue__", keel::move(st), /*isHandle=*/true, res);
}

inline UpvaluePayload *asUpvalueBox(const Value &v) noexcept
{
    if (!v.isObject() || v.objectClassNameRef() != "__upvalue__")
        return nullptr;
    const ObjectState *st = v.objectStateConst();
    return st ? static_cast<UpvaluePayload *>(st->native.get()) : nullptr;
}

} // namespace numkit
