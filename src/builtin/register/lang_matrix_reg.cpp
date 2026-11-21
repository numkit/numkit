// toolboxes/signal/src/language/arrays/matrix_reg.cpp
//
// CallContext register half of language/arrays/matrix.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <keel/complex.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elmat.hpp>
#include <numkit/builtin/datafun.hpp>
#include <numkit/builtin/ops.hpp>
#include <numkit/builtin/polyfun.hpp>
#include <numkit/ops/binary_ops.hpp>
#include <numkit/ops/la_solve.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/builtin/detail/datafun/cumsum.hpp>
#include <numkit/builtin/detail/elmat/matrix_detail.hpp>
#include <numkit/ops/reductions.hpp>
#include <numkit/ops/rows_helpers.hpp>
#include <numkit/value/error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

void zeros_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    // Strip trailing class-name (e.g. 'uint8') or 'like' form before
    // parsing dims. Default type is DOUBLE.
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto d = parseDimsArgsND(dimArgs, "zeros", scratch.allocator());
    stripTrailingOnes(d);
    // Value::matrix*/matrixND zero-fill the buffer for any type, so
    // createMatrixND with the requested type IS the zeros() output.
    outs[0] = createMatrixND(d.data(), d.size(), t, mr);
}

// Fill `v` with one in its declared type (1 / 1.0 / true).
namespace { inline void fillOnes(Value &v, ValueType t)
{
    const keel::usize n = v.numel();
    if (n == 0) return;
    switch (t) {
      case ValueType::DOUBLE:  { auto *p = v.doubleDataMut();  std::fill(p, p + n, 1.0); break; }
      case ValueType::SINGLE:  { auto *p = v.singleDataMut();  std::fill(p, p + n, 1.0f); break; }
      case ValueType::LOGICAL: { auto *p = v.logicalDataMut(); std::fill(p, p + n, keel::u8(1)); break; }
      case ValueType::INT8:    { auto *p = v.int8DataMut();    std::fill(p, p + n, keel::i8(1)); break; }
      case ValueType::INT16:   { auto *p = v.int16DataMut();   std::fill(p, p + n, keel::i16(1)); break; }
      case ValueType::INT32:   { auto *p = v.int32DataMut();   std::fill(p, p + n, keel::i32(1)); break; }
      case ValueType::INT64:   { auto *p = v.int64DataMut();   std::fill(p, p + n, keel::i64(1)); break; }
      case ValueType::UINT8:   { auto *p = v.uint8DataMut();   std::fill(p, p + n, keel::u8(1)); break; }
      case ValueType::UINT16:  { auto *p = v.uint16DataMut();  std::fill(p, p + n, keel::u16(1)); break; }
      case ValueType::UINT32:  { auto *p = v.uint32DataMut();  std::fill(p, p + n, keel::u32(1)); break; }
      case ValueType::UINT64:  { auto *p = v.uint64DataMut();  std::fill(p, p + n, keel::u64(1)); break; }
      default: throw Error("ones: unsupported type for fill",
                           0, 0, "ones", "", "numkit:ones:badType");
    }
}}

void ones_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto d = parseDimsArgsND(dimArgs, "ones", scratch.allocator());
    stripTrailingOnes(d);
    auto m = createMatrixND(d.data(), d.size(), t, mr);
    fillOnes(m, t);
    outs[0] = std::move(m);
}

// MATLAB's colon function: colon(j, k) = j:k, colon(j, i, k) = j:i:k.
// Useful when the operator form is awkward (function-handle slot, etc.)
// and to be a real callable for parity tests. Type preservation matches
// the operator path (see core/src/tree_walker.cpp:colonOutputType and
// core/src/vm.cpp:OpCode::COLON).
namespace { ValueType colonOutType(const Value *ops, keel::usize n)
{
    ValueType nonDouble = ValueType::DOUBLE;
    bool found = false;
    for (keel::usize i = 0; i < n; ++i) {
        ValueType t = ops[i].type();
        if (t == ValueType::DOUBLE) continue;
        if (!found) { nonDouble = t; found = true; }
        else if (t != nonDouble)
            throw Error("colon: operands must be all the same type, "
                        "or mixed with real scalar doubles",
                        0, 0, "colon", "", "numkit:colon:typeMix");
    }
    return nonDouble;
}}

void colon_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 3, "colon");
    auto *mr = ctx.engine->allocator();
    for (keel::usize i = 0; i < args.size(); ++i) {
        if (args[i].isObject()) {
            const BuiltinClass *cls = ctx.engine->findClass(args[i].objectClassNameRef());
            if (cls) {
                if (const ObjectMethod *op = cls->ops.get("colon")) {
                    Value self = args[i];
                    (*op)(self, args, 1, outs, ctx);
                    return;
                }
            }
            break;
        }
    }
    if (args.size() == 2) {
        ValueType t = colonOutType(args.data(), 2);
        outs[0] = Value::colonRangeTyped(args[0].toScalar(),
                                          args[1].toScalar(), t, mr);
    } else if (args.size() == 3) {
        ValueType t = colonOutType(args.data(), 3);
        outs[0] = Value::colonRangeTyped(args[0].toScalar(),
                                          args[1].toScalar(),
                                          args[2].toScalar(), t, mr);
    }
}

// MATLAB's sparse() with size args allocates an MxN sparse zero matrix.
// NumKit has no sparse storage class -- this stub returns dense zeros.
// Matches issparse=false (we ship that stub; see types.cpp:issparse).
// KNOWN GAP: NumKit returns dense; MATLAB returns sparse storage class.
// All numerical operations match (zeros on both sides), only class()
// differs. Documented in PROGRESS for sparse().
void sparse_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 6, "sparse");
    auto *mr = ctx.engine->allocator();
    if (args.size() == 2) {
        // sparse(M, N) -- allocate MxN dense zeros.
        const keel::usize M = argcheck::arrayDim(args[0], "sparse");
        const keel::usize N = argcheck::arrayDim(args[1], "sparse");
        outs[0] = Value::matrix(M, N, ValueType::DOUBLE, mr);
        return;
    }
    if (args.size() == 1) {
        // sparse(A) -- "convert" dense to sparse. We just return A as-is
        // (since we have no sparse storage). For most numerical use this
        // is correct; issparse() still returns false (matches nk
        // semantics).
        outs[0] = args[0];
        return;
    }
    if (args.size() >= 3) {
        // sparse(i, j, v [, m, n [, nzmax]]) -- accumulate triplets into dense m x n.
        const Value &vi = args[0];
        const Value &vj = args[1];
        const Value &vv = args[2];
        const keel::usize ni = vi.numel();
        const keel::usize nj = vj.numel();
        const keel::usize nv = vv.numel();
        const keel::usize len = std::max({ni, nj, nv});
        keel::usize M = 0, N = 0;
        for (keel::usize k = 0; k < ni; ++k) {
            keel::i64 r = keel::as<keel::i64>(vi.elemAsDouble(k));
            if (r > keel::as<keel::i64>(M)) M = keel::as<keel::usize>(r);
        }
        for (keel::usize k = 0; k < nj; ++k) {
            keel::i64 c = keel::as<keel::i64>(vj.elemAsDouble(k));
            if (c > keel::as<keel::i64>(N)) N = keel::as<keel::usize>(c);
        }
        if (args.size() >= 5) {
            M = argcheck::arrayDim(args[3], "sparse");
            N = argcheck::arrayDim(args[4], "sparse");
        }
        Value out = Value::matrix(M, N, ValueType::DOUBLE, mr);
        keel::f64 *od = out.doubleDataMut();
        for (keel::usize k = 0; k < len; ++k) {
            keel::usize r = keel::as<keel::usize>(vi.elemAsDouble(ni == 1 ? 0 : k));
            keel::usize c = keel::as<keel::usize>(vj.elemAsDouble(nj == 1 ? 0 : k));
            keel::f64 val = vv.elemAsDouble(nv == 1 ? 0 : k);
            if (r >= 1 && r <= M && c >= 1 && c <= N) {
                od[(r - 1) + (c - 1) * M] += val;
            }
        }
        outs[0] = std::move(out);
        return;
    }
}

