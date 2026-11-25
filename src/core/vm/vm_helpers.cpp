// src/core/vm_helpers.cpp
// Extracted cold-path VM dispatch helpers: slow-path operators, object
// subsref/subsasgn frame pushers, named & indirect call dispatch, indirect
// indexing, and display/who/whos formatting.
#include <keel/array.h>
#include <keel/algo.h>
#include <keel/memory.h>
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
#include "core/vm/vm.hpp"
#include <numkit/value/error.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/index.hpp>
#include "core/vm/vm_internal.hpp"


namespace numkit {

namespace {

struct RegDestContinuation : VmContinuation
{
    keel::Arc<VmContinuation> inner;
    keel::Arc<Value> outBox;
    keel::usize targetRegOffset = 0;

    RegDestContinuation(keel::Arc<VmContinuation> in,
                        keel::Arc<Value> box,
                        keel::usize offset)
        : inner(keel::move(in))
        , outBox(keel::move(box))
        , targetRegOffset(offset)
    {}

    bool step(VM &vm, Value *prevResult,
              const keel::Arc<VmContinuation> &self) override
    {
        if (inner->step(vm, prevResult, self))
            return true;
        vm.writeRegAtOffset(targetRegOffset, keel::move(*outBox));
        return false;
    }
};

} // namespace

// ============================================================
// Extracted dispatch helpers
// ============================================================

// Opcode → MATLAB operator token (the form Engine's operator tables key on).
// Shared by the slow paths and the object-operator frame-push helpers so the
// mapping has a single source of truth.
static const char *binaryOpString(OpCode op)
{
    switch (op) {
    case OpCode::ADD:   return "+";
    case OpCode::SUB:   return "-";
    case OpCode::MUL:   return "*";
    case OpCode::RDIV:  return "/";
    case OpCode::LDIV:  return "\\";
    case OpCode::POW:   return "^";
    case OpCode::EMUL:  return ".*";
    case OpCode::ERDIV: return "./";
    case OpCode::ELDIV: return ".\\";
    case OpCode::EPOW:  return ".^";
    case OpCode::EQ:    return "==";
    case OpCode::NE:    return "~=";
    case OpCode::LT:    return "<";
    case OpCode::GT:    return ">";
    case OpCode::LE:    return "<=";
    case OpCode::GE:    return ">=";
    case OpCode::AND:   return "&";
    case OpCode::OR:    return "|";
    default:            return nullptr;
    }
}

static const char *unaryOpString(OpCode op)
{
    switch (op) {
    case OpCode::NEG:        return "-";
    case OpCode::NOT:        return "~";
    case OpCode::CTRANSPOSE: return "'";
    case OpCode::TRANSPOSE:  return ".'";
    default:                 return nullptr;
    }
}

Value VM::binarySlowPath(OpCode op, const Value &lhs, const Value &rhs)
{
    const char *opStr = binaryOpString(op);
    // OBJECT operator overloading: dispatch to the dominant object's class
    // `ops` before the numeric builtin path (throws if no overload).
    if (opStr && (lhs.isObject() || rhs.isObject())) {
        Value out;
        if (engine_.tryObjectBinaryOp(opStr, lhs, rhs, currentCallEnv(), out))
            return out;
    }
    if (opStr) {
        if (const auto *fn = engine_.binaryOps_.get(opStr))
            return (*fn)(lhs, rhs);
    }
    throw numkit::Error("VM: unsupported binary op");
}

Value VM::unarySlowPath(OpCode op, const Value &operand)
{
    const char *opStr = unaryOpString(op);
    // OBJECT unary operator overloading — before the numeric builtin path.
    if (opStr && operand.isObject()) {
        Value out;
        if (engine_.tryObjectUnaryOp(opStr, operand, currentCallEnv(), out))
            return out;
    }
    if (opStr) {
        if (const auto *fn = engine_.unaryOps_.get(opStr))
            return (*fn)(operand);
    }
    throw numkit::Error("VM: unsupported unary op");
}

bool VM::tryBinaryOpFrame(keel::u16 dst, OpCode op, keel::u16 lhsReg, keel::u16 rhsReg,
                          CallFrame &frame, const Instruction *ip)
{
    Value *R = frame.R;
    if (!R[lhsReg].isObject() && !R[rhsReg].isObject())
        return false; // numeric fast path — no object operand
    const char *opStr = binaryOpString(op);
    if (!opStr)
        return false;
    keel::String ownerClass;
    const BytecodeChunk *cc =
        engine_.resolveBinaryOpChunk(opStr, R[lhsReg], R[rhsReg], ownerClass);
    if (!cc)
        return false; // no user overload / uncompilable → caller's slow path
    Value argbuf[2] = {R[lhsReg], R[rhsReg]}; // operator params ARE the operands
    frame.ip = ip + 1;
    pushCallFrame(*cc, argbuf, 2, dst, 1, false, 0, 0, ownerClass, /*isCtor=*/false);
    return true;
}

bool VM::tryUnaryOpFrame(keel::u16 dst, OpCode op, keel::u16 operandReg,
                         CallFrame &frame, const Instruction *ip)
{
    Value *R = frame.R;
    if (!R[operandReg].isObject())
        return false;
    const char *opStr = unaryOpString(op);
    if (!opStr)
        return false;
    keel::String ownerClass;
    const BytecodeChunk *cc = engine_.resolveUnaryOpChunk(opStr, R[operandReg], ownerClass);
    if (!cc)
        return false;
    Value argbuf[1] = {R[operandReg]};
    frame.ip = ip + 1;
    pushCallFrame(*cc, argbuf, 1, dst, 1, false, 0, 0, ownerClass, /*isCtor=*/false);
    return true;
}

bool VM::tryObjectSubsrefFrame(keel::u16 dst, keel::u16 selfReg, keel::ArrayRef<Value> idx,
                               CallFrame &frame, const Instruction *ip)
{
    Value *R = frame.R;
    if (!R[selfReg].isObject())
        return false;
    return tryObjectSubsrefFrameObj(R[selfReg], dst, idx, frame, ip);
}

bool VM::tryObjectSubsrefFrameObj(const Value &self, keel::u16 dst, keel::ArrayRef<Value> idx,
                                  CallFrame &frame, const Instruction *ip)
{
    if (!self.isObject())
        return false;
    keel::String ownerClass;
    keel::Array<Value> args;
    const BytecodeChunk *cc = engine_.resolveSubsrefChunk(self, idx, ownerClass, args);
    if (!cc)
        return false;
    frame.ip = ip + 1;
    pushCallFrame(*cc, args.data(), args.size(), dst, 1, false, 0, 0,
                  ownerClass, /*isCtor=*/false);
    return true;
}

bool VM::tryObjectSubsasgnFrame(keel::u16 objReg, keel::ArrayRef<Value> idxAndVal,
                                CallFrame &frame, const Instruction *ip)
{
    Value *R = frame.R;
    if (!R[objReg].isObject())
        return false;
    keel::String ownerClass;
    keel::Array<Value> args;
    const BytecodeChunk *cc = engine_.resolveSubsasgnChunk(R[objReg], idxAndVal, ownerClass, args);
    if (!cc)
        return false;
    // subsasgn returns the modified object → write back into the object register
    // (value class); a handle mutates shared state and returns itself.
    frame.ip = ip + 1;
    pushCallFrame(*cc, args.data(), args.size(), objReg, 1, false, 0, 0,
                  ownerClass, /*isCtor=*/false);
    return true;
}

bool VM::execIndexSet1DSlow(const Instruction &I, Value *R,
                            CallFrame &frame, const Instruction *ip)
{
    const Value &ix = R[I.b];
    if (ix.isObject()) {
        Value out;
        if (engine_.tryObjectSubscriptAssign(keel::ArrayRef<Value>(&ix, 1),
                                             R[I.c], currentCallEnv(), out)) {
            R[I.a] = keel::move(out);
            return false;
        }
    }
    if (R[I.a].isObject()) {
        Value sargs[2] = {ix, R[I.c]};
        if (tryObjectSubsasgnFrame(I.a, keel::ArrayRef<Value>(sargs, 2), frame, ip))
            return true;
        if (engine_.tryObjectSubsasgn(R[I.a], keel::ArrayRef<Value>(sargs, 2), currentCallEnv()))
            return false;
    }
    if (engine_.prepareObjectArrayStore(R[I.a], R[I.c])) {
        Value &dst = R[I.a];
        keel::Array<keel::usize> pos = resolveStoreSubscript(ix, dst.objectCount());
        engine_.objectStoreSlice(
            dst, keel::ArrayRef<keel::Array<keel::usize>>(&pos, 1),
            R[I.c], currentCallEnv());
        return false;
    }
    if (R[I.a].isObject())
        throw numkit::Error(
            keel::format("'()' assignment is not defined for class '{}'",
                         R[I.a].objectClassNameRef())
                .c_str());

    if (ix.isDoubleScalar()) {
        keel::usize i = Value::checkedScalarIndex(ix.scalarVal());
        Value &dst = R[I.a];
        const Value &rhs = R[I.c];
        if (rhs.isStruct() && (dst.isUnset() || dst.isEmpty() || dst.isStruct())) {
            if (dst.isUnset() || dst.isEmpty())
                dst = Value::structArray(0, 0, engine_.mr_);
            dst.growStructArrayTo(i, engine_.mr_);
            for (const auto &name : rhs.fieldNamesInOrder())
                dst.setField(i, name, rhs.field(name));
            return false;
        }
        if (rhs.isScalar() && !rhs.isComplex()
            && i == dst.numel()
            && dst.heapRefCount() == 1
            && (dst.isEmpty() || (dst.isHeapDouble() && dst.dims().rows() == 1))) {
            dst.appendScalar(rhs.toScalar(), engine_.mr_);
            return false;
        }
        if (rhs.isScalar() && (dst.isEmpty() || dst.isScalar() || i >= dst.numel()))
            dst.ensureSize(i, engine_.mr_);
        dst.elemSet(i, rhs);
        return false;
    }

    if (ix.isChar() && ix.numel() == 1 && ix.charData()[0] == ':') {
        Value &dst = R[I.a];
        Value &rhs = R[I.c];
        const keel::usize n = dst.numel();
        const bool sameCount = (rhs.numel() == n);

        if (rhs.isScalar() && !rhs.isComplex()
            && dst.type() == ValueType::DOUBLE && dst.isHeapDouble()) {
            keel::fill_n(dst.doubleDataMut(), n, rhs.toScalar());
            return false;
        }
        if (sameCount
            && dst.hasHeap() && rhs.hasHeap()
            && dst.heapRefCount() == 1 && rhs.heapRefCount() == 1
            && dst.type() == rhs.type()
            && (dst.type() == ValueType::DOUBLE || dst.type() == ValueType::COMPLEX)
            && dst.isComplex() == rhs.isComplex()) {
            dst.swapHeapBufferUnchecked(rhs);
            return false;
        }
        if (sameCount
            && dst.type() == ValueType::DOUBLE && dst.isHeapDouble()
            && rhs.type() == ValueType::DOUBLE && !rhs.isComplex()) {
            keel::copy_n(dst.doubleDataMut(), rhs.doubleData(), n);
            return false;
        }
        if (sameCount && dst.isComplex() && rhs.isComplex()) {
            keel::copy_n(dst.complexDataMut(), rhs.complexData(), n);
            return false;
        }
        auto indices = Value::resolveIndices(ix, n);
        dst.indexSet(indices.data(), indices.size(), rhs);
        return false;
    }

    auto indices = Value::resolveIndicesUnchecked(ix);
    if (!indices.empty() && (R[I.c].isScalar() || R[I.c].numel() == indices.size())) {
        keel::usize maxIdx = 0;
        for (keel::usize idx : indices) maxIdx = keel::max(maxIdx, idx);
        if (R[I.a].isEmpty() || maxIdx >= R[I.a].numel())
            R[I.a].ensureSize(maxIdx, engine_.mr_);
    }
    R[I.a].indexSet(indices.data(), indices.size(), R[I.c]);
    return false;
}

bool VM::execIndexSet2DSlow(const Instruction &I, Value *R,
                            CallFrame &frame, const Instruction *ip)
{
    const Value &ri = R[I.b];
    const Value &ci = R[I.c];
    const Value &val = R[I.e];

    if (ri.isObject() || ci.isObject()) {
        Value idxArgs[2] = {ri, ci};
        Value out;
        if (engine_.tryObjectSubscriptAssign(keel::ArrayRef<Value>(idxArgs, 2),
                                             val, currentCallEnv(), out)) {
            R[I.a] = keel::move(out);
            return false;
        }
    }
    if (R[I.a].isObject()) {
        Value sargs[3] = {ri, ci, val};
        if (tryObjectSubsasgnFrame(I.a, keel::ArrayRef<Value>(sargs, 3), frame, ip))
            return true;
        if (engine_.tryObjectSubsasgn(R[I.a], keel::ArrayRef<Value>(sargs, 3), currentCallEnv()))
            return false;
    }
    if (engine_.prepareObjectArrayStore(R[I.a], val)) {
        Value &dst = R[I.a];
        const keel::usize rDim = (dst.isObject() && dst.objectCount() > 0)
                                ? dst.dims().rows() : val.dims().rows();
        const keel::usize cDim = (dst.isObject() && dst.objectCount() > 0)
                                ? dst.dims().cols() : val.numel();
        keel::Array<keel::usize> perDim[2] = {
            resolveStoreSubscript(ri, rDim),
            resolveStoreSubscript(ci, cDim)};
        engine_.objectStoreSlice(
            dst, keel::ArrayRef<keel::Array<keel::usize>>(perDim, 2),
            val, currentCallEnv());
        return false;
    }
    if (R[I.a].isObject())
        throw numkit::Error(
            keel::format("'()' assignment is not defined for class '{}'",
                         R[I.a].objectClassNameRef())
                .c_str());

    auto rowIds = ResolvedSubscript::resolveUnchecked(ri);
    auto colIds = ResolvedSubscript::resolveUnchecked(ci);
    bool riIsColon = rowIds.isColon;
    bool ciIsColon = colIds.isColon;

    const keel::usize curRows = R[I.a].dims().rows();
    const keel::usize curColDim = R[I.a].dims().ndim() > 2
        ? (curRows > 0 ? R[I.a].dims().numel() / curRows : R[I.a].dims().cols())
        : R[I.a].dims().cols();

    keel::usize maxR = rowIds.maxPlusOne(curRows);
    keel::usize maxC = colIds.maxPlusOne(curColDim);
    if (R[I.a].isEmpty()) {
        if (riIsColon && ciIsColon) {
            maxR = val.dims().rows();
            maxC = val.dims().cols();
        } else if (riIsColon) {
            maxR = (colIds.count == 1) ? val.numel() : val.dims().rows();
        } else if (ciIsColon) {
            maxC = (rowIds.count == 1) ? val.numel() : val.dims().cols();
        }
    }
    if (maxR > curRows || maxC > curColDim) {
        const keel::usize propR = keel::max(curRows, maxR);
        const keel::usize propC = keel::max(curColDim, maxC);
        const keel::usize targetCnt =
            (riIsColon ? propR : rowIds.count) * (ciIsColon ? propC : colIds.count);
        if (val.isScalar() || val.numel() == targetCnt)
            R[I.a].resize(maxR, maxC, engine_.mr_);
    }

    const keel::usize finalRows = R[I.a].dims().rows();
    const keel::usize finalColDim = R[I.a].dims().ndim() > 2
        ? (finalRows > 0 ? R[I.a].dims().numel() / finalRows : R[I.a].dims().cols())
        : R[I.a].dims().cols();

    R[I.a].indexSet2D(rowIds.data(),
                      riIsColon ? finalRows : rowIds.count,
                      colIds.data(),
                      ciIsColon ? finalColDim : colIds.count, val);
    return false;
}

bool VM::execIndexSetND(const Instruction &I, Value *R,
                        CallFrame &frame, const Instruction *ip)
{
    keel::u16 base = I.b, ndims = I.c;
    {
        bool anyObj = false;
        for (keel::u16 i = 0; i < ndims; ++i) {
            if (R[base + i].isObject()) {
                anyObj = true;
                break;
            }
        }
        if (anyObj) {
            Value out;
            if (engine_.tryObjectSubscriptAssign(keel::ArrayRef<Value>(&R[base], ndims),
                                                 R[I.e], currentCallEnv(), out)) {
                R[I.a] = keel::move(out);
                return false;
            }
        }
    }
    if (R[I.a].isObject()) {
        keel::Array<Value> sargs;
        sargs.reserve(ndims + 1);
        for (keel::u16 i = 0; i < ndims; ++i)
            sargs.push_back(R[base + i]);
        sargs.push_back(R[I.e]);
        if (tryObjectSubsasgnFrame(I.a, sargs.as_ref(), frame, ip))
            return true;
        if (engine_.tryObjectSubsasgn(R[I.a], sargs.as_ref(), currentCallEnv()))
            return false;
    }
    if (engine_.prepareObjectArrayStore(R[I.a], R[I.e])) {
        Value &dst = R[I.a];
        const Value &val = R[I.e];
        keel::Array<keel::Array<keel::usize>> perDim;
        perDim.resize(ndims);
        for (keel::u16 i = 0; i < ndims; ++i) {
            keel::usize curDim =
                dst.isObject()
                    ? (i < dst.dims().ndim() ? dst.dims().dim(i) : 1)
                    : 0;
            perDim[i] = resolveStoreSubscript(R[base + i], curDim);
        }
        engine_.objectStoreSlice(dst, perDim, val, currentCallEnv());
        return false;
    }
    if (R[I.a].isObject())
        throw numkit::Error(
            keel::format("'()' assignment is not defined for class '{}'",
                         R[I.a].objectClassNameRef())
                .c_str());

    if (ndims == 3) {
        auto rowIds = ResolvedSubscript::resolveUnchecked(R[base]);
        auto colIds = ResolvedSubscript::resolveUnchecked(R[base + 1]);
        auto pageIds = ResolvedSubscript::resolveUnchecked(R[base + 2]);
        bool riColon = rowIds.isColon;
        bool ciColon = colIds.isColon;
        bool piColon = pageIds.isColon;

        const Value &val = R[I.e];
        keel::usize maxR = rowIds.maxPlusOne(R[I.a].dims().rows());
        keel::usize maxC = colIds.maxPlusOne(R[I.a].dims().cols());
        keel::usize maxP = pageIds.maxPlusOne(R[I.a].dims().pages());
        if (R[I.a].isEmpty()
            && (!val.isScalar()
                || (R[I.a].dims().rows() == 0 && R[I.a].dims().cols() == 0 && !val.isEmpty()))) {
            const keel::i32 nColons = (riColon ? 1 : 0) + (ciColon ? 1 : 0) + (piColon ? 1 : 0);
            const keel::usize nonColonProd = (riColon ? 1 : rowIds.count)
                                      * (ciColon ? 1 : colIds.count)
                                      * (piColon ? 1 : pageIds.count);
            if (nColons == 1 && nonColonProd == 1 && val.dims().isVector()) {
                if (riColon) maxR = val.numel();
                else if (ciColon) maxC = val.numel();
                else if (piColon) maxP = val.numel();
            } else if (riColon && ciColon && piColon) {
                maxR = val.dims().rows();
                maxC = val.dims().cols();
                maxP = val.dims().pages();
            } else if (nColons > 0) {
                keel::usize rCursor = 0;
                keel::i32 colonsLeft = nColons;
                auto nextRhsDim = [&]() -> keel::usize {
                    while (rCursor < keel::as<keel::usize>(val.dims().ndim())
                           && val.dims().dim(keel::as<keel::i32>(rCursor)) == 1
                           && (keel::as<keel::usize>(val.dims().ndim()) - rCursor) > keel::as<keel::usize>(colonsLeft)) {
                        ++rCursor;
                    }
                    --colonsLeft;
                    return (rCursor < keel::as<keel::usize>(val.dims().ndim()))
                               ? val.dims().dim(keel::as<keel::i32>(rCursor++))
                               : 1;
                };
                if (riColon) maxR = nextRhsDim();
                if (ciColon) maxC = nextRhsDim();
                if (piColon) maxP = nextRhsDim();
            }
        }
        if (maxR > R[I.a].dims().rows() || maxC > R[I.a].dims().cols()
            || maxP > R[I.a].dims().pages())
            R[I.a].resize3d(maxR, maxC, maxP, engine_.mr_);

        R[I.a].indexSet3D(rowIds.data(),
                          riColon ? R[I.a].dims().rows() : rowIds.count,
                          colIds.data(),
                          ciColon ? R[I.a].dims().cols() : colIds.count,
                          pageIds.data(),
                          piColon ? R[I.a].dims().pages() : pageIds.count,
                          val);
        return false;
    }

    const keel::i32 nd = keel::as<keel::i32>(ndims);
    const keel::i32 curNd = R[I.a].dims().ndim();
    const keel::i32 newNd = keel::max(nd, curNd);
    keel::Array<keel::usize> need(newNd, keel::as<keel::usize>(1));
    for (keel::i32 i = 0; i < newNd; ++i)
        need[i] = (i < curNd) ? R[I.a].dims().dim(i) : 1;
    auto isColon = [](const Value &v) {
        return v.isChar() && v.numel() == 1 && v.charData()[0] == ':';
    };
    for (keel::i32 i = 0; i < nd; ++i) {
        const Value &iv = R[base + i];
        if (isColon(iv) || iv.isLogical()) continue;
        if (iv.isDoubleScalar()) {
            keel::usize v = keel::as<keel::usize>(iv.toScalar());
            if (v > need[i]) need[i] = v;
        } else if (iv.type() == ValueType::DOUBLE) {
            const keel::f64 *d = iv.doubleData();
            for (keel::usize k = 0; k < iv.numel(); ++k) {
                keel::usize v = keel::as<keel::usize>(d[k]);
                if (v > need[i]) need[i] = v;
            }
        }
    }
    bool grow = (newNd > curNd);
    for (keel::i32 i = 0; i < curNd && !grow; ++i)
        if (need[i] > R[I.a].dims().dim(i)) grow = true;
    if (grow)
        R[I.a].resizeND(need.data(), newNd, engine_.mr_);

    const keel::usize ndu = keel::as<keel::usize>(nd);
    keel::Array<keel::Array<keel::usize>> idxLists(ndu);
    keel::Array<const keel::usize *> idxPtrs(ndu);
    keel::Array<keel::usize> idxCounts(ndu);
    for (keel::i32 i = 0; i < nd; ++i) {
        const keel::usize lim = (i < R[I.a].dims().ndim()) ? R[I.a].dims().dim(i) : 1;
        idxLists[i] = Value::resolveIndices(R[base + i], lim);
        idxPtrs[i] = idxLists[i].data();
        idxCounts[i] = idxLists[i].size();
    }
    R[I.a].indexSetND(idxPtrs.data(), idxCounts.data(), nd, R[I.e]);
    return false;
}

bool VM::execFieldGet(keel::u16 dstReg, keel::u16 objReg, keel::StringRef fname,
                      Value *R, CallFrame &frame, const Instruction *ip)
{
    if (R[objReg].isObject()) {
        const keel::StringRef cn = R[objReg].objectClassNameRef();
        if (const UserFunction *g = engine_.classGetter(cn, fname)) {
            if (const BytecodeChunk *gc = engine_.ensureClassMethodChunk(*g)) {
                engine_.enforcePropGetAccess(cn, fname);
                Value selfBuf = R[objReg];
                frame.ip = ip + 1;
                pushCallFrame(*gc, &selfBuf, 1, dstReg, 1, false, 0, 0, cn, /*isCtor=*/false);
                return true;
            }
        }
        R[dstReg] = engine_.objectPropGet(R[objReg], fname, currentCallEnv());
        return false;
    }
    if (!R[objReg].isStruct())
        throw numkit::Error("Dot indexing requires a struct");
    if (R[objReg].isStructArray() || R[objReg].numel() == 0) {
        keel::usize n = R[objReg].numel();
        if (n == 0 && !R[objReg].hasField(fname))
            throw numkit::Error(
                keel::format("Reference to non-existent field '{}'", fname).c_str());
        Value csl = Value::csl(n, engine_.mr_);
        for (keel::usize i = 0; i < n; ++i) {
            const auto &m = R[objReg].structArrayElem(i);
            const Value *vp = m.get(fname);
            if (!vp)
                throw numkit::Error(
                    keel::format("Reference to non-existent field '{}'", fname).c_str());
            csl.cslAt(i) = *vp;
        }
        R[dstReg] = keel::move(csl);
        return false;
    }
    if (!R[objReg].hasField(fname))
        throw numkit::Error(
            keel::format("Reference to non-existent field '{}'", fname).c_str());
    R[dstReg] = R[objReg].field(fname);
    return false;
}

void VM::execFieldGetOrCreate(keel::u16 dstReg, keel::u16 objReg, keel::StringRef fname,
                              Value *R)
{
    if (R[objReg].isObject()) {
        const BuiltinClass *cls = engine_.findClass(R[objReg].objectClassNameRef());
        if (cls && cls->propGet) {
            CallContext ctx{&engine_, currentCallEnv()};
            Value out;
            if (cls->propGet(R[objReg], fname, out, ctx)) {
                R[dstReg] = keel::move(out);
                return;
            }
        }
        throw numkit::Error(
            keel::format("No appropriate property '{}' for class '{}'",
                         fname, R[objReg].objectClassNameRef())
                .c_str());
    }
    if (R[objReg].isEmpty())
        R[objReg] = Value::structure();
    if (!R[objReg].isStruct())
        throw numkit::Error("Dot indexing requires a struct");
    R[dstReg] = R[objReg].field(fname);
}

bool VM::execFieldSet(keel::u16 objReg, keel::u16 valReg, keel::StringRef fname,
                      Value *R, CallFrame &frame, const Instruction *ip)
{
    if (R[objReg].isObject()) {
        const keel::StringRef cn = R[objReg].objectClassNameRef();
        if (const UserFunction *s = engine_.classSetter(cn, fname)) {
            if (const BytecodeChunk *sc = engine_.ensureClassMethodChunk(*s)) {
                engine_.enforcePropSetAccess(cn, fname);
                Value argbuf[2] = {R[objReg], R[valReg]};
                if (!s->returns.empty()) {
                    frame.ip = ip + 1;
                    pushCallFrame(*sc, argbuf, 2, objReg, 1, false, 0, 0, cn, /*isCtor=*/false);
                    return true;
                }
                callReentrant(*sc, keel::ArrayRef<Value>(argbuf, 2), 1, cn, /*isCtor=*/false);
                return false;
            }
        }
        engine_.objectPropSet(R[objReg], fname, R[valReg], currentCallEnv());
        return false;
    }
    if (R[objReg].isEmpty())
        R[objReg] = Value::structure();
    if (!R[objReg].isStruct())
        throw numkit::Error("Dot indexing requires a struct");
    if (R[objReg].isStructArray()) {
        R[objReg].setFieldAll(fname, R[valReg]);
    } else if (!lvalDetaches_.empty()
               && lvalDetaches_.back().kind == LvalDetach::Kind::Field
               && lvalDetaches_.back().containerRegAbs == frame.regBase + objReg
               && lvalDetaches_.back().valRegAbs == frame.regBase + valReg
               && lvalDetaches_.back().fieldName == fname) {
        lvalDetaches_.pop_back();
        R[objReg].field(fname) = keel::move(R[valReg]);
    } else {
        R[objReg].field(fname) = R[valReg];
    }
    return false;
}

bool VM::execCallMethod(const Instruction &I, Value *R,
                        CallFrame &frame, const Instruction *ip)
{
    const BytecodeChunk &chunk = *frame.chunk;
    const keel::String &mname = chunk.strings[I.d];
    const Value &obj = R[I.b];
    SplatArgs splat = splatCallArgs(R, I.c, I.e);
    const Value *argPtr = splat.data();
    keel::usize argN = splat.size();
    if (obj.isObject()) {
        const BuiltinClass *cls = engine_.findClass(obj.objectClassNameRef());
        if (cls) {
            if (const auto *mfp = cls->methodFns.get(mname)) {
                const BytecodeChunk *mc = engine_.ensureClassMethodChunk(**mfp);
                if (mc) {
                    engine_.enforceMethodAccess(obj.objectClassNameRef(), mname);
                    keel::Array<Value> frameArgs;
                    frameArgs.reserve(argN + 1);
                    frameArgs.push_back(obj);
                    for (keel::usize k = 0; k < argN; ++k)
                        frameArgs.push_back(argPtr[k]);
                    frame.ip = ip + 1;
                    pushCallFrame(*mc, frameArgs.data(), frameArgs.size(), I.a, 1, false,
                                  0, 0, (*mfp)->ownerClass, false);
                    return true;
                }
            }
            if (const auto *mfn = cls->methods.get(mname)) {
                Value self = obj;
                keel::ArrayRef<Value> args(argPtr, argN);
                Value out[1];
                CallContext ctx{&engine_, currentCallEnv()};
                (*mfn)(self, args, 1, keel::MutArrayRef<Value>(out, 1), ctx);
                R[I.a] = keel::move(out[0]);
                return false;
            }
        }
        Value out;
        CallContext ctx{&engine_, currentCallEnv()};
        if (!cls || !cls->propGet || !cls->propGet(obj, mname, out, ctx))
            throw numkit::Error(
                keel::format("No appropriate property '{}' for class '{}'",
                             mname, obj.objectClassNameRef())
                    .c_str());
        return execCallIndirectSpan(out, I.a, argPtr, argN, R, frame, ip);
    }
    if (!obj.isStruct())
        throw numkit::Error("Dot indexing requires a struct");
    if (!obj.hasField(mname))
        throw numkit::Error(
            keel::format("Reference to non-existent field '{}'", mname).c_str());
    Value fv = obj.field(mname);
    return execCallIndirectSpan(fv, I.a, argPtr, argN, R, frame, ip);
}

bool VM::execCallMethodMulti(const Instruction &I, Value *R,
                             CallFrame &frame, const Instruction *ip)
{
    const BytecodeChunk &chunk = *frame.chunk;
    keel::u16 outBase = I.a, objReg = I.b, argBase = I.c;
    keel::u16 na = keel::as<keel::u16>((I.e >> 8) & 0xFF);
    keel::u16 nout = keel::as<keel::u16>(I.e & 0xFF);
    const keel::String &mname = chunk.strings[I.d];
    Value &obj = R[objReg];
    if (!obj.isObject())
        throw numkit::Error(
            keel::format("Dot-indexing multi-output requires an object; '{}' is not a method of a {}",
                         mname, mtypeName(obj.type()))
                .c_str());
    SplatArgs splat = splatCallArgs(R, argBase, na);
    const Value *argPtr = splat.data();
    keel::usize argN = splat.size();
    const BuiltinClass *cls = engine_.findClass(obj.objectClassNameRef());
    if (cls) {
        if (const auto *mfp = cls->methodFns.get(mname)) {
            const BytecodeChunk *mc = engine_.ensureClassMethodChunk(**mfp);
            if (mc) {
                engine_.enforceMethodAccess(obj.objectClassNameRef(), mname);
                keel::Array<Value> frameArgs;
                frameArgs.reserve(argN + 1);
                frameArgs.push_back(obj);
                for (keel::usize k = 0; k < argN; ++k)
                    frameArgs.push_back(argPtr[k]);
                frame.ip = ip + 1;
                returnBuf_.clear();
                pushCallFrame(*mc, frameArgs.data(), frameArgs.size(), 0, nout, true,
                              outBase, nout, (*mfp)->ownerClass, false);
                return true;
            }
        }
    }
    const ObjectMethod *mfn = cls ? cls->methods.get(mname) : nullptr;
    if (!mfn)
        throw numkit::Error(
            keel::format("No appropriate method '{}' for class '{}'",
                         mname, obj.objectClassNameRef())
                .c_str());
    Value self = obj;
    keel::ArrayRef<Value> args(argPtr, argN);
    keel::Array<Value> outBuf(nout);
    CallContext ctx{&engine_, currentCallEnv()};
    (*mfn)(self, args, nout, keel::MutArrayRef<Value>(outBuf.data(), nout), ctx);
    for (keel::u16 i = 0; i < nout; ++i)
        if (outBuf[i].isUnset())
            throw numkit::Error("Too many output arguments.");
    for (keel::usize i = 0; i < nout; ++i)
        R[outBase + i] = keel::move(outBuf[i]);
    return false;
}

void VM::execCallBuiltin(const Instruction &I, Value *R)
{
    keel::u16 argBase = I.b, na = I.c;
    const FastBuiltinId bid = static_cast<FastBuiltinId>(I.d);

    // 1-arg scalar fast path
    if (na == 1 && R[argBase].isDoubleScalar()) {
        keel::f64 v = R[argBase].scalarVal();
        keel::f64 result;
        bool handled = true;
        switch (bid) {
        case FastBuiltinId::Abs:   result = keel::abs(v); break;
        case FastBuiltinId::Floor: result = keel::floor(v); break;
        case FastBuiltinId::Ceil:  result = keel::ceil(v); break;
        case FastBuiltinId::Round: result = keel::round(v); break;
        case FastBuiltinId::Fix:   result = keel::trunc(v); break;
        // sqrt / log / log2 / log10 of a negative scalar promote to a
        // complex result. The scalar fast path can only hold a real double,
        // so for v < 0 we bail and let the full builtin produce complex.
        case FastBuiltinId::Sqrt:  if (v < 0.0) { handled = false; break; } result = keel::sqrt(v);  break;
        case FastBuiltinId::Exp:   result = keel::exp(v); break;
        case FastBuiltinId::Log:   if (v < 0.0) { handled = false; break; } result = keel::log(v);   break;
        case FastBuiltinId::Log2:  if (v < 0.0) { handled = false; break; } result = keel::log2(v);  break;
        case FastBuiltinId::Log10: if (v < 0.0) { handled = false; break; } result = keel::log10(v); break;
        case FastBuiltinId::Sin:   result = keel::sin(v); break;
        case FastBuiltinId::Cos:   result = keel::cos(v); break;
        case FastBuiltinId::Tan:   result = keel::tan(v); break;
        case FastBuiltinId::Sign:
            result = keel::is_nan(v) ? keel::Limits<keel::f64>::quiet_nan()
                     : (v > 0)     ? 1.0
                     : (v < 0)     ? -1.0
                                   : 0.0;
            break;
        case FastBuiltinId::IsNan: R[I.a].setLogicalFast(keel::is_nan(v)); return;
        case FastBuiltinId::IsInf: R[I.a].setLogicalFast(keel::is_inf(v)); return;
        default: handled = false; break;
        }
        if (handled) {
            R[I.a].setScalarFast(result);
            return;
        }
    }

    // 2-arg scalar fast path
    if (na == 2 && R[argBase].isDoubleScalar() && R[argBase + 1].isDoubleScalar()) {
        keel::f64 a = R[argBase].scalarVal(), b = R[argBase + 1].scalarVal();
        keel::f64 result;
        bool handled = true;
        switch (bid) {
        case FastBuiltinId::Mod:
            if (b == 0.0) { result = a; break; }
            result = keel::fmod(a, b);
            if (result != 0.0 && ((result > 0) != (b > 0)))
                result += b;
            break;
        case FastBuiltinId::Rem:   result = keel::fmod(a, b); break;
        case FastBuiltinId::Max:   result = keel::fmax(a, b); break;
        case FastBuiltinId::Min:   result = keel::fmin(a, b); break;
        case FastBuiltinId::Pow:   result = keel::pow(a, b); break;
        case FastBuiltinId::Atan2: result = keel::atan2(a, b); break;
        default: handled = false; break;
        }
        if (handled) {
            R[I.a].setScalarFast(result);
            return;
        }
    }

    // Generic fallback via externalFuncs_
    const char *fname = fastBuiltinName(bid);
    if (fname) {
        if (na >= 1 && R[argBase].isObject()) {
            const BuiltinClass *cls = engine_.findClass(R[argBase].objectClassNameRef());
            if (cls) {
                if (const auto *mfn = cls->methods.get(fname)) {
                    Value self = R[argBase];
                    keel::ArrayRef<Value> rest((na > 1) ? &R[argBase + 1] : nullptr, na - 1);
                    Value out[1];
                    CallContext ctx{&engine_, currentCallEnv()};
                    (*mfn)(self, rest, 1, keel::MutArrayRef<Value>(out, 1), ctx);
                    R[I.a] = keel::move(out[0]);
                    return;
                }
            }
        }
        const ExternalFunc *fnPtr = engine_.findExternal(
            fname, currentCallEnv());
        if (fnPtr) {
            keel::ArrayRef<Value> as(&R[argBase], na);
            Value ob[1];
            // Output-reuse hint — same logic as the generic CALL path.
            // Hand the destination's current value to the adapter via
            // outs[0] when no arg register aliases R[I.a]; opt-in
            // adapters (NUMKIT_UNARY_ADAPTER_HINT) reuse its buffer
            // instead of allocating fresh.
            bool canHint = R[I.a].hasHeap();
            for (keel::u16 i = 0; i < na && canHint; ++i)
                if (&R[argBase + i] == &R[I.a])
                    canHint = false;
            if (canHint)
                ob[0] = keel::move(R[I.a]);
            keel::MutArrayRef<Value> os(ob, 1);
            CallContext ctx{&engine_, currentCallEnv()};
            (*fnPtr)(as, 1, os, ctx);
            R[I.a] = keel::move(ob[0]);
            return;
        }
    }
    throw numkit::Error("VM: unsupported builtin");
}

keel::Optional<Value> VM::lookupDynamicVar(const BytecodeChunk &chunk, const CallFrame &frame,
                                           const Value *R, keel::StringRef name,
                                           bool checkParentFunc) const
{
    // If this chunk explicitly cleared a locally assigned variable, do not
    // resurrect it from dynVars or workspaceEnv.
    if (R && chunk.assignedVars.contains(name)) {
        for (const auto &[vn, reg] : chunk.varMap) {
            if (vn == name && reg < chunk.numRegisters) {
                Value rv = readChunkVar(chunk, R, chunk.numRegisters, vn, reg);
                if (rv.isDeleted())
                    return keel::kNullOpt;
                break;
            }
        }
    }
    if (frame.dynVars) {
        if (const Value *dvp = frame.dynVars->get(name);
            dvp && !dvp->isUnset() && !dvp->isDeleted())
            return *dvp;
    }
    if (frame.isScriptFrame) {
        Environment &scopeEnv = inheritedScope_ ? *inheritedScope_ : engine_.workspaceEnv();
        if (const Value *wv = scopeEnv.get(name); wv && !wv->isUnset() && !wv->isDeleted())
            return *wv;
    } else if (frame.env) {
        if (const Value *lv = frame.env->getLocal(name); lv && !lv->isUnset() && !lv->isDeleted())
            return *lv;
    }
    if (checkParentFunc && !chunk.parentFuncName.empty()) {
        for (keel::i32 fi = keel::as<keel::i32>(frames_.size()) - 2; fi >= 0; --fi) {
            const CallFrame &pf = frames_[fi];
            if (!pf.chunk || pf.chunk->name != chunk.parentFuncName)
                continue;
            for (const auto &[vname, reg] : pf.chunk->varMap) {
                if (vname == name && reg < pf.nregs) {
                    Value v = readChunkVar(*pf.chunk, pf.R, pf.nregs, vname, reg);
                    if (!v.isUnset() && !v.isDeleted())
                        return v;
                    break;
                }
            }
        }
        for (keel::i32 psi = keel::as<keel::i32>(activePausedStates_.size()) - 1; psi >= 0; --psi) {
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
                    if (vname == name && reg < pf.nregs) {
                        Value v = readChunkVar(*pf.chunk, psR, pf.nregs, vname, reg);
                        if (!v.isUnset() && !v.isDeleted())
                            return v;
                        break;
                    }
                }
            }
        }
    }
    return keel::kNullOpt;
}

