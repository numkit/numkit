// src/bundle/src/register/builtin/elmat_reg.cpp

#include <keel/array.h>
#include <keel/core.h>
#include <keel/math.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/builtin/elmat.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/callback_builtin.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/argcheck.hpp>

#include <keel/random.h>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace numkit::builtin::detail {

void blkdiag_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void cat_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void circshift_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void colon_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void compan_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void cylinder_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void diag_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ellipsoid_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void eye_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void false_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void find_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void flip_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void fliplr_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void flipud_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void freqspace_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void gallery_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void hadamard_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void hankel_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void hilb_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ichol_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ind2sub_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void inf_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void invhilb_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ipermute_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void iscolumn_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isempty_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isequal_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isequaln_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isequalwithequalnans_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ismatrix_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isrow_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isscalar_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void issorted_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void issortedrows_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void issparse_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isvector_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void kron_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void length_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void linspace_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void logspace_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void magic_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void meshgrid_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void nan_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ndgrid_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ndims_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void nnz_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void nonzeros_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void numel_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void nzmax_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ones_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void paddata_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void pagectranspose_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void pagetranspose_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void pascal_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void peaks_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void permute_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void repelem_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void repmat_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void reshape_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void resize_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void rosser_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void rot90_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void shiftdim_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void size_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void sparse_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void spdiags_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void sphere_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void squeeze_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void sub2ind_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void toeplitz_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void topkrows_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void transpose_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void tril_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void trimdata_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void triu_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void true_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void vander_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void wilkinson_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void zeros_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);

struct BsxfunCallbackBuiltin : CallbackBuiltin
{
    // Single-shot: bsxfun forwards the WHOLE arrays to the handle in one
    // call (the pinned contract, debug_session_test BreakInsideBsxfunCallback
    // — one pausable frame). Lost when e95e6054 rewrote this per-element.
    keel::Arc<VmContinuation> tryStart(keel::ArrayRef<Value> args, keel::usize nargout,
                                       Value *dest, Engine &eng) override
    {
        if (args.size() < 3 || nargout > 1)
            return nullptr;
        if (!eng.isUserCodeHandle(args[0]))
            return nullptr;
        Value arg1 = args[1];
        Value arg2 = args[2];
        auto cont = keel::make_arc<LoopContinuation>();
        cont->handle = args[0];
        cont->n = 1;
        cont->dest = dest;
        cont->makeArgs = [arg1, arg2](keel::usize) -> keel::Array<Value> {
            keel::Array<Value> callArgs;
            callArgs.reserve(2);
            callArgs.push_back(arg1);
            callArgs.push_back(arg2);
            return callArgs;
        };
        cont->pack = [](keel::Array<Value> &results) -> Value {
            return results.empty() ? Value() : keel::move(results[0]);
        };
        cont->results.reserve(1);
        return cont;
    }
};

} // namespace numkit::builtin::detail

namespace numkit::builtin::detail {

namespace {

void rands_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
               CallContext &ctx) {
    auto *mr = ctx.engine->allocator();
    keel::usize m = 1, n = 1;
    if (args.size() >= 2) {
        m = argcheck::arrayDim(args[0], "rands");
        n = argcheck::arrayDim(args[1], "rands");
    } else if (args.size() == 1) {
        // NN toolbox: rands(S) -> Sx1 column vector
        m = argcheck::arrayDim(args[0], "rands");
        n = 1;
    }
    auto out = Value::matrix(m, n, ValueType::DOUBLE, mr);
    keel::f64 *p = out.doubleDataMut();
    auto &rng = ctx.engine->rng();
    for (keel::usize i = 0; i < m * n; ++i) p[i] = keel::random::uniform_f64(rng, -1.0, 1.0);
    outs[0] = std::move(out);
}

void head_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
              CallContext &ctx) {
    if (args.empty()) throw std::runtime_error("head requires at least 1 argument");
    keel::usize k = (args.size() > 1) ? argcheck::arrayDim(args[1], "head") : 8;
    outs[0] = numkit::builtin::head(args[0], k, ctx.engine->allocator());
}

