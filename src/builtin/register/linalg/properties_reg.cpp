// toolboxes/linalg/src/properties_reg.cpp
//
// Register half of the matrix-property builtins: the CallContext wrappers
// inv / trace / det / rank / cond / normest / rcond / condest / condeig
// that delegate to the engine-free compute in properties.cpp. condeig's
// multi-output form reuses the engine-free eig_symmetric / eig_general_VD
// (hence the eig.hpp include). library.cpp forward-declares + registers
// these by name.
//
// Phase 2b compute/register split â€” see project_layering_refactor memory.

#include <numkit/linalg/properties.hpp>
#include <numkit/linalg/eig.hpp>     // eig_symmetric / eig_general_VD (condeig [V,D])

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->resource()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace numkit::linalg {
namespace detail {

void inv_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "inv");
    outs[0] = inv(args[0], ctx.engine->resource());
}

void trace_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "trace");
    outs[0] = trace(args[0], ctx.engine->resource());
}

void det_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "det");
    outs[0] = det(args[0], ctx.engine->resource());
}

void rank_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "rank");
    const double tol = (args.size() >= 2) ? args[1].toScalar() : -1.0;
    outs[0] = rank_of(args[0], tol, ctx.engine->resource());
}

void cond_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "cond");
    auto *mr = ctx.engine->resource();
    if (args.size() == 1) {
        outs[0] = cond_2norm(args[0], mr);
        return;
    }
    const Value &p = args[1];
    int p_kind = 2;
    if (p.isChar() || p.isString()) {
        const std::string s = p.toString();
        if      (s == "fro" || s == "Fro") p_kind = 4;
        else if (s == "inf" || s == "Inf") p_kind = 3;
        else throw Error("cond: string p must be 'fro' or 'inf'",
                         0, 0, "cond", "", "numkit:cond:badStringP");
    } else {
        const double pv = p.toScalar();
        if      (pv == 1.0) p_kind = 1;
        else if (pv == 2.0) p_kind = 2;
        else if (std::isinf(pv)) p_kind = 3;
        else throw Error("cond: numeric p must be 1, 2, or Inf",
                         0, 0, "cond", "", "numkit:cond:badP");
    }
    outs[0] = cond_pnorm(args[0], p_kind, mr);
}

void normest_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "normest");
    outs[0] = normest(args[0], ctx.engine->resource());
}

void rcond_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "rcond");
    outs[0] = rcond(args[0], ctx.engine->resource());
}

void condest_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "condest");
    outs[0] = condest(args[0], ctx.engine->resource());
}

void condeig_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "condeig");
    auto *mr = ctx.engine->resource();
    if (nargout <= 1) {
        outs[0] = condeig(args[0], mr);
        return;
    }
    // [V, D[, s]] form: hand off to eig for the first two, compute s
    // separately (no shared work yet â€” refactor candidate when this
    // becomes a hot path).
    const Value &A = args[0];
    // Eig dispatch matches the eig_reg path.
    if (!A.isComplex() && isSymmetricApprox(A, 1e-10)) {
        auto [V, D] = eig_symmetric(A, mr);
        outs[0] = std::move(V);
        outs[1] = std::move(D);
    } else {
        auto [V, D] = eig_general_VD(A, mr);
        outs[0] = std::move(V);
        outs[1] = std::move(D);
    }
    if (nargout >= 3 && outs.size() >= 3)
        outs[2] = condeig(A, mr);
}

} // namespace detail
} // namespace numkit::linalg