const Value *VM::findDynLocalVar(const BytecodeChunk &chunk, const CallFrame &frame,
                                 const Value *R, const keel::String &name) const
{
    if (!frame.dynVars && !(frame.env && frame.env->hasLocals()) && !frame.isScriptFrame)
        return nullptr;
    if (chunk.assignedVars.contains(name)) {
        for (const auto &[vn, reg] : chunk.varMap) {
            if (vn == name && reg < chunk.numRegisters) {
                Value rv = readChunkVar(chunk, R, chunk.numRegisters, vn, reg);
                if (rv.isDeleted())
                    return nullptr;
                break;
            }
        }
    }
    if (frame.dynVars) {
        if (const Value *dvp = frame.dynVars->get(name);
            dvp && !dvp->isUnset() && !dvp->isDeleted())
            return dvp;
    }
    const Value *dynLocal = nullptr;
    if (frame.isScriptFrame) {
        Environment &scopeEnv = inheritedScope_ ? *inheritedScope_ : engine_.workspaceEnv();
        dynLocal = scopeEnv.getLocal(name);
    } else if (frame.env) {
        dynLocal = frame.env->getLocal(name);
    }
    if (dynLocal && (dynLocal->isUnset() || dynLocal->isDeleted()))
        return nullptr;
    return dynLocal;
}

