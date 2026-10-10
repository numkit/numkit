// toolboxes/signal/src/language/arrays/nd_manip_reg.cpp
//
// CallContext register half of language/arrays/nd_manip.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elmat.hpp>  // reshape, horzcat, vertcat
#include <numkit/builtin/elmat.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/shape_ops.hpp>      // computeStridesColMajor, incrementCoords
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/span.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

namespace {

ScratchVec<int> permFromValue(const Value &v, std::pmr::memory_resource *mr)
{
    ScratchVec<int> p(mr);
    p.reserve(v.numel());
    for (size_t i = 0; i < v.numel(); ++i)
        p.push_back(static_cast<int>(v.doubleData()[i]));
    return p;
}

} // namespace

void permute_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "permute");
    argcheck::checkNargin(args.size(), 2, 2, "permute");
    auto *mr = ctx.engine->resource();
    ScratchArena scratch(mr);
    auto perm = permFromValue(args[1], &scratch);
    outs[0] = permute(args[0], Span<const int>(perm.data(), perm.size()), mr);
}

void ipermute_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                  CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "ipermute");
    argcheck::checkNargin(args.size(), 2, 2, "ipermute");
    auto *mr = ctx.engine->resource();
    ScratchArena scratch(mr);
    auto perm = permFromValue(args[1], &scratch);
    outs[0] = ipermute(args[0], Span<const int>(perm.data(), perm.size()), mr);
}

void squeeze_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "squeeze");
    outs[0] = squeeze(args[0], ctx.engine->resource());
}

void cat_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
             CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "cat");
    const int dim = argcheck::dimArg(args[0], "cat");
    // Pass &args[1] as the start of the values array.
    outs[0] = cat(dim, args.subspan(1), ctx.engine->resource());
}

void blkdiag_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                 CallContext &ctx)
{
    outs[0] = blkdiag(args, ctx.engine->resource());
}

void shiftdim_reg(Span<const Value> args, size_t nargout, Span<Value> outs,
                  CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "shiftdim");
    auto *mr = ctx.engine->resource();
    if (args.size() >= 2 && !args[1].isEmpty()) {
        const int n = argcheck::expectedInteger(
        argcheck::expectedReal(args[1], 2, "n", "shiftdim"), 2, "n", "shiftdim");
        outs[0] = shiftdim(args[0], n, mr);
        return;
    }
    // Auto form: [B, k] = shiftdim(A).
    auto res = shiftdimAuto(args[0], mr);
    outs[0] = std::move(res.v);
    if (nargout > 1)
        outs[1] = Value::scalar(static_cast<double>(res.dropped), mr);
}

} // namespace detail

} // namespace numkit::builtin
