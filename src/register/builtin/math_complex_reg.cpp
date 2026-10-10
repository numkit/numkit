// math/src/complex/complex_reg.cpp
//
// CallContext register half of math/complex/complex.cpp (Phase 2b split).
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elfun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/span.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace numkit::builtin {

using namespace numkit::builtin;  // C4c: localized using (umbrella shim removed)

namespace detail {

void real_reg(Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "real");
    outs[0] = real(args[0], ctx.engine->resource());
}

void imag_reg(Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "imag");
    outs[0] = imag(args[0], ctx.engine->resource());
}

void conj_reg(Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "conj");
    outs[0] = conj(args[0], ctx.engine->resource());
}

void complex_reg(Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "complex");
    if (args.size() == 1)
        outs[0] = complex(args[0], ctx.engine->resource());
    else
        outs[0] = complex(args[0], args[1], ctx.engine->resource());
}

void angle_reg(Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "angle");
    outs[0] = angle(args[0], ctx.engine->resource());
}

} // namespace detail

} // namespace numkit::builtin