void spdiags_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "spdiags");
    auto *mr = ctx.engine->allocator();

    // 1 or 2 arguments: [B, d] = spdiags(A) or B = spdiags(A, d)
    if (args.size() <= 2) {
        const Value &A = args[0];
        const keel::i64 m = keel::as<keel::i64>(A.dims().rows());
        const keel::i64 n = keel::as<keel::i64>(A.dims().cols());
        const keel::i64 pLen = std::min(m, n);

        keel::Array<keel::i64> diags(mr);
        if (args.size() == 1) {
            for (keel::i64 k = -(m - 1); k <= n - 1; ++k) {
                bool anyNz = false;
                const keel::i64 iStart = std::max(keel::i64{0}, -k);
                const keel::i64 iEnd = std::min(m - 1, n - 1 - k);
                for (keel::i64 i = iStart; i <= iEnd; ++i) {
                    const keel::i64 j = i + k;
                    if (A.elemAsDouble(keel::as<keel::usize>(i + j * m)) != 0.0) {
                        anyNz = true;
                        break;
                    }
                }
                if (anyNz) diags.push_back(k);
            }
        } else {
            const Value &dVal = args[1];
            diags.resize(dVal.numel());
            for (keel::usize idx = 0; idx < dVal.numel(); ++idx)
                diags[idx] = keel::as<keel::i64>(dVal.elemAsDouble(idx));
        }

        const keel::usize numD = diags.size();
        Value B = Value::matrix(keel::as<keel::usize>(pLen), numD, ValueType::DOUBLE, mr);
        keel::f64 *bd = B.doubleDataMut();
        for (keel::usize col = 0; col < numD; ++col) {
            const keel::i64 k = diags[col];
            const keel::i64 iStart = std::max(keel::i64{0}, -k);
            const keel::i64 iEnd = std::min(m - 1, n - 1 - k);
            for (keel::i64 i = iStart; i <= iEnd; ++i) {
                const keel::i64 j = i + k;
                const keel::i64 rowB = (m >= n) ? j : i;
                if (rowB >= 0 && rowB < pLen)
                    bd[keel::as<keel::usize>(rowB) + col * keel::as<keel::usize>(pLen)] =
                        A.elemAsDouble(keel::as<keel::usize>(i + j * m));
            }
        }
        outs[0] = std::move(B);
        if (nargout >= 2 && args.size() == 1) {
            Value dOut = Value::matrix(numD, 1, ValueType::DOUBLE, mr);
            keel::f64 *dd = dOut.doubleDataMut();
            for (keel::usize idx = 0; idx < numD; ++idx)
                dd[idx] = keel::as<keel::f64>(diags[idx]);
            outs[1] = std::move(dOut);
        }
        return;
    }

    // 3 or 4 arguments: A = spdiags(B, d, A_old) or A = spdiags(B, d, m, n)
    const Value &B = args[0];
    const Value &dVal = args[1];
    keel::i64 m = 0, n = 0;
    Value out;
    if (args.size() == 3) {
        const Value &Aold = args[2];
        m = keel::as<keel::i64>(Aold.dims().rows());
        n = keel::as<keel::i64>(Aold.dims().cols());
        out = Value::matrix(keel::as<keel::usize>(m), keel::as<keel::usize>(n), ValueType::DOUBLE, mr);
        keel::f64 *od = out.doubleDataMut();
        for (keel::usize idx = 0; idx < Aold.numel(); ++idx)
            od[idx] = Aold.elemAsDouble(idx);
    } else {
        m = keel::as<keel::i64>(argcheck::arrayDim(args[2], "spdiags"));
        n = keel::as<keel::i64>(argcheck::arrayDim(args[3], "spdiags"));
        out = Value::matrix(keel::as<keel::usize>(m), keel::as<keel::usize>(n), ValueType::DOUBLE, mr);
    }

    keel::f64 *od = out.doubleDataMut();
    const keel::i64 bRows = keel::as<keel::i64>(B.dims().rows());
    const keel::i64 bCols = keel::as<keel::i64>(B.dims().cols());
    const keel::usize numD = dVal.numel();
    for (keel::usize col = 0; col < numD && keel::as<keel::i64>(col) < bCols; ++col) {
        const keel::i64 k = keel::as<keel::i64>(dVal.elemAsDouble(col));
        const keel::i64 iStart = std::max(keel::i64{0}, -k);
        const keel::i64 iEnd = std::min(m - 1, n - 1 - k);
        for (keel::i64 i = iStart; i <= iEnd; ++i) {
            const keel::i64 j = i + k;
            const keel::i64 rowB = (m >= n) ? j : i;
            const keel::f64 val = (rowB >= 0 && rowB < bRows)
                                   ? B.elemAsDouble(keel::as<keel::usize>(rowB + keel::as<keel::i64>(col) * bRows))
                                   : 0.0;
            od[keel::as<keel::usize>(i + j * m)] = val;
        }
    }
    outs[0] = std::move(out);
}

// Zero-fill incomplete Cholesky factorization ichol(A [, opts]):
// returns lower-triangular L preserving the sparsity pattern of A.
void ichol_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "ichol");
    auto *mr = ctx.engine->allocator();
    const Value &A = args[0];
    const keel::usize n = A.dims().rows();
    if (n != A.dims().cols())
        throw Error("ichol: input matrix must be square",
                    0, 0, "ichol", "", "numkit:ichol:notSquare");
    Value L = Value::matrix(n, n, ValueType::DOUBLE, mr);
    keel::f64 *ld = L.doubleDataMut();
    for (keel::usize j = 0; j < n; ++j) {
        for (keel::usize i = j; i < n; ++i) {
            keel::f64 aij = A.elemAsDouble(i + j * n);
            ld[i + j * n] = aij;
        }
    }
    for (keel::usize k = 0; k < n; ++k) {
        keel::f64 lkk = ld[k + k * n];
        if (lkk <= 0.0)
            throw Error("ichol: encountered nonpositive pivot",
                        0, 0, "ichol", "", "numkit:ichol:nonposPivot");
        lkk = std::sqrt(lkk);
        ld[k + k * n] = lkk;
        for (keel::usize i = k + 1; i < n; ++i) {
            if (ld[i + k * n] != 0.0) {
                ld[i + k * n] /= lkk;
            }
        }
        for (keel::usize j = k + 1; j < n; ++j) {
            keel::f64 ljk = ld[j + k * n];
            if (ljk == 0.0) continue;
            for (keel::usize i = j; i < n; ++i) {
                if (ld[i + j * n] != 0.0) {
                    ld[i + j * n] -= ld[i + k * n] * ljk;
                }
            }
        }
    }
    outs[0] = std::move(L);
}

// `nan` / `NaN` / `inf` / `Inf` are MATLAB built-in functions (not
// constants): bare `nan` returns scalar NaN; `nan(M, N, ..., 'type')`
// returns float array filled with NaN (only 'double' or 'single' are
// allowed -- integer types can't represent NaN/Inf, MATLAB throws).
// Same shape parsing as zeros/ones.
namespace { void nanInfFill(Value &v, keel::f64 fillD, keel::f32 fillS, ValueType t)
{
    const keel::usize n = v.numel();
    if (n == 0) return;
    if (t == ValueType::DOUBLE) {
        auto *p = v.doubleDataMut(); std::fill(p, p + n, fillD);
    } else if (t == ValueType::SINGLE) {
        auto *p = v.singleDataMut(); std::fill(p, p + n, fillS);
    }
}}

void nan_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        outs[0] = Value::scalar(std::numeric_limits<keel::f64>::quiet_NaN(), mr);
        return;
    }
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    if (t != ValueType::DOUBLE && t != ValueType::SINGLE)
        throw Error("nan: type must be 'double' or 'single' (NaN is float-only)",
                    0, 0, "nan", "", "numkit:nan:badType");
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto d = parseDimsArgsND(dimArgs, "nan", scratch.allocator());
    stripTrailingOnes(d);
    auto m = createMatrixND(d.data(), d.size(), t, mr);
    nanInfFill(m, std::numeric_limits<keel::f64>::quiet_NaN(),
                  std::numeric_limits<keel::f32>::quiet_NaN(), t);
    outs[0] = std::move(m);
}

void inf_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        outs[0] = Value::scalar(std::numeric_limits<keel::f64>::infinity(), mr);
        return;
    }
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    if (t != ValueType::DOUBLE && t != ValueType::SINGLE)
        throw Error("inf: type must be 'double' or 'single' (Inf is float-only)",
                    0, 0, "inf", "", "numkit:inf:badType");
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto d = parseDimsArgsND(dimArgs, "inf", scratch.allocator());
    stripTrailingOnes(d);
    auto m = createMatrixND(d.data(), d.size(), t, mr);
    nanInfFill(m, std::numeric_limits<keel::f64>::infinity(),
                  std::numeric_limits<keel::f32>::infinity(), t);
    outs[0] = std::move(m);
}

// `true` and `false` are MATLAB built-in functions (not constants):
// bare `true` returns a scalar logical 1; `true(M, N, ...)` returns a
// logical array filled with 1 (or 0 for false). Mirrors zeros/ones
// shape parsing. See BUGS.md #30.
void true_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        outs[0] = Value::logicalScalar(true, mr);
        return;
    }
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto d = parseDimsArgsND(args, "true", scratch.allocator());
    stripTrailingOnes(d);
    auto v = createMatrixND(d.data(), d.size(), ValueType::LOGICAL, mr);
    keel::u8 *p = v.logicalDataMut();
    for (keel::usize i = 0; i < v.numel(); ++i) p[i] = 1;
    outs[0] = std::move(v);
}