void tail_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
              CallContext &ctx) {
    if (args.empty()) throw std::runtime_error("tail requires at least 1 argument");
    keel::usize k = (args.size() > 1) ? argcheck::arrayDim(args[1], "tail") : 8;
    outs[0] = numkit::builtin::tail(args[0], k, ctx.engine->allocator());
}

void bsxfun_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                CallContext &ctx) {
    if (args.size() < 3)
        throw std::runtime_error("bsxfun requires at least 3 arguments (fun, A, B)");
    const auto &fn = args[0];
    const auto &a = args[1];
    const auto &b = args[2];
    auto *mr = ctx.engine->allocator();

    if (fn.isFuncHandle()) {
        const keel::StringRef name = fn.funcHandleNameRef();
        if (!name.empty() && name[0] != '@') {
            try {
                outs[0] = numkit::builtin::bsxfun(name, a, b, mr);
                return;
            } catch (...) {
            }
        }
    } else if (fn.isChar() || fn.isString()) {
        try {
            outs[0] = numkit::builtin::bsxfun(fn.asStringRef(), a, b, mr);
            return;
        } catch (...) {
        }
    }

    if (a.isEmpty() || b.isEmpty()) {
        outs[0] = Value::Empty;
        return;
    }
    keel::usize ar = a.dims().rows(), ac = a.dims().cols();
    keel::usize br = b.dims().rows(), bc = b.dims().cols();
    if ((ar != br && ar != 1 && br != 1) || (ac != bc && ac != 1 && bc != 1)) {
        throw std::runtime_error("bsxfun: non-singleton dimensions must match");
    }
    keel::usize outR = std::max(ar, br);
    keel::usize outC = std::max(ac, bc);
    auto res = Value::matrix(outR, outC, ValueType::DOUBLE, mr);
    keel::f64 *outData = res.doubleDataMut();
    for (keel::usize c = 0; c < outC; ++c) {
        keel::usize acIdx = (ac == 1) ? 0 : c;
        keel::usize bcIdx = (bc == 1) ? 0 : c;
        for (keel::usize r = 0; r < outR; ++r) {
            keel::usize arIdx = (ar == 1) ? 0 : r;
            keel::usize brIdx = (br == 1) ? 0 : r;
            Value elemA = Value::scalar(a.elemAsDouble(acIdx * ar + arIdx));
            Value elemB = Value::scalar(b.elemAsDouble(bcIdx * br + brIdx));
            Value argsArr[2] = {elemA, elemB};
            Value callOut = ctx.engine->callFunctionHandle(fn, keel::ArrayRef<Value>(argsArr, 2));
            outData[c * outR + r] = callOut.toScalar();
        }
    }
    outs[0] = std::move(res);
}

