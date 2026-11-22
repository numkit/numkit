// src/core/tree_walker_expr.cpp
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_set.h>
#include <keel/optional.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/tree_walker.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/value/index.hpp>
#include "tree_walker_internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace numkit {

// ============================================================
Value TreeWalker::execBinaryOp(const ASTNode *node, Environment *env)
{
    const keel::String &op = node->strValue;

    if (op == "&&" || op == "||") {
        keel::Array<const ASTNode *> spine;
        const ASTNode *cur = node;
        while (cur->type == NodeType::BINARY_OP
               && (cur->strValue == "&&" || cur->strValue == "||")) {
            spine.push_back(cur);
            cur = cur->children[0].get();
        }
        bool acc = toLogicalScalarBool(execNode(cur, env));
        for (keel::usize i = spine.size(); i > 0; --i) {
            const ASTNode *step = spine[i - 1];
            if (step->strValue == "&&") {
                if (acc)
                    acc = toLogicalScalarBool(execNode(step->children[1].get(), env));
            } else {
                if (!acc)
                    acc = toLogicalScalarBool(execNode(step->children[1].get(), env));
            }
        }
        return Value::logicalScalar(acc, engine_.mr_);
    }

    // Element-wise fusion fast path: a binary-rooted idiom (affine `a.*x+b`, …)
    // runs as one fused SIMD kernel. Operands are gated side-effect-free, so a
    // declined kernel falls back to the normal per-op path below.
    if (engine_.fusionEnabled()) {
        const ASTNode *probe = node;
        keel::i32 spineDepth = 0;
        while (probe->type == NodeType::BINARY_OP && ++spineDepth <= 5)
            probe = probe->children[0].get();
        if (spineDepth <= 5) {
            Value fused;
            if (tryFusion(node, env, fused))
                return fused;
        }
    }

    auto evalPair = [&](keel::StringRef stepOp, const Value &left, const Value &right) -> Value {
        if (left.isObject() || right.isObject()) {
            Value out;
            if (engine_.tryObjectBinaryOp(stepOp, left, right, env, out))
                return out;
        }
        if (const auto *fn = engine_.binaryOps_.get(stepOp))
            return (*fn)(left, right);
        auto msg = keel::format("Undefined binary operator: {}", stepOp);
        throw std::runtime_error(msg.c_str());
    };

    if (node->children[0]->type == NodeType::BINARY_OP
        && node->children[0]->strValue != "&&"
        && node->children[0]->strValue != "||") {
        keel::Array<const ASTNode *> spine;
        const ASTNode *cur = node;
        while (cur->type == NodeType::BINARY_OP
               && cur->strValue != "&&"
               && cur->strValue != "||") {
            spine.push_back(cur);
            cur = cur->children[0].get();
        }
        Value acc = execNode(cur, env);
        for (keel::usize i = spine.size(); i > 0; --i) {
            const ASTNode *step = spine[i - 1];
            Value right = execNode(step->children[1].get(), env);
            acc = evalPair(step->strValue, acc, right);
        }
        return acc;
    }

    auto left = execNode(node->children[0].get(), env);
    auto right = execNode(node->children[1].get(), env);
    return evalPair(op, left, right);
}

Value TreeWalker::execUnaryOp(const ASTNode *node, Environment *env)
{
    auto operand = execNode(node->children[0].get(), env);

    // OBJECT unary operator overloading — before the builtin path.
    if (operand.isObject()) {
        Value out;
        if (engine_.tryObjectUnaryOp(node->strValue, operand, env, out))
            return out;
    }

    if (const auto *fn = engine_.unaryOps_.get(node->strValue))
        return (*fn)(operand);

    auto msg = keel::format("Undefined unary operator: {}", node->strValue);
    throw std::runtime_error(msg.c_str());
}

// ============================================================
Value TreeWalker::callFuncHandle(const Value &handle, keel::ArrayRef<Value> args, Environment *env,
                                  const ASTNode *callNode)
{
    auto results = callFuncHandleMulti(handle, args, env, 1, callNode);
    return results.empty() ? Value() : results[0];
}

Value TreeWalker::callHandlePublic(const Value &handle,
                                    keel::ArrayRef<Value> args,
                                    Environment *env)
{
    return callFuncHandle(handle, args, env);
}

keel::Array<Value>
TreeWalker::callHandleMultiPublic(const Value &handle,
                                  keel::ArrayRef<Value> args,
                                  Environment *env,
                                  keel::usize nout)
{
    return callFuncHandleMulti(handle, args, env, nout);
}

keel::Array<Value> TreeWalker::callFuncHandleMulti(const Value &handle,
                                                    keel::ArrayRef<Value> args,
                                                    Environment *env,
                                                    keel::usize nout,
                                                    const ASTNode *callNode)
{
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
    Environment *e = env ? env : engine_.workspaceEnv_.get();
    const keel::StringRef name = handle.funcHandleNameRef();
    if (auto *_uf = engine_.lookupUserFunction(name, e))
        return callUserFunctionMulti(*_uf, args, e, nout, callNode);
    if (const ExternalFunc *fn = engine_.findExternal(name, e)) {
        keel::Array<Value> outBuf(nout);
        CallContext ctx{&engine_, e};
        (*fn)(args, nout, keel::MutArrayRef<Value>(outBuf), ctx);
        return outBuf;
    }
    auto msg = keel::format("Undefined function in handle: @{}", name);
    throw std::runtime_error(msg.c_str());
}

keel::Array<Value> TreeWalker::execSuperCall(const ASTNode *superRef, const ASTNode *callNode,
                                             Environment *env, keel::usize nargout)
{
    const keel::String &base = superRef->strValue;
    const ASTNode *lhs = superRef->children[0].get();

    // Evaluate the call arguments (children[1..] of the enclosing CALL).
    keel::Array<Value> args;
    args.reserve(callNode->children.size() - 1);
    for (keel::usize i = 1; i < callNode->children.size(); ++i)
        args.push_back(execNode(callNode->children[i].get(), env));
    keel::ArrayRef<Value> argSpan(args.data(), args.size());

    // Disambiguate the two MATLAB super-call forms:
    //   obj@Base(args)       — lhs is a variable holding the object being
    //                          constructed → run Base's constructor with that
    //                          object as the seed.
    //   method@Base(obj,...) — lhs is a method-name identifier that is *not* a
    //                          variable → run Base's method (obj is args[0]).
    if (lhs->type == NodeType::IDENTIFIER) {
        if (Value *var = env->get(lhs->strValue)) {
            Value out = engine_.superConstruct(base, *var, argSpan);
            // obj@Base(args) initialises obj in place — the MATLAB
            // statement form carries no assignment, so the constructed
            // value must land back in the variable
            // (bugs/closed/core/super-ctor-result-discarded.md).
            *var = out;
            keel::Array<Value> res;
            res.push_back(keel::move(out));
            return res;
        }
        return engine_.superMethod(base, lhs->strValue, argSpan, nargout);
    }

    // Non-identifier lhs (uncommon): evaluate it as the object and run the
    // superclass constructor with it as the seed.
    Value seed = execNode(lhs, env);
    Value out = engine_.superConstruct(base, seed, argSpan);
    keel::Array<Value> res;
    res.push_back(keel::move(out));
    return res;
}

