// include/compiler.hpp
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/ast.hpp>
#include <numkit/core/bytecode.hpp>
#include <algorithm>
#include <cstdint>

namespace numkit {

class Engine;
struct UserFunction; // defined in types.hpp; used by-reference here

class Compiler
{
    friend class IndexContextGuard;

public:
    explicit Compiler(Engine &engine);

    BytecodeChunk compile(const ASTNode *ast,
                          keel::Arc<const keel::String> sourceCode = nullptr);

    // Compile a function definition into a BytecodeChunk
    BytecodeChunk compileFunction(const ASTNode *funcDef,
                                  keel::Arc<const keel::String> sourceCode = nullptr);

    // Register a FUNCTION_DEF node in the currently-active compiled
    // function table (+ engine.{script,user}Funcs_) without running
    // it. Used by Engine::beginScript to pre-compile a script's
    // local functions into the script-scope bucket, and by the
    // split-mode driver so forward references resolve.
    void registerFunction(const ASTNode *funcDef);

    // Same as above but binds the function under an explicit name
    // instead of funcDef->strValue. resolveMFile_ uses this for
    // package-qualified m-files (`+pkg/foo.m` is bound under
    // "pkg.foo", not the leaf "foo", so two packages with the same
    // leaf don't collide in compiledFuncs_).
    void registerFunctionAs(keel::StringRef qualifiedName,
                             const ASTNode *funcDef);

    // Same, with the function file's provenance (m-file resolver path).
    // Anonymous functions compiled inside the body capture THAT file —
    // at runtime their ANON_FUNC node executes under the function's
    // origin frame, and the compile-time capture must agree.
    void registerFunctionAs(keel::StringRef qualifiedName,
                             const ASTNode *funcDef, keel::StringRef fsName,
                             keel::StringRef scriptDir, keel::StringRef scriptFile);

    // Stamp m-file provenance onto an already-registered chunk. The
    // engine's m-file resolver compiles first and learns the concrete
    // file from its search loop only afterwards.
    void setChunkOrigin(keel::StringRef qualifiedName,
                        keel::StringRef fsName, keel::StringRef scriptDir,
                        keel::StringRef scriptFile);

    // Compile a classdef method / constructor body (reconstructed from its
    // UserFunction) into the GLOBAL compiled-function table under `uf.name`
    // ("Class>member"), returning the chunk. Idempotent. Unlike
    // registerFunctionAs this never lands in the script-local bucket —
    // classdef bodies persist with the class across evals. Lets the VM run
    // classdef method bodies as native frames.
    const BytecodeChunk *ensureClassMethodCompiled(const UserFunction &uf);

    // Workspace-scope compiled functions. Populated by `function` at
    // the REPL or by anonymous-function allocation. Cleared by
    // `clear all` / `clear functions` (see Engine::clearUserFunctions).
    const keel::HashMap<keel::String, BytecodeChunk> &compiledFuncs() const
    {
        return compiledFuncs_;
    }
    void clearCompiledFuncs() { compiledFuncs_.clear(); }
    // Drop a single compiled chunk by name (used by rehash to invalidate just
    // the reloaded m-file, not every compiled function), plus any nested chunks.
    void eraseCompiledFunc(keel::StringRef name);
    // Drop every compiled classdef-method chunk for `className` — keys of the
    // form "ClassName>method" — so redefining the class recompiles its bodies
    // from scratch instead of reusing stale cached chunks.
    void eraseCompiledFuncsForClass(keel::StringRef className);

    // Script-scope compiled functions. Populated by beginScriptScope
    // via compileFunctionDef routing. NEVER cleared by
    // clearCompiledFuncs — that's the whole point of keeping this
    // bucket separate. Its lifetime is bounded by beginScriptScope /
    // endScriptScope pairs.
    const keel::HashMap<keel::String, BytecodeChunk> &scriptLocalCompiledFuncs() const
    {
        return scriptLocalCompiledFuncs_;
    }

    // Unified lookup used by dispatch sites outside the compiler —
    // tries script-scope first, then workspace-scope. Returns
    // nullptr if unknown.
    const BytecodeChunk *findCompiled(keel::StringRef name) const;