bool VM::execCallNamedSingleSpan(keel::u16 dstReg, keel::i32 funcIdx, keel::u16 nargout_val,
                                 const Value *AB, keel::usize NF, Value *&R,
                                 CallFrame &frame, const Instruction *ip,
                                 keel::Array<CallCacheEntry> &resolvedFuncs)
{
    const BytecodeChunk &chunk = *frame.chunk;
    const keel::String &funcName = chunk.strings[funcIdx];

    // Dynamically injected local variable (via eval/load/assignin)
    // shadows functions and builtins.
    if (const Value *dynLocal = findDynLocalVar(chunk, frame, R, funcName)) {
        Value target = *dynLocal;
        if (NF == 0 && !target.isFuncHandle()) {
            R[dstReg] = keel::move(target);
            return false;
        }
        return execCallIndirectSpan(target, dstReg, AB, NF, R, frame, ip);
    }

    const keel::u64 curEpoch = engine_.callCacheEpoch();
    const Environment *scopeForImports = frame.isScriptFrame
        ? (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv())
        : frame.env.get();
    const bool hasImports = scopeForImports && scopeForImports->hasAnyActiveImports();
    const bool originStable = chunk.originKnown
        || !engine_.currentScriptDir()
        || engine_.currentScriptDir()->empty();

    // Try resolved inline cache first (skip when first arg is an object
    // so a class method beats a same-named global function).
    const bool firstArgIsObj = (NF >= 1 && AB[0].isObject());
    const BytecodeChunk *targetChunk = nullptr;
    if (!firstArgIsObj
        && funcIdx < keel::as<keel::i32>(resolvedFuncs.size())
        && resolvedFuncs[funcIdx].epoch == curEpoch) {
        const auto &ce = resolvedFuncs[funcIdx];
        if (ce.userChunk) {
            targetChunk = ce.userChunk;
        } else if (ce.extFunc && !hasImports && originStable) {
            keel::ArrayRef<Value> as(AB, NF);
            Value ob[1];
            bool canHint = nargout_val > 0 && R[dstReg].hasHeap();
            for (keel::usize i = 0; i < NF && canHint; ++i)
                if (&AB[i] == &R[dstReg])
                    canHint = false;
            if (canHint)
                ob[0] = keel::move(R[dstReg]);
            keel::MutArrayRef<Value> os(ob, 1);
            CallContext ctx{&engine_, currentCallEnv()};
            (*ce.extFunc)(as, nargout_val, os, ctx);
            R = frames_.back().R;
            R[dstReg] = keel::move(ob[0]);
            return false;
        }
    }

    // OBJECT function-form: m(obj, ...) where obj's class has method m
    // beats a same-named global/path function (matches TreeWalker).
    if (firstArgIsObj) {
        const BuiltinClass *cls = engine_.findClass(AB[0].objectClassNameRef());
        if (cls) {
            if (const auto *mfp = cls->methodFns.get(funcName)) {
                const BytecodeChunk *mc = engine_.ensureClassMethodChunk(**mfp);
                if (mc) {
                    engine_.enforceMethodAccess(AB[0].objectClassNameRef(), funcName);
                    frame.ip = ip + 1;
                    pushCallFrame(*mc, AB, NF, dstReg, nargout_val, false, 0, 0,
                                  (*mfp)->ownerClass, false);
                    return true;
                }
            }
            if (const auto *mfn = cls->methods.get(funcName)) {
                Value self = AB[0];
                keel::ArrayRef<Value> rest((NF > 1) ? &AB[1] : nullptr, NF - 1);
                Value out[1];
                CallContext ctx{&engine_, currentCallEnv()};
                (*mfn)(self, rest, nargout_val, keel::MutArrayRef<Value>(out, 1), ctx);
                R[dstReg] = keel::move(out[0]);
                return false;
            }
        }
    }

    if (!targetChunk) {
        if (const BytecodeChunk *found = findCompiledFunc(funcName)) {
            if (funcIdx >= keel::as<keel::i32>(resolvedFuncs.size()))
                resolvedFuncs.resize(funcIdx + 1);
            resolvedFuncs[funcIdx] = {found, nullptr, curEpoch};
            targetChunk = found;
        }
    }

    if (targetChunk) {
        frame.ip = ip + 1;
        pushCallFrame(*targetChunk, AB, NF, dstReg, nargout_val);
        return true;
    }

    // OBJECT: ClassName(args) constructs an instance when the name is a registered class.
    if (const BuiltinClass *cls = engine_.findClass(funcName); cls && cls->construct) {
        engine_.enforceCtorAccess(funcName);
        if (const UserFunction *cuf = engine_.classCtor(funcName)) {
            if (const BytecodeChunk *cc = engine_.ensureClassMethodChunk(*cuf)) {
                Value seed = engine_.makeDefaultInstance(funcName);
                frame.ip = ip + 1;
                pushCallFrame(*cc, AB, NF, dstReg, nargout_val,
                              false, 0, 0, funcName, /*isCtor=*/true, &seed);
                return true;
            }
        }
        keel::ArrayRef<Value> as(AB, NF);
        CallContext ctx{&engine_, currentCallEnv()};
        R[dstReg] = cls->construct(as, ctx);
        return false;
    }

    // M-file lookup beats builtins.
    if (auto *uf = engine_.lookupUserFunction(funcName, currentCallEnv())) {
        const BytecodeChunk *found = findCompiledFunc(uf->name);
        if (!found)
            found = engine_.ensureClassMethodChunk(*uf);
        if (!hasImports && originStable) {
            if (funcIdx >= keel::as<keel::i32>(resolvedFuncs.size()))
                resolvedFuncs.resize(funcIdx + 1);
            resolvedFuncs[funcIdx] = {found, nullptr, curEpoch};
        }
        frame.ip = ip + 1;
        pushCallFrame(*found, AB, NF, dstReg, nargout_val);
        return true;
    }

    CallbackBuiltin *cb = engine_.callbackBuiltin(funcName);
    if (cb) {
        keel::ArrayRef<Value> as(AB, NF);
        auto outBox = keel::make_arc<Value>();
        if (auto inner = cb->tryStart(as, nargout_val, outBox.get(), engine_)) {
            auto cont = keel::make_arc<RegDestContinuation>(
                keel::move(inner), keel::move(outBox), frame.regBase + dstReg);
            frame.ip = ip + 1;
            if (startContinuation(keel::move(cont)))
                return true;
            R = frames_.back().R;
            return false;
        }
    }

    if (const ExternalFunc *fnPtr = engine_.findExternal(funcName, currentCallEnv())) {
        if (!cb && !hasImports && originStable && !firstArgIsObj && !engine_.findClass(funcName)) {
            if (funcIdx >= keel::as<keel::i32>(resolvedFuncs.size()))
                resolvedFuncs.resize(funcIdx + 1);
            resolvedFuncs[funcIdx] = {nullptr, fnPtr, curEpoch};
        }
        keel::ArrayRef<Value> as(AB, NF);
        Value ob[1];
        bool canHint = nargout_val > 0 && R[dstReg].hasHeap();
        for (keel::usize i = 0; i < NF && canHint; ++i)
            if (&AB[i] == &R[dstReg])
                canHint = false;
        if (canHint)
            ob[0] = keel::move(R[dstReg]);
        keel::MutArrayRef<Value> os(ob, 1);
        CallContext ctx{&engine_, currentCallEnv()};
        (*fnPtr)(as, nargout_val, os, ctx);
        R = frames_.back().R;
        R[dstReg] = keel::move(ob[0]);
        return false;
    }

    // Fallback: nested-function parent-frame variable or scope-isolated dynamic struct/var.
    const keel::usize dot = funcName.find('.');
    if (dot != keel::kInvalidIndex) {
        keel::StringRef root = funcName.substr(0, dot);
        if (auto rootOpt = lookupDynamicVar(chunk, frame, R, root, /*checkParentFunc=*/true)) {
            Value cur = keel::move(*rootOpt);
            keel::usize pos = dot + 1;
            while (pos < funcName.size()) {
                keel::usize nextDot = funcName.find('.', pos);
                keel::StringRef seg = (nextDot == keel::kInvalidIndex)
                                          ? funcName.substr(pos)
                                          : funcName.substr(pos, nextDot - pos);
                if (!cur.isStruct())
                    throw numkit::Error("Dot indexing requires a struct");
                if (!cur.hasField(seg))
                    throw numkit::Error(
                        keel::format("Reference to non-existent field '{}'", seg).c_str());
                Value nextVal = cur.field(seg);
                cur = keel::move(nextVal);
                if (nextDot == keel::kInvalidIndex)
                    break;
                pos = nextDot + 1;
            }
            if (NF == 0) {
                R[dstReg] = keel::move(cur);
                return false;
            }
            return execCallIndirectSpan(cur, dstReg, AB, NF, R, frame, ip);
        }
    }
    if (dot == keel::kInvalidIndex && NF > 0) {
        if (auto varOpt = lookupDynamicVar(chunk, frame, R, funcName, /*checkParentFunc=*/true)) {
            Value target = keel::move(*varOpt);
            return execCallIndirectSpan(target, dstReg, AB, NF, R, frame, ip);
        }
    }

    throw numkit::Error(
        engine_.formatUndefinedFunctionError(
            funcName, keel::format("VM: undefined function '{}'", funcName)).c_str());
}

