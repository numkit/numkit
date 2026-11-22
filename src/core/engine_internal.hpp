// src/core/engine_internal.hpp
// Private internal structs & helpers shared across engine*.cpp TUs.
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/types.hpp>
#include <numkit/value/value.hpp>

namespace numkit {

// ── classdef → BuiltinClass adapter ──────────────────────────
// Descriptor captured by the synthesised BuiltinClass hooks; kept alive by
// the keel::SmallFunctions in the class registry, and stored in classDefs_ so a
// subclass can merge its bases. (Namespace-scope to match the engine.hpp
// forward declaration used by Engine::classDefs_.)
// classdef member-access levels (private/protected enforcement). `Immutable`
// is a SetAccess-only level: the property is settable only inside its
// declaring class's constructor.
enum class Access
{
    Public,
    Protected,
    Private,
    Immutable
};

// A property: name, default value, and per-side access. `declClass` is the
// class that declared it (needed because protected access admits subclasses,
// and inherited properties keep their original declaring class).
struct PropInfo
{
    keel::String name;
    Value def;
    Access getAccess = Access::Public;
    Access setAccess = Access::Public;
    keel::String declClass;
    bool isConstant = false; // `Constant` → also exposed as ClassName.Prop
    bool isDependent = false; // `properties (Dependent)` → computed via get.Prop

    PropInfo clone(keel::Allocator *alloc = nullptr) const
    {
        return PropInfo{
            name.clone(alloc),
            def,
            getAccess,
            setAccess,
            declClass.clone(alloc),
            isConstant,
            isDependent,
        };
    }
};
// Method access recorded ONLY for non-public methods (absent == public →
// no check, no overhead). `declClass` as above.
struct MethodAccess
{
    Access level = Access::Public;
    keel::String declClass;

    MethodAccess clone(keel::Allocator *alloc = nullptr) const
    {
        return MethodAccess{level, declClass.clone(alloc)};
    }
};
// A Static method: callable as `ClassName.method(args)`. Stored so it can be
// inherited (re-registered under a subclass name) and access-checked.
struct StaticInfo
{
    keel::Arc<UserFunction> uf;
    Access access = Access::Public;
    keel::String declClass;

    StaticInfo clone(keel::Allocator *alloc = nullptr) const
    {
        return StaticInfo{uf, access, declClass.clone(alloc)};
    }
};

struct ClassDefDesc
{
    keel::String name;
    bool isHandle = false;
    keel::Array<PropInfo> props;                                     // name+default+access
    bool anyNonPublicProp = false;                                   // fast-path gate
    keel::Arc<UserFunction> ctor;                                    // null if none
    keel::HashMap<keel::String, keel::Arc<UserFunction>> methods;
    keel::HashMap<keel::String, keel::Arc<UserFunction>> getters;    // get.Prop
    keel::HashMap<keel::String, keel::Arc<UserFunction>> setters;    // set.Prop
    keel::HashMap<keel::String, StaticInfo> statics;                 // Static methods
    keel::Array<keel::String> superclasses;                          // transitive, for isa
    // Non-public methods only (public == absent).
    keel::HashMap<keel::String, MethodAccess> methodAccess;
    // Constructor access (this class's OWN ctor). Public unless the ctor sits
    // in a `methods (Access = private|protected)` block.
    Access ctorAccess = Access::Public;
    keel::String ctorDeclClass;
    bool isEnum = false;                          // has an `enumeration` block
    keel::Array<keel::String> abstractMethods;    // declared Abstract (own + inherited)
    bool isAbstract = false;                      // an abstract method is unimplemented
    bool isSealed = false;                        // `Sealed` — cannot be subclassed
    keel::Array<keel::String> hiddenMembers;      // `Hidden` props/methods (own + inherited)
};

inline bool hasPathSepOrColon(keel::StringRef s) noexcept
{
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':')
            return true;
    }
    return false;
}

} // namespace numkit
