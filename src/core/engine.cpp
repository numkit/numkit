// src/engine.cpp
#include <keel/array.h>
#include <keel/core.h>
#include <keel/env.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/small_array.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <cstdio>
#include <numkit/core/engine.hpp>
#include <numkit/core/figure_manager.hpp>
#include <numkit/core/caching_allocator.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/lexer.hpp>
#include <numkit/core/parser.hpp>
// NOTE: core is library-agnostic — it includes ZERO <nk/<lib>/library.hpp>.
// The standard library is installed by bundle/ (installStandardLibrary), not
// by the Engine ctor. This is what keeps core free of any toolbox dependency.
#include <numkit/core/tree_walker.hpp>
#include <numkit/core/vm.hpp>
#include <numkit/core/plugin_loader.hpp>
#include <numkit/sys/env.hpp>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace numkit {

// ============================================================
// Construction
// ============================================================
// No explicit allocator → recycle large transient buffers through a
// process-wide cache (see caching_allocator.hpp). Embedders that supply
// their own allocator are used as-is and never wrapped.
Engine::Engine() : Engine(numkit::defaultCachedAllocator()) {}

Engine::Engine(keel::Allocator *mr)
    : mr_(mr ? mr : numkit::defaultCachedAllocator())
{
    globalsEnv_ = keel::make_arc<Environment>();
    constantsEnv_ = keel::make_unique<Environment>(nullptr, globalsEnv_.get());
    workspaceEnv_ = keel::make_unique<Environment>(constantsEnv_.get(), globalsEnv_.get());
    figureManager_ = keel::make_unique<FigureManager>();
    figureManager_->setOutputFunc([this](keel::StringRef s) { this->outputText(s); });
    treeWalker_ = keel::make_unique<TreeWalker>(*this);
    compiler_ = keel::make_unique<Compiler>(*this);
    vm_ = keel::make_unique<VM>(*this);
    vm_->setCompiledFuncs(&compiler_->compiledFuncs(),
                          &compiler_->scriptLocalCompiledFuncs());
    pluginLoader_ = keel::make_unique<PluginLoader>(*this);

    reinstallConstants();
    registerVirtualFS(keel::make_unique<NativeFS>());

    // Element-wise fusion default: on unless NUMKIT_FUSE=0 (kill switch). No
    // effect until the standard library registers rules (see fusionEnabled()).
    if (auto fe = keel::env_get("NUMKIT_FUSE"))
        fusionEnabled_ = (fe->as_ref() != "0");

    keel::String nm = numkit::envGet("NUMKIT_MODE");
    if (nm.empty())
        nm = numkit::envGet("NUMKIT_NAMESPACE_MODE");
    if (nm.as_ref() == "isolated" || nm.as_ref() == "namespaced" || nm.as_ref() == "strict")
        namespaceMode_ = NamespaceMode::Isolated;
    // No library installs here — a bare Engine has the language runtime,
    // constants and primitive arithmetic, but no named functions. Call
    // numkit::installStandardLibrary(engine) to load the full set.
}

Engine::~Engine()
{
    // Flush any files the user left open — best-effort, swallow any
    // backend errors because we're already tearing down.
    closeAllFiles();
    vm_.reset();
    compiler_.reset();
    treeWalker_.reset();
    debugController_.reset();
    debugObserver_.reset();
    workspaceEnv_.reset();
    constantsEnv_.reset();
    globalsEnv_.reset();
    userConstants_.clear();
    scriptLocalUserFuncs_.clear();
    savedScriptLocalUserFuncs_.clear();
    userFuncs_.clear();
    classDefAst_.clear();
    classDefs_.clear();
    classes_.clear();
    callbackBuiltins_.clear();
    fusionRules_.clear();
    binaryOps_.clear();
    unaryOps_.clear();
    externalFuncs_.clear();
    shortNameIndex_.clear();
    pluginLoader_.reset();
    figureManager_.reset();
}

void Engine::reinstallConstants()
{
    constantsEnv_->set("pi", Value::scalar(3.14159265358979323846, mr_));
    // `nan` / `NaN` / `inf` / `Inf` are MATLAB built-in functions, not
    // constants. Bare `nan` calls nan() → scalar NaN; `nan(M, N)` calls
    // nan(M, N) → MxN matrix of NaN; `nan(M, N, 'single')` returns
    // single-precision. Registration lives in toolboxes/builtin/src/library.cpp
    // via nan_reg / inf_reg. Same pattern as true/false (BUGS.md #30).
    // `true` and `false` are MATLAB built-in functions, not constants.
    // Bare `true` calls true() → scalar logical 1; `true(M, N)` calls
    // true(M, N) → MxN logical array. Registration lives in
    // toolboxes/builtin/src/library.cpp via true_reg / false_reg. See
    // BUGS.md #30.
    constantsEnv_->set("i", Value::complexScalar(0.0, 1.0, mr_));
    constantsEnv_->set("j", Value::complexScalar(0.0, 1.0, mr_));

    // Re-install host-registered constants so they survive `clear all`.
    for (auto [name, val] : userConstants_)
        constantsEnv_->set(name, val);
}

void Engine::registerConstant(keel::StringRef name, Value val)
{
    userConstants_.put(keel::String::from(name), val);
    constantsEnv_->set(name, keel::move(val));
}

bool Engine::isReservedName(keel::StringRef name) const
{
    return isBuiltinName(name) || userConstants_.contains(name);
}

// ============================================================
// Registration & accessors
// ============================================================
void Engine::registerBinaryOp(keel::StringRef op, BinaryOpFunc func)
{
    binaryOps_.put(keel::String::from(op), keel::move(func));
}
void Engine::registerUnaryOp(keel::StringRef op, UnaryOpFunc func)
{
    unaryOps_.put(keel::String::from(op), keel::move(func));
}
// Internal helper — implements the actual registration logic shared
// by both registerFunction overloads. Maintains the auxiliary indices
// (shortNameIndex_, namespaceOrder_) and enforces uniqueness of the
// full name in the primary externalFuncs_ map.
void Engine::registerFunctionImpl_(keel::StringRef fullName,
                                   keel::StringRef leafName,
                                   ExternalFunc func)
{
    if (externalFuncs_.contains(fullName)) {
        // Common cause for `compat.<name>` collisions: a stale `noop`
        // placeholder was left in a library's install() while the real
        // implementation was registered elsewhere. Each `reg(<sub>,
        // <name>, …)` helper auto-registers `compat.<name>` once, so
        // two such calls with the same `<name>` (even from different
        // sub-namespaces) crash here. Hint the user where to look.
        keel::String hint;
        if (fullName.starts_with("compat.")) {
            hint = keel::format(
                " (look for a stale `reg(<sub>, \"{}\", noop)` or two real implementations sharing this"
                " name across sub-namespaces)",
                leafName);
        }
        auto msg = keel::format("duplicate function registration: {}{}", fullName, hint);
        throw std::runtime_error(msg.c_str());
    }
    externalFuncs_.put(keel::String::from(fullName), keel::move(func));
    shortNameIndex_.get_or_insert(keel::String::from(leafName),
                                  keel::SmallArray<keel::String, 2>{})
        .push_back(keel::String::from(fullName));
    bumpCallCacheEpoch();

    // Track top-level namespace (everything before the first '.').
    // Core registrations (no '.') do not introduce a namespace.
    const keel::usize dot = fullName.find('.');
    if (dot != keel::kInvalidIndex) {
        keel::StringRef topNs = fullName.substr(0, dot);
        if (namespaceSet_.insert(keel::String::from(topNs))) {
            namespaceOrder_.push_back(keel::String::from(topNs));
        }
    }
}

void Engine::registerFunction(keel::StringRef name, ExternalFunc func)
{
    // 1-arg form: equivalent to namespace = "" (core). full name == leaf name.
    registerFunctionImpl_(name, name, keel::move(func));
}

void Engine::registerClass(BuiltinClass cls)
{
    if (classes_.contains(cls.name)) {
        auto msg = keel::format("Engine::registerClass: duplicate class '{}'", cls.name);
        throw std::runtime_error(msg.c_str());
    }
    keel::String key = cls.name.clone();
    classes_.put(keel::move(key), keel::move(cls));
    bumpCallCacheEpoch();
}

const BuiltinClass *Engine::findClass(keel::StringRef name) const
{
    return classes_.get(name);
}

void Engine::registerCallbackBuiltin(keel::StringRef name, keel::Arc<CallbackBuiltin> cb)
{
    callbackBuiltins_.put(keel::String::from(name), keel::move(cb));
    bumpCallCacheEpoch();
}

CallbackBuiltin *Engine::callbackBuiltin(keel::StringRef name) const
{
    const auto *p = callbackBuiltins_.get(name);
    return p ? p->get() : nullptr;
}

bool Engine::isUserCodeHandle(const Value &handle) const
{
    if (!handle.isFuncHandle())
        return false;
    keel::String fn = handle.funcHandleName();
    return lookupUserFunctionLocal(fn) != nullptr
           || (compiler_ && compiler_->findCompiled(fn) != nullptr);
}

keel::String Engine::formatObjectDisplay(keel::StringRef name, const Value &obj) const
{
    keel::String clsName = obj.objectClassName();
    const BuiltinClass *cls = findClass(clsName);
    keel::String body;
    const keel::usize n = obj.objectCount();
    if (cls && cls->dispText) {
        body = cls->dispText(obj);
    }
    if (body.empty()) {
        if (n == 1) {
            body = keel::format("  {}\n", clsName);
        } else {
            // Object array: "<rows>×<cols> <ClassName> array" + property list.
            const Dims &d = obj.dims();
            keel::String hdr = keel::format("  {}\xC3\x97{} {} array", d.rows(), d.cols(), clsName);
            if (cls && !cls->propNames.empty()) {
                hdr.append(" with properties:\n\n");
                for (const auto &p : cls->propNames)
                    keel::format_into(hdr, "    {}\n", p);
            } else {
                hdr.push_back('\n');
            }
            body = keel::move(hdr);
        }
    }
    if (name.empty())
        return body;
    return keel::format("{} =\n\n{}\n", name, body);
}

void Engine::displayObject(keel::StringRef name, const Value &obj)
{
    const BuiltinClass *cls = findClass(obj.objectClassName());
    // Custom disp/display only for a scalar object; arrays use the default.
    if (cls && obj.objectCount() == 1) {
        auto call = [&](const ObjectMethod &m) {
            Value self = obj;
            Value outBuf[1];
            CallContext ctx{this, workspaceEnv_.get()};
            m(self, keel::ArrayRef<Value>(nullptr, 0), /*nargout=*/0, keel::MutArrayRef<Value>(outBuf, 1), ctx);
        };
        // `display` owns the entire output (including any variable-name line).
        if (const auto *m = cls->methods.get("display")) {
            call(*m);
            return;
        }
        // `disp` owns the body; the default `display` prints the `name =`
        // header around it (only when shown as a named variable).
        if (const auto *m = cls->methods.get("disp")) {
            if (!name.empty())
                outputText(keel::format("{} =\n\n", name));
            call(*m);
            if (!name.empty())
                outputText("\n");
            return;
        }
    }
    outputText(formatObjectDisplay(name, obj));
}

// MATLAB operator-overload methods — the single source for the source-token
// ⇆ method-name mapping. The same token can be both binary and unary
// (`-` → minus / uminus), so each row is arity-tagged and lookups
// disambiguate. operatorMethodName / unaryOperatorMethodName /
// isOperatorMethodName are thin views over this table.
struct OpMethod
{
    const char *token;
    const char *method;
    bool unary;
};
static constexpr OpMethod kOpMethods[] = {
    {"+", "plus", false},      {"-", "minus", false},     {"*", "mtimes", false},
    {".*", "times", false},    {"/", "mrdivide", false},  {"./", "rdivide", false},
    {"\\", "mldivide", false}, {".\\", "ldivide", false}, {"^", "mpower", false},
    {".^", "power", false},    {"==", "eq", false},       {"~=", "ne", false},
    {"<", "lt", false},        {"<=", "le", false},       {">", "gt", false},
    {">=", "ge", false},       {"&", "and", false},       {"|", "or", false},
    {"-", "uminus", true},     {"+", "uplus", true},      {"~", "not", true},
    {"'", "ctranspose", true}, {".'", "transpose", true},
};

static const char *operatorMethodName(keel::StringRef op)
{
    for (const auto &e : kOpMethods)
        if (!e.unary && op == e.token)
            return e.method;
    return nullptr;
}
static const char *unaryOperatorMethodName(keel::StringRef op)
{
    for (const auto &e : kOpMethods)
        if (e.unary && op == e.token)
            return e.method;
    return nullptr;
}
// True when a classdef method name is an operator-overload method (wired into
// BuiltinClass::ops so `a + b`, `-a`, `a == b`, … dispatch to it).
static bool isOperatorMethodName(keel::StringRef n)
{
    for (const auto &e : kOpMethods)
        if (n == e.method)
            return true;
    return false;
}

bool Engine::tryObjectBinaryOp(keel::StringRef op, const Value &lhs, const Value &rhs,
                               Environment *env, Value &out)
{
    if (!lhs.isObject() && !rhs.isObject())
        return false;
    // The dominant object decides the class (first object operand wins —
    // v1 dispatch fidelity, see object_model.md §3).
    const Value &dom = lhs.isObject() ? lhs : rhs;
    keel::String clsName = dom.objectClassName();
    const BuiltinClass *cls = findClass(clsName);
    if (const char *mname = operatorMethodName(op); cls && mname) {
        if (const auto *m = cls->ops.get(mname)) {
            Value self = dom;                 // class context for the hook
            Value operands[2] = {lhs, rhs};   // args in source order
            Value res[1];
            CallContext ctx{this, env};
            (*m)(self, keel::ArrayRef<Value>(operands, 2), 1, keel::MutArrayRef<Value>(res, 1), ctx);
            out = keel::move(res[0]);
            return true;
        }
    }
    auto msg = keel::format("Undefined operator '{}' for input arguments of type '{}'.", op, clsName);
    throw std::runtime_error(msg.c_str());
}

bool Engine::tryObjectUnaryOp(keel::StringRef op, const Value &operand,
                              Environment *env, Value &out)
{
    if (!operand.isObject())
        return false;
    keel::String clsName = operand.objectClassName();
    const BuiltinClass *cls = findClass(clsName);
    if (const char *mname = unaryOperatorMethodName(op); cls && mname) {
        if (const auto *m = cls->ops.get(mname)) {
            Value self = operand;
            Value res[1];
            CallContext ctx{this, env};
            (*m)(self, keel::ArrayRef<Value>(nullptr, 0), 1, keel::MutArrayRef<Value>(res, 1), ctx);
            out = keel::move(res[0]);
            return true;
        }
    }
    // Transpose has a builtin array meaning for any object array (reorder
    // elements), so with no class override fall through to the registered
    // builtin transpose instead of erroring. Other unary operators
    // (uminus/uplus/not) are overload-only → undefined without one.
    if (op == "'" || op == ".'")
        return false;
    auto msg = keel::format("Undefined operator '{}' for input arguments of type '{}'.", op, clsName);
    throw std::runtime_error(msg.c_str());
}

Value Engine::applyBinaryOp(keel::StringRef op, const Value &lhs, const Value &rhs)
{
    // Object overloading first (env=nullptr: an embedding-initiated call has no
    // caller environment; a numeric operand short-circuits
    // tryObjectBinaryOp before env is ever read).
    if (lhs.isObject() || rhs.isObject()) {
        Value out;
        if (tryObjectBinaryOp(op, lhs, rhs, /*env=*/nullptr, out))
            return out;
    }
    if (const auto *fn = binaryOps_.get(op))
        return (*fn)(lhs, rhs);
    auto msg = keel::format("Undefined binary operator: {}", op);
    throw std::runtime_error(msg.c_str());
}

Value Engine::applyUnaryOp(keel::StringRef op, const Value &operand)
{
    if (operand.isObject()) {
        Value out;
        if (tryObjectUnaryOp(op, operand, /*env=*/nullptr, out))
            return out;  // (returns false for ' / .': fall through to builtin transpose)
    }
    if (const auto *fn = unaryOps_.get(op))
        return (*fn)(operand);
    auto msg = keel::format("Undefined unary operator: {}", op);
    throw std::runtime_error(msg.c_str());
}

const BytecodeChunk *Engine::resolveBinaryOpChunk(keel::StringRef op, const Value &lhs,
                                                  const Value &rhs, keel::String &ownerClassOut)
{
    if (!lhs.isObject() && !rhs.isObject())
        return nullptr; // numeric fast path
    const Value &dom = lhs.isObject() ? lhs : rhs; // first object operand wins
    keel::String cn = dom.objectClassName();
    const BuiltinClass *cls = findClass(cn);
    const char *mname = operatorMethodName(op);
    if (!cls || !mname)
        return nullptr;
    // methodFns holds user-defined operator methods (real UserFunctions);
    // synthetic enum eq/ne live only in `ops` (no UserFunction) → not found
    // here → caller's slow path handles them. Same membership as `ops` for
    // genuine overloads.
    const auto *mfp = cls->methodFns.get(mname);
    if (!mfp)
        return nullptr;
    const BytecodeChunk *cc = ensureClassMethodChunk(**mfp);
    if (!cc)
        return nullptr; // uncompilable body → slow path (callReentrant / TW)
    enforceMethodAccess(cn, mname);
    ownerClassOut.assign(cn);
    return cc;
}

const BytecodeChunk *Engine::resolveUnaryOpChunk(keel::StringRef op, const Value &operand,
                                                 keel::String &ownerClassOut)
{
    if (!operand.isObject())
        return nullptr;
    keel::String cn = operand.objectClassName();
    const BuiltinClass *cls = findClass(cn);
    const char *mname = unaryOperatorMethodName(op);
    if (!cls || !mname)
        return nullptr;
    const auto *mfp = cls->methodFns.get(mname);
    if (!mfp)
        return nullptr; // no override → slow path (transpose→builtin, else throw)
    const BytecodeChunk *cc = ensureClassMethodChunk(**mfp);
    if (!cc)
        return nullptr;
    enforceMethodAccess(cn, mname);
    ownerClassOut.assign(cn);
    return cc;
}

// Defined below; forward-declared so the subsref/subsasgn resolvers (which run
// before its definition) can marshal the substruct argument.
static Value buildSubsStruct(const char *type, keel::ArrayRef<Value> subscripts,
                             keel::Allocator *mr);