void eps_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
             CallContext &ctx) {
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        outs[0] = Value::scalar(keel::Limits<keel::f64>::kEpsilon, mr);
        return;
    }
    const Value &x = args[0];
    if (x.isChar() || x.isString()) {
        keel::String s = x.toString();
        if (s == "single") {
            Value out = Value::matrix(1, 1, ValueType::SINGLE, mr);
            out.singleDataMut()[0] = keel::Limits<keel::f32>::kEpsilon;
            outs[0] = std::move(out);
        } else if (s == "double") {
            outs[0] = Value::scalar(keel::Limits<keel::f64>::kEpsilon, mr);
        } else {
            throw Error("eps: class name must be 'double' or 'single'",
                        0, 0, "eps", "", "numkit:eps:invalidClassName");
        }
        return;
    }
    const ValueType outType = (x.type() == ValueType::SINGLE) ? ValueType::SINGLE : ValueType::DOUBLE;
    if (x.isEmpty()) {
        keel::usize dArr[Dims::kMaxRank];
        keel::i32 nd = x.dims().ndims();
        for (keel::i32 d = 0; d < nd; ++d) dArr[d] = x.dims().dim(d);
        outs[0] = Value::matrixND(dArr, nd, outType, mr);
        return;
    }
    keel::usize dArr[Dims::kMaxRank];
    keel::i32 nd = x.dims().ndims();
    for (keel::i32 d = 0; d < nd; ++d) dArr[d] = x.dims().dim(d);
    Value out = Value::matrixND(dArr, nd, outType, mr);
    const keel::usize n = x.numel();
    if (outType == ValueType::SINGLE) {
        keel::f32 *dst = out.singleDataMut();
        for (keel::usize i = 0; i < n; ++i) {
            keel::f32 v = x.isComplex()
                ? keel::as<keel::f32>(keel::abs(x.complexData()[i]))
                : keel::abs(keel::as<keel::f32>(x.elemAsDouble(i)));
            if (keel::is_nan(v) || keel::is_inf(v)) {
                dst[i] = keel::Limits<keel::f32>::quiet_nan();
            } else if (v <= keel::Limits<keel::f32>::kMin) {
                dst[i] = keel::Limits<keel::f32>::kDenormMin;
            } else {
                dst[i] = keel::nextafter(v, keel::Limits<keel::f32>::infinity()) - v;
            }
        }
    } else {
        keel::f64 *dst = out.doubleDataMut();
        for (keel::usize i = 0; i < n; ++i) {
            keel::f64 v = x.isComplex()
                ? keel::abs(x.complexData()[i])
                : keel::abs(x.elemAsDouble(i));
            if (keel::is_nan(v) || keel::is_inf(v)) {
                dst[i] = keel::Limits<keel::f64>::quiet_nan();
            } else if (v <= keel::Limits<keel::f64>::kMin) {
                dst[i] = keel::Limits<keel::f64>::kDenormMin;
            } else {
                dst[i] = keel::nextafter(v, keel::Limits<keel::f64>::infinity()) - v;
            }
        }
    }
    outs[0] = std::move(out);
}

} // namespace

