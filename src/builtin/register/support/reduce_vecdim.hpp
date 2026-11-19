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

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/small_array.h>
#include <keel/string.h>
#include <algorithm>
#include <utility>

#include <numkit/builtin/elmat.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::vecdim {

// Parse the VECTOR form of a dim slot into unique positive integers.
// Entries beyond the input's rank are kept (they reduce as singletons);
// duplicates and non-vector shapes are rejected here, non-positive /
// fractional entries by argcheck::dimArg per element.
inline keel::SmallArray<int, 8> parseDimSet(const Value &a, keel::StringRef fn)
{
    if (!a.dims().isVector())
        throw Error(keel::format("{}: dimension argument must be a "
                                 "scalar or a vector of unique positive integers "
                                 "(argument is {}).",
                                 fn, argcheck::detail::dimsStr(a)),
                    0, 0, fn, "",
                    keel::format_small<96>("numkit:{}:dimNotVector", fn));
    keel::SmallArray<int, 8> set;
    set.reserve(a.numel());
    for (keel::usize i = 0; i < a.numel(); ++i)
        set.push_back(argcheck::dimArg(a.elemAsDouble(i), fn));
    keel::SmallArray<int, 8> sorted = set.clone();
    std::sort(sorted.begin(), sorted.end());
    for (keel::usize i = 1; i < sorted.size(); ++i)
        if (sorted[i] == sorted[i - 1])
            throw Error(keel::format("{}: dimension vector entries "
                                     "must be unique ({} repeats).",
                                     fn, sorted[i]),
                        0, 0, fn, "",
                        keel::format_small<96>("numkit:{}:dimNotUnique", fn));
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
inline bool coversAllDims(keel::ArrayRef<int> set, const Value &x)
{
    const int rank = realRank(x);
    keel::SmallArray<char, 16> seen(static_cast<keel::usize>(rank) + 1, '\0');
    for (int d : set)
        if (d <= rank)
            seen[static_cast<keel::usize>(d)] = 1;
    for (int d = 1; d <= rank; ++d)
        if (!seen[static_cast<keel::usize>(d)])
            return false;
    return true;
}

// Collapse the listed dims into one trailing reduction dim: out has the
// non-listed sizes (original order) followed by prod(listed real sizes).
// Reduce along the LAST dim of the result. Type-preserving.
inline Value collapseDims(const Value &x, keel::ArrayRef<int> set,
                          keel::Allocator *mr)
{
    const int nd = x.dims().ndim();
    keel::SmallArray<char, 16> listed(static_cast<keel::usize>(nd) + 1, '\0');
    for (int d : set)
        if (d <= nd)
            listed[static_cast<keel::usize>(d)] = 1;
    keel::SmallArray<int, 8> order;          // non-listed first, listed last
    order.reserve(static_cast<keel::usize>(nd));
    keel::usize red = 1;                     // product of the listed real sizes
    for (int d = 1; d <= nd; ++d)
        if (!listed[static_cast<keel::usize>(d)])
            order.push_back(d);
    for (int d = 1; d <= nd; ++d)
        if (listed[static_cast<keel::usize>(d)])
            red *= x.dims().dim(d - 1);
    for (int d = 1; d <= nd; ++d)
        if (listed[static_cast<keel::usize>(d)])
            order.push_back(d);
    Value p = builtin::permute(x, keel::ArrayRef<int>(order.data(), order.size()), mr);
    keel::SmallArray<keel::usize, 8> shape;
    for (int d = 1; d <= nd; ++d)
        if (!listed[static_cast<keel::usize>(d)])
            shape.push_back(x.dims().dim(d - 1));
    if (shape.empty())               // every real dim listed
        shape.push_back(1);
    shape.push_back(red);
    return builtin::reshape(std::move(p), keel::ArrayRef<keel::usize>(shape.data(), shape.size()), mr);
}

// Reshape a reduction RESULT of the collapsed array back to the
// position-preserving output shape: original sizes with the listed dims
// set to 1, trailing 1s trimmed, minimum 2-D. Inserting or removing
// unit axes never changes the column-major order, so a plain reshape of
// the reduced value suffices (regardless of whether the kernel kept or
// dropped the reduced trailing singleton).
inline Value expandReduced(Value reduced, const Value &x, keel::ArrayRef<int> set,
                           keel::Allocator *mr)
{
    const int nd = x.dims().ndim();
    keel::SmallArray<char, 16> listed(static_cast<keel::usize>(nd) + 1, '\0');
    for (int d : set)
        if (d <= nd)
            listed[static_cast<keel::usize>(d)] = 1;
    keel::SmallArray<keel::usize, 8> shape;
    for (int d = 1; d <= nd; ++d)
        shape.push_back(listed[static_cast<keel::usize>(d)] ? 1 : x.dims().dim(d - 1));
    while (shape.size() > 2 && shape.back() == 1)
        shape.pop_back();
    if (shape.size() == 1)
        shape.push_back(1);
    return builtin::reshape(std::move(reduced),
                            keel::ArrayRef<keel::usize>(shape.data(), shape.size()), mr);
}

} // namespace numkit::vecdim
