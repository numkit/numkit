// src/core/engine_object.cpp
// Engine object model: BuiltinClass registry, display, operator & subsref
// dispatch, object-array slice store, and classdef registration/access control.
#include <keel/array.h>
#include <cstdio>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/small_array.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/error.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/tree_walker.hpp>
#include <numkit/core/vm.hpp>
#include "engine_internal.hpp"
#include <algorithm>

namespace numkit {

void Engine::registerClass(BuiltinClass cls)
{
    if (classes_.contains(cls.name)) {
        auto msg = keel::format("Engine::registerClass: duplicate class '{}'", cls.name);
        throw numkit::Error(msg.c_str());
    }
    keel::String key = cls.name.clone();
    classes_.put(keel::move(key), keel::make_arc<BuiltinClass>(keel::move(cls)));
    bumpCallCacheEpoch();
}

const BuiltinClass *Engine::findClass(keel::StringRef name) const
{
    if (const auto *entry = classes_.get(name))
        return entry->get();
    return nullptr;
}

keel::String Engine::formatObjectDisplay(keel::StringRef name, const Value &obj) const
{
    const keel::StringRef clsName = obj.objectClassNameRef();
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
    const BuiltinClass *cls = findClass(obj.objectClassNameRef());
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
    const keel::StringRef clsName = dom.objectClassNameRef();
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
    throw numkit::Error(msg.c_str());
}

bool Engine::tryObjectUnaryOp(keel::StringRef op, const Value &operand,
                              Environment *env, Value &out)
{
    if (!operand.isObject())
        return false;
    const keel::StringRef clsName = operand.objectClassNameRef();
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
    throw numkit::Error(msg.c_str());
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
    throw numkit::Error(msg.c_str());
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
    throw numkit::Error(msg.c_str());
}

const BytecodeChunk *Engine::resolveBinaryOpChunk(keel::StringRef op, const Value &lhs,
                                                  const Value &rhs, keel::String &ownerClassOut)
{
    if (!lhs.isObject() && !rhs.isObject())
        return nullptr; // numeric fast path
    const Value &dom = lhs.isObject() ? lhs : rhs; // first object operand wins
    const keel::StringRef cn = dom.objectClassNameRef();
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
    const keel::StringRef cn = operand.objectClassNameRef();
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
    const keel::StringRef cn = self.objectClassNameRef();
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
    const keel::StringRef cn = self.objectClassNameRef();
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
    const BuiltinClass *cls = findClass(self.objectClassNameRef());
    if (!cls || !cls->subsref)
        return false;
    Value res[1];
    CallContext ctx{this, env};
    cls->subsref(self, args, nargout, keel::MutArrayRef<Value>(res, 1), ctx);
    out = keel::move(res[0]);
    return true;
}

bool Engine::tryObjectSubsasgn(Value &self, keel::ArrayRef<Value> idxAndVal, Environment *env)
{
    const BuiltinClass *cls = findClass(self.objectClassNameRef());
    if (!cls || !cls->subsasgn)
        return false;
    Value res[1];
    CallContext ctx{this, env};
    cls->subsasgn(self, idxAndVal, 0, keel::MutArrayRef<Value>(res, 1), ctx);
    return true;
}

Value Engine::objectPropGet(const Value &obj, keel::StringRef name, Environment *env)
{
    const keel::StringRef clsName = obj.objectClassNameRef();
    const BuiltinClass *cls = findClass(clsName);
    if (cls) {
        CallContext ctx{this, env};
        if (cls->propGet) {
            Value out;
            if (cls->propGet(obj, name, out, ctx))
                return out;
        }
        // Bare `obj.method` (no parens) invokes a zero-arg method.
        if (const auto *m = cls->methods.get(name)) {
            Value self = obj;
            Value outBuf[1];
            (*m)(self, keel::ArrayRef<Value>(nullptr, 0), 1,
                 keel::MutArrayRef<Value>(outBuf, 1), ctx);
            return outBuf[0];
        }
    }
    auto msg = keel::format("No appropriate property '{}' for class '{}'", name, clsName);
    throw numkit::Error(msg.c_str());
}

void Engine::objectPropSet(Value &objSlot, keel::StringRef name,
                           const Value &rhs, Environment *env)
{
    const BuiltinClass *cls = findClass(objSlot.objectClassNameRef());
    if (cls && cls->propSet) {
        CallContext ctx{this, env};
        if (cls->propSet(objSlot, name, rhs, ctx))
            return;
    }
    auto msg = keel::format("Cannot set property '{}' on class '{}'",
                            name, objSlot.objectClassNameRef());
    throw numkit::Error(msg.c_str());
}

void Engine::objectElemPropSet(Value &objSlot, keel::usize linearIdx, keel::StringRef name,
                               const Value &rhs, Environment *env)
{
    if (linearIdx >= objSlot.objectCount())
        throw numkit::Error("Index exceeds object array bounds");
    Value elem = objSlot.objectSubArray(keel::ArrayRef<keel::usize>(&linearIdx, 1), mr_);
    objectPropSet(elem, name, rhs, env);
    if (!objSlot.objectIsHandle())
        objSlot.objectAssignLinear(keel::ArrayRef<keel::usize>(&linearIdx, 1), elem, elem, mr_);
}

bool Engine::tryObjectSubscriptAssign(keel::ArrayRef<Value> idxArgs, const Value &rhs,
                                      Environment *env, Value &out)
{
    for (keel::usize i = 0; i < idxArgs.size(); ++i) {
        if (!idxArgs[i].isObject())
            continue;
        const BuiltinClass *cls = findClass(idxArgs[i].objectClassNameRef());
        const ObjectMethod *m = cls ? cls->methods.get("__indexAssign") : nullptr;
        if (!m)
            continue;
        keel::Array<Value> callArgs;
        callArgs.reserve(idxArgs.size() + 1);
        callArgs.push_back(rhs);
        for (keel::usize j = 0; j < idxArgs.size(); ++j)
            callArgs.push_back(idxArgs[j]);
        Value self = idxArgs[i];
        Value res[1];
        CallContext ctx{this, env};
        (*m)(self, callArgs.as_ref(), 1, keel::MutArrayRef<Value>(res, 1), ctx);
        out = keel::move(res[0]);
        return true;
    }
    return false;
}

bool Engine::prepareObjectArrayStore(Value &dst, const Value &rhs) const
{
    const bool canPromote =
        dst.isObject() && !rhs.isObject()
        && (rhs.isNumeric() || rhs.isLogical())
        && dst.objectStateConst() && dst.objectStateConst()->native
        && dst.objectStateConst()->native->fromScalarDouble(0.0) != nullptr;
    if (!rhs.isObject() && !canPromote)
        return false;
    const BuiltinClass *rhsCls =
        rhs.isObject() ? findClass(rhs.objectClassNameRef()) : nullptr;
    if (!dst.isObject() && dst.isNumeric() && rhsCls
        && !rhsCls->heterogeneousGroup.empty()) {
        dst = Value();
    }
    if (!dst.isObject() && (dst.isUnset() || dst.isEmpty()))
        return true;
    if (canPromote)
        return true;
    if (dst.isObject() && rhs.isObject()) {
        if (dst.objectClassNameRef() == rhs.objectClassNameRef())
            return true;
        const BuiltinClass *dstCls = findClass(dst.objectClassNameRef());
        if (dstCls && rhsCls && !dstCls->heterogeneousGroup.empty()
            && dstCls->heterogeneousGroup == rhsCls->heterogeneousGroup) {
            return true;
        }
    }
    return false;
}

void Engine::objectStoreSlice(Value &dst, keel::ArrayRef<keel::Array<keel::usize>> perDim,
                              const Value &val, Environment *env)
{
    const keel::StringRef clsName = dst.isObject() ? dst.objectClassNameRef() : val.objectClassNameRef();
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
        throw numkit::Error(msg.c_str());
    }
    if (engine->classCtxAllows(declClass, /*privateOnly=*/level == Access::Private))
        return;
    auto msg = keel::format("Cannot access {} '{}' of class '{}': it has {} access",
                            kind, name, declClass, accessWord(level));
    throw numkit::Error(msg.c_str());
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
            throw numkit::Error(msg.c_str());
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
                throw numkit::Error(msg.c_str());
            }
            if (pi->setAccess != Access::Public)
                enforceAccess(ctx.engine, pi->setAccess, pi->declClass, "property", nameRef);
        } else if (!desc->setters.contains(nameRef)
                   && std::find(desc->superclasses.begin(), desc->superclasses.end(),
                                "dynamicprops")
                          == desc->superclasses.end()) {
            auto msg = keel::format("No public property '{}' for class '{}'", nameRef, desc->name);
            throw numkit::Error(msg.c_str());
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
                    keel::i32 n = std::snprintf(numBuf, sizeof(numBuf), "%f", vp->toScalar());
                    if (n > 0) {
                        body.append(": ");
                        body.append(keel::StringRef(numBuf, keel::as<keel::usize>(n)));
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
                && args[0].objectClassNameRef() == args[1].objectClassNameRef()) {
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
            throw numkit::Error("classdef methods require an active VM");
        const BytecodeChunk *cc = ensureClassMethodChunk(uf);
        if (!cc) {
            auto msg = keel::format("Failed to compile classdef method '{}'", uf.name);
            throw numkit::Error(msg.c_str());
        }
        return vm_->callReentrant(*cc, args, std::max<keel::usize>(nout, 1), uf.ownerClass,
                                  /*isCtor=*/false);
    }
    if (treeWalker_) {
        ClassCtxGuard ctx(this, uf.ownerClass, /*isCtor=*/false);
        return treeWalker_->runClassMethod(uf, args, nout);
    }
    throw numkit::Error("classdef methods require the interpreter backend");
}

