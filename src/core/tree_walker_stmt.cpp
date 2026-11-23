// src/core/tree_walker_stmt.cpp
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
#include "scope_analysis.hpp"
#include <numkit/value/error.hpp>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/value/index.hpp>
#include "tree_walker_internal.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace numkit {

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
        const keel::f64 *src = rangeVal.doubleData();
        env->set(varName, Value::scalar(0.0, engine_.mr_));
        Value *varPtr = env->get(varName);
        keel::f64 *slot = varPtr->doubleDataMut();
        for (keel::usize c = 0; c < count; ++c) {
            engine_.checkCancelled();
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
                    engine_.checkCancelled();
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
        engine_.checkCancelled();
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
        engine_.checkCancelled();
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
            if (a.isChar() && b.isChar()) return a.asStringRef() == b.asStringRef();
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
        engine_.scriptLocalUserFuncs_.put(keel::move(key),
                                          keel::make_arc<UserFunction>(keel::move(func)));
    } else {
        if (auto *cp = engine_.compilerPtr())
            cp->eraseCompiledFunc(func.name);
        keel::String key = func.name.clone();
        engine_.userFuncs_.put(keel::move(key),
                               keel::make_arc<UserFunction>(keel::move(func)));
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

    // Statement context: dispatch CALL and IDENTIFIER with nargout=0,
    // or nargout=1 when result is captured by eval/evalin.
    const keel::usize stmtNargout = node->captureResult ? 1 : 0;
    Value val;
    if (child->type == NodeType::CALL)
        val = execCall(child, env, stmtNargout);
    else if (child->type == NodeType::IDENTIFIER)
        val = execIdentifier(child, env, stmtNargout);
    else
        val = execNodeExpand(child, env);

    if (node->captureResult)
        return val;

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

    // Anonymous value — bind to `ans` regardless of semicolon; leave `ans`
    // untouched when a void call produced no output value.
    if (!val.isUnset() && !val.isDeleted()) {
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
                            && (keel::is_ascii_alnum(arg[1])
                                || arg[1] == '.')
                            && !isUnshadowedConst)
                               ? cand : nullptr;
        if (lhs) {
            const keel::String tail = keel::String::from(arg.substr(1));
            char *endp = nullptr;
            const keel::f64 parsedNum = std::strtod(tail.c_str(), &endp);
            const bool isNum = !tail.empty() && endp == tail.c_str() + tail.size();
            bool isIdent = !tail.empty()
                           && (keel::is_ascii_alpha(tail[0])
                               || tail[0] == '_');
            if (isIdent)
                for (char c : tail)
                    if (!(keel::is_ascii_alnum(c) || c == '_'))
                        { isIdent = false; break; }
            if (isNum || isIdent) {
                if (!isNum) {
                    const Value *rv = env->get(tail);
                    if (!rv) {
                        auto msg = keel::format("Undefined function or variable '{}'", tail);
                        throw numkit::Error(msg.c_str());
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
        (*fn)(args, node->captureResult ? 1 : 0, keel::MutArrayRef<Value>(outBuf, 1), ctx);
        result = outBuf[0];
        if (!node->suppressOutput && !result.isEmpty()) {
            env->set("ans", result);
            displayValue("ans", result);
        }
        return result;
    }

    auto msg = engine_.formatUndefinedFunctionError(
        name, keel::format("Undefined function: {}", name));
    throw numkit::Error(msg.c_str());
}

// ============================================================
Value TreeWalker::execAnonFunc(const ASTNode *node, Environment *env)
{
    if (!node->strValue.empty() && node->children.empty()) {
        if (env) {
            if (const UserFunction *nf = env->findNestedFunc(node->strValue)) {
                keel::i32 nid = engine_.anonNameCounter_.fetch_add(1, keel::MemoryOrder::Relaxed);
                keel::String nestedHandleId = keel::format("__nested_fh_{}", nid);
                UserFunction escaped = nf->clone();
                if (!escaped.closureEnv)
                    escaped.closureEnv = resolveClosureEnv(escaped);
                keel::String dispSrc = escaped.name.clone();
                std::replace(dispSrc.begin(), dispSrc.end(), '>', '/');
                engine_.userFuncs_.put(nestedHandleId.clone(),
                                       keel::make_arc<UserFunction>(keel::move(escaped)));
                Value h = Value::funcHandle(nestedHandleId, engine_.mr_);
                h.setFuncHandleSource(dispSrc);
                return h;
            }
            if (const keel::StringRef sub = env->findFileSubfunc(node->strValue); !sub.empty()) {
                Value h = Value::funcHandle(sub, engine_.mr_);
                h.setFuncHandleSource(node->strValue);
                return h;
            }
        }
        return Value::funcHandle(node->strValue, engine_.mr_);
    }

    // Engine-wide counter (shared with the VM compiler) — see the comment
    // on Engine::anonNameCounter_: a TreeWalker-executed literal must never
    // mint an id a compiled chunk already owns.
    keel::i32 id = engine_.anonNameCounter_.fetch_add(1, keel::MemoryOrder::Relaxed);
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

    engine_.userFuncs_.put(anonName.clone(), keel::make_arc<UserFunction>(keel::move(uf)));
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
        if (mle.identifier() == "numkit:interrupted")
            throw;
        auto stack = engine_.takeLastErrorStack();
        if (stack.empty())
            stack = captureCallStack();
        if (node->children.size() > 1) {
            if (!node->strValue.empty()) {
                keel::StringRef id = mle.identifier().empty()
                                         ? keel::StringRef("numkit:error")
                                         : mle.identifier().as_ref();
                Value err = makeExceptionStruct(mle.what(), id, stack.as_ref(), engine_.mr_);
                env->set(node->strValue, keel::move(err));
            }
            return execNode(node->children[1].get(), env);
        }
        return Value();
    } catch (const std::exception &e) {
        auto stack = engine_.takeLastErrorStack();
        if (stack.empty())
            stack = captureCallStack();
        if (node->children.size() > 1) {
            if (!node->strValue.empty()) {
                Value err = makeExceptionStruct(e.what(), "numkit:error", stack.as_ref(), engine_.mr_);
                env->set(node->strValue, keel::move(err));
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
            throw numkit::Error("persistent declarations are only allowed inside functions");
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

void twCollectDescendantAssigned(const ASTNode *nestedFn,
                                 keel::HashSet<keel::String> &descAssigned)
{
    if (!nestedFn || nestedFn->children.empty())
        return;
    const ASTNode *nb = nestedFn->children[0].get();
    keel::HashSet<keel::String> localAssigned;
    keel::HashSet<keel::String> localIds;
    keel::HashSet<keel::String> localGP;
    scope::scanBodyLexical(nb, localAssigned, localIds, localGP);
    keel::HashSet<keel::String> ownShadow = localGP.clone();
    for (const auto &p : nestedFn->paramNames) ownShadow.insert(p.clone());
    for (const auto &r : nestedFn->returnNames) ownShadow.insert(r.clone());
    for (const auto &a : localAssigned)
        if (!ownShadow.contains(a))
            descAssigned.insert(a.clone());

    keel::Array<const ASTNode *> deeper;
    scope::collectDirectNestedDefs(nb, deeper);
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
    scope::scanBodyLexical(func.body.get(), assigned, identifiers, ownGlobPers);

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

Value TreeWalker::callUserFunction(const UserFunction &funcIn,
                                    keel::ArrayRef<Value> args,
                                    Environment *env,
                                    const ASTNode *callNode)
{
    (void) env;
    // Pin the active UserFunction Arc (when owned by Engine) so `clear functions`
    // or `rehash` during the body cannot free `func` mid-call.
    keel::Arc<UserFunction> pinnedFunc = engine_.lookupUserFunctionLocalArc(funcIn.name);
    const UserFunction &func = (pinnedFunc && pinnedFunc.get() == &funcIn) ? *pinnedFunc : funcIn;
    engine_.checkCancelled();
    RecursionGuard rguard(currentRecursionDepth_, engine_.maxRecursionDepth());

    // varargin: the LAST declared parameter absorbs any extra arguments
    // (packed into a 1xN cell; empty 0x0 cell when there are none).
    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const keel::usize required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size()) {
        auto msg = keel::format("Too many input arguments for function '{}'", func.name);
        throw numkit::Error(msg.c_str());
    }

    keel::Arc<Environment> resolvedClosure = resolveClosureEnv(func);
    keel::Array<const ASTNode *> directNested;
    scope::collectDirectNestedDefs(func.body.get(), directNested);
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
    if (func.fileSubfuncs)
        localEnv.setFileSubfuncs(func.fileSubfuncs);
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv,
                          extractCallerArgNames(callNode, engine_));
    const keel::String *curSf = engine_.currentScriptFile();
    keel::StringRef traceFile = func.originKnown
                                    ? func.sourceFile.as_ref()
                                    : (curSf ? curSf->as_ref() : keel::StringRef{});
    TraceGuard traceGuard(callTrace_, traceFile, func.name.as_ref(),
                          func.body ? func.body->line : 0);

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
                          Value::scalar(keel::as<keel::f64>(args.size()), engine_.mr_));
        localEnv.setLocal("nargout", Value::scalar(keel::as<keel::f64>(nout), engine_.mr_));
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

keel::Array<Value> TreeWalker::callUserFunctionMulti(const UserFunction &funcIn,
                                                      keel::ArrayRef<Value> args,
                                                      Environment *env,
                                                      keel::usize nout,
                                                      const ASTNode *callNode)
{
    (void) env;
    keel::Arc<UserFunction> pinnedFunc = engine_.lookupUserFunctionLocalArc(funcIn.name);
    const UserFunction &func = (pinnedFunc && pinnedFunc.get() == &funcIn) ? *pinnedFunc : funcIn;
    engine_.checkCancelled();
    RecursionGuard rguard(currentRecursionDepth_, engine_.maxRecursionDepth());

    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const keel::usize required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size()) {
        auto msg = keel::format("Too many input arguments for function '{}'", func.name);
        throw numkit::Error(msg.c_str());
    }

    keel::Arc<Environment> resolvedClosure = resolveClosureEnv(func);
    keel::Array<const ASTNode *> directNested;
    scope::collectDirectNestedDefs(func.body.get(), directNested);
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
    if (func.fileSubfuncs)
        localEnv.setFileSubfuncs(func.fileSubfuncs);
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv,
                          extractCallerArgNames(callNode, engine_));
    const keel::String *curSf = engine_.currentScriptFile();
    keel::StringRef traceFile = func.originKnown
                                    ? func.sourceFile.as_ref()
                                    : (curSf ? curSf->as_ref() : keel::StringRef{});
    TraceGuard traceGuard(callTrace_, traceFile, func.name.as_ref(),
                          func.body ? func.body->line : 0);

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
                          Value::scalar(keel::as<keel::f64>(args.size()), engine_.mr_));
        localEnv.setLocal("nargout", Value::scalar(keel::as<keel::f64>(nout), engine_.mr_));
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
            throw numkit::Error("Too many output arguments.");
        return results;
    }

    if (func.returns.size() < nout)
        throw numkit::Error("Too many output arguments.");
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
    engine_.checkCancelled();
    RecursionGuard rguard(currentRecursionDepth_, engine_.maxRecursionDepth());
    const bool va = !func.params.empty() && func.params.back() == "varargin";
    const keel::usize required = va ? func.params.size() - 1 : func.params.size();
    if (!va && args.size() > func.params.size()) {
        auto msg = keel::format("Too many input arguments to constructor '{}'", func.name);
        throw numkit::Error(msg.c_str());
    }

    keel::Arc<Environment> resolvedClosure = resolveClosureEnv(func);
    keel::Array<const ASTNode *> directNested;
    scope::collectDirectNestedDefs(func.body.get(), directNested);
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
    if (func.fileSubfuncs)
        localEnv.setFileSubfuncs(func.fileSubfuncs);
    localEnv.bindPersistentBucket(engine_.persistentBucketPtr(func.name));
    if (useLexicalHeap)
        twPopulateLexicalEnv(func, heapEnv, directNested, resolvedClosure);

    FrameGuard frameGuard(activeFrames_, &localEnv, {});
    const keel::String *curSf = engine_.currentScriptFile();
    keel::StringRef traceFile = func.originKnown
                                    ? func.sourceFile.as_ref()
                                    : (curSf ? curSf->as_ref() : keel::StringRef{});
    TraceGuard traceGuard(callTrace_, traceFile, func.name.as_ref(),
                          func.body ? func.body->line : 0);

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
    localEnv.setLocal("nargin", Value::scalar(keel::as<keel::f64>(args.size()), engine_.mr_));
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