bool TreeWalker::tryFusion(const ASTNode *node, Environment *env, Value &out)
{
    auto isKnownVarOrConst = [&](const ASTNode *op) -> bool {
        while (op && (op->type == NodeType::UNARY_OP || op->type == NodeType::FIELD_ACCESS)
               && !op->children.empty()) {
            op = op->children[0].get();
        }
        if (!op || op->type != NodeType::IDENTIFIER)
            return true;
        const keel::String &name = op->strValue;
        return (env && env->get(name) != nullptr) || engine_.isReservedName(name);
    };
    for (const FusionRule &rule : engine_.fusionRules()) {
        auto operands = rule.match(node);
        if (!operands) continue;
        for (const ASTNode *opNode : *operands) {
            if (!isKnownVarOrConst(opNode))
                return false;
        }
        // Matched this idiom's shape. Evaluate its (side-effect-free) operands
        // and try the kernel; if it declines, fall back (the normal path may
        // safely re-evaluate the pure operands). A node maps to one idiom, so
        // we don't try further rules after a structural match.
        keel::Array<Value> vals;
        vals.reserve(operands->size());
        for (const ASTNode *opNode : *operands)
            vals.push_back(execNode(opNode, env));
        if (rule.execute(vals.data(), vals.size(), out, engine_.allocator())) {
            engine_.noteFusionHit();
            return true;
        }
        return false;
    }
    return false;
}

