// src/core/engine/classdef_register.cpp
// classdef AST registration, BuiltinClass synthesis, unregistration, and
// derived-class re-registration cascade.
#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/small_array.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/error.hpp>
#include "core/compiler/compiler.hpp"
#include "core/engine/engine_internal.hpp"
#include "core/tree_walker/tree_walker.hpp"
#include "core/vm/vm.hpp"

namespace numkit {

namespace {

// Translate an attribute keyword to an Access level. `Immutable` is only
// meaningful for SetAccess. Friend-class access (`Access = ?Foo` / a `{?A,?B}`
// list) is NOT a silent downgrade here — the `?` is rejected at lex time, so
// such a class fails to load loudly and never reaches this function. The
// default below therefore only covers `public` and any access keyword we don't
// yet model, which fall back to Public (permissive, forward-compatible).
Access parseAccessLevel(keel::StringRef v)
{
    if (v == "private")
        return Access::Private;
    if (v == "protected")
        return Access::Protected;
    if (v == "immutable")
        return Access::Immutable;
    return Access::Public; // "public" + any not-yet-modeled keyword
}

// Inverse of parseAccessLevel — the attribute string for reflection
// (metaclass / meta.property / meta.method GetAccess/SetAccess/Access).
const char *accessName(Access a)
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

const keel::String *findAttrValue(const keel::Array<keel::String> &attrs, const char *key)
{
    for (keel::usize i = 0; i + 1 < attrs.size(); ++i)
        if (attrs[i] == key)
            return &attrs[i + 1];
    return nullptr;
}

BlockAccess parseBlockAccess(const keel::Array<keel::String> &attrs)
{
    BlockAccess ba;
    // `Access` is the default for both sides; the specific `GetAccess` /
    // `SetAccess` always override it (order-independent, so resolve the
    // general first, then the specific).
    if (const keel::String *a = findAttrValue(attrs, "Access")) {
        Access lvl = parseAccessLevel(*a);
        ba.get = (lvl == Access::Immutable) ? Access::Public : lvl; // immutable is set-only
        ba.set = lvl;
    }
    if (const keel::String *g = findAttrValue(attrs, "GetAccess")) {
        Access lvl = parseAccessLevel(*g);
        ba.get = (lvl == Access::Immutable) ? Access::Public : lvl;
    }
    if (const keel::String *s = findAttrValue(attrs, "SetAccess"))
        ba.set = parseAccessLevel(*s);
    ba.any = ba.get != Access::Public || ba.set != Access::Public;
    return ba;
}

bool hasAttr(const ASTNode *n, const char *attr)
{
    for (const auto &s : n->classAttrs)
        if (s == attr)
            return true;
    return false;
}

bool isMemberHidden(const ClassDefDesc &desc, keel::StringRef name)
{
    for (const auto &h : desc.hiddenMembers)
        if (h == name)
            return true;
    return false;
}

keel::StringRef leafOfMethodName(keel::StringRef qualified)
{
    const keel::usize sep = qualified.rfind('>');
    return (sep != keel::kInvalidIndex) ? qualified.substr(sep + 1) : qualified;
}

void copyCallResults(keel::ArrayRef<Value> results, keel::usize nout,
                     keel::MutArrayRef<Value> outs)
{
    const keel::usize writeN = keel::min(nout, results.size());
    for (keel::usize i = 0; i < writeN && i < outs.size(); ++i)
        outs[i] = results[i];
}

struct AccessorGuard
{
    Engine *eng;
    AccessorGuard(Engine *e, keel::StringRef accessorName)
        : eng(e)
    {
        eng->pushPropAccessor(accessorName);
    }
    ~AccessorGuard() { eng->popPropAccessor(); }
    AccessorGuard(const AccessorGuard &) = delete;
    AccessorGuard &operator=(const AccessorGuard &) = delete;
};

// ── Named runtime hooks backing synthesized BuiltinClass entries ─────────

bool classPropGet(const ClassDefDesc &desc, const Value &self, keel::StringRef nameRef,
                  Value &out, CallContext &ctx)
{
    if (desc.anyNonPublicProp) {
        if (const PropInfo *pi = findProp(desc, nameRef);
            pi && pi->getAccess != Access::Public) {
            enforceAccess(coreEnginePtr(ctx), pi->getAccess, pi->declClass, "property", nameRef);
        }
    }
    if (const auto *gp = desc.getters.get(nameRef);
        gp && !coreEngine(ctx).isPropAccessorActive((*gp)->name)) {
        AccessorGuard guard(coreEnginePtr(ctx), (*gp)->name);
        auto r = coreEngine(ctx).invokeClassMethod(**gp, keel::ArrayRef<Value>(&self, 1), 1);
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
}

bool classPropSet(const ClassDefDesc &desc, Value &self, keel::StringRef nameRef,
                  const Value &val, CallContext &ctx)
{
    const PropInfo *pi = findProp(desc, nameRef);
    if (pi) {
        if (pi->isConstant) {
            auto msg = keel::format("Cannot set Constant property '{}' of class '{}'",
                                    nameRef, desc.name);
            throw numkit::Error(msg.c_str());
        }
        if (pi->setAccess != Access::Public)
            enforceAccess(coreEnginePtr(ctx), pi->setAccess, pi->declClass, "property", nameRef);
    } else if (!desc.setters.contains(nameRef)
               && keel::find(desc.superclasses.begin(), desc.superclasses.end(), "dynamicprops")
                      == desc.superclasses.end()) {
        auto msg = keel::format("No public property '{}' for class '{}'", nameRef, desc.name);
        throw numkit::Error(msg.c_str());
    }
    if (const auto *sp = desc.setters.get(nameRef);
        sp && !coreEngine(ctx).isPropAccessorActive((*sp)->name)) {
        AccessorGuard guard(coreEnginePtr(ctx), (*sp)->name);
        Value callArgs[2] = {self, val};
        auto r = coreEngine(ctx).invokeClassMethod(**sp, keel::ArrayRef<Value>(callArgs, 2), 1);
        if (!r.empty() && r[0].isObject())
            self = keel::move(r[0]);
        return true;
    }
    self.objectStateMut()->props.put(keel::String::from(nameRef), val);
    return true;
}

Value classConstruct(const ClassDefDesc &desc, keel::ArrayRef<Value> args, CallContext &ctx)
{
    Value obj = coreEngine(ctx).makeDefaultInstance(desc.name);
    if (desc.ctor)
        obj = coreEngine(ctx).invokeClassCtor(*desc.ctor, obj, args);
    return obj;
}

void classInvokeMethod(const ClassDefDesc &desc, const UserFunction &uf,
                       Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    const keel::StringRef mname = leafOfMethodName(uf.name);
    if (const auto *ma = desc.methodAccess.get(mname); ma && ma->level != Access::Public)
        enforceAccess(coreEnginePtr(ctx), ma->level, ma->declClass, "method", mname);
    keel::Array<Value> callArgs;
    callArgs.reserve(args.size() + 1);
    callArgs.push_back(self); // obj is the first parameter
    for (const auto &a : args)
        callArgs.push_back(a);
    const keel::usize nout = keel::max<keel::usize>(nargout, 1);
    auto results = coreEngine(ctx).invokeClassMethod(
        uf, keel::ArrayRef<Value>(callArgs.data(), callArgs.size()), nout);
    copyCallResults(results.as_ref(), nout, outs);
}

void classInvokeOp(const ClassDefDesc &desc, const UserFunction &uf,
                   Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    const keel::StringRef mname = leafOfMethodName(uf.name);
    if (const auto *ma = desc.methodAccess.get(mname); ma && ma->level != Access::Public)
        enforceAccess(coreEnginePtr(ctx), ma->level, ma->declClass, "method", mname);
    keel::Array<Value> callArgs;
    if (args.empty())
        callArgs.push_back(self); // unary: operand came as self
    else
        callArgs.append(args); // binary: [lhs, rhs]
    const keel::usize nout = keel::max<keel::usize>(nargout, 1);
    auto results = coreEngine(ctx).invokeClassMethod(
        uf, keel::ArrayRef<Value>(callArgs.data(), callArgs.size()), nout);
    copyCallResults(results.as_ref(), nout, outs);
}

void classInvokeStatic(const ClassDefDesc &desc, const UserFunction &uf,
                       keel::ArrayRef<Value> args, keel::usize nargout,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    const keel::StringRef sname = leafOfMethodName(uf.name);
    if (const auto *si = desc.statics.get(sname); si && si->access != Access::Public)
        enforceAccess(coreEnginePtr(ctx), si->access, si->declClass, "method", sname);
    const keel::usize nout = keel::max<keel::usize>(nargout, 1);
    auto results = coreEngine(ctx).invokeClassMethod(uf, args, nout);
    copyCallResults(results.as_ref(), nout, outs);
}

void classReadConstant(const ClassDefDesc &desc, keel::usize propIdx,
                       keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    const PropInfo &p = desc.props[propIdx];
    if (p.getAccess != Access::Public)
        enforceAccess(coreEnginePtr(ctx), p.getAccess, p.declClass, "property", p.name);
    outs[0] = p.def;
}

void classSubsref(const UserFunction &uf, Value &self, keel::ArrayRef<Value> args,
                  keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    Value s = buildSubsStruct("()", args, ctx.engine->allocator());
    Value callArgs[2] = {self, s};
    const keel::usize nout = keel::max<keel::usize>(nargout, 1);
    auto results = coreEngine(ctx).invokeClassMethod(uf, keel::ArrayRef<Value>(callArgs, 2), nout);
    copyCallResults(results.as_ref(), nout, outs);
}

void classSubsasgn(const UserFunction &uf, Value &self, keel::ArrayRef<Value> args,
                   CallContext &ctx)
{
    // Hook convention: args = [subscripts..., value] (value last).
    const keel::usize nsub = args.empty() ? 0 : args.size() - 1;
    Value s = buildSubsStruct("()", keel::ArrayRef<Value>(args.data(), nsub),
                              ctx.engine->allocator());
    Value callArgs[3] = {self, s, nsub < args.size() ? args[nsub] : Value()};
    auto results = coreEngine(ctx).invokeClassMethod(uf, keel::ArrayRef<Value>(callArgs, 3), 1);
    // Value class: subsasgn returns the modified object -> write back.
    // Handle class: mutates shared state in place (write-back is harmless).
    if (!results.empty() && results[0].isObject())
        self = keel::move(results[0]);
}

keel::String classDispText(const ClassDefDesc &desc, const Value &self)
{
    if (self.objectCount() != 1)
        return keel::String{};
    keel::String body = keel::format("  {} with properties:\n\n", desc.name);
    const auto *st = self.objectStateConst();
    for (const auto &p : desc.props) {
        keel::format_into(body, "    {}", p.name);
        if (st) {
            if (const Value *vp = st->props.get(p.name);
                vp && vp->isScalar() && vp->type() == ValueType::DOUBLE) {
                char numBuf[64];
                keel::usize n = keel::format_f64_fixed(numBuf, sizeof(numBuf), vp->toScalar(), 6);
                if (n > 0) {
                    body.append(": ");
                    body.append(keel::StringRef(numBuf, n));
                }
            }
        }
        body.push_back('\n');
    }
    return body;
}

void enumEq(Value &, keel::ArrayRef<Value> args, keel::usize,
            keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    bool eq = false;
    if (args.size() == 2 && args[0].isObject() && args[1].isObject()
        && args[0].objectClassNameRef() == args[1].objectClassNameRef()) {
        const auto *a = args[0].objectStateConst();
        const auto *b = args[1].objectStateConst();
        eq = a && b && !a->enumName.empty() && a->enumName == b->enumName;
    }
    outs[0] = Value::logicalScalar(eq, ctx.engine->allocator());
}

void enumNe(Value &self, keel::ArrayRef<Value> args, keel::usize no,
            keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    enumEq(self, args, no, outs, ctx);
    if (!outs.empty())
        outs[0] = Value::logicalScalar(!outs[0].toBool(), ctx.engine->allocator());
}

keel::String enumDispText(const Value &self)
{
    if (self.objectCount() != 1)
        return keel::String{};
    const auto *st = self.objectStateConst();
    if (st && !st->enumName.empty())
        return keel::format("    {}\n", st->enumName);
    return keel::String{};
}

// Mirror merged classdef member attributes into BuiltinClass for reflection
// (metaclass / meta.property / meta.method). Hidden members are omitted.
void populateReflectionMeta(const ClassDefDesc &desc, BuiltinClass &cls)
{
    for (const auto &p : desc.props) {
        if (!isMemberHidden(desc, p.name)) {
            cls.propMeta.push_back({p.name.clone(),
                                    keel::String::from(accessName(p.getAccess)),
                                    keel::String::from(accessName(p.setAccess)),
                                    p.isConstant, p.isDependent});
        }
    }
    struct TmpMethodMeta
    {
        const char *access = "public";
        bool isStatic = false;
        bool isAbstract = false;
    };
    keel::HashMap<keel::String, TmpMethodMeta> mm; // dedup; sorted below for deterministic order
    for (auto [name, uf] : desc.methods) {
        (void)uf;
        TmpMethodMeta m;
        if (const auto *ma = desc.methodAccess.get(name))
            m.access = accessName(ma->level);
        mm.put(name.clone(), m);
    }
    for (auto [name, si] : desc.statics) {
        TmpMethodMeta &m = mm.get_or_insert(name, TmpMethodMeta{});
        m.isStatic = true;
        m.access = accessName(si.access);
    }
    for (const auto &name : desc.abstractMethods) {
        TmpMethodMeta &m = mm.get_or_insert(name, TmpMethodMeta{});
        m.isAbstract = true;
    }
    for (auto [name, m] : mm) {
        if (!isMemberHidden(desc, name)) {
            MethodMeta meta;
            meta.name = name.clone();
            meta.access = keel::String::from(m.access);
            meta.isStatic = m.isStatic;
            meta.isAbstract = m.isAbstract;
            cls.methodMeta.push_back(keel::move(meta));
        }
    }
    keel::sort(cls.methodMeta.begin(), cls.methodMeta.end(),
               [](const MethodMeta &a, const MethodMeta &b) { return a.name < b.name; });
}

// Synthesize the runtime BuiltinClass from a finalized (immutable) ClassDefDesc.
// Every closure captures at most Arc<ClassDefDesc> + raw pointer (<= 24 bytes),
// fitting strictly inside keel::SmallFunction<..., 32> SBO with zero heap spill.
BuiltinClass synthesizeBuiltinClass(const keel::Arc<ClassDefDesc> &desc)
{
    BuiltinClass cls;
    cls.name = desc->name.clone();
    cls.isHandle = desc->isHandle;
    cls.isSealed = desc->isSealed;
    cls.isAbstract = desc->isAbstract;
    cls.hidden.reserve(desc->hiddenMembers.size());
    for (const auto &h : desc->hiddenMembers)
        cls.hidden.push_back(h.clone());
    cls.propNames.reserve(desc->props.size());
    for (const auto &p : desc->props)
        if (!isMemberHidden(*desc, p.name))
            cls.propNames.push_back(p.name.clone());
    cls.superclasses.reserve(desc->superclasses.size());
    for (const auto &s : desc->superclasses)
        cls.superclasses.push_back(s.clone());

    populateReflectionMeta(*desc, cls);

    cls.propGet = [desc](const Value &self, keel::StringRef nameRef, Value &out,
                         CallContext &ctx) -> bool {
        return classPropGet(*desc, self, nameRef, out, ctx);
    };
    cls.propSet = [desc](Value &self, keel::StringRef nameRef, const Value &val,
                         CallContext &ctx) -> bool {
        return classPropSet(*desc, self, nameRef, val, ctx);
    };
    cls.construct = [desc](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
        return classConstruct(*desc, args, ctx);
    };

    for (auto [mname, uf] : desc->methods) {
        cls.methodFns.put(mname.clone(), keel::Arc<const UserFunction>(uf));
        const UserFunction *ufPtr = uf.get();
        auto methodHook = [desc, ufPtr](Value &self, keel::ArrayRef<Value> args,
                                        keel::usize nargout, keel::MutArrayRef<Value> outs,
                                        CallContext &ctx) {
            classInvokeMethod(*desc, *ufPtr, self, args, nargout, outs, ctx);
        };
        static_assert(sizeof(methodHook) <= 32, "methodHook must fit in SmallFunction SBO");
        cls.methods.put(mname.clone(), keel::move(methodHook));

        if (isOperatorMethodName(mname)) {
            auto opHook = [desc, ufPtr](Value &self, keel::ArrayRef<Value> args,
                                        keel::usize nargout, keel::MutArrayRef<Value> outs,
                                        CallContext &ctx) {
                classInvokeOp(*desc, *ufPtr, self, args, nargout, outs, ctx);
            };
            static_assert(sizeof(opHook) <= 32, "opHook must fit in SmallFunction SBO");
            cls.ops.put(mname.clone(), keel::move(opHook));
        }
    }

    if (const auto *ufp = desc->methods.get("subsref")) {
        auto uf = *ufp;
        cls.subsref = [uf](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            classSubsref(*uf, self, args, nargout, outs, ctx);
        };
    }
    if (const auto *ufp = desc->methods.get("subsasgn")) {
        auto uf = *ufp;
        cls.subsasgn = [uf](Value &self, keel::ArrayRef<Value> args, keel::usize,
                            keel::MutArrayRef<Value>, CallContext &ctx) {
            classSubsasgn(*uf, self, args, ctx);
        };
    }

    cls.dispText = [desc](const Value &self) -> keel::String {
        return classDispText(*desc, self);
    };

    // Enumeration defaults: members compare by their member name and display as
    // that name unless the class defines its own eq / disp.
    if (desc->isEnum) {
        if (!cls.ops.contains("eq"))
            cls.ops.put(keel::String::from("eq"), &enumEq);
        if (!cls.ops.contains("ne"))
            cls.ops.put(keel::String::from("ne"), &enumNe);
        if (!desc->methods.contains("disp") && !desc->methods.contains("display"))
            cls.dispText = &enumDispText;
    }

    return cls;
}

} // namespace

// ── Engine classdef registration / unregistration / inheritance cascade ──

void Engine::unregisterClassDef(keel::StringRef name)
{
    if (const auto *dp = classReg_->classDefs.get(name)) {
        const ClassDefDesc &desc = **dp;
        // Remove the `Name.member` qualified externals registered for Static
        // methods and Constant properties. registerFunctionImpl_ throws on a
        // duplicate full name, so these MUST be cleared before re-registration.
        auto dropExternal = [&](keel::StringRef leaf) {
            keel::String full = keel::format("{}.{}", name, leaf);
            funcReg_->externalFuncs.remove(full);
            if (auto *vec = funcReg_->shortNameIndex.get(leaf)) {
                for (keel::usize i = vec->size(); i > 0; --i) {
                    if ((*vec)[i - 1] == full)
                        vec->remove_at(i - 1);
                }
                if (vec->empty())
                    funcReg_->shortNameIndex.remove(leaf);
            }
        };
        for (auto [sname, si] : desc.statics) {
            (void)si;
            dropExternal(sname);
        }
        for (const auto &p : desc.props)
            if (p.isConstant)
                dropExternal(p.name);
        classReg_->classDefs.remove(name);
    }
    // Drop compiled method chunks ("Name>method") so rewritten bodies recompile
    // instead of serving the stale cache.
    if (compiler_)
        compiler_->eraseCompiledFuncsForClass(name);
    clearPersistentForFunc(name);
    // Source AST kept for derived-class re-merge — drop it too (the cascade in
    // reregisterDerivedClasses holds its own Arc across this erase).
    classReg_->classDefAst.remove(name);
    // Drop the registry class last: registerClass throws on a duplicate, so the
    // re-registration that follows depends on this entry being gone.
    classReg_->classes.remove(name);
    bumpCallCacheEpoch();
}

void Engine::dropFileClassCtorExternal_(keel::StringRef name)
{
    // resolveMFile_ registers a bare constructor external for a class loaded
    // from `Name.m`. Guard on mFileCache_ membership: only a file-loaded class
    // has this external, so we never erase a core builtin that merely shares a
    // name with an inline classdef.
    if (!mFileReg_->mFileCache.contains(name))
        return;
    funcReg_->externalFuncs.remove(name);
    if (auto *vec = funcReg_->shortNameIndex.get(name)) {
        for (keel::usize i = vec->size(); i > 0; --i) {
            if ((*vec)[i - 1] == name)
                vec->remove_at(i - 1);
        }
        if (vec->empty())
            funcReg_->shortNameIndex.remove(name);
    }
    bumpCallCacheEpoch();
}

void Engine::clearClassDefs()
{
    // Collect names first — unregisterClassDef mutates classDefs_ as it goes.
    // Built-in classes live in classes_ but NOT classDefs_, so iterating
    // classDefs_ touches only user classes.
    keel::Array<keel::String> names;
    names.reserve(classReg_->classDefs.size());
    for (auto [k, v] : classReg_->classDefs) {
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

    if (findClass(className) != nullptr)
        unregisterClassDef(className);

    auto desc = keel::make_arc<ClassDefDesc>();
    desc->name = className.clone();
    for (const auto &super : cd->paramNames)
        if (super == "handle")
            desc->isHandle = true;
    desc->isSealed = hasAttr(cd, "Sealed");

    // 1. Collect own members from the classdef AST.
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
            BlockAccess ba = parseBlockAccess(child->classAttrs);
            desc->props.push_back(PropInfo{
                child->strValue.clone(), def, ba.get, ba.set, desc->name.clone(),
                hasAttr(child, "Constant"), hasAttr(child, "Dependent")});
            desc->anyNonPublicProp = desc->anyNonPublicProp || ba.any;
            if (hasAttr(child, "Hidden"))
                desc->hiddenMembers.push_back(child->strValue.clone());
        } else if (child->type == NodeType::FUNCTION_DEF) {
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
            uf->ownerClass = desc->name.clone();
            uf->originKnown = originKnown;
            uf->sourceFs = keel::String::from(fsName);
            uf->sourceDir = keel::String::from(scriptDir);
            uf->sourceFile = keel::String::from(scriptFile);
            const keel::String &mn = child->strValue;
            if (mn.starts_with("get.")) {
                desc->getters.put(keel::String::from(mn.substr(4)), uf);
            } else if (mn.starts_with("set.")) {
                desc->setters.put(keel::String::from(mn.substr(4)), uf);
            } else if (hasAttr(child, "Static")) {
                BlockAccess ba = parseBlockAccess(child->classAttrs);
                Access slvl = (ba.set == Access::Immutable) ? Access::Public : ba.set;
                desc->statics.put(child->strValue.clone(), StaticInfo{uf, slvl, desc->name.clone()});
            } else if (child->strValue == classLeaf) {
                desc->ctor = uf;
                BlockAccess ba = parseBlockAccess(child->classAttrs);
                Access clvl = (ba.set == Access::Immutable) ? Access::Public : ba.set;
                if (clvl != Access::Public) {
                    desc->ctorAccess = clvl;
                    desc->ctorDeclClass = desc->name.clone();
                }
            } else {
                desc->methods.put(child->strValue.clone(), uf);
                BlockAccess ba = parseBlockAccess(child->classAttrs);
                Access lvl = (ba.set == Access::Immutable) ? Access::Public : ba.set;
                if (lvl != Access::Public)
                    desc->methodAccess.put(child->strValue.clone(), MethodAccess{lvl, desc->name.clone()});
                if (hasAttr(child, "Hidden"))
                    desc->hiddenMembers.push_back(child->strValue.clone());
            }
        }
    }

