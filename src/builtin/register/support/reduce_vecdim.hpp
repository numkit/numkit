// bundle/src/register/support/reduce_vecdim.hpp
//
// Shared vecdim machinery for every reduction family that takes a dim
// slot (sum/prod/mean/max/min, var/std, median/mode, iqr/bounds,
// trimmean/moment, mape/rmse …) — the single replacement for the
// per-function copies of the old rank heuristic.
//
// Reference-probed contract (2026-09-17, probed against R2025b):
//   * a dim-slot VECTOR must be unique positive integers; entries beyond
//     the input's rank are legal singletons (sum(ones(2,2),7) is the
//     identity; sum(ones(2,2),[1 2 7]) is the full reduction);
//   * [] keeps the DEFAULT dim (first non-singleton) — it is NOT 'all';
//   * the output keeps the original dimension POSITIONS with the listed
//     dims reduced to 1; trailing singletons are trimmed (minimum 2-D):
//     sum(ones(2,3,4),[1 2]) is 1x1x4, median(ones(2,3,4),[2 3]) is 2x1;
//   * duplicate entries are their own rejection class
//     ("Elements of a dimension vector must be unique").

#pragma once

#include <algorithm>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include <numkit/builtin/elmat.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/span.hpp>
#include <numkit/value/value.hpp>

namespace numkit::vecdim {

// Parse the VECTOR form of a dim slot into unique positive integers.
// Entries beyond the input's rank are kept (they reduce as singletons);
// duplicates and non-vector shapes are rejected here, non-positive /
// fractional entries by argcheck::dimArg per element.
inline std::vector<int> parseDimSet(const Value &a, std::string_view fn)
{
    if (!a.dims().isVector())
        throw Error(std::string(fn) + ": dimension argument must be a "
                    "scalar or a vector of unique positive integers "
                    "(argument is " + argcheck::detail::dimsStr(a) + ").",
                    0, 0, std::string(fn), "",
                    "numkit:" + std::string(fn) + ":dimNotVector");
    std::vector<int> set;
    set.reserve(a.numel());
    for (size_t i = 0; i < a.numel(); ++i)
        set.push_back(argcheck::dimArg(a.elemAsDouble(i), fn));
    std::vector<int> sorted = set;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i] == sorted[i - 1])
            throw Error(std::string(fn) + ": dimension vector entries "
                        "must be unique (" + std::to_string(sorted[i]) +
                        " repeats).",
                        0, 0, std::string(fn), "",
                        "numkit:" + std::string(fn) + ":dimNotUnique");
    return set;
}

// Real rank of x per the reference semantics: every array is at least
// 2-D (a row vector has a dimension 2).
inline int realRank(const Value &x)
{
    return std::max(2, x.dims().ndim());
}

// True when every real dimension of x is listed. Extra entries beyond
// the rank don't affect coverage.
inline bool coversAllDims(Span<const int> set, const Value &x)
{
    const int rank = realRank(x);
    std::vector<char> seen(static_cast<size_t>(rank) + 1, 0);
    for (int d : set)
        if (d <= rank)
            seen[static_cast<size_t>(d)] = 1;
    for (int d = 1; d <= rank; ++d)
        if (!seen[static_cast<size_t>(d)])
            return false;
    return true;
}

// Collapse the listed dims into one trailing reduction dim: out has the
// non-listed sizes (original order) followed by prod(listed real sizes).
// Reduce along the LAST dim of the result. Type-preserving.
inline Value collapseDims(const Value &x, Span<const int> set,
                          std::pmr::memory_resource *mr)
{
    const int nd = x.dims().ndim();
    std::vector<char> listed(static_cast<size_t>(nd) + 1, 0);
    for (int d : set)
        if (d <= nd)
            listed[static_cast<size_t>(d)] = 1;
    std::vector<int> order;          // non-listed first, listed last
    order.reserve(static_cast<size_t>(nd));
    size_t red = 1;                  // product of the listed real sizes
    for (int d = 1; d <= nd; ++d)
        if (!listed[static_cast<size_t>(d)])
            order.push_back(d);
    for (int d = 1; d <= nd; ++d)
        if (listed[static_cast<size_t>(d)])
            red *= x.dims().dim(d - 1);
    for (int d = 1; d <= nd; ++d)
        if (listed[static_cast<size_t>(d)])
            order.push_back(d);
    Value p = builtin::permute(x, Span<const int>(order.data(), order.size()), mr);
    std::vector<size_t> shape;
    for (int d = 1; d <= nd; ++d)
        if (!listed[static_cast<size_t>(d)])
            shape.push_back(x.dims().dim(d - 1));
    shape.push_back(red);
    if (shape.size() == 1)           // every real dim listed
        shape.insert(shape.begin(), 1);
    return builtin::reshape(std::move(p), Span<const size_t>(shape.data(), shape.size()), mr);
}

// Reshape a reduction RESULT of the collapsed array back to the
// position-preserving output shape: original sizes with the listed dims
// set to 1, trailing 1s trimmed, minimum 2-D. Inserting or removing
// unit axes never changes the column-major order, so a plain reshape of
// the reduced value suffices (regardless of whether the kernel kept or
// dropped the reduced trailing singleton).
inline Value expandReduced(Value reduced, const Value &x, Span<const int> set,
                           std::pmr::memory_resource *mr)
{
    const int nd = x.dims().ndim();
    std::vector<char> listed(static_cast<size_t>(nd) + 1, 0);
    for (int d : set)
        if (d <= nd)
            listed[static_cast<size_t>(d)] = 1;
    std::vector<size_t> shape;
    for (int d = 1; d <= nd; ++d)
        shape.push_back(listed[static_cast<size_t>(d)] ? 1 : x.dims().dim(d - 1));
    while (shape.size() > 2 && shape.back() == 1)
        shape.pop_back();
    if (shape.size() == 1)
        shape.push_back(1);
    return builtin::reshape(std::move(reduced),
                            Span<const size_t>(shape.data(), shape.size()), mr);
}

} // namespace numkit::vecdim
