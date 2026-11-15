// include/vm.hpp
#pragma once

#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/core/bytecode.hpp>
#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/debugger.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/value.hpp>

#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace numkit {

class Engine;
struct UserFunction;

class VM
{
public:
    explicit VM(Engine &engine);

    // Legacy API: runs to completion, throws DebugStopException on pause
    Value execute(const BytecodeChunk &chunk, const Value *args = nullptr, keel::usize nargs = 0);

    // Debug-aware API: can return Paused (state preserved for resume)
    // initialAction controls the debugger's step mode at entry:
    //   StepInto (default) — pause on the first source line
    //   Continue           — run until a breakpoint is hit
    ExecStatus startExecution(const BytecodeChunk &chunk,
                              const Value *args = nullptr,
                              keel::usize nargs = 0,
                              DebugAction initialAction = DebugAction::StepInto);
    ExecStatus resumeExecution();
    bool isPaused() const { return !frames_.empty(); }
    Value takeResult() { return std::move(lastResult_); }

    // Re-entrant call from C++ INTO a compiled chunk, returning all outputs.
    std::vector<Value> callReentrant(const BytecodeChunk &chunk, keel::ArrayRef<Value> args,
                                     keel::usize nargout,
                                     const std::string &ownerClass = std::string(),
                                     bool isCtor = false, const Value *ctorSeed = nullptr);

    // Schedule a user-code function-handle callback as a pausable VM frame on
    // behalf of a native continuation.
    bool pushCallbackFrame(const Value &handle, keel::ArrayRef<Value> args, keel::usize nargout,
                           std::shared_ptr<VmContinuation> cont);

    // Write a Value into regStack_ at an absolute offset (used by heap-boxed
    // continuation adapters so regStack_ growth never dangles a raw Value*).
    void writeRegAtOffset(keel::usize regOffset, Value val)
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

    keel::usize frameCount() const { return frames_.size(); }

    FrameView frameViewAt(keel::usize depthFromTop)
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
    static constexpr keel::usize kInitialRegStackSize = 256 * 50;
    std::vector<Value> regStack_;
    keel::usize regStackTop_ = 0;
    Value *R_ = nullptr;
    void ensureRegStackCapacity(keel::usize needed);

    // ── Explicit call frame stack (non-recursive VM) ────────
    struct CallFrame
    {
        const BytecodeChunk *chunk = nullptr;
        const Instruction *ip = nullptr; // saved instruction pointer
        Value *R = nullptr;             // register base for this frame
        keel::usize regBase = 0;              // offset in regStack_
        keel::u32 nregs = 0;              // register count
        keel::usize forStackBase = 0;         // forStack_.size() at frame entry
        keel::usize tryStackBase = 0;         // tryStack_.size() at frame entry
        keel::u16 destReg = 0;            // caller register for return value
        bool isMultiReturn = false;      // CALL_MULTI?
        keel::u16 outBase = 0;            // multi-return: output base register
        keel::u16 nout = 0;               // multi-return: output count
        keel::usize nargout = 1;
        std::string ownerClass;
        bool isCtor = false;
        bool isScriptFrame = false;

        std::unordered_map<std::string, Value> *dynVars = nullptr;
        std::unique_ptr<Environment> env;
        std::vector<std::string> callerArgNames;
        keel::usize originDepth = 0;
        std::shared_ptr<VmContinuation> cont;
    };
    std::vector<CallFrame> frames_;
    Value lastResult_;

    // For-loop state
    struct ForState
    {
        Value range;
        const keel::f64 *data = nullptr;
        const void *rawData = nullptr; // for char/logical
        ValueType rangeType = ValueType::DOUBLE;
        keel::usize index = 0;
        keel::usize count = 0;
        keel::usize rows = 0;
        bool   lazy = false;
        keel::f64 lazyStart = 0.0;
        keel::f64 lazyStep  = 1.0;
    };
    std::vector<ForState> forStack_;

    // Try/catch handler stack
    struct TryHandler
    {
        const Instruction *catchIp; // where to jump on exception
        keel::u16 exReg;             // register for exception struct
        keel::usize forStackSize;        // forStack_ size at TRY_BEGIN (restore on catch)
        keel::usize frameIndex;          // index in frames_ that owns this handler
    };
    std::vector<TryHandler> tryStack_;