bool VM::execCallNamedMultiSpan(keel::u16 outBase, keel::i32 funcIdx, keel::u16 nout,
                                const Value *AB, keel::usize NF, Value *R,
                                CallFrame &frame, const Instruction *ip,
                                keel::Array<CallCacheEntry> &resolvedFuncs)
{
    const BytecodeChunk &chunk = *frame.chunk;
    const keel::String &funcName = chunk.strings[funcIdx];

    auto writeMultiOutputs = [&](Value *outPtr) {
        for (keel::u16 i = 0; i < nout; ++i)
            if (outPtr[i].isUnset())
                throw numkit::Error("Too many output arguments.");
        for (keel::usize i = 0; i < nout; ++i)
            R[outBase + i] = keel::move(outPtr[i]);
    };

    if (const Value *dynLocal = findDynLocalVar(chunk, frame, R, funcName);
        dynLocal && dynLocal->isFuncHandle()) {
        const Value fh = *dynLocal;
        const keel::usize ncaps = fh.funcHandleCaptureCount();
        const keel::StringRef hName = fh.funcHandleNameRef();
        keel::Array<Value> cargs(NF + ncaps);
        for (keel::usize i = 0; i < NF; ++i) cargs[i] = AB[i];
        for (keel::usize i = 0; i < ncaps; ++i) cargs[NF + i] = fh.funcHandleCapture(i);
        if (const BytecodeChunk *fc = findCompiledFunc(hName)) {
            frame.ip = ip + 1;
            returnBuf_.clear();
            pushCallFrame(*fc, cargs.data(), cargs.size(), 0, nout, true, outBase, nout);
            return true;
        }
        if (const ExternalFunc *fp = engine_.findExternal(hName, currentCallEnv())) {
            Value smallOb[8];
            keel::Array<Value> bigOb;
            Value *obPtr = (nout <= 8) ? smallOb : (bigOb.resize(nout), bigOb.data());
            CallContext ctx{&engine_, currentCallEnv()};
            (*fp)(keel::ArrayRef<Value>(cargs.data(), NF), nout,
                  keel::MutArrayRef<Value>(obPtr, nout), ctx);
            writeMultiOutputs(obPtr);
            return false;
        }
    }

    const keel::u64 curEpoch = engine_.callCacheEpoch();
    const Environment *scopeForImports = frame.isScriptFrame
        ? (inheritedScope_ ? inheritedScope_ : &engine_.workspaceEnv())
        : frame.env.get();
    const bool hasImports = scopeForImports && scopeForImports->hasAnyActiveImports();
    const bool originStable = chunk.originKnown
        || !engine_.currentScriptDir()
        || engine_.currentScriptDir()->empty();

    const bool firstArgIsObj = (NF >= 1 && AB[0].isObject());
    if (!firstArgIsObj
        && funcIdx < keel::as<keel::i32>(resolvedFuncs.size())
        && resolvedFuncs[funcIdx].epoch == curEpoch) {
        const auto &ce = resolvedFuncs[funcIdx];
        if (ce.userChunk) {
            frame.ip = ip + 1;
            returnBuf_.clear();
            pushCallFrame(*ce.userChunk, AB, NF, 0, nout, true, outBase, nout);
            return true;
        }
        if (ce.extFunc && !hasImports && originStable) {
            Value smallOut[8];
            keel::Array<Value> bigOut;
            Value *outPtr = (nout <= 8) ? smallOut : (bigOut.resize(nout), bigOut.data());
            CallContext ctx{&engine_, currentCallEnv()};
            (*ce.extFunc)(keel::ArrayRef<Value>(AB, NF), nout,
                          keel::MutArrayRef<Value>(outPtr, nout), ctx);
            writeMultiOutputs(outPtr);
            return false;
        }
    }

    // OBJECT function-form multi-output: [a,b] = m(obj, ...).
    // A class method on the dominant (first) object beats global/path functions.
    if (firstArgIsObj) {
        const BuiltinClass *cls = engine_.findClass(AB[0].objectClassNameRef());
        if (cls) {
            if (const auto *mfp = cls->methodFns.get(funcName)) {
                const BytecodeChunk *mc = engine_.ensureClassMethodChunk(**mfp);
                if (mc) {
                    engine_.enforceMethodAccess(AB[0].objectClassNameRef(), funcName);
                    frame.ip = ip + 1;
                    returnBuf_.clear();
                    pushCallFrame(*mc, AB, NF, 0, nout, true, outBase, nout,
                                  (*mfp)->ownerClass, false);
                    return true;
                }
            }
            if (const auto *mfn = cls->methods.get(funcName)) {
                Value self = AB[0];
                keel::ArrayRef<Value> rest((NF > 1) ? &AB[1] : nullptr, NF - 1);
                Value smallOut[8];
                keel::Array<Value> bigOut;
                Value *outPtr = (nout <= 8) ? smallOut : (bigOut.resize(nout), bigOut.data());
                CallContext ctx{&engine_, currentCallEnv()};
                (*mfn)(self, rest, nout, keel::MutArrayRef<Value>(outPtr, nout), ctx);
                writeMultiOutputs(outPtr);
                return false;
            }
        }
    }

    if (const BytecodeChunk *found = findCompiledFunc(funcName)) {
        if (funcIdx >= keel::as<keel::i32>(resolvedFuncs.size()))
            resolvedFuncs.resize(funcIdx + 1);
        resolvedFuncs[funcIdx] = {found, nullptr, curEpoch};
        frame.ip = ip + 1;
        returnBuf_.clear();
        pushCallFrame(*found, AB, NF, 0, nout, true, outBase, nout);
        return true;
    }

    if (auto *uf = engine_.lookupUserFunction(funcName, currentCallEnv())) {
        const BytecodeChunk *found = findCompiledFunc(uf->name);
        if (!found)
            found = engine_.ensureClassMethodChunk(*uf);
        if (!hasImports && originStable) {
            if (funcIdx >= keel::as<keel::i32>(resolvedFuncs.size()))
                resolvedFuncs.resize(funcIdx + 1);
            resolvedFuncs[funcIdx] = {found, nullptr, curEpoch};
        }
        frame.ip = ip + 1;
        returnBuf_.clear();
        pushCallFrame(*found, AB, NF, 0, nout, true, outBase, nout);
        return true;
    }

    if (const ExternalFunc *fnPtr = engine_.findExternal(funcName, currentCallEnv())) {
        if (!hasImports && originStable && !firstArgIsObj
            && !engine_.callbackBuiltin(funcName)
            && !engine_.findClass(funcName)) {
            if (funcIdx >= keel::as<keel::i32>(resolvedFuncs.size()))
                resolvedFuncs.resize(funcIdx + 1);
            resolvedFuncs[funcIdx] = {nullptr, fnPtr, curEpoch};
        }
        Value smallOut[8];
        keel::Array<Value> bigOut;
        Value *outPtr = (nout <= 8) ? smallOut : (bigOut.resize(nout), bigOut.data());
        CallContext ctx{&engine_, currentCallEnv()};
        (*fnPtr)(keel::ArrayRef<Value>(AB, NF), nout,
                 keel::MutArrayRef<Value>(outPtr, nout), ctx);
        writeMultiOutputs(outPtr);
        return false;
    }

    throw numkit::Error(
        engine_.formatUndefinedFunctionError(
            funcName, keel::format("VM: undefined function '{}'", funcName)).c_str());
}

