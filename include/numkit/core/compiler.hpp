// include/compiler.hpp
#pragma once

#include <keel/core.h>
#include <numkit/core/ast.hpp>
#include <numkit/core/bytecode.hpp>
#include <algorithm>
#include <cstdint>
#include <unordered_set>

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace numkit {

class Engine;
struct UserFunction; // defined in types.hpp; used by-reference here

class Compiler
{
    friend class IndexContextGuard;

public:
    explicit Compiler(Engine &engine);

    BytecodeChunk compile(const ASTNode *ast,
                          std::shared_ptr<const std::string> sourceCode = nullptr);

    // Compile a function definition into a BytecodeChunk
    BytecodeChunk compileFunction(const ASTNode *funcDef,
                                  std::shared_ptr<const std::string> sourceCode = nullptr);

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
    void registerFunctionAs(const std::string &qualifiedName,
                             const ASTNode *funcDef);

    // Same, with the function file's provenance (m-file resolver path).
    // Anonymous functions compiled inside the body capture THAT file —
    // at runtime their ANON_FUNC node executes under the function's
    // origin frame, and the compile-time capture must agree.
    void registerFunctionAs(const std::string &qualifiedName,
                             const ASTNode *funcDef, const std::string &fsName,
                             const std::string &scriptDir, const std::string &scriptFile);

    // Stamp m-file provenance onto an already-registered chunk. The
    // engine's m-file resolver compiles first and learns the concrete
    // file from its search loop only afterwards.
    void setChunkOrigin(const std::string &qualifiedName,
                        const std::string &fsName, const std::string &scriptDir,
                        const std::string &scriptFile);

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
    const std::unordered_map<std::string, BytecodeChunk> &compiledFuncs() const
    {
        return compiledFuncs_;
    }
    void clearCompiledFuncs() { compiledFuncs_.clear(); }
    // Drop a single compiled chunk by name (used by rehash to invalidate just
    // the reloaded m-file, not every compiled function), plus any nested chunks.
    void eraseCompiledFunc(const std::string &name);
    // Drop every compiled classdef-method chunk for `className` — keys of the
    // form "ClassName>method" — so redefining the class recompiles its bodies
    // from scratch instead of reusing stale cached chunks.
    void eraseCompiledFuncsForClass(const std::string &className);

    // Script-scope compiled functions. Populated by beginScriptScope
    // via compileFunctionDef routing. NEVER cleared by
    // clearCompiledFuncs — that's the whole point of keeping this
    // bucket separate. Its lifetime is bounded by beginScriptScope /
    // endScriptScope pairs.
    const std::unordered_map<std::string, BytecodeChunk> &scriptLocalCompiledFuncs() const
    {
        return scriptLocalCompiledFuncs_;
    }

    // Unified lookup used by dispatch sites outside the compiler —
    // tries script-scope first, then workspace-scope. Returns
    // nullptr if unknown.
    const BytecodeChunk *findCompiled(const std::string &name) const;

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
    static std::string disassemble(const BytecodeChunk &chunk);

private:
    struct RegBitset
    {
        std::vector<keel::u64> words;

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
    std::unordered_map<std::string, keel::u16> varRegisters_;
    // nextReg_ is `int` so we can detect overflow (>= kInvalidReg = 65535)
    // before it wraps.
    int nextReg_ = 0;
    // High-water mark for nextReg_ across the chunk. Used at end of
    // compilation to size the runtime register file.
    int peakReg_ = 0;
    // Highest variable register reserved so far (exclusive). Slots
    // [0, maxVarReg_) are pinned to variables and never reclaimable;
    // [maxVarReg_, nextReg_) are transient temps that compileBlock
    // releases between statements.
    int maxVarReg_ = 0;
    bool isTopLevel_ = false;
    bool inFunctionBody_ = false;
    // True while compiling a fusion idiom's fallback path (the normal compile),
    // so the nested compileCall doesn't re-attempt fusion (infinite recursion).
    bool inFusionFallback_ = false;
    // Maps pre-evaluated fusion operand AST nodes to their registers while
    // emitting the fallback path of FUSE_EWISE so operands never evaluate twice.
    std::unordered_map<const ASTNode *, keel::u16> fusedOperandRegs_;
    keel::u16 nargoutContext_ = 1; // expected number of outputs (0=statement, 1=expression)
    int activeTryDepth_ = 0;      // active enclosing TRY_BEGIN depth in current chunk

    // Provenance context for anonymous functions compiled as part of a
    // NAMED function's body. Managed by AnonOriginSentry.
    bool anonOriginKnown_ = false;
    std::string anonOriginFs_;
    std::string anonOriginDir_;
    std::string anonOriginFile_;

    class AnonOriginSentry
    {
    public:
        AnonOriginSentry(Compiler &c, bool known, std::string fs,
                         std::string dir, std::string file);
        ~AnonOriginSentry();
        AnonOriginSentry(const AnonOriginSentry &) = delete;
        AnonOriginSentry &operator=(const AnonOriginSentry &) = delete;
    private:
        Compiler &c_;
        bool known_;
        std::string fs_, dir_, file_;
    };
    friend class AnonOriginSentry;

    void registerFunctionAsImpl_(const std::string &qualifiedName,
                                  const ASTNode *funcDef);

    // Index context for END_VAL compilation (kInvalidReg when outside indexing)
    keel::u16 indexContextArr_ = kInvalidReg;
    keel::u16 indexContextDim_ = 0;
    keel::u16 indexContextNdims_ = 1;

    // Current source location (updated before compiling each node)
    SourceLoc currentLoc_{};

    // Compiled function table (persists across compile() calls) —
    // workspace-scope (cleared by `clear all`/`clear functions`).
    std::unordered_map<std::string, BytecodeChunk> compiledFuncs_;

    // Script-lexical compiled functions.
    std::unordered_map<std::string, BytecodeChunk> scriptLocalCompiledFuncs_;
    std::vector<std::unordered_map<std::string, BytecodeChunk>> savedScriptLocalCompiledFuncs_;
    int scriptDepth_ = 0;

    keel::u16 varRegWrite(const std::string &name);
    keel::u16 varRegRead(const std::string &name);
    keel::u16 varRegLookup(const std::string &name, bool preloadReserved = true);
    keel::u16 allocPinnedReg();
    void markAssigned(const std::string &name) { chunk_.assignedVars.insert(name); }
    void preImportGlobals(const ASTNode *ast);
    void collectAllIdentifiers(const ASTNode *node, std::unordered_set<std::string> &out);
    void collectAssignedNames(const ASTNode *node, std::vector<std::string> &out);
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
    keel::i32 addStringConstant(const std::string &s);
    keel::i32 appendConstant(Value v);
    keel::i32 appendString(const std::string &s);

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
    keel::u16 emitPlainBinary(const std::string &op, keel::u16 left, keel::u16 right);
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
                               int inValReg, bool suppress);
    keel::u16 compileCellStore(const ASTNode *lhs, const ASTNode *rhsNode,
                              int inValReg, bool suppress);
    keel::u16 compileIndexStore(const ASTNode *lhs, const ASTNode *rhsNode,
                               int inValReg, bool suppress);
    void compileStoreLValue(const ASTNode *target, keel::u16 valReg, bool suppress);

    bool needsGeneralLValuePath(const ASTNode *lhs) const;
    keel::u16 compileLValueStore(const ASTNode *lhs, const ASTNode *rhsNode,
                                int inValReg, bool suppress);
    keel::u16 compileSuperCall(const ASTNode *callNode, const ASTNode *superRef,
                              int explicitOutBase, keel::u16 nout);
    keel::u16 compileAnonFunc(const ASTNode *node);
    void collectFreeVars(const ASTNode *node,
                         const std::vector<std::string> &params,
                         std::vector<std::string> &freeVars);
    keel::u16 compileWhile(const ASTNode *node);
    keel::u16 compileBreak(const ASTNode *node);
    keel::u16 compileContinue(const ASTNode *node);

    keel::u16 compileFor(const ASTNode *node);
    keel::u16 compileColonExpr(const ASTNode *node);
    keel::u16 compileMatrixLiteral(const ASTNode *node);
    keel::u16 compileIndexExpr(const ASTNode *node);
    keel::u16 compileIndexAssign(const ASTNode *node);

    keel::u16 compileCall(const ASTNode *node);
    int tryCompileFused(const ASTNode *node);
    keel::u16 compileFused(const ASTNode *node, keel::usize ruleIdx,
                          const std::vector<const ASTNode *> &operands);
    keel::u16 compileCommandCall(const ASTNode *node);
    void recordCallArgNames(const ASTNode *callNode, keel::usize callInstrIdx);
    keel::u16 compileFunctionDef(const ASTNode *node);
    keel::u16 compileReturn(const ASTNode *node);
    void emitVarargoutReturn();
    keel::u16 compileDeleteAssign(const ASTNode *node);
    static keel::i8 resolveBuiltinId(const std::string &name, keel::usize nargs);

    // Constant register cache: maps constant pool index → register that holds it
    std::unordered_map<keel::i32, keel::u16> constRegCache_;
    // Pinned constant registers allocated at chunk entry (< maxVarReg_)
    std::unordered_map<keel::i32, keel::u16> pinnedConstRegs_;

    // Scalar register tracking (dynamic bitset across 16-bit register space)
    RegBitset scalarRegs_;

    // Defined register tracking (dynamic bitset across 16-bit register space)
    RegBitset definedRegs_;
    bool mayClear_ = false;
    bool mayInjectVars_ = false;

    struct NestedFuncInfo
    {
        std::string compiledName;
        std::string displaySource;
        std::vector<std::string> upvalueNames;
    };

    struct EnclosingLexicalContext
    {
        std::vector<std::string> inheritedUpvalueNames;
        std::unordered_map<std::string, NestedFuncInfo> siblingNestedFuncs;
    };

    std::unordered_map<std::string, keel::u16> upvalueRegs_;
    std::unordered_map<std::string, keel::u16> allScopeUpvalueBoxRegs_;
    std::unordered_map<std::string, NestedFuncInfo> nestedFuncInfo_;
    const EnclosingLexicalContext *pendingLexicalCtx_ = nullptr;

    void emitUpvalLoadRawIfShared(const std::string &name, keel::u16 reg);
    void emitUpvalStoreIfShared(const std::string &name, keel::u16 reg);
    keel::u16 emitNestedFuncHandle(const NestedFuncInfo &info);

    void peepholeOptimize();

    struct LoopContext
    {
        std::vector<keel::usize> breakPatches;
        std::vector<keel::usize> continuePatches;
        bool isForLoop = false;
        int tryDepthAtEntry = 0;
    };
    std::vector<LoopContext> loopStack_;
};

} // namespace numkit
