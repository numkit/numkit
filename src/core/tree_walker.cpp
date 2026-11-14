// src/tree_walker.cpp
#include <numkit/core/tree_walker.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/value/index.hpp>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>

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

    std::optional<DebugController::FrameGuard> dbgFrame;
    if (auto *ctl = debugCtl()) {
        ctl->reset();
        StackFrame frame;
        frame.functionName = topLevelName_;
        frame.env = env;
        dbgFrame.emplace(*ctl, std::move(frame));
    }

    return execNode(ast, env);
}

bool TreeWalker::isKnownFunction(const std::string &name) const
{
    static const std::unordered_set<std::string> kBuiltinFuncs = {"tic", "toc"};
    return engine_.externalFuncs_.count(name) || engine_.hasUserFunction(name)
           || kBuiltinFuncs.count(name);
}

// ============================================================
void TreeWalker::output(const std::string &s)
{
    if (engine_.outputFunc_)
        engine_.outputFunc_(s);
    else
        std::cout << s;
}

void TreeWalker::displayValue(const std::string &name, const Value &val)
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
                                       size_t &outIdx)
{
    // Fast path for number literals: B(3)
    if (indexExpr->type == NodeType::NUMBER_LITERAL) {
        return tryIndex0(indexExpr->numValue, outIdx);
    }

    // Fast path for simple identifiers: B(i)
    if (indexExpr->type == NodeType::IDENTIFIER) {
        const std::string &name = indexExpr->strValue;
        if (name == "end") {
            Size sz;
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

std::vector<size_t> TreeWalker::resolveIndex(
    const ASTNode *indexExpr, const Value &array, int dim, int ndims, Environment *env)
{
    IndexContextGuard guard(indexContextStack_, {&array, dim, ndims});

    Value val = execNode(indexExpr, env);

    std::vector<size_t> indices;

    if (val.isChar() && val.toString() == ":") {
        size_t sz;
        if (ndims == 1) {
            sz = array.numel();
        } else if (dim == ndims - 1 && array.dims().ndim() > static_cast<size_t>(ndims)) {
            sz = 1;
            for (size_t d = static_cast<size_t>(dim); d < array.dims().ndim(); ++d)
                sz *= array.dims().dimSize(d);
        } else {
            sz = array.dims().dimSize(dim);
        }
        indices.resize(sz);
        for (size_t i = 0; i < sz; ++i)
            indices[i] = i;
        return indices;
    }

    if (val.isLogical()) {
        const uint8_t *ld = val.logicalData();
        for (size_t i = 0; i < val.numel(); ++i)
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
        for (size_t i = 0; i < val.numel(); ++i)
            indices.push_back(Value::checkedScalarIndex(val.elemAsDouble(i)));
        return indices;
    }

    throw std::runtime_error("Invalid index type: " + std::string(mtypeName(val.type())));
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
    case NodeType::SUPERCLASS_REF:
        // Bare `lhs@Base` with no call — invalid: a super-call must be
        // invoked (`obj@Base(args)` / `method@Base(obj,...)`).
        throw std::runtime_error(
            "superclass reference '" + node->strValue
            + "' must be called: use lhs@" + node->strValue + "(...)");
    case NodeType::CELL_INDEX:
        return execCellIndex(node, env);
    case NodeType::FIELD_ACCESS:
        return execFieldAccess(node, env);
    case NodeType::DYNAMIC_FIELD_ACCESS: {
        auto obj = execNode(node->children[0].get(), env);
        std::string fname = execNode(node->children[1].get(), env).toString();
        if (obj.isObject())
            return objectPropGet(obj, fname, env);
        if (!obj.isStruct())
            throw std::runtime_error("Dot indexing requires a struct");
        if (obj.isStructArray() || obj.numel() == 0) {
            size_t n = obj.numel();
            if (n == 0 && !obj.hasField(fname))
                throw std::runtime_error("Reference to non-existent field '" + fname + "'");
            Value csl = Value::csl(n, engine_.mr_);
            for (size_t i = 0; i < n; ++i) {
                const auto &m = obj.structArrayElem(i);
                auto it = m.find(fname);
                if (it == m.end())
                    throw std::runtime_error("Reference to non-existent field '" + fname + "'");
                csl.cslAt(i) = it->second;
            }
            return csl;
        }
        if (!obj.hasField(fname))
            throw std::runtime_error("Reference to non-existent field '" + fname + "'");
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
                if (cls && cls->methods.count("__end")) {
                    Value self = *ctx.array;
                    Value eargs[2] = {
                        Value::scalar(static_cast<double>(ctx.dimension), engine_.mr_),
                        Value::scalar(static_cast<double>(ctx.ndims), engine_.mr_)};
                    Value out[1];
                    CallContext cctx{&engine_, env};
                    cls->methods.at("__end")(self, Span<const Value>(eargs, 2), 1,
                                             Span<Value>(out, 1), cctx);
                    return out[0];
                }
            }
            size_t sz = (ctx.ndims == 1) ? ctx.array->numel()
                                         : ctx.array->dims().dimSize(ctx.dimension);
            return Value::scalar(static_cast<double>(sz), engine_.mr_);
        }
        throw std::runtime_error("'end' used outside of indexing context");
    }
    default:
        throw std::runtime_error("Unknown AST node type");
    }
}

static std::string describeNode(const ASTNode *node)
{
    switch (node->type) {
    case NodeType::CALL:
        if (!node->strValue.empty())
            return "in call to '" + node->strValue + "'";
        if (!node->children.empty() && node->children[0]->type == NodeType::IDENTIFIER)
            return "in call to '" + node->children[0]->strValue + "'";
        return "in function call";
    case NodeType::CELL_INDEX:
        if (!node->children.empty() && node->children[0]->type == NodeType::IDENTIFIER)
            return "in cell indexing of '" + node->children[0]->strValue + "'";
        return "in cell indexing";
    case NodeType::BINARY_OP:
        return "in operator '" + node->strValue + "'";
    case NodeType::UNARY_OP:
        return "in unary operator '" + node->strValue + "'";
    case NodeType::FIELD_ACCESS:
        return "in field access '." + node->strValue + "'";
    case NodeType::DYNAMIC_FIELD_ACCESS:
        return "in dynamic field access";
    case NodeType::ASSIGN:
        if (!node->children.empty() && node->children[0]->type == NodeType::IDENTIFIER)
            return "in assignment to '" + node->children[0]->strValue + "'";
        return "in assignment";
    case NodeType::COLON_EXPR:
        return "in colon expression";
    case NodeType::MATRIX_LITERAL:
        return "in matrix construction";
    case NodeType::CELL_LITERAL:
        return "in cell construction";
    case NodeType::IDENTIFIER:
        return "'" + node->strValue + "'";
    case NodeType::EXPR_STMT:
        if (!node->children.empty())
            return describeNode(node->children[0].get());
        return "";
    default:
        return "";
    }
}

