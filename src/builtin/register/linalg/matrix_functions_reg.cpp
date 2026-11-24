// toolboxes/linalg/src/matrix_functions_reg.cpp
//
// Register half of the matrix-function builtins: the CallContext wrappers
// expm / logm / sqrtm / expmv that delegate to the engine-free compute in
// matrix_functions.cpp. library.cpp forward-declares + registers these by
// name.
//
// Phase 2b compute/register split — see project_layering_refactor memory.

#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/complex.h>
#include <numkit/linalg/matrix_functions.hpp>

#include <numkit/core/engine.hpp>   // CallContext, Span, ctx.engine->allocator()
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/error.hpp>

namespace numkit::linalg {
namespace detail {

void expm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "expm");
    outs[0] = expm(args[0], ctx.engine->allocator());
}

void logm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "logm");
    outs[0] = logm(args[0], ctx.engine->allocator());
}

void sqrtm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "sqrtm");
    outs[0] = sqrtm(args[0], ctx.engine->allocator());
}

void expmv_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, 3, "expmv");
    const keel::f64 t = args[0].toScalar();
    outs[0] = expmv(t, args[1], args[2], ctx.engine->allocator());
}

void funm_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 2, "funm");
    auto *mr = ctx.engine->allocator();
    const Value &A = args[0];
    const Value &fnArg = args[1];

    keel::StringRef knownName;
    if (fnArg.isChar() || fnArg.isString()) {
        knownName = fnArg.asStringRef();
    } else if (fnArg.isFuncHandle()) {
        knownName = fnArg.funcHandleNameRef();
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
    } else if (!fnArg.isFuncHandle()) {
        throw Error("funm: second argument must be a function handle or function name",
                    0, 0, "funm", "", "numkit:funm:badFun");
    }

    auto evalScalarFn = [&](keel::c64 z) -> keel::c64 {
        Value x = (z.imag() == 0.0) ? Value::scalar(z.real(), mr)
                                    : Value::complexScalar(z.real(), z.imag(), mr);
        Value y = ctx.engine->callFunctionHandle(handle, keel::ArrayRef<Value>(&x, 1), ctx.env);
        if (y.isComplex()) return y.toComplex();
        return keel::c64(y.toScalar(), 0.0);
    };
    outs[0] = funm(A, evalScalarFn, mr);
}

} // namespace detail
} // namespace numkit::linalg
