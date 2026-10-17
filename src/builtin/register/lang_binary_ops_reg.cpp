// toolboxes/builtin/src/language/operators/binary_ops_reg.cpp
//
// CallContext register half (Phase 2b multi-block split).
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/builtin/ops.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/ops/binary_ops.hpp>
#include <numkit/ops/compare.hpp>
#include <numkit/ops/la_solve.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/value.hpp>
#include <numkit/builtin/detail/ops/binary_ops_detail.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/span.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

#define NUMKIT_BINOP_REG(OP_NAME, CXX_FN)                                            \
    void OP_NAME##_reg(Span<const Value> args, size_t /*nargout*/,              \
                           Span<Value> outs, CallContext &ctx)                       \
    {                                                                                 \
        argcheck::checkNargin(args.size(), 2, 2, #OP_NAME); \
        outs[0] = CXX_FN(args[0], args[1], ctx.engine->resource());                  \
    }

NUMKIT_BINOP_REG(plus,     plus)
NUMKIT_BINOP_REG(minus,    minus)
NUMKIT_BINOP_REG(times,    times)
NUMKIT_BINOP_REG(mtimes,   mtimes)
NUMKIT_BINOP_REG(rdivide,  rdivide)
NUMKIT_BINOP_REG(mrdivide, mrdivide)
NUMKIT_BINOP_REG(mldivide, mldivide)
NUMKIT_BINOP_REG(power,    elementPower)   // MATLAB power(a,b) = a.^b → C++ elementPower
NUMKIT_BINOP_REG(mpower,   power)          // MATLAB mpower(a,b) = a^b  → C++ power
NUMKIT_BINOP_REG(eq,       eq)
NUMKIT_BINOP_REG(ne,       ne)
NUMKIT_BINOP_REG(lt,       lt)
NUMKIT_BINOP_REG(le,       le)
NUMKIT_BINOP_REG(gt,       gt)
NUMKIT_BINOP_REG(ge,       ge)

#undef NUMKIT_BINOP_REG

// ldivide(a, b) = b ./ a (MATLAB convention).
void ldivide_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "ldivide");
    argcheck::checkNargin(args.size(), 2, 2, "ldivide");
    outs[0] = rdivide(args[1], args[0], ctx.engine->resource());
}

// `and` / `or` builtins map to logicalAnd / logicalOr.
void and_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
             CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "and");
    argcheck::checkNargin(args.size(), 2, 2, "and");
    outs[0] = logicalAnd(args[0], args[1], ctx.engine->resource());
}

void or_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
            CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "or");
    argcheck::checkNargin(args.size(), 2, 2, "or");
    outs[0] = logicalOr(args[0], args[1], ctx.engine->resource());
}

} // namespace detail

} // namespace numkit::builtin