void false_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        outs[0] = Value::logicalScalar(false, mr);
        return;
    }
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto d = parseDimsArgsND(args, "false", scratch.allocator());
    stripTrailingOnes(d);
    // createMatrixND zero-fills LOGICAL by default.
    outs[0] = createMatrixND(d.data(), d.size(), ValueType::LOGICAL, mr);
}

void eye_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "eye");
    auto *mr = ctx.engine->allocator();
    ValueType t;
    auto dimArgs = extractTypeArg(args, t);
    auto d = parseDimsArgs(dimArgs, "eye");
    if (t == ValueType::DOUBLE) {
        // Fast path: direct double eye().
        outs[0] = eye(d.rows, d.cols, mr);
        return;
    }
    // Typed eye: zero-fill matrix of `t`, then set diagonal to one.
    auto m = Value::matrix(d.rows, d.cols, t, mr);
    const keel::usize k = std::min(d.rows, d.cols);
    switch (t) {
      case ValueType::SINGLE: { auto *p = m.singleDataMut(); for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1.0f; break; }
      case ValueType::LOGICAL:{ auto *p = m.logicalDataMut(); for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      case ValueType::INT8:   { auto *p = m.int8DataMut();    for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      case ValueType::INT16:  { auto *p = m.int16DataMut();   for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      case ValueType::INT32:  { auto *p = m.int32DataMut();   for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      case ValueType::INT64:  { auto *p = m.int64DataMut();   for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      case ValueType::UINT8:  { auto *p = m.uint8DataMut();   for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      case ValueType::UINT16: { auto *p = m.uint16DataMut();  for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      case ValueType::UINT32: { auto *p = m.uint32DataMut();  for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      case ValueType::UINT64: { auto *p = m.uint64DataMut();  for (keel::usize i = 0; i < k; ++i) p[i + i*d.rows] = 1; break; }
      default: throw Error("eye: unsupported type", 0, 0, "eye", "", "numkit:eye:badType");
    }
    outs[0] = std::move(m);
}

void magic_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "magic");
    if (args.size() != 1)
        throw Error("magic: requires exactly 1 argument",
                     0, 0, "magic", "", "numkit:magic:nargin");
    const keel::f64 nd = args[0].toScalar();
    if (nd < 0.0 || nd != std::floor(nd))
        throw Error("magic: N must be a non-negative integer",
                     0, 0, "magic", "", "numkit:magic:badN");
    outs[0] = magic(keel::as<keel::usize>(nd), ctx.engine->allocator());
}

void toeplitz_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "toeplitz");
    if (args.size() < 1 || args.size() > 2)
        throw Error("toeplitz: requires 1 or 2 arguments",
                    0, 0, "toeplitz", "", "numkit:toeplitz:nargin");
    const Value &r = args.size() == 2 ? args[1] : Value::Empty;
    outs[0] = toeplitz(args[0], r, ctx.engine->allocator());
}

void hankel_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "hankel");
    if (args.size() < 1 || args.size() > 2)
        throw Error("hankel: requires 1 or 2 arguments",
                    0, 0, "hankel", "", "numkit:hankel:nargin");
    const Value &r = args.size() == 2 ? args[1] : Value::Empty;
    outs[0] = hankel(args[0], r, ctx.engine->allocator());
}

void vander_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "vander");
    if (args.size() != 1)
        throw Error("vander: requires exactly 1 argument",
                    0, 0, "vander", "", "numkit:vander:nargin");
    outs[0] = vander(args[0], ctx.engine->allocator());
}

void compan_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "compan");
    if (args.size() != 1)
        throw Error("compan: requires exactly 1 argument",
                    0, 0, "compan", "", "numkit:compan:nargin");
    outs[0] = compan(args[0], ctx.engine->allocator());
}

namespace {

// Common gateway for the "size-from-scalar" test-matrix functions
// (pascal, hilb, invhilb, wilkinson, hadamard).
keel::usize requireSizeArg(keel::ArrayRef<Value> args, const char *fn)
{
    if (args.size() != 1)
        throw Error(keel::format("{}: requires exactly 1 argument", fn),
                    0, 0, fn, "", keel::format("numkit:{}:nargin", fn));
    const keel::f64 nd = args[0].toScalar();
    if (nd < 0.0 || nd != std::floor(nd))
        throw Error(keel::format("{}: N must be a non-negative integer", fn),
                    0, 0, fn, "", keel::format("numkit:{}:badN", fn));
    return keel::as<keel::usize>(nd);
}

} // anonymous namespace

void pascal_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "pascal");
    outs[0] = pascal(requireSizeArg(args, "pascal"), ctx.engine->allocator());
}

void hilb_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "hilb");
    outs[0] = hilb(requireSizeArg(args, "hilb"), ctx.engine->allocator());
}

void invhilb_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "invhilb");
    outs[0] = invhilb(requireSizeArg(args, "invhilb"), ctx.engine->allocator());
}

void wilkinson_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "wilkinson");
    outs[0] = wilkinson(requireSizeArg(args, "wilkinson"), ctx.engine->allocator());
}

void hadamard_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "hadamard");
    outs[0] = hadamard(requireSizeArg(args, "hadamard"), ctx.engine->allocator());
}

void rosser_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 0, "rosser");
    outs[0] = rosser(ctx.engine->allocator());
}

// NOTE: inv_reg migrated to toolboxes/linalg (properties.cpp).

// NOTE: linsolve_reg migrated to toolboxes/linalg (solvers.cpp).
// NOTE: pageinv_reg  migrated to toolboxes/linalg (page_ops.cpp).

// NOTE: trace_reg / det_reg migrated to toolboxes/linalg (properties.cpp).
// NOTE: chol_reg / lu_reg / qr_reg / svd_reg migrated to
//       toolboxes/linalg (decompositions.cpp).
// NOTE: rank_reg / cond_reg / normest_reg migrated to
//       toolboxes/linalg (properties.cpp).
// NOTE: pinv_reg / orth_reg / null_reg migrated to
//       toolboxes/linalg (pseudo_subspace.cpp).
// NOTE: eig_reg / hess_reg / schur_reg / sylvester_reg /
//       expm_reg / logm_reg / sqrtm_reg migrated to toolboxes/linalg
//       (eig.cpp, matrix_functions.cpp). Block below disabled.

