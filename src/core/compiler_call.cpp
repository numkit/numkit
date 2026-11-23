// src/core/compiler_call.cpp
#include <keel/core.h>
#include <keel/format.h>
#include <keel/utf8.h>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/core/vm.hpp>
#include "compiler_internal.hpp"

#include <algorithm>
#include <cstdlib>

namespace numkit {

// `inputname(k)` inside the callee can return it), otherwise empty.
// Skips the side-table write entirely if every arg is non-identifier
// (the common case for arithmetic / literal args) to keep the table
// sparse.
void Compiler::recordCallArgNames(const ASTNode *callNode, keel::usize callInstrIdx)
{
    if (!callNode || callNode->children.size() < 2)
        return;
    keel::Array<keel::String> names;
    names.reserve(callNode->children.size() - 1);
    bool anyIdent = false;
    for (keel::usize i = 1; i < callNode->children.size(); ++i) {
        const auto &child = callNode->children[i];
        if (child && child->type == NodeType::IDENTIFIER
            && !engine_.isReservedName(child->strValue)) {
            names.push_back(child->strValue.clone());
            anyIdent = true;
        } else {
            names.emplace_back();
        }
    }
    if (anyIdent) {
        chunk_.callSiteArgNames.put(keel::as<keel::u32>(callInstrIdx),
                                    keel::move(names));
    }
}

keel::u16 Compiler::compileFused(const ASTNode *node, keel::usize ruleIdx,
                               keel::ArrayRef<const ASTNode *> operands)
{
    // 1. Compile the operand sub-exprs into a contiguous register block
    //    (same shape as call args) so the VM can read R[base..base+n).
    keel::Array<keel::u16> opRegs;
    opRegs.reserve(operands.size());
    for (const ASTNode *op : operands)
        opRegs.push_back(compileNode(op));
    const keel::u16 opBase = keel::as<keel::u16>(nextReg_);
    for (keel::u16 r : opRegs) {
        keel::u16 slot = tempReg();
        if (r != slot) emitAB(OpCode::MOVE, slot, r);
    }
    // 2. FUSE_EWISE with placeholder dst (a) and skip (e) — patched in step 4.
    emit(Instruction::make_abcde(OpCode::FUSE_EWISE, /*dst*/ 0, opBase,
                                 keel::as<keel::u16>(operands.size()),
                                 keel::as<keel::i32>(ruleIdx), /*skip*/ 0));
    const keel::usize fusePos = chunk_.code.size() - 1;
    const keel::usize spanStart = chunk_.code.size();
    // 3. Fallback = the normally compiled idiom (exact per-op semantics). The
    //    flag stops the nested compileCall from re-attempting fusion; reset is
    //    exception-safe (compileNode may throw RegisterExhaustionError).
    keel::u16 fbDst;
    {
        auto savedConstCache = constRegCache_.clone();
        RegBitset savedScalars{scalarRegs_.words.clone()};
        struct Guard {
            bool &f;
            keel::HashMap<const ASTNode *, keel::u16> &opMap;
            keel::HashMap<const ASTNode *, keel::u16> savedOpMap;
            Guard(bool &x, keel::HashMap<const ASTNode *, keel::u16> &m)
                : f(x), opMap(m), savedOpMap(m.clone())
            {
                f = true;
            }
            ~Guard()
            {
                f = false;
                opMap = keel::move(savedOpMap);
            }
        } guard(inFusionFallback_, fusedOperandRegs_);
        fusedOperandRegs_.clear();
        for (keel::usize i = 0; i < operands.size(); ++i)
            fusedOperandRegs_.put(operands[i], keel::as<keel::u16>(opBase + i));
        // Fall back to the node's OWN normal compile (the guard stops it from
        // re-attempting fusion, and fusedOperandRegs_ reuses the pre-evaluated
        // operand registers so side-effecting operands never evaluate twice).
        switch (node->type) {
            case NodeType::BINARY_OP: fbDst = compileBinaryOp(node); break;
            case NodeType::UNARY_OP:  fbDst = compileUnaryOp(node);  break;
            default:                  fbDst = compileCall(node);     break;
        }
        constRegCache_ = keel::move(savedConstCache);
        scalarRegs_ = keel::move(savedScalars);
    }
    // 4. Both paths write fbDst; on success FUSE_EWISE skips the fallback span.
    const keel::usize span = chunk_.code.size() - spanStart;
    if (span > 65534) {
        chunk_.code[fusePos] = Instruction::make_abc(OpCode::MOVE, fbDst, fbDst, 0);
        return fbDst;
    }
    chunk_.code[fusePos].a = fbDst;
    chunk_.code[fusePos].e = keel::as<keel::u16>(span);
    // Both paths write fbDst — FUSE_EWISE on success (then skips the span),
    // the fallback's last op on decline. Funnel through one explicit MOVE
    // (placed AFTER the skipped span, so it runs on both paths) into a fresh
    // result register. Without this, an enclosing assignment's MOVE-elimination
    // keys off the fallback's last instruction and never receives the
    // FUSE-success write, so the target variable is left unset.
    const keel::u16 res = tempReg();
    emitAB(OpCode::MOVE, res, fbDst);
    return res;
}

keel::i32 Compiler::tryCompileFused(const ASTNode *node)
{
    if (inFusionFallback_ || !engine_.fusionEnabled() || nextReg_ >= 65000)
        return -1;
    auto isKnownVarOrConst = [&](const ASTNode *op) -> bool {
        while (op && (op->type == NodeType::UNARY_OP || op->type == NodeType::FIELD_ACCESS)
               && !op->children.empty()) {
            op = op->children[0].get();
        }
        if (!op || op->type != NodeType::IDENTIFIER)
            return true;
        const keel::String &name = op->strValue;
        if (varRegisters_.contains(name) || upvalueRegs_.contains(name)
            || engine_.isReservedName(name))
            return true;
        if (isTopLevel_ && engine_.getVariable(name) != nullptr)
            return true;
        return false;
    };
    const auto &rules = engine_.fusionRules();
    for (keel::usize ri = 0; ri < rules.size(); ++ri) {
        auto operands = rules[ri].match(node);
        if (!operands)
            continue;
        bool allPure = true;
        for (const ASTNode *op : *operands) {
            if (!isKnownVarOrConst(op)) {
                allPure = false;
                break;
            }
        }
        if (!allPure)
            return -1;
        return compileFused(node, ri, operands->as_ref());
    }
    return -1;
}

keel::u16 Compiler::compileCall(const ASTNode *node)
{
    const keel::u16 callNargout = nargoutContext_;
    struct NargoutRestore
    {
        keel::u16 &ref;
        keel::u16 saved;
        ~NargoutRestore() { ref = saved; }
    } nargoutGuard{nargoutContext_, callNargout};
    nargoutContext_ = 1;

    // Element-wise fusion fast path: if `node` matches a registered idiom, emit
    // FUSE_EWISE + the normally compiled idiom as fallback.
    if (keel::i32 r = tryCompileFused(node); r >= 0) return keel::as<keel::u16>(r);

    auto *funcNode = node->children[0].get();

    // Superclass-qualified call `lhs@Base(args)`. Only meaningful inside a
    // classdef super-call `obj@Base(args)` / `method@Base(obj,…)` (single
    // output). Compiled into the method/ctor chunk so the body runs on the VM.
    if (funcNode->type == NodeType::SUPERCLASS_REF)
        return compileSuperCall(node, funcNode, /*explicitOutBase=*/-1, /*nout=*/1);

    // Qualified-name call: a chain of FIELD_ACCESS over a root IDENTIFIER
    // (`pkg.foo(x)`, `pkg.sub.bar(x)`). Treat as a flat CALL on the
    // dotted name when the root identifier is NOT a known variable
    // (workspace or local register). A local/workspace variable named
    // the same as the namespace shadows it — fall through to the
    // FIELD_ACCESS path so existing struct semantics are preserved.
    //
    // INVARIANT: this check uses workspaceEnv state at compile time. It
    // works because Engine::eval splits multi-statement scripts into
    // per-statement chunks and syncs workspace between them — so by the
    // time we compile statement N, workspaceEnv reflects 1..N-1's
    // assignments. If split-mode is ever removed, this check needs to
    // be runtime-driven via a new opcode.
    bool argHasEnd = false;
    for (keel::usize i = 1; i < node->children.size(); ++i)
        if (refsTopLevelEnd(node->children[i].get())) {
            argHasEnd = true;
            break;
        }
    if (funcNode->type == NodeType::FIELD_ACCESS && !argHasEnd) {
        const ASTNode *rootIdent = nullptr;
        keel::String qualified = tryBuildQualifiedName(funcNode, &rootIdent);
        if (!qualified.empty()) {
            const keel::String &root = rootIdent->strValue;
            const bool isLocalVar = varRegisters_.contains(root)
                                    || upvalueRegs_.contains(root);
            const bool isWorkspaceVar =
                isTopLevel_ && engine_.workspaceEnv().getLocal(root) != nullptr;
            if (!isLocalVar && !isWorkspaceVar
                // MATLAB `gca.Parent.Children(2)`: nothing registered
                // under the dotted name, but the HEAD is a plain
                // callable → the callee is a VALUE (call-then-fields
                // chain, portion 30) and the args index it — fall
                // through to the CALL_METHOD (property-then-index) path
                // below.
                && !(engine_.hasCallableName(root)
                     && !engine_.findClass(root)
                     && !engine_.classCtor(root)
                     && !engine_.hasExternalFunction(qualified))) {
                // Compile arguments and emit a regular CALL with the
                // dotted name as the function reference. Resolution is
                // delegated to the runtime CALL handler, which already
                // walks externalFuncs_ (incl. registered namespaces) and
                // the m-file resolver (incl. +pkg/.../<leaf>.m). A
                // brace-index arg (pkg.fn(c{:})) routes through
                // CALL_FLATTEN instead — same full dispatch, plus the
                // runtime CSL splat — exactly like plain-name calls
                // (bugs/closed/core/qualified-call-no-csl-splat).
                keel::usize nargs = node->children.size() - 1;
                bool anyBraceArg = false;
                for (keel::usize i = 1; i < node->children.size(); ++i)
                    if (node->children[i]->type == NodeType::CELL_INDEX) {
                        anyBraceArg = true;
                        break;
                    }
                keel::Array<keel::u16> argRegs;
                argRegs.reserve(nargs);
                for (keel::usize i = 1; i < node->children.size(); ++i)
                    argRegs.push_back(anyBraceArg
                                          ? compileNodeExpand(node->children[i].get())
                                          : compileNode(node->children[i].get()));
                keel::u16 argBase = nextReg_;
                for (keel::usize i = 0; i < argRegs.size(); ++i) {
                    keel::u16 slot = tempReg();
                    if (argRegs[i] != slot)
                        emitAB(OpCode::MOVE, slot, argRegs[i]);
                }
                keel::u16 dst = tempReg();
                keel::i32 funcIdx = addStringConstant(qualified);
                emit(Instruction::make_abcde(anyBraceArg ? OpCode::CALL_FLATTEN
                                                         : OpCode::CALL,
                                             dst,
                                             argBase,
                                             keel::as<keel::u16>(nargs),
                                             funcIdx,
                                             anyBraceArg ? callNargout : 0));
                return dst;
            }
        }
    }

    // OBJECT dotted call `obj.name(args)`: emit CALL_METHOD so the
    // runtime can dispatch a class method (vs property-then-index or a
    // struct-field func handle / array index in the fallback). Object
    // model §3. The qualified-namespace form (`pkg.fn(x)`) was handled
    // above; this is the variable/expression-rooted dotted call.
    //
    // Exception: when an argument references `end` at this level
    // (`s.arr(end)`), CALL_METHOD can't supply the field's size for the
    // `end` context (the field is fetched only at runtime). Those fall
    // through to the FIELD_GET + CALL_INDIRECT path below, which binds
    // `end` to the materialised field value. `end` in a method arg is
    // not meaningful, so objects lose nothing.
    if (funcNode->type == NodeType::FIELD_ACCESS && !argHasEnd) {
        keel::u16 objReg = compileNode(funcNode->children[0].get());
        const keel::usize nargs = node->children.size() - 1;
        // Brace-index args with a dotted callee (obj.fh(c{:}),
        // obj.method(c{:})): expansion compile — the CALL_METHOD handler
        // flattens the CSL at runtime
        // (bugs/closed/lang/handle-call-csl-splat).
        bool mAnyBrace = false;
        for (keel::usize i = 1; i < node->children.size(); ++i)
            if (node->children[i]->type == NodeType::CELL_INDEX)
                { mAnyBrace = true; break; }
        keel::Array<keel::u16> argRegs;
        argRegs.reserve(nargs);
        if (mAnyBrace) {
            for (keel::usize i = 1; i < node->children.size(); ++i)
                argRegs.push_back(compileNodeExpand(node->children[i].get()));
        } else {
            IndexContextGuard guard(*this, objReg, keel::as<keel::u16>(nargs));
            for (keel::usize i = 1; i < node->children.size(); ++i) {
                guard.setDim(keel::as<keel::u16>(i - 1));
                argRegs.push_back(compileNode(node->children[i].get()));
            }
        }
        keel::u16 argBase = nextReg_;
        for (keel::usize i = 0; i < argRegs.size(); ++i) {
            keel::u16 slot = tempReg();
            if (argRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, argRegs[i]);
        }
        keel::u16 dst = tempReg();
        keel::i32 nameIdx = addStringConstant(funcNode->strValue);
        keel::usize callIdx = chunk_.code.size();
        emit(Instruction::make_abcde(OpCode::CALL_METHOD, dst, objReg, argBase,
                                     nameIdx, keel::as<keel::u16>(argRegs.size())));
        recordCallArgNames(node, callIdx);
        return dst;
    }

    // Non-identifier call target (e.g. anonymous func expression called
    // directly, or a chained indexing form like `f(x)(i)`).
    // For the indexing case, `end` inside the arg list must bind to numel /
    // dimSize of the call target — without this guard it leaks the outer
    // (possibly zero-init) indexContextArr_ and resolves to the wrong size.
    // For function-handle invocation `end` isn't legal in MATLAB anyway, so
    // binding the guard to fhReg is harmless there.
    if (funcNode->type != NodeType::IDENTIFIER) {
        keel::u16 fhReg = compileNode(funcNode);
        const keel::usize nargs = node->children.size() - 1;
        // Brace-index args with a computed callee (obj.fh(c{:}),
        // s.f(x, c{:})): expansion compile + CALL_INDIRECT_FLATTEN —
        // the same CSL splat the identifier-callee gate provides
        // (bugs/closed/lang/handle-call-csl-splat).
        bool faAnyBrace = false;
        for (keel::usize i = 1; i < node->children.size(); ++i)
            if (node->children[i]->type == NodeType::CELL_INDEX)
                { faAnyBrace = true; break; }
        if (faAnyBrace) {
            keel::Array<keel::u16> argRegs;
            for (keel::usize i = 1; i < node->children.size(); ++i)
                argRegs.push_back(compileNodeExpand(node->children[i].get()));
            keel::u16 argBase = nextReg_;
            for (keel::usize i = 0; i < argRegs.size(); ++i) {
                keel::u16 slot = tempReg();
                if (argRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, argRegs[i]);
            }
            keel::u16 dst = tempReg();
            emit(Instruction::make_abcde(OpCode::CALL_INDIRECT_FLATTEN,
                                         dst, fhReg, argBase,
                                         keel::as<keel::u16>(argRegs.size()),
                                         std::max<keel::usize>(nargoutContext_, 1)));
            return dst;
        }
        keel::Array<keel::u16> argRegs;
        argRegs.reserve(nargs);
        {
            IndexContextGuard guard(*this, fhReg, keel::as<keel::u16>(nargs));
            for (keel::usize i = 1; i < node->children.size(); ++i) {
                guard.setDim(keel::as<keel::u16>(i - 1));
                argRegs.push_back(compileNode(node->children[i].get()));
            }
        }
        keel::u16 argBase = nextReg_;
        for (keel::usize i = 0; i < argRegs.size(); ++i) {
            keel::u16 slot = tempReg();
            if (argRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, argRegs[i]);
        }
        keel::u16 dst = tempReg();
        keel::usize callIdx = chunk_.code.size();
        emit(Instruction::make_abcde(OpCode::CALL_INDIRECT,
                                     dst,
                                     fhReg,
                                     argBase,
                                     0,
                                     keel::as<keel::u16>(argRegs.size())));
        recordCallArgNames(node, callIdx);
        return dst;
    }

    const keel::String &name = funcNode->strValue;
    keel::usize nargs = node->children.size() - 1;

    if (engine_.isVarDeclCommand(name)) {
        keel::Array<keel::StringRef> rawArgs;
        for (keel::usize i = 1; i <= nargs; ++i) {
            auto *argNode = node->children[i].get();
            if (argNode->type == NodeType::STRING_LITERAL)
                rawArgs.push_back(argNode->strValue.as_ref());
        }
        keel::Array<keel::String> declared;
        engine_.extractVarDeclCommandNames(name, rawArgs.as_ref(), declared);
        for (const auto &tok : declared) {
            if (!engine_.isReservedName(tok)) {
                keel::u16 reg = varRegWrite(tok);
                if (!mayClear_)
                    definedRegs_.set(reg);
            }
        }
    }

    // ── Workspace introspection builtins → opcodes inside functions ──
    if (!isTopLevel_) {
        // exist(expr) or exist(expr, filter) → EXIST_VAR dst, nameReg, filterReg
        if (name == "exist" && (nargs == 1 || nargs == 2)) {
            keel::u16 nameReg = compileNode(node->children[1].get());
            keel::u16 filterReg = 0;
            if (nargs == 2) {
                filterReg = compileNode(node->children[2].get());
            }
            keel::u16 dst = tempReg();
            emitABC(OpCode::EXIST_VAR, dst, nameReg, filterReg);
            return dst;
        }
    }

    // clear(expr) works both inside functions and at top-level
    if (name == "clear") {
        bool anyBrace = false;
        keel::Array<keel::u16> argRegs;
        argRegs.reserve(nargs);
        for (keel::usize i = 1; i <= nargs; ++i) {
            auto *argNode = node->children[i].get();
            if (argNode->type == NodeType::STRING_LITERAL) {
                if (const keel::u16 *regp = varRegisters_.get(argNode->strValue);
                    regp && engine_.isReservedName(argNode->strValue) == 0) {
                    emitA(OpCode::CLEAR_VAR, *regp);
                    emitUpvalStoreIfShared(argNode->strValue, *regp);
                }
                if (isTopLevel_ || mayInjectVars_)
                    argRegs.push_back(compileNode(argNode));
            } else {
                const bool isBrace = (argNode->type == NodeType::CELL_INDEX);
                if (isBrace)
                    anyBrace = true;
                keel::u16 nameReg = isBrace ? compileNodeExpand(argNode) : compileNode(argNode);
                emitAB(OpCode::CLEAR_DYN, nameReg, keel::as<keel::u16>(nargs));
                if (isTopLevel_ || mayInjectVars_)
                    argRegs.push_back(nameReg);
            }
        }
        if (nargs == 0) {
            for (auto [vname, reg] : varRegisters_) {
                if (engine_.isReservedName(vname) == 0) {
                    emitA(OpCode::CLEAR_VAR, reg);
                    emitUpvalStoreIfShared(vname, reg);
                }
            }
        }
        // On top-level (or when dynamic injection may exist), also call externalFunc.
        if (isTopLevel_ || mayInjectVars_) {
            keel::u16 argBase = nextReg_;
            for (keel::usize i = 0; i < argRegs.size(); ++i) {
                keel::u16 slot = tempReg();
                if (argRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, argRegs[i]);
            }
            keel::u16 dst = tempReg();
            keel::i32 funcIdx = addStringConstant(name);
            emit(Instruction::make_abcde(anyBrace ? OpCode::CALL_FLATTEN : OpCode::CALL,
                                         dst,
                                         argBase,
                                         keel::as<keel::u16>(nargs),
                                         funcIdx,
                                         0));
            return dst;
        }
        return tempReg();
    }

    if (!isTopLevel_ && nargoutContext_ == 0) {
        // who(expr...) / whos(expr...) → WHO/WHOS with args (statement context only;
        // nargout > 0 falls through to CALL so the builtin returns the cell/struct array)
        if (name == "who" || name == "whos") {
            OpCode op = (name == "who") ? OpCode::WHO : OpCode::WHOS;
            if (nargs == 0) {
                emitAB(op, 0, 0);
            } else {
                keel::Array<keel::u16> argRegs;
                for (keel::usize i = 1; i <= nargs; ++i)
                    argRegs.push_back(compileNode(node->children[i].get()));
                keel::u16 base = nextReg_;
                for (keel::usize i = 0; i < argRegs.size(); ++i) {
                    keel::u16 slot = tempReg();
                    if (argRegs[i] != slot)
                        emitAB(OpCode::MOVE, slot, argRegs[i]);
                }
                emitAB(op, base, keel::as<keel::u16>(argRegs.size()));
            }
            return tempReg();
        }
    }

    // Check if it's a known variable (local or from workspaceEnv)
    // → could be array indexing OR func handle call — emit CALL_INDIRECT
    bool isKnownVar = varRegisters_.contains(name) || upvalueRegs_.contains(name);

    // Also check workspaceEnv (or scoped eval caller vars) for variables from previous eval() calls
    if (!isKnownVar && isTopLevel_ && !engine_.isReservedName(name)) {
        Value *existing = nullptr;
        if (engine_.vm_ && engine_.vm_->inheritedScope_) {
            if (auto *dv = engine_.vm_->nextFrameDynVars())
                existing = dv->get(name);
            if (!existing)
                existing = engine_.vm_->inheritedScope_->get(name);
        } else {
            existing = engine_.getVariable(name);
        }
        if (existing && !existing->isUnset() && !existing->isDeleted()) {
            // Variable exists — check if it's a callable function handle.
            // Callable variables take priority over same-name external functions.
            bool isCallable = existing->isFuncHandle();
            if (isCallable || (!engine_.isKnownLeafName(name) && !engine_.hasFunction(name))) {
                isKnownVar = true;
            }
        }
    }

    if (isKnownVar) {
        keel::u16 fhReg = varRegLookup(name);
        if (const keel::u16 *uvReg = upvalueRegs_.get(name)) {
            keel::i32 nameIdx = addStringConstant(name);
            emit(Instruction::make_abcde(OpCode::UPVAL_LOAD, fhReg, *uvReg, 0, nameIdx, 0));
        }

        bool varAnyBrace = false;
        for (keel::usize i = 1; i < node->children.size(); ++i)
            if (node->children[i]->type == NodeType::CELL_INDEX)
                { varAnyBrace = true; break; }
        if (varAnyBrace) {
            keel::Array<keel::u16> argRegs;
            for (keel::usize i = 1; i < node->children.size(); ++i)
                argRegs.push_back(compileNodeExpand(node->children[i].get()));
            keel::u16 argBase = nextReg_;
            for (keel::usize i = 0; i < argRegs.size(); ++i) {
                keel::u16 slot = tempReg();
                if (argRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, argRegs[i]);
            }
            keel::u16 dst = tempReg();
            emit(Instruction::make_abcde(OpCode::CALL_INDIRECT_FLATTEN,
                                         dst, fhReg, argBase,
                                         keel::as<keel::u16>(argRegs.size()),
                                         std::max<keel::usize>(nargoutContext_, 1)));
            return dst;
        }

        IndexContextGuard guard(*this, fhReg, keel::as<keel::u16>(nargs));

        keel::Array<keel::u16> argRegs;
        for (keel::usize i = 1; i < node->children.size(); ++i) {
            guard.setDim(keel::as<keel::u16>(i - 1));
            argRegs.push_back(compileNode(node->children[i].get()));
        }

        keel::u16 argBase = keel::as<keel::u16>(nextReg_);
        for (keel::usize i = 0; i < argRegs.size(); ++i) {
            keel::u16 slot = tempReg();
            if (argRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, argRegs[i]);
        }
        keel::u16 dst = tempReg();
        keel::usize callIdx = chunk_.code.size();
        emit(Instruction::make_abcde(OpCode::CALL_INDIRECT,
                                     dst,
                                     fhReg,
                                     argBase,
                                     0,
                                     keel::as<keel::u16>(argRegs.size())));
        recordCallArgNames(node, callIdx);
        return dst;
    }

    // Nested function call in lexical scope: pass user args + hidden upvalue boxes
    {
        if (const NestedFuncInfo *ninfo = nestedFuncInfo_.get(name)) {
            bool anyBraceArg = false;
            for (keel::usize i = 1; i < node->children.size(); ++i)
                if (node->children[i]->type == NodeType::CELL_INDEX) { anyBraceArg = true; break; }
            keel::Array<keel::u16> argRegs;
            argRegs.reserve(nargs + ninfo->upvalueNames.size());
            for (keel::usize i = 1; i < node->children.size(); ++i) {
                argRegs.push_back(anyBraceArg
                                      ? compileNodeExpand(node->children[i].get())
                                      : compileNode(node->children[i].get()));
            }
            for (const auto &uvName : ninfo->upvalueNames) {
                if (const keel::u16 *box = allScopeUpvalueBoxRegs_.get(uvName))
                    argRegs.push_back(*box);
            }
            keel::u16 argBase = keel::as<keel::u16>(nextReg_);
            for (keel::usize i = 0; i < argRegs.size(); ++i) {
                keel::u16 slot = tempReg();
                if (argRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, argRegs[i]);
            }
            keel::u16 dst = tempReg();
            keel::i32 funcIdx = addStringConstant(ninfo->compiledName);
            keel::usize callIdx = chunk_.code.size();
            emit(Instruction::make_abcde(anyBraceArg ? OpCode::CALL_FLATTEN : OpCode::CALL,
                                         dst,
                                         argBase,
                                         keel::as<keel::u16>(argRegs.size()),
                                         funcIdx,
                                         callNargout));
            if (!anyBraceArg)
                recordCallArgNames(node, callIdx);
            return dst;
        }
    }

    // File-private local subfunction in the current m-file shadows builtins/globals.
    const keel::StringRef fileSub = resolveFileSubfunc(name);
    const keel::StringRef callTarget = fileSub.empty() ? name.as_ref() : fileSub;

    // Fast path: 1-arg inline builtins — skip MOVE, pass arg register directly
    if (!mayInjectVars_ && fileSub.empty()) {
        FastBuiltinId builtinId = resolveBuiltinId(name, nargs);
        if (builtinId != FastBuiltinId::None && nargs == 1) {
            keel::u16 arg = compileNode(node->children[1].get());
            keel::u16 dst = tempReg();
            emit(Instruction::make_abcde(OpCode::CALL_BUILTIN,
                                         dst, arg, 1,
                                         static_cast<keel::i32>(builtinId), 0));
            if (scalarRegs_.test(arg) && isUnaryFastBuiltin(builtinId))
                scalarRegs_.set(dst);
            return dst;
        }
    }

    // Comma-separated-list call args f(a, c{:}, c{vec}, b): a cell brace-index arg
    // with a multi-valued subscript splices its selected contents into the call's
    // argument list at runtime (CSL). Lower to CALL_VARARGS, which builds the runtime
    // arg vector from a single cell and dispatches. v1: bare colon c{:} (mode 1, all)
    // and a SYNTACTICALLY-multi subscript c{1:2} / c{[1 3]} (mode 2, selected); a
    // scalar/variable subscript c{i} stays the normal path (single value -- the
    // scalar/vector distinction is only known at runtime, and f(c{i}) must stay cheap).
    // First-class call args: if any argument is a brace cell-index c{sub} (which may
    // produce a comma-separated list at runtime -- including a variable subscript, the
    // case the campaign opcodes could not splice), compile every arg with
    // compileNodeExpand (preserving a CSL) into consecutive registers and emit
    // CALL_FLATTEN, which flattens any CSL arg into the runtime arg list and runs the
    // full target dispatch (all call targets). A call with no brace-index arg uses the
    // hot normal CALL below, untouched. Single output here; multi-output is in
    // compileCallMulti. See dev-docs/memory/csl_first_class.md.
    bool anyBraceArg = false;
    for (keel::usize i = 1; i < node->children.size(); ++i)
        if (node->children[i]->type == NodeType::CELL_INDEX) { anyBraceArg = true; break; }
    if (anyBraceArg) {
        keel::Array<keel::u16> argRegs;
        for (keel::usize i = 1; i < node->children.size(); ++i)
            argRegs.push_back(compileNodeExpand(node->children[i].get()));
        keel::u16 argBase = nextReg_;
        for (keel::usize i = 0; i < argRegs.size(); ++i) {
            keel::u16 slot = tempReg();
            if (argRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, argRegs[i]);
        }
        keel::u16 dst     = tempReg();
        keel::i32 funcIdx = addStringConstant(callTarget);
        emit(Instruction::make_abcde(OpCode::CALL_FLATTEN, dst, argBase,
                                     keel::as<keel::u16>(argRegs.size()), funcIdx,
                                     callNargout));
        return dst;
    }

    // Compile arguments into consecutive registers
    keel::Array<keel::u16> argRegs;
    for (keel::usize i = 1; i < node->children.size(); ++i) {
        argRegs.push_back(compileNode(node->children[i].get()));
    }

    keel::u16 argBase = nextReg_;
    for (keel::usize i = 0; i < argRegs.size(); ++i) {
        keel::u16 slot = tempReg();
        if (argRegs[i] != slot) {
            emitAB(OpCode::MOVE, slot, argRegs[i]);
        }
    }

    keel::u16 dst = tempReg();

    // Try to resolve as inline scalar builtin
    FastBuiltinId builtinId = (mayInjectVars_ || !fileSub.empty())
                                  ? FastBuiltinId::None
                                  : resolveBuiltinId(name, nargs);
    if (builtinId != FastBuiltinId::None) {
        emit(Instruction::make_abcde(OpCode::CALL_BUILTIN,
                                     dst,
                                     argBase,
                                     keel::as<keel::u16>(nargs),
                                     static_cast<keel::i32>(builtinId),
                                     0));
        return dst;
    }

    // General CALL
    keel::i32 funcIdx = addStringConstant(callTarget);
    keel::usize callIdx = chunk_.code.size();
    emit(Instruction::make_abcde(OpCode::CALL,
                                 dst,
                                 argBase,
                                 keel::as<keel::u16>(nargs),
                                 funcIdx,
                                 callNargout));
    recordCallArgNames(node, callIdx);
    return dst;
}

// ============================================================
// Command-style calls: clear all, hold on, grid on, etc.
//
// The parser generates COMMAND_CALL nodes with:
//   strValue   = function name
//   children[] = arguments (all STRING_LITERAL)
//
// We compile these as standard CALL opcodes with string args,
// which the VM dispatches through externalFuncs_ like any other call.
// ============================================================

keel::u16 Compiler::compileCommandCall(const ASTNode *node)
{
    const keel::String &name = node->strValue;
    keel::usize nargs = node->children.size();

    // Variable-headed command-syntax misparse inside functions: `x -1`, `x /2`, `x -y`
    if (!isTopLevel_ && nargs == 1
        && name != "clear" && name != "who" && name != "whos"
        && name != "exist" && !engine_.isVarDeclCommand(name)
        && (varRegisters_.contains(name) || upvalueRegs_.contains(name)
            || engine_.isReservedName(name) != 0)) {
        const keel::String &arg = node->children[0]->strValue;
        if (arg.size() >= 2 && (arg[0] == '-' || arg[0] == '/')) {
            const keel::String rest = keel::String::from(arg.substr(1));
            char *endPtr = nullptr;
            keel::f64 numVal = std::strtod(rest.c_str(), &endPtr);
            const bool isNum = (endPtr != rest.c_str() && *endPtr == '\0');
            auto isIdent = [](keel::StringRef s) {
                if (s.empty())
                    return false;
                keel::u8 c0 = keel::as<keel::u8>(s[0]);
                if (!keel::is_ascii_alpha(c0) && c0 != '_')
                    return false;
                for (keel::usize k = 1; k < s.size(); ++k) {
                    keel::u8 ck = keel::as<keel::u8>(s[k]);
                    if (!keel::is_ascii_alnum(ck) && ck != '_')
                        return false;
                }
                return true;
            };
            if (isNum || isIdent(rest)) {
                keel::u16 lhsReg = varRegRead(name);
                keel::u16 rhsReg = 0;
                if (isNum) {
                    keel::i32 cIdx = addConstant(numVal);
                    rhsReg = tempReg();
                    emitAD(OpCode::LOAD_CONST, rhsReg, cIdx);
                    scalarRegs_.set(rhsReg);
                } else {
                    rhsReg = varRegRead(rest);
                }
                keel::u16 resReg = emitPlainBinary(arg.substr(0, 1), lhsReg, rhsReg);
                keel::u16 ansReg = varRegWrite("ans");
                if (ansReg != resReg)
                    emitAB(OpCode::MOVE, ansReg, resReg);
                emitUpvalStoreIfShared("ans", ansReg);
                if (!node->suppressOutput) {
                    keel::i32 nameIdx = addStringConstant("ans");
                    emitAD(OpCode::DISPLAY, ansReg, nameIdx);
                }
                return ansReg;
            }
        }
    }

    if (name == "clear") {
        // Determine what kind of clear this is
        auto isClearFlag = [](keel::StringRef s) noexcept {
            return s == "all" || s == "classes" || s == "functions"
                   || s == "global" || s == "-regexp" || s == "import";
        };

        bool isFlag = nargs > 0 && isClearFlag(node->children[0]->strValue);
        bool isClearAll = nargs == 0
                          || (nargs > 0
                              && (node->children[0]->strValue == "all"
                                  || node->children[0]->strValue == "classes"));

        // Step 1: emit CLEAR_VAR opcodes for local registers
        if (isClearAll) {
            for (auto [vname, reg] : varRegisters_) {
                if (engine_.isReservedName(vname) == 0) {
                    emitA(OpCode::CLEAR_VAR, reg);
                    emitUpvalStoreIfShared(vname, reg);
                }
            }
        } else if (!isFlag) {
            for (keel::usize i = 0; i < nargs; ++i) {
                const keel::String &varName = node->children[i]->strValue;
                if (engine_.isReservedName(varName) == 0) {
                    if (const keel::u16 *regp = varRegisters_.get(varName)) {
                        emitA(OpCode::CLEAR_VAR, *regp);
                        emitUpvalStoreIfShared(varName, *regp);
                    }
                }
            }
        }
        // Flags (functions, global, -regexp, import) don't clear local registers

        // Step 2: call externalFunc for side effects on top level,
        // or for flags / dynamic frames that need runtime handling.
        if (isTopLevel_ || isFlag || mayInjectVars_) {
            keel::u16 argBase = nextReg_;
            for (keel::usize i = 0; i < nargs; ++i) {
                keel::u16 slot = tempReg();
                keel::i32 strIdx = addStringConstant(node->children[i]->strValue);
                emitAD(OpCode::LOAD_STRING, slot, strIdx);
            }
            keel::u16 dst = tempReg();
            keel::i32 funcIdx = addStringConstant(name);
            emit(Instruction::make_abcde(OpCode::CALL,
                                         dst,
                                         argBase,
                                         keel::as<keel::u16>(nargs),
                                         funcIdx,
                                         0));
            return dst;
        }

        return tempReg();
    }

    // ── who/whos inside function → opcode ──
    if (!isTopLevel_ && (name == "who" || name == "whos")) {
        // Check for unsupported flags — fall through to generic CALL
        if (nargs > 0 && node->children[0]->strValue == "-file") {
            // Fall through to generic command call (externalFunc warns)
        } else {
            OpCode op = (name == "who") ? OpCode::WHO : OpCode::WHOS;
            if (nargs == 0) {
                emitAB(op, 0, 0);
            } else {
                keel::u16 base = nextReg_;
                for (keel::usize i = 0; i < nargs; ++i) {
                    keel::u16 slot = tempReg();
                    keel::i32 strIdx = addStringConstant(node->children[i]->strValue);
                    emitAD(OpCode::LOAD_STRING, slot, strIdx);
                }
                emitAB(op, base, keel::as<keel::u16>(nargs));
            }
            return tempReg();
        }
    }

    // ── exist x [type] inside function → EXIST_VAR ──
    if (!isTopLevel_ && name == "exist" && (nargs == 1 || nargs == 2)) {
        keel::u16 nameReg = tempReg();
        keel::i32 strIdx = addStringConstant(node->children[0]->strValue);
        emitAD(OpCode::LOAD_STRING, nameReg, strIdx);
        keel::u16 filterReg = 0;
        if (nargs == 2) {
            filterReg = tempReg();
            keel::i32 fIdx = addStringConstant(node->children[1]->strValue);
            emitAD(OpCode::LOAD_STRING, filterReg, fIdx);
        }
        keel::u16 dst = tempReg();
        emitABC(OpCode::EXIST_VAR, dst, nameReg, filterReg);
        return dst;
    }

    // ── Generic command call ──
    if (engine_.isVarDeclCommand(name)) {
        keel::Array<keel::StringRef> rawArgs;
        rawArgs.reserve(nargs);
        for (keel::usize i = 0; i < nargs; ++i)
            rawArgs.push_back(node->children[i]->strValue.as_ref());
        keel::Array<keel::String> declared;
        engine_.extractVarDeclCommandNames(name, rawArgs.as_ref(), declared);
        for (const auto &tok : declared) {
            if (!engine_.isReservedName(tok)) {
                keel::u16 reg = varRegWrite(tok);
                if (!mayClear_)
                    definedRegs_.set(reg);
            }
        }
    }

    // Compile arguments: each child is a STRING_LITERAL
    const NestedFuncInfo *ninfo = (!varRegisters_.contains(name)
                                   && !upvalueRegs_.contains(name))
                                      ? nestedFuncInfo_.get(name)
                                      : nullptr;
    keel::u16 argBase = nextReg_;
    for (keel::usize i = 0; i < nargs; ++i) {
        keel::u16 slot = tempReg();
        keel::i32 strIdx = addStringConstant(node->children[i]->strValue);
        emitAD(OpCode::LOAD_STRING, slot, strIdx);
    }
    keel::usize totalCallArgs = nargs;
    if (ninfo) {
        for (const auto &uvName : ninfo->upvalueNames) {
            keel::u16 slot = tempReg();
            if (const keel::u16 *box = allScopeUpvalueBoxRegs_.get(uvName)) {
                keel::u16 boxReg = *box;
                if (boxReg != slot)
                    emitAB(OpCode::MOVE, slot, boxReg);
            }
            ++totalCallArgs;
        }
    }

    keel::u16 dst = tempReg();
    keel::StringRef cmdSub = (!ninfo && !varRegisters_.contains(name) && !upvalueRegs_.contains(name))
                                 ? resolveFileSubfunc(name)
                                 : keel::StringRef{};
    keel::i32 funcIdx = addStringConstant(
        ninfo ? ninfo->compiledName.as_ref()
              : (!cmdSub.empty() ? cmdSub : name.as_ref()));
    emit(
        Instruction::make_abcde(OpCode::CALL, dst, argBase,
                                keel::as<keel::u16>(totalCallArgs), funcIdx,
                                node->captureResult ? 1 : 0));

    // Display result if no semicolon (rare for commands, but correct)
    if (!node->suppressOutput) {
        keel::i32 nameIdx = addStringConstant("ans");
        emitAD(OpCode::DISPLAY, dst, nameIdx);
    }

    return dst;
}

// Collect free variables in AST node — identifiers used but not in params/locals
void Compiler::collectFreeVars(const ASTNode *node,
                               keel::ArrayRef<keel::String> params,
                               keel::Array<keel::String> &freeVars)
{
    if (!node)
        return;

    if (node->type == NodeType::IDENTIFIER) {
        const keel::String &name = node->strValue;
        // Skip if it's a parameter
        for (const auto &p : params)
            if (p == name)
                return;
        // Skip if already collected
        for (const auto &f : freeVars)
            if (f == name)
                return;
        // Skip __result__ (synthetic return var)
        if (name == "__result__")
            return;
        // Skip unshadowed built-in reserved constants (pi, eps, inf, nan, ...):
        // the anonymous function chunk resolves them directly from constantsEnv_.
        if (engine_.isReservedName(name) && !chunk_.assignedVars.contains(name)
            && (!isTopLevel_ || !engine_.workspaceEnv().get(name)))
            return;
        // Check if exists in outer scope
        if (varRegisters_.contains(name) || upvalueRegs_.contains(name) || nestedFuncInfo_.contains(name)) {
            freeVars.push_back(name.clone());
        }
        return;
    }

    if (node->type == NodeType::ANON_FUNC) {
        if (!node->strValue.empty() && node->children.empty()) {
            const keel::String &fn = node->strValue;
            for (const auto &p : params)
                if (p == fn)
                    return;
            for (const auto &f : freeVars)
                if (f == fn)
                    return;
            if (nestedFuncInfo_.contains(fn))
                freeVars.push_back(fn.clone());
            return;
        }
        keel::Array<keel::String> mergedParams;
        mergedParams.reserve(params.size() + node->paramNames.size());
        for (const auto &p : params)
            mergedParams.push_back(p.clone());
        for (const auto &p : node->paramNames)
            mergedParams.push_back(p.clone());
        for (auto &child : node->children)
            collectFreeVars(child.get(), mergedParams.as_ref(), freeVars);
        return;
    }

    // Recurse into children
    for (auto &child : node->children)
        collectFreeVars(child.get(), params, freeVars);
    for (auto &[a, b] : node->branches) {
        collectFreeVars(a.get(), params, freeVars);
        collectFreeVars(b.get(), params, freeVars);
    }
    if (node->elseBranch)
        collectFreeVars(node->elseBranch.get(), params, freeVars);
}

keel::u16 Compiler::compileAnonFunc(const ASTNode *node)
{
    // Case 1: @funcname — handle to existing function
    if (!node->strValue.empty() && node->children.empty()) {
        if (const NestedFuncInfo *ninfo = nestedFuncInfo_.get(node->strValue))
            return emitNestedFuncHandle(*ninfo);
        keel::u16 dst = tempReg();
        keel::StringRef sub = resolveFileSubfunc(node->strValue);
        Value fh = Value::funcHandle(sub.empty() ? node->strValue.as_ref() : sub,
                                     engine_.allocator());
        if (!sub.empty())
            fh.setFuncHandleSource(node->strValue);
        keel::i32 constIdx = appendConstant(keel::move(fh));
        emitAD(OpCode::LOAD_CONST, dst, constIdx);
        return dst;
    }

    // Case 2: @(params) expr — anonymous function with possible closure capture.
    keel::i32 id = engine_.anonNameCounter_.fetch_add(1, std::memory_order_relaxed);
    keel::String anonName = keel::format("__anon_{}", id);

    // Build a synthetic function AST node
    auto funcNode = makeNode(NodeType::FUNCTION_DEF);
    funcNode->strValue = anonName.clone();
    funcNode->paramNames = node->paramNames.clone();
    const ASTNode *bodyExpr = node->children[0].get();
    bool forwardVarargout =
        bodyExpr->type == NodeType::CALL
        && !bodyExpr->children.empty()
        && bodyExpr->children[0]->type == NodeType::IDENTIFIER
        && bodyExpr->children[0]->strValue != "__numkit_fwd_call__";
    if (forwardVarargout) {
        const keel::String &callee = bodyExpr->children[0]->strValue;
        bool calleeIsVar =
            varRegisters_.contains(callee)
            || upvalueRegs_.contains(callee)
            || nestedFuncInfo_.contains(callee);
        for (const auto &p : node->paramNames)
            if (p == callee) { calleeIsVar = true; break; }
        if (calleeIsVar)
            forwardVarargout = false;
    }

    auto rewriteNestedRefs = [&](auto &self, ASTNode *cur,
                                 keel::ArrayRef<keel::String> boundParams,
                                 bool isCallCallee) -> void {
        if (!cur || nestedFuncInfo_.empty())
            return;
        auto isBound = [&](keel::StringRef nm) {
            for (const auto &bp : boundParams)
                if (bp == nm)
                    return true;
            return false;
        };
        if (cur->type == NodeType::ANON_FUNC) {
            if (!cur->strValue.empty() && cur->children.empty()
                && !isBound(cur->strValue) && nestedFuncInfo_.contains(cur->strValue)) {
                cur->type = NodeType::IDENTIFIER;
                return;
            }
            keel::Array<keel::String> merged;
            merged.reserve(boundParams.size() + cur->paramNames.size());
            for (const auto &bp : boundParams)
                merged.push_back(bp.clone());
            for (const auto &p : cur->paramNames)
                merged.push_back(p.clone());
            for (auto &ch : cur->children)
                self(self, ch.get(), merged.as_ref(), false);
            return;
        }
        if (cur->type == NodeType::IDENTIFIER && !isCallCallee
            && !isBound(cur->strValue) && nestedFuncInfo_.contains(cur->strValue)
            && !varRegisters_.contains(cur->strValue) && !upvalueRegs_.contains(cur->strValue)) {
            keel::String fn = keel::move(cur->strValue);
            cur->type = NodeType::CALL;
            cur->strValue.clear();
            auto calleeNode = makeNode(NodeType::IDENTIFIER);
            calleeNode->strValue = keel::move(fn);
            cur->children.push_back(keel::move(calleeNode));
            return;
        }
        for (keel::usize i = 0; i < cur->children.size(); ++i) {
            const bool childIsCallee = (cur->type == NodeType::CALL && i == 0);
            self(self, cur->children[i].get(), boundParams, childIsCallee);
        }
        for (auto &[a, b] : cur->branches) {
            self(self, a.get(), boundParams, false);
            self(self, b.get(), boundParams, false);
        }
        if (cur->elseBranch)
            self(self, cur->elseBranch.get(), boundParams, false);
    };

    auto bodyBlock = makeNode(NodeType::BLOCK);
    auto assignNode = makeNode(NodeType::ASSIGN);
    assignNode->suppressOutput = true;

    if (forwardVarargout) {
        funcNode->returnNames.push_back(keel::String::from("varargout"));
        auto vaId = makeNode(NodeType::IDENTIFIER);
        vaId->strValue.assign("varargout");
        assignNode->children.push_back(keel::move(vaId));
        // RHS: __numkit_fwd_call__(nargout, 'g', args...)
        auto fwdCall = makeNode(NodeType::CALL);
        auto fwdId = makeNode(NodeType::IDENTIFIER);
        fwdId->strValue.assign("__numkit_fwd_call__");
        fwdCall->children.push_back(keel::move(fwdId));
        auto nargoutId = makeNode(NodeType::IDENTIFIER);
        nargoutId->strValue.assign("nargout");
        fwdCall->children.push_back(keel::move(nargoutId));
        auto fnameStr = makeNode(NodeType::STRING_LITERAL);
        fnameStr->strValue = bodyExpr->children[0]->strValue.clone();
        fwdCall->children.push_back(keel::move(fnameStr));
        for (keel::usize i = 1; i < bodyExpr->children.size(); ++i) {
            auto clonedArg = cloneNode(bodyExpr->children[i].get());
            rewriteNestedRefs(rewriteNestedRefs, clonedArg.get(), node->paramNames.as_ref(), false);
            fwdCall->children.push_back(keel::move(clonedArg));
        }
        assignNode->children.push_back(keel::move(fwdCall));
    } else {
        funcNode->returnNames.push_back(keel::String::from("__result__"));
        auto resultId = makeNode(NodeType::IDENTIFIER);
        resultId->strValue.assign("__result__");
        assignNode->children.push_back(keel::move(resultId));
        auto clonedExpr = cloneNode(node->children[0].get());
        rewriteNestedRefs(rewriteNestedRefs, clonedExpr.get(), node->paramNames.as_ref(), false);
        assignNode->children.push_back(keel::move(clonedExpr));
    }
    bodyBlock->children.push_back(keel::move(assignNode));
    funcNode->children.push_back(keel::move(bodyBlock));

    // Find free variables in body that exist in outer scope but are not params
    keel::Array<keel::String> capturedNames;
    collectFreeVars(node->children[0].get(), node->paramNames.as_ref(), capturedNames);

    keel::Array<keel::String> activeCaptures;
    for (auto &name : capturedNames) {
        if (varRegisters_.contains(name) || upvalueRegs_.contains(name) || nestedFuncInfo_.contains(name)) {
            activeCaptures.push_back(name.clone());
            funcNode->paramNames.push_back(name.clone());
        }
    }

    // Compile as separate chunk
    BytecodeChunk funcChunk = compileFunction(funcNode.get());

    keel::Array<keel::u16> capturedOuterRegs;
    capturedOuterRegs.reserve(activeCaptures.size());
    for (const auto &name : activeCaptures) {
        if (const keel::u16 *uvReg = upvalueRegs_.get(name)) {
            keel::u16 capReg = tempReg();
            emit(Instruction::make_abcde(OpCode::UPVAL_LOAD, capReg, *uvReg, 1, 0, 0));
            capturedOuterRegs.push_back(capReg);
            continue;
        }
        if (const keel::u16 *regp = varRegisters_.get(name)) {
            capturedOuterRegs.push_back(*regp);
            continue;
        }
        if (const NestedFuncInfo *ninfo = nestedFuncInfo_.get(name)) {
            capturedOuterRegs.push_back(emitNestedFuncHandle(*ninfo));
        }
    }
    funcChunk.capturedRegisters = capturedOuterRegs.clone();
    // Provenance is fixed at creation (reference rule). Compilation of a
    // NAMED function body carries that function's file explicitly (the
    // resolver compiles before the origin frame is active); everywhere
    // else the currently executing origin at compile time equals the one
    // at the ANON_FUNC node's runtime — capture from the stack.
    {
        UserFunction stamp;
        if (anonOriginKnown_) {
            stamp.originKnown = true;
            stamp.sourceFs = anonOriginFs_.clone();
            stamp.sourceDir = anonOriginDir_.clone();
            stamp.sourceFile = anonOriginFile_.clone();
        } else {
            engine_.captureScriptOrigin(stamp);
        }
        funcChunk.originKnown = true;
        funcChunk.originFs = keel::move(stamp.sourceFs);
        funcChunk.originDir = keel::move(stamp.sourceDir);
        funcChunk.originFile = keel::move(stamp.sourceFile);
    }
    compiledFuncs_.put(anonName.clone(), keel::make_arc<BytecodeChunk>(keel::move(funcChunk)));

    // Mirror-register the anon-fn into engine.userFuncs_ so that callbacks
    // routed through Engine::callFunctionHandle (TW-side) can find it
    // even when VM was the active backend at handle creation time.
    // Captures travel as appended args (Engine::callFunctionHandle
    // unwraps the {handle, captures} closure cell), and our funcNode's
    // paramNames is exactly [user_params..., captured_names...] — the
    // same layout VM uses for the cell→params mapping.
    {
        UserFunction uf;
        uf.name = anonName.clone();
        uf.params = funcNode->paramNames.clone();
        uf.returns = funcNode->returnNames.clone();   // {"__result__"} or {"varargout"}
        uf.body = keel::Arc<const ASTNode>(cloneNode(funcNode->children[0].get()));
        uf.closureEnv = nullptr;
        uf.fileSubfuncs = fileSubfuncs_;
        engine_.captureScriptOrigin(uf);
        engine_.adoptUserFunction(anonName, keel::move(uf));
    }

    if (capturedOuterRegs.empty()) {
        // No captures — simple func handle
        keel::u16 dst = tempReg();
        Value fh = Value::funcHandle(anonName, engine_.allocator());
        fh.setFuncHandleSource(node->strValue);   // reconstructed "@(...)..." for func2str
        keel::i32 constIdx = appendConstant(keel::move(fh));
        emitAD(OpCode::LOAD_CONST, dst, constIdx);
        return dst;
    }

    // Has captures — create a cell: {funcHandle, cap1, cap2, ...}
    keel::usize totalSlots = 1 + capturedOuterRegs.size();

    // Compile elements: funcHandle, then captured values
    keel::Array<keel::u16> elemRegs;
    // funcHandle constant
    keel::u16 fhReg = tempReg();
    Value fh = Value::funcHandle(anonName, engine_.allocator());
    fh.setFuncHandleSource(node->strValue);   // reconstructed "@(...)..." for func2str
    keel::i32 constIdx = appendConstant(keel::move(fh));
    emitAD(OpCode::LOAD_CONST, fhReg, constIdx);
    elemRegs.push_back(fhReg);

    for (keel::u16 outerReg : capturedOuterRegs) {
        elemRegs.push_back(outerReg);
    }

    // Pack into consecutive registers for CLOSURE_MAKE (FUNC_HANDLE with captures)
    keel::u16 base = nextReg_;
    for (keel::usize i = 0; i < totalSlots; ++i) {
        keel::u16 slot = tempReg();
        if (elemRegs[i] != slot)
            emitAB(OpCode::MOVE, slot, elemRegs[i]);
    }

    keel::u16 dst = tempReg();
    emitABC(OpCode::CLOSURE_MAKE, dst, base, keel::as<keel::u16>(totalSlots));
    return dst;
}

keel::u16 Compiler::compileSuperCall(const ASTNode *callNode, const ASTNode *superRef,
                                   keel::i32 explicitOutBase, keel::u16 nout)
{
    const keel::String &base = superRef->strValue;
    const ASTNode *lhs = superRef->children[0].get();
    // `obj@Base(args)` (lhs is a local var/param) → super-constructor;
    // `method@Base(obj,…)` (lhs is a method-name identifier) → super-method.
    const bool isCtor = lhs->type == NodeType::IDENTIFIER
                        && (varRegisters_.contains(lhs->strValue)
                            || upvalueRegs_.contains(lhs->strValue));

    // Constructor: the seed object comes from lhs; args are the ctor args.
    keel::u16 objReg = isCtor ? compileNode(lhs) : 0;

    // Compile the call args (children[1..]) into a contiguous register
    // block. Brace-index args (obj@Base(c{:})) compile expand so the
    // runtime CSL survives to the VM chain, which splats it
    // (bugs/closed/core/super-call-no-csl-splat).
    bool anyBrace = false;
    for (keel::usize i = 1; i < callNode->children.size(); ++i)
        if (callNode->children[i]->type == NodeType::CELL_INDEX) {
            anyBrace = true;
            break;
        }
    keel::Array<keel::u16> ar;
    for (keel::usize i = 1; i < callNode->children.size(); ++i)
        ar.push_back(anyBrace ? compileNodeExpand(callNode->children[i].get())
                              : compileNode(callNode->children[i].get()));
    keel::u16 argBase = nextReg_;
    for (keel::usize i = 0; i < ar.size(); ++i) {
        keel::u16 slot = tempReg();
        if (ar[i] != slot)
            emitAB(OpCode::MOVE, slot, ar[i]);
    }
    const keel::u16 nargs = keel::as<keel::u16>(ar.size());

    if (isCtor) {
        // The super-ctor initialises obj in place: MATLAB's `obj@Base(a)`
        // is a statement form — no assignment needed. Write the result
        // straight to the obj register (varRegRead returns the
        // variable's own register, so dst == objReg updates obj
        // directly; the assign form's follow-up MOVE is skipped by
        // compileAssign's dst==src check). Saves the extra temp
        // register the tempReg()+MOVE sequence spent
        // (bugs/closed/core/super-ctor-result-discarded.md).
        keel::u16 dst = (explicitOutBase >= 0) ? keel::as<keel::u16>(explicitOutBase)
                                             : objReg;
        keel::i32 baseIdx = addStringConstant(base);
        emit(Instruction::make_abcde(OpCode::CALL_SUPER_CTOR, dst, objReg, argBase, baseIdx,
                                     nargs));
        if (dst != objReg)
            emitAB(OpCode::MOVE, objReg, dst);
        emitUpvalStoreIfShared(lhs->strValue, objReg);
        return dst;
    }
    keel::u16 outBase;
    if (explicitOutBase >= 0) {
        outBase = keel::as<keel::u16>(explicitOutBase);
    } else {
        outBase = reserveRegs(std::max<keel::u16>(nout, 1));
    }
    keel::i32 qualIdx = addStringConstant(keel::format("{}>{}", base, lhs->strValue));
    emit(Instruction::make_abcde(OpCode::CALL_SUPER_METHOD, outBase, argBase, nargs, qualIdx,
                                 nout));
    return outBase;
}

} // namespace numkit