    // Method names declared by THIS class, captured before inheritance merge so
    // an inherited method-access entry never overrides a public derived override.
    keel::Array<keel::String> ownMethodNames;
    ownMethodNames.reserve(desc->methods.size());
    for (auto [mn, uf] : desc->methods) {
        (void)uf;
        ownMethodNames.push_back(mn.clone());
    }

    // Pull in any file-based superclasses not yet loaded so inheritance is order-independent.
    for (const auto &superName : cd->paramNames)
        if (superName != "handle" && !classReg_->classDefs.contains(superName))
            resolveMFile_(superName);

    // 2. Merge registered superclasses (base members first, derived overrides).
    for (const auto &superName : cd->paramNames) {
        if (superName == "handle")
            continue;
        const auto *bp = classReg_->classDefs.get(superName);
        if (!bp)
            continue;
        const auto &base = *bp;
        if (base->isSealed) {
            auto msg = keel::format("Cannot subclass sealed class '{}'", superName);
            throw numkit::Error(msg.c_str());
        }
        desc->isHandle = desc->isHandle || base->isHandle;

        keel::Array<PropInfo> merged = base->props.clone();
        for (auto &dp : desc->props) {
            auto it = keel::find_if(merged.begin(), merged.end(),
                                    [&](const PropInfo &m) { return m.name == dp.name; });
            if (it != merged.end())
                *it = keel::move(dp);
            else
                merged.push_back(keel::move(dp));
        }
        desc->props = keel::move(merged);
        desc->anyNonPublicProp = desc->anyNonPublicProp || base->anyNonPublicProp;

        for (auto [mn, uf] : base->methods)
            if (!desc->methods.contains(mn))
                desc->methods.put(mn.clone(), uf);
        for (auto [pn, uf] : base->getters)
            if (!desc->getters.contains(pn))
                desc->getters.put(pn.clone(), uf);
        for (auto [pn, uf] : base->setters)
            if (!desc->setters.contains(pn))
                desc->setters.put(pn.clone(), uf);
        for (auto [sn, si] : base->statics)
            if (!desc->statics.contains(sn))
                desc->statics.put(sn.clone(), si.clone());
        for (const auto &am : base->abstractMethods)
            if (keel::find(desc->abstractMethods.begin(), desc->abstractMethods.end(), am)
                == desc->abstractMethods.end())
                desc->abstractMethods.push_back(am.clone());
        for (const auto &hm : base->hiddenMembers)
            if (keel::find(desc->hiddenMembers.begin(), desc->hiddenMembers.end(), hm)
                == desc->hiddenMembers.end())
                desc->hiddenMembers.push_back(hm.clone());
        for (auto [mn, ma] : base->methodAccess)
            if (keel::find(ownMethodNames.begin(), ownMethodNames.end(), mn)
                == ownMethodNames.end())
                desc->methodAccess.try_insert(mn.clone(), ma.clone());
        if (!desc->ctor) {
            desc->ctor = base->ctor;
            desc->ctorAccess = base->ctorAccess;
            desc->ctorDeclClass = base->ctorDeclClass.clone();
        }
    }