void topkrows_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "topkrows");
    const keel::f64 kd = args[1].toScalar();
    if (kd < 0.0 || kd != std::floor(kd))
        throw Error("topkrows: k must be a non-negative integer",
                    0, 0, "topkrows", "", "numkit:topkrows:badK");
    auto *mr = ctx.engine->allocator();

    auto toLower = [mr](keel::StringRef sv) {
        keel::String s = keel::String::from(sv, mr);
        for (keel::usize idx = 0; idx < s.size(); ++idx)
            s[idx] = keel::to_ascii_lower(s[idx]);
        return s;
    };
    auto parseDirStr = [&](keel::StringRef raw) -> keel::u8 {
        keel::String s = toLower(raw);
        if (s == "descend") return 1u;
        if (s == "ascend")  return 0u;
        throw Error("topkrows: direction must be 'ascend' or 'descend'",
                    0, 0, "topkrows", "", "numkit:topkrows:badDir");
    };

    keel::Array<keel::usize> cols(mr);
    keel::Array<keel::u8> desc(mr);
    keel::String compMethod = keel::String::from("auto", mr);
    keel::usize i = 2;
    if (i < args.size() && !args[i].isChar() && !args[i].isString() && !args[i].isCell()) {
        // Numeric col vector.
        const Value &c = args[i];
        cols.reserve(c.numel());
        for (keel::usize kk = 0; kk < c.numel(); ++kk) {
            const keel::f64 v = c.elemAsDouble(kk);
            if (v < 1.0 || v != std::floor(v))
                throw Error("topkrows: col must be a positive integer "
                            "or vector of positive integers",
                            0, 0, "topkrows", "", "numkit:topkrows:badCol");
            cols.push_back(keel::as<keel::usize>(v) - 1);
        }
        ++i;
    }
    if (i < args.size()) {
        if (args[i].isCell() || (args[i].isString() && args[i].numel() > 1)) {
            const auto &vec = args[i].cellDataVec();
            desc.reserve(vec.size());
            for (const auto &el : vec)
                desc.push_back(parseDirStr(el.toString()));
            ++i;
        } else if (args[i].isChar() || args[i].isString()) {
            const keel::String s = toLower(args[i].toString());
            if (s == "comparisonmethod") {
                if (i + 1 >= args.size() || (!args[i + 1].isChar() && !args[i + 1].isString()))
                    throw Error("topkrows: ComparisonMethod must be 'auto', 'real', or 'abs'",
                                0, 0, "topkrows", "", "numkit:topkrows:badCompMethod");
                compMethod = toLower(args[i + 1].toString());
                if (compMethod != "auto" && compMethod != "real" && compMethod != "abs")
                    throw Error("topkrows: ComparisonMethod must be 'auto', 'real', or 'abs'",
                                0, 0, "topkrows", "", "numkit:topkrows:badCompMethod");
                i += 2;
            } else {
                desc.push_back(parseDirStr(s));
                ++i;
            }
        }
    }
    // Optional trailing 'ComparisonMethod' NV (after dir).
    while (i + 1 < args.size() && (args[i].isChar() || args[i].isString())) {
        const keel::String nm = toLower(args[i].toString());
        if (nm == "comparisonmethod") {
            if (!args[i + 1].isChar() && !args[i + 1].isString())
                throw Error("topkrows: ComparisonMethod must be 'auto', 'real', or 'abs'",
                            0, 0, "topkrows", "", "numkit:topkrows:badCompMethod");
            compMethod = toLower(args[i + 1].toString());
            if (compMethod != "auto" && compMethod != "real" && compMethod != "abs")
                throw Error("topkrows: ComparisonMethod must be 'auto', 'real', or 'abs'",
                            0, 0, "topkrows", "", "numkit:topkrows:badCompMethod");
            i += 2;
            continue;
        }
        throw Error(keel::format("topkrows: unexpected argument '{}'", args[i].toString()),
                    0, 0, "topkrows", "", "numkit:topkrows:badArg");
    }

    const Value &A = args[0];
    if (A.isComplex() || compMethod == "abs") {
        if (A.dims().ndim() != 2)
            throw Error("topkrows: input must be a 2D matrix",
                        0, 0, "topkrows", "", "numkit:topkrows:notMatrix");
        const keel::usize m = A.dims().rows();
        const keel::usize n = A.dims().cols();
        const keel::usize kk = std::min(keel::as<keel::usize>(kd), m);
        if (cols.empty()) {
            cols.reserve(n);
            for (keel::usize j = 0; j < n; ++j) cols.push_back(j);
        }
        for (keel::usize j : cols)
            if (j >= n)
                throw Error("topkrows: column index out of range",
                            0, 0, "topkrows", "", "numkit:topkrows:badCol");
        if (desc.empty()) desc.assign(cols.size(), 1u);
        else if (desc.size() == 1) desc.assign(cols.size(), desc[0]);
        else if (desc.size() != cols.size())
            throw Error("topkrows: direction must be a scalar or match the length of col",
                        0, 0, "topkrows", "", "numkit:topkrows:dirSize");

        keel::Array<keel::usize> ord(mr);
        ord.resize(m);
        for (keel::usize r = 0; r < m; ++r) ord[r] = r;
        const bool isCplx = A.isComplex();
        const bool useReal = (compMethod == "real") || (compMethod == "auto" && !isCplx);
        auto getCplx = [&](keel::usize r, keel::usize c) -> keel::c64 {
            return isCplx ? A.complexData()[r + c * m] : keel::c64(A.elemAsDouble(r + c * m), 0.0);
        };
        std::stable_sort(ord.begin(), ord.end(), [&](keel::usize a, keel::usize b) {
            for (keel::usize k2 = 0; k2 < cols.size(); ++k2) {
                const keel::usize j = cols[k2];
                const keel::c64 za = getCplx(a, j);
                const keel::c64 zb = getCplx(b, j);
                keel::f64 ka1, ka2, kb1, kb2;
                if (useReal) {
                    ka1 = za.real(); ka2 = za.imag();
                    kb1 = zb.real(); kb2 = zb.imag();
                } else {
                    ka1 = keel::abs(za); ka2 = std::atan2(za.imag(), za.real());
                    kb1 = keel::abs(zb); kb2 = std::atan2(zb.imag(), zb.real());
                }
                if (ka1 != kb1) return desc[k2] ? (ka1 > kb1) : (ka1 < kb1);
                if (ka2 != kb2) return desc[k2] ? (ka2 > kb2) : (ka2 < kb2);
            }
            return false;
        });

        Value out = (n == 0) ? Value::matrix(kk, 0, A.type(), mr)
                             : Value::matrix(kk, n, A.type(), mr);
        for (keel::usize r = 0; r < kk; ++r) {
            const keel::usize srcR = ord[r];
            for (keel::usize j = 0; j < n; ++j) {
                const keel::usize srcLin = srcR + j * m;
                const keel::usize dstLin = r + j * kk;
                if (isCplx)
                    out.complexDataMut()[dstLin] = A.complexData()[srcLin];
                else
                    out.doubleDataMut()[dstLin] = A.elemAsDouble(srcLin);
            }
        }
        outs[0] = std::move(out);
        if (nargout >= 2) {
            auto I = Value::matrix(kk, 1, ValueType::DOUBLE, mr);
            keel::f64 *id = I.doubleDataMut();
            for (keel::usize k2 = 0; k2 < kk; ++k2)
                id[k2] = keel::as<keel::f64>(ord[k2] + 1);
            outs[1] = std::move(I);
        }
        return;
    }

    keel::Array<keel::usize> idx_out(mr);
    auto B = topkrows_full(args[0], keel::as<keel::usize>(kd),
                            keel::ArrayRef<keel::usize>(cols.data(), cols.size()),
                            keel::ArrayRef<keel::u8>(desc.data(), desc.size()),
                            (nargout >= 2) ? &idx_out : nullptr, mr);
    outs[0] = std::move(B);
    if (nargout >= 2) {
        auto I = Value::matrix(idx_out.size(), 1, ValueType::DOUBLE, mr);
        keel::f64 *id = I.doubleDataMut();
        for (keel::usize k2 = 0; k2 < idx_out.size(); ++k2)
            id[k2] = keel::f64(idx_out[k2] + 1);  // 1-indexed
        outs[1] = std::move(I);
    }
}

void size_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "size");
    auto *mr = ctx.engine->allocator();

    if (args.size() >= 2) {
        outs[0] = size(args[0], argcheck::dimArg(args[1], "size"), mr);
        return;
    }

    if (nargout > 1) {
        const auto &dims = args[0].dims();
        // Multi-output form: [r, c] = size(A) or [r, c, p, ...] = size(A).
        // For ND tensors, dims past nargout-1 are gathered into the last
        // requested output (MATLAB behaviour: extra-dim sizes multiplied
        // into the trailing slot). For dims past actual ndim, return 1.
        for (keel::usize i = 0; i < nargout && i < outs.size(); ++i) {
            keel::f64 v;
            if (i + 1 < nargout) {
                v = keel::as<keel::f64>(dims.dim(keel::as<keel::i32>(i)));
            } else {
                // Last requested output: multiply remaining dims (if any).
                keel::usize prod = 1;
                for (keel::i32 j = keel::as<keel::i32>(i); j < dims.ndim(); ++j)
                    prod *= dims.dim(j);
                if (dims.ndim() <= keel::as<keel::i32>(i)) prod = 1;
                v = keel::as<keel::f64>(prod);
            }
            outs[i] = Value::scalar(v, mr);
        }
        return;
    }

    outs[0] = size(args[0], mr);
}

void length_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "length");
    outs[0] = length(args[0], ctx.engine->allocator());
}

void numel_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "numel");
    outs[0] = numel(args[0], ctx.engine->allocator());
}

void ndims_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "ndims");
    outs[0] = ndims(args[0], ctx.engine->allocator());
}

// reshape's Value-boundary resolver (the plan-resolver shape for a
// native-dims API): validates the size inputs and resolves [] inference,
// returning the final dim vector. Lives at the reg layer, not in compute,
// because the public C++ API takes size_t dims which cannot express what
// this resolves (bugs/closed/lang/reshape-size-arg-validation.md).
namespace {
keel::Array<keel::usize> parseReshapeDims(const Value &x, keel::ArrayRef<Value> sizeArgs,
                                    const keel::ArenaTemp &scratch)
{
    keel::Array<keel::usize> dims(scratch.allocator());

    // Dims-vector form: reshape(A, [m n p ...]) — any numeric class, never
    // complex. No [] inference here.
    if (sizeArgs.size() == 1 && !sizeArgs[0].isScalar()
        && !sizeArgs[0].isEmpty()) {
        argcheck::reshapeSizeReal(sizeArgs[0], "reshape");
        dims.reserve(sizeArgs[0].numel());
        for (keel::usize i = 0; i < sizeArgs[0].numel(); ++i)
            dims.push_back(
                argcheck::reshapeSize(sizeArgs[0].elemAsDouble(i), "reshape"));
    } else {
        // Scalar-args form: reshape(A, m, n, ...). One [] allowed for
        // dimension inference from x.numel().
        const keel::usize dimCount = sizeArgs.size();
        dims.assign(dimCount, 1);
        keel::i32 inferPos = -1;
        keel::usize knownProd = 1;
        for (keel::usize i = 0; i < dimCount; ++i) {
            if (sizeArgs[i].isEmpty()) {
                if (inferPos >= 0)
                    throw Error("reshape: only one size dimension may be "
                                "inferred with []; found a second one.",
                                 0, 0, "reshape", "",
                                 "numkit:reshape:multipleInferred");
                inferPos = keel::as<keel::i32>(i);
            } else {
                dims[i] = argcheck::reshapeSize(
                    argcheck::reshapeSizeArg(sizeArgs[i], "reshape"),
                    "reshape");
                knownProd *= dims[i];
            }
        }
        if (inferPos >= 0) {
            if (knownProd == 0 || x.numel() % knownProd != 0)
                throw Error(keel::format("reshape: product of known dimensions ({}) "
                                         "does not divide the total number of elements ({}).",
                                         knownProd, x.numel()),
                             0, 0, "reshape", "",
                             "numkit:reshape:notDivisible");
            dims[inferPos] = x.numel() / knownProd;
        }
    }

    // Strip trailing 1s past the 2nd dim (MATLAB convention).
    stripTrailingOnes(dims);
    return dims;
}
} // namespace

