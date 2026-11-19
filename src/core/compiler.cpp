// src/compiler.cpp
#include <keel/core.h>
#include <keel/format.h>
#include <keel/utf8.h>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/core/vm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace numkit {

// RAII guard for END_VAL compilation context
class IndexContextGuard
{
public:
    IndexContextGuard(Compiler &c, keel::u16 arr, keel::u16 ndims)
        : comp_(c)
        , savedArr_(c.indexContextArr_)
        , savedDim_(c.indexContextDim_)
        , savedNdims_(c.indexContextNdims_)
    {
        comp_.indexContextArr_ = arr;
        comp_.indexContextDim_ = 0;
        comp_.indexContextNdims_ = ndims;
    }
    ~IndexContextGuard()
    {
        comp_.indexContextArr_ = savedArr_;
        comp_.indexContextDim_ = savedDim_;
        comp_.indexContextNdims_ = savedNdims_;
    }
    void setDim(keel::u16 d) { comp_.indexContextDim_ = d; }

    IndexContextGuard(const IndexContextGuard &) = delete;
    IndexContextGuard &operator=(const IndexContextGuard &) = delete;

private:
    Compiler &comp_;
    keel::u16 savedArr_, savedDim_, savedNdims_;
};


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
    chunk_.numRegisters = static_cast<keel::u32>(peakReg_);

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
            if (nextReg_ >= static_cast<int>(kInvalidReg))
                throw std::runtime_error(
                    "Compiler: register limit exceeded (>65534 registers in chunk)");
            keel::u16 r = static_cast<keel::u16>(nextReg_++);
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
        if (nextReg_ >= static_cast<int>(kInvalidReg))
            throw std::runtime_error(
                "Compiler: register limit exceeded (>65534 registers in chunk)");
        keel::u16 r = static_cast<keel::u16>(nextReg_++);
        if (nextReg_ > peakReg_) peakReg_ = nextReg_;
        if (nextReg_ > maxVarReg_) maxVarReg_ = nextReg_;
        varRegisters_.put(name.clone(), r);
    }
}

namespace {
[[nodiscard]] constexpr bool isSymsFlag(keel::StringRef s) noexcept
{
    return s == "real" || s == "positive" || s == "negative"
        || s == "nonnegative" || s == "integer" || s == "clear";
}

[[nodiscard]] constexpr bool isClearFlag(keel::StringRef s) noexcept
{
    return s == "all" || s == "classes" || s == "functions"
        || s == "global" || s == "-regexp" || s == "import";
}

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
        keel::u16 r = static_cast<keel::u16>(nextReg_++);
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
    if (node->type == NodeType::COMMAND_CALL && node->strValue == "syms") {
        for (const auto &ch : node->children) {
            if (!ch->strValue.empty() && !isSymsFlag(ch->strValue))
                out.push_back(ch->strValue.clone());
        }
    }
    if (node->type == NodeType::CALL && !node->children.empty()
        && node->children[0]->type == NodeType::IDENTIFIER
        && node->children[0]->strValue == "syms") {
        for (keel::usize i = 1; i < node->children.size(); ++i) {
            const auto *ch = node->children[i].get();
            if (ch->type == NodeType::STRING_LITERAL && !ch->strValue.empty()
                && !isSymsFlag(ch->strValue))
                out.push_back(ch->strValue.clone());
        }
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
    int allocReg = (nextReg_ > peakReg_) ? nextReg_ : peakReg_;
    if (allocReg >= static_cast<int>(kInvalidReg))
        throw std::runtime_error(
            "Compiler: register limit exceeded (>65534 registers in chunk)");
    keel::u16 r = static_cast<keel::u16>(allocReg);
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
    int allocReg = (nextReg_ > peakReg_) ? nextReg_ : peakReg_;
    if (allocReg >= static_cast<int>(kInvalidReg))
        throw std::runtime_error(
            "Compiler: register limit exceeded (>65534 registers in chunk)");
    keel::u16 r = static_cast<keel::u16>(allocReg);
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

    keel::u16 base = static_cast<keel::u16>(nextReg_);
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
    emitABC(OpCode::CLOSURE_MAKE, dst, base, static_cast<keel::u16>(totalSlots));
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
        keel::u16 argBase = static_cast<keel::u16>(nextReg_);
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
                                     static_cast<keel::u16>(nit->upvalueNames.size()),
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
    if (nextReg_ >= static_cast<int>(kInvalidReg))
        throw std::runtime_error(
            "Compiler: register limit exceeded (>65534 registers in chunk)");
    keel::u16 r = static_cast<keel::u16>(nextReg_++);
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
    if (chunk_.constants.size() >= static_cast<keel::usize>(INT32_MAX))
        throw std::runtime_error(
            "Compiler: constant pool limit exceeded (>2^31-1 entries in one chunk)");
    keel::i32 idx = static_cast<keel::i32>(chunk_.constants.size());
    chunk_.constants.push_back(keel::move(v));
    return idx;
}

keel::i32 Compiler::appendString(keel::StringRef s)
{
    if (chunk_.strings.size() >= static_cast<keel::usize>(INT32_MAX))
        throw std::runtime_error(
            "Compiler: string pool limit exceeded (>2^31-1 entries in one chunk)");
    keel::i32 idx = static_cast<keel::i32>(chunk_.strings.size());
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
                return static_cast<keel::i32>(i);
        }
    }
    return appendConstant(Value::scalar(value, engine_.allocator()));
}

