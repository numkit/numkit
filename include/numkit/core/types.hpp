// include/types.hpp
#pragma once

#include <keel/core.h>
#include <keel/memory.h>
#include <numkit/sys/plugin.hpp>
#include <numkit/core/ast.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/value/error.hpp>   // numkit::Error (L0) — re-exported here for existing users
#include <numkit/value/value.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace numkit {

// ============================================================
// Timer types
// ============================================================
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

// ============================================================
// Function types
// ============================================================
using BinaryOpFunc = std::function<Value(const Value &, const Value &)>;
using UnaryOpFunc = std::function<Value(const Value &)>;

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
extern const std::unordered_set<std::string> kBuiltinConstants;

// Runtime pseudo-variables set by the VM / display pipeline, not by
// user code. `nargin` / `nargout` are loaded on function entry; `ans`
// is set by DISPLAY for bare expressions; `end` is the magic keyword
// used in indexing. None of them live in any environment.
extern const std::unordered_set<std::string> kPseudoVars;

// Union of the above — "any name the compiler / debugger treats
// specially and must not mistake for a plain user variable". Most
// filter sites want this broad set; the narrower ones are for places
// where we specifically need "true constant" semantics (e.g. deciding
// whether to LOAD_CONST from `constantsEnv_`).
extern const std::unordered_set<std::string> kBuiltinNames;

// numkit::Error (runtime error with source location) moved to an L0 header:
// <nk/value/error.hpp>, included above. Kept out of this heavy core header
// so value/ops/toolboxes can throw it without pulling in the engine.

// ============================================================
// User-defined function descriptor
// ============================================================
struct UserFunction
{
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> returns;
    std::shared_ptr<const ASTNode> body;
    std::shared_ptr<Environment> closureEnv;
    std::weak_ptr<Environment> weakClosureEnv;
    mutable keel::i8 usesNarginNargout = -1;
    // classdef methods/constructors only: the class that declares this body.
    // Drives the member-access execution context (pushed while the body runs).
    // Empty for ordinary functions.
    std::string ownerClass;
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
    std::string sourceFs;
    std::string sourceDir;
    std::string sourceFile;
    keel::usize numUpvalueCaptures = 0;
};

// Shared mutable box for nested-function lexical variables (Upvalues).
// Backed by a handle ObjectState (`isHandle = true`), so copying the Value
// across VM registers and closure captures shares the exact same cell.
struct UpvaluePayload final : NativePayload
{
    Value value;
    std::shared_ptr<Environment> globalEnv;
    std::shared_ptr<std::unordered_map<std::string, Value>> persistentBucket;
    std::string backingName;

    explicit UpvaluePayload(Value v = Value()) : value(std::move(v)) {}

    Value read() const
    {
        if (globalEnv) {
            if (Value *gv = globalEnv->get(backingName))
                return *gv;
            return Value::Empty;
        }
        if (persistentBucket) {
            auto it = persistentBucket->find(backingName);
            return (it != persistentBucket->end()) ? it->second : Value::Empty;
        }
        return value;
    }

    void write(Value v)
    {
        if (globalEnv) {
            if (v.isDeleted() || v.isUnset())
                globalEnv->remove(backingName);
            else
                globalEnv->set(backingName, std::move(v));
            return;
        }
        if (persistentBucket) {
            if (v.isDeleted() || v.isUnset())
                persistentBucket->erase(backingName);
            else
                (*persistentBucket)[backingName] = std::move(v);
            return;
        }
        value = std::move(v);
    }

    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        auto cp = keel::make_arc_in<UpvaluePayload>(alloc ? alloc : keel::default_allocator(), value);
        cp->globalEnv = globalEnv;
        cp->persistentBucket = persistentBucket;
        cp->backingName = backingName;
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
    if (!v.isObject() || v.objectClassName() != "__upvalue__")
        return nullptr;
    const ObjectState *st = v.objectStateConst();
    return st ? static_cast<UpvaluePayload *>(st->native.get()) : nullptr;
}

} // namespace numkit