bool VM::execCallIndirect(const Instruction &I, Value *R,
                           CallFrame &frame, const Instruction *ip)
{
    return execCallIndirectTarget(R[I.b], I.a, I.c, I.e, R, frame, ip);
}

bool VM::execCallIndirectTarget(const Value &target, keel::u16 dstReg, keel::u16 argBase, keel::u16 na,
                                Value *R, CallFrame &frame, const Instruction *ip)
{
    return execCallIndirectSpan(target, dstReg, na ? &R[argBase] : nullptr, na,
                                R, frame, ip);
}

// Span form of execCallIndirectTarget: the argument list arrives as an
// explicit AB[0..na) span rather than a register window — the
// CALL_INDIRECT_FLATTEN path materializes its flattened CSL args in a
// vector and reuses this dispatch verbatim
// (bugs/closed/lang/handle-call-csl-splat).
bool VM::execCallIndirectSpan(const Value &target, keel::u16 dstReg, const Value *AB, keel::usize na,
                              Value *R, CallFrame &frame, const Instruction *ip)
{
    // Resolve function handle (plain or closure)
    Value funcHandleVal;
    keel::usize numCaptures = 0;

    if (target.isFuncHandle()) {
        funcHandleVal = target;
        numCaptures = target.funcHandleCaptureCount();
    } else if (target.isObject()) {
        // OBJECT: obj(i…) dispatches to the class subsref overload.
        // (A known variable `obj(i)` compiles to CALL_INDIRECT, so the
        // object subsref hook is needed here too, not just in INDEX_GET.)
        {
            keel::Array<Value> idx;
            idx.resize(na);
            for (keel::usize i = 0; i < na; ++i)
                idx[i] = AB[i];
            // classdef subsref → same-stack VM frame (pausable, P4e). Returns
            // true on push; the caller then `goto enter_frame`. Without this
            // the body runs through the engine call below — outside any VM
            // frame, so breakpoints inside subsref never fire.
            if (tryObjectSubsrefFrameObj(target, dstReg, keel::ArrayRef<Value>(idx.data(), na), frame, ip))
                return true;
            Value out;
            Value selfCopy = target;
            if (engine_.tryObjectSubsref(selfCopy, keel::ArrayRef<Value>(idx.data(), na), 1,
                                         out, currentCallEnv())) {
                R[dstReg] = keel::move(out);
                return false;
            }
        }
        // No custom subsref → builtin object-array indexing (1-D / 2-D /
        // N-D), routed through the OBJECT-aware Value index methods.
        const Value &mv = target;
        if (na == 1) {
            auto idxs = Value::resolveIndices(AB[0], mv.objectCount());
            R[dstReg] = mv.objectSubArray(idxs, engine_.mr_);
        } else if (na == 2) {
            auto rids = Value::resolveIndices(AB[0], mv.dims().rows());
            auto cids = Value::resolveIndices(AB[1], mv.dims().cols());
            R[dstReg] = mv.indexGet2D(rids.data(), rids.size(), cids.data(), cids.size(),
                                   engine_.mr_);
        } else {
            const keel::i32 nd = keel::as<keel::i32>(na);
            keel::Array<keel::Array<keel::usize>> lists;
            lists.resize(nd);
            keel::Array<const keel::usize *> ptrs;
            ptrs.resize(nd);
            keel::Array<keel::usize> counts;
            counts.resize(nd);
            for (keel::i32 i = 0; i < nd; ++i) {
                const keel::usize lim = (i < mv.dims().ndim()) ? mv.dims().dim(i) : 1;
                lists[i] = Value::resolveIndices(AB[i], lim);
                ptrs[i] = lists[i].data();
                counts[i] = lists[i].size();
            }
            R[dstReg] = mv.indexGetND(ptrs.data(), counts.data(), nd, engine_.mr_);
        }
        return false;
    } else {
        // Array indexing fallback
        execIndirectIndexSpan(target, dstReg, AB, na, R);
        return false;
    }

    const keel::StringRef funcName = funcHandleVal.funcHandleNameRef();

    // Build args: user args + captured values
    keel::usize totalArgsN = keel::as<keel::usize>(na) + numCaptures;
    keel::Array<Value> argsBuf;
    argsBuf.resize(totalArgsN);
    for (keel::usize i = 0; i < na; ++i)
        argsBuf[i] = AB[i];
    for (keel::usize i = 0; i < numCaptures; ++i)
        argsBuf[na + i] = funcHandleVal.funcHandleCapture(i);
    const keel::usize totalArgs = totalArgsN;

    // Try compiled user function
    if (const BytecodeChunk *found = findCompiledFunc(funcName)) {
        frame.ip = ip + 1;
        pushCallFrame(*found, argsBuf.data(), totalArgs, dstReg, 1);
        return true; // caller must re-enter dispatch loop
    }

    // External function
    if (const ExternalFunc *extFn = engine_.findExternal(funcName, currentCallEnv())) {
        keel::ArrayRef<Value> as(argsBuf.data(), na);
        Value ob[1];
        keel::MutArrayRef<Value> os(ob, 1);
        CallContext ctx{&engine_, currentCallEnv()};
        (*extFn)(as, 1, os, ctx);
        frames_.back().R[dstReg] = keel::move(ob[0]);
        return false;
    }

    // M-file fallback: a handle to a FILE function resolves exactly like
    // a direct call (script-origin dirs included)
    // (bugs/opened/lang/handle-to-file-function-unresolved.md).
    if (auto *uf = engine_.lookupUserFunction(funcName, currentCallEnv())) {
        const BytecodeChunk *found = findCompiledFunc(uf->name);
        if (!found)
            found = engine_.ensureClassMethodChunk(*uf);
        frame.ip = ip + 1;
        pushCallFrame(*found, argsBuf.data(), totalArgs, dstReg, 1);
        return true;
    }
    if (const BuiltinClass *cls = engine_.findClass(funcName); cls && cls->construct) {
        CallContext ctx{&engine_, currentCallEnv()};
        R[dstReg] = cls->construct(keel::ArrayRef<Value>(argsBuf.data(), totalArgs), ctx);
        return false;
    }
    throw numkit::Error(
        keel::format("VM: undefined function in handle '@{}'", funcName).c_str());
}

