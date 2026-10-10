// toolboxes/signal/src/math/trig/trigonometry_reg.cpp
//
// CallContext register half of math/trig/trigonometry.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elfun.hpp>          // hypot decl
#include <numkit/builtin/elfun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include "elfun/_unary_hint.hpp"   // 3-arg sin/cos hint overloads
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

NUMKIT_UNARY_ADAPTER(tan,  tan)
NUMKIT_UNARY_ADAPTER(asin, asin)
NUMKIT_UNARY_ADAPTER(acos, acos)
NUMKIT_UNARY_ADAPTER(atan, atan)

NUMKIT_UNARY_ADAPTER(sinh,  sinh)
NUMKIT_UNARY_ADAPTER(cosh,  cosh)
NUMKIT_UNARY_ADAPTER(tanh,  tanh)
NUMKIT_UNARY_ADAPTER(asinh, asinh)
NUMKIT_UNARY_ADAPTER(acosh, acosh)
NUMKIT_UNARY_ADAPTER(atanh, atanh)

NUMKIT_UNARY_ADAPTER(sind,  sind)
NUMKIT_UNARY_ADAPTER(cosd,  cosd)
NUMKIT_UNARY_ADAPTER(tand,  tand)
NUMKIT_UNARY_ADAPTER(asind, asind)
NUMKIT_UNARY_ADAPTER(acosd, acosd)
NUMKIT_UNARY_ADAPTER(atand, atand)

NUMKIT_UNARY_ADAPTER(sinpi, sinpi)
NUMKIT_UNARY_ADAPTER(cospi, cospi)

NUMKIT_UNARY_ADAPTER(sec,   sec)
NUMKIT_UNARY_ADAPTER(csc,   csc)
NUMKIT_UNARY_ADAPTER(cot,   cot)
NUMKIT_UNARY_ADAPTER(sech,  sech)
NUMKIT_UNARY_ADAPTER(csch,  csch)
NUMKIT_UNARY_ADAPTER(coth,  coth)
NUMKIT_UNARY_ADAPTER(secd,  secd)
NUMKIT_UNARY_ADAPTER(cscd,  cscd)
NUMKIT_UNARY_ADAPTER(cotd,  cotd)
NUMKIT_UNARY_ADAPTER(asec,  asec)
NUMKIT_UNARY_ADAPTER(acsc,  acsc)
NUMKIT_UNARY_ADAPTER(acot,  acot)
NUMKIT_UNARY_ADAPTER(asech, asech)
NUMKIT_UNARY_ADAPTER(acsch, acsch)
NUMKIT_UNARY_ADAPTER(acoth, acoth)
NUMKIT_UNARY_ADAPTER(asecd, asecd)
NUMKIT_UNARY_ADAPTER(acscd, acscd)
NUMKIT_UNARY_ADAPTER(acotd, acotd)

#undef NUMKIT_UNARY_ADAPTER

void atan2_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "atan2");
    argcheck::checkNargin(args.size(), 2, 2, "atan2");
    outs[0] = atan2(args[0], args[1], ctx.engine->resource());
}

void atan2d_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "atan2d");
    argcheck::checkNargin(args.size(), 2, 2, "atan2d");
    outs[0] = atan2d(args[0], args[1], ctx.engine->resource());
}

void cart2pol_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "cart2pol");
    argcheck::checkNargin(args.size(), 2, 3, "cart2pol");
    auto *mr = ctx.engine->resource();
    if (args.size() >= 3) {
        auto [theta, rho, z] = cart2pol(args[0], args[1], args[2], mr);
        outs[0] = std::move(theta);
        if (nargout > 1) outs[1] = std::move(rho);
        if (nargout > 2) outs[2] = std::move(z);
        return;
    }
    auto [theta, rho] = cart2pol(args[0], args[1], mr);
    outs[0] = std::move(theta);
    if (nargout > 1) outs[1] = std::move(rho);
}

void pol2cart_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "pol2cart");
    argcheck::checkNargin(args.size(), 2, 3, "pol2cart");
    auto *mr = ctx.engine->resource();
    if (args.size() >= 3) {
        auto [xv, yv, zv] = pol2cart(args[0], args[1], args[2], mr);
        outs[0] = std::move(xv);
        if (nargout > 1) outs[1] = std::move(yv);
        if (nargout > 2) outs[2] = std::move(zv);
        return;
    }
    auto [xv, yv] = pol2cart(args[0], args[1], mr);
    outs[0] = std::move(xv);
    if (nargout > 1) outs[1] = std::move(yv);
}

void cart2sph_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "cart2sph");
    argcheck::checkNargin(args.size(), 3, 3, "cart2sph");
    auto [az, el, r] = cart2sph(args[0], args[1], args[2], ctx.engine->resource());
    outs[0] = std::move(az);
    if (nargout > 1) outs[1] = std::move(el);
    if (nargout > 2) outs[2] = std::move(r);
}

void sph2cart_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "sph2cart");
    argcheck::checkNargin(args.size(), 3, 3, "sph2cart");
    auto [xv, yv, zv] = sph2cart(args[0], args[1], args[2], ctx.engine->resource());
    outs[0] = std::move(xv);
    if (nargout > 1) outs[1] = std::move(yv);
    if (nargout > 2) outs[2] = std::move(zv);
}

} // namespace detail

} // namespace numkit::builtin
