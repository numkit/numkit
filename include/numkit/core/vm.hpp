// include/vm.hpp
#pragma once

#include <numkit/core/bytecode.hpp>
#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/debugger.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/value/value.hpp>

#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace numkit {

class Engine;
struct UserFunction;

// Result of a dispatch loop iteration — Completed or Paused (for debugger)
enum class ExecStatus : uint8_t { Completed, Paused };

class VM
{
public:
    explicit VM(Engine &engine);

    // Legacy API: runs to completion, throws DebugStopException on pause
    Value execute(const BytecodeChunk &chunk, const Value *args = nullptr, size_t nargs = 0);

    // Debug-aware API: can return Paused (state preserved for resume)
    // initialAction controls the debugger's step mode at entry:
    //   StepInto (default) — pause on the first source line
    //   Continue           — run until a breakpoint is hit
    ExecStatus startExecution(const BytecodeChunk &chunk,
                              const Value *args = nullptr,
                              size_t nargs = 0,
                              DebugAction initialAction = DebugAction::StepInto);
    ExecStatus resumeExecution();
    bool isPaused() const { return !frames_.empty(); }
    Value takeResult() { return std::move(lastResult_); }

    // Re-entrant call from C++ INTO a compiled chunk, returning all outputs.
    std::vector<Value> callReentrant(const BytecodeChunk &chunk, Span<const Value> args,
                                     size_t nargout,
                                     const std::string &ownerClass = std::string(),
                                     bool isCtor = false, const Value *ctorSeed = nullptr);

    // Schedule a user-code function-handle callback as a pausable VM frame on
    // behalf of a native continuation.
    bool pushCallbackFrame(const Value &handle, Span<const Value> args, size_t nargout,
                           std::shared_ptr<VmContinuation> cont);

    // Write a Value into regStack_ at an absolute offset (used by heap-boxed
    // continuation adapters so regStack_ growth never dangles a raw Value*).
    void writeRegAtOffset(size_t regOffset, Value val)
    {
        ensureRegStackCapacity(regOffset + 1);
        regStack_[regOffset] = std::move(val);
    }

    struct PausedState;
    std::unique_ptr<PausedState> savePausedState();
    void restorePausedState(std::unique_ptr<PausedState> state);

    void setCompiledFuncs(const std::unordered_map<std::string, BytecodeChunk> *funcs)
    {
        compiledFuncs_ = funcs;
        scriptLocalFuncs_ = nullptr;
    }
    void setCompiledFuncs(const std::unordered_map<std::string, BytecodeChunk> *funcs,
                          const std::unordered_map<std::string, BytecodeChunk> *scriptLocal)
    {
        compiledFuncs_ = funcs;
        scriptLocalFuncs_ = scriptLocal;
    }

    const BytecodeChunk *findCompiledFunc(const std::string &name) const
    {
        if (scriptLocalFuncs_) {
            auto it = scriptLocalFuncs_->find(name);
            if (it != scriptLocalFuncs_->end())
                return &it->second;
        }
        if (compiledFuncs_) {
            auto it = compiledFuncs_->find(name);
            if (it != compiledFuncs_->end())
                return &it->second;
        }
        return nullptr;
    }

    // After execute(), get exported variables for environment sync
    const std::vector<std::pair<std::string, Value>> &lastVarMap() const { return lastVarMap_; }
    void clearLastVarMap() { lastVarMap_.clear(); }

    void setFrameDynVars(std::unordered_map<std::string, Value> *dv);

    struct FrameView
    {
        const BytecodeChunk *chunk = nullptr;
        Value *registers = nullptr;
        bool wasReentrantEval = false;
    };
    FrameView currentFrameView()
    {
        if (frames_.empty())
            return {};
        auto &f = frames_.back();
        return { f.chunk, f.R };
    }

    size_t frameCount() const { return frames_.size(); }

    FrameView frameViewAt(size_t depthFromTop)
    {
        if (depthFromTop >= frames_.size())
            return {};
        auto &f = frames_[frames_.size() - 1 - depthFromTop];
        return { f.chunk, f.R };
    }

    int callDepth() const;

    const std::string &currentMethodClass() const
    {
        static const std::string kEmpty;
        return frames_.empty() ? kEmpty : frames_.back().ownerClass;
    }
    bool currentMethodIsCtor() const
    {
        return !frames_.empty() && frames_.back().isCtor;
    }

