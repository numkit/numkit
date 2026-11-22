// src/vm.cpp
#include <keel/array.h>
#include <keel/math.h>
#include <keel/complex.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/optional.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/vm.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/index.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>

// Forward-declarations from toolboxes/builtin/src/backends/binary_ops_loops.hpp.
// Linked from the same nk library; lets the VM bypass the public
// builtin::plus()/etc. wrappers (which always allocate a fresh result
// Value) when it can write straight into a uniquely-owned destination
// buffer of the right shape.
namespace numkit::ops::detail {
void plusLoop   (const keel::f64 *a, const keel::f64 *b, keel::f64 *out, keel::usize n);
void minusLoop  (const keel::f64 *a, const keel::f64 *b, keel::f64 *out, keel::usize n);
void timesLoop  (const keel::f64 *a, const keel::f64 *b, keel::f64 *out, keel::usize n);
void rdivideLoop(const keel::f64 *a, const keel::f64 *b, keel::f64 *out, keel::usize n);
} // namespace numkit::ops::detail

#include "vm_internal.hpp"

namespace numkit {

// Output-reuse fast path for `dst = lhs op rhs` where both inputs and
// the destination are real heap doubles of the same shape and dst has
// unique ownership. Skips the N-element alloc + free that the public
// builtin::plus()/etc. wrappers would otherwise do per call — at
// N ≥ 256k that allocation hits Windows VirtualAlloc / page-commit
// and starts costing 3× more than the SIMD kernel itself
// (see benchmarks BM_PlusAlloc / BM_PlusKernel for the breakdown).
//
// Aliasing notes: the four supported ops (+ − .* ./) are independent
// per element — output may safely alias either input (e.g. `z = z + z`
// or `z = x + z`) because plusLoop reads (a[i], b[i]) before writing
// out[i]. Cross-output sharing is the dangerous case; the
// heapRefCount() == 1 guard rules it out (any other variable holding
// dst's buffer would bump it to ≥2).
static bool tryInPlaceBinaryOp(Value &dst, OpCode op,
                               const Value &lhs, const Value &rhs)
{
    if (!dst.isHeapDouble() || dst.heapRefCount() != 1) return false;
    if (!lhs.isHeapDouble() || !rhs.isHeapDouble())     return false;
    if (!(dst.dims() == lhs.dims()) || !(dst.dims() == rhs.dims())) return false;

    const keel::usize n = dst.numel();
    const keel::f64 *a = lhs.doubleData();
    const keel::f64 *b = rhs.doubleData();
    keel::f64       *o = dst.doubleDataMut();   // refCount==1 → no detach copy
    switch (op) {
    case OpCode::ADD:   ops::detail::plusLoop   (a, b, o, n); return true;
    case OpCode::SUB:   ops::detail::minusLoop  (a, b, o, n); return true;
    case OpCode::EMUL:  ops::detail::timesLoop  (a, b, o, n); return true;
    case OpCode::ERDIV: ops::detail::rdivideLoop(a, b, o, n); return true;
    default: return false;
    }
}

VM::VM(Engine &engine)
    : engine_(engine)
{
    regStack_.resize(kInitialRegStackSize);
}

void VM::ensureRegStackCapacity(keel::usize needed)
{
    if (needed <= regStack_.size())
        return;
    keel::usize newCap = std::max(regStack_.size(), kInitialRegStackSize);
    while (newCap < needed)
        newCap *= 2;
    regStack_.resize(newCap);
    for (auto &f : frames_)
        f.R = regStack_.data() + f.regBase;
    R_ = frames_.empty() ? regStack_.data() : frames_.back().R;
    if (auto *ctl = debugCtl()) {
        auto &stack = ctl->callStack();
        const keel::usize n = std::min(stack.size(), frames_.size());
        for (keel::usize i = 0; i < n; ++i)
            stack[i].registers = frames_[i].R;
    }
}

// Single source of truth for inline-builtin id → name. Must track the unary
// (0-15) and binary (20-25) ids in Compiler::resolveBuiltinId; holes (16-19)
// are nullptr. Shared by describeInstruction (error text) and execCallBuiltin
// (generic external fallback) so the two can never drift out of sync.
static const char *builtinIdName(keel::i32 bid)
{
    return fastBuiltinName(static_cast<FastBuiltinId>(bid));
}

// Fast scalar check for VM arithmetic — accepts double scalars AND logical scalar tags
static inline bool isArithScalar(const Value &v)
{
    return v.isDoubleScalar() || v.isLogicalScalar();
}

static inline keel::f64 asScalar(const Value &v)
{
    return v.fastScalarVal();
}

static void writeChunkVar(const BytecodeChunk &chunk, Value *R, keel::u32 nregs,
                          keel::StringRef name, keel::u16 reg, const Value &val)
{
    if (reg < nregs)
        R[reg] = val;
    if (const keel::u16 *uv = chunk.upvalueBoxRegs.get(name); uv && *uv < nregs) {
        if (UpvaluePayload *box = asUpvalueBox(R[*uv]))
            box->write(val);
    }
}

static void exportChunkGlobalsAndPersistents(Engine &engine, Environment &globalsEnv,
                                             const BytecodeChunk &chunk, const Value *R,
                                             keel::u32 nregs, bool isTopLevel)
{
    for (const auto &gname : chunk.globalNames) {
        for (const auto &[vname, reg] : chunk.varMap) {
            if (vname == gname && reg < nregs) {
                Value gv = readChunkVar(chunk, R, nregs, vname, reg);
                if (isTopLevel) {
                    if (!gv.isUnset() && !gv.isDeleted())
                        globalsEnv.set(gname, keel::move(gv));
                } else {
                    if (gv.isDeleted())
                        globalsEnv.remove(gname);
                    else if (!gv.isUnset())
                        globalsEnv.set(gname, keel::move(gv));
                }
                break;
            }
        }
    }
    if (!isTopLevel && !chunk.persistentNames.empty()) {
        auto &bucket = engine.persistentBucket(chunk.name);
        for (const auto &pname : chunk.persistentNames) {
            for (const auto &[vname, reg] : chunk.varMap) {
                if (vname == pname && reg < nregs) {
                    Value pv = readChunkVar(chunk, R, nregs, vname, reg);
                    if (pv.isDeleted())
                        bucket.remove(pname);
                    else if (!pv.isUnset())
                        bucket.put(pname.clone(), keel::move(pv));
                    break;
                }
            }
        }
    }
}

static void importChunkGlobalsAndPersistents(Engine &engine, Environment &globalsEnv,
                                             const BytecodeChunk &chunk, Value *R,
                                             keel::u32 nregs, bool isTopLevel, bool seedMissing)
{
    for (const auto &gname : chunk.globalNames) {
        for (const auto &[vname, reg] : chunk.varMap) {
            if (vname == gname && reg < nregs) {
                Value *gval = globalsEnv.get(gname);
                if (gval && !gval->isUnset() && !gval->isDeleted()) {
                    writeChunkVar(chunk, R, nregs, vname, reg, *gval);
                } else if (seedMissing) {
                    writeChunkVar(chunk, R, nregs, vname, reg, Value::Empty);
                }
                break;
            }
        }
    }
    if (!isTopLevel && !chunk.persistentNames.empty()) {
        auto &bucket = engine.persistentBucket(chunk.name);
        for (const auto &pname : chunk.persistentNames) {
            for (const auto &[vname, reg] : chunk.varMap) {
                if (vname == pname && reg < nregs) {
                    const Value *pv = bucket.get(pname);
                    if (pv && !pv->isUnset() && !pv->isDeleted()) {
                        writeChunkVar(chunk, R, nregs, vname, reg, *pv);
                    } else if (seedMissing) {
                        bucket.put(pname.clone(), Value::Empty);
                        writeChunkVar(chunk, R, nregs, vname, reg, Value::Empty);
                    } else if (!pv) {
                        writeChunkVar(chunk, R, nregs, vname, reg, Value::Empty);
                    }
                    break;
                }
            }
        }
    }
}

keel::i32 VM::callDepth() const
{
    keel::i32 d = 0;
    for (const auto &f : frames_) {
        if (!f.isScriptFrame)
            ++d;
    }
    for (const PausedState *ps : activePausedStates_) {
        if (!ps) continue;
        for (const auto &f : ps->frames) {
            if (!f.isScriptFrame)
                ++d;
        }
    }
    return d;
}

bool VM::hasActiveFrameNamed(keel::StringRef funcName) const
{
    for (const auto &f : frames_) {
        if (f.chunk && f.chunk->name == funcName)
            return true;
    }
    for (const PausedState *ps : activePausedStates_) {
        if (!ps) continue;
        for (const auto &f : ps->frames) {
            if (f.chunk && f.chunk->name == funcName)
                return true;
        }
    }
    return false;
}

// ============================================================
// Public API
// ============================================================

Value VM::execute(const BytecodeChunk &chunk, const Value *args, keel::usize nargs)
{
    // Re-entrancy: a builtin invoked during dispatch (e.g. the `run`
    // builtin or any future eval-from-handler) may call back into
    // engine.eval(), which lands here while the outer chunk is mid-
    // execution. Snapshot the running VM state, run the inner chunk
    // through the normal startExecution / dispatch path, then restore
    // so the outer dispatch loop resumes seamlessly.
    const bool reentrant = !frames_.empty();
    keel::UniquePtr<PausedState> outerState;
    if (reentrant)
        outerState = savePausedState();

    // RAII: export variables and cleanup on ANY exit path (success, exception,
    // debug stop). Top-level only — re-entrant runs restore the outer frames
    // explicitly below so the dispatch loop can resume.
    struct ExecuteGuard
    {
        VM &vm;
        bool reentrant = false;
        ExecuteGuard(VM &v, bool r) : vm(v), reentrant(r) {}
        ~ExecuteGuard()
        {
            if (reentrant)
                return; // restoration happens on the success path below
            if (!vm.frames_.empty()) {
                vm.exportTopLevelVariables();
                // Abandoned frames (error unwind) — drop their origin pushes
                // down to the depth this run started at.
                vm.engine_.truncateScriptOrigins(vm.frames_.front().originDepth);
            }
            vm.frames_.clear();
            vm.forStack_.clear();
            vm.tryStack_.clear();
            vm.regStackTop_ = 0;
            vm.R_ = nullptr;
        }
        ExecuteGuard(const ExecuteGuard &) = delete;
        ExecuteGuard &operator=(const ExecuteGuard &) = delete;
    } guard(*this, reentrant);

    ExecStatus status;
    try {
        status = startExecution(chunk, args, nargs);
    } catch (...) {
        // Inner threw before producing a result. Restore the outer so the
        // outer's catch handler sees consistent VM state, then rethrow.
        if (reentrant)
            restorePausedState(keel::move(outerState));
        throw;
    }

    if (status == ExecStatus::Paused) {
        // Legacy API: convert pause to exception. The non-reentrant guard
        // wipes state; the reentrant path restores the outer first.
        if (reentrant)
            restorePausedState(keel::move(outerState));
        throw DebugStopException();
    }

    Value result = keel::move(lastResult_);
    if (reentrant) {
        // Commit any workspace exports the inner script produced, then
        // bring back the outer frame stack so dispatch can resume.
        if (!frames_.empty())
            exportTopLevelVariables();
        restorePausedState(keel::move(outerState));
    }
    return result;
}

keel::Array<Value> VM::callReentrant(const BytecodeChunk &chunk, keel::ArrayRef<Value> args,
                                     keel::usize nargout, keel::StringRef ownerClass,
                                     bool isCtor, const Value *ctorSeed)
{
    // Save the outer VM state and run `chunk` as a fresh top-level frame, then
    // restore. Mirrors execute()'s re-entrancy (which is why it is safe: the
    // outer dispatch loop's frame/register references survive — savePausedState
    // parks the frames_ buffer and restorePausedState move-assigns it back, so
    // the buffer address is preserved). Unlike execute() we do NOT reset the
    // debug controller, so the callee's frames nest on the live debug stack.
    const bool reentrant = !frames_.empty();
    keel::UniquePtr<PausedState> outerState;
    if (reentrant)
        outerState = savePausedState(); // frames_ now empty; outer buffer parked

    // popCallFrame's top-level (frames_.size()==1) branch repopulates
    // lastVarMap_ from the callee's locals; snapshot+restore so the outer's
    // pending workspace export is not clobbered by this nested run.
    keel::Array<std::pair<keel::String, Value>> savedVarMap = keel::move(lastVarMap_);

    struct Restorer
    {
        VM &vm;
        bool reentrant = false;
        keel::UniquePtr<PausedState> st;
        keel::Array<std::pair<keel::String, Value>> vm_savedVarMap;
        bool done = false;
        void run()
        {
            if (done)
                return;
            done = true;
            vm.lastVarMap_ = keel::move(vm_savedVarMap);
            if (reentrant)
                vm.restorePausedState(keel::move(st));
        }
        ~Restorer() { run(); }
    } restorer{*this, reentrant, keel::move(outerState), keel::move(savedVarMap)};

    const keel::usize nout = std::max<keel::usize>(nargout, 1);
    const bool multi = nout > 1;
    keel::Array<Value> argbuf(args);

    returnBuf_.clear();
    lastResult_ = Value();
    pushCallFrame(chunk, argbuf.empty() ? nullptr : argbuf.data(),
                  argbuf.size(), /*destReg=*/0, nout,
                  /*isMulti=*/multi, /*outBase=*/0, /*nout=*/keel::as<keel::u16>(nout),
                  ownerClass, isCtor, ctorSeed);

    ExecStatus status = dispatchLoop();
    if (status == ExecStatus::Paused) {
        // A debugger pause cannot suspend across the C++ re-entry boundary.
        restorer.run(); // restore the outer before unwinding
        throw DebugStopException();
    }

    keel::Array<Value> results;
    if (!returnBuf_.empty()) {
        results = keel::move(returnBuf_);
        returnBuf_.clear();
    } else if (!lastResult_.isUnset()) {
        results.push_back(keel::move(lastResult_));
    }
    lastResult_ = Value();
    return results; // restorer dtor restores the outer state
}

// ── Paused state save/restore (for debug eval) ────────────

keel::UniquePtr<VM::PausedState> VM::savePausedState()
{
    for (keel::usize fi = 0; fi < frames_.size(); ++fi) {
        const auto &f = frames_[fi];
        if (!f.chunk) continue;
        exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, f.R, f.nregs,
                                         f.isScriptFrame);
    }
    auto s = keel::make_unique<PausedState>();
    // Move (not copy) — CallFrame holds a UniquePtr<Environment>. The
    // inner re-entrant execution will clear frames_ in startExecution()
    // anyway, so move semantics drop nothing we need.
    s->frames = keel::move(frames_);
    s->forStack = keel::move(forStack_);
    s->tryStack = keel::move(tryStack_);
    s->lvalDetaches = keel::move(lvalDetaches_);
    s->regStackTop = regStackTop_;
    s->lastResult = keel::move(lastResult_);
    // BUG #3: rescue chunkCallCache_ before startExecution clears it.
    // The outer dispatch loop holds a reference into one of the map's
    // value arrays; HashMap move preserves table buffer addresses, so
    // taking custody here keeps the outer's reference valid.
    s->chunkCallCache = keel::move(chunkCallCache_);
    // Snapshot only the used portion of the register stack
    s->regSnapshot.assign(keel::ArrayRef<Value>(regStack_.data(), regStackTop_));
    s->wasReentrantEval = inReentrantEval_;
    inReentrantEval_ = true;
    activePausedStates_.push_back(s.get());
    return s;
}

void VM::restorePausedState(keel::UniquePtr<PausedState> s)
{
    if (!s) return;
    for (keel::usize i = activePausedStates_.size(); i-- > 0;) {
        if (activePausedStates_[i] == s.get()) {
            activePausedStates_.remove_at(i);
            break;
        }
    }
    // The frames being replaced belong to an inner, abandoned run — drop
    // their origin pushes before the outer's frames (whose pushes are still
    // on the stack) become current again.
    if (!frames_.empty())
        engine_.truncateScriptOrigins(frames_.front().originDepth);
    inReentrantEval_ = s->wasReentrantEval;
    frames_ = keel::move(s->frames);
    forStack_ = keel::move(s->forStack);
    tryStack_ = keel::move(s->tryStack);
    lvalDetaches_ = keel::move(s->lvalDetaches);
    regStackTop_ = s->regStackTop;
    lastResult_ = keel::move(s->lastResult);
    // BUG #3: restore the outer's call-target cache. The inner's
    // cache (built up during re-entry) is destroyed by this move-assign;
    // outer's table buffers reclaim their original addresses, so the dispatch
    // loop's captured `resolvedFuncs` reference becomes live again.
    chunkCallCache_ = keel::move(s->chunkCallCache);
    // Restore registers (grow regStack_ first if needed)
    ensureRegStackCapacity(std::max(s->regSnapshot.size(), s->regStackTop));
    std::copy(s->regSnapshot.begin(), s->regSnapshot.end(), regStack_.begin());
    // Fix R pointers in frames (they pointed into regStack_)
    R_ = regStack_.data();
    for (keel::usize fi = 0; fi < frames_.size(); ++fi) {
        auto &f = frames_[fi];
        f.R = &regStack_[f.regBase];
        if (f.chunk) {
            importChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, f.R, f.nregs,
                                             f.isScriptFrame, /*seedMissing=*/false);
        }
    }
    // Fix ForState data pointers (they point into ForState::range).
    // Lazy ranges (FOR_INIT_RANGE) don't have a backing range Value —
    // the iteration value is recomputed from lazyStart/lazyStep each
    // step — so skip the pointer fixup. Calling doubleData() on a
    // default-constructed empty Value would throw because empty
    // MValues hold the emptyTag() sentinel rather than nullptr.
    for (auto &fs : forStack_) {
        if (fs.lazy)
            continue;
        if (fs.rangeType == ValueType::DOUBLE && fs.range.doubleData())
            fs.data = fs.range.doubleData();
        else
            fs.rawData = fs.range.rawData();
    }
}

// ── Debug-aware execution API ───────────────────────────────

ExecStatus VM::startExecution(const BytecodeChunk &chunk, const Value *args, keel::usize nargs,
                              DebugAction initialAction)
{
    chunkCallCache_.clear();
    if (!frames_.empty()) {
        engine_.truncateScriptOrigins(frames_.front().originDepth);
    }
    frames_.clear();
    forStack_.clear();
    tryStack_.clear();
    lvalDetaches_.clear();
    regStackTop_ = 0;
    returnBuf_.clear();
    lastResult_ = Value();
    atErrorPause_ = false;
    pausedError_ = nullptr;

    keel::u32 nregs = chunk.numRegisters;
    keel::usize pc = args ? std::min(nargs, keel::as<keel::usize>(chunk.numParams)) : 0;
    keel::Array<Value> argsCopy(pc);
    for (keel::usize i = 0; i < pc; ++i)
        argsCopy[i] = args[i];

    ensureRegStackCapacity(nregs);
    R_ = regStack_.data();

    for (keel::u32 i = 0; i < nregs; ++i)
        R_[i] = Value();

    for (keel::usize i = 0; i < pc; ++i)
        R_[i] = keel::move(argsCopy[i]);

    regStackTop_ = nregs;

    // Push initial call frame
    CallFrame cf;
    cf.chunk = &chunk;
    cf.ip = chunk.code.data();
    cf.R = R_;
    cf.regBase = 0;
    cf.nregs = nregs;
    cf.forStackBase = 0;
    cf.tryStackBase = 0;
    cf.isScriptFrame = true;
    cf.originDepth = engine_.scriptOriginDepth();
    // Scoped-eval read-side: caller's variable snapshot was attached
    // via setNextFrameDynVars before this startExecution call. Plug it
    // in so this top-level frame's ASSERT_DEF fallback resolves bare
    // identifiers from the caller's scope.
    if (nextFrameDynVars_) {
        cf.dynVars = nextFrameDynVars_;
        nextFrameDynVars_ = nullptr;  // single-shot
    }
    frames_.push_back(keel::move(cf));

    // Debug: push top-level debug frame
    if (auto *ctl = debugCtl()) {
        ctl->reset(initialAction);
        StackFrame sf;
        sf.functionName = chunk.name.clone();
        sf.chunk = &chunk;
        sf.registers = R_;
        ctl->pushFrame(keel::move(sf));
    }

    return dispatchLoop();
}

