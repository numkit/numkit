// src/core/vm_internal.hpp
// Private internal helpers shared across vm.cpp and vm_helpers.cpp.
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/string.h>
#include <numkit/core/bytecode.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/index.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <stdexcept>

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
        if (isSingle) return std::max(currentMax, single + 1);
        for (keel::usize v : vec)
            currentMax = std::max(currentMax, v + 1);
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
                throw std::runtime_error("Index exceeds array dimensions");
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
        throw std::runtime_error(
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

} // namespace numkit