const BytecodeChunk *Engine::resolveSubsrefChunk(const Value &self, keel::ArrayRef<Value> idx,
                                                 keel::String &ownerClassOut,
                                                 keel::Array<Value> &argsOut)
{
    keel::String cn = self.objectClassName();
    const BuiltinClass *cls = findClass(cn);
    if (!cls || !cls->subsref)
        return nullptr;
    const auto *mfp = cls->methodFns.get("subsref");
    if (!mfp)
        return nullptr; // builtin/native subsref (no UserFunction) → slow path
    const BytecodeChunk *cc = ensureClassMethodChunk(**mfp);
    if (!cc)
        return nullptr;
    Value s = buildSubsStruct("()", idx, allocator());
    argsOut.clear();
    argsOut.reserve(2);
    argsOut.push_back(self);          // subsref(obj, S)
    argsOut.push_back(keel::move(s));
    ownerClassOut.assign(cn);
    return cc;
}

const BytecodeChunk *Engine::resolveSubsasgnChunk(const Value &self, keel::ArrayRef<Value> idxAndVal,
                                                  keel::String &ownerClassOut,
                                                  keel::Array<Value> &argsOut)
{
    keel::String cn = self.objectClassName();
    const BuiltinClass *cls = findClass(cn);
    if (!cls || !cls->subsasgn)
        return nullptr;
    const auto *mfp = cls->methodFns.get("subsasgn");
    if (!mfp)
        return nullptr;
    const BytecodeChunk *cc = ensureClassMethodChunk(**mfp);
    if (!cc)
        return nullptr;
    // Hook convention: idxAndVal = [subscripts…, value] (value last).
    const keel::usize nsub = idxAndVal.empty() ? 0 : idxAndVal.size() - 1;
    Value s = buildSubsStruct("()", keel::ArrayRef<Value>(idxAndVal.data(), nsub), allocator());
    argsOut.clear();
    argsOut.reserve(3);
    argsOut.push_back(self);          // subsasgn(obj, S, val)
    argsOut.push_back(keel::move(s));
    argsOut.push_back(nsub < idxAndVal.size() ? idxAndVal[nsub] : Value());
    ownerClassOut.assign(cn);
    return cc;
}

bool Engine::tryObjectSubsref(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                              Value &out, Environment *env)
{
    const BuiltinClass *cls = findClass(self.objectClassName());
    if (!cls || !cls->subsref)
        return false;
    Value res[1];
    CallContext ctx{this, env};
    cls->subsref(self, args, nargout, keel::MutArrayRef<Value>(res, 1), ctx);
    out = keel::move(res[0]);
    return true;
}

void Engine::objectStoreSlice(Value &dst, keel::ArrayRef<keel::Array<keel::usize>> perDim,
                              const Value &val, Environment *env)
{
    keel::String clsName = dst.isObject() ? dst.objectClassName() : val.objectClassName();
    const BuiltinClass *cls = findClass(clsName);
    Value coercedVal = val;
    if (dst.isObject() && !val.isObject() && cls && cls->construct) {
        const ObjectState *st = dst.objectStateConst();
        if (st && st->native && st->native->fromScalarDouble(0.0) != nullptr) {
            CallContext ctx{this, env};
            Value argBuf[1] = {val};
            coercedVal = cls->construct(keel::ArrayRef<Value>(argBuf, 1), ctx);
        }
    }
    Value fill; // default element for grown gaps (class no-arg constructor)
    if (cls && cls->construct) {
        CallContext ctx{this, env};
        fill = cls->construct(keel::ArrayRef<Value>(nullptr, 0), ctx);
    }
    if (perDim.size() == 1)
        dst.objectAssignLinear(perDim[0], coercedVal, fill, allocator());
    else
        dst.objectAssignND(perDim, coercedVal, fill, allocator());
}

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

// Translate an attribute keyword to an Access level. `Immutable` is only
// meaningful for SetAccess. Friend-class access (`Access = ?Foo` / a `{?A,?B}`
// list) is NOT a silent downgrade here — the `?` is rejected at lex time, so
// such a class fails to load loudly and never reaches this function. The
// default below therefore only covers `public` and any access keyword we don't
// yet model, which fall back to Public (permissive, forward-compatible).
static Access parseAccessLevel(keel::StringRef v)
{
    if (v == "private")
        return Access::Private;
    if (v == "protected")
        return Access::Protected;
    if (v == "immutable")
        return Access::Immutable;
    return Access::Public; // "public" + any not-yet-modeled keyword
}

// Inverse of parseAccessLevel — the MATLAB attribute string for reflection
// (metaclass / meta.property / meta.method GetAccess/SetAccess/Access).
static const char *accessName(Access a)
{
    switch (a) {
    case Access::Private:   return "private";
    case Access::Protected: return "protected";
    case Access::Immutable: return "immutable";
    default:                return "public";
    }
}

// Resolve a properties/methods block's flattened attribute tokens
// (e.g. {"Access","private","SetAccess","immutable"}) into get/set levels.
// `Access` sets the default for both sides; `GetAccess` / `SetAccess`
// override their side. Returns false in `any` when every side stays public.
struct BlockAccess
{
    Access get = Access::Public;
    Access set = Access::Public;
    bool any = false;
};
static BlockAccess parseBlockAccess(const keel::Array<keel::String> &attrs)
{
    BlockAccess ba;
    auto valueOf = [&](const char *key) -> const keel::String * {
        for (keel::usize i = 0; i + 1 < attrs.size(); ++i)
            if (attrs[i] == key)
                return &attrs[i + 1];
        return nullptr;
    };
    // `Access` is the default for both sides; the specific `GetAccess` /
    // `SetAccess` always override it (MATLAB is order-independent here, so
    // resolve the general first, then the specific).
    if (const keel::String *a = valueOf("Access")) {
        Access lvl = parseAccessLevel(*a);
        ba.get = (lvl == Access::Immutable) ? Access::Public : lvl; // immutable is set-only
        ba.set = lvl;
    }
    if (const keel::String *g = valueOf("GetAccess")) {
        Access lvl = parseAccessLevel(*g);
        ba.get = (lvl == Access::Immutable) ? Access::Public : lvl;
    }
    if (const keel::String *s = valueOf("SetAccess"))
        ba.set = parseAccessLevel(*s);
    ba.any = ba.get != Access::Public || ba.set != Access::Public;
    return ba;
}

static const char *accessWord(Access a)
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

// Throw a MATLAB-style error when the current execution context may not
// touch a member with the given access level. `kind` is "property" or
// "method". Public members and allowed contexts return silently.
static void enforceAccess(Engine *engine, Access level, keel::StringRef declClass,
                          const char *kind, keel::StringRef name)
{
    if (level == Access::Public)
        return;
    if (level == Access::Immutable) {
        // Settable only inside the declaring class's own constructor.
        if (engine->classCtxInCtorOf(declClass))
            return;
        auto msg = keel::format(
            "Cannot set property '{}': it has immutable SetAccess (settable only in the {} constructor)",
            name, declClass);
        throw std::runtime_error(msg.c_str());
    }
    if (engine->classCtxAllows(declClass, /*privateOnly=*/level == Access::Private))
        return;
    auto msg = keel::format("Cannot access {} '{}' of class '{}': it has {} access",
                            kind, name, declClass, accessWord(level));
    throw std::runtime_error(msg.c_str());
}

// Find a property by name in a class descriptor (linear; property counts are
// small, and access checks only reach here when the class has a non-public
// property). Returns nullptr if absent.
static const PropInfo *findProp(const ClassDefDesc &d, keel::StringRef name)
{
    for (const auto &p : d.props)
        if (p.name == name)
            return &p;
    return nullptr;
}