ExecStatus VM::resumeExecution()
{
    if (frames_.empty())
        return ExecStatus::Completed;

    // Resuming from a dbstop-if-error pause: the error was deferred, not
    // handled — let it propagate now (the debug session ends with the error).
    // Clean up the parked frames first, mirroring execute()'s guard.
    if (atErrorPause_) {
        atErrorPause_ = false;
        std::exception_ptr ep = pausedError_;
        pausedError_ = nullptr;
        if (!frames_.empty())
            engine_.truncateScriptOrigins(frames_.front().originDepth);
        frames_.clear();
        forStack_.clear();
        tryStack_.clear();
        regStackTop_ = 0;
        R_ = nullptr;
        if (ep)
            std::rethrow_exception(ep);
    }

    return dispatchLoop();
}

// ============================================================
// Internal dispatch — Value directly, scalar fast paths
// ============================================================

void VM::rollbackLvalDetaches()
{
    while (!lvalDetaches_.empty()) {
        LvalDetach d = keel::move(lvalDetaches_.back());
        lvalDetaches_.pop_back();
        if (d.containerRegAbs < regStack_.size() && d.valRegAbs < regStack_.size()) {
            Value &cont = regStack_[d.containerRegAbs];
            Value &val = regStack_[d.valRegAbs];
            if (d.kind == LvalDetach::Kind::Field) {
                if (cont.isStruct() && !cont.isStructArray())
                    cont.field(d.fieldName) = keel::move(val);
            } else {
                if (cont.isCell() && d.cellLinearIdx < cont.numel())
                    cont.cellAt(d.cellLinearIdx) = keel::move(val);
            }
        }
    }
}

// Forward declarations for helpers defined later in this translation unit
// but called from dispatchLoop's catch blocks.
static keel::String describeInstruction(const Instruction &instr,
                                        const BytecodeChunk &chunk);