    // Transitive ancestry for isa() + abstractness check.
    for (const auto &superName : cd->paramNames) {
        if (superName == "handle")
            continue;
        desc->superclasses.push_back(superName.clone());
        if (const auto *bp = classReg_->classDefs.get(superName))
            for (const auto &a : (*bp)->superclasses)
                if (keel::find(desc->superclasses.begin(), desc->superclasses.end(), a)
                    == desc->superclasses.end())
                    desc->superclasses.push_back(a.clone());
    }
    for (const auto &am : desc->abstractMethods) {
        if (!desc->methods.contains(am)) {
            desc->isAbstract = true;
            break;
        }
    }
    classReg_->classDefs.put(desc->name.clone(), desc);

    // 3. Register `ClassName.member` qualified externals for Static methods and
    // Constant properties (own + inherited). Desc is immutable from here on.
    for (auto [sname, si] : desc->statics) {
        const UserFunction *ufPtr = si.uf.get();
        auto staticHook = [desc, ufPtr](keel::ArrayRef<Value> args, keel::usize nargout,
                                        keel::MutArrayRef<Value> outs, CallContext &ctx) {
            classInvokeStatic(*desc, *ufPtr, args, nargout, outs, ctx);
        };
        static_assert(sizeof(staticHook) <= 32, "staticHook must fit in SmallFunction SBO");
        registerFunction(desc->name, sname, keel::move(staticHook));
    }
    for (keel::usize propIdx = 0; propIdx < desc->props.size(); ++propIdx) {
        if (!desc->props[propIdx].isConstant)
            continue;
        auto constHook = [desc, propIdx](keel::ArrayRef<Value>, keel::usize,
                                         keel::MutArrayRef<Value> outs, CallContext &ctx) {
            classReadConstant(*desc, propIdx, outs, ctx);
        };
        static_assert(sizeof(constHook) <= 32, "constHook must fit in SmallFunction SBO");
        registerFunction(desc->name, desc->props[propIdx].name, keel::move(constHook));
    }

