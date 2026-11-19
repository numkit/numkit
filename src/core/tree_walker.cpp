// src/tree_walker.cpp
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_set.h>
#include <keel/optional.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/tree_walker.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/value/index.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace numkit {

TreeWalker::TreeWalker(Engine &engine)
    : engine_(engine)
{}

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
                                       int dim,
                                       int ndims,
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
            } else if (dim == ndims - 1 && array.dims().ndim() > static_cast<int>(ndims)) {
                sz = 1;
                for (int d = dim; d < array.dims().ndim(); ++d)
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
    const ASTNode *indexExpr, const Value &array, int dim, int ndims, Environment *env)
{
    IndexContextGuard guard(indexContextStack_, {&array, dim, ndims});

    Value val = execNode(indexExpr, env);

    keel::Array<keel::usize> indices;

    if (val.isChar() && val.toString().as_ref() == ":") {
        keel::usize sz;
        if (ndims == 1) {
            sz = array.numel();
        } else if (dim == ndims - 1 && array.dims().ndim() > ndims) {
            sz = 1;
            for (int d = dim; d < array.dims().ndim(); ++d)
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
    throw std::runtime_error(msg.c_str());
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
        throw std::runtime_error(msg.c_str());
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
            throw std::runtime_error("Dot indexing requires a struct");
        if (obj.isStructArray() || obj.numel() == 0) {
            keel::usize n = obj.numel();
            if (n == 0 && !obj.hasField(fname)) {
                auto msg = keel::format("Reference to non-existent field '{}'", fname);
                throw std::runtime_error(msg.c_str());
            }
            Value csl = Value::csl(n, engine_.mr_);
            for (keel::usize i = 0; i < n; ++i) {
                const auto &m = obj.structArrayElem(i);
                const Value *vp = m.get(fname);
                if (!vp) {
                    auto msg = keel::format("Reference to non-existent field '{}'", fname);
                    throw std::runtime_error(msg.c_str());
                }
                csl.cslAt(i) = *vp;
            }
            return csl;
        }
        if (!obj.hasField(fname)) {
            auto msg = keel::format("Reference to non-existent field '{}'", fname);
            throw std::runtime_error(msg.c_str());
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
                const BuiltinClass *cls = engine_.findClass(ctx.array->objectClassName());
                if (const auto *m = cls ? cls->methods.get("__end") : nullptr) {
                    Value self = *ctx.array;
                    Value eargs[2] = {
                        Value::scalar(static_cast<double>(ctx.dimension), engine_.mr_),
                        Value::scalar(static_cast<double>(ctx.ndims), engine_.mr_)};
                    Value out[1];
                    CallContext cctx{&engine_, env};
                    (*m)(self, keel::ArrayRef<Value>(eargs, 2), 1,
                         keel::MutArrayRef<Value>(out, 1), cctx);
                    return out[0];
                }
            }
            keel::usize sz = (ctx.ndims == 1) ? ctx.array->numel()
                                         : ctx.array->dims().dimSize(ctx.dimension);
            return Value::scalar(static_cast<double>(sz), engine_.mr_);
        }
        throw std::runtime_error("'end' used outside of indexing context");
    }
    default:
        throw std::runtime_error("Unknown AST node type");
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

    try {
        return execNodeInner(node, env);
    } catch (Error &e) {
        // Enrich with this node's source location if the inner throw
        // didn't already attach one (e.g. from a public C++ library API).
        if (node->line > 0)
            e.attachIfMissing(node->line, node->col, "", describeNode(node));
        throw;
    } catch (const DebugStopException &) {
        throw;
    } catch (const std::runtime_error &e) {
        if (node->line > 0)
            throw Error(e.what(), node->line, node->col, "", describeNode(node));
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
                if (!ctl->checkLine(static_cast<keel::u16>(child->line),
                                    static_cast<keel::u16>(child->col),
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
    if (auto *_uf = engine_.lookupUserFunction(name, env))
        return callUserFunction(*_uf, {}, env);
    if (const ExternalFunc *fn = engine_.findExternal(name, env)) {
        Value outBuf[1];
        CallContext ctx{&engine_, env};
        (*fn)({}, nargout, keel::MutArrayRef<Value>(outBuf, 1), ctx);
        return outBuf[0].isEmpty() ? Value() : outBuf[0];
    }
    // Bare class name constructs a default instance (`x = MyClass`), like
    // MATLAB — when the name is a registered class and not shadowed above.
    if (const BuiltinClass *cls = engine_.findClass(name); cls && cls->construct) {
        CallContext ctx{&engine_, env};
        return engine_.constructChecked(cls, keel::ArrayRef<Value>(nullptr, 0), ctx);
    }

    // MATLAB-exact error for the nargin/nargout pseudo-vars when they're
    // referenced outside of any function scope. Inside a function they are
    // setLocal'd on entry so env->get() above finds them.
    if (name == "nargin" || name == "nargout")
        throw std::runtime_error(
            "You can only call nargin/nargout from within a function.");

    auto msg = keel::format("Undefined variable or function: {}", name);
    throw std::runtime_error(msg.c_str());
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
            throw std::runtime_error("Dynamic field assign: unsupported target");
        }
    } else if (lhs->type == NodeType::CELL_INDEX) {
        execCellAssign(lhs, rhs, env);
    } else {
        throw std::runtime_error("Invalid assignment target");
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
                const BuiltinClass *cls = engine_.findClass(baseObj.objectClassName());
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

    // OBJECT: obj(i…) = v dispatches to the class subsasgn overload,
    // which mutates `var` in place (value/handle rule via objectStateMut).
    // args = [subscripts…, value].
    if (var->isObject()) {
        const BuiltinClass *cls = engine_.findClass(var->objectClassName());
        if (cls && cls->subsasgn) {
            keel::Array<Value> args;
            args.reserve(nargs + 1);
            for (keel::usize i = 0; i < nargs; ++i) {
                IndexContextGuard guard(indexContextStack_,
                                        {var, static_cast<int>(i), static_cast<int>(nargs)});
                args.push_back(execNode(lhs->children[i + 1].get(), env));
            }
            args.push_back(rhs);
            Value outBuf[1];
            CallContext ctx{&engine_, env};
            cls->subsasgn(*var, keel::ArrayRef<Value>(args.data(), args.size()), 0,
                          keel::MutArrayRef<Value>(outBuf, 1), ctx);
            return;
        }
        // No custom subsasgn → fall through to the builtin object-array
        // element store below (arr(i) = obj).
    }

    // Builtin object-array indexed assignment: arr(i)=obj / arr(i,j)=obj.
    // Fires when the RHS is an object and the target is empty/unset (→ a
    // fresh object array) or an existing object array of the same class.
    // Grows (any rank) with default-constructed gap fill. v1: each
    // subscript selects a single element.
    const bool canPromoteRhs = var->isObject() && !rhs.isObject()
                               && (rhs.isNumeric() || rhs.isLogical())
                               && var->objectStateConst() && var->objectStateConst()->native
                               && var->objectStateConst()->native->fromScalarDouble(0.0) != nullptr;
    if (rhs.isObject() || canPromoteRhs) {
        auto isGfxCls = [](keel::StringRef c) {
            return c.starts_with("matlab.graphics.") || c.starts_with("matlab.ui.");
        };
        if (!var->isObject() && var->isNumeric() && isGfxCls(rhs.objectClassName())) {
            *var = Value();
        }
        const bool newable = !var->isObject() && (var->isUnset() || var->isEmpty());
        const bool sameArr = canPromoteRhs ||
            (var->isObject() && (var->objectClassName() == rhs.objectClassName()
                                 || (isGfxCls(var->objectClassName()) && isGfxCls(rhs.objectClassName()))));
        if (newable || sameArr) {
            keel::Array<keel::Array<keel::usize>> perDim;
            perDim.resize(nargs);
            for (keel::usize d = 0; d < nargs; ++d) {
                const ASTNode *sub = lhs->children[d + 1].get();
                if ((var->isUnset() || var->isEmpty())
                    && sub->type == NodeType::COLON_EXPR && sub->children.empty()) {
                    const keel::usize n = (nargs == 2 && d == 1) ? rhs.numel() : rhs.dims().dim(static_cast<int>(d));
                    perDim[d].resize(n);
                    for (keel::usize k = 0; k < n; ++k) perDim[d][k] = k;
                } else {
                    perDim[d] = resolveIndex(sub, *var,
                                             static_cast<int>(d), static_cast<int>(nargs), env);
                }
            }
            engine_.objectStoreSlice(*var, perDim, rhs, env);
            return;
        }
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
            const keel::String rs = rhs.toString();
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
            throw std::runtime_error("Subscripted assignment dimension mismatch");
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
                    var->resize(std::max(var->dims().rows(), needR),
                                std::max(var->dims().cols(), needC),
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
            maxR = std::max(maxR, r + 1);
        for (auto c : colIdx)
            maxC = std::max(maxC, c + 1);
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
            throw std::runtime_error("Subscripted assignment dimension mismatch");
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
                    var->resize3d(std::max(var->dims().rows(), needR),
                                  std::max(var->dims().cols(), needC),
                                  std::max(var->dims().pages(), needP),
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
            maxR = std::max(maxR, r + 1);
        for (auto c : colIdx)
            maxC = std::max(maxC, c + 1);
        for (auto p : pageIdx)
            maxP = std::max(maxP, p + 1);
        if (var->isEmpty() && (!rhs.isScalar() || (var->dims().rows() == 0 && var->dims().cols() == 0 && !rhs.isEmpty()))) {
            const int nColons = (riColon ? 1 : 0) + (ciColon ? 1 : 0) + (piColon ? 1 : 0);
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
                int colonsLeft = nColons;
                auto nextRhsDim = [&]() -> keel::usize {
                    while (rCursor < static_cast<keel::usize>(rhs.dims().ndim())
                           && rhs.dims().dim(static_cast<int>(rCursor)) == 1
                           && (static_cast<keel::usize>(rhs.dims().ndim()) - rCursor) > static_cast<keel::usize>(colonsLeft)) {
                        ++rCursor;
                    }
                    --colonsLeft;
                    return (rCursor < static_cast<keel::usize>(rhs.dims().ndim()))
                               ? rhs.dims().dim(static_cast<int>(rCursor++))
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
            throw std::runtime_error("Subscripted assignment dimension mismatch");
        if (maxR > var->dims().rows() || maxC > var->dims().cols() || maxP > var->dims().pages())
            var->resize3d(std::max(var->dims().rows(), maxR),
                          std::max(var->dims().cols(), maxC),
                          std::max(var->dims().pages(), maxP),
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
        const int nd = static_cast<int>(nargs);
        keel::Array<keel::Array<keel::usize>> idxLists(static_cast<keel::usize>(nd));
        keel::Array<const keel::usize *> idxPtrs(static_cast<keel::usize>(nd));
        keel::Array<keel::usize> idxCounts(static_cast<keel::usize>(nd));
        for (int i = 0; i < nd; ++i) {
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
        throw std::runtime_error("Invalid assignment target");
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
        throw std::runtime_error("Cell contents indexing on a non-cell value");

    Value *var = &cellVar;
    const keel::usize nidx = node->children.size() - 1;
    keel::Array<keel::usize> coords(nidx);
    for (keel::usize i = 0; i < nidx; ++i) {
        IndexContextGuard guard(indexContextStack_,
                                {var, static_cast<int>(i), static_cast<int>(nidx)});
        Value v = execNode(node->children[i + 1].get(), env);
        coords[i] = Value::checkedScalarIndex(v.toScalar());
    }
    keel::usize linear = var->growCellTo(coords.data(), static_cast<int>(nidx), engine_.mr_);
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
            throw std::runtime_error("Indexed field assignment on a non-struct value");

        const keel::usize nargs = objNode->children.size() - 1;
        if (nargs > static_cast<keel::usize>(Dims::kMaxRank))
            throw std::runtime_error("Struct array indexing rank exceeds maximum supported rank");
        keel::Array<keel::usize> coords(nargs);
        for (keel::usize a = 0; a < nargs; ++a) {
            IndexContextGuard guard(indexContextStack_,
                                    {var, static_cast<int>(a), static_cast<int>(nargs)});
            Value v = execNode(objNode->children[a + 1].get(), env);
            coords[a] = Value::checkedScalarIndex(v.toScalar());
        }
        keel::usize linear;
        if (nargs == 1) {
            linear = coords[0];
            var->growStructArrayTo(linear, engine_.mr_); // preserves row/col vector shape
        } else {
            linear = var->growStructArrayND(coords.data(), static_cast<int>(nargs),
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
        const BuiltinClass *cls = engine_.findClass(parent.objectClassName());
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
            const BuiltinClass *cls = engine_.findClass(baseObj.objectClassName());
            if (const auto *m = cls ? cls->methods.get("__brace_set") : nullptr) {
                const keel::usize nidx = lhs->children.size() - 1;
                keel::Array<Value> args;
                args.reserve(nidx + 1);
                for (keel::usize i = 0; i < nidx; ++i) {
                    IndexContextGuard guard(indexContextStack_,
                                            {&baseObj, static_cast<int>(i), static_cast<int>(nidx)});
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
            const bool isColonSub = sub.isChar() && sub.toString().as_ref() == ":";
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
            throw std::runtime_error("Too many output arguments.");

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
            throw std::runtime_error("Cell indexing requires a cell array");

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
            throw std::runtime_error(msg.c_str());
        }
    }

    if (node->type != NodeType::CALL) {
        if (nout <= 1) {
            keel::Array<Value> single;
            single.push_back(execNode(node, env));
            return single;
        }
        throw std::runtime_error("Expected function call in multi-assignment");
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
                const BuiltinClass *cls = engine_.findClass(objPtr->objectClassName());
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
            const BuiltinClass *cls = engine_.findClass(callee.objectClassName());
            if (cls && cls->subsref) {
                keel::Array<Value> outBuf(nout);
                CallContext ctx{&engine_, env};
                cls->subsref(callee, keel::ArrayRef<Value>(cargs.data(), cargs.size()), nout,
                             keel::MutArrayRef<Value>(outBuf), ctx);
                return outBuf;
            }
        }
        throw std::runtime_error("Cannot invoke non-function value with multiple outputs");
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
        const BuiltinClass *cls = engine_.findClass(var->objectClassName());
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
        const BuiltinClass *cls = engine_.findClass(args[0].objectClassName());
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
    throw std::runtime_error(msg.c_str());
}

// ============================================================
Value TreeWalker::execDeleteAssign(const ASTNode *node, Environment *env)
{
    auto *lhs = node->children[0].get();
    if (lhs->type != NodeType::CALL || lhs->children.empty())
        throw std::runtime_error("Invalid delete assignment syntax");

    auto *target = lhs->children[0].get();
    if (target->type != NodeType::IDENTIFIER)
        throw std::runtime_error("Invalid delete target");

    auto *var = env->get(target->strValue);
    if (var && engine_.constantsEnv_
        && var == engine_.constantsEnv_->getLocal(target->strValue)) {
        var = nullptr;
    }
    if (!var) {
        auto msg = keel::format("Undefined variable: {}", target->strValue);
        throw std::runtime_error(msg.c_str());
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
                                     static_cast<int>(i), static_cast<int>(nargs), env);
            perDimPtrs[i]  = perDim[i].data();
            perDimCount[i] = perDim[i].size();
        }
        var->indexDeleteND(perDimPtrs.data(), perDimCount.data(),
                           static_cast<int>(nargs), engine_.mr_);
    }
    return Value();
}

namespace {

bool toLogicalScalarBool(const Value &v)
{
    if (!v.isScalar() || v.isEmpty() || v.isUnset()) {
        throw std::runtime_error(
            "Operands to the logical AND (&&) and OR (||) operators must be convertible to logical scalar values.");
    }
    return v.toBool();
}

bool condTruth(const Value &v)
{
    if (v.isEmpty() && !v.isDeleted()
        && (v.isUnset() || v.isNumeric() || v.isLogical() || v.isChar())) {
        return false;
    }
    return v.toBool();
}

} // namespace

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
        int spineDepth = 0;
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
    keel::String name = handle.funcHandleName();
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
                const BuiltinClass *cls = engine_.findClass(objPtr->objectClassName());
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
                const BuiltinClass *cls = engine_.findClass(recv.objectClassName());
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
            const BuiltinClass *cls = engine_.findClass(var->objectClassName());
            if (cls && cls->subsref) {
                const keel::usize nsub = node->children.size() - 1;
                keel::Array<Value> args;
                args.reserve(nsub);
                for (keel::usize i = 0; i < nsub; ++i) {
                    IndexContextGuard guard(indexContextStack_,
                                            {var, static_cast<int>(i), static_cast<int>(nsub)});
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
            const BuiltinClass *cls = engine_.findClass(args[0].objectClassName());
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
        const int nd = static_cast<int>(nargs);
        keel::Array<keel::Array<keel::usize>> idxLists(static_cast<keel::usize>(nd));
        keel::Array<const keel::usize *> idxPtrs(static_cast<keel::usize>(nd));
        keel::Array<keel::usize> idxCounts(static_cast<keel::usize>(nd));
        for (int i = 0; i < nd; ++i) {
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
        IndexContextGuard guard(indexContextStack_, {&obj, static_cast<int>(i),
                                                     static_cast<int>(nidx)});
        Value v = execNode(node->children[i + 1].get(), env);
        coords[i] = Value::checkedScalarIndex(v.toScalar());
    }
    const auto &d = obj.dims();
    keel::usize idx = 0, stride = 1;
    for (keel::usize i = 0; i < nidx; ++i) {
        const keel::usize lim = (static_cast<int>(i) < d.ndim()) ? d.dim(static_cast<int>(i)) : 1;
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
        const BuiltinClass *cls = engine_.findClass(obj.objectClassName());
        if (const auto *m = cls ? cls->methods.get("__brace_get") : nullptr) {
            keel::usize nidx = node->children.size() - 1;
            keel::Array<Value> args;
            args.reserve(nidx);
            for (keel::usize i = 0; i < nidx; ++i) {
                IndexContextGuard guard(indexContextStack_,
                                        {&obj, static_cast<int>(i), static_cast<int>(nidx)});
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
        if (node->strValue.size() == 1 && obj.hasField("kind")) {
            char up = static_cast<char>(std::toupper(static_cast<unsigned char>(node->strValue[0])));
            char upBuf[2] = {up, '\0'};
            keel::StringRef upField(upBuf, 1);
            if ((up == 'A' || up == 'B' || up == 'C' || up == 'D') && obj.hasField(upField))
                return obj.field(upField);
        }
        auto msg = keel::format("Reference to non-existent field '{}'", node->strValue);
        throw std::runtime_error(msg.c_str());
    }
    return obj.field(node->strValue);
}

Value TreeWalker::objectPropGet(const Value &obj, keel::StringRef name, Environment *env)
{
    keel::String clsName = obj.objectClassName();
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
    keel::String clsName = objSlot.objectClassName();
    const BuiltinClass *cls = engine_.findClass(clsName);
    if (cls && cls->propSet) {
        CallContext ctx{&engine_, env};
        if (cls->propSet(objSlot, name, rhs, ctx))
            return;
    }
    auto msg = keel::format("Cannot set property '{}' on class '{}'", name, clsName);
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
                    keel::String clsName = base.objectClassName();
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
                keel::String vs = v.toString();
                s.append(vs);
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
                const BuiltinClass *cls = engine_.findClass(elems[i].objectClassName());
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

    auto colonScalar = [](const Value &v) -> double {
        return (v.numel() > 1 && v.isNumeric()) ? v.elemAsDouble(0) : v.toScalar();
    };

    if (node->children.size() == 2) {
        Value a = execNode(node->children[0].get(), env);
        Value b = execNode(node->children[1].get(), env);
        Value ops[2] = {a, b};
        if (a.isObject() || b.isObject()) {
            const Value &dom = a.isObject() ? a : b;
            const BuiltinClass *cls = engine_.findClass(dom.objectClassName());
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
            const BuiltinClass *cls = engine_.findClass(dom.objectClassName());
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

// ============================================================
Value TreeWalker::execIf(const ASTNode *node, Environment *env)
{
    for (const auto &b : node->branches) {
        if (condTruth(execNode(b.cond.get(), env)))
            return execNode(b.body.get(), env);
    }
    if (node->elseBranch)
        return execNode(node->elseBranch.get(), env);
    return Value();
}

Value TreeWalker::execFor(const ASTNode *node, Environment *env)
{
    const keel::String &varName = node->strValue;
    auto rangeVal = execNode(node->children[0].get(), env);

    // Empty range — body never executes
    if (rangeVal.isEmpty()) {
        env->set(varName, Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_));
        return Value();
    }

    const auto &dims = rangeVal.dims();
    const keel::usize rows = dims.rows();
    const keel::usize count = (rows > 0) ? (dims.numel() / rows) : 0;
    if (count == 0) {
        env->set(varName, Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_));
        return Value();
    }

    if (rangeVal.type() == ValueType::DOUBLE && rows == 1) {
        // Fast path: scalar iteration over row vector
        const double *src = rangeVal.doubleData();
        env->set(varName, Value::scalar(0.0, engine_.mr_));
        Value *varPtr = env->get(varName);
        double *slot = varPtr->doubleDataMut();
        for (keel::usize c = 0; c < count; ++c) {
            *slot = src[c];
            execNode(node->children[1].get(), env);
            if (flowSignal_ == FlowSignal::BREAK) {
                flowSignal_ = FlowSignal::NONE;
                break;
            }
            if (flowSignal_ == FlowSignal::CONTINUE) {
                flowSignal_ = FlowSignal::NONE;
                continue;
            }
            if (flowSignal_ == FlowSignal::RETURN)
                return Value();
            varPtr = env->get(varName);
            if (!varPtr || !varPtr->isScalar() || varPtr->type() != ValueType::DOUBLE) {
                for (keel::usize c2 = c + 1; c2 < count; ++c2) {
                    env->set(varName, Value::scalar(src[c2], engine_.mr_));
                    execNode(node->children[1].get(), env);
                    if (flowSignal_ == FlowSignal::BREAK) {
                        flowSignal_ = FlowSignal::NONE;
                        return Value();
                    }
                    if (flowSignal_ == FlowSignal::CONTINUE) {
                        flowSignal_ = FlowSignal::NONE;
                        continue;
                    }
                    if (flowSignal_ == FlowSignal::RETURN)
                        return Value();
                }
                return Value();
            }
            slot = varPtr->doubleDataMut();
        }
        return Value();
    }

    // Unified path for all ValueTypes (DOUBLE matrix, SINGLE, INT8..UINT64,
    // COMPLEX, LOGICAL, CHAR, CELL, STRING, STRUCT, OBJECT) and N-D arrays.
    for (keel::usize c = 0; c < count; ++c) {
        if (rows == 1) {
            env->set(varName, rangeVal.elemAt(c, engine_.mr_));
        } else {
            env->set(varName, rangeVal.indexGet2D(nullptr, rows, &c, 1, engine_.mr_));
        }
        execNode(node->children[1].get(), env);
        if (flowSignal_ == FlowSignal::BREAK) {
            flowSignal_ = FlowSignal::NONE;
            break;
        }
        if (flowSignal_ == FlowSignal::CONTINUE) {
            flowSignal_ = FlowSignal::NONE;
            continue;
        }
        if (flowSignal_ == FlowSignal::RETURN)
            return Value();
    }
    return Value();
}

Value TreeWalker::execWhile(const ASTNode *node, Environment *env)
{
    auto *condNode = node->children[0].get();
    for (;;) {
        if (!condTruth(execNode(condNode, env)))
            break;
        execNode(node->children[1].get(), env);
        if (flowSignal_ == FlowSignal::BREAK) {
            flowSignal_ = FlowSignal::NONE;
            break;
        }
        if (flowSignal_ == FlowSignal::CONTINUE) {
            flowSignal_ = FlowSignal::NONE;
            continue;
        }
        if (flowSignal_ == FlowSignal::RETURN)
            return Value();
    }
    return Value();
}

Value TreeWalker::execSwitch(const ASTNode *node, Environment *env)
{
    auto sv = execNode(node->children[0].get(), env);

    for (const auto &b : node->branches) {
        auto cv = execNode(b.cond.get(), env);
        bool matched = false;

        // MATLAB `switch` matches a case by VALUE (==), cross-type — verified
        // against R2025b: `switch int8(1); case 1` and `switch uint8(65);
        // case 'A'` both match (65 == 65). char-vs-char compares as strings;
        // anything else numeric/char/logical compares element-wise on the
        // numeric/code-point value (this is what the VM's EQ-based switch does,
        // so both backends agree). The old type-strict gate diverged from both.
        auto valuesEqual = [](const Value &a, const Value &b) -> bool {
            if (a.isChar() && b.isChar()) return a.toString() == b.toString();
            const bool an = a.isNumeric() || a.isLogical() || a.isChar();
            const bool bn = b.isNumeric() || b.isLogical() || b.isChar();
            if (!an || !bn) return false; // non-numeric (cell/struct/object) → no match
            if (a.numel() != b.numel()) return false;
            if (a.isScalar() && b.isScalar()) return a.elemAsDouble(0) == b.elemAsDouble(0);
            if (a.dims() != b.dims()) return false;
            for (keel::usize i = 0; i < a.numel(); ++i)
                if (a.elemAsDouble(i) != b.elemAsDouble(i)) return false;
            return true;
        };

        if (cv.isCell()) {
            for (keel::usize i = 0; i < cv.numel() && !matched; ++i)
                matched = valuesEqual(sv, cv.cellAt(i));
        } else {
            matched = valuesEqual(sv, cv);
        }

        if (matched)
            return execNode(b.body.get(), env);
    }

    if (node->elseBranch)
        return execNode(node->elseBranch.get(), env);
    return Value();
}

// ============================================================
Value TreeWalker::execFunctionDef(const ASTNode *node, Environment *env)
{
    (void) env;
    // Nested FUNCTION_DEFs inside a user function are registered lexically
    // on function entry — never pollute global/script function tables.
    if (!activeFrames_.empty())
        return Value();

    UserFunction func;
    func.name = node->strValue.clone();
    func.params = node->paramNames.clone();
    func.returns = node->returnNames.clone();
    func.body = keel::Arc<const ASTNode>(cloneNode(node->children[0].get()));
    func.closureEnv = nullptr;
    // Route to the script-lexical bucket while inside a script
    // scope — matches the compiler's rule so `clear all` in TW
    // mode behaves the same as in VM mode (local functions survive).
    if (engine_.inScriptScope()) {
        keel::String key = func.name.clone();
        engine_.scriptLocalUserFuncs_.put(keel::move(key), keel::move(func));
    } else {
        if (auto *cp = engine_.compilerPtr())
            cp->eraseCompiledFunc(func.name);
        keel::String key = func.name.clone();
        engine_.userFuncs_.put(keel::move(key), keel::move(func));
    }
    return Value();
}

Value TreeWalker::execExprStmt(const ASTNode *node, Environment *env)
{
    auto *child = node->children[0].get();

    // MATLAB display / ans rule (see Compiler::compileExprStmt for the
    // VM mirror). Bare read of a user variable → display by its own name,
    // no ans. Everything else → ans store regardless of semicolon, "ans"
    // label for display.
    bool bareUserVar = false;
    keel::StringRef displayName;
    if (child->type == NodeType::IDENTIFIER) {
        const keel::String &name = child->strValue;
        const Value *found = env->get(name);
        const bool isUnshadowedConst =
            found && engine_.constantsEnv_ && found == engine_.constantsEnv_->getLocal(name);
        if (found && !isUnshadowedConst) {
            bareUserVar = true;
            displayName = name.as_ref();
        }
    }

    // Statement context: dispatch CALL and IDENTIFIER with nargout=0
    Value val;
    if (child->type == NodeType::CALL)
        val = execCall(child, env, 0);
    else if (child->type == NodeType::IDENTIFIER)
        val = execIdentifier(child, env, 0);
    else
        val = execNodeExpand(child, env);

    if (bareUserVar) {
        if (!node->suppressOutput)
            displayValue(displayName, val);
        return val;
    }

    if (val.isCsl()) {
        Value last;
        for (keel::usize k = 0; k < val.cslCount(); ++k) {
            last = val.cslAt(k);
            env->set("ans", last);
            if (!node->suppressOutput)
                displayValue("ans", last);
        }
        return last;
    }

    // Anonymous value — bind to `ans` regardless of semicolon (matches
    // MATLAB; `ans` persists even when display is suppressed).
    if (!val.isEmpty()) {
        env->set("ans", val);
        if (!node->suppressOutput)
            displayValue("ans", val);
    }
    return val;
}

// ============================================================
Value TreeWalker::execCommandCall(const ASTNode *node, Environment *env)
{
    const keel::String &name = node->strValue;

    // MATLAB (probed R2025b): a variable-headed glued -/ form (`N-1`,
    // `x -1`, `a/2` — the `which -all` idiom shape) is the binary
    // expression when the head is a VARIABLE.
    if (node->children.size() == 1
        && node->children[0]->type == NodeType::STRING_LITERAL) {
        const keel::String &arg = node->children[0]->strValue;
        const Value *cand = env->get(name);
        const bool isUnshadowedConst =
            cand && engine_.constantsEnv_ && cand == engine_.constantsEnv_->getLocal(name);
        const Value *lhs = (arg.size() >= 2 && (arg[0] == '-' || arg[0] == '/')
                            && (std::isalnum(static_cast<unsigned char>(arg[1]))
                                || arg[1] == '.')
                            && !isUnshadowedConst)
                               ? cand : nullptr;
        if (lhs) {
            const keel::String tail = keel::String::from(arg.substr(1));
            char *endp = nullptr;
            const double parsedNum = std::strtod(tail.c_str(), &endp);
            const bool isNum = !tail.empty() && endp == tail.c_str() + tail.size();
            bool isIdent = !tail.empty()
                           && (std::isalpha(static_cast<unsigned char>(tail[0]))
                               || tail[0] == '_');
            if (isIdent)
                for (char c : tail)
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                        { isIdent = false; break; }
            if (isNum || isIdent) {
                if (!isNum) {
                    const Value *rv = env->get(tail);
                    if (!rv) {
                        auto msg = keel::format("Undefined function or variable '{}'", tail);
                        throw std::runtime_error(msg.c_str());
                    }
                }
                // Reuse the expression evaluator: rebuild a tiny
                // BINARY_OP AST and exec it (same path as parsed code).
                auto bin = keel::make_unique<ASTNode>(NodeType::BINARY_OP);
                bin->strValue.assign((arg[0] == '-') ? "-" : "/");
                auto l = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
                l->strValue = name.clone();
                auto r = keel::make_unique<ASTNode>(
                    isNum ? NodeType::NUMBER_LITERAL : NodeType::IDENTIFIER);
                if (isNum) r->numValue = parsedNum;
                else r->strValue = tail.clone();
                bin->children.push_back(keel::move(l));
                bin->children.push_back(keel::move(r));
                Value result = execBinaryOp(bin.get(), env);
                env->set("ans", result);
                if (!node->suppressOutput) displayValue("ans", result);
                return result;
            }
        }
    }

    keel::Array<Value> args;
    args.reserve(node->children.size());
    for (auto &child : node->children)
        args.push_back(Value::fromString(child->strValue, engine_.mr_));

    // 1. Lexical nested functions, then user functions over external builtins.
    Value result;
    if (env) {
        if (const UserFunction *nf = env->findNestedFunc(name)) {
            result = callUserFunction(*nf, args, env);
            if (!node->suppressOutput && !result.isEmpty()) {
                env->set("ans", result);
                displayValue("ans", result);
            }
            return result;
        }
    }
    if (auto *_uf = engine_.lookupUserFunction(name, env)) {
        result = callUserFunction(*_uf, args, env);
        if (!node->suppressOutput && !result.isEmpty()) {
            env->set("ans", result);
            displayValue("ans", result);
        }
        return result;
    }

    // 2. External registered functions (includes workspace builtins,
    //    import-aware via findExternal).
    if (const ExternalFunc *fn = engine_.findExternal(name, env)) {
        Value outBuf[1];
        CallContext ctx{&engine_, env};
        (*fn)(args, 0, keel::MutArrayRef<Value>(outBuf, 1), ctx);
        result = outBuf[0];
        if (!node->suppressOutput && !result.isEmpty()) {
            env->set("ans", result);
            displayValue("ans", result);
        }
        return result;
    }

    auto msg = engine_.formatUndefinedFunctionError(
        name, keel::format("Undefined function: {}", name));
    throw std::runtime_error(msg.c_str());
}

// ============================================================
Value TreeWalker::execAnonFunc(const ASTNode *node, Environment *env)
{
    if (!node->strValue.empty() && node->children.empty()) {
        if (env) {
            if (const UserFunction *nf = env->findNestedFunc(node->strValue)) {
                int nid = engine_.anonNameCounter_.fetch_add(1, std::memory_order_relaxed);
                keel::String nestedHandleId = keel::format("__nested_fh_{}", nid);
                UserFunction escaped = nf->clone();
                if (!escaped.closureEnv)
                    escaped.closureEnv = resolveClosureEnv(escaped);
                keel::String dispSrc = escaped.name.clone();
                std::replace(dispSrc.begin(), dispSrc.end(), '>', '/');
                engine_.userFuncs_.put(nestedHandleId.clone(), keel::move(escaped));
                Value h = Value::funcHandle(nestedHandleId, engine_.mr_);
                h.setFuncHandleSource(dispSrc);
                return h;
            }
        }
        return Value::funcHandle(node->strValue, engine_.mr_);
    }

    // Engine-wide counter (shared with the VM compiler) — see the comment
    // on Engine::anonNameCounter_: a TreeWalker-executed literal must never
    // mint an id a compiled chunk already owns.
    int id = engine_.anonNameCounter_.fetch_add(1, std::memory_order_relaxed);
    keel::String anonName = keel::format("__anon_{}", id);

    UserFunction uf;
    uf.name = anonName.clone();
    uf.params = node->paramNames.clone();

    const ASTNode *bodyExpr = node->children[0].get();
    bool forwardVarargout =
        bodyExpr->type == NodeType::CALL
        && !bodyExpr->children.empty()
        && bodyExpr->children[0]->type == NodeType::IDENTIFIER
        && bodyExpr->children[0]->strValue != "__numkit_fwd_call__";
    if (forwardVarargout) {
        const keel::String &callee = bodyExpr->children[0]->strValue;
        const Value *cand = env ? env->get(callee) : nullptr;
        const bool isUnshadowedConst =
            cand && engine_.constantsEnv_ && cand == engine_.constantsEnv_->getLocal(callee);
        bool calleeIsVar = env && ((cand != nullptr && !isUnshadowedConst)
                                   || env->isGlobal(callee)
                                   || env->isPersistent(callee)
                                   || env->findNestedFunc(callee) != nullptr);
        for (const auto &p : node->paramNames)
            if (p == callee) { calleeIsVar = true; break; }
        if (calleeIsVar)
            forwardVarargout = false;
    }

    auto bodyBlock = keel::make_arc<ASTNode>(NodeType::BLOCK);
    auto assignNode = keel::make_unique<ASTNode>(NodeType::ASSIGN);
    assignNode->suppressOutput = true;

    if (forwardVarargout) {
        uf.returns.push_back(keel::String::from("varargout"));
        auto vaId = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
        vaId->strValue.assign("varargout");
        assignNode->children.push_back(keel::move(vaId));
        auto fwdCall = keel::make_unique<ASTNode>(NodeType::CALL);
        auto fwdId = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
        fwdId->strValue.assign("__numkit_fwd_call__");
        fwdCall->children.push_back(keel::move(fwdId));
        auto nargoutId = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
        nargoutId->strValue.assign("nargout");
        fwdCall->children.push_back(keel::move(nargoutId));
        auto fnameStr = keel::make_unique<ASTNode>(NodeType::STRING_LITERAL);
        fnameStr->strValue = bodyExpr->children[0]->strValue.clone();
        fwdCall->children.push_back(keel::move(fnameStr));
        for (keel::usize i = 1; i < bodyExpr->children.size(); ++i)
            fwdCall->children.push_back(cloneNode(bodyExpr->children[i].get()));
        assignNode->children.push_back(keel::move(fwdCall));
    } else {
        uf.returns.push_back(keel::String::from("__result__"));
        auto resultId = keel::make_unique<ASTNode>(NodeType::IDENTIFIER);
        resultId->strValue.assign("__result__");
        assignNode->children.push_back(keel::move(resultId));
        assignNode->children.push_back(cloneNode(bodyExpr));
    }

    bodyBlock->children.push_back(keel::move(assignNode));
    uf.body = keel::move(bodyBlock);

    keel::Array<keel::String> capturedNames;
    keel::Array<Value> capturedVals;
    keel::Arc<Environment> anonNestedEnv;
    auto bindNestedRef = [&](keel::StringRef nm) {
        if (!env || nm.empty())
            return;
        if (const UserFunction *nf = env->findNestedFunc(nm)) {
            if (!anonNestedEnv) {
                anonNestedEnv = keel::make_arc<Environment>(
                    &engine_.constantsEnv(), engine_.globalsEnv_.get());
            }
            if (!anonNestedEnv->findNestedFunc(nm)) {
                UserFunction escaped = nf->clone();
                if (!escaped.closureEnv)
                    escaped.closureEnv = resolveClosureEnv(escaped);
                anonNestedEnv->defineNestedFunc(nm, keel::move(escaped));
            }
        }
    };
    auto collectFree = [&](auto &self, const ASTNode *cur,
                           keel::ArrayRef<keel::String> bound) -> void {
        if (!cur)
            return;
        if (cur->type == NodeType::IDENTIFIER) {
            const keel::String &nm = cur->strValue;
            if (nm == "__result__" || nm == "varargout")
                return;
            for (const auto &p : bound)
                if (p == nm)
                    return;
            for (const auto &c : capturedNames)
                if (c == nm)
                    return;
            if (!env)
                return;
            const Value *valPtr = env->get(nm);
            if (!valPtr || valPtr->isUnset() || valPtr->isDeleted()) {
                bindNestedRef(nm);
                return;
            }
            if (engine_.constantsEnv_ && valPtr == engine_.constantsEnv_->getLocal(nm))
                return;
            capturedNames.push_back(nm.clone());
            capturedVals.push_back(*valPtr);
            return;
        }
        if (cur->type == NodeType::ANON_FUNC) {
            if (!cur->strValue.empty() && cur->children.empty()) {
                bindNestedRef(cur->strValue);
                return;
            }
            keel::Array<keel::String> merged;
            merged.reserve(bound.size() + cur->paramNames.size());
            for (const auto &p : bound)
                merged.push_back(p.clone());
            for (const auto &p : cur->paramNames)
                merged.push_back(p.clone());
            for (const auto &ch : cur->children)
                self(self, ch.get(), merged.as_ref());
            return;
        }
        for (const auto &ch : cur->children)
            self(self, ch.get(), bound);
        for (const auto &b : cur->branches) {
            self(self, b.cond.get(), bound);
            self(self, b.body.get(), bound);
        }
        if (cur->elseBranch)
            self(self, cur->elseBranch.get(), bound);
    };
    collectFree(collectFree, bodyExpr, node->paramNames.as_ref());
    for (auto &cn : capturedNames)
        uf.params.push_back(keel::move(cn));
    uf.closureEnv = keel::move(anonNestedEnv);
    // Provenance is fixed at creation (reference rule for anonymous
    // functions): a handle born in a script/file keeps THAT file, one born
    // at the command window keeps '' — masking any caller's file later.
    engine_.captureScriptOrigin(uf);

    engine_.userFuncs_.put(anonName.clone(), keel::move(uf));
    Value h = Value::funcHandle(anonName, engine_.mr_);
    h.setFuncHandleSource(node->strValue);   // reconstructed "@(...)..." for func2str
    if (!capturedVals.empty())
        h.setFuncHandleCaptures(capturedVals.data(), capturedVals.size());
    return h;
}

// ============================================================
Value TreeWalker::execTryCatch(const ASTNode *node, Environment *env)
{
    try {
        auto result = execNode(node->children[0].get(), env);
        // Propagate flow signals — try/catch doesn't intercept break/continue/return
        if (flowSignal_ != FlowSignal::NONE)
            return result;
        return result;
    } catch (const Error &mle) {
        if (node->children.size() > 1) {
            if (!node->strValue.empty()) {
                auto err = Value::structure();
                err.field("message") = Value::fromString(mle.what(), engine_.mr_);
                keel::StringRef id = mle.identifier().empty()
                                         ? keel::StringRef("numkit:error")
                                         : mle.identifier().as_ref();
                err.field("identifier") = Value::fromString(id, engine_.mr_);
                env->set(node->strValue, err);
            }
            return execNode(node->children[1].get(), env);
        }
        return Value();
    } catch (const std::exception &e) {
        if (node->children.size() > 1) {
            if (!node->strValue.empty()) {
                auto err = Value::structure();
                err.field("message") = Value::fromString(e.what(), engine_.mr_);
                err.field("identifier") = Value::fromString("numkit:error", engine_.mr_);
                env->set(node->strValue, err);
            }
            return execNode(node->children[1].get(), env);
        }
        return Value();
    }
}

// ============================================================
Value TreeWalker::execGlobalPersistent(const ASTNode *node, Environment *env)
{
    if (node->type == NodeType::PERSISTENT_STMT) {
        if (activeFrames_.empty())
            throw std::runtime_error("persistent declarations are only allowed inside functions");
        for (auto &name : node->paramNames)
            env->declarePersistent(name);
        return Value();
    }
    for (auto &name : node->paramNames) {
        env->declareGlobal(name);
        // A freshly declared global with no value yet IS [] in MATLAB (visible
        // to who/exist, reads as an empty matrix), not "unset". Seed the empty
        // matrix so both reads and introspection match. The VM mirrors this in
        // Engine::runOneChunk's updateTopLevelGlobals.
        if (!engine_.globalsEnv_->get(name))
            engine_.globalsEnv_->set(name, Value::Empty);
    }
    return Value();
}

namespace {

void twCollectDirectNestedDefs(const ASTNode *node, keel::Array<const ASTNode *> &out)
{
    if (!node)
        return;
    if (node->type == NodeType::FUNCTION_DEF) {
        out.push_back(node);
        return;
    }
    if (node->type == NodeType::CLASSDEF_DEF)
        return;
    for (const auto &ch : node->children)
        twCollectDirectNestedDefs(ch.get(), out);
    for (const auto &b : node->branches) {
        twCollectDirectNestedDefs(b.cond.get(), out);
        twCollectDirectNestedDefs(b.body.get(), out);
    }
    if (node->elseBranch)
        twCollectDirectNestedDefs(node->elseBranch.get(), out);
}

const ASTNode *twRootTargetId(const ASTNode *lhs)
{
    const ASTNode *cur = lhs;
    while (cur && !cur->children.empty()
           && (cur->type == NodeType::CALL || cur->type == NodeType::FIELD_ACCESS
               || cur->type == NodeType::DYNAMIC_FIELD_ACCESS
               || cur->type == NodeType::CELL_INDEX)) {
        cur = cur->children[0].get();
    }
    return (cur && cur->type == NodeType::IDENTIFIER) ? cur : nullptr;
}

void twScanBodyLexical(const ASTNode *node,
                       keel::HashSet<keel::String> &assigned,
                       keel::HashSet<keel::String> &identifiers,
                       keel::HashSet<keel::String> &ownGlobPers)
{
    if (!node || node->type == NodeType::FUNCTION_DEF || node->type == NodeType::CLASSDEF_DEF)
        return;
    switch (node->type) {
    case NodeType::IDENTIFIER:
        if (!node->strValue.empty())
            identifiers.insert(node->strValue.clone());
        break;
    case NodeType::ASSIGN:
    case NodeType::DELETE_ASSIGN:
        if (!node->children.empty()) {
            if (const ASTNode *id = twRootTargetId(node->children[0].get()))
                assigned.insert(id->strValue.clone());
        }
        break;
    case NodeType::MULTI_ASSIGN:
        for (const auto &rn : node->returnNames)
            if (!rn.empty() && rn != "~")
                assigned.insert(rn.clone());
        for (const auto &lt : node->lhsTargets) {
            if (const ASTNode *id = twRootTargetId(lt.get()))
                assigned.insert(id->strValue.clone());
        }
        break;
    case NodeType::FOR_STMT:
        if (!node->strValue.empty())
            assigned.insert(node->strValue.clone());
        break;
    case NodeType::TRY_STMT:
        if (!node->strValue.empty() && node->children.size() > 1)
            assigned.insert(node->strValue.clone());
        break;
    case NodeType::GLOBAL_STMT:
    case NodeType::PERSISTENT_STMT:
        for (const auto &nm : node->paramNames)
            ownGlobPers.insert(nm.clone());
        break;
    default:
        break;
    }
    for (const auto &lt : node->lhsTargets)
        twScanBodyLexical(lt.get(), assigned, identifiers, ownGlobPers);
    for (const auto &ch : node->children)
        twScanBodyLexical(ch.get(), assigned, identifiers, ownGlobPers);
    for (const auto &b : node->branches) {
        twScanBodyLexical(b.cond.get(), assigned, identifiers, ownGlobPers);
        twScanBodyLexical(b.body.get(), assigned, identifiers, ownGlobPers);
    }
    if (node->elseBranch)
        twScanBodyLexical(node->elseBranch.get(), assigned, identifiers, ownGlobPers);
}

void twCollectDescendantAssigned(const ASTNode *nestedFn,
                                 keel::HashSet<keel::String> &descAssigned)
{
    if (!nestedFn || nestedFn->children.empty())
        return;
    const ASTNode *nb = nestedFn->children[0].get();
    keel::HashSet<keel::String> localAssigned;
    keel::HashSet<keel::String> localIds;
    keel::HashSet<keel::String> localGP;
    twScanBodyLexical(nb, localAssigned, localIds, localGP);
    keel::HashSet<keel::String> ownShadow = localGP.clone();
    for (const auto &p : nestedFn->paramNames) ownShadow.insert(p.clone());
    for (const auto &r : nestedFn->returnNames) ownShadow.insert(r.clone());
    for (const auto &a : localAssigned)
        if (!ownShadow.contains(a))
            descAssigned.insert(a.clone());

    keel::Array<const ASTNode *> deeper;
    twCollectDirectNestedDefs(nb, deeper);
    for (const ASTNode *d : deeper) {
        keel::HashSet<keel::String> sub;
        twCollectDescendantAssigned(d, sub);
        for (const auto &a : sub)
            if (!ownShadow.contains(a))
                descAssigned.insert(a.clone());
    }
}

bool twAncestorOwnsVar(const Environment *parentEnv, keel::StringRef nm)
{
    for (const Environment *p = parentEnv; p && p->isLexicalScope(); p = p->parentForImports()) {
        if (p->hasLexicalVar(nm) || p->hasLocalSlot(nm)
            || p->isGlobal(nm) || p->isPersistent(nm)) {
            return true;
        }
    }
    return false;
}

void twPopulateLexicalEnv(const UserFunction &func,
                          const keel::Arc<Environment> &heapEnv,
                          const keel::Array<const ASTNode *> &directNested,
                          const keel::Arc<Environment> &resolvedClosure)
{
    heapEnv->markLexicalScope(true);
    keel::HashSet<keel::String> assigned;
    keel::HashSet<keel::String> identifiers;
    keel::HashSet<keel::String> ownGlobPers;
    twScanBodyLexical(func.body.get(), assigned, identifiers, ownGlobPers);

    for (const auto &p : func.params) heapEnv->declareLexicalVar(p);
    for (const auto &r : func.returns) heapEnv->declareLexicalVar(r);
    heapEnv->declareLexicalVar("nargin");
    heapEnv->declareLexicalVar("nargout");

    keel::HashSet<keel::String> descAssigned;
    for (const ASTNode *nd : directNested)
        twCollectDescendantAssigned(nd, descAssigned);

    auto maybeDeclareOwn = [&](keel::StringRef nm) {
        if (nm.empty() || nm == "~" || ownGlobPers.contains(nm))
            return;
        if (!twAncestorOwnsVar(resolvedClosure.get(), nm))
            heapEnv->declareLexicalVar(nm);
    };
    for (const auto &a : assigned)
        maybeDeclareOwn(a);
    for (const auto &id : identifiers)
        if (descAssigned.contains(id))
            maybeDeclareOwn(id);

    for (const ASTNode *nd : directNested) {
        if (!nd || nd->children.empty())
            continue;
        UserFunction nuf;
        nuf.name = keel::format("{}>{}", func.name, nd->strValue);
        nuf.params = nd->paramNames.clone();
        nuf.returns = nd->returnNames.clone();
        nuf.body = keel::Arc<const ASTNode>(cloneNode(nd->children[0].get()));
        nuf.rawClosureEnv = heapEnv.get();
        nuf.originKnown = func.originKnown;
        nuf.sourceFs = func.sourceFs.clone();
        nuf.sourceDir = func.sourceDir.clone();
        nuf.sourceFile = func.sourceFile.clone();
        heapEnv->defineNestedFunc(nd->strValue, keel::move(nuf));
    }
}

} // namespace

keel::Arc<Environment> TreeWalker::resolveClosureEnv(const UserFunction &uf) const
{
    if (uf.closureEnv)
        return uf.closureEnv;
    if (!uf.rawClosureEnv)
        return {};
    for (keel::usize i = activeLexicalEnvs_.size(); i > 0; --i) {
        for (keel::Arc<Environment> cur = activeLexicalEnvs_[i - 1]; cur;
             cur = cur->owningParentArc()) {
            if (cur.get() == uf.rawClosureEnv)
                return cur;
        }
    }
    return {};
}

// Extract bare-identifier arg names from a CALL AST node for inputname(k).
// Returns empty array if callNode is null. Reserved names (pi, eps, …)
// are treated as non-identifier (returned as empty string).
static keel::Array<keel::String> extractCallerArgNames(const ASTNode *callNode,
                                                       const Engine &engine)
{
    keel::Array<keel::String> names;
    if (!callNode || callNode->children.size() < 2) return names;
    names.reserve(callNode->children.size() - 1);
    bool anyIdent = false;
    for (keel::usize i = 1; i < callNode->children.size(); ++i) {
        const auto &child = callNode->children[i];
        if (child && child->type == NodeType::IDENTIFIER
            && !engine.isReservedName(child->strValue)) {
            names.push_back(child->strValue.clone());
            anyIdent = true;
        } else {
            names.emplace_back();
        }
    }
    if (!anyIdent) names.clear();
    return names;
}

Value TreeWalker::callUserFunction(const UserFunction &func,
                                    keel::ArrayRef<Value> args,
                                    Environment *env,
                                    const ASTNode *callNode)
{
    (void) env;
    RecursionGuard rguard(currentRecursionDepth_, maxRecursionDepth_);

    // varargin: the LAST declared parameter absorbs any extra arguments
    // (packed into a 1xN cell; empty 0x0 cell when there are none).
    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const keel::usize required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size()) {
        auto msg = keel::format("Too many input arguments for function '{}'", func.name);
        throw std::runtime_error(msg.c_str());
    }

    keel::Arc<Environment> resolvedClosure = resolveClosureEnv(func);
    keel::Array<const ASTNode *> directNested;
    twCollectDirectNestedDefs(func.body.get(), directNested);
    const bool useLexicalHeap =
        !directNested.empty() || (resolvedClosure && resolvedClosure->isLexicalScope());

    keel::Optional<Environment> stackEnv;
    keel::Arc<Environment> heapEnv;
    if (useLexicalHeap) {
        heapEnv = resolvedClosure
                      ? keel::make_arc<Environment>(resolvedClosure, engine_.globalsEnv_.get())
                      : keel::make_arc<Environment>(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    } else if (resolvedClosure) {
        stackEnv.emplace(resolvedClosure, engine_.globalsEnv_.get());
    } else {
        stackEnv.emplace(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    }
    LexicalEnvGuard lexGuard(activeLexicalEnvs_, heapEnv);
    Environment &localEnv = useLexicalHeap ? *heapEnv : *stackEnv;
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv,
                          extractCallerArgNames(callNode, engine_));

    for (keel::usize i = 0; i < required && i < args.size(); ++i)
        localEnv.setLocal(func.params[i], args[i]);
    if (va) {
        Value vaCell = args.size() > required
                           ? Value::cell(1, args.size() - required, engine_.mr_)
                           : Value::cell(0, 0, engine_.mr_);
        for (keel::usize i = required; i < args.size(); ++i)
            vaCell.cellAt(i - required) = args[i];
        localEnv.setLocal("varargin", keel::move(vaCell));
    }

    func.usesNarginNargout = 1;
    if (func.usesNarginNargout) {
        keel::usize nout = std::max(func.returns.size(), keel::usize(1));
        localEnv.setLocal("nargin",
                          Value::scalar(static_cast<double>(args.size()), engine_.mr_));
        localEnv.setLocal("nargout", Value::scalar(static_cast<double>(nout), engine_.mr_));
    }

    keel::Optional<DebugController::FrameGuard> dbgFrame;
    if (auto *ctl = debugCtl()) {
        StackFrame frame;
        frame.functionName = func.name.clone();
        frame.env = &localEnv;
        dbgFrame.emplace(*ctl, keel::move(frame));
    }

    keel::Optional<FsContext::OriginFrame> originGuard;
    if (func.originKnown)
        originGuard.emplace(engine_.fsContext(), func.sourceFs,
                            func.sourceDir, func.sourceFile);

    execNode(func.body.get(), &localEnv);

    dbgFrame.reset();

    if (flowSignal_ == FlowSignal::RETURN)
        flowSignal_ = FlowSignal::NONE;

    if (func.returns.empty())
        return Value();

    if (func.returns[0] == "varargout") {
        auto *vaVal = localEnv.getLocal("varargout");
        if (!vaVal)
            vaVal = localEnv.get("varargout");
        if (vaVal && vaVal->isCell() && vaVal->numel() > 0)
            return vaVal->cellAt(0);
        return Value();
    }

    const keel::String &r0 = func.returns[0];
    if (!useLexicalHeap && !localEnv.isGlobal(r0) && !localEnv.isPersistent(r0)) {
        if (auto *localVal = localEnv.getLocal(r0))
            return keel::move(*localVal);
    }
    auto *val = localEnv.get(r0);
    return val ? *val : Value();
}

keel::Array<Value> TreeWalker::callUserFunctionMulti(const UserFunction &func,
                                                      keel::ArrayRef<Value> args,
                                                      Environment *env,
                                                      keel::usize nout,
                                                      const ASTNode *callNode)
{
    (void) env;
    RecursionGuard rguard(currentRecursionDepth_, maxRecursionDepth_);

    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const keel::usize required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size()) {
        auto msg = keel::format("Too many input arguments for function '{}'", func.name);
        throw std::runtime_error(msg.c_str());
    }

    keel::Arc<Environment> resolvedClosure = resolveClosureEnv(func);
    keel::Array<const ASTNode *> directNested;
    twCollectDirectNestedDefs(func.body.get(), directNested);
    const bool useLexicalHeap =
        !directNested.empty() || (resolvedClosure && resolvedClosure->isLexicalScope());

    keel::Optional<Environment> stackEnv;
    keel::Arc<Environment> heapEnv;
    if (useLexicalHeap) {
        heapEnv = resolvedClosure
                      ? keel::make_arc<Environment>(resolvedClosure, engine_.globalsEnv_.get())
                      : keel::make_arc<Environment>(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    } else if (resolvedClosure) {
        stackEnv.emplace(resolvedClosure, engine_.globalsEnv_.get());
    } else {
        stackEnv.emplace(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    }
    LexicalEnvGuard lexGuard(activeLexicalEnvs_, heapEnv);
    Environment &localEnv = useLexicalHeap ? *heapEnv : *stackEnv;
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv,
                          extractCallerArgNames(callNode, engine_));

    for (keel::usize i = 0; i < required && i < args.size(); ++i)
        localEnv.setLocal(func.params[i], args[i]);
    if (va) {
        Value vaCell = args.size() > required
                           ? Value::cell(1, args.size() - required, engine_.mr_)
                           : Value::cell(0, 0, engine_.mr_);
        for (keel::usize i = required; i < args.size(); ++i)
            vaCell.cellAt(i - required) = args[i];
        localEnv.setLocal("varargin", keel::move(vaCell));
    }

    func.usesNarginNargout = 1;
    if (func.usesNarginNargout) {
        localEnv.setLocal("nargin",
                          Value::scalar(static_cast<double>(args.size()), engine_.mr_));
        localEnv.setLocal("nargout", Value::scalar(static_cast<double>(nout), engine_.mr_));
    }

    for (auto &retName : func.returns)
        if (!localEnv.getLocal(retName))
            localEnv.setLocal(retName, Value());

    keel::Optional<DebugController::FrameGuard> dbgFrame;
    if (auto *ctl = debugCtl()) {
        StackFrame frame;
        frame.functionName = func.name.clone();
        frame.env = &localEnv;
        dbgFrame.emplace(*ctl, keel::move(frame));
    }

    keel::Optional<FsContext::OriginFrame> originGuard;
    if (func.originKnown)
        originGuard.emplace(engine_.fsContext(), func.sourceFs,
                            func.sourceDir, func.sourceFile);

    execNode(func.body.get(), &localEnv);

    dbgFrame.reset();

    if (flowSignal_ == FlowSignal::RETURN)
        flowSignal_ = FlowSignal::NONE;

    auto readRet = [&](keel::StringRef rname) -> Value {
        if (!useLexicalHeap && !localEnv.isGlobal(rname) && !localEnv.isPersistent(rname)) {
            if (auto *lv = localEnv.getLocal(rname))
                return keel::move(*lv);
        }
        auto *val = localEnv.get(rname);
        return val ? *val : Value();
    };

    const bool hasVarargout = !func.returns.empty() && func.returns.back() == "varargout";
    keel::Array<Value> results;
    if (hasVarargout) {
        const keel::usize numFixed = func.returns.size() - 1;
        results.reserve(nout);
        for (keel::usize i = 0; i < numFixed && results.size() < nout; ++i)
            results.push_back(readRet(func.returns[i]));
        auto *vaVal = localEnv.getLocal("varargout");
        if (!vaVal)
            vaVal = localEnv.get("varargout");
        if (vaVal && vaVal->isCell()) {
            const keel::usize n = vaVal->numel();
            for (keel::usize k = 0; k < n && results.size() < nout; ++k)
                results.push_back(vaVal->cellAt(k));
        }
        if (results.size() < nout)
            throw std::runtime_error("Too many output arguments.");
        return results;
    }

    if (func.returns.size() < nout)
        throw std::runtime_error("Too many output arguments.");
    results.reserve(std::min(func.returns.size(), nout));
    for (keel::usize i = 0; i < func.returns.size() && i < nout; ++i)
        results.push_back(readRet(func.returns[i]));
    return results;
}

// ============================================================
// classdef method / constructor invocation
// ============================================================
keel::Array<Value> TreeWalker::runClassMethod(const UserFunction &uf,
                                              keel::ArrayRef<Value> args, keel::usize nout)
{
    return callUserFunctionMulti(uf, args, engine_.workspaceEnv_.get(), nout, nullptr);
}

Value TreeWalker::evalExpressionPublic(const ASTNode *expr, Environment *env)
{
    return execNode(expr, env ? env : engine_.workspaceEnv_.get());
}

Value TreeWalker::runClassCtor(const UserFunction &func, const Value &seed,
                               keel::ArrayRef<Value> args)
{
    RecursionGuard rguard(currentRecursionDepth_, maxRecursionDepth_);
    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const keel::usize required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size()) {
        auto msg = keel::format("Too many input arguments to constructor '{}'", func.name);
        throw std::runtime_error(msg.c_str());
    }

    keel::Arc<Environment> resolvedClosure = resolveClosureEnv(func);
    keel::Array<const ASTNode *> directNested;
    twCollectDirectNestedDefs(func.body.get(), directNested);
    const bool useLexicalHeap =
        !directNested.empty() || (resolvedClosure && resolvedClosure->isLexicalScope());

    keel::Optional<Environment> stackEnv;
    keel::Arc<Environment> heapEnv;
    if (useLexicalHeap) {
        heapEnv = resolvedClosure
                      ? keel::make_arc<Environment>(resolvedClosure, engine_.globalsEnv_.get())
                      : keel::make_arc<Environment>(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    } else if (resolvedClosure) {
        stackEnv.emplace(resolvedClosure, engine_.globalsEnv_.get());
    } else {
        stackEnv.emplace(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    }
    LexicalEnvGuard lexGuard(activeLexicalEnvs_, heapEnv);
    Environment &localEnv = useLexicalHeap ? *heapEnv : *stackEnv;
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv, {});

    for (keel::usize i = 0; i < required && i < args.size(); ++i)
        localEnv.setLocal(func.params[i], args[i]);
    if (va) {
        Value vaCell = args.size() > required
                           ? Value::cell(1, args.size() - required, engine_.mr_)
                           : Value::cell(0, 0, engine_.mr_);
        for (keel::usize i = required; i < args.size(); ++i)
            vaCell.cellAt(i - required) = args[i];
        localEnv.setLocal("varargin", keel::move(vaCell));
    }
    // MATLAB seeds the constructor's output variable with a default instance.
    if (!func.returns.empty())
        localEnv.setLocal(func.returns[0], seed);
    localEnv.setLocal("nargin", Value::scalar(static_cast<double>(args.size()), engine_.mr_));
    localEnv.setLocal("nargout", Value::scalar(1.0, engine_.mr_));

    keel::Optional<FsContext::OriginFrame> originGuard;
    if (func.originKnown)
        originGuard.emplace(engine_.fsContext(), func.sourceFs,
                            func.sourceDir, func.sourceFile);

    execNode(func.body.get(), &localEnv);
    if (flowSignal_ == FlowSignal::RETURN)
        flowSignal_ = FlowSignal::NONE;

    if (func.returns.empty())
        return seed;
    const keel::String &r0 = func.returns[0];
    if (!useLexicalHeap && !localEnv.isGlobal(r0) && !localEnv.isPersistent(r0)) {
        if (auto *localVal = localEnv.getLocal(r0))
            return keel::move(*localVal);
    }
    auto *val = localEnv.get(r0);
    return val ? *val : seed;
}

// ============================================================
// Debugger helpers
// ============================================================

DebugController *TreeWalker::debugCtl()
{
    return engine_.debugController_.get();
}

} // namespace numkit
