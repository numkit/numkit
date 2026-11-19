// src/bundle/src/register/builtin/matfun_reg.cpp

#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/builtin/matfun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>

namespace numkit::builtin::detail {

void register_matfun(Engine &engine) {
    engine.registerFunction("idivide",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 3, "idivide");
            keel::String opt = keel::String::from("fix");
            if (args.size() >= 3 && (args[2].isChar() || args[2].isString())) {
                opt = args[2].toString();
            }
            outs[0] = numkit::builtin::idivide(args[0], args[1], opt, ctx.engine->allocator());
        });
}

} // namespace numkit::builtin::detail
