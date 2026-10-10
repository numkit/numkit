// toolboxes/signal/src/math/special/special_reg.cpp
//
// CallContext register half of math/special/special.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <numkit/core/engine.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/specfun.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
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

#define NUMKIT_UNARY_ADAPTER(name, fn)                                              \
    void name##_reg(Span<const Value> args, size_t /*nargout*/,                \
                    Span<Value> outs, CallContext &ctx)                        \
    {                                                                            \
        if (args.empty())                                                        \
            throw Error(#name ": requires 1 argument",                          \
                         0, 0, #name, "", "numkit:" #name ":nargin");                 \
        outs[0] = fn(args[0], ctx.engine->resource());                          \
    }

NUMKIT_UNARY_ADAPTER(gamma,   gamma)
NUMKIT_UNARY_ADAPTER(gammaln, gammaln)
NUMKIT_UNARY_ADAPTER(erf,     erf)
NUMKIT_UNARY_ADAPTER(erfc,    erfc)
NUMKIT_UNARY_ADAPTER(erfinv,  erfinv)
NUMKIT_UNARY_ADAPTER(erfcinv, erfcinv)
NUMKIT_UNARY_ADAPTER(erfcx,   erfcx)
NUMKIT_UNARY_ADAPTER(expint,  expint)
NUMKIT_UNARY_ADAPTER(psi,     psi)

#undef NUMKIT_UNARY_ADAPTER

void beta_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
              CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "beta");
    argcheck::checkNargin(args.size(), 2, 2, "beta");
    outs[0] = beta(args[0], args[1], ctx.engine->resource());
}

void betaln_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "betaln");
    argcheck::checkNargin(args.size(), 2, 2, "betaln");
    outs[0] = betaln(args[0], args[1], ctx.engine->resource());
}

void gammainc_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "gammainc");
    argcheck::checkNargin(args.size(), 2, 3, "gammainc");
    outs[0] = gammainc(args[0], args[1], ctx.engine->resource());
}

void betainc_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "betainc");
    argcheck::checkNargin(args.size(), 3, 4, "betainc");
    outs[0] = betainc(args[0], args[1], args[2], ctx.engine->resource());
}

void legendre_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "legendre");
    argcheck::checkNargin(args.size(), 2, 3, "legendre");
    const double nv = argcheck::expectedReal(args[0], 1, "n", "legendre");
    argcheck::expectedInteger(nv, 1, "n", "legendre");
    argcheck::expectedNonNegative(nv, 1, "n", "legendre");
    const int n = static_cast<int>(nv);
    outs[0] = legendre(n, args[1], ctx.engine->resource());
}

#define NUMKIT_BESSEL_REG(name)                                                       \
    void name##_reg(Span<const Value> args, size_t /*nargout*/,                  \
                    Span<Value> outs, CallContext &ctx)                          \
    {                                                                              \
        if (args.size() < 2)                                                       \
            throw Error(#name ": requires (nu, x)",                              \
                         0, 0, #name, "", "numkit:" #name ":nargin");                  \
        outs[0] = name(args[0], args[1], ctx.engine->resource());                \
    }

NUMKIT_BESSEL_REG(besselj)
NUMKIT_BESSEL_REG(bessely)
NUMKIT_BESSEL_REG(besseli)
NUMKIT_BESSEL_REG(besselk)

#undef NUMKIT_BESSEL_REG

void besselh_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "besselh");
    argcheck::checkNargin(args.size(), 2, 4, "besselh");
    auto *mr = ctx.engine->resource();
    if (args.size() == 2) {
        // 2-arg form defaults to k = 1.
        outs[0] = besselh(args[0], 1, args[1], mr);
        return;
    }
    const int k = argcheck::expectedInteger(
        argcheck::expectedReal(args[1], 2, "k", "besselh"), 2, "k", "besselh");
    outs[0] = besselh(args[0], k, args[2], mr);
}

void ellipke_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "ellipke");
    auto res = ellipke(args[0], ctx.engine->resource());
    outs[0] = std::move(res.K);
    if (nargout > 1) outs[1] = std::move(res.E);
}

void airy_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
              CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "airy");
    auto *mr = ctx.engine->resource();
    if (args.size() == 1) {
        outs[0] = airy(0, args[0], mr);  // default Ai
        return;
    }
    int k = argcheck::expectedOneOf(
        argcheck::expectedReal(args[0], 1, "k", "airy"), {0, 1, 2, 3}, 1, "k", "airy");
    outs[0] = airy(k, args[1], mr);
}

void gammaincinv_reg(Span<const Value> args, size_t, Span<Value> outs,
                     CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "gammaincinv");
    argcheck::checkNargin(args.size(), 2, 3, "gammaincinv");
    outs[0] = gammaincinv(args[0], args[1], ctx.engine->resource());
}

void betaincinv_reg(Span<const Value> args, size_t, Span<Value> outs,
                    CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "betaincinv");
    argcheck::checkNargin(args.size(), 3, 4, "betaincinv");
    outs[0] = betaincinv(args[0], args[1], args[2], ctx.engine->resource());
}

void ellipj_reg(Span<const Value> args, size_t nargout, Span<Value> outs,
                CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "ellipj");
    argcheck::checkNargin(args.size(), 2, 3, "ellipj");
    auto r = ellipj(args[0], args[1], ctx.engine->resource());
    outs[0] = std::move(r.sn);
    if (nargout > 1) outs[1] = std::move(r.cn);
    if (nargout > 2) outs[2] = std::move(r.dn);
}

} // namespace detail

} // namespace numkit::builtin