ExecStatus VM::dispatchLoop()
{
enter_frame:
    if (frames_.empty())
        return ExecStatus::Completed;

    {
        CallFrame &frame = frames_.back();
        const Instruction *ip = frame.ip;
        const BytecodeChunk &chunk = *frame.chunk;
        const Instruction *end = chunk.code.data() + chunk.code.size();
        auto *R = frame.R;
        auto &resolvedFuncs = chunkCallCache_.get_or_insert(frame.chunk);

    try {
        auto *dbgCtl = debugCtl(); // hoist out of hot loop
        while (ip < end) {
            // ── Debug hook: check for line change, breakpoints ──
            if (dbgCtl) {
                keel::usize idx = keel::as<keel::usize>(ip - chunk.code.data());
                if (idx < chunk.sourceMap.size()) {
                    auto &loc = chunk.sourceMap[idx];
                    if (loc.line > 0) {
                        if (auto *f = dbgCtl->currentFrame())
                            f->registers = R;
                        if (!dbgCtl->checkLine(loc.line, loc.col, callDepth())) {
                            frame.ip = ip;
                            return ExecStatus::Paused;
                        }
                    }
                }
            }

            const Instruction &I = *ip;

            switch (I.op) {
            // ── Data movement ────────────────────────────────────
            case OpCode::LOAD_CONST: {
                const Value &cv = chunk.constants[I.d];
                if (cv.isDoubleScalar()) {
                    if (R[I.a].isDoubleScalar())
                        R[I.a].setScalarFast(cv.scalarVal());
                    else
                        R[I.a].setScalarVal(cv.scalarVal());
                } else {
                    R[I.a] = cv;
                }
                break;
            }
            case OpCode::LOAD_EMPTY:
                R[I.a] = Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_);
                break;
            case OpCode::LOAD_STRING:
                R[I.a] = Value::fromString(chunk.strings[I.d], engine_.mr_);
                break;
            case OpCode::MOVE:
                if (R[I.b].isDoubleScalar()) {
                    if (R[I.a].isDoubleScalar())
                        R[I.a].setScalarFast(R[I.b].scalarVal());
                    else
                        R[I.a].setScalarVal(R[I.b].scalarVal());
                } else {
                    R[I.a] = R[I.b];
                }
                break;
            case OpCode::COLLAPSE:
                // Collapse a CSL reaching a single-value sink (no-op on a non-CSL).
                // When I.c == 1 (statement-level sink to ans), bind the last CSL element
                // or defined value, leaving R[I.a] (`ans`) untouched when the statement
                // produced no output value (isUnset()/isDeleted()/0-element CSL).
                if (I.c == 1) {
                    if (R[I.b].isCsl()) {
                        keel::usize n = R[I.b].cslCount();
                        if (n > 0)
                            R[I.a] = R[I.b].cslAt(n - 1);
                    } else if (!R[I.b].isUnset() && !R[I.b].isDeleted()) {
                        if (I.a != I.b)
                            R[I.a] = R[I.b];
                    }
                    break;
                }
                R[I.a] = collapseCsl(std::move(R[I.b]));
                break;
            case OpCode::COLON_ALL:
                R[I.a] = Value::fromString(":", engine_.mr_);
                break;
            case OpCode::LOAD_END: {
                // a=dst, b=arrReg, c=dim (0-based), d=ndims
                const Value &arr = R[I.b];
                keel::i32 ndims = I.d;
                if (arr.isObject()) {
                    const BuiltinClass *cls = engine_.findClass(arr.objectClassNameRef());
                    if (cls) {
                        if (const auto *endFn = cls->methods.get("__end")) {
                            Value self = arr;
                            Value eargs[2] = {
                                Value::scalar(keel::as<keel::f64>(I.c), engine_.mr_),
                                Value::scalar(keel::as<keel::f64>(ndims), engine_.mr_)};
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*endFn)(self, keel::ArrayRef<Value>(eargs, 2), 1,
                                     keel::MutArrayRef<Value>(out, 1), ctx);
                            R[I.a] = std::move(out[0]);
                            break;
                        }
                    }
                }
                keel::usize sz;
                if (ndims <= 1) {
                    // Linear indexing: end = numel
                    sz = arr.numel();
                } else if (I.c == ndims - 1 && arr.dims().ndim() > keel::as<keel::i32>(ndims)) {
                    keel::usize prod = 1;
                    for (keel::i32 d = keel::as<keel::i32>(I.c); d < arr.dims().ndim(); ++d)
                        prod *= arr.dims().dimSize(d);
                    sz = prod;
                } else {
                    // Dimensional indexing: end = size along dimension c
                    sz = arr.dims().dimSize(I.c);
                }
                R[I.a] = Value::scalar(keel::as<keel::f64>(sz), engine_.mr_);
                break;
            }

            // ── Scalar arithmetic ────────────────────────────────
            case OpCode::ADD:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setScalarFast(asScalar(R[I.b]) + asScalar(R[I.c]));
                } else if (!tryInPlaceBinaryOp(R[I.a], OpCode::ADD, R[I.b], R[I.c])) {
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                }
                break;
            case OpCode::SUB:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setScalarFast(asScalar(R[I.b]) - asScalar(R[I.c]));
                } else if (!tryInPlaceBinaryOp(R[I.a], OpCode::SUB, R[I.b], R[I.c])) {
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                }
                break;
            case OpCode::MUL:  // matrix multiply — output reuse skipped (different shape rules)
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setScalarFast(asScalar(R[I.b]) * asScalar(R[I.c]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::EMUL:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setScalarFast(asScalar(R[I.b]) * asScalar(R[I.c]));
                } else if (!tryInPlaceBinaryOp(R[I.a], OpCode::EMUL, R[I.b], R[I.c])) {
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                }
                break;
            case OpCode::RDIV:  // matrix right divide — output reuse skipped
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setScalarFast(asScalar(R[I.b]) / asScalar(R[I.c]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::ERDIV:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setScalarFast(asScalar(R[I.b]) / asScalar(R[I.c]));
                } else if (!tryInPlaceBinaryOp(R[I.a], OpCode::ERDIV, R[I.b], R[I.c])) {
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                }
                break;
            case OpCode::LDIV:
            case OpCode::ELDIV:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setScalarFast(asScalar(R[I.c]) / asScalar(R[I.b]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::POW:
            case OpCode::EPOW:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    keel::f64 base = asScalar(R[I.b]);
                    keel::f64 exp = asScalar(R[I.c]);
                    if (base < 0.0 && exp != keel::floor(exp)) {
                        // negative base ^ non-integer exp -> complex; defer to power().
                        R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]);
                    } else {
                        keel::f64 result;
                        if (exp == 2.0)
                            result = base * base;
                        else if (exp == 3.0)
                            result = base * base * base;
                        else if (exp == 0.5)
                            result = keel::sqrt(base);
                        else if (exp == -1.0)
                            result = 1.0 / base;
                        else
                            result = keel::pow(base, exp);
                        R[I.a].setScalarFast(result);
                    }
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;

            // ── Scalar-specialized arithmetic (no type checks) ────
            case OpCode::ADD_SS:
                R[I.a].setScalarFast(R[I.b].scalarVal() + R[I.c].scalarVal());
                break;
            case OpCode::SUB_SS:
                R[I.a].setScalarFast(R[I.b].scalarVal() - R[I.c].scalarVal());
                break;
            case OpCode::MUL_SS:
                R[I.a].setScalarFast(R[I.b].scalarVal() * R[I.c].scalarVal());
                break;
            case OpCode::RDIV_SS:
                R[I.a].setScalarFast(R[I.b].scalarVal() / R[I.c].scalarVal());
                break;
            case OpCode::POW_SS: {
                keel::f64 base = R[I.b].scalarVal();
                keel::f64 exp = R[I.c].scalarVal();
                if (base < 0.0 && exp != keel::floor(exp)) {
                    // negative base ^ non-integer exp -> complex; defer to power().
                    R[I.a] = binarySlowPath(OpCode::EPOW, R[I.b], R[I.c]);
                    break;
                }
                keel::f64 result;
                if (exp == 2.0)
                    result = base * base;
                else if (exp == 3.0)
                    result = base * base * base;
                else if (exp == 0.5)
                    result = keel::sqrt(base);
                else if (exp == -1.0)
                    result = 1.0 / base;
                else
                    result = keel::pow(base, exp);
                R[I.a].setScalarFast(result);
                break;
            }
            case OpCode::NEG_S:
                R[I.a].setScalarFast(-R[I.b].scalarVal());
                break;

            // ── Fused multiply-add/sub (loop opt #2) ──────────────
            // Fast path: all-scalar, two-step double (prod = c*e; a = b ± prod)
            // — bit-identical to the unfused MUL+ADD/SUB. Else fall back to the
            // matching product (matmul / elementwise) then sum/diff via the slow
            // path (handles arrays / complex / object operator overloads). The
            // product double is read before the dst write, so dst==src aliasing
            // is safe.
            case OpCode::MULADD:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c]) && isArithScalar(R[I.e])) {
                    keel::f64 prod = asScalar(R[I.c]) * asScalar(R[I.e]);
                    R[I.a].setScalarFast(asScalar(R[I.b]) + prod);
                } else {
                    Value prod = binarySlowPath(OpCode::MUL, R[I.c], R[I.e]);
                    R[I.a] = binarySlowPath(OpCode::ADD, R[I.b], prod);
                }
                break;
            case OpCode::MULSUB:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c]) && isArithScalar(R[I.e])) {
                    keel::f64 prod = asScalar(R[I.c]) * asScalar(R[I.e]);
                    R[I.a].setScalarFast(asScalar(R[I.b]) - prod);
                } else {
                    Value prod = binarySlowPath(OpCode::MUL, R[I.c], R[I.e]);
                    R[I.a] = binarySlowPath(OpCode::SUB, R[I.b], prod);
                }
                break;
            case OpCode::EMULADD:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c]) && isArithScalar(R[I.e])) {
                    keel::f64 prod = asScalar(R[I.c]) * asScalar(R[I.e]);
                    R[I.a].setScalarFast(asScalar(R[I.b]) + prod);
                } else {
                    Value prod = binarySlowPath(OpCode::EMUL, R[I.c], R[I.e]);
                    R[I.a] = binarySlowPath(OpCode::ADD, R[I.b], prod);
                }
                break;
            case OpCode::EMULSUB:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c]) && isArithScalar(R[I.e])) {
                    keel::f64 prod = asScalar(R[I.c]) * asScalar(R[I.e]);
                    R[I.a].setScalarFast(asScalar(R[I.b]) - prod);
                } else {
                    Value prod = binarySlowPath(OpCode::EMUL, R[I.c], R[I.e]);
                    R[I.a] = binarySlowPath(OpCode::SUB, R[I.b], prod);
                }
                break;

            // ── Comparison ───────────────────────────────────────
            case OpCode::EQ:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setLogicalFast(asScalar(R[I.b]) == asScalar(R[I.c]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::NE:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setLogicalFast(asScalar(R[I.b]) != asScalar(R[I.c]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::LT:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setLogicalFast(asScalar(R[I.b]) < asScalar(R[I.c]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::GT:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setLogicalFast(asScalar(R[I.b]) > asScalar(R[I.c]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::LE:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setLogicalFast(asScalar(R[I.b]) <= asScalar(R[I.c]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::GE:
                if (isArithScalar(R[I.b]) && isArithScalar(R[I.c])) {
                    R[I.a].setLogicalFast(asScalar(R[I.b]) >= asScalar(R[I.c]));
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::AND:
                if (R[I.b].isDoubleScalar() && R[I.c].isDoubleScalar()) {
                    keel::f64 bv = R[I.b].scalarVal(), cv = R[I.c].scalarVal();
                    if (keel::is_nan(bv) || keel::is_nan(cv))
                        throw std::runtime_error("NaN values cannot be converted to logicals.");
                    R[I.a].setLogicalFast(bv != 0.0 && cv != 0.0);
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::OR:
                if (R[I.b].isDoubleScalar() && R[I.c].isDoubleScalar()) {
                    keel::f64 bv = R[I.b].scalarVal(), cv = R[I.c].scalarVal();
                    if (keel::is_nan(bv) || keel::is_nan(cv))
                        throw std::runtime_error("NaN values cannot be converted to logicals.");
                    R[I.a].setLogicalFast(bv != 0.0 || cv != 0.0);
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;

            // ── Unary ────────────────────────────────────────────
            case OpCode::NEG:
                if (R[I.b].isDoubleScalar()) {
                    R[I.a].setScalarFast(-R[I.b].scalarVal());
                } else
                    { if (tryUnaryOpFrame(I.a, I.op, I.b, frame, ip)) goto enter_frame;
                          R[I.a] = unarySlowPath(I.op, R[I.b]); }
                break;
            case OpCode::UPLUS:
                R[I.a] = R[I.b];
                break;
            case OpCode::NOT:
                if (R[I.b].isDoubleScalar()) {
                    keel::f64 bv = R[I.b].scalarVal();
                    if (keel::is_nan(bv))
                        throw std::runtime_error("NaN values cannot be converted to logicals.");
                    R[I.a].setLogicalFast(bv == 0.0);
                } else
                    { if (tryUnaryOpFrame(I.a, I.op, I.b, frame, ip)) goto enter_frame;
                          R[I.a] = unarySlowPath(I.op, R[I.b]); }
                break;
            case OpCode::TO_LOGICAL_SCALAR: {
                const Value &src = R[I.b];
                if (!src.isScalar() || src.isEmpty() || src.isUnset()) {
                    throw std::runtime_error(
                        "Operands to the logical AND (&&) and OR (||) operators must be convertible to logical scalar values.");
                }
                R[I.a].setLogicalFast(src.toBool());
                break;
            }
            case OpCode::CTRANSPOSE:
            case OpCode::TRANSPOSE:
                if (R[I.b].isDoubleScalar()) {
                    R[I.a] = R[I.b];
                } else
                    { if (tryUnaryOpFrame(I.a, I.op, I.b, frame, ip)) goto enter_frame;
                          R[I.a] = unarySlowPath(I.op, R[I.b]); }
                break;

            // ── Fused element-wise idiom ─────────────────────────
            case OpCode::FUSE_EWISE: {
                // Gather the contiguous operand register block and run the
                // kernel; on success write dst and skip the `e` fallback
                // instructions, else fall through to them (the normally
                // compiled idiom). The runtime gate lets setFusion(false)
                // disable fusion even for already-compiled bytecode.
                if (engine_.fusionEnabled()) {
                    const FusionRule &rule =
                        engine_.fusionRules()[keel::as<keel::usize>(I.d)];
                    Value fout;
                    if (rule.execute(&R[I.b], I.c, fout, engine_.mr_)) {
                        engine_.noteFusionHit();
                        R[I.a] = std::move(fout);
                        ip += keel::as<keel::i32>(I.e) + 1;
                        continue;
                    }
                }
                break;  // disabled or declined → execute the fallback opcodes
            }

            // ── Control flow ─────────────────────────────────────
            case OpCode::JMP:
                ip += I.d;
                continue;
            case OpCode::JMP_TRUE:
                if (R[I.a].isLogicalScalar()) {
                    if (R[I.a].isLogicalTrue()) {
                        ip += I.d;
                        continue;
                    }
                    break;
                }
                if (R[I.a].isEmpty() && !R[I.a].isUnset()
                    && (R[I.a].isNumeric() || R[I.a].isLogical() || R[I.a].isChar())) {
                    break;
                }
                if (R[I.a].toBool()) {
                    ip += I.d;
                    continue;
                }
                break;
            case OpCode::JMP_FALSE:
                if (R[I.a].isLogicalScalar()) {
                    if (R[I.a].isLogicalFalse()) {
                        ip += I.d;
                        continue;
                    }
                    break;
                }
                if (R[I.a].isEmpty() && !R[I.a].isUnset()
                    && (R[I.a].isNumeric() || R[I.a].isLogical() || R[I.a].isChar())) {
                    ip += I.d;
                    continue;
                }
                if (!R[I.a].toBool()) {
                    ip += I.d;
                    continue;
                }
                break;

            // ── For-loop ─────────────────────────────────────────
            case OpCode::FOR_INIT: {
                ForState fs;
                fs.index = 0;
                if (R[I.b].isDoubleScalar()) {
                    fs.range = R[I.b];
                    fs.count = 1;
                    fs.rows = 0;
                    fs.data = nullptr;
                    fs.rangeType = ValueType::DOUBLE;
                } else if (R[I.b].isEmpty()) {
                    fs.count = 0;
                } else {
                    fs.range = R[I.b];
                    fs.rangeType = fs.range.type();
                    const auto &dims = fs.range.dims();
                    fs.rows = dims.rows();
                    fs.count = (fs.rows > 0) ? (dims.numel() / fs.rows) : 0;
                    if (fs.rangeType == ValueType::DOUBLE)
                        fs.data = fs.range.doubleData();
                }
                if (fs.count == 0) { R[I.a] = Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_);
                    ip += I.d;
                    continue;
                }
                forStack_.push_back(std::move(fs));
                forSetVar(R[I.a], forStack_.back());
                break;
            }
            case OpCode::FOR_INIT_RANGE: {
                // Fused `for v = start:stop` / `for v = start:step:stop`.
                // No COLON allocation when all operands are DOUBLE: start/step/count
                // live in the ForState, the loop var is recomputed as start + index*step
                // each iteration. Non-double or object operands materialize the typed
                // colon range so char/integer/custom-class loops match COLON semantics.
                auto colonScalar = [](const Value &v) -> keel::f64 {
                    return (v.numel() > 1 && v.isNumeric()) ? v.elemAsDouble(0) : v.toScalar();
                };
                const bool allDouble = (R[I.b].type() == ValueType::DOUBLE)
                                       && (R[I.c].type() == ValueType::DOUBLE)
                                       && (I.e == kInvalidReg || R[I.e].type() == ValueType::DOUBLE);
                if (!allDouble) {
                    Value rangeVal;
                    if (I.e == kInvalidReg) {
                        if (R[I.b].isObject() || R[I.c].isObject()) {
                            const Value &dom = R[I.b].isObject() ? R[I.b] : R[I.c];
                            const BuiltinClass *cls = engine_.findClass(dom.objectClassNameRef());
                            if (cls) {
                                if (const auto *colonOp = cls->ops.get("colon")) {
                                    Value self = dom;
                                    Value ops[2] = {R[I.b], R[I.c]};
                                    Value out[1];
                                    CallContext ctx{&engine_, currentCallEnv()};
                                    (*colonOp)(self, keel::ArrayRef<Value>(ops, 2), 1,
                                               keel::MutArrayRef<Value>(out, 1), ctx);
                                    rangeVal = std::move(out[0]);
                                }
                            }
                        }
                        if (rangeVal.isUnset()) {
                            ValueType ta = R[I.b].type(), tb = R[I.c].type();
                            ValueType t = (ta != ValueType::DOUBLE) ? ta
                                        : (tb != ValueType::DOUBLE) ? tb
                                        : ValueType::DOUBLE;
                            if (ta != ValueType::DOUBLE && tb != ValueType::DOUBLE && ta != tb)
                                throw std::runtime_error(
                                    "Colon operands must be all the same type, "
                                    "or mixed with real scalar doubles");
                            if (R[I.b].isEmpty() || R[I.c].isEmpty())
                                rangeVal = Value::matrix(1, 0, t, engine_.mr_);
                            else
                                rangeVal = Value::colonRangeTyped(colonScalar(R[I.b]),
                                                                  colonScalar(R[I.c]), t,
                                                                  engine_.mr_);
                        }
                    } else {
                        if (R[I.b].isObject() || R[I.e].isObject() || R[I.c].isObject()) {
                            const Value &dom = R[I.b].isObject() ? R[I.b]
                                             : (R[I.c].isObject() ? R[I.c] : R[I.e]);
                            const BuiltinClass *cls = engine_.findClass(dom.objectClassNameRef());
                            if (cls) {
                                if (const auto *colonOp = cls->ops.get("colon")) {
                                    Value self = dom;
                                    Value ops[3] = {R[I.b], R[I.e], R[I.c]};
                                    Value out[1];
                                    CallContext ctx{&engine_, currentCallEnv()};
                                    (*colonOp)(self, keel::ArrayRef<Value>(ops, 3), 1,
                                               keel::MutArrayRef<Value>(out, 1), ctx);
                                    rangeVal = std::move(out[0]);
                                }
                            }
                        }
                        if (rangeVal.isUnset()) {
                            ValueType ts[3] = {R[I.b].type(), R[I.e].type(), R[I.c].type()};
                            ValueType t = ValueType::DOUBLE;
                            bool found = false;
                            for (ValueType ti : ts) {
                                if (ti == ValueType::DOUBLE) continue;
                                if (!found) { t = ti; found = true; }
                                else if (ti != t)
                                    throw std::runtime_error(
                                        "Colon operands must be all the same type, "
                                        "or mixed with real scalar doubles");
                            }
                            if (R[I.b].isEmpty() || R[I.e].isEmpty() || R[I.c].isEmpty())
                                rangeVal = Value::matrix(1, 0, t, engine_.mr_);
                            else
                                rangeVal = Value::colonRangeTyped(
                                    colonScalar(R[I.b]), colonScalar(R[I.e]),
                                    colonScalar(R[I.c]), t, engine_.mr_);
                        }
                    }
                    ForState fs;
                    fs.index = 0;
                    if (!rangeVal.isEmpty()) {
                        fs.range = std::move(rangeVal);
                        fs.rangeType = fs.range.type();
                        const auto &dims = fs.range.dims();
                        fs.rows = dims.rows();
                        fs.count = (fs.rows > 0) ? (dims.numel() / fs.rows) : 0;
                        if (fs.rangeType == ValueType::DOUBLE)
                            fs.data = fs.range.doubleData();
                    }
                    if (fs.count == 0) {
                        R[I.a] = Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_);
                        ip += I.d;
                        continue;
                    }
                    forStack_.push_back(std::move(fs));
                    forSetVar(R[I.a], forStack_.back());
                    break;
                }
                if (R[I.b].isEmpty() || R[I.c].isEmpty()
                    || (I.e != kInvalidReg && R[I.e].isEmpty())) { R[I.a] = Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_);
                    ip += I.d;
                    continue;
                }
                const keel::f64 start = colonScalar(R[I.b]);
                const keel::f64 stop  = colonScalar(R[I.c]);
                const keel::f64 step  = (I.e == kInvalidReg) ? 1.0
                                                          : colonScalar(R[I.e]);
                const keel::usize count = Value::colonCount(start, step, stop);
                if (count == 0) { R[I.a] = Value::matrix(0, 0, ValueType::DOUBLE, engine_.mr_);
                    ip += I.d;
                    continue;
                }
                ForState fs;
                fs.index = 0;
                fs.count = count;
                fs.rows  = 1;
                fs.rangeType = ValueType::DOUBLE;
                fs.lazy = true;
                fs.lazyStart = start;
                fs.lazyStep  = step;
                forStack_.push_back(std::move(fs));
                forSetVar(R[I.a], forStack_.back());
                break;
            }
            case OpCode::FOR_NEXT: {
                auto &fs = forStack_.back();
                fs.index++;
                if (fs.index < fs.count) {
                    forSetVar(R[I.a], fs);
                    ip += I.d;
                    continue;
                }
                forStack_.pop_back();
                break;
            }

            // ── Colon ────────────────────────────────────────────
            // Type rule (matches MATLAB + tree_walker.cpp:colonOutputType):
            //   * all double → DOUBLE
            //   * one non-double type T (others double) → T
            //   * two different non-double types → throw
            case OpCode::COLON: {
                if (R[I.b].isObject() || R[I.c].isObject()) {
                    const Value &dom = R[I.b].isObject() ? R[I.b] : R[I.c];
                    const BuiltinClass *cls = engine_.findClass(dom.objectClassNameRef());
                    if (cls) {
                        if (const auto *colonOp = cls->ops.get("colon")) {
                            Value self = dom;
                            Value ops[2] = {R[I.b], R[I.c]};
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*colonOp)(self, keel::ArrayRef<Value>(ops, 2), 1,
                                       keel::MutArrayRef<Value>(out, 1), ctx);
                            R[I.a] = std::move(out[0]);
                            break;
                        }
                    }
                }
                ValueType ta = R[I.b].type(), tb = R[I.c].type();
                ValueType t = (ta != ValueType::DOUBLE) ? ta
                            : (tb != ValueType::DOUBLE) ? tb
                            : ValueType::DOUBLE;
                if (ta != ValueType::DOUBLE && tb != ValueType::DOUBLE && ta != tb)
                    throw std::runtime_error(
                        "Colon operands must be all the same type, "
                        "or mixed with real scalar doubles");
                if (R[I.b].isEmpty() || R[I.c].isEmpty()) {
                    R[I.a] = Value::matrix(1, 0, t, engine_.mr_);
                    break;
                }
                auto colonScalar = [](const Value &v) -> keel::f64 {
                    return (v.numel() > 1 && v.isNumeric()) ? v.elemAsDouble(0) : v.toScalar();
                };
                keel::f64 start = colonScalar(R[I.b]), stop = colonScalar(R[I.c]);
                R[I.a] = Value::colonRangeTyped(start, stop, t, engine_.mr_);
                break;
            }
            case OpCode::COLON3: {
                if (R[I.b].isObject() || R[I.c].isObject() || R[I.e].isObject()) {
                    const Value &dom = R[I.b].isObject() ? R[I.b]
                                     : (R[I.e].isObject() ? R[I.e] : R[I.c]);
                    const BuiltinClass *cls = engine_.findClass(dom.objectClassNameRef());
                    if (cls) {
                        if (const auto *colonOp = cls->ops.get("colon")) {
                            Value self = dom;
                            Value ops[3] = {R[I.b], R[I.c], R[I.e]};
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*colonOp)(self, keel::ArrayRef<Value>(ops, 3), 1,
                                       keel::MutArrayRef<Value>(out, 1), ctx);
                            R[I.a] = std::move(out[0]);
                            break;
                        }
                    }
                }
                ValueType ta = R[I.b].type(), tb = R[I.c].type(), tc = R[I.e].type();
                ValueType t = ValueType::DOUBLE;
                ValueType nondefs[3] = {ta, tb, tc};
                for (keel::i32 k = 0; k < 3; ++k) {
                    if (nondefs[k] == ValueType::DOUBLE) continue;
                    if (t == ValueType::DOUBLE) t = nondefs[k];
                    else if (t != nondefs[k])
                        throw std::runtime_error(
                            "Colon operands must be all the same type, "
                            "or mixed with real scalar doubles");
                }
                if (R[I.b].isEmpty() || R[I.c].isEmpty() || R[I.e].isEmpty()) {
                    R[I.a] = Value::matrix(1, 0, t, engine_.mr_);
                    break;
                }
                auto colonScalar = [](const Value &v) -> keel::f64 {
                    return (v.numel() > 1 && v.isNumeric()) ? v.elemAsDouble(0) : v.toScalar();
                };
                keel::f64 start = colonScalar(R[I.b]), step = colonScalar(R[I.c]),
                       stop = colonScalar(R[I.e]);
                R[I.a] = Value::colonRangeTyped(start, step, stop, t, engine_.mr_);
                break;
            }

            // ── Array construction ───────────────────────────────
            case OpCode::HORZCAT: {
                if (I.c == 1) {
                    R[I.a] = R[I.b];
                    break;
                }
                bool handled = false;
                for (keel::u16 k = 0; k < I.c; ++k) {
                    if (R[I.b + k].isObject()) {
                        const BuiltinClass *cls = engine_.findClass(R[I.b + k].objectClassNameRef());
                        if (cls) {
                            if (const auto *catOp = cls->ops.get("horzcat")) {
                                Value self = R[I.b];
                                Value out[1];
                                CallContext ctx{&engine_, currentCallEnv()};
                                (*catOp)(self, keel::ArrayRef<Value>(&R[I.b + 1], I.c - 1), 1,
                                         keel::MutArrayRef<Value>(out, 1), ctx);
                                R[I.a] = std::move(out[0]);
                                handled = true;
                            }
                        }
                        break;
                    }
                }
                if (!handled)
                    R[I.a] = Value::horzcat(&R[I.b], I.c, engine_.mr_);
                break;
            }
            case OpCode::HORZCAT_APPEND: {
                Value &dst = R[I.a];
                const Value &val = R[I.b];
                if ((val.isDoubleScalar() || (val.isHeapDouble() && val.numel() == 1))
                    && dst.heapRefCount() == 1
                    && (dst.isEmpty()
                        || (dst.isHeapDouble() && dst.dims().rows() == 1))) {
                    dst.appendScalar(val.toScalar(), engine_.mr_);
                    break;
                }
                Value elems[2] = { dst, val };
                if (dst.isObject() || val.isObject()) {
                    const Value &obj = dst.isObject() ? dst : val;
                    const BuiltinClass *cls = engine_.findClass(obj.objectClassNameRef());
                    if (cls) {
                        if (const auto *catOp = cls->ops.get("horzcat")) {
                            Value self = dst;
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*catOp)(self, keel::ArrayRef<Value>(&elems[1], 1), 1,
                                     keel::MutArrayRef<Value>(out, 1), ctx);
                            R[I.a] = std::move(out[0]);
                            break;
                        }
                    }
                }
                R[I.a] = Value::horzcat(elems, 2, engine_.mr_);
                break;
            }
            case OpCode::VERTCAT: {
                if (I.c == 1) {
                    R[I.a] = R[I.b];
                    break;
                }
                bool handled = false;
                for (keel::u16 k = 0; k < I.c; ++k) {
                    if (R[I.b + k].isObject()) {
                        const BuiltinClass *cls = engine_.findClass(R[I.b + k].objectClassNameRef());
                        if (cls) {
                            if (const auto *catOp = cls->ops.get("vertcat")) {
                                Value self = R[I.b];
                                Value out[1];
                                CallContext ctx{&engine_, currentCallEnv()};
                                (*catOp)(self, keel::ArrayRef<Value>(&R[I.b + 1], I.c - 1), 1,
                                         keel::MutArrayRef<Value>(out, 1), ctx);
                                R[I.a] = std::move(out[0]);
                                handled = true;
                            }
                        }
                        break;
                    }
                }
                if (!handled)
                    R[I.a] = Value::vertcat(&R[I.b], I.c, engine_.mr_);
                break;
            }
            case OpCode::VERTCAT_APPEND: {
                // dst = [dst; val] — vertical twin of HORZCAT_APPEND:
                // appendScalarCol fast path when dst is an (or empty)
                // uniquely-owned column and val a real scalar; generic
                // two-element vertcat otherwise.
                Value &dst = R[I.a];
                const Value &val = R[I.b];
                if ((val.isDoubleScalar() || (val.isHeapDouble() && val.numel() == 1))
                    && dst.heapRefCount() == 1
                    && (dst.isEmpty()
                        || (dst.isHeapDouble() && dst.dims().cols() == 1))) {
                    dst.appendScalarCol(val.toScalar(), engine_.mr_);
                    break;
                }
                Value elems[2] = { dst, val };
                if (dst.isObject() || val.isObject()) {
                    const Value &obj = dst.isObject() ? dst : val;
                    const BuiltinClass *cls = engine_.findClass(obj.objectClassNameRef());
                    if (cls) {
                        if (const auto *catOp = cls->ops.get("vertcat")) {
                            Value self = dst;
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*catOp)(self, keel::ArrayRef<Value>(&elems[1], 1), 1,
                                     keel::MutArrayRef<Value>(out, 1), ctx);
                            R[I.a] = std::move(out[0]);
                            break;
                        }
                    }
                }
                R[I.a] = Value::vertcat(elems, 2, engine_.mr_);
                break;
            }
            case OpCode::HORZCAT_APPEND_CSL: {
                // a = dst (in/out), b = struct array source, d = nameIdx.
                // Expand `[dst, src(0).f, ..., src(N-1).f]` in one
                // horzcat to avoid quadratic growth.
                const keel::String &fname = chunk.strings[I.d];
                Value &dst = R[I.a];
                const Value &src = R[I.b];
                keel::Array<Value> elems;
                elems.push_back(dst);
                if (src.isStruct()) {
                    keel::usize n = src.numel();
                    if (n == 0 && !src.hasField(fname))
                        throw std::runtime_error(
                            keel::format("Reference to non-existent field '{}'", fname).c_str());
                    elems.reserve(n + 1);
                    for (keel::usize i = 0; i < n; ++i) {
                        const auto &m = src.structArrayElem(i);
                        const Value *vp = m.get(fname);
                        if (!vp)
                            throw std::runtime_error(
                                keel::format("Reference to non-existent field '{}'", fname).c_str());
                        elems.push_back(*vp);
                    }
                } else if (src.isObject()) {
                    // OBJECT array CSL: [arr.prop] expands prop over each
                    // element via the class propGet hook.
                    const BuiltinClass *cls = engine_.findClass(src.objectClassNameRef());
                    keel::usize n = src.objectCount();
                    elems.reserve(n + 1);
                    CallContext ctx{&engine_, currentCallEnv()};
                    for (keel::usize i = 0; i < n; ++i) {
                        keel::usize idx = i;
                        Value elem = src.objectSubArray(keel::ArrayRef<keel::usize>(&idx, 1), engine_.mr_);
                        Value out;
                        if (!cls || !cls->propGet || !cls->propGet(elem, fname, out, ctx))
                            throw std::runtime_error(
                                keel::format("No appropriate property '{}' for class '{}'",
                                             fname, src.objectClassNameRef())
                                    .c_str());
                        elems.push_back(keel::move(out));
                    }
                } else {
                    throw std::runtime_error(
                        "Comma-separated list expansion needs a struct or object");
                }
                dst = Value::horzcat(elems.data(), elems.size(), engine_.mr_);
                break;
            }
            case OpCode::HORZCAT_APPEND_FLATTEN: {
                // a=dst (in/out), b=elem. First-class concat builder: if R[elem] is a
                // comma-separated list, horzcat-append each item; else append the one
                // value. Driven by the runtime value, so [a, c{idx}, b] flattens for any
                // subscript. See dev-docs/memory/csl_first_class.md.
                Value             &dst  = R[I.a];
                const Value       &elem = R[I.b];
                keel::Array<Value> elems;
                elems.push_back(dst);
                if (elem.isCsl())
                    for (keel::usize k = 0; k < elem.cslCount(); ++k)
                        elems.push_back(elem.cslAt(k));
                else
                    elems.push_back(elem);
                dst = Value::horzcat(elems.data(), elems.size(), engine_.mr_);
                break;
            }

            // ── Array indexing ───────────────────────────────────
            case OpCode::INDEX_GET: {
                const Value &mv = R[I.b];
                const Value &ix = R[I.c];
                // Hot path: heap double indexed by a scalar double -- the
                // dominant `x(n)` read in scalar loops. Skips the object/cell/
                // scalar branch chain and elemAt's internal type-switch; reads
                // straight into an inline scalar register. (Argument is read
                // before the write, so a dst==src register alias is safe.)
                if (mv.isHeapDouble() && ix.isDoubleScalar()) {
                    keel::usize i = checkedIndex(ix.scalarVal(), mv.heapDims().numel());
                    R[I.a].setScalarFast(mv.doubleDataFast()[i]);
                    break;
                }
                if (mv.isDoubleScalar() && ix.isDoubleScalar()) {
                    checkedIndex(ix.scalarVal(), 1);
                    R[I.a].setScalarFast(mv.scalarVal());
                    break;
                }
                // OBJECT: obj(i) dispatches to the class subsref overload.
                if (mv.isObject()) {
                    Value idxArgs[1] = {ix};
                    // classdef subsref → same-stack VM frame (pausable, P4)
                    if (tryObjectSubsrefFrame(I.a, I.b, keel::ArrayRef<Value>(idxArgs, 1), frame, ip))
                        goto enter_frame;
                    Value out;
                    if (engine_.tryObjectSubsref(R[I.b], keel::ArrayRef<Value>(idxArgs, 1), 1,
                                                 out, currentCallEnv())) {
                        R[I.a] = std::move(out);
                        break;
                    }
                    // No custom subsref → builtin object-array indexing.
                    auto idxs = Value::resolveIndices(ix, mv.objectCount());
                    R[I.a] = mv.objectSubArray(idxs, engine_.mr_);
                    break;
                }
                if (mv.isCell()) {
                    // Cell () indexing always returns sub-cell
                    auto indices = Value::resolveIndices(ix, mv.numel());
                    R[I.a] = mv.indexGet(indices.data(), indices.size(),
                                         engine_.mr_);
                } else if (mv.isScalar() && ix.isDoubleScalar()) {
                    checkedIndex(ix.scalarVal(), 1); // validate bounds
                    R[I.a] = mv;
                } else if (ix.isDoubleScalar()) {
                    keel::usize i = checkedIndex(ix.scalarVal(), mv.numel());
                    R[I.a] = mv.elemAt(i, engine_.mr_);
                } else if (ix.isLogical()) {
                    R[I.a] = mv.logicalIndex(ix.logicalData(), ix.numel(),
                                             engine_.mr_);
                } else {
                    auto indices = Value::resolveIndices(ix, mv.numel());
                    Value res = mv.indexGet(indices.data(), indices.size(), engine_.mr_);
                    const bool mvIsNonScalarVec = !mv.dims().is3D() && mv.dims().ndim() == 2
                        && ((mv.dims().rows() == 1 && mv.dims().cols() > 1)
                            || (mv.dims().cols() == 1 && mv.dims().rows() > 1));
                    const bool ixIsVec = !ix.dims().is3D() && ix.dims().ndim() == 2
                        && (ix.dims().rows() == 1 || ix.dims().cols() == 1);
                    if (indices.size() > 1 && (!mvIsNonScalarVec || !ixIsVec)) {
                        if (res.dims().rows() != ix.dims().rows() || res.dims().cols() != ix.dims().cols())
                            res = res.reshape(ix.dims().rows(), ix.dims().cols(), engine_.mr_);
                    }
                    R[I.a] = std::move(res);
                }
                break;
            }
            case OpCode::INDEX_GET_2D: {
                const Value &mv = R[I.b];
                // OBJECT: a custom subsref controls indexing; otherwise the
                // builtin 2-D path below (indexGet2D) handles object arrays.
                if (mv.isObject()) {
                    Value idxArgs[2] = {R[I.c], R[I.e]};
                    // classdef subsref → same-stack VM frame (pausable, P4)
                    if (tryObjectSubsrefFrame(I.a, I.b, keel::ArrayRef<Value>(idxArgs, 2), frame, ip))
                        goto enter_frame;
                    Value out;
                    if (engine_.tryObjectSubsref(R[I.b], keel::ArrayRef<Value>(idxArgs, 2), 1,
                                                 out, currentCallEnv())) {
                        R[I.a] = std::move(out);
                        break;
                    }
                }
                // ── Scalar fast path: A(i,j) with scalar double indices ──
                if (R[I.c].isDoubleScalar() && R[I.e].isDoubleScalar()
                    && mv.isHeapDouble() && mv.heapDims().ndim() <= 2) {
                    const auto &hd = mv.heapDims();
                    keel::usize r = Value::checkedScalarIndex(R[I.c].scalarVal());
                    keel::usize c = Value::checkedScalarIndex(R[I.e].scalarVal());
                    if (r >= hd.rows() || c >= hd.cols())
                        throw std::runtime_error("Index exceeds array dimensions");
                    R[I.a].setScalarFast(mv.doubleDataFast()[hd.sub2ind(r, c)]);
                    break;
                }
                auto rowIds = ResolvedSubscript::resolve(R[I.c], mv.dims().rows());
                keel::usize colDim = mv.dims().ndim() > 2
                    ? (mv.dims().rows() > 0 ? mv.dims().numel() / mv.dims().rows() : mv.dims().cols())
                    : mv.dims().cols();
                auto colIds = ResolvedSubscript::resolve(R[I.e], colDim);
                R[I.a] = mv.indexGet2D(rowIds.data(), rowIds.count,
                                        colIds.data(), colIds.count,
                                        engine_.mr_);
                break;
            }
            case OpCode::INDEX_SET: {
                const Value &ix = R[I.b];
                if (R[I.a].isHeapDouble() && R[I.a].heapRefCount() == 1
                    && ix.isDoubleScalar() && R[I.c].isDoubleScalar()) {
                    keel::usize i = checkedIndex(ix.scalarVal(), SIZE_MAX);
                    if (i < R[I.a].heapDims().numel()) {
                        R[I.a].doubleDataMutFast()[i] = R[I.c].scalarVal();
                        break;
                    }
                }
                if (execIndexSet1DSlow(I, R, frame, ip))
                    goto enter_frame;
                break;
            }
            case OpCode::INDEX_SET_2D: {
                const Value &ri = R[I.b];
                const Value &ci = R[I.c];
                const Value &val = R[I.e];
                if (ri.isDoubleScalar() && ci.isDoubleScalar()
                    && val.isDoubleScalar() && R[I.a].isHeapDouble()
                    && R[I.a].heapDims().ndim() <= 2) {
                    keel::usize r = Value::checkedScalarIndex(ri.scalarVal());
                    keel::usize c = Value::checkedScalarIndex(ci.scalarVal());
                    const auto &d = R[I.a].heapDims();
                    if (r < d.rows() && c < d.cols()) {
                        R[I.a].doubleDataMutFast()[d.sub2ind(r, c)] = val.scalarVal();
                        break;
                    }
                    keel::usize newR = std::max(d.rows(), r + 1);
                    keel::usize newC = std::max(d.cols(), c + 1);
                    R[I.a].resize(newR, newC, engine_.mr_);
                    R[I.a].doubleDataMutFast()[R[I.a].heapDims().sub2ind(r, c)] = val.scalarVal();
                    break;
                }
                if (execIndexSet2DSlow(I, R, frame, ip))
                    goto enter_frame;
                break;
            }

            // ── ND array/cell indexing (3D+) ─────────────────────
            case OpCode::INDEX_GET_ND: {
                // a=dst, b=arr/cell, c=base, e=ndims
                keel::u16 base = I.c, ndims = I.e;
                const Value &mv = R[I.b];
                // OBJECT: a custom subsref controls indexing; otherwise the
                // builtin N-D path below (indexGet3D/indexGetND) handles it.
                if (mv.isObject()) {
                    keel::Array<Value> idx(ndims);
                    for (keel::u16 i = 0; i < ndims; ++i)
                        idx[i] = R[base + i];
                    // classdef subsref → same-stack VM frame (pausable, P4)
                    if (tryObjectSubsrefFrame(I.a, I.b, keel::ArrayRef<Value>(idx.data(), ndims),
                                              frame, ip))
                        goto enter_frame;
                    Value out;
                    if (engine_.tryObjectSubsref(R[I.b], keel::ArrayRef<Value>(idx.data(), ndims),
                                                 1, out, currentCallEnv())) {
                        R[I.a] = keel::move(out);
                        break;
                    }
                }
                if (ndims == 3) {
                    auto rowIds = ResolvedSubscript::resolve(R[base], mv.dims().rows());
                    auto colIds = ResolvedSubscript::resolve(R[base + 1], mv.dims().cols());
                    auto pageIds = ResolvedSubscript::resolve(R[base + 2], mv.dims().pages());
                    R[I.a] = mv.indexGet3D(rowIds.data(), rowIds.count,
                                           colIds.data(), colIds.count,
                                           pageIds.data(), pageIds.count,
                                           engine_.mr_);
                } else {
                    // ND read (≥4): CELL handled by indexGetND directly now.
                    const keel::i32 nd = keel::as<keel::i32>(ndims);
                    const keel::usize ndu = keel::as<keel::usize>(nd);
                    keel::Array<keel::Array<keel::usize>> idxLists(ndu);
                    keel::Array<const keel::usize *> idxPtrs(ndu);
                    keel::Array<keel::usize> idxCounts(ndu);
                    for (keel::i32 i = 0; i < nd; ++i) {
                        const keel::usize lim = (i < mv.dims().ndim()) ? mv.dims().dim(i) : 1;
                        idxLists[i] = Value::resolveIndices(R[base + i], lim);
                        idxPtrs[i] = idxLists[i].data();
                        idxCounts[i] = idxLists[i].size();
                    }
                    R[I.a] = mv.indexGetND(idxPtrs.data(), idxCounts.data(), nd,
                                           engine_.mr_);
                }
                break;
            }
            case OpCode::INDEX_SET_ND: {
                if (execIndexSetND(I, R, frame, ip))
                    goto enter_frame;
                break;
            }

            // ── Index delete (v(idx) = []) ──────────────────────
            case OpCode::INDEX_DELETE: {
                // a=arr, b=idx
                auto indices = Value::resolveIndicesUnchecked(R[I.b]);
                R[I.a].indexDelete(indices.data(), indices.size(), engine_.mr_);
                break;
            }
            case OpCode::INDEX_DELETE_2D: {
                // a=arr, b=row, c=col
                keel::usize Rows = R[I.a].dims().rows(), Cols = R[I.a].dims().cols();
                auto rowIdx = Value::resolveIndices(R[I.b], Rows);
                auto colIdx = Value::resolveIndices(R[I.c], Cols);
                R[I.a].indexDelete2D(rowIdx.data(), rowIdx.size(),
                                     colIdx.data(), colIdx.size(),
                                     engine_.mr_);
                break;
            }

            case OpCode::INDEX_DELETE_ND: {
                // a=arr, b=base, c=ndims
                keel::u16 base = I.b, ndims = I.c;
                keel::Array<keel::Array<keel::usize>> perDim(ndims);
                keel::Array<const keel::usize *> perDimPtrs(ndims);
                keel::Array<keel::usize> perDimCount(ndims);
                const auto &srcDims = R[I.a].dims();
                const keel::i32 srcNd = srcDims.ndim();
                for (keel::u16 i = 0; i < ndims; ++i) {
                    const keel::usize lim = (i < srcNd) ? srcDims.dim(i) : 1;
                    perDim[i] = Value::resolveIndices(R[base + i], lim);
                    perDimPtrs[i]  = perDim[i].data();
                    perDimCount[i] = perDim[i].size();
                }
                R[I.a].indexDeleteND(perDimPtrs.data(), perDimCount.data(),
                                     ndims, engine_.mr_);
                break;
            }

            // ── Struct field access ──────────────────────────────
            case OpCode::FIELD_GET: {
                if (execFieldGet(I.a, I.b, chunk.strings[I.d], R, frame, ip))
                    goto enter_frame;
                break;
            }
            case OpCode::FIELD_GET_OR_CREATE: {
                execFieldGetOrCreate(I.a, I.b, chunk.strings[I.d], R);
                break;
            }
            case OpCode::FIELD_SET: {
                if (execFieldSet(I.a, I.b, chunk.strings[I.d], R, frame, ip))
                    goto enter_frame;
                break;
            }
            case OpCode::STRUCT_ELEM_FIELD_SET: {
                // a=obj (struct or object array, in/out), b=idxReg, c=valReg, d=nameIdx
                const keel::String &fname = chunk.strings[I.d];
                keel::usize idx = Value::checkedScalarIndex(R[I.b].toScalar());
                Value &obj = R[I.a];
                if (obj.isObject()) {
                    engine_.objectElemPropSet(obj, idx, fname, R[I.c], currentCallEnv());
                    break;
                }
                obj.growStructArrayTo(idx, engine_.allocator());
                obj.setField(idx, fname, R[I.c]);  // BUG #15
                break;
            }
            case OpCode::STRUCT_ELEM_GET_OR_CREATE: {
                // a=dst, b=obj (in/out), c=base, e=nargs — d(i…) as a 1×1
                // struct or object element (compound-lvalue container).
                Value &obj = R[I.b];
                const keel::i32 nargs = I.e;
                if (nargs <= 0 || nargs > Dims::kMaxRank)
                    throw std::runtime_error("Struct index rank exceeds maximum supported rank");
                keel::usize coords[Dims::kMaxRank];
                for (keel::i32 k = 0; k < nargs; ++k)
                    coords[k] = Value::checkedScalarIndex(R[I.c + k].toScalar());
                if (obj.isObject()) {
                    keel::usize linear = 0;
                    if (nargs == 1) {
                        linear = coords[0];
                    } else {
                        keel::usize stride = 1;
                        for (keel::i32 k = 0; k < nargs; ++k) {
                            linear += coords[k] * stride;
                            stride *= obj.dims().dim(k);
                        }
                    }
                    if (linear >= obj.objectCount())
                        throw std::runtime_error("Index exceeds object array bounds");
                    R[I.a] = obj.objectSubArray(keel::ArrayRef<keel::usize>(&linear, 1), engine_.mr_);
                    break;
                }
                if (obj.isUnset() || obj.isEmpty())
                    obj = Value::structArray(0, 0, engine_.allocator());
                if (!obj.isStruct())
                    throw std::runtime_error("Indexed assignment on a non-struct value");
                keel::usize linear;
                if (nargs == 1) {
                    linear = coords[0];
                    obj.growStructArrayTo(linear, engine_.allocator());
                } else {
                    linear = obj.growStructArrayND(coords, nargs, engine_.allocator());
                }
                R[I.a] = obj.elemAt(linear, engine_.mr_);  // 1×1 struct (empty if vacant)
                break;
            }
            case OpCode::STRUCT_ELEM_SET: {
                // a=obj (in/out), b=base, c=nargs, e=valReg — d(i…) = struct/object.
                Value &obj = R[I.a];
                const keel::i32 nargs = I.c;
                if (nargs <= 0 || nargs > Dims::kMaxRank)
                    throw std::runtime_error("Struct index rank exceeds maximum supported rank");
                keel::usize coords[Dims::kMaxRank];
                for (keel::i32 k = 0; k < nargs; ++k)
                    coords[k] = Value::checkedScalarIndex(R[I.b + k].toScalar());
                if (obj.isObject()) {
                    keel::usize linear = 0;
                    if (nargs == 1) {
                        linear = coords[0];
                    } else {
                        keel::usize stride = 1;
                        for (keel::i32 k = 0; k < nargs; ++k) {
                            linear += coords[k] * stride;
                            stride *= obj.dims().dim(k);
                        }
                    }
                    if (linear >= obj.objectCount())
                        throw std::runtime_error("Index exceeds object array bounds");
                    if (!obj.objectIsHandle())
                        obj.objectAssignLinear(keel::ArrayRef<keel::usize>(&linear, 1),
                                               R[I.e], R[I.e], engine_.mr_);
                    break;
                }
                if (obj.isUnset() || obj.isEmpty())
                    obj = Value::structArray(0, 0, engine_.allocator());
                if (!obj.isStruct())
                    throw std::runtime_error("Indexed assignment on a non-struct value");
                keel::usize linear;
                if (nargs == 1) {
                    linear = coords[0];
                    obj.growStructArrayTo(linear, engine_.allocator());
                } else {
                    linear = obj.growStructArrayND(coords, nargs, engine_.allocator());
                }
                const Value &val = R[I.e];
                if (!val.isStruct())
                    throw std::runtime_error("Cannot assign a non-struct to a struct-array element");
                for (const auto &name : val.fieldNamesInOrder())
                    obj.setField(linear, name, val.field(name));  // BUG #15 order
                break;
            }
            case OpCode::FIELD_GET_DYN: {
                if (execFieldGet(I.a, I.b, R[I.c].asStringRef(), R, frame, ip))
                    goto enter_frame;
                break;
            }
            case OpCode::FIELD_GET_OR_CREATE_DYN: {
                execFieldGetOrCreate(I.a, I.b, R[I.c].asStringRef(), R);
                break;
            }
            case OpCode::FIELD_SET_DYN: {
                if (execFieldSet(I.a, I.c, R[I.b].asStringRef(), R, frame, ip))
                    goto enter_frame;
                break;
            }
            case OpCode::LVAL_UNSHARE_FIELD: {
                // a=childReg, b=objReg, c=isDyn(0/1), d=nameIdx, e=nameReg
                Value &obj = R[I.b];
                if (obj.isStruct() && !obj.isStructArray() && obj.heapRefCount() == 1
                    && !R[I.a].isObject()) {
                    keel::String fname = (I.c == 0) ? chunk.strings[I.d].clone() : R[I.e].toString();
                    if (obj.hasField(fname)) {
                        obj.field(fname) = Value();
                        lvalDetaches_.push_back({frame.regBase + I.b, frame.regBase + I.a,
                                                 LvalDetach::Kind::Field, keel::move(fname), 0});
                    }
                }
                break;
            }
            case OpCode::LVAL_UNSHARE_CELL: {
                // a=childReg, b=cellReg, c=idxBase, e=nargs
                Value &cell = R[I.b];
                if (cell.isCell() && cell.heapRefCount() == 1 && !R[I.a].isObject()) {
                    const keel::i32 nargs = I.e;
                    if (nargs > 0 && nargs <= Dims::kMaxRank) {
                        keel::usize linear = 0;
                        if (nargs == 1) {
                            linear = Value::checkedScalarIndex(R[I.c].toScalar());
                        } else {
                            const auto &d = cell.dims();
                            keel::usize stride = 1;
                            for (keel::i32 k = 0; k < nargs; ++k) {
                                keel::usize ck = Value::checkedScalarIndex(R[I.c + k].toScalar());
                                linear += ck * stride;
                                stride *= d.dim(k);
                            }
                        }
                        if (linear < cell.numel()) {
                            cell.cellAt(linear) = Value();
                            lvalDetaches_.push_back({frame.regBase + I.b, frame.regBase + I.a,
                                                     LvalDetach::Kind::Cell, {}, linear});
                        }
                    }
                }
                break;
            }

            // ── Cell & closure operations ─────────────────────────
            case OpCode::CLOSURE_MAKE: {
                keel::u16 base = I.b, count = I.c;
                Value fh = R[base];
                if (count > 1)
                    fh.setFuncHandleCaptures(&R[base + 1], count - 1);
                R[I.a] = std::move(fh);
                break;
            }
            case OpCode::UPVAL_NEW: {
                Value initVal = (I.b != kInvalidReg) ? R[I.b] : Value();
                Value boxVal = makeUpvalueBox(keel::move(initVal), engine_.mr_);
                if (!chunk.globalNames.empty() || !chunk.persistentNames.empty()) {
                    if (UpvaluePayload *box = asUpvalueBox(boxVal)) {
                        for (const auto &gname : chunk.globalNames) {
                            const keel::u16 *uv = chunk.upvalueBoxRegs.get(gname);
                            if (uv && *uv == I.a) {
                                box->globalEnv = engine_.globalsEnv_;
                                box->backingName = gname.clone();
                                if (!box->value.isUnset() && !box->value.isDeleted())
                                    engine_.globalsEnv_->set(gname, box->value);
                                break;
                            }
                        }
                        if (!box->globalEnv) {
                            for (const auto &pname : chunk.persistentNames) {
                                const keel::u16 *uv = chunk.upvalueBoxRegs.get(pname);
                                if (uv && *uv == I.a) {
                                    box->persistentBucket = engine_.persistentBucketPtr(chunk.name);
                                    box->backingName = pname.clone();
                                    if (!box->value.isUnset() && !box->value.isDeleted())
                                        box->persistentBucket->put(pname.clone(), box->value);
                                    break;
                                }
                            }
                        }
                    }
                }
                R[I.a] = keel::move(boxVal);
                break;
            }
            case OpCode::UPVAL_LOAD: {
                UpvaluePayload *box = asUpvalueBox(R[I.b]);
                Value v = box ? box->read() : Value();
                if (I.c == 0 && (v.isUnset() || v.isDeleted())) {
                    throw std::runtime_error(
                        keel::format("Undefined function or variable '{}'", chunk.strings[I.d])
                            .c_str());
                }
                R[I.a] = keel::move(v);
                break;
            }
            case OpCode::UPVAL_STORE: {
                if (UpvaluePayload *box = asUpvalueBox(R[I.a]))
                    box->write(R[I.b]);
                break;
            }
            case OpCode::CELL_LITERAL: {
                keel::u16 base = I.b, count = I.c;
                auto cell = Value::cell(count == 0 ? 0 : 1, count);
                for (keel::u16 i = 0; i < count; ++i) {
                    cell.cellAt(i) = R[base + i];
                }
                R[I.a] = std::move(cell);
                break;
            }
            case OpCode::CELL_APPEND_FLATTEN: {
                Value &acc = R[I.a];
                if (!acc.isCell())
                    acc = Value::cell(1, 0, engine_.mr_);
                keel::usize       n    = acc.numel();
                const Value &elem = R[I.b];
                if (elem.isCsl()) {
                    keel::usize m = elem.cslCount();
                    if (m > 0) {
                        keel::usize lastCoord = n + m - 1;
                        acc.growCellTo(&lastCoord, 1, engine_.mr_);
                        for (keel::usize j = 0; j < m; ++j)
                            acc.cellAt(n + j) = elem.cslAt(j);
                    }
                } else {
                    keel::usize lastCoord = n;
                    acc.growCellTo(&lastCoord, 1, engine_.mr_);
                    acc.cellAt(n) = elem;
                }
                break;
            }
            case OpCode::CELL_GET: {
                if (R[I.b].isObject()) {
                    const BuiltinClass *cls = engine_.findClass(R[I.b].objectClassNameRef());
                    if (cls) {
                        if (const auto *bgFn = cls->methods.get("__brace_get")) {
                            Value self = R[I.b];
                            Value args[1] = {R[I.c]};
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*bgFn)(self, keel::ArrayRef<Value>(args, 1), 1,
                                    keel::MutArrayRef<Value>(out, 1), ctx);
                            R[I.a] = std::move(out[0]);
                            break;
                        }
                    }
                }
                if (!R[I.b].isCell())
                    throw std::runtime_error("Cell indexing requires a cell array");
                const Value &cell = R[I.b];
                const Value &sub  = R[I.c];
                if (sub.isDoubleScalar()) {
                    R[I.a] = cell.cellAt(Value::checkedScalarIndex(sub.scalarVal()));
                    break;
                }
                auto ids = Value::resolveIndices(sub, cell.numel());
                if (ids.size() == 1) {
                    R[I.a] = cell.cellAt(ids[0]);
                } else {
                    Value csl = Value::csl(ids.size());
                    for (keel::usize k = 0; k < ids.size(); ++k)
                        csl.cslAt(k) = cell.cellAt(ids[k]);
                    R[I.a] = std::move(csl);
                }
                break;
            }
            case OpCode::CELL_GET_OR_CREATE: {
                Value &cell = R[I.b];
                const keel::i32 nargs = I.e;
                keel::usize coords[Dims::kMaxRank];
                for (keel::i32 k = 0; k < nargs; ++k)
                    coords[k] = Value::checkedScalarIndex(R[I.c + k].toScalar());
                keel::usize linear = cell.growCellTo(coords, nargs, engine_.allocator());
                R[I.a] = cell.cellAt(linear);
                break;
            }
            case OpCode::CELL_GET_2D: {
                if (R[I.b].isObject()) {
                    const BuiltinClass *cls = engine_.findClass(R[I.b].objectClassNameRef());
                    if (cls) {
                        if (const auto *bgFn = cls->methods.get("__brace_get")) {
                            Value self = R[I.b];
                            Value args[2] = {R[I.c], R[I.e]};
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*bgFn)(self, keel::ArrayRef<Value>(args, 2), 1,
                                    keel::MutArrayRef<Value>(out, 1), ctx);
                            R[I.a] = std::move(out[0]);
                            break;
                        }
                    }
                }
                if (!R[I.b].isCell())
                    throw std::runtime_error("Cell indexing requires a cell array");
                const Value &cell = R[I.b];
                const Value &rsub = R[I.c];
                const Value &csub = R[I.e];
                if (rsub.isDoubleScalar() && csub.isDoubleScalar()) {
                    keel::usize r = Value::checkedScalarIndex(rsub.scalarVal());
                    keel::usize c = Value::checkedScalarIndex(csub.scalarVal());
                    R[I.a] = cell.cellAt(cell.dims().sub2ind(r, c));
                    break;
                }
                auto rows = Value::resolveIndices(rsub, cell.dims().rows());
                auto cols = Value::resolveIndices(csub, cell.dims().cols());
                if (rows.size() * cols.size() == 1) {
                    R[I.a] = cell.cellAt(cell.dims().sub2ind(rows[0], cols[0]));
                } else {
                    Value  csl = Value::csl(rows.size() * cols.size());
                    keel::usize k   = 0;
                    for (keel::usize c : cols)
                        for (keel::usize r : rows)
                            csl.cslAt(k++) = cell.cellAt(cell.dims().sub2ind(r, c));
                    R[I.a] = std::move(csl);
                }
                break;
            }
            case OpCode::CELL_GET_MULTI: {
                if (!R[I.b].isCell())
                    throw std::runtime_error("Cell indexing requires a cell array");
                auto indices = Value::resolveIndices(R[I.c], R[I.b].numel());
                keel::u16 outBase = I.a, nout = I.e;
                for (keel::u16 i = 0; i < nout && i < indices.size(); ++i)
                    R[outBase + i] = R[I.b].cellAt(indices[i]);
                break;
            }
            case OpCode::CELL_GET_MULTI_2D: {
                if (!R[I.b].isCell())
                    throw std::runtime_error("Cell indexing requires a cell array");
                const Value &cell   = R[I.b];
                auto         rowIdx = Value::resolveIndices(R[I.c], cell.dims().rows());
                auto         colIdx = Value::resolveIndices(R[I.d], cell.dims().cols());
                keel::u16      outBase = I.a, nout = I.e;
                keel::usize       k = 0;
                for (keel::usize c : colIdx) {
                    for (keel::usize r : rowIdx) {
                        if (k >= nout) break;
                        R[outBase + k++] = cell.cellAt(cell.dims().sub2ind(r, c));
                    }
                    if (k >= nout) break;
                }
                break;
            }
            case OpCode::CELL_SET: {
                if (R[I.a].isObject()) {
                    const BuiltinClass *cls = engine_.findClass(R[I.a].objectClassNameRef());
                    if (cls) {
                        if (const auto *bsFn = cls->methods.get("__brace_set")) {
                            Value args[2] = {R[I.b], R[I.c]};
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*bsFn)(R[I.a], keel::ArrayRef<Value>(args, 2), 0,
                                    keel::MutArrayRef<Value>(out, 1), ctx);
                            break;
                        }
                    }
                }
                if (R[I.a].isEmpty())
                    R[I.a] = Value::cell(0, 0);
                if (!R[I.a].isCell())
                    throw std::runtime_error("Cell indexing requires a cell array");
                const bool isColonSub = R[I.b].isChar() && R[I.b].asStringRef() == ":";
                if (R[I.b].isLogical() || isColonSub || !R[I.b].isScalar()) {
                    keel::usize maxLen = R[I.a].numel();
                    if (!R[I.b].isLogical() && !isColonSub) {
                        for (keel::usize k = 0; k < R[I.b].numel(); ++k) {
                            keel::usize idx = Value::checkedScalarIndex(R[I.b].elemAsDouble(k));
                            if (idx + 1 > maxLen)
                                maxLen = idx + 1;
                        }
                        if (maxLen > R[I.a].numel()) {
                            keel::usize lastCoord = maxLen - 1;
                            R[I.a].growCellTo(&lastCoord, 1, engine_.mr_);
                        }
                    }
                    auto indices = Value::resolveIndices(R[I.b], R[I.a].numel());
                    for (keel::usize idx : indices)
                        R[I.a].cellAt(idx) = R[I.c];
                    break;
                }
                keel::usize i = Value::checkedScalarIndex(R[I.b].toScalar());
                if (i >= R[I.a].numel())
                    R[I.a].growCellTo(&i, 1, engine_.mr_);
                if (!lvalDetaches_.empty()
                    && lvalDetaches_.back().kind == LvalDetach::Kind::Cell
                    && lvalDetaches_.back().containerRegAbs == frame.regBase + I.a
                    && lvalDetaches_.back().valRegAbs == frame.regBase + I.c
                    && lvalDetaches_.back().cellLinearIdx == i) {
                    lvalDetaches_.pop_back();
                    R[I.a].cellAt(i) = std::move(R[I.c]);
                } else {
                    R[I.a].cellAt(i) = R[I.c];
                }
                break;
            }
            case OpCode::CELL_SET_2D: {
                if (R[I.a].isObject()) {
                    const BuiltinClass *cls = engine_.findClass(R[I.a].objectClassNameRef());
                    if (cls) {
                        if (const auto *bsFn = cls->methods.get("__brace_set")) {
                            Value args[3] = {R[I.b], R[I.c], R[I.e]};
                            Value out[1];
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*bsFn)(R[I.a], keel::ArrayRef<Value>(args, 3), 0,
                                    keel::MutArrayRef<Value>(out, 1), ctx);
                            break;
                        }
                    }
                }
                if (R[I.a].isEmpty())
                    R[I.a] = Value::cell(0, 0);
                if (!R[I.a].isCell())
                    throw std::runtime_error("Cell indexing requires a cell array");
                keel::usize r = Value::checkedScalarIndex(R[I.b].toScalar()), c = Value::checkedScalarIndex(R[I.c].toScalar());
                // Auto-grow if needed
                keel::usize nr = R[I.a].dims().rows(), nc = R[I.a].dims().cols();
                if (r + 1 > nr || c + 1 > nc) {
                    keel::usize newR = std::max(nr, r + 1), newC = std::max(nc, c + 1);
                    auto grown = Value::cell(newR, newC);
                    for (keel::usize cc = 0; cc < nc; ++cc)
                        for (keel::usize rr = 0; rr < nr; ++rr)
                            grown.cellAt(cc * newR + rr) = R[I.a].cellAt(cc * nr + rr);
                    R[I.a] = std::move(grown);
                }
                keel::usize lin = R[I.a].dims().sub2ind(r, c);
                if (!lvalDetaches_.empty()
                    && lvalDetaches_.back().kind == LvalDetach::Kind::Cell
                    && lvalDetaches_.back().containerRegAbs == frame.regBase + I.a
                    && lvalDetaches_.back().valRegAbs == frame.regBase + I.e
                    && lvalDetaches_.back().cellLinearIdx == lin) {
                    lvalDetaches_.pop_back();
                    R[I.a].cellAt(lin) = std::move(R[I.e]);
                } else {
                    R[I.a].cellAt(lin) = R[I.e];
                }
                break;
            }
            case OpCode::CELL_GET_ND: {
                // a=dst, b=cell, c=base, e=ndims
                if (!R[I.b].isCell())
                    throw std::runtime_error("Cell indexing requires a cell array");
                keel::u16 base = I.c, ndims = I.e;
                if (ndims == 3) {
                    keel::usize r = Value::checkedScalarIndex(R[base].toScalar());
                    keel::usize c = Value::checkedScalarIndex(R[base + 1].toScalar());
                    keel::usize p = Value::checkedScalarIndex(R[base + 2].toScalar());
                    keel::usize idx = R[I.b].dims().sub2ind(r, c, p);
                    R[I.a] = R[I.b].cellAt(idx);
                } else {
                    // General ND (≥4): column-major linear index from
                    // per-axis subscripts using actual dim(i).
                    const auto &d = R[I.b].dims();
                    keel::usize idx = 0, stride = 1;
                    for (keel::u16 i = 0; i < ndims; ++i) {
                        keel::usize si = Value::checkedScalarIndex(R[base + i].toScalar());
                        idx += si * stride;
                        stride *= d.dim(i);
                    }
                    R[I.a] = R[I.b].cellAt(idx);
                }
                break;
            }
            case OpCode::CELL_SET_ND: {
                // a=cell, b=base, c=ndims, e=val
                if (R[I.a].isEmpty())
                    R[I.a] = Value::cell(0, 0);
                if (!R[I.a].isCell())
                    throw std::runtime_error("Cell indexing requires a cell array");
                keel::u16 base = I.b, ndims = I.c;
                if (ndims == 3) {
                    keel::usize r = Value::checkedScalarIndex(R[base].toScalar());
                    keel::usize c = Value::checkedScalarIndex(R[base + 1].toScalar());
                    keel::usize p = Value::checkedScalarIndex(R[base + 2].toScalar());
                    // Auto-grow if needed
                    keel::usize nr = R[I.a].dims().rows(), nc = R[I.a].dims().cols(), np = R[I.a].dims().pages();
                    if (r + 1 > nr || c + 1 > nc || p + 1 > np) {
                        keel::usize newR = std::max(nr, r + 1);
                        keel::usize newC = std::max(nc, c + 1);
                        keel::usize newP = std::max(np, p + 1);
                        auto grown = Value::cell3D(newR, newC, newP);
                        // Copy existing elements
                        for (keel::usize pp = 0; pp < np; ++pp)
                            for (keel::usize cc = 0; cc < nc; ++cc)
                                for (keel::usize rr = 0; rr < nr; ++rr) {
                                    keel::usize oldIdx = rr + cc * nr + pp * nr * nc;
                                    keel::usize newIdx = rr + cc * newR + pp * newR * newC;
                                    grown.cellAt(newIdx) = R[I.a].cellAt(oldIdx);
                                }
                        R[I.a] = std::move(grown);
                    }
                    keel::usize idx = R[I.a].dims().sub2ind(r, c, p);
                    if (!lvalDetaches_.empty()
                        && lvalDetaches_.back().kind == LvalDetach::Kind::Cell
                        && lvalDetaches_.back().containerRegAbs == frame.regBase + I.a
                        && lvalDetaches_.back().valRegAbs == frame.regBase + I.e
                        && lvalDetaches_.back().cellLinearIdx == idx) {
                        lvalDetaches_.pop_back();
                        R[I.a].cellAt(idx) = std::move(R[I.e]);
                    } else {
                        R[I.a].cellAt(idx) = R[I.e];
                    }
                } else {
                    // General ND (≥4): auto-grow on out-of-range subscripts
                    // (rank ↑ for new trailing axes, axis-size ↑ otherwise),
                    // then column-major linear-index assign.
                    keel::Array<keel::usize> coords(ndims);
                    for (keel::u16 i = 0; i < ndims; ++i)
                        coords[i] = Value::checkedScalarIndex(R[base + i].toScalar());
                    const keel::i32 curNd = R[I.a].dims().ndim();
                    const keel::i32 newNd = std::max(keel::as<keel::i32>(ndims), curNd);
                    keel::Array<keel::usize> need(newNd, keel::as<keel::usize>(1));
                    for (keel::i32 i = 0; i < newNd; ++i)
                        need[i] = (i < curNd) ? R[I.a].dims().dim(i) : 1;
                    bool grow = (keel::as<keel::i32>(ndims) > curNd);
                    for (keel::u16 i = 0; i < ndims; ++i) {
                        if (coords[i] + 1 > need[i]) {
                            need[i] = coords[i] + 1;
                            grow = true;
                        }
                    }
                    if (grow)
                        R[I.a].resizeND(need.data(), newNd, engine_.mr_);
                    const auto &d = R[I.a].dims();
                    keel::usize idx = 0, stride = 1;
                    for (keel::u16 i = 0; i < ndims; ++i) {
                        idx += coords[i] * stride;
                        stride *= d.dim(i);
                    }
                    if (!lvalDetaches_.empty()
                        && lvalDetaches_.back().kind == LvalDetach::Kind::Cell
                        && lvalDetaches_.back().containerRegAbs == frame.regBase + I.a
                        && lvalDetaches_.back().valRegAbs == frame.regBase + I.e
                        && lvalDetaches_.back().cellLinearIdx == idx) {
                        lvalDetaches_.pop_back();
                        R[I.a].cellAt(idx) = std::move(R[I.e]);
                    } else {
                        R[I.a].cellAt(idx) = R[I.e];
                    }
                }
                break;
            }

            // ── Inline scalar builtins ───────────────────────────
            case OpCode::CALL_BUILTIN:
                execCallBuiltin(I, R);
                break;

            // ── General function calls ───────────────────────────
            case OpCode::CALL: {
                if (execCallNamedSingleSpan(I.a, I.d, I.e,
                                            I.c ? &R[I.b] : nullptr, I.c,
                                            R, frame, ip, resolvedFuncs))
                    goto enter_frame;
                break;
            }

            case OpCode::CALL_FLATTEN: {
                SplatArgs splat = splatCallArgs(R, I.b, I.c);
                if (execCallNamedSingleSpan(I.a, I.d, I.e,
                                            splat.data(), splat.size(),
                                            R, frame, ip, resolvedFuncs))
                    goto enter_frame;
                break;
            }

            case OpCode::CALL_MULTI:
            case OpCode::CALL_FLATTEN_MULTI: {
                SplatArgs splat = splatCallArgs(R, I.b, I.c);
                if (execCallNamedMultiSpan(I.a, I.d, I.e,
                                           splat.data(), splat.size(),
                                           R, frame, ip, resolvedFuncs))
                    goto enter_frame;
                break;
            }

            // ── Indirect function call (func handle) or array indexing ─
            case OpCode::CALL_METHOD: {
                if (execCallMethod(I, R, frame, ip))
                    goto enter_frame;
                break;
            }
            case OpCode::CALL_METHOD_MULTI: {
                if (execCallMethodMulti(I, R, frame, ip))
                    goto enter_frame;
                break;
            }
            // ── Superclass calls inside a classdef body ──
            case OpCode::CALL_SUPER_CTOR: {
                // obj = obj@Base(args): a=dst, b=objReg(seed), c=argBase,
                // d=baseNameIdx, e=nargs.
                const keel::String &base = chunk.strings[I.d];
                SplatArgs splat = splatCallArgs(R, I.c, I.e);
                const Value *argPtr = splat.data();
                keel::usize argN = splat.size();
                R[I.a] = engine_.superConstruct(base, R[I.b],
                                                keel::ArrayRef<Value>(argPtr, argN));
                break;
            }
            case OpCode::CALL_SUPER_METHOD: {
                // [outs] = method@Base(obj, args): a=outBase, b=argBase
                // (obj at [0]), c=nargs, d=idx of "Base>method", e=nout.
                // The receiver at [0] is an object (never a CSL), so the
                // splat below can only expand the trailing args
                // (bugs/closed/core/super-call-no-csl-splat).
                const keel::String &qual = chunk.strings[I.d];
                keel::usize gt = qual.find('>');
                keel::StringRef base = qual.substr(0, gt);
                keel::StringRef method = (gt == keel::kInvalidIndex) ? keel::StringRef{} : qual.substr(gt + 1);
                keel::u16 outBase = I.a, argBase = I.b, nout = I.e;
                SplatArgs splat = splatCallArgs(R, argBase, I.c);
                const Value *argPtr = splat.data();
                keel::usize argN = splat.size();
                auto results = engine_.superMethod(
                    base, method, keel::ArrayRef<Value>(argPtr, argN),
                    std::max<keel::usize>(nout, 1));
                for (keel::u16 i = 0; i < nout; ++i)
                    R[outBase + i] = (i < results.size()) ? keel::move(results[i]) : Value();
                break;
            }
            case OpCode::CALL_INDIRECT:
                if (execCallIndirect(I, R, frame, ip))
                    goto enter_frame;
                break;

            case OpCode::CALL_INDIRECT_MULTI:
                if (execCallIndirectMulti(I, R, frame, ip))
                    goto enter_frame;
                break;

            case OpCode::CALL_INDIRECT_FLATTEN: {
                // f(c{:}) with a variable callee: flatten any CSL arg,
                // then the CALL_INDIRECT(_MULTI) dispatch over the
                // flattened span (bugs/closed/lang/handle-call-csl-splat).
                const keel::u16 fhReg = I.b, fargBase = I.c, fna = I.d;
                const keel::u16 fnout = I.e;
                bool anyCsl = false;
                for (keel::u16 i = 0; i < fna; ++i)
                    if (R[fargBase + i].isCsl()) { anyCsl = true; break; }
                if (!anyCsl && fnout <= 1) {
                    if (execCallIndirectSpan(R[fhReg], I.a,
                                             fna ? &R[fargBase] : nullptr, fna,
                                             R, frame, ip))
                        goto enter_frame;
                    break;
                }
                if (!anyCsl) {
                    if (execCallIndirectMulti(I, R, frame, ip))
                        goto enter_frame;
                    break;
                }
                SplatArgs splat = splatCallArgs(R, fargBase, fna);
                const Value *ABf = splat.data();
                keel::usize nflat = splat.size();
                if (fnout <= 1) {
                    if (execCallIndirectSpan(R[fhReg], I.a, ABf, nflat,
                                             R, frame, ip))
                        goto enter_frame;
                    break;
                }
                // Multi-output flattened handle call: resolve + push a
                // MULTI frame over the flattened args (captures appended).
                if (!R[fhReg].isFuncHandle()) {
                    throw std::runtime_error(
                        "VM: multi-output call of a value that is not a "
                        "function handle");
                }
                const Value &fh = R[fhReg];
                const keel::usize ncaps = fh.funcHandleCaptureCount();
                const keel::StringRef fName = fh.funcHandleNameRef();
                keel::Array<Value> cargs(nflat + ncaps);
                for (keel::usize i = 0; i < nflat; ++i) cargs[i] = ABf[i];
                for (keel::usize i = 0; i < ncaps; ++i)
                    cargs[nflat + i] = fh.funcHandleCapture(i);
                const keel::usize ctot = cargs.size();
                if (const BytecodeChunk *fc = findCompiledFunc(fName)) {
                    frame.ip = ip + 1;
                    returnBuf_.clear();
                    pushCallFrame(*fc, cargs.data(), ctot, 0, fnout,
                                  true, I.a, fnout);
                    goto enter_frame;
                }
                if (auto *uf = engine_.lookupUserFunction(fName, currentCallEnv())) {
                    const BytecodeChunk *fc = findCompiledFunc(uf->name);
                    if (!fc)
                        fc = engine_.ensureClassMethodChunk(*uf);
                    frame.ip = ip + 1;
                    returnBuf_.clear();
                    pushCallFrame(*fc, cargs.data(), ctot, 0, fnout,
                                  true, I.a, fnout);
                    goto enter_frame;
                }
                if (const ExternalFunc *fp =
                        engine_.findExternal(fName, currentCallEnv())) {
                    keel::Array<Value> ob(fnout);
                    CallContext ctx{&engine_, currentCallEnv()};
                    (*fp)(keel::ArrayRef<Value>(cargs.data(), ctot), fnout,
                          keel::MutArrayRef<Value>(ob.data(), fnout), ctx);
                    for (keel::u16 i = 0; i < fnout; ++i)
                        if (ob[i].isUnset())
                            throw std::runtime_error("Too many output arguments.");
                    for (keel::u16 i = 0; i < fnout; ++i)
                        R[I.a + i] = keel::move(ob[i]);
                    break;
                }
                throw std::runtime_error(
                    keel::format("VM: undefined function in handle '@{}'", fName).c_str());
            }

            // ── Display ──────────────────────────────────────────
            case OpCode::DISPLAY:
                execDisplay(I, R, chunk);
                break;

            // ── Return ───────────────────────────────────────────
            case OpCode::RET:
                returnBuf_.clear();
                popCallFrame(R[I.a]);
                goto enter_frame;
            case OpCode::RET_MULTI: {
                keel::u16 base = I.a, count = I.b;
                returnBuf_.clear();
                returnBuf_.reserve(count);
                for (keel::u16 i = 0; i < count; ++i)
                    returnBuf_.push_back(R[base + i]);
                popCallFrame(count > 0 ? Value(returnBuf_[0]) : Value());
                goto enter_frame;
            }
            case OpCode::RET_VARARGOUT: {
                keel::u16 fixedBase = I.a, numFixed = I.b, vaReg = I.c;
                returnBuf_.clear();
                const Value &va = R[vaReg];
                const keel::usize n = va.isCell() ? va.numel() : 0;
                returnBuf_.reserve(numFixed + n);
                for (keel::u16 i = 0; i < numFixed; ++i)
                    returnBuf_.push_back(R[fixedBase + i]);
                if (va.isCell()) {
                    for (keel::usize i = 0; i < n; ++i)
                        returnBuf_.push_back(va.cellAt(i));
                }
                popCallFrame(!returnBuf_.empty() ? Value(returnBuf_[0]) : Value());
                goto enter_frame;
            }
            case OpCode::RET_EMPTY:
            case OpCode::HALT:
                returnBuf_.clear();
                popCallFrame(Value());
                goto enter_frame;
            case OpCode::NOP:
                if (I.a == 1 && !forStack_.empty())
                    forStack_.pop_back(); // break from for-loop
                break;

            case OpCode::ASSERT_DEF:
                if (R[I.a].isUnset() || R[I.a].isDeleted()) {
                    // nargin/nargout at top level are illegal in MATLAB —
                    // this check must precede the env fallbacks below
                    // (workspaceEnv HOLDS a top-level nargin value, which
                    // would otherwise silently resolve the name).
                    if (!inReentrantEval_) {
                        const keel::String &nn = chunk.strings[I.d];
                        if (nn == "nargin" || nn == "nargout")
                            throw std::runtime_error(
                                "You can only call nargin/nargout from within a function.");
                    }
                    // Fallback: check dynamic variables (debug eval, runtime eval)
                    if (frame.dynVars) {
                        if (const Value *dv = frame.dynVars->get(chunk.strings[I.d]);
                            dv && !dv->isDeleted()) {
                            R[I.a] = *dv;
                            break;
                        }
                    }
                    // The BASE frame also consults the workspace env:
                    // names created MID-CHUNK by eval()/assignin land
                    // there after the inner chunk syncs, but this
                    // chunk's registers predate them (write side of the
                    // eval-family bug). Only frames_.front() — function
                    // frames allocate their private env LAZILY (env can
                    // still be null mid-body), so `!frame.env` was the
                    // wrong predicate and leaked base variables into
                    // functions (ScopeIsolationTest regression).
                    if (frame.isScriptFrame) {
                        Environment &scopeEnv = inheritedScope_ ? *inheritedScope_ : engine_.workspaceEnv();
                        const Value *wv = scopeEnv.get(chunk.strings[I.d]);
                        if (wv && !wv->isUnset() && !wv->isDeleted()) {
                            R[I.a] = *wv;
                            break;
                        }
                    } else if (frame.env) {
                        const Value *lv = frame.env->getLocal(
                            chunk.strings[I.d]);
                        if (lv && !lv->isUnset() && !lv->isDeleted()) {
                            R[I.a] = *lv;
                            break;
                        }
                    }
                    if (!chunk.parentFuncName.empty()) {
                        const keel::String &nn = chunk.strings[I.d];
                        bool foundInParent = false;
                        for (keel::i32 fi = keel::as<keel::i32>(frames_.size()) - 2; fi >= 0; --fi) {
                            const CallFrame &pf = frames_[fi];
                            if (!pf.chunk || pf.chunk->name != chunk.parentFuncName)
                                continue;
                            for (const auto &[vname, reg] : pf.chunk->varMap) {
                                if (vname == nn && reg < pf.nregs) {
                                    Value pv = readChunkVar(*pf.chunk, pf.R, pf.nregs, vname, reg);
                                    if (!pv.isUnset() && !pv.isDeleted()) {
                                        R[I.a] = keel::move(pv);
                                        foundInParent = true;
                                    }
                                    break;
                                }
                            }
                            if (foundInParent)
                                break;
                        }
                        if (!foundInParent) {
                            for (keel::i32 psi = keel::as<keel::i32>(activePausedStates_.size()) - 1; psi >= 0 && !foundInParent; --psi) {
                                const VM::PausedState *ps = activePausedStates_[psi];
                                if (!ps) continue;
                                for (keel::i32 fi = keel::as<keel::i32>(ps->frames.size()) - 1; fi >= 0; --fi) {
                                    const CallFrame &pf = ps->frames[fi];
                                    if (!pf.chunk || pf.chunk->name != chunk.parentFuncName)
                                        continue;
                                    if (pf.regBase + pf.nregs > ps->regSnapshot.size())
                                        continue;
                                    const Value *psR = &ps->regSnapshot[pf.regBase];
                                    for (const auto &[vname, reg] : pf.chunk->varMap) {
                                        if (vname == nn && reg < pf.nregs) {
                                            Value pv = readChunkVar(*pf.chunk, psR, pf.nregs, vname, reg);
                                            if (!pv.isUnset() && !pv.isDeleted()) {
                                                R[I.a] = keel::move(pv);
                                                foundInParent = true;
                                            }
                                            break;
                                        }
                                    }
                                    if (foundInParent)
                                        break;
                                }
                            }
                        }
                        if (foundInParent)
                            break;
                    }
                    {
                        const keel::String &nn = chunk.strings[I.d];
                        if (auto *uf = engine_.lookupUserFunction(nn, currentCallEnv())) {
                            const BytecodeChunk *found = findCompiledFunc(uf->name);
                            if (!found)
                                found = engine_.ensureClassMethodChunk(*uf);
                            frame.ip = ip + 1;
                            pushCallFrame(*found, nullptr, 0, I.a, 1);
                            goto enter_frame;
                        }
                        if (const ExternalFunc *fnPtr = engine_.findExternal(nn, currentCallEnv())) {
                            Value ob[1];
                            keel::MutArrayRef<Value> os(ob, 1);
                            CallContext ctx{&engine_, currentCallEnv()};
                            (*fnPtr)(keel::ArrayRef<Value>(nullptr, 0), 0, os, ctx);
                            R[I.a] = keel::move(ob[0]);
                            break;
                        }
                    }
                    throw std::runtime_error(
                        keel::format("Undefined function or variable '{}'", chunk.strings[I.d]).c_str());
                }
                break;

            case OpCode::CLEAR_VAR:
                R[I.a] = Value::deleted();
                break;

            case OpCode::CLEAR_DYN: {
                auto clearOneVar = [&](keel::StringRef varName) {
                    for (const auto &[vn, reg] : chunk.varMap) {
                        if (vn == varName && reg < chunk.numRegisters) {
                            writeChunkVar(chunk, R, chunk.numRegisters, vn, reg, Value::deleted());
                            break;
                        }
                    }
                    if (frame.dynVars)
                        frame.dynVars->remove(varName);
                    if (frame.env)
                        frame.env->remove(varName);
                };
                if (R[I.a].isCsl()) {
                    const keel::usize n = R[I.a].cslCount();
                    if (n == 0 && I.b <= 1) {
                        for (const auto &[vn, reg] : chunk.varMap) {
                            if (reg < chunk.numRegisters && engine_.isReservedName(vn) == 0)
                                writeChunkVar(chunk, R, chunk.numRegisters, vn, reg, Value::deleted());
                        }
                        if (frame.dynVars)
                            frame.dynVars->clear();
                        if (frame.env)
                            frame.env->clearAll(/*keepImports=*/true);
                    } else {
                        for (keel::usize k = 0; k < n; ++k)
                            clearOneVar(R[I.a].cslAt(k).toString());
                    }
                    break;
                }
                clearOneVar(R[I.a].toString());
                break;
            }

            case OpCode::EXIST_VAR: {
                // a=dst, b=nameReg, c=filterReg (0 = no filter)
                const keel::StringRef varName = R[I.b].asStringRef();
                keel::StringRef filter;
                if (I.c != 0 && !R[I.c].isEmpty())
                    filter = R[I.c].asStringRef();

                keel::f64 code = 0;

                auto hasLocalVar = [&]() -> bool {
                    for (const auto &[vn, reg] : chunk.varMap) {
                        if (vn == varName && reg < chunk.numRegisters) {
                            Value rv = readChunkVar(chunk, R, chunk.numRegisters, vn, reg);
                            if (!rv.isUnset() && !rv.isDeleted())
                                return true;
                            if (rv.isDeleted())
                                return false;
                            break;
                        }
                    }
                    if (frame.dynVars) {
                        if (const Value *dv = frame.dynVars->get(varName);
                            dv && !dv->isUnset() && !dv->isDeleted())
                            return true;
                    }
                    if (frame.env) {
                        if (const Value *lv = frame.env->getLocal(varName);
                            lv && !lv->isUnset() && !lv->isDeleted())
                            return true;
                    }
                    return false;
                };

                auto delegateToExternal = [&]() -> bool {
                    const ExternalFunc *fn =
                        engine_.findExternal("exist", currentCallEnv());
                    if (!fn)
                        return false;
                    Value argv[2] = {Value::fromString(varName, engine_.mr_),
                                     Value::fromString(filter, engine_.mr_)};
                    keel::ArrayRef<Value> as(argv, filter.empty() ? 1u : 2u);
                    Value ob[1];
                    CallContext ctx{&engine_, currentCallEnv()};
                    (*fn)(as, 1, keel::MutArrayRef<Value>(ob, 1), ctx);
                    if (ob[0].isScalar())
                        code = ob[0].toScalar();
                    return true;
                };

                if (filter.empty()) {
                    if (hasLocalVar())
                        code = 1;
                    else if (engine_.findClass(varName) != nullptr)
                        code = 8;
                    else if (engine_.hasFunction(varName))
                        code = 5;
                    if (code == 0)
                        delegateToExternal();
                } else if (filter == "var") {
                    if (hasLocalVar())
                        code = 1;
                } else if (filter == "builtin") {
                    if (engine_.hasExternalFunction(varName))
                        code = 5;
                } else if (filter == "class") {
                    if (engine_.findClass(varName) != nullptr)
                        code = 8;
                    else
                        delegateToExternal();
                } else if (filter == "file" || filter == "dir") {
                    delegateToExternal();
                }

                R[I.a] = Value::scalar(code, engine_.mr_);
                break;
            }

            case OpCode::WHO:
                execWho(I, R, chunk);
                break;

            case OpCode::WHOS:
                execWhos(I, R, chunk);
                break;

            // ── Try/catch ────────────────────────────────────────
            case OpCode::TRY_BEGIN: {
                // I.d = offset to catch block, I.a = exception register
                TryHandler th;
                th.catchIp = ip + I.d;
                th.exReg = I.a;
                th.forStackSize = forStack_.size();
                th.frameIndex = frames_.size() - 1;
                tryStack_.push_back(th);
                break;
            }
            case OpCode::TRY_END:
                if (!tryStack_.empty())
                    tryStack_.pop_back();
                break;

            case OpCode::THROW: {
                // a = register containing error message (string or struct)
                if (R[I.a].isChar() || R[I.a].isString())
                    throw Error(R[I.a].asStringRef());
                if (R[I.a].isStruct()) {
                    const keel::StringRef msg = R[I.a].hasField("message")
                                                    ? R[I.a].field("message").asStringRef()
                                                    : keel::StringRef("User error");
                    const keel::StringRef id = R[I.a].hasField("identifier")
                                                   ? R[I.a].field("identifier").asStringRef()
                                                   : keel::StringRef{};
                    throw Error(msg, 0, 0, "", "", id);
                }
                throw Error("User error");
            }

            default:
                throw std::runtime_error(
                    keel::format("VM: unimplemented opcode {}", (keel::i32) I.op).c_str());

            } // switch
            ++ip;
        } // while
    } catch (const DebugStopException &) {
        rollbackLvalDetaches();
        throw; // pass through — not a user error
    } catch (Error &mle) {
        rollbackLvalDetaches();
        keel::String id = mle.identifier().empty()
                              ? keel::String::from("numkit:error")
                              : mle.identifier().clone();
        if (dispatchTryCatch(mle.what(), id.c_str()))
            goto enter_frame;
        // Enrich with current instruction's source location if missing
        // (e.g. Error thrown from a public C++ library API that didn't
        // know its call site).
        keel::usize instrIdx = keel::as<keel::usize>(ip - chunk.code.data());
        if (instrIdx < chunk.sourceMap.size() && chunk.sourceMap[instrIdx].line > 0) {
            mle.attachIfMissing(chunk.sourceMap[instrIdx].line,
                                chunk.sourceMap[instrIdx].col,
                                chunk.name,
                                describeInstruction(*ip, chunk));
        }
        if (stopOnError_ && debugCtl()) {
            // dbstop if error: pause at the failing instruction with frames
            // intact; resumeExecution() rethrows the stored exception later.
            frame.ip = ip;
            errorPauseMsg_.assign(mle.what());
            pausedError_ = std::current_exception();
            atErrorPause_ = true;
            return ExecStatus::Paused;
        }
        throw;
    } catch (const std::exception &ex) {
        rollbackLvalDetaches();
        if (dispatchTryCatch(ex.what(), "numkit:error"))
            goto enter_frame;
        if (stopOnError_ && debugCtl()) {
            frame.ip = ip;
            errorPauseMsg_.assign(ex.what());
            pausedError_ = std::current_exception();
            atErrorPause_ = true;
            return ExecStatus::Paused;
        }
        enrichAndThrow(ex, ip, chunk);
    }

    // Fell off end of bytecode — implicit return empty
    popCallFrame(Value());
    goto enter_frame;
    } // scope for frame locals
}