Value TreeWalker::execCall(const ASTNode *node, Environment *env, keel::usize nargout)
{
    // Element-wise fusion fast path: a registered idiom (e.g. max(lo,min(hi,x)))
    // runs as one fused SIMD kernel. Single-output context only; operands are
    // gated side-effect-free, so a declined kernel falls back to the normal
    // path below without re-running side effects.
    if (nargout <= 1 && engine_.fusionEnabled()) {
        Value fused;
        if (tryFusion(node, env, fused)) return fused;
    }

    auto *funcNode = node->children[0].get();

    // Superclass-qualified call `lhs@Base(args)` (classdef super-call).
    if (funcNode->type == NodeType::SUPERCLASS_REF) {
        auto results = execSuperCall(funcNode, node, env, std::max<keel::usize>(nargout, 1));
        return results.empty() ? Value() : keel::move(results[0]);
    }

    if (funcNode->type != NodeType::IDENTIFIER) {
        // Qualified-name call: a chain of FIELD_ACCESS over a root
        // IDENTIFIER (`pkg.foo(x)`, `pkg.sub.bar(x)`). When the root
        // identifier is NOT a workspace variable, try resolving the
        // dotted name as a namespace member (registered builtin or
        // +pkg/.../<leaf>.m). A real struct variable shadows this —
        // the existing FIELD_ACCESS path then handles `s.field(x)`.
        const ASTNode *rootIdent = nullptr;
        keel::String qualified = tryBuildQualifiedName(funcNode, &rootIdent);
        if (!qualified.empty() && !env->get(rootIdent->strValue)) {
            keel::Array<Value> args;
            args.reserve(node->children.size() - 1);
            for (keel::usize i = 1; i < node->children.size(); ++i)
                args.push_back(execNode(node->children[i].get(), env));
            // OBJECT: package-qualified constructor (`containers.Map(...)`).
            if (const BuiltinClass *cls = engine_.findClass(qualified);
                cls && cls->construct) {
                CallContext ctx{&engine_, env};
                return engine_.constructChecked(cls, keel::ArrayRef<Value>(args.data(), args.size()),
                                                ctx);
            }
            // User-defined first (MATLAB precedence).
            if (auto *uf = engine_.lookupUserFunction(qualified, env))
                return callUserFunction(*uf, args, env, node);
            if (const ExternalFunc *fn = engine_.findExternal(qualified, env)) {
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                (*fn)(args, nargout, keel::MutArrayRef<Value>(outBuf, 1), ctx);
                return outBuf[0];
            }
            // MATLAB `gca.Parent.Children(2)` / `gcf.Color(2)`: nothing
            // registered under the dotted name, but the HEAD is a plain
            // callable → the callee is a VALUE (the call-then-fields
            // chain — execFieldAccess resolves it) and the call args
            // INDEX it (a func-handle property is called instead).
            if (engine_.hasCallableName(rootIdent->strValue)
                && !engine_.findClass(rootIdent->strValue)
                && !engine_.classCtor(rootIdent->strValue)) {
                Value callee = execNode(funcNode, env);
                if (callee.isFuncHandle())
                    return callFuncHandle(callee, args, env, node);
                return execIndexAccess(callee, node, env);
            }
            auto msg = keel::format("Undefined function or variable: {}", qualified);
            throw std::runtime_error(msg.c_str());
        }

        // OBJECT: obj.method(args) (dotted method) or obj.prop(idx)
        // (property then index). Peek the receiver via env->get so a
        // method name isn't mis-read as a missing property. Limited to
        // identifier-rooted receivers (the common case; no re-eval /
        // double side effects).
        if (funcNode->type == NodeType::FIELD_ACCESS
            && funcNode->children[0]->type == NodeType::IDENTIFIER) {
            Value *objPtr = env->get(funcNode->children[0]->strValue);
            if (objPtr && objPtr->isObject()) {
                const keel::String &mname = funcNode->strValue;
                const BuiltinClass *cls = engine_.findClass(objPtr->objectClassNameRef());
                if (const auto *m = cls ? cls->methods.get(mname) : nullptr) {
                    keel::Array<Value> args;
                    args.reserve(node->children.size() - 1);
                    for (keel::usize i = 1; i < node->children.size(); ++i)
                        args.push_back(execNode(node->children[i].get(), env));
                    Value self = *objPtr; // handle: shares state; value: own copy
                    Value outBuf[1];
                    CallContext ctx{&engine_, env};
                    (*m)(self, keel::ArrayRef<Value>(args.data(), args.size()),
                         nargout, keel::MutArrayRef<Value>(outBuf, 1), ctx);
                    return outBuf[0];
                }
                // Not a method → property read, then CALL (a func-handle
                // property: obj.fh(args) — the inline classdef's direct
                // dispatch, bugs/closed/lang/handle-call-csl-splat) or
                // index with the args.
                Value prop = objectPropGet(*objPtr, mname, env);
                if (prop.isFuncHandle()) {
                    keel::Array<Value> args;
                    args.reserve(node->children.size() - 1);
                    for (keel::usize i = 1; i < node->children.size(); ++i) {
                        Value v = execNodeExpand(node->children[i].get(), env);
                        if (v.isCsl())
                            for (keel::usize k = 0; k < v.cslCount(); ++k)
                                args.push_back(keel::move(v.cslAt(k)));
                        else
                            args.push_back(keel::move(v));
                    }
                    return callFuncHandle(prop, args, env, node);
                }
                return execIndexAccess(prop, node, env);
            }
        }

        // OBJECT method/property on an rvalue receiver: `expr.m(args)` where
        // the receiver is NOT a bare variable (`Vec(2).plus(x)`, `f().g()`,
        // `a.b.method()`). The receiver is evaluated exactly ONCE here; an
        // object dispatches its method (with the call args — unlike the bare
        // `obj.method` no-arg form objectPropGet would otherwise pick) or a
        // property read + index. A non-object value (struct field, etc.) is
        // completed below from the same single evaluation (no re-eval, so a
        // side-effecting receiver runs once).
        Value target;
        bool haveTarget = false;
        if (funcNode->type == NodeType::FIELD_ACCESS
            && funcNode->children[0]->type != NodeType::IDENTIFIER) {
            Value recv = execNode(funcNode->children[0].get(), env);
            const keel::String &mname = funcNode->strValue;
            if (recv.isObject()) {
                const BuiltinClass *cls = engine_.findClass(recv.objectClassNameRef());
                if (const auto *m = cls ? cls->methods.get(mname) : nullptr) {
                    keel::Array<Value> margs;
                    margs.reserve(node->children.size() - 1);
                    for (keel::usize i = 1; i < node->children.size(); ++i)
                        margs.push_back(execNode(node->children[i].get(), env));
                    Value self = recv;
                    Value outBuf[1];
                    CallContext ctx{&engine_, env};
                    (*m)(self, keel::ArrayRef<Value>(margs.data(), margs.size()),
                         nargout, keel::MutArrayRef<Value>(outBuf, 1), ctx);
                    return outBuf[0];
                }
                Value prop = objectPropGet(recv, mname, env);
                return execIndexAccess(prop, node, env);
            }
            // Non-object receiver: finish the field access on the evaluated
            // value (mirrors execFieldAccess) and feed the shared tail below.
            if (!recv.isStruct()) {
                auto msg = keel::format("Dot indexing requires a struct, got {}",
                                        mtypeName(recv.type()));
                throw std::runtime_error(msg.c_str());
            }
            if (!recv.hasField(mname)) {
                auto msg = keel::format("Reference to non-existent field '{}'", mname);
                throw std::runtime_error(msg.c_str());
            }
            target = recv.field(mname);
            haveTarget = true;
        }

        if (!haveTarget)
            target = execNode(funcNode, env);

        if (target.isFuncHandle()) {
            // CSL-splice args (obj.fh(c{:}) — the computed-callee twin of
            // the identifier path's buildArgs;
            // bugs/closed/lang/handle-call-csl-splat).
            keel::Array<Value> args;
            args.reserve(node->children.size() - 1);
            for (keel::usize i = 1; i < node->children.size(); ++i) {
                Value v = execNodeExpand(node->children[i].get(), env);
                if (v.isCsl())
                    for (keel::usize k = 0; k < v.cslCount(); ++k)
                        args.push_back(keel::move(v.cslAt(k)));
                else
                    args.push_back(keel::move(v));
            }
            return callFuncHandle(target, args, env, node);
        }

        if (target.isNumeric() || target.isLogical() || target.isChar()
            || target.isCell() || target.isStruct() || target.isString())
            return execIndexAccess(target, node, env);

        auto msg = keel::format("Cannot call or index into value of type {}",
                                mtypeName(target.type()));
        throw std::runtime_error(msg.c_str());
    }

    const keel::String &name = funcNode->strValue;

    auto buildArgs = [&]() {
        keel::Array<Value> args;
        args.reserve(node->children.size() - 1);
        for (keel::usize i = 1; i < node->children.size(); ++i) {
            // CSL: an argument expression may expand to a comma-separated list
            // (c{:} / c{idx} / c{r,:}, and any future CSL producer). execNodeExpand
            // preserves a CSL; splice its elements into the argument list. A non-CSL
            // value is a single argument.
            Value v = execNodeExpand(node->children[i].get(), env);
            if (v.isCsl())
                for (keel::usize k = 0; k < v.cslCount(); ++k)
                    args.push_back(keel::move(v.cslAt(k)));
            else
                args.push_back(keel::move(v));
        }
        return args;
    };

    auto *var = env->get(name);
    if (var) {
        if (var->isFuncHandle()) {
            auto args = buildArgs();
            return callFuncHandle(*var, args, env, node);
        }
        // OBJECT: obj(i…) dispatches to the class subsref overload.
        if (var->isObject()) {
            const BuiltinClass *cls = engine_.findClass(var->objectClassNameRef());
            if (cls && cls->subsref) {
                const keel::usize nsub = node->children.size() - 1;
                keel::Array<Value> args;
                args.reserve(nsub);
                for (keel::usize i = 0; i < nsub; ++i) {
                    IndexContextGuard guard(indexContextStack_,
                                            {var, keel::as<keel::i32>(i), keel::as<keel::i32>(nsub)});
                    Value v = execNodeExpand(node->children[i + 1].get(), env);
                    if (v.isCsl())
                        for (keel::usize k = 0; k < v.cslCount(); ++k)
                            args.push_back(keel::move(v.cslAt(k)));
                    else
                        args.push_back(keel::move(v));
                }
                Value self = *var;
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                cls->subsref(self, keel::ArrayRef<Value>(args.data(), args.size()), nargout,
                             keel::MutArrayRef<Value>(outBuf, 1), ctx);
                return outBuf[0];
            }
            // No custom subsref → builtin array indexing. execIndexAccess
            // drives the OBJECT-aware elemAt/indexGet/indexGet2D/indexGetND
            // path, so 1-D, 2-D and N-D object-array reads (incl. `end`,
            // ranges, logical, vector subscripts) all work uniformly.
            return execIndexAccess(*var, node, env);
        }
        if (var->isNumeric() || var->isLogical() || var->isChar() || var->isCell()
            || var->isStruct() || var->isString())
            return execIndexAccess(*var, node, env);
    }

    // OBJECT: ClassName(args) constructs an instance when `name` is a
    // registered class not shadowed by a variable (object model §3).
    if (!var) {
        if (const BuiltinClass *cls = engine_.findClass(name); cls && cls->construct) {
            auto args = buildArgs();
            CallContext ctx{&engine_, env};
            return engine_.constructChecked(cls, keel::ArrayRef<Value>(args.data(), args.size()), ctx);
        }
    }

    // Slow path: look up, cache, and call
    {
        auto args = buildArgs();

        // OBJECT function-form dispatch: m(obj, ...) where obj's class
        // defines method m beats a same-named global function (MATLAB).
        // Kept in the slow path (object-method calls don't get cached),
        // so the common builtin path stays on its fast cache.
        if (!args.empty() && args[0].isObject()) {
            const BuiltinClass *cls = engine_.findClass(args[0].objectClassNameRef());
            if (const auto *m = cls ? cls->methods.get(name) : nullptr) {
                Value self = args[0];
                keel::ArrayRef<Value> rest(args.data() + 1, args.size() - 1);
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                (*m)(self, rest, nargout, keel::MutArrayRef<Value>(outBuf, 1), ctx);
                return outBuf[0];
            }
        }

        // Lexical nested functions first, then user-defined over imports/builtins.
        if (env) {
            if (const UserFunction *nf = env->findNestedFunc(name)) {
                return callUserFunction(*nf, args, env, node);
            }
        }
        if (auto *uf = engine_.lookupUserFunction(name, env)) {
            return callUserFunction(*uf, args, env, node);
        }
        if (const ExternalFunc *fn = engine_.findExternal(name, env)) {
            Value outBuf[1];
            CallContext ctx{&engine_, env};
            (*fn)(args, nargout, keel::MutArrayRef<Value>(outBuf, 1), ctx);
            return outBuf[0];
        }
    }

    if (var) {
        auto msg = keel::format(
            "Cannot index into variable '{}' of type {}, and no function '{}' was found",
            name, mtypeName(var->type()), name);
        throw std::runtime_error(msg.c_str());
    }
    auto msg = engine_.formatUndefinedFunctionError(
        name, keel::format("Undefined function or variable: {}", name));
    throw std::runtime_error(msg.c_str());
}

