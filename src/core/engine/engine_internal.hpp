// src/core/engine_internal.hpp
// Private internal structs & helpers shared across engine*.cpp TUs.
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/small_array.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/core/fusion_rule.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/object.hpp>
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

// Active classdef method/constructor frame on the TreeWalker access-control stack.
struct ClassCtxFrame
{
    keel::String className;
    bool isCtor = false;
};

// Encapsulated class-registry and classdef metadata state owned by Engine.
struct ClassRegistryState
{
    keel::HashMap<keel::String, keel::Arc<BuiltinClass>> classes;
    keel::HashMap<keel::String, keel::Arc<ClassDefDesc>> classDefs;
    keel::HashMap<keel::String, keel::Arc<const ASTNode>> classDefAst;
    bool suppressDependentCascade = false;
    keel::Array<keel::String> activePropAccessors;
    keel::Array<keel::String> builtinClassSourceTexts;
    keel::Array<ClassCtxFrame> classCtx;
};

struct MFileCacheEntry
{
    keel::String fullPath;                    // VFS-prefixed, the resolved location
    keel::i64 mtime = 0;                      // 0 if backend can't supply mtime
    keel::i64 mtimeNs = 0;                    // nanosecond mtime when available
    keel::i64 fileSize = -1;                  // file size from stat (-1 if unknown)
    keel::String contentHash;                 // fallback when mtime is unavailable
    keel::Arc<const keel::String> sourceCode; // keeps AST live
    bool hasExternal = false;                 // true when resolveMFile_ registered externalFuncs_[name]
};

// Encapsulated m-file search path and cache state owned by Engine.
struct MFileResolverState
{
    keel::Array<keel::String> mPath;
    keel::HashMap<keel::String, MFileCacheEntry> mFileCache;
    keel::HashSet<keel::String> mFileMissCache;
};

// Encapsulated function, namespace, callback, and persistent state owned by Engine.
struct FunctionRegistryState
{
    // Arc-backed symbol maps guarantee pointer stability across open-addressing
    // HashMap rehashes (returned const T* and cached CallCacheEntry pointers stay valid).
    keel::HashMap<keel::String, keel::Arc<ExternalFunc>> externalFuncs;
    // Auxiliary indices into externalFuncs, populated by registerFunction.
    keel::HashMap<keel::String, keel::SmallArray<keel::String, 2>> shortNameIndex;
    keel::Array<keel::String> namespaceOrder;
    keel::HashSet<keel::String> namespaceSet;
    keel::HashSet<keel::String> pluginNamespaces;
    keel::HashSet<keel::String> isolatedPluginNamespaces;
    NamespaceMode namespaceMode = NamespaceMode::Compat;
    // Higher-order builtins that can run callbacks as pausable VM frames.
    keel::HashMap<keel::String, keel::Arc<CallbackBuiltin>> callbackBuiltins;
    keel::HashMap<keel::String, keel::Arc<UserFunction>> userFuncs;
    // Script-lexical user functions — peer to userFuncs but never cleared by `clear all`.
    keel::HashMap<keel::String, keel::Arc<UserFunction>> scriptLocalUserFuncs;
    keel::Array<keel::HashMap<keel::String, keel::Arc<UserFunction>>> savedScriptLocalUserFuncs;
    keel::HashMap<keel::String, keel::Arc<keel::HashMap<keel::String, Value>>> persistentStore;
    // Startup registration sources retained for post-clear replay (reinstallBuiltinSources).
    keel::Array<keel::String> builtinMSourceTexts;
};

// Encapsulated session state (warnings, env overlay, timers, errors, host
// constants, var-decl commands, and fusion telemetry) owned by Engine.
struct SessionState
{
    keel::String lastWarnMsg;
    keel::String lastWarnId;
    keel::HashMap<keel::String, keel::String> envOverlay;
    keel::HashMap<keel::String, Value> userConstants;
    keel::HashMap<keel::String, PluginContext::VarDeclExtractor> varDeclCommands;
    TimePoint ticBase{};
    bool ticCalled = false;
    bool clearAllCalled = false;
    keel::Array<CallStackFrame> lastErrorStack;
    Value lastErrorStruct;
    keel::Array<FusionRule> fusionRules;
    bool fusionEnabled = true;
    keel::usize fusionHits = 0;
};

inline bool hasPathSepOrColon(keel::StringRef s) noexcept
{
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':')
            return true;
    }
    return false;
}


struct OpMethod
{
    const char *token;
    const char *method;
    bool unary;
};

inline constexpr OpMethod kOpMethods[] = {
    {"+", "plus", false},      {"-", "minus", false},     {"*", "mtimes", false},
    {".*", "times", false},    {"/", "mrdivide", false},  {"./", "rdivide", false},
    {"\\", "mldivide", false}, {".\\", "ldivide", false}, {"^", "mpower", false},
    {".^", "power", false},    {"==", "eq", false},       {"~=", "ne", false},
    {"<", "lt", false},        {"<=", "le", false},       {">", "gt", false},
    {">=", "ge", false},       {"&", "and", false},       {"|", "or", false},
    {"-", "uminus", true},     {"+", "uplus", true},      {"~", "not", true},
    {"'", "ctranspose", true}, {".'", "transpose", true},
};

inline const char *operatorMethodName(keel::StringRef op)
{
    for (const auto &e : kOpMethods)
        if (!e.unary && op == e.token)
            return e.method;
    return nullptr;
}

inline const char *unaryOperatorMethodName(keel::StringRef op)
{
    for (const auto &e : kOpMethods)
        if (e.unary && op == e.token)
            return e.method;
    return nullptr;
}

inline bool isOperatorMethodName(keel::StringRef n)
{
    for (const auto &e : kOpMethods)
        if (n == e.method)
            return true;
    return false;
}

inline const char *accessWord(Access a)
{
    switch (a) {
    case Access::Private:
        return "private";
    case Access::Protected:
        return "protected";
    case Access::Immutable:
        return "immutable";
    default:
        return "public";
    }
}

inline void enforceAccess(Engine *engine, Access level, keel::StringRef declClass,
                          const char *kind, keel::StringRef name)
{
    if (level == Access::Public)
        return;
    if (level == Access::Immutable) {
        if (engine->classCtxInCtorOf(declClass))
            return;
        auto msg = keel::format(
            "Cannot set property '{}': it has immutable SetAccess (settable only in the {} constructor)",
            name, declClass);
        throw numkit::Error(msg.c_str());
    }
    if (engine->classCtxAllows(declClass, /*privateOnly=*/level == Access::Private))
        return;
    auto msg = keel::format("Cannot access {} '{}' of class '{}': it has {} access",
                            kind, name, declClass, accessWord(level));
    throw numkit::Error(msg.c_str());
}

inline const PropInfo *findProp(const ClassDefDesc &d, keel::StringRef name)
{
    for (const auto &p : d.props)
        if (p.name == name)
            return &p;
    return nullptr;
}

inline Value buildSubsStruct(const char *type, keel::ArrayRef<Value> subscripts,
                             keel::Allocator *mr)
{
    Value subs = Value::cell(1, subscripts.size(), mr);
    for (keel::usize i = 0; i < subscripts.size(); ++i)
        subs.cellAt(i) = subscripts[i];
    Value s = Value::structure(mr);
    s.setField(0, "type", Value::fromString(type, mr));
    s.setField(0, "subs", subs);
    return s;
}

} // namespace numkit
