// include/vm.hpp
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/bytecode.hpp>
#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/debugger.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/value.hpp>

#include <exception>
#include <utility>

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
    Value takeResult() { return keel::move(lastResult_); }

    // Re-entrant call from C++ INTO a compiled chunk, returning all outputs.
    keel::Array<Value> callReentrant(const BytecodeChunk &chunk, keel::ArrayRef<Value> args,
                                     keel::usize nargout,
                                     keel::StringRef ownerClass = {},
                                     bool isCtor = false, const Value *ctorSeed = nullptr);

    // Schedule a user-code function-handle callback as a pausable VM frame on
    // behalf of a native continuation.
    bool pushCallbackFrame(const Value &handle, keel::ArrayRef<Value> args, keel::usize nargout,
                           keel::Arc<VmContinuation> cont);

    // Write a Value into regStack_ at an absolute offset (used by heap-boxed
    // continuation adapters so regStack_ growth never dangles a raw Value*).
    void writeRegAtOffset(keel::usize regOffset, Value val)
    {
        ensureRegStackCapacity(regOffset + 1);
        regStack_[regOffset] = keel::move(val);
    }

    struct PausedState;
    keel::UniquePtr<PausedState> savePausedState();
    void restorePausedState(keel::UniquePtr<PausedState> state);

    using CompiledFuncMap = keel::HashMap<keel::String, keel::Arc<BytecodeChunk>>;

    void setCompiledFuncs(const CompiledFuncMap *funcs)
    {
        compiledFuncs_ = funcs;
        scriptLocalFuncs_ = nullptr;
    }
    void setCompiledFuncs(const CompiledFuncMap *funcs,
                          const CompiledFuncMap *scriptLocal)
    {
        compiledFuncs_ = funcs;
        scriptLocalFuncs_ = scriptLocal;
    }

    const BytecodeChunk *findCompiledFunc(keel::StringRef name) const
    {
        if (scriptLocalFuncs_) {
            if (const auto *c = scriptLocalFuncs_->get(name))
                return c->get();
        }
        if (compiledFuncs_) {
            if (const auto *c = compiledFuncs_->get(name))
                return c->get();
        }
        return nullptr;
    }

    // After execute(), get exported variables for environment sync
    const keel::Array<std::pair<keel::String, Value>> &lastVarMap() const { return lastVarMap_; }
    void clearLastVarMap() { lastVarMap_.clear(); }

    void setFrameDynVars(keel::HashMap<keel::String, Value> *dv);

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

    keel::i32 callDepth() const;
    keel::Array<CallStackFrame> captureCallStack() const;

    keel::StringRef currentMethodClass() const
    {
        return frames_.empty() ? keel::StringRef{} : frames_.back().ownerClass.as_ref();
    }
    bool currentMethodIsCtor() const
    {
        return !frames_.empty() && frames_.back().isCtor;
    }

    keel::i64 currentFunctionNargin() const
    {
        for (keel::usize i = frames_.size(); i > 0; --i) {
            const auto &f = frames_[i - 1];
            if (!f.isScriptFrame) return keel::as<keel::i64>(f.nargin);
        }
        return -1;
    }
    keel::i64 currentFunctionNargout() const
    {
        for (keel::usize i = frames_.size(); i > 0; --i) {
            const auto &f = frames_[i - 1];
            if (!f.isScriptFrame) return keel::as<keel::i64>(f.nargout);
        }
        return -1;
    }

    void setMaxRecursionDepth(keel::i32 d) { maxRecursion_ = d; }

    Environment *callerEnvAtDepth(keel::i32 n);
    void assignInCallerFrame(keel::i32 n, keel::StringRef name, const Value &val);

    Environment *inheritedScope_ = nullptr;
    bool inReentrantEval_ = false;

    void setStopOnError(bool b) { stopOnError_ = b; }
    bool atErrorPause() const { return atErrorPause_; }
    const keel::String &errorPauseMessage() const { return errorPauseMsg_; }

    bool stopOnError_ = false;
    bool atErrorPause_ = false;
    std::exception_ptr pausedError_;
    keel::String errorPauseMsg_;

    void writeToFrameMatchingEnv(Environment *env, keel::StringRef name,
                                 const Value &val);
    void syncFrameToEnv(Environment *env);
    void syncEnvRemovalsToFrame(Environment *env);
    bool hasActiveFrameNamed(keel::StringRef funcName) const;
    void flushWorkspaceFrame();

    keel::ArrayRef<keel::String> currentFrameCallerArgNames() const
    {
        if (frames_.size() < 2) return {};
        return frames_.back().callerArgNames.as_ref();
    }

    keel::HashMap<keel::String, Value>
    snapshotFrameVars(Environment *frameEnv);

    void setNextFrameDynVars(keel::HashMap<keel::String, Value> *dv)
    {
        nextFrameDynVars_ = dv;
    }
    keel::HashMap<keel::String, Value> *nextFrameDynVars() const
    {
        return nextFrameDynVars_;
    }