Value TreeWalker::execIndexAccess(const Value &var, const ASTNode *callNode, Environment *env)
{
    keel::usize nargs = callNode->children.size() - 1;

    auto checkBounds = [](keel::ArrayRef<keel::usize> indices, keel::usize limit, const char *ctx) {
        for (auto idx : indices) {
            if (idx >= limit) {
                auto msg = keel::format("Index exceeds array dimensions ({}: {} > {})",
                                        ctx, idx + 1, limit);
                throw std::runtime_error(msg.c_str());
            }
        }
    };

    // CHAR routes through the generic path below. 1D reads go through
    // Value::indexGet (which has a CHAR case building a char row via
    // fromString); 2D/3D reads use the memcpy path in indexGet2D /
    // indexGet3D (elementSize(CHAR)==1, so the raw byte copy preserves
    // matrix shape). Indexed assignment works via writeElem / writeScalar.

    if (nargs == 1) {
        // x(:) — the whole-array linear index ALWAYS yields a numel×1 column,
        // independent of the source's orientation. (General linear indices
        // x(idx) follow the index/source shape and go through the paths below.)
        // The bare colon parses as an empty COLON_EXPR. Matches the VM + MATLAB.
        {
            const ASTNode *sub = callNode->children[1].get();
            if (sub->type == NodeType::COLON_EXPR && sub->children.empty())
                // x(:) linearizes; MATLAB narrows an all-real complex result
                // (unlike a structural reshape, which preserves complex).
                return narrowComplex(var.reshape(var.numel(), 1, engine_.mr_),
                                     engine_.mr_);
        }

        // ── scalar fast path: skip resolveIndex + array entirely ──
        keel::usize scalarIdx;
        if (tryResolveScalarIndex(callNode->children[1].get(), var, 0, 1, env, scalarIdx)) {
            if (scalarIdx >= var.numel()) {
                auto msg = keel::format("Index exceeds array dimensions (linear index: {} > {})",
                                        scalarIdx + 1, var.numel());
                throw std::runtime_error(msg.c_str());
            }
            // Cell () returns 1×1 sub-cell, not content
            if (var.isCell()) {
                auto result = Value::cell(1, 1);
                result.cellAt(0) = var.cellAt(scalarIdx);
                return result;
            }
            return var.elemAt(scalarIdx, engine_.mr_);
        }

        Value idxVal;
        {
            IndexContextGuard guard(indexContextStack_, {&var, 0, 1});
            idxVal = execNode(callNode->children[1].get(), env);
        }
        if (idxVal.isLogical()) {
            return var.logicalIndex(idxVal.logicalData(), idxVal.numel(), engine_.mr_);
        }
        auto indices = Value::resolveIndices(idxVal, var.numel());
        Value res = var.indexGet(indices.data(), indices.size(), engine_.mr_);
        const bool varIsNonScalarVec = !var.dims().is3D() && var.dims().ndim() == 2
            && ((var.dims().rows() == 1 && var.dims().cols() > 1)
                || (var.dims().cols() == 1 && var.dims().rows() > 1));
        const bool ixIsVec = !idxVal.dims().is3D() && idxVal.dims().ndim() == 2
            && (idxVal.dims().rows() == 1 || idxVal.dims().cols() == 1);
        if (indices.size() > 1 && (!varIsNonScalarVec || !ixIsVec)) {
            if (res.dims().rows() != idxVal.dims().rows() || res.dims().cols() != idxVal.dims().cols())
                res = res.reshape(idxVal.dims().rows(), idxVal.dims().cols(), engine_.mr_);
        }
        return res;
    }
    if (nargs == 2) {
        // ── scalar fast path: M(i, j) ──
        {
            keel::usize sri, sci;
            if (tryResolveScalarIndex(callNode->children[1].get(), var, 0, 2, env, sri)
                && tryResolveScalarIndex(callNode->children[2].get(), var, 1, 2, env, sci)) {
                if (sri >= var.dims().rows()) {
                    auto msg = keel::format("Index exceeds array dimensions (row index: {} > {})",
                                            sri + 1, var.dims().rows());
                    throw std::runtime_error(msg.c_str());
                }
                const keel::usize colDim = var.dims().ndim() > 2
                    ? (var.dims().rows() > 0 ? var.dims().numel() / var.dims().rows() : var.dims().cols())
                    : var.dims().cols();
                if (sci >= colDim) {
                    auto msg = keel::format("Index exceeds array dimensions (column index: {} > {})",
                                            sci + 1, colDim);
                    throw std::runtime_error(msg.c_str());
                }
                // Cell () returns 1×1 sub-cell
                if (var.isCell()) {
                    auto result = Value::cell(1, 1);
                    result.cellAt(0) = var.cellAt(var.dims().sub2ind(sri, sci));
                    return result;
                }
                return var.elemAt(var.dims().sub2ind(sri, sci), engine_.mr_);
            }
        }

        auto ri = resolveIndex(callNode->children[1].get(), var, 0, 2, env);
        auto ci = resolveIndex(callNode->children[2].get(), var, 1, 2, env);
        const keel::usize colDim = var.dims().ndim() > 2
            ? (var.dims().rows() > 0 ? var.dims().numel() / var.dims().rows() : var.dims().cols())
            : var.dims().cols();
        checkBounds(ri, var.dims().rows(), "row index");
        checkBounds(ci, colDim, "column index");
        return var.indexGet2D(ri.data(), ri.size(), ci.data(), ci.size(), engine_.mr_);
    }
    if (nargs == 3) {
        auto ri = resolveIndex(callNode->children[1].get(), var, 0, 3, env);
        auto ci = resolveIndex(callNode->children[2].get(), var, 1, 3, env);
        auto pi = resolveIndex(callNode->children[3].get(), var, 2, 3, env);
        return var.indexGet3D(ri.data(), ri.size(), ci.data(), ci.size(),
                             pi.data(), pi.size(), engine_.mr_);
    }
    // ND read: nargs >= 4
    {
        const keel::i32 nd = keel::as<keel::i32>(nargs);
        keel::Array<keel::Array<keel::usize>> idxLists(keel::as<keel::usize>(nd));
        keel::Array<const keel::usize *> idxPtrs(nd);
        keel::Array<keel::usize> idxCounts(nd);
        for (keel::i32 i = 0; i < nd; ++i) {
            idxLists[i] = resolveIndex(callNode->children[i + 1].get(), var, i, nd, env);
            idxPtrs[i] = idxLists[i].data();
            idxCounts[i] = idxLists[i].size();
        }
        return var.indexGetND(idxPtrs.data(), idxCounts.data(), nd, engine_.mr_);
    }
}

