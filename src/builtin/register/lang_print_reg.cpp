#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/runtime/io.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/builtin/iofun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/shape_ops.hpp>
#include <numkit/value/value.hpp>
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
using namespace numkit::builtin;

namespace detail {

void disp_reg(Span<const Value> args, size_t, Span<Value>, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "disp");
    if (args[0].isObject()) {
        ctx.engine->outputText(ctx.engine->formatObjectDisplay("", args[0]));
        return;
    }
    ctx.engine->outputText(dispFormat(args[0]));
}

void fprintf_reg(Span<const Value> args, size_t nargout, Span<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "fprintf");
    size_t count = ::numkit::runtime::fprintf(*ctx.engine, args);
    if (nargout >= 1 && !outs.empty())
        outs[0] = Value::scalar(static_cast<double>(count), ctx.engine->resource());
}

} // namespace detail

} // namespace numkit::builtin
