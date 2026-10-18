// toolboxes/linalg/src/pseudo_subspace_reg.cpp
//
// Register half of the pseudoinverse / subspace builtins: the CallContext
// wrappers pinv / orth / null / subspace that delegate to the engine-free
// compute in pseudo_subspace.cpp. library.cpp forward-declares + registers
// these by name.
//
// Phase 2b compute/register split â€” see project_layering_refactor memory.

#include <numkit/linalg/pseudo_subspace.hpp>

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->resource()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

namespace numkit::linalg {
namespace detail {

void pinv_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "pinv");
    const double tol = (args.size() >= 2) ? args[1].toScalar() : -1.0;
    outs[0] = pinv(args[0], tol, ctx.engine->resource());
}

void orth_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "orth");
    const double tol = (args.size() >= 2) ? args[1].toScalar() : -1.0;
    outs[0] = orth(args[0], tol, ctx.engine->resource());
}

void null_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "null");
    const double tol = (args.size() >= 2) ? args[1].toScalar() : -1.0;
    outs[0] = null_basis(args[0], tol, ctx.engine->resource());
}

void subspace_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 2, "subspace");
    outs[0] = subspace(args[0], args[1], ctx.engine->resource());
}

} // namespace detail
} // namespace numkit::linalg