    void setMaxRecursionDepth(int d) { maxRecursion_ = d; }

    Environment *callerEnvAtDepth(int n);
    void assignInCallerFrame(int n, const std::string &name, const Value &val);

    Environment *inheritedScope_ = nullptr;
    bool inReentrantEval_ = false;

    void setStopOnError(bool b) { stopOnError_ = b; }
    bool atErrorPause() const { return atErrorPause_; }
    const std::string &errorPauseMessage() const { return errorPauseMsg_; }

    bool stopOnError_ = false;
    bool atErrorPause_ = false;
    std::exception_ptr pausedError_;
    std::string errorPauseMsg_;

    void writeToFrameMatchingEnv(Environment *env, const std::string &name,
                                 const Value &val);
    void syncFrameToEnv(Environment *env);
    void syncEnvRemovalsToFrame(Environment *env);
    bool hasActiveFrameNamed(const std::string &funcName) const;
    void flushWorkspaceFrame();

    const std::vector<std::string> &currentFrameCallerArgNames() const
    {
        static const std::vector<std::string> kEmpty;
        if (frames_.size() < 2) return kEmpty;
        return frames_.back().callerArgNames;
    }

    std::unordered_map<std::string, Value>
    snapshotFrameVars(Environment *frameEnv);

    void setNextFrameDynVars(std::unordered_map<std::string, Value> *dv)
    {
        nextFrameDynVars_ = dv;
    }
    std::unordered_map<std::string, Value> *nextFrameDynVars() const
    {
        return nextFrameDynVars_;
    }

private:
    Engine &engine_;
    std::unordered_map<std::string, Value> *nextFrameDynVars_ = nullptr;
    const std::unordered_map<std::string, BytecodeChunk> *compiledFuncs_ = nullptr;
    const std::unordered_map<std::string, BytecodeChunk> *scriptLocalFuncs_ = nullptr;

    // Register stack — grows dynamically via ensureRegStackCapacity
    static constexpr size_t kInitialRegStackSize = 256 * 50;
    std::vector<Value> regStack_;
    size_t regStackTop_ = 0;
    Value *R_ = nullptr;
    void ensureRegStackCapacity(size_t needed);

    // ── Explicit call frame stack (non-recursive VM) ────────
    struct CallFrame
    {
        const BytecodeChunk *chunk = nullptr;
        const Instruction *ip = nullptr; // saved instruction pointer
        Value *R = nullptr;             // register base for this frame
        size_t regBase = 0;              // offset in regStack_
        uint32_t nregs = 0;              // register count
        size_t forStackBase = 0;         // forStack_.size() at frame entry
        size_t tryStackBase = 0;         // tryStack_.size() at frame entry
        uint16_t destReg = 0;            // caller register for return value
        bool isMultiReturn = false;      // CALL_MULTI?
        uint16_t outBase = 0;            // multi-return: output base register
        uint16_t nout = 0;               // multi-return: output count
        size_t nargout = 1;
        std::string ownerClass;
        bool isCtor = false;
        bool isScriptFrame = false;

        std::unordered_map<std::string, Value> *dynVars = nullptr;
        std::unique_ptr<Environment> env;
        std::vector<std::string> callerArgNames;
        size_t originDepth = 0;
        std::shared_ptr<VmContinuation> cont;
    };
    std::vector<CallFrame> frames_;
    Value lastResult_;

    // For-loop state
    struct ForState
    {
        Value range;
        const double *data = nullptr;
        const void *rawData = nullptr; // for char/logical
        ValueType rangeType = ValueType::DOUBLE;
        size_t index = 0;
        size_t count = 0;
        size_t rows = 0;
        bool   lazy = false;
        double lazyStart = 0.0;
        double lazyStep  = 1.0;
    };
    std::vector<ForState> forStack_;

