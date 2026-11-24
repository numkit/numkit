// toolboxes/linalg/src/solvers_reg.cpp
//
// Register half of the linear-solver builtins: the CallContext wrappers
// linsolve / lsqminnorm / lsqnonneg that delegate to the engine-free
// compute in solvers.cpp. lsqnonneg's multi-output forms destructure the
// NnlsResult returned by lsqnonneg_impl. library.cpp forward-declares +
// registers these by name.
//
// Phase 2b compute/register split — see project_layering_refactor memory.

#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/linalg/solvers.hpp>
#include <numkit/linalg/detail/linalg_detail.hpp>

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->allocator()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>


namespace numkit::linalg {
namespace detail {

void linsolve_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "linsolve");
    outs[0] = linsolve(args[0], args[1], ctx.engine->allocator());
}

void lsqminnorm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "lsqminnorm");
    bool have_tol = (args.size() >= 3);
    keel::f64 tol = have_tol ? args[2].toScalar() : 0.0;
    outs[0] = lsqminnorm(args[0], args[1], have_tol, tol, ctx.engine->allocator());
}

void lsqnonneg_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "lsqnonneg");
    auto *mr = ctx.engine->allocator();
    auto R = lsqnonneg_impl(args[0], args[1], mr);
    const bool wantSingle = (args[0].type() == ValueType::SINGLE ||
                             args[1].type() == ValueType::SINGLE);
    outs[0] = R.x;
    if (nargout >= 2 && outs.size() >= 2)
        outs[1] = detail::narrowDoubleToSingleIf(Value::scalar(R.resnorm, mr), wantSingle, mr);
    if (nargout >= 3 && outs.size() >= 3)
        outs[2] = R.residual;
    if (nargout >= 4 && outs.size() >= 4)
        outs[3] = Value::scalar(keel::as<keel::f64>(R.exitflag), mr);
    if (nargout >= 5 && outs.size() >= 5) {
        Value out_struct = Value::structure(mr);
        out_struct.field("iterations") =
            Value::scalar(keel::as<keel::f64>(R.iterations), mr);
        out_struct.field("algorithm") =
            Value::fromString(R.algorithm, mr);
        out_struct.field("message") =
            Value::fromString(R.message, mr);
        outs[4] = keel::move(out_struct);
    }
}

} // namespace detail
} // namespace numkit::linalg