// ============================================================
Value TreeWalker::execCellIndex(const ASTNode *node, Environment *env)
{
    keel::usize nidx = node->children.size() - 1;

    // 1-D / 2-D brace index: gather the selected items (cellBraceContents handles colon
    // / vector / range / 2-D slice, evaluating the base once). A single selected element
    // returns the bare element (the hot c{i} scalar path); a multi-select returns a CSL
    // transient that splice contexts flatten and single-value contexts collapse (via the
    // execNode wrapper). See dev-docs/memory/csl_first_class.md.
    if (nidx == 1 || nidx == 2) {
        keel::Array<Value> items = cellBraceContents(node, env);
        if (items.size() == 1)
            return keel::move(items[0]);
        Value out = Value::csl(items.size());
        for (keel::usize i = 0; i < items.size(); ++i)
            out.cslAt(i) = keel::move(items[i]);
        return out;
    }

    // 3-D+ : scalar N-D brace read only (no CSL form yet).
    auto obj = execNode(node->children[0].get(), env);
    if (!obj.isCell())
        throw std::runtime_error("Cell indexing {}-operator requires a cell array");
    if (nidx == 3) {
        Value ridx, cidx, pidx;
        {
            IndexContextGuard guard(indexContextStack_, {&obj, 0, 3});
            ridx = execNode(node->children[1].get(), env);
        }
        {
            IndexContextGuard guard(indexContextStack_, {&obj, 1, 3});
            cidx = execNode(node->children[2].get(), env);
        }
        {
            IndexContextGuard guard(indexContextStack_, {&obj, 2, 3});
            pidx = execNode(node->children[3].get(), env);
        }
        keel::usize r = Value::checkedScalarIndex(ridx.toScalar());
        keel::usize c = Value::checkedScalarIndex(cidx.toScalar());
        keel::usize p = Value::checkedScalarIndex(pidx.toScalar());
        return obj.cellAt(obj.dims().sub2indChecked(r, c, p));
    }
    // ND brace-cell read (nidx ≥ 4): column-major linear index, bounds-checked.
    keel::Array<keel::usize> coords(nidx);
    for (keel::usize i = 0; i < nidx; ++i) {
        IndexContextGuard guard(indexContextStack_, {&obj, keel::as<keel::i32>(i),
                                                     keel::as<keel::i32>(nidx)});
        Value v = execNode(node->children[i + 1].get(), env);
        coords[i] = Value::checkedScalarIndex(v.toScalar());
    }
    const auto &d = obj.dims();
    keel::usize idx = 0, stride = 1;
    for (keel::usize i = 0; i < nidx; ++i) {
        const keel::usize lim = (keel::as<keel::i32>(i) < d.ndim()) ? d.dim(keel::as<keel::i32>(i)) : 1;
        if (coords[i] >= lim) {
            auto msg = keel::format("Cell index out of bounds (dim {}: {} > {})",
                                    i + 1, coords[i] + 1, lim);
            throw std::runtime_error(msg.c_str());
        }
        idx += coords[i] * stride;
        stride *= lim;
    }
    return obj.cellAt(idx);
}

