// toolboxes/signal/src/math/exp_log/exponents_reg.cpp
//
// CallContext register half of math/exp_log/exponents.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elfun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include "elfun/_unary_hint.hpp"   // 3-arg exp/log hint overloads
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

NUMKIT_UNARY_ADAPTER(sqrt,  sqrt)
NUMKIT_UNARY_ADAPTER(log2,  log2)
NUMKIT_UNARY_ADAPTER(log10, log10)
NUMKIT_UNARY_ADAPTER(expm1, expm1)
NUMKIT_UNARY_ADAPTER(log1p, log1p)

NUMKIT_UNARY_ADAPTER(reallog,  reallog)
NUMKIT_UNARY_ADAPTER(realsqrt, realsqrt)

#undef NUMKIT_UNARY_ADAPTER

void pow2_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "pow2");
    auto *mr = ctx.engine->resource();
    if (args.size() >= 2)
        outs[0] = pow2(args[0], args[1], mr);
    else
        outs[0] = pow2(args[0], mr);
}

void realpow_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "realpow");
    argcheck::checkNargin(args.size(), 2, 2, "realpow");
    outs[0] = realpow(args[0], args[1], ctx.engine->resource());
}

} // namespace detail

} // namespace numkit::builtin