// Build the MATLAB subscript struct `S` passed to a user subsref/subsasgn
// method: a 1×1 struct with `.type` (e.g. "()") and `.subs` (a 1×N cell of
// the subscripts). Our object index hooks only fire for paren-indexing, so
// the type is always "()".
static Value buildSubsStruct(const char *type, keel::ArrayRef<Value> subscripts,
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

void Engine::unregisterClassDef(keel::StringRef name)
{
    if (const auto *dp = classDefs_.get(name)) {
        const ClassDefDesc &desc = **dp;
        // Remove the `Name.member` qualified externals registered for Static
        // methods and Constant properties. registerFunctionImpl_ throws on a
        // duplicate full name, so these MUST be cleared before re-registration.
        auto dropExternal = [&](keel::StringRef leaf) {
            keel::String full = keel::format("{}.{}", name, leaf);
            externalFuncs_.remove(full);
            if (auto *vec = shortNameIndex_.get(leaf)) {
                for (keel::usize i = vec->size(); i > 0; --i) {
                    if ((*vec)[i - 1] == full)
                        vec->remove_at(i - 1);
                }
                if (vec->empty())
                    shortNameIndex_.remove(leaf);
            }
        };
        for (auto [sname, si] : desc.statics) {
            (void)si;
            dropExternal(sname);
        }
        for (const auto &p : desc.props)
            if (p.isConstant)
                dropExternal(p.name);
        classDefs_.remove(name);
    }
    // Drop compiled method chunks ("Name>method") so rewritten bodies recompile
    // instead of serving the stale cache.
    if (compiler_)
        compiler_->eraseCompiledFuncsForClass(name);
    clearPersistentForFunc(name);
    // Source AST kept for derived-class re-merge — drop it too (the cascade in
    // reregisterDerivedClasses holds its own Arc across this erase).
    classDefAst_.remove(name);
    // Drop the registry class last: registerClass throws on a duplicate, so the
    // re-registration that follows depends on this entry being gone.
    classes_.remove(name);
    bumpCallCacheEpoch();
}

void Engine::dropFileClassCtorExternal_(keel::StringRef name)
{
    // resolveMFile_ registers a bare constructor external for a class loaded
    // from `Name.m`. Guard on mFileCache_ membership: only a file-loaded class
    // has this external, so we never erase a core builtin that merely shares a
    // name with an inline classdef.
    if (!mFileCache_.contains(name))
        return;
    externalFuncs_.remove(name);
    if (auto *vec = shortNameIndex_.get(name)) {
        for (keel::usize i = vec->size(); i > 0; --i) {
            if ((*vec)[i - 1] == name)
                vec->remove_at(i - 1);
        }
        if (vec->empty())
            shortNameIndex_.remove(name);
    }
    bumpCallCacheEpoch();
}

void Engine::clearClassDefs()
{
    // Collect names first — unregisterClassDef mutates classDefs_ as it goes.
    // Built-in classes live in classes_ but NOT classDefs_, so iterating
    // classDefs_ touches only user classes.
    keel::Array<keel::String> names;
    names.reserve(classDefs_.size());
    for (auto [k, v] : classDefs_) {
        (void)v;
        names.push_back(k.clone());
    }
    for (const auto &name : names) {
        dropFileClassCtorExternal_(name); // enable file-class reload on next ref
        unregisterClassDef(name);
    }
    bumpCallCacheEpoch();
}

void Engine::registerClassDef(const ASTNode *cd, keel::StringRef qualifiedName)
{
    // Inline classdef (script statement / eval'd source): the defining file
    // is whatever is executing now. Known-but-empty at the command window.
    UserFunction prov;
    captureScriptOrigin(prov);
    registerClassDefImpl_(cd, qualifiedName, prov.originKnown,
                          prov.sourceFs, prov.sourceDir, prov.sourceFile);
}

void Engine::registerClassDefFrom(const ASTNode *cd, keel::StringRef qualifiedName,
                                  keel::StringRef fsName, keel::StringRef scriptDir,
                                  keel::StringRef scriptFile)
{
    registerClassDefImpl_(cd, qualifiedName, /*originKnown=*/true,
                          fsName, scriptDir, scriptFile);
}

void Engine::registerClassDefImpl_(const ASTNode *cd, keel::StringRef qualifiedName,
                                   bool originKnown, keel::StringRef fsName,
                                   keel::StringRef scriptDir, keel::StringRef scriptFile)
{
    if (!cd || cd->type != NodeType::CLASSDEF_DEF || cd->strValue.empty())
        return;
    // Registry identity: the qualified name for a `+pkg/Name.m` class, else the
    // node's own (leaf == full for inline / unpackaged classes).
    const keel::String className = qualifiedName.empty()
                                       ? cd->strValue.clone()
                                       : keel::String::from(qualifiedName);
    // The in-source constructor / accessor names are always the LEAF (`Vec`,
    // never `geo.Vec`), so ctor detection compares against the leaf even though
    // the class is registered under its qualified name.
    keel::StringRef classLeaf = className.as_ref();
    if (const keel::usize dot = classLeaf.rfind('.'); dot != keel::kInvalidIndex)
        classLeaf = classLeaf.substr(dot + 1);

    const bool wasRedefinition = findClass(className) != nullptr;
    if (wasRedefinition)
        // Redefinition (REPL / IDE re-run of `classdef Name … end`): evict the
        // old class wholesale, then fall through to re-register. The previous
        // idempotent early-return silently ignored the new body — so rewriting
        // a method had no effect until a full restart.
        unregisterClassDef(className);

    auto desc = keel::make_arc<ClassDefDesc>();
    desc->name = className.clone();
    for (const auto &super : cd->paramNames)
        if (super == "handle")
            desc->isHandle = true;

    auto hasAttr = [](const ASTNode *n, const char *a) {
        for (const auto &s : n->classAttrs)
            if (s == a)
                return true;
        return false;
    };
    desc->isSealed = hasAttr(cd, "Sealed"); // class-level attribute
    for (const auto &childPtr : cd->children) {
        const ASTNode *child = childPtr.get();
        if (child->type == NodeType::CLASSDEF_ENUM_MEMBER) {
            desc->isEnum = true; // members are instantiated after the class is built
            continue;
        }
        if (child->type == NodeType::CLASSDEF_PROPERTY) {
            Value def = Value::Empty;
            if (!child->children.empty())
                def = evalConstExpr(child->children[0].get());
            // Record non-public get/set access (this class is the declarer).
            // A `Constant` property is also exposed as `ClassName.Prop`; the
            // qualified external is registered after the merge (so inherited
            // constants resolve under a subclass too).
            BlockAccess ba = parseBlockAccess(child->classAttrs);
            desc->props.push_back(PropInfo{
                child->strValue.clone(), def, ba.get, ba.set, desc->name.clone(),
                hasAttr(child, "Constant"), hasAttr(child, "Dependent")});
            desc->anyNonPublicProp = desc->anyNonPublicProp || ba.any;
            if (hasAttr(child, "Hidden"))
                desc->hiddenMembers.push_back(child->strValue.clone());
        } else if (child->type == NodeType::FUNCTION_DEF) {
            // Abstract method: a signature with no body (no children[0]).
            // Record the name; a concrete subclass implements it. The class
            // stays abstract (uninstantiable) until every such name is
            // implemented.
            if (hasAttr(child, "Abstract")) {
                desc->abstractMethods.push_back(child->strValue.clone());
                continue;
            }
            auto uf = keel::make_arc<UserFunction>();
            uf->name = keel::format("{}>{}", desc->name, child->strValue);
            uf->params = child->paramNames.clone();
            uf->returns = child->returnNames.clone();
            uf->body = keel::Arc<const ASTNode>(cloneNode(child->children[0].get()));
            uf->closureEnv = nullptr;
            uf->ownerClass = desc->name.clone(); // member-access execution context
            // mfilename() inside any method reports the CLASSDEF file (the
            // class name == file base). Inherited methods keep their
            // declaring class's provenance — the merge reuses these ufs.
            uf->originKnown = originKnown;
            uf->sourceFs = keel::String::from(fsName);
            uf->sourceDir = keel::String::from(scriptDir);
            uf->sourceFile = keel::String::from(scriptFile);
            const keel::String &mn = child->strValue;
            if (mn.starts_with("get.")) {
                desc->getters.put(keel::String::from(mn.substr(4)), uf); // property get accessor
            } else if (mn.starts_with("set.")) {
                desc->setters.put(keel::String::from(mn.substr(4)), uf); // property set accessor
            } else if (hasAttr(child, "Static")) {
                // Static method: callable as `ClassName.method(args)` (no
                // self). Stored now; the qualified external is registered
                // after the merge (so a subclass inherits it). `immutable` is
                // meaningless for a method.
                BlockAccess ba = parseBlockAccess(child->classAttrs);
                Access slvl = (ba.set == Access::Immutable) ? Access::Public : ba.set;
                desc->statics.put(child->strValue.clone(), StaticInfo{uf, slvl, desc->name.clone()});
            } else if (child->strValue == classLeaf) {
                desc->ctor = uf; // constructor: method named like the class (leaf)
                BlockAccess ba = parseBlockAccess(child->classAttrs);
                Access clvl = (ba.set == Access::Immutable) ? Access::Public : ba.set;
                if (clvl != Access::Public) {
                    desc->ctorAccess = clvl;
                    desc->ctorDeclClass = desc->name.clone();
                }
            } else {
                desc->methods.put(child->strValue.clone(), uf);
                // Record non-public method access (Access sets the level;
                // immutable is meaningless for a method → treat as public).
                BlockAccess ba = parseBlockAccess(child->classAttrs);
                Access lvl = (ba.set == Access::Immutable) ? Access::Public : ba.set;
                if (lvl != Access::Public)
                    desc->methodAccess.put(child->strValue.clone(), MethodAccess{lvl, desc->name.clone()});
                if (hasAttr(child, "Hidden"))
                    desc->hiddenMembers.push_back(child->strValue.clone());
            }
        }
    }

    // Method names declared by THIS class, captured before the merge mutates
    // the descriptor — so an inherited method-access entry never overrides a
    // method the derived class declares itself (a public override of a
    // protected base method stays public). Properties need no such capture:
    // their access travels inside PropInfo, and a derived property fully
    // replaces the inherited entry in the merge below.
    keel::Array<keel::String> ownMethodNames;
    ownMethodNames.reserve(desc->methods.size());
    for (auto [mn, uf] : desc->methods) {
        (void)uf;
        ownMethodNames.push_back(mn.clone());
    }

    // Make inheritance order-independent: if a superclass is a file-based
    // class not yet loaded, pull it in from the path now (it registers as a
    // side effect). Without this, `Derived(...)` referenced before `Base`
    // would register Derived with the base members missing. Inline classes
    // that aren't on the path simply stay unregistered (recorded for isa).
    for (const auto &superName : cd->paramNames)
        if (superName != "handle" && !classDefs_.contains(superName))
            resolveMFile_(superName);

    // ── Inheritance: merge registered superclasses (base members first,
    // derived overrides). `handle` is the semantics marker, not a classdef.
    for (const auto &superName : cd->paramNames) {
        if (superName == "handle")
            continue;
        const auto *bp = classDefs_.get(superName);
        if (!bp)
            continue; // unknown / non-classdef base — skip (still recorded below)
        const auto &base = *bp;
        if (base->isSealed) {
            auto msg = keel::format("Cannot subclass sealed class '{}'", superName);
            throw std::runtime_error(msg.c_str());
        }
        desc->isHandle = desc->isHandle || base->isHandle;
        // Properties: base first; a derived property of the same name fully
        // replaces the inherited entry (default + access + declaring class),
        // so a public override of a protected base property stays public.
        keel::Array<PropInfo> merged = base->props.clone();
        for (auto &dp : desc->props) {
            auto it = std::find_if(merged.begin(), merged.end(),
                                   [&](const PropInfo &m) { return m.name == dp.name; });
            if (it != merged.end())
                *it = keel::move(dp);
            else
                merged.push_back(keel::move(dp));
        }
        desc->props = keel::move(merged);
        desc->anyNonPublicProp = desc->anyNonPublicProp || base->anyNonPublicProp;
        // Methods + accessors: inherit those the derived class doesn't override.
        for (auto [mn, uf] : base->methods)
            if (!desc->methods.contains(mn))
                desc->methods.put(mn.clone(), uf);
        for (auto [pn, uf] : base->getters)
            if (!desc->getters.contains(pn))
                desc->getters.put(pn.clone(), uf);
        for (auto [pn, uf] : base->setters)
            if (!desc->setters.contains(pn))
                desc->setters.put(pn.clone(), uf);
        // Static methods: inherit those not overridden (keeping the base's
        // declaring class for the access check).
        for (auto [sn, si] : base->statics)
            if (!desc->statics.contains(sn))
                desc->statics.put(sn.clone(), si.clone());
        // Abstract method obligations propagate to the subclass (satisfied
        // when the subclass provides a concrete method of the same name).
        for (const auto &am : base->abstractMethods)
            if (std::find(desc->abstractMethods.begin(), desc->abstractMethods.end(), am)
                == desc->abstractMethods.end())
                desc->abstractMethods.push_back(am.clone());
        // Hidden members stay hidden in the subclass.
        for (const auto &hm : base->hiddenMembers)
            if (std::find(desc->hiddenMembers.begin(), desc->hiddenMembers.end(), hm)
                == desc->hiddenMembers.end())
                desc->hiddenMembers.push_back(hm.clone());
        // Method access: inherit base entries (keeping the base's declaring
        // class so `protected` still admits this subclass) unless the derived
        // class declared the method itself (its own access — or public by
        // omission — then wins).
        for (auto [mn, ma] : base->methodAccess)
            if (std::find(ownMethodNames.begin(), ownMethodNames.end(), mn)
                == ownMethodNames.end())
                desc->methodAccess.try_insert(mn.clone(), ma.clone());
        if (!desc->ctor) {
            // Inherit the base constructor — and its access, so a subclass
            // without its own ctor honours a private/protected base ctor
            // (the declaring class stays the base, for protected/subclass).
            desc->ctor = base->ctor;
            desc->ctorAccess = base->ctorAccess;
            desc->ctorDeclClass = base->ctorDeclClass.clone();
        }
    }
    // Ancestry (transitive) for isa(): direct supers + their ancestors.
    for (const auto &superName : cd->paramNames) {
        if (superName == "handle")
            continue;
        desc->superclasses.push_back(superName.clone());
        if (const auto *bp = classDefs_.get(superName))
            for (const auto &a : (*bp)->superclasses)
                if (std::find(desc->superclasses.begin(), desc->superclasses.end(), a)
                    == desc->superclasses.end())
                    desc->superclasses.push_back(a.clone());
    }
    // A class is abstract — uninstantiable — while any declared abstract
    // method has no concrete implementation (own or inherited).
    for (const auto &am : desc->abstractMethods)
        if (!desc->methods.contains(am)) {
            desc->isAbstract = true;
            break;
        }
    classDefs_.put(desc->name.clone(), desc);

    // Register `ClassName.member` qualified externals for every Static method
    // and Constant property — own AND inherited (the merged maps above hold
    // both) — so e.g. `Subclass.baseStatic()` / `Subclass.BASE_CONST` resolve.
    for (auto [sname, si] : desc->statics) {
        auto uf = si.uf;
        Access acc = si.access;
        keel::String decl = si.declClass.clone();
        keel::String mn = sname.clone();
        registerFunction(desc->name, sname,
                         [uf, acc, decl = keel::move(decl), mn = keel::move(mn)](
                             keel::ArrayRef<Value> args, keel::usize nargout,
                             keel::MutArrayRef<Value> outs, CallContext &ctx) {
                             if (acc != Access::Public)
                                 enforceAccess(ctx.engine, acc, decl, "method", mn);
                             const keel::usize nout = std::max<keel::usize>(nargout, 1);
                             auto results = ctx.engine->invokeClassMethod(*uf, args, nout);
                             const keel::usize writeN = std::min(nout, results.size());
                             for (keel::usize i = 0; i < writeN && i < outs.size(); ++i)
                                 outs[i] = keel::move(results[i]);
                         });
    }
    for (const auto &p : desc->props) {
        if (!p.isConstant)
            continue;
        auto cval = keel::make_arc<Value>(p.def);
        Access getLvl = p.getAccess;
        keel::String decl = p.declClass.clone();
        keel::String pn = p.name.clone();
        registerFunction(desc->name, p.name,
                         [cval, getLvl, decl = keel::move(decl), pn = keel::move(pn)](
                             keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                             CallContext &ctx) {
                             if (getLvl != Access::Public)
                                 enforceAccess(ctx.engine, getLvl, decl, "property", pn);
                             outs[0] = *cval;
                         });
    }

    BuiltinClass cls;
    cls.name = desc->name.clone();
    cls.isHandle = desc->isHandle;
    cls.hidden.reserve(desc->hiddenMembers.size());
    for (const auto &h : desc->hiddenMembers)
        cls.hidden.push_back(h.clone());
    cls.propNames.reserve(desc->props.size());
    for (const auto &p : desc->props)
        if (std::find(desc->hiddenMembers.begin(), desc->hiddenMembers.end(), p.name) == desc->hiddenMembers.end())
            cls.propNames.push_back(p.name.clone()); // Hidden props omitted from properties()/disp
    cls.superclasses.reserve(desc->superclasses.size());
    for (const auto &s : desc->superclasses)
        cls.superclasses.push_back(s.clone());
    cls.isSealed = desc->isSealed;
    cls.isAbstract = desc->isAbstract;
    // ── Reflection metadata (metaclass / meta.property / meta.method) ──
    // Mirrors the merged classdef member attributes into the runtime class so
    // introspection needn't reach back into ClassDefDesc. Hidden members are
    // omitted, matching properties()/methods().
    {
        auto isHidden = [&](keel::StringRef n) {
            for (const auto &h : desc->hiddenMembers)
                if (h == n)
                    return true;
            return false;
        };
        for (const auto &p : desc->props)
            if (!isHidden(p.name))
                cls.propMeta.push_back({p.name.clone(),
                                        keel::String::from(accessName(p.getAccess)),
                                        keel::String::from(accessName(p.setAccess)),
                                        p.isConstant, p.isDependent});
        struct TmpMethodMeta {
            const char *access = "public";
            bool isStatic = false;
            bool isAbstract = false;
        };
        keel::HashMap<keel::String, TmpMethodMeta> mm; // dedup; sorted below for deterministic order
        for (auto [name, uf] : desc->methods) {
            (void)uf;
            TmpMethodMeta m;
            if (const auto *ma = desc->methodAccess.get(name))
                m.access = accessName(ma->level);
            mm.put(name.clone(), m);
        }
        for (auto [name, si] : desc->statics) {
            TmpMethodMeta &m = mm.get_or_insert(name, TmpMethodMeta{});
            m.isStatic = true;
            m.access = accessName(si.access);
        }
        for (const auto &name : desc->abstractMethods) {
            TmpMethodMeta &m = mm.get_or_insert(name, TmpMethodMeta{});
            m.isAbstract = true;
        }
        for (auto [name, m] : mm)
            if (!isHidden(name)) {
                MethodMeta meta;
                meta.name = name.clone();
                meta.access = keel::String::from(m.access);
                meta.isStatic = m.isStatic;
                meta.isAbstract = m.isAbstract;
                cls.methodMeta.push_back(keel::move(meta));
            }
        std::sort(cls.methodMeta.begin(), cls.methodMeta.end(),
                  [](const MethodMeta &a, const MethodMeta &b) { return a.name < b.name; });
    }
    cls.propGet = [desc](const Value &self, keel::StringRef nameRef, Value &out,
                         CallContext &ctx) -> bool {
        if (desc->anyNonPublicProp)
            if (const PropInfo *pi = findProp(*desc, nameRef);
                pi && pi->getAccess != Access::Public)
                enforceAccess(ctx.engine, pi->getAccess, pi->declClass, "property", nameRef);
        if (const auto *gp = desc->getters.get(nameRef);
            gp && !ctx.engine->isPropAccessorActive((*gp)->name)) {
            struct AccessorGuard {
                Engine *eng;
                ~AccessorGuard() { eng->popPropAccessor(); }
            };
            ctx.engine->pushPropAccessor((*gp)->name);
            AccessorGuard guard{ctx.engine};
            auto r = ctx.engine->invokeClassMethod(**gp,
                                                   keel::ArrayRef<Value>(&self, 1), 1);
            out = r.empty() ? Value() : keel::move(r[0]);
            return true;
        }
        const auto *st = self.objectStateConst();
        if (!st)
            return false;
        const Value *vp = st->props.get(nameRef);
        if (!vp)
            return false;
        out = *vp;
        return true;
    };
    cls.propSet = [desc](Value &self, keel::StringRef nameRef, const Value &val,
                         CallContext &ctx) -> bool {
        const PropInfo *pi = findProp(*desc, nameRef);
        if (pi) {
            if (pi->isConstant) {
                auto msg = keel::format("Cannot set Constant property '{}' of class '{}'",
                                        nameRef, desc->name);
                throw std::runtime_error(msg.c_str());
            }
            if (pi->setAccess != Access::Public)
                enforceAccess(ctx.engine, pi->setAccess, pi->declClass, "property", nameRef);
        } else if (!desc->setters.contains(nameRef)
                   && std::find(desc->superclasses.begin(), desc->superclasses.end(),
                                "dynamicprops")
                          == desc->superclasses.end()) {
            auto msg = keel::format("No public property '{}' for class '{}'", nameRef, desc->name);
            throw std::runtime_error(msg.c_str());
        }
        if (const auto *sp = desc->setters.get(nameRef);
            sp && !ctx.engine->isPropAccessorActive((*sp)->name)) {
            struct AccessorGuard {
                Engine *eng;
                ~AccessorGuard() { eng->popPropAccessor(); }
            };
            ctx.engine->pushPropAccessor((*sp)->name);
            AccessorGuard guard{ctx.engine};
            Value callArgs[2] = {self, val};
            auto r = ctx.engine->invokeClassMethod(**sp,
                                                   keel::ArrayRef<Value>(callArgs, 2), 1);
            if (!r.empty() && r[0].isObject())
                self = keel::move(r[0]);
            return true;
        }
        self.objectStateMut()->props.put(keel::String::from(nameRef), val);
        return true;
    };
    cls.construct = [desc](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
        Value obj = ctx.engine->makeDefaultInstance(desc->name);
        if (desc->ctor)
            obj = ctx.engine->invokeClassCtor(*desc->ctor, obj, args);
        return obj;
    };
    for (auto [mname, uf] : desc->methods) {
        auto ufCopy = uf; // Arc keeps the body alive
        cls.methodFns.put(mname.clone(), keel::Arc<const UserFunction>(uf)); // classdef method → VM can run it as a frame
        Access mlevel = Access::Public;
        keel::StringRef mdeclRef;
        if (const auto *ma = desc->methodAccess.get(mname)) {
            mlevel = ma->level;
            mdeclRef = ma->declClass.as_ref();
        }
        cls.methods.put(mname.clone(),
                        [ufCopy, nameCopy = mname.clone(), mlevel,
                         mdecl = keel::String::from(mdeclRef)](
                            Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                            keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (mlevel != Access::Public)
                enforceAccess(ctx.engine, mlevel, mdecl, "method", nameCopy);
            keel::Array<Value> callArgs;
            callArgs.reserve(args.size() + 1);
            callArgs.push_back(self); // MATLAB: obj is the first parameter
            for (const auto &a : args)
                callArgs.push_back(a);
            const keel::usize nout = std::max<keel::usize>(nargout, 1);
            auto results = ctx.engine->invokeClassMethod(
                *ufCopy, keel::ArrayRef<Value>(callArgs.data(), callArgs.size()), nout);
            // Always produce at least one output (the `ans`/expression value
            // when called with nargout==0), bounded by the outs span.
            const keel::usize writeN = std::min(nout, results.size());
            for (keel::usize i = 0; i < writeN && i < outs.size(); ++i)
                outs[i] = keel::move(results[i]);
        });
        // Operator-overload method (plus/minus/eq/uminus/…): also wire it into
        // `ops` so `a + b`, `-a`, `a == b`, … dispatch here. Unlike a regular
        // method, an operator method's parameters ARE its operands (no `self`
        // is prepended): binary ops arrive as `args` = [lhs, rhs]; a unary op
        // arrives as the receiver `self` with empty `args`.
        if (isOperatorMethodName(mname)) {
            auto ufOp = uf;
            cls.ops.put(mname.clone(),
                        [ufOp, nameCopy = mname.clone(), mlevel,
                         mdecl = keel::String::from(mdeclRef)](
                            Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                            keel::MutArrayRef<Value> outs, CallContext &ctx) {
                if (mlevel != Access::Public)
                    enforceAccess(ctx.engine, mlevel, mdecl, "method", nameCopy);
                keel::Array<Value> callArgs;
                if (args.empty())
                    callArgs.push_back(self); // unary: operand came as self
                else
                    callArgs.append(args); // binary: [lhs, rhs]
                const keel::usize nout = std::max<keel::usize>(nargout, 1);
                auto results = ctx.engine->invokeClassMethod(
                    *ufOp, keel::ArrayRef<Value>(callArgs.data(), callArgs.size()), nout);
                const keel::usize writeN = std::min(nout, results.size());
                for (keel::usize i = 0; i < writeN && i < outs.size(); ++i)
                    outs[i] = keel::move(results[i]);
            });
        }
    }
    // Custom indexing: a `subsref` / `subsasgn` method overrides `obj(...)`
    // read / assignment. The user method takes MATLAB's substruct form
    // (subsref(obj, S) / subsasgn(obj, S, val)); our paren-index hooks pass
    // flat subscripts, which we wrap into S here. (`.`/`{}` keep their default
    // property / cell semantics — only `()` routes through these.)
    if (const auto *ufp = desc->methods.get("subsref")) {
        auto uf = *ufp;
        cls.subsref = [uf](Value &self, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                           CallContext &ctx) {
            Value s = buildSubsStruct("()", args, ctx.engine->allocator());
            Value callArgs[2] = {self, s};
            const keel::usize nout = std::max<keel::usize>(nargout, 1);
            auto results = ctx.engine->invokeClassMethod(*uf, keel::ArrayRef<Value>(callArgs, 2), nout);
            const keel::usize writeN = std::min(nout, results.size());
            for (keel::usize i = 0; i < writeN && i < outs.size(); ++i)
                outs[i] = keel::move(results[i]);
        };
    }
    if (const auto *ufp = desc->methods.get("subsasgn")) {
        auto uf = *ufp;
        cls.subsasgn = [uf](Value &self, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>,
                            CallContext &ctx) {
            // Hook convention: args = [subscripts…, value] (value last).
            const keel::usize nsub = args.empty() ? 0 : args.size() - 1;
            Value s = buildSubsStruct("()", keel::ArrayRef<Value>(args.data(), nsub),
                                      ctx.engine->allocator());
            Value callArgs[3] = {self, s, nsub < args.size() ? args[nsub] : Value()};
            auto results = ctx.engine->invokeClassMethod(*uf, keel::ArrayRef<Value>(callArgs, 3), 1);
            // Value class: subsasgn returns the modified object → write back.
            // Handle class: it mutates shared state in place (write-back of the
            // returned handle is harmless).
            if (!results.empty() && results[0].isObject())
                self = keel::move(results[0]);
        };
    }
    cls.dispText = [desc](const Value &self) -> keel::String {
        if (self.objectCount() != 1)
            return keel::String{};
        keel::String body = keel::format("  {} with properties:\n\n", desc->name);
        const auto *st = self.objectStateConst();
        for (const auto &p : desc->props) {
            keel::format_into(body, "    {}", p.name);
            if (st) {
                if (const Value *vp = st->props.get(p.name);
                    vp && vp->isScalar() && vp->type() == ValueType::DOUBLE) {
                    char numBuf[64];
                    int n = std::snprintf(numBuf, sizeof(numBuf), "%f", vp->toScalar());
                    if (n > 0) {
                        body.append(": ");
                        body.append(keel::StringRef(numBuf, static_cast<keel::usize>(n)));
                    }
                }
            }
            body.push_back('\n');
        }
        return body;
    };
    // Enumeration defaults: members compare by their member name (so
    // `Color.Red == Color.Red`), and display as that name — unless the class
    // defines its own eq / disp.
    if (desc->isEnum) {
        auto enumEq = [](Value &, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                         CallContext &ctx) {
            bool eq = false;
            if (args.size() == 2 && args[0].isObject() && args[1].isObject()
                && args[0].objectClassName() == args[1].objectClassName()) {
                const auto *a = args[0].objectStateConst();
                const auto *b = args[1].objectStateConst();
                eq = a && b && !a->enumName.empty() && a->enumName == b->enumName;
            }
            outs[0] = Value::logicalScalar(eq, ctx.engine->allocator());
        };
        if (!cls.ops.contains("eq"))
            cls.ops.put(keel::String::from("eq"), enumEq);
        if (!cls.ops.contains("ne"))
            cls.ops.put(keel::String::from("ne"),
                        [enumEq](Value &self, keel::ArrayRef<Value> args, keel::usize no,
                                 keel::MutArrayRef<Value> outs, CallContext &ctx) {
                            enumEq(self, args, no, outs, ctx);
                            if (!outs.empty())
                                outs[0] = Value::logicalScalar(!outs[0].toBool(), ctx.engine->allocator());
                        });
        if (!desc->methods.contains("disp") && !desc->methods.contains("display"))
            cls.dispText = [](const Value &self) -> keel::String {
                if (self.objectCount() != 1)
                    return keel::String{};
                const auto *st = self.objectStateConst();
                if (st && !st->enumName.empty())
                    return keel::format("    {}\n", st->enumName);
                return keel::String{};
            };
    }
    registerClass(keel::move(cls));

    // Instantiate enumeration members: each is a constant instance exposed as
    // `ClassName.Member`, built by the constructor with the member's args (so
    // an underlying value is set) and tagged with its member name.
    if (desc->isEnum) {
        const BuiltinClass *ecls = findClass(desc->name);
        if (ecls) {
            for (const auto &childPtr : cd->children) {
                const ASTNode *child = childPtr.get();
                if (child->type != NodeType::CLASSDEF_ENUM_MEMBER)
                    continue;
                keel::Array<Value> args;
                for (const auto &a : child->children)
                    args.push_back(evalConstExpr(a.get()));
                CallContext ctx{this, workspaceEnv_.get()};
                Value inst = ecls->construct(keel::ArrayRef<Value>(args.data(), args.size()), ctx);
                if (inst.isObject())
                    inst.objectStateMut()->enumName = child->strValue.clone();
                auto instPtr = keel::make_arc<Value>(keel::move(inst));
                registerFunction(desc->name, child->strValue,
                                 [instPtr](keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                                           CallContext &) { outs[0] = *instPtr; });
            }
        }
    }

    // Remember the source AST so a later base-class redefinition can re-merge
    // this class (see reregisterDerivedClasses). Cloned because `cd` may be
    // transient — resolveMFile_'s parsed AST is freed once this returns, and on
    // a redefinition `cd` may alias the very entry we overwrite here (the
    // cascade holds its own Arc to keep it alive across this clone).
    classDefAst_.put(className.clone(), keel::Arc<const ASTNode>(cloneNode(cd)));

    // Re-register every class that (transitively) derives from this one, so the
    // new members propagate to subclasses (which hold a snapshot of the base's
    // methods/props). Fires on EVERY registration, not just redefinitions: it
    // also back-fills a subclass that was defined BEFORE its base (forward
    // reference — the subclass recorded the base name but registered without its
    // members). No-op when nothing derives from `className`. Guarded so the
    // nested registerClassDef calls don't each re-cascade.
    if (!suppressDependentCascade_)
        reregisterDerivedClasses(className);
}

void Engine::reregisterDerivedClasses(keel::StringRef base)
{
    // Classes whose transitive superclasses include `base`.
    keel::Array<keel::String> deps;
    for (auto [name, desc] : classDefs_) {
        if (name == base)
            continue;
        if (std::find(desc->superclasses.begin(), desc->superclasses.end(), base)
            != desc->superclasses.end())
            deps.push_back(name.clone());
    }
    if (deps.empty())
        return;
    // Parent-first: a subclass's transitive-ancestor set strictly contains each
    // ancestor's, so ascending ancestor count is a valid topological order (a
    // base is re-registered before any class that derives from it).
    std::sort(deps.begin(), deps.end(), [&](const keel::String &a, const keel::String &b) {
        return (*classDefs_.get(a))->superclasses.size() < (*classDefs_.get(b))->superclasses.size();
    });
    // RAII restore: a re-register that throws must NOT leave the flag stuck at
    // true — that would silently disable propagation for the rest of the
    // session. The guard restores the previous value on every exit path.
    struct CascadeGuard
    {
        bool &flag;
        bool prev;
        explicit CascadeGuard(bool &f) : flag(f), prev(f) { flag = true; }
        ~CascadeGuard() { flag = prev; }
        CascadeGuard(const CascadeGuard &) = delete;
        CascadeGuard &operator=(const CascadeGuard &) = delete;
    } guard(suppressDependentCascade_);
    for (const auto &name : deps) {
        const auto *astPtr = classDefAst_.get(name);
        if (!astPtr || !*astPtr)
            continue;
        // Hold our own ref: registerClassDef → unregisterClassDef erases the map
        // entry, then re-stores a fresh clone; this keeps the node alive in
        // between. Re-register under the same (possibly qualified) name.
        keel::Arc<const ASTNode> ast = *astPtr;
        registerClassDef(ast.get(), name);
    }
}

namespace {
// Push the running class onto the access-context stack for the duration of a
// method/constructor body; pops on scope exit (incl. exceptions). The class
// is `uf.ownerClass`, set when the classdef body was registered.
struct ClassCtxGuard
{
    Engine *engine;
    ClassCtxGuard(Engine *e, keel::StringRef className, bool isCtor)
        : engine(e)
    {
        engine->pushClassCtx(className, isCtor);
    }
    ~ClassCtxGuard() { engine->popClassCtx(); }
    ClassCtxGuard(const ClassCtxGuard &) = delete;
    ClassCtxGuard &operator=(const ClassCtxGuard &) = delete;
};
} // namespace

keel::Array<Value> Engine::invokeClassMethod(const UserFunction &uf, keel::ArrayRef<Value> args,
                                             keel::usize nout)
{
    // Under the VM backend, run the body on the VM (P4) so C++-initiated
    // classdef callbacks — operator methods, subsref/subsasgn, custom
    // disp/display, super-method targets, and method calls made from a builtin —
    // execute on the same engine as the rest of the program. callReentrant
    // carries the class context on the frame (ownerClass), so no ClassCtxGuard
    // is needed. Falls back to the TreeWalker if the body can't VM-compile.
    if (backend_ == Backend::VM) {
        if (!vm_)
            throw std::runtime_error("classdef methods require an active VM");
        const BytecodeChunk *cc = ensureClassMethodChunk(uf);
        if (!cc) {
            auto msg = keel::format("Failed to compile classdef method '{}'", uf.name);
            throw std::runtime_error(msg.c_str());
        }
        return vm_->callReentrant(*cc, args, std::max<keel::usize>(nout, 1), uf.ownerClass,
                                  /*isCtor=*/false);
    }
    if (treeWalker_) {
        ClassCtxGuard ctx(this, uf.ownerClass, /*isCtor=*/false);
        return treeWalker_->runClassMethod(uf, args, nout);
    }
    throw std::runtime_error("classdef methods require the interpreter backend");
}

Value Engine::invokeClassCtor(const UserFunction &ctor, const Value &seed,
                              keel::ArrayRef<Value> args)
{
    if (backend_ == Backend::VM) {
        if (!vm_)
            throw std::runtime_error("classdef constructor requires an active VM");
        const BytecodeChunk *cc = ensureClassMethodChunk(ctor);
        if (!cc) {
            auto msg = keel::format("Failed to compile classdef constructor '{}'", ctor.name);
            throw std::runtime_error(msg.c_str());
        }
        auto r = vm_->callReentrant(*cc, args, 1, ctor.ownerClass, /*isCtor=*/true, &seed);
        return r.empty() ? seed : keel::move(r[0]);
    }
    if (treeWalker_) {
        ClassCtxGuard ctx(this, ctor.ownerClass, /*isCtor=*/true);
        return treeWalker_->runClassCtor(ctor, seed, args);
    }
    throw std::runtime_error("classdef constructor requires the interpreter backend");
}

Value Engine::evalConstExpr(const ASTNode *expr)
{
    if (!expr)
        return Value::Empty;
    if (backend_ == Backend::VM) {
        if (!compiler_ || !vm_)
            throw std::runtime_error("evalConstExpr: VM backend is unavailable");
        ASTNode fn(NodeType::FUNCTION_DEF);
        fn.strValue.assign("__const_expr__");
        fn.returnNames.push_back(keel::String::from("__result__"));
        auto bodyBlock = keel::make_unique<ASTNode>(NodeType::BLOCK);
        auto assignNode = keel::make_unique<ASTNode>(NodeType::ASSIGN);
        assignNode->suppressOutput = true;
        auto resultId = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
        resultId->strValue.assign("__result__");
        assignNode->children.push_back(keel::move(resultId));
        assignNode->children.push_back(cloneNode(expr));
        bodyBlock->children.push_back(keel::move(assignNode));
        fn.children.push_back(keel::move(bodyBlock));
        BytecodeChunk chunk = compiler_->compileFunction(&fn);
        vm_->setCompiledFuncs(&compiler_->compiledFuncs(),
                              &compiler_->scriptLocalCompiledFuncs());
        auto r = vm_->callReentrant(chunk, keel::ArrayRef<Value>{}, 1);
        return r.empty() ? Value::Empty : keel::move(r[0]);
    }
    if (treeWalker_)
        return treeWalker_->evalExpressionPublic(expr, &constantsEnv());
    throw std::runtime_error("evalConstExpr: interpreter backend is unavailable");
}

const BytecodeChunk *Engine::ensureClassMethodChunk(const UserFunction &uf)
{
    return compiler_ ? compiler_->ensureClassMethodCompiled(uf) : nullptr;
}

void Engine::enforceMethodAccess(keel::StringRef className, keel::StringRef method)
{
    const auto *dp = classDefs_.get(className);
    if (!dp)
        return;
    if (const auto *ma = (*dp)->methodAccess.get(method))
        enforceAccess(this, ma->level, ma->declClass, "method", method);
}

const UserFunction *Engine::classGetter(keel::StringRef className,
                                        keel::StringRef prop) const
{
    const auto *dp = classDefs_.get(className);
    if (!dp)
        return nullptr;
    const auto *gp = (*dp)->getters.get(prop);
    if (!gp)
        return nullptr;
    if (isPropAccessorActive((*gp)->name))
        return nullptr;
    return gp->get();
}

const UserFunction *Engine::classSetter(keel::StringRef className,
                                        keel::StringRef prop) const
{
    const auto *dp = classDefs_.get(className);
    if (!dp)
        return nullptr;
    const auto *sp = (*dp)->setters.get(prop);
    if (!sp)
        return nullptr;
    if (isPropAccessorActive((*sp)->name))
        return nullptr;
    return sp->get();
}

bool Engine::isPropAccessorActive(keel::StringRef accessorName) const
{
    for (const auto &a : activePropAccessors_)
        if (a == accessorName)
            return true;
    if (vm_ && vm_->hasActiveFrameNamed(accessorName))
        return true;
    return false;
}

void Engine::enforcePropGetAccess(keel::StringRef className, keel::StringRef prop)
{
    const auto *dp = classDefs_.get(className);
    if (!dp || !(*dp)->anyNonPublicProp)
        return;
    if (const PropInfo *pi = findProp(**dp, prop); pi && pi->getAccess != Access::Public)
        enforceAccess(this, pi->getAccess, pi->declClass, "property", prop);
}

void Engine::enforcePropSetAccess(keel::StringRef className, keel::StringRef prop)
{
    const auto *dp = classDefs_.get(className);
    if (!dp)
        return;
    const ClassDefDesc &desc = **dp;
    const PropInfo *pi = findProp(desc, prop);
    if (pi) {
        if (pi->isConstant) {
            auto msg = keel::format("Cannot set Constant property '{}' of class '{}'",
                                    prop, className);
            throw std::runtime_error(msg.c_str());
        }
        if (pi->setAccess != Access::Public)
            enforceAccess(this, pi->setAccess, pi->declClass, "property", prop);
    } else if (!desc.setters.contains(prop)
               && std::find(desc.superclasses.begin(), desc.superclasses.end(), "dynamicprops")
                      == desc.superclasses.end()) {
        auto msg = keel::format("No public property '{}' for class '{}'", prop, className);
        throw std::runtime_error(msg.c_str());
    }
}

Value Engine::superConstruct(keel::StringRef base, const Value &seed,
                             keel::ArrayRef<Value> args)
{
    const auto *dp = classDefs_.get(base);
    if (!dp) {
        auto msg = keel::format("superclass '{}' is not a classdef", base);
        throw std::runtime_error(msg.c_str());
    }
    const auto &desc = *dp;
    // A base with no explicit constructor contributes only its default
    // property values, which are already present on `seed` — nothing to run.
    if (!desc->ctor)
        return seed;
    return invokeClassCtor(*desc->ctor, seed, args);
}

keel::Array<Value> Engine::superMethod(keel::StringRef base, keel::StringRef method,
                                       keel::ArrayRef<Value> args, keel::usize nout)
{
    const auto *dp = classDefs_.get(base);
    if (!dp) {
        auto msg = keel::format("superclass '{}' is not a classdef", base);
        throw std::runtime_error(msg.c_str());
    }
    const auto &desc = *dp;
    const auto *mp = desc->methods.get(method);
    if (!mp) {
        auto msg = keel::format("superclass '{}' has no method '{}'", base, method);
        throw std::runtime_error(msg.c_str());
    }
    // Enforce the base method's access from the calling context (the current
    // top frame is the subclass method making the super-call): a `protected`
    // base method is reachable from a subclass, a `private` one is not.
    if (const auto *ma = desc->methodAccess.get(method))
        enforceAccess(this, ma->level, ma->declClass, "method", method);
    return invokeClassMethod(**mp, args, nout);
}

void Engine::pushClassCtx(keel::StringRef className, bool isCtor)
{
    classCtx_.push_back({keel::String::from(className), isCtor});
}

void Engine::popClassCtx()
{
    if (!classCtx_.empty())
        classCtx_.pop_back();
}

// The class whose method/constructor is currently executing. A TW callback's
// ClassCtxGuard (classCtx_) is innermost when present; otherwise, under the VM
// backend, the running class is the VM's top method frame. Empty == script
// scope. (No separate VM stack — read on demand, so it stays exception-safe.)
keel::StringRef Engine::currentClassCtx_() const
{
    if (!classCtx_.empty())
        return classCtx_.back().className.as_ref();
    if (vm_ && backend_ == Backend::VM)
        return vm_->currentMethodClass();
    return {};
}

bool Engine::classCtxAllows(keel::StringRef declClass, bool privateOnly) const
{
    const keel::StringRef ctx = currentClassCtx_();
    if (ctx.empty())
        return false; // script scope — only public members are reachable
    if (ctx == declClass)
        return true; // the declaring class itself (covers private + protected)
    if (privateOnly)
        return false;
    // protected: the running class must be a subclass of the declaring class.
    const auto *dp = classDefs_.get(ctx);
    if (!dp)
        return false;
    const auto &sc = (*dp)->superclasses;
    return std::find(sc.begin(), sc.end(), declClass) != sc.end();
}

bool Engine::classCtxInCtorOf(keel::StringRef declClass) const
{
    if (!classCtx_.empty())
        return classCtx_.back().isCtor && classCtx_.back().className == declClass;
    if (vm_ && backend_ == Backend::VM)
        return vm_->currentMethodIsCtor() && vm_->currentMethodClass() == declClass;
    return false;
}

void Engine::enforceCtorAccess(keel::StringRef className)
{
    const auto *dp = classDefs_.get(className);
    if (dp && (*dp)->ctorAccess != Access::Public) {
        const Access lvl = (*dp)->ctorAccess;
        if (!classCtxAllows((*dp)->ctorDeclClass, /*privateOnly=*/lvl == Access::Private)) {
            auto msg = keel::format("Cannot call the {} constructor of '{}' from outside the class",
                                    accessWord(lvl), className);
            throw std::runtime_error(msg.c_str());
        }
    }
}

Value Engine::constructChecked(const BuiltinClass *cls, keel::ArrayRef<Value> args, CallContext &ctx)
{
    enforceCtorAccess(cls->name);
    return cls->construct(args, ctx);
}

Value Engine::makeDefaultInstance(keel::StringRef className)
{
    const auto *dp = classDefs_.get(className);
    if (!dp) {
        auto msg = keel::format("'{}' is not a classdef", className);
        throw std::runtime_error(msg.c_str());
    }
    const auto &desc = *dp;
    if (desc->isAbstract) {
        auto msg = keel::format("Cannot instantiate abstract class '{}' (unimplemented abstract method)",
                                className);
        throw std::runtime_error(msg.c_str());
    }
    auto st = keel::make_arc_in<ObjectState>(mr_, mr_);
    for (const auto &p : desc->props)
        st->props.put(p.name.clone(), p.def);
    return Value::object(desc->name, st, desc->isHandle, mr_);
}

const UserFunction *Engine::classCtor(keel::StringRef className) const
{
    const auto *dp = classDefs_.get(className);
    if (!dp || !(*dp)->ctor)
        return nullptr;
    return (*dp)->ctor.get();
}

void Engine::registerFunction(keel::StringRef ns,
                              keel::StringRef name,
                              ExternalFunc func)
{
    if (ns.empty()) {
        registerFunctionImpl_(name, name, keel::move(func));
    } else {
        keel::String full = keel::format("{}.{}", ns, name);
        registerFunctionImpl_(full, name, keel::move(func));
    }
}

// Internal helper: walk active imports across env→parent chain plus the
// engine's workspace fallback, calling `tryQualified(qualified)` for each
// candidate. Returns true the first time the callback returns true.
//
// Resolution rules (mirrors namespace_design.md §4):
//   * `import a.b.*`     → tries "a.b.<name>" first, then "deep" candidates
//                          via shortNameIndex_: any registered fullname that
//                          starts with "a.b." and ends with ".<name>". This
//                          lets `import signal.*` find `signal.transforms.fft`
//                          when calling bare `fft()` — sub-namespaces are
//                          transparent for wildcard imports.
//   * `import a.b.<name>` → tries "a.b.<name>" (when last path segment matches)
//   * `import a.b as x`   → when `name` starts with "x.", rewrites to
//                           "a.b.<rest>" and tries that; otherwise skipped.
template <class ShortNameIndex, class Fn>
static bool walkImportCandidates_(keel::StringRef name,
                                   const Environment *env,
                                   const Environment *workspaceEnv,
                                   const ShortNameIndex &shortNameIndex,
                                   Fn &&tryQualified)
{
    auto buildPrefix = [](const keel::Array<keel::String> &path) {
        keel::String s;
        s.reserve(64);
        for (const auto &p : path) {
            if (!s.empty()) s.push_back('.');
            s.append(p);
        }
        return s;
    };

    // Visit every import visible from `env`, child-most env first and
    // newest-first within an env (the same shadowing order the walk
    // below uses), until `fn` returns true. Returns whether `fn` did.
    // The workspace fallback mirrors the walk: TW user functions parent
    // their local env at constantsEnv, not workspaceEnv.
    auto eachVisibleImport = [&](auto &&fn) -> bool {
        for (const Environment *cur = env; cur != nullptr;
             cur = cur->parentForImports()) {
            const auto &imps = cur->activeImports();
            for (keel::usize i = imps.size(); i > 0; --i)
                if (fn(imps[i - 1]))
                    return true;
        }
        if (workspaceEnv && env != workspaceEnv) {
            const auto &imps = workspaceEnv->activeImports();
            for (keel::usize i = imps.size(); i > 0; --i)
                if (fn(imps[i - 1]))
                    return true;
        }
        return false;
    };

    // Chained aliases (`import ns as t1; import t1.sub as t2`): the
    // candidate built from one alias may still start with another
    // alias's name. Rewrite its head through the visible imports (one
    // shadowing-order match per hop) until a fixed point; the hop cap
    // guards cyclic imports (`import a as b; import b as a`)
    // (bugs/closed/core/import-alias-chain.md).
    auto resolveAliasHeads = [&](keel::String s) -> keel::String {
        for (int hop = 0; hop < 8; ++hop) {
            keel::String next = s.clone();
            eachVisibleImport([&](const Import &imp) {
                if (imp.alias.empty())
                    return false;
                if (next.size() > imp.alias.size() + 1
                    && next.starts_with(imp.alias)
                    && next[imp.alias.size()] == '.') {
                    keel::String r = buildPrefix(imp.path);
                    r.push_back('.');
                    r.append(next.substr(imp.alias.size() + 1));
                    if (r != next) // self-alias counts as settled
                        next = keel::move(r);
                    return true;
                }
                return false;
            });
            if (next == s)
                break; // fixed point: no head matched (or settled)
            s = keel::move(next);
        }
        return s;
    };

    auto runOne = [&](const Environment *cur) -> bool {
        // Walk imports newest-first: in `import a.*; import b.*;` the
        // second import shadows the first when both contain the same
        // leaf. activeImports() pushes append-only, so iterate in
        // reverse order.
        const auto &imps = cur->activeImports();
        for (keel::usize i = imps.size(); i > 0; --i) {
            const auto &imp = imps[i - 1];
            if (imp.path.empty()) continue;
            if (imp.wildcard) {
                keel::String prefix = buildPrefix(imp.path);
                keel::String direct = keel::format("{}.{}", prefix, name);
                if (tryQualified(direct.as_ref()))
                    return true;
                // Deep scan: any registered "a.b.SUB.<name>" (or deeper)
                // also matches `import a.b.*`.
                keel::String dottedPrefix = keel::format("{}.", prefix);
                keel::String dottedSuffix = keel::format(".{}", name);
                if (const auto *vec = shortNameIndex.get(name)) {
                    for (const auto &full : *vec) {
                        if (full == direct) continue;            // already tried
                        if (full.size() <= dottedPrefix.size())  continue;
                        if (!full.starts_with(dottedPrefix))     continue;
                        if (!full.ends_with(dottedSuffix))       continue;
                        if (tryQualified(full.as_ref()))
                            return true;
                    }
                }
            } else if (!imp.alias.empty()) {
                // Alias import (`import a.b as alias`): rewrite a name
                // that begins with "alias." to "a.b.<rest>" and try the
                // registered qualified form. Bare names (no dot in
                // `name`) and names whose prefix doesn't match the
                // alias fall through.
                if (name.size() > imp.alias.size() + 1
                    && name.starts_with(imp.alias)
                    && name[imp.alias.size()] == '.') {
                    keel::String candidate = buildPrefix(imp.path);
                    candidate.push_back('.');
                    candidate.append(name.substr(imp.alias.size() + 1));
                    candidate = resolveAliasHeads(keel::move(candidate));
                    if (tryQualified(candidate.as_ref()))
                        return true;
                }
            } else if (imp.path.back() == name) {
                keel::String cand = buildPrefix(imp.path);
                if (tryQualified(cand.as_ref()))
                    return true;
            }
        }
        return false;
    };

    for (const Environment *cur = env; cur != nullptr;
         cur = cur->parentForImports()) {
        if (runOne(cur)) return true;
    }
    // Top-level imports always cascade into function calls. TW user
    // functions parent localEnv at constantsEnv (not workspaceEnv), so
    // the parent-walk above doesn't reach workspaceEnv — this fallback
    // is load-bearing for TW. VM frame.env parents at workspaceEnv
    // directly, so this re-visits an env we already walked (harmless).
    if (workspaceEnv && env != workspaceEnv) {
        if (runOne(workspaceEnv)) return true;
    }
    return false;
}

void Engine::markPluginNamespace(keel::StringRef ns, bool isolated)
{
    if (ns.empty()) return;
    const keel::usize dot = ns.find('.');
    keel::StringRef topNs = (dot != keel::kInvalidIndex) ? ns.substr(0, dot) : ns;
    pluginNamespaces_.insert(keel::String::from(topNs));
    if (isolated) {
        isolatedPluginNamespaces_.insert(keel::String::from(topNs));
    }
    bumpCallCacheEpoch();
}

bool Engine::isPluginNamespace(keel::StringRef ns) const
{
    const keel::usize dot = ns.find('.');
    keel::StringRef topNs = (dot != keel::kInvalidIndex) ? ns.substr(0, dot) : ns;
    return pluginNamespaces_.contains(topNs);
}

bool Engine::isIsolatedPluginNamespace(keel::StringRef ns) const
{
    const keel::usize dot = ns.find('.');
    keel::StringRef topNs = (dot != keel::kInvalidIndex) ? ns.substr(0, dot) : ns;
    return isolatedPluginNamespaces_.contains(topNs);
}

keel::String Engine::formatUndefinedFunctionError(keel::StringRef name,
                                                  keel::StringRef baseMsg) const
{
    if (name.find('.') != keel::kInvalidIndex)
        return keel::String::from(baseMsg);

    if (namespaceMode_ == NamespaceMode::Isolated) {
        keel::String modName;
        if (const auto *vec = shortNameIndex_.get(name)) {
            for (const auto &full : *vec) {
                const keel::usize dot = full.find('.');
                if (dot != keel::kInvalidIndex) {
                    keel::StringRef topNs = full.substr(0, dot);
                    if (pluginNamespaces_.contains(topNs)) {
                        modName.assign(topNs);
                        break;
                    }
                }
            }
        }
        if (modName.empty()) {
            modName = const_cast<Engine *>(this)->pluginLoader().moduleProvidingFunction(name);
        }
        if (!modName.empty()) {
            return keel::format(
                "{} (isolated namespace mode: '{}' is in module '{}' — use '{}.{}(...)' or 'import {}.*')",
                baseMsg, name, modName, modName, name, modName);
        }
    } else {
        if (const auto *vec = shortNameIndex_.get(name)) {
            for (const auto &full : *vec) {
                const keel::usize dot = full.find('.');
                if (dot != keel::kInvalidIndex) {
                    keel::StringRef topNs = full.substr(0, dot);
                    if (isolatedPluginNamespaces_.contains(topNs)) {
                        return keel::format(
                            "{} ('{}' is loaded in isolated namespace '{}' — use '{}.{}(...)' or 'import {}.*')",
                            baseMsg, name, topNs, topNs, name, topNs);
                    }
                }
            }
        }
    }
    return keel::String::from(baseMsg);
}

const ExternalFunc *Engine::findExternal(keel::StringRef name,
                                         const Environment *env) const
{
    // 1. Direct hit — covers core, promotions, and already-qualified names.
    if (const ExternalFunc *direct = externalFuncs_.get(name))
        return direct;

    // 2. Walk active imports across env→parent → workspaceEnv fallback.
    //    The workspace fallback is a pragmatic relaxation so that REPL /
    //    test code with imports at the top can flatten
    //    those names inside nested function bodies too. MATLAB-strict
    //    mode would scope imports to the declaring function only.
    const ExternalFunc *hit = nullptr;
    walkImportCandidates_(name, env, workspaceEnv_.get(), shortNameIndex_,
        [&](keel::StringRef qualified) {
            if (const ExternalFunc *q = externalFuncs_.get(qualified)) {
                hit = q;
                return true;
            }
            if (const_cast<Engine *>(this)->pluginLoader().tryAutoLoadForFunction(qualified)) {
                if (const ExternalFunc *q2 = externalFuncs_.get(qualified)) {
                    hit = q2;
                    return true;
                }
            }
            return false;
        });
    if (hit) return hit;

    auto allowBareFromFull = [this](keel::StringRef full) -> bool {
        const keel::usize dot = full.find('.');
        if (dot == keel::kInvalidIndex) return true;
        keel::StringRef topNs = full.substr(0, dot);
        if (isolatedPluginNamespaces_.contains(topNs))
            return false;
        if (namespaceMode_ == NamespaceMode::Isolated
            && pluginNamespaces_.contains(topNs))
            return false;
        return true;
    };

    // 3. Bare-name fallback (MATLAB semantics in Compat mode; core-only in Isolated mode):
    //    shortNameIndex_ maps leaf names to full qualified names.
    //    For the rare leaf-name collision across namespaces, pick by namespace
    //    registration order (namespaceOrder_), which is deterministic.
    const bool isQualified = (name.find('.') != keel::kInvalidIndex);
    if (!isQualified) {
        if (const auto *vec = shortNameIndex_.get(name); vec && !vec->empty()) {
            // Fast path: unique leaf name (99%+ of cases) — one lookup.
            if (vec->size() == 1) {
                if (allowBareFromFull((*vec)[0])) {
                    if (const ExternalFunc *f = externalFuncs_.get((*vec)[0]))
                        return f;
                }
            } else {
                // Collision: multiple namespaces have this leaf name.
                // Pick the one whose top-level namespace registered first.
                auto rankOf = [this](keel::StringRef full) {
                    const keel::usize dot = full.find('.');
                    keel::StringRef topNs = (dot != keel::kInvalidIndex)
                                                ? full.substr(0, dot)
                                                : full;
                    for (keel::usize i = 0; i < namespaceOrder_.size(); ++i)
                        if (namespaceOrder_[i] == topNs)
                            return static_cast<int>(i);
                    return static_cast<int>(namespaceOrder_.size());
                };
                const keel::String *best = nullptr;
                int bestRank = INT_MAX;
                for (const auto &full : *vec) {
                    if (!allowBareFromFull(full))
                        continue;
                    int r = rankOf(full);
                    if (r < bestRank) {
                        bestRank = r;
                        best = &full;
                    }
                }
                if (best) {
                    if (const ExternalFunc *f = externalFuncs_.get(*best))
                        return f;
                }
            }
        }
    }

    // 4. Plugin / modular toolbox autoload fallback:
    //    If the function is provided by an installed plugin module in the manifest
    //    that has not been loaded yet, try to load it now on-demand.
    //    In Isolated mode, only qualified names (with '.') auto-load here;
    //    imported names were already handled in Step 2.
    if (isQualified || namespaceMode_ == NamespaceMode::Compat) {
        if (const_cast<Engine *>(this)->pluginLoader().tryAutoLoadForFunction(name)) {
            if (const ExternalFunc *f = externalFuncs_.get(name))
                return f;
            if (!isQualified) {
                if (const auto *vec2 = shortNameIndex_.get(name)) {
                    for (const auto &full : *vec2) {
                        if (!allowBareFromFull(full))
                            continue;
                        if (const ExternalFunc *f = externalFuncs_.get(full))
                            return f;
                    }
                }
            }
        }
    }

    return nullptr;
}

keel::String Engine::bareNameSource(keel::StringRef name) const
{
    if (name.find('.') != keel::kInvalidIndex)
        return {};
    const auto *vec = shortNameIndex_.get(name);
    if (!vec || vec->empty())
        return {};

    auto allowBareFromFull = [this](keel::StringRef full) -> bool {
        const keel::usize dot = full.find('.');
        if (dot == keel::kInvalidIndex) return true;
        keel::StringRef topNs = full.substr(0, dot);
        if (isolatedPluginNamespaces_.contains(topNs))
            return false;
        if (namespaceMode_ == NamespaceMode::Isolated
            && pluginNamespaces_.contains(topNs))
            return false;
        return true;
    };

    // Unique leaf: return directly if allowed.
    if (vec->size() == 1) {
        return allowBareFromFull((*vec)[0]) ? (*vec)[0].clone() : keel::String{};
    }
    // Collision: same ranking as findExternal step 3.
    auto rankOf = [this](keel::StringRef full) {
        const keel::usize dot = full.find('.');
        keel::StringRef topNs = (dot != keel::kInvalidIndex)
                                    ? full.substr(0, dot)
                                    : full;
        for (keel::usize i = 0; i < namespaceOrder_.size(); ++i)
            if (namespaceOrder_[i] == topNs)
                return static_cast<int>(i);
        return static_cast<int>(namespaceOrder_.size());
    };
    const keel::String *best = nullptr;
    int bestRank = INT_MAX;
    for (const auto &full : *vec) {
        if (!allowBareFromFull(full))
            continue;
        int r = rankOf(full);
        if (r < bestRank) {
            bestRank = r;
            best = &full;
        }
    }
    return best ? best->clone() : keel::String{};
}

bool Engine::isKnownLeafName(keel::StringRef name) const
{
    return findExternal(name, workspaceEnv_.get()) != nullptr;
}

PluginLoader &Engine::pluginLoader()
{
    if (!pluginLoader_) {
        pluginLoader_ = keel::make_unique<PluginLoader>(*this);
    }
    return *pluginLoader_;
}

const PluginLoader &Engine::pluginLoader() const
{
    if (!pluginLoader_) {
        pluginLoader_ = keel::make_unique<PluginLoader>(const_cast<Engine &>(*this));
    }
    return *pluginLoader_;
}

void Engine::setVariable(keel::StringRef name, Value val)
{
    workspaceEnv_->set(name, keel::move(val));
}
Value *Engine::getVariable(keel::StringRef name)
{
    // Environment::get already routes a name the base workspace declared global
    // to globalsEnv_ (its isGlobal delegation), and resolves everything else
    // against local storage then the constants parent. That single call is the
    // whole gate: a function's `global G; G=5` lives in globalsEnv_ but the base
    // never declared G global, so workspaceEnv_->get("G") never consults
    // globalsEnv_ — no leak into the base who/exist/IDE-viewer view. Base
    // membership is the single set workspaceEnv_->globals_, populated by every
    // top-level `global X` on both engines (TreeWalker::execGlobalPersistent and
    // Engine::runOneChunk's updateTopLevelGlobals). See
    // FrameIntrospectionEdgesTest.GlobalInsideEvalinBase.
    return workspaceEnv_->get(name);
}

void Engine::setOutputFunc(OutputFunc f)
{
    outputFunc_ = keel::move(f);
}

Engine::OutputFunc Engine::swapOutputFunc(OutputFunc f)
{
    OutputFunc old = keel::move(outputFunc_);
    outputFunc_ = keel::move(f);
    return old;
}

void Engine::setMaxRecursionDepth(int d)
{
    treeWalker_->setMaxRecursionDepth(d);
    vm_->setMaxRecursionDepth(d);
}

void Engine::outputText(keel::StringRef s)
{
    if (outputFunc_) {
        outputFunc_(s);
    } else {
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }
}

bool Engine::hasFunction(keel::StringRef name) const
{
    return externalFuncs_.contains(name) || hasUserFunction(name);
}

bool Engine::hasUserFunction(keel::StringRef name) const
{
    return scriptLocalUserFuncs_.contains(name)
           || userFuncs_.contains(name);
}

const UserFunction *Engine::lookupUserFunctionLocal(keel::StringRef name) const
{
    if (const UserFunction *f = scriptLocalUserFuncs_.get(name))
        return f;
    return userFuncs_.get(name);
}

const UserFunction *Engine::lookupUserFunction(keel::StringRef name,
                                                const Environment *env)
{
    if (auto *f = lookupUserFunctionLocal(name))
        return f;
    // Direct path: parse-and-load <name>.m (or +pkg/.../<leaf>.m for a
    // dotted name) from the search path.
    if (auto *f = resolveMFile_(name))
        return f;

    // No scope to walk imports from — nothing more to try.
    if (!env)
        return nullptr;
    // Bare names walk wildcard / single-symbol / alias imports below.
    // Dotted names only benefit from alias rewriting (`x.foo` → `a.b.foo`
    // when `import a.b as x` is active); the wildcard / single-symbol
    // branches in walkImportCandidates_ no-op for dotted names.

    // Walk imports across env→parent → workspaceEnv fallback. Each
    // successful resolveMFile_ caches the entry under its full
    // qualified key, so subsequent calls hit userFuncs_ directly.
    const UserFunction *hit = nullptr;
    walkImportCandidates_(name, env, workspaceEnv_.get(), shortNameIndex_,
        [&](keel::StringRef qualified) {
            if (auto *f = lookupUserFunctionLocal(qualified)) {
                hit = f;
                return true;
            }
            if (auto *f = resolveMFile_(qualified)) {
                hit = f;
                return true;
            }
            return false;
        });
    return hit;
}

void Engine::addPath(keel::StringRef dir)
{
    if (dir.find(';') != keel::kInvalidIndex) {
        keel::usize start = 0;
        while (start <= dir.size()) {
            keel::usize pos = dir.find(';', start);
            keel::StringRef part = (pos == keel::kInvalidIndex)
                ? dir.substr(start)
                : dir.substr(start, pos - start);
            if (!part.empty()) addPath(part);
            if (pos == keel::kInvalidIndex) break;
            start = pos + 1;
        }
        return;
    }
    if (dir.empty() || dir == "-begin" || dir == "-end" || dir == "-frozen")
        return;
    keel::String effectiveDir = keel::String::from(dir);
    try {
        ResolvedPath rp = fsCtx_.resolvePath(dir);
        if (rp.fs && rp.fs->exists(rp.path)) {
            keel::String fsName = rp.fs->name();
            effectiveDir = (fsName != "native")
                ? keel::format("{}:{}", fsName, rp.path)
                : rp.path.clone();
        }
    } catch (...) {}
    // De-dup: ignore if already present.
    for (const auto &p : mPath_) {
        if (p == effectiveDir || p == dir) return;
    }
    mFileMissCache_.clear();
    mPath_.push_back(keel::move(effectiveDir));
    bumpCallCacheEpoch();
}

void Engine::rmPath(keel::StringRef dir)
{
    if (dir.find(';') != keel::kInvalidIndex) {
        keel::usize start = 0;
        while (start <= dir.size()) {
            keel::usize pos = dir.find(';', start);
            keel::StringRef part = (pos == keel::kInvalidIndex)
                ? dir.substr(start)
                : dir.substr(start, pos - start);
            if (!part.empty()) rmPath(part);
            if (pos == keel::kInvalidIndex) break;
            start = pos + 1;
        }
        return;
    }
    keel::String effectiveDir = keel::String::from(dir);
    try {
        ResolvedPath rp = fsCtx_.resolvePath(dir);
        if (rp.fs) {
            keel::String fsName = rp.fs->name();
            effectiveDir = (fsName != "native")
                ? keel::format("{}:{}", fsName, rp.path)
                : rp.path.clone();
        }
    } catch (...) {}
    for (keel::usize i = 0; i < mPath_.size(); ++i) {
        if (mPath_[i] == dir || mPath_[i] == effectiveDir) {
            mFileMissCache_.clear();
            mPath_.remove_at(i);
            bumpCallCacheEpoch();
            break;
        }
    }
}

void Engine::adoptUserFunction(keel::StringRef name,
                                UserFunction uf,
                                bool scriptScope)
{
    clearPersistentForFunc(name);
    if (scriptScope)
        scriptLocalUserFuncs_.put(keel::String::from(name), keel::move(uf));
    else
        userFuncs_.put(keel::String::from(name), keel::move(uf));
    bumpCallCacheEpoch();
}

void Engine::registerBuiltinMSource(keel::StringRef src)
{
    builtinMSourceTexts_.push_back(keel::String::from(src));
    adoptBuiltinMSource_(src);
}

// Parse and register one startup m-source: the parse+adopt half of
// registerBuiltinMSource, factored out so reinstallBuiltinSources() can
// replay a retained text without re-appending it to the retention list.
void Engine::adoptBuiltinMSource_(keel::StringRef src)
{
    Lexer lexer(src);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    // Persistently register one parsed `function` def (mirrors the m-file
    // loader): VM compiled chunk via registerFunctionAs + TW via userFuncs_.
    auto adopt = [this](const ASTNode *fd) {
        UserFunction func;
        func.name = fd->strValue.clone();
        func.params = fd->paramNames.clone();
        func.returns = fd->returnNames.clone();
        func.body = keel::Arc<const ASTNode>(cloneNode(fd->children[0].get()));
        func.closureEnv = nullptr;
        if (compiler_) {
            try {
                compiler_->registerFunctionAs(func.name, fd);
            } catch (const std::exception &) {
                // Non-fatal at adopt time — userFuncs_ remains populated and
                // VM call sites re-trigger compilation via ensureClassMethodChunk.
            }
        }
        keel::String key = func.name.clone();
        userFuncs_.put(keel::move(key), keel::move(func));
        bumpCallCacheEpoch();
    };
    if (ast->type == NodeType::FUNCTION_DEF) {
        adopt(ast.get());
    } else {
        for (const auto &c : ast->children)
            if (c && c->type == NodeType::FUNCTION_DEF)
                adopt(c.get());
    }
}

void Engine::registerBuiltinClassSource(keel::StringRef src)
{
    builtinClassSourceTexts_.push_back(keel::String::from(src));
    adoptBuiltinClassSource_(src);
}

// Parse and register each top-level CLASSDEF_DEF in one startup source —
// the identical registerClassDef call the compiler makes for a classdef
// statement, so direct registration is equivalent to evalSafe minus the
// (codeless) run.
void Engine::adoptBuiltinClassSource_(keel::StringRef src)
{
    Lexer lexer(src);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    if (ast->type == NodeType::CLASSDEF_DEF) {
        registerClassDef(ast.get());
    } else {
        for (const auto &c : ast->children)
            if (c && c->type == NodeType::CLASSDEF_DEF)
                registerClassDef(c.get());
    }
}

// MATLAB reloads cleared functions/classes from disk on the next call;
// engine-startup registrations have no disk backing, so after
// clearUserFunctions()/clearClassDefs() wipe them, re-register from the
// retained texts. Parser/registry work only — no VM run — safe to call
// from inside the `clear` external mid-chunk.
void Engine::reinstallBuiltinSources()
{
    for (const auto &src : builtinMSourceTexts_)
        adoptBuiltinMSource_(src);
    for (const auto &src : builtinClassSourceTexts_)
        adoptBuiltinClassSource_(src);
}

void Engine::rehashMFiles()
{
    // Drop cache entries AND the user-function/compiled mirrors created
    // by resolveMFile_. Functions registered by execFunctionDef from
    // top-level scripts are kept — only m-file-loaded ones should go.
    // The compiler stores chunks under the same key resolveMFile_ used
    // (qualified for +pkg/foo.m, bare for plain foo.m), so erase that exact
    // key — NOT clearCompiledFuncs(), which would also nuke script-defined
    // compiled functions (contradicting the "kept" promise above).
    keel::Array<keel::String> unregisteredClasses;
    for (auto [name, entry] : mFileCache_) {
        userFuncs_.remove(name);
        clearPersistentForFunc(name);
        if (compiler_)
            compiler_->eraseCompiledFunc(name);
        if (classDefs_.contains(name)) {
            // A class loaded from `Name.m`: evicting the bare chunk above is not
            // enough — the class machinery (registry, descriptor, `Name>method`
            // chunks, qualified externals) and the bare constructor external all
            // persist and would shadow the edited file. Drop them so the next
            // reference re-reads and re-registers. (mFileCache_ is still
            // populated here, so the ctor-external guard passes.)
            dropFileClassCtorExternal_(name);
            unregisterClassDef(name);
            unregisteredClasses.push_back(name.clone());
        } else if (entry.hasExternal) {
            dropFileClassCtorExternal_(name);
        }
    }
    mFileCache_.clear();
    mFileMissCache_.clear();
    bumpCallCacheEpoch();

    // An INLINE subclass of a just-unregistered file base survives rehash still
    // holding the old base snapshot (it is not in mFileCache_, so the loop above
    // didn't touch it). File subclasses need no help — they were unregistered
    // too and reload lazily on next use.
    reloadEvictedClassBases_(unregisteredClasses);
}

void Engine::refreshStaleMFiles()
{
    mFileMissCache_.clear();
    if (mFileCache_.empty())
        return;
    keel::Array<keel::String> stale;
    keel::Array<keel::String> evictedClasses;
    for (auto [name, entry] : mFileCache_) {
        ResolvedPath rp;
        try {
            rp = resolvePath(entry.fullPath);
        } catch (...) {
            stale.push_back(name.clone());
            continue;
        }
        if (!rp.fs || !rp.fs->exists(rp.path)) {
            stale.push_back(name.clone());
            continue;
        }
        auto st = rp.fs->stat(rp.path);
        const bool haveCachedStat = (entry.mtime != 0 || entry.mtimeNs != 0);
        const bool haveCurStat = st.has_value() && (st->mtime != 0 || st->mtimeNs != 0);
        if (haveCachedStat && haveCurStat) {
            const bool sameMtime = (entry.mtimeNs != 0 && st->mtimeNs != 0)
                ? (st->mtimeNs == entry.mtimeNs)
                : (st->mtime == entry.mtime);
            const bool sameSize = (entry.fileSize < 0) || (st->size == entry.fileSize);
            if (!sameMtime || !sameSize)
                stale.push_back(name.clone());
        } else {
            try {
                keel::String content = rp.fs->readFile(rp.path);
                if (entry.sourceCode && content.as_ref() != entry.sourceCode->as_ref())
                    stale.push_back(name.clone());
            } catch (...) {
                stale.push_back(name.clone());
            }
        }
    }
    if (!stale.empty())
        bumpCallCacheEpoch();
    for (const auto &name : stale) {
        if (const auto *entry = mFileCache_.get(name)) {
            const bool hadExternal = entry->hasExternal;
            userFuncs_.remove(name);
            clearPersistentForFunc(name);
            if (compiler_)
                compiler_->eraseCompiledFunc(name);
            if (classDefs_.contains(name)) {
                dropFileClassCtorExternal_(name);
                unregisterClassDef(name);
                evictedClasses.push_back(name.clone());
            } else if (hadExternal) {
                dropFileClassCtorExternal_(name);
            }
            mFileCache_.remove(name);
        }
    }
    // Without this cascade an inline subclass of an evicted base keeps its
    // stale base snapshot (d9ab393f regression: the manual `rehash` that
    // follows this refresh found nothing left to reload).
    reloadEvictedClassBases_(evictedClasses);
}

void Engine::reloadEvictedClassBases_(keel::ArrayRef<keel::String> bases)
{
    for (const auto &base : bases) {
        if (findClass(base))
            continue; // already pulled back in by an earlier base's reload
        bool hasLiveDependent = false;
        for (auto [cn, desc] : classDefs_) {
            (void)cn;
            if (std::find(desc->superclasses.begin(), desc->superclasses.end(), base)
                != desc->superclasses.end()) {
                hasLiveDependent = true;
                break;
            }
        }
        if (hasLiveDependent)
            resolveMFile_(base); // reload from the edited file + cascade to subclasses
    }
}

static keel::usize findLastPathSep(keel::StringRef s) noexcept
{
    for (keel::usize i = s.size(); i > 0; --i) {
        if (s[i - 1] == '/' || s[i - 1] == '\\')
            return i - 1;
    }
    return keel::kInvalidIndex;
}

static bool hasPathSepOrColon(keel::StringRef s) noexcept
{
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':')
            return true;
    }
    return false;
}

