#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/runtime/io.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/builtin/iofun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/shape_ops.hpp>
#include <numkit/value/value.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace numkit::builtin {
using namespace numkit::builtin;

namespace detail {

void disp_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "disp");
    if (args[0].isObject()) {
        ctx.engine->outputText(ctx.engine->formatObjectDisplay("", args[0]));
        return;
    }
    ctx.engine->outputText(dispFormat(args[0]));
}

void display_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "display");
    std::string name = (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                           ? args[1].toString()
                           : "ans";
    if (args[0].isObject()) {
        ctx.engine->displayObject(name, args[0]);
        return;
    }
    ctx.engine->outputText(args[0].formatDisplay(name));
}

void fprintf_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "fprintf");
    keel::usize count = ::numkit::runtime::fprintf(*ctx.engine, args);
    if (nargout >= 1 && !outs.empty())
        outs[0] = Value::scalar(static_cast<double>(count), ctx.engine->allocator());
}

} // namespace detail

} // namespace numkit::builtin