Value Engine::invokeClassCtor(const UserFunction &ctor, const Value &seed,
                              keel::ArrayRef<Value> args)
{
    if (backend_ == Backend::VM) {
        if (!vm_)
            throw numkit::Error("classdef constructor requires an active VM");
        const BytecodeChunk *cc = ensureClassMethodChunk(ctor);
        if (!cc) {
            auto msg = keel::format("Failed to compile classdef constructor '{}'", ctor.name);
            throw numkit::Error(msg.c_str());
        }
        auto r = vm_->callReentrant(*cc, args, 1, ctor.ownerClass, /*isCtor=*/true, &seed);
        return r.empty() ? seed : keel::move(r[0]);
    }
    if (treeWalker_) {
        ClassCtxGuard ctx(this, ctor.ownerClass, /*isCtor=*/true);
        return treeWalker_->runClassCtor(ctor, seed, args);
    }
    throw numkit::Error("classdef constructor requires the interpreter backend");
}

Value Engine::evalConstExpr(const ASTNode *expr)
{
    if (!expr)
        return Value::Empty;
    if (backend_ == Backend::VM) {
        if (!compiler_ || !vm_)
            throw numkit::Error("evalConstExpr: VM backend is unavailable");
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
    throw numkit::Error("evalConstExpr: interpreter backend is unavailable");
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
            throw numkit::Error(msg.c_str());
        }
        if (pi->setAccess != Access::Public)
            enforceAccess(this, pi->setAccess, pi->declClass, "property", prop);
    } else if (!desc.setters.contains(prop)
               && std::find(desc.superclasses.begin(), desc.superclasses.end(), "dynamicprops")
                      == desc.superclasses.end()) {
        auto msg = keel::format("No public property '{}' for class '{}'", prop, className);
        throw numkit::Error(msg.c_str());
    }
}

Value Engine::superConstruct(keel::StringRef base, const Value &seed,
                             keel::ArrayRef<Value> args)
{
    const auto *dp = classDefs_.get(base);
    if (!dp) {
        auto msg = keel::format("superclass '{}' is not a classdef", base);
        throw numkit::Error(msg.c_str());
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
        throw numkit::Error(msg.c_str());
    }
    const auto &desc = *dp;
    const auto *mp = desc->methods.get(method);
    if (!mp) {
        auto msg = keel::format("superclass '{}' has no method '{}'", base, method);
        throw numkit::Error(msg.c_str());
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
            throw numkit::Error(msg.c_str());
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
        throw numkit::Error(msg.c_str());
    }
    const auto &desc = *dp;
    if (desc->isAbstract) {
        auto msg = keel::format("Cannot instantiate abstract class '{}' (unimplemented abstract method)",
                                className);
        throw numkit::Error(msg.c_str());
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

} // namespace numkit