// ============================================================
// Variable export from top-level frame
// ============================================================

// Rebuild `out` (lastVarMap_) as a chunk's assigned LOCAL variables, for syncing
// back to the workspace. Only names the chunk actually wrote to are exported
// (assignedVars) — reading `pi` or another reserved name must not shadow it in
// the base workspace. Globals are excluded: their value round-trips through
// globalsEnv_, and a bare `global X` is "assigned" with an unset register that
// would otherwise make syncVMToWorkspace remove() it (cascading into globalsEnv_
// and clobbering the live value). Global membership is O(1) via a set built once
// — and skipped entirely when the chunk declares no globals (the common case).
static void rebuildLocalVarMap(keel::Array<std::pair<keel::String, Value>> &out,
                               const BytecodeChunk &chunk, const Value *R, keel::u32 nregs)
{
    out.clear();
    const auto &assigned = chunk.assignedVars;
    if (chunk.globalNames.empty() && chunk.persistentNames.empty()) {
        for (const auto &[name, reg] : chunk.varMap)
            if (reg < nregs && assigned.contains(name))
                out.push_back({name.clone(), readChunkVar(chunk, R, nregs, name, reg)});
        return;
    }
    keel::HashSet<keel::StringRef> excluded;
    for (const auto &gn : chunk.globalNames)
        excluded.insert(gn.as_ref());
    for (const auto &pn : chunk.persistentNames)
        excluded.insert(pn.as_ref());
    for (const auto &[name, reg] : chunk.varMap)
        if (reg < nregs && assigned.contains(name) && !excluded.contains(name.as_ref()))
            out.push_back({name.clone(), readChunkVar(chunk, R, nregs, name, reg)});
}

