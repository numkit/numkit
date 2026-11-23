// src/core/vm_internal.hpp
// Private internal helpers shared across vm.cpp and vm_helpers.cpp.
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/string.h>
#include <numkit/core/bytecode.hpp>
#include <numkit/value/error.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/index.hpp>
#include <numkit/value/value.hpp>


namespace numkit {

struct ResolvedSubscript {
    keel::usize single = 0;
    keel::Array<keel::usize> vec;
    keel::usize count = 0;
    bool isColon = false;
    bool isSingle = false;

    const keel::usize *data() const noexcept {
        if (isColon) return nullptr;
        if (isSingle) return &single;
        return vec.data();
    }

    keel::usize maxPlusOne(keel::usize currentMax) const noexcept {
        if (isColon) return currentMax;
        if (isSingle) return keel::max(currentMax, single + 1);
        for (keel::usize v : vec)
            currentMax = keel::max(currentMax, v + 1);
        return currentMax;
    }

    static ResolvedSubscript resolve(const Value &idx, keel::usize dimSize) {
        ResolvedSubscript r;
        if (idx.isChar() && idx.numel() == 1 && idx.charData()[0] == ':') {
            r.isColon = true;
            r.count = dimSize;
            return r;
        }
        if (idx.isDoubleScalar()) {
            keel::usize ii = Value::checkedScalarIndex(idx.scalarVal());
            if (ii >= dimSize)
                throw numkit::Error("Index exceeds array dimensions");
            r.single = ii;
            r.isSingle = true;
            r.count = 1;
            return r;
        }
        r.vec = Value::resolveIndices(idx, dimSize);
        r.count = r.vec.size();
        return r;
    }

    static ResolvedSubscript resolveUnchecked(const Value &idx) {
        ResolvedSubscript r;
        if (idx.isChar() && idx.numel() == 1 && idx.charData()[0] == ':') {
            r.isColon = true;
            return r;
        }
        if (idx.isDoubleScalar()) {
            r.single = Value::checkedScalarIndex(idx.scalarVal());
            r.isSingle = true;
            r.count = 1;
            return r;
        }
        r.vec = Value::resolveIndicesUnchecked(idx);
        r.count = r.vec.size();
        return r;
    }
};

inline keel::usize checkedIndex(keel::f64 idx, keel::usize numel)
{
    keel::usize i = 0;
    if (tryIndex0(idx, i)) {
        if (i < numel)
            return i;
        throw numkit::Error(
            keel::format("Index {} exceeds array size {}", i + 1, numel).c_str());
    }
    (void) Value::checkedScalarIndex(idx);
    return 0;
}

// Visibility rule for `who`/`whos` over a chunk's register file: hide
// reserved names unless the chunk has actually assigned them.
inline bool whoVisible(const Engine &engine, keel::StringRef name,
                       const BytecodeChunk &chunk)
{
    if (!engine.isReservedName(name))
        return true;
    return chunk.assignedVars.contains(name);
}

inline Value readChunkVar(const BytecodeChunk &chunk, const Value *R, keel::u32 nregs,
                          keel::StringRef name, keel::u16 reg)
{
    if (const keel::u16 *uv = chunk.upvalueBoxRegs.get(name); uv && *uv < nregs) {
        if (const UpvaluePayload *box = asUpvalueBox(R[*uv]))
            return box->read();
    }
    return (reg < nregs) ? R[reg] : Value();
}

inline void writeChunkVar(const BytecodeChunk &chunk, Value *R, keel::u32 nregs,
                          keel::StringRef name, keel::u16 reg, const Value &val)
{
    if (reg < nregs)
        R[reg] = val;
    if (const keel::u16 *uv = chunk.upvalueBoxRegs.get(name); uv && *uv < nregs) {
        if (UpvaluePayload *box = asUpvalueBox(R[*uv]))
            box->write(val);
    }
}

// Resolve one assignment subscript to 0-based indices for an object-array
// slice store: ':' expands to the whole current dimension; everything else
// (scalar / range / vector / logical) is grow-friendly (unchecked).
inline keel::Array<keel::usize> resolveStoreSubscript(const Value &s, keel::usize curDim)
{
    if (s.isChar() && s.numel() == 1 && s.charData()[0] == ':') {
        keel::Array<keel::usize> out;
        out.resize(curDim);
        for (keel::usize i = 0; i < curDim; ++i)
            out[i] = i;
        return out;
    }
    return Value::resolveIndicesUnchecked(s);
}

class SplatArgs;
inline SplatArgs splatCallArgs(const Value *, keel::u16, keel::u16);

class SplatArgs
{
public:
    SplatArgs(const SplatArgs &) = delete;
    SplatArgs &operator=(const SplatArgs &) = delete;
    SplatArgs(SplatArgs &&) = default;
    SplatArgs &operator=(SplatArgs &&) = delete;

    const Value *data() const { return ptr_; }
    keel::usize size() const { return n_; }
    bool empty() const { return n_ == 0; }
    const Value &operator[](keel::usize i) const { return ptr_[i]; }

private:
    friend SplatArgs splatCallArgs(const Value *, keel::u16, keel::u16);

    SplatArgs() = default;
    keel::Array<Value> buf_;
    const Value *ptr_ = nullptr;
    keel::usize n_ = 0;
};

inline SplatArgs splatCallArgs(const Value *R, keel::u16 base, keel::u16 n)
{
    SplatArgs r;
    bool anyCsl = false;
    for (keel::u16 i = 0; i < n; ++i)
        if (R[base + i].isCsl()) {
            anyCsl = true;
            break;
        }
    if (!anyCsl) {
        r.ptr_ = n ? &R[base] : nullptr;
        r.n_ = n;
        return r;
    }
    if (n == 1 && R[base].isCsl()) {
        r.ptr_ = R[base].cellDataVec().data();
        r.n_ = R[base].cslCount();
        return r;
    }
    r.buf_.reserve(n);
    for (keel::u16 i = 0; i < n; ++i) {
        const Value &a = R[base + i];
        if (a.isCsl())
            for (keel::usize k = 0; k < a.cslCount(); ++k)
                r.buf_.push_back(a.cslAt(k));
        else
            r.buf_.push_back(a);
    }
    r.ptr_ = r.buf_.empty() ? nullptr : r.buf_.data();
    r.n_ = r.buf_.size();
    return r;
}

void exportChunkGlobalsAndPersistents(Engine &engine, Environment &globalsEnv,
                                      const BytecodeChunk &chunk, const Value *R,
                                      keel::u32 nregs, bool isTopLevel);
void importChunkGlobalsAndPersistents(Engine &engine, Environment &globalsEnv,
                                      const BytecodeChunk &chunk, Value *R,
                                      keel::u32 nregs, bool isTopLevel, bool seedMissing);

} // namespace numkit