const UserFunction *Engine::resolveMFile_(keel::StringRef name)
{
    // Negative-lookup fast path: avoid walking searchDirs and hitting OS
    // filesystem stat/exists on every builtin call inside a hot loop.
    const keel::String *curDir = currentScriptDir();
    keel::StringRef curDirRef = curDir ? curDir->as_ref() : keel::StringRef{};
    keel::String missKey = !curDirRef.empty()
                               ? keel::format("{}\n{}", curDirRef, name)
                               : keel::String::from(name);
    if (mFileMissCache_.contains(missKey))
        return nullptr;

    // Build search-path list: script-dir first (if any), then mPath_.
    // The implicit script-dir entry is what makes sibling lookup work
    // without addpath — `caller.m` calling `helper(x)` resolves against
    // the directory the running script came from. Routes through the
    // script's FS via `resolvePath` (which also falls back to the
    // origin's fsName when the relative path has no scheme).
    keel::Array<keel::StringRef> searchDirs;
    searchDirs.reserve(mPath_.size() + (curDirRef.empty() ? 0 : 1));
    if (!curDirRef.empty())
        searchDirs.push_back(curDirRef);
    for (const auto &p : mPath_)
        searchDirs.push_back(p.as_ref());

    // Decompose dotted name. "pkg.sub.foo" → +pkg/+sub/foo.m. The leaf
    // (last segment) is the function name and what we cache under; the
    // earlier segments become +<seg>/ directory components per
    // MATLAB's package-folder convention.
    keel::String leafName = keel::String::from(name);
    keel::String nsPrefix;            // "+pkg/+sub/" form, empty for unqualified
    {
        keel::usize dot = name.find('.');
        if (dot != keel::kInvalidIndex) {
            keel::StringRef tail = name;
            while ((dot = tail.find('.')) != keel::kInvalidIndex) {
                nsPrefix.push_back('+');
                nsPrefix.append(tail.substr(0, dot));
                nsPrefix.push_back('/');
                tail = tail.substr(dot + 1);
            }
            leafName.assign(tail);
        }
    }

    for (keel::StringRef dir : searchDirs) {
        keel::String userPath = keel::String::from(dir);
        if (!userPath.empty() && userPath.back() != '/' && userPath.back() != '\\')
            userPath.push_back('/');
        userPath.append(nsPrefix);
        userPath.append(leafName);
        userPath.append(".m");

        ResolvedPath rp;
        try {
            rp = resolvePath(userPath);
        } catch (const std::exception &) {
            continue;
        }
        if (!rp.fs || !rp.fs->exists(rp.path))
            continue;
        keel::StringRef rpPath = rp.path.as_ref();
        keel::String rpFsName = rp.fs->name();
        {
            keel::String wantFile = keel::format("{}.m", leafName);
            const keel::usize slashPos = findLastPathSep(rpPath);
            keel::StringRef parentDir = (slashPos == keel::kInvalidIndex)
                                            ? keel::StringRef(".")
                                            : rpPath.substr(0, slashPos);
            auto entries = rp.fs->listDir(parentDir);
            if (!entries.empty()) {
                bool exactFound = false;
                for (const auto &ent : entries) {
                    if (ent.name.as_ref() == wantFile) {
                        exactFound = true;
                        break;
                    }
                }
                if (!exactFound)
                    continue;
            }
        }

        const keel::String cachePath = (rpFsName != "native")
            ? keel::format("{}:{}", rpFsName, rpPath)
            : keel::String::from(rpPath);

        // Cache hit? Validate via mtime; re-parse if stale.
        if (const auto *ce = mFileCache_.get(name);
            ce && (ce->fullPath == cachePath || ce->fullPath == userPath)) {
            auto st = rp.fs->stat(rp.path);
            const bool haveCachedStat = (ce->mtime != 0 || ce->mtimeNs != 0);
            const bool haveCurStat = st.has_value() && (st->mtime != 0 || st->mtimeNs != 0);
            if (haveCachedStat && haveCurStat) {
                const bool sameMtime = (ce->mtimeNs != 0 && st->mtimeNs != 0)
                    ? (st->mtimeNs == ce->mtimeNs)
                    : (st->mtime == ce->mtime);
                const bool sameSize = (ce->fileSize < 0) || (st->size == ce->fileSize);
                if (sameMtime && sameSize) {
                    if (auto *uf = lookupUserFunctionLocal(name))
                        return uf;
                    if (ce->hasExternal && externalFuncs_.contains(name))
                        return nullptr;
                }
            }
            // Stale or no mtime — drop and re-parse.
            const bool hadExternal = ce->hasExternal;
            if (hadExternal)
                dropFileClassCtorExternal_(name);
            mFileCache_.remove(name);
            userFuncs_.remove(name);
            bumpCallCacheEpoch();
        }

        // Read + parse + extract FUNCTION_DEF. The file EXISTS (checked above),
        // so a read/lex/parse failure is a real error in the matched m-file —
        // surface it (with the path) instead of silently skipping to the next
        // search dir, which would mask a syntax error as "undefined function".
        // MATLAB likewise reports the first path-matched file's error.
        auto loadError = [&](const std::exception &e) -> Error {
            return Error(keel::format("error loading '{}': {}", userPath, e.what()),
                         0, 0, "", "", "numkit:mfile:loadError");
        };
        keel::String content;
        try {
            content = rp.fs->readFile(rp.path);
        } catch (const std::exception &e) {
            throw loadError(e);
        }

        Lexer lexer(content);
        keel::Array<Token> tokens;
        try {
            tokens = lexer.tokenize();
        } catch (const std::exception &e) {
            throw loadError(e);
        }
        Parser parser(keel::move(tokens));
        ASTNodePtr ast;
        try {
            ast = parser.parse();
        } catch (const std::exception &e) {
            throw loadError(e);
        }
        if (!ast) continue;

        // First top-level FUNCTION_DEF whose name matches the leaf. The
        // function-def name inside a +pkg/foo.m file is "foo" — the
        // package qualification lives in the path, not the source.
        const ASTNode *funcDef = nullptr;
        if (ast->type == NodeType::BLOCK) {
            for (const auto &c : ast->children) {
                if (c && c->type == NodeType::FUNCTION_DEF && c->strValue == leafName) {
                    funcDef = c.get();
                    break;
                }
            }
            if (!funcDef && !ast->children.empty() && ast->children[0]
                && ast->children[0]->type == NodeType::FUNCTION_DEF) {
                funcDef = ast->children[0].get();
            }
        } else if (ast->type == NodeType::FUNCTION_DEF) {
            funcDef = ast.get();
        }

        // classdef file `Name.m`: register the class and a constructor
        // external under `name` so this very call resolves (subsequent calls
        // hit findClass directly). Mirrors the FUNCTION_DEF path's caching.
        // Provenance (dir extracted exactly like the run() builtin does
        // it — root-level files keep "/", separator-less paths keep "")
        // backs mfilename() inside the loaded code: file functions report
        // this file, classdef methods report the class file, and sibling
        // lookups keep working transitively through function calls.
        keel::String scriptDir;
        {
            const keel::usize slash = findLastPathSep(rpPath);
            if (slash == 0)
                scriptDir.push_back(rpPath[0]);
            else if (slash != keel::kInvalidIndex)
                scriptDir.assign(rpPath.substr(0, slash));
        }

        if (!funcDef) {
            const ASTNode *classDef = nullptr;
            if (ast->type == NodeType::BLOCK) {
                for (const auto &c : ast->children)
                    if (c && c->type == NodeType::CLASSDEF_DEF && c->strValue == leafName) {
                        classDef = c.get();
                        break;
                    }
            } else if (ast->type == NodeType::CLASSDEF_DEF && ast->strValue == leafName) {
                classDef = ast.get();
            }
            if (classDef) {
                // Register under the QUALIFIED lookup name so a `+pkg/Name.m`
                // class is identified as `pkg.Name` (class()/isa()/registry keys)
                // — the source node carries only the leaf. The ctor external and
                // mFileCache_ key below already use the qualified `name`, so all
                // three now agree (clear/rehash evict cleanly).
                registerClassDefFrom(classDef, name, rpFsName, scriptDir, rpPath);
                keel::String cn = keel::String::from(name);
                registerFunction(name, [cn = keel::move(cn)](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                                            CallContext &ctx) {
                    const BuiltinClass *c = ctx.engine->findClass(cn);
                    if (!c || !c->construct) {
                        auto msg = keel::format("classdef '{}' has no constructor", cn);
                        throw std::runtime_error(msg.c_str());
                    }
                    outs[0] = ctx.engine->constructChecked(c, args, ctx);
                });
                MFileCacheEntry ce;
                ce.fullPath = cachePath.clone();
                if (auto st = rp.fs->stat(rp.path)) {
                    ce.mtime = st->mtime;
                    ce.mtimeNs = st->mtimeNs;
                    ce.fileSize = st->size;
                }
                ce.sourceCode = keel::make_arc<const keel::String>(keel::move(content));
                ce.hasExternal = true;
                mFileCache_.put(keel::String::from(name), keel::move(ce));
                bumpCallCacheEpoch();
                return nullptr; // class + ctor-external resolve the call
            }
            if (!hasExternalFunction(name) && !findExternal(name, nullptr)) {
                keel::String scriptName = leafName.clone();
                keel::String fsName = keel::String::from(rpFsName);
                keel::String sDir = scriptDir.clone();
                keel::String sPath = keel::String::from(rpPath);
                auto srcPtr = keel::make_arc<const keel::String>(keel::move(content));
                registerFunction(name, [scriptName = keel::move(scriptName),
                                        fsName = keel::move(fsName),
                                        sDir = keel::move(sDir),
                                        sPath = keel::move(sPath),
                                        srcPtr](
                                           keel::ArrayRef<Value> args, keel::usize nargout,
                                           keel::MutArrayRef<Value> outs, CallContext &ctx) {
                    if (!args.empty() || nargout > 0)
                        throw Error(keel::format("Attempt to execute SCRIPT {} as a function.", scriptName),
                                    0, 0, scriptName, "", "numkit:script:CalledAsFunction");
                    auto keepSrc = srcPtr;
                    const keel::String keepFs = fsName.clone();
                    const keel::String keepDir = sDir.clone();
                    const keel::String keepPath = sPath.clone();
                    Environment *scope = ctx.env ? ctx.env : &ctx.engine->workspaceEnv();
                    ctx.engine->pushScriptOrigin(keepFs, keepDir, keepPath);
                    try {
                        ctx.engine->eval(*keepSrc, scope);
                    } catch (...) {
                        ctx.engine->popScriptOrigin();
                        throw;
                    }
                    ctx.engine->popScriptOrigin();
                    if (!outs.empty()) {
                        const Value *ansVal = scope->getLocal("ans");
                        outs[0] = (ansVal && !ansVal->isUnset() && !ansVal->isDeleted())
                                      ? *ansVal
                                      : Value();
                    }
                });
                MFileCacheEntry ce;
                ce.fullPath = cachePath.clone();
                if (auto st = rp.fs->stat(rp.path)) {
                    ce.mtime = st->mtime;
                    ce.mtimeNs = st->mtimeNs;
                    ce.fileSize = st->size;
                }
                ce.sourceCode = keel::move(srcPtr);
                ce.hasExternal = true;
                mFileCache_.put(keel::String::from(name), keel::move(ce));
                bumpCallCacheEpoch();
                return nullptr;
            }
            continue;
        }

        // Build UserFunction (mirrors TreeWalker::execFunctionDef). We
        // store under the QUALIFIED key (`name`) so multiple packages
        // can host functions with the same leaf without colliding.
        UserFunction func;
        func.name = keel::String::from(name);
        func.params = funcDef->paramNames.clone();
        func.returns = funcDef->returnNames.clone();
        func.body = keel::Arc<const ASTNode>(cloneNode(funcDef->children[0].get()));
        func.closureEnv = nullptr;
        func.originKnown = true;
        func.sourceFs = keel::String::from(rpFsName);
        func.sourceDir = scriptDir.clone();
        func.sourceFile = keel::String::from(rpPath);

        // Register for VM dispatch (mirrors what execFunctionDef +
        // beginScript pre-compile pass do for in-script function defs).
        // Bind under the QUALIFIED name so two packages with the same
        // leaf (`+a/foo.m` and `+b/foo.m`) don't collide in
        // compiledFuncs_. registerFunctionAs also writes
        // engine_.userFuncs_[qualified] — but we still set it
        // explicitly below so non-VM-backed calls work even when the
        // compiler is unavailable or rejects the chunk.
        if (compiler_) {
            try {
                // Provenance rides along so anonymous functions compiled
                // inside the body capture THIS file (their origin frame
                // is not active yet at resolve-time compilation).
                compiler_->registerFunctionAs(name, funcDef, rpFsName,
                                              scriptDir, rpPath);
            } catch (const std::exception &) {
                // Defer compile errors (e.g. bare 'end') until actual invocation.
            }
        }
        userFuncs_.put(keel::String::from(name), keel::move(func));
        if (compiler_)
            compiler_->setChunkOrigin(name, rpFsName, scriptDir, rpPath);

        // Cache stat metadata for mtime-based invalidation.
        MFileCacheEntry e;
        e.fullPath = cachePath.clone();
        if (auto st = rp.fs->stat(rp.path)) {
            e.mtime = st->mtime;
            e.mtimeNs = st->mtimeNs;
            e.fileSize = st->size;
        }
        e.sourceCode = keel::make_arc<const keel::String>(keel::move(content));
        mFileCache_.put(keel::String::from(name), keel::move(e));
        bumpCallCacheEpoch();

        // Register the file's OTHER top-level functions as local
        // (sub)functions — a `.m` file may define a main function followed by
        // helper functions that the main one calls (a core MATLAB feature).
        // We register them under their bare names so the entry's calls
        // resolve, mirroring how beginScript registers entry-SCRIPT locals.
        // Guard: never shadow an existing builtin / already-loaded function
        // (keeps locals from leaking over the global namespace). File-private
        // scoping is a known simplification — same-named locals in two files
        // resolve to the first loaded.
        if (ast->type == NodeType::BLOCK) {
            for (const auto &c : ast->children) {
                if (!c || c->type != NodeType::FUNCTION_DEF || c.get() == funcDef)
                    continue;
                const keel::String &ln = c->strValue;
                if (ln.empty() || ln == name || ln == leafName)
                    continue;
                if (externalFuncs_.contains(ln) || userFuncs_.contains(ln))
                    continue;
                if (c->children.empty())
                    continue;
                if (compiler_) {
                    try {
                        compiler_->registerFunctionAs(ln, c.get(), rpFsName,
                                                      scriptDir, rpPath);
                    }
                    catch (const std::exception &) { /* TW still dispatches via userFuncs_ */ }
                }
                UserFunction lf;
                lf.name       = ln.clone();
                lf.params     = c->paramNames.clone();
                lf.returns    = c->returnNames.clone();
                lf.body       = keel::Arc<const ASTNode>(cloneNode(c->children[0].get()));
                lf.closureEnv = nullptr;
                lf.originKnown = true;
                lf.sourceFs   = keel::String::from(rpFsName);
                lf.sourceDir  = scriptDir.clone();
                lf.sourceFile = keel::String::from(rpPath);
                userFuncs_.put(ln.clone(), keel::move(lf));
                if (compiler_)
                    compiler_->setChunkOrigin(ln, rpFsName, scriptDir, rpPath);
            }
        }

        return userFuncs_.get(name);
    }
    mFileMissCache_.insert(keel::move(missKey));
    return nullptr;
}