bool VM::execCallIndirectMulti(const Instruction &I, Value *R,
                               CallFrame &frame, const Instruction *ip)
{
    // a=outBase, b=fhReg, c=argBase, d=nargs, e=nout
    keel::u16 outBase = I.a, fhReg = I.b, argBase = I.c;
    keel::u16 na = keel::as<keel::u16>(I.d);
    keel::u16 nout = I.e;

    // Resolve the function handle (plain or closure) + its captures.
    Value funcHandleVal;
    keel::usize numCaptures = 0;
    if (R[fhReg].isFuncHandle()) {
        funcHandleVal = R[fhReg];
        numCaptures = funcHandleVal.funcHandleCaptureCount();
    } else if (R[fhReg].isObject()) {
        const BuiltinClass *cls = engine_.findClass(R[fhReg].objectClassNameRef());
        if (cls && cls->subsref) {
            keel::Array<Value> argsBuf;
            argsBuf.resize(na);
            for (keel::usize i = 0; i < na; ++i)
                argsBuf[i] = R[argBase + i];
            keel::Array<Value> outBuf;
            outBuf.resize(nout);
            CallContext ctx{&engine_, currentCallEnv()};
            cls->subsref(R[fhReg], keel::ArrayRef<Value>(argsBuf.data(), na), nout,
                         keel::MutArrayRef<Value>(outBuf.data(), nout), ctx);
            for (keel::usize i = 0; i < nout; ++i)
                R[outBase + i] = keel::move(outBuf[i]);
            return false;
        }
        throw numkit::Error(
            "VM: multi-output call '[...] = f(...)' of a value that is not a "
            "function handle");
    } else {
        throw numkit::Error(
            "VM: multi-output call '[...] = f(...)' of a value that is not a "
            "function handle");
    }

    const keel::StringRef funcName = funcHandleVal.funcHandleNameRef();

    // Build args: user args followed by captured values.
    keel::usize totalArgsN = keel::as<keel::usize>(na) + numCaptures;
    keel::Array<Value> argsBuf;
    argsBuf.resize(totalArgsN);
    for (keel::usize i = 0; i < na; ++i)
        argsBuf[i] = R[argBase + i];
    for (keel::usize i = 0; i < numCaptures; ++i)
        argsBuf[na + i] = funcHandleVal.funcHandleCapture(i);
    const keel::usize totalArgs = totalArgsN;

    // Compiled user / anonymous function → multi-return frame push (the body
    // runs with nargout = nout and RET_MULTI distributes back to outBase).
    if (const BytecodeChunk *found = findCompiledFunc(funcName)) {
        frame.ip = ip + 1;
        returnBuf_.clear();
        pushCallFrame(*found, argsBuf.data(), totalArgs,
                      0, nout, true, outBase, nout);
        return true; // caller re-enters the dispatch loop
    }

    // External (builtin) function called for several outputs.
    const ExternalFunc *fnPtr = engine_.findExternal(funcName, currentCallEnv());
    if (fnPtr) {
        keel::Array<Value> outBuf;
        outBuf.resize(nout);
        keel::ArrayRef<Value> as(argsBuf.data(), totalArgs);
        keel::MutArrayRef<Value> os(outBuf.data(), nout);
        CallContext ctx{&engine_, currentCallEnv()};
        (*fnPtr)(as, nout, os, ctx);
        for (keel::u16 i = 0; i < nout; ++i)
            if (outBuf[i].isUnset())
                throw numkit::Error("Too many output arguments.");
        for (keel::usize i = 0; i < nout; ++i)
            R[outBase + i] = keel::move(outBuf[i]);
        return false;
    }

    if (auto *uf = engine_.lookupUserFunction(funcName, currentCallEnv())) {
        const BytecodeChunk *found = findCompiledFunc(uf->name);
        if (!found)
            found = engine_.ensureClassMethodChunk(*uf);
        frame.ip = ip + 1;
        returnBuf_.clear();
        pushCallFrame(*found, argsBuf.data(), totalArgs,
                      0, nout, true, outBase, nout);
        return true;
    }
    if (const BuiltinClass *cls = engine_.findClass(funcName); cls && cls->construct) {
        if (nout > 1)
            throw numkit::Error("Too many output arguments.");
        CallContext ctx{&engine_, currentCallEnv()};
        Value obj = cls->construct(keel::ArrayRef<Value>(argsBuf.data(), totalArgs), ctx);
        if (nout > 0)
            R[outBase] = keel::move(obj);
        return false;
    }
    throw numkit::Error(
        keel::format("VM: undefined function in handle '@{}'", funcName).c_str());
}