    // Enter/leave a script-lexical scope. While inside, compiled
    // FUNCTION_DEFs land in scriptLocalCompiledFuncs_ rather than
    // the workspace map. Nestable — each pair push/pop an isolated
    // bucket, so a recursive eval's inner script can't leak into
    // the outer one.
    void beginScriptScope();
    void endScriptScope();
    bool inScriptScope() const { return scriptDepth_ > 0; }

    // Peer of Engine::promoteScriptLocalsToWorkspace: copy the
    // script-scope compiled chunks into the workspace bucket so
    // their VM dispatches keep resolving after the scope ends.
    void promoteScriptLocalsToWorkspace();

    // Debug: dump bytecode
    static keel::String disassemble(const BytecodeChunk &chunk);

private:
    struct RegBitset
    {
        keel::Array<keel::u64> words;

        bool test(keel::usize pos) const
        {
            keel::usize w = pos >> 6;
            return w < words.size() && ((words[w] >> (pos & 63)) & 1ULL) != 0;
        }
        void set(keel::usize pos)
        {
            keel::usize w = pos >> 6;
            if (w >= words.size())
                words.resize(w + 1, 0ULL);
            words[w] |= (1ULL << (pos & 63));
        }
        void reset()
        {
            std::fill(words.begin(), words.end(), 0ULL);
        }
        void reset(keel::usize pos)
        {
            keel::usize w = pos >> 6;
            if (w < words.size())
                words[w] &= ~(1ULL << (pos & 63));
        }
    };

    Engine &engine_;

    // Current chunk being compiled
    BytecodeChunk chunk_;

    // Register allocation: variable name → register index
    keel::HashMap<keel::String, keel::u16> varRegisters_;
    // nextReg_ is `int` so we can detect overflow (>= kInvalidReg = 65535)
    // before it wraps.
    keel::i32 nextReg_ = 0;
    // High-water mark for nextReg_ across the chunk. Used at end of
    // compilation to size the runtime register file.
    keel::i32 peakReg_ = 0;
    // Highest variable register reserved so far (exclusive). Slots
    // [0, maxVarReg_) are pinned to variables and never reclaimable;
    // [maxVarReg_, nextReg_) are transient temps that compileBlock
    // releases between statements.
    keel::i32 maxVarReg_ = 0;
    bool isTopLevel_ = false;
    bool inFunctionBody_ = false;
    // True while compiling a fusion idiom's fallback path (the normal compile),
    // so the nested compileCall doesn't re-attempt fusion (infinite recursion).
    bool inFusionFallback_ = false;
    // Maps pre-evaluated fusion operand AST nodes to their registers while
    // emitting the fallback path of FUSE_EWISE so operands never evaluate twice.
    keel::HashMap<const ASTNode *, keel::u16> fusedOperandRegs_;
    keel::u16 nargoutContext_ = 1; // expected number of outputs (0=statement, 1=expression)
    keel::i32 activeTryDepth_ = 0;      // active enclosing TRY_BEGIN depth in current chunk

    // Provenance context for anonymous functions compiled as part of a
    // NAMED function's body. Managed by AnonOriginSentry.
    bool anonOriginKnown_ = false;
    keel::String anonOriginFs_;
    keel::String anonOriginDir_;
    keel::String anonOriginFile_;

    class AnonOriginSentry
    {
    public:
        AnonOriginSentry(Compiler &c, bool known, keel::String fs,
                         keel::String dir, keel::String file);
        ~AnonOriginSentry();
        AnonOriginSentry(const AnonOriginSentry &) = delete;
        AnonOriginSentry &operator=(const AnonOriginSentry &) = delete;
    private:
        Compiler &c_;
        bool known_;
        keel::String fs_, dir_, file_;
    };
    friend class AnonOriginSentry;

    void registerFunctionAsImpl_(keel::StringRef qualifiedName,
                                  const ASTNode *funcDef);

    // Index context for END_VAL compilation (kInvalidReg when outside indexing)
    keel::u16 indexContextArr_ = kInvalidReg;
    keel::u16 indexContextDim_ = 0;
    keel::u16 indexContextNdims_ = 1;