bool Engine::hasExternalFunction(keel::StringRef name) const
{
    if (externalFuncs_.contains(name))
        return true;
    if (name.find('.') != keel::kInvalidIndex) {
        return findExternal(name, workspaceEnv_.get()) != nullptr;
    }
    return false;
}

bool Engine::hasCallableName(keel::StringRef name) const
{
    return externalFuncs_.contains(name)
        || !bareNameSource(name).empty()
        || lookupUserFunctionLocal(name) != nullptr;
}

Value Engine::callFunctionHandle(const Value &handle,
                                  keel::ArrayRef<Value> args,
                                  Environment *env)
{
    auto results = callFunctionHandleMulti(handle, args, 1, env);
    return results.empty() ? Value() : results[0];
}

keel::Array<Value> Engine::callFunctionHandleMulti(const Value &handle,
                                                   keel::ArrayRef<Value> args,
                                                   keel::usize nout,
                                                   Environment *env)
{
    if (!handle.isFuncHandle())
        throw std::runtime_error("callFunctionHandleMulti: argument is not a function handle");
    Environment *e = env ? env : workspaceEnv_.get();

    // TreeWalker mode: dispatch strictly on TreeWalker (which unwraps
    // funcHandleCaptures, resolves externals/imports, and lazy-loads .m files).
    if (backend_ != Backend::VM) {
        if (!treeWalker_)
            throw std::runtime_error("callFunctionHandleMulti: TreeWalker backend is unavailable");
        return treeWalker_->callHandleMultiPublic(handle, args, e, nout);
    }

    // VM mode: dispatch strictly on VM (zero TreeWalker fallback).
    if (!vm_)
        throw std::runtime_error("callFunctionHandleMulti: VM backend is unavailable");

    keel::Array<Value> withCaptures;
    if (handle.funcHandleCaptureCount() > 0) {
        const keel::usize ncaps = handle.funcHandleCaptureCount();
        withCaptures.reserve(args.size() + ncaps);
        for (const auto &a : args)
            withCaptures.push_back(a);
        for (keel::usize i = 0; i < ncaps; ++i)
            withCaptures.push_back(handle.funcHandleCapture(i));
        args = keel::ArrayRef<Value>(withCaptures.data(), withCaptures.size());
    }
    keel::String name = handle.funcHandleName();

    if (const ExternalFunc *fn = findExternal(name, e)) {
        keel::Array<Value> out(nout);
        CallContext ctx{this, e};
        (*fn)(args, nout, keel::MutArrayRef<Value>(out.data(), out.size()), ctx);
        return out;
    }

    if (compiler_) {
        if (const BytecodeChunk *fc = compiler_->findCompiled(name))
            return vm_->callReentrant(*fc, args, nout);
    }

    if (const UserFunction *uf = lookupUserFunction(name, e)) {
        const BytecodeChunk *cc = ensureClassMethodChunk(*uf);
        if (!cc) {
            auto msg = keel::format("Failed to compile function handle target '@{}'", name);
            throw std::runtime_error(msg.c_str());
        }
        return vm_->callReentrant(*cc, args, nout);
    }

    // Re-check externals in case lookupUserFunction lazy-loaded a classdef or script .m file.
    if (const ExternalFunc *fn = findExternal(name, e)) {
        keel::Array<Value> out(nout);
        CallContext ctx{this, e};
        (*fn)(args, nout, keel::MutArrayRef<Value>(out.data(), out.size()), ctx);
        return out;
    }

    auto msg = keel::format("Undefined function in handle: @{}", name);
    throw std::runtime_error(msg.c_str());
}