keel::Array<Value> TreeWalker::cellBraceContents(const ASTNode *node, Environment *env)
{
    // 1-D c{:} / c{vec} / c{i} -> the selected elements column-major; 2-D c{r,:} /
    // c{:,j} -> resolveIndex per dim + sub2ind, column-major (mirrors execMultiAssign).
    // 1-based, bounds-checked; `end` resolves per dim through the index-context guard.
    // The base is evaluated ONCE here. 3-D+ is refused (same message as multi-assign).
    Value obj = execNode(node->children[0].get(), env);
    if (obj.isObject()) {
        const BuiltinClass *cls = engine_.findClass(obj.objectClassNameRef());
        if (const auto *m = cls ? cls->methods.get("__brace_get") : nullptr) {
            keel::usize nidx = node->children.size() - 1;
            keel::Array<Value> args;
            args.reserve(nidx);
            for (keel::usize i = 0; i < nidx; ++i) {
                IndexContextGuard guard(indexContextStack_,
                                        {&obj, keel::as<keel::i32>(i), keel::as<keel::i32>(nidx)});
                args.push_back(execNode(node->children[i + 1].get(), env));
            }
            Value outBuf[1];
            CallContext ctx{&engine_, env};
            (*m)(obj, keel::ArrayRef<Value>(args.data(), args.size()),
                 1, keel::MutArrayRef<Value>(outBuf, 1), ctx);
            keel::Array<Value> single;
            single.push_back(keel::move(outBuf[0]));
            return single;
        }
    }
    if (!obj.isCell())
        throw std::runtime_error("Cell contents indexing requires a cell array");
    keel::Array<Value> out;
    keel::usize nidx = node->children.size() - 1;
    if (nidx == 1) {
        const ASTNode *sub = node->children[1].get();
        if (sub->type == NodeType::COLON_EXPR && sub->children.empty()) {
            out.reserve(obj.numel());
            for (keel::usize i = 0; i < obj.numel(); ++i)
                out.push_back(obj.cellAt(i));
        } else {
            IndexContextGuard guard(indexContextStack_, {&obj, 0, 1});
            Value             idxv = execNode(sub, env);
            for (keel::usize id : Value::resolveIndices(idxv, obj.numel()))
                out.push_back(obj.cellAt(id));
        }
    } else if (nidx == 2) {
        auto rowIdx = resolveIndex(node->children[1].get(), obj, 0, 2, env);
        auto colIdx = resolveIndex(node->children[2].get(), obj, 1, 2, env);
        for (keel::usize c : colIdx)
            for (keel::usize r : rowIdx)
                out.push_back(obj.cellAt(obj.dims().sub2ind(r, c)));
    } else {
        auto msg = keel::format("Cell CSL with {} indices not supported", nidx);
        throw std::runtime_error(msg.c_str());
    }
    return out;
}

Value TreeWalker::execFieldAccess(const ASTNode *node, Environment *env)
{
    // Resolve the root once. When it is a bare identifier that is NOT a
    // workspace variable, `Pkg.member` / `ClassName.Const` may be a 0-arg
    // qualified function/constant — resolve that before erroring. (Single
    // env lookup keeps the common `var.field` hot path cheap.)
    Value obj;
    const ASTNode *rootNode = node->children[0].get();
    if (rootNode->type == NodeType::IDENTIFIER) {
        if (Value *v = env->get(rootNode->strValue)) {
            obj = *v;
        } else {
            const ASTNode *root = nullptr;
            keel::String qualified = tryBuildQualifiedName(node, &root);
            if (!qualified.empty()) {
                if (const ExternalFunc *fn = engine_.findExternal(qualified, env)) {
                    Value outBuf[1];
                    CallContext ctx{&engine_, env};
                    (*fn)({}, 1, keel::MutArrayRef<Value>(outBuf, 1), ctx);
                    return outBuf[0];
                }
                if (const BuiltinClass *cls = engine_.findClass(qualified); cls && cls->construct) {
                    CallContext ctx{&engine_, env};
                    return engine_.constructChecked(cls, {}, ctx);
                }
            }
            obj = execNode(rootNode, env); // produces the proper undefined error
        }
    } else {
        obj = execNode(rootNode, env);
    }
    // OBJECT: obj.Prop reads via the class property hook (object model,
    // object_model.md §3). No-arg method call form is wired in P3.
    if (obj.isObject())
        return objectPropGet(obj, node->strValue, env);
    if (!obj.isStruct()) {
        auto msg = keel::format("Dot indexing requires a struct, got {}", mtypeName(obj.type()));
        throw std::runtime_error(msg.c_str());
    }
    if (obj.isStructArray() || obj.numel() == 0) {
        keel::usize n = obj.numel();
        if (n == 0 && !obj.hasField(node->strValue)) {
            auto msg = keel::format("Reference to non-existent field '{}'", node->strValue);
            throw std::runtime_error(msg.c_str());
        }
        Value csl = Value::csl(n, engine_.mr_);
        for (keel::usize i = 0; i < n; ++i) {
            const auto &m = obj.structArrayElem(i);
            const Value *vp = m.get(node->strValue);
            if (!vp) {
                auto msg = keel::format("Reference to non-existent field '{}'", node->strValue);
                throw std::runtime_error(msg.c_str());
            }
            csl.cslAt(i) = *vp;
        }
        return csl;
    }
    if (!obj.hasField(node->strValue)) {
        auto msg = keel::format("Reference to non-existent field '{}'", node->strValue);
        throw std::runtime_error(msg.c_str());
    }
    return obj.field(node->strValue);
}

Value TreeWalker::objectPropGet(const Value &obj, keel::StringRef name, Environment *env)
{
    const keel::StringRef clsName = obj.objectClassNameRef();
    const BuiltinClass *cls = engine_.findClass(clsName);
    if (cls) {
        CallContext ctx{&engine_, env};
        if (cls->propGet) {
            Value out;
            if (cls->propGet(obj, name, out, ctx))
                return out;
        }
        // Bare `obj.method` (no parens) invokes a no-arg method (MATLAB).
        if (const auto *m = cls->methods.get(name)) {
            Value self = obj;
            Value outBuf[1];
            (*m)(self, keel::ArrayRef<Value>(nullptr, 0), 1, keel::MutArrayRef<Value>(outBuf, 1), ctx);
            return outBuf[0];
        }
    }
    auto msg = keel::format("No appropriate property '{}' for class '{}'", name, clsName);
    throw std::runtime_error(msg.c_str());
}

