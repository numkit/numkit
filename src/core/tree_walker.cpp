// src/core/tree_walker.cpp
#include <keel/array.h>
#include <keel/exception.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_set.h>
#include <keel/optional.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/tree_walker.hpp>
#include <numkit/value/error.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/value/index.hpp>
#include "tree_walker_internal.hpp"
#include <cstdio>
#include <cstdlib>

namespace numkit {

TreeWalker::TreeWalker(Engine &engine)
    : engine_(engine)
{}

Environment *TreeWalker::callerEnvAtDepth(keel::i32 n, Environment *fallback) const
{
    if (n < 0)
        n = 0;
    if (n < keel::as<keel::i32>(activeFrames_.size()))
        return activeFrames_[activeFrames_.size() - 1 - n].env;
    return fallback;
}

keel::ArrayRef<keel::String> TreeWalker::currentFrameCallerArgNames() const
{
    if (activeFrames_.empty())
        return {};
    return activeFrames_.back().callerArgNames.as_ref();
}

keel::i64 TreeWalker::currentFunctionNargin() const
{
    for (keel::usize i = activeFrames_.size(); i > 0; --i) {
        if (auto *env = activeFrames_[i - 1].env) {
            if (const Value *v = env->getLocal("nargin"))
                return keel::as<keel::i64>(v->toScalar());
        }
    }
    return -1;
}

keel::i64 TreeWalker::currentFunctionNargout() const
{
    for (keel::usize i = activeFrames_.size(); i > 0; --i) {
        if (auto *env = activeFrames_[i - 1].env) {
            if (const Value *v = env->getLocal("nargout"))
                return keel::as<keel::i64>(v->toScalar());
        }
    }
    return -1;
}

keel::Array<CallStackFrame> TreeWalker::captureCallStack() const
{
    keel::Array<CallStackFrame> out;
    out.reserve(callTrace_.size());
    for (keel::usize i = callTrace_.size(); i-- > 0;)
        out.push_back(callTrace_[i].clone());
    return out;
}

Value TreeWalker::execute(const ASTNode *ast, Environment *env)
{
    // Pre-scan: register all top-level function definitions before executing
    // the main body.  MATLAB makes local functions available to the entire
    // script regardless of where they appear in the file.
    if (ast && ast->type == NodeType::BLOCK) {
        for (auto &child : ast->children) {
            if (child && child->type == NodeType::FUNCTION_DEF)
                execFunctionDef(child.get(), env);
        }
    }

    const keel::String *sf = engine_.currentScriptFile();
    TraceGuard traceGuard(callTrace_,
                          sf ? sf->as_ref() : keel::StringRef{},
                          topLevelName_.as_ref(),
                          ast ? ast->line : 0);

    keel::Optional<DebugController::FrameGuard> dbgFrame;
    if (auto *ctl = debugCtl()) {
        ctl->reset();
        StackFrame frame;
        frame.functionName = topLevelName_.clone();
        frame.env = env;
        dbgFrame.emplace(*ctl, keel::move(frame));
    }

    return execNode(ast, env);
}

bool TreeWalker::isKnownFunction(keel::StringRef name) const
{
    return engine_.externalFuncs_.contains(name) || engine_.hasUserFunction(name)
           || name == "tic" || name == "toc";
}

// ============================================================
void TreeWalker::output(keel::StringRef s)
{
    engine_.outputText(s);
}

void TreeWalker::displayValue(keel::StringRef name, const Value &val)
{
    if (val.isObject()) {
        engine_.displayObject(name, val); // honours class disp/display override
        return;
    }
    output(val.formatDisplay(name));
}

// Returns true and sets `outIdx` if the index expression evaluates to a scalar integer.
bool TreeWalker::tryResolveScalarIndex(const ASTNode *indexExpr,
                                       const Value &array,
                                       keel::i32 dim,
                                       keel::i32 ndims,
                                       Environment *env,
                                       keel::usize &outIdx)
{
    // Fast path for number literals: B(3)
    if (indexExpr->type == NodeType::NUMBER_LITERAL) {
        return tryIndex0(indexExpr->numValue, outIdx);
    }

    // Fast path for simple identifiers: B(i)
    if (indexExpr->type == NodeType::IDENTIFIER) {
        const keel::String &name = indexExpr->strValue;
        if (name == "end") {
            keel::usize sz;
            if (ndims == 1) {
                sz = array.numel();
            } else if (dim == ndims - 1 && array.dims().ndim() > keel::as<keel::i32>(ndims)) {
                sz = 1;
                for (keel::i32 d = dim; d < array.dims().ndim(); ++d)
                    sz *= array.dims().dimSize(d);
            } else {
                sz = array.dims().dimSize(dim);
            }
            if (sz == 0)
                return false;
            outIdx = sz - 1;
            return true;
        }
        auto *val = env->get(name);
        if (val && val->isScalar() && val->type() == ValueType::DOUBLE) {
            return tryIndex0(val->toScalar(), outIdx);
        }
        return false;
    }

    return false;
}

keel::Array<keel::usize> TreeWalker::resolveIndex(
    const ASTNode *indexExpr, const Value &array, keel::i32 dim, keel::i32 ndims, Environment *env)
{
    IndexContextGuard guard(indexContextStack_, {&array, dim, ndims});

    Value val = execNode(indexExpr, env);

    keel::Array<keel::usize> indices;

    if (val.isChar() && val.asStringRef() == ":") {
        keel::usize sz;
        if (ndims == 1) {
            sz = array.numel();
        } else if (dim == ndims - 1 && array.dims().ndim() > ndims) {
            sz = 1;
            for (keel::i32 d = dim; d < array.dims().ndim(); ++d)
                sz *= array.dims().dimSize(d);
        } else {
            sz = array.dims().dimSize(dim);
        }
        indices.resize(sz);
        for (keel::usize i = 0; i < sz; ++i)
            indices[i] = i;
        return indices;
    }

    if (val.isLogical()) {
        const keel::u8 *ld = val.logicalData();
        for (keel::usize i = 0; i < val.numel(); ++i)
            if (ld[i])
                indices.push_back(i);
        return indices;
    }

    if (val.isNumeric() || val.isChar()) {
        // Any numeric (double / single / int*) or char (code-point) index,
        // scalar OR array — read generically via elemAsDouble so A(int32(2))
        // and A(int16([1 3])) work. (doubleData() threw "Not a double array" on
        // non-DOUBLE indices; integer ARRAYS were previously "not yet
        // supported".) Validate positivity/integrality.
        indices.reserve(val.numel());
        for (keel::usize i = 0; i < val.numel(); ++i)
            indices.push_back(Value::checkedScalarIndex(val.elemAsDouble(i)));
        return indices;
    }

    auto msg = keel::format("Invalid index type: {}", mtypeName(val.type()));
    throw numkit::Error(msg.c_str());
}

// ============================================================
Value TreeWalker::execNodeInner(const ASTNode *node, Environment *env)
{
    switch (node->type) {
    case NodeType::BLOCK:
        return execBlock(node, env);
    case NodeType::NUMBER_LITERAL:
        return Value::scalar(node->numValue, engine_.mr_);
    case NodeType::STRING_LITERAL:
        return Value::fromString(node->strValue, engine_.mr_);
    case NodeType::DQSTRING_LITERAL:
        return Value::stringScalar(node->strValue, engine_.mr_);
    case NodeType::BOOL_LITERAL:
        return Value::logicalScalar(node->boolValue, engine_.mr_);
    case NodeType::IMAG_LITERAL:
        return Value::complexScalar(0.0, node->numValue, engine_.mr_);
    case NodeType::IDENTIFIER:
        return execIdentifier(node, env);
    case NodeType::ASSIGN:
        return execAssign(node, env);
    case NodeType::MULTI_ASSIGN:
        return execMultiAssign(node, env);
    case NodeType::BINARY_OP:
        return execBinaryOp(node, env);
    case NodeType::UNARY_OP:
        return execUnaryOp(node, env);
    case NodeType::CALL:
        return execCall(node, env);
    case NodeType::SUPERCLASS_REF: {
        // Bare `lhs@Base` with no call — invalid: a super-call must be
        // invoked (`obj@Base(args)` / `method@Base(obj,...)`).
        auto msg = keel::format(
            "superclass reference '{}' must be called: use lhs@{}(...)",
            node->strValue, node->strValue);
        throw numkit::Error(msg.c_str());
    }
    case NodeType::CELL_INDEX:
        return execCellIndex(node, env);
    case NodeType::FIELD_ACCESS:
        return execFieldAccess(node, env);
    case NodeType::DYNAMIC_FIELD_ACCESS: {
        auto obj = execNode(node->children[0].get(), env);
        keel::String fname = execNode(node->children[1].get(), env).toString();
        if (obj.isObject())
            return objectPropGet(obj, fname, env);
        if (!obj.isStruct())
            throw numkit::Error("Dot indexing requires a struct");
        if (obj.isStructArray() || obj.numel() == 0) {
            keel::usize n = obj.numel();
            if (n == 0 && !obj.hasField(fname)) {
                auto msg = keel::format("Reference to non-existent field '{}'", fname);
                throw numkit::Error(msg.c_str());
            }
            Value csl = Value::csl(n, engine_.mr_);
            for (keel::usize i = 0; i < n; ++i) {
                const auto &m = obj.structArrayElem(i);
                const Value *vp = m.get(fname);
                if (!vp) {
                    auto msg = keel::format("Reference to non-existent field '{}'", fname);
                    throw numkit::Error(msg.c_str());
                }
                csl.cslAt(i) = *vp;
            }
            return csl;
        }
        if (!obj.hasField(fname)) {
            auto msg = keel::format("Reference to non-existent field '{}'", fname);
            throw numkit::Error(msg.c_str());
        }
        return obj.field(fname);
    }
    case NodeType::MATRIX_LITERAL:
        return execMatrixLiteral(node, env);
    case NodeType::CELL_LITERAL:
        return execCellLiteral(node, env);
    case NodeType::COLON_EXPR:
        return execColonExpr(node, env);
    case NodeType::IF_STMT:
        return execIf(node, env);
    case NodeType::FOR_STMT:
        return execFor(node, env);
    case NodeType::WHILE_STMT:
        return execWhile(node, env);
    case NodeType::SWITCH_STMT:
        return execSwitch(node, env);
    case NodeType::BREAK_STMT:
        flowSignal_ = FlowSignal::BREAK;
        return Value();
    case NodeType::CONTINUE_STMT:
        flowSignal_ = FlowSignal::CONTINUE;
        return Value();
    case NodeType::RETURN_STMT:
        flowSignal_ = FlowSignal::RETURN;
        return Value();
    case NodeType::FUNCTION_DEF:
        return execFunctionDef(node, env);
    case NodeType::CLASSDEF_DEF:
        engine_.registerClassDef(node); // register the class; statement yields nothing
        return Value();
    case NodeType::EXPR_STMT:
        return execExprStmt(node, env);
    case NodeType::ANON_FUNC:
        return execAnonFunc(node, env);
    case NodeType::TRY_STMT:
        return execTryCatch(node, env);
    case NodeType::DELETE_ASSIGN:
        return execDeleteAssign(node, env);
    case NodeType::GLOBAL_STMT:
    case NodeType::PERSISTENT_STMT:
        return execGlobalPersistent(node, env);
    case NodeType::COMMAND_CALL:
        return execCommandCall(node, env);
    case NodeType::END_VAL: {
        if (!indexContextStack_.empty()) {
            auto &ctx = indexContextStack_.back();
            if (ctx.array && ctx.array->isObject()) {
                const BuiltinClass *cls = engine_.findClass(ctx.array->objectClassNameRef());
                if (const auto *m = cls ? cls->methods.get("__end") : nullptr) {
                    Value self = *ctx.array;
                    Value eargs[2] = {
                        Value::scalar(keel::as<keel::f64>(ctx.dimension), engine_.mr_),
                        Value::scalar(keel::as<keel::f64>(ctx.ndims), engine_.mr_)};
                    Value out[1];
                    CallContext cctx{&engine_, env};
                    (*m)(self, keel::ArrayRef<Value>(eargs, 2), 1,
                         keel::MutArrayRef<Value>(out, 1), cctx);
                    return out[0];
                }
            }
            keel::usize sz = (ctx.ndims == 1) ? ctx.array->numel()
                                         : ctx.array->dims().dimSize(ctx.dimension);
            return Value::scalar(keel::as<keel::f64>(sz), engine_.mr_);
        }
        throw numkit::Error("'end' used outside of indexing context");
    }
    default:
        throw numkit::Error("Unknown AST node type");
    }
}

static keel::String describeNode(const ASTNode *node)
{
    switch (node->type) {
    case NodeType::CALL:
        if (!node->strValue.empty())
            return keel::format("in call to '{}'", node->strValue);
        if (!node->children.empty() && node->children[0]->type == NodeType::IDENTIFIER)
            return keel::format("in call to '{}'", node->children[0]->strValue);
        return keel::String::from("in function call");
    case NodeType::CELL_INDEX:
        if (!node->children.empty() && node->children[0]->type == NodeType::IDENTIFIER)
            return keel::format("in cell indexing of '{}'", node->children[0]->strValue);
        return keel::String::from("in cell indexing");
    case NodeType::BINARY_OP:
        return keel::format("in operator '{}'", node->strValue);
    case NodeType::UNARY_OP:
        return keel::format("in unary operator '{}'", node->strValue);
    case NodeType::FIELD_ACCESS:
        return keel::format("in field access '.{}'", node->strValue);
    case NodeType::DYNAMIC_FIELD_ACCESS:
        return keel::String::from("in dynamic field access");
    case NodeType::ASSIGN:
        if (!node->children.empty() && node->children[0]->type == NodeType::IDENTIFIER)
            return keel::format("in assignment to '{}'", node->children[0]->strValue);
        return keel::String::from("in assignment");
    case NodeType::COLON_EXPR:
        return keel::String::from("in colon expression");
    case NodeType::MATRIX_LITERAL:
        return keel::String::from("in matrix construction");
    case NodeType::CELL_LITERAL:
        return keel::String::from("in cell construction");
    case NodeType::IDENTIFIER:
        return keel::format("'{}'", node->strValue);
    case NodeType::EXPR_STMT:
        if (!node->children.empty())
            return describeNode(node->children[0].get());
        return {};
    default:
        return {};
    }
}

Value TreeWalker::execNode(const ASTNode *node, Environment *env)
{
    // Single-value sink: collapse any comma-separated list the expression produced.
    // The isCsl() guard keeps the hot path a single branch + move (collapseCsl, which
    // takes the value by copy, only runs for an actual CSL). See dev-docs/memory/csl_first_class.md.
    Value v = execNodeExpand(node, env);
    if (v.isCsl())
        return collapseCsl(keel::move(v));
    return v;
}

Value TreeWalker::execNodeExpand(const ASTNode *node, Environment *env)
{
    if (!node)
        return Value();

    StackGuard::check("tree walker execution");
    if (node->line > 0 && !callTrace_.empty())
        callTrace_.back().line = node->line;

    try {
        return execNodeInner(node, env);
    } catch (Error &e) {
        if (engine_.lastErrorStack().empty()) {
            engine_.setLastErrorStack(captureCallStack());
            keel::StringRef id = e.identifier().empty()
                                     ? keel::StringRef("numkit:error")
                                     : e.identifier().as_ref();
            engine_.recordLastError(e.what(), id, engine_.lastErrorStack().as_ref());
        }
        // Enrich with this node's source location if the inner throw
        // didn't already attach one (e.g. from a public C++ library API).
        if (node->line > 0) {
            keel::StringRef fn = callTrace_.empty() ? keel::StringRef{} : callTrace_.back().name.as_ref();
            e.attachIfMissing(node->line, node->col, fn, describeNode(node));
        }
        throw;
    } catch (const DebugStopException &) {
        throw;
    } catch (const keel::Exception &e) {
        if (engine_.lastErrorStack().empty())
            engine_.setLastErrorStack(captureCallStack());
        if (node->line > 0) {
            keel::StringRef fn = callTrace_.empty() ? keel::StringRef{} : callTrace_.back().name.as_ref();
            throw Error(e.what(), node->line, node->col, fn, describeNode(node));
        }
        throw;
    } catch (const std::exception &e) {
        if (engine_.lastErrorStack().empty())
            engine_.setLastErrorStack(captureCallStack());
        if (node->line > 0) {
            keel::StringRef fn = callTrace_.empty() ? keel::StringRef{} : callTrace_.back().name.as_ref();
            throw Error(e.what(), node->line, node->col, fn, describeNode(node));
        }
        throw;
    }
}

Value TreeWalker::execBlock(const ASTNode *node, Environment *env)
{
    StackGuard::check("tree walker execution");
    Value last = Value();
    for (auto &child : node->children) {
        // ── Debug hook: check for line change, breakpoints ──
        if (auto *ctl = debugCtl()) {
            if (child->line > 0) {
                if (!ctl->checkLine(keel::as<keel::u16>(child->line),
                                    keel::as<keel::u16>(child->col),
                                    currentRecursionDepth_))
                    throw DebugStopException();
            }
        }

        last = execNode(child.get(), env);
        if (flowSignal_ != FlowSignal::NONE)
            return last;
    }
    return last;
}

// ============================================================
Value TreeWalker::execIdentifier(const ASTNode *node, Environment *env, keel::usize nargout)
{
    const keel::String &name = node->strValue;

    auto *val = env->get(name);
    if (val)
        return *val;

    // MATLAB precedence: lexical nested functions first, then user-defined
    // functions, then external (builtin) registrations sharing a short name.
    if (env) {
        if (const UserFunction *nf = env->findNestedFunc(name))
            return callUserFunction(*nf, {}, env);
    }
    const auto resolved = engine_.resolveCallable(name, env);
    if (resolved.kind == Engine::ResolvedCallable::Kind::UserFunc)
        return callUserFunction(*resolved.userFunc, {}, env);
    if (resolved.kind == Engine::ResolvedCallable::Kind::External) {
        Value outBuf[1];
        CallContext ctx{&engine_, env};
        (*resolved.extFunc)({}, nargout, keel::MutArrayRef<Value>(outBuf, 1), ctx);
        return outBuf[0].isEmpty() ? Value() : outBuf[0];
    }
    if (resolved.kind == Engine::ResolvedCallable::Kind::ClassCtor) {
        CallContext ctx{&engine_, env};
        return engine_.constructChecked(resolved.builtinClass, keel::ArrayRef<Value>(nullptr, 0), ctx);
    }

    // MATLAB-exact error for the nargin/nargout pseudo-vars when they're
    // referenced outside of any function scope. Inside a function they are
    // setLocal'd on entry so env->get() above finds them.
    if (name == "nargin" || name == "nargout")
        throw numkit::Error(
            "You can only call nargin/nargout from within a function.");

    auto msg = keel::format("Undefined variable or function: {}", name);
    throw numkit::Error(msg.c_str());
}

// ============================================================
Value TreeWalker::execAssign(const ASTNode *node, Environment *env)
{
    auto *lhs = node->children[0].get();
    auto rhs = execNode(node->children[1].get(), env);

    if (lhs->type == NodeType::IDENTIFIER) {
        // Fast path: if variable already exists as a scalar double
        // and rhs is scalar double, write in-place (no hash lookup for set)
        if (rhs.isScalar() && rhs.type() == ValueType::DOUBLE
            && !env->isGlobal(lhs->strValue) && !env->isPersistent(lhs->strValue)) {
            Value *existing = env->getLocal(lhs->strValue);
            if (existing && existing->isScalar() && existing->type() == ValueType::DOUBLE) {
                *existing->doubleDataMut() = rhs.toScalar();
                if (!node->suppressOutput)
                    displayValue(lhs->strValue, *existing);
                return *existing;
            }
        }
        env->set(lhs->strValue, rhs);
        if (!node->suppressOutput)
            displayValue(lhs->strValue, rhs);
        return rhs;
    }

    // Non-identifier lvalue (a(i)=, s.f=, s.(e)=, c{i}=). Behaviour
    // unchanged: no auto-display for these targets in the TreeWalker.
    assignLValue(lhs, rhs, env);
    return rhs;
}

void TreeWalker::assignLValue(const ASTNode *lhs, const Value &rhs, Environment *env)
{
    if (lhs->type == NodeType::IDENTIFIER) {
        env->set(lhs->strValue, rhs);
        return;
    } else if (lhs->type == NodeType::CALL) {
        execIndexedAssign(lhs, rhs, env);
    } else if (lhs->type == NodeType::FIELD_ACCESS) {
        execFieldAssign(lhs, rhs, env);
    } else if (lhs->type == NodeType::DYNAMIC_FIELD_ACCESS) {
        // s.(expr) = val
        auto *objNode = lhs->children[0].get();
        keel::String fname = execNode(lhs->children[1].get(), env).toString();
        if (objNode->type == NodeType::IDENTIFIER) {
            auto *var = env->get(objNode->strValue);
            if (var && engine_.constantsEnv_
                && var == engine_.constantsEnv_->getLocal(objNode->strValue)) {
                var = nullptr;
            }
            if (!var) {
                env->set(objNode->strValue, Value::structure());
                var = env->get(objNode->strValue);
            }
            if (var->isObject()) {
                objectPropSet(*var, fname, rhs, env);
                return;
            }
            if (!var->isStruct())
                *var = Value::structure();
            var->field(fname) = rhs;
        } else {
            throw numkit::Error("Dynamic field assign: unsupported target");
        }
    } else if (lhs->type == NodeType::CELL_INDEX) {
        execCellAssign(lhs, rhs, env);
    } else {
        throw numkit::Error("Invalid assignment target");
    }

    // If a nested mutation (`obj.Prop.Sub = v`, `obj.Prop(i) = v`,
    // `obj.Prop{i} = v`) modified a cached property slot on an object with a
    // custom `propSet` hook, propagate the updated property value back through
    // `objectPropSet`.
    const ASTNode *cur = lhs->children.empty() ? nullptr : lhs->children[0].get();
    while (cur && !cur->children.empty()) {
        if (cur->type == NodeType::FIELD_ACCESS
            && (cur->children[0]->type == NodeType::IDENTIFIER
                || cur->children[0]->type == NodeType::FIELD_ACCESS)) {
            Value &baseObj = resolveObjectSlot(cur->children[0].get(), env);
            if (baseObj.isObject()) {
                const BuiltinClass *cls = engine_.findClass(baseObj.objectClassNameRef());
                const auto *st = baseObj.objectStateConst();
                if (cls && cls->propSet && st) {
                    if (const Value *vp = st->props.get(cur->strValue)) {
                        Value updated = *vp;
                        objectPropSet(baseObj, cur->strValue, updated, env);
                        break;
                    }
                }
            }
        }
        cur = cur->children[0].get();
    }
}

void TreeWalker::execIndexedAssign(const ASTNode *lhs, const Value &rhs, Environment *env)
{
    // The object being indexed may itself be any lvalue: a variable
    // (`a(2)=…`), a struct field (`s.x(2)=…`), a struct-array element
    // field (`d(i).a(2)=…`), or cell content (`c{i}(2)=…`). Resolve it
    // to a mutable slot; the index write below targets that slot. For a
    // bare identifier this is just the variable slot — no copy, hot path
    // unchanged.
    auto *var = &resolveObjectSlot(lhs->children[0].get(), env);

    keel::usize nargs = lhs->children.size() - 1;

    // Symbolic function definition: f(x) = expr or g(x, y) = expr when any subscript is `sym`.
    if (nargs >= 1) {
        bool allPlainNonColon = true;
        for (keel::usize i = 0; i < nargs; ++i) {
            const ASTNode *sub = lhs->children[i + 1].get();
            if (sub->type == NodeType::COLON_EXPR && sub->children.empty()) {
                allPlainNonColon = false;
                break;
            }
        }
        if (allPlainNonColon) {
            // Check whether any identifier subscript already holds an object with
            // `__indexAssign` (or rhs itself is an object) before evaluating subscripts.
            bool maybeObjSubscript = rhs.isObject();
            if (!maybeObjSubscript) {
                for (keel::usize i = 0; i < nargs; ++i) {
                    const ASTNode *sub = lhs->children[i + 1].get();
                    if (sub->type == NodeType::IDENTIFIER) {
                        const Value *sv = env->get(sub->strValue);
                        if (sv && sv->isObject()) {
                            const BuiltinClass *cls = engine_.findClass(sv->objectClassNameRef());
                            if (cls && cls->methods.get("__indexAssign")) {
                                maybeObjSubscript = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (maybeObjSubscript) {
                keel::Array<Value> idxVals;
                idxVals.reserve(nargs);
                bool hasObjIdx = false;
                for (keel::usize i = 0; i < nargs; ++i) {
                    Value sv = execNode(lhs->children[i + 1].get(), env);
                    if (sv.isObject())
                        hasObjIdx = true;
                    idxVals.push_back(keel::move(sv));
                }
                if (hasObjIdx) {
                    Value out;
                    if (engine_.tryObjectSubscriptAssign(idxVals.as_ref(), rhs, env, out)) {
                        *var = keel::move(out);
                        return;
                    }
                }
            }
        }
    }

    // OBJECT: obj(i…) = v dispatches to the class subsasgn overload,
    // which mutates `var` in place (value/handle rule via objectStateMut).
    // args = [subscripts…, value].
    if (var->isObject()) {
        const BuiltinClass *cls = engine_.findClass(var->objectClassNameRef());
        if (cls && cls->subsasgn) {
            keel::Array<Value> args;
            args.reserve(nargs + 1);
            for (keel::usize i = 0; i < nargs; ++i) {
                IndexContextGuard guard(indexContextStack_,
                                        {var, keel::as<keel::i32>(i), keel::as<keel::i32>(nargs)});
                args.push_back(execNode(lhs->children[i + 1].get(), env));
            }
            args.push_back(rhs);
            if (engine_.tryObjectSubsasgn(*var, args.as_ref(), env))
                return;
        }
        // No custom subsasgn → fall through to the builtin object-array
        // element store below (arr(i) = obj).
    }

    // Builtin object-array indexed assignment: arr(i)=obj / arr(i,j)=obj.
    // Fires when the RHS is an object and the target is empty/unset (→ a
    // Builtin object-array element store + grow (any rank).
    if (engine_.prepareObjectArrayStore(*var, rhs)) {
        keel::Array<keel::Array<keel::usize>> perDim;
        perDim.resize(nargs);
        for (keel::usize d = 0; d < nargs; ++d) {
            const ASTNode *sub = lhs->children[d + 1].get();
            if ((var->isUnset() || var->isEmpty())
                && sub->type == NodeType::COLON_EXPR && sub->children.empty()) {
                const keel::usize n = (nargs == 2 && d == 1) ? rhs.numel() : rhs.dims().dim(keel::as<keel::i32>(d));
                perDim[d].resize(n);
                for (keel::usize k = 0; k < n; ++k) perDim[d][k] = k;
            } else {
                perDim[d] = resolveIndex(sub, *var,
                                         keel::as<keel::i32>(d), keel::as<keel::i32>(nargs), env);
            }
        }
        engine_.objectStoreSlice(*var, perDim, rhs, env);
        return;
    }

    if (var->isChar() && rhs.isChar()) {
        if (nargs == 1) {
            auto indices = resolveIndex(lhs->children[1].get(), *var, 0, 1, env);
            // Grow char array if any index exceeds current size (space-fill)
            keel::usize maxIdx = 0;
            for (auto idx : indices)
                if (idx >= maxIdx) maxIdx = idx + 1;
            if (maxIdx > var->numel()) {
                bool isColVec = (var->dims().cols() == 1 && var->dims().rows() > 1);
                if (isColVec)
                    var->resize(maxIdx, 1, engine_.mr_);
                else
                    var->resize(1, maxIdx, engine_.mr_);
            }
            const keel::StringRef rs = rhs.asStringRef();
            char *data = var->charDataMut();
            if (rs.size() == 1) {
                for (auto idx : indices)
                    data[idx] = rs[0];
            } else {
                for (keel::usize i = 0; i < indices.size() && i < rs.size(); ++i)
                    data[indices[i]] = rs[i];
            }
            return;
        }
    }

    if (nargs == 1) {
        if (rhs.isStruct() && (var->isUnset() || var->isEmpty() || var->isStruct())) {
            auto indices = resolveIndex(lhs->children[1].get(), *var, 0, 1, env);
            if (var->isUnset() || var->isEmpty())
                *var = Value::structArray(0, 0, engine_.mr_);
            for (keel::usize idx : indices) {
                var->growStructArrayTo(idx, engine_.mr_);
                for (const auto &name : rhs.fieldNamesInOrder())
                    var->setField(idx, name, rhs.field(name));
            }
            return;
        }
        // ── scalar fast path: B(i) = scalar ──
        if (rhs.isScalar()) {
            keel::usize scalarIdx;
            if (tryResolveScalarIndex(lhs->children[1].get(), *var, 0, 1, env, scalarIdx)) {
                var->ensureSize(scalarIdx, engine_.mr_);
                var->elemSet(scalarIdx, rhs);
                return;
            }
        }

        auto indices = resolveIndex(lhs->children[1].get(), *var, 0, 1, env);
        if (!rhs.isScalar() && rhs.numel() != indices.size())
            throw numkit::Error("Subscripted assignment dimension mismatch");
        for (auto idx : indices)
            var->ensureSize(idx, engine_.mr_);
        var->indexSet(indices.data(), indices.size(), rhs);
    } else if (nargs == 2) {
        // ── scalar fast path: M(i,j) = scalar ──
        if (rhs.isScalar()) {
            keel::usize ri, ci;
            if (tryResolveScalarIndex(lhs->children[1].get(), *var, 0, 2, env, ri)
                && tryResolveScalarIndex(lhs->children[2].get(), *var, 1, 2, env, ci)) {
                keel::usize needR = ri + 1, needC = ci + 1;
                if (needR > var->dims().rows() || needC > var->dims().cols())
                    var->resize(keel::max(var->dims().rows(), needR),
                                keel::max(var->dims().cols(), needC),
                                engine_.mr_);
                var->elemSet(var->dims().sub2ind(ri, ci), rhs);
                return;
            }
        }

        const ASTNode *rSub = lhs->children[1].get();
        const ASTNode *cSub = lhs->children[2].get();
        bool riIsColon = (rSub->type == NodeType::COLON_EXPR && rSub->children.empty());
        bool ciIsColon = (cSub->type == NodeType::COLON_EXPR && cSub->children.empty());

        auto rowIdx = riIsColon ? keel::Array<keel::usize>() : resolveIndex(rSub, *var, 0, 2, env);
        auto colIdx = ciIsColon ? keel::Array<keel::usize>() : resolveIndex(cSub, *var, 1, 2, env);

        keel::usize maxR = var->dims().rows(), maxC = var->dims().cols();
        for (auto r : rowIdx)
            maxR = keel::max(maxR, r + 1);
        for (auto c : colIdx)
            maxC = keel::max(maxC, c + 1);
        if (var->isEmpty()) {
            if (riIsColon && ciIsColon) {
                maxR = rhs.dims().rows();
                maxC = rhs.dims().cols();
            } else if (riIsColon) {
                maxR = (colIdx.size() == 1) ? rhs.numel() : rhs.dims().rows();
            } else if (ciIsColon) {
                maxC = (rowIdx.size() == 1) ? rhs.numel() : rhs.dims().cols();
            }
        }
        const keel::usize target2D = (riIsColon ? maxR : rowIdx.size())
                              * (ciIsColon ? maxC : colIdx.size());
        if (!rhs.isScalar() && rhs.numel() != target2D)
            throw numkit::Error("Subscripted assignment dimension mismatch");
        if (maxR > var->dims().rows() || maxC > var->dims().cols())
            var->resize(maxR, maxC, engine_.mr_);

        if (riIsColon) rowIdx = resolveIndex(rSub, *var, 0, 2, env);
        if (ciIsColon) colIdx = resolveIndex(cSub, *var, 1, 2, env);

        var->indexSet2D(rowIdx.data(), rowIdx.size(),
                        colIdx.data(), colIdx.size(), rhs);
    } else if (nargs == 3) {
        // ── scalar fast path: A(r,c,p) = scalar ──
        if (rhs.isScalar()) {
            keel::usize ri, ci, pi;
            if (tryResolveScalarIndex(lhs->children[1].get(), *var, 0, 3, env, ri)
                && tryResolveScalarIndex(lhs->children[2].get(), *var, 1, 3, env, ci)
                && tryResolveScalarIndex(lhs->children[3].get(), *var, 2, 3, env, pi)) {
                keel::usize needR = ri + 1, needC = ci + 1, needP = pi + 1;
                if (needR > var->dims().rows() || needC > var->dims().cols()
                    || needP > var->dims().pages())
                    var->resize3d(keel::max(var->dims().rows(), needR),
                                  keel::max(var->dims().cols(), needC),
                                  keel::max(var->dims().pages(), needP),
                                  engine_.mr_);
                var->elemSet(var->dims().sub2ind(ri, ci, pi), rhs);
                return;
            }
        }

        const ASTNode *rSub = lhs->children[1].get();
        const ASTNode *cSub = lhs->children[2].get();
        const ASTNode *pSub = lhs->children[3].get();
        bool riColon = (rSub->type == NodeType::COLON_EXPR && rSub->children.empty());
        bool ciColon = (cSub->type == NodeType::COLON_EXPR && cSub->children.empty());
        bool piColon = (pSub->type == NodeType::COLON_EXPR && pSub->children.empty());

        auto rowIdx = riColon ? keel::Array<keel::usize>() : resolveIndex(rSub, *var, 0, 3, env);
        auto colIdx = ciColon ? keel::Array<keel::usize>() : resolveIndex(cSub, *var, 1, 3, env);
        auto pageIdx = piColon ? keel::Array<keel::usize>() : resolveIndex(pSub, *var, 2, 3, env);

        keel::usize maxR = var->dims().rows(), maxC = var->dims().cols(), maxP = var->dims().pages();
        for (auto r : rowIdx)
            maxR = keel::max(maxR, r + 1);
        for (auto c : colIdx)
            maxC = keel::max(maxC, c + 1);
        for (auto p : pageIdx)
            maxP = keel::max(maxP, p + 1);
        if (var->isEmpty() && (!rhs.isScalar() || (var->dims().rows() == 0 && var->dims().cols() == 0 && !rhs.isEmpty()))) {
            const keel::i32 nColons = (riColon ? 1 : 0) + (ciColon ? 1 : 0) + (piColon ? 1 : 0);
            const keel::usize nonColonProd = (riColon ? 1 : rowIdx.size())
                                      * (ciColon ? 1 : colIdx.size())
                                      * (piColon ? 1 : pageIdx.size());
            if (nColons == 1 && nonColonProd == 1 && rhs.dims().isVector()) {
                if (riColon) maxR = rhs.numel();
                else if (ciColon) maxC = rhs.numel();
                else if (piColon) maxP = rhs.numel();
            } else if (riColon && ciColon && piColon) {
                maxR = rhs.dims().rows();
                maxC = rhs.dims().cols();
                maxP = rhs.dims().pages();
            } else if (nColons > 0) {
                keel::usize rCursor = 0;
                keel::i32 colonsLeft = nColons;
                auto nextRhsDim = [&]() -> keel::usize {
                    while (rCursor < keel::as<keel::usize>(rhs.dims().ndim())
                           && rhs.dims().dim(keel::as<keel::i32>(rCursor)) == 1
                           && (keel::as<keel::usize>(rhs.dims().ndim()) - rCursor) > keel::as<keel::usize>(colonsLeft)) {
                        ++rCursor;
                    }
                    --colonsLeft;
                    return (rCursor < keel::as<keel::usize>(rhs.dims().ndim()))
                               ? rhs.dims().dim(keel::as<keel::i32>(rCursor++))
                               : 1;
                };
                if (riColon) maxR = nextRhsDim();
                if (ciColon) maxC = nextRhsDim();
                if (piColon) maxP = nextRhsDim();
            }
        }
        const keel::usize target3D = (riColon ? maxR : rowIdx.size())
                              * (ciColon ? maxC : colIdx.size())
                              * (piColon ? maxP : pageIdx.size());
        if (!rhs.isScalar() && rhs.numel() != target3D)
            throw numkit::Error("Subscripted assignment dimension mismatch");
        if (maxR > var->dims().rows() || maxC > var->dims().cols() || maxP > var->dims().pages())
            var->resize3d(keel::max(var->dims().rows(), maxR),
                          keel::max(var->dims().cols(), maxC),
                          keel::max(var->dims().pages(), maxP),
                          engine_.mr_);

        if (riColon) rowIdx = resolveIndex(rSub, *var, 0, 3, env);
        if (ciColon) colIdx = resolveIndex(cSub, *var, 1, 3, env);
        if (piColon) pageIdx = resolveIndex(pSub, *var, 2, 3, env);

        var->indexSet3D(rowIdx.data(), rowIdx.size(),
                        colIdx.data(), colIdx.size(),
                        pageIdx.data(), pageIdx.size(), rhs);
    } else {
        // ND assign: nargs >= 4. indexSetND auto-grows the target rank
        // (and per-axis size) to fit any out-of-range subscripts.
        const keel::i32 nd = keel::as<keel::i32>(nargs);
        keel::Array<keel::Array<keel::usize>> idxLists(keel::as<keel::usize>(nd));
        keel::Array<const keel::usize *> idxPtrs(nd);
        keel::Array<keel::usize> idxCounts(nd);
        for (keel::i32 i = 0; i < nd; ++i) {
            idxLists[i] = resolveIndex(lhs->children[i + 1].get(), *var, i, nd, env);
            idxPtrs[i] = idxLists[i].data();
            idxCounts[i] = idxLists[i].size();
        }
        var->indexSetND(idxPtrs.data(), idxCounts.data(), nd, rhs);
    }
}

Value &TreeWalker::resolveObjectSlot(const ASTNode *node, Environment *env)
{
    switch (node->type) {
    case NodeType::IDENTIFIER: {
        Value *var = env->get(node->strValue);
        if (var && engine_.constantsEnv_
            && var == engine_.constantsEnv_->getLocal(node->strValue)) {
            var = nullptr;
        }
        if (!var) {
            env->set(node->strValue, Value());
            var = env->get(node->strValue);
        }
        return *var;
    }
    case NodeType::FIELD_ACCESS:
        return resolveFieldLValue(node, env);
    case NodeType::DYNAMIC_FIELD_ACCESS: {
        keel::String fname = execNode(node->children[1].get(), env).toString();
        Value &parent = resolveObjectSlot(node->children[0].get(), env);
        if (!parent.isStruct())
            parent = Value::structure();
        return parent.field(fname);
    }
    case NodeType::CELL_INDEX:
        return resolveCellSlot(node, env);
    default:
        throw numkit::Error("Invalid assignment target");
    }
}

Value &TreeWalker::resolveCellSlot(const ASTNode *node, Environment *env)
{
    // Resolve the cell container (itself any lvalue), coercing an
    // unset/empty slot to a cell, then return a mutable reference to the
    // addressed content, auto-growing to fit the subscripts (any rank).
    Value &cellVar = resolveObjectSlot(node->children[0].get(), env);
    if (cellVar.isUnset() || cellVar.isEmpty())
        cellVar = Value::cell(0, 0);
    if (!cellVar.isCell())
        throw numkit::Error("Cell contents indexing on a non-cell value");

    Value *var = &cellVar;
    const keel::usize nidx = node->children.size() - 1;
    keel::Array<keel::usize> coords(nidx);
    for (keel::usize i = 0; i < nidx; ++i) {
        IndexContextGuard guard(indexContextStack_,
                                {var, keel::as<keel::i32>(i), keel::as<keel::i32>(nidx)});
        Value v = execNode(node->children[i + 1].get(), env);
        coords[i] = Value::checkedScalarIndex(v.toScalar());
    }
    keel::usize linear = var->growCellTo(coords.data(), keel::as<keel::i32>(nidx), engine_.mr_);
    return var->cellAt(linear);
}

Value &TreeWalker::resolveFieldLValue(const ASTNode *node, Environment *env)
{
    auto *objNode = node->children[0].get();
    const keel::String &fieldName = node->strValue;

    if (objNode->type == NodeType::CALL) {
        // d(i).field = val — paren-indexed struct-array element write,
        // any rank, auto-growing to fit (`d(end+1).f`, `d(i,j,k).f`).
        // The indexed object is itself any lvalue (`x.d(i).field`,
        // `c{k}(i).field`, …), resolved to a mutable struct-array slot.
        auto *target = objNode->children[0].get();
        Value *var = &resolveObjectSlot(target, env);
        if (var->isUnset() || var->isEmpty())
            *var = Value::structArray(0, 0, engine_.mr_);
        if (!var->isStruct())
            throw numkit::Error("Indexed field assignment on a non-struct value");

        const keel::usize nargs = objNode->children.size() - 1;
        if (nargs > keel::as<keel::usize>(Dims::kMaxRank))
            throw numkit::Error("Struct array indexing rank exceeds maximum supported rank");
        keel::Array<keel::usize> coords(nargs);
        for (keel::usize a = 0; a < nargs; ++a) {
            IndexContextGuard guard(indexContextStack_,
                                    {var, keel::as<keel::i32>(a), keel::as<keel::i32>(nargs)});
            Value v = execNode(objNode->children[a + 1].get(), env);
            coords[a] = Value::checkedScalarIndex(v.toScalar());
        }
        keel::usize linear;
        if (nargs == 1) {
            linear = coords[0];
            var->growStructArrayTo(linear, engine_.mr_); // preserves row/col vector shape
        } else {
            linear = var->growStructArrayND(coords.data(), keel::as<keel::i32>(nargs),
                                            engine_.mr_);
        }

        auto &fieldMap = var->structArrayElem(linear);
        // BUG #15: track insertion order for fieldnames() before [] auto-creates.
        if (!fieldMap.contains(fieldName)) {
            if (var->isStruct()) {
                // setField updates fieldOrder for new key; reuse it then return ref.
                var->setField(linear, fieldName, Value{});
            } else {
                fieldMap.put(fieldName.clone(), Value{});
            }
        }
        return *var->structArrayElem(linear).get(fieldName);
    }

    // General case: the object is any other addressable lvalue
    // (identifier, nested field, dynamic field, cell content). Resolve
    // it, coerce to a scalar struct, and return the field slot.
    Value &parent = resolveObjectSlot(objNode, env);
    // Compound assignment drilling into an OBJECT property
    // (`obj.prop(i) = v`, `obj.prop.sub = v`, `obj.prop{i} = v`): properties
    // live behind the class hooks with no addressable slot, so detach the
    // state — COW for a value class, shared for a handle, exactly as
    // objectPropSet does — and expose the stored property slot for the
    // in-place sub-assignment. (A set.Prop accessor governs a whole-property
    // `obj.prop = v` write, not element writes, so it is intentionally
    // bypassed here.)
    if (parent.isObject()) {
        auto *st = parent.objectStateMut();
        const BuiltinClass *cls = engine_.findClass(parent.objectClassNameRef());
        if (cls && cls->propGet) {
            CallContext ctx{&engine_, env};
            Value cur;
            if (cls->propGet(parent, fieldName, cur, ctx))
                st->props.put(fieldName.clone(), keel::move(cur));
        }
        if (!st->props.contains(fieldName))
            st->props.put(fieldName.clone(), Value{});
        return *st->props.get(fieldName);
    }
    if (!parent.isStruct())
        parent = Value::structure();
    return parent.field(fieldName);
}

void TreeWalker::execFieldAssign(const ASTNode *lhs, const Value &rhs, Environment *env)
{
    {
        const ASTNode *objNode = lhs->children[0].get();
        if (objNode->type == NodeType::IDENTIFIER
            || objNode->type == NodeType::FIELD_ACCESS
            || objNode->type == NodeType::DYNAMIC_FIELD_ACCESS
            || objNode->type == NodeType::CELL_INDEX) {
            Value &parent = resolveObjectSlot(objNode, env);
            if (parent.isObject()) {
                objectPropSet(parent, lhs->strValue, rhs, env);
                return;
            }
        } else if ((objNode->type == NodeType::CALL || objNode->type == NodeType::INDEX)
                   && objNode->children.size() >= 2) {
            Value &parent = resolveObjectSlot(objNode->children[0].get(), env);
            if (parent.isObject()) {
                const keel::usize nargs = objNode->children.size() - 1;
                keel::usize linear = 0;
                keel::usize stride = 1;
                for (keel::usize a = 0; a < nargs; ++a) {
                    IndexContextGuard guard(indexContextStack_,
                                            {&parent, keel::as<keel::i32>(a), keel::as<keel::i32>(nargs)});
                    Value v = execNode(objNode->children[a + 1].get(), env);
                    keel::usize c = Value::checkedScalarIndex(v.toScalar());
                    linear += c * stride;
                    if (nargs > 1)
                        stride *= parent.dims().dim(keel::as<keel::i32>(a));
                }
                engine_.objectElemPropSet(parent, linear, lhs->strValue, rhs, env);
                return;
            }
        }
    }
    if (lhs->children.size() == 1
        && lhs->children[0]->type == NodeType::IDENTIFIER) {
        auto *var = env->get(lhs->children[0]->strValue);
        if (var && var->isStructArray()) {
            const keel::String &fname = lhs->strValue;
            var->setFieldAll(fname, rhs);  // BUG #15: track insertion order
            return;
        }
    }
    resolveFieldLValue(lhs, env) = rhs;
}

void TreeWalker::execCellAssign(const ASTNode *lhs, const Value &rhs, Environment *env)
{
    if (!lhs->children.empty()
        && (lhs->children[0]->type == NodeType::IDENTIFIER
            || lhs->children[0]->type == NodeType::FIELD_ACCESS)) {
        Value &baseObj = resolveObjectSlot(lhs->children[0].get(), env);
        if (baseObj.isObject()) {
            const BuiltinClass *cls = engine_.findClass(baseObj.objectClassNameRef());
            if (const auto *m = cls ? cls->methods.get("__brace_set") : nullptr) {
                const keel::usize nidx = lhs->children.size() - 1;
                keel::Array<Value> args;
                args.reserve(nidx + 1);
                for (keel::usize i = 0; i < nidx; ++i) {
                    IndexContextGuard guard(indexContextStack_,
                                            {&baseObj, keel::as<keel::i32>(i), keel::as<keel::i32>(nidx)});
                    args.push_back(execNode(lhs->children[i + 1].get(), env));
                }
                args.push_back(rhs);
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                (*m)(baseObj, keel::ArrayRef<Value>(args.data(), args.size()),
                     0, keel::MutArrayRef<Value>(outBuf, 1), ctx);
                return;
            }
        }
    }
    if (lhs->children.size() == 2) {
        Value &cellVar = resolveObjectSlot(lhs->children[0].get(), env);
        if (cellVar.isUnset() || cellVar.isEmpty())
            cellVar = Value::cell(0, 0);
        if (cellVar.isCell()) {
            IndexContextGuard guard(indexContextStack_, {&cellVar, 0, 1});
            Value sub = execNode(lhs->children[1].get(), env);
            const bool isColonSub = sub.isChar() && sub.asStringRef() == ":";
            if (sub.isLogical() || isColonSub || !sub.isScalar()) {
                keel::usize maxLen = cellVar.numel();
                if (!sub.isLogical() && !isColonSub) {
                    for (keel::usize k = 0; k < sub.numel(); ++k) {
                        keel::usize idx = Value::checkedScalarIndex(sub.elemAsDouble(k));
                        if (idx + 1 > maxLen)
                            maxLen = idx + 1;
                    }
                    if (maxLen > cellVar.numel()) {
                        keel::usize lastCoord = maxLen - 1;
                        cellVar.growCellTo(&lastCoord, 1, engine_.mr_);
                    }
                }
                auto indices = Value::resolveIndices(sub, cellVar.numel());
                for (keel::usize idx : indices)
                    cellVar.cellAt(idx) = rhs;
                return;
            }
        }
    }
    resolveObjectSlot(lhs, env) = rhs;
}

// ============================================================
Value TreeWalker::execMultiAssign(const ASTNode *node, Environment *env)
{
    const keel::usize nout = node->returnNames.size();
    auto results = execCallMulti(node->children[0].get(), env, nout);

    // MATLAB: requesting more outputs than the RHS produces is an error
    // at the call site ("Too many output arguments"), not a silently
    // unassigned target that later reads as a phantom undefined function
    // (bug #44 — `[c,sz,n2,p] = bwconncomp(A)` where bwconncomp yields
    // one value). An unfilled output slot is the unset sentinel.
    for (keel::usize i = 0; i < nout; ++i)
        if (i >= results.size() || results[i].isUnset())
            throw numkit::Error("Too many output arguments.");

    // Complex-target path: at least one output is a general lvalue
    // (`s.f`, `a(i)`, `c{i}`, ...). lhsTargets is authoritative; a
    // nullptr entry is an ignored `~`.
    if (!node->lhsTargets.empty()) {
        for (keel::usize i = 0; i < node->lhsTargets.size() && i < results.size(); ++i)
            if (node->lhsTargets[i])
                assignLValue(node->lhsTargets[i].get(), results[i], env);

        // Display only bare-identifier targets (mirrors single-assign,
        // where `s.f = v` / `a(i) = v` do not auto-display in the TW).
        if (!node->suppressOutput && !results.empty())
            for (keel::usize i = 0; i < node->lhsTargets.size() && i < results.size(); ++i) {
                const ASTNode *t = node->lhsTargets[i].get();
                if (t && t->type == NodeType::IDENTIFIER)
                    displayValue(t->strValue, results[i]);
            }
        return results.empty() ? Value() : results[0];
    }

    for (keel::usize i = 0; i < node->returnNames.size() && i < results.size(); ++i)
        if (node->returnNames[i] != "~")
            env->set(node->returnNames[i], results[i]);

    if (!node->suppressOutput && !results.empty())
        for (keel::usize i = 0; i < node->returnNames.size() && i < results.size(); ++i)
            if (node->returnNames[i] != "~")
                displayValue(node->returnNames[i], results[i]);

    return results.empty() ? Value() : results[0];
}

keel::Array<Value> TreeWalker::execCallMulti(const ASTNode *node, Environment *env, keel::usize nout)
{
    // Cell CSL: [a,b] = c{idx} — expand cell elements into separate outputs
    if (node->type == NodeType::CELL_INDEX) {
        const Value *cell = env->get(node->children[0]->strValue);
        if (!cell || !cell->isCell())
            throw numkit::Error("Cell indexing requires a cell array");

        keel::usize nidx = node->children.size() - 1;
        if (nidx == 1) {
            auto indices = resolveIndex(node->children[1].get(), *cell, 0, 1, env);
            keel::Array<Value> out;
            out.reserve(indices.size());
            for (keel::usize idx : indices)
                out.push_back(cell->cellAt(idx));
            return out;
        } else if (nidx == 2) {
            auto rowIdx = resolveIndex(node->children[1].get(), *cell, 0, 2, env);
            auto colIdx = resolveIndex(node->children[2].get(), *cell, 1, 2, env);
            keel::Array<Value> out;
            for (keel::usize c : colIdx)
                for (keel::usize r : rowIdx)
                    out.push_back(cell->cellAt(cell->dims().sub2ind(r, c)));
            return out;
        } else {
            auto msg = keel::format("Cell CSL with {} indices not supported", nidx);
            throw numkit::Error(msg.c_str());
        }
    }

    if (node->type != NodeType::CALL) {
        if (nout <= 1) {
            keel::Array<Value> single;
            single.push_back(execNode(node, env));
            return single;
        }
        throw numkit::Error("Expected function call in multi-assignment");
    }

    // Superclass-qualified multi-output call `[a,b] = method@Base(obj,...)`.
    if (node->children[0]->type == NodeType::SUPERCLASS_REF)
        return execSuperCall(node->children[0].get(), node, env, nout);

    // OBJECT dotted multi-output method: [a,b] = recv.m(args). An identifier
    // receiver is peeked (no eval, lets a handle method mutate in place); an
    // rvalue receiver (`f().m()`, `a.b.m()`) is evaluated exactly once. A
    // class method returning several outputs fills nout result slots.
    auto *headNode = node->children[0].get();
    keel::String funcName;
    if (headNode->type == NodeType::FIELD_ACCESS) {
        const ASTNode *recvNode = headNode->children[0].get();
        keel::String dotted = headNode->strValue.clone();
        const ASTNode *cur = recvNode;
        while (cur && cur->type == NodeType::FIELD_ACCESS) {
            dotted = keel::format("{}.{}", cur->strValue, dotted);
            cur = cur->children.empty() ? nullptr : cur->children[0].get();
        }
        bool isPackageCall = false;
        if (cur && cur->type == NodeType::IDENTIFIER) {
            dotted = keel::format("{}.{}", cur->strValue, dotted);
            if (!env->get(cur->strValue)) {
                isPackageCall = true;
                if (engine_.hasCallableName(cur->strValue)
                    && !engine_.findClass(cur->strValue)
                    && !engine_.classCtor(cur->strValue)
                    && !engine_.hasExternalFunction(dotted)
                    && !engine_.lookupUserFunction(dotted, env)) {
                    isPackageCall = false;
                }
            }
        }
        if (!isPackageCall) {
            Value recvTmp;
            Value *objPtr = nullptr;
            if (recvNode->type == NodeType::IDENTIFIER) {
                objPtr = env->get(recvNode->strValue);
            } else {
                recvTmp = execNode(recvNode, env);
                objPtr = &recvTmp;
            }
            if (objPtr && objPtr->isObject()) {
                const keel::String &mname = headNode->strValue;
                const BuiltinClass *cls = engine_.findClass(objPtr->objectClassNameRef());
                if (const auto *m = cls ? cls->methods.get(mname) : nullptr) {
                    keel::Array<Value> margs;
                    margs.reserve(node->children.size() - 1);
                    for (keel::usize i = 1; i < node->children.size(); ++i) {
                        Value v = execNodeExpand(node->children[i].get(), env);
                        if (v.isCsl())
                            for (keel::usize k = 0; k < v.cslCount(); ++k)
                                margs.push_back(keel::move(v.cslAt(k)));
                        else
                            margs.push_back(keel::move(v));
                    }
                    Value self = *objPtr; // handle: shares state; value: own copy
                    keel::Array<Value> outBuf(nout);
                    CallContext ctx{&engine_, env};
                    (*m)(self, keel::ArrayRef<Value>(margs.data(), margs.size()),
                         nout, keel::MutArrayRef<Value>(outBuf), ctx);
                    return outBuf;
                }
                Value prop = objectPropGet(*objPtr, mname, env);
                if (prop.isFuncHandle()) {
                    keel::Array<Value> pargs;
                    pargs.reserve(node->children.size() - 1);
                    for (keel::usize i = 1; i < node->children.size(); ++i) {
                        Value v = execNodeExpand(node->children[i].get(), env);
                        if (v.isCsl())
                            for (keel::usize k = 0; k < v.cslCount(); ++k)
                                pargs.push_back(keel::move(v.cslAt(k)));
                        else
                            pargs.push_back(keel::move(v));
                    }
                    return callFuncHandleMulti(prop, pargs, env, nout, node);
                }
            } else if (objPtr && objPtr->isStruct() && objPtr->hasField(headNode->strValue)) {
                Value fld = objPtr->field(headNode->strValue);
                if (fld.isFuncHandle()) {
                    keel::Array<Value> fargs;
                    fargs.reserve(node->children.size() - 1);
                    for (keel::usize i = 1; i < node->children.size(); ++i) {
                        Value v = execNodeExpand(node->children[i].get(), env);
                        if (v.isCsl())
                            for (keel::usize k = 0; k < v.cslCount(); ++k)
                                fargs.push_back(keel::move(v.cslAt(k)));
                        else
                            fargs.push_back(keel::move(v));
                    }
                    return callFuncHandleMulti(fld, fargs, env, nout, node);
                }
            }
        }
        funcName = keel::move(dotted);
    } else if (headNode->type != NodeType::IDENTIFIER) {
        Value callee = execNode(headNode, env);
        keel::Array<Value> cargs;
        cargs.reserve(node->children.size() - 1);
        for (keel::usize i = 1; i < node->children.size(); ++i) {
            Value v = execNodeExpand(node->children[i].get(), env);
            if (v.isCsl())
                for (keel::usize k = 0; k < v.cslCount(); ++k)
                    cargs.push_back(keel::move(v.cslAt(k)));
            else
                cargs.push_back(keel::move(v));
        }
        if (callee.isFuncHandle())
            return callFuncHandleMulti(callee, cargs, env, nout, node);
        if (callee.isObject()) {
            const BuiltinClass *cls = engine_.findClass(callee.objectClassNameRef());
            if (cls && cls->subsref) {
                keel::Array<Value> outBuf(nout);
                CallContext ctx{&engine_, env};
                cls->subsref(callee, keel::ArrayRef<Value>(cargs.data(), cargs.size()), nout,
                             keel::MutArrayRef<Value>(outBuf), ctx);
                return outBuf;
            }
        }
        throw numkit::Error("Cannot invoke non-function value with multiple outputs");
    } else {
        funcName = headNode->strValue.clone();
    }

    keel::Array<Value> args;
    args.reserve(node->children.size() - 1);
    for (keel::usize i = 1; i < node->children.size(); ++i) {
        // CSL arg [a,b]=f(.., c{:}, ..): splice a comma-separated list (c{:} / c{idx}
        // / c{r,:}, or any CSL producer) into the argument list; non-CSL is one arg.
        Value v = execNodeExpand(node->children[i].get(), env);
        if (v.isCsl())
            for (keel::usize k = 0; k < v.cslCount(); ++k)
                args.push_back(keel::move(v.cslAt(k)));
        else
            args.push_back(keel::move(v));
    }

    auto *var = env->get(funcName);
    if (var && var->isFuncHandle())
        return callFuncHandleMulti(*var, args, env, nout, node);
    if (var && var->isObject()) {
        const BuiltinClass *cls = engine_.findClass(var->objectClassNameRef());
        if (cls && cls->subsref) {
            Value self = *var;
            keel::Array<Value> outBuf(nout);
            CallContext ctx{&engine_, env};
            cls->subsref(self, keel::ArrayRef<Value>(args.data(), args.size()), nout,
                         keel::MutArrayRef<Value>(outBuf), ctx);
            return outBuf;
        }
    }

    // OBJECT function-form multi-output: [a,b] = m(obj, ...). A class
    // method on the dominant (first) object argument beats a path function.
    if (!args.empty() && args[0].isObject()) {
        const BuiltinClass *cls = engine_.findClass(args[0].objectClassNameRef());
        if (const auto *m = cls ? cls->methods.get(funcName) : nullptr) {
            Value self = args[0];
            keel::ArrayRef<Value> rest(args.data() + 1, args.size() - 1);
            keel::Array<Value> outBuf(nout);
            CallContext ctx{&engine_, env};
            (*m)(self, rest, nout, keel::MutArrayRef<Value>(outBuf), ctx);
            return outBuf;
        }
    }

    // Lexical nested functions first, then user-defined functions over builtins.
    if (env) {
        if (const UserFunction *nf = env->findNestedFunc(funcName)) {
            return callUserFunctionMulti(*nf, args, env, nout, node);
        }
    }
    if (auto *_uf = engine_.lookupUserFunction(funcName, env)) {
        return callUserFunctionMulti(*_uf, args, env, nout, node);
    }
    if (const ExternalFunc *fn = engine_.findExternal(funcName, env)) {
        keel::Array<Value> outBuf(nout);
        CallContext ctx{&engine_, env};
        (*fn)(args, nout, keel::MutArrayRef<Value>(outBuf), ctx);
        return outBuf;
    }

    auto msg = engine_.formatUndefinedFunctionError(
        funcName, keel::format("Undefined function: {}", funcName));
    throw numkit::Error(msg.c_str());
}

// ============================================================
Value TreeWalker::execDeleteAssign(const ASTNode *node, Environment *env)
{
    auto *lhs = node->children[0].get();
    if (lhs->type != NodeType::CALL || lhs->children.empty())
        throw numkit::Error("Invalid delete assignment syntax");

    auto *target = lhs->children[0].get();
    if (target->type != NodeType::IDENTIFIER)
        throw numkit::Error("Invalid delete target");

    auto *var = env->get(target->strValue);
    if (var && engine_.constantsEnv_
        && var == engine_.constantsEnv_->getLocal(target->strValue)) {
        var = nullptr;
    }
    if (!var) {
        auto msg = keel::format("Undefined variable: {}", target->strValue);
        throw numkit::Error(msg.c_str());
    }

    keel::usize nargs = lhs->children.size() - 1;

    if (nargs == 1) {
        auto indices = resolveIndex(lhs->children[1].get(), *var, 0, 1, env);
        var->indexDelete(indices.data(), indices.size(), engine_.mr_);
    } else if (nargs == 2) {
        auto rowIdx = resolveIndex(lhs->children[1].get(), *var, 0, 2, env);
        auto colIdx = resolveIndex(lhs->children[2].get(), *var, 1, 2, env);
        var->indexDelete2D(rowIdx.data(), rowIdx.size(),
                           colIdx.data(), colIdx.size(),
                           engine_.mr_);
    } else if (nargs == 3) {
        auto rowIdx = resolveIndex(lhs->children[1].get(), *var, 0, 3, env);
        auto colIdx = resolveIndex(lhs->children[2].get(), *var, 1, 3, env);
        auto pageIdx = resolveIndex(lhs->children[3].get(), *var, 2, 3, env);
        var->indexDelete3D(rowIdx.data(), rowIdx.size(),
                           colIdx.data(), colIdx.size(),
                           pageIdx.data(), pageIdx.size(),
                           engine_.mr_);
    } else {
        // ND delete: A(i_1, ..., i_n) = []. Resolve every dim's index
        // vector, then dispatch to the generic ND deleter (which checks
        // exactly-one-partial-axis itself).
        keel::Array<keel::Array<keel::usize>> perDim(nargs);
        keel::Array<const keel::usize *> perDimPtrs(nargs);
        keel::Array<keel::usize> perDimCount(nargs);
        for (keel::usize i = 0; i < nargs; ++i) {
            perDim[i] = resolveIndex(lhs->children[i + 1].get(), *var,
                                     keel::as<keel::i32>(i), keel::as<keel::i32>(nargs), env);
            perDimPtrs[i]  = perDim[i].data();
            perDimCount[i] = perDim[i].size();
        }
        var->indexDeleteND(perDimPtrs.data(), perDimCount.data(),
                           keel::as<keel::i32>(nargs), engine_.mr_);
    }
    return Value();
}

} // namespace numkit
