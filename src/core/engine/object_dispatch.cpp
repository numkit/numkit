// src/core/engine/object_dispatch.cpp
// Runtime object model dispatch: class registry lookup, display, operator &
// subsref/subsasgn dispatch, object-array slice store, method/ctor invocation,
// and member access-control enforcement.
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

void Engine::registerClass(BuiltinClass cls)
{
    if (classReg_->classes.contains(cls.name)) {
        auto msg = keel::format("Engine::registerClass: duplicate class '{}'", cls.name);
        throw numkit::Error(msg.c_str());
    }
    keel::String key = cls.name.clone();
    classReg_->classes.put(keel::move(key), keel::make_arc<BuiltinClass>(keel::move(cls)));
    bumpCallCacheEpoch();
}

bool Engine::registerClassIfAbsent(BuiltinClass cls)
{
    if (classReg_->classes.contains(cls.name))
        return false;
    keel::String key = cls.name.clone();
    classReg_->classes.put(keel::move(key), keel::make_arc<BuiltinClass>(keel::move(cls)));
    bumpCallCacheEpoch();
    return true;
}

const BuiltinClass *Engine::findClass(keel::StringRef name) const
{
    if (const auto *entry = classReg_->classes.get(name))
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
    Value fill; // default element for grown gaps
    if (cls && cls->heterogeneousGroup == "graphics") {
        if (const BuiltinClass *ph = findClass("matlab.graphics.GraphicsPlaceholder"); ph && ph->construct) {
            CallContext ctx{this, env};
            fill = ph->construct(keel::ArrayRef<Value>(nullptr, 0), ctx);
        }
    } else if (cls && cls->construct) {
        CallContext ctx{this, env};
        fill = cls->construct(keel::ArrayRef<Value>(nullptr, 0), ctx);
    }
    if (perDim.size() == 1)
        dst.objectAssignLinear(perDim[0], coercedVal, fill, allocator());
    else
        dst.objectAssignND(perDim, coercedVal, fill, allocator());
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

keel::Array<Value> Engine::invokeUserFunction_(const UserFunction &uf,
                                                 keel::ArrayRef<Value> args,
                                                 keel::usize nout,
                                                 bool isCtor,
                                                 const Value *ctorSeed)
{
    const keel::usize effNout = keel::max<keel::usize>(nout, 1);
    if (backend_ == Backend::VM) {
        if (!vm_)
            throw numkit::Error("VM backend is unavailable");
        const BytecodeChunk *cc = ensureClassMethodChunk(uf);
        if (!cc) {
            auto msg = keel::format("Failed to compile function '{}'", uf.name);
            throw numkit::Error(msg.c_str());
        }
        return vm_->callReentrant(*cc, args, effNout, uf.ownerClass, isCtor, ctorSeed);
    }
    if (!treeWalker_)
        throw numkit::Error("TreeWalker backend is unavailable");
    if (!uf.ownerClass.empty()) {
        ClassCtxGuard ctx(this, uf.ownerClass, isCtor);
        if (isCtor) {
            keel::Array<Value> out;
            out.push_back(treeWalker_->runClassCtor(uf, ctorSeed ? *ctorSeed : Value(), args));
            return out;
        }
        return treeWalker_->runClassMethod(uf, args, effNout);
    }
    return treeWalker_->callUserFunctionMulti(uf, args, workspaceEnv_.get(), effNout);
}

keel::Array<Value> Engine::invokeClassMethod(const UserFunction &uf, keel::ArrayRef<Value> args,
                                             keel::usize nout)
{
    return invokeUserFunction_(uf, args, keel::max<keel::usize>(nout, 1), /*isCtor=*/false);
}

Value Engine::invokeClassCtor(const UserFunction &ctor, const Value &seed,
                              keel::ArrayRef<Value> args)
{
    auto r = invokeUserFunction_(ctor, args, 1, /*isCtor=*/true, &seed);
    return r.empty() ? seed : keel::move(r[0]);
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
    const auto *dp = classReg_->classDefs.get(className);
    if (!dp)
        return;
    if (const auto *ma = (*dp)->methodAccess.get(method))
        enforceAccess(this, ma->level, ma->declClass, "method", method);
}

const UserFunction *Engine::classGetter(keel::StringRef className,
                                        keel::StringRef prop) const
{
    const auto *dp = classReg_->classDefs.get(className);
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
    const auto *dp = classReg_->classDefs.get(className);
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
    for (const auto &a : classReg_->activePropAccessors)
        if (a == accessorName)
            return true;
    if (vm_ && vm_->hasActiveFrameNamed(accessorName))
        return true;
    return false;
}

void Engine::enforcePropGetAccess(keel::StringRef className, keel::StringRef prop)
{
    const auto *dp = classReg_->classDefs.get(className);
    if (!dp || !(*dp)->anyNonPublicProp)
        return;
    if (const PropInfo *pi = findProp(**dp, prop); pi && pi->getAccess != Access::Public)
        enforceAccess(this, pi->getAccess, pi->declClass, "property", prop);
}

void Engine::enforcePropSetAccess(keel::StringRef className, keel::StringRef prop)
{
    const auto *dp = classReg_->classDefs.get(className);
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
               && keel::find(desc.superclasses.begin(), desc.superclasses.end(), "dynamicprops")
                      == desc.superclasses.end()) {
        auto msg = keel::format("No public property '{}' for class '{}'", prop, className);
        throw numkit::Error(msg.c_str());
    }
}

Value Engine::superConstruct(keel::StringRef base, const Value &seed,
                             keel::ArrayRef<Value> args)
{
    const auto *dp = classReg_->classDefs.get(base);
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
    const auto *dp = classReg_->classDefs.get(base);
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
    classReg_->classCtx.push_back({keel::String::from(className), isCtor});
}

void Engine::popClassCtx()
{
    if (!classReg_->classCtx.empty())
        classReg_->classCtx.pop_back();
}

// The class whose method/constructor is currently executing. A TW callback's
// ClassCtxGuard (classCtx_) is innermost when present; otherwise, under the VM
// backend, the running class is the VM's top method frame. Empty == script
// scope. (No separate VM stack — read on demand, so it stays exception-safe.)
keel::StringRef Engine::currentClassCtx_() const
{
    if (!classReg_->classCtx.empty())
        return classReg_->classCtx.back().className.as_ref();
    if (useVM_())
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
    const auto *dp = classReg_->classDefs.get(ctx);
    if (!dp)
        return false;
    const auto &sc = (*dp)->superclasses;
    return keel::find(sc.begin(), sc.end(), declClass) != sc.end();
}

bool Engine::classCtxInCtorOf(keel::StringRef declClass) const
{
    if (!classReg_->classCtx.empty())
        return classReg_->classCtx.back().isCtor && classReg_->classCtx.back().className == declClass;
    if (useVM_())
        return vm_->currentMethodIsCtor() && vm_->currentMethodClass() == declClass;
    return false;
}

void Engine::enforceCtorAccess(keel::StringRef className)
{
    const auto *dp = classReg_->classDefs.get(className);
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
    const auto *dp = classReg_->classDefs.get(className);
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
    const auto *dp = classReg_->classDefs.get(className);
    if (!dp || !(*dp)->ctor)
        return nullptr;
    return (*dp)->ctor.get();
}

} // namespace numkit
