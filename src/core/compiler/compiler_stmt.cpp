// src/core/compiler_stmt.cpp
#include <keel/core.h>
#include <keel/algo.h>
#include <keel/format.h>
#include <keel/utf8.h>
#include "core/compiler/compiler.hpp"
#include <numkit/value/error.hpp>
#include <numkit/core/engine.hpp>
#include "core/engine/stack_guard.hpp"
#include "core/vm/vm.hpp"
#include "core/compiler/compiler_internal.hpp"


namespace numkit {

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
        currentLoc_ = {keel::as<keel::u16>(node->endLine), 1};
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
        currentLoc_ = {keel::as<keel::u16>(node->endLine), 1};
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
            if (keel::find(chunk_.globalNames.begin(), chunk_.globalNames.end(), name)
                == chunk_.globalNames.end()) {
                chunk_.globalNames.push_back(name.clone());
            }
            chunk_.ownGlobalDecls.push_back(name.clone());
        } else {
            if (keel::find(chunk_.persistentNames.begin(), chunk_.persistentNames.end(), name)
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
            keel::i32 &depth;
            explicit TryDepthGuard(keel::i32 &d) : depth(d) { ++depth; }
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
        currentLoc_ = {keel::as<keel::u16>(node->endLine), 1};
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
    const keel::i32 condMark = nextReg_;
    keel::u16 condReg = compileNode(node->children[0].get());

    // JMP_FALSE to end (placeholder)
    keel::usize exitPos = currentPos();
    emitAD(OpCode::JMP_FALSE, condReg, 0);

    const keel::i32 condFloor = condMark > maxVarReg_ ? condMark : maxVarReg_;
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
        currentLoc_ = {keel::as<keel::u16>(node->endLine), 1};

    // JMP back to start
    emitD(OpCode::JMP, keel::as<keel::i32>(loopStart) - keel::as<keel::i32>(currentPos()));

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
        throw numkit::Error("Compiler: break outside of loop");

    keel::i32 triesToPop = activeTryDepth_ - loopStack_.back().tryDepthAtEntry;
    for (keel::i32 i = 0; i < triesToPop; ++i)
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
        throw numkit::Error("Compiler: continue outside of loop");
    keel::i32 triesToPop = activeTryDepth_ - loopStack_.back().tryDepthAtEntry;
    for (keel::i32 i = 0; i < triesToPop; ++i)
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
        currentLoc_ = {keel::as<keel::u16>(node->endLine), 1};

    emitAD(OpCode::FOR_NEXT, vReg, 0);
    chunk_.code.back().d =
        keel::as<keel::i32>(bodyStart) - keel::as<keel::i32>(currentPos() - 1);

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

    throw numkit::Error("Compiler: invalid colon expression");
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
            const keel::i32 mark = nextReg_;
            for (auto &elem : row->children) {
                keel::u16 v = compileNode(elem.get());
                emitAB(OpCode::HORZCAT_APPEND, dst, v);
                const keel::i32 floor = mark > maxVarReg_ ? mark : maxVarReg_;
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
        emitABC(OpCode::HORZCAT, dst, hBase, keel::as<keel::u16>(elemRegs.size()));
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
        const keel::i32 mark = nextReg_;
        for (auto &row : node->children) {
            keel::u16 rr = compileRow(row.get());
            emitAB(OpCode::VERTCAT_APPEND, dst, rr);
            const keel::i32 floor = mark > maxVarReg_ ? mark : maxVarReg_;
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
    emitABC(OpCode::VERTCAT, dst, vBase, keel::as<keel::u16>(rowRegs.size()));
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
        IndexContextGuard guard(*this, arr, keel::as<keel::u16>(nargs));
        keel::Array<keel::u16> idxRegs;
        for (keel::usize i = 0; i < nargs; ++i) {
            idxRegs.push_back(compileNode(node->children[argOffset + i].get()));
            if (i + 1 < nargs)
                guard.setDim(keel::as<keel::u16>(i + 1));
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
                                     keel::as<keel::u16>(nargs)));
        return dst;
    }
}

keel::u16 Compiler::compileIndexAssign(const ASTNode *node)
{
    return compileIndexStore(node->children[0].get(), node->children[1].get(),
                             -1, node->suppressOutput);
}

keel::u16 Compiler::compileIndexStore(const ASTNode *lhs, const ASTNode *rhs,
                                    keel::i32 inValReg, bool suppress)
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
    keel::u16 val = (inValReg >= 0) ? keel::as<keel::u16>(inValReg) : compileNode(rhs);

    {
        IndexContextGuard guard(*this, arr, keel::as<keel::u16>(nargs));

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
                    guard.setDim(keel::as<keel::u16>(i + 1));
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
                                         keel::as<keel::u16>(nargs),
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
                                    keel::i32 inValReg, bool suppress)
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
        keel::u16 valReg = (inValReg >= 0) ? keel::as<keel::u16>(inValReg)
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
        throw numkit::Error("Compiler: invalid nested field assign target");

    // fieldChain is in reverse: ["b", "a"] for s.a.b
    // Root variable
    keel::u16 rootReg = varRegWrite(cur->strValue);
    emitUpvalLoadRawIfShared(cur->strValue, rootReg);
    keel::u16 val = (inValReg >= 0) ? keel::as<keel::u16>(inValReg) : compileNode(rhs);

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
        emitABC(OpCode::CELL_LITERAL, dst, base, keel::as<keel::u16>(elemRegs.size()));
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
    emitABC(OpCode::VERTCAT, dst, vBase, keel::as<keel::u16>(rowRegs.size()));
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
        IndexContextGuard guard(*this, cell, keel::as<keel::u16>(nidx));
        keel::Array<keel::u16> idxRegs;
        for (keel::usize i = 0; i < nidx; ++i) {
            idxRegs.push_back(compileNode(node->children[1 + i].get()));
            if (i + 1 < nidx)
                guard.setDim(keel::as<keel::u16>(i + 1));
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
                                     keel::as<keel::u16>(nidx)));
        return dst;
    }
}