void VM::exportTopLevelVariables()
{
    if (frames_.empty())
        return;

    CallFrame &topFrame = frames_[0];
    if (!topFrame.chunk)
        return;
    if (topFrame.isScriptFrame)
        rebuildLocalVarMap(lastVarMap_, *topFrame.chunk, topFrame.R, topFrame.nregs);
    exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *topFrame.chunk, topFrame.R,
                                     topFrame.nregs, topFrame.isScriptFrame);
}

// ============================================================
// Exception helpers
// ============================================================

bool VM::dispatchTryCatch(const char *msg, const char *identifier)
{
    if (tryStack_.empty())
        return false;

    TryHandler th = tryStack_.back();
    tryStack_.pop_back();

    // Unwind call frames to the handler's frame (exception may propagate across calls)
    while (frames_.size() > th.frameIndex + 1) {
        CallFrame &f = frames_.back();
        if (f.chunk) {
            exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, f.R, f.nregs,
                                             f.isScriptFrame);
        }
        // Drop the unwound frames' provenance pushes: the lowest unwound
        // frame's originDepth is the cut point — entries below it belong to
        // surviving frames or non-VM scopes (the run() builtin).
        engine_.truncateScriptOrigins(f.originDepth);
        // Pop debug frame
        if (auto *ctl = debugCtl())
            ctl->popFrame();
        // Remove try handlers from this frame
        while (!tryStack_.empty() && tryStack_.back().frameIndex >= frames_.size() - 1)
            tryStack_.pop_back();
        // Trim for-loop stack
        forStack_.resize(std::min(forStack_.size(), f.forStackBase));
        // Cleanup registers
        for (keel::u32 i = 0; i < f.nregs; ++i)
            if (!f.R[i].isDoubleScalar() && !f.R[i].isEmpty())
                f.R[i] = Value();
        regStackTop_ = f.regBase;
        frames_.pop_back();
    }

    // Restore for-loop stack to the state at TRY_BEGIN
    if (forStack_.size() > th.forStackSize)
        forStack_.resize(th.forStackSize);

    // Set up catch context in the handler's frame
    CallFrame &frame = frames_.back();
    R_ = frame.R;
    if (frame.chunk) {
        importChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *frame.chunk, frame.R,
                                         frame.nregs, frame.isScriptFrame, /*seedMissing=*/false);
    }

    Value err = Value::structure();
    err.field("message") = Value::fromString(msg, engine_.mr_);
    err.field("identifier") = Value::fromString(identifier, engine_.mr_);
    frame.R[th.exReg] = keel::move(err);
    frame.ip = th.catchIp;
    return true;
}