Value TreeWalker::execNode(const ASTNode *node, Environment *env)
{
    // Single-value sink: collapse any comma-separated list the expression produced.
    // The isCsl() guard keeps the hot path a single branch + move (collapseCsl, which
    // takes the value by copy, only runs for an actual CSL). See dev-docs/memory/csl_first_class.md.
    Value v = execNodeExpand(node, env);
    if (v.isCsl())
        return collapseCsl(std::move(v));
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
                if (!ctl->checkLine(static_cast<uint16_t>(child->line),
                                    static_cast<uint16_t>(child->col),
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
Value TreeWalker::execIdentifier(const ASTNode *node, Environment *env, size_t nargout)
{
    const std::string &name = node->strValue;

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
        (*fn)({}, nargout, Span<Value>(outBuf, 1), ctx);
        return outBuf[0].isEmpty() ? Value() : outBuf[0];
    }
    // Bare class name constructs a default instance (`x = MyClass`), like
    // MATLAB — when the name is a registered class and not shadowed above.
    if (const BuiltinClass *cls = engine_.findClass(name); cls && cls->construct) {
        CallContext ctx{&engine_, env};
        return engine_.constructChecked(cls, Span<const Value>(nullptr, 0), ctx);
    }

    // MATLAB-exact error for the nargin/nargout pseudo-vars when they're
    // referenced outside of any function scope. Inside a function they are
    // setLocal'd on entry so env->get() above finds them.
    if (name == "nargin" || name == "nargout")
        throw std::runtime_error(
            "You can only call nargin/nargout from within a function.");

    throw std::runtime_error("Undefined variable or function: " + name);
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
        std::string fname = execNode(lhs->children[1].get(), env).toString();
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
                if (cls && cls->propSet && st && st->props.count(cur->strValue)) {
                    Value updated = st->props.at(cur->strValue);
                    objectPropSet(baseObj, cur->strValue, updated, env);
                    break;
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

    size_t nargs = lhs->children.size() - 1;

    // OBJECT: obj(i…) = v dispatches to the class subsasgn overload,
    // which mutates `var` in place (value/handle rule via objectStateMut).
    // args = [subscripts…, value].
    if (var->isObject()) {
        const BuiltinClass *cls = engine_.findClass(var->objectClassName());
        if (cls && cls->subsasgn) {
            std::vector<Value> args;
            args.reserve(nargs + 1);
            for (size_t i = 0; i < nargs; ++i) {
                IndexContextGuard guard(indexContextStack_,
                                        {var, static_cast<int>(i), static_cast<int>(nargs)});
                args.push_back(execNode(lhs->children[i + 1].get(), env));
            }
            args.push_back(rhs);
            Value outBuf[1];
            CallContext ctx{&engine_, env};
            cls->subsasgn(*var, Span<const Value>(args.data(), args.size()), 0,
                          Span<Value>(outBuf, 1), ctx);
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
        auto isGfxCls = [](const std::string &c) {
            return c.rfind("matlab.graphics.", 0) == 0 || c.rfind("matlab.ui.", 0) == 0;
        };
        if (!var->isObject() && var->isNumeric() && isGfxCls(rhs.objectClassName())) {
            *var = Value();
        }
        const bool newable = !var->isObject() && (var->isUnset() || var->isEmpty());
        const bool sameArr = canPromoteRhs ||
            (var->isObject() && (var->objectClassName() == rhs.objectClassName()
                                 || (isGfxCls(var->objectClassName()) && isGfxCls(rhs.objectClassName()))));
        if (newable || sameArr) {
            std::vector<std::vector<size_t>> perDim(nargs);
            for (size_t d = 0; d < nargs; ++d) {
                const ASTNode *sub = lhs->children[d + 1].get();
                if ((var->isUnset() || var->isEmpty())
                    && sub->type == NodeType::COLON_EXPR && sub->children.empty()) {
                    const size_t n = (nargs == 2 && d == 1) ? rhs.numel() : rhs.dims().dim(static_cast<int>(d));
                    perDim[d].resize(n);
                    for (size_t k = 0; k < n; ++k) perDim[d][k] = k;
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
            size_t maxIdx = 0;
            for (auto idx : indices)
                if (idx >= maxIdx) maxIdx = idx + 1;
            if (maxIdx > var->numel()) {
                bool isColVec = (var->dims().cols() == 1 && var->dims().rows() > 1);
                if (isColVec)
                    var->resize(maxIdx, 1, engine_.mr_);
                else
                    var->resize(1, maxIdx, engine_.mr_);
            }
            const std::string rs = rhs.toString();
            char *data = var->charDataMut();
            if (rs.size() == 1) {
                for (auto idx : indices)
                    data[idx] = rs[0];
            } else {
                for (size_t i = 0; i < indices.size() && i < rs.size(); ++i)
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
            for (size_t idx : indices) {
                var->growStructArrayTo(idx, engine_.mr_);
                for (const auto &name : rhs.fieldNamesInOrder())
                    var->setField(idx, name, rhs.field(name));
            }
            return;
        }
        // ── scalar fast path: B(i) = scalar ──
        if (rhs.isScalar()) {
            size_t scalarIdx;
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
            size_t ri, ci;
            if (tryResolveScalarIndex(lhs->children[1].get(), *var, 0, 2, env, ri)
                && tryResolveScalarIndex(lhs->children[2].get(), *var, 1, 2, env, ci)) {
                size_t needR = ri + 1, needC = ci + 1;
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

        auto rowIdx = riIsColon ? std::vector<size_t>() : resolveIndex(rSub, *var, 0, 2, env);
        auto colIdx = ciIsColon ? std::vector<size_t>() : resolveIndex(cSub, *var, 1, 2, env);

        size_t maxR = var->dims().rows(), maxC = var->dims().cols();
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
        const size_t target2D = (riIsColon ? maxR : rowIdx.size())
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
            size_t ri, ci, pi;
            if (tryResolveScalarIndex(lhs->children[1].get(), *var, 0, 3, env, ri)
                && tryResolveScalarIndex(lhs->children[2].get(), *var, 1, 3, env, ci)
                && tryResolveScalarIndex(lhs->children[3].get(), *var, 2, 3, env, pi)) {
                size_t needR = ri + 1, needC = ci + 1, needP = pi + 1;
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

        auto rowIdx = riColon ? std::vector<size_t>() : resolveIndex(rSub, *var, 0, 3, env);
        auto colIdx = ciColon ? std::vector<size_t>() : resolveIndex(cSub, *var, 1, 3, env);
        auto pageIdx = piColon ? std::vector<size_t>() : resolveIndex(pSub, *var, 2, 3, env);

        size_t maxR = var->dims().rows(), maxC = var->dims().cols(), maxP = var->dims().pages();
        for (auto r : rowIdx)
            maxR = std::max(maxR, r + 1);
        for (auto c : colIdx)
            maxC = std::max(maxC, c + 1);
        for (auto p : pageIdx)
            maxP = std::max(maxP, p + 1);
        if (var->isEmpty() && (!rhs.isScalar() || (var->dims().rows() == 0 && var->dims().cols() == 0 && !rhs.isEmpty()))) {
            const int nColons = (riColon ? 1 : 0) + (ciColon ? 1 : 0) + (piColon ? 1 : 0);
            const size_t nonColonProd = (riColon ? 1 : rowIdx.size())
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
                size_t rCursor = 0;
                int colonsLeft = nColons;
                auto nextRhsDim = [&]() -> size_t {
                    while (rCursor < static_cast<size_t>(rhs.dims().ndim())
                           && rhs.dims().dim(static_cast<int>(rCursor)) == 1
                           && (static_cast<size_t>(rhs.dims().ndim()) - rCursor) > static_cast<size_t>(colonsLeft)) {
                        ++rCursor;
                    }
                    --colonsLeft;
                    return (rCursor < static_cast<size_t>(rhs.dims().ndim()))
                               ? rhs.dims().dim(static_cast<int>(rCursor++))
                               : 1;
                };
                if (riColon) maxR = nextRhsDim();
                if (ciColon) maxC = nextRhsDim();
                if (piColon) maxP = nextRhsDim();
            }
        }
        const size_t target3D = (riColon ? maxR : rowIdx.size())
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
        std::vector<std::vector<size_t>> idxLists(nd);
        std::vector<const size_t *> idxPtrs(nd);
        std::vector<size_t> idxCounts(nd);
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
        std::string fname = execNode(node->children[1].get(), env).toString();
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
    const size_t nidx = node->children.size() - 1;
    std::vector<size_t> coords(nidx);
    for (size_t i = 0; i < nidx; ++i) {
        IndexContextGuard guard(indexContextStack_,
                                {var, static_cast<int>(i), static_cast<int>(nidx)});
        Value v = execNode(node->children[i + 1].get(), env);
        coords[i] = Value::checkedScalarIndex(v.toScalar());
    }
    size_t linear = var->growCellTo(coords.data(), static_cast<int>(nidx), engine_.mr_);
    return var->cellAt(linear);
}

Value &TreeWalker::resolveFieldLValue(const ASTNode *node, Environment *env)
{
    auto *objNode = node->children[0].get();
    const std::string &fieldName = node->strValue;

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

        const size_t nargs = objNode->children.size() - 1;
        if (nargs > static_cast<size_t>(Dims::kMaxRank))
            throw std::runtime_error("Struct array indexing rank exceeds maximum supported rank");
        std::vector<size_t> coords(nargs);
        for (size_t a = 0; a < nargs; ++a) {
            IndexContextGuard guard(indexContextStack_,
                                    {var, static_cast<int>(a), static_cast<int>(nargs)});
            Value v = execNode(objNode->children[a + 1].get(), env);
            coords[a] = Value::checkedScalarIndex(v.toScalar());
        }
        size_t linear;
        if (nargs == 1) {
            linear = coords[0];
            var->growStructArrayTo(linear, engine_.mr_); // preserves row/col vector shape
        } else {
            linear = var->growStructArrayND(coords.data(), static_cast<int>(nargs),
                                            engine_.mr_);
        }

        auto &fieldMap = var->structArrayElem(linear);
        // BUG #15: track insertion order for fieldnames() before [] auto-creates.
        if (fieldMap.find(fieldName) == fieldMap.end()) {
            if (var->isStruct()) {
                // setField updates fieldOrder for new key; reuse it then return ref.
                var->setField(linear, fieldName, Value{});
            }
        }
        return var->structArrayElem(linear)[fieldName];
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
                st->props[fieldName] = std::move(cur);
        }
        return st->props[fieldName];
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
            const std::string &fname = lhs->strValue;
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
            if (cls && cls->methods.count("__brace_set")) {
                const size_t nidx = lhs->children.size() - 1;
                std::vector<Value> args;
                args.reserve(nidx + 1);
                for (size_t i = 0; i < nidx; ++i) {
                    IndexContextGuard guard(indexContextStack_,
                                            {&baseObj, static_cast<int>(i), static_cast<int>(nidx)});
                    args.push_back(execNode(lhs->children[i + 1].get(), env));
                }
                args.push_back(rhs);
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                cls->methods.at("__brace_set")(baseObj, Span<const Value>(args.data(), args.size()),
                                               0, Span<Value>(outBuf, 1), ctx);
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
            const bool isColonSub = sub.isChar() && sub.toString() == ":";
            if (sub.isLogical() || isColonSub || !sub.isScalar()) {
                size_t maxLen = cellVar.numel();
                if (!sub.isLogical() && !isColonSub) {
                    for (size_t k = 0; k < sub.numel(); ++k) {
                        size_t idx = Value::checkedScalarIndex(sub.elemAsDouble(k));
                        if (idx + 1 > maxLen)
                            maxLen = idx + 1;
                    }
                    if (maxLen > cellVar.numel()) {
                        size_t lastCoord = maxLen - 1;
                        cellVar.growCellTo(&lastCoord, 1, engine_.mr_);
                    }
                }
                auto indices = Value::resolveIndices(sub, cellVar.numel());
                for (size_t idx : indices)
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
    const size_t nout = node->returnNames.size();
    auto results = execCallMulti(node->children[0].get(), env, nout);

    // MATLAB: requesting more outputs than the RHS produces is an error
    // at the call site ("Too many output arguments"), not a silently
    // unassigned target that later reads as a phantom undefined function
    // (bug #44 — `[c,sz,n2,p] = bwconncomp(A)` where bwconncomp yields
    // one value). An unfilled output slot is the unset sentinel.
    for (size_t i = 0; i < nout; ++i)
        if (i >= results.size() || results[i].isUnset())
            throw std::runtime_error("Too many output arguments.");

    // Complex-target path: at least one output is a general lvalue
    // (`s.f`, `a(i)`, `c{i}`, ...). lhsTargets is authoritative; a
    // nullptr entry is an ignored `~`.
    if (!node->lhsTargets.empty()) {
        for (size_t i = 0; i < node->lhsTargets.size() && i < results.size(); ++i)
            if (node->lhsTargets[i])
                assignLValue(node->lhsTargets[i].get(), results[i], env);

        // Display only bare-identifier targets (mirrors single-assign,
        // where `s.f = v` / `a(i) = v` do not auto-display in the TW).
        if (!node->suppressOutput && !results.empty())
            for (size_t i = 0; i < node->lhsTargets.size() && i < results.size(); ++i) {
                const ASTNode *t = node->lhsTargets[i].get();
                if (t && t->type == NodeType::IDENTIFIER)
                    displayValue(t->strValue, results[i]);
            }
        return results.empty() ? Value() : results[0];
    }

    for (size_t i = 0; i < node->returnNames.size() && i < results.size(); ++i)
        if (node->returnNames[i] != "~")
            env->set(node->returnNames[i], results[i]);

    if (!node->suppressOutput && !results.empty())
        for (size_t i = 0; i < node->returnNames.size() && i < results.size(); ++i)
            if (node->returnNames[i] != "~")
                displayValue(node->returnNames[i], results[i]);

    return results.empty() ? Value() : results[0];
}

std::vector<Value> TreeWalker::execCallMulti(const ASTNode *node, Environment *env, size_t nout)
{
    // Cell CSL: [a,b] = c{idx} — expand cell elements into separate outputs
    if (node->type == NodeType::CELL_INDEX) {
        const Value *cell = env->get(node->children[0]->strValue);
        if (!cell || !cell->isCell())
            throw std::runtime_error("Cell indexing requires a cell array");

        size_t nidx = node->children.size() - 1;
        if (nidx == 1) {
            auto indices = resolveIndex(node->children[1].get(), *cell, 0, 1, env);
            std::vector<Value> out;
            out.reserve(indices.size());
            for (size_t idx : indices)
                out.push_back(cell->cellAt(idx));
            return out;
        } else if (nidx == 2) {
            auto rowIdx = resolveIndex(node->children[1].get(), *cell, 0, 2, env);
            auto colIdx = resolveIndex(node->children[2].get(), *cell, 1, 2, env);
            std::vector<Value> out;
            for (size_t c : colIdx)
                for (size_t r : rowIdx)
                    out.push_back(cell->cellAt(cell->dims().sub2ind(r, c)));
            return out;
        } else {
            throw std::runtime_error("Cell CSL with " + std::to_string(nidx) + " indices not supported");
        }
    }

    if (node->type != NodeType::CALL) {
        if (nout <= 1)
            return {execNode(node, env)};
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
    std::string funcName;
    if (headNode->type == NodeType::FIELD_ACCESS) {
        const ASTNode *recvNode = headNode->children[0].get();
        std::string dotted = headNode->strValue;
        const ASTNode *cur = recvNode;
        while (cur && cur->type == NodeType::FIELD_ACCESS) {
            dotted = cur->strValue + "." + dotted;
            cur = cur->children.empty() ? nullptr : cur->children[0].get();
        }
        bool isPackageCall = false;
        if (cur && cur->type == NodeType::IDENTIFIER) {
            dotted = cur->strValue + "." + dotted;
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
                const std::string &mname = headNode->strValue;
                const BuiltinClass *cls = engine_.findClass(objPtr->objectClassName());
                if (cls && cls->methods.count(mname)) {
                    std::vector<Value> margs;
                    margs.reserve(node->children.size() - 1);
                    for (size_t i = 1; i < node->children.size(); ++i) {
                        Value v = execNodeExpand(node->children[i].get(), env);
                        if (v.isCsl())
                            for (size_t k = 0; k < v.cslCount(); ++k)
                                margs.push_back(std::move(v.cslAt(k)));
                        else
                            margs.push_back(std::move(v));
                    }
                    Value self = *objPtr; // handle: shares state; value: own copy
                    std::vector<Value> outBuf(nout);
                    CallContext ctx{&engine_, env};
                    cls->methods.at(mname)(self, Span<const Value>(margs.data(), margs.size()),
                                           nout, Span<Value>(outBuf), ctx);
                    return outBuf;
                }
                Value prop = objectPropGet(*objPtr, mname, env);
                if (prop.isFuncHandle()) {
                    std::vector<Value> pargs;
                    pargs.reserve(node->children.size() - 1);
                    for (size_t i = 1; i < node->children.size(); ++i) {
                        Value v = execNodeExpand(node->children[i].get(), env);
                        if (v.isCsl())
                            for (size_t k = 0; k < v.cslCount(); ++k)
                                pargs.push_back(std::move(v.cslAt(k)));
                        else
                            pargs.push_back(std::move(v));
                    }
                    return callFuncHandleMulti(prop, pargs, env, nout, node);
                }
            } else if (objPtr && objPtr->isStruct() && objPtr->hasField(headNode->strValue)) {
                Value fld = objPtr->field(headNode->strValue);
                if (fld.isFuncHandle()) {
                    std::vector<Value> fargs;
                    fargs.reserve(node->children.size() - 1);
                    for (size_t i = 1; i < node->children.size(); ++i) {
                        Value v = execNodeExpand(node->children[i].get(), env);
                        if (v.isCsl())
                            for (size_t k = 0; k < v.cslCount(); ++k)
                                fargs.push_back(std::move(v.cslAt(k)));
                        else
                            fargs.push_back(std::move(v));
                    }
                    return callFuncHandleMulti(fld, fargs, env, nout, node);
                }
            }
        }
        funcName = std::move(dotted);
    } else if (headNode->type != NodeType::IDENTIFIER) {
        Value callee = execNode(headNode, env);
        std::vector<Value> cargs;
        cargs.reserve(node->children.size() - 1);
        for (size_t i = 1; i < node->children.size(); ++i) {
            Value v = execNodeExpand(node->children[i].get(), env);
            if (v.isCsl())
                for (size_t k = 0; k < v.cslCount(); ++k)
                    cargs.push_back(std::move(v.cslAt(k)));
            else
                cargs.push_back(std::move(v));
        }
        if (callee.isFuncHandle())
            return callFuncHandleMulti(callee, cargs, env, nout, node);
        if (callee.isObject()) {
            const BuiltinClass *cls = engine_.findClass(callee.objectClassName());
            if (cls && cls->subsref) {
                std::vector<Value> outBuf(nout);
                CallContext ctx{&engine_, env};
                cls->subsref(callee, Span<const Value>(cargs.data(), cargs.size()), nout,
                             Span<Value>(outBuf), ctx);
                return outBuf;
            }
        }
        throw std::runtime_error("Cannot invoke non-function value with multiple outputs");
    } else {
        funcName = headNode->strValue;
    }

    std::vector<Value> args;
    args.reserve(node->children.size() - 1);
    for (size_t i = 1; i < node->children.size(); ++i) {
        // CSL arg [a,b]=f(.., c{:}, ..): splice a comma-separated list (c{:} / c{idx}
        // / c{r,:}, or any CSL producer) into the argument list; non-CSL is one arg.
        Value v = execNodeExpand(node->children[i].get(), env);
        if (v.isCsl())
            for (size_t k = 0; k < v.cslCount(); ++k)
                args.push_back(std::move(v.cslAt(k)));
        else
            args.push_back(std::move(v));
    }

    auto *var = env->get(funcName);
    if (var && var->isFuncHandle())
        return callFuncHandleMulti(*var, args, env, nout, node);
    if (var && var->isObject()) {
        const BuiltinClass *cls = engine_.findClass(var->objectClassName());
        if (cls && cls->subsref) {
            Value self = *var;
            std::vector<Value> outBuf(nout);
            CallContext ctx{&engine_, env};
            cls->subsref(self, Span<const Value>(args.data(), args.size()), nout,
                         Span<Value>(outBuf), ctx);
            return outBuf;
        }
    }

    // OBJECT function-form multi-output: [a,b] = m(obj, ...). A class
    // method on the dominant (first) object argument beats a path function.
    if (!args.empty() && args[0].isObject()) {
        const BuiltinClass *cls = engine_.findClass(args[0].objectClassName());
        if (cls && cls->methods.count(funcName)) {
            Value self = args[0];
            std::vector<Value> rest(args.begin() + 1, args.end());
            std::vector<Value> outBuf(nout);
            CallContext ctx{&engine_, env};
            cls->methods.at(funcName)(self, Span<const Value>(rest.data(), rest.size()),
                                      nout, Span<Value>(outBuf), ctx);
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
        std::vector<Value> outBuf(nout);
        CallContext ctx{&engine_, env};
        (*fn)(args, nout, Span<Value>(outBuf), ctx);
        return outBuf;
    }

    throw std::runtime_error(
        engine_.formatUndefinedFunctionError(funcName, "Undefined function: " + funcName));
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
    if (!var)
        throw std::runtime_error("Undefined variable: " + target->strValue);

    size_t nargs = lhs->children.size() - 1;

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
        std::vector<std::vector<size_t>> perDim(nargs);
        std::vector<const size_t *> perDimPtrs(nargs);
        std::vector<size_t> perDimCount(nargs);
        for (size_t i = 0; i < nargs; ++i) {
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
    const std::string &op = node->strValue;

    if (op == "&&" || op == "||") {
        std::vector<const ASTNode *> spine;
        const ASTNode *cur = node;
        while (cur->type == NodeType::BINARY_OP
               && (cur->strValue == "&&" || cur->strValue == "||")) {
            spine.push_back(cur);
            cur = cur->children[0].get();
        }
        bool acc = toLogicalScalarBool(execNode(cur, env));
        for (auto it = spine.rbegin(); it != spine.rend(); ++it) {
            const ASTNode *step = *it;
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

    auto evalPair = [&](const std::string &stepOp, const Value &left, const Value &right) -> Value {
        if (left.isObject() || right.isObject()) {
            Value out;
            if (engine_.tryObjectBinaryOp(stepOp, left, right, env, out))
                return out;
        }
        auto it = engine_.binaryOps_.find(stepOp);
        if (it != engine_.binaryOps_.end())
            return it->second(left, right);
        throw std::runtime_error("Undefined binary operator: " + stepOp);
    };

    if (node->children[0]->type == NodeType::BINARY_OP
        && node->children[0]->strValue != "&&"
        && node->children[0]->strValue != "||") {
        std::vector<const ASTNode *> spine;
        const ASTNode *cur = node;
        while (cur->type == NodeType::BINARY_OP
               && cur->strValue != "&&"
               && cur->strValue != "||") {
            spine.push_back(cur);
            cur = cur->children[0].get();
        }
        Value acc = execNode(cur, env);
        for (auto it = spine.rbegin(); it != spine.rend(); ++it) {
            const ASTNode *step = *it;
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

    auto it = engine_.unaryOps_.find(node->strValue);
    if (it != engine_.unaryOps_.end())
        return it->second(operand);

    throw std::runtime_error("Undefined unary operator: " + node->strValue);
}

// ============================================================
Value TreeWalker::callFuncHandle(const Value &handle, Span<const Value> args, Environment *env,
                                  const ASTNode *callNode)
{
    auto results = callFuncHandleMulti(handle, args, env, 1, callNode);
    return results.empty() ? Value() : results[0];
}

Value TreeWalker::callHandlePublic(const Value &handle,
                                    Span<const Value> args,
                                    Environment *env)
{
    return callFuncHandle(handle, args, env);
}

std::vector<Value>
TreeWalker::callHandleMultiPublic(const Value &handle,
                                  Span<const Value> args,
                                  Environment *env,
                                  size_t nout)
{
    return callFuncHandleMulti(handle, args, env, nout);
}

std::vector<Value> TreeWalker::callFuncHandleMulti(const Value &handle,
                                                    Span<const Value> args,
                                                    Environment *env,
                                                    size_t nout,
                                                    const ASTNode *callNode)
{
    std::vector<Value> withCaptures;
    if (handle.funcHandleCaptureCount() > 0) {
        const std::size_t ncaps = handle.funcHandleCaptureCount();
        withCaptures.reserve(args.size() + ncaps);
        for (const auto &a : args)
            withCaptures.push_back(a);
        for (std::size_t i = 0; i < ncaps; ++i)
            withCaptures.push_back(handle.funcHandleCapture(i));
        args = Span<const Value>(withCaptures.data(), withCaptures.size());
    }
    Environment *e = env ? env : engine_.workspaceEnv_.get();
    const std::string &name = handle.funcHandleName();
    if (auto *_uf = engine_.lookupUserFunction(name, e))
        return callUserFunctionMulti(*_uf, args, e, nout, callNode);
    if (const ExternalFunc *fn = engine_.findExternal(name, e)) {
        std::vector<Value> outBuf(nout);
        CallContext ctx{&engine_, e};
        (*fn)(args, nout, Span<Value>(outBuf), ctx);
        return outBuf;
    }
    throw std::runtime_error("Undefined function in handle: @" + name);
}

std::vector<Value> TreeWalker::execSuperCall(const ASTNode *superRef, const ASTNode *callNode,
                                             Environment *env, size_t nargout)
{
    const std::string &base = superRef->strValue;
    const ASTNode *lhs = superRef->children[0].get();

    // Evaluate the call arguments (children[1..] of the enclosing CALL).
    std::vector<Value> args;
    args.reserve(callNode->children.size() - 1);
    for (size_t i = 1; i < callNode->children.size(); ++i)
        args.push_back(execNode(callNode->children[i].get(), env));
    Span<const Value> argSpan(args.data(), args.size());

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
            return {std::move(out)};
        }
        return engine_.superMethod(base, lhs->strValue, argSpan, nargout);
    }

    // Non-identifier lhs (uncommon): evaluate it as the object and run the
    // superclass constructor with it as the seed.
    Value seed = execNode(lhs, env);
    Value out = engine_.superConstruct(base, seed, argSpan);
    return {std::move(out)};
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
        const std::string &name = op->strValue;
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
        std::vector<Value> vals;
        vals.reserve(operands->size());
        for (const ASTNode *opNode : *operands)
            vals.push_back(execNode(opNode, env));
        if (rule.execute(vals.data(), vals.size(), out, engine_.resource())) {
            engine_.noteFusionHit();
            return true;
        }
        return false;
    }
    return false;
}

Value TreeWalker::execCall(const ASTNode *node, Environment *env, size_t nargout)
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
        auto results = execSuperCall(funcNode, node, env, std::max<size_t>(nargout, 1));
        return results.empty() ? Value() : std::move(results[0]);
    }

    if (funcNode->type != NodeType::IDENTIFIER) {
        // Qualified-name call: a chain of FIELD_ACCESS over a root
        // IDENTIFIER (`pkg.foo(x)`, `pkg.sub.bar(x)`). When the root
        // identifier is NOT a workspace variable, try resolving the
        // dotted name as a namespace member (registered builtin or
        // +pkg/.../<leaf>.m). A real struct variable shadows this —
        // the existing FIELD_ACCESS path then handles `s.field(x)`.
        const ASTNode *rootIdent = nullptr;
        std::string qualified = tryBuildQualifiedName(funcNode, &rootIdent);
        if (!qualified.empty() && !env->get(rootIdent->strValue)) {
            std::vector<Value> args;
            args.reserve(node->children.size() - 1);
            for (size_t i = 1; i < node->children.size(); ++i)
                args.push_back(execNode(node->children[i].get(), env));
            // OBJECT: package-qualified constructor (`containers.Map(...)`).
            if (const BuiltinClass *cls = engine_.findClass(qualified);
                cls && cls->construct) {
                CallContext ctx{&engine_, env};
                return engine_.constructChecked(cls, Span<const Value>(args.data(), args.size()),
                                                ctx);
            }
            // User-defined first (MATLAB precedence).
            if (auto *uf = engine_.lookupUserFunction(qualified, env))
                return callUserFunction(*uf, args, env, node);
            if (const ExternalFunc *fn = engine_.findExternal(qualified, env)) {
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                (*fn)(args, nargout, Span<Value>(outBuf, 1), ctx);
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
            throw std::runtime_error("Undefined function or variable: " + qualified);
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
                const std::string &mname = funcNode->strValue;
                const BuiltinClass *cls = engine_.findClass(objPtr->objectClassName());
                if (cls && cls->methods.count(mname)) {
                    std::vector<Value> args;
                    args.reserve(node->children.size() - 1);
                    for (size_t i = 1; i < node->children.size(); ++i)
                        args.push_back(execNode(node->children[i].get(), env));
                    Value self = *objPtr; // handle: shares state; value: own copy
                    Value outBuf[1];
                    CallContext ctx{&engine_, env};
                    cls->methods.at(mname)(self, Span<const Value>(args.data(), args.size()),
                                           nargout, Span<Value>(outBuf, 1), ctx);
                    return outBuf[0];
                }
                // Not a method → property read, then CALL (a func-handle
                // property: obj.fh(args) — the inline classdef's direct
                // dispatch, bugs/closed/lang/handle-call-csl-splat) or
                // index with the args.
                Value prop = objectPropGet(*objPtr, mname, env);
                if (prop.isFuncHandle()) {
                    std::vector<Value> args;
                    args.reserve(node->children.size() - 1);
                    for (size_t i = 1; i < node->children.size(); ++i) {
                        Value v = execNodeExpand(node->children[i].get(), env);
                        if (v.isCsl())
                            for (size_t k = 0; k < v.cslCount(); ++k)
                                args.push_back(std::move(v.cslAt(k)));
                        else
                            args.push_back(std::move(v));
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
            const std::string &mname = funcNode->strValue;
            if (recv.isObject()) {
                const BuiltinClass *cls = engine_.findClass(recv.objectClassName());
                if (cls && cls->methods.count(mname)) {
                    std::vector<Value> margs;
                    margs.reserve(node->children.size() - 1);
                    for (size_t i = 1; i < node->children.size(); ++i)
                        margs.push_back(execNode(node->children[i].get(), env));
                    Value self = recv;
                    Value outBuf[1];
                    CallContext ctx{&engine_, env};
                    cls->methods.at(mname)(self, Span<const Value>(margs.data(), margs.size()),
                                           nargout, Span<Value>(outBuf, 1), ctx);
                    return outBuf[0];
                }
                Value prop = objectPropGet(recv, mname, env);
                return execIndexAccess(prop, node, env);
            }
            // Non-object receiver: finish the field access on the evaluated
            // value (mirrors execFieldAccess) and feed the shared tail below.
            if (!recv.isStruct())
                throw std::runtime_error("Dot indexing requires a struct, got "
                                         + std::string(mtypeName(recv.type())));
            if (!recv.hasField(mname))
                throw std::runtime_error("Reference to non-existent field '" + mname + "'");
            target = recv.field(mname);
            haveTarget = true;
        }

        if (!haveTarget)
            target = execNode(funcNode, env);

        if (target.isFuncHandle()) {
            // CSL-splice args (obj.fh(c{:}) — the computed-callee twin of
            // the identifier path's buildArgs;
            // bugs/closed/lang/handle-call-csl-splat).
            std::vector<Value> args;
            args.reserve(node->children.size() - 1);
            for (size_t i = 1; i < node->children.size(); ++i) {
                Value v = execNodeExpand(node->children[i].get(), env);
                if (v.isCsl())
                    for (size_t k = 0; k < v.cslCount(); ++k)
                        args.push_back(std::move(v.cslAt(k)));
                else
                    args.push_back(std::move(v));
            }
            return callFuncHandle(target, args, env, node);
        }

        if (target.isNumeric() || target.isLogical() || target.isChar()
            || target.isCell() || target.isStruct() || target.isString())
            return execIndexAccess(target, node, env);

        throw std::runtime_error("Cannot call or index into value of type "
                                 + std::string(mtypeName(target.type())));
    }

    const std::string &name = funcNode->strValue;

    auto buildArgs = [&]() {
        std::vector<Value> args;
        args.reserve(node->children.size() - 1);
        for (size_t i = 1; i < node->children.size(); ++i) {
            // CSL: an argument expression may expand to a comma-separated list
            // (c{:} / c{idx} / c{r,:}, and any future CSL producer). execNodeExpand
            // preserves a CSL; splice its elements into the argument list. A non-CSL
            // value is a single argument.
            Value v = execNodeExpand(node->children[i].get(), env);
            if (v.isCsl())
                for (size_t k = 0; k < v.cslCount(); ++k)
                    args.push_back(std::move(v.cslAt(k)));
            else
                args.push_back(std::move(v));
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
                const size_t nsub = node->children.size() - 1;
                std::vector<Value> args;
                args.reserve(nsub);
                for (size_t i = 0; i < nsub; ++i) {
                    IndexContextGuard guard(indexContextStack_,
                                            {var, static_cast<int>(i), static_cast<int>(nsub)});
                    Value v = execNodeExpand(node->children[i + 1].get(), env);
                    if (v.isCsl())
                        for (size_t k = 0; k < v.cslCount(); ++k)
                            args.push_back(std::move(v.cslAt(k)));
                    else
                        args.push_back(std::move(v));
                }
                Value self = *var;
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                cls->subsref(self, Span<const Value>(args.data(), args.size()), nargout,
                             Span<Value>(outBuf, 1), ctx);
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
            return engine_.constructChecked(cls, Span<const Value>(args.data(), args.size()), ctx);
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
            if (cls && cls->methods.count(name)) {
                Value self = args[0];
                std::vector<Value> rest(args.begin() + 1, args.end());
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                cls->methods.at(name)(self, Span<const Value>(rest.data(), rest.size()),
                                      nargout, Span<Value>(outBuf, 1), ctx);
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
            (*fn)(args, nargout, Span<Value>(outBuf, 1), ctx);
            return outBuf[0];
        }
    }

    if (var) {
        throw std::runtime_error("Cannot index into variable '" + name + "' of type "
                                 + std::string(mtypeName(var->type())) + ", and no function '"
                                 + name + "' was found");
    }
    throw std::runtime_error(
        engine_.formatUndefinedFunctionError(name, "Undefined function or variable: " + name));
}

Value TreeWalker::execIndexAccess(const Value &var, const ASTNode *callNode, Environment *env)
{
    size_t nargs = callNode->children.size() - 1;

    auto checkBounds = [](const std::vector<size_t> &indices, size_t limit, const char *ctx) {
        for (auto idx : indices) {
            if (idx >= limit)
                throw std::runtime_error(std::string("Index exceeds array dimensions (") + ctx
                                         + ": " + std::to_string(idx + 1) + " > "
                                         + std::to_string(limit) + ")");
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

        // ── scalar fast path: skip resolveIndex + vector<size_t> entirely ──
        size_t scalarIdx;
        if (tryResolveScalarIndex(callNode->children[1].get(), var, 0, 1, env, scalarIdx)) {
            if (scalarIdx >= var.numel())
                throw std::runtime_error("Index exceeds array dimensions (linear index: "
                                         + std::to_string(scalarIdx + 1) + " > "
                                         + std::to_string(var.numel()) + ")");
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
            size_t sri, sci;
            if (tryResolveScalarIndex(callNode->children[1].get(), var, 0, 2, env, sri)
                && tryResolveScalarIndex(callNode->children[2].get(), var, 1, 2, env, sci)) {
                if (sri >= var.dims().rows())
                    throw std::runtime_error("Index exceeds array dimensions (row index: "
                                             + std::to_string(sri + 1) + " > "
                                             + std::to_string(var.dims().rows()) + ")");
                const size_t colDim = var.dims().ndim() > 2
                    ? (var.dims().rows() > 0 ? var.dims().numel() / var.dims().rows() : var.dims().cols())
                    : var.dims().cols();
                if (sci >= colDim)
                    throw std::runtime_error("Index exceeds array dimensions (column index: "
                                             + std::to_string(sci + 1) + " > "
                                             + std::to_string(colDim) + ")");
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
        const size_t colDim = var.dims().ndim() > 2
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
        std::vector<std::vector<size_t>> idxLists(nd);
        std::vector<const size_t *> idxPtrs(nd);
        std::vector<size_t> idxCounts(nd);
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
    size_t nidx = node->children.size() - 1;

    // 1-D / 2-D brace index: gather the selected items (cellBraceContents handles colon
    // / vector / range / 2-D slice, evaluating the base once). A single selected element
    // returns the bare element (the hot c{i} scalar path); a multi-select returns a CSL
    // transient that splice contexts flatten and single-value contexts collapse (via the
    // execNode wrapper). See dev-docs/memory/csl_first_class.md.
    if (nidx == 1 || nidx == 2) {
        std::vector<Value> items = cellBraceContents(node, env);
        if (items.size() == 1)
            return std::move(items[0]);
        Value out = Value::csl(items.size());
        for (size_t i = 0; i < items.size(); ++i)
            out.cslAt(i) = std::move(items[i]);
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
        size_t r = Value::checkedScalarIndex(ridx.toScalar());
        size_t c = Value::checkedScalarIndex(cidx.toScalar());
        size_t p = Value::checkedScalarIndex(pidx.toScalar());
        return obj.cellAt(obj.dims().sub2indChecked(r, c, p));
    }
    // ND brace-cell read (nidx ≥ 4): column-major linear index, bounds-checked.
    std::vector<size_t> coords(nidx);
    for (size_t i = 0; i < nidx; ++i) {
        IndexContextGuard guard(indexContextStack_, {&obj, static_cast<int>(i),
                                                     static_cast<int>(nidx)});
        Value v = execNode(node->children[i + 1].get(), env);
        coords[i] = Value::checkedScalarIndex(v.toScalar());
    }
    const auto &d = obj.dims();
    size_t idx = 0, stride = 1;
    for (size_t i = 0; i < nidx; ++i) {
        const size_t lim = (static_cast<int>(i) < d.ndim()) ? d.dim(static_cast<int>(i)) : 1;
        if (coords[i] >= lim)
            throw std::runtime_error("Cell index out of bounds (dim "
                                     + std::to_string(i + 1)
                                     + ": " + std::to_string(coords[i] + 1)
                                     + " > " + std::to_string(lim) + ")");
        idx += coords[i] * stride;
        stride *= lim;
    }
    return obj.cellAt(idx);
}

std::vector<Value> TreeWalker::cellBraceContents(const ASTNode *node, Environment *env)
{
    // 1-D c{:} / c{vec} / c{i} -> the selected elements column-major; 2-D c{r,:} /
    // c{:,j} -> resolveIndex per dim + sub2ind, column-major (mirrors execMultiAssign).
    // 1-based, bounds-checked; `end` resolves per dim through the index-context guard.
    // The base is evaluated ONCE here. 3-D+ is refused (same message as multi-assign).
    Value obj = execNode(node->children[0].get(), env);
    if (obj.isObject()) {
        const BuiltinClass *cls = engine_.findClass(obj.objectClassName());
        if (cls && cls->methods.count("__brace_get")) {
            size_t nidx = node->children.size() - 1;
            std::vector<Value> args;
            args.reserve(nidx);
            for (size_t i = 0; i < nidx; ++i) {
                IndexContextGuard guard(indexContextStack_,
                                        {&obj, static_cast<int>(i), static_cast<int>(nidx)});
                args.push_back(execNode(node->children[i + 1].get(), env));
            }
            Value outBuf[1];
            CallContext ctx{&engine_, env};
            cls->methods.at("__brace_get")(obj, Span<const Value>(args.data(), args.size()),
                                           1, Span<Value>(outBuf, 1), ctx);
            return {std::move(outBuf[0])};
        }
    }
    if (!obj.isCell())
        throw std::runtime_error("Cell contents indexing requires a cell array");
    std::vector<Value> out;
    size_t             nidx = node->children.size() - 1;
    if (nidx == 1) {
        const ASTNode *sub = node->children[1].get();
        if (sub->type == NodeType::COLON_EXPR && sub->children.empty()) {
            out.reserve(obj.numel());
            for (size_t i = 0; i < obj.numel(); ++i)
                out.push_back(obj.cellAt(i));
        } else {
            IndexContextGuard guard(indexContextStack_, {&obj, 0, 1});
            Value             idxv = execNode(sub, env);
            for (size_t id : Value::resolveIndices(idxv, obj.numel()))
                out.push_back(obj.cellAt(id));
        }
    } else if (nidx == 2) {
        auto rowIdx = resolveIndex(node->children[1].get(), obj, 0, 2, env);
        auto colIdx = resolveIndex(node->children[2].get(), obj, 1, 2, env);
        for (size_t c : colIdx)
            for (size_t r : rowIdx)
                out.push_back(obj.cellAt(obj.dims().sub2ind(r, c)));
    } else {
        throw std::runtime_error("Cell CSL with " + std::to_string(nidx)
                                 + " indices not supported");
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
            std::string qualified = tryBuildQualifiedName(node, &root);
            if (!qualified.empty()) {
                if (const ExternalFunc *fn = engine_.findExternal(qualified, env)) {
                    Value outBuf[1];
                    CallContext ctx{&engine_, env};
                    (*fn)({}, 1, Span<Value>(outBuf, 1), ctx);
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
    if (!obj.isStruct())
        throw std::runtime_error("Dot indexing requires a struct, got "
                                 + std::string(mtypeName(obj.type())));
    if (obj.isStructArray() || obj.numel() == 0) {
        size_t n = obj.numel();
        if (n == 0 && !obj.hasField(node->strValue))
            throw std::runtime_error("Reference to non-existent field '" + node->strValue + "'");
        Value csl = Value::csl(n, engine_.mr_);
        for (size_t i = 0; i < n; ++i) {
            const auto &m = obj.structArrayElem(i);
            auto it = m.find(node->strValue);
            if (it == m.end())
                throw std::runtime_error("Reference to non-existent field '" + node->strValue + "'");
            csl.cslAt(i) = it->second;
        }
        return csl;
    }
    if (!obj.hasField(node->strValue)) {
        if (node->strValue.size() == 1 && obj.hasField("kind")) {
            char up = static_cast<char>(std::toupper(static_cast<unsigned char>(node->strValue[0])));
            std::string upField(1, up);
            if ((up == 'A' || up == 'B' || up == 'C' || up == 'D') && obj.hasField(upField))
                return obj.field(upField);
        }
        throw std::runtime_error("Reference to non-existent field '" + node->strValue + "'");
    }
    return obj.field(node->strValue);
}

Value TreeWalker::objectPropGet(const Value &obj, const std::string &name, Environment *env)
{
    const BuiltinClass *cls = engine_.findClass(obj.objectClassName());
    if (cls) {
        CallContext ctx{&engine_, env};
        if (cls->propGet) {
            Value out;
            if (cls->propGet(obj, name, out, ctx))
                return out;
        }
        // Bare `obj.method` (no parens) invokes a no-arg method (MATLAB).
        auto mit = cls->methods.find(name);
        if (mit != cls->methods.end()) {
            Value self = obj;
            Value outBuf[1];
            mit->second(self, Span<const Value>(nullptr, 0), 1, Span<Value>(outBuf, 1), ctx);
            return outBuf[0];
        }
    }
    throw std::runtime_error("No appropriate property '" + name + "' for class '"
                             + obj.objectClassName() + "'");
}

void TreeWalker::objectPropSet(Value &objSlot, const std::string &name,
                               const Value &rhs, Environment *env)
{
    const BuiltinClass *cls = engine_.findClass(objSlot.objectClassName());
    if (cls && cls->propSet) {
        CallContext ctx{&engine_, env};
        if (cls->propSet(objSlot, name, rhs, ctx))
            return;
    }
    throw std::runtime_error("Cannot set property '" + name + "' on class '"
                             + objSlot.objectClassName() + "'");
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
    std::vector<std::vector<Value>> rows;
    bool anyChar = false, allChar = true;

    auto pushElem = [&](Value &&val, std::vector<Value> &dst) {
        // Empties PARTICIPATE in concatenation (MATLAB: [zeros(0,2),
        // zeros(0,2)] is 0x4) — dropping them here lost the dims and,
        // for all-empty concats, left the assignment unbound
        // (bugs/closed/lang/vertcat-all-empty). Value::horzcat/vertcat
        // apply the probed empty rules.
        if (val.isChar()) anyChar = true;
        else              allChar = false;
        dst.push_back(std::move(val));
    };

    for (auto &rowNode : node->children) {
        std::vector<Value> rowElems;
        for (auto &elemNode : rowNode->children) {
            // CSL: `s.f` / `s.(name)` over a multi-element struct
            // array expands to one rowElem per element.
            if ((elemNode->type == NodeType::FIELD_ACCESS
                 || elemNode->type == NodeType::DYNAMIC_FIELD_ACCESS)
                && elemNode->children.size() >= 1) {
                auto base = execNode(elemNode->children[0].get(), env);
                if (base.isStruct() && (base.isStructArray() || base.numel() == 0)) {
                    std::string fname;
                    if (elemNode->type == NodeType::FIELD_ACCESS) {
                        fname = elemNode->strValue;
                    } else {
                        // DYNAMIC_FIELD_ACCESS: child[1] is the name expr.
                        fname = execNode(elemNode->children[1].get(), env).toString();
                    }
                    if (base.numel() == 0 && !base.hasField(fname))
                        throw std::runtime_error(
                            "Reference to non-existent field '" + fname + "'");
                    for (size_t i = 0; i < base.numel(); ++i) {
                        const auto &m = base.structArrayElem(i);
                        auto it = m.find(fname);
                        if (it == m.end())
                            throw std::runtime_error(
                                "Reference to non-existent field '" + fname + "'");
                        pushElem(Value(it->second), rowElems);
                    }
                    continue;
                }
                // OBJECT array CSL: [arr.prop] expands prop over each
                // element via propGet. A scalar object falls through to
                // the generic path (keeps the property-or-method fallback).
                if (base.isObject() && base.objectCount() > 1) {
                    const BuiltinClass *cls = engine_.findClass(base.objectClassName());
                    std::string fname = (elemNode->type == NodeType::FIELD_ACCESS)
                        ? elemNode->strValue
                        : execNode(elemNode->children[1].get(), env).toString();
                    CallContext ctx{&engine_, env};
                    for (size_t i = 0; i < base.objectCount(); ++i) {
                        Value elem = base.objectSubArray({i}, engine_.mr_);
                        Value out;
                        if (!cls || !cls->propGet || !cls->propGet(elem, fname, out, ctx))
                            throw std::runtime_error("No appropriate property '" + fname
                                                     + "' for class '"
                                                     + base.objectClassName() + "'");
                        pushElem(std::move(out), rowElems);
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
                for (size_t k = 0; k < val.cslCount(); ++k)
                    pushElem(std::move(val.cslAt(k)), rowElems);
            else
                pushElem(std::move(val), rowElems);
        }
        if (!rowElems.empty())
            rows.push_back(std::move(rowElems));
    }

    if (rows.empty())
        return Value();

    // All-char: MATLAB pads shorter strings with spaces in vertical stacking
    if (allChar && anyChar) {
        // horzcat each row → one string per row
        std::vector<std::string> strs;
        size_t maxCols = 0;
        for (auto &rowElems : rows) {
            std::string s;
            for (auto &v : rowElems)
                s += v.toString();
            maxCols = std::max(maxCols, s.size());
            strs.push_back(std::move(s));
        }
        if (strs.size() == 1)
            return Value::fromString(strs[0], engine_.mr_);

        // Build char matrix with space-padding
        size_t totalRows = strs.size();
        auto result = Value::matrix(totalRows, maxCols, ValueType::CHAR, engine_.mr_);
        char *dst = result.charDataMut();
        std::memset(dst, ' ', totalRows * maxCols);
        for (size_t row = 0; row < totalRows; ++row) {
            const auto &s = strs[row];
            for (size_t c = 0; c < s.size(); ++c)
                dst[c * totalRows + row] = s[c];
        }
        return result;
    }

    // Numeric / object: horzcat each row, then vertcat all rows
    auto concatWithHook = [&](const char *opName, const Value *elems, size_t count) -> Value {
        if (count == 1)
            return elems[0];
        for (size_t i = 0; i < count; ++i) {
            if (elems[i].isObject()) {
                const BuiltinClass *cls = engine_.findClass(elems[i].objectClassName());
                if (cls && cls->ops.count(opName)) {
                    Value self = elems[0];
                    Value outBuf[1];
                    CallContext ctx{&engine_, env};
                    cls->ops.at(opName)(self, Span<const Value>(elems + 1, count - 1), 1,
                                        Span<Value>(outBuf, 1), ctx);
                    return outBuf[0];
                }
                break;
            }
        }
        return (std::string(opName) == "horzcat")
                   ? Value::horzcat(elems, count, engine_.mr_)
                   : Value::vertcat(elems, count, engine_.mr_);
    };

    std::vector<Value> rowValues;
    rowValues.reserve(rows.size());
    for (auto &rowElems : rows)
        rowValues.push_back(concatWithHook("horzcat", rowElems.data(), rowElems.size()));

    if (rowValues.size() == 1)
        return std::move(rowValues[0]);

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
    auto collectRow = [&](const std::vector<ASTNodePtr> &elems) {
        std::vector<Value> row;
        for (const auto &e : elems) {
            // CSL: an element may expand to a comma-separated list (c{:} / c{idx} /
            // c{r,:}, or any CSL producer) -> splice its elements; non-CSL is one entry.
            Value v = execNodeExpand(e.get(), env);
            if (v.isCsl())
                for (size_t k = 0; k < v.cslCount(); ++k)
                    row.push_back(std::move(v.cslAt(k)));
            else
                row.push_back(std::move(v));
        }
        return row;
    };

    const bool                      is2D = node->children[0]->type == NodeType::BLOCK;
    std::vector<std::vector<Value>> rows;
    if (!is2D) {
        rows.push_back(collectRow(node->children));  // a 1-row literal of direct elements
    } else {
        for (const auto &rowNode : node->children) rows.push_back(collectRow(rowNode->children));
    }

    const size_t numRows = rows.size();
    const size_t numCols = rows.empty() ? 0 : rows[0].size();
    for (const auto &r : rows)
        if (r.size() != numCols)
            throw std::runtime_error(
                "Dimensions of cell arrays being concatenated are not consistent");

    auto cell = Value::cell(numRows, numCols);
    for (size_t r = 0; r < numRows; ++r)
        for (size_t c = 0; c < numCols; ++c)
            cell.cellAt(cell.dims().sub2ind(r, c)) = std::move(rows[r][c]);
    return cell;
}

static ValueType colonOutputType(const Value *ops, size_t n)
{
    ValueType nonDouble = ValueType::DOUBLE;
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
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
            if (cls && cls->ops.count("colon")) {
                Value self = dom;
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                cls->ops.at("colon")(self, Span<const Value>(ops, 2), 1,
                                     Span<Value>(outBuf, 1), ctx);
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
            if (cls && cls->ops.count("colon")) {
                Value self = dom;
                Value outBuf[1];
                CallContext ctx{&engine_, env};
                cls->ops.at("colon")(self, Span<const Value>(ops, 3), 1,
                                     Span<Value>(outBuf, 1), ctx);
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
    for (auto &[cond, body] : node->branches) {
        if (condTruth(execNode(cond.get(), env)))
            return execNode(body.get(), env);
    }
    if (node->elseBranch)
        return execNode(node->elseBranch.get(), env);
    return Value();
}

Value TreeWalker::execFor(const ASTNode *node, Environment *env)
{
    const std::string &varName = node->strValue;
    auto rangeVal = execNode(node->children[0].get(), env);

    // Empty range — body never executes
    if (rangeVal.isEmpty()) {
        env->set(varName, Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_));
        return Value();
    }

    const auto &dims = rangeVal.dims();
    const size_t rows = dims.rows();
    const size_t count = (rows > 0) ? (dims.numel() / rows) : 0;
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
        for (size_t c = 0; c < count; ++c) {
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
                for (size_t c2 = c + 1; c2 < count; ++c2) {
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
    for (size_t c = 0; c < count; ++c) {
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

    for (auto &[ce, body] : node->branches) {
        auto cv = execNode(ce.get(), env);
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
            for (size_t i = 0; i < a.numel(); ++i)
                if (a.elemAsDouble(i) != b.elemAsDouble(i)) return false;
            return true;
        };

        if (cv.isCell()) {
            for (size_t i = 0; i < cv.numel() && !matched; ++i)
                matched = valuesEqual(sv, cv.cellAt(i));
        } else {
            matched = valuesEqual(sv, cv);
        }

        if (matched)
            return execNode(body.get(), env);
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
    func.name = node->strValue;
    func.params = node->paramNames;
    func.returns = node->returnNames;
    func.body = std::shared_ptr<const ASTNode>(cloneNode(node->children[0].get()));
    func.closureEnv = nullptr;
    // Route to the script-lexical bucket while inside a script
    // scope — matches the compiler's rule so `clear all` in TW
    // mode behaves the same as in VM mode (local functions survive).
    if (engine_.inScriptScope()) {
        engine_.scriptLocalUserFuncs_[func.name] = std::move(func);
    } else {
        if (auto *cp = engine_.compilerPtr())
            cp->eraseCompiledFunc(func.name);
        engine_.userFuncs_[func.name] = std::move(func);
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
    std::string displayName;
    if (child->type == NodeType::IDENTIFIER) {
        const std::string &name = child->strValue;
        const Value *found = env->get(name);
        const bool isUnshadowedConst =
            found && engine_.constantsEnv_ && found == engine_.constantsEnv_->getLocal(name);
        if (found && !isUnshadowedConst) {
            bareUserVar = true;
            displayName = name;
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
        for (size_t k = 0; k < val.cslCount(); ++k) {
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
    const std::string &name = node->strValue;

    // MATLAB (probed R2025b): a variable-headed glued -/ form (`N-1`,
    // `x -1`, `a/2` — the `which -all` idiom shape) is the binary
    // expression when the head is a VARIABLE.
    if (node->children.size() == 1
        && node->children[0]->type == NodeType::STRING_LITERAL) {
        const std::string &arg = node->children[0]->strValue;
        const Value *cand = env->get(name);
        const bool isUnshadowedConst =
            cand && engine_.constantsEnv_ && cand == engine_.constantsEnv_->getLocal(name);
        const Value *lhs = (arg.size() >= 2 && (arg[0] == '-' || arg[0] == '/')
                            && (std::isalnum(static_cast<unsigned char>(arg[1]))
                                || arg[1] == '.')
                            && !isUnshadowedConst)
                               ? cand : nullptr;
        if (lhs) {
            const std::string tail = arg.substr(1);
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
                    if (!rv)
                        throw std::runtime_error(
                            "Undefined function or variable '" + tail + "'");
                }
                // Reuse the expression evaluator: rebuild a tiny
                // BINARY_OP AST and exec it (same path as parsed code).
                auto bin = std::make_unique<ASTNode>(NodeType::BINARY_OP);
                bin->strValue = (arg[0] == '-') ? "-" : "/";
                auto l = std::make_unique<ASTNode>(NodeType::IDENTIFIER);
                l->strValue = name;
                auto r = std::make_unique<ASTNode>(
                    isNum ? NodeType::NUMBER_LITERAL : NodeType::IDENTIFIER);
                if (isNum) r->numValue = parsedNum;
                else r->strValue = tail;
                bin->children.push_back(std::move(l));
                bin->children.push_back(std::move(r));
                Value result = execBinaryOp(bin.get(), env);
                env->set("ans", result);
                if (!node->suppressOutput) displayValue("ans", result);
                return result;
            }
        }
    }

    std::vector<Value> args;
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
        (*fn)(args, 0, Span<Value>(outBuf, 1), ctx);
        result = outBuf[0];
        if (!node->suppressOutput && !result.isEmpty()) {
            env->set("ans", result);
            displayValue("ans", result);
        }
        return result;
    }

    throw std::runtime_error(
        engine_.formatUndefinedFunctionError(name, "Undefined function: " + name));
}

// ============================================================
Value TreeWalker::execAnonFunc(const ASTNode *node, Environment *env)
{
    if (!node->strValue.empty() && node->children.empty()) {
        if (env) {
            if (const UserFunction *nf = env->findNestedFunc(node->strValue)) {
                int nid = engine_.anonNameCounter_.fetch_add(1, std::memory_order_relaxed);
                std::string nestedHandleId = "__nested_fh_" + std::to_string(nid);
                UserFunction escaped = *nf;
                if (!escaped.closureEnv)
                    escaped.closureEnv = escaped.weakClosureEnv.lock();
                std::string dispSrc = escaped.name;
                std::replace(dispSrc.begin(), dispSrc.end(), '>', '/');
                engine_.userFuncs_[nestedHandleId] = std::move(escaped);
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
    std::string anonName = "__anon_" + std::to_string(id);

    UserFunction uf;
    uf.name = anonName;
    uf.params = node->paramNames;

    const ASTNode *bodyExpr = node->children[0].get();
    bool forwardVarargout =
        bodyExpr->type == NodeType::CALL
        && !bodyExpr->children.empty()
        && bodyExpr->children[0]->type == NodeType::IDENTIFIER
        && bodyExpr->children[0]->strValue != "__numkit_fwd_call__";
    if (forwardVarargout) {
        const std::string &callee = bodyExpr->children[0]->strValue;
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

    auto bodyBlock = std::make_shared<ASTNode>(NodeType::BLOCK);
    auto assignNode = std::make_unique<ASTNode>(NodeType::ASSIGN);
    assignNode->suppressOutput = true;

    if (forwardVarargout) {
        uf.returns = {"varargout"};
        auto vaId = std::make_unique<ASTNode>(NodeType::IDENTIFIER);
        vaId->strValue = "varargout";
        assignNode->children.push_back(std::move(vaId));
        auto fwdCall = std::make_unique<ASTNode>(NodeType::CALL);
        auto fwdId = std::make_unique<ASTNode>(NodeType::IDENTIFIER);
        fwdId->strValue = "__numkit_fwd_call__";
        fwdCall->children.push_back(std::move(fwdId));
        auto nargoutId = std::make_unique<ASTNode>(NodeType::IDENTIFIER);
        nargoutId->strValue = "nargout";
        fwdCall->children.push_back(std::move(nargoutId));
        auto fnameStr = std::make_unique<ASTNode>(NodeType::STRING_LITERAL);
        fnameStr->strValue = bodyExpr->children[0]->strValue;
        fwdCall->children.push_back(std::move(fnameStr));
        for (size_t i = 1; i < bodyExpr->children.size(); ++i)
            fwdCall->children.push_back(cloneNode(bodyExpr->children[i].get()));
        assignNode->children.push_back(std::move(fwdCall));
    } else {
        uf.returns = {"__result__"};
        auto resultId = std::make_unique<ASTNode>(NodeType::IDENTIFIER);
        resultId->strValue = "__result__";
        assignNode->children.push_back(std::move(resultId));
        assignNode->children.push_back(cloneNode(bodyExpr));
    }

    bodyBlock->children.push_back(std::move(assignNode));
    uf.body = std::move(bodyBlock);

    std::vector<std::string> capturedNames;
    std::vector<Value> capturedVals;
    std::shared_ptr<Environment> anonNestedEnv;
    auto bindNestedRef = [&](const std::string &nm) {
        if (!env || nm.empty())
            return;
        if (const UserFunction *nf = env->findNestedFunc(nm)) {
            if (!anonNestedEnv) {
                anonNestedEnv = std::make_shared<Environment>(
                    &engine_.constantsEnv(), engine_.globalsEnv_.get());
            }
            if (!anonNestedEnv->findNestedFunc(nm)) {
                UserFunction escaped = *nf;
                if (!escaped.closureEnv)
                    escaped.closureEnv = escaped.weakClosureEnv.lock();
                anonNestedEnv->defineNestedFunc(nm, std::move(escaped));
            }
        }
    };
    auto collectFree = [&](auto &self, const ASTNode *cur,
                           const std::vector<std::string> &bound) -> void {
        if (!cur)
            return;
        if (cur->type == NodeType::IDENTIFIER) {
            const std::string &nm = cur->strValue;
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
            capturedNames.push_back(nm);
            capturedVals.push_back(*valPtr);
            return;
        }
        if (cur->type == NodeType::ANON_FUNC) {
            if (!cur->strValue.empty() && cur->children.empty()) {
                bindNestedRef(cur->strValue);
                return;
            }
            std::vector<std::string> merged = bound;
            merged.insert(merged.end(), cur->paramNames.begin(), cur->paramNames.end());
            for (const auto &ch : cur->children)
                self(self, ch.get(), merged);
            return;
        }
        for (const auto &ch : cur->children)
            self(self, ch.get(), bound);
        for (const auto &[a, b] : cur->branches) {
            self(self, a.get(), bound);
            self(self, b.get(), bound);
        }
        if (cur->elseBranch)
            self(self, cur->elseBranch.get(), bound);
    };
    collectFree(collectFree, bodyExpr, node->paramNames);
    for (const auto &cn : capturedNames)
        uf.params.push_back(cn);
    uf.closureEnv = std::move(anonNestedEnv);
    // Provenance is fixed at creation (reference rule for anonymous
    // functions): a handle born in a script/file keeps THAT file, one born
    // at the command window keeps '' — masking any caller's file later.
    engine_.captureScriptOrigin(uf);

    engine_.userFuncs_[anonName] = std::move(uf);
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
                std::string id = mle.identifier().empty() ? "numkit:error" : mle.identifier();
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

void twCollectDirectNestedDefs(const ASTNode *node, std::vector<const ASTNode *> &out)
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
    for (const auto &[a, b] : node->branches) {
        twCollectDirectNestedDefs(a.get(), out);
        twCollectDirectNestedDefs(b.get(), out);
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
                       std::unordered_set<std::string> &assigned,
                       std::unordered_set<std::string> &identifiers,
                       std::unordered_set<std::string> &ownGlobPers)
{
    if (!node || node->type == NodeType::FUNCTION_DEF || node->type == NodeType::CLASSDEF_DEF)
        return;
    switch (node->type) {
    case NodeType::IDENTIFIER:
        if (!node->strValue.empty())
            identifiers.insert(node->strValue);
        break;
    case NodeType::ASSIGN:
    case NodeType::DELETE_ASSIGN:
        if (!node->children.empty()) {
            if (const ASTNode *id = twRootTargetId(node->children[0].get()))
                assigned.insert(id->strValue);
        }
        break;
    case NodeType::MULTI_ASSIGN:
        for (const auto &rn : node->returnNames)
            if (!rn.empty() && rn != "~")
                assigned.insert(rn);
        for (const auto &lt : node->lhsTargets) {
            if (const ASTNode *id = twRootTargetId(lt.get()))
                assigned.insert(id->strValue);
        }
        break;
    case NodeType::FOR_STMT:
        if (!node->strValue.empty())
            assigned.insert(node->strValue);
        break;
    case NodeType::TRY_STMT:
        if (!node->strValue.empty() && node->children.size() > 1)
            assigned.insert(node->strValue);
        break;
    case NodeType::GLOBAL_STMT:
    case NodeType::PERSISTENT_STMT:
        for (const auto &nm : node->paramNames)
            ownGlobPers.insert(nm);
        break;
    default:
        break;
    }
    for (const auto &lt : node->lhsTargets)
        twScanBodyLexical(lt.get(), assigned, identifiers, ownGlobPers);
    for (const auto &ch : node->children)
        twScanBodyLexical(ch.get(), assigned, identifiers, ownGlobPers);
    for (const auto &[a, b] : node->branches) {
        twScanBodyLexical(a.get(), assigned, identifiers, ownGlobPers);
        twScanBodyLexical(b.get(), assigned, identifiers, ownGlobPers);
    }
    if (node->elseBranch)
        twScanBodyLexical(node->elseBranch.get(), assigned, identifiers, ownGlobPers);
}

void twCollectDescendantAssigned(const ASTNode *nestedFn,
                                 std::unordered_set<std::string> &descAssigned)
{
    if (!nestedFn || nestedFn->children.empty())
        return;
    const ASTNode *nb = nestedFn->children[0].get();
    std::unordered_set<std::string> localAssigned;
    std::unordered_set<std::string> localIds;
    std::unordered_set<std::string> localGP;
    twScanBodyLexical(nb, localAssigned, localIds, localGP);
    std::unordered_set<std::string> ownShadow = localGP;
    for (const auto &p : nestedFn->paramNames) ownShadow.insert(p);
    for (const auto &r : nestedFn->returnNames) ownShadow.insert(r);
    for (const auto &a : localAssigned)
        if (!ownShadow.count(a))
            descAssigned.insert(a);

    std::vector<const ASTNode *> deeper;
    twCollectDirectNestedDefs(nb, deeper);
    for (const ASTNode *d : deeper) {
        std::unordered_set<std::string> sub;
        twCollectDescendantAssigned(d, sub);
        for (const auto &a : sub)
            if (!ownShadow.count(a))
                descAssigned.insert(a);
    }
}

bool twAncestorOwnsVar(const Environment *parentEnv, const std::string &nm)
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
                          const std::shared_ptr<Environment> &heapEnv,
                          const std::vector<const ASTNode *> &directNested,
                          const std::shared_ptr<Environment> &resolvedClosure)
{
    heapEnv->markLexicalScope(true);
    std::unordered_set<std::string> assigned;
    std::unordered_set<std::string> identifiers;
    std::unordered_set<std::string> ownGlobPers;
    twScanBodyLexical(func.body.get(), assigned, identifiers, ownGlobPers);

    for (const auto &p : func.params) heapEnv->declareLexicalVar(p);
    for (const auto &r : func.returns) heapEnv->declareLexicalVar(r);
    heapEnv->declareLexicalVar("nargin");
    heapEnv->declareLexicalVar("nargout");

    std::unordered_set<std::string> descAssigned;
    for (const ASTNode *nd : directNested)
        twCollectDescendantAssigned(nd, descAssigned);

    auto maybeDeclareOwn = [&](const std::string &nm) {
        if (nm.empty() || nm == "~" || ownGlobPers.count(nm))
            return;
        if (!twAncestorOwnsVar(resolvedClosure.get(), nm))
            heapEnv->declareLexicalVar(nm);
    };
    for (const auto &a : assigned)
        maybeDeclareOwn(a);
    for (const auto &id : identifiers)
        if (descAssigned.count(id))
            maybeDeclareOwn(id);

    for (const ASTNode *nd : directNested) {
        if (!nd || nd->children.empty())
            continue;
        UserFunction nuf;
        nuf.name = func.name + ">" + nd->strValue;
        nuf.params = nd->paramNames;
        nuf.returns = nd->returnNames;
        nuf.body = std::shared_ptr<const ASTNode>(cloneNode(nd->children[0].get()));
        nuf.weakClosureEnv = heapEnv;
        nuf.originKnown = func.originKnown;
        nuf.sourceFs = func.sourceFs;
        nuf.sourceDir = func.sourceDir;
        nuf.sourceFile = func.sourceFile;
        heapEnv->defineNestedFunc(nd->strValue, std::move(nuf));
    }
}

} // namespace

// Extract bare-identifier arg names from a CALL AST node for inputname(k).
// Returns empty vector if callNode is null. Reserved names (pi, eps, …)
// are treated as non-identifier (returned as empty string).
static std::vector<std::string> extractCallerArgNames(const ASTNode *callNode,
                                                       const Engine &engine)
{
    std::vector<std::string> names;
    if (!callNode || callNode->children.size() < 2) return names;
    names.reserve(callNode->children.size() - 1);
    bool anyIdent = false;
    for (size_t i = 1; i < callNode->children.size(); ++i) {
        const auto &child = callNode->children[i];
        if (child && child->type == NodeType::IDENTIFIER
            && !engine.isReservedName(child->strValue)) {
            names.push_back(child->strValue);
            anyIdent = true;
        } else {
            names.emplace_back();
        }
    }
    if (!anyIdent) names.clear();
    return names;
}

Value TreeWalker::callUserFunction(const UserFunction &func,
                                    Span<const Value> args,
                                    Environment *env,
                                    const ASTNode *callNode)
{
    (void) env;
    RecursionGuard rguard(currentRecursionDepth_, maxRecursionDepth_);

    // varargin: the LAST declared parameter absorbs any extra arguments
    // (packed into a 1xN cell; empty 0x0 cell when there are none).
    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const size_t required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size())
        throw std::runtime_error("Too many input arguments for function '" + func.name + "'");

    std::shared_ptr<Environment> resolvedClosure =
        func.closureEnv ? func.closureEnv : func.weakClosureEnv.lock();
    std::vector<const ASTNode *> directNested;
    twCollectDirectNestedDefs(func.body.get(), directNested);
    const bool useLexicalHeap =
        !directNested.empty() || (resolvedClosure && resolvedClosure->isLexicalScope());

    std::optional<Environment> stackEnv;
    std::shared_ptr<Environment> heapEnv;
    if (useLexicalHeap) {
        heapEnv = resolvedClosure
                      ? std::make_shared<Environment>(resolvedClosure, engine_.globalsEnv_.get())
                      : std::make_shared<Environment>(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    } else if (resolvedClosure) {
        stackEnv.emplace(resolvedClosure, engine_.globalsEnv_.get());
    } else {
        stackEnv.emplace(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    }
    Environment &localEnv = useLexicalHeap ? *heapEnv : *stackEnv;
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv,
                          extractCallerArgNames(callNode, engine_));

    for (size_t i = 0; i < required && i < args.size(); ++i)
        localEnv.setLocal(func.params[i], args[i]);
    if (va) {
        Value vaCell = args.size() > required
                           ? Value::cell(1, args.size() - required, engine_.mr_)
                           : Value::cell(0, 0, engine_.mr_);
        for (size_t i = required; i < args.size(); ++i)
            vaCell.cellAt(i - required) = args[i];
        localEnv.setLocal("varargin", std::move(vaCell));
    }

    func.usesNarginNargout = 1;
    if (func.usesNarginNargout) {
        size_t nout = std::max(func.returns.size(), size_t(1));
        localEnv.setLocal("nargin",
                          Value::scalar(static_cast<double>(args.size()), engine_.mr_));
        localEnv.setLocal("nargout", Value::scalar(static_cast<double>(nout), engine_.mr_));
    }

    std::optional<DebugController::FrameGuard> dbgFrame;
    if (auto *ctl = debugCtl()) {
        StackFrame frame;
        frame.functionName = func.name;
        frame.env = &localEnv;
        dbgFrame.emplace(*ctl, std::move(frame));
    }

    std::optional<FsContext::OriginFrame> originGuard;
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

    const std::string &r0 = func.returns[0];
    if (!useLexicalHeap && !localEnv.isGlobal(r0) && !localEnv.isPersistent(r0)) {
        if (auto *localVal = localEnv.getLocal(r0))
            return std::move(*localVal);
    }
    auto *val = localEnv.get(r0);
    return val ? *val : Value();
}

std::vector<Value> TreeWalker::callUserFunctionMulti(const UserFunction &func,
                                                      Span<const Value> args,
                                                      Environment *env,
                                                      size_t nout,
                                                      const ASTNode *callNode)
{
    (void) env;
    RecursionGuard rguard(currentRecursionDepth_, maxRecursionDepth_);

    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const size_t required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size())
        throw std::runtime_error("Too many input arguments for function '" + func.name + "'");

    std::shared_ptr<Environment> resolvedClosure =
        func.closureEnv ? func.closureEnv : func.weakClosureEnv.lock();
    std::vector<const ASTNode *> directNested;
    twCollectDirectNestedDefs(func.body.get(), directNested);
    const bool useLexicalHeap =
        !directNested.empty() || (resolvedClosure && resolvedClosure->isLexicalScope());

    std::optional<Environment> stackEnv;
    std::shared_ptr<Environment> heapEnv;
    if (useLexicalHeap) {
        heapEnv = resolvedClosure
                      ? std::make_shared<Environment>(resolvedClosure, engine_.globalsEnv_.get())
                      : std::make_shared<Environment>(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    } else if (resolvedClosure) {
        stackEnv.emplace(resolvedClosure, engine_.globalsEnv_.get());
    } else {
        stackEnv.emplace(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    }
    Environment &localEnv = useLexicalHeap ? *heapEnv : *stackEnv;
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv,
                          extractCallerArgNames(callNode, engine_));

    for (size_t i = 0; i < required && i < args.size(); ++i)
        localEnv.setLocal(func.params[i], args[i]);
    if (va) {
        Value vaCell = args.size() > required
                           ? Value::cell(1, args.size() - required, engine_.mr_)
                           : Value::cell(0, 0, engine_.mr_);
        for (size_t i = required; i < args.size(); ++i)
            vaCell.cellAt(i - required) = args[i];
        localEnv.setLocal("varargin", std::move(vaCell));
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

    std::optional<DebugController::FrameGuard> dbgFrame;
    if (auto *ctl = debugCtl()) {
        StackFrame frame;
        frame.functionName = func.name;
        frame.env = &localEnv;
        dbgFrame.emplace(*ctl, std::move(frame));
    }

    std::optional<FsContext::OriginFrame> originGuard;
    if (func.originKnown)
        originGuard.emplace(engine_.fsContext(), func.sourceFs,
                            func.sourceDir, func.sourceFile);

    execNode(func.body.get(), &localEnv);

    dbgFrame.reset();

    if (flowSignal_ == FlowSignal::RETURN)
        flowSignal_ = FlowSignal::NONE;

    auto readRet = [&](const std::string &rname) -> Value {
        if (!useLexicalHeap && !localEnv.isGlobal(rname) && !localEnv.isPersistent(rname)) {
            if (auto *lv = localEnv.getLocal(rname))
                return std::move(*lv);
        }
        auto *val = localEnv.get(rname);
        return val ? *val : Value();
    };

    const bool hasVarargout = !func.returns.empty() && func.returns.back() == "varargout";
    std::vector<Value> results;
    if (hasVarargout) {
        const size_t numFixed = func.returns.size() - 1;
        results.reserve(nout);
        for (size_t i = 0; i < numFixed && results.size() < nout; ++i)
            results.push_back(readRet(func.returns[i]));
        auto *vaVal = localEnv.getLocal("varargout");
        if (!vaVal)
            vaVal = localEnv.get("varargout");
        if (vaVal && vaVal->isCell()) {
            const size_t n = vaVal->numel();
            for (size_t k = 0; k < n && results.size() < nout; ++k)
                results.push_back(vaVal->cellAt(k));
        }
        if (results.size() < nout)
            throw std::runtime_error("Too many output arguments.");
        return results;
    }

    if (func.returns.size() < nout)
        throw std::runtime_error("Too many output arguments.");
    results.reserve(std::min(func.returns.size(), nout));
    for (size_t i = 0; i < func.returns.size() && i < nout; ++i)
        results.push_back(readRet(func.returns[i]));
    return results;
}

// ============================================================
// classdef method / constructor invocation
// ============================================================
std::vector<Value> TreeWalker::runClassMethod(const UserFunction &uf,
                                              Span<const Value> args, size_t nout)
{
    return callUserFunctionMulti(uf, args, engine_.workspaceEnv_.get(), nout, nullptr);
}

Value TreeWalker::evalExpressionPublic(const ASTNode *expr, Environment *env)
{
    return execNode(expr, env ? env : engine_.workspaceEnv_.get());
}

Value TreeWalker::runClassCtor(const UserFunction &func, const Value &seed,
                               Span<const Value> args)
{
    RecursionGuard rguard(currentRecursionDepth_, maxRecursionDepth_);
    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const size_t required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size())
        throw std::runtime_error("Too many input arguments to constructor '" + func.name
                                 + "'");

    std::shared_ptr<Environment> resolvedClosure =
        func.closureEnv ? func.closureEnv : func.weakClosureEnv.lock();
    std::vector<const ASTNode *> directNested;
    twCollectDirectNestedDefs(func.body.get(), directNested);
    const bool useLexicalHeap =
        !directNested.empty() || (resolvedClosure && resolvedClosure->isLexicalScope());

    std::optional<Environment> stackEnv;
    std::shared_ptr<Environment> heapEnv;
    if (useLexicalHeap) {
        heapEnv = resolvedClosure
                      ? std::make_shared<Environment>(resolvedClosure, engine_.globalsEnv_.get())
                      : std::make_shared<Environment>(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    } else if (resolvedClosure) {
        stackEnv.emplace(resolvedClosure, engine_.globalsEnv_.get());
    } else {
        stackEnv.emplace(&engine_.constantsEnv(), engine_.globalsEnv_.get());
    }
    Environment &localEnv = useLexicalHeap ? *heapEnv : *stackEnv;
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv, {});

    for (size_t i = 0; i < required && i < args.size(); ++i)
        localEnv.setLocal(func.params[i], args[i]);
    if (va) {
        Value vaCell = args.size() > required
                           ? Value::cell(1, args.size() - required, engine_.mr_)
                           : Value::cell(0, 0, engine_.mr_);
        for (size_t i = required; i < args.size(); ++i)
            vaCell.cellAt(i - required) = args[i];
        localEnv.setLocal("varargin", std::move(vaCell));
    }
    // MATLAB seeds the constructor's output variable with a default instance.
    if (!func.returns.empty())
        localEnv.setLocal(func.returns[0], seed);
    localEnv.setLocal("nargin", Value::scalar(static_cast<double>(args.size()), engine_.mr_));
    localEnv.setLocal("nargout", Value::scalar(1.0, engine_.mr_));

    std::optional<FsContext::OriginFrame> originGuard;
    if (func.originKnown)
        originGuard.emplace(engine_.fsContext(), func.sourceFs,
                            func.sourceDir, func.sourceFile);

    execNode(func.body.get(), &localEnv);
    if (flowSignal_ == FlowSignal::RETURN)
        flowSignal_ = FlowSignal::NONE;

    if (func.returns.empty())
        return seed;
    const std::string &r0 = func.returns[0];
    if (!useLexicalHeap && !localEnv.isGlobal(r0) && !localEnv.isPersistent(r0)) {
        if (auto *localVal = localEnv.getLocal(r0))
            return std::move(*localVal);
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
