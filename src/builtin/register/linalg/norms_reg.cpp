// toolboxes/linalg/src/norms_reg.cpp
//
// Register half of the norm builtins: the CallContext wrappers norm /
// vecnorm that parse the p / dim arguments and delegate to the engine-free
// compute in norms.cpp. library.cpp forward-declares + registers these by
// name.
//
// Phase 2b compute/register split — see project_layering_refactor memory.

#include <keel/core.h>
#include <keel/math.h>
#include <keel/ref.h>
#include <numkit/linalg/norms.hpp>

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->allocator()
#include <numkit/value/value.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>


namespace numkit::linalg {
namespace detail {

void norm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "norm");
    auto *mr = ctx.engine->allocator();
    if (args.size() == 1) {
        outs[0] = norm_value(args[0], 2.0, mr);
        return;
    }
    const Value &p = args[1];
    if (p.isChar() || p.isString()) {
        const auto s = p.toString();
        if (s.equals_ignore_ascii_case("fro")) {
            outs[0] = norm_fro(args[0], mr);
            return;
        }
        if (s.equals_ignore_ascii_case("inf")) {
            outs[0] = norm_inf(args[0], mr);
            return;
        }
        throw Error("norm: string p must be 'fro' or 'inf'",
                    0, 0, "norm", "", "numkit:norm:badStringP");
    }
    const keel::f64 pv = p.toScalar();
    if (keel::is_inf(pv)) {
        if (pv > 0.0) {
            outs[0] = norm_inf(args[0], mr);
            return;
        }
        const Value &X = args[0];
        const bool isVec = !X.dims().is3D()
                           && (X.dims().rows() <= 1 || X.dims().cols() <= 1);
        if (!isVec)
            throw Error("norm: the only matrix norms available are 1, 2, "
                        "Inf, and 'fro'",
                        0, 0, "norm", "", "numkit:norm:badMatrixP");
        outs[0] = vecnorm(X, pv, 0, mr);  // vecnorm handles -Inf = min(|v|)
        return;
    }
    outs[0] = norm_value(args[0], pv, mr);
}

void vecnorm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "vecnorm");
    keel::f64 p = 2.0;
    keel::i32 dim = 0;
    if (args.size() >= 2) p = args[1].toScalar();
    if (args.size() >= 3) dim = argcheck::dimArg(args[2], "vecnorm");
    outs[0] = vecnorm(args[0], p, dim, ctx.engine->allocator());
}

} // namespace detail
} // namespace numkit::linalg