    // Try/catch handler stack
    struct TryHandler
    {
        const Instruction *catchIp; // where to jump on exception
        uint16_t exReg;             // register for exception struct
        size_t forStackSize;        // forStack_ size at TRY_BEGIN (restore on catch)
        size_t frameIndex;          // index in frames_ that owns this handler
    };
    std::vector<TryHandler> tryStack_;

public:
    struct PausedState
    {
        std::vector<CallFrame> frames;
        std::vector<ForState> forStack;
        std::vector<TryHandler> tryStack;
        std::vector<Value> regSnapshot;
        size_t regStackTop = 0;
        Value lastResult;
        std::unordered_map<const BytecodeChunk *,
                           std::vector<const BytecodeChunk *>>
            chunkCallCache;
        bool wasReentrantEval = false;
    };
    std::vector<PausedState *> activePausedStates_;
private:

    int maxRecursion_ = 200;

    // Per-chunk call target cache
    std::unordered_map<const BytecodeChunk *, std::vector<const BytecodeChunk *>> chunkCallCache_;

    // Exported variables after execution
    std::vector<std::pair<std::string, Value>> lastVarMap_;

    // Multi-return buffer (unbounded; cleared after distribution)
    std::vector<Value> returnBuf_;

    // ── Core dispatch loop (non-recursive) ──────────────────
    ExecStatus dispatchLoop();

    void pushCallFrame(const BytecodeChunk &funcChunk, const Value *args, size_t nargs,
                       uint16_t destReg, size_t nargout,
                       bool isMulti = false, uint16_t outBase = 0, uint16_t nout = 0,
                       const std::string &ownerClass = std::string(), bool isCtor = false,
                       const Value *ctorSeed = nullptr);
    void popCallFrame(Value retVal);
    bool startContinuation(std::shared_ptr<VmContinuation> cont);

    Environment *currentCallEnv();
    Environment *materializeFrameEnv(CallFrame &f, const Value *frameRegs);
    void exportTopLevelVariables();

    bool dispatchTryCatch(const char *msg, const char *identifier);
    [[noreturn]] void enrichAndThrow(const std::exception &ex,
                                     const Instruction *ip,
                                     const BytecodeChunk &chunk);

    void forSetVar(Value &varReg, const ForState &fs);

    Value binarySlowPath(OpCode op, const Value &lhs, const Value &rhs);
    Value unarySlowPath(OpCode op, const Value &operand);
    bool tryBinaryOpFrame(uint16_t dst, OpCode op, uint16_t lhsReg, uint16_t rhsReg,
                          CallFrame &frame, const Instruction *ip);
    bool tryUnaryOpFrame(uint16_t dst, OpCode op, uint16_t operandReg,
                         CallFrame &frame, const Instruction *ip);
    bool tryObjectSubsrefFrame(uint16_t dst, uint16_t selfReg, Span<const Value> idx,
                               CallFrame &frame, const Instruction *ip);
    bool tryObjectSubsrefFrameObj(const Value &self, uint16_t dst, Span<const Value> idx,
                                  CallFrame &frame, const Instruction *ip);
    bool tryObjectSubsasgnFrame(uint16_t objReg, Span<const Value> idxAndVal,
                                CallFrame &frame, const Instruction *ip);
    void execCallBuiltin(const Instruction &I, Value *R);
    bool execCallIndirect(const Instruction &I, Value *R,
                          CallFrame &frame, const Instruction *ip);
    bool execCallIndirectTarget(const Value &target, uint16_t dstReg, uint16_t argBase, uint16_t na,
                                Value *R, CallFrame &frame, const Instruction *ip);
    bool execCallIndirectSpan(const Value &target, uint16_t dstReg,
                              const Value *AB, size_t na, Value *R, CallFrame &frame,
                              const Instruction *ip);
    bool execCallIndirectMulti(const Instruction &I, Value *R,
                               CallFrame &frame, const Instruction *ip);
    void execIndirectIndex(const Instruction &I, Value *R);
    void execIndirectIndexTarget(const Value &target, uint16_t dstReg, uint16_t argBase, uint16_t na,
                                 Value *R);
    void execIndirectIndexSpan(const Value &mv, uint16_t dstReg, const Value *AB,
                               size_t na, Value *R);
    void execDisplay(const Instruction &I, Value *R, const BytecodeChunk &chunk);
    void execWho(const Instruction &I, Value *R, const BytecodeChunk &chunk);
    void execWhos(const Instruction &I, Value *R, const BytecodeChunk &chunk);

    DebugController *debugCtl();
};

} // namespace numkit