bool Engine::isInsideFunctionCall() const
{
    if (vm_ && backend_ == Backend::VM)
        return vm_->callDepth() > 0;
    if (treeWalker_)
        return treeWalker_->callDepth() > 0;
    return false;
}

// ── Frame stack introspection ───────────────────────────────────

int Engine::callerDepth() const
{
    if (vm_ && backend_ == Backend::VM)
        return vm_->callDepth();
    if (treeWalker_)
        return static_cast<int>(treeWalker_->activeFrames().size());
    return 0;
}

Environment *Engine::callerEnv(int n)
{
    if (n < 0) n = 0;
    if (vm_ && backend_ == Backend::VM)
        return vm_->callerEnvAtDepth(n);
    if (treeWalker_) {
        const auto &frames = treeWalker_->activeFrames();
        if (n < static_cast<int>(frames.size()))
            return frames[frames.size() - 1 - n].env;
        return workspaceEnv_.get();
    }
    return workspaceEnv_.get();
}

void Engine::assignToCaller(keel::i32 n, keel::StringRef name, Value val)
{
    Environment *env = callerEnv(n);
    if (!env) env = workspaceEnv_.get();
    // Write-through to register if VM caller frame has the name in its
    // varMap (so static reads pick up the new value).
    if (vm_ && backend_ == Backend::VM)
        vm_->assignInCallerFrame(n, name, val);
    env->set(name, keel::move(val));
}