void VM::execIndirectIndex(const Instruction &I, Value *R)
{
    execIndirectIndexTarget(R[I.b], I.a, I.c, I.e, R);
}

void VM::execIndirectIndexTarget(const Value &mv, keel::u16 dstReg, keel::u16 argBase, keel::u16 na, Value *R)
{
    execIndirectIndexSpan(mv, dstReg, na ? &R[argBase] : nullptr, na, R);
}

void VM::execIndirectIndexSpan(const Value &mv, keel::u16 dstReg, const Value *AB, keel::usize na, Value *R)
{
    if (mv.isObject()) {
        Value selfCopy = mv;
        Value out;
        if (engine_.tryObjectSubsref(selfCopy, keel::ArrayRef<Value>(AB, na), 1,
                                     out, currentCallEnv())) {
            R[dstReg] = keel::move(out);
            return;
        }
    }
    if (na == 1) {
        const Value &ix = AB[0];
        if (mv.isCell()) {
            auto indices = Value::resolveIndices(ix, mv.numel());
            R[dstReg] = mv.indexGet(indices.data(), indices.size(), engine_.mr_);
        } else if (ix.isChar() && ix.numel() == 1 && ix.charData()[0] == ':') {
            // BUG #14: scalar(:) on a tag-stored value (e.g. `true(:)`,
            // `false(:)`) used to segfault here — the previous fast path
            // memcpy'd `n*es` bytes from `mv.rawData()`, but tag scalars
            // have no heap buffer, so rawData() returned nullptr/garbage.
            // Identity short-circuit covers every scalar type (DOUBLE /
            // LOGICAL tag / INT* / STRUCT / etc.) without touching the
            // raw byte representation.
            if (mv.isScalar()) {
                R[dstReg] = mv;
            } else {
                keel::usize n = mv.numel();
                ValueType t = mv.type();
                auto res = Value::matrix(n, 1, t, engine_.mr_);
                if (n > 0) {
                    keel::usize es = elementSize(t);
                    keel::mem_copy(res.rawDataMut(), mv.rawData(), n * es);
                }
                R[dstReg] = numkit::narrowComplex(keel::move(res), engine_.mr_);  // z(:) all-real -> real
            }
        } else if (mv.isScalar() && ix.isDoubleScalar()) {
            checkedIndex(ix.scalarVal(), 1);
            R[dstReg] = mv;
        } else if (ix.isDoubleScalar()) {
            keel::usize i = checkedIndex(ix.scalarVal(), mv.numel());
            R[dstReg] = mv.elemAt(i, engine_.mr_);
        } else if (ix.isLogical()) {
            R[dstReg] = mv.logicalIndex(ix.logicalData(), ix.numel(), engine_.mr_);
        } else {
            // General index vector — resolveIndices handles all numeric/char
            // index types (int*, single, char codes) and validates
            // positivity/integrality/bounds, matching the INDEX_GET opcode.
            // (Raw doubleData() threw "Not a double array" on e.g. A(int32(2))
            // and silently truncated fractional indices.)
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
            R[dstReg] = keel::move(res);
        }
    } else if (na == 2) {
        const Value &ri = AB[0];
        const Value &ci = AB[1];
        auto rowIds = ResolvedSubscript::resolve(ri, mv.dims().rows());
        keel::usize colDim = mv.dims().ndim() > 2
            ? (mv.dims().rows() > 0 ? mv.dims().numel() / mv.dims().rows() : mv.dims().cols())
            : mv.dims().cols();
        auto colIds = ResolvedSubscript::resolve(ci, colDim);
        R[dstReg] = mv.indexGet2D(rowIds.data(), rowIds.count,
                               colIds.data(), colIds.count,
                               engine_.mr_);
    } else if (na == 3) {
        if (mv.isCell()) {
            keel::usize r = (keel::usize) AB[0].toScalar() - 1;
            keel::usize c = (keel::usize) AB[1].toScalar() - 1;
            keel::usize p = (keel::usize) AB[2].toScalar() - 1;
            R[dstReg] = mv.cellAt(mv.dims().sub2indChecked(r, c, p));
        } else {
            auto rowIds = ResolvedSubscript::resolve(AB[0], mv.dims().rows());
            auto colIds = ResolvedSubscript::resolve(AB[1], mv.dims().cols());
            auto pageIds = ResolvedSubscript::resolve(AB[2], mv.dims().pages());
            R[dstReg] = mv.indexGet3D(rowIds.data(), rowIds.count,
                                   colIds.data(), colIds.count,
                                   pageIds.data(), pageIds.count,
                                   engine_.mr_);
        }
    } else {
        // ND indexing fallback for na >= 4. CELL handled by indexGetND.
        const keel::i32 nd = keel::as<keel::i32>(na);
        keel::Array<keel::Array<keel::usize>> idxLists;
        idxLists.resize(nd);
        keel::Array<const keel::usize *> idxPtrs;
        idxPtrs.resize(nd);
        keel::Array<keel::usize> idxCounts;
        idxCounts.resize(nd);
        for (keel::i32 i = 0; i < nd; ++i) {
            const keel::usize lim = (i < mv.dims().ndim()) ? mv.dims().dim(i) : 1;
            idxLists[i] = Value::resolveIndices(AB[i], lim);
            idxPtrs[i] = idxLists[i].data();
            idxCounts[i] = idxLists[i].size();
        }
        R[dstReg] = mv.indexGetND(idxPtrs.data(), idxCounts.data(), nd, engine_.mr_);
    }
}

