// src/bundle/src/register/builtin/lang_reg.cpp

#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/builtin/lang.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/core/vm.hpp>
#include <numkit/runtime/runtime.hpp>

#include <algorithm>
#include <stdexcept>

namespace numkit::builtin::detail {
void runNative_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void system_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
struct FevalCallbackBuiltin : CallbackBuiltin
{
    keel::Arc<VmContinuation> tryStart(keel::ArrayRef<Value> args, keel::usize nargout,
                                       Value *dest, Engine &eng) override
    {
        if (args.empty() || nargout > 1)
            return nullptr;
        if (!eng.isUserCodeHandle(args[0]))
            return nullptr; // name/string/builtin handle -> synchronous feval
        keel::Array<Value> callArgs;
        callArgs.reserve(args.size() - 1);
        for (keel::usize i = 1; i < args.size(); ++i)
            callArgs.push_back(args[i]);
        auto cont = keel::make_arc<LoopContinuation>();
        cont->handle = args[0];
        cont->n = 1;
        cont->dest = dest;
        cont->makeArgs = [callArgs = std::move(callArgs)](keel::usize) -> keel::Array<Value> {
            return callArgs.clone();
        };
        cont->pack = [](keel::Array<Value> &results) -> Value {
            return results.empty() ? Value() : std::move(results[0]);
        };
        cont->results.reserve(1);
        return cont;
    }
};
} // namespace numkit::builtin::detail


namespace numkit::builtin::detail {

void register_lang(Engine &engine) {
    using namespace ::numkit::builtin::detail;

// ── OS process execution built-ins ─────────────────────────────
    engine.registerFunction("system",     &::numkit::builtin::detail::system_reg);
    engine.registerFunction("runNative",  &::numkit::builtin::detail::runNative_reg);

    
// ── Pack 13: function handles ─────────────────────────────────────
    engine.registerFunction("feval",
                            [](keel::ArrayRef<Value> args, keel::usize nargout,
                               keel::MutArrayRef<Value> outs, CallContext &ctx) {
                                argcheck::checkNargin(args.size(), 1, "feval");
                                Value handle;
                                if (args[0].isFuncHandle()) {
                                    handle = args[0];
                                } else if (args[0].isChar() || args[0].isString()) {
                                    handle = Value::funcHandle(args[0].toString(),
                                                                ctx.engine->allocator());
                                } else {
                                    throw std::runtime_error(
                                        "feval: first argument must be a function handle or name");
                                }
                                keel::ArrayRef<Value> callArgs(args.data() + 1, args.size() - 1);
                                if (nargout <= 1) {
                                    outs[0] = ctx.engine->callFunctionHandle(handle, callArgs, ctx.env);
                                } else {
                                    auto rs = ctx.engine->callFunctionHandleMulti(
                                        handle, callArgs, nargout, ctx.env);
                                    for (keel::usize i = 0; i < nargout && i < outs.size() && i < rs.size(); ++i)
                                        outs[i] = std::move(rs[i]);
                                }
                            });
    engine.registerCallbackBuiltin(
        "feval", keel::make_arc<::numkit::builtin::detail::FevalCallbackBuiltin>());

    auto fwdCallFn = [](keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs,
                        CallContext &ctx) {
        argcheck::checkNargin(args.size(), 2, "__numkit_fwd_call__");
        keel::i64 nll = keel::as<keel::i64>(args[0].toScalar());
        keel::usize n = (nll > 0) ? keel::as<keel::usize>(nll) : 1;
        keel::ArrayRef<Value> callArgs(args.data() + 2, args.size() - 2);
        keel::Array<Value> rs(ctx.engine->allocator());
        bool done = false;
        if (args[1].isChar() || args[1].isString()) {
            const keel::String fname = args[1].toString();
            if (const ExternalFunc *ef = ctx.engine->findExternal(fname, ctx.env)) {
                rs.resize(n);
                (*ef)(callArgs, n, keel::MutArrayRef<Value>(rs.data(), n), ctx);
                done = true;
            }
        }
        if (!done) {
            Value handle = args[1].isFuncHandle()
                               ? args[1]
                               : Value::funcHandle(args[1].toString(),
                                                   ctx.engine->allocator());
            rs = ctx.engine->callFunctionHandleMulti(handle, callArgs, n, ctx.env);
        }
        Value cell = Value::cell(1, n, ctx.engine->allocator());
        for (keel::usize i = 0; i < n && i < rs.size(); ++i)
            cell.cellAt(i) = std::move(rs[i]);
        outs[0] = std::move(cell);
    };
    engine.registerFunction("__numkit_fwd_call__", fwdCallFn);

    engine.registerFunction("iskeyword",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 1, "iskeyword");
            outs[0] = ::numkit::builtin::iskeyword(args, ctx.engine->allocator());
        });

    engine.registerFunction("nargchk",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 3, 4, "nargchk");
            auto *mr = ctx.engine->allocator();
            keel::f64 lo = args[0].toScalar(), hi = args[1].toScalar(), n = args[2].toScalar();
            if (n < lo) outs[0] = Value::fromString("Not enough input arguments.", mr);
            else if (n > hi) outs[0] = Value::fromString("Too many input arguments.", mr);
            else outs[0] = Value::logicalScalar(false, mr);
        });

    engine.registerFunction("narginchk",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 2, "narginchk");
            const keel::f64 lo = args[0].toScalar();
            const keel::f64 hi = args[1].toScalar();
            const keel::i64 n = ctx.engine->currentFunctionNargin();
            if (n >= 0) {
                if (keel::as<keel::f64>(n) < lo)
                    throw Error("Not enough input arguments.",
                                0, 0, "narginchk", "", "numkit:narginchk:notEnoughInputs");
                if (keel::as<keel::f64>(n) > hi)
                    throw Error("Too many input arguments.",
                                0, 0, "narginchk", "", "numkit:narginchk:tooManyInputs");
            }
            outs[0] = Value();
        });

    engine.registerFunction("nargoutchk",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 4, "nargoutchk");
            auto *mr = ctx.engine->allocator();
            const keel::f64 lo = args[0].toScalar();
            const keel::f64 hi = args[1].toScalar();
            if (args.size() >= 3) {
                const keel::f64 n = args[2].toScalar();
                if (n < lo) outs[0] = Value::fromString("Not enough output arguments.", mr);
                else if (n > hi) outs[0] = Value::fromString("Too many output arguments.", mr);
                else outs[0] = Value::fromString("", mr);
                return;
            }
            const keel::i64 n = ctx.engine->currentFunctionNargout();
            if (n >= 0) {
                if (keel::as<keel::f64>(n) < lo)
                    throw Error("Not enough output arguments.",
                                0, 0, "nargoutchk", "", "numkit:nargoutchk:notEnoughOutputs");
                if (keel::as<keel::f64>(n) > hi)
                    throw Error("Too many output arguments.",
                                0, 0, "nargoutchk", "", "numkit:nargoutchk:tooManyOutputs");
            }
            (void)nargout;
            outs[0] = Value();
        });

    engine.registerFunction("memoize",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &) {
            argcheck::checkNargin(args.size(), 1, 1, "memoize");
            outs[0] = args[0];
        });

    engine.registerFunction("savepath",
        [](keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            outs[0] = Value::scalar(0.0, ctx.engine->allocator());
        });

    engine.registerFunction("isvarname",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "isvarname");
            outs[0] = ::numkit::builtin::isvarname(args, ctx.engine->allocator());
        });

}


} // namespace numkit::builtin::detail