void Engine::assignInBase(keel::StringRef name, Value val)
{
    if (vm_ && backend_ == Backend::VM)
        vm_->writeToFrameMatchingEnv(workspaceEnv_.get(), name, val);
    workspaceEnv_->set(name, keel::move(val));
}

void Engine::syncFrameToEnv(Environment *env)
{
    if (vm_ && backend_ == Backend::VM)
        vm_->syncFrameToEnv(env);
}

void Engine::syncEnvRemovalsToFrame(Environment *env)
{
    if (vm_ && backend_ == Backend::VM)
        vm_->syncEnvRemovalsToFrame(env);
}

void Engine::clearPersistentForFunc(keel::StringRef funcName)
{
    if (auto *bp = persistentStore_.get(funcName); bp && *bp)
        (*bp)->clear();
    const keel::String prefix = keel::format("{}>", funcName);
    for (auto [key, bucket] : persistentStore_) {
        if (bucket && key.starts_with(prefix))
            bucket->clear();
    }
}

keel::String Engine::getEnvVar(keel::StringRef name) const
{
    if (const auto *v = envOverlay_.get(name))
        return v->clone();
    return numkit::envGet(name);
}

keel::String Engine::inputName(int k)
{
    if (k < 1)
        throw std::runtime_error("inputname: argument index must be >= 1");
    if (callerDepth() < 1)
        throw std::runtime_error(
            "inputname: must be called from within a function");

    if (vm_ && backend_ == Backend::VM) {
        const auto names = vm_->currentFrameCallerArgNames();
        if (k > static_cast<int>(names.size()))
            return {};  // beyond known names: arg wasn't recorded
        return names[k - 1].clone();
    }
    if (treeWalker_) {
        const auto &frames = treeWalker_->activeFrames();
        if (frames.empty()) return {};
        const auto &names = frames.back().callerArgNames;
        if (k > static_cast<int>(names.size()))
            return {};
        return names[k - 1].clone();
    }
    return {};
}

void Engine::clearUserFunctions()
{
    userFuncs_.clear();
    clearAllPersistent();
    if (compiler_)
        compiler_->clearCompiledFuncs();
    bumpCallCacheEpoch();
}

void Engine::beginScript(const ASTNode *ast)
{
    savedScriptLocalUserFuncs_.push_back(keel::move(scriptLocalUserFuncs_));
    scriptLocalUserFuncs_.clear();
    if (compiler_)
        compiler_->beginScriptScope();
    bumpCallCacheEpoch();
    if (!ast)
        return;
    // Pre-register the script's top-level FUNCTION_DEFs into the
    // (now-active) script-local buckets so forward references inside
    // the script resolve on the active backend.
    auto adoptLocal = [&](const ASTNode *f) {
        if (!f || f->type != NodeType::FUNCTION_DEF || f->children.empty())
            return;
        if (backend_ == Backend::VM && compiler_) {
            try {
                compiler_->registerFunction(f);
                return;
            } catch (const std::exception &) {
                // Defer compile-time errors (e.g. bare 'end') until called.
            }
        }
        UserFunction uf;
        uf.name = f->strValue.clone();
        uf.params = f->paramNames.clone();
        uf.returns = f->returnNames.clone();
        uf.body = keel::Arc<const ASTNode>(cloneNode(f->children[0].get()));
        uf.closureEnv = nullptr;
        keel::String key = uf.name.clone();
        scriptLocalUserFuncs_.put(keel::move(key), keel::move(uf));
    };
    if (ast->type == NodeType::BLOCK) {
        for (const auto &c : ast->children)
            adoptLocal(c.get());
    } else {
        adoptLocal(ast);
    }
}

void Engine::endScript()
{
    if (compiler_)
        compiler_->endScriptScope();
    bumpCallCacheEpoch();
    if (savedScriptLocalUserFuncs_.empty()) {
        scriptLocalUserFuncs_.clear();
        return;
    }
    scriptLocalUserFuncs_ = keel::move(savedScriptLocalUserFuncs_.back());
    savedScriptLocalUserFuncs_.pop_back();
}

void Engine::promoteScriptLocalsToWorkspace()
{
    for (auto [k, v] : scriptLocalUserFuncs_) {
        if (backend_ != Backend::VM && compiler_)
            compiler_->eraseCompiledFunc(k);
        userFuncs_.put(k.clone(), keel::move(v));
    }
    scriptLocalUserFuncs_.clear();
    if (compiler_)
        compiler_->promoteScriptLocalsToWorkspace();
    bumpCallCacheEpoch();
}

void Engine::setDebugObserver(keel::Arc<DebugObserver> observer)
{
    debugObserver_ = keel::move(observer);
    if (debugObserver_)
        debugController_ = keel::make_unique<DebugController>(debugObserver_.get(), &breakpointManager_);
    else
        debugController_.reset();
}

// ============================================================
// eval
// ============================================================

// Compile one AST subtree into a chunk and run it on the VM, syncing
// modified registers to workspaceEnv before returning. Used by eval() when
// executing a single statement (or a whole single-expression chunk).
Value Engine::runOneChunk(const ASTNode *ast, keel::Arc<const keel::String> src)
{
    clearAllCalled_ = false;
    vm_->clearLastVarMap();

    auto chunk = compiler_->compile(ast, src);
    vm_->setCompiledFuncs(&compiler_->compiledFuncs(),
                          &compiler_->scriptLocalCompiledFuncs());

    // Register this chunk's OWN `global X` declarations (NOT the inherited
    // globalNames seed — iterating that would resurrect a global cleared
    // earlier in the same chunk, e.g. `clear global g`) as base-workspace
    // membership (workspaceEnv_->globals_, the single source of truth) so the
    // next chunk's compile can see them and reads/writes route through
    // globalsEnv_. A freshly declared global with no value yet IS [] (matches
    // TreeWalker::execGlobalPersistent and MATLAB: visible to who/exist, reads
    // as an empty matrix). Idempotent.
    auto updateTopLevelGlobals = [&]() {
        for (auto &g : chunk.ownGlobalDecls) {
            workspaceEnv_->declareGlobal(g);
            if (!globalsEnv_->get(g))
                globalsEnv_->set(g, Value::Empty);
        }
    };

    try {
        Value result = vm_->execute(chunk);
        syncVMToWorkspace();
        updateTopLevelGlobals();
        return result;
    } catch (const DebugStopException &) {
        syncVMToWorkspace();
        updateTopLevelGlobals();
        throw;
    } catch (...) {
        syncVMToWorkspace();
        updateTopLevelGlobals();
        throw;
    }
}