void reshape_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "reshape");
    auto *mr = ctx.engine->allocator();
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::Array<keel::usize> dims = parseReshapeDims(args[0], args.slice(1),
                                               scratch);
    outs[0] = reshape(args[0], keel::ArrayRef<keel::usize>(dims.data(), dims.size()), mr);
}

void transpose_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "transpose");
    outs[0] = transpose(args[0], ctx.engine->allocator());
}

void pagetranspose_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                       CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "pagetranspose");
    outs[0] = pagetranspose(args[0], ctx.engine->allocator());
}

void pagectranspose_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                        CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "pagectranspose");
    outs[0] = pagectranspose(args[0], ctx.engine->allocator());
}

void peaks_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
               CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "peaks");
    keel::usize n = 49;  // MATLAB default
    if (!args.empty()) {
        const keel::f64 dn = args[0].toScalar();
        if (dn < 0 || dn > 1.0e9 || std::isnan(dn))
            throw Error("peaks: n must be a non-negative integer",
                         0, 0, "peaks", "", "numkit:peaks:badN");
        n = keel::as<keel::usize>(dn);
    }
    outs[0] = peaks(n, ctx.engine->allocator());
}

void sphere_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                CallContext &ctx)
{
    keel::usize n = 20;  // MATLAB default
    if (!args.empty())
        n = keel::as<keel::usize>(argcheck::expectedPositiveInteger(
            argcheck::expectedReal(args[0], 1, "n", "sphere"), 1, "n", "sphere"));
    auto s = sphere(n, ctx.engine->allocator());
    outs[0] = std::move(s.X);
    if (nargout > 1) outs[1] = std::move(s.Y);
    if (nargout > 2) outs[2] = std::move(s.Z);
}

void cylinder_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    keel::usize n = 20;
    Value R;
    if (args.empty()) {
        // Default profile [1 1].
        R = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        R.doubleDataMut()[0] = 1.0;
        R.doubleDataMut()[1] = 1.0;
    } else {
        if (args[0].isScalar()) {
            // cylinder(n) — single integer arg is `n`, R defaults to [1 1].
            n = keel::as<keel::usize>(argcheck::expectedPositiveInteger(
                argcheck::expectedReal(args[0], 1, "n", "cylinder"), 1, "n", "cylinder"));
            R = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            R.doubleDataMut()[0] = 1.0;
            R.doubleDataMut()[1] = 1.0;
        } else {
            R = args[0];
            if (args.size() >= 2)
                n = keel::as<keel::usize>(argcheck::expectedPositiveInteger(
                    argcheck::expectedReal(args[1], 2, "n", "cylinder"), 2, "n", "cylinder"));
        }
    }
    auto s = cylinder(R, n, mr);
    outs[0] = std::move(s.X);
    if (nargout > 1) outs[1] = std::move(s.Y);
    if (nargout > 2) outs[2] = std::move(s.Z);
}

void ellipsoid_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                   CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 6, "ellipsoid");
    const keel::f64 xc = args[0].toScalar();
    const keel::f64 yc = args[1].toScalar();
    const keel::f64 zc = args[2].toScalar();
    const keel::f64 xr = args[3].toScalar();
    const keel::f64 yr = args[4].toScalar();
    const keel::f64 zr = args[5].toScalar();
    keel::usize n = 20;
    if (args.size() >= 7)
        n = keel::as<keel::usize>(argcheck::expectedPositiveInteger(
            argcheck::expectedReal(args[6], 7, "n", "ellipsoid"), 7, "n", "ellipsoid"));
    auto s = ellipsoid(xc, yc, zc, xr, yr, zr, n, ctx.engine->allocator());
    outs[0] = std::move(s.X);
    if (nargout > 1) outs[1] = std::move(s.Y);
    if (nargout > 2) outs[2] = std::move(s.Z);
}

void pagemtimes_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "pagemtimes");
    auto parseFlag = [](const Value &v) -> TranspOp {
        if (!v.isChar() && !v.isString())
            throw Error("pagemtimes: transpose flag must be a string",
                         0, 0, "pagemtimes", "", "numkit:pagemtimes:flagType");
        const keel::String s = v.toString();
        if (s == "none")       return TranspOp::None;
        if (s == "transpose")  return TranspOp::Transpose;
        if (s == "ctranspose") return TranspOp::CTranspose;
        throw Error(keel::format("pagemtimes: invalid transpose flag '{}' "
                                 "(expected 'none', 'transpose', or 'ctranspose')", s),
                     0, 0, "pagemtimes", "", "numkit:pagemtimes:invalidFlag");
    };
    keel::Allocator *mr = ctx.engine->allocator();
    if (args.size() == 2) {
        outs[0] = pagemtimes(args[0], args[1], mr);
        return;
    }
    if (args.size() == 4) {
        outs[0] = pagemtimes(args[0], parseFlag(args[1]), args[2], parseFlag(args[3]), mr);
        return;
    }
    throw Error("pagemtimes: expected 2 or 4 arguments",
                 0, 0, "pagemtimes", "", "numkit:pagemtimes:nargin");
}

void diag_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "diag");
    keel::i64 k = 0;
    if (args.size() >= 2)
        k = argcheck::expectedInteger(
            argcheck::expectedReal(args[1], 2, "k", "diag"), 2, "k", "diag");
    outs[0] = numkit::narrowComplex(diag(args[0], k, ctx.engine->allocator()),
                                    ctx.engine->allocator());
}

void sort_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "sort");
    // sort(X[, dim][, direction]): a numeric trailing arg is the dim, a
    // string is the direction ('ascend' default / 'descend'). MATLAB
    // accepts sort(X,direction) and sort(X,dim,direction).
    keel::i32 dim = -1;
    bool descend = false;
    NanPlace nanPlace = NanPlace::Auto;
    auto lower = [](keel::StringRef sv) {
        keel::String s = keel::String::from(sv);
        for (keel::usize idx = 0; idx < s.size(); ++idx)
            if (s[idx] >= 'A' && s[idx] <= 'Z') s[idx] = char(s[idx] + 32);
        return s;
    };
    for (keel::usize i = 1; i < args.size(); ++i) {
        if (args[i].isEmpty()) continue;
        if (args[i].isChar() || args[i].isString()) {
            const keel::String d = lower(args[i].toString());
            if (d == "descend") descend = true;
            else if (d == "ascend") descend = false;
            else if (d == "missingplacement" && i + 1 < args.size()) {
                const keel::String v = lower(args[i + 1].toString());
                nanPlace = (v == "first") ? NanPlace::First
                         : (v == "last")  ? NanPlace::Last
                                          : NanPlace::Auto;
                ++i;  // consume the placement value
            }
            // ignore other Name-Value tokens (e.g. ComparisonMethod)
        } else {
            dim = argcheck::dimArg(args[i], "sort");
        }
    }
    auto *mr = ctx.engine->allocator();
    // Integer types: MATLAB keeps the class on the sorted VALUES (the index
    // output stays double). Sort through the double path (the order is
    // identical) then cast the sorted values back to the integer class.
    if (isIntegerType(args[0].type())) {
        const ValueType vt = args[0].type();
        Value xd = copyToDouble(args[0], mr);
        auto [sortedD, idxD] = sort(xd, dim, descend, nanPlace, mr);
        outs[0] = doubleToIntegerExact(sortedD, vt, mr);
        if (nargout > 1)
            outs[1] = std::move(idxD);
        return;
    }
    // Logical: MATLAB keeps the logical class on the sorted VALUES (the index
    // output stays double) — same shape as the integer path above. Sort via
    // the double path (order/indices identical) then narrow 0/1 -> LOGICAL.
    // (Only logical is handled here; sorting a CHAR array is a separate
    // still-unsupported case — see bugs/builtin/sort-logical.md "Related".)
    if (args[0].isLogical()) {
        Value xd = copyToDouble(args[0], mr);
        auto [sortedD, idxD] = sort(xd, dim, descend, nanPlace, mr);
        outs[0] = logicalizeCumResult(sortedD, mr);
        if (nargout > 1)
            outs[1] = std::move(idxD);
        return;
    }
    // Char: MATLAB sorts char by code point and PRESERVES the char class on the
    // VALUES (index stays double) — same shape as the integer/logical branches.
    // Gated on isChar(); string arrays sort through a different path and are
    // left untouched. bugs/builtin/sort-char.md.
    if (args[0].isChar()) {
        Value xd = copyToDouble(args[0], mr);
        auto [sortedD, idxD] = sort(xd, dim, descend, nanPlace, mr);
        outs[0] = charizeSortResult(sortedD, mr);
        if (nargout > 1)
            outs[1] = std::move(idxD);
        return;
    }
    auto [sorted, idx] = sort(args[0], dim, descend, nanPlace, mr);
    outs[0] = std::move(sorted);
    if (nargout > 1)
        outs[1] = std::move(idx);
}

