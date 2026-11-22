// src/core/compiler.cpp
#include <keel/core.h>
#include <keel/format.h>
#include <keel/utf8.h>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/core/vm.hpp>
#include "compiler_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

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
                throw std::runtime_error(
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
            throw std::runtime_error(
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
            if (v == node->numValue && std::signbit(v) == std::signbit(node->numValue)) {
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
        throw std::runtime_error(
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
        throw std::runtime_error(
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
            throw std::runtime_error("Compiler: missing upvalue box register");
        keel::u16 boxReg = *boxPtr;
        if (boxReg != slot)
            emitAB(OpCode::MOVE, slot, boxReg);
    }
    keel::u16 dst = tempReg();
    emitABC(OpCode::CLOSURE_MAKE, dst, base, keel::as<keel::u16>(totalSlots));
    return dst;
}

keel::u16 Compiler::varRegRead(keel::StringRef name)
{
    // Shared lexical upvalue read (outer <-> nested function)
    if (const keel::u16 *uv = upvalueRegs_.get(name)) {
        keel::u16 reg = varRegLookup(name, /*preloadReserved=*/false);
        keel::i32 nameIdx = addStringConstant(name);
        emit(Instruction::make_abcde(OpCode::UPVAL_LOAD, reg, *uv, 0, nameIdx, 0));
        scalarRegs_.reset(reg);
        return reg;
    }

    // Check if variable is already known (local scope)
    if (const keel::u16 *vr = varRegisters_.get(name)) {
        if (!engine_.isReservedName(name)) {
            if (mayClear_ || !definedRegs_.test(*vr)) {
                keel::i32 nameIdx = addStringConstant(name);
                emitAD(OpCode::ASSERT_DEF, *vr, nameIdx);
                if (!mayClear_ && *vr < chunk_.numParams)
                    definedRegs_.set(*vr);
            }
        }
        return *vr;
    }

    // Zero-arg nested function call in expression context (`y = inner`)
    if (const NestedFuncInfo *nit = nestedFuncInfo_.get(name)) {
        keel::u16 argBase = keel::as<keel::u16>(nextReg_);
        for (const auto &uvName : nit->upvalueNames) {
            keel::u16 slot = tempReg();
            const keel::u16 *boxPtr = allScopeUpvalueBoxRegs_.get(uvName);
            if (!boxPtr)
                throw std::runtime_error("Compiler: missing upvalue box register");
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

    // Check workspaceEnv
    if (isTopLevel_ && !engine_.isReservedName(name)) {
        Value *existing = engine_.getVariable(name);
        if (existing)
            return varRegLookup(name); // will import from workspaceEnv (including empty values)
    }

    if (engine_.isReservedName(name)) {
        const bool topLevelPseudo = isTopLevel_ && (name == "nargin" || name == "nargout");
        if (!topLevelPseudo)
            return varRegLookup(name);
    }

    // Check if it's a known function — emit zero-arg CALL unless dynamic
    // variable injection (`load`/`eval`/`evalin`/`run`) may shadow it in frame.env,
    // in which case ASSERT_DEF checks dynVars/frame.env first and falls back to 0-arg CALL.
    const BuiltinClass *kls = engine_.findClass(name);
    if (!mayInjectVars_
        && (engine_.hasFunction(name) || engine_.isKnownLeafName(name)
            || (kls && kls->construct))) {
        keel::u16 argBase = nextReg_;
        keel::u16 dst = tempReg();
        keel::i32 funcIdx = addStringConstant(name);
        emit(Instruction::make_abcde(OpCode::CALL, dst, argBase, 0, funcIdx, nargoutContext_));
        return dst;
    }

    // Unknown variable — allocate register and emit runtime check.
    keel::u16 reg = varRegLookup(name);
    keel::i32 nameIdx = addStringConstant(name);
    emitAD(OpCode::ASSERT_DEF, reg, nameIdx);
    return reg;
}

keel::u16 Compiler::tempReg()
{
    // Bytecode reg fields are uint16_t — max 256 distinct slots (0..255).
    // numRegisters is uint16_t, so its count must fit too: cap at 255.
    // Throw early rather than wrap nextReg_ and silently corrupt slot 0+.
    if (nextReg_ >= keel::as<keel::i32>(kInvalidReg))
        throw std::runtime_error(
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
    if (chunk_.constants.size() >= keel::as<keel::usize>(INT32_MAX))
        throw std::runtime_error(
            "Compiler: constant pool limit exceeded (>2^31-1 entries in one chunk)");
    keel::i32 idx = keel::as<keel::i32>(chunk_.constants.size());
    chunk_.constants.push_back(keel::move(v));
    return idx;
}

keel::i32 Compiler::appendString(keel::StringRef s)
{
    if (chunk_.strings.size() >= keel::as<keel::usize>(INT32_MAX))
        throw std::runtime_error(
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
            if (cv == value && std::signbit(cv) == std::signbit(value))
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
        throw std::runtime_error(
            keel::format("Compiler: unsupported node type {}",
                         keel::as<keel::i32>(node->type)).c_str());
    }
}

keel::u16 Compiler::compileBlock(const ASTNode *node)
{
    // Release transient temps allocated during each statement so
    // subsequent statements can reuse those slots. Without this, a
    // long block (e.g. the heavy try-body in audio_cepstral) bumps
    // nextReg_ past the 256-slot bytecode limit; the old code silently
    // wrapped uint16_t and corrupted variable slot 0.
    //
    // Floor is max(entryNext, maxVarReg_):
    //   * entryNext preserves temps that the *parent* compileX
    //     allocated before invoking us (e.g. compileSwitch's
    //     switchReg/cmpReg live across case bodies).
    //   * maxVarReg_ preserves any *new* variable slot the statement
    //     introduced via varRegLookup or the canEliminate adopt-temp
    //     path in compileAssign.
    // Cached LOAD_CONST regs above the floor are released and must be
    // re-emitted by future statements; that's safe and cheap.
    //
    // CRITICAL: we must also emit runtime CLEAR_VAR for each released
    // slot. Otherwise the runtime register file keeps the previous
    // statement's value in that slot, and a subsequent varRegLookup
    // for an undefined variable will allocate the same slot and see
    // a stale value — ASSERT_DEF then passes incorrectly. (Nested
    // try/catch with undefined variables exposed this: the inner
    // catch's LOAD_CONST(1) at slot S survived after release, and a
    // following `r + undef` re-allocated S for `undef`, so ASSERT_DEF
    // saw R[S]=1 and never threw — turning the expected outer-catch
    // into a silent computation of `r + 1`.)
    const keel::i32 entryNext = nextReg_;
    keel::u16 last = 0;
    for (auto &child : node->children) {
        last = compileNode(child.get());
        const keel::i32 floor = entryNext > maxVarReg_ ? entryNext : maxVarReg_;
        if (nextReg_ > floor) {
            nextReg_ = floor;
            constRegCache_.clear();
            scalarRegs_.reset();
        }
    }
    return last;
}

keel::u16 Compiler::compileNumber(const ASTNode *node)
{
    keel::i32 idx = addConstant(node->numValue);
    if (const keel::u16 *pr = pinnedConstRegs_.get(idx)) {
        scalarRegs_.set(*pr);
        return *pr;
    }
    // Reuse register if this constant was already loaded
    if (const keel::u16 *cr = constRegCache_.get(idx)) {
        scalarRegs_.set(*cr);
        return *cr;
    }
    keel::u16 dst = tempReg();
    emitAD(OpCode::LOAD_CONST, dst, idx);
    constRegCache_.put(idx, dst);
    scalarRegs_.set(dst);
    return dst;
}

keel::u16 Compiler::compileString(const ASTNode *node)
{
    keel::u16 dst = tempReg();
    keel::i32 idx = addStringConstant(node->strValue);
    emitAD(OpCode::LOAD_STRING, dst, idx);
    return dst;
}

keel::u16 Compiler::compileBool(const ASTNode *node)
{
    keel::u16 dst = tempReg();
    // Store as logical (tag-based, no heap allocation) for islogical() correctness
    keel::i32 idx = appendConstant(Value::logicalScalar(node->boolValue, nullptr));
    emitAD(OpCode::LOAD_CONST, dst, idx);
    return dst;
}

keel::u16 Compiler::compileIdentifier(const ASTNode *node)
{
    return varRegRead(node->strValue);
}

keel::u16 Compiler::compileAssign(const ASTNode *node)
{
    auto *lhs = node->children[0].get();
    auto *rhs = node->children[1].get();

    if (lhs->type == NodeType::IDENTIFIER) {
        // ── Grow-by-one rewrite ─────────────────────────────────
        // `A = [A, x]` is the canonical MATLAB incremental-build
        // pattern but compiles to a fresh O(N) HORZCAT every loop
        // iteration. Detect it and emit HORZCAT_APPEND instead, which
        // routes through Value::appendScalar (geometric capacity)
        // when the runtime shape allows — turning an O(N²) loop into
        // amortised O(N). Same observable shape semantics: the
        // HORZCAT_APPEND opcode falls back to a full two-element
        // horzcat when fast-path conditions don't hold.
        if (rhs->type == NodeType::MATRIX_LITERAL
            && rhs->children.size() == 1                            // single row
            && rhs->children[0]->children.size() == 2               // two elements
            && rhs->children[0]->children[0]->type == NodeType::IDENTIFIER
            && rhs->children[0]->children[0]->strValue == lhs->strValue
            && varRegisters_.contains(lhs->strValue)) {
            keel::u16 valReg = compileNode(rhs->children[0]->children[1].get());
            keel::u16 dst = varRegLookup(lhs->strValue);
            markAssigned(lhs->strValue);
            emitUpvalLoadRawIfShared(lhs->strValue, dst);
            emitAB(OpCode::HORZCAT_APPEND, dst, valReg);
            emitUpvalStoreIfShared(lhs->strValue, dst);
            scalarRegs_.reset(dst);  // result is no longer scalar
            if (!mayClear_)
                definedRegs_.set(dst);
            if (!node->suppressOutput) {
                keel::i32 nameIdx = addStringConstant(lhs->strValue);
                emitAD(OpCode::DISPLAY, dst, nameIdx);
            }
            return dst;
        }

        keel::u16 src = compileNode(rhs);

        // MOVE elimination: redirect last instruction to write directly to var register
        const keel::u16 *varPtr = varRegisters_.get(lhs->strValue);
        bool canEliminate = (src == nextReg_ - 1 && !chunk_.code.empty()
                             && chunk_.code.back().a == src);
        if (canEliminate) {
            OpCode lastOp = chunk_.code.back().op;
            // Only redirect arithmetic/builtin ops (not LOAD_CONST — constRegCache)
            canEliminate = (lastOp >= OpCode::ADD && lastOp <= OpCode::UPLUS)
                           || (lastOp >= OpCode::EMUL && lastOp <= OpCode::EPOW)
                           || (lastOp >= OpCode::ADD_SS && lastOp <= OpCode::NEG_S)
                           || (lastOp >= OpCode::MULADD && lastOp <= OpCode::EMULSUB)
                           || lastOp == OpCode::CALL_BUILTIN;
        }

        // This node produces a write to lhs — track it for DebugWorkspace
        // regardless of which codegen path we take below.
        markAssigned(lhs->strValue);

        keel::u16 dst;
        if (canEliminate && !varPtr) {
            // First assignment: adopt temp register as the variable.
            // We MUST pin this slot in maxVarReg_ so the compileBlock
            // statement-boundary release does not later free a slot
            // that still holds the variable's value.
            varRegisters_.put(lhs->strValue.clone(), src);
            if (keel::as<keel::i32>(src) + 1 > maxVarReg_)
                maxVarReg_ = keel::as<keel::i32>(src) + 1;
            dst = src;
            // No instruction emitted, no reclaim needed — src IS the variable now
        } else if (canEliminate && varPtr) {
            // Existing variable: redirect last instruction to write to var register
            dst = *varPtr;
            chunk_.code.back().a = dst;
            nextReg_--;  // reclaim temp
            scalarRegs_.reset(src);
        } else {
            // Write site; assignedVars already updated via markAssigned()
            // above — hence the Lookup variant here.
            dst = varRegLookup(lhs->strValue);
            if (src != dst)
                emitAB(OpCode::MOVE, dst, src);
        }
        // Propagate scalar tracking through assignment
        if (scalarRegs_.test(src))
            scalarRegs_.set(dst);
        else
            scalarRegs_.reset(dst);
        if (!mayClear_)
            definedRegs_.set(dst);
        emitUpvalStoreIfShared(lhs->strValue, dst);

        // Display if no semicolon
        if (!node->suppressOutput) {
            keel::i32 nameIdx = addStringConstant(lhs->strValue);
            emitAD(OpCode::DISPLAY, dst, nameIdx);
        }
        return dst;
    }

    // Compound / non-identifier-rooted lvalue (`s.x(2)`, `d(i).a.b`,
    // `c{i}(2)`, `c{i}.f`, …) → general get/set write-back chain.
    if (needsGeneralLValuePath(lhs))
        return compileLValueStore(lhs, node->children[1].get(), -1, node->suppressOutput);

    if (lhs->type == NodeType::INDEX || lhs->type == NodeType::CALL) {
        return compileIndexAssign(node);
    }

    if (lhs->type == NodeType::FIELD_ACCESS) {
        return compileFieldAssign(node);
    }

    if (lhs->type == NodeType::DYNAMIC_FIELD_ACCESS) {
        // s.(expr) = val — child[0]=obj, child[1]=field name expr
        auto *objNode = lhs->children[0].get();
        if (objNode->type != NodeType::IDENTIFIER)
            throw std::runtime_error("Compiler: dynamic field assign requires identifier target");
        keel::u16 obj = varRegWrite(objNode->strValue);
        emitUpvalLoadRawIfShared(objNode->strValue, obj);
        keel::u16 val = compileNode(node->children[1].get());
        keel::u16 nameReg = compileNode(lhs->children[1].get());
        emitABC(OpCode::FIELD_SET_DYN, obj, nameReg, val);
        emitUpvalStoreIfShared(objNode->strValue, obj);
        if (!mayClear_)
            definedRegs_.set(obj);
        if (!node->suppressOutput) {
            keel::i32 dispIdx = addStringConstant(objNode->strValue);
            emitAD(OpCode::DISPLAY, obj, dispIdx);
        }
        return obj;
    }

    if (lhs->type == NodeType::CELL_INDEX) {
        return compileCellAssign(node);
    }

    throw std::runtime_error("Compiler: unsupported assignment target");
}

keel::u16 Compiler::compileMultiAssign(const ASTNode *node)
{
    // [a, b, c] = func(args)  OR  [a, b] = c{idx}
    // node->returnNames = ["a", "b", "c"]  (~ means ignore)
    // node->children[0] = CALL or CELL_INDEX node
    //
    // Complex-target form ([s.f, a(i), c{j}] = ...) carries an
    // lhsTargets entry per output (nullptr = `~`); outputs are routed
    // through compileStoreLValue instead of a bare var-register write.
    auto *rhsNode = node->children[0].get();
    keel::usize nout = node->returnNames.size();
    const bool complex = !node->lhsTargets.empty();

    // Superclass multi-output call `[a,b] = method@Base(obj)` is handled below
    // (after the distribute lambda is in scope) via compileSuperCall.

    // Allocate destination registers for outputs (simple path only; the
    // complex path materialises outputs at outBase then stores them).
    keel::Array<keel::u16> outRegs;
    if (!complex) {
        for (const auto &name : node->returnNames) {
            if (name == "~")
                outRegs.push_back(tempReg()); // throwaway
            else
                outRegs.push_back(varRegWrite(name));
        }
    }

    // `[x] = <expr>` where the RHS is neither a CALL nor a cell CSL is a
    // single-value bracketed assignment (MATLAB: `[X] = Y` ≡ `X = Y`). The
    // multi-output codegen below assumes the RHS is a CALL and reads
    // callNode->children[0], so a literal/expression RHS would index past the
    // node's children — an out-of-bounds read (access violation). Handle it
    // here as a single assignment to the lone target; >1 targets with a
    // single-valued RHS is "too many outputs".
    if (rhsNode->type != NodeType::CELL_INDEX
        && rhsNode->type != NodeType::CALL) {
        if (nout > 1)
            throw std::runtime_error(
                "Compiler: too many output arguments for single-valued "
                "right-hand side");
        keel::u16 src = compileNode(rhsNode);
        // Complex single target (`[s.f] = 5`, `[a(i)] = x`): route the
        // value through the general lvalue store.
        if (complex) {
            if (!node->lhsTargets.empty() && node->lhsTargets[0])
                compileStoreLValue(node->lhsTargets[0].get(), src,
                                   node->suppressOutput);
            return src;
        }
        keel::u16 dst = outRegs.empty() ? src : outRegs[0];
        if (!outRegs.empty() && dst != src)
            emitAB(OpCode::MOVE, dst, src);
        if (scalarRegs_.test(src)) scalarRegs_.set(dst);
        else scalarRegs_.reset(dst);
        if (!mayClear_ && !node->returnNames.empty() && node->returnNames[0] != "~")
            definedRegs_.set(dst);
        if (!node->returnNames.empty() && node->returnNames[0] != "~")
            emitUpvalStoreIfShared(node->returnNames[0], dst);
        if (!node->suppressOutput && !node->returnNames.empty()
            && node->returnNames[0] != "~") {
            keel::i32 nameIdx = addStringConstant(node->returnNames[0]);
            emitAD(OpCode::DISPLAY, dst, nameIdx);
        }
        return dst;
    }

    // Distribute the nout outputs sitting at [outBase, outBase+nout) into
    // their destinations and emit displays. Shared by both RHS shapes.
    auto distribute = [&](keel::u16 outBase) {
        if (complex) {
            for (keel::usize i = 0; i < nout; ++i) {
                const ASTNode *t = node->lhsTargets[i].get();
                if (t)
                    compileStoreLValue(t, outBase + keel::as<keel::u16>(i),
                                       node->suppressOutput);
            }
            return;
        }
        for (keel::usize i = 0; i < nout; ++i) {
            if (outRegs[i] != outBase + i)
                emitAB(OpCode::MOVE, outRegs[i], outBase + keel::as<keel::u16>(i));
            if (i < node->returnNames.size() && node->returnNames[i] != "~") {
                if (!mayClear_)
                    definedRegs_.set(outRegs[i]);
                emitUpvalStoreIfShared(node->returnNames[i], outRegs[i]);
            }
        }
        if (!node->suppressOutput) {
            for (keel::usize i = 0; i < node->returnNames.size(); ++i)
                if (node->returnNames[i] != "~") {
                    keel::i32 nameIdx = addStringConstant(node->returnNames[i]);
                    emitAD(OpCode::DISPLAY, outRegs[i], nameIdx);
                }
        }
    };

    // Cell CSL: [a, b] = c{idx}  (1-D)  or  [a, b] = c{r, cols}  (2-D).
    if (rhsNode->type == NodeType::CELL_INDEX) {
        keel::usize  nidx    = rhsNode->children.size() - 1;
        keel::u16 cellReg = compileNode(rhsNode->children[0].get());

        if (nidx == 2) {
            // 2-D subscript [a,b]=c{r,:} / c{:,j}: resolveIndices per dim + sub2ind.
            // Guard so `end` binds to the cell rows (dim 0) / cols (dim 1).
            IndexContextGuard guard(*this, cellReg, 2);
            keel::u16 rowReg = compileNode(rhsNode->children[1].get());
            guard.setDim(1);
            keel::u16 colReg = compileNode(rhsNode->children[2].get());
            keel::u16 outBase = reserveRegs(nout);
            emit(Instruction::make_abcde(OpCode::CELL_GET_MULTI_2D,
                                         outBase, cellReg, rowReg, colReg,
                                         keel::as<keel::u16>(nout)));
            distribute(outBase);
            if (complex)
                return nout ? outBase : 0;
            return outRegs.empty() ? 0 : outRegs[0];
        }
        if (nidx != 1)  // 3-D+ cell CSL multi-assign — mirror the TreeWalker refusal.
            throw std::runtime_error(
                keel::format("Cell CSL with {} indices not supported", nidx).c_str());

        // 1-D subscript [a,b]=c{:} / c{vec}. Guard so `end` binds to the cell numel.
        IndexContextGuard guard(*this, cellReg, 1);
        keel::u16 idxReg = compileNode(rhsNode->children[1].get());

        keel::u16 outBase = reserveRegs(nout);

        emit(Instruction::make_abcde(OpCode::CELL_GET_MULTI,
                                     outBase, cellReg, idxReg, 0,
                                     keel::as<keel::u16>(nout)));

        distribute(outBase);
        if (complex)
            return nout ? outBase : 0;
        return outRegs.empty() ? 0 : outRegs[0];
    }

    auto *callNode = rhsNode;

    // Superclass multi-output method call: [a,b] = method@Base(obj, …).
    // Compiled into the method/ctor chunk so the body runs on the VM (P2).
    if (callNode->children[0]->type == NodeType::SUPERCLASS_REF) {
        keel::u16 outBase = reserveRegs(nout);
        compileSuperCall(callNode, callNode->children[0].get(),
                         keel::as<keel::i32>(outBase), keel::as<keel::u16>(nout));
        distribute(outBase);
        if (complex)
            return nout ? outBase : 0;
        return outRegs.empty() ? 0 : outRegs[0];
    }

    // Dotted multi-output RHS: [a,b] = X.leaf(args). Mirror compileCall's
    // gating so the two shapes route correctly:
    //   • pkg.fn(args), `pkg` NOT a variable → qualified function name on
    //     CALL_MULTI (runtime resolves the namespace / +pkg m-file).
    //   • obj.m(args), root IS a variable → CALL_METHOD_MULTI (runtime
    //     dispatches the class method with nout outputs).
    // (Same split-mode workspace invariant as compileCall.)
    if (callNode->children[0]->type == NodeType::FIELD_ACCESS) {
        const ASTNode *fa = callNode->children[0].get();
        const ASTNode *rootIdent = nullptr;
        keel::String qualified = tryBuildQualifiedName(fa, &rootIdent);
        bool rootIsVar = false;
        if (rootIdent) {
            const keel::String &root = rootIdent->strValue;
            rootIsVar = varRegisters_.contains(root)
                        || upvalueRegs_.contains(root)
                        || (isTopLevel_ && engine_.workspaceEnv().getLocal(root) != nullptr);
        }
        auto compileArgsBlock = [&](keel::u16 &argBaseOut, bool splatCsl) {
            // splatCsl: a brace-index arg (obj.method(c{:})) stays an
            // uncollapsed CSL so the VM handler flattens it at runtime —
            // same split as the single-output CALL_METHOD compile
            // (bugs/closed/core/call-method-multi-no-csl-splat).
            bool anyBrace = false;
            if (splatCsl)
                for (keel::usize i = 1; i < callNode->children.size(); ++i)
                    if (callNode->children[i]->type == NodeType::CELL_INDEX) {
                        anyBrace = true;
                        break;
                    }
            keel::Array<keel::u16> ar;
            ar.reserve(callNode->children.size() - 1);
            for (keel::usize i = 1; i < callNode->children.size(); ++i)
                ar.push_back(anyBrace
                                 ? compileNodeExpand(callNode->children[i].get())
                                 : compileNode(callNode->children[i].get()));
            argBaseOut = nextReg_;
            for (keel::usize i = 0; i < ar.size(); ++i) {
                keel::u16 slot = tempReg();
                if (ar[i] != slot)
                    emitAB(OpCode::MOVE, slot, ar[i]);
            }
            return ar.size();
        };

        if (!qualified.empty() && !rootIsVar
            // MATLAB `gca.Parent.Children(2)`: nothing registered under
            // the dotted name, but the HEAD is a plain callable → the
            // callee is a VALUE (call-then-fields chain, portion 30) and
            // the args index it — fall through to CALL_METHOD_MULTI.
            && !(rootIdent && engine_.hasCallableName(rootIdent->strValue)
                 && !engine_.findClass(rootIdent->strValue)
                 && !engine_.classCtor(rootIdent->strValue)
                 && !engine_.hasExternalFunction(qualified))) {
            keel::u16 argBase = 0;
            keel::usize nargs = compileArgsBlock(argBase, /*splatCsl=*/true);
            keel::u16 outBase = reserveRegs(nout);
            keel::i32 funcIdx = addStringConstant(qualified);
            emit(Instruction::make_abcde(OpCode::CALL_MULTI, outBase, argBase,
                                         keel::as<keel::u16>(nargs), funcIdx,
                                         keel::as<keel::u16>(nout)));
            distribute(outBase);
            if (complex)
                return nout ? outBase : 0;
            return outRegs.empty() ? 0 : outRegs[0];
        }

        // Receiver compiles into a register whether it is a bare variable or
        // an rvalue expression (`f().m()`, `a.b.m()`), so no IDENTIFIER gate:
        // CALL_METHOD_MULTI dispatches on the value in objReg either way (a
        // handle's shared state still propagates through the register copy).
        if (nout <= 255 && (callNode->children.size() - 1) <= 255) {
            keel::u16 objReg = compileNode(fa->children[0].get());
            keel::u16 argBase = 0;
            keel::usize nargs = compileArgsBlock(argBase, /*splatCsl=*/true);
            keel::u16 outBase = reserveRegs(nout);
            keel::i32 nameIdx = addStringConstant(fa->strValue);
            keel::u16 packed = keel::as<keel::u16>((nargs << 8) | (nout & 0xFF));
            emit(Instruction::make_abcde(OpCode::CALL_METHOD_MULTI, outBase, objReg,
                                         argBase, nameIdx, packed));
            distribute(outBase);
            if (complex)
                return nout ? outBase : 0;
            return outRegs.empty() ? 0 : outRegs[0];
        }
    }

    if (callNode->children[0]->type != NodeType::IDENTIFIER) {
        keel::u16 fhReg = compileNode(callNode->children[0].get());
        bool anyBraceArg = false;
        for (keel::usize i = 1; i < callNode->children.size(); ++i)
            if (callNode->children[i]->type == NodeType::CELL_INDEX) {
                anyBraceArg = true;
                break;
            }
        keel::Array<keel::u16> argRegs;
        argRegs.reserve(callNode->children.size() - 1);
        for (keel::usize i = 1; i < callNode->children.size(); ++i) {
            argRegs.push_back(anyBraceArg
                                  ? compileNodeExpand(callNode->children[i].get())
                                  : compileNode(callNode->children[i].get()));
        }
        keel::u16 argBase = nextReg_;
        for (keel::usize i = 0; i < argRegs.size(); ++i) {
            keel::u16 slot = tempReg();
            if (argRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, argRegs[i]);
        }
        if (anyBraceArg) {
            const keel::u16 noutF = keel::as<keel::u16>(std::max<keel::usize>(nout, 1));
            keel::u16 outBase = reserveRegs(noutF);
            emit(Instruction::make_abcde(OpCode::CALL_INDIRECT_FLATTEN,
                                         outBase, fhReg, argBase,
                                         keel::as<keel::i32>(argRegs.size()),
                                         noutF));
            distribute(outBase);
            if (complex)
                return nout ? outBase : 0;
            return outRegs.empty() ? 0 : outRegs[0];
        }
        keel::u16 outBase = reserveRegs(nout);
        keel::usize callIdx = chunk_.code.size();
        emit(Instruction::make_abcde(OpCode::CALL_INDIRECT_MULTI,
                                     outBase, fhReg, argBase,
                                     keel::as<keel::i32>(argRegs.size()),
                                     keel::as<keel::u16>(nout)));
        recordCallArgNames(callNode, callIdx);
        distribute(outBase);
        if (complex)
            return nout ? outBase : 0;
        return outRegs.empty() ? 0 : outRegs[0];
    }

    // First-class multi-output call args [a,b]=f(x, c{idx}): if any arg is a brace cell-
    // index (a potential CSL, incl. a variable subscript), compile every arg with
    // compileNodeExpand (preserving a CSL) into consecutive registers and emit
    // CALL_FLATTEN_MULTI, which flattens any CSL arg into the runtime arg list and runs
    // the full CALL_MULTI target dispatch. A plain named-function callee only -- a
    // handle/variable callee falls through to the normal path. See csl_first_class.md.
    if (callNode->children[0]->type == NodeType::IDENTIFIER) {
        bool anyBraceArg = false;
        for (keel::usize i = 1; i < callNode->children.size(); ++i)
            if (callNode->children[i]->type == NodeType::CELL_INDEX) { anyBraceArg = true; break; }
        const keel::String &fname = callNode->children[0]->strValue;
        const bool calleeIsVar = varRegisters_.contains(fname)
                                 || upvalueRegs_.contains(fname)
                                 || (isTopLevel_ && engine_.workspaceEnv().getLocal(fname) != nullptr);
        if (anyBraceArg && !calleeIsVar) {
            keel::Array<keel::u16> argRegs;
            for (keel::usize i = 1; i < callNode->children.size(); ++i)
                argRegs.push_back(compileNodeExpand(callNode->children[i].get()));
            const NestedFuncInfo *nit = nestedFuncInfo_.get(fname);
            if (nit) {
                for (const auto &uvName : nit->upvalueNames) {
                    const keel::u16 *boxPtr = allScopeUpvalueBoxRegs_.get(uvName);
                    if (!boxPtr)
                        throw std::runtime_error("Compiler: missing upvalue box register");
                    argRegs.push_back(*boxPtr);
                }
            }
            keel::u16 argBase = nextReg_;
            for (keel::usize i = 0; i < argRegs.size(); ++i) {
                keel::u16 slot = tempReg();
                if (argRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, argRegs[i]);
            }
            keel::u16 outBase = reserveRegs(nout);
            keel::i32 funcIdx = addStringConstant(
                nit ? nit->compiledName.as_ref() : fname.as_ref());
            emit(Instruction::make_abcde(OpCode::CALL_FLATTEN_MULTI, outBase, argBase,
                                         keel::as<keel::u16>(argRegs.size()), funcIdx,
                                         keel::as<keel::u16>(nout)));
            distribute(outBase);
            if (complex)
                return nout ? outBase : 0;
            return outRegs.empty() ? 0 : outRegs[0];
        }
        if (anyBraceArg && calleeIsVar) {
            keel::Array<keel::u16> flatRegs;
            for (keel::usize i = 1; i < callNode->children.size(); ++i)
                flatRegs.push_back(compileNodeExpand(callNode->children[i].get()));
            keel::u16 flatBase = nextReg_;
            for (keel::usize i = 0; i < flatRegs.size(); ++i) {
                keel::u16 slot = tempReg();
                if (flatRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, flatRegs[i]);
            }
            keel::u16 fhReg = varRegLookup(fname);
            keel::i32 nameIdx = addStringConstant(fname);
            if (const keel::u16 *uv = upvalueRegs_.get(fname))
                emit(Instruction::make_abcde(OpCode::UPVAL_LOAD, fhReg, *uv, 0, nameIdx, 0));
            else
                emitAD(OpCode::ASSERT_DEF, fhReg, nameIdx);
            const keel::u16 noutF = keel::as<keel::u16>(std::max<keel::usize>(nout, 1));
            keel::u16 outBase = reserveRegs(noutF);
            emit(Instruction::make_abcde(OpCode::CALL_INDIRECT_FLATTEN,
                                         outBase, fhReg, flatBase,
                                         keel::as<keel::u16>(flatRegs.size()),
                                         noutF));
            distribute(outBase);
            if (complex)
                return nout ? outBase : 0;
            return outRegs.empty() ? 0 : outRegs[0];
        }
    }

    const keel::String &funcName = callNode->children[0]->strValue;

    bool calleeIsVar = (varRegisters_.contains(funcName)
                        || upvalueRegs_.contains(funcName));
    if (!calleeIsVar && isTopLevel_ && !engine_.isReservedName(funcName)) {
        Value *existing = engine_.getVariable(funcName);
        if (existing && !existing->isUnset() && !existing->isDeleted()) {
            const bool isCallable = existing->isFuncHandle();
            if (isCallable
                || (!engine_.isKnownLeafName(funcName) && !engine_.hasFunction(funcName)))
                calleeIsVar = true;
        }
    }

    // Compile call arguments
    keel::Array<keel::u16> argRegs;
    for (keel::usize i = 1; i < callNode->children.size(); ++i)
        argRegs.push_back(compileNode(callNode->children[i].get()));

    const NestedFuncInfo *nestedIt = (!calleeIsVar) ? nestedFuncInfo_.get(funcName) : nullptr;
    if (nestedIt) {
        for (const auto &uvName : nestedIt->upvalueNames) {
            const keel::u16 *boxPtr = allScopeUpvalueBoxRegs_.get(uvName);
            if (!boxPtr)
                throw std::runtime_error("Compiler: missing upvalue box register");
            argRegs.push_back(*boxPtr);
        }
    }

    keel::u16 argBase = nextReg_;
    for (keel::usize i = 0; i < argRegs.size(); ++i) {
        keel::u16 slot = tempReg();
        if (argRegs[i] != slot)
            emitAB(OpCode::MOVE, slot, argRegs[i]);
    }

    // Allocate consecutive registers for output
    keel::u16 outBase = reserveRegs(nout);

    if (calleeIsVar) {
        keel::u16 fhReg = varRegLookup(funcName);
        if (const keel::u16 *uv = upvalueRegs_.get(funcName)) {
            keel::i32 nameIdx = addStringConstant(funcName);
            emit(Instruction::make_abcde(OpCode::UPVAL_LOAD, fhReg, *uv, 0, nameIdx, 0));
        }
        keel::usize callIdx = chunk_.code.size();
        // CALL_INDIRECT_MULTI: a=outBase, b=fhReg, c=argBase, d=nargs, e=nout
        emit(Instruction::make_abcde(OpCode::CALL_INDIRECT_MULTI,
                                     outBase, fhReg, argBase,
                                     keel::as<keel::i32>(argRegs.size()),
                                     keel::as<keel::u16>(nout)));
        recordCallArgNames(callNode, callIdx);
        distribute(outBase);
        if (complex)
            return nout ? outBase : 0;
        return outRegs.empty() ? 0 : outRegs[0];
    }

    keel::i32 funcIdx = addStringConstant(
        nestedIt ? nestedIt->compiledName.as_ref() : funcName.as_ref());

    // CALL_MULTI: a=outBase, d=funcIdx, b=argBase, c=nargs, e=nout
    keel::usize callMultiIdx = chunk_.code.size();
    emit(Instruction::make_abcde(OpCode::CALL_MULTI,
                                 outBase,
                                 argBase,
                                 keel::as<keel::u16>(argRegs.size()),
                                 funcIdx,
                                 keel::as<keel::u16>(nout)));
    recordCallArgNames(callNode, callMultiIdx);

    distribute(outBase);

    if (complex)
        return nout ? outBase : 0;
    return outRegs.empty() ? 0 : outRegs[0];
}

keel::u16 Compiler::compileBinaryOp(const ASTNode *node)
{
    const keel::String &op = node->strValue;

    // Short-circuit: && and || need special handling
    // (right operand should not be evaluated if left decides result)
    if (op == "&&" || op == "||") {
        keel::u16 dst = tempReg();
        keel::u16 left = compileNode(node->children[0].get());
        emitAB(OpCode::TO_LOGICAL_SCALAR, dst, left);
        keel::usize jumpPos = currentPos();
        emitAD(op == "&&" ? OpCode::JMP_FALSE : OpCode::JMP_TRUE, dst, 0); // placeholder
        // The right operand is conditionally executed: whatever it
        // loads into the const cache is only valid when the jump was
        // NOT taken, so its cache entries must not survive past the
        // merge. Save/restore via move — the right operand compiles
        // against an empty cache (it may emit a duplicate LOAD_CONST,
        // always safe) instead of paying an O(n) map copy per
        // short-circuit op (PVS V836).
        keel::HashMap<keel::i32, keel::u16> savedConsts = keel::move(constRegCache_);
        constRegCache_.clear();
        RegBitset savedScalars{scalarRegs_.words.clone()};
        RegBitset savedDefined{definedRegs_.words.clone()};
        keel::u16 right = compileNode(node->children[1].get());
        emitAB(OpCode::TO_LOGICAL_SCALAR, dst, right);
        patchJump(jumpPos, keel::as<keel::i32>(currentPos()) - keel::as<keel::i32>(jumpPos));
        constRegCache_ = keel::move(savedConsts);
        scalarRegs_ = keel::move(savedScalars);
        scalarRegs_.reset(dst);
        definedRegs_ = keel::move(savedDefined);
        return dst;
    }

    // Element-wise fusion fast path: a binary-rooted idiom (affine `a.*x+b`, …)
    // emits FUSE_EWISE + the normally compiled op as fallback. After the
    // short-circuit ops (never fusable); a matched node has an array operand so
    // it can't constant-fold, so checking here vs. below is equivalent.
    if (keel::i32 r = tryCompileFused(node); r >= 0) return keel::as<keel::u16>(r);

    // Constant folding: both operands are number literals → compute at compile time
    if (node->children[0]->type == NodeType::NUMBER_LITERAL
        && node->children[1]->type == NodeType::NUMBER_LITERAL) {
        keel::f64 lv = node->children[0]->numValue;
        keel::f64 rv = node->children[1]->numValue;
        keel::f64 result;
        bool folded = true;
        if (op == "+")       result = lv + rv;
        else if (op == "-")  result = lv - rv;
        else if (op == "*")  result = lv * rv;
        else if (op == "/")  result = lv / rv;
        // negative base ^ non-integer exp is complex in MATLAB — don't
        // constant-fold to a real NaN; let it run through power() at runtime.
        else if (op == "^")  { if (lv < 0.0 && rv != std::floor(rv)) folded = false; else result = std::pow(lv, rv); }
        else if (op == ".*") result = lv * rv;
        else if (op == "./") result = lv / rv;
        else if (op == ".^") { if (lv < 0.0 && rv != std::floor(rv)) folded = false; else result = std::pow(lv, rv); }
        else folded = false;
        if (folded) {
            keel::i32 idx = addConstant(result);
            if (const keel::u16 *pr = pinnedConstRegs_.get(idx)) {
                scalarRegs_.set(*pr);
                return *pr;
            }
            if (const keel::u16 *cr = constRegCache_.get(idx)) {
                scalarRegs_.set(*cr);
                return *cr;
            }
            keel::u16 dst = tempReg();
            emitAD(OpCode::LOAD_CONST, dst, idx);
            constRegCache_.put(idx, dst);
            scalarRegs_.set(dst);
            return dst;
        }
    }

    // ── Left-assoc chain spine ────────────────────────────────────
    // `((a+b)+c)+d` recursed one compileBinaryOp level per term; a
    // ~1000-term chain overflows Debug stacks before StackGuard can
    // fire (Release frames happen to fit). Compile the spine
    // iteratively: collect the levels down to the base, then fold
    // upward — the temp-allocation order is identical to the recursive
    // post-order, so codegen is unchanged. Per-level pre-passes:
    // const-fold can't match (a spine level's left is a BINARY_OP, not
    // a literal); fusion idioms need a product-shaped left operand,
    // never a +/- chain, so skipping the per-level fusion check costs
    // nothing today (and fusion is an optimization, never semantics);
    // MULADD fires per level with the accumulated left.
    // (bugs/closed/core/debug-compiler-overflow-crash.md)
    if (node->children[0]->type == NodeType::BINARY_OP) {
        keel::Array<const ASTNode *> levels;
        const ASTNode *n = node;
        while (n->type == NodeType::BINARY_OP && n->children.size() == 2
               && n->strValue != "&&" && n->strValue != "||"
               && (n == node || !fusedOperandRegs_.contains(n))) {
            levels.push_back(n);
            n = n->children[0].get();
        }
        keel::u16 acc = compileNode(n);
        for (keel::usize li = levels.size(); li > 0; --li) {
            const ASTNode *lv = levels[li - 1];
            const keel::String &lOp = lv->strValue;
            const ASTNode *rhs = lv->children[1].get();
            if ((lOp == "+" || lOp == "-") && rhs->type == NodeType::BINARY_OP
                && (rhs->strValue == "*" || rhs->strValue == ".*")
                && !fusedOperandRegs_.contains(rhs)) {
                acc = emitFusedMulAdd(lOp == "+", rhs, acc);
                continue;
            }
            keel::u16 right = compileNode(rhs);
            acc = emitPlainBinary(lOp, acc, right);
        }
        return acc;
    }

    // Fused multiply-add/sub (loop opt #2): `acc ± a*b` (the rhs is a product)
    // → one MULADD/MULSUB opcode instead of MUL into a temp + ADD/SUB. Collapses
    // MAC chains (filters / polynomials / dot products). The VM keeps a two-step
    // double evaluation, so the result is bit-identical to the unfused form;
    // non-scalar operands fall back to the matching product + sum/diff. Runs
    // after const-fold + vector fusion (tryCompileFused), so those win first.
    if (op == "+" || op == "-") {
        const ASTNode *rhs = node->children[1].get();
        if (rhs->type == NodeType::BINARY_OP
            && (rhs->strValue == "*" || rhs->strValue == ".*")
            && !fusedOperandRegs_.contains(rhs))
            return emitFusedMulAdd(op == "+", rhs, compileNode(node->children[0].get()));
    }

    keel::u16 left = compileNode(node->children[0].get());
    keel::u16 right = compileNode(node->children[1].get());
    return emitPlainBinary(op, left, right);
}

// Fused multiply-add/sub helper: `acc ± a*b` where `rhs` is the product
// node — compiles its operands and emits one MULADD/MULSUB opcode.
keel::u16 Compiler::emitFusedMulAdd(bool isPlus, const ASTNode *rhs, keel::u16 acc)
{
    keel::u16 mulA = compileNode(rhs->children[0].get());
    keel::u16 mulB = compileNode(rhs->children[1].get());
    keel::u16 dst  = tempReg();
    const bool ewise = (rhs->strValue == ".*");
    OpCode fop = isPlus ? (ewise ? OpCode::EMULADD : OpCode::MULADD)
                        : (ewise ? OpCode::EMULSUB : OpCode::MULSUB);
    emit(Instruction::make_abcde(fop, dst, acc, mulA, 0, mulB));
    if (scalarRegs_.test(acc) && scalarRegs_.test(mulA) && scalarRegs_.test(mulB))
        scalarRegs_.set(dst);
    return dst;
}

// The plain (unfused, unfolded) binary-op tail: opcode selection over
// already-compiled operand registers, emit, scalar-result tracking.
keel::u16 Compiler::emitPlainBinary(keel::StringRef op, keel::u16 left, keel::u16 right)
{
    keel::u16 dst = tempReg();
    bool bothScalar = scalarRegs_.test(left) && scalarRegs_.test(right);

    OpCode opcode;
    if (op == "+")
        opcode = bothScalar ? OpCode::ADD_SS : OpCode::ADD;
    else if (op == "-")
        opcode = bothScalar ? OpCode::SUB_SS : OpCode::SUB;
    else if (op == "*")
        opcode = bothScalar ? OpCode::MUL_SS : OpCode::MUL;
    else if (op == "/")
        opcode = bothScalar ? OpCode::RDIV_SS : OpCode::RDIV;
    else if (op == "\\")
        opcode = OpCode::LDIV;
    else if (op == "^")
        opcode = bothScalar ? OpCode::POW_SS : OpCode::POW;
    else if (op == ".*")
        opcode = bothScalar ? OpCode::MUL_SS : OpCode::EMUL;
    else if (op == "./")
        opcode = bothScalar ? OpCode::RDIV_SS : OpCode::ERDIV;
    else if (op == ".\\")
        opcode = OpCode::ELDIV;
    else if (op == ".^")
        opcode = bothScalar ? OpCode::POW_SS : OpCode::EPOW;
    else if (op == "==")
        opcode = OpCode::EQ;
    else if (op == "~=")
        opcode = OpCode::NE;
    else if (op == "<")
        opcode = OpCode::LT;
    else if (op == ">")
        opcode = OpCode::GT;
    else if (op == "<=")
        opcode = OpCode::LE;
    else if (op == ">=")
        opcode = OpCode::GE;
    else if (op == "&")
        opcode = OpCode::AND;
    else if (op == "|")
        opcode = OpCode::OR;
    else
        throw std::runtime_error(
            keel::format("Compiler: unsupported binary operator '{}'", op).c_str());

    emitABC(opcode, dst, left, right);
    // Scalar-specialized ops produce scalar results
    if (bothScalar && (opcode == OpCode::ADD_SS || opcode == OpCode::SUB_SS
                       || opcode == OpCode::MUL_SS || opcode == OpCode::RDIV_SS
                       || opcode == OpCode::POW_SS))
        scalarRegs_.set(dst);
    return dst;
}

keel::u16 Compiler::compileUnaryOp(const ASTNode *node)
{
    const keel::String &op = node->strValue;

    // Constant folding: -<number> → load negated constant directly
    if (op == "-" && node->children[0]->type == NodeType::NUMBER_LITERAL) {
        keel::i32 idx = addConstant(-node->children[0]->numValue);
        if (const keel::u16 *cr = constRegCache_.get(idx)) {
            scalarRegs_.set(*cr);
            return *cr;
        }
        keel::u16 dst = tempReg();
        emitAD(OpCode::LOAD_CONST, dst, idx);
        constRegCache_.put(idx, dst);
        scalarRegs_.set(dst);
        return dst;
    }

    keel::u16 src = compileNode(node->children[0].get());
    keel::u16 dst = tempReg();

    if (op == "-") {
        if (scalarRegs_.test(src)) {
            emitAB(OpCode::NEG_S, dst, src);
            scalarRegs_.set(dst);
        } else {
            emitAB(OpCode::NEG, dst, src);
        }
    } else if (op == "+")
        emitAB(OpCode::UPLUS, dst, src);
    else if (op == "~")
        emitAB(OpCode::NOT, dst, src);
    else if (op == "'")
        emitAB(OpCode::CTRANSPOSE, dst, src);
    else if (op == ".'")
        emitAB(OpCode::TRANSPOSE, dst, src);
    else
        throw std::runtime_error(
            keel::format("Compiler: unsupported unary operator '{}'", op).c_str());

    return dst;
}

keel::u16 Compiler::compileExprStmt(const ASTNode *node)
{
    auto *child = node->children[0].get();

    // Expression statements are nargout=0 context for top-level calls —
    // functions should not produce output values (tic, figure, fprintf, etc.).
    // Non-call expressions (e.g. `1 + numel(dir(p))`) evaluate operands in
    // value context (nargout=1).
    const keel::u16 stmtNargout =
        node->captureResult
            ? 1
            : ((child->type == NodeType::CALL || child->type == NodeType::COMMAND_CALL) ? 0 : 1);
    struct NargoutGuard
    {
        keel::u16 &ref;
        keel::u16 saved;
        NargoutGuard(keel::u16 &r, keel::u16 v)
            : ref(r)
            , saved(r)
        {
            ref = v;
        }
        ~NargoutGuard() { ref = saved; }
    } nargoutGuard(nargoutContext_, stmtNargout);

    // MATLAB display / ans rule:
    //   - A bare read of a user variable (`v` on a line by itself) displays
    //     using the variable's own name and does NOT overwrite `ans`.
    //   - Any other expression (`5+3`, `sin(0.5)`, `pi`, function call, …)
    //     stores its result into `ans` and displays with the label "ans".
    //   - Display happens only when the statement isn't suppressed by `;`;
    //     the `ans` store happens regardless.
    //
    // "User variable" is classified by storage location, not by varRegisters_
    // (preImport brings even built-ins into the chunk's register file now,
    // so we can't use that as the signal). Instead:
    //   - At top level, ask the base workspace directly: getLocal() returns
    //     non-null only for actual workspace locals, never for the
    //     inherited constantsEnv_ entries (pi, eps, …). A shadow
    //     (`pi = 5; pi`) therefore counts as a user variable.
    //   - Inside a function, varRegisters_ holds params + locals + preImport
    //     constants; exclude the reserved names so pi/nargin/… still hit
    //     the ans path.
    bool bareUserVar = false;
    keel::StringRef displayName;
    if (child->type == NodeType::IDENTIFIER) {
        const keel::String &name = child->strValue;
        if (isTopLevel_) {
            Value *local = engine_.workspaceEnv().getLocal(name);
            if (local && !local->isUnset() && !local->isDeleted())
                bareUserVar = true; // empty `[]` is a defined value: `x=[]; x` shows x, not ans
        } else {
            if ((varRegisters_.contains(name) || upvalueRegs_.contains(name))
                && !engine_.isReservedName(name))
                bareUserVar = true;
        }
        if (bareUserVar)
            displayName = name.as_ref();
    }

    if (!bareUserVar && child->type == NodeType::IDENTIFIER) {
        const keel::String &name = child->strValue;
        bool isKnownVar = varRegisters_.contains(name) || upvalueRegs_.contains(name);

        // Also check workspaceEnv for variables from previous eval() calls
        if (!isKnownVar && isTopLevel_ && !engine_.isReservedName(name)) {
            Value *existing = engine_.getVariable(name);
            if (existing && !existing->isUnset() && !existing->isDeleted())
                isKnownVar = true;
        }

        if (!isKnownVar) {
            if (const NestedFuncInfo *nit = nestedFuncInfo_.get(name)) {
                keel::u16 argBase = keel::as<keel::u16>(nextReg_);
                for (const auto &uvName : nit->upvalueNames) {
                    keel::u16 slot = tempReg();
                    const keel::u16 *boxPtr = allScopeUpvalueBoxRegs_.get(uvName);
                    if (!boxPtr)
                        throw std::runtime_error("Compiler: missing upvalue box register");
                    keel::u16 boxReg = *boxPtr;
                    if (boxReg != slot)
                        emitAB(OpCode::MOVE, slot, boxReg);
                }
                keel::u16 dst = tempReg();
                keel::i32 funcIdx = addStringConstant(nit->compiledName);
                emit(Instruction::make_abcde(OpCode::CALL, dst, argBase,
                                             keel::as<keel::u16>(nit->upvalueNames.size()),
                                             funcIdx, node->captureResult ? 1 : 0));
                if (node->captureResult)
                    return dst;
                if (!node->suppressOutput) {
                    keel::i32 nameIdx = addStringConstant("ans");
                    emitAD(OpCode::DISPLAY, dst, nameIdx);
                }
                keel::u16 ansReg = varRegWrite("ans");
                emitABC(OpCode::COLLAPSE, ansReg, dst, 1);
                emitUpvalStoreIfShared("ans", ansReg);
                return ansReg;
            }
        }

        bool knownCallable = !isKnownVar && !mayInjectVars_
            && (engine_.isKnownLeafName(name) || engine_.hasFunction(name));
        if (!isKnownVar && !mayInjectVars_ && !knownCallable) {
            // lookupUserFunction may register a script or classdef .m file as an
            // external callable while returning nullptr; re-check hasFunction after.
            if (engine_.lookupUserFunction(name, &engine_.workspaceEnv()) != nullptr
                || engine_.hasFunction(name)) {
                knownCallable = true;
            }
        }
        if (knownCallable) {
            // Workspace builtins inside functions -> opcodes (statement nargout=0 only)
            if (!isTopLevel_ && !node->captureResult) {
                if (name == "who") {
                    emitAB(OpCode::WHO, 0, 0);
                    return tempReg();
                }
                if (name == "whos") {
                    emitAB(OpCode::WHOS, 0, 0);
                    return tempReg();
                }
                if (name == "clear") {
                    for (auto [vname, reg] : varRegisters_) {
                        if (!engine_.isReservedName(vname)) {
                            emitA(OpCode::CLEAR_VAR, reg);
                            emitUpvalStoreIfShared(vname, reg);
                        }
                    }
                    if (!mayInjectVars_)
                        return tempReg();
                }
                if (name == "exist") {
                    // exist (no args) — error, but let it go to CALL
                }
            } else if (name == "clear") {
                for (auto [vname, reg] : varRegisters_) {
                    if (!engine_.isReservedName(vname)) {
                        emitA(OpCode::CLEAR_VAR, reg);
                        emitUpvalStoreIfShared(vname, reg);
                    }
                }
            }

            // Compile as zero-arg CALL (nargout=1 when result is captured by eval/evalin).
            keel::u16 argBase = nextReg_;
            keel::u16 dst = tempReg();
            keel::i32 funcIdx = addStringConstant(name);
            emit(Instruction::make_abcde(OpCode::CALL, dst, argBase, 0, funcIdx,
                                         node->captureResult ? 1 : 0));
            if (node->captureResult)
                return dst;

            // Anonymous call result -> bind to `ans` (preserving prior `ans` if void).
            if (!node->suppressOutput) {
                keel::i32 nameIdx = addStringConstant("ans");
                emitAD(OpCode::DISPLAY, dst, nameIdx);
            }
            keel::u16 ansReg = varRegWrite("ans");
            emitABC(OpCode::COLLAPSE, ansReg, dst, 1);
            emitUpvalStoreIfShared("ans", ansReg);
            return ansReg;
        }
    }

    keel::u16 reg = compileNodeExpand(child);

    if (bareUserVar) {
        if (!node->suppressOutput) {
            keel::i32 nameIdx = addStringConstant(displayName);
            emitAD(OpCode::DISPLAY, reg, nameIdx);
        }
        return reg;
    }

    if (node->captureResult) {
        keel::u16 dst = tempReg();
        emitABC(OpCode::COLLAPSE, dst, reg, 1);
        return dst;
    }

    if (!node->suppressOutput) {
        keel::i32 nameIdx = addStringConstant("ans");
        emitAD(OpCode::DISPLAY, reg, nameIdx);
    }
    keel::u16 ansReg = varRegWrite("ans");
    emitABC(OpCode::COLLAPSE, ansReg, reg, 1);
    emitUpvalStoreIfShared("ans", ansReg);
    return ansReg;
}

// ============================================================
} // namespace numkit
