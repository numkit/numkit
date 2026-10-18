// toolboxes/linalg/src/schur_convert_reg.cpp
//
// Register half of the Schur-form conversion builtins: the CallContext
// wrappers cdf2rdf / rsf2csf that delegate to the engine-free compute in
// schur_convert.cpp. library.cpp forward-declares + registers these by name.
//
// Phase 2b compute/register split â€” see project_layering_refactor memory.

#include <numkit/linalg/schur_convert.hpp>

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->resource()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

#include <utility>

namespace numkit::linalg {
namespace detail {

void cdf2rdf_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 2, "cdf2rdf");
    auto [VR, DR] = cdf2rdf(args[0], args[1], ctx.engine->resource());
    outs[0] = std::move(VR);
    if (nargout >= 2 && outs.size() >= 2) outs[1] = std::move(DR);
}

void rsf2csf_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 2, "rsf2csf");
    auto [U, T] = rsf2csf(args[0], args[1], ctx.engine->resource());
    outs[0] = std::move(U);
    if (nargout >= 2 && outs.size() >= 2) outs[1] = std::move(T);
}

} // namespace detail
} // namespace numkit::linalg