void sortrows_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "sortrows");
    keel::Allocator *mr = ctx.engine->allocator();
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto cols = keel::Array<keel::i32>(scratch.allocator());

    // 'ascend' (false) / 'descend' (true), case-insensitive.
    auto dirDescend = [](keel::StringRef sv) -> bool {
        keel::String s = keel::String::from(sv);
        for (keel::usize idx = 0; idx < s.size(); ++idx)
            s[idx] = keel::to_ascii_lower(s[idx]);
        if (s == "ascend") return false;
        if (s == "descend") return true;
        throw Error("sortrows: direction must be 'ascend' or 'descend'",
                     0, 0, "sortrows", "", "numkit:sortrows:badDirection");
    };
    // CHAR/STRING scalar → one flag; CELL or multi-element STRING → one per
    // element (both store their elements as Values in cellDataVec()).
    auto collectDirs = [&](const Value &v, keel::Array<keel::u8> &out) {
        if (v.type() == ValueType::CELL ||
            (v.type() == ValueType::STRING && v.numel() > 1)) {
            const auto &vec = v.cellDataVec();
            for (const auto &e : vec)
                out.push_back(dirDescend(e.toString()) ? 1u : 0u);
        } else {
            out.push_back(dirDescend(v.toString()) ? 1u : 0u);
        }
    };
    auto isDirArg = [](const Value &v) {
        return v.type() == ValueType::CHAR || v.type() == ValueType::STRING ||
               v.type() == ValueType::CELL;
    };

    if (args.size() >= 2 && !args[1].isEmpty()) {
        const auto &c = args[1];
        if (isDirArg(c)) {
            // sortrows(A, direction[s]) — direction(s) applied over ALL
            // columns. A single direction covers every column; a cell/string
            // array must supply exactly one direction per column.
            const keel::usize C = args[0].dims().cols();
            keel::Array<keel::u8> dirs(scratch.allocator());
            collectDirs(c, dirs);
            if (dirs.size() == 1) {
                const bool d = dirs[0] != 0;
                for (keel::usize k = 1; k <= C; ++k)
                    cols.push_back(d ? -keel::as<keel::i32>(k) : keel::as<keel::i32>(k));
            } else {
                if (dirs.size() != C)
                    throw Error("sortrows: number of directions must match the "
                                "number of columns",
                                 0, 0, "sortrows", "", "numkit:sortrows:dirCount");
                for (keel::usize k = 0; k < C; ++k)
                    cols.push_back(dirs[k] ? -keel::as<keel::i32>(k + 1)
                                           : keel::as<keel::i32>(k + 1));
            }
        } else {
            cols.reserve(c.numel());
            for (keel::usize i = 0; i < c.numel(); ++i) {
                const keel::f64 v = c.elemAsDouble(i);
                if (v != std::floor(v))
                    throw Error("sortrows: column index must be an integer",
                                 0, 0, "sortrows", "", "numkit:sortrows:badCol");
                cols.push_back(keel::as<keel::i32>(v));
            }
            // Optional direction argument: sortrows(A, cols, direction[s]).
            // Re-signs the listed columns by direction (overriding any sign in
            // the numeric spec). A single direction covers all listed columns.
            if (args.size() >= 3 && !args[2].isEmpty()) {
                if (!isDirArg(args[2]))
                    throw Error("sortrows: direction must be 'ascend' or "
                                "'descend'",
                                 0, 0, "sortrows", "", "numkit:sortrows:badDirection");
                keel::Array<keel::u8> dirs(scratch.allocator());
                collectDirs(args[2], dirs);
                if (dirs.size() == 1) {
                    const bool d = dirs[0] != 0;
                    for (auto &cc : cols) {
                        const keel::i32 a = cc < 0 ? -cc : cc;
                        cc = d ? -a : a;
                    }
                } else {
                    if (dirs.size() != cols.size())
                        throw Error("sortrows: number of directions must match "
                                    "the number of sort columns",
                                     0, 0, "sortrows", "", "numkit:sortrows:dirCount");
                    for (keel::usize i = 0; i < cols.size(); ++i) {
                        const keel::i32 a = cols[i] < 0 ? -cols[i] : cols[i];
                        cols[i] = dirs[i] ? -a : a;
                    }
                }
            }
        }
    }
    auto [sorted, idx] = sortrows(args[0], keel::ArrayRef<keel::i32>(cols.data(), cols.size()), mr);
    outs[0] = std::move(sorted);
    if (nargout > 1)
        outs[1] = std::move(idx);
}

void find_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "find");
    auto *mr = ctx.engine->allocator();
    const Value &x = args[0];

    // Optional count K (args[1]) and direction (args[2]: 'first' default /
    // 'last'). MATLAB: K must be a positive scalar integer; K exceeding the
    // nonzero count returns all. 'last' returns the last K nonzeros, still in
    // ascending index order. Applies to BOTH the single-output (linear
    // indices) and the [r,c] / [r,c,v] subscript forms.
    bool haveK = args.size() >= 2 && !args[1].isEmpty();
    keel::usize kReq = 0;
    if (haveK) {
        if (args[1].numel() != 1)
            throw Error("find: Second argument must be a positive scalar integer.",
                         0, 0, "find", "", "numkit:find:badK");
        const keel::f64 kd = args[1].toScalar();
        if (!std::isfinite(kd) || kd < 1.0 || kd != std::floor(kd))
            throw Error("find: Second argument must be a positive scalar integer.",
                         0, 0, "find", "", "numkit:find:badK");
        kReq = keel::as<keel::usize>(kd);
    }
    bool fromLast = false;
    if (args.size() >= 3 && !args[2].isEmpty()) {
        if (!(args[2].isChar() || args[2].isString()))
            throw Error("find: third argument must be 'first' or 'last'",
                         0, 0, "find", "", "numkit:find:badDir");
        keel::String d = args[2].toString();
        for (char &ch : d)
            if (ch >= 'A' && ch <= 'Z') ch = keel::as<char>(ch + 32);
        if (d == "last") fromLast = true;
        else if (d == "first") fromLast = false;
        else
            throw Error("find: third argument must be 'first' or 'last'",
                         0, 0, "find", "", "numkit:find:badDir");
    }

    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto lin = keel::Array<keel::usize>(scratch.allocator());
    forEachNonzero(x, [&](keel::usize i) { lin.push_back(i); });

    // Window of [start, start+count) into the ascending nonzero-index list.
    keel::usize start = 0, count = lin.size();
    if (haveK && kReq < lin.size()) {
        count = kReq;
        start = fromLast ? lin.size() - kReq : 0;
    }

    const keel::usize R = x.dims().rows() == 0 ? 1 : x.dims().rows();
    const bool rowResult = !x.dims().is3D() && x.dims().rows() == 1;
    auto mk = [&](ValueType t) {
        return rowResult ? Value::matrix(1, count, t, mr)
                         : Value::matrix(count, 1, t, mr);
    };

    if (nargout <= 1) {
        Value r = mk(ValueType::DOUBLE);
        keel::f64 *rd = r.doubleDataMut();
        for (keel::usize t = 0; t < count; ++t)
            rd[t] = keel::as<keel::f64>(lin[start + t] + 1);
        outs[0] = std::move(r);
        return;
    }

    // [r, c] = find(X) / [r, c, v] = find(X): row/column subscripts (and the
    // nonzero values). Subscripts/values inherit X's vector orientation (row
    // vector → row results, otherwise column results), matching MATLAB.
    Value rowV = mk(ValueType::DOUBLE);
    Value colV = mk(ValueType::DOUBLE);
    keel::f64 *rd = rowV.doubleDataMut();
    keel::f64 *cd = colV.doubleDataMut();
    for (keel::usize t = 0; t < count; ++t) {
        const keel::usize i = lin[start + t];
        rd[t] = keel::as<keel::f64>(i % R + 1);
        cd[t] = keel::as<keel::f64>(i / R + 1);
    }
    outs[0] = std::move(rowV);
    outs[1] = std::move(colV);

    if (nargout >= 3) {
        if (x.type() == ValueType::COMPLEX) {
            Value valV = mk(ValueType::COMPLEX);
            const keel::c64 *src = x.complexData();
            keel::c64 *vd = valV.complexDataMut();
            for (keel::usize t = 0; t < count; ++t) vd[t] = src[lin[start + t]];
            outs[2] = std::move(valV);
        } else {
            Value valV = mk(ValueType::DOUBLE);
            keel::f64 *vd = valV.doubleDataMut();
            for (keel::usize t = 0; t < count; ++t) vd[t] = x.elemAsDouble(lin[start + t]);
            outs[2] = std::move(valV);
        }
    }
}

