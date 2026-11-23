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
#include <numkit/value/error.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/index.hpp>

#include <algorithm>
#include <cstdio>

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


// Fast scalar check for VM arithmetic — accepts double scalars AND logical scalar tags
static inline bool isArithScalar(const Value &v)
{
    return v.isDoubleScalar() || v.isLogicalScalar();
}

static inline keel::f64 asScalar(const Value &v)
{
    return v.fastScalarVal();
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
                R[I.a] = collapseCsl(keel::move(R[I.b]));
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
                            R[I.a] = keel::move(out[0]);
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
                        throw numkit::Error("NaN values cannot be converted to logicals.");
                    R[I.a].setLogicalFast(bv != 0.0 && cv != 0.0);
                } else
                    { if (tryBinaryOpFrame(I.a, I.op, I.b, I.c, frame, ip)) goto enter_frame;
                          R[I.a] = binarySlowPath(I.op, R[I.b], R[I.c]); }
                break;
            case OpCode::OR:
                if (R[I.b].isDoubleScalar() && R[I.c].isDoubleScalar()) {
                    keel::f64 bv = R[I.b].scalarVal(), cv = R[I.c].scalarVal();
                    if (keel::is_nan(bv) || keel::is_nan(cv))
                        throw numkit::Error("NaN values cannot be converted to logicals.");
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
                        throw numkit::Error("NaN values cannot be converted to logicals.");
                    R[I.a].setLogicalFast(bv == 0.0);
                } else
                    { if (tryUnaryOpFrame(I.a, I.op, I.b, frame, ip)) goto enter_frame;
                          R[I.a] = unarySlowPath(I.op, R[I.b]); }
                break;
            case OpCode::TO_LOGICAL_SCALAR: {
                const Value &src = R[I.b];
                if (!src.isScalar() || src.isEmpty() || src.isUnset()) {
                    throw numkit::Error(
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
                        R[I.a] = keel::move(fout);
                        ip += keel::as<keel::i32>(I.e) + 1;
                        continue;
                    }
                }
                break;  // disabled or declined → execute the fallback opcodes
            }

            // ── Control flow ─────────────────────────────────────
            case OpCode::JMP:
                if (I.d < 0)
                    engine_.checkCancelled();
                ip += I.d;
                continue;
            case OpCode::JMP_TRUE:
                if (R[I.a].isLogicalScalar()) {
                    if (R[I.a].isLogicalTrue()) {
                        if (I.d < 0)
                            engine_.checkCancelled();
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
                    if (I.d < 0)
                        engine_.checkCancelled();
                    ip += I.d;
                    continue;
                }
                break;
            case OpCode::JMP_FALSE:
                if (R[I.a].isLogicalScalar()) {
                    if (R[I.a].isLogicalFalse()) {
                        if (I.d < 0)
                            engine_.checkCancelled();
                        ip += I.d;
                        continue;
                    }
                    break;
                }
                if (R[I.a].isEmpty() && !R[I.a].isUnset()
                    && (R[I.a].isNumeric() || R[I.a].isLogical() || R[I.a].isChar())) {
                    if (I.d < 0)
                        engine_.checkCancelled();
                    ip += I.d;
                    continue;
                }
                if (!R[I.a].toBool()) {
                    if (I.d < 0)
                        engine_.checkCancelled();
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
                forStack_.push_back(keel::move(fs));
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
                                    rangeVal = keel::move(out[0]);
                                }
                            }
                        }
                        if (rangeVal.isUnset()) {
                            ValueType ta = R[I.b].type(), tb = R[I.c].type();
                            ValueType t = (ta != ValueType::DOUBLE) ? ta
                                        : (tb != ValueType::DOUBLE) ? tb
                                        : ValueType::DOUBLE;
                            if (ta != ValueType::DOUBLE && tb != ValueType::DOUBLE && ta != tb)
                                throw numkit::Error(
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
                                    rangeVal = keel::move(out[0]);
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
                                    throw numkit::Error(
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
                        fs.range = keel::move(rangeVal);
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
                    forStack_.push_back(keel::move(fs));
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
                forStack_.push_back(keel::move(fs));
                forSetVar(R[I.a], forStack_.back());
                break;
            }
            case OpCode::FOR_NEXT: {
                auto &fs = forStack_.back();
                fs.index++;
                if (fs.index < fs.count) {
                    engine_.checkCancelled();
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
                            R[I.a] = keel::move(out[0]);
                            break;
                        }
                    }
                }
                ValueType ta = R[I.b].type(), tb = R[I.c].type();
                ValueType t = (ta != ValueType::DOUBLE) ? ta
                            : (tb != ValueType::DOUBLE) ? tb
                            : ValueType::DOUBLE;
                if (ta != ValueType::DOUBLE && tb != ValueType::DOUBLE && ta != tb)
                    throw numkit::Error(
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
                            R[I.a] = keel::move(out[0]);
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
                        throw numkit::Error(
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
                                R[I.a] = keel::move(out[0]);
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
                            R[I.a] = keel::move(out[0]);
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
                                R[I.a] = keel::move(out[0]);
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
                            R[I.a] = keel::move(out[0]);
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
                        throw numkit::Error(
                            keel::format("Reference to non-existent field '{}'", fname).c_str());
                    elems.reserve(n + 1);
                    for (keel::usize i = 0; i < n; ++i) {
                        const auto &m = src.structArrayElem(i);
                        const Value *vp = m.get(fname);
                        if (!vp)
                            throw numkit::Error(
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
                            throw numkit::Error(
                                keel::format("No appropriate property '{}' for class '{}'",
                                             fname, src.objectClassNameRef())
                                    .c_str());
                        elems.push_back(keel::move(out));
                    }
                } else {
                    throw numkit::Error(
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
                        R[I.a] = keel::move(out);
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
                    R[I.a] = keel::move(res);
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
                        R[I.a] = keel::move(out);
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
                        throw numkit::Error("Index exceeds array dimensions");
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
                    keel::usize i = checkedIndex(ix.scalarVal(), keel::Limits<keel::usize>::kMax);
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
                    throw numkit::Error("Struct index rank exceeds maximum supported rank");
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
                        throw numkit::Error("Index exceeds object array bounds");
                    R[I.a] = obj.objectSubArray(keel::ArrayRef<keel::usize>(&linear, 1), engine_.mr_);
                    break;
                }
                if (obj.isUnset() || obj.isEmpty())
                    obj = Value::structArray(0, 0, engine_.allocator());
                if (!obj.isStruct())
                    throw numkit::Error("Indexed assignment on a non-struct value");
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
                    throw numkit::Error("Struct index rank exceeds maximum supported rank");
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
                        throw numkit::Error("Index exceeds object array bounds");
                    if (!obj.objectIsHandle())
                        obj.objectAssignLinear(keel::ArrayRef<keel::usize>(&linear, 1),
                                               R[I.e], R[I.e], engine_.mr_);
                    break;
                }
                if (obj.isUnset() || obj.isEmpty())
                    obj = Value::structArray(0, 0, engine_.allocator());
                if (!obj.isStruct())
                    throw numkit::Error("Indexed assignment on a non-struct value");
                keel::usize linear;
                if (nargs == 1) {
                    linear = coords[0];
                    obj.growStructArrayTo(linear, engine_.allocator());
                } else {
                    linear = obj.growStructArrayND(coords, nargs, engine_.allocator());
                }
                const Value &val = R[I.e];
                if (!val.isStruct())
                    throw numkit::Error("Cannot assign a non-struct to a struct-array element");
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
                R[I.a] = keel::move(fh);
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
                    throw numkit::Error(
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
                R[I.a] = keel::move(cell);
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
                            R[I.a] = keel::move(out[0]);
                            break;
                        }
                    }
                }
                if (!R[I.b].isCell())
                    throw numkit::Error("Cell indexing requires a cell array");
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
                    R[I.a] = keel::move(csl);
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
                            R[I.a] = keel::move(out[0]);
                            break;
                        }
                    }
                }
                if (!R[I.b].isCell())
                    throw numkit::Error("Cell indexing requires a cell array");
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
                    R[I.a] = keel::move(csl);
                }
                break;
            }
            case OpCode::CELL_GET_MULTI: {
                if (!R[I.b].isCell())
                    throw numkit::Error("Cell indexing requires a cell array");
                auto indices = Value::resolveIndices(R[I.c], R[I.b].numel());
                keel::u16 outBase = I.a, nout = I.e;
                for (keel::u16 i = 0; i < nout && i < indices.size(); ++i)
                    R[outBase + i] = R[I.b].cellAt(indices[i]);
                break;
            }
            case OpCode::CELL_GET_MULTI_2D: {
                if (!R[I.b].isCell())
                    throw numkit::Error("Cell indexing requires a cell array");
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
                    throw numkit::Error("Cell indexing requires a cell array");
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
                    R[I.a].cellAt(i) = keel::move(R[I.c]);
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
                    throw numkit::Error("Cell indexing requires a cell array");
                keel::usize r = Value::checkedScalarIndex(R[I.b].toScalar()), c = Value::checkedScalarIndex(R[I.c].toScalar());
                // Auto-grow if needed
                keel::usize nr = R[I.a].dims().rows(), nc = R[I.a].dims().cols();
                if (r + 1 > nr || c + 1 > nc) {
                    keel::usize newR = std::max(nr, r + 1), newC = std::max(nc, c + 1);
                    auto grown = Value::cell(newR, newC);
                    for (keel::usize cc = 0; cc < nc; ++cc)
                        for (keel::usize rr = 0; rr < nr; ++rr)
                            grown.cellAt(cc * newR + rr) = R[I.a].cellAt(cc * nr + rr);
                    R[I.a] = keel::move(grown);
                }
                keel::usize lin = R[I.a].dims().sub2ind(r, c);
                if (!lvalDetaches_.empty()
                    && lvalDetaches_.back().kind == LvalDetach::Kind::Cell
                    && lvalDetaches_.back().containerRegAbs == frame.regBase + I.a
                    && lvalDetaches_.back().valRegAbs == frame.regBase + I.e
                    && lvalDetaches_.back().cellLinearIdx == lin) {
                    lvalDetaches_.pop_back();
                    R[I.a].cellAt(lin) = keel::move(R[I.e]);
                } else {
                    R[I.a].cellAt(lin) = R[I.e];
                }
                break;
            }
            case OpCode::CELL_GET_ND: {
                // a=dst, b=cell, c=base, e=ndims
                if (!R[I.b].isCell())
                    throw numkit::Error("Cell indexing requires a cell array");
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
                    throw numkit::Error("Cell indexing requires a cell array");
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
                        R[I.a] = keel::move(grown);
                    }
                    keel::usize idx = R[I.a].dims().sub2ind(r, c, p);
                    if (!lvalDetaches_.empty()
                        && lvalDetaches_.back().kind == LvalDetach::Kind::Cell
                        && lvalDetaches_.back().containerRegAbs == frame.regBase + I.a
                        && lvalDetaches_.back().valRegAbs == frame.regBase + I.e
                        && lvalDetaches_.back().cellLinearIdx == idx) {
                        lvalDetaches_.pop_back();
                        R[I.a].cellAt(idx) = keel::move(R[I.e]);
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
                        R[I.a].cellAt(idx) = keel::move(R[I.e]);
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
                frame.ip = ip;
                if (execCallNamedSingleSpan(I.a, I.d, I.e,
                                            I.c ? &R[I.b] : nullptr, I.c,
                                            R, frame, ip, resolvedFuncs))
                    goto enter_frame;
                break;
            }

            case OpCode::CALL_FLATTEN: {
                frame.ip = ip;
                SplatArgs splat = splatCallArgs(R, I.b, I.c);
                if (execCallNamedSingleSpan(I.a, I.d, I.e,
                                            splat.data(), splat.size(),
                                            R, frame, ip, resolvedFuncs))
                    goto enter_frame;
                break;
            }

            case OpCode::CALL_MULTI:
            case OpCode::CALL_FLATTEN_MULTI: {
                frame.ip = ip;
                SplatArgs splat = splatCallArgs(R, I.b, I.c);
                if (execCallNamedMultiSpan(I.a, I.d, I.e,
                                           splat.data(), splat.size(),
                                           R, frame, ip, resolvedFuncs))
                    goto enter_frame;
                break;
            }

            // ── Indirect function call (func handle) or array indexing ─
            case OpCode::CALL_METHOD: {
                frame.ip = ip;
                if (execCallMethod(I, R, frame, ip))
                    goto enter_frame;
                break;
            }
            case OpCode::CALL_METHOD_MULTI: {
                frame.ip = ip;
                if (execCallMethodMulti(I, R, frame, ip))
                    goto enter_frame;
                break;
            }
            // ── Superclass calls inside a classdef body ──
            case OpCode::CALL_SUPER_CTOR: {
                frame.ip = ip;
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
                frame.ip = ip;
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
                frame.ip = ip;
                if (execCallIndirect(I, R, frame, ip))
                    goto enter_frame;
                break;

            case OpCode::CALL_INDIRECT_MULTI:
                frame.ip = ip;
                if (execCallIndirectMulti(I, R, frame, ip))
                    goto enter_frame;
                break;

            case OpCode::CALL_INDIRECT_FLATTEN: {
                frame.ip = ip;
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
                    throw numkit::Error(
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
                            throw numkit::Error("Too many output arguments.");
                    for (keel::u16 i = 0; i < fnout; ++i)
                        R[I.a + i] = keel::move(ob[i]);
                    break;
                }
                throw numkit::Error(
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

            case OpCode::ASSERT_DEF: {
                if (I.c == 0 && !R[I.a].isUnset() && !R[I.a].isDeleted())
                    break;
                const keel::String &nn = chunk.strings[I.d];
                if (!inReentrantEval_ && (nn == "nargin" || nn == "nargout"))
                    throw numkit::Error(
                        "You can only call nargin/nargout from within a function.");
                // When I.c == 0, R[I.a] may be deleted (explicitly cleared in this frame):
                // check dynamic/env/parent only if R[I.a] wasn't explicitly cleared here,
                // wait — ASSERT_DEF for cleared vars still falls back to functions below.
                if (auto dynVal = lookupDynamicVar(chunk, frame, /*R=*/nullptr, nn, /*checkParentFunc=*/true)) {
                    R[I.a] = keel::move(*dynVal);
                    break;
                }
                const keel::usize nout = I.e;
                auto resolved = engine_.resolveCallable(nn, currentCallEnv(),
                                                        chunk.fileSubfuncs.get());
                if (resolved.kind == Engine::ResolvedCallable::Kind::UserFunc) {
                    const BytecodeChunk *found = findCompiledFunc(resolved.userFunc->name);
                    if (!found)
                        found = engine_.ensureClassMethodChunk(*resolved.userFunc);
                    frame.ip = ip + 1;
                    pushCallFrame(*found, nullptr, 0, I.a, nout);
                    goto enter_frame;
                }
                if (resolved.kind == Engine::ResolvedCallable::Kind::External) {
                    Value ob[1];
                    keel::MutArrayRef<Value> os(ob, 1);
                    CallContext ctx{&engine_, currentCallEnv()};
                    (*resolved.extFunc)(keel::ArrayRef<Value>(nullptr, 0), nout, os, ctx);
                    R = frames_.back().R;
                    R[I.a] = keel::move(ob[0]);
                    break;
                }
                if (resolved.kind == Engine::ResolvedCallable::Kind::ClassCtor) {
                    CallContext ctx{&engine_, currentCallEnv()};
                    R[I.a] = engine_.constructChecked(resolved.builtinClass,
                                                      keel::ArrayRef<Value>(nullptr, 0), ctx);
                    R = frames_.back().R;
                    break;
                }
                throw numkit::Error(
                    keel::format("Undefined function or variable '{}'", nn).c_str());
            }

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
                throw numkit::Error(
                    keel::format("VM: unimplemented opcode {}", (keel::i32) I.op).c_str());

            } // switch
            ++ip;
        } // while
    } catch (const DebugStopException &) {
        rollbackLvalDetaches();
        throw; // pass through — not a user error
    } catch (Error &mle) {
        rollbackLvalDetaches();
        frame.ip = ip;
        keel::String id = mle.identifier().empty()
                              ? keel::String::from("numkit:error")
                              : mle.identifier().clone();
        if (id != "numkit:interrupted" && dispatchTryCatch(mle.what(), id.c_str()))
            goto enter_frame;
        enrichError(mle, ip, chunk);
        if (stopOnError_ && debugCtl()) {
            // dbstop if error: pause at the failing instruction with frames
            // intact; resumeExecution() rethrows the stored exception later.
            errorPauseMsg_.assign(mle.what());
            pausedError_ = std::current_exception();
            atErrorPause_ = true;
            return ExecStatus::Paused;
        }
        throw;
    } catch (const std::exception &ex) {
        rollbackLvalDetaches();
        frame.ip = ip;
        if (dispatchTryCatch(ex.what(), "numkit:error"))
            goto enter_frame;
        if (stopOnError_ && debugCtl()) {
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


} // namespace numkit
