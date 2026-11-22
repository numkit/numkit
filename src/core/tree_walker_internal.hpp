// src/core/tree_walker_internal.hpp
// Private helpers shared across tree_walker*.cpp TUs.
#pragma once

#include <numkit/value/value.hpp>

namespace numkit {


inline bool toLogicalScalarBool(const Value &v)
{
    if (!v.isScalar() || v.isEmpty() || v.isUnset()) {
        throw std::runtime_error(
            "Operands to the logical AND (&&) and OR (||) operators must be convertible to logical scalar values.");
    }
    return v.toBool();
}

inline bool condTruth(const Value &v)
{
    if (v.isEmpty() && !v.isDeleted()
        && (v.isUnset() || v.isNumeric() || v.isLogical() || v.isChar())) {
        return false;
    }
    return v.toBool();
}


} // namespace numkit