    struct CallCacheEntry
    {
        const BytecodeChunk *userChunk = nullptr;
        const ExternalFunc *extFunc = nullptr;
        keel::u64 epoch = 0;
    };

    struct LvalDetach
    {
        enum class Kind : keel::u8 { Field, Cell };
        keel::usize containerRegAbs = 0;
        keel::usize valRegAbs = 0;
        Kind kind = Kind::Field;
        std::string fieldName;
        keel::usize cellLinearIdx = 0;
    };

public:
    struct PausedState
    {
        std::vector<CallFrame> frames;
        std::vector<ForState> forStack;
        std::vector<TryHandler> tryStack;
        std::vector<LvalDetach> lvalDetaches;
        std::vector<Value> regSnapshot;
        keel::usize regStackTop = 0;
        Value lastResult;
        std::unordered_map<const BytecodeChunk *,
                           std::vector<CallCacheEntry>>
            chunkCallCache;
        bool wasReentrantEval = false;
    };
    std::vector<PausedState *> activePausedStates_;
private:

    int maxRecursion_ = 200;

    // Per-chunk call target inline cache (user functions + external builtins)
    std::unordered_map<const BytecodeChunk *, std::vector<CallCacheEntry>> chunkCallCache_;

    // Unwind-safe temporary unshare stack for compound lvalues (`s.arr(i) = x`)
    std::vector<LvalDetach> lvalDetaches_;
    void rollbackLvalDetaches();

    // Reusable Environment pool to avoid heap allocations per function call
    std::vector<std::unique_ptr<Environment>> envPool_;

    // Exported variables after execution
    std::vector<std::pair<std::string, Value>> lastVarMap_;

    // Multi-return buffer (unbounded; cleared after distribution)
    std::vector<Value> returnBuf_;

    // ── Core dispatch loop (non-recursive) ──────────────────
    ExecStatus dispatchLoop();

    void pushCallFrame(const BytecodeChunk &funcChunk, const Value *args, keel::usize nargs,
                       keel::u16 destReg, keel::usize nargout,
                       bool isMulti = false, keel::u16 outBase = 0, keel::u16 nout = 0,
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
    bool tryBinaryOpFrame(keel::u16 dst, OpCode op, keel::u16 lhsReg, keel::u16 rhsReg,
                          CallFrame &frame, const Instruction *ip);
    bool tryUnaryOpFrame(keel::u16 dst, OpCode op, keel::u16 operandReg,
                         CallFrame &frame, const Instruction *ip);
    bool tryObjectSubsrefFrame(keel::u16 dst, keel::u16 selfReg, keel::ArrayRef<Value> idx,
                               CallFrame &frame, const Instruction *ip);
    bool tryObjectSubsrefFrameObj(const Value &self, keel::u16 dst, keel::ArrayRef<Value> idx,
                                  CallFrame &frame, const Instruction *ip);
    bool tryObjectSubsasgnFrame(keel::u16 objReg, keel::ArrayRef<Value> idxAndVal,
                                CallFrame &frame, const Instruction *ip);
    void execCallBuiltin(const Instruction &I, Value *R);
    bool execCallIndirect(const Instruction &I, Value *R,
                          CallFrame &frame, const Instruction *ip);
    bool execCallIndirectTarget(const Value &target, keel::u16 dstReg, keel::u16 argBase, keel::u16 na,
                                Value *R, CallFrame &frame, const Instruction *ip);
    bool execCallIndirectSpan(const Value &target, keel::u16 dstReg,
                              const Value *AB, keel::usize na, Value *R, CallFrame &frame,
                              const Instruction *ip);
    bool execCallIndirectMulti(const Instruction &I, Value *R,
                               CallFrame &frame, const Instruction *ip);
    void execIndirectIndex(const Instruction &I, Value *R);
    void execIndirectIndexTarget(const Value &target, keel::u16 dstReg, keel::u16 argBase, keel::u16 na,
                                 Value *R);
    void execIndirectIndexSpan(const Value &mv, keel::u16 dstReg, const Value *AB,
                               keel::usize na, Value *R);
    void execDisplay(const Instruction &I, Value *R, const BytecodeChunk &chunk);
    void execWho(const Instruction &I, Value *R, const BytecodeChunk &chunk);
    void execWhos(const Instruction &I, Value *R, const BytecodeChunk &chunk);

    DebugController *debugCtl();
};

} // namespace numkit
