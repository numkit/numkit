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
        if (superName != "handle" && !classReg_->classDefs.contains(superName))
            resolveMFile_(superName);

    // ── Inheritance: merge registered superclasses (base members first,
    // derived overrides). `handle` is the semantics marker, not a classdef.
    for (const auto &superName : cd->paramNames) {
        if (superName == "handle")
            continue;
        const auto *bp = classReg_->classDefs.get(superName);
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
            auto it = keel::find_if(merged.begin(), merged.end(),
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
            if (keel::find(desc->abstractMethods.begin(), desc->abstractMethods.end(), am)
                == desc->abstractMethods.end())
                desc->abstractMethods.push_back(am.clone());
        // Hidden members stay hidden in the subclass.
        for (const auto &hm : base->hiddenMembers)
            if (keel::find(desc->hiddenMembers.begin(), desc->hiddenMembers.end(), hm)
                == desc->hiddenMembers.end())
                desc->hiddenMembers.push_back(hm.clone());
        // Method access: inherit base entries (keeping the base's declaring
        // class so `protected` still admits this subclass) unless the derived
        // class declared the method itself (its own access — or public by
        // omission — then wins).
        for (auto [mn, ma] : base->methodAccess)
            if (keel::find(ownMethodNames.begin(), ownMethodNames.end(), mn)
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
        if (const auto *bp = classReg_->classDefs.get(superName))
            for (const auto &a : (*bp)->superclasses)
                if (keel::find(desc->superclasses.begin(), desc->superclasses.end(), a)
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
    classReg_->classDefs.put(desc->name.clone(), desc);

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
                                 enforceAccess(coreEnginePtr(ctx), acc, decl, "method", mn);
                             const keel::usize nout = keel::max<keel::usize>(nargout, 1);
                             auto results = coreEngine(ctx).invokeClassMethod(*uf, args, nout);
                             const keel::usize writeN = keel::min(nout, results.size());
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
                                 enforceAccess(coreEnginePtr(ctx), getLvl, decl, "property", pn);
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
        if (keel::find(desc->hiddenMembers.begin(), desc->hiddenMembers.end(), p.name) == desc->hiddenMembers.end())
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
        keel::sort(cls.methodMeta.begin(), cls.methodMeta.end(),
                  [](const MethodMeta &a, const MethodMeta &b) { return a.name < b.name; });
    }
    cls.propGet = [desc](const Value &self, keel::StringRef nameRef, Value &out,
                         CallContext &ctx) -> bool {
        if (desc->anyNonPublicProp)
            if (const PropInfo *pi = findProp(*desc, nameRef);
                pi && pi->getAccess != Access::Public)
                enforceAccess(coreEnginePtr(ctx), pi->getAccess, pi->declClass, "property", nameRef);
        if (const auto *gp = desc->getters.get(nameRef);
            gp && !coreEngine(ctx).isPropAccessorActive((*gp)->name)) {
            struct AccessorGuard {
                Engine *eng;
                ~AccessorGuard() { eng->popPropAccessor(); }
            };
            coreEngine(ctx).pushPropAccessor((*gp)->name);
            AccessorGuard guard{coreEnginePtr(ctx)};
            auto r = coreEngine(ctx).invokeClassMethod(**gp,
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
                enforceAccess(coreEnginePtr(ctx), pi->setAccess, pi->declClass, "property", nameRef);
        } else if (!desc->setters.contains(nameRef)
                   && keel::find(desc->superclasses.begin(), desc->superclasses.end(),
                                "dynamicprops")
                          == desc->superclasses.end()) {
            auto msg = keel::format("No public property '{}' for class '{}'", nameRef, desc->name);
            throw numkit::Error(msg.c_str());
        }
        if (const auto *sp = desc->setters.get(nameRef);
            sp && !coreEngine(ctx).isPropAccessorActive((*sp)->name)) {
            struct AccessorGuard {
                Engine *eng;
                ~AccessorGuard() { eng->popPropAccessor(); }
            };
            coreEngine(ctx).pushPropAccessor((*sp)->name);
            AccessorGuard guard{coreEnginePtr(ctx)};
            Value callArgs[2] = {self, val};
            auto r = coreEngine(ctx).invokeClassMethod(**sp,
                                                   keel::ArrayRef<Value>(callArgs, 2), 1);
            if (!r.empty() && r[0].isObject())
                self = keel::move(r[0]);
            return true;
        }
        self.objectStateMut()->props.put(keel::String::from(nameRef), val);
        return true;
    };
    cls.construct = [desc](keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
        Value obj = coreEngine(ctx).makeDefaultInstance(desc->name);
        if (desc->ctor)
            obj = coreEngine(ctx).invokeClassCtor(*desc->ctor, obj, args);
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
                enforceAccess(coreEnginePtr(ctx), mlevel, mdecl, "method", nameCopy);
            keel::Array<Value> callArgs;
            callArgs.reserve(args.size() + 1);
            callArgs.push_back(self); // MATLAB: obj is the first parameter
            for (const auto &a : args)
                callArgs.push_back(a);
            const keel::usize nout = keel::max<keel::usize>(nargout, 1);
            auto results = coreEngine(ctx).invokeClassMethod(
                *ufCopy, keel::ArrayRef<Value>(callArgs.data(), callArgs.size()), nout);
            // Always produce at least one output (the `ans`/expression value
            // when called with nargout==0), bounded by the outs span.
            const keel::usize writeN = keel::min(nout, results.size());
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
                    enforceAccess(coreEnginePtr(ctx), mlevel, mdecl, "method", nameCopy);
                keel::Array<Value> callArgs;
                if (args.empty())
                    callArgs.push_back(self); // unary: operand came as self
                else
                    callArgs.append(args); // binary: [lhs, rhs]
                const keel::usize nout = keel::max<keel::usize>(nargout, 1);
                auto results = coreEngine(ctx).invokeClassMethod(
                    *ufOp, keel::ArrayRef<Value>(callArgs.data(), callArgs.size()), nout);
                const keel::usize writeN = keel::min(nout, results.size());
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
            const keel::usize nout = keel::max<keel::usize>(nargout, 1);
            auto results = coreEngine(ctx).invokeClassMethod(*uf, keel::ArrayRef<Value>(callArgs, 2), nout);
            const keel::usize writeN = keel::min(nout, results.size());
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
            auto results = coreEngine(ctx).invokeClassMethod(*uf, keel::ArrayRef<Value>(callArgs, 3), 1);
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
    classReg_->classDefAst.put(className.clone(), keel::Arc<const ASTNode>(cloneNode(cd)));

    // Re-register every class that (transitively) derives from this one, so the
    // new members propagate to subclasses (which hold a snapshot of the base's
    // methods/props). Fires on EVERY registration, not just redefinitions: it
    // also back-fills a subclass that was defined BEFORE its base (forward
    // reference — the subclass recorded the base name but registered without its
    // members). No-op when nothing derives from `className`. Guarded so the
    // nested registerClassDef calls don't each re-cascade.
    if (!classReg_->suppressDependentCascade)
        reregisterDerivedClasses(className);
}

void Engine::reregisterDerivedClasses(keel::StringRef base)
{
    // Classes whose transitive superclasses include `base`.
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
    // Parent-first: a subclass's transitive-ancestor set strictly contains each
    // ancestor's, so ascending ancestor count is a valid topological order (a
    // base is re-registered before any class that derives from it).
    keel::sort(deps.begin(), deps.end(), [&](const keel::String &a, const keel::String &b) {
        return (*classReg_->classDefs.get(a))->superclasses.size() < (*classReg_->classDefs.get(b))->superclasses.size();
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
    } guard(classReg_->suppressDependentCascade);
    for (const auto &name : deps) {
        const auto *astPtr = classReg_->classDefAst.get(name);
        if (!astPtr || !*astPtr)
            continue;
        // Hold our own ref: registerClassDef → unregisterClassDef erases the map
        // entry, then re-stores a fresh clone; this keeps the node alive in
        // between. Re-register under the same (possibly qualified) name.
        keel::Arc<const ASTNode> ast = *astPtr;
        registerClassDef(ast.get(), name);
    }
}


} // namespace numkit
