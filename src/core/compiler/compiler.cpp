// src/core/compiler.cpp
#include <keel/core.h>
#include <keel/math.h>
#include <keel/format.h>
#include <keel/utf8.h>
#include "core/compiler/compiler.hpp"
#include <numkit/value/error.hpp>
#include <numkit/core/engine.hpp>
#include "core/engine/stack_guard.hpp"
#include "core/vm/vm.hpp"
#include "core/compiler/compiler_internal.hpp"


namespace numkit {

Compiler::Compiler(Engine &engine)
    : engine_(engine)
{}

BytecodeChunk Compiler::compile(const ASTNode *ast, keel::Arc<const keel::String> sourceCode)
{
    chunk_ = BytecodeChunk{};
    chunk_.name.assign("<script>");
    chunk_.sourceCode = keel::move(sourceCode);
    varRegisters_.clear();
    loopStack_.clear();
    constRegCache_.clear();
    pinnedConstRegs_.clear();
    scalarRegs_.reset();
    definedRegs_.reset();
    mayClear_ = false;
    mayInjectVars_ = false;
    upvalueRegs_.clear();
    allScopeUpvalueBoxRegs_.clear();
    nestedFuncInfo_.clear();
    nextReg_ = 0;
    peakReg_ = 0;
    maxVarReg_ = 0;
    currentLoc_ = {};
    isTopLevel_ = true;
    inFunctionBody_ = false;
    inFusionFallback_ = false;
    fusedOperandRegs_.clear();
    nargoutContext_ = 1;
    activeTryDepth_ = 0;
    indexContextArr_ = kInvalidReg;
    indexContextDim_ = 0;
    indexContextNdims_ = 1;

    // Inherit prior-chunk `global X` declarations so split-mode execution
    // keeps routing writes through globalsEnv_. The base workspace's global
    // membership (workspaceEnv_->globals_) is the single source of truth,
    // populated as each top-level `global X` runs on either engine.
    for (const auto &g : engine_.workspaceEnv().globalNames())
        chunk_.globalNames.push_back(g.clone());

    // Pre-import: scan AST for identifiers that exist in workspaceEnv,
    // allocate registers and emit LOAD_CONST before main code.
    // This prevents imports inside loops from resetting each iteration.
    preImportGlobals(ast);

    keel::u16 lastReg = compileNode(ast);
    emitA(OpCode::RET, lastReg);

    peepholeOptimize();

    // Use the peak nextReg_ over the chunk — nextReg_ shrinks at
    // statement boundaries (compileBlock releases temps), but the
    // runtime needs to size R[] for the highest slot ever written.
    chunk_.numRegisters = keel::as<keel::u32>(peakReg_);

    // Save variable→register mapping for environment export
    for (auto [name, reg] : varRegisters_)
        chunk_.varMap.push_back(VarRegEntry{name.clone(), reg});

    isTopLevel_ = false;
    return keel::move(chunk_);
}

// ============================================================
// Register allocation
// ============================================================

void Compiler::preImportGlobals(const ASTNode *ast)
{
    // Collect all identifiers referenced in the AST
    keel::HashSet<keel::String> identifiers;
    collectAllIdentifiers(ast, identifiers);
    // Pre-import `ans` when already defined in the enclosing workspace/scope so
    // a void statement (e.g. `disp(...)`, `set(...)`) that writes no value
    // preserves the existing `ans` instead of exporting an unset slot.
    identifiers.insert(keel::String::from("ans"));

    // Pre-allocate a register and emit LOAD_CONST for every identifier that
    // resolves to a value — including built-in constants (pi, eps, …) and
    // pseudo-vars (ans) that have been persisted or injected. Keeping the
    // LOAD_CONST at chunk entry is load-bearing: if varRegLookup lazily
    // emitted the LOAD_CONST inside a loop body on the first read of, say,
    // a shadowed `i`, the backward JMP would land on that LOAD_CONST every
    // iteration and re-initialise the variable to its import value (infinite
    // loop).
    for (const auto &name : identifiers) {
        if (varRegisters_.contains(name))
            continue; // already allocated

        Value *existing = nullptr;
        if (engine_.vm_ && engine_.vm_->inheritedScope_) {
            if (auto *dv = engine_.vm_->nextFrameDynVars()) {
                if (Value *v = dv->get(name))
                    existing = v;
            }
            if (!existing)
                existing = engine_.vm_->inheritedScope_->get(name);
        } else {
            existing = engine_.getVariable(name);
        }
        // Import any defined value (including `[]` empty matrices, which
        // are legal MATLAB values). Only unset / deleted slots get skipped.
        if (existing && !existing->isUnset() && !existing->isDeleted()) {
            if (nextReg_ >= keel::as<keel::i32>(kInvalidReg))
                throw numkit::Error(
                    "Compiler: register limit exceeded (>65534 registers in chunk)");
            keel::u16 r = keel::as<keel::u16>(nextReg_++);
            if (nextReg_ > peakReg_) peakReg_ = nextReg_;
            if (nextReg_ > maxVarReg_) maxVarReg_ = nextReg_;
            varRegisters_.put(name.clone(), r);
            keel::i32 idx = appendConstant(*existing);
            emitAD(OpCode::LOAD_CONST, r, idx);
            definedRegs_.set(r);
        }
    }

    // Pre-allocate slots for every variable that gets WRITTEN anywhere
    // in the AST. Without this, a variable's slot is whatever happened
    // to be free when its first assignment was compiled — high if the
    // RHS used many temps. With statement-boundary temp release in
    // compileBlock that puts variables fragmented across the slot
    // range, wasting space below maxVarReg_ and exhausting the 256-slot
    // chunk on workloads that don't otherwise need that many slots.
    // Pre-allocating clusters all variables at LOW slots and leaves
    // the high range free for transient temps. No code is emitted —
    // values land via assignment as usual.
    preallocateAssignedVars(ast);
    preallocatePinnedConstants(ast);
}

void Compiler::preallocateAssignedVars(const ASTNode *body)
{
    keel::Array<keel::String> assignedNames;
    collectAssignedNames(body, assignedNames);
    for (const auto &name : assignedNames) {
        if (varRegisters_.contains(name))
            continue;
        if (engine_.isReservedName(name))
            continue; // reserved/pseudo-vars are looked up on demand
        if (nextReg_ >= keel::as<keel::i32>(kInvalidReg))
            throw numkit::Error(
                "Compiler: register limit exceeded (>65534 registers in chunk)");
        keel::u16 r = keel::as<keel::u16>(nextReg_++);
        if (nextReg_ > peakReg_) peakReg_ = nextReg_;
        if (nextReg_ > maxVarReg_) maxVarReg_ = nextReg_;
        varRegisters_.put(name.clone(), r);
    }
}

namespace {
void scanAstForLoopsAndClear(const ASTNode *node, bool inLoop, bool &mayClear,
                             bool &mayInjectVars, keel::Array<keel::f64> &loopNums)
{
    if (!node)
        return;
    if (node->type == NodeType::FUNCTION_DEF || node->type == NodeType::ANON_FUNC)
        return;
    if (node->type == NodeType::IDENTIFIER || node->type == NodeType::COMMAND_CALL) {
        const keel::String &s = node->strValue;
        if (s == "clear" || s == "clearvars" || s == "eval" || s == "evalin" || s == "run")
            mayClear = true;
        if (s == "load" || s == "eval" || s == "evalin" || s == "run")
            mayInjectVars = true;
    } else if (node->type == NodeType::CALL && !node->children.empty()
               && node->children[0]->type == NodeType::IDENTIFIER) {
        const keel::String &s = node->children[0]->strValue;
        if (s == "clear" || s == "clearvars" || s == "eval" || s == "evalin" || s == "run")
            mayClear = true;
        if (s == "load" || s == "eval" || s == "evalin" || s == "run")
            mayInjectVars = true;
    } else if (inLoop && node->type == NodeType::NUMBER_LITERAL && loopNums.size() < 16) {
        bool exists = false;
        for (keel::f64 v : loopNums) {
            if (v == node->numValue && keel::signbit(v) == keel::signbit(node->numValue)) {
                exists = true;
                break;
            }
        }
        if (!exists)
            loopNums.push_back(node->numValue);
    }

    const bool childInLoop = inLoop || node->type == NodeType::FOR_STMT
                             || node->type == NodeType::WHILE_STMT;
    for (const auto &child : node->children)
        scanAstForLoopsAndClear(child.get(), childInLoop, mayClear, mayInjectVars, loopNums);
    for (const auto &t : node->lhsTargets)
        scanAstForLoopsAndClear(t.get(), childInLoop, mayClear, mayInjectVars, loopNums);
    for (const auto &branch : node->branches) {
        scanAstForLoopsAndClear(branch.cond.get(), childInLoop, mayClear, mayInjectVars, loopNums);
        scanAstForLoopsAndClear(branch.body.get(), childInLoop, mayClear, mayInjectVars, loopNums);
    }
    if (node->elseBranch)
        scanAstForLoopsAndClear(node->elseBranch.get(), childInLoop, mayClear, mayInjectVars, loopNums);
}
} // namespace

void Compiler::preallocatePinnedConstants(const ASTNode *body)
{
    keel::Array<keel::f64> loopNums;
    scanAstForLoopsAndClear(body, false, mayClear_, mayInjectVars_, loopNums);
    if (mayClear_)
        definedRegs_.reset();
    for (keel::f64 v : loopNums) {
        if (nextReg_ >= 65000 || pinnedConstRegs_.size() >= 16)
            break;
        keel::i32 idx = addConstant(v);
        if (pinnedConstRegs_.contains(idx))
            continue;
        keel::u16 r = keel::as<keel::u16>(nextReg_++);
        if (nextReg_ > peakReg_) peakReg_ = nextReg_;
        if (nextReg_ > maxVarReg_) maxVarReg_ = nextReg_;
        emitAD(OpCode::LOAD_CONST, r, idx);
        pinnedConstRegs_.put(idx, r);
    }
}

void Compiler::collectAssignedNames(const ASTNode *node, keel::Array<keel::String> &out)
{
    if (!node)
        return;
    // Don't descend into nested function definitions — they have their
    // own scope and compile separately.
    if (node->type == NodeType::FUNCTION_DEF)
        return;

    auto pushIdent = [&out](const ASTNode *target) {
        if (target && target->type == NodeType::IDENTIFIER
            && !target->strValue.empty())
            out.push_back(target->strValue.clone());
    };

    if (node->type == NodeType::ASSIGN && !node->children.empty()) {
        // Simple `name = expr` (also covers a(idx)=... where LHS is
        // INDEX/CALL → identifier is children[0]->children[0]).
        const ASTNode *lhs = node->children[0].get();
        if (lhs->type == NodeType::IDENTIFIER) {
            pushIdent(lhs);
        } else if ((lhs->type == NodeType::INDEX || lhs->type == NodeType::CALL
                    || lhs->type == NodeType::FIELD_ACCESS
                    || lhs->type == NodeType::DYNAMIC_FIELD_ACCESS
                    || lhs->type == NodeType::CELL_INDEX)
                   && !lhs->children.empty()) {
            pushIdent(lhs->children[0].get());
        }
    }
    if (node->type == NodeType::MULTI_ASSIGN) {
        for (const auto &name : node->returnNames)
            if (name != "~" && !name.empty())
                out.push_back(name.clone());
        // Complex targets ([s.f, a(i)] = ...) carry their lvalue here;
        // the mutated variable is the chain root.
        for (const auto &t : node->lhsTargets) {
            const ASTNode *root = t.get();
            while (root && root->type != NodeType::IDENTIFIER
                   && !root->children.empty())
                root = root->children[0].get();
            if (root && root->type == NodeType::IDENTIFIER)
                out.push_back(root->strValue.clone());
        }
    }
    if (node->type == NodeType::FOR_STMT && !node->strValue.empty())
        out.push_back(node->strValue.clone());
    if (node->type == NodeType::COMMAND_CALL && engine_.isVarDeclCommand(node->strValue)) {
        keel::Array<keel::StringRef> rawArgs;
        rawArgs.reserve(node->children.size());
        for (const auto &ch : node->children)
            rawArgs.push_back(ch->strValue.as_ref());
        engine_.extractVarDeclCommandNames(node->strValue, rawArgs.as_ref(), out);
    }
    if (node->type == NodeType::CALL && !node->children.empty()
        && node->children[0]->type == NodeType::IDENTIFIER
        && engine_.isVarDeclCommand(node->children[0]->strValue)) {
        keel::Array<keel::StringRef> rawArgs;
        for (keel::usize i = 1; i < node->children.size(); ++i) {
            const auto *ch = node->children[i].get();
            if (ch->type == NodeType::STRING_LITERAL && !ch->strValue.empty())
                rawArgs.push_back(ch->strValue.as_ref());
        }
        engine_.extractVarDeclCommandNames(node->children[0]->strValue, rawArgs.as_ref(), out);
    }

    for (const auto &child : node->children)
        collectAssignedNames(child.get(), out);
    for (const auto &branch : node->branches) {
        collectAssignedNames(branch.cond.get(), out);
        collectAssignedNames(branch.body.get(), out);
    }
    if (node->elseBranch)
        collectAssignedNames(node->elseBranch.get(), out);
}

void Compiler::collectAllIdentifiers(const ASTNode *node, keel::HashSet<keel::String> &out)
{
    if (!node)
        return;
    // Don't recurse into function definitions — they compile with separate scope
    if (node->type == NodeType::FUNCTION_DEF)
        return;
    auto insertIdent = [&out](const keel::String &s) {
        if (!out.contains(s))
            out.insert(s.clone());
    };
    if (node->type == NodeType::IDENTIFIER)
        insertIdent(node->strValue);
    // Also collect from CALL target names
    if (node->type == NodeType::CALL && !node->children.empty()
        && node->children[0]->type == NodeType::IDENTIFIER)
        insertIdent(node->children[0]->strValue);
    // Recurse
    for (const auto &child : node->children)
        collectAllIdentifiers(child.get(), out);
    // MULTI_ASSIGN complex targets ([a(i), s.(e)] = ...) read variables
    // (indices, dynamic field names) that live only under lhsTargets.
    for (const auto &t : node->lhsTargets)
        collectAllIdentifiers(t.get(), out);
    for (const auto &branch : node->branches) {
        collectAllIdentifiers(branch.cond.get(), out);
        collectAllIdentifiers(branch.body.get(), out);
    }
    if (node->elseBranch)
        collectAllIdentifiers(node->elseBranch.get(), out);
    // Also strValue for for-loop variable, function names, etc.
    if (node->type == NodeType::FOR_STMT && !node->strValue.empty())
        insertIdent(node->strValue);
}

keel::u16 Compiler::varRegWrite(keel::StringRef name)
{
    markAssigned(name);
    // A write target (param / assignment) gets its value from the call or the
    // assignment — never the reserved-constant pre-load (which would clobber a
    // param named `i`/`j` with the imaginary unit).
    return varRegLookup(name, /*preloadReserved=*/false);
}

keel::u16 Compiler::varRegLookup(keel::StringRef name, bool preloadReserved)
{
    if (const keel::u16 *found = varRegisters_.get(name))
        return *found;
    keel::i32 allocReg = (nextReg_ > peakReg_) ? nextReg_ : peakReg_;
    if (allocReg >= keel::as<keel::i32>(kInvalidReg))
        throw numkit::Error(
            "Compiler: register limit exceeded (>65534 registers in chunk)");
    keel::u16 r = keel::as<keel::u16>(allocReg);
    peakReg_ = allocReg + 1;
    nextReg_ = allocReg + 1;
    if (nextReg_ > maxVarReg_)
        maxVarReg_ = nextReg_;
    varRegisters_.put(keel::String::from(name), r);

    // Pre-load a value from the engine for any reserved name that happens
    // to be resolvable — this handles both true built-in constants
    // (pi/eps/… from constantsEnv_) and pseudo-vars that higher layers
    // have injected into workspaceEnv (e.g. the debug-console eval
    // injecting `nargin` so the user can inspect it from K>>).
    if (preloadReserved && engine_.isReservedName(name)) {
        Value *existing = isTopLevel_ ? engine_.getVariable(name)
                                      : engine_.constantsEnv().get(name);
        if (!existing)
            existing = engine_.getVariable(name);
        if (existing && !existing->isEmpty()) {
            keel::i32 idx = appendConstant(*existing);
            emitAD(OpCode::LOAD_CONST, r, idx);
            if (!mayClear_)
                definedRegs_.set(r);
        }
    }

    return r;
}

keel::u16 Compiler::allocPinnedReg()
{
    keel::i32 allocReg = (nextReg_ > peakReg_) ? nextReg_ : peakReg_;
    if (allocReg >= keel::as<keel::i32>(kInvalidReg))
        throw numkit::Error(
            "Compiler: register limit exceeded (>65534 registers in chunk)");
    keel::u16 r = keel::as<keel::u16>(allocReg);
    peakReg_ = allocReg + 1;
    nextReg_ = allocReg + 1;
    if (nextReg_ > maxVarReg_)
        maxVarReg_ = nextReg_;
    return r;
}

void Compiler::emitUpvalLoadRawIfShared(keel::StringRef name, keel::u16 reg)
{
    const keel::u16 *uv = upvalueRegs_.get(name);
    if (!uv)
        return;
    emit(Instruction::make_abcde(OpCode::UPVAL_LOAD, reg, *uv, 1, 0, 0));
}

void Compiler::emitUpvalStoreIfShared(keel::StringRef name, keel::u16 reg)
{
    const keel::u16 *uv = upvalueRegs_.get(name);
    if (!uv)
        return;
    emitAB(OpCode::UPVAL_STORE, *uv, reg);
}

keel::u16 Compiler::emitNestedFuncHandle(const NestedFuncInfo &info)
{
    if (info.upvalueNames.empty()) {
        keel::u16 dst = tempReg();
        Value fh = Value::funcHandle(info.compiledName, engine_.allocator());
        fh.setFuncHandleSource(info.displaySource);
        keel::i32 constIdx = appendConstant(keel::move(fh));
        emitAD(OpCode::LOAD_CONST, dst, constIdx);
        return dst;
    }

    keel::usize totalSlots = 1 + info.upvalueNames.size();
    keel::u16 fhReg = tempReg();
    Value fh = Value::funcHandle(info.compiledName, engine_.allocator());
    fh.setFuncHandleSource(info.displaySource);
    keel::i32 constIdx = appendConstant(keel::move(fh));
    emitAD(OpCode::LOAD_CONST, fhReg, constIdx);

    keel::u16 base = keel::as<keel::u16>(nextReg_);
    {
        keel::u16 slot0 = tempReg();
        if (fhReg != slot0)
            emitAB(OpCode::MOVE, slot0, fhReg);
    }
    for (const auto &uvName : info.upvalueNames) {
        keel::u16 slot = tempReg();
        const keel::u16 *boxPtr = allScopeUpvalueBoxRegs_.get(uvName);
        if (!boxPtr)
            throw numkit::Error("Compiler: missing upvalue box register");
        keel::u16 boxReg = *boxPtr;
        if (boxReg != slot)
            emitAB(OpCode::MOVE, slot, boxReg);
    }
    keel::u16 dst = tempReg();
    emitABC(OpCode::CLOSURE_MAKE, dst, base, keel::as<keel::u16>(totalSlots));
    return dst;
}

Compiler::ResolvedSymbol Compiler::resolveSymbol(keel::StringRef name) const
{
    if (const keel::u16 *uv = upvalueRegs_.get(name))
        return {ResolvedSymbol::Kind::Upvalue, *uv, nullptr, name};
    if (const keel::u16 *vr = varRegisters_.get(name))
        return {ResolvedSymbol::Kind::LocalVar, *vr, nullptr, name};
    if (const NestedFuncInfo *nit = nestedFuncInfo_.get(name))
        return {ResolvedSymbol::Kind::NestedFunc, kInvalidReg, nit, nit->compiledName.as_ref()};
    if (const keel::StringRef sub = resolveFileSubfunc(name); !sub.empty())
        return {ResolvedSymbol::Kind::FileSubfunc, kInvalidReg, nullptr, sub};
    if (isTopLevel_ && !engine_.isReservedName(name)) {
        if (Value *existing = engine_.getVariable(name); existing != nullptr)
            return {ResolvedSymbol::Kind::WorkspaceVar, kInvalidReg, nullptr, name};
    }
    if (engine_.isReservedName(name)) {
        const bool topLevelPseudo = isTopLevel_ && (name == "nargin" || name == "nargout");
        if (!topLevelPseudo)
            return {ResolvedSymbol::Kind::Reserved, kInvalidReg, nullptr, name};
    }
    const BuiltinClass *kls = engine_.findClass(name);
    if (engine_.hasFunction(name) || engine_.isKnownLeafName(name) || (kls && kls->construct))
        return {ResolvedSymbol::Kind::KnownFunc, kInvalidReg, nullptr, name};
    return {ResolvedSymbol::Kind::Dynamic, kInvalidReg, nullptr, name};
}

keel::u16 Compiler::varRegRead(keel::StringRef name)
{
    const ResolvedSymbol sym = resolveSymbol(name);
    switch (sym.kind) {
    case ResolvedSymbol::Kind::Upvalue: {
        keel::u16 reg = varRegLookup(name, /*preloadReserved=*/false);
        keel::i32 nameIdx = addStringConstant(name);
        emit(Instruction::make_abcde(OpCode::UPVAL_LOAD, reg, sym.reg, 0, nameIdx, 0));
        scalarRegs_.reset(reg);
        return reg;
    }
    case ResolvedSymbol::Kind::LocalVar: {
        if (!engine_.isReservedName(name)) {
            if (mayClear_ || !definedRegs_.test(sym.reg)) {
                keel::i32 nameIdx = addStringConstant(name);
                emit(Instruction::make_abcde(OpCode::ASSERT_DEF, sym.reg, sym.reg, 0, nameIdx, nargoutContext_));
                if (!mayClear_ && sym.reg < chunk_.numParams)
                    definedRegs_.set(sym.reg);
            }
        }
        return sym.reg;
    }
    case ResolvedSymbol::Kind::NestedFunc: {
        const NestedFuncInfo *nit = sym.nested;
        keel::u16 argBase = keel::as<keel::u16>(nextReg_);
        for (const auto &uvName : nit->upvalueNames) {
            keel::u16 slot = tempReg();
            const keel::u16 *boxPtr = allScopeUpvalueBoxRegs_.get(uvName);
            if (!boxPtr)
                throw numkit::Error("Compiler: missing upvalue box register");
            keel::u16 boxReg = *boxPtr;
            if (boxReg != slot)
                emitAB(OpCode::MOVE, slot, boxReg);
        }
        keel::u16 dst = tempReg();
        keel::i32 funcIdx = addStringConstant(nit->compiledName);
        emit(Instruction::make_abcde(OpCode::CALL, dst, argBase,
                                     keel::as<keel::u16>(nit->upvalueNames.size()),
                                     funcIdx, nargoutContext_));
        return dst;
    }
    case ResolvedSymbol::Kind::FileSubfunc:
    case ResolvedSymbol::Kind::KnownFunc: {
        if (!mayInjectVars_) {
            keel::u16 argBase = nextReg_;
            keel::u16 dst = tempReg();
            keel::i32 funcIdx = addStringConstant(sym.targetName);
            emit(Instruction::make_abcde(OpCode::CALL, dst, argBase, 0, funcIdx, nargoutContext_));
            return dst;
        }
        // Under mayInjectVars_, check dynamic vars before falling back to 0-arg call
        // without registering `name` in varRegisters_ (keeps subsequent `name(arg)` calls intact).
        keel::u16 dst = tempReg();
        keel::i32 nameIdx = addStringConstant(name);
        emit(Instruction::make_abcde(OpCode::ASSERT_DEF, dst, 0, 1, nameIdx, nargoutContext_));
        return dst;
    }
    case ResolvedSymbol::Kind::WorkspaceVar:
    case ResolvedSymbol::Kind::Reserved:
        return varRegLookup(name);
    case ResolvedSymbol::Kind::Dynamic: {
        keel::u16 reg = varRegLookup(name);
        keel::i32 nameIdx = addStringConstant(name);
        emit(Instruction::make_abcde(OpCode::ASSERT_DEF, reg, reg, 0, nameIdx, nargoutContext_));
        return reg;
    }
    }
    return kInvalidReg;
}

keel::u16 Compiler::tempReg()
{
    // Bytecode reg fields are uint16_t — max 256 distinct slots (0..255).
    // numRegisters is uint16_t, so its count must fit too: cap at 255.
    // Throw early rather than wrap nextReg_ and silently corrupt slot 0+.
    if (nextReg_ >= keel::as<keel::i32>(kInvalidReg))
        throw numkit::Error(
            "Compiler: register limit exceeded (>65534 registers in chunk)");
    keel::u16 r = keel::as<keel::u16>(nextReg_++);
    if (nextReg_ > peakReg_)
        peakReg_ = nextReg_;
    return r;
}

keel::u16 Compiler::reserveRegs(keel::usize n)
{
    keel::u16 base = nextReg_;
    for (keel::usize i = 0; i < n; ++i)
        tempReg();
    return base;
}

// ============================================================
// Peephole optimization
// ============================================================

void Compiler::peepholeOptimize()
{
    // No peephole pass runs. A MOVE-elimination + NOP-stripping pass was
    // implemented here but is intentionally disabled: it needs thorough
    // liveness analysis to avoid corrupting register state, and the
    // constant-register cache in compileNumber() already removes most
    // redundant LOAD_CONST instructions. The full (disabled) implementation
    // lives in git history if a future pass wants a starting point; this
    // stub is kept as the pipeline hook (called from compile()).
}

// ============================================================
// Emit helpers
// ============================================================

void Compiler::emit(Instruction instr)
{
    chunk_.code.push_back(instr);
    chunk_.sourceMap.push_back(currentLoc_);
}

void Compiler::emitABC(OpCode op, keel::u16 a, keel::u16 b, keel::u16 c)
{
    emit(Instruction::make_abc(op, a, b, c));
}

void Compiler::emitAB(OpCode op, keel::u16 a, keel::u16 b)
{
    emit(Instruction::make_abc(op, a, b, 0));
}

void Compiler::emitAD(OpCode op, keel::u16 a, keel::i32 d)
{
    emit(Instruction::make_ad(op, a, d));
}

void Compiler::emitA(OpCode op, keel::u16 a)
{
    emit(Instruction::make_a(op, a));
}

void Compiler::emitD(OpCode op, keel::i32 d)
{
    emit(Instruction::make_d(op, d));
}

void Compiler::emitNone(OpCode op)
{
    emit(Instruction::make_none(op));
}

keel::usize Compiler::currentPos() const
{
    return chunk_.code.size();
}

void Compiler::patchJump(keel::usize instrPos, keel::i32 offset)
{
    chunk_.code[instrPos].d = offset;
}

// ============================================================
// Constant pool
// ============================================================

// The single choke point for pool growth: the int32 index space is checked
// here so no caller can wrap it silently (PVS V1029 family).
keel::i32 Compiler::appendConstant(Value v)
{
    if (chunk_.constants.size() >= keel::as<keel::usize>(keel::Limits<keel::i32>::kMax))
        throw numkit::Error(
            "Compiler: constant pool limit exceeded (>2^31-1 entries in one chunk)");
    keel::i32 idx = keel::as<keel::i32>(chunk_.constants.size());
    chunk_.constants.push_back(keel::move(v));
    return idx;
}

keel::i32 Compiler::appendString(keel::StringRef s)
{
    if (chunk_.strings.size() >= keel::as<keel::usize>(keel::Limits<keel::i32>::kMax))
        throw numkit::Error(
            "Compiler: string pool limit exceeded (>2^31-1 entries in one chunk)");
    keel::i32 idx = keel::as<keel::i32>(chunk_.strings.size());
    chunk_.strings.push_back(keel::String::from(s));
    return idx;
}

keel::i32 Compiler::addConstant(keel::f64 value)
{
    // Check if constant already exists (preserve -0.0 vs +0.0 signbit)
    for (keel::usize i = 0; i < chunk_.constants.size(); ++i) {
        if (chunk_.constants[i].isScalar() && chunk_.constants[i].type() == ValueType::DOUBLE) {
            keel::f64 cv = chunk_.constants[i].toScalar();
            if (cv == value && keel::signbit(cv) == keel::signbit(value))
                return keel::as<keel::i32>(i);
        }
    }
    return appendConstant(Value::scalar(value, engine_.allocator()));
}

keel::i32 Compiler::addStringConstant(keel::StringRef s)
{
    for (keel::usize i = 0; i < chunk_.strings.size(); ++i) {
        if (chunk_.strings[i] == s)
            return keel::as<keel::i32>(i);
    }
    return appendString(s);
}

// ============================================================
// AST compilation
// ============================================================

keel::u16 Compiler::compileNode(const ASTNode *node)
{
    if (node && !fusedOperandRegs_.empty()) {
        if (const keel::u16 *r = fusedOperandRegs_.get(node))
            return *r;
    }
    // Single-value sink: a brace cell-index c{sub} may produce a comma-separated list
    // once the producer emits one; collapse it in place so single-value contexts never
    // see a CSL. COLLAPSE is a runtime no-op on a non-CSL, so this is inert until the
    // CELL_GET producer is flipped (brick 5b). Splice contexts call compileNodeExpand
    // directly and skip this. See dev-docs/memory/csl_first_class.md.
    keel::u16 reg = compileNodeExpand(node);
    if (node && (node->type == NodeType::CELL_INDEX
                 || node->type == NodeType::FIELD_ACCESS
                 || node->type == NodeType::DYNAMIC_FIELD_ACCESS))
        emitAB(OpCode::COLLAPSE, reg, reg);
    return reg;
}

keel::u16 Compiler::compileNodeExpand(const ASTNode *node)
{
    if (!node)
        return 0;
    if (!fusedOperandRegs_.empty()) {
        if (const keel::u16 *r = fusedOperandRegs_.get(node))
            return *r;
    }

    StackGuard::check("bytecode compilation");

    // Update current source location from AST node (if it has valid line info)
    if (node->line > 0) {
        currentLoc_.line = keel::as<keel::u16>(node->line);
        currentLoc_.col = keel::as<keel::u16>(node->col);
    }

    switch (node->type) {
    case NodeType::BLOCK:
        return compileBlock(node);
    case NodeType::NUMBER_LITERAL:
        return compileNumber(node);
    case NodeType::IMAG_LITERAL: {
        // 3i → complexScalar(0, 3)
        keel::u16 dst = tempReg();
        keel::i32 idx = appendConstant(
            Value::complexScalar(0.0, node->numValue, nullptr));
        emitAD(OpCode::LOAD_CONST, dst, idx);
        return dst;
    }
    case NodeType::STRING_LITERAL:
        return compileString(node);
    case NodeType::DQSTRING_LITERAL: {
        keel::u16 dst = tempReg();
        keel::i32 idx = appendConstant(Value::stringScalar(node->strValue, nullptr));
        emitAD(OpCode::LOAD_CONST, dst, idx);
        return dst;
    }
    case NodeType::BOOL_LITERAL:
        return compileBool(node);
    case NodeType::IDENTIFIER:
        return compileIdentifier(node);
    case NodeType::ASSIGN:
        return compileAssign(node);
    case NodeType::MULTI_ASSIGN:
        return compileMultiAssign(node);
    case NodeType::BINARY_OP:
        return compileBinaryOp(node);
    case NodeType::UNARY_OP:
        return compileUnaryOp(node);
    case NodeType::EXPR_STMT:
        return compileExprStmt(node);
    case NodeType::IF_STMT:
        return compileIf(node);
    case NodeType::SWITCH_STMT:
        return compileSwitch(node);
    case NodeType::TRY_STMT:
        return compileTryCatch(node);
    case NodeType::GLOBAL_STMT:
    case NodeType::PERSISTENT_STMT:
        return compileGlobalPersistent(node);
    case NodeType::FIELD_ACCESS:
        return compileFieldAccess(node);
    case NodeType::DYNAMIC_FIELD_ACCESS: {
        // s.(expr) — child[0]=obj, child[1]=field name expr
        keel::u16 obj = compileNode(node->children[0].get());
        keel::u16 nameReg = compileNode(node->children[1].get());
        keel::u16 dst = tempReg();
        emitABC(OpCode::FIELD_GET_DYN, dst, obj, nameReg);
        return dst;
    }
    case NodeType::CELL_INDEX:
        return compileCellIndex(node);
    case NodeType::CELL_LITERAL:
        return compileCellLiteral(node);
    case NodeType::ANON_FUNC:
        return compileAnonFunc(node);
    case NodeType::WHILE_STMT:
        return compileWhile(node);
    case NodeType::BREAK_STMT:
        return compileBreak(node);
    case NodeType::CONTINUE_STMT:
        return compileContinue(node);
    case NodeType::FOR_STMT:
        return compileFor(node);
    case NodeType::COLON_EXPR:
        return compileColonExpr(node);
    case NodeType::MATRIX_LITERAL:
        return compileMatrixLiteral(node);
    case NodeType::INDEX:
        return compileIndexExpr(node);
    case NodeType::CALL:
        return compileCall(node);
    case NodeType::COMMAND_CALL:
        return compileCommandCall(node);
    case NodeType::FUNCTION_DEF:
        return compileFunctionDef(node);
    case NodeType::CLASSDEF_DEF:
        engine_.registerClassDef(node); // register the class; emits no code
        return 0;
    case NodeType::RETURN_STMT:
        return compileReturn(node);
    case NodeType::SUPERCLASS_REF: {
        keel::u16 msgReg = tempReg();
        keel::String msg = keel::format(
            "superclass reference '{}' must be called: use lhs@{}(...)",
            node->strValue, node->strValue);
        keel::i32 msgIdx = addStringConstant(msg);
        emitAD(OpCode::LOAD_STRING, msgReg, msgIdx);
        emitA(OpCode::THROW, msgReg);
        return msgReg;
    }
    case NodeType::END_VAL: {
        if (indexContextArr_ == kInvalidReg) {
            keel::u16 msgReg = tempReg();
            keel::i32 msgIdx = addStringConstant("'end' used outside of indexing context");
            emitAD(OpCode::LOAD_STRING, msgReg, msgIdx);
            emitA(OpCode::THROW, msgReg);
            return msgReg;
        }
        keel::u16 dst = tempReg();
        emit(Instruction::make_abcde(OpCode::LOAD_END,
                                     dst,
                                     indexContextArr_,
                                     indexContextDim_,
                                     keel::as<keel::i32>(indexContextNdims_),
                                     0));
        return dst;
    }
    case NodeType::DELETE_ASSIGN:
        return compileDeleteAssign(node);
    default:
        throw numkit::Error(
            keel::format("Compiler: unsupported node type {}",
                         keel::as<keel::i32>(node->type)).c_str());
    }
}


} // namespace numkit
