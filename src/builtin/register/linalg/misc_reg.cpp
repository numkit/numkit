// toolboxes/linalg/src/misc_reg.cpp
//
// Register half of the misc builtins: the CallContext wrappers rref /
// planerot that delegate to the engine-free compute in misc.cpp.
// library.cpp forward-declares + registers these by name.
//
// Phase 2b compute/register split â€” see project_layering_refactor memory.

#include <numkit/linalg/misc.hpp>

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->resource()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

namespace numkit::linalg {
namespace detail {

void rref_reg(Span<const Value> args, size_t nargout,
              Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "rref");
    bool have_tol = (args.size() >= 2);
    double tol = have_tol ? args[1].toScalar() : 0.0;
    auto [R, jb] = rref(args[0], have_tol, tol, ctx.engine->resource());
    outs[0] = R;
    if (nargout >= 2 && outs.size() >= 2) outs[1] = jb;
}

void planerot_reg(Span<const Value> args, size_t nargout,
                  Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "planerot");
    auto [G, y] = planerot(args[0], ctx.engine->resource());
    outs[0] = G;
    if (nargout >= 2 && outs.size() >= 2) outs[1] = y;
}

} // namespace detail
} // namespace numkit::linalg