    // Current source location (updated before compiling each node)
    SourceLoc currentLoc_{};

    // Compiled function table (persists across compile() calls) —
    // workspace-scope (cleared by `clear all`/`clear functions`).
    keel::HashMap<keel::String, BytecodeChunk> compiledFuncs_;

    // Script-lexical compiled functions.
    keel::HashMap<keel::String, BytecodeChunk> scriptLocalCompiledFuncs_;
    keel::Array<keel::HashMap<keel::String, BytecodeChunk>> savedScriptLocalCompiledFuncs_;
    keel::i32 scriptDepth_ = 0;

    keel::u16 varRegWrite(keel::StringRef name);
    keel::u16 varRegRead(keel::StringRef name);
    keel::u16 varRegLookup(keel::StringRef name, bool preloadReserved = true);
    keel::u16 allocPinnedReg();
    void markAssigned(keel::StringRef name)
    {
        if (!chunk_.assignedVars.contains(name))
            chunk_.assignedVars.insert(keel::String::from(name));
    }
    void preImportGlobals(const ASTNode *ast);
    void collectAllIdentifiers(const ASTNode *node, keel::HashSet<keel::String> &out);
    void collectAssignedNames(const ASTNode *node, keel::Array<keel::String> &out);
    void preallocateAssignedVars(const ASTNode *body);
    void preallocatePinnedConstants(const ASTNode *body);
    keel::u16 tempReg();

    // Emit instructions
    void emit(Instruction instr);
    void emitABC(OpCode op, keel::u16 a, keel::u16 b, keel::u16 c);
    void emitAB(OpCode op, keel::u16 a, keel::u16 b);
    void emitAD(OpCode op, keel::u16 a, keel::i32 d);
    void emitA(OpCode op, keel::u16 a);
    void emitD(OpCode op, keel::i32 d);
    void emitNone(OpCode op);

    keel::usize currentPos() const;
    void patchJump(keel::usize instrPos, keel::i32 offset);

    keel::i32 addConstant(keel::f64 value);
    keel::i32 addStringConstant(keel::StringRef s);
    keel::i32 appendConstant(Value v);
    keel::i32 appendString(keel::StringRef s);

    keel::u16 reserveRegs(keel::usize n);

    keel::u16 compileNode(const ASTNode *node);
    keel::u16 compileNodeExpand(const ASTNode *node);
    keel::u16 compileBlock(const ASTNode *node);
    keel::u16 compileNumber(const ASTNode *node);
    keel::u16 compileString(const ASTNode *node);
    keel::u16 compileBool(const ASTNode *node);
    keel::u16 compileIdentifier(const ASTNode *node);
    keel::u16 compileAssign(const ASTNode *node);
    keel::u16 compileMultiAssign(const ASTNode *node);
    keel::u16 compileBinaryOp(const ASTNode *node);
    keel::u16 emitPlainBinary(keel::StringRef op, keel::u16 left, keel::u16 right);
    keel::u16 emitFusedMulAdd(bool isPlus, const ASTNode *rhs, keel::u16 acc);
    keel::u16 compileUnaryOp(const ASTNode *node);
    keel::u16 compileExprStmt(const ASTNode *node);

    keel::u16 compileIf(const ASTNode *node);
    keel::u16 compileSwitch(const ASTNode *node);
    keel::u16 compileTryCatch(const ASTNode *node);
    keel::u16 compileGlobalPersistent(const ASTNode *node);
    keel::u16 compileFieldAccess(const ASTNode *node);
    keel::u16 compileFieldAssign(const ASTNode *node);
    keel::u16 compileCellLiteral(const ASTNode *node);
    keel::u16 compileCellIndex(const ASTNode *node);
    keel::u16 compileCellAssign(const ASTNode *node);

    keel::u16 compileFieldStore(const ASTNode *lhs, const ASTNode *rhsNode,
                               keel::i32 inValReg, bool suppress);
    keel::u16 compileCellStore(const ASTNode *lhs, const ASTNode *rhsNode,
                              keel::i32 inValReg, bool suppress);
    keel::u16 compileIndexStore(const ASTNode *lhs, const ASTNode *rhsNode,
                               keel::i32 inValReg, bool suppress);
    void compileStoreLValue(const ASTNode *target, keel::u16 valReg, bool suppress);