// Derive error context description from the opcode + operands at throw time.
// This is the VM equivalent of TreeWalker's describeNode() — zero storage overhead,
// only runs on the error path.
static keel::String describeInstruction(const Instruction &instr,
                                        const BytecodeChunk &chunk)
{
    switch (instr.op) {
    // Function calls
    case OpCode::CALL:
    case OpCode::CALL_FLATTEN:
    case OpCode::CALL_MULTI:
    case OpCode::CALL_FLATTEN_MULTI: {
        keel::i32 funcIdx = instr.d;
        if (funcIdx >= 0 && funcIdx < (keel::i32)chunk.strings.size())
            return keel::format("in call to '{}'", chunk.strings[funcIdx]);
        return keel::String::from("in function call");
    }
    case OpCode::CALL_BUILTIN: {
        const char *nm = builtinIdName(instr.d);
        return nm ? keel::format("in call to '{}'", nm)
                  : keel::String::from("in builtin call");
    }
    case OpCode::CALL_INDIRECT:
    case OpCode::CALL_INDIRECT_FLATTEN:
    case OpCode::CALL_INDIRECT_MULTI:
        return keel::String::from("in function call");
    case OpCode::CALL_METHOD:
    case OpCode::CALL_METHOD_MULTI: {
        keel::i32 nameIdx = instr.d;
        if (nameIdx >= 0 && nameIdx < (keel::i32)chunk.strings.size())
            return keel::format("in method call '.{}'", chunk.strings[nameIdx]);
        return keel::String::from("in method call");
    }

    // Cell indexing
    case OpCode::CELL_GET:
    case OpCode::CELL_GET_OR_CREATE:
    case OpCode::CELL_SET:
    case OpCode::CELL_GET_2D:
    case OpCode::CELL_SET_2D:
    case OpCode::CELL_GET_MULTI:
    case OpCode::CELL_GET_ND:
    case OpCode::CELL_SET_ND:
        return keel::String::from("in cell indexing");

    // Field access
    case OpCode::FIELD_GET:
    case OpCode::FIELD_GET_OR_CREATE:
    case OpCode::FIELD_SET: {
        keel::i32 nameIdx = instr.d;
        if (nameIdx >= 0 && nameIdx < (keel::i32)chunk.strings.size())
            return keel::format("in field access '.{}'", chunk.strings[nameIdx]);
        return keel::String::from("in field access");
    }
    case OpCode::FIELD_GET_DYN:
    case OpCode::FIELD_GET_OR_CREATE_DYN:
    case OpCode::FIELD_SET_DYN:
        return keel::String::from("in dynamic field access");
    case OpCode::STRUCT_ELEM_FIELD_SET: {
        keel::i32 nameIdx = instr.d;
        if (nameIdx >= 0 && nameIdx < (keel::i32)chunk.strings.size())
            return keel::format("in struct-array element write '.{}'", chunk.strings[nameIdx]);
        return keel::String::from("in struct-array element write");
    }
    case OpCode::STRUCT_ELEM_GET_OR_CREATE:
    case OpCode::STRUCT_ELEM_SET:
        return keel::String::from("in struct-array element access");

    // Binary operators (base and scalar-specialized variants share text)
    case OpCode::ADD:  case OpCode::ADD_SS:  return keel::String::from("in operator '+'");
    case OpCode::SUB:  case OpCode::SUB_SS:  return keel::String::from("in operator '-'");
    case OpCode::MUL:  case OpCode::MUL_SS:  return keel::String::from("in operator '*'");
    case OpCode::RDIV: case OpCode::RDIV_SS: return keel::String::from("in operator '/'");
    case OpCode::LDIV: return keel::String::from("in operator '\\'");
    case OpCode::POW:  case OpCode::POW_SS:  return keel::String::from("in operator '^'");
    case OpCode::EMUL: return keel::String::from("in operator '.*'");
    case OpCode::ERDIV:return keel::String::from("in operator './'");
    case OpCode::ELDIV:return keel::String::from("in operator '.\\'");
    case OpCode::EPOW: return keel::String::from("in operator '.^'");

    // Unary operators
    case OpCode::NEG:        case OpCode::NEG_S: return keel::String::from("in unary operator '-'");
    case OpCode::NOT:        return keel::String::from("in unary operator '~'");
    case OpCode::CTRANSPOSE:
    case OpCode::TRANSPOSE:  return keel::String::from("in transpose operator");

    // Colon expressions
    case OpCode::COLON:
    case OpCode::COLON3:
        return keel::String::from("in colon expression");

    // Matrix/cell construction
    case OpCode::HORZCAT:
    case OpCode::HORZCAT_APPEND:
    case OpCode::VERTCAT:
    case OpCode::VERTCAT_APPEND:
        return keel::String::from("in matrix construction");
    case OpCode::CELL_LITERAL:
        return keel::String::from("in cell construction");
    case OpCode::CLOSURE_MAKE:
        return keel::String::from("in anonymous function creation");

    // Indexing
    case OpCode::INDEX_GET:
    case OpCode::INDEX_GET_2D:
    case OpCode::INDEX_GET_ND:
    case OpCode::INDEX_SET:
    case OpCode::INDEX_SET_2D:
    case OpCode::INDEX_SET_ND:
    case OpCode::INDEX_DELETE:
    case OpCode::INDEX_DELETE_2D:
    case OpCode::INDEX_DELETE_ND:
        return keel::String::from("in array indexing");

    default:
        return keel::String{};
    }
}