keel::i32 Compiler::addStringConstant(keel::StringRef s)
{
    for (keel::usize i = 0; i < chunk_.strings.size(); ++i) {
        if (chunk_.strings[i] == s)
            return static_cast<keel::i32>(i);
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
        currentLoc_.line = static_cast<keel::u16>(node->line);
        currentLoc_.col = static_cast<keel::u16>(node->col);
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
                                     static_cast<keel::i32>(indexContextNdims_),
                                     0));
        return dst;
    }
    case NodeType::DELETE_ASSIGN:
        return compileDeleteAssign(node);
    default:
        throw std::runtime_error(
            keel::format("Compiler: unsupported node type {}",
                         static_cast<int>(node->type)).c_str());
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
    const int entryNext = nextReg_;
    keel::u16 last = 0;
    for (auto &child : node->children) {
        last = compileNode(child.get());
        const int floor = entryNext > maxVarReg_ ? entryNext : maxVarReg_;
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
            if (static_cast<int>(src) + 1 > maxVarReg_)
                maxVarReg_ = static_cast<int>(src) + 1;
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
                    compileStoreLValue(t, outBase + static_cast<keel::u16>(i),
                                       node->suppressOutput);
            }
            return;
        }
        for (keel::usize i = 0; i < nout; ++i) {
            if (outRegs[i] != outBase + i)
                emitAB(OpCode::MOVE, outRegs[i], outBase + static_cast<keel::u16>(i));
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
                                         static_cast<keel::u16>(nout)));
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
                                     static_cast<keel::u16>(nout)));

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
                         static_cast<int>(outBase), static_cast<keel::u16>(nout));
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
                                         static_cast<keel::u16>(nargs), funcIdx,
                                         static_cast<keel::u16>(nout)));
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
            keel::u16 packed = static_cast<keel::u16>((nargs << 8) | (nout & 0xFF));
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
            const keel::u16 noutF = static_cast<keel::u16>(std::max<keel::usize>(nout, 1));
            keel::u16 outBase = reserveRegs(noutF);
            emit(Instruction::make_abcde(OpCode::CALL_INDIRECT_FLATTEN,
                                         outBase, fhReg, argBase,
                                         static_cast<keel::i32>(argRegs.size()),
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
                                     static_cast<keel::i32>(argRegs.size()),
                                     static_cast<keel::u16>(nout)));
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
                                         static_cast<keel::u16>(argRegs.size()), funcIdx,
                                         static_cast<keel::u16>(nout)));
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
            const keel::u16 noutF = static_cast<keel::u16>(std::max<keel::usize>(nout, 1));
            keel::u16 outBase = reserveRegs(noutF);
            emit(Instruction::make_abcde(OpCode::CALL_INDIRECT_FLATTEN,
                                         outBase, fhReg, flatBase,
                                         static_cast<keel::u16>(flatRegs.size()),
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
                                     static_cast<keel::i32>(argRegs.size()),
                                     static_cast<keel::u16>(nout)));
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
                                 static_cast<keel::u16>(argRegs.size()),
                                 funcIdx,
                                 static_cast<keel::u16>(nout)));
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
        patchJump(jumpPos, static_cast<keel::i32>(currentPos()) - static_cast<keel::i32>(jumpPos));
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
    if (int r = tryCompileFused(node); r >= 0) return static_cast<keel::u16>(r);

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
        (child->type == NodeType::CALL || child->type == NodeType::COMMAND_CALL) ? 0 : 1;
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
                keel::u16 argBase = static_cast<keel::u16>(nextReg_);
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
                                             static_cast<keel::u16>(nit->upvalueNames.size()),
                                             funcIdx, 0));
                keel::u16 ansReg = varRegWrite("ans");
                if (ansReg != dst)
                    emitAB(OpCode::MOVE, ansReg, dst);
                emitUpvalStoreIfShared("ans", ansReg);
                if (!node->suppressOutput) {
                    keel::i32 nameIdx = addStringConstant("ans");
                    emitAD(OpCode::DISPLAY, ansReg, nameIdx);
                }
                return ansReg;
            }
        }

        if (!isKnownVar && !mayInjectVars_
            && (engine_.isKnownLeafName(name) || engine_.hasFunction(name)
                || engine_.lookupUserFunction(name, &engine_.workspaceEnv()) != nullptr)) {
            // Workspace builtins inside functions -> opcodes
            if (!isTopLevel_) {
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

            // Compile as zero-arg CALL (nargout=0 for statement context).
            keel::u16 argBase = nextReg_;
            keel::u16 dst = tempReg();
            keel::i32 funcIdx = addStringConstant(name);
            emit(Instruction::make_abcde(OpCode::CALL, dst, argBase, 0, funcIdx, 0));

            // Anonymous call result -> bind to `ans`.
            keel::u16 ansReg = varRegWrite("ans");
            if (ansReg != dst)
                emitAB(OpCode::MOVE, ansReg, dst);
            emitUpvalStoreIfShared("ans", ansReg);
            if (!node->suppressOutput) {
                keel::i32 nameIdx = addStringConstant("ans");
                emitAD(OpCode::DISPLAY, ansReg, nameIdx);
            }
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
// Phase 2: Control flow
// ============================================================

keel::u16 Compiler::compileIf(const ASTNode *node)
{
    constRegCache_.clear(); scalarRegs_.reset(); // branches may skip LOAD_CONST
    // branches: Array<ASTBranch{cond, body}>
    // elseBranch: optional else body
    //
    // Compiled as:
    //   eval cond1
    //   JMP_FALSE cond1 → L_next1
    //   body1
    //   JMP → L_end
    // L_next1:
    //   eval cond2
    //   JMP_FALSE cond2 → L_next2
    //   body2
    //   JMP → L_end
    // L_next2:
    //   else body
    // L_end:

    keel::Array<keel::usize> endJumps; // positions of JMP → L_end
    RegBitset savedDefined{definedRegs_.words.clone()};

    for (keel::usize i = 0; i < node->branches.size(); ++i) {
        const auto &branch = node->branches[i];

        // Clear cache: previous branch body may have been skipped
        constRegCache_.clear(); scalarRegs_.reset();
        definedRegs_.words = savedDefined.words.clone();
        // Compile condition
        keel::u16 condReg = compileNode(branch.cond.get());

        // JMP_FALSE to next branch (placeholder)
        keel::usize skipPos = currentPos();
        emitAD(OpCode::JMP_FALSE, condReg, 0);

        // Compile body
        compileNode(branch.body.get());

        // JMP to end (skip remaining branches) — unless this is the last branch with no else
        if (i < node->branches.size() - 1 || node->elseBranch) {
            endJumps.push_back(currentPos());
            emitD(OpCode::JMP, 0); // placeholder
        }

        // Patch the JMP_FALSE to jump here (next branch)
        patchJump(skipPos, currentPos() - skipPos);
    }

    // Compile else branch if present
    if (node->elseBranch) {
        constRegCache_.clear(); scalarRegs_.reset();
        definedRegs_.words = savedDefined.words.clone();
        compileNode(node->elseBranch.get());
    }

    // Emit NOP for the 'end' keyword so breakpoints on it fire
    keel::usize endNopPos = currentPos();
    if (node->endLine > 0) {
        currentLoc_ = {static_cast<keel::u16>(node->endLine), 1};
        emitNone(OpCode::NOP);
    }

    // Patch all end jumps to the NOP (all branches converge here)
    for (keel::usize pos : endJumps) {
        patchJump(pos, endNopPos - pos);
    }

    constRegCache_.clear(); scalarRegs_.reset(); // don't know which branch ran
    definedRegs_ = keel::move(savedDefined);
    return 0;
}

// ============================================================
// Switch/case
// ============================================================
keel::u16 Compiler::compileSwitch(const ASTNode *node)
{
    constRegCache_.clear(); scalarRegs_.reset();
    // AST: children[0] = switch expression
    //      branches = Array<ASTBranch{case_expr, case_body}>
    //      elseBranch = otherwise body (optional)
    //
    // Compiled as chain of EQ + JMP_FALSE (like if/elseif):
    //   eval switch_val → rSV
    //   eval case1_val → rCV
    //   EQ rCmp, rSV, rCV
    //   JMP_FALSE rCmp → L_next1
    //   body1
    //   JMP → L_end
    // L_next1:
    //   eval case2_val → rCV
    //   EQ rCmp, rSV, rCV
    //   JMP_FALSE rCmp → L_next2
    //   body2
    //   JMP → L_end
    // L_next2:
    //   otherwise body
    // L_end:

    // Compile switch expression
    keel::u16 switchReg = compileNode(node->children[0].get());

    // Temp register for comparison result
    keel::u16 cmpReg = tempReg();

    keel::Array<keel::usize> endJumps;
    RegBitset savedDefined{definedRegs_.words.clone()};

    for (keel::usize i = 0; i < node->branches.size(); ++i) {
        const auto &branch = node->branches[i];
        const auto &caseExpr = branch.cond;
        const auto &caseBody = branch.body;
        definedRegs_.words = savedDefined.words.clone();

        if (caseExpr->type == NodeType::CELL_LITERAL) {
            // Cell case {val1, val2, ...}: match if switchReg == any element
            // Get cell elements: may be wrapped in a BLOCK child
            const keel::Array<ASTNodePtr> *elements = &caseExpr->children;
            if (caseExpr->children.size() == 1 && caseExpr->children[0]->type == NodeType::BLOCK) {
                elements = &caseExpr->children[0]->children;
            }

            // Compile: cmpReg = (sv==el1) | (sv==el2) | ...
            keel::u16 tmpCmp = tempReg();
            // Start with false
            keel::i32 zeroIdx = addConstant(0.0);
            emitAD(OpCode::LOAD_CONST, cmpReg, zeroIdx);

            for (keel::usize ei = 0; ei < elements->size(); ++ei) {
                keel::u16 elReg = compileNode((*elements)[ei].get());
                emitABC(OpCode::EQ, tmpCmp, switchReg, elReg);
                emitABC(OpCode::OR, cmpReg, cmpReg, tmpCmp);
            }
        } else {
            // Simple case: scalar comparison
            keel::u16 caseReg = compileNode(caseExpr.get());
            emitABC(OpCode::EQ, cmpReg, switchReg, caseReg);
        }

        // JMP_FALSE to next case
        keel::usize skipPos = currentPos();
        emitAD(OpCode::JMP_FALSE, cmpReg, 0);

        // Compile case body
        compileNode(caseBody.get());

        // JMP to end
        if (i < node->branches.size() - 1 || node->elseBranch) {
            endJumps.push_back(currentPos());
            emitD(OpCode::JMP, 0);
        }

        // Patch JMP_FALSE
        patchJump(skipPos, currentPos() - skipPos);
    }

    // Compile otherwise
    if (node->elseBranch) {
        definedRegs_.words = savedDefined.words.clone();
        compileNode(node->elseBranch.get());
    }

    // Emit NOP for the 'end' keyword so breakpoints on it fire
    keel::usize endNopPos = currentPos();
    if (node->endLine > 0) {
        currentLoc_ = {static_cast<keel::u16>(node->endLine), 1};
        emitNone(OpCode::NOP);
    }

    // Patch end jumps
    for (keel::usize pos : endJumps) {
        patchJump(pos, endNopPos - pos);
    }

    definedRegs_ = keel::move(savedDefined);
    return 0;
}

// ============================================================
// Global / Persistent
// ============================================================
keel::u16 Compiler::compileGlobalPersistent(const ASTNode *node)
{
    const bool isGlobal = (node->type == NodeType::GLOBAL_STMT);
    if (!isGlobal && isTopLevel_) {
        keel::u16 msgReg = tempReg();
        keel::i32 msgIdx = addStringConstant(
            "persistent declarations are only allowed inside functions");
        emitAD(OpCode::LOAD_STRING, msgReg, msgIdx);
        emitA(OpCode::THROW, msgReg);
        return msgReg;
    }
    for (const auto &name : node->paramNames) {
        varRegWrite(name); // allocate register, mark as assigned
        if (isGlobal) {
            if (std::find(chunk_.globalNames.begin(), chunk_.globalNames.end(), name)
                == chunk_.globalNames.end()) {
                chunk_.globalNames.push_back(name.clone());
            }
            chunk_.ownGlobalDecls.push_back(name.clone());
        } else {
            if (std::find(chunk_.persistentNames.begin(), chunk_.persistentNames.end(), name)
                == chunk_.persistentNames.end()) {
                chunk_.persistentNames.push_back(name.clone());
            }
        }
    }
    return 0;
}

// ============================================================
// Try/catch
// ============================================================
keel::u16 Compiler::compileTryCatch(const ASTNode *node)
{
    constRegCache_.clear(); scalarRegs_.reset();
    // AST: children[0] = try body
    //      children[1] = catch body (optional)
    //      strValue = catch variable name (e.g. "e")
    //
    // Compiled as:
    //   TRY_BEGIN catchOffset, exReg
    //   ... try body ...
    //   TRY_END
    //   JMP → L_end
    // L_catch:
    //   ... catch body ...
    // L_end:

    RegBitset savedDefined{definedRegs_.words.clone()};

    // Register for exception variable (if named)
    keel::u16 exReg = 0;
    if (!node->strValue.empty()) {
        exReg = varRegWrite(node->strValue);
    } else {
        exReg = tempReg();
    }

    // TRY_BEGIN: d = catchOffset (placeholder), a = exReg
    keel::usize tryBeginPos = currentPos();
    emitAD(OpCode::TRY_BEGIN, exReg, 0);

    // Compile try body (track activeTryDepth_ so break/continue inside try pop TRY_END)
    {
        struct TryDepthGuard {
            int &depth;
            explicit TryDepthGuard(int &d) : depth(d) { ++depth; }
            ~TryDepthGuard() { --depth; }
        } tryDepthGuard(activeTryDepth_);
        compileNode(node->children[0].get());
    }

    // TRY_END
    emitA(OpCode::TRY_END, 0);

    // JMP past catch body
    keel::usize endJmpPos = currentPos();
    emitD(OpCode::JMP, 0);

    // Patch TRY_BEGIN to jump here on exception
    patchJump(tryBeginPos, currentPos() - tryBeginPos);

    // Compile catch body
    definedRegs_.words = savedDefined.words.clone();
    if (!node->strValue.empty()) {
        if (!mayClear_)
            definedRegs_.set(exReg);
        emitUpvalStoreIfShared(node->strValue, exReg);
    }
    if (node->children.size() > 1) {
        compileNode(node->children[1].get());
    }
    definedRegs_ = keel::move(savedDefined);

    // Emit NOP for the 'end' keyword so breakpoints on it fire
    keel::usize endNopPos = currentPos();
    if (node->endLine > 0) {
        currentLoc_ = {static_cast<keel::u16>(node->endLine), 1};
        emitNone(OpCode::NOP);
    }

    // Patch JMP to end (land on NOP)
    patchJump(endJmpPos, endNopPos - endJmpPos);

    return 0;
}

keel::u16 Compiler::compileWhile(const ASTNode *node)
{
    constRegCache_.clear(); scalarRegs_.reset();
    // children[0] = condition, children[1] = body
    //
    // L_start:
    //   eval condition
    //   JMP_FALSE → L_end
    //   body
    //   JMP → L_start
    // L_end:

    loopStack_.push_back(LoopContext{{}, {}, false, activeTryDepth_});

    keel::usize loopStart = currentPos();

    // Compile condition
    const int condMark = nextReg_;
    keel::u16 condReg = compileNode(node->children[0].get());

    // JMP_FALSE to end (placeholder)
    keel::usize exitPos = currentPos();
    emitAD(OpCode::JMP_FALSE, condReg, 0);

    const int condFloor = condMark > maxVarReg_ ? condMark : maxVarReg_;
    if (nextReg_ > condFloor)
        nextReg_ = condFloor;

    // Compile body
    RegBitset savedDefined{definedRegs_.words.clone()};
    compileNode(node->children[1].get());
    definedRegs_ = keel::move(savedDefined);

    // Patch continue targets → jump to loopStart (re-evaluate condition)
    for (keel::usize pos : loopStack_.back().continuePatches) {
        patchJump(pos, loopStart - pos);
    }

    // Emit source location for the 'end' keyword so breakpoints on it fire
    if (node->endLine > 0)
        currentLoc_ = {static_cast<keel::u16>(node->endLine), 1};

    // JMP back to start
    emitD(OpCode::JMP, static_cast<keel::i32>(loopStart) - static_cast<keel::i32>(currentPos()));

    // Patch JMP_FALSE exit
    patchJump(exitPos, currentPos() - exitPos);

    // Patch break targets → jump to here (after loop)
    for (keel::usize pos : loopStack_.back().breakPatches) {
        patchJump(pos, currentPos() - pos);
    }

    loopStack_.pop_back();
    constRegCache_.clear(); scalarRegs_.reset(); // body constants may not have executed
    return 0;
}

keel::u16 Compiler::compileBreak(const ASTNode * /*node*/)
{
    if (loopStack_.empty())
        throw std::runtime_error("Compiler: break outside of loop");

    int triesToPop = activeTryDepth_ - loopStack_.back().tryDepthAtEntry;
    for (int i = 0; i < triesToPop; ++i)
        emitA(OpCode::TRY_END, 0);

    if (loopStack_.back().isForLoop) {
        emitA(OpCode::NOP, 1);
    }

    loopStack_.back().breakPatches.push_back(currentPos());
    emitD(OpCode::JMP, 0);
    return 0;
}

keel::u16 Compiler::compileContinue(const ASTNode * /*node*/)
{
    if (loopStack_.empty())
        throw std::runtime_error("Compiler: continue outside of loop");
    int triesToPop = activeTryDepth_ - loopStack_.back().tryDepthAtEntry;
    for (int i = 0; i < triesToPop; ++i)
        emitA(OpCode::TRY_END, 0);
    loopStack_.back().continuePatches.push_back(currentPos());
    emitD(OpCode::JMP, 0);
    return 0;
}

// ============================================================
// Phase 3: For-loop, colon, arrays, indexing
// ============================================================

keel::u16 Compiler::compileFor(const ASTNode *node)
{
    // node->strValue = loop variable name
    // children[0] = range expression
    // children[1] = body
    //
    // Compiled as:
    //   eval range → rangeReg
    //   FOR_INIT varReg, rangeReg, endOffset(placeholder)
    // L_body:
    //   body
    // L_continue:
    //   FOR_NEXT varReg, backOffset  (jumps to L_body or falls through)
    // L_end:

    // Fast path: `for v = a:b` and `for v = a:s:b` — emit FOR_INIT_RANGE
    // directly so the colon range never gets materialised. Saves an
    // 8N-byte heap allocation for the common `for i = 1:N` numeric loop.
    // Bare ":" (no children) is the colon-all marker; falls through to
    // the general path.
    const ASTNode *rangeNode = node->children[0].get();
    const bool isColonRange = rangeNode->type == NodeType::COLON_EXPR
                              && (rangeNode->children.size() == 2
                                  || rangeNode->children.size() == 3);

    // Compile range expression(s) FIRST — they emit LOAD_CONST etc.
    // Then capture forInitPos so the patchJump after the body targets
    // the FOR_INIT[_RANGE] instruction itself, not whatever range
    // emission ended up first in the bytecode.
    keel::u16 startReg = 0, stopReg = 0, stepReg = kInvalidReg, rangeReg = 0;
    if (isColonRange) {
        startReg = compileNode(rangeNode->children[0].get());
        if (rangeNode->children.size() == 2) {
            stopReg = compileNode(rangeNode->children[1].get());
            stepReg = kInvalidReg;  // sentinel: implicit step=1
        } else {
            stepReg = compileNode(rangeNode->children[1].get());
            stopReg = compileNode(rangeNode->children[2].get());
        }
    } else {
        rangeReg = compileNode(rangeNode);
    }
    keel::u16 vReg = varRegWrite(node->strValue);
    loopStack_.push_back(LoopContext{{}, {}, true, activeTryDepth_});

    keel::usize forInitPos = currentPos();
    if (isColonRange) {
        // FOR_INIT_RANGE: a=vReg, b=start, c=stop, d=endOffset, e=step (or 0xFF)
        emit(Instruction::make_abcde(OpCode::FOR_INIT_RANGE,
                                     vReg, startReg, stopReg, 0, stepReg));
        scalarRegs_.set(vReg);
    } else {
        // FOR_INIT: a=varReg, b=rangeReg, d=endOffset(placeholder)
        emit(Instruction::make_abd(OpCode::FOR_INIT, vReg, rangeReg, 0));
        if (rangeNode->type == NodeType::COLON_EXPR)
            scalarRegs_.set(vReg);
    }

    keel::usize bodyStart = currentPos();
    emitUpvalStoreIfShared(node->strValue, vReg);

    // Compile body
    RegBitset savedDefined{definedRegs_.words.clone()};
    if (!mayClear_)
        definedRegs_.set(vReg);
    compileNode(node->children[1].get());
    definedRegs_ = keel::move(savedDefined);

    // Patch continue targets -> jump to FOR_NEXT
    keel::usize forNextPos = currentPos();
    for (keel::usize pos : loopStack_.back().continuePatches) {
        patchJump(pos, forNextPos - pos);
    }

    // Emit source location for the 'end' keyword so breakpoints on it fire
    if (node->endLine > 0)
        currentLoc_ = {static_cast<keel::u16>(node->endLine), 1};

    emitAD(OpCode::FOR_NEXT, vReg, 0);
    chunk_.code.back().d =
        static_cast<keel::i32>(bodyStart) - static_cast<keel::i32>(currentPos() - 1);

    // Patch FOR_INIT endOffset -> jump past FOR_NEXT
    patchJump(forInitPos, currentPos() - forInitPos);
    emitUpvalStoreIfShared(node->strValue, vReg);

    // Patch break targets → jump to here (after loop)
    for (keel::usize pos : loopStack_.back().breakPatches) {
        patchJump(pos, currentPos() - pos);
    }

    loopStack_.pop_back();
    constRegCache_.clear(); scalarRegs_.reset(); // body constants may not have executed (empty range)
    return 0;
}

keel::u16 Compiler::compileColonExpr(const ASTNode *node)
{
    // No children = bare ":" (colon-all marker)
    if (node->children.empty()) {
        keel::u16 dst = tempReg();
        emitA(OpCode::COLON_ALL, dst);
        return dst;
    }

    // 2 children: start:stop
    if (node->children.size() == 2) {
        keel::u16 start = compileNode(node->children[0].get());
        keel::u16 stop = compileNode(node->children[1].get());
        keel::u16 dst = tempReg();
        emitABC(OpCode::COLON, dst, start, stop);
        return dst;
    }

    // 3 children: start:step:stop
    if (node->children.size() == 3) {
        keel::u16 start = compileNode(node->children[0].get());
        keel::u16 step = compileNode(node->children[1].get());
        keel::u16 stop = compileNode(node->children[2].get());
        keel::u16 dst = tempReg();
        // COLON3: dst, start, step, stop — need 4 regs
        // Use: a=dst, b=start, c=step, d is not suitable (int16), e=stop
        emit(Instruction::make_abcde(OpCode::COLON3, dst, start, step, 0, stop));
        return dst;
    }

    throw std::runtime_error("Compiler: invalid colon expression");
}

keel::u16 Compiler::compileMatrixLiteral(const ASTNode *node)
{
    // node->children = rows
    // Each row's children = elements

    if (node->children.empty()) {
        keel::u16 dst = tempReg();
        emitA(OpCode::LOAD_EMPTY, dst);
        return dst;
    }

    // Detect comma-separated-list elements: a FIELD_ACCESS or
    // DYNAMIC_FIELD_ACCESS on a struct-array base. We can't tell at
    // compile time whether the base is single or multi-element, so we
    // conservatively treat any FIELD_ACCESS-on-IDENTIFIER as a CSL
    // candidate when the row has more than one element. Single-struct
    // bases still produce one element (HORZCAT_APPEND_CSL of a 1-element
    // struct array yields a single-value horzcat — same as the regular
    // path).
    auto isCslCandidate = [](const ASTNode *elem) {
        return (elem->type == NodeType::FIELD_ACCESS && !elem->children.empty())
            || (elem->type == NodeType::DYNAMIC_FIELD_ACCESS && !elem->children.empty())
            // a cell brace-index: 1-D c{:} / c{vec} / c{i} or 2-D c{r,:} / c{:,j} (CSL)
            || (elem->type == NodeType::CELL_INDEX
                && (elem->children.size() == 2 || elem->children.size() == 3));
    };

    // Row compiler: compiles one row (a semicolon-separated line of the
    // literal) into a single register. Shared by the vertical
    // strategies below so both produce identical row semantics.
    constexpr keel::usize kLiteralBlockMax = 16;
    auto compileRow = [&](const ASTNode *row) -> keel::u16 {
        if (row->children.empty()) {
            keel::u16 dst = tempReg();
            emitA(OpCode::LOAD_EMPTY, dst);
            return dst;
        }

        // Single-element row: skip the HORZCAT path entirely UNLESS the
        // sole element is a CSL candidate — `[d.bytes]` may expand to
        // multiple values at runtime, so it has to go through the
        // append-CSL builder even though the AST has only one child.
        if (row->children.size() == 1 && !isCslCandidate(row->children[0].get())) {
            return compileNode(row->children[0].get());
        }

        bool hasCsl = false;
        for (auto &elem : row->children) {
            if (isCslCandidate(elem.get())) { hasCsl = true; break; }
        }

        if (hasCsl) {
            // Build dst incrementally. Seed as empty, then append each element.
            keel::u16 dst = tempReg();
            emitA(OpCode::LOAD_EMPTY, dst);
            for (auto &elem : row->children) {
                if (elem->type == NodeType::CELL_INDEX || elem->type == NodeType::DYNAMIC_FIELD_ACCESS) {
                    // cell CSL c{:} / c{vec} / c{idx} / c{r,:} or s.(f): compileNodeExpand
                    // preserves a CSL; HORZCAT_APPEND_FLATTEN appends all of a CSL or one
                    // of a non-CSL at runtime. See csl_first_class.md.
                    keel::u16 v = compileNodeExpand(elem.get());
                    emitAB(OpCode::HORZCAT_APPEND_FLATTEN, dst, v);
                } else if (elem->type == NodeType::FIELD_ACCESS && !elem->children.empty()) {
                    // struct-array field [s.field] or chained [s.sub.field]
                    keel::u16 baseReg = compileNode(elem->children[0].get());
                    keel::i32 nameIdx = addStringConstant(elem->strValue);
                    emit(Instruction::make_abcde(
                        OpCode::HORZCAT_APPEND_CSL, dst, baseReg, 0, nameIdx, 0));
                } else {
                    keel::u16 valReg = compileNode(elem.get());
                    emitAB(OpCode::HORZCAT_APPEND, dst, valReg);
                }
            }
            return dst;
        }

        // Multi-element row. Two compilation strategies:
        //
        // SMALL rows (≤ kLiteralBlockMax): compile each element, move
        // into a consecutive block, one HORZCAT — the single-opcode
        // vectorized fast path.
        //
        // LARGE rows: the ACCUMULATOR — seed dst with LOAD_EMPTY, then
        // HORZCAT_APPEND each element, releasing the element's temps
        // back to a mark after each append (the compileBlock
        // register-release pattern). Register usage is O(1) regardless
        // of the element count, so a literal of ANY size compiles
        // (bugs/closed/core/register-exhaustion-no-fallback — the
        // block path burned one register per element and died at 252).
        // Scalar appends hit the appendScalar fast path (amortised
        // O(1)); matrix-valued elements go through the generic
        // two-element horzcat (O(N²) total for exotic all-matrix
        // literals — documented, rare).
        if (row->children.size() > kLiteralBlockMax) {
            keel::u16 dst = tempReg();
            emitA(OpCode::LOAD_EMPTY, dst);
            const int mark = nextReg_;
            for (auto &elem : row->children) {
                keel::u16 v = compileNode(elem.get());
                emitAB(OpCode::HORZCAT_APPEND, dst, v);
                const int floor = mark > maxVarReg_ ? mark : maxVarReg_;
                if (nextReg_ > floor) {
                    nextReg_ = floor;
                    constRegCache_.clear();
                    scalarRegs_.reset();
                }
            }
            return dst;
        }

        // Multi-element row: compile each, move into consecutive block, HORZCAT
        keel::Array<keel::u16> elemRegs;
        for (auto &elem : row->children) {
            elemRegs.push_back(compileNode(elem.get()));
        }

        keel::u16 hBase = nextReg_;
        for (keel::usize i = 0; i < elemRegs.size(); ++i) {
            keel::u16 slot = tempReg();
            if (elemRegs[i] != slot) {
                emitAB(OpCode::MOVE, slot, elemRegs[i]);
            }
        }

        keel::u16 dst = tempReg();
        emitABC(OpCode::HORZCAT, dst, hBase, static_cast<keel::u16>(elemRegs.size()));
        return dst;
    };

    const keel::usize rowCount = node->children.size();
    if (rowCount == 1)
        return compileRow(node->children[0].get());

    // MANY rows: vertical accumulator with mark/reset per row — each
    // row's temps are dead the moment VERTCAT_APPEND consumes them, so
    // a 1000-row column literal `[1;2;3;…]` compiles in O(1)
    // registers (the block path burned one per row).
    if (rowCount > kLiteralBlockMax) {
        keel::u16 dst = tempReg();
        emitA(OpCode::LOAD_EMPTY, dst);
        const int mark = nextReg_;
        for (auto &row : node->children) {
            keel::u16 rr = compileRow(row.get());
            emitAB(OpCode::VERTCAT_APPEND, dst, rr);
            const int floor = mark > maxVarReg_ ? mark : maxVarReg_;
            if (nextReg_ > floor) {
                nextReg_ = floor;
                constRegCache_.clear();
                scalarRegs_.reset();
            }
        }
        return dst;
    }

    // Few rows: compile each, move into consecutive block, VERTCAT
    keel::Array<keel::u16> rowRegs;
    for (auto &row : node->children)
        rowRegs.push_back(compileRow(row.get()));

    keel::u16 vBase = nextReg_;
    for (keel::usize i = 0; i < rowRegs.size(); ++i) {
        keel::u16 slot = tempReg();
        if (rowRegs[i] != slot) {
            emitAB(OpCode::MOVE, slot, rowRegs[i]);
        }
    }

    keel::u16 dst = tempReg();
    emitABC(OpCode::VERTCAT, dst, vBase, static_cast<keel::u16>(rowRegs.size()));
    return dst;
}

keel::u16 Compiler::compileIndexExpr(const ASTNode *node)
{
    // For CALL nodes: children[0] = base identifier, children[1..] = index args
    // For INDEX nodes: strValue = name, children = index args

    keel::StringRef name;
    keel::usize nargs;
    keel::usize argOffset; // first index arg in children[]

    if (node->type == NodeType::CALL) {
        name = node->children[0]->strValue.as_ref();
        nargs = node->children.size() - 1;
        argOffset = 1;
    } else {
        name = node->strValue.as_ref();
        nargs = node->children.size();
        argOffset = 0;
    }

    keel::u16 arr = varRegRead(name);

    if (nargs == 1) {
        IndexContextGuard guard(*this, arr, 1);
        keel::u16 idx = compileNode(node->children[argOffset].get());
        keel::u16 dst = tempReg();
        emitABC(OpCode::INDEX_GET, dst, arr, idx);
        return dst;
    }

    if (nargs == 2) {
        IndexContextGuard guard(*this, arr, 2);
        keel::u16 row = compileNode(node->children[argOffset].get());
        guard.setDim(1);
        keel::u16 col = compileNode(node->children[argOffset + 1].get());
        keel::u16 dst = tempReg();
        emit(Instruction::make_abcde(OpCode::INDEX_GET_2D, dst, arr, row, 0, col));
        return dst;
    }

    // ND indexing (3D+): compile indices into consecutive registers
    {
        IndexContextGuard guard(*this, arr, static_cast<keel::u16>(nargs));
        keel::Array<keel::u16> idxRegs;
        for (keel::usize i = 0; i < nargs; ++i) {
            idxRegs.push_back(compileNode(node->children[argOffset + i].get()));
            if (i + 1 < nargs)
                guard.setDim(static_cast<keel::u16>(i + 1));
        }
        keel::u16 base = nextReg_;
        for (keel::usize i = 0; i < nargs; ++i) {
            keel::u16 slot = tempReg();
            if (idxRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, idxRegs[i]);
        }
        keel::u16 dst = tempReg();
        emit(Instruction::make_abcde(OpCode::INDEX_GET_ND,
                                     dst,
                                     arr,
                                     base,
                                     0,
                                     static_cast<keel::u16>(nargs)));
        return dst;
    }
}

keel::u16 Compiler::compileIndexAssign(const ASTNode *node)
{
    return compileIndexStore(node->children[0].get(), node->children[1].get(),
                             -1, node->suppressOutput);
}

keel::u16 Compiler::compileIndexStore(const ASTNode *lhs, const ASTNode *rhs,
                                    int inValReg, bool suppress)
{
    // lhs = CALL or INDEX node; rhs = RHS expr (used only when valReg < 0)

    // For CALL: children[0] = IDENTIFIER, children[1..] = indices
    // For INDEX: strValue = name, children = indices
    keel::StringRef name;
    keel::usize nargs;
    keel::usize argOffset;

    if (lhs->type == NodeType::CALL) {
        name = lhs->children[0]->strValue.as_ref();
        nargs = lhs->children.size() - 1;
        argOffset = 1;
    } else {
        name = lhs->strValue.as_ref();
        nargs = lhs->children.size();
        argOffset = 0;
    }

    keel::u16 arr = varRegWrite(name);
    emitUpvalLoadRawIfShared(name, arr);
    keel::u16 val = (inValReg >= 0) ? static_cast<keel::u16>(inValReg) : compileNode(rhs);

    {
        IndexContextGuard guard(*this, arr, static_cast<keel::u16>(nargs));

        if (nargs == 1) {
            keel::u16 idx = compileNode(lhs->children[argOffset].get());
            emitABC(OpCode::INDEX_SET, arr, idx, val);
        } else if (nargs == 2) {
            keel::u16 row = compileNode(lhs->children[argOffset].get());
            guard.setDim(1);
            keel::u16 col = compileNode(lhs->children[argOffset + 1].get());
            emit(Instruction::make_abcde(OpCode::INDEX_SET_2D, arr, row, col, 0, val));
        } else {
            // ND indexed assign (3D+)
            keel::Array<keel::u16> idxRegs;
            for (keel::usize i = 0; i < nargs; ++i) {
                idxRegs.push_back(compileNode(lhs->children[argOffset + i].get()));
                if (i + 1 < nargs)
                    guard.setDim(static_cast<keel::u16>(i + 1));
            }
            keel::u16 base = nextReg_;
            for (keel::usize i = 0; i < nargs; ++i) {
                keel::u16 slot = tempReg();
                if (idxRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, idxRegs[i]);
            }
            keel::u16 safeVal = tempReg();
            if (val != safeVal)
                emitAB(OpCode::MOVE, safeVal, val);
            emit(Instruction::make_abcde(OpCode::INDEX_SET_ND,
                                         arr,
                                         base,
                                         static_cast<keel::u16>(nargs),
                                         0,
                                         safeVal));
        }
    } // guard restores index context

    emitUpvalStoreIfShared(name, arr);
    if (!mayClear_)
        definedRegs_.set(arr);
    if (!suppress) {
        keel::i32 nameIdx = addStringConstant(name);
        emitAD(OpCode::DISPLAY, arr, nameIdx);
    }

    return arr;
}

// ============================================================
// Struct field access and assignment
// ============================================================

keel::u16 Compiler::compileFieldAccess(const ASTNode *node)
{
    // Bare qualified access `Pkg.member` / `ClassName.Const`: when the root
    // identifier is not a known variable, resolve as a 0-arg qualified call
    // (mirrors compileCall's qualified path) so namespace constants and
    // classdef Constant properties read without parentheses.
    {
        const ASTNode *root = nullptr;
        keel::String qualified = tryBuildQualifiedName(node, &root);
        if (!qualified.empty() && root) {
            const keel::String &rootName = root->strValue;
            const bool isLocalVar = varRegisters_.contains(rootName)
                                    || upvalueRegs_.contains(rootName);
            const bool isWsVar = isTopLevel_ && engine_.workspaceEnv().getLocal(rootName) != nullptr;
            if (!isLocalVar && !isWsVar) {
                // Split into dotted segments. Normally the whole name is one
                // qualified reference (`pkg.sub.fn`, `Class.Const`). But when
                // the full name is NOT registered yet a proper prefix IS
                // (e.g. an enum member `Weekday.Monday`), that prefix is a
                // VALUE and the trailing segments are field accesses on it
                // (`Weekday.Monday.num` → (Weekday.Monday).num). If nothing is
                // registered, keep the single CALL — the runtime resolves it
                // (an m-file qualified name loaded lazily).
                keel::Array<keel::StringRef> segs;
                for (keel::usize s = 0, e; s < qualified.size(); s = e + 1) {
                    e = qualified.find('.', s);
                    if (e == keel::kInvalidIndex)
                        e = qualified.size();
                    segs.push_back(qualified.substr(s, e - s));
                }
                auto join = [&](keel::usize n) {
                    keel::String r = keel::String::from(segs[0]);
                    for (keel::usize i = 1; i < n; ++i) {
                        r.push_back('.');
                        r.append(segs[i]);
                    }
                    return r;
                };
                keel::usize prefixLen = segs.size(); // default: the whole name
                if (segs.size() >= 3 && !engine_.hasExternalFunction(qualified))
                    for (keel::usize k = segs.size() - 1; k >= 2; --k)
                        if (engine_.hasExternalFunction(join(k))) {
                            prefixLen = k;
                            break;
                        }
                // MATLAB `gcf.Number`: NOTHING dotted is registered, but
                // the HEAD is a callable function name → the expression
                // is a 0-arg call of the head with field reads on the
                // result — exactly what the TreeWalker does. Class heads
                // are excluded: classdef Static/Constant members register
                // the full dotted name (caught above), and an
                // unregistered drift must not silently construct an
                // instance. Package-like heads (`pkg.sub.fn` m-files,
                // lazily resolved at runtime) are not callable names
                // either — they keep the qualified CALL below.
                // (bugs/closed/lang/dotted-rvalue-on-call-result.md)
                if (prefixLen == segs.size()
                    && !engine_.hasExternalFunction(qualified)
                    && !engine_.findClass(segs[0])
                    && !engine_.classCtor(segs[0])
                    && engine_.hasCallableName(segs[0])) {
                    keel::u16 hd = tempReg();
                    keel::u16 argBase = nextReg_;
                    keel::i32 funcIdx = addStringConstant(segs[0]);
                    emit(Instruction::make_abcde(OpCode::CALL, hd, argBase, 0,
                                                 funcIdx, 0));
                    for (keel::usize i = 1; i < segs.size(); ++i) {
                        keel::u16 nd = tempReg();
                        keel::i32 nameIdx = addStringConstant(segs[i]);
                        emitABC(OpCode::FIELD_GET, nd, hd, 0);
                        chunk_.code.back().d = nameIdx;
                        hd = nd;
                    }
                    return hd;
                }
                keel::u16 dst = tempReg();
                keel::u16 argBase = nextReg_;
                keel::i32 funcIdx = addStringConstant(join(prefixLen));
                emit(Instruction::make_abcde(OpCode::CALL, dst, argBase, 0, funcIdx, 0));
                // Trailing segments (if the prefix was a value) → field reads.
                for (keel::usize i = prefixLen; i < segs.size(); ++i) {
                    keel::u16 nd = tempReg();
                    keel::i32 nameIdx = addStringConstant(segs[i]);
                    emitABC(OpCode::FIELD_GET, nd, dst, 0);
                    chunk_.code.back().d = nameIdx;
                    dst = nd;
                }
                return dst;
            }
        }
    }
    // node->children[0] = object expression
    // node->strValue = field name
    keel::u16 obj = compileNode(node->children[0].get());
    keel::u16 dst = tempReg();
    keel::i32 nameIdx = addStringConstant(node->strValue);
    emitABC(OpCode::FIELD_GET, dst, obj, 0);
    // Store nameIdx in d field
    chunk_.code.back().d = nameIdx;
    return dst;
}

keel::u16 Compiler::compileFieldAssign(const ASTNode *node)
{
    return compileFieldStore(node->children[0].get(), node->children[1].get(),
                             -1, node->suppressOutput);
}

keel::u16 Compiler::compileFieldStore(const ASTNode *lhs, const ASTNode *rhs,
                                    int inValReg, bool suppress)
{
    // lhs = FIELD_ACCESS node; rhs = RHS expr (used only when valReg < 0)

    // Collect the field chain: s.a.b → ["b", "a"] (outermost first)
    // Walk from outermost FIELD_ACCESS inward to find root IDENTIFIER
    keel::Array<keel::StringRef> fieldChain;
    const ASTNode *cur = lhs;
    while (cur->type == NodeType::FIELD_ACCESS) {
        fieldChain.push_back(cur->strValue.as_ref());
        cur = cur->children[0].get();
    }

    // Struct-array element write: `d(i).field = v`. The root is a CALL
    // node with a single index. Emit STRUCT_ELEM_FIELD_SET — handles
    // auto-grow on out-of-range index. Nested chains (`d(i).a.b = v`)
    // fall through to the existing IDENTIFIER-rooted path on TW only;
    // VM rejects them here so callers get a clear error rather than
    // silent breakage.
    if (cur->type == NodeType::CALL && fieldChain.size() == 1
        && cur->children.size() == 2
        && cur->children[0]->type == NodeType::IDENTIFIER) {
        const keel::String &rootName = cur->children[0]->strValue;
        keel::u16 rootReg = varRegWrite(rootName);
        emitUpvalLoadRawIfShared(rootName, rootReg);
        keel::u16 idxReg;
        {
            IndexContextGuard guard(*this, rootReg, 1);
            idxReg = compileNode(cur->children[1].get());
        }
        keel::u16 valReg = (inValReg >= 0) ? static_cast<keel::u16>(inValReg)
                                          : compileNode(rhs);
        keel::i32 nameIdx = addStringConstant(fieldChain[0]);
        emit(Instruction::make_abcde(OpCode::STRUCT_ELEM_FIELD_SET,
                                     rootReg, idxReg, valReg, nameIdx, 0));
        emitUpvalStoreIfShared(rootName, rootReg);
        if (!mayClear_)
            definedRegs_.set(rootReg);
        if (!suppress) {
            keel::i32 dispIdx = addStringConstant(rootName);
            emitAD(OpCode::DISPLAY, rootReg, dispIdx);
        }
        return rootReg;
    }

    if (cur->type != NodeType::IDENTIFIER)
        throw std::runtime_error("Compiler: invalid nested field assign target");

    // fieldChain is in reverse: ["b", "a"] for s.a.b
    // Root variable
    keel::u16 rootReg = varRegWrite(cur->strValue);
    emitUpvalLoadRawIfShared(cur->strValue, rootReg);
    keel::u16 val = (inValReg >= 0) ? static_cast<keel::u16>(inValReg) : compileNode(rhs);

    if (fieldChain.size() == 1) {
        // Simple: s.x = val
        keel::i32 nameIdx = addStringConstant(fieldChain[0]);
        emitABC(OpCode::FIELD_SET, rootReg, val, 0);
        chunk_.code.back().d = nameIdx;
    } else {
        // Nested: s.a.b = val  →  fieldChain = ["b", "a"]
        // Step 1: FIELD_GET chain from root to second-to-last
        //   intermediates[0] = s (rootReg)
        //   intermediates[1] = FIELD_GET s."a" → t1
        //   For s.a.b.c: intermediates[2] = FIELD_GET t1."b" → t2
        keel::Array<keel::u16> intermediates;
        intermediates.push_back(rootReg);

        // Walk from deepest to shallowest: fieldChain reversed = ["a", "b"]
        for (keel::usize i = fieldChain.size() - 1; i >= 1; --i) {
            keel::u16 src = intermediates.back();
            keel::u16 dst = tempReg();
            keel::i32 nameIdx = addStringConstant(fieldChain[i]);
            emitABC(OpCode::FIELD_GET_OR_CREATE, dst, src, 0);
            chunk_.code.back().d = nameIdx;
            intermediates.push_back(dst);
        }

        // Unshare non-aliased parent containers before mutating the deepest
        // struct so nested field updates do not trigger O(N) COW clones.
        for (keel::usize idx = 0; idx + 1 < intermediates.size(); ++idx) {
            keel::usize fi = fieldChain.size() - 1 - idx;
            keel::i32 nameIdx = addStringConstant(fieldChain[fi]);
            emitABC(OpCode::LVAL_UNSHARE_FIELD, intermediates[idx + 1], intermediates[idx], 0);
            chunk_.code.back().d = nameIdx;
        }

        // Step 2: FIELD_SET on the deepest intermediate
        keel::u16 deepest = intermediates.back();
        keel::i32 leafIdx = addStringConstant(fieldChain[0]);
        emitABC(OpCode::FIELD_SET, deepest, val, 0);
        chunk_.code.back().d = leafIdx;

        // Step 3: Write back chain (reverse order)
        for (keel::usize i = intermediates.size() - 1; i >= 1; --i) {
            keel::u16 child = intermediates[i];
            keel::u16 parent = intermediates[i - 1];
            // fieldChain index: fieldChain[intermediates.size() - i]
            keel::usize fi = fieldChain.size() - i;
            keel::i32 nameIdx = addStringConstant(fieldChain[fi]);
            emitABC(OpCode::FIELD_SET, parent, child, 0);
            chunk_.code.back().d = nameIdx;
        }
    }

    emitUpvalStoreIfShared(cur->strValue, rootReg);
    if (!mayClear_)
        definedRegs_.set(rootReg);
    if (!suppress) {
        keel::i32 dispIdx = addStringConstant(cur->strValue);
        emitAD(OpCode::DISPLAY, rootReg, dispIdx);
    }

    return rootReg;
}

// ============================================================
// Cell operations
// ============================================================

keel::u16 Compiler::compileCellLiteral(const ASTNode *node)
{
    if (node->children.empty()) {
        keel::u16 dst = tempReg();
        emitABC(OpCode::CELL_LITERAL, dst, 0, 0);
        return dst;
    }

    // Each child is a row (BLOCK node). Same structure as matrix literals.
    // {a, b; c, d} → 2 BLOCK children: BLOCK(a,b) and BLOCK(c,d) → 2x2 cell
    // {a, b, c}    → 1 BLOCK child: BLOCK(a,b,c) → 1x3 cell

    // Compile each row into a cell row
    keel::Array<keel::u16> rowRegs;
    for (auto &row : node->children) {
        if (row->children.empty()) {
            keel::u16 dst = tempReg();
            emitABC(OpCode::CELL_LITERAL, dst, 0, 0);
            rowRegs.push_back(dst);
            continue;
        }

        // CSL row: any cell brace-index element c{sub} may expand to a comma-separated
        // list whose count is runtime-variable, so the row can't go through the fixed-
        // count CELL_LITERAL. First-class build: compile each element with
        // compileNodeExpand (preserving a CSL -- CELL_GET produces one for a multi-select,
        // including a variable subscript), then CELL_APPEND_FLATTEN, which appends all of
        // a CSL or one of a non-CSL at runtime. Handles {c{:}} / {c{vec}} / {c{idx}}
        // (variable) / {0, c{:}, 9} uniformly. A row with no brace index keeps the fast
        // fixed-count path below. See dev-docs/memory/csl_first_class.md.
        bool hasBraceIndex = false;
        for (auto &elem : row->children)
            if (elem->type == NodeType::CELL_INDEX
                || elem->type == NodeType::FIELD_ACCESS
                || elem->type == NodeType::DYNAMIC_FIELD_ACCESS) {
                hasBraceIndex = true;
                break;
            }
        if (hasBraceIndex) {
            keel::u16 acc = tempReg();
            emitABC(OpCode::CELL_LITERAL, acc, 0, 0);  // seed an empty 1x0 cell
            for (auto &elem : row->children) {
                keel::u16 v = compileNodeExpand(elem.get());  // preserve a CSL
                emitAB(OpCode::CELL_APPEND_FLATTEN, acc, v);
            }
            rowRegs.push_back(acc);
            continue;
        }

        // Compile row elements into consecutive registers → CELL_LITERAL
        keel::Array<keel::u16> elemRegs;
        for (auto &elem : row->children)
            elemRegs.push_back(compileNode(elem.get()));

        keel::u16 base = nextReg_;
        for (keel::usize i = 0; i < elemRegs.size(); ++i) {
            keel::u16 slot = tempReg();
            if (elemRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, elemRegs[i]);
        }

        keel::u16 dst = tempReg();
        emitABC(OpCode::CELL_LITERAL, dst, base, static_cast<keel::u16>(elemRegs.size()));
        rowRegs.push_back(dst);
    }

    if (rowRegs.size() == 1)
        return rowRegs[0];

    // Multiple rows: vertcat the cell rows
    keel::u16 vBase = nextReg_;
    for (keel::usize i = 0; i < rowRegs.size(); ++i) {
        keel::u16 slot = tempReg();
        if (rowRegs[i] != slot)
            emitAB(OpCode::MOVE, slot, rowRegs[i]);
    }

    keel::u16 dst = tempReg();
    emitABC(OpCode::VERTCAT, dst, vBase, static_cast<keel::u16>(rowRegs.size()));
    return dst;
}

keel::u16 Compiler::compileCellIndex(const ASTNode *node)
{
    // node->children[0] = cell variable
    // node->children[1..] = indices
    keel::u16 cell = compileNode(node->children[0].get());
    keel::usize nidx = node->children.size() - 1;

    if (nidx == 1) {
        IndexContextGuard guard(*this, cell, 1);
        keel::u16 idx = compileNode(node->children[1].get());
        keel::u16 dst = tempReg();
        emitABC(OpCode::CELL_GET, dst, cell, idx);
        return dst;
    }
    if (nidx == 2) {
        IndexContextGuard guard(*this, cell, 2);
        keel::u16 row = compileNode(node->children[1].get());
        guard.setDim(1);
        keel::u16 col = compileNode(node->children[2].get());
        keel::u16 dst = tempReg();
        emit(Instruction::make_abcde(OpCode::CELL_GET_2D, dst, cell, row, 0, col));
        return dst;
    }
    // ND cell indexing (3D+): use CELL_GET_ND — returns content, not sub-cell
    {
        IndexContextGuard guard(*this, cell, static_cast<keel::u16>(nidx));
        keel::Array<keel::u16> idxRegs;
        for (keel::usize i = 0; i < nidx; ++i) {
            idxRegs.push_back(compileNode(node->children[1 + i].get()));
            if (i + 1 < nidx)
                guard.setDim(static_cast<keel::u16>(i + 1));
        }
        keel::u16 base = nextReg_;
        for (keel::usize i = 0; i < nidx; ++i) {
            keel::u16 slot = tempReg();
            if (idxRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, idxRegs[i]);
        }
        keel::u16 dst = tempReg();
        emit(Instruction::make_abcde(OpCode::CELL_GET_ND,
                                     dst,
                                     cell,
                                     base,
                                     0,
                                     static_cast<keel::u16>(nidx)));
        return dst;
    }
}

keel::u16 Compiler::compileCellAssign(const ASTNode *node)
{
    return compileCellStore(node->children[0].get(), node->children[1].get(),
                            -1, node->suppressOutput);
}

keel::u16 Compiler::compileCellStore(const ASTNode *lhs, const ASTNode *rhs,
                                   int inValReg, bool suppress)
{
    // lhs = CELL_INDEX node; rhs = RHS expr (used only when inValReg < 0)
    auto *target = lhs->children[0].get();
    if (target->type != NodeType::IDENTIFIER)
        throw std::runtime_error("Compiler: invalid cell assignment target");

    keel::u16 cell = varRegWrite(target->strValue);
    emitUpvalLoadRawIfShared(target->strValue, cell);
    keel::u16 val = (inValReg >= 0) ? static_cast<keel::u16>(inValReg) : compileNode(rhs);

    keel::usize nidx = lhs->children.size() - 1;
    if (nidx == 1) {
        IndexContextGuard guard(*this, cell, 1);
        keel::u16 idx = compileNode(lhs->children[1].get());
        emitABC(OpCode::CELL_SET, cell, idx, val);
    } else if (nidx == 2) {
        IndexContextGuard guard(*this, cell, 2);
        keel::u16 row = compileNode(lhs->children[1].get());
        guard.setDim(1);
        keel::u16 col = compileNode(lhs->children[2].get());
        emit(Instruction::make_abcde(OpCode::CELL_SET_2D, cell, row, col, 0, val));
    } else {
        // ND cell assign
        IndexContextGuard guard(*this, cell, static_cast<keel::u16>(nidx));
        keel::Array<keel::u16> idxRegs;
        for (keel::usize i = 0; i < nidx; ++i) {
            idxRegs.push_back(compileNode(lhs->children[1 + i].get()));
            if (i + 1 < nidx)
                guard.setDim(static_cast<keel::u16>(i + 1));
        }
        keel::u16 base = nextReg_;
        for (keel::usize i = 0; i < nidx; ++i) {
            keel::u16 slot = tempReg();
            if (idxRegs[i] != slot)
                emitAB(OpCode::MOVE, slot, idxRegs[i]);
        }
        keel::u16 safeVal = tempReg();
        if (val != safeVal)
            emitAB(OpCode::MOVE, safeVal, val);
        emit(Instruction::make_abcde(OpCode::CELL_SET_ND,
                                     cell,
                                     base,
                                     static_cast<keel::u16>(nidx),
                                     0,
                                     safeVal));
    }

    emitUpvalStoreIfShared(target->strValue, cell);
    if (!mayClear_)
        definedRegs_.set(cell);
    if (!suppress) {
        keel::i32 nameIdx = addStringConstant(target->strValue);
        emitAD(OpCode::DISPLAY, cell, nameIdx);
    }

    return cell;
}

// Dispatch an already-materialised value register into any lvalue target.
// Used by compileMultiAssign for complex outputs (`[s.f, a(i)] = ...`).
void Compiler::compileStoreLValue(const ASTNode *target, keel::u16 valReg, bool suppress)
{
    if (target->type != NodeType::IDENTIFIER && needsGeneralLValuePath(target)) {
        compileLValueStore(target, nullptr, static_cast<int>(valReg), suppress);
        return;
    }
    switch (target->type) {
    case NodeType::IDENTIFIER: {
        keel::u16 dst = varRegWrite(target->strValue);
        if (dst != valReg)
            emitAB(OpCode::MOVE, dst, valReg);
        if (scalarRegs_.test(valReg))
            scalarRegs_.set(dst);
        else
            scalarRegs_.reset(dst);
        if (!mayClear_)
            definedRegs_.set(dst);
        emitUpvalStoreIfShared(target->strValue, dst);
        if (!suppress) {
            keel::i32 nameIdx = addStringConstant(target->strValue);
            emitAD(OpCode::DISPLAY, dst, nameIdx);
        }
        break;
    }
    case NodeType::CALL:
    case NodeType::INDEX:
        compileIndexStore(target, nullptr, valReg, suppress);
        break;
    case NodeType::FIELD_ACCESS:
        compileFieldStore(target, nullptr, valReg, suppress);
        break;
    case NodeType::CELL_INDEX:
        compileCellStore(target, nullptr, valReg, suppress);
        break;
    case NodeType::DYNAMIC_FIELD_ACCESS: {
        // s.(expr) = val
        auto *objNode = target->children[0].get();
        if (objNode->type != NodeType::IDENTIFIER)
            throw std::runtime_error("Compiler: dynamic field assign requires identifier target");
        keel::u16 obj = varRegWrite(objNode->strValue);
        emitUpvalLoadRawIfShared(objNode->strValue, obj);
        keel::u16 nameReg = compileNode(target->children[1].get());
        emitABC(OpCode::FIELD_SET_DYN, obj, nameReg, valReg);
        emitUpvalStoreIfShared(objNode->strValue, obj);
        if (!mayClear_)
            definedRegs_.set(obj);
        if (!suppress) {
            keel::i32 dispIdx = addStringConstant(objNode->strValue);
            emitAD(OpCode::DISPLAY, obj, dispIdx);
        }
        break;
    }
    default:
        throw std::runtime_error("Compiler: unsupported multi-assign target");
    }
}

bool Compiler::needsGeneralLValuePath(const ASTNode *lhs) const
{
    switch (lhs->type) {
    case NodeType::INDEX:
        return false; // `name(idx)` — root-level, simple
    // index-assign: simple iff the indexed object is a bare identifier
    case NodeType::CALL:
    case NodeType::CELL_INDEX:
    case NodeType::DYNAMIC_FIELD_ACCESS:
        return lhs->children[0]->type != NodeType::IDENTIFIER;
    case NodeType::FIELD_ACCESS: {
        const ASTNode *cur = lhs;
        int fieldChainLen = 0;
        while (cur->type == NodeType::FIELD_ACCESS) {
            ++fieldChainLen;
            cur = cur->children[0].get();
        }
        if (cur->type == NodeType::IDENTIFIER)
            return false; // a.b.c — compileFieldStore handles nested fields
        if (cur->type == NodeType::CALL && fieldChainLen == 1
            && cur->children.size() == 2 // exactly one subscript
            && cur->children[0]->type == NodeType::IDENTIFIER)
            return false; // d(i).field — struct-elem fast branch (1-D only)
        return true;
    }
    default:
        return false;
    }
}

namespace {
// True if `n` references `end` at THIS index level (not inside a nested
// indexing scope, which owns its own `end`). Used to keep `s.arr(end)`
// on the FIELD_GET path (where the index context is set correctly)
// rather than the runtime-deferred CALL_METHOD path.
bool refsTopLevelEnd(const ASTNode *n)
{
    if (!n)
        return false;
    if (n->type == NodeType::END_VAL)
        return true;
    if (n->type == NodeType::CALL || n->type == NodeType::INDEX
        || n->type == NodeType::CELL_INDEX)
        return false; // nested index scope binds its own `end`
    for (const auto &c : n->children)
        if (refsTopLevelEnd(c.get()))
            return true;
    return false;
}

// One link of a flattened lvalue accessor chain (root → outermost).
struct LvAcc
{
    enum Kind { Field, DynField, Index, Cell } kind = Field;
    const ASTNode *node = nullptr; // the accessor AST node
    bool structElem = false;   // Index used as an intermediate container ⇒
                               // struct-array element (`d(i).field…`)
};
} // namespace

keel::u16 Compiler::compileLValueStore(const ASTNode *lhs, const ASTNode *rhsNode,
                                     int inValReg, bool suppress)
{
    // ── Flatten the accessor chain (root → outermost) ──
    keel::Array<LvAcc> acc;
    const ASTNode *cur = lhs;
    for (;;) {
        if (cur->type == NodeType::FIELD_ACCESS)
            acc.push_back({LvAcc::Field, cur, false});
        else if (cur->type == NodeType::DYNAMIC_FIELD_ACCESS)
            acc.push_back({LvAcc::DynField, cur, false});
        else if (cur->type == NodeType::CALL)
            acc.push_back({LvAcc::Index, cur, false});
        else if (cur->type == NodeType::CELL_INDEX)
            acc.push_back({LvAcc::Cell, cur, false});
        else
            break;
        cur = cur->children[0].get();
    }
    if (cur->type != NodeType::IDENTIFIER)
        throw std::runtime_error("Compiler: unsupported lvalue root");
    std::reverse(acc.begin(), acc.end());
    const keel::usize n = acc.size();
    // A paren-index that is NOT the final accessor indexes into a
    // struct array to reach an element used as a container for the rest
    // of the chain (`d(i).a.b`).
    for (keel::usize k = 0; k + 1 < n; ++k)
        if (acc[k].kind == LvAcc::Index)
            acc[k].structElem = true;

    // Value to store (multi-assign output register, or compile the RHS).
    keel::u16 val = (inValReg >= 0) ? static_cast<keel::u16>(inValReg) : compileNode(rhsNode);

    keel::u16 rootReg = varRegWrite(cur->strValue);
    emitUpvalLoadRawIfShared(cur->strValue, rootReg);

    // regs[k] = container that accessor k applies to (regs[0] = root).
    keel::Array<keel::u16> regs;
    regs.resize(n + 1, 0);
    regs[0] = rootReg;
    // Per-accessor subscripts packed into a consecutive register block
    // [idxBase, idxBase+idxN) (for the (base, nargs) opcodes), or a
    // dynamic-field-name register. Computed once in the forward pass and
    // reused for write-back so `end` / side-effecting subscripts evaluate
    // exactly once.
    keel::Array<keel::u16> idxBase, idxN, nameReg;
    idxBase.resize(n, 0);
    idxN.resize(n, 0);
    nameReg.resize(n, 0);

    auto compileIdxArgs = [&](keel::usize k) {
        const ASTNode *node = acc[k].node;
        keel::usize nargs = node->children.size() - 1; // children[0] is the object
        keel::Array<keel::u16> tmp;
        {
            IndexContextGuard guard(*this, regs[k], static_cast<keel::u16>(nargs));
            for (keel::usize a = 0; a < nargs; ++a) {
                if (a > 0)
                    guard.setDim(static_cast<keel::u16>(a));
                tmp.push_back(compileNode(node->children[a + 1].get()));
            }
        }
        idxN[k] = static_cast<keel::u16>(nargs);
        if (nargs == 1) {
            idxBase[k] = tmp[0]; // single subscript is trivially "consecutive"
        } else {
            keel::u16 base = nextReg_;
            for (keel::usize a = 0; a < nargs; ++a) {
                keel::u16 slot = tempReg();
                if (tmp[a] != slot)
                    emitAB(OpCode::MOVE, slot, tmp[a]);
            }
            idxBase[k] = base;
        }
    };

    // ── Forward pass: compute subscripts and load intermediate containers ──
    for (keel::usize k = 0; k < n; ++k) {
        if (acc[k].kind == LvAcc::DynField)
            nameReg[k] = compileNode(acc[k].node->children[1].get());
        else if (acc[k].kind != LvAcc::Field)
            compileIdxArgs(k);

        if (k + 1 < n) {
            keel::u16 dst = tempReg();
            switch (acc[k].kind) {
            case LvAcc::Field: {
                keel::i32 nameIdx = addStringConstant(acc[k].node->strValue);
                emitABC(OpCode::FIELD_GET_OR_CREATE, dst, regs[k], 0);
                chunk_.code.back().d = nameIdx;
                break;
            }
            case LvAcc::DynField:
                emitABC(OpCode::FIELD_GET_OR_CREATE_DYN, dst, regs[k], nameReg[k]);
                break;
            case LvAcc::Index: // intermediate ⇒ struct-array element (any rank)
                emit(Instruction::make_abcde(OpCode::STRUCT_ELEM_GET_OR_CREATE,
                                             dst, regs[k], idxBase[k], 0, idxN[k]));
                break;
            case LvAcc::Cell:
                emit(Instruction::make_abcde(OpCode::CELL_GET_OR_CREATE,
                                             dst, regs[k], idxBase[k], 0, idxN[k]));
                break;
            }
            regs[k + 1] = dst;
        }
    }

    // emitSet: apply accessor k as a SET on regs[k] with `value`.
    auto emitSet = [&](keel::usize k, keel::u16 value) {
        const keel::u16 b = idxBase[k], na = idxN[k];
        switch (acc[k].kind) {
        case LvAcc::Field: {
            keel::i32 nameIdx = addStringConstant(acc[k].node->strValue);
            emitABC(OpCode::FIELD_SET, regs[k], value, 0);
            chunk_.code.back().d = nameIdx;
            break;
        }
        case LvAcc::DynField:
            emitABC(OpCode::FIELD_SET_DYN, regs[k], nameReg[k], value);
            break;
        case LvAcc::Cell:
            if (na == 1)
                emitABC(OpCode::CELL_SET, regs[k], b, value);
            else if (na == 2)
                emit(Instruction::make_abcde(OpCode::CELL_SET_2D, regs[k],
                                             b, b + 1, 0, value));
            else
                emit(Instruction::make_abcde(OpCode::CELL_SET_ND, regs[k],
                                             b, na, 0, value));
            break;
        case LvAcc::Index:
            if (acc[k].structElem) {
                emit(Instruction::make_abcde(OpCode::STRUCT_ELEM_SET, regs[k],
                                             b, na, 0, value));
            } else if (na == 1) {
                emitABC(OpCode::INDEX_SET, regs[k], b, value);
            } else if (na == 2) {
                emit(Instruction::make_abcde(OpCode::INDEX_SET_2D, regs[k],
                                             b, b + 1, 0, value));
            } else {
                emit(Instruction::make_abcde(OpCode::INDEX_SET_ND, regs[k],
                                             b, na, 0, value));
            }
            break;
        }
    };

    // ── Unshare non-aliased intermediate containers before mutating regs[n-1] ──
    for (keel::usize k = 0; k + 1 < n; ++k) {
        switch (acc[k].kind) {
        case LvAcc::Field: {
            keel::i32 nameIdx = addStringConstant(acc[k].node->strValue);
            emitABC(OpCode::LVAL_UNSHARE_FIELD, regs[k + 1], regs[k], 0);
            chunk_.code.back().d = nameIdx;
            break;
        }
        case LvAcc::DynField:
            emit(Instruction::make_abcde(OpCode::LVAL_UNSHARE_FIELD, regs[k + 1],
                                         regs[k], 1, 0, nameReg[k]));
            break;
        case LvAcc::Cell:
            emit(Instruction::make_abcde(OpCode::LVAL_UNSHARE_CELL, regs[k + 1],
                                         regs[k], idxBase[k], 0, idxN[k]));
            break;
        case LvAcc::Index:
            break;
        }
    }

    // ── Final write on the deepest container, then write-back up. ──
    emitSet(n - 1, val);
    for (keel::usize k = n - 1; k-- > 0;)
        emitSet(k, regs[k + 1]);

    emitUpvalStoreIfShared(cur->strValue, rootReg);
    if (!mayClear_)
        definedRegs_.set(rootReg);
    if (!suppress) {
        keel::i32 nameIdx = addStringConstant(cur->strValue);
        emitAD(OpCode::DISPLAY, rootReg, nameIdx);
    }
    return rootReg;
}

// ============================================================
// Phase 4: Function calls
// ============================================================

// Record the arg names from a CALL AST node into the chunk's
// per-call-site side table. Each child[1..] of `callNode` is an arg
// expression; if it's a bare IDENTIFIER, we record the name (so
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
        chunk_.callSiteArgNames.put(static_cast<keel::u32>(callInstrIdx),
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
    const keel::u16 opBase = static_cast<keel::u16>(nextReg_);
    for (keel::u16 r : opRegs) {
        keel::u16 slot = tempReg();
        if (r != slot) emitAB(OpCode::MOVE, slot, r);
    }
    // 2. FUSE_EWISE with placeholder dst (a) and skip (e) — patched in step 4.
    emit(Instruction::make_abcde(OpCode::FUSE_EWISE, /*dst*/ 0, opBase,
                                 static_cast<keel::u16>(operands.size()),
                                 static_cast<keel::i32>(ruleIdx), /*skip*/ 0));
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
            fusedOperandRegs_.put(operands[i], static_cast<keel::u16>(opBase + i));
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
    chunk_.code[fusePos].e = static_cast<keel::u16>(span);
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

int Compiler::tryCompileFused(const ASTNode *node)
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
    if (int r = tryCompileFused(node); r >= 0) return static_cast<keel::u16>(r);

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
                                             static_cast<keel::u16>(nargs),
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
            IndexContextGuard guard(*this, objReg, static_cast<keel::u16>(nargs));
            for (keel::usize i = 1; i < node->children.size(); ++i) {
                guard.setDim(static_cast<keel::u16>(i - 1));
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
                                     nameIdx, static_cast<keel::u16>(argRegs.size())));
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
                                         static_cast<keel::u16>(argRegs.size()),
                                         std::max<keel::usize>(nargoutContext_, 1)));
            return dst;
        }
        keel::Array<keel::u16> argRegs;
        argRegs.reserve(nargs);
        {
            IndexContextGuard guard(*this, fhReg, static_cast<keel::u16>(nargs));
            for (keel::usize i = 1; i < node->children.size(); ++i) {
                guard.setDim(static_cast<keel::u16>(i - 1));
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
                                     static_cast<keel::u16>(argRegs.size())));
        recordCallArgNames(node, callIdx);
        return dst;
    }

    const keel::String &name = funcNode->strValue;
    keel::usize nargs = node->children.size() - 1;

    if (name == "syms") {
        auto isSymsFlag = [](keel::StringRef s) noexcept {
            return s == "real" || s == "positive" || s == "negative"
                   || s == "nonnegative" || s == "integer" || s == "clear";
        };
        for (keel::usize i = 1; i <= nargs; ++i) {
            auto *argNode = node->children[i].get();
            if (argNode->type == NodeType::STRING_LITERAL) {
                const keel::String &tok = argNode->strValue;
                if (!isSymsFlag(tok) && !engine_.isReservedName(tok)) {
                    keel::u16 reg = varRegWrite(tok);
                    if (!mayClear_)
                        definedRegs_.set(reg);
                }
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
                emitAB(OpCode::CLEAR_DYN, nameReg, static_cast<keel::u16>(nargs));
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
                                         static_cast<keel::u16>(nargs),
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
                emitAB(op, base, static_cast<keel::u16>(argRegs.size()));
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
                                         static_cast<keel::u16>(argRegs.size()),
                                         std::max<keel::usize>(nargoutContext_, 1)));
            return dst;
        }

        IndexContextGuard guard(*this, fhReg, static_cast<keel::u16>(nargs));

        keel::Array<keel::u16> argRegs;
        for (keel::usize i = 1; i < node->children.size(); ++i) {
            guard.setDim(static_cast<keel::u16>(i - 1));
            argRegs.push_back(compileNode(node->children[i].get()));
        }

        keel::u16 argBase = static_cast<keel::u16>(nextReg_);
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
                                     static_cast<keel::u16>(argRegs.size())));
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
            keel::u16 argBase = static_cast<keel::u16>(nextReg_);
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
                                         static_cast<keel::u16>(argRegs.size()),
                                         funcIdx,
                                         callNargout));
            if (!anyBraceArg)
                recordCallArgNames(node, callIdx);
            return dst;
        }
    }

    // Fast path: 1-arg inline builtins — skip MOVE, pass arg register directly
    if (!mayInjectVars_) {
        keel::i8 builtinId = resolveBuiltinId(name, nargs);
        if (builtinId >= 0 && nargs == 1) {
            keel::u16 arg = compileNode(node->children[1].get());
            keel::u16 dst = tempReg();
            emit(Instruction::make_abcde(OpCode::CALL_BUILTIN,
                                         dst, arg, 1,
                                         static_cast<keel::i32>(builtinId), 0));
            constexpr keel::i8 kMaxUnaryInlineBuiltinId = 15;
            if (scalarRegs_.test(arg) && builtinId <= kMaxUnaryInlineBuiltinId)
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
        keel::i32 funcIdx = addStringConstant(name);
        emit(Instruction::make_abcde(OpCode::CALL_FLATTEN, dst, argBase,
                                     static_cast<keel::u16>(argRegs.size()), funcIdx,
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

    // Try to resolve as inline scalar builtin (numeric ID)
    keel::i8 builtinId = mayInjectVars_ ? -1 : resolveBuiltinId(name, nargs);
    if (builtinId >= 0) {
        emit(Instruction::make_abcde(OpCode::CALL_BUILTIN,
                                     dst,
                                     argBase,
                                     static_cast<keel::u16>(nargs),
                                     static_cast<keel::i32>(builtinId),
                                     0));
        return dst;
    }

    // General CALL
    keel::i32 funcIdx = addStringConstant(name);
    keel::usize callIdx = chunk_.code.size();
    emit(Instruction::make_abcde(OpCode::CALL,
                                 dst,
                                 argBase,
                                 static_cast<keel::u16>(nargs),
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
        && name != "exist" && name != "syms"
        && (varRegisters_.contains(name) || upvalueRegs_.contains(name)
            || engine_.isReservedName(name) != 0)) {
        const keel::String &arg = node->children[0]->strValue;
        if (arg.size() >= 2 && (arg[0] == '-' || arg[0] == '/')) {
            const keel::String rest = keel::String::from(arg.substr(1));
            char *endPtr = nullptr;
            double numVal = std::strtod(rest.c_str(), &endPtr);
            const bool isNum = (endPtr != rest.c_str() && *endPtr == '\0');
            auto isIdent = [](keel::StringRef s) {
                if (s.empty())
                    return false;
                unsigned char c0 = static_cast<unsigned char>(s[0]);
                if (!keel::is_ascii_alpha(c0) && c0 != '_')
                    return false;
                for (keel::usize k = 1; k < s.size(); ++k) {
                    unsigned char ck = static_cast<unsigned char>(s[k]);
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
                                         static_cast<keel::u16>(nargs),
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
                emitAB(op, base, static_cast<keel::u16>(nargs));
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
    if (name == "syms") {
        auto isSymsFlag = [](keel::StringRef s) noexcept {
            return s == "real" || s == "positive" || s == "negative"
                   || s == "nonnegative" || s == "integer" || s == "clear";
        };
        for (keel::usize i = 0; i < nargs; ++i) {
            const keel::String &tok = node->children[i]->strValue;
            if (!isSymsFlag(tok) && !engine_.isReservedName(tok)) {
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
    keel::i32 funcIdx = addStringConstant(
        ninfo ? ninfo->compiledName.as_ref() : name.as_ref());
    emit(
        Instruction::make_abcde(OpCode::CALL, dst, argBase,
                                static_cast<keel::u16>(totalCallArgs), funcIdx, 0));

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
        Value fh = Value::funcHandle(node->strValue, engine_.allocator());
        keel::i32 constIdx = appendConstant(keel::move(fh));
        emitAD(OpCode::LOAD_CONST, dst, constIdx);
        return dst;
    }

    // Case 2: @(params) expr — anonymous function with possible closure capture.
    int id = engine_.anonNameCounter_.fetch_add(1, std::memory_order_relaxed);
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
    compiledFuncs_.put(anonName.clone(), keel::move(funcChunk));

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
    emitABC(OpCode::CLOSURE_MAKE, dst, base, static_cast<keel::u16>(totalSlots));
    return dst;
}

keel::u16 Compiler::compileSuperCall(const ASTNode *callNode, const ASTNode *superRef,
                                   int explicitOutBase, keel::u16 nout)
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
    const keel::u16 nargs = static_cast<keel::u16>(ar.size());

    if (isCtor) {
        // The super-ctor initialises obj in place: MATLAB's `obj@Base(a)`
        // is a statement form — no assignment needed. Write the result
        // straight to the obj register (varRegRead returns the
        // variable's own register, so dst == objReg updates obj
        // directly; the assign form's follow-up MOVE is skipped by
        // compileAssign's dst==src check). Saves the extra temp
        // register the tempReg()+MOVE sequence spent
        // (bugs/closed/core/super-ctor-result-discarded.md).
        keel::u16 dst = (explicitOutBase >= 0) ? static_cast<keel::u16>(explicitOutBase)
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
        outBase = static_cast<keel::u16>(explicitOutBase);
    } else {
        outBase = reserveRegs(std::max<keel::u16>(nout, 1));
    }
    keel::i32 qualIdx = addStringConstant(keel::format("{}>{}", base, lhs->strValue));
    emit(Instruction::make_abcde(OpCode::CALL_SUPER_METHOD, outBase, argBase, nargs, qualIdx,
                                 nout));
    return outBase;
}

keel::u16 Compiler::compileFunctionDef(const ASTNode *node)
{
    // Nested functions inside an enclosing function body are pre-compiled
    // into scoped "outer>inner" chunks by compileFunction's prologue and must
    // never leak into the global / script-level function table.
    if (inFunctionBody_)
        return 0;

    // Compile function body into a separate BytecodeChunk. Which
    // bucket it lands in depends on whether we're inside a script
    // scope — script-local functions live in their own store so
    // `clear all` can't touch them (MATLAB parity).
    BytecodeChunk funcChunk = compileFunction(node);

    UserFunction uf;
    uf.name = node->strValue.clone();
    uf.params = node->paramNames.clone();
    uf.returns = node->returnNames.clone();
    uf.body = keel::Arc<const ASTNode>(cloneNode(node->children[0].get()));
    uf.closureEnv = nullptr;

    if (scriptDepth_ > 0) {
        scriptLocalCompiledFuncs_.put(node->strValue.clone(), keel::move(funcChunk));
        engine_.adoptUserFunction(node->strValue, keel::move(uf), /*scriptScope=*/true);
    } else {
        compiledFuncs_.put(node->strValue.clone(), keel::move(funcChunk));
        engine_.adoptUserFunction(node->strValue, keel::move(uf), /*scriptScope=*/false);
    }
    return 0;
}

void Compiler::registerFunction(const ASTNode *funcDef)
{
    // compileFunction (invoked by compileFunctionDef) already saves and
    // restores chunk_/varRegisters_/… so this is safe on an otherwise-idle
    // Compiler.
    compileFunctionDef(funcDef);
}

void Compiler::registerFunctionAs(keel::StringRef qualifiedName,
                                   const ASTNode *funcDef)
{
    AnonOriginSentry sentry(*this, /*known=*/false, keel::String{}, keel::String{}, keel::String{});
    registerFunctionAsImpl_(qualifiedName, funcDef);
}

void Compiler::registerFunctionAs(keel::StringRef qualifiedName,
                                   const ASTNode *funcDef, keel::StringRef fsName,
                                   keel::StringRef scriptDir,
                                   keel::StringRef scriptFile)
{
    AnonOriginSentry sentry(*this, /*known=*/true, keel::String::from(fsName),
                            keel::String::from(scriptDir), keel::String::from(scriptFile));
    registerFunctionAsImpl_(qualifiedName, funcDef);
}

Compiler::AnonOriginSentry::AnonOriginSentry(Compiler &c, bool known, keel::String fs,
                                             keel::String dir, keel::String file)
    : c_(c)
    , known_(c.anonOriginKnown_)
    , fs_(keel::move(c.anonOriginFs_))
    , dir_(keel::move(c.anonOriginDir_))
    , file_(keel::move(c.anonOriginFile_))
{
    c_.anonOriginKnown_ = known;
    c_.anonOriginFs_ = keel::move(fs);
    c_.anonOriginDir_ = keel::move(dir);
    c_.anonOriginFile_ = keel::move(file);
}

Compiler::AnonOriginSentry::~AnonOriginSentry()
{
    c_.anonOriginKnown_ = known_;
    c_.anonOriginFs_ = keel::move(fs_);
    c_.anonOriginDir_ = keel::move(dir_);
    c_.anonOriginFile_ = keel::move(file_);
}

void Compiler::registerFunctionAsImpl_(keel::StringRef qualifiedName,
                                        const ASTNode *funcDef)
{
    BytecodeChunk funcChunk;
    if (funcDef->strValue == qualifiedName) {
        funcChunk = compileFunction(funcDef);
    } else {
        ASTNode fn(NodeType::FUNCTION_DEF);
        fn.strValue = keel::String::from(qualifiedName);
        fn.paramNames = funcDef->paramNames.clone();
        fn.returnNames = funcDef->returnNames.clone();
        fn.line = funcDef->line;
        fn.col = funcDef->col;
        fn.endLine = funcDef->endLine;
        if (!funcDef->children.empty())
            fn.children.push_back(cloneNode(funcDef->children[0].get()));
        funcChunk = compileFunction(&fn);
    }

    UserFunction uf;
    uf.name = keel::String::from(qualifiedName);
    uf.params = funcDef->paramNames.clone();
    uf.returns = funcDef->returnNames.clone();
    uf.body = keel::Arc<const ASTNode>(cloneNode(funcDef->children[0].get()));
    uf.closureEnv = nullptr;

    if (scriptDepth_ > 0) {
        scriptLocalCompiledFuncs_.put(keel::String::from(qualifiedName), keel::move(funcChunk));
        engine_.adoptUserFunction(qualifiedName, keel::move(uf), /*scriptScope=*/true);
    } else {
        compiledFuncs_.put(keel::String::from(qualifiedName), keel::move(funcChunk));
        engine_.adoptUserFunction(qualifiedName, keel::move(uf), /*scriptScope=*/false);
    }
}

void Compiler::setChunkOrigin(keel::StringRef qualifiedName,
                              keel::StringRef fsName, keel::StringRef scriptDir,
                              keel::StringRef scriptFile)
{
    auto stampMap = [&](keel::HashMap<keel::String, BytecodeChunk> &tbl) {
        const keel::String prefix = keel::format("{}>", qualifiedName);
        for (auto [k, ch] : tbl) {
            if (k == qualifiedName
                || (k.size() > prefix.size() && k.starts_with(prefix))) {
                ch.originKnown = true;
                ch.originFs.assign(fsName);
                ch.originDir.assign(scriptDir);
                ch.originFile.assign(scriptFile);
            }
        }
    };
    stampMap(compiledFuncs_);
    stampMap(scriptLocalCompiledFuncs_);
}

void Compiler::eraseCompiledFunc(keel::StringRef name)
{
    compiledFuncs_.remove(name);
    const keel::String prefix = keel::format("{}>", name);
    keel::Array<keel::String> toRemove;
    for (auto [k, ch] : compiledFuncs_) {
        if (k.size() > prefix.size() && k.starts_with(prefix))
            toRemove.push_back(k.clone());
    }
    for (const auto &k : toRemove)
        compiledFuncs_.remove(k);
}

void Compiler::eraseCompiledFuncsForClass(keel::StringRef className)
{
    // Method chunks are keyed by their fully-qualified name "ClassName>method"
    // (set in ensureClassMethodCompiled). Erase every such key so a redefined
    // class recompiles fresh bodies rather than serving the stale cache — the
    // root of "I rewrote the method but behaviour didn't change". Inherited
    // methods keep their own "Base>method" keys and are untouched.
    const keel::String prefix = keel::format("{}>", className);
    keel::Array<keel::String> toRemove;
    for (auto [k, ch] : compiledFuncs_) {
        if (k.size() > prefix.size() && k.starts_with(prefix))
            toRemove.push_back(k.clone());
    }
    for (const auto &k : toRemove)
        compiledFuncs_.remove(k);
}

const BytecodeChunk *Compiler::ensureClassMethodCompiled(const UserFunction &uf)
{
    if (const BytecodeChunk *existing = findCompiled(uf.name))
        return existing;
    ASTNode fn(NodeType::FUNCTION_DEF);
    fn.strValue = uf.name.clone();
    fn.paramNames = uf.params.clone();
    fn.returnNames = uf.returns.clone();
    if (uf.body)
        fn.children.push_back(cloneNode(uf.body.get()));
    BytecodeChunk chunk;
    {
        AnonOriginSentry sentry(*this, uf.originKnown, uf.sourceFs.clone(),
                                 uf.sourceDir.clone(), uf.sourceFile.clone());
        chunk = compileFunction(&fn);
    }
    chunk.name = uf.name.clone();
    // The method body runs under the classdef file's provenance —
    // mfilename() inside reports the class file (registerClassDef
    // stamped it on the UserFunction; inherited methods carry their
    // declaring class's file).
    chunk.originKnown = uf.originKnown;
    chunk.originFs = uf.sourceFs.clone();
    chunk.originDir = uf.sourceDir.clone();
    chunk.originFile = uf.sourceFile.clone();
    auto ins = compiledFuncs_.try_insert(uf.name.clone(), keel::move(chunk));
    return ins.value;
}

const BytecodeChunk *Compiler::findCompiled(keel::StringRef name) const
{
    if (const BytecodeChunk *c = scriptLocalCompiledFuncs_.get(name))
        return c;
    return compiledFuncs_.get(name);
}

void Compiler::beginScriptScope()
{
    savedScriptLocalCompiledFuncs_.push_back(keel::move(scriptLocalCompiledFuncs_));
    scriptLocalCompiledFuncs_.clear();
    ++scriptDepth_;
}

void Compiler::endScriptScope()
{
    if (scriptDepth_ == 0)
        return;
    --scriptDepth_;
    if (savedScriptLocalCompiledFuncs_.empty()) {
        scriptLocalCompiledFuncs_.clear();
        return;
    }
    scriptLocalCompiledFuncs_ = keel::move(savedScriptLocalCompiledFuncs_.back());
    savedScriptLocalCompiledFuncs_.pop_back();
}

void Compiler::promoteScriptLocalsToWorkspace()
{
    for (auto [k, ch] : scriptLocalCompiledFuncs_)
        compiledFuncs_.put(k.clone(), keel::move(ch));
    scriptLocalCompiledFuncs_.clear();
}

namespace {

void collectScopeGlobalPersistentDecls(const ASTNode *node,
                                       keel::Array<keel::String> &globals,
                                       keel::Array<keel::String> &persistents)
{
    if (!node || node->type == NodeType::FUNCTION_DEF || node->type == NodeType::ANON_FUNC)
        return;
    if (node->type == NodeType::GLOBAL_STMT) {
        for (const auto &nm : node->paramNames) {
            bool found = false;
            for (const auto &g : globals)
                if (g == nm) { found = true; break; }
            if (!found)
                globals.push_back(nm.clone());
        }
        return;
    }
    if (node->type == NodeType::PERSISTENT_STMT) {
        for (const auto &nm : node->paramNames) {
            bool found = false;
            for (const auto &p : persistents)
                if (p == nm) { found = true; break; }
            if (!found)
                persistents.push_back(nm.clone());
        }
        return;
    }
    for (const auto &ch : node->children)
        collectScopeGlobalPersistentDecls(ch.get(), globals, persistents);
    for (const auto &[cond, body] : node->branches) {
        collectScopeGlobalPersistentDecls(cond.get(), globals, persistents);
        collectScopeGlobalPersistentDecls(body.get(), globals, persistents);
    }
    if (node->elseBranch)
        collectScopeGlobalPersistentDecls(node->elseBranch.get(), globals, persistents);
}

void collectDirectNestedDefs(const ASTNode *node, keel::Array<const ASTNode *> &out)
{
    if (!node || node->type == NodeType::ANON_FUNC)
        return;
    if (node->type == NodeType::FUNCTION_DEF) {
        out.push_back(node);
        return;
    }
    for (const auto &ch : node->children)
        collectDirectNestedDefs(ch.get(), out);
    for (const auto &[cond, body] : node->branches) {
        collectDirectNestedDefs(cond.get(), out);
        collectDirectNestedDefs(body.get(), out);
    }
    if (node->elseBranch)
        collectDirectNestedDefs(node->elseBranch.get(), out);
}

bool isPseudoScopeVar(keel::StringRef name)
{
    return name == "nargin" || name == "nargout" || name == "varargin" || name == "varargout";
}

} // namespace

BytecodeChunk Compiler::compileFunction(const ASTNode *funcDef,
                                        keel::Arc<const keel::String> sourceCode)
{
    struct FunctionCompileGuard
    {
        Compiler &c;
        BytecodeChunk savedChunk;
        keel::HashMap<keel::String, keel::u16> savedVarRegs;
        keel::Array<LoopContext> savedLoopStack;
        keel::HashMap<keel::i32, keel::u16> savedConstCache;
        keel::HashMap<keel::i32, keel::u16> savedPinnedConsts;
        RegBitset savedScalarRegs;
        RegBitset savedDefinedRegs;
        bool savedMayClear;
        bool savedMayInjectVars;
        keel::HashMap<keel::String, keel::u16> savedUpvalueRegs;
        keel::HashMap<keel::String, keel::u16> savedAllScopeUpvalBoxRegs;
        keel::HashMap<keel::String, NestedFuncInfo> savedNestedFuncInfo;
        const EnclosingLexicalContext *savedPendingLexicalCtx;
        int savedNextReg;
        int savedPeakReg;
        int savedMaxVarReg;
        bool savedTopLevel;
        bool savedInFunctionBody;
        bool savedInFusionFallback;
        keel::u16 savedNargoutContext;
        int savedActiveTryDepth;
        keel::u16 savedIdxArr;
        keel::u16 savedIdxDim;
        keel::u16 savedIdxNdims;
        SourceLoc savedLoc;

        explicit FunctionCompileGuard(Compiler &comp)
            : c(comp)
            , savedChunk(keel::move(comp.chunk_))
            , savedVarRegs(keel::move(comp.varRegisters_))
            , savedLoopStack(keel::move(comp.loopStack_))
            , savedConstCache(keel::move(comp.constRegCache_))
            , savedPinnedConsts(keel::move(comp.pinnedConstRegs_))
            , savedScalarRegs(keel::move(comp.scalarRegs_))
            , savedDefinedRegs(keel::move(comp.definedRegs_))
            , savedMayClear(comp.mayClear_)
            , savedMayInjectVars(comp.mayInjectVars_)
            , savedUpvalueRegs(keel::move(comp.upvalueRegs_))
            , savedAllScopeUpvalBoxRegs(keel::move(comp.allScopeUpvalueBoxRegs_))
            , savedNestedFuncInfo(keel::move(comp.nestedFuncInfo_))
            , savedPendingLexicalCtx(comp.pendingLexicalCtx_)
            , savedNextReg(comp.nextReg_)
            , savedPeakReg(comp.peakReg_)
            , savedMaxVarReg(comp.maxVarReg_)
            , savedTopLevel(comp.isTopLevel_)
            , savedInFunctionBody(comp.inFunctionBody_)
            , savedInFusionFallback(comp.inFusionFallback_)
            , savedNargoutContext(comp.nargoutContext_)
            , savedActiveTryDepth(comp.activeTryDepth_)
            , savedIdxArr(comp.indexContextArr_)
            , savedIdxDim(comp.indexContextDim_)
            , savedIdxNdims(comp.indexContextNdims_)
            , savedLoc(comp.currentLoc_)
        {}

        ~FunctionCompileGuard()
        {
            c.chunk_ = keel::move(savedChunk);
            c.varRegisters_ = keel::move(savedVarRegs);
            c.loopStack_ = keel::move(savedLoopStack);
            c.constRegCache_ = keel::move(savedConstCache);
            c.pinnedConstRegs_ = keel::move(savedPinnedConsts);
            c.scalarRegs_ = keel::move(savedScalarRegs);
            c.definedRegs_ = keel::move(savedDefinedRegs);
            c.mayClear_ = savedMayClear;
            c.mayInjectVars_ = savedMayInjectVars;
            c.upvalueRegs_ = keel::move(savedUpvalueRegs);
            c.allScopeUpvalueBoxRegs_ = keel::move(savedAllScopeUpvalBoxRegs);
            c.nestedFuncInfo_ = keel::move(savedNestedFuncInfo);
            c.pendingLexicalCtx_ = savedPendingLexicalCtx;
            c.nextReg_ = savedNextReg;
            c.peakReg_ = savedPeakReg;
            c.maxVarReg_ = savedMaxVarReg;
            c.isTopLevel_ = savedTopLevel;
            c.inFunctionBody_ = savedInFunctionBody;
            c.inFusionFallback_ = savedInFusionFallback;
            c.nargoutContext_ = savedNargoutContext;
            c.activeTryDepth_ = savedActiveTryDepth;
            c.indexContextArr_ = savedIdxArr;
            c.indexContextDim_ = savedIdxDim;
            c.indexContextNdims_ = savedIdxNdims;
            c.currentLoc_ = savedLoc;
        }
    } stateGuard(*this);

    const EnclosingLexicalContext *lexicalCtx = stateGuard.savedPendingLexicalCtx;
    pendingLexicalCtx_ = nullptr;

    isTopLevel_ = false;
    inFunctionBody_ = true;
    inFusionFallback_ = false;
    nargoutContext_ = 1;
    activeTryDepth_ = 0;
    indexContextArr_ = kInvalidReg;
    indexContextDim_ = 0;
    indexContextNdims_ = 1;

    chunk_ = BytecodeChunk{};
    chunk_.name = funcDef->strValue.clone();
    chunk_.sourceCode = sourceCode ? keel::move(sourceCode) : stateGuard.savedChunk.sourceCode;
    chunk_.paramNames = funcDef->paramNames.clone();
    chunk_.returnNames = funcDef->returnNames.clone();
    if (funcDef->paramNames.size() >= static_cast<keel::usize>(kInvalidReg)
        || funcDef->returnNames.size() >= static_cast<keel::usize>(kInvalidReg))
        throw std::runtime_error(
            keel::format("Compiler: function '{}' declares more than 65534 parameters or returns",
                         funcDef->strValue)
                .c_str());
    chunk_.numParams = static_cast<keel::u16>(funcDef->paramNames.size());
    chunk_.numReturns = static_cast<keel::u16>(funcDef->returnNames.size());
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

    const ASTNode *bodyRoot = funcDef->children.empty() ? nullptr : funcDef->children[0].get();

    // Pre-scan global and persistent declarations in this function's own body
    // before setting up lexical upvalues and preallocating locals.
    if (bodyRoot)
        collectScopeGlobalPersistentDecls(bodyRoot, chunk_.globalNames, chunk_.persistentNames);

    // Allocate registers for parameters (they come pre-loaded by VM)
    for (auto &param : funcDef->paramNames) {
        varRegWrite(param);
    }

    // Allocate registers for return variables (default to empty; no initialization needed).
    for (auto &ret : funcDef->returnNames) {
        varRegWrite(ret); // will be populated by the body
    }

    for (auto &gn : chunk_.globalNames)
        varRegWrite(gn);
    for (auto &pn : chunk_.persistentNames)
        varRegWrite(pn);

    // Always allocate nargin/nargout so they're visible in debugger (pre-
    // loaded by the VM on entry).
    keel::u16 narginReg = varRegLookup("nargin");
    keel::u16 nargoutReg = varRegLookup("nargout");

    // Collect direct nested function definitions and set up lexical upvalues.
    keel::Array<const ASTNode *> directNestedDefs;
    if (bodyRoot)
        collectDirectNestedDefs(bodyRoot, directNestedDefs);

    keel::HashSet<keel::String> ownDeclaredShadow;
    for (const auto &p : funcDef->paramNames) ownDeclaredShadow.insert(p.clone());
    for (const auto &r : funcDef->returnNames) ownDeclaredShadow.insert(r.clone());
    for (const auto &g : chunk_.globalNames) ownDeclaredShadow.insert(g.clone());
    for (const auto &p : chunk_.persistentNames) ownDeclaredShadow.insert(p.clone());

    keel::Array<keel::String> activeUpvalues;
    if (lexicalCtx) {
        for (const auto &u : lexicalCtx->inheritedUpvalueNames) {
            keel::u16 boxReg = allocPinnedReg();
            chunk_.upvalueParamRegs.push_back(boxReg);
            allScopeUpvalueBoxRegs_.put(u.clone(), boxReg);
            activeUpvalues.push_back(u.clone());
            if (!ownDeclaredShadow.contains(u) && !isPseudoScopeVar(u)) {
                varRegLookup(u, /*preloadReserved=*/false);
                upvalueRegs_.put(u.clone(), boxReg);
                chunk_.upvalueBoxRegs.put(u.clone(), boxReg);
            }
        }
        for (auto [sibName, sibInfo] : lexicalCtx->siblingNestedFuncs)
            nestedFuncInfo_.put(sibName.clone(), sibInfo.clone());
    }

    keel::Array<keel::String> assignedVec;
    if (bodyRoot)
        collectAssignedNames(bodyRoot, assignedVec);
    keel::HashSet<keel::String> assignedSet;
    for (const auto &a : assignedVec)
        assignedSet.insert(a.clone());
    keel::HashSet<keel::String> identifiers;
    if (bodyRoot)
        collectAllIdentifiers(bodyRoot, identifiers);

    if (!directNestedDefs.empty()) {
        keel::HashSet<keel::String> directNestedNames;
        for (const ASTNode *nd : directNestedDefs)
            directNestedNames.insert(nd->strValue.clone());

        auto collectDescendantRefs = [&](auto &self, const ASTNode *nfn,
                                         keel::HashSet<keel::String> &allRefs,
                                         keel::HashSet<keel::String> &asgnRefs) -> void {
            if (!nfn || nfn->children.empty())
                return;
            const ASTNode *nb = nfn->children[0].get();
            keel::HashSet<keel::String> shadow;
            for (const auto &p : nfn->paramNames) shadow.insert(p.clone());
            for (const auto &r : nfn->returnNames) shadow.insert(r.clone());
            keel::Array<keel::String> ng, np;
            collectScopeGlobalPersistentDecls(nb, ng, np);
            for (const auto &g : ng) shadow.insert(g.clone());
            for (const auto &p : np) shadow.insert(p.clone());

            keel::HashSet<keel::String> nIds;
            collectAllIdentifiers(nb, nIds);
            keel::Array<keel::String> nAsgnVec;
            collectAssignedNames(nb, nAsgnVec);

            for (const auto &id : nIds) {
                if (!shadow.contains(id) && !isPseudoScopeVar(id))
                    allRefs.insert(id.clone());
            }
            for (const auto &asgn : nAsgnVec) {
                if (!shadow.contains(asgn) && !isPseudoScopeVar(asgn)) {
                    allRefs.insert(asgn.clone());
                    asgnRefs.insert(asgn.clone());
                }
            }

            keel::Array<const ASTNode *> subDefs;
            collectDirectNestedDefs(nb, subDefs);
            for (const ASTNode *sd : subDefs)
                shadow.insert(sd->strValue.clone());
            for (const ASTNode *sd : subDefs) {
                keel::HashSet<keel::String> subAll, subAsgn;
                self(self, sd, subAll, subAsgn);
                for (const auto &id : subAll)
                    if (!shadow.contains(id) && !isPseudoScopeVar(id))
                        allRefs.insert(id.clone());
                for (const auto &asgn : subAsgn)
                    if (!shadow.contains(asgn) && !isPseudoScopeVar(asgn))
                        asgnRefs.insert(asgn.clone());
            }
        };

        keel::HashSet<keel::String> descAllRefs, descAsgnRefs;
        for (const ASTNode *nd : directNestedDefs)
            collectDescendantRefs(collectDescendantRefs, nd, descAllRefs, descAsgnRefs);

        keel::HashSet<keel::String> ownDeclaredOrAssigned = ownDeclaredShadow.clone();
        for (const auto &a : assignedVec)
            ownDeclaredOrAssigned.insert(a.clone());

        // Deterministic order for newly boxed variables of this scope
        keel::Array<keel::String> candidateVars;
        auto addCandidate = [&](keel::StringRef nm) {
            if (nm.empty() || isPseudoScopeVar(nm) || directNestedNames.contains(nm)
                || allScopeUpvalueBoxRegs_.contains(nm))
                return;
            for (const auto &cv : candidateVars)
                if (cv == nm)
                    return;
            candidateVars.push_back(keel::String::from(nm));
        };
        for (const auto &p : funcDef->paramNames) addCandidate(p);
        for (const auto &r : funcDef->returnNames) addCandidate(r);
        for (const auto &g : chunk_.globalNames) addCandidate(g);
        for (const auto &p : chunk_.persistentNames) addCandidate(p);
        for (const auto &a : assignedVec) addCandidate(a);
        for (const auto &id : identifiers) {
            if (descAsgnRefs.contains(id))
                addCandidate(id);
        }

        keel::HashSet<keel::String> preloadedOnEntry;
        for (const auto &p : funcDef->paramNames) preloadedOnEntry.insert(p.clone());
        for (const auto &r : funcDef->returnNames) preloadedOnEntry.insert(r.clone());
        for (const auto &g : chunk_.globalNames) preloadedOnEntry.insert(g.clone());
        for (const auto &p : chunk_.persistentNames) preloadedOnEntry.insert(p.clone());

        for (const auto &nm : candidateVars) {
            const bool shared = (ownDeclaredOrAssigned.contains(nm) && descAllRefs.contains(nm))
                                || (identifiers.contains(nm) && descAsgnRefs.contains(nm));
            if (!shared)
                continue;
            keel::u16 mirrorReg = varRegLookup(nm, /*preloadReserved=*/false);
            keel::u16 boxReg = allocPinnedReg();
            keel::u16 initSrc = preloadedOnEntry.contains(nm) ? mirrorReg : kInvalidReg;
            emitAB(OpCode::UPVAL_NEW, boxReg, initSrc);
            upvalueRegs_.put(nm.clone(), boxReg);
            allScopeUpvalueBoxRegs_.put(nm.clone(), boxReg);
            chunk_.upvalueBoxRegs.put(nm.clone(), boxReg);
            activeUpvalues.push_back(nm.clone());
            chunk_.assignedVars.insert(nm.clone());
        }

        // Register all direct nested functions before compiling any of them so
        // mutual recursion between siblings and self-recursion resolve cleanly.
        for (const ASTNode *nd : directNestedDefs) {
            keel::String compiledName = keel::format("{}>{}", chunk_.name, nd->strValue);
            keel::String dispSrc = compiledName.clone();
            for (keel::usize ci = 0; ci < dispSrc.size(); ++ci)
                if (dispSrc[ci] == '>')
                    dispSrc[ci] = '/';
            nestedFuncInfo_.put(nd->strValue.clone(), NestedFuncInfo{
                keel::move(compiledName), keel::move(dispSrc), activeUpvalues.clone()});
        }

        EnclosingLexicalContext childLexCtx{activeUpvalues.clone(), nestedFuncInfo_.clone()};
        for (const ASTNode *nd : directNestedDefs) {
            const NestedFuncInfo &info = *nestedFuncInfo_.get(nd->strValue);
            ASTNode nestedNode(NodeType::FUNCTION_DEF);
            nestedNode.strValue = info.compiledName.clone();
            nestedNode.paramNames = nd->paramNames.clone();
            nestedNode.returnNames = nd->returnNames.clone();
            nestedNode.line = nd->line;
            nestedNode.col = nd->col;
            nestedNode.endLine = nd->endLine;
            if (!nd->children.empty())
                nestedNode.children.push_back(cloneNode(nd->children[0].get()));

            pendingLexicalCtx_ = &childLexCtx;
            BytecodeChunk nestedChunk = compileFunction(&nestedNode, chunk_.sourceCode);
            pendingLexicalCtx_ = nullptr;
            nestedChunk.parentFuncName = chunk_.parentFuncName.empty()
                                             ? chunk_.name.clone()
                                             : chunk_.parentFuncName.clone();

            UserFunction nuf;
            nuf.name = info.compiledName.clone();
            nuf.params = nd->paramNames.clone();
            nuf.returns = nd->returnNames.clone();
            if (!nd->children.empty())
                nuf.body = keel::Arc<const ASTNode>(cloneNode(nd->children[0].get()));
            nuf.closureEnv = nullptr;
            nuf.numUpvalueCaptures = info.upvalueNames.size();

            if (scriptDepth_ > 0) {
                scriptLocalCompiledFuncs_.put(info.compiledName.clone(), keel::move(nestedChunk));
                engine_.adoptUserFunction(info.compiledName, keel::move(nuf), /*scriptScope=*/true);
            } else {
                compiledFuncs_.put(info.compiledName.clone(), keel::move(nestedChunk));
                engine_.adoptUserFunction(info.compiledName, keel::move(nuf), /*scriptScope=*/false);
            }
        }
    }

    for (const auto &name : identifiers) {
        if (varRegisters_.contains(name) || assignedSet.contains(name) || upvalueRegs_.contains(name))
            continue;
        if (engine_.isReservedName(name)) {
            varRegLookup(name, /*preloadReserved=*/true);
        }
    }

    if (bodyRoot) {
        preallocateAssignedVars(bodyRoot);
        preallocatePinnedConstants(bodyRoot);
    }
    if (!mayClear_) {
        definedRegs_.set(narginReg);
        definedRegs_.set(nargoutReg);
    }

    // Compile body
    if (bodyRoot)
        compileNode(bodyRoot);

    // Set source location to the 'end' keyword so breakpoints on it fire
    if (funcDef->endLine > 0)
        currentLoc_ = {static_cast<keel::u16>(funcDef->endLine), 1};

    // Emit return: collect return values
    const auto &rnames = funcDef->returnNames;
    const bool hasVarargout = !rnames.empty() && rnames.back() == "varargout";
    if (hasVarargout) {
        emitVarargoutReturn();
    } else if (rnames.size() == 1) {
        keel::u16 retReg = varRegLookup(funcDef->returnNames[0]);
        emitUpvalLoadRawIfShared(funcDef->returnNames[0], retReg);
        emitA(OpCode::RET, retReg);
    } else if (funcDef->returnNames.empty()) {
        emitNone(OpCode::RET_EMPTY);
    } else {
        keel::usize nret = funcDef->returnNames.size();
        keel::Array<keel::u16> retRegs;
        for (auto &name : funcDef->returnNames) {
            keel::u16 r = varRegLookup(name);
            emitUpvalLoadRawIfShared(name, r);
            retRegs.push_back(r);
        }
        bool consecutive = true;
        for (keel::usize i = 1; i < nret; ++i)
            if (retRegs[i] != retRegs[0] + i) { consecutive = false; break; }
        if (consecutive) {
            emitAB(OpCode::RET_MULTI, retRegs[0], static_cast<keel::u16>(nret));
        } else {
            keel::u16 base = nextReg_;
            for (keel::usize i = 0; i < nret; ++i) {
                keel::u16 slot = tempReg();
                if (retRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, retRegs[i]);
            }
            emitAB(OpCode::RET_MULTI, base, static_cast<keel::u16>(nret));
        }
    }

    chunk_.numRegisters = static_cast<keel::u32>(peakReg_);

    for (auto [name, reg] : varRegisters_)
        chunk_.varMap.push_back(VarRegEntry{name.clone(), reg});

    chunk_.narginReg = narginReg;
    chunk_.nargoutReg = nargoutReg;
    if (const keel::u16 *vaReg = varRegisters_.get("varargin"))
        chunk_.vararginReg = *vaReg;
    if (!chunk_.returnNames.empty()) {
        if (const keel::u16 *frReg = varRegisters_.get(chunk_.returnNames[0]))
            chunk_.firstReturnReg = *frReg;
    }
    chunk_.mayUseInputname = mayInjectVars_;
    if (!chunk_.mayUseInputname) {
        for (const auto &s : chunk_.strings) {
            if (s == "inputname" || s == "eval" || s == "evalin" || s == "feval") {
                chunk_.mayUseInputname = true;
                break;
            }
        }
    }

    return keel::move(chunk_);
}

void Compiler::emitVarargoutReturn()
{
    const auto &rns = chunk_.returnNames;
    const keel::usize numFixed = rns.size() - 1;
    keel::u16 vaReg = varRegLookup("varargout");
    emitUpvalLoadRawIfShared("varargout", vaReg);
    keel::u16 fixedBase = 0;
    if (numFixed > 0) {
        fixedBase = static_cast<keel::u16>(nextReg_);
        for (keel::usize i = 0; i < numFixed; ++i) {
            keel::u16 slot = tempReg();
            keel::u16 r = varRegLookup(rns[i]);
            emitUpvalLoadRawIfShared(rns[i], r);
            if (r != slot)
                emitAB(OpCode::MOVE, slot, r);
        }
    }
    emit(Instruction::make_abcde(OpCode::RET_VARARGOUT, fixedBase,
                                 static_cast<keel::u16>(numFixed), vaReg, 0, 0));
}

keel::u16 Compiler::compileReturn(const ASTNode * /*node*/)
{
    if (!chunk_.returnNames.empty() && chunk_.returnNames.back() == "varargout") {
        emitVarargoutReturn();
        return 0;
    }
    if (chunk_.returnNames.size() <= 1) {
        if (!chunk_.returnNames.empty()) {
            keel::u16 retReg = varRegLookup(chunk_.returnNames[0]);
            emitUpvalLoadRawIfShared(chunk_.returnNames[0], retReg);
            emitA(OpCode::RET, retReg);
        } else {
            emitNone(OpCode::RET_EMPTY);
        }
    } else {
        keel::usize nret = chunk_.returnNames.size();
        keel::Array<keel::u16> retRegs;
        for (auto &name : chunk_.returnNames) {
            keel::u16 r = varRegLookup(name);
            emitUpvalLoadRawIfShared(name, r);
            retRegs.push_back(r);
        }
        bool consecutive = true;
        for (keel::usize i = 1; i < nret; ++i)
            if (retRegs[i] != retRegs[0] + i) { consecutive = false; break; }
        if (consecutive) {
            emitAB(OpCode::RET_MULTI, retRegs[0], static_cast<keel::u16>(nret));
        } else {
            keel::u16 base = nextReg_;
            for (keel::usize i = 0; i < nret; ++i) {
                keel::u16 slot = tempReg();
                if (retRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, retRegs[i]);
            }
            emitAB(OpCode::RET_MULTI, base, static_cast<keel::u16>(nret));
        }
    }
    return 0;
}

keel::u16 Compiler::compileDeleteAssign(const ASTNode *node)
{
    auto *lhs = node->children[0].get();
    if (lhs->type != NodeType::CALL || lhs->children.empty())
        throw std::runtime_error("Compiler: invalid delete assignment syntax");

    auto *target = lhs->children[0].get();
    if (target->type != NodeType::IDENTIFIER)
        throw std::runtime_error("Compiler: invalid delete target");

    keel::u16 arr = varRegWrite(target->strValue);
    emitUpvalLoadRawIfShared(target->strValue, arr);
    keel::usize nargs = lhs->children.size() - 1;

    {
        IndexContextGuard guard(*this, arr, static_cast<keel::u16>(nargs));

        if (nargs == 1) {
            keel::u16 idx = compileNode(lhs->children[1].get());
            emitAB(OpCode::INDEX_DELETE, arr, idx);
        } else if (nargs == 2) {
            keel::u16 row = compileNode(lhs->children[1].get());
            guard.setDim(1);
            keel::u16 col = compileNode(lhs->children[2].get());
            emitABC(OpCode::INDEX_DELETE_2D, arr, row, col);
        } else {
            keel::Array<keel::u16> idxRegs;
            for (keel::usize i = 0; i < nargs; ++i) {
                idxRegs.push_back(compileNode(lhs->children[1 + i].get()));
                if (i + 1 < nargs)
                    guard.setDim(static_cast<keel::u16>(i + 1));
            }
            keel::u16 base = nextReg_;
            for (keel::usize i = 0; i < nargs; ++i) {
                keel::u16 slot = tempReg();
                if (idxRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, idxRegs[i]);
            }
            emitABC(OpCode::INDEX_DELETE_ND, arr, base, static_cast<keel::u16>(nargs));
        }
    }

    emitUpvalStoreIfShared(target->strValue, arr);
    return arr;
}

// ============================================================
// Builtin ID resolution (compile-time)
// ============================================================

// Must match the switch in VM::execute CALL_BUILTIN handler
keel::i8 Compiler::resolveBuiltinId(keel::StringRef name, keel::usize nargs)
{
    if (nargs == 1) {
        if (name == "abs")
            return 0;
        if (name == "floor")
            return 1;
        if (name == "ceil")
            return 2;
        if (name == "round")
            return 3;
        if (name == "fix")
            return 4;
        if (name == "sqrt")
            return 5;
        if (name == "exp")
            return 6;
        if (name == "log")
            return 7;
        if (name == "log2")
            return 8;
        if (name == "log10")
            return 9;
        if (name == "sin")
            return 10;
        if (name == "cos")
            return 11;
        if (name == "tan")
            return 12;
        if (name == "sign")
            return 13;
        if (name == "isnan")
            return 14;
        if (name == "isinf")
            return 15;
    }
    if (nargs == 2) {
        if (name == "mod")
            return 20;
        if (name == "rem")
            return 21;
        if (name == "max")
            return 22;
        if (name == "min")
            return 23;
        if (name == "pow")
            return 24;
        if (name == "atan2")
            return 25;
    }
    return -1; // not a known inline builtin
}

keel::String Compiler::disassemble(const BytecodeChunk &chunk)
{
    keel::String os = keel::format("=== {} (regs={} params={} returns={}) ===\n",
                                   chunk.name,
                                   (int) chunk.numRegisters,
                                   (int) chunk.numParams,
                                   (int) chunk.numReturns);

    auto opName = [](OpCode op) -> const char * {
        switch (op) {
        case OpCode::LOAD_CONST:
            return "LOAD_CONST";
        case OpCode::LOAD_EMPTY:
            return "LOAD_EMPTY";
        case OpCode::LOAD_STRING:
            return "LOAD_STRING";
        case OpCode::MOVE:
            return "MOVE";
        case OpCode::COLLAPSE:
            return "COLLAPSE";
        case OpCode::ADD:
            return "ADD";
        case OpCode::SUB:
            return "SUB";
        case OpCode::MUL:
            return "MUL";
        case OpCode::RDIV:
            return "RDIV";
        case OpCode::LDIV:
            return "LDIV";
        case OpCode::POW:
            return "POW";
        case OpCode::NEG:
            return "NEG";
        case OpCode::UPLUS:
            return "UPLUS";
        case OpCode::NOT:
            return "NOT";
        case OpCode::EMUL:
            return "EMUL";
        case OpCode::ERDIV:
            return "ERDIV";
        case OpCode::ELDIV:
            return "ELDIV";
        case OpCode::EPOW:
            return "EPOW";
        case OpCode::EQ:
            return "EQ";
        case OpCode::NE:
            return "NE";
        case OpCode::LT:
            return "LT";
        case OpCode::GT:
            return "GT";
        case OpCode::LE:
            return "LE";
        case OpCode::GE:
            return "GE";
        case OpCode::AND:
            return "AND";
        case OpCode::OR:
            return "OR";
        case OpCode::JMP:
            return "JMP";
        case OpCode::JMP_TRUE:
            return "JMP_TRUE";
        case OpCode::JMP_FALSE:
            return "JMP_FALSE";
        case OpCode::FOR_INIT:
            return "FOR_INIT";
        case OpCode::FOR_NEXT:
            return "FOR_NEXT";
        case OpCode::FOR_INIT_RANGE:
            return "FOR_INIT_RANGE";
        case OpCode::COLON:
            return "COLON";
        case OpCode::COLON3:
            return "COLON3";
        case OpCode::COLON_ALL:
            return "COLON_ALL";
        case OpCode::HORZCAT:
            return "HORZCAT";
        case OpCode::HORZCAT_APPEND:
            return "HORZCAT_APPEND";
        case OpCode::HORZCAT_APPEND_FLATTEN:
            return "HORZCAT_APPEND_FLATTEN";
        case OpCode::VERTCAT:
            return "VERTCAT";
        case OpCode::INDEX_GET:
            return "INDEX_GET";
        case OpCode::INDEX_GET_2D:
            return "INDEX_GET_2D";
        case OpCode::INDEX_SET:
            return "INDEX_SET";
        case OpCode::INDEX_SET_2D:
            return "INDEX_SET_2D";
        case OpCode::INDEX_GET_ND:
            return "INDEX_GET_ND";
        case OpCode::INDEX_SET_ND:
            return "INDEX_SET_ND";
        case OpCode::INDEX_DELETE:
            return "INDEX_DELETE";
        case OpCode::INDEX_DELETE_2D:
            return "INDEX_DELETE_2D";
        case OpCode::INDEX_DELETE_ND:
            return "INDEX_DELETE_ND";
        case OpCode::LOAD_END:
            return "LOAD_END";
        case OpCode::CALL:
            return "CALL";
        case OpCode::CALL_FLATTEN:
            return "CALL_FLATTEN";
        case OpCode::CALL_FLATTEN_MULTI:
            return "CALL_FLATTEN_MULTI";
        case OpCode::CALL_BUILTIN:
            return "CALL_BUILTIN";
        case OpCode::CTRANSPOSE:
            return "CTRANSPOSE";
        case OpCode::TRANSPOSE:
            return "TRANSPOSE";
        case OpCode::DISPLAY:
            return "DISPLAY";
        case OpCode::RET:
            return "RET";
        case OpCode::RET_EMPTY:
            return "RET_EMPTY";
        case OpCode::HALT:
            return "HALT";
        case OpCode::NOP:
            return "NOP";
        case OpCode::ASSERT_DEF:
            return "ASSERT_DEF";
        case OpCode::ADD_SS:
            return "ADD_SS";
        case OpCode::SUB_SS:
            return "SUB_SS";
        case OpCode::MUL_SS:
            return "MUL_SS";
        case OpCode::RDIV_SS:
            return "RDIV_SS";
        case OpCode::POW_SS:
            return "POW_SS";
        case OpCode::NEG_S:
            return "NEG_S";
        case OpCode::CLEAR_VAR:
            return "CLEAR_VAR";
        case OpCode::CLEAR_DYN:
            return "CLEAR_DYN";
        case OpCode::EXIST_VAR:
            return "EXIST_VAR";
        case OpCode::WHO:
            return "WHO";
        case OpCode::WHOS:
            return "WHOS";
        case OpCode::FIELD_GET:
            return "FIELD_GET";
        case OpCode::FIELD_GET_OR_CREATE:
            return "FIELD_GET_OR_CREATE";
        case OpCode::FIELD_SET:
            return "FIELD_SET";
        case OpCode::FIELD_GET_DYN:
            return "FIELD_GET_DYN";
        case OpCode::FIELD_GET_OR_CREATE_DYN:
            return "FIELD_GET_OR_CREATE_DYN";
        case OpCode::FIELD_SET_DYN:
            return "FIELD_SET_DYN";
        case OpCode::STRUCT_ELEM_FIELD_SET:
            return "STRUCT_ELEM_FIELD_SET";
        case OpCode::STRUCT_ELEM_GET_OR_CREATE:
            return "STRUCT_ELEM_GET_OR_CREATE";
        case OpCode::STRUCT_ELEM_SET:
            return "STRUCT_ELEM_SET";
        case OpCode::LVAL_UNSHARE_FIELD:
            return "LVAL_UNSHARE_FIELD";
        case OpCode::LVAL_UNSHARE_CELL:
            return "LVAL_UNSHARE_CELL";
        case OpCode::HORZCAT_APPEND_CSL:
            return "HORZCAT_APPEND_CSL";
        case OpCode::CELL_LITERAL:
            return "CELL_LITERAL";
        case OpCode::CELL_GET:
            return "CELL_GET";
        case OpCode::CELL_GET_OR_CREATE:
            return "CELL_GET_OR_CREATE";
        case OpCode::CELL_GET_2D:
            return "CELL_GET_2D";
        case OpCode::CELL_SET:
            return "CELL_SET";
        case OpCode::CELL_SET_2D:
            return "CELL_SET_2D";
        case OpCode::CELL_GET_MULTI:
            return "CELL_GET_MULTI";
        case OpCode::CELL_GET_MULTI_2D:
            return "CELL_GET_MULTI_2D";
        case OpCode::CELL_GET_ND:
            return "CELL_GET_ND";
        case OpCode::CELL_SET_ND:
            return "CELL_SET_ND";
        case OpCode::CELL_APPEND_FLATTEN:
            return "CELL_APPEND_FLATTEN";
        case OpCode::TRY_BEGIN:
            return "TRY_BEGIN";
        case OpCode::TRY_END:
            return "TRY_END";
        case OpCode::THROW:
            return "THROW";
        case OpCode::CALL_INDIRECT:
            return "CALL_INDIRECT";
        case OpCode::CALL_METHOD:
            return "CALL_METHOD";
        case OpCode::CALL_METHOD_MULTI:
            return "CALL_METHOD_MULTI";
        case OpCode::CLOSURE_MAKE:
            return "CLOSURE_MAKE";
        case OpCode::UPVAL_NEW:
            return "UPVAL_NEW";
        case OpCode::UPVAL_LOAD:
            return "UPVAL_LOAD";
        case OpCode::UPVAL_STORE:
            return "UPVAL_STORE";
        case OpCode::CALL_MULTI:
            return "CALL_MULTI";
        case OpCode::AND_SC:
            return "AND_SC";
        case OpCode::OR_SC:
            return "OR_SC";
        case OpCode::LOAD_NARGIN:
            return "LOAD_NARGIN";
        case OpCode::LOAD_NARGOUT:
            return "LOAD_NARGOUT";
        case OpCode::MATRIX_BUILD:
            return "MATRIX_BUILD";
        case OpCode::BREAK:
            return "BREAK";
        case OpCode::CONTINUE:
            return "CONTINUE";
        case OpCode::GLOBAL_DECL:
            return "GLOBAL_DECL";
        case OpCode::PERSISTENT_DECL:
            return "PERSISTENT_DECL";
        case OpCode::TO_LOGICAL_SCALAR:
            return "TO_LOGICAL_SCALAR";
        default:
            return "???";
        }
    };

    for (keel::usize i = 0; i < chunk.code.size(); ++i) {
        const auto &ins = chunk.code[i];

        // Show source location if available
        if (i < chunk.sourceMap.size() && chunk.sourceMap[i].line > 0) {
            os.append(keel::format("  L{}:{}\t", chunk.sourceMap[i].line, chunk.sourceMap[i].col));
        } else {
            os.append("  \t");
        }

        os.append(keel::format("[{}] {} a={} b={} c={} d={} e={}",
                               i,
                               opName(ins.op),
                               (int) ins.a,
                               (int) ins.b,
                               (int) ins.c,
                               ins.d,
                               (int) ins.e));

        if (ins.op == OpCode::LOAD_CONST
            && ins.d >= 0
            && static_cast<keel::usize>(ins.d) < chunk.constants.size()) {
            auto &cv = chunk.constants[ins.d];
            if (cv.isDoubleScalar())
                os.append(keel::format("  ; val={}", cv.scalarVal()));
            else
                os.append(keel::format("  ; {}", cv.debugString()));
        }
        os.push_back('\n');
    }

    return os;
}

} // namespace numkit