    // 4. Synthesize and register runtime BuiltinClass.
    registerClass(synthesizeBuiltinClass(desc));

    // 5. Instantiate enumeration members (`ClassName.Member`).
    if (desc->isEnum) {
        if (const BuiltinClass *ecls = findClass(desc->name)) {
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
                                 [instPtr](keel::ArrayRef<Value>, keel::usize,
                                           keel::MutArrayRef<Value> outs, CallContext &) {
                                     outs[0] = *instPtr;
                                 });
            }
        }
    }

    // 6. Store source AST and propagate to derived classes.
    classReg_->classDefAst.put(className.clone(), keel::Arc<const ASTNode>(cloneNode(cd)));
    if (!classReg_->suppressDependentCascade)
        reregisterDerivedClasses(className);
}

void Engine::reregisterDerivedClasses(keel::StringRef base)
{
    keel::Array<keel::String> deps;
    for (auto [name, desc] : classReg_->classDefs) {
        if (name == base)
            continue;
        if (keel::find(desc->superclasses.begin(), desc->superclasses.end(), base)
            != desc->superclasses.end())
            deps.push_back(name.clone());
    }
    if (deps.empty())
        return;
    // Parent-first: ascending transitive-ancestor count is a valid topological order.
    keel::sort(deps.begin(), deps.end(), [&](const keel::String &a, const keel::String &b) {
        return (*classReg_->classDefs.get(a))->superclasses.size()
             < (*classReg_->classDefs.get(b))->superclasses.size();
    });
    struct CascadeGuard
    {
        bool &flag;
        bool prev;
        explicit CascadeGuard(bool &f) : flag(f), prev(f) { flag = true; }
        ~CascadeGuard() { flag = prev; }
        CascadeGuard(const CascadeGuard &) = delete;
        CascadeGuard &operator=(const CascadeGuard &) = delete;
    } guard(classReg_->suppressDependentCascade);
    for (const auto &name : deps) {
        const auto *astPtr = classReg_->classDefAst.get(name);
        if (!astPtr || !*astPtr)
            continue;
        keel::Arc<const ASTNode> ast = *astPtr;
        registerClassDef(ast.get(), name);
    }
}

} // namespace numkit