void TreeWalker::objectPropSet(Value &objSlot, keel::StringRef name,
                               const Value &rhs, Environment *env)
{
    const BuiltinClass *cls = engine_.findClass(objSlot.objectClassNameRef());
    if (cls && cls->propSet) {
        CallContext ctx{&engine_, env};
        if (cls->propSet(objSlot, name, rhs, ctx))
            return;
    }
    auto msg = keel::format("Cannot set property '{}' on class '{}'",
                            name, objSlot.objectClassNameRef());
    throw std::runtime_error(msg.c_str());
}

// ============================================================
Value TreeWalker::execMatrixLiteral(const ASTNode *node, Environment *env)
{
    if (node->children.empty())
        return Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_);

    // NOTE: there is deliberately NO in-place row-append fast path here.
    // execMatrixLiteral is target-agnostic — it does not know the assignment
    // LHS — so appending into the first element's variable would corrupt the
    // SOURCE: `b = [a, x]` (or a bare `[a, x]`) would mutate `a`. The VM keeps
    // this optimization safely because its compiler emits HORZCAT_APPEND only
    // when the LHS identifier == the RHS's first element (`a = [a, x]`); the
    // TreeWalker is the fallback backend (not the perf path) and takes the
    // correct allocating general path below.

    // Evaluate all elements per row, with comma-separated-list expansion
    // for struct-array dot access (`[d.field]` / `[s.fname]`).
    keel::Array<keel::Array<Value>> rows;
    bool anyChar = false, allChar = true;

    auto pushElem = [&](Value &&val, keel::Array<Value> &dst) {
        // Empties PARTICIPATE in concatenation (MATLAB: [zeros(0,2),
        // zeros(0,2)] is 0x4) — dropping them here lost the dims and,
        // for all-empty concats, left the assignment unbound
        // (bugs/closed/lang/vertcat-all-empty). Value::horzcat/vertcat
        // apply the probed empty rules.
        if (val.isChar()) anyChar = true;
        else              allChar = false;
        dst.push_back(keel::move(val));
    };

    for (auto &rowNode : node->children) {
        keel::Array<Value> rowElems;
        for (auto &elemNode : rowNode->children) {
            // CSL: `s.f` / `s.(name)` over a multi-element struct
            // array expands to one rowElem per element.
            if ((elemNode->type == NodeType::FIELD_ACCESS
                 || elemNode->type == NodeType::DYNAMIC_FIELD_ACCESS)
                && elemNode->children.size() >= 1) {
                auto base = execNode(elemNode->children[0].get(), env);
                if (base.isStruct() && (base.isStructArray() || base.numel() == 0)) {
                    keel::String fname = (elemNode->type == NodeType::FIELD_ACCESS)
                        ? elemNode->strValue.clone()
                        : execNode(elemNode->children[1].get(), env).toString();
                    if (base.numel() == 0 && !base.hasField(fname)) {
                        auto msg = keel::format("Reference to non-existent field '{}'", fname);
                        throw std::runtime_error(msg.c_str());
                    }
                    for (keel::usize i = 0; i < base.numel(); ++i) {
                        const auto &m = base.structArrayElem(i);
                        const Value *vp = m.get(fname);
                        if (!vp) {
                            auto msg = keel::format("Reference to non-existent field '{}'", fname);
                            throw std::runtime_error(msg.c_str());
                        }
                        pushElem(Value(*vp), rowElems);
                    }
                    continue;
                }
                // OBJECT array CSL: [arr.prop] expands prop over each
                // element via propGet. A scalar object falls through to
                // the generic path (keeps the property-or-method fallback).
                if (base.isObject() && base.objectCount() > 1) {
                    const keel::StringRef clsName = base.objectClassNameRef();
                    const BuiltinClass *cls = engine_.findClass(clsName);
                    keel::String fname = (elemNode->type == NodeType::FIELD_ACCESS)
                        ? elemNode->strValue.clone()
                        : execNode(elemNode->children[1].get(), env).toString();
                    CallContext ctx{&engine_, env};
                    for (keel::usize i = 0; i < base.objectCount(); ++i) {
                        keel::usize idx = i;
                        Value elem = base.objectSubArray(keel::ArrayRef<keel::usize>(&idx, 1), engine_.mr_);
                        Value out;
                        if (!cls || !cls->propGet || !cls->propGet(elem, fname, out, ctx)) {
                            auto msg = keel::format("No appropriate property '{}' for class '{}'",
                                                    fname, clsName);
                            throw std::runtime_error(msg.c_str());
                        }
                        pushElem(keel::move(out), rowElems);
                    }
                    continue;
                }
                // Single struct or non-struct base — fall through to the
                // generic execNode path so existing semantics apply.
            }
            // CSL: an element may expand to a comma-separated list (c{:} / c{idx} /
            // c{r,:}, or any CSL producer) -> splice its elements into the row. A
            // non-CSL element is a single row entry. (Struct-array field CSL is handled
            // by the field-access branch above, which continues before reaching here.)
            Value val = execNodeExpand(elemNode.get(), env);
            if (val.isCsl())
                for (keel::usize k = 0; k < val.cslCount(); ++k)
                    pushElem(keel::move(val.cslAt(k)), rowElems);
            else
                pushElem(keel::move(val), rowElems);
        }
        if (!rowElems.empty())
            rows.push_back(keel::move(rowElems));
    }

    if (rows.empty())
        return Value();

    // All-char: MATLAB pads shorter strings with spaces in vertical stacking
    if (allChar && anyChar) {
        // horzcat each row → one string per row
        keel::Array<keel::String> strs;
        keel::usize maxCols = 0;
        for (auto &rowElems : rows) {
            keel::String s;
            for (auto &v : rowElems) {
                s.append(v.asStringRef());
            }
            maxCols = std::max(maxCols, s.size());
            strs.push_back(keel::move(s));
        }
        if (strs.size() == 1)
            return Value::fromString(strs[0], engine_.mr_);

        // Build char matrix with space-padding
        keel::usize totalRows = strs.size();
        auto result = Value::matrix(totalRows, maxCols, ValueType::CHAR, engine_.mr_);
        char *dst = result.charDataMut();
        std::memset(dst, ' ', totalRows * maxCols);
        for (keel::usize row = 0; row < totalRows; ++row) {
            const auto &s = strs[row];
            for (keel::usize c = 0; c < s.size(); ++c)
                dst[c * totalRows + row] = s[c];
        }
        return result;
    }

    // Numeric / object: horzcat each row, then vertcat all rows
    auto concatWithHook = [&](const char *opName, const Value *elems, keel::usize count) -> Value {
        if (count == 1)
            return elems[0];
        for (keel::usize i = 0; i < count; ++i) {
            if (elems[i].isObject()) {
                const BuiltinClass *cls = engine_.findClass(elems[i].objectClassNameRef());
                if (const auto *m = cls ? cls->ops.get(opName) : nullptr) {
                    Value self = elems[0];
                    Value outBuf[1];
                    CallContext ctx{&engine_, env};
                    (*m)(self, keel::ArrayRef<Value>(elems + 1, count - 1), 1,
                         keel::MutArrayRef<Value>(outBuf, 1), ctx);
                    return outBuf[0];
                }
                break;
            }
        }
        return (keel::StringRef(opName) == "horzcat")
                   ? Value::horzcat(elems, count, engine_.mr_)
                   : Value::vertcat(elems, count, engine_.mr_);
    };

    keel::Array<Value> rowValues;
    rowValues.reserve(rows.size());
    for (auto &rowElems : rows)
        rowValues.push_back(concatWithHook("horzcat", rowElems.data(), rowElems.size()));

    if (rowValues.size() == 1)
        return keel::move(rowValues[0]);

    return concatWithHook("vertcat", rowValues.data(), rowValues.size());
}