keel::u16 Compiler::compileCellAssign(const ASTNode *node)
{
    return compileCellStore(node->children[0].get(), node->children[1].get(),
                            -1, node->suppressOutput);
}

keel::u16 Compiler::compileCellStore(const ASTNode *lhs, const ASTNode *rhs,
                                   keel::i32 inValReg, bool suppress)
{
    // lhs = CELL_INDEX node; rhs = RHS expr (used only when inValReg < 0)
    auto *target = lhs->children[0].get();
    if (target->type != NodeType::IDENTIFIER)
        throw numkit::Error("Compiler: invalid cell assignment target");

    keel::u16 cell = varRegWrite(target->strValue);
    emitUpvalLoadRawIfShared(target->strValue, cell);
    keel::u16 val = (inValReg >= 0) ? keel::as<keel::u16>(inValReg) : compileNode(rhs);

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
        IndexContextGuard guard(*this, cell, keel::as<keel::u16>(nidx));
        keel::Array<keel::u16> idxRegs;
        for (keel::usize i = 0; i < nidx; ++i) {
            idxRegs.push_back(compileNode(lhs->children[1 + i].get()));
            if (i + 1 < nidx)
                guard.setDim(keel::as<keel::u16>(i + 1));
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
                                     keel::as<keel::u16>(nidx),
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
        compileLValueStore(target, nullptr, keel::as<keel::i32>(valReg), suppress);
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
            throw numkit::Error("Compiler: dynamic field assign requires identifier target");
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
        throw numkit::Error("Compiler: unsupported multi-assign target");
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
        keel::i32 fieldChainLen = 0;
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


keel::u16 Compiler::compileLValueStore(const ASTNode *lhs, const ASTNode *rhsNode,
                                     keel::i32 inValReg, bool suppress)
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
        throw numkit::Error("Compiler: unsupported lvalue root");
    keel::reverse(acc.begin(), acc.end());
    const keel::usize n = acc.size();
    // A paren-index that is NOT the final accessor indexes into a
    // struct array to reach an element used as a container for the rest
    // of the chain (`d(i).a.b`).
    for (keel::usize k = 0; k + 1 < n; ++k)
        if (acc[k].kind == LvAcc::Index)
            acc[k].structElem = true;

    // Value to store (multi-assign output register, or compile the RHS).
    keel::u16 val = (inValReg >= 0) ? keel::as<keel::u16>(inValReg) : compileNode(rhsNode);

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
            IndexContextGuard guard(*this, regs[k], keel::as<keel::u16>(nargs));
            for (keel::usize a = 0; a < nargs; ++a) {
                if (a > 0)
                    guard.setDim(keel::as<keel::u16>(a));
                tmp.push_back(compileNode(node->children[a + 1].get()));
            }
        }
        idxN[k] = keel::as<keel::u16>(nargs);
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
} // namespace numkit
