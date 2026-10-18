// src/bundle/src/register/builtin/matfun_reg.cpp

#include <numkit/builtin/matfun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/span.hpp>

#include <stdexcept>
#include <string>

namespace numkit::builtin::detail {

void register_matfun(Engine &engine) {
    engine.registerFunction("idivide",
        [](Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 3, "idivide");
            std::string opt = "fix";
            if (args.size() >= 3 && (args[2].isChar() || args[2].isString())) {
                opt = args[2].toString();
            }
            outs[0] = numkit::builtin::idivide(args[0], args[1], opt, ctx.engine->resource());
        });
}

} // namespace numkit::builtin::detail