Value TreeWalker::execCellLiteral(const ASTNode *node, Environment *env)
{
    if (node->children.empty())
        return Value::cell(0, 0);

    // Collect-then-build (handles a comma-separated-list element c{:} / c{vec},
    // whose run-time count is unknown at parse time -- it can't go through the old
    // pre-sized path). Each element either contributes one value, or, for a single-
    // subscript cell brace-index, its selected contents (CSL). Behaviour for a
    // non-CSL literal is unchanged: row r, column c -> sub2ind(r,c), column-major.
    auto collectRow = [&](const keel::Array<ASTNodePtr> &elems) {
        keel::Array<Value> row;
        for (const auto &e : elems) {
            // CSL: an element may expand to a comma-separated list (c{:} / c{idx} /
            // c{r,:}, or any CSL producer) -> splice its elements; non-CSL is one entry.
            Value v = execNodeExpand(e.get(), env);
            if (v.isCsl())
                for (keel::usize k = 0; k < v.cslCount(); ++k)
                    row.push_back(keel::move(v.cslAt(k)));
            else
                row.push_back(keel::move(v));
        }
        return row;
    };

    const bool                      is2D = node->children[0]->type == NodeType::BLOCK;
    keel::Array<keel::Array<Value>> rows;
    if (!is2D) {
        rows.push_back(collectRow(node->children));  // a 1-row literal of direct elements
    } else {
        for (const auto &rowNode : node->children) rows.push_back(collectRow(rowNode->children));
    }

    const keel::usize numRows = rows.size();
    const keel::usize numCols = rows.empty() ? 0 : rows[0].size();
    for (const auto &r : rows)
        if (r.size() != numCols)
            throw std::runtime_error(
                "Dimensions of cell arrays being concatenated are not consistent");

    auto cell = Value::cell(numRows, numCols);
    for (keel::usize r = 0; r < numRows; ++r)
        for (keel::usize c = 0; c < numCols; ++c)
            cell.cellAt(cell.dims().sub2ind(r, c)) = keel::move(rows[r][c]);
    return cell;
}

static ValueType colonOutputType(const Value *ops, keel::usize n)
{
    ValueType nonDouble = ValueType::DOUBLE;
    bool found = false;
    for (keel::usize i = 0; i < n; ++i) {
        ValueType t = ops[i].type();
        if (t == ValueType::DOUBLE) continue;
        if (!found) { nonDouble = t; found = true; }
        else if (t != nonDouble) {
            throw std::runtime_error(
                "Colon operands must be all the same type, "
                "or mixed with real scalar doubles");
        }
    }
    return nonDouble;
}

Value TreeWalker::execColonExpr(const ASTNode *node, Environment *env)
{
    if (node->children.empty())
        return Value::fromString(":", engine_.mr_);

    auto colonScalar = [](const Value &v) -> keel::f64 {
        return (v.numel() > 1 && v.isNumeric()) ? v.elemAsDouble(0) : v.toScalar();
    };

    if (node->children.size() == 2) {
        Value a = execNode(node->children[0].get(), env);
        Value b = execNode(node->children[1].get(), env);
        Value ops[2] = {a, b};
        if (a.isObject() || b.isObject()) {
            const Value &dom = a.isObject() ? a : b;
            const BuiltinClass *cls = engine_.findClass(dom.objectClassNameRef());
            if (const auto *m = cls ? cls->ops.get("colon") : nullptr) {
                Value self = dom;
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                (*m)(self, keel::ArrayRef<Value>(ops, 2), 1,
                     keel::MutArrayRef<Value>(outBuf, 1), ctx);
                return outBuf[0];
            }
        }
        ValueType t = colonOutputType(ops, 2);
        if (a.isEmpty() || b.isEmpty())
            return Value::matrix(1, 0, t, engine_.mr_);
        return Value::colonRangeTyped(colonScalar(a), colonScalar(b), t, engine_.mr_);
    }

    if (node->children.size() == 3) {
        Value a = execNode(node->children[0].get(), env);
        Value b = execNode(node->children[1].get(), env);
        Value c = execNode(node->children[2].get(), env);
        Value ops[3] = {a, b, c};
        if (a.isObject() || b.isObject() || c.isObject()) {
            const Value &dom = a.isObject() ? a : (c.isObject() ? c : b);
            const BuiltinClass *cls = engine_.findClass(dom.objectClassNameRef());
            if (const auto *m = cls ? cls->ops.get("colon") : nullptr) {
                Value self = dom;
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                (*m)(self, keel::ArrayRef<Value>(ops, 3), 1,
                     keel::MutArrayRef<Value>(outBuf, 1), ctx);
                return outBuf[0];
            }
        }
        ValueType t = colonOutputType(ops, 3);
        if (a.isEmpty() || b.isEmpty() || c.isEmpty())
            return Value::matrix(1, 0, t, engine_.mr_);
        return Value::colonRangeTyped(colonScalar(a), colonScalar(b), colonScalar(c),
                                       t, engine_.mr_);
    }

    return Value();
}

} // namespace numkit