private:
    Engine &engine_;
    keel::HashMap<keel::String, Value> *nextFrameDynVars_ = nullptr;
    const CompiledFuncMap *compiledFuncs_ = nullptr;
    const CompiledFuncMap *scriptLocalFuncs_ = nullptr;

    // Register stack — grows dynamically via ensureRegStackCapacity
    static constexpr keel::usize kInitialRegStackSize = 256 * 50;
    keel::Array<Value> regStack_;
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
        keel::usize nargin = 0;
        keel::usize nargout = 1;
        keel::String ownerClass;
        bool isCtor = false;
        bool isScriptFrame = false;

        keel::HashMap<keel::String, Value> *dynVars = nullptr;
        keel::UniquePtr<Environment> env;
        keel::Array<keel::String> callerArgNames;
        keel::usize originDepth = 0;
        keel::Arc<VmContinuation> cont;
    };
    keel::Array<CallFrame> frames_;
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
    keel::Array<ForState> forStack_;

    // Try/catch handler stack
    struct TryHandler
    {
        const Instruction *catchIp; // where to jump on exception
        keel::u16 exReg;             // register for exception struct
        keel::usize forStackSize;        // forStack_ size at TRY_BEGIN (restore on catch)
        keel::usize frameIndex;          // index in frames_ that owns this handler
    };
    keel::Array<TryHandler> tryStack_;

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
        keel::String fieldName;
        keel::usize cellLinearIdx = 0;

        LvalDetach clone(keel::Allocator *alloc = nullptr) const
        {
            return LvalDetach{
                containerRegAbs,
                valRegAbs,
                kind,
                fieldName.clone(alloc),
                cellLinearIdx,
            };
        }
    };

public:
    struct PausedState
    {
        keel::Array<CallFrame> frames;
        keel::Array<ForState> forStack;
        keel::Array<TryHandler> tryStack;
        keel::Array<LvalDetach> lvalDetaches;
        keel::Array<Value> regSnapshot;
        keel::usize regStackTop = 0;
        Value lastResult;
        keel::HashMap<const BytecodeChunk *,
                      keel::Array<CallCacheEntry>>
            chunkCallCache;
        bool wasReentrantEval = false;
    };
    keel::Array<PausedState *> activePausedStates_;
private:

    keel::i32 maxRecursion_ = 200;

    // Per-chunk call target inline cache (user functions + external builtins)
    keel::HashMap<const BytecodeChunk *, keel::Array<CallCacheEntry>> chunkCallCache_;

    // Unwind-safe temporary unshare stack for compound lvalues (`s.arr(i) = x`)
    keel::Array<LvalDetach> lvalDetaches_;
    void rollbackLvalDetaches();

    // Reusable Environment pool to avoid heap allocations per function call
    keel::Array<keel::UniquePtr<Environment>> envPool_;

    // Exported variables after execution
    keel::Array<std::pair<keel::String, Value>> lastVarMap_;

    // Multi-return buffer (unbounded; cleared after distribution)
    keel::Array<Value> returnBuf_;

    // ── Core dispatch loop (non-recursive) ──────────────────
    ExecStatus dispatchLoop();

    void pushCallFrame(const BytecodeChunk &funcChunk, const Value *args, keel::usize nargs,
                       keel::u16 destReg, keel::usize nargout,
                       bool isMulti = false, keel::u16 outBase = 0, keel::u16 nout = 0,
                       keel::StringRef ownerClass = {}, bool isCtor = false,
                       const Value *ctorSeed = nullptr);
    void popCallFrame(Value retVal);
    bool startContinuation(keel::Arc<VmContinuation> cont);

    Environment *currentCallEnv();
    Environment *materializeFrameEnv(CallFrame &f, const Value *frameRegs);
    void exportTopLevelVariables();

    bool dispatchTryCatch(const char *msg, const char *identifier);
    static keel::String describeInstruction(const Instruction &instr,
                                            const BytecodeChunk &chunk);
    void enrichError(Error &mle, const Instruction *ip, const BytecodeChunk &chunk);
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
    bool execIndexSet1DSlow(const Instruction &I, Value *R,
                            CallFrame &frame, const Instruction *ip);
    bool execIndexSet2DSlow(const Instruction &I, Value *R,
                            CallFrame &frame, const Instruction *ip);
    bool execIndexSetND(const Instruction &I, Value *R,
                        CallFrame &frame, const Instruction *ip);
    bool execFieldGet(keel::u16 dstReg, keel::u16 objReg, keel::StringRef fname,
                      Value *R, CallFrame &frame, const Instruction *ip);
    void execFieldGetOrCreate(keel::u16 dstReg, keel::u16 objReg, keel::StringRef fname,
                              Value *R);
    bool execFieldSet(keel::u16 objReg, keel::u16 valReg, keel::StringRef fname,
                      Value *R, CallFrame &frame, const Instruction *ip);
    bool execCallMethod(const Instruction &I, Value *R,
                        CallFrame &frame, const Instruction *ip);
    bool execCallMethodMulti(const Instruction &I, Value *R,
                             CallFrame &frame, const Instruction *ip);
    void execCallBuiltin(const Instruction &I, Value *R);
    const Value *findDynLocalVar(const BytecodeChunk &chunk, const CallFrame &frame,
                                 const Value *R, const keel::String &name) const;
    // Unified scope-isolated dynamic variable lookup shared by ASSERT_DEF and
    // execCallNamedSingleSpan fallbacks. Never leaks workspaceEnv into function frames.
    keel::Optional<Value> lookupDynamicVar(const BytecodeChunk &chunk, const CallFrame &frame,
                                           const Value *R, keel::StringRef name,
                                           bool checkParentFunc) const;
    bool execCallNamedSingleSpan(keel::u16 dstReg, keel::i32 funcIdx, keel::u16 nargout_val,
                                 const Value *AB, keel::usize NF, Value *&R,
                                 CallFrame &frame, const Instruction *ip,
                                 keel::Array<CallCacheEntry> &resolvedFuncs);
    bool execCallNamedMultiSpan(keel::u16 outBase, keel::i32 funcIdx, keel::u16 nout,
                                const Value *AB, keel::usize NF, Value *R,
                                CallFrame &frame, const Instruction *ip,
                                keel::Array<CallCacheEntry> &resolvedFuncs);
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