void nnz_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "nnz");
    outs[0] = nnz(args[0], ctx.engine->allocator());
}

void nonzeros_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "nonzeros");
    outs[0] = nonzeros(args[0], ctx.engine->allocator());
}

void horzcat_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (!args.empty() && args[0].isObject()) {
        if (const BuiltinClass *bc = ctx.engine->findClass(args[0].objectClassNameRef())) {
            if (const ObjectMethod *op = bc->ops.get("horzcat")) {
                Value lhs = args[0];
                (*op)(lhs, args.slice(1), nargout, outs, ctx);
                return;
            }
        }
    }
    outs[0] = horzcat(args, ctx.engine->allocator());
}

void vertcat_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (!args.empty() && args[0].isObject()) {
        if (const BuiltinClass *bc = ctx.engine->findClass(args[0].objectClassNameRef())) {
            if (const ObjectMethod *op = bc->ops.get("vertcat")) {
                Value lhs = args[0];
                (*op)(lhs, args.slice(1), nargout, outs, ctx);
                return;
            }
        }
    }
    outs[0] = vertcat(args, ctx.engine->allocator());
}

void meshgrid_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "meshgrid");
    auto *mr = ctx.engine->allocator();
    if (args.size() == 1) {
        // meshgrid(x) ≡ meshgrid(x, x). See BUGS.md #21.
        auto [X, Y] = meshgrid(args[0], args[0], mr);
        outs[0] = std::move(X);
        if (nargout > 1) outs[1] = std::move(Y);
        return;
    }
    if (args.size() == 2) {
        auto [X, Y] = meshgrid(args[0], args[1], mr);
        outs[0] = std::move(X);
        if (nargout > 1) outs[1] = std::move(Y);
        return;
    }
    if (args.size() == 3) {
        auto [X, Y, Z] = meshgrid(args[0], args[1], args[2], mr);
        outs[0] = std::move(X);
        if (nargout > 1) outs[1] = std::move(Y);
        if (nargout > 2) outs[2] = std::move(Z);
        return;
    }
    throw Error("meshgrid: 4+ inputs are not supported",
                 0, 0, "meshgrid", "", "numkit:meshgrid:tooManyInputs");
}

void ndgrid_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "ndgrid");
    keel::Allocator *mr = ctx.engine->allocator();
    if (args.size() == 1) {
        if (nargout <= 1) {
            keel::usize d[2] = {args[0].numel(), 1};
            outs[0] = reshape(args[0], keel::ArrayRef<keel::usize>(d, 2), mr);
            return;
        }
        if (nargout == 2) {
            auto [X, Y] = ndgrid(args[0], args[0], mr);
            outs[0] = std::move(X);
            outs[1] = std::move(Y);
            return;
        }
        if (nargout == 3) {
            auto [X, Y, Z] = ndgrid(args[0], args[0], args[0], mr);
            outs[0] = std::move(X);
            outs[1] = std::move(Y);
            outs[2] = std::move(Z);
            return;
        }
        const keel::usize D = nargout;
        const keel::usize n0 = args[0].numel();
        keel::Array<keel::usize> shape(mr);
        shape.assign(D, n0);
        keel::Array<keel::usize> strides(mr);
        strides.assign(D, 1);
        keel::usize total = (D == 0) ? 0 : 1;
        for (keel::usize k = 0; k < D; ++k) {
            strides[k] = total;
            total *= shape[k];
        }
        const keel::usize outCount = std::min(D, outs.size());
        for (keel::usize m = 0; m < outCount; ++m) {
            Value G = Value::matrixND(shape.data(), keel::as<keel::i32>(D), ValueType::DOUBLE, mr);
            keel::f64 *gd = G.doubleDataMut();
            const keel::usize stride_m = strides[m];
            for (keel::usize lin = 0; lin < total; ++lin) {
                const keel::usize coord = (lin / stride_m) % n0;
                gd[lin] = args[0].elemAsDouble(coord);
            }
            outs[m] = std::move(G);
        }
        return;
    }
    if (args.size() == 2) {
        auto [X, Y] = ndgrid(args[0], args[1], mr);
        outs[0] = std::move(X);
        if (nargout > 1) outs[1] = std::move(Y);
        return;
    }
    if (args.size() == 3) {
        auto [X, Y, Z] = ndgrid(args[0], args[1], args[2], mr);
        outs[0] = std::move(X);
        if (nargout > 1) outs[1] = std::move(Y);
        if (nargout > 2) outs[2] = std::move(Z);
        return;
    }
    const keel::usize D = args.size();
    keel::Array<keel::usize> shape(mr);
    shape.resize(D);
    keel::Array<keel::usize> strides(mr);
    strides.assign(D, 1);
    keel::usize total = 1;
    for (keel::usize k = 0; k < D; ++k) {
        shape[k] = args[k].numel();
        strides[k] = total;
        total *= shape[k];
    }
    const keel::usize wantOut = std::max<keel::usize>(1, nargout);
    const keel::usize outCount = std::min({wantOut, D, outs.size()});
    for (keel::usize m = 0; m < outCount; ++m) {
        Value G = Value::matrixND(shape.data(), keel::as<keel::i32>(D), ValueType::DOUBLE, mr);
        keel::f64 *gd = G.doubleDataMut();
        const keel::usize stride_m = strides[m];
        const keel::usize len_m = shape[m];
        for (keel::usize lin = 0; lin < total; ++lin) {
            const keel::usize coord = (lin / stride_m) % len_m;
            gd[lin] = args[m].elemAsDouble(coord);
        }
        outs[m] = std::move(G);
    }
}

// NOTE: kron_reg migrated to toolboxes/linalg/src/vector_ops.cpp.

// cumsum_reg / cumprod_reg are defined below (after the cum-flag helpers),
// where flip() is in scope — they parse the 'reverse'/'forward' direction
// and 'omitnan'/'includenan' flags like MATLAB.