void register_elmat(Engine &engine) {
    engine.registerFunction("blkdiag",    &::numkit::builtin::detail::blkdiag_reg);
    engine.registerFunction("cat",        &::numkit::builtin::detail::cat_reg);
    engine.registerFunction("circshift",  &::numkit::builtin::detail::circshift_reg);
    engine.registerFunction("colon",      &::numkit::builtin::detail::colon_reg);
    engine.registerFunction("compan",     &::numkit::builtin::detail::compan_reg);
    engine.registerFunction("cylinder",   &::numkit::builtin::detail::cylinder_reg);
    engine.registerFunction("ellipsoid",  &::numkit::builtin::detail::ellipsoid_reg);
    engine.registerFunction("eye",        &::numkit::builtin::detail::eye_reg);
    engine.registerFunction("false",      &::numkit::builtin::detail::false_reg);
    engine.registerFunction("flip",       &::numkit::builtin::detail::flip_reg);
    engine.registerFunction("flipdim",    &::numkit::builtin::detail::flip_reg);
    engine.registerFunction("fliplr",     &::numkit::builtin::detail::fliplr_reg);
    engine.registerFunction("flipud",     &::numkit::builtin::detail::flipud_reg);
    engine.registerFunction("hadamard",   &::numkit::builtin::detail::hadamard_reg);
    engine.registerFunction("hankel",     &::numkit::builtin::detail::hankel_reg);
    engine.registerFunction("hilb",       &::numkit::builtin::detail::hilb_reg);
    engine.registerFunction("ichol",      &::numkit::builtin::detail::ichol_reg);
    engine.registerFunction("ind2sub",    &::numkit::builtin::detail::ind2sub_reg);
    engine.registerFunction("inf",        &::numkit::builtin::detail::inf_reg);
    engine.registerFunction("Inf",        &::numkit::builtin::detail::inf_reg);
    engine.registerFunction("invhilb",    &::numkit::builtin::detail::invhilb_reg);
    engine.registerFunction("ipermute",   &::numkit::builtin::detail::ipermute_reg);
    engine.registerFunction("length",     &::numkit::builtin::detail::length_reg);
    engine.registerFunction("magic",      &::numkit::builtin::detail::magic_reg);
    engine.registerFunction("nan",        &::numkit::builtin::detail::nan_reg);
    engine.registerFunction("NaN",        &::numkit::builtin::detail::nan_reg);
    engine.registerFunction("ndims",      &::numkit::builtin::detail::ndims_reg);
    engine.registerFunction("numel",      &::numkit::builtin::detail::numel_reg);
    engine.registerFunction("ones",       &::numkit::builtin::detail::ones_reg);
    engine.registerFunction("pagectranspose", &::numkit::builtin::detail::pagectranspose_reg);
    engine.registerFunction("pagetranspose",  &::numkit::builtin::detail::pagetranspose_reg);
    engine.registerFunction("pascal",     &::numkit::builtin::detail::pascal_reg);
    engine.registerFunction("peaks",      &::numkit::builtin::detail::peaks_reg);
    engine.registerFunction("permute",    &::numkit::builtin::detail::permute_reg);
    engine.registerFunction("repelem",    &::numkit::builtin::detail::repelem_reg);
    engine.registerFunction("repmat",     &::numkit::builtin::detail::repmat_reg);
    engine.registerFunction("reshape",    &::numkit::builtin::detail::reshape_reg);
    engine.registerFunction("resize",     &::numkit::builtin::detail::resize_reg);
    engine.registerFunction("rosser",     &::numkit::builtin::detail::rosser_reg);
    engine.registerFunction("rot90",      &::numkit::builtin::detail::rot90_reg);
    engine.registerFunction("shiftdim",   &::numkit::builtin::detail::shiftdim_reg);
    engine.registerFunction("size",       &::numkit::builtin::detail::size_reg);
    engine.registerFunction("sparse",     &::numkit::builtin::detail::sparse_reg);
    engine.registerFunction("spdiags",    &::numkit::builtin::detail::spdiags_reg);
    engine.registerFunction("sphere",     &::numkit::builtin::detail::sphere_reg);
    engine.registerFunction("squeeze",    &::numkit::builtin::detail::squeeze_reg);
    engine.registerFunction("sub2ind",    &::numkit::builtin::detail::sub2ind_reg);
    engine.registerFunction("toeplitz",   &::numkit::builtin::detail::toeplitz_reg);
    engine.registerFunction("topkrows",   &::numkit::builtin::detail::topkrows_reg);
    engine.registerFunction("transpose",  &::numkit::builtin::detail::transpose_reg);
    engine.registerFunction("tril",       &::numkit::builtin::detail::tril_reg);
    engine.registerFunction("triu",       &::numkit::builtin::detail::triu_reg);
    engine.registerFunction("true",       &::numkit::builtin::detail::true_reg);
    engine.registerFunction("vander",     &::numkit::builtin::detail::vander_reg);
    engine.registerFunction("wilkinson",  &::numkit::builtin::detail::wilkinson_reg);
    engine.registerFunction("rands",      &rands_reg);
    engine.registerFunction("zeros",      &::numkit::builtin::detail::zeros_reg);
    engine.registerFunction("paddata",    &::numkit::builtin::detail::paddata_reg);
    engine.registerFunction("trimdata",   &::numkit::builtin::detail::trimdata_reg);
    engine.registerFunction("head",       &head_reg);
    engine.registerFunction("tail",       &tail_reg);
    engine.registerFunction("bsxfun",     &bsxfun_reg);
    engine.registerCallbackBuiltin("bsxfun", keel::make_arc<::numkit::builtin::detail::BsxfunCallbackBuiltin>());
    engine.registerFunction("eps",        &eps_reg);
}

} // namespace numkit::builtin::detail
