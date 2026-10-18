// toolboxes/linalg/src/balance_reg.cpp
//
// Register half of the balance builtin: the CallContext wrapper that
// delegates to the engine-free balance_impl compute in balance.cpp and
// assembles the 1-/2-/3-output MATLAB forms. library.cpp forward-declares
// + registers this by name.
//
// Phase 2b compute/register split â€” see project_layering_refactor memory.

#include <numkit/linalg/balance.hpp>
#include <numkit/linalg/detail/linalg_detail.hpp>

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->resource()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace numkit::linalg {
namespace detail {

void balance_reg(Span<const Value> args, size_t nargout,
                 Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "balance");
    bool noperm = false;
    if (args.size() >= 2) {
        if (!args[1].isChar() && !args[1].isString())
            throw Error("balance: optional arg must be 'noperm'",
                        0, 0, "balance", "", "numkit:balance:BadOpt");
        std::string s = args[1].toString();
        if (s == "noperm") noperm = true;
        else
            throw Error("balance: unknown option '" + s + "'",
                        0, 0, "balance", "", "numkit:balance:BadOpt");
    }

    auto *mr = ctx.engine->resource();
    auto R = balance_impl(args[0], noperm, mr);
    const size_t n = R.B.dims().rows();

    if (nargout <= 1) {
        outs[0] = std::move(R.B);
        return;
    }

    if (nargout == 2 || (nargout >= 2 && outs.size() == 2)) {
        // 2-out: [T, B] where T = diag(d).
        Value T = Value::matrix(n, n, ValueType::DOUBLE, mr);
        double *td = T.doubleDataMut();
        std::fill(td, td + n * n, 0.0);
        for (size_t i = 0; i < n; ++i) td[i + i * n] = R.d_col.elemAsDouble(i);
        outs[0] = detail::narrowDoubleToSingleIf(T, R.d_col.type() == ValueType::SINGLE, mr);
        outs[1] = std::move(R.B);
        return;
    }

    // 3-out: [S, P, B].
    outs[0] = std::move(R.d_col);
    outs[1] = std::move(R.perm_col);
    if (outs.size() >= 3) outs[2] = std::move(R.B);
}

} // namespace detail
} // namespace numkit::linalg
