// toolboxes/builtin/src/language/operators/unary_ops_reg.cpp
//
// CallContext register half (Phase 2b multi-block split).
#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/ops.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/builtin/detail/ops/unary_ops_detail.hpp>
#include <numkit/value/error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

#define NUMKIT_UNOP_REG(OP_NAME, CXX_FN)                                             \
    void OP_NAME##_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,              \
                           keel::MutArrayRef<Value> outs, CallContext &ctx)                       \
    {                                                                                 \
        argcheck::checkNargin(args.size(), 1, 1, #OP_NAME); \
        outs[0] = CXX_FN(args[0], ctx.engine->allocator());                           \
    }

NUMKIT_UNOP_REG(uminus,     uminus)
NUMKIT_UNOP_REG(uplus,      uplus)
NUMKIT_UNOP_REG(not,        logicalNot)
NUMKIT_UNOP_REG(ctranspose, ctranspose)

#undef NUMKIT_UNOP_REG

} // namespace detail

} // namespace numkit::builtin