void VM::execDisplay(const Instruction &I, Value *R, const BytecodeChunk &chunk)
{
    if (R[I.a].isUnset())
        return;
    const keel::String &name = chunk.strings[I.d];
    if (R[I.a].isCsl()) {
        for (keel::usize k = 0; k < R[I.a].cslCount(); ++k) {
            const Value &elem = R[I.a].cslAt(k);
            if (elem.isUnset())
                continue;
            if (elem.isObject())
                engine_.displayObject(name, elem);
            else
                engine_.outputText(elem.formatDisplay(name));
        }
        return;
    }
    if (R[I.a].isObject()) {
        engine_.displayObject(name, R[I.a]); // honours class disp/display override
        return;
    }
    engine_.outputText(R[I.a].formatDisplay(name));
}

void VM::execWho(const Instruction &I, Value *R, const BytecodeChunk &chunk)
{
    keel::HashSet<keel::String> seen;
    keel::Array<keel::String> names;
    auto addName = [&](keel::StringRef nm) {
        if (seen.insert(keel::String::from(nm)))
            names.push_back(keel::String::from(nm));
    };
    const CallFrame *curFrame = frames_.empty() ? nullptr : &frames_.back();
    if (I.b == 0) {
        for (const auto &[vn, reg] : chunk.varMap) {
            if (reg < chunk.numRegisters && whoVisible(engine_, vn, chunk)) {
                Value rv = readChunkVar(chunk, R, chunk.numRegisters, vn, reg);
                if (!rv.isUnset() && !rv.isDeleted())
                    addName(vn);
            }
        }
        if (curFrame && curFrame->dynVars) {
            for (auto [dn, dv] : *curFrame->dynVars)
                if (!dv.isUnset() && !dv.isDeleted() && dn != "nargin" && dn != "nargout")
                    addName(dn);
        }
        if (curFrame && curFrame->env) {
            for (const auto &dn : curFrame->env->localNames())
                if (dn != "nargin" && dn != "nargout")
                    addName(dn);
        }
    } else {
        for (keel::u16 i = 0; i < I.b; ++i) {
            const keel::StringRef reqName = R[I.a + i].asStringRef();
            bool found = false;
            for (const auto &[vn, reg] : chunk.varMap) {
                if (vn == reqName && reg < chunk.numRegisters) {
                    Value rv = readChunkVar(chunk, R, chunk.numRegisters, vn, reg);
                    if (!rv.isUnset() && !rv.isDeleted()) {
                        addName(vn);
                        found = true;
                    }
                    break;
                }
            }
            if (!found && curFrame) {
                if (curFrame->dynVars) {
                    if (const Value *dv = curFrame->dynVars->get(reqName);
                        dv && !dv->isUnset() && !dv->isDeleted()) {
                        addName(reqName);
                        found = true;
                    }
                }
                if (!found && curFrame->env && curFrame->env->getLocal(reqName))
                    addName(reqName);
            }
        }
    }
    keel::sort(names.begin(), names.end());
    if (!names.empty()) {
        keel::String os;
        os.append("\nYour variables are:\n\n");
        for (const auto &n : names) {
            os.append(n);
            os.append("  ");
        }
        os.append("\n\n");
        engine_.outputText(os);
    }
}

void VM::execWhos(const Instruction &I, Value *R, const BytecodeChunk &chunk)
{
    keel::HashMap<keel::String, Value> liveVals;
    const CallFrame *curFrame = frames_.empty() ? nullptr : &frames_.back();
    if (I.b == 0) {
        for (const auto &[vn, reg] : chunk.varMap) {
            if (reg < chunk.numRegisters && whoVisible(engine_, vn, chunk)) {
                Value rv = readChunkVar(chunk, R, chunk.numRegisters, vn, reg);
                if (!rv.isUnset() && !rv.isDeleted())
                    liveVals.put(vn.clone(), keel::move(rv));
            }
        }
        if (curFrame && curFrame->dynVars) {
            for (auto [dn, dv] : *curFrame->dynVars)
                if (!dv.isUnset() && !dv.isDeleted() && dn != "nargin" && dn != "nargout")
                    liveVals.put(dn.clone(), dv);
        }
        if (curFrame && curFrame->env) {
            for (const auto &dn : curFrame->env->localNames()) {
                if (dn != "nargin" && dn != "nargout") {
                    if (const Value *lv = curFrame->env->getLocal(dn);
                        lv && !lv->isUnset() && !lv->isDeleted())
                        liveVals.put(dn.clone(), *lv);
                }
            }
        }
    } else {
        for (keel::u16 i = 0; i < I.b; ++i) {
            const keel::StringRef reqName = R[I.a + i].asStringRef();
            bool found = false;
            for (const auto &[vn, reg] : chunk.varMap) {
                if (vn == reqName && reg < chunk.numRegisters) {
                    Value rv = readChunkVar(chunk, R, chunk.numRegisters, vn, reg);
                    if (!rv.isUnset() && !rv.isDeleted()) {
                        liveVals.put(vn.clone(), keel::move(rv));
                        found = true;
                    }
                    break;
                }
            }
            if (!found && curFrame) {
                if (curFrame->dynVars) {
                    if (const Value *dv = curFrame->dynVars->get(reqName);
                        dv && !dv->isUnset() && !dv->isDeleted()) {
                        liveVals.put(keel::String::from(reqName), *dv);
                        found = true;
                    }
                }
                if (!found && curFrame->env) {
                    if (const Value *lv = curFrame->env->getLocal(reqName);
                        lv && !lv->isUnset() && !lv->isDeleted())
                        liveVals.put(keel::String::from(reqName), *lv);
                }
            }
        }
    }
    keel::Array<keel::String> names;
    names.reserve(liveVals.size());
    for (auto [k, _] : liveVals)
        names.push_back(k.clone());
    keel::sort(names.begin(), names.end());
    keel::HashSet<keel::StringRef> globalSet;
    for (const auto &gn : chunk.globalNames)
        globalSet.insert(gn.as_ref());
    keel::HashSet<keel::StringRef> persistentSet;
    for (const auto &pn : chunk.persistentNames)
        persistentSet.insert(pn.as_ref());
    if (!names.empty()) {
        keel::String os;
        os.append("  Name      Size             Bytes  Class     Attributes\n\n");
        for (const auto &n : names) {
            const Value *valPtr = liveVals.get(n);
            if (!valPtr)
                continue;
            const Value &val = *valPtr;
            auto &d = val.dims();
            keel::String sizeStr = d.is3D()
                                       ? keel::format("{}x{}x{}", d.rows(), d.cols(), d.pages())
                                       : keel::format("{}x{}", d.rows(), d.cols());
            keel::String bytesStr = keel::format("{}", val.deepBytes());
            keel::StringRef classStr = mtypeName(val.type());
            keel::StringRef attrStr;
            if (globalSet.contains(n.as_ref()))
                attrStr = "global";
            else if (persistentSet.contains(n.as_ref()))
                attrStr = "persistent";
            os.append("  ");
            os.append(n);
            for (keel::usize i = n.size(); i < 10; ++i)
                os.push_back(' ');
            os.append(sizeStr);
            for (keel::usize i = sizeStr.size(); i < 17; ++i)
                os.push_back(' ');
            for (keel::usize i = bytesStr.size(); i < 5; ++i)
                os.push_back(' ');
            os.append(bytesStr);
            os.append("  ");
            os.append(classStr);
            for (keel::usize i = classStr.size(); i < 10; ++i)
                os.push_back(' ');
            os.append(attrStr);
            os.push_back('\n');
        }
        os.push_back('\n');
        engine_.outputText(os);
    }
}

} // namespace numkit