// When the eval-builtin's caller captures the result (`r = eval(...)`),
// MATLAB suppresses any "ans = ..." or lhs-display the inner code would
// otherwise emit. We honour that by flipping `suppressOutput=true` on
// each top-level statement before TW/VM gets to it — both backends
// already gate their DISPLAY emission on that flag, so this single hook
// covers ASSIGN, EXPR_STMT, FIELD_ASSIGN, CELL_ASSIGN, etc. Side-effect
// prints inside called functions (disp, fprintf, ...) are unaffected:
// those originate inside CALL nodes whose own statement-level suppress
// flag we don't touch.
static void markTopLevelSuppressed(ASTNode *ast)
{
    if (!ast) return;
    if (ast->type == NodeType::BLOCK) {
        for (auto &c : ast->children)
            if (c) c->suppressOutput = true;
    } else {
        ast->suppressOutput = true;
    }
}

// MATLAB (probed R2025b): a statement like `N-1` / `x -1` / `a/2` —
// IDENT, glued - or /, operand — lexes as a COMMAND_CALL (the `which -all`
// idiom), but when the head names a WORKSPACE VARIABLE the statement is the
// binary subtraction/division (x=5; x -1 -> 4). The parser cannot see
// variables, so rewrite such top-level nodes into real BINARY_OP ASTs here,
// BEFORE compiling — both backends then run the plain expression path with
// correct statement-value/ans semantics. (bugs: eval-family sibling found
// it via eval('N/2').)
void Engine::rewriteVarHeadedCommands(ASTNode *root)
{
    if (!root) return;
    keel::Array<ASTNode *> stmts;
    if (root->type == NodeType::BLOCK) {
        for (auto &c : root->children)
            if (c) stmts.push_back(c.get());
    } else {
        stmts.push_back(root);
    }
    for (ASTNode *node : stmts) {
        if (!node || node->type != NodeType::COMMAND_CALL)
            continue;
        if (node->children.size() != 1
            || node->children[0]->type != NodeType::STRING_LITERAL)
            continue;
        const keel::String &name = node->strValue;
        const keel::String &arg = node->children[0]->strValue;
        if (arg.size() < 2 || (arg[0] != '-' && arg[0] != '/'))
            continue;
        if (!std::isalnum(static_cast<unsigned char>(arg[1])) && arg[1] != '.')
            continue;
        if (!getVariable(name) && !isReservedName(name))
            continue;
        const keel::String tail = keel::String::from(arg.substr(1));
        char *endPtr = nullptr;
        errno = 0;
        double parsedNum = 0.0;
        bool isNum = false;
        if (!tail.empty() && tail != ".") {
            parsedNum = std::strtod(tail.c_str(), &endPtr);
            isNum = (errno == 0 && endPtr == tail.c_str() + tail.size());
        }
        bool isIdent = !tail.empty()
                       && (std::isalpha(static_cast<unsigned char>(tail[0]))
                           || tail[0] == '_');
        if (isIdent)
            for (char c : tail)
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                    { isIdent = false; break; }
        if (!isNum && !isIdent) continue;
        const int ln = node->line, cl = node->col;
        const bool suppress = node->suppressOutput;
        auto ident = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
        ident->strValue = name.clone();
        ident->line = ln; ident->col = cl;
        ASTNodePtr operand;
        if (isNum) {
            operand = keel::make_unique<ASTNode>(NodeType::NUMBER_LITERAL);
            operand->numValue = parsedNum;
        } else {
            operand = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
            operand->strValue = tail.clone();
        }
        operand->line = ln; operand->col = cl;
        // Replicate parseExpressionStatement's shape: the statement
        // becomes EXPR_STMT(BINARY_OP) so the expr-statement compiler
        // binds/displays/returns the value like any expression
        // statement (a bare BINARY_OP statement skips that path and
        // yields an empty chunk result).
        auto bin = keel::make_unique<ASTNode>(NodeType::BINARY_OP);
        bin->strValue.assign((arg[0] == '-') ? "-" : "/");
        bin->line = ln; bin->col = cl;
        bin->children.push_back(keel::move(ident));
        bin->children.push_back(keel::move(operand));
        node->type = NodeType::EXPR_STMT;
        node->strValue.clear();
        node->children.clear();
        node->children.push_back(keel::move(bin));
        node->suppressOutput = suppress;
    }
}
Value Engine::eval(keel::StringRef code, bool suppressTopLevelDisplay)
{
    auto src = keel::make_arc<const keel::String>(keel::String::from(code));
    refreshStaleMFiles();
    if (backend_ == Backend::VM && vm_)
        vm_->flushWorkspaceFrame();

    Lexer lexer(*src);
    auto tokens = lexer.tokenize();
    Parser parser(keel::move(tokens));
    auto ast = parser.parse();
    if (suppressTopLevelDisplay)
        markTopLevelSuppressed(ast.get());

    // A "script" here = a BLOCK that mixes FUNCTION_DEFs with
    // executable statements (the shape of a .m file with local
    // helper functions after the script body). Those FUNCTION_DEFs
    // are file-local and vanish on return — MATLAB script
    // semantics. Any other shape — a pure-statement paste, a lone
    // `function ...`, or a batch of function defs at the REPL —
    // registers functions into the workspace bucket so subsequent
    // evals can see them.
    bool hasFunc = false, hasStmt = false;
    if (ast && ast->type == NodeType::BLOCK) {
        for (auto &c : ast->children) {
            if (!c) continue;
            if (c->type == NodeType::FUNCTION_DEF) hasFunc = true;
            else hasStmt = true;
        }
    }
    const bool isScript = hasFunc && hasStmt;
    if (isScript)
        beginScript(ast.get());
    // At eval exit, promote script-locals into the workspace
    // before tearing down the scope — matches the engine's
    // established REPL contract where defining a function and
    // calling it in the same paste keeps the function around for
    // later evals. DebugSession's own beginScript path doesn't
    // call this promotion, so .m file-local helpers stay file-local.
    struct ScriptEndGuard {
        Engine &e;
        bool armed;
        ~ScriptEndGuard() {
            if (armed) {
                e.promoteScriptLocalsToWorkspace();
                e.endScript();
            }
        }
    } _scriptGuard{*this, isScript};

    // TreeWalker already executes top-level BLOCK statements sequentially
    // against `workspaceEnv_`, so its behaviour matches MATLAB's script
    // semantics out of the box — no split needed here.
    if (backend_ != Backend::VM)
        return treeWalker_->execute(ast.get(), workspaceEnv_.get());

    // VM: a whole multi-statement script compiled as a single chunk keeps
    // its variables in chunk-local registers and only commits them to
    // workspaceEnv on completion. That leaves mid-script `whos` / `clear x`
    // blind to the running state. Match MATLAB by executing every top-level
    // statement as its own mini-chunk, with a sync in between.
    //
    // An attached debug observer runs the whole eval as one chunk: the
    // observer expects step/line semantics to correspond to the source as
    // a unit, and a split would re-fire initial-stop events between every
    // top-level statement.
    const bool splittable = !debugObserver_ && ast
                            && ast->type == NodeType::BLOCK
                            && ast->children.size() > 1;
    if (splittable) {
        // Pre-compile FUNCTION_DEF children: the per-statement
        // loop below skips them (they're definitions, not stmts),
        // so if nothing else compiles them they'd be unreachable.
        // In script mode, beginScript has already routed them into
        // scriptLocalCompiledFuncs_. Outside script mode we register
        // into the workspace bucket so REPL-style forward references
        // keep working.
        if (!isScript) {
            for (auto &c : ast->children) {
                if (c && c->type == NodeType::FUNCTION_DEF)
                    compiler_->registerFunction(c.get());
            }
        }
        Value result = Value();
        for (auto &c : ast->children) {
            if (!c || c->type == NodeType::FUNCTION_DEF)
                continue;
            rewriteVarHeadedCommands(c.get());
            result = runOneChunk(c.get(), src);
        }
        return result;
    }

    // Single-statement path: works for REPL lines, lone expressions, and
    // scripts consisting of just one top-level construct.
    rewriteVarHeadedCommands(ast.get());
    return runOneChunk(ast.get(), src);
}

Value Engine::evalStatement(const ASTNode *stmt,
                            keel::Arc<const keel::String> src)
{
    if (!stmt)
        return Value();
    rewriteVarHeadedCommands(const_cast<ASTNode *>(stmt));
    return runOneChunk(stmt, keel::move(src));
}

// ============================================================
// evalSafe
// ============================================================
Engine::EvalResult Engine::evalSafe(keel::StringRef code)
{
    EvalResult r;
    try {
        r.value = eval(code);
    } catch (const DebugStopException &) {
        r.ok = false;
        r.debugStop = true;
    } catch (const Error &e) {
        r.ok = false;
        r.errorMessage.assign(e.what());
        r.errorId = e.identifier().clone();
        r.errorLine = e.line();
        r.errorCol = e.col();
        r.errorFunc = e.funcName().clone();
        r.errorContext = e.context().clone();
    } catch (const std::exception &e) {
        r.ok = false;
        r.errorMessage.assign(e.what());
    } catch (...) {
        r.ok = false;
        r.errorMessage.assign("Unknown exception");
    }
    return r;
}

// ============================================================
// VM → workspaceEnv sync
// ============================================================
ExecStatus Engine::debugResume(DebugAction action)
{
    // The debugger runs exclusively on the bytecode VM — pause/step live in the
    // VM dispatch loop and DebugSession drives vm_->startExecution/resumeExecution.
    // The TreeWalker only keeps the debug call-frame stack in sync (no breakpoint
    // pausing). vm_ is always constructed, so this is a defensive invariant.
    if (!vm_)
        throw Error("debugger requires the VM backend", 0, 0, "",
                    "", "numkit:debug:requiresVM");
    if (!vm_->isPaused())
        return ExecStatus::Completed;   // not paused → nothing to resume

    // Set the resume action on the debug controller
    if (debugController_)
        debugController_->setResumeAction(action, vm_->callDepth());

    ExecStatus status = vm_->resumeExecution();

    // Sync variables on completion
    if (status == ExecStatus::Completed)
        syncVMToWorkspace();

    return status;
}

void Engine::syncVMToWorkspace()
{
    if (clearAllCalled_) {
        workspaceEnv_->clearAll();
        if (vm_ && backend_ == Backend::VM)
            vm_->syncEnvRemovalsToFrame(workspaceEnv_.get());
        clearAllCalled_ = false;
    }
    for (const auto &[name, val] : vm_->lastVarMap()) {
        if (val.isUnset() || val.isDeleted()) {
            workspaceEnv_->remove(name);
            if (vm_ && backend_ == Backend::VM)
                vm_->writeToFrameMatchingEnv(workspaceEnv_.get(), name, val);
        } else {
            Value *gsVal = workspaceEnv_->isGlobal(name) ? globalsEnv_->get(name) : nullptr;
            const Value &finalVal = gsVal ? *gsVal : val;
            workspaceEnv_->set(name, finalVal);
            if (vm_ && backend_ == Backend::VM)
                vm_->writeToFrameMatchingEnv(workspaceEnv_.get(), name, finalVal);
        }
    }
}

void Engine::syncVMToScope(Environment *scope)
{
    if (!scope || scope == workspaceEnv_.get()) {
        syncVMToWorkspace();
        return;
    }
    if (clearAllCalled_) {
        scope->clearAll();
        if (vm_ && backend_ == Backend::VM)
            vm_->syncEnvRemovalsToFrame(scope);
        clearAllCalled_ = false;
    }
    for (const auto &[name, val] : vm_->lastVarMap()) {
        if (val.isUnset() || val.isDeleted()) {
            scope->remove(name);
            if (vm_ && backend_ == Backend::VM)
                vm_->writeToFrameMatchingEnv(scope, name, val);
            continue;
        }
        scope->set(name, val);
        // VM mode: write-through to the scope-owning frame's static
        // register slot if any, so subsequent register-based reads in
        // the caller pick up the value.
        if (vm_ && backend_ == Backend::VM)
            vm_->writeToFrameMatchingEnv(scope, name, val);
    }
}

Value Engine::eval(keel::StringRef code, Environment *scope,
                   bool suppressTopLevelDisplay)
{
    if (!scope || scope == workspaceEnv_.get())
        return eval(code, suppressTopLevelDisplay);

    // Scoped re-entrant eval: inner script's imports go to scope, and
    // its top-level variable assignments are pushed back into scope on
    // exit (with VM register write-through where applicable). Used by
    // evalin, and by eval/run when called from inside a user function
    // (so script-defined vars stay scoped to the caller's frame).
    Lexer lexer(code);
    auto tokens = lexer.tokenize();
    Parser parser(keel::move(tokens));
    auto ast = parser.parse();
    auto src = keel::make_arc<const keel::String>(keel::String::from(code));
    if (suppressTopLevelDisplay)
        markTopLevelSuppressed(ast.get());

    if (backend_ != Backend::VM) {
        // TW already executes statements against the env passed in;
        // imports and assignments naturally land in `scope`.
        return treeWalker_->execute(ast.get(), scope);
    }

    // Pre-populate inner top-level's dynVars with caller's variables
    // (registers + existing overlay + env-resident vars) so
    // ASSERT_DEF fallback resolves bare identifiers that live in the
    // caller's scope. Map lives on this C++ stack frame — must
    // outlive vm_->execute(chunk).
    // NOTE: snapshot BEFORE updating inheritedScope_ so frames_[0] (the
    // outer script frame) is not mistaken for `scope` when `scope` belongs
    // to a user-function frame.
    auto callerSnapshot = vm_->snapshotFrameVars(scope);
    // Also include vars set in scope.env directly (e.g. by assignin
    // when the name wasn't in the caller's static varMap). Env values
    // lose to register values when both exist (registers are more
    // up-to-date for static caller writes).
    scope->forEachLocal([&](const keel::String &n, const Value &v) {
        if (v.isUnset() || v.isDeleted()) return;
        callerSnapshot.try_insert(n.clone(), v);
    });
    vm_->setNextFrameDynVars(callerSnapshot.empty() ? nullptr : &callerSnapshot);

    Environment *prevInherited = vm_->inheritedScope_;
    bool prevClearAll = clearAllCalled_;
    struct ScopedEvalGuard {
        Engine *eng;
        VM *vm;
        Environment *prevInherited;
        bool prevClearAll;
        ~ScopedEvalGuard() {
            if (vm) {
                vm->setNextFrameDynVars(nullptr);
                vm->inheritedScope_ = prevInherited;
            }
            eng->clearAllCalled_ = prevClearAll;
        }
    } guard{this, vm_.get(), prevInherited, prevClearAll};

    vm_->inheritedScope_ = scope;
    clearAllCalled_ = false;
    vm_->clearLastVarMap();

    auto chunk = compiler_->compile(ast.get(), src);
    vm_->setCompiledFuncs(&compiler_->compiledFuncs(),
                          &compiler_->scriptLocalCompiledFuncs());

    Value result;
    try {
        result = vm_->execute(chunk);
        vm_->setNextFrameDynVars(nullptr);
        vm_->inheritedScope_ = prevInherited;
        syncVMToScope(scope);
    } catch (...) {
        vm_->setNextFrameDynVars(nullptr);
        vm_->inheritedScope_ = prevInherited;
        syncVMToScope(scope);
        throw;
    }

    return result;
}

// ============================================================
// REPL helpers
// ============================================================
keel::Array<keel::String> Engine::workspaceVarNames() const
{
    // `localNames()` only returns variables that were written into the base
    // workspace — built-in constants live in `constantsEnv_` (a parent env)
    // and don't appear here unless the user has explicitly shadowed them,
    // which matches MATLAB's `who`/`whos` behaviour.
    auto names = workspaceEnv_->localNames();
    std::sort(names.begin(), names.end());
    return names;
}

// ============================================================
// Virtual filesystem registry + path resolver
//
// State + logic live in FsContext (fs/, L0). Engine HAS-A FsContext and
// forwards; resolvePath additionally rethrows the fs-layer std::runtime_error
// as numkit::Error so the MATLAB-visible error type is unchanged for
// in-engine callers.
// ============================================================

void Engine::registerVirtualFS(keel::UniquePtr<VirtualFS> fs)
{
    fsCtx_.registerVirtualFS(keel::move(fs));
    bumpCallCacheEpoch();
}

VirtualFS *Engine::findVirtualFS(keel::StringRef name) const
{
    return fsCtx_.findVirtualFS(name);
}

void Engine::pushScriptOrigin(keel::StringRef fsName)
{
    fsCtx_.pushScriptOrigin(fsName);
}

void Engine::pushScriptOrigin(keel::StringRef fsName, keel::StringRef scriptDir)
{
    fsCtx_.pushScriptOrigin(fsName, scriptDir);
}

void Engine::pushScriptOrigin(keel::StringRef fsName, keel::StringRef scriptDir,
                              keel::StringRef scriptFile)
{
    fsCtx_.pushScriptOrigin(fsName, scriptDir, scriptFile);
}

void Engine::popScriptOrigin()
{
    fsCtx_.popScriptOrigin();
}

const keel::String *Engine::currentScriptOrigin() const
{
    return fsCtx_.currentScriptOrigin();
}

const keel::String *Engine::currentScriptDir() const
{
    return fsCtx_.currentScriptDir();
}

const keel::String *Engine::currentScriptFile() const
{
    return fsCtx_.currentScriptFile();
}

Engine::ResolvedPath Engine::resolvePath(keel::StringRef userPath) const
{
    // FsContext throws std::runtime_error for unknown/unavailable
    // filesystems; rethrow as numkit::Error so the MATLAB-visible error type
    // (and message) is exactly what in-engine callers saw before.
    try {
        ResolvedPath rp = fsCtx_.resolvePath(userPath);
        if ((!rp.fs || !rp.fs->exists(rp.path)) &&
            !userPath.empty() &&
            !hasPathSepOrColon(userPath)) {
            if (rp.fs && rp.fs->exists(userPath)) {
                rp.path = keel::String::from(userPath);
                return rp;
            }
            for (const auto &dir : mPath_) {
                if (dir.empty()) continue;
                keel::String candPath = dir.clone();
                if (candPath.back() != '/' && candPath.back() != '\\')
                    candPath.push_back('/');
                candPath.append(userPath);
                ResolvedPath cand = fsCtx_.resolvePath(candPath);
                if (cand.fs && cand.fs->exists(cand.path))
                    return cand;
                if (rp.fs && rp.fs->exists(candPath)) {
                    rp.path = keel::move(candPath);
                    return rp;
                }
            }
        }
        return rp;
    } catch (const std::runtime_error &e) {
        throw Error(e.what());
    }
}

// ============================================================
// File descriptor table — MATLAB fopen/fclose/fprintf plumbing
// ============================================================


} // namespace numkit