// MATLAB cummax / cummin accept positional 'reverse' / 'omitnan' /
// 'includenan' string flags after the optional dim. Trick: 'reverse'
// = flip + cum + flip; 'includenan' propagation requires a second pass
// that fills NaN forward from the first NaN onwards (since the cum*
// kernel itself already skips NaN per omitnan default).
namespace {

void parseCumDirNan(keel::ArrayRef<Value> args, keel::usize start, const char *fn,
                    keel::i32 &dim, bool &reverse, bool &include_nan)
{
    dim = 0;
    reverse = false;
    include_nan = false;        // matches nk default = MATLAB default
    keel::usize i = start;
    if (i < args.size() && !args[i].isChar() && !args[i].isString()
        && !args[i].isEmpty()) {
        dim = argcheck::dimArg(args[i], fn); ++i;
    }
    while (i < args.size()) {
        if (!(args[i].isChar() || args[i].isString())) {
            throw Error("cummax/cummin: trailing positional must be a string flag",
                        0, 0, "cummax/cummin", "", "numkit:cum:badArg");
        }
        keel::String s = args[i].toString();
        for (keel::usize idx = 0; idx < s.size(); ++idx)
            s[idx] = keel::to_ascii_lower(s[idx]);
        if      (s == "reverse")    reverse = true;
        else if (s == "forward")    reverse = false;
        else if (s == "omitnan")    include_nan = false;
        else if (s == "includenan") include_nan = true;
        else
            throw Error(keel::format("cummax/cummin: unknown flag '{}'", s),
                        0, 0, "cummax/cummin", "", "numkit:cum:flag");
        ++i;
    }
}

// Propagate NaN forward in `out` based on the NaN positions in the
// (already same-shape) `src` input. Used to implement 'includenan'
// for cummax/cummin: once a NaN is hit in src along the operating
// dim, every subsequent output entry is set to NaN.
void propagateNanFromSrc(Value &out, const Value &src, keel::i32 dim1Based)
{
    const auto &dd = out.dims();
    const keel::i32 nd = dd.ndim();
    const keel::i32 d = dim1Based;
    if (d < 1 || d > nd) return;
    keel::usize inner = 1;
    for (keel::i32 i = 0; i < d - 1; ++i) inner *= dd.dim(i);
    keel::usize outer = 1;
    for (keel::i32 i = d; i < nd; ++i) outer *= dd.dim(i);
    const keel::usize L = dd.dim(d - 1);
    const keel::f64 qnan = std::numeric_limits<keel::f64>::quiet_NaN();
    // Complex cummax/cummin (out + src both COMPLEX): an element is "missing"
    // when EITHER component is NaN, and the propagated fill is NaN+NaNi.
    if (out.isComplex() && src.isComplex()) {
        keel::c64 *o = out.complexDataMut();
        const keel::c64 *s = src.complexData();
        for (keel::usize oc = 0; oc < outer; ++oc)
            for (keel::usize b = 0; b < inner; ++b) {
                const keel::usize base = oc * inner * L + b;
                bool seenNaN = false;
                keel::c64 fill{qnan, qnan};
                for (keel::usize k = 0; k < L; ++k) {
                    const keel::c64 z = s[base + k * inner];
                    if (!seenNaN && (std::isnan(z.real()) || std::isnan(z.imag()))) {
                        // includenan: the first NaN operand wins the running
                        // max/min and propagates UNCHANGED (MATLAB keeps its
                        // imaginary part: cummax([2 NaN+1i 3],'includenan') is
                        // [2 NaN+1i NaN+1i]) — not a canonical NaN+NaNi.
                        seenNaN = true;
                        fill = z;
                    }
                    if (seenNaN)
                        o[base + k * inner] = fill;
                }
            }
        return;
    }
    keel::f64 *o = out.doubleDataMut();
    const keel::f64 *s = src.doubleData();
    for (keel::usize oc = 0; oc < outer; ++oc)
        for (keel::usize b = 0; b < inner; ++b) {
            const keel::usize base = oc * inner * L + b;
            bool seenNaN = false;
            for (keel::usize k = 0; k < L; ++k) {
                if (!seenNaN && std::isnan(s[base + k * inner]))
                    seenNaN = true;
                if (seenNaN)
                    o[base + k * inner] = qnan;
            }
        }
}

template <typename Fn>
Value runCumWithFlags(const Value &x, keel::ArrayRef<Value> args, const char *fn,
                      Fn impl, keel::Allocator *mr)
{
    keel::i32 dim; bool reverse; bool include_nan;
    parseCumDirNan(args, 1, fn, dim, reverse, include_nan);
    Value src = x;
    if (reverse) src = flip(src, dim, mr);
    Value out = (dim > 0) ? impl(src, dim, mr) : impl(src, 0, mr);
    if (include_nan) {
        // Determine effective dim (firstNonSingleton when dim=0).
        keel::i32 effDim = dim;
        if (effDim <= 0) {
            const auto &dd = out.dims();
            effDim = 1;
            for (keel::i32 k = 0; k < dd.ndim(); ++k)
                if (dd.dim(k) > 1) { effDim = k + 1; break; }
        }
        propagateNanFromSrc(out, src, effDim);
    }
    if (reverse) {
        keel::i32 effDim = dim;
        if (effDim <= 0) {
            const auto &dd = out.dims();
            effDim = 1;
            for (keel::i32 k = 0; k < dd.ndim(); ++k)
                if (dd.dim(k) > 1) { effDim = k + 1; break; }
        }
        out = flip(out, effDim, mr);
    }
    return out;
}

} // anonymous

void cummax_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "cummax");
    outs[0] = numkit::narrowComplex(
        runCumWithFlags(args[0], args, "cummax", [](const Value &v, keel::i32 d, keel::Allocator *mr) {
            return cummax(v, d, mr);
        }, ctx.engine->allocator()),
        ctx.engine->allocator());
}

void cummin_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "cummin");
    outs[0] = numkit::narrowComplex(
        runCumWithFlags(args[0], args, "cummin", [](const Value &v, keel::i32 d, keel::Allocator *mr) {
            return cummin(v, d, mr);
        }, ctx.engine->allocator()),
        ctx.engine->allocator());
}

namespace {

// cumsum/cumprod option handling. Unlike cummax/cummin (whose kernel skips
// NaN), the cumsum/cumprod kernels PROPAGATE NaN — which is MATLAB's
// 'includenan' default. So 'omitnan' is implemented by replacing NaN with
// the additive/multiplicative identity (0 / 1) BEFORE the scan. 'reverse'
// = flip → scan → flip along the operating dimension.
Value cumScanFlags(const Value &x, keel::ArrayRef<Value> args, bool isProd,
                   const char *fn, keel::Allocator *mr)
{
    keel::i32 dim = 0; bool reverse = false; bool omitnan = false;
    keel::usize i = 1;
    if (i < args.size() && !args[i].isEmpty()
        && !args[i].isChar() && !args[i].isString()) {
        dim = argcheck::dimArg(args[i], fn); ++i;
    }
    for (; i < args.size(); ++i) {
        if (!(args[i].isChar() || args[i].isString())) continue;
        keel::String s = args[i].toString();
        for (keel::usize idx = 0; idx < s.size(); ++idx)
            s[idx] = keel::to_ascii_lower(s[idx]);
        if      (s == "reverse")    reverse = true;
        else if (s == "forward")    reverse = false;
        else if (s == "omitnan")    omitnan = true;
        else if (s == "includenan") omitnan = false;
    }
    // Effective (1-based) dim: first non-singleton when unspecified.
    keel::i32 effDim = dim;
    if (effDim <= 0) {
        const auto &dd = x.dims();
        effDim = 1;
        for (keel::i32 k = 0; k < dd.ndim(); ++k)
            if (dd.dim(k) > 1) { effDim = k + 1; break; }
    }
    Value src = x;
    if (omitnan && src.type() == ValueType::DOUBLE) {
        const keel::f64 id = isProd ? 1.0 : 0.0;
        keel::f64 *d = src.doubleDataMut();
        const keel::usize n = src.numel();
        for (keel::usize k = 0; k < n; ++k)
            if (std::isnan(d[k])) d[k] = id;
    }
    if (reverse) src = flip(src, effDim, mr);
    Value out = isProd ? cumprod(src, effDim, mr) : cumsum(src, effDim, mr);
    if (reverse) out = flip(out, effDim, mr);
    return out;
}

} // anonymous

void cumsum_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "cumsum");
    outs[0] = numkit::narrowComplex(
        cumScanFlags(args[0], args, /*isProd=*/false, "cumsum", ctx.engine->allocator()),
        ctx.engine->allocator());
}

void cumprod_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 4, "cumprod");
    outs[0] = numkit::narrowComplex(
        cumScanFlags(args[0], args, /*isProd=*/true, "cumprod", ctx.engine->allocator()),
        ctx.engine->allocator());
}

void diff_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "diff");
    keel::i32 n = 1;
    keel::i32 dim = 0;
    if (args.size() >= 2 && !args[1].isEmpty()) {
        // MATLAB: the difference order N must be a positive integer scalar
        // (a zero, negative, fractional or non-scalar order is an error).
        if (args[1].numel() != 1)
            throw Error("diff: Difference order N must be a positive integer scalar",
                         0, 0, "diff", "", "numkit:diff:badOrder");
        const keel::f64 nv = args[1].toScalar();
        if (!std::isfinite(nv) || nv != std::floor(nv) || nv < 1.0)
            throw Error("diff: Difference order N must be a positive integer scalar",
                         0, 0, "diff", "", "numkit:diff:badOrder");
        n = keel::as<keel::i32>(nv);
    }
    if (args.size() >= 3 && !args[2].isEmpty())
        dim = argcheck::dimArg(args[2], "diff");
    outs[0] = numkit::narrowComplex(diff(args[0], n, dim, ctx.engine->allocator()),
                                    ctx.engine->allocator());
}

#define NUMKIT_LOGICAL_RED_REG(name, fn)                                           \
    void name##_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/,               \
                    keel::MutArrayRef<Value> outs, CallContext &ctx)                       \
    {                                                                          \
        argcheck::checkNargin(args.size(), 1, 2, #name); \
        keel::i32 dim = 0;                                                           \
        if (args.size() >= 2 && !args[1].isEmpty())                            \
            dim = argcheck::dimArg(args[1], #name);                        \
        outs[0] = fn(args[0], dim, ctx.engine->allocator());                   \
    }

NUMKIT_LOGICAL_RED_REG(any, numkit::builtin::anyOf)
NUMKIT_LOGICAL_RED_REG(all, numkit::builtin::allOf)

#undef NUMKIT_LOGICAL_RED_REG

void xor_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "xor");
    argcheck::checkNargin(args.size(), 2, 2, "xor");
    outs[0] = xorOf(args[0], args[1], ctx.engine->allocator());
}

// NOTE: cross_reg / dot_reg migrated to toolboxes/linalg/src/vector_ops.cpp.

} // namespace detail

} // namespace numkit::builtin