[[noreturn]] void VM::enrichAndThrow(const std::exception &ex,
                                     const Instruction *ip,
                                     const BytecodeChunk &chunk)
{
    keel::usize instrIdx = keel::as<keel::usize>(ip - chunk.code.data());
    if (instrIdx < chunk.sourceMap.size() && chunk.sourceMap[instrIdx].line > 0) {
        keel::String context = describeInstruction(*ip, chunk);
        throw Error(ex.what(),
                    chunk.sourceMap[instrIdx].line,
                    chunk.sourceMap[instrIdx].col,
                    chunk.name,
                    context);
    }
    // No source location available — wrap in Error anyway for consistency
    throw Error(ex.what());
}

// ============================================================
// Debugger helpers
// ============================================================

DebugController *VM::debugCtl()
{
    return engine_.debugController_.get();
}

// ============================================================
// Frame management — non-recursive call/return
// ============================================================

Environment *VM::currentCallEnv()
{
    if (frames_.empty() || frames_.back().isScriptFrame)
        return inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv();
    auto &frame = frames_.back();
    if (!frame.env) {
        if (!envPool_.empty()) {
            frame.env = keel::move(envPool_.back());
            envPool_.pop_back();
            frame.env->reset(&engine_.constantsEnv(), engine_.globalsEnv_.get());
        } else {
            frame.env = keel::make_unique<Environment>(
                &engine_.constantsEnv(),
                engine_.globalsEnv_.get());
        }
    }
    return frame.env.get();
}

Environment *VM::materializeFrameEnv(CallFrame &f, const Value *frameRegs)
{
    if (f.isScriptFrame)
        return f.env ? f.env.get() : (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv());
    if (!f.env) {
        if (!envPool_.empty()) {
            f.env = keel::move(envPool_.back());
            envPool_.pop_back();
            f.env->reset(&engine_.constantsEnv(), engine_.globalsEnv_.get());
        } else {
            f.env = keel::make_unique<Environment>(
                &engine_.constantsEnv(),
                engine_.globalsEnv_.get());
        }
    }
    if (f.chunk && frameRegs) {
        if (!f.chunk->persistentNames.empty()) {
            f.env->bindPersistentBucket(engine_.persistentBucketPtr(f.chunk->name));
            for (const auto &pname : f.chunk->persistentNames)
                f.env->declarePersistent(pname);
        }
        for (const auto &gname : f.chunk->globalNames)
            f.env->declareGlobal(gname);

        exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, frameRegs,
                                         f.nregs, /*isTopLevel=*/false);

        keel::HashSet<keel::StringRef> gset;
        for (const auto &gn : f.chunk->globalNames)
            gset.insert(gn.as_ref());
        keel::HashSet<keel::StringRef> pset;
        for (const auto &pn : f.chunk->persistentNames)
            pset.insert(pn.as_ref());
        for (const auto &[vn, reg] : f.chunk->varMap) {
            if (gset.contains(vn.as_ref()) || pset.contains(vn.as_ref()))
                continue;
            if (!whoVisible(engine_, vn, *f.chunk) && vn != "nargin" && vn != "nargout")
                continue;
            Value v = readChunkVar(*f.chunk, frameRegs, f.nregs, vn, reg);
            if (!v.isUnset() && !v.isDeleted())
                f.env->setLocal(vn, v);
            else if (v.isDeleted())
                f.env->remove(vn);
        }
    }
    if (f.dynVars) {
        for (auto [dn, dv] : *f.dynVars) {
            if (!dv.isUnset() && !dv.isDeleted())
                f.env->setLocal(dn, dv);
        }
    }
    return f.env.get();
}

Environment *VM::callerEnvAtDepth(keel::i32 n)
{
    if (n < 0) n = 0;
    keel::i32 seen = 0;
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        if (f.isScriptFrame)
            continue;
        if (seen == n)
            return materializeFrameEnv(f, f.R);
        ++seen;
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            if (f.isScriptFrame)
                continue;
            if (seen == n) {
                const Value *psR = (f.regBase + f.nregs <= ps->regSnapshot.size())
                                       ? &ps->regSnapshot[f.regBase]
                                       : nullptr;
                return materializeFrameEnv(f, psR);
            }
            ++seen;
        }
    }
    return &engine_.workspaceEnv();
}

void VM::assignInCallerFrame(keel::i32 n, keel::StringRef name, const Value &val)
{
    if (n < 0) n = 0;
    keel::i32 seen = 0;
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        if (f.isScriptFrame)
            continue;
        if (seen == n) {
            if (f.chunk) {
                for (const auto &entry : f.chunk->varMap) {
                    if (entry.name == name && entry.reg < f.nregs) {
                        writeChunkVar(*f.chunk, f.R, f.nregs, entry.name, entry.reg, val);
                        break;
                    }
                }
            }
            if (f.env)
                writeToFrameMatchingEnv(f.env.get(), name, val);
            return;
        }
        ++seen;
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            if (f.isScriptFrame)
                continue;
            if (seen == n) {
                if (f.chunk && f.regBase + f.nregs <= ps->regSnapshot.size()) {
                    Value *psR = &ps->regSnapshot[f.regBase];
                    for (const auto &entry : f.chunk->varMap) {
                        if (entry.name == name && entry.reg < f.nregs) {
                            writeChunkVar(*f.chunk, psR, f.nregs, entry.name, entry.reg, val);
                            break;
                        }
                    }
                }
                if (f.env)
                    writeToFrameMatchingEnv(f.env.get(), name, val);
                return;
            }
            ++seen;
        }
    }
    // Walked off user-function frames (e.g. n == 0 in a top-level script): write to base frame(s)
    writeToFrameMatchingEnv(&engine_.workspaceEnv(), name, val);
}

void VM::writeToFrameMatchingEnv(Environment *env, keel::StringRef name,
                                 const Value &val)
{
    if (!env) return;
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        Environment *fEnv = f.env ? f.env.get()
                                  : (f.isScriptFrame
                                         ? (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv())
                                         : nullptr);
        if (fEnv != env) continue;
        if (f.chunk) {
            for (const auto &entry : f.chunk->varMap) {
                if (entry.name == name && entry.reg < f.nregs) {
                    writeChunkVar(*f.chunk, f.R, f.nregs, entry.name, entry.reg, val);
                    break;
                }
            }
        }
        if (f.dynVars)
            f.dynVars->put(keel::String::from(name), val);
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            Environment *fEnv = f.env ? f.env.get()
                                      : (f.isScriptFrame ? &engine_.workspaceEnv() : nullptr);
            if (fEnv != env) continue;
            if (f.chunk && f.regBase + f.nregs <= ps->regSnapshot.size()) {
                Value *psR = &ps->regSnapshot[f.regBase];
                for (const auto &entry : f.chunk->varMap) {
                    if (entry.name == name && entry.reg < f.nregs) {
                        writeChunkVar(*f.chunk, psR, f.nregs, entry.name, entry.reg, val);
                        break;
                    }
                }
            }
            if (f.dynVars)
                f.dynVars->put(keel::String::from(name), val);
        }
    }
}

void VM::syncFrameToEnv(Environment *env)
{
    if (!env) return;
    if (env == &engine_.workspaceEnv()) {
        flushWorkspaceFrame();
        return;
    }
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        if (!f.isScriptFrame && f.env.get() == env) {
            materializeFrameEnv(f, f.R);
            return;
        }
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            if (!f.isScriptFrame && f.env.get() == env) {
                const Value *psR = (f.regBase + f.nregs <= ps->regSnapshot.size())
                                       ? &ps->regSnapshot[f.regBase]
                                       : nullptr;
                materializeFrameEnv(f, psR);
                return;
            }
        }
    }
}

void VM::syncEnvRemovalsToFrame(Environment *env)
{
    if (!env) return;
    auto syncOne = [&](CallFrame &f, Value *regs) {
        if (!f.chunk || !regs) return;
        for (const auto &[vn, reg] : f.chunk->varMap) {
            if (reg >= f.nregs) continue;
            if (engine_.isReservedName(vn) && !f.chunk->assignedVars.contains(vn))
                continue;
            if (vn == "nargin" || vn == "nargout")
                continue;
            const bool stillPresent = (env->getLocal(vn) != nullptr)
                                      || (env->isGlobal(vn) && env->get(vn) != nullptr)
                                      || env->isPersistent(vn);
            if (!stillPresent) {
                Value cur = readChunkVar(*f.chunk, regs, f.nregs, vn, reg);
                if (!cur.isUnset() && !cur.isDeleted())
                    writeChunkVar(*f.chunk, regs, f.nregs, vn, reg, Value::deleted());
            }
        }
        if (f.dynVars) {
            keel::Array<keel::String> toRemove;
            for (auto [dk, dv] : *f.dynVars) {
                if (env->getLocal(dk) == nullptr && !env->isGlobal(dk)
                    && !env->isPersistent(dk))
                    toRemove.push_back(dk.clone());
            }
            for (const auto &dk : toRemove)
                f.dynVars->remove(dk);
        }
    };
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        auto &f = frames_[fi];
        Environment *fEnv = f.env ? f.env.get()
                                  : (f.isScriptFrame
                                         ? (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv())
                                         : nullptr);
        if (fEnv == env)
            syncOne(f, f.R);
    }
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        PausedState *ps = activePausedStates_[psi];
        if (!ps) continue;
        for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
            auto &f = ps->frames[fi];
            Environment *fEnv = f.env ? f.env.get()
                                      : (f.isScriptFrame ? &engine_.workspaceEnv() : nullptr);
            if (fEnv == env && f.regBase + f.nregs <= ps->regSnapshot.size())
                syncOne(f, &ps->regSnapshot[f.regBase]);
        }
    }
}