    bool needsGeneralLValuePath(const ASTNode *lhs) const;
    keel::u16 compileLValueStore(const ASTNode *lhs, const ASTNode *rhsNode,
                                keel::i32 inValReg, bool suppress);
    keel::u16 compileSuperCall(const ASTNode *callNode, const ASTNode *superRef,
                              keel::i32 explicitOutBase, keel::u16 nout);
    keel::u16 compileAnonFunc(const ASTNode *node);
    void collectFreeVars(const ASTNode *node,
                         keel::ArrayRef<keel::String> params,
                         keel::Array<keel::String> &freeVars);
    keel::u16 compileWhile(const ASTNode *node);
    keel::u16 compileBreak(const ASTNode *node);
    keel::u16 compileContinue(const ASTNode *node);

    keel::u16 compileFor(const ASTNode *node);
    keel::u16 compileColonExpr(const ASTNode *node);
    keel::u16 compileMatrixLiteral(const ASTNode *node);
    keel::u16 compileIndexExpr(const ASTNode *node);
    keel::u16 compileIndexAssign(const ASTNode *node);

    keel::u16 compileCall(const ASTNode *node);
    keel::i32 tryCompileFused(const ASTNode *node);
    keel::u16 compileFused(const ASTNode *node, keel::usize ruleIdx,
                          keel::ArrayRef<const ASTNode *> operands);
    keel::u16 compileCommandCall(const ASTNode *node);
    void recordCallArgNames(const ASTNode *callNode, keel::usize callInstrIdx);
    keel::u16 compileFunctionDef(const ASTNode *node);
    keel::u16 compileReturn(const ASTNode *node);
    void emitVarargoutReturn();
    keel::u16 compileDeleteAssign(const ASTNode *node);
    static FastBuiltinId resolveBuiltinId(keel::StringRef name, keel::usize nargs);

    // Constant register cache: maps constant pool index → register that holds it
    keel::HashMap<keel::i32, keel::u16> constRegCache_;
    // Pinned constant registers allocated at chunk entry (< maxVarReg_)
    keel::HashMap<keel::i32, keel::u16> pinnedConstRegs_;

    // Scalar register tracking (dynamic bitset across 16-bit register space)
    RegBitset scalarRegs_;

    // Defined register tracking (dynamic bitset across 16-bit register space)
    RegBitset definedRegs_;
    bool mayClear_ = false;
    bool mayInjectVars_ = false;

    struct NestedFuncInfo
    {
        keel::String compiledName;
        keel::String displaySource;
        keel::Array<keel::String> upvalueNames;

        NestedFuncInfo clone(keel::Allocator *alloc = nullptr) const
        {
            return NestedFuncInfo{
                compiledName.clone(alloc),
                displaySource.clone(alloc),
                upvalueNames.clone(alloc),
            };
        }
    };

    struct EnclosingLexicalContext
    {
        keel::Array<keel::String> inheritedUpvalueNames;
        keel::HashMap<keel::String, NestedFuncInfo> siblingNestedFuncs;
    };

    keel::HashMap<keel::String, keel::u16> upvalueRegs_;
    keel::HashMap<keel::String, keel::u16> allScopeUpvalueBoxRegs_;
    keel::HashMap<keel::String, NestedFuncInfo> nestedFuncInfo_;
    const EnclosingLexicalContext *pendingLexicalCtx_ = nullptr;

    void emitUpvalLoadRawIfShared(keel::StringRef name, keel::u16 reg);
    void emitUpvalStoreIfShared(keel::StringRef name, keel::u16 reg);
    keel::u16 emitNestedFuncHandle(const NestedFuncInfo &info);

    void peepholeOptimize();

    struct LoopContext
    {
        keel::Array<keel::usize> breakPatches;
        keel::Array<keel::usize> continuePatches;
        bool isForLoop = false;
        keel::i32 tryDepthAtEntry = 0;
    };
    keel::Array<LoopContext> loopStack_;
};

} // namespace numkit
