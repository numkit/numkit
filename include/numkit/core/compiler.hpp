// include/compiler.hpp
#pragma once

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
        std::vector<uint64_t> words;

        bool test(size_t pos) const
        {
            size_t w = pos >> 6;
            return w < words.size() && ((words[w] >> (pos & 63)) & 1ULL) != 0;
        }
        void set(size_t pos)
        {
            size_t w = pos >> 6;
            if (w >= words.size())
                words.resize(w + 1, 0ULL);
            words[w] |= (1ULL << (pos & 63));
        }
        void reset()
        {
            std::fill(words.begin(), words.end(), 0ULL);
        }
        void reset(size_t pos)
        {
            size_t w = pos >> 6;
            if (w < words.size())
                words[w] &= ~(1ULL << (pos & 63));
        }
    };

    Engine &engine_;

    // Current chunk being compiled
    BytecodeChunk chunk_;

    // Register allocation: variable name → register index
    std::unordered_map<std::string, uint16_t> varRegisters_;
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
    uint16_t nargoutContext_ = 1; // expected number of outputs (0=statement, 1=expression)
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
    uint16_t indexContextArr_ = kInvalidReg;
    uint16_t indexContextDim_ = 0;
    uint16_t indexContextNdims_ = 1;

    // Current source location (updated before compiling each node)
    SourceLoc currentLoc_{};

    // Compiled function table (persists across compile() calls) —
    // workspace-scope (cleared by `clear all`/`clear functions`).
    std::unordered_map<std::string, BytecodeChunk> compiledFuncs_;

    // Script-lexical compiled functions.
    std::unordered_map<std::string, BytecodeChunk> scriptLocalCompiledFuncs_;
    std::vector<std::unordered_map<std::string, BytecodeChunk>> savedScriptLocalCompiledFuncs_;
    int scriptDepth_ = 0;

    uint16_t varRegWrite(const std::string &name);
    uint16_t varRegRead(const std::string &name);
    uint16_t varRegLookup(const std::string &name, bool preloadReserved = true);
    uint16_t allocPinnedReg();
    void markAssigned(const std::string &name) { chunk_.assignedVars.insert(name); }
    void preImportGlobals(const ASTNode *ast);
    void collectAllIdentifiers(const ASTNode *node, std::unordered_set<std::string> &out);
    void collectAssignedNames(const ASTNode *node, std::vector<std::string> &out);
    void preallocateAssignedVars(const ASTNode *body);
    void preallocatePinnedConstants(const ASTNode *body);
    uint16_t tempReg();

    // Emit instructions
    void emit(Instruction instr);
    void emitABC(OpCode op, uint16_t a, uint16_t b, uint16_t c);
    void emitAB(OpCode op, uint16_t a, uint16_t b);
    void emitAD(OpCode op, uint16_t a, int32_t d);
    void emitA(OpCode op, uint16_t a);
    void emitD(OpCode op, int32_t d);
    void emitNone(OpCode op);

    size_t currentPos() const;
    void patchJump(size_t instrPos, int32_t offset);

    int32_t addConstant(double value);
    int32_t addStringConstant(const std::string &s);
    int32_t appendConstant(Value v);
    int32_t appendString(const std::string &s);

    uint16_t reserveRegs(size_t n);

    uint16_t compileNode(const ASTNode *node);
    uint16_t compileNodeExpand(const ASTNode *node);
    uint16_t compileBlock(const ASTNode *node);
    uint16_t compileNumber(const ASTNode *node);
    uint16_t compileString(const ASTNode *node);
    uint16_t compileBool(const ASTNode *node);
    uint16_t compileIdentifier(const ASTNode *node);
    uint16_t compileAssign(const ASTNode *node);
    uint16_t compileMultiAssign(const ASTNode *node);
    uint16_t compileBinaryOp(const ASTNode *node);
    uint16_t emitPlainBinary(const std::string &op, uint16_t left, uint16_t right);
    uint16_t emitFusedMulAdd(bool isPlus, const ASTNode *rhs, uint16_t acc);
    uint16_t compileUnaryOp(const ASTNode *node);
    uint16_t compileExprStmt(const ASTNode *node);

    uint16_t compileIf(const ASTNode *node);
    uint16_t compileSwitch(const ASTNode *node);
    uint16_t compileTryCatch(const ASTNode *node);
    uint16_t compileGlobalPersistent(const ASTNode *node);
    uint16_t compileFieldAccess(const ASTNode *node);
    uint16_t compileFieldAssign(const ASTNode *node);
    uint16_t compileCellLiteral(const ASTNode *node);
    uint16_t compileCellIndex(const ASTNode *node);
    uint16_t compileCellAssign(const ASTNode *node);

    uint16_t compileFieldStore(const ASTNode *lhs, const ASTNode *rhsNode,
                               int inValReg, bool suppress);
    uint16_t compileCellStore(const ASTNode *lhs, const ASTNode *rhsNode,
                              int inValReg, bool suppress);
    uint16_t compileIndexStore(const ASTNode *lhs, const ASTNode *rhsNode,
                               int inValReg, bool suppress);
    void compileStoreLValue(const ASTNode *target, uint16_t valReg, bool suppress);

    bool needsGeneralLValuePath(const ASTNode *lhs) const;
    uint16_t compileLValueStore(const ASTNode *lhs, const ASTNode *rhsNode,
                                int inValReg, bool suppress);
    uint16_t compileSuperCall(const ASTNode *callNode, const ASTNode *superRef,
                              int explicitOutBase, uint16_t nout);
    uint16_t compileAnonFunc(const ASTNode *node);
    void collectFreeVars(const ASTNode *node,
                         const std::vector<std::string> &params,
                         std::vector<std::string> &freeVars);
    uint16_t compileWhile(const ASTNode *node);
    uint16_t compileBreak(const ASTNode *node);
    uint16_t compileContinue(const ASTNode *node);

    uint16_t compileFor(const ASTNode *node);
    uint16_t compileColonExpr(const ASTNode *node);
    uint16_t compileMatrixLiteral(const ASTNode *node);
    uint16_t compileIndexExpr(const ASTNode *node);
    uint16_t compileIndexAssign(const ASTNode *node);

    uint16_t compileCall(const ASTNode *node);
    int tryCompileFused(const ASTNode *node);
    uint16_t compileFused(const ASTNode *node, size_t ruleIdx,
                          const std::vector<const ASTNode *> &operands);
    uint16_t compileCommandCall(const ASTNode *node);
    void recordCallArgNames(const ASTNode *callNode, size_t callInstrIdx);
    uint16_t compileFunctionDef(const ASTNode *node);
    uint16_t compileReturn(const ASTNode *node);
    void emitVarargoutReturn();
    uint16_t compileDeleteAssign(const ASTNode *node);
    static int8_t resolveBuiltinId(const std::string &name, size_t nargs);

    // Constant register cache: maps constant pool index → register that holds it
    std::unordered_map<int32_t, uint16_t> constRegCache_;
    // Pinned constant registers allocated at chunk entry (< maxVarReg_)
    std::unordered_map<int32_t, uint16_t> pinnedConstRegs_;

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

    std::unordered_map<std::string, uint16_t> upvalueRegs_;
    std::unordered_map<std::string, uint16_t> allScopeUpvalueBoxRegs_;
    std::unordered_map<std::string, NestedFuncInfo> nestedFuncInfo_;
    const EnclosingLexicalContext *pendingLexicalCtx_ = nullptr;

    void emitUpvalLoadRawIfShared(const std::string &name, uint16_t reg);
    void emitUpvalStoreIfShared(const std::string &name, uint16_t reg);
    uint16_t emitNestedFuncHandle(const NestedFuncInfo &info);

    void peepholeOptimize();

    struct LoopContext
    {
        std::vector<size_t> breakPatches;
        std::vector<size_t> continuePatches;
        bool isForLoop = false;
        int tryDepthAtEntry = 0;
    };
    std::vector<LoopContext> loopStack_;
};

} // namespace numkit