void VM::flushWorkspaceFrame()
{
    auto flushOne = [&](const CallFrame &f, const Value *regs, Environment &ws) {
        if (!f.chunk || !regs || f.env) return;
        for (const auto &entry : f.chunk->varMap) {
            if (entry.reg < f.nregs) {
                if (!whoVisible(engine_, entry.name, *f.chunk))
                    continue;
                Value v = readChunkVar(*f.chunk, regs, f.nregs, entry.name, entry.reg);
                if (!v.isUnset() && !v.isDeleted())
                    ws.set(entry.name, keel::move(v));
                else if (v.isDeleted())
                    ws.remove(entry.name);
            }
        }
        exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, regs, f.nregs,
                                         /*isTopLevel=*/true);
    };
    if (!frames_.empty() && frames_[0].isScriptFrame) {
        Environment &ws = inheritedScope_ ? *inheritedScope_ : engine_.workspaceEnv();
        flushOne(frames_[0], frames_[0].R, ws);
    }
    for (const PausedState *ps : activePausedStates_) {
        if (!ps || ps->frames.empty() || !ps->frames[0].isScriptFrame) continue;
        const auto &f0 = ps->frames[0];
        if (f0.regBase + f0.nregs <= ps->regSnapshot.size())
            flushOne(f0, &ps->regSnapshot[f0.regBase], engine_.workspaceEnv());
    }
}

keel::HashMap<keel::String, Value> VM::snapshotFrameVars(Environment *frameEnv)
{
    keel::HashMap<keel::String, Value> out;
    if (!frameEnv) return out;
    const CallFrame *match = nullptr;
    const Value *matchRegs = nullptr;
    for (keel::usize fi = frames_.size(); fi-- > 0;) {
        const auto &f = frames_[fi];
        Environment *fEnv = f.env ? f.env.get()
                                  : (f.isScriptFrame
                                         ? (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv())
                                         : nullptr);
        if (fEnv == frameEnv) { match = &f; matchRegs = f.R; break; }
    }
    if (!match) {
        for (keel::usize psi = activePausedStates_.size(); psi-- > 0 && !match;) {
            const PausedState *ps = activePausedStates_[psi];
            if (!ps) continue;
            for (keel::usize fi = ps->frames.size(); fi-- > 0;) {
                const auto &f = ps->frames[fi];
                Environment *fEnv = f.env ? f.env.get()
                                          : (f.isScriptFrame ? &engine_.workspaceEnv() : nullptr);
                if (fEnv == frameEnv && f.regBase + f.nregs <= ps->regSnapshot.size()) {
                    match = &f;
                    matchRegs = &ps->regSnapshot[f.regBase];
                    break;
                }
            }
        }
    }
    if (match && match->chunk && matchRegs) {
        for (const auto &entry : match->chunk->varMap) {
            if (entry.reg < match->nregs) {
                Value v = readChunkVar(*match->chunk, matchRegs, match->nregs, entry.name, entry.reg);
                if (!v.isUnset() && !v.isDeleted())
                    out.try_insert(entry.name.clone(), keel::move(v));
            }
        }
        if (match->dynVars) {
            for (auto [dk, dv] : *match->dynVars) {
                if (!dv.isUnset() && !dv.isDeleted())
                    out.put(dk.clone(), dv);
            }
        }
    }
    return out;
}

void VM::pushCallFrame(const BytecodeChunk &funcChunk, const Value *args, keel::usize nargs,
                       keel::u16 destReg, keel::usize nargout,
                       bool isMulti, keel::u16 outBase, keel::u16 nout,
                       keel::StringRef ownerClass, bool isCtor,
                       const Value *ctorSeed)
{
    if (callDepth() >= maxRecursion_)
        throw std::runtime_error("VM: maximum recursion depth exceeded");

    const keel::usize nUpvals = funcChunk.upvalueParamRegs.size();
    const keel::usize userNargs = (nargs >= nUpvals) ? (nargs - nUpvals) : nargs;

    // varargin: the LAST declared parameter absorbs extras (1xN cell,
    // empty 0x0 when none) — same semantics as TreeWalker.
    const bool va = !funcChunk.paramNames.empty()
                    && funcChunk.paramNames.back() == "varargin";
    if (!va && userNargs > keel::as<keel::usize>(funcChunk.numParams))
        throw std::runtime_error(
            keel::format("Too many input arguments for function '{}'", funcChunk.name).c_str());

    keel::u32 nregs = funcChunk.numRegisters;
    keel::usize pc = std::min(userNargs, keel::as<keel::usize>(funcChunk.numParams));

    // Copy args, upvalue boxes, varargin, and ctorSeed BEFORE ensureRegStackCapacity,
    // since `args` or `ctorSeed` may point into caller registers inside regStack_.
    // Use stack buffers for common arities to avoid heap allocation per call.
    Value smallArgs[16];
    keel::Array<Value> bigArgs;
    Value *argsPtr = (pc <= 16) ? smallArgs : (bigArgs.resize(pc), bigArgs.data());
    for (keel::usize i = 0; i < pc; ++i)
        argsPtr[i] = args[i];

    Value smallUpvals[8];
    keel::Array<Value> bigUpvals;
    Value *upvalsPtr = (nUpvals <= 8) ? smallUpvals : (bigUpvals.resize(nUpvals), bigUpvals.data());
    if (nUpvals > 0 && nargs >= nUpvals) {
        for (keel::usize i = 0; i < nUpvals; ++i)
            upvalsPtr[i] = args[userNargs + i];
    }

    Value vaCell;
    if (va) {
        const keel::usize required = funcChunk.numParams - 1;
        vaCell = userNargs > required
                     ? Value::cell(1, userNargs - required, engine_.mr_)
                     : Value::cell(0, 0, engine_.mr_);
        for (keel::usize i = required; i < userNargs; ++i)
            vaCell.cellAt(i - required) = args[i];
    }

    Value seedCopy;
    const bool hasSeed = (ctorSeed != nullptr && !funcChunk.returnNames.empty());
    if (hasSeed)
        seedCopy = *ctorSeed;

    ensureRegStackCapacity(regStackTop_ + nregs);

    // Allocate registers
    Value *newR = regStack_.data() + regStackTop_;
    for (keel::u32 i = 0; i < nregs; ++i)
        newR[i] = Value();

    for (keel::usize i = 0; i < nUpvals; ++i) {
        keel::u16 uvReg = funcChunk.upvalueParamRegs[i];
        if (uvReg < nregs)
            newR[uvReg] = keel::move(upvalsPtr[i]);
    }

    for (keel::usize i = 0; i < pc; ++i)
        newR[i] = keel::move(argsPtr[i]);

    if (va) {
        if (funcChunk.vararginReg < nregs) {
            newR[funcChunk.vararginReg] = keel::move(vaCell);
        } else {
            for (const auto &[vname, reg] : funcChunk.varMap)
                if (vname == "varargin" && reg < nregs) {
                    newR[reg] = keel::move(vaCell);
                    break;
                }
        }
    }

    // Inject nargin/nargout into function scope in O(1)
    if (funcChunk.narginReg < nregs)
        newR[funcChunk.narginReg] = Value::scalar(keel::as<keel::f64>(userNargs), nullptr);
    if (funcChunk.nargoutReg < nregs)
        newR[funcChunk.nargoutReg] = Value::scalar(keel::as<keel::f64>(nargout), nullptr);
    if (funcChunk.narginReg == kInvalidReg && funcChunk.nargoutReg == kInvalidReg) {
        for (const auto &[vname, reg] : funcChunk.varMap) {
            if (vname == "nargin" && reg < nregs)
                newR[reg] = Value::scalar(keel::as<keel::f64>(userNargs), nullptr);
            else if (vname == "nargout" && reg < nregs)
                newR[reg] = Value::scalar(keel::as<keel::f64>(nargout), nullptr);
        }
    }

    if (hasSeed) {
        if (funcChunk.firstReturnReg < nregs) {
            newR[funcChunk.firstReturnReg] = keel::move(seedCopy);
        } else {
            const keel::String &rn = funcChunk.returnNames[0];
            for (const auto &[vname, reg] : funcChunk.varMap)
                if (vname == rn && reg < nregs) {
                    newR[reg] = keel::move(seedCopy);
                    break;
                }
        }
    }

    // Sync caller's live global/persistent registers before importing into callee
    if (!frames_.empty()) {
        const auto &cf0 = frames_.back();
        if (cf0.chunk && (!cf0.chunk->globalNames.empty() || !cf0.chunk->persistentNames.empty())) {
            exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *cf0.chunk, cf0.R,
                                             cf0.nregs, cf0.isScriptFrame);
        }
    }

    // Import global and persistent variables (seed [] if uninitialized)
    if (!funcChunk.globalNames.empty() || !funcChunk.persistentNames.empty()) {
        importChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, funcChunk, newR, nregs,
                                         /*isTopLevel=*/false, /*seedMissing=*/true);
    }

    // Pre-compute caller's CALL-site arg names only when callee may inspect inputname(k).
    keel::Array<keel::String> callerArgNames;
    if (funcChunk.mayUseInputname && !frames_.empty()) {
        const auto &callerFrame = frames_.back();
        if (callerFrame.chunk && callerFrame.ip
            && callerFrame.ip > callerFrame.chunk->code.data()) {
            keel::usize callIdx = keel::as<keel::usize>(
                (callerFrame.ip - 1) - callerFrame.chunk->code.data());
            if (const auto *argNames = callerFrame.chunk->callSiteArgNames.get(
                    keel::as<keel::u32>(callIdx)))
                callerArgNames = argNames->clone();
        }
    }

    // m-file provenance: entering the body pushes the function's own
    // origin so mfilename()/sibling lookup see ITS file. A KNOWN-but-empty
    // origin (command-window-born function) masks the caller's file.
    // originDepth is captured before the push; every removal path
    // truncates back to it.
    const keel::usize originDepth = engine_.scriptOriginDepth();
    if (funcChunk.originKnown)
        engine_.pushScriptOrigin(funcChunk.originFs, funcChunk.originDir,
                                 funcChunk.originFile);

    // Push call frame
    CallFrame cf;
    cf.chunk = &funcChunk;
    cf.ip = funcChunk.code.data();
    cf.R = newR;
    cf.regBase = regStackTop_;
    cf.nregs = nregs;
    cf.forStackBase = forStack_.size();
    cf.tryStackBase = tryStack_.size();
    cf.destReg = destReg;
    cf.nargin = userNargs;
    cf.nargout = nargout;
    cf.isMultiReturn = isMulti;
    cf.outBase = outBase;
    cf.nout = nout;
    cf.ownerClass = keel::String::from(ownerClass);
    cf.isCtor = isCtor;
    cf.originDepth = originDepth;
    cf.callerArgNames = keel::move(callerArgNames);
    frames_.push_back(keel::move(cf));

    regStackTop_ += nregs;
    R_ = newR;

    // Push debug frame
    if (auto *ctl = debugCtl()) {
        StackFrame sf;
        sf.functionName = funcChunk.name.clone();
        sf.chunk = &funcChunk;
        sf.registers = newR;
        ctl->pushFrame(keel::move(sf));
    }
}

void VM::popCallFrame(Value retVal)
{
    CallFrame &frame = frames_.back();

    // State-machine callback: detach the continuation so we can route this
    // frame's result into it (instead of a caller register) after the frame's
    // bookkeeping is unwound below.
    keel::Arc<VmContinuation> cont = keel::move(frame.cont);
    // Captured before pop_back invalidates the reference.
    const keel::usize originDepth = frame.originDepth;

    // Update parent frame's registers BEFORE popFrame —
    // so onFunctionExit sees correct parent variables.
    if (auto *ctl = debugCtl()) {
        auto &stack = ctl->callStack();
        if (stack.size() >= 2 && frames_.size() >= 2)
            stack[stack.size() - 2].registers = frames_[frames_.size() - 2].R;
        ctl->popFrame();
    }

    bool isScriptFrame = frame.isScriptFrame;
    const bool isMultiReturn = frame.isMultiReturn;
    const keel::u16 outBase = frame.outBase;
    const keel::u16 nout = frame.nout;
    const keel::u16 destReg = frame.destReg;

    if (frame.chunk && (!frame.chunk->globalNames.empty() || !frame.chunk->persistentNames.empty())) {
        exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *frame.chunk, frame.R,
                                         frame.nregs, isScriptFrame);
    }

    if (isScriptFrame && frame.chunk)
        rebuildLocalVarMap(lastVarMap_, *frame.chunk, frame.R, frame.nregs);

    // Cleanup: release heap objects in frame
    for (keel::u32 i = 0; i < frame.nregs; ++i) {
        if (!frame.R[i].isDoubleScalar() && !frame.R[i].isEmpty())
            frame.R[i] = Value();
    }

    if (frame.env && !isScriptFrame && envPool_.size() < 32) {
        frame.env->reset(nullptr, nullptr);
        envPool_.push_back(keel::move(frame.env));
    }

    // Restore stack pointers
    regStackTop_ = frame.regBase;
    forStack_.resize(std::min(forStack_.size(), frame.forStackBase));
    tryStack_.resize(std::min(tryStack_.size(), frame.tryStackBase));

    frames_.pop_back();
    engine_.truncateScriptOrigins(originDepth);

    if (cont) {
        R_ = frames_.empty() ? nullptr : frames_.back().R;
        cont->step(*this, &retVal, cont);
        return;
    }

    if (!frames_.empty()) {
        CallFrame &caller = frames_.back();
        R_ = caller.R;
        if (caller.chunk && (!caller.chunk->globalNames.empty() || !caller.chunk->persistentNames.empty())) {
            importChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *caller.chunk, caller.R,
                                             caller.nregs, caller.isScriptFrame,
                                             /*seedMissing=*/false);
        }

        if (isMultiReturn) {
            keel::usize produced = !returnBuf_.empty()
                                   ? returnBuf_.size()
                                   : (retVal.isUnset() ? 0u : 1u);
            if (produced < nout) {
                returnBuf_.clear();
                throw std::runtime_error("Too many output arguments.");
            }
            if (!returnBuf_.empty()) {
                for (keel::usize i = 0; i < nout && i < returnBuf_.size(); ++i)
                    caller.R[outBase + i] = keel::move(returnBuf_[i]);
                returnBuf_.clear();
            } else {
                caller.R[outBase] = keel::move(retVal);
                for (keel::usize i = 1; i < nout; ++i)
                    caller.R[outBase + i] = Value();
            }
        } else {
            caller.R[destReg] = keel::move(retVal);
            returnBuf_.clear();
        }
    } else {
        R_ = nullptr;
        lastResult_ = keel::move(retVal);
    }
}

bool VM::startContinuation(keel::Arc<VmContinuation> cont)
{
    // First step: no prior callback result yet.
    return cont->step(*this, nullptr, cont);
}

bool VM::pushCallbackFrame(const Value &handle, keel::ArrayRef<Value> args, keel::usize nargout,
                           keel::Arc<VmContinuation> cont)
{
    // Unwrap a closure cell {handle, captures…}: captures append to the args,
    // matching the [user_params…, captures…] chunk parameter layout (same as
    // callFunctionHandleMulti).
    if (!handle.isFuncHandle())
        return false;
    keel::Array<Value> withCaptures;
    if (handle.funcHandleCaptureCount() > 0) {
        const keel::usize ncaps = handle.funcHandleCaptureCount();
        withCaptures.reserve(args.size() + ncaps);
        for (const auto &a : args)
            withCaptures.push_back(a);
        for (keel::usize i = 0; i < ncaps; ++i)
            withCaptures.push_back(handle.funcHandleCapture(i));
        args = keel::ArrayRef<Value>(withCaptures.data(), withCaptures.size());
    }
    const keel::StringRef fhName = handle.funcHandleNameRef();
    const UserFunction *uf = engine_.lookupUserFunctionLocal(fhName);
    if (!uf)
        uf = engine_.lookupUserFunction(fhName, currentCallEnv());
    if (!uf) {
        if (const BytecodeChunk *fc = findCompiledFunc(fhName)) {
            pushCallFrame(*fc, args.data(), args.size(), /*destReg=*/0, nargout,
                          /*isMulti=*/false, 0, 0, keel::StringRef{}, /*isCtor=*/false, /*ctorSeed=*/nullptr);
            frames_.back().cont = keel::move(cont);
            return true;
        }
        return false; // not user code — caller must use the synchronous path
    }
    const BytecodeChunk *cc = engine_.ensureClassMethodChunk(*uf);
    if (!cc)
        return false;
    pushCallFrame(*cc, args.data(), args.size(), /*destReg=*/0, nargout,
                  /*isMulti=*/false, 0, 0, keel::StringRef{}, /*isCtor=*/false, /*ctorSeed=*/nullptr);
    frames_.back().cont = keel::move(cont);
    return true;
}

bool LoopContinuation::step(VM &vm, Value *prevResult,
                            const keel::Arc<VmContinuation> &self)
{
    if (prevResult) {
        results.push_back(keel::move(*prevResult));
        ++i;
    }
    if (i >= n) {
        *dest = pack ? pack(results) : Value();
        return false; // finished — output written
    }
    keel::Array<Value> a = makeArgs(i);
    return vm.pushCallbackFrame(handle, keel::ArrayRef<Value>(a.data(), a.size()), 1, self);
}

void VM::setFrameDynVars(keel::HashMap<keel::String, Value> *dv)
{
    if (!frames_.empty())
        frames_.back().dynVars = dv;
}

void VM::forSetVar(Value &varReg, const ForState &fs)
{
    if (fs.lazy) {
        // Lazy colon range: compute scalar from start + index*step.
        // start/stop bounds are validated at FOR_INIT_RANGE time, so
        // here we just emit the value with no allocation.
        const keel::f64 v = fs.lazyStart + keel::as<keel::f64>(fs.index) * fs.lazyStep;
        if (varReg.isDoubleScalar())
            varReg.setScalarFast(v);
        else
            varReg.setScalarVal(v);
        return;
    }
    if (fs.rows == 0) {
        // Scalar range — only one iteration, use safe path
        if (varReg.isDoubleScalar())
            varReg.setScalarFast(fs.range.scalarVal());
        else
            varReg.setScalarVal(fs.range.scalarVal());
        return;
    }

    // Fast double row-vector / matrix path
    if (fs.rangeType == ValueType::DOUBLE && fs.data != nullptr) {
        if (fs.rows == 1) {
            if (varReg.isDoubleScalar())
                varReg.setScalarFast(fs.data[fs.index]);
            else
                varReg.setScalarVal(fs.data[fs.index]);
            return;
        }
        keel::usize rows = fs.rows;
        auto col = Value::matrix(rows, 1, ValueType::DOUBLE, engine_.mr_);
        keel::f64 *dst = col.doubleDataMut();
        const keel::f64 *src = fs.data + fs.index * rows;
        for (keel::usize r = 0; r < rows; ++r)
            dst[r] = src[r];
        varReg = std::move(col);
        return;
    }

    // Unified path for all other ValueTypes (SINGLE, INT8..UINT64, COMPLEX,
    // LOGICAL, CHAR, CELL, STRING, STRUCT, OBJECT) and N-D arrays.
    if (fs.rows == 1) {
        varReg = fs.range.elemAt(fs.index, engine_.mr_);
        return;
    }
    keel::usize colIdx = fs.index;
    varReg = fs.range.indexGet2D(nullptr, fs.rows, &colIdx, 1, engine_.mr_);
}

} // namespace numkit
