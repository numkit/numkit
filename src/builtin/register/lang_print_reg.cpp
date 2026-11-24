#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/runtime/io.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/builtin/iofun.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/shape_ops.hpp>
#include <numkit/value/value.hpp>


namespace numkit::builtin {
using namespace numkit::builtin;

namespace detail {

void disp_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "disp");
    if (args[0].isObject()) {
        coreEngine(ctx).outputText(coreEngine(ctx).formatObjectDisplay("", args[0]));
        return;
    }
    coreEngine(ctx).outputText(dispFormat(args[0]));
}

void display_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "display");
    keel::String name = (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                            ? args[1].toString()
                            : keel::String("ans", ctx.engine->allocator());
    if (args[0].isObject()) {
        coreEngine(ctx).displayObject(name, args[0]);
        return;
    }
    coreEngine(ctx).outputText(args[0].formatDisplay(name));
}

void fprintf_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "fprintf");
    keel::usize count = ::numkit::runtime::fprintf(coreEngine(ctx), args);
    if (nargout >= 1 && !outs.empty())
        outs[0] = Value::scalar(keel::as<keel::f64>(count), ctx.engine->allocator());
}

} // namespace detail

} // namespace numkit::builtin
