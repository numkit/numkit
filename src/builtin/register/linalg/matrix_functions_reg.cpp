// toolboxes/linalg/src/matrix_functions_reg.cpp
//
// Register half of the matrix-function builtins: the CallContext wrappers
// expm / logm / sqrtm / expmv that delegate to the engine-free compute in
// matrix_functions.cpp. library.cpp forward-declares + registers these by
// name.
//
// Phase 2b compute/register split â€” see project_layering_refactor memory.

#include <numkit/linalg/matrix_functions.hpp>

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->resource()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

namespace numkit::linalg {
namespace detail {

void expm_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    if (args.size() != 1)
        throw Error("expm: requires exactly 1 argument",
                    0, 0, "expm", "", "numkit:expm:nargin");
    outs[0] = expm(args[0], ctx.engine->resource());
}

void logm_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    if (args.size() != 1)
        throw Error("logm: requires exactly 1 argument",
                    0, 0, "logm", "", "numkit:logm:nargin");
    // General Schur-based solver (real/complex, symmetric or not) — the
    // compute entry owns the type contract.
    outs[0] = logm(args[0], ctx.engine->resource());
}

void sqrtm_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    if (args.size() != 1)
        throw Error("sqrtm: requires exactly 1 argument",
                    0, 0, "sqrtm", "", "numkit:sqrtm:nargin");
    // General Schur-based solver (real/complex, symmetric or not) — the
    // compute entry owns the type contract.
    outs[0] = sqrtm(args[0], ctx.engine->resource());
}

// Reference signature: w = expmv(t, A, v) — three positional args, t first.
// Some flavours (e.g. expmv from Higham's package) use (A, v) and an
// optional t; we mirror the documented order with t leading.
void expmv_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    if (args.size() != 3)
        throw Error("expmv: requires (t, A, v)",
                    0, 0, "expmv", "", "numkit:expmv:nargin");
    const double t = args[0].toScalar();
    outs[0] = expmv(t, args[1], args[2], ctx.engine->resource());
}

void funm_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "funm");
    auto *mr = ctx.engine->resource();
    const Value &A = args[0];
    const Value &fnArg = args[1];

    std::string knownName;
    if (fnArg.isChar() || fnArg.isString()) {
        knownName = fnArg.toString();
    } else if (fnArg.isFuncHandle()) {
        knownName = fnArg.funcHandleName();
    }
    if (knownName == "exp" || knownName == "log" || knownName == "sqrt"
        || knownName == "sin" || knownName == "cos"
        || knownName == "sinh" || knownName == "cosh") {
        outs[0] = funm(A, knownName, mr);
        return;
    }

    Value handle = fnArg;
    if (fnArg.isChar() || fnArg.isString()) {
        handle = Value::funcHandle(knownName, mr);
    } else if (!fnArg.isFuncHandle()
               && !(fnArg.isCell() && fnArg.numel() >= 1 && fnArg.cellAt(0).isFuncHandle())) {
        throw Error("funm: second argument must be a function handle or function name",
                    0, 0, "funm", "", "numkit:funm:badFun");
    }

    auto evalScalarFn = [&](std::complex<double> z) -> std::complex<double> {
        Value x = (z.imag() == 0.0) ? Value::scalar(z.real(), mr)
                                    : Value::complexScalar(z.real(), z.imag(), mr);
        Value y = ctx.engine->callFunctionHandle(handle, Span<const Value>(&x, 1), ctx.env);
        if (y.isComplex()) return y.toComplex();
        return std::complex<double>(y.toScalar(), 0.0);
    };
    outs[0] = funm(A, evalScalarFn, mr);
}

} // namespace detail
} // namespace numkit::linalg
