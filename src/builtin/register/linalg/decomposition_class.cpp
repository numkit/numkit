// src/builtin/register/linalg/decomposition_class.cpp
//
// Native C++ BuiltinClass for `decomposition` — cached matrix factorizations
// (LU, Cholesky, LDL, QR, COD, triangular, permutedTriangular, banded,
// hessenberg, diagonal) with O(1) copy-on-write value semantics for scaling,
// conjugate transpose, and CheckCondition mutations.

#include <keel/algo.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <keel/complex.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/linalg.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/utf8.h>

#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/object.hpp>
#include <numkit/value/value.hpp>

namespace numkit::linalg::detail {

namespace {

inline constexpr const char *kDecompClass = "decomposition";
inline constexpr keel::f64 kDoubleEps = 2.2204460492503131e-16;
inline constexpr keel::f64 kSingleEps = 1.1920928955078125e-7;

enum class DecompKind : keel::u8 {
    LU = 0,
    Chol,
    LDL,
    QR,
    COD,
    Triangular,
    PermutedTriangular,
    Banded,
    Hessenberg,
    Diagonal,
};

const char *kindName(DecompKind k) noexcept
{
    switch (k) {
    case DecompKind::LU:                 return "lu";
    case DecompKind::Chol:               return "chol";
    case DecompKind::LDL:                return "ldl";
    case DecompKind::QR:                 return "qr";
    case DecompKind::COD:                return "cod";
    case DecompKind::Triangular:         return "triangular";
    case DecompKind::PermutedTriangular: return "permutedTriangular";
    case DecompKind::Banded:             return "banded";
    case DecompKind::Hessenberg:         return "hessenberg";
    case DecompKind::Diagonal:           return "diagonal";
    }
    return "lu";
}

bool asciiEqualNoCase(keel::StringRef a, keel::StringRef b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (keel::usize i = 0; i < a.size(); ++i) {
        if (keel::to_ascii_lower(a[i]) != keel::to_ascii_lower(b[i]))
            return false;
    }
    return true;
}

template <typename T>
inline constexpr bool kIsComplex = false;

template <>
inline constexpr bool kIsComplex<keel::c64> = true;

inline keel::Allocator *ctxAllocator(CallContext &ctx) noexcept
{
    return ctx.engine ? ctx.engine->allocator() : keel::default_allocator();
}

template <typename T>
inline keel::f64 elemAbs(T v) noexcept
{
    return keel::abs(v);
}

template <typename T>
inline T elemConj(T v) noexcept
{
    if constexpr (kIsComplex<T>)
        return keel::conj(v);
    else
        return v;
}

template <typename T>
inline bool elemIsZero(T v) noexcept
{
    if constexpr (kIsComplex<T>)
        return v.real() == 0.0 && v.imag() == 0.0;
    else
        return v == 0.0;
}

template <typename T>
inline bool elemIsNaN(T v) noexcept
{
    if constexpr (kIsComplex<T>)
        return keel::is_nan(v.real()) || keel::is_nan(v.imag());
    else
        return keel::is_nan(v);
}

template <typename T>
inline bool elemIsFinite(T v) noexcept
{
    if constexpr (kIsComplex<T>)
        return keel::is_finite(v.real()) && keel::is_finite(v.imag());
    else
        return keel::is_finite(v);
}

// Immutable cached factorization shared across value copies (O(1) clone).
struct DecompositionFactors {
    DecompKind kind = DecompKind::Triangular;
    keel::usize rows = 0;
    keel::usize cols = 0;
    bool isComplex = false;
    bool isSingle = false;
    bool isUpper = true;
    bool ldlUsesLUFallback = false;
    bool hasRcond = false;
    bool hasRank = false;
    bool isSingular = false;

    keel::Array<keel::f64> data1R;
    keel::Array<keel::f64> data2R;
    keel::Array<keel::c64> data1C;
    keel::Array<keel::c64> data2C;
    keel::Array<keel::f64> vecR;
    keel::Array<keel::c64> vecC;
    keel::Array<keel::usize> perm1;
    keel::Array<keel::usize> perm2;

    keel::f64 rcondVal = keel::Limits<keel::f64>::infinity();
    keel::f64 rankTol = 0.0;
    keel::usize numRank = 0;
};

struct DecompositionPayload final : NativePayload {
    keel::Arc<const DecompositionFactors> factors;
    bool isConjTrans = false;
    keel::c64 scaleFactor{1.0, 0.0};
    bool scaleIsComplex = false;
    bool checkCondition = true;

    keel::Arc<NativePayload> clone(keel::Allocator *alloc = nullptr) const override
    {
        auto *a = alloc ? alloc : keel::default_allocator();
        auto cp = keel::make_arc_in<DecompositionPayload>(a);
        cp->factors = factors;
        cp->isConjTrans = isConjTrans;
        cp->scaleFactor = scaleFactor;
        cp->scaleIsComplex = scaleIsComplex;
        cp->checkCondition = checkCondition;
        return cp;
    }
};

const DecompositionPayload &requireDecompConst(const Value &v, const char *caller)
{
    if (v.isObject() && v.objectClassNameRef() == kDecompClass) {
        if (const auto *st = v.objectStateConst(); st && st->native) {
            if (const auto *p = dynamic_cast<const DecompositionPayload *>(st->native.get()))
                return *p;
        }
    }
    auto msg = keel::format("{}: invalid decomposition object", caller);
    throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidObject");
}

DecompositionPayload &requireDecompMut(Value &v, const char *caller)
{
    if (v.isObject() && v.objectClassNameRef() == kDecompClass) {
        if (auto *st = v.objectStateMut(); st && st->native) {
            if (auto *p = dynamic_cast<DecompositionPayload *>(st->native.get()))
                return *p;
        }
    }
    auto msg = keel::format("{}: invalid decomposition object", caller);
    throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidObject");
}

bool isDecompObject(const Value &v) noexcept
{
    return v.isObject() && v.objectClassNameRef() == kDecompClass;
}

Value makeDecompValue(keel::Arc<DecompositionPayload> payload, keel::Allocator *mr)
{
    auto *a = mr ? mr : keel::default_allocator();
    auto st = keel::make_arc_in<ObjectState>(a, a);
    st->native = keel::move(payload);
    return Value::object(kDecompClass, keel::move(st), /*isHandle=*/false, a);
}

Value cloneWithChanges(const DecompositionPayload &src,
                       bool isConjTrans,
                       keel::c64 scaleFactor,
                       bool scaleIsComplex,
                       bool checkCondition,
                       keel::Allocator *mr)
{
    auto *a = mr ? mr : keel::default_allocator();
    auto p = keel::make_arc_in<DecompositionPayload>(a);
    p->factors = src.factors;
    p->isConjTrans = isConjTrans;
    p->scaleFactor = scaleFactor;
    p->scaleIsComplex = scaleIsComplex;
    p->checkCondition = checkCondition;
    return makeDecompValue(keel::move(p), a);
}

bool isIllConditionedFactors(const DecompositionFactors &f) noexcept
{
    if (f.hasRank) {
        const keel::usize k = keel::min(f.rows, f.cols);
        return k > 0 && f.numRank < k;
    }
    if (f.rows == 0 || f.cols == 0)
        return false;
    if (f.isSingular)
        return true;
    const keel::f64 eps = f.isSingle ? kSingleEps : kDoubleEps;
    return keel::is_nan(f.rcondVal) || f.rcondVal < eps;
}

// ---------------------------------------------------------------------------
// Structure predicates on column-major buffers
// ---------------------------------------------------------------------------

template <typename T>
bool isUpperTriBuf(const T *A, keel::usize n) noexcept
{
    for (keel::usize j = 0; j < n; ++j) {
        for (keel::usize i = j + 1; i < n; ++i) {
            if (!elemIsZero(A[i + j * n]))
                return false;
        }
    }
    return true;
}

template <typename T>
bool isLowerTriBuf(const T *A, keel::usize n) noexcept
{
    for (keel::usize j = 1; j < n; ++j) {
        for (keel::usize i = 0; i < j; ++i) {
            if (!elemIsZero(A[i + j * n]))
                return false;
        }
    }
    return true;
}

template <typename T>
bool isDiagonalBuf(const T *A, keel::usize n) noexcept
{
    for (keel::usize j = 0; j < n; ++j) {
        for (keel::usize i = 0; i < n; ++i) {
            if (i != j && !elemIsZero(A[i + j * n]))
                return false;
        }
    }
    return true;
}

template <typename T>
bool isUpperHessenbergBuf(const T *A, keel::usize n) noexcept
{
    for (keel::usize j = 0; j < n; ++j) {
        for (keel::usize i = j + 2; i < n; ++i) {
            if (!elemIsZero(A[i + j * n]))
                return false;
        }
    }
    return true;
}

template <typename T>
bool isHermitianBuf(const T *A, keel::usize n) noexcept
{
    for (keel::usize j = 0; j < n; ++j) {
        if constexpr (kIsComplex<T>) {
            if (A[j + j * n].imag() != 0.0)
                return false;
        }
        for (keel::usize i = j + 1; i < n; ++i) {
            const T aij = A[i + j * n];
            const T aji = elemConj(A[j + i * n]);
            if (aij != aji)
                return false;
        }
    }
    return true;
}

// Find row/col permutations P, Q such that T = P * A * Q is upper triangular,
// where T(r, c) = A(rowPerm[r], colPerm[c]).
template <typename T>
bool factorPermutedTriangular(const T *A, keel::usize n,
                              keel::Array<keel::usize> &rowPerm,
                              keel::Array<keel::usize> &colPerm,
                              keel::Array<T> &Tmat,
                              keel::Allocator *scratchAlloc)
{
    rowPerm.resize(n);
    colPerm.resize(n);
    Tmat.resize(n * n);
    if (n == 0)
        return true;

    keel::Array<keel::usize> rowDeg(n, scratchAlloc);
    keel::Array<keel::usize> colDeg(n, scratchAlloc);
    keel::Array<keel::u8> rowUsed(n, scratchAlloc);
    keel::Array<keel::u8> colUsed(n, scratchAlloc);
    keel::fill(rowDeg.begin(), rowDeg.end(), keel::usize(0));
    keel::fill(colDeg.begin(), colDeg.end(), keel::usize(0));
    keel::fill(rowUsed.begin(), rowUsed.end(), keel::u8(0));
    keel::fill(colUsed.begin(), colUsed.end(), keel::u8(0));

    for (keel::usize j = 0; j < n; ++j) {
        for (keel::usize i = 0; i < n; ++i) {
            if (!elemIsZero(A[i + j * n])) {
                ++rowDeg[i];
                ++colDeg[j];
            }
        }
    }

    for (keel::usize step = 0; step < n; ++step) {
        keel::usize chosenCol = n;
        for (keel::usize j = 0; j < n; ++j) {
            if (!colUsed[j] && colDeg[j] <= 1) {
                chosenCol = j;
                if (colDeg[j] == 1)
                    break;
            }
        }
        if (chosenCol == n)
            return false;

        keel::usize chosenRow = n;
        if (colDeg[chosenCol] == 1) {
            for (keel::usize i = 0; i < n; ++i) {
                if (!rowUsed[i] && !elemIsZero(A[i + chosenCol * n])) {
                    chosenRow = i;
                    break;
                }
            }
        } else {
            // Column is all-zero among remaining rows: pick the densest remaining row
            // so all of its nonzeros land strictly above the diagonal.
            keel::usize bestDeg = 0;
            bool haveAny = false;
            for (keel::usize i = 0; i < n; ++i) {
                if (!rowUsed[i] && (!haveAny || rowDeg[i] > bestDeg)) {
                    chosenRow = i;
                    bestDeg = rowDeg[i];
                    haveAny = true;
                }
            }
        }
        if (chosenRow == n)
            return false;

        rowPerm[step] = chosenRow;
        colPerm[step] = chosenCol;
        rowUsed[chosenRow] = 1;
        colUsed[chosenCol] = 1;

        for (keel::usize j = 0; j < n; ++j) {
            if (!colUsed[j] && !elemIsZero(A[chosenRow + j * n])) {
                if (colDeg[j] > 0)
                    --colDeg[j];
            }
        }
        for (keel::usize i = 0; i < n; ++i) {
            if (!rowUsed[i] && !elemIsZero(A[i + chosenCol * n])) {
                if (rowDeg[i] > 0)
                    --rowDeg[i];
            }
        }
    }

    // Assemble T = P * A * Q and explicitly verify upper-triangularity.
    for (keel::usize c = 0; c < n; ++c) {
        const keel::usize origC = colPerm[c];
        for (keel::usize r = 0; r < n; ++r) {
            const T val = A[rowPerm[r] + origC * n];
            if (r > c && !elemIsZero(val))
                return false;
            Tmat[r + c * n] = val;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Core raw-buffer solve kernels (op(A) * X = B, column-major)
// ---------------------------------------------------------------------------

template <typename T>
const keel::Array<T> &factorData1(const DecompositionFactors &f) noexcept
{
    if constexpr (kIsComplex<T>)
        return f.data1C;
    else
        return f.data1R;
}

template <typename T>
const keel::Array<T> &factorData2(const DecompositionFactors &f) noexcept
{
    if constexpr (kIsComplex<T>)
        return f.data2C;
    else
        return f.data2R;
}

template <typename T>
const keel::Array<T> &factorDiag(const DecompositionFactors &f) noexcept
{
    if constexpr (kIsComplex<T>)
        return f.vecC;
    else
        return f.vecR;
}

template <typename T>
void solveLUCore(const T *lu, const keel::usize *perm, keel::usize n,
                 bool conjTrans, const T *B, keel::usize nrhs, T *X,
                 keel::Allocator *scratchAlloc)
{
    using namespace keel::linalg;
    if (!conjTrans) {
        // P * A = L * U  =>  L * U * X = P * B
        for (keel::usize col = 0; col < nrhs; ++col) {
            const T *bcol = B + col * n;
            T *xcol = X + col * n;
            for (keel::usize i = 0; i < n; ++i)
                xcol[i] = bcol[perm[i]];
        }
        trsm(MatrixSide::Left, MatrixUplo::Lower, MatrixTranspose::NoTrans,
             MatrixDiag::Unit, n, nrhs, T(1), lu, n, X, n);
        trsm(MatrixSide::Left, MatrixUplo::Upper, MatrixTranspose::NoTrans,
             MatrixDiag::NonUnit, n, nrhs, T(1), lu, n, X, n);
    } else {
        // A' = U' * L' * P  =>  U' * L' * (P * X) = B
        keel::Array<T> Y(n * nrhs, scratchAlloc);
        keel::copy(B, B + n * nrhs, Y.begin());
        trsm(MatrixSide::Left, MatrixUplo::Upper, MatrixTranspose::ConjTrans,
             MatrixDiag::NonUnit, n, nrhs, T(1), lu, n, Y.data(), n);
        trsm(MatrixSide::Left, MatrixUplo::Lower, MatrixTranspose::ConjTrans,
             MatrixDiag::Unit, n, nrhs, T(1), lu, n, Y.data(), n);
        for (keel::usize col = 0; col < nrhs; ++col) {
            const T *ycol = Y.data() + col * n;
            T *xcol = X + col * n;
            for (keel::usize i = 0; i < n; ++i)
                xcol[perm[i]] = ycol[i];
        }
    }
}

template <typename T>
void solveFactorsTyped(const DecompositionFactors &f, bool conjTrans,
                       const T *B, keel::usize nrhs, T *X,
                       keel::Allocator *scratchAlloc)
{
    using namespace keel::linalg;
    const keel::usize m = f.rows;
    const keel::usize n = f.cols;

    switch (f.kind) {
    case DecompKind::LU:
    case DecompKind::Banded:
    case DecompKind::Hessenberg: {
        const auto &lu = factorData1<T>(f);
        solveLUCore<T>(lu.data(), f.perm1.data(), n, conjTrans, B, nrhs, X, scratchAlloc);
        return;
    }
    case DecompKind::Chol: {
        // A = R' * R is Hermitian => op(A) = A for both NoTrans and ConjTrans.
        const auto &R = factorData1<T>(f);
        keel::copy(B, B + n * nrhs, X);
        trsm(MatrixSide::Left, MatrixUplo::Upper, MatrixTranspose::ConjTrans,
             MatrixDiag::NonUnit, n, nrhs, T(1), R.data(), n, X, n);
        trsm(MatrixSide::Left, MatrixUplo::Upper, MatrixTranspose::NoTrans,
             MatrixDiag::NonUnit, n, nrhs, T(1), R.data(), n, X, n);
        return;
    }
    case DecompKind::LDL: {
        if (f.ldlUsesLUFallback) {
            const auto &lu = factorData1<T>(f);
            solveLUCore<T>(lu.data(), f.perm1.data(), n, conjTrans, B, nrhs, X, scratchAlloc);
            return;
        }
        // A = L * diag(D) * L' is Hermitian => op(A) = A.
        const auto &L = factorData1<T>(f);
        keel::copy(B, B + n * nrhs, X);
        trsm(MatrixSide::Left, MatrixUplo::Lower, MatrixTranspose::NoTrans,
             MatrixDiag::Unit, n, nrhs, T(1), L.data(), n, X, n);
        for (keel::usize col = 0; col < nrhs; ++col) {
            T *xcol = X + col * n;
            for (keel::usize i = 0; i < n; ++i)
                xcol[i] = xcol[i] / T(f.vecR[i]);
        }
        trsm(MatrixSide::Left, MatrixUplo::Lower, MatrixTranspose::ConjTrans,
             MatrixDiag::Unit, n, nrhs, T(1), L.data(), n, X, n);
        return;
    }
    case DecompKind::Triangular: {
        const auto &tri = factorData1<T>(f);
        keel::copy(B, B + n * nrhs, X);
        const MatrixUplo uplo = f.isUpper ? MatrixUplo::Upper : MatrixUplo::Lower;
        const MatrixTranspose tr = conjTrans ? MatrixTranspose::ConjTrans : MatrixTranspose::NoTrans;
        trsm(MatrixSide::Left, uplo, tr, MatrixDiag::NonUnit, n, nrhs, T(1), tri.data(), n, X, n);
        return;
    }
    case DecompKind::PermutedTriangular: {
        const auto &tri = factorData1<T>(f);
        keel::Array<T> Y(n * nrhs, scratchAlloc);
        if (!conjTrans) {
            // P * A * Q = T  =>  T * (Q' * X) = P * B
            for (keel::usize col = 0; col < nrhs; ++col) {
                const T *bcol = B + col * n;
                T *ycol = Y.data() + col * n;
                for (keel::usize i = 0; i < n; ++i)
                    ycol[i] = bcol[f.perm1[i]];
            }
            trsm(MatrixSide::Left, MatrixUplo::Upper, MatrixTranspose::NoTrans,
                 MatrixDiag::NonUnit, n, nrhs, T(1), tri.data(), n, Y.data(), n);
            for (keel::usize col = 0; col < nrhs; ++col) {
                const T *ycol = Y.data() + col * n;
                T *xcol = X + col * n;
                for (keel::usize i = 0; i < n; ++i)
                    xcol[f.perm2[i]] = ycol[i];
            }
        } else {
            // A' = Q * T' * P'  =>  T' * (P' * X) = Q' * B
            for (keel::usize col = 0; col < nrhs; ++col) {
                const T *bcol = B + col * n;
                T *ycol = Y.data() + col * n;
                for (keel::usize i = 0; i < n; ++i)
                    ycol[i] = bcol[f.perm2[i]];
            }
            trsm(MatrixSide::Left, MatrixUplo::Upper, MatrixTranspose::ConjTrans,
                 MatrixDiag::NonUnit, n, nrhs, T(1), tri.data(), n, Y.data(), n);
            for (keel::usize col = 0; col < nrhs; ++col) {
                const T *ycol = Y.data() + col * n;
                T *xcol = X + col * n;
                for (keel::usize i = 0; i < n; ++i)
                    xcol[f.perm1[i]] = ycol[i];
            }
        }
        return;
    }
    case DecompKind::Diagonal: {
        const auto &diag = factorDiag<T>(f);
        for (keel::usize col = 0; col < nrhs; ++col) {
            const T *bcol = B + col * n;
            T *xcol = X + col * n;
            for (keel::usize i = 0; i < n; ++i) {
                const T di = conjTrans ? elemConj(diag[i]) : diag[i];
                xcol[i] = bcol[i] / di;
            }
        }
        return;
    }
    case DecompKind::QR: {
        // Only called when !conjTrans (caller guards QRmldivideTransp).
        // A * P = Q * R  =>  R(0:r-1,0:r-1) * Y(0:r-1,:) = (Q' * B)(0:r-1,:)
        const auto &Q = factorData1<T>(f);
        const auto &R = factorData2<T>(f);
        const keel::usize r = f.numRank;
        keel::fill(X, X + n * nrhs, T(0));
        if (r == 0 || m == 0)
            return;
        keel::Array<T> C(m * nrhs, scratchAlloc);
        gemm(MatrixTranspose::ConjTrans, MatrixTranspose::NoTrans,
             m, nrhs, m, T(1), Q.data(), m, B, m, T(0), C.data(), m);
        trsm(MatrixSide::Left, MatrixUplo::Upper, MatrixTranspose::NoTrans,
             MatrixDiag::NonUnit, r, nrhs, T(1), R.data(), m, C.data(), m);
        for (keel::usize col = 0; col < nrhs; ++col) {
            const T *ccol = C.data() + col * m;
            T *xcol = X + col * n;
            for (keel::usize i = 0; i < r; ++i)
                xcol[f.perm1[i]] = ccol[i];
        }
        return;
    }
    case DecompKind::COD: {
        // A = U * diag(S) * V'
        const auto &U = factorData1<T>(f);
        const auto &V = factorData2<T>(f);
        const keel::usize r = f.numRank;
        const keel::usize outRows = conjTrans ? m : n;
        keel::fill(X, X + outRows * nrhs, T(0));
        if (r == 0)
            return;
        keel::Array<T> C(r * nrhs, scratchAlloc);
        if (!conjTrans) {
            // X = V(:,0:r-1) * diag(1/S) * U(:,0:r-1)' * B
            gemm(MatrixTranspose::ConjTrans, MatrixTranspose::NoTrans,
                 r, nrhs, m, T(1), U.data(), m, B, m, T(0), C.data(), r);
            for (keel::usize col = 0; col < nrhs; ++col) {
                T *ccol = C.data() + col * r;
                for (keel::usize i = 0; i < r; ++i)
                    ccol[i] = ccol[i] / T(f.vecR[i]);
            }
            gemm(MatrixTranspose::NoTrans, MatrixTranspose::NoTrans,
                 n, nrhs, r, T(1), V.data(), n, C.data(), r, T(0), X, n);
        } else {
            // A' = V * diag(S) * U'  =>  X = U(:,0:r-1) * diag(1/S) * V(:,0:r-1)' * B
            gemm(MatrixTranspose::ConjTrans, MatrixTranspose::NoTrans,
                 r, nrhs, n, T(1), V.data(), n, B, n, T(0), C.data(), r);
            for (keel::usize col = 0; col < nrhs; ++col) {
                T *ccol = C.data() + col * r;
                for (keel::usize i = 0; i < r; ++i)
                    ccol[i] = ccol[i] / T(f.vecR[i]);
            }
            gemm(MatrixTranspose::NoTrans, MatrixTranspose::NoTrans,
                 m, nrhs, r, T(1), U.data(), m, C.data(), r, T(0), X, m);
        }
        return;
    }
    }
}

// ---------------------------------------------------------------------------
// 1-norm & rcond estimation using already-cached square factors
// ---------------------------------------------------------------------------

template <typename T>
keel::f64 oneNormMat(const T *A, keel::usize m, keel::usize n, bool &hasNaNOut) noexcept
{
    hasNaNOut = false;
    keel::f64 maxCol = 0.0;
    for (keel::usize j = 0; j < n; ++j) {
        keel::f64 s = 0.0;
        for (keel::usize i = 0; i < m; ++i) {
            const T v = A[i + j * m];
            if (elemIsNaN(v)) {
                hasNaNOut = true;
                return keel::Limits<keel::f64>::quiet_nan();
            }
            s += elemAbs(v);
        }
        if (s > maxCol)
            maxCol = s;
    }
    return maxCol;
}

template <typename T>
keel::f64 estimateRcondSquare(const DecompositionFactors &f, keel::f64 anorm, bool hasNaN,
                              keel::Allocator *scratchAlloc)
{
    const keel::usize n = f.rows;
    if (n == 0)
        return keel::Limits<keel::f64>::infinity();
    if (hasNaN)
        return keel::Limits<keel::f64>::quiet_nan();
    if (f.isSingular || anorm == 0.0 || !keel::is_finite(anorm))
        return 0.0;

    if (f.kind == DecompKind::Diagonal) {
        const auto &diag = factorDiag<T>(f);
        keel::f64 mn = keel::Limits<keel::f64>::infinity();
        keel::f64 mx = 0.0;
        for (keel::usize i = 0; i < n; ++i) {
            const keel::f64 a = elemAbs(diag[i]);
            if (keel::is_nan(a))
                return keel::Limits<keel::f64>::quiet_nan();
            if (a < mn) mn = a;
            if (a > mx) mx = a;
        }
        if (mx == 0.0 || mn == 0.0 || !keel::is_finite(mx))
            return 0.0;
        return mn / mx;
    }

    // Exact ||A^{-1}||_1 for small matrices (n <= 64), Hager-Higham 1-norm estimator for larger.
    if (n <= 64) {
        keel::Array<T> Imat(n * n, scratchAlloc);
        keel::Array<T> Inv(n * n, scratchAlloc);
        keel::fill(Imat.begin(), Imat.end(), T(0));
        for (keel::usize i = 0; i < n; ++i)
            Imat[i + i * n] = T(1);
        solveFactorsTyped<T>(f, /*conjTrans=*/false, Imat.data(), n, Inv.data(), scratchAlloc);
        bool invNaN = false;
        const keel::f64 invNorm = oneNormMat<T>(Inv.data(), n, n, invNaN);
        if (invNaN || !keel::is_finite(invNorm) || invNorm == 0.0)
            return 0.0;
        return 1.0 / (anorm * invNorm);
    }

    // Higham (1988) / LAPACK dlacon-style 1-norm estimator for ||A^{-1}||_1 (O(n^2)).
    keel::Array<T> x(n, scratchAlloc);
    keel::Array<T> v(n, scratchAlloc);
    keel::Array<T> xi(n, scratchAlloc);
    keel::Array<T> z(n, scratchAlloc);
    const keel::f64 invN = 1.0 / keel::as<keel::f64>(n);
    for (keel::usize i = 0; i < n; ++i)
        x[i] = T(invN);

    solveFactorsTyped<T>(f, /*conjTrans=*/false, x.data(), 1, v.data(), scratchAlloc);
    keel::f64 est = 0.0;
    for (keel::usize i = 0; i < n; ++i) {
        if (!elemIsFinite(v[i]))
            return 0.0;
        est += elemAbs(v[i]);
    }
    if (est == 0.0)
        return 0.0;

    keel::usize lastJ = n;
    for (keel::usize iter = 0; iter < 5; ++iter) {
        for (keel::usize i = 0; i < n; ++i) {
            const keel::f64 av = elemAbs(v[i]);
            xi[i] = (av > 0.0) ? (v[i] / T(av)) : T(1);
        }
        solveFactorsTyped<T>(f, /*conjTrans=*/true, xi.data(), 1, z.data(), scratchAlloc);
        keel::f64 maxZ = -1.0;
        keel::usize maxJ = 0;
        keel::f64 zDotX = 0.0;
        for (keel::usize i = 0; i < n; ++i) {
            if (!elemIsFinite(z[i]))
                return 0.0;
            const keel::f64 az = elemAbs(z[i]);
            if (az > maxZ) {
                maxZ = az;
                maxJ = i;
            }
            if constexpr (kIsComplex<T>)
                zDotX += z[i].real() * x[i].real() + z[i].imag() * x[i].imag();
            else
                zDotX += z[i] * x[i];
        }
        if (maxZ <= zDotX || maxJ == lastJ)
            break;
        lastJ = maxJ;
        keel::fill(x.begin(), x.end(), T(0));
        x[maxJ] = T(1);
        solveFactorsTyped<T>(f, /*conjTrans=*/false, x.data(), 1, v.data(), scratchAlloc);
        keel::f64 newEst = 0.0;
        for (keel::usize i = 0; i < n; ++i) {
            if (!elemIsFinite(v[i]))
                return 0.0;
            newEst += elemAbs(v[i]);
        }
        if (newEst <= est)
            break;
        est = newEst;
    }

    // Final alternating probe (catches pathological upper-triangular matrices).
    const keel::f64 denom = (n > 1) ? keel::as<keel::f64>(n - 1) : 1.0;
    for (keel::usize i = 0; i < n; ++i) {
        const keel::f64 sign = (i % 2 == 0) ? 1.0 : -1.0;
        x[i] = T(sign * (1.0 + keel::as<keel::f64>(i) / denom));
    }
    solveFactorsTyped<T>(f, /*conjTrans=*/false, x.data(), 1, v.data(), scratchAlloc);
    keel::f64 altSum = 0.0;
    for (keel::usize i = 0; i < n; ++i) {
        if (!elemIsFinite(v[i]))
            return 0.0;
        altSum += elemAbs(v[i]);
    }
    const keel::f64 altEst = 2.0 * altSum / (3.0 * keel::as<keel::f64>(n));
    if (altEst > est)
        est = altEst;

    if (!keel::is_finite(est) || est == 0.0)
        return 0.0;
    return 1.0 / (anorm * est);
}

// ---------------------------------------------------------------------------
// Factorization builder (templated on f64 / c64)
// ---------------------------------------------------------------------------

enum class TriFlag : keel::u8 { None = 0, Upper, Lower };

template <typename T>
keel::Arc<DecompositionFactors> buildFactorsTyped(const T *A, keel::usize m, keel::usize n,
                                                  bool isSingle, DecompKind kind,
                                                  TriFlag triFlag, bool hasUserRankTol,
                                                  keel::f64 userRankTol, keel::Allocator *mr)
{
    auto *a = mr ? mr : keel::default_allocator();
    auto f = keel::make_arc_in<DecompositionFactors>(a);
    f->kind = kind;
    f->rows = m;
    f->cols = n;
    f->isComplex = kIsComplex<T>;
    f->isSingle = isSingle;
    f->isUpper = (triFlag != TriFlag::Lower);

    keel::ArenaTemp scratch = keel::get_scratch(a);
    const keel::f64 eps = isSingle ? kSingleEps : kDoubleEps;

    auto &d1 = [&]() -> keel::Array<T> & {
        if constexpr (kIsComplex<T>) return f->data1C;
        else return f->data1R;
    }();
    auto &d2 = [&]() -> keel::Array<T> & {
        if constexpr (kIsComplex<T>) return f->data2C;
        else return f->data2R;
    }();

    // Helper: compute LU factorization of square matrix src (n x n).
    auto factorLU = [&](const T *src) {
        d1.resize(n * n);
        f->perm1.resize(n);
        if (n == 0)
            return;
        keel::copy(src, src + n * n, d1.begin());
        keel::Array<keel::i32> piv(n, scratch.allocator());
        bool nonsing = keel::linalg::lu_factor(d1.data(), n, piv.data(), n, n);
        if (!nonsing) {
            keel::copy(src, src + n * n, d1.begin());
            nonsing = keel::linalg::lu_factor_general(d1.data(), n, n, piv.data());
        }
        for (keel::usize i = 0; i < n; ++i)
            f->perm1[i] = i;
        for (keel::usize k = 0; k < n; ++k)
            keel::swap(f->perm1[k], f->perm1[keel::as<keel::usize>(piv[k])]);
        f->isSingular = !nonsing;
        for (keel::usize i = 0; i < n; ++i) {
            if (elemIsZero(d1[i + i * n]))
                f->isSingular = true;
        }
    };

    switch (kind) {
    case DecompKind::LU:
    case DecompKind::Banded:
    case DecompKind::Hessenberg: {
        if (m != n) {
            throw Error("decomposition: input matrix must be square for this decomposition type",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidASquare");
        }
        if (kind == DecompKind::Hessenberg && !isUpperHessenbergBuf(A, n)) {
            throw Error("decomposition: Hessenberg decomposition requires a matrix that is upper triangular with one lower subdiagonal",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidAForHessenberg");
        }
        factorLU(A);
        bool hasNaN = false;
        const keel::f64 anorm = oneNormMat<T>(A, n, n, hasNaN);
        f->hasRcond = true;
        f->rcondVal = estimateRcondSquare<T>(*f, anorm, hasNaN, scratch.allocator());
        return f;
    }

    case DecompKind::Chol: {
        if (m != n) {
            throw Error("decomposition: Cholesky decomposition requires a square matrix",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidAForCholSquare");
        }
        d1.resize(n * n);
        if (n == 0) {
            f->hasRcond = true;
            f->rcondVal = keel::Limits<keel::f64>::infinity();
            return f;
        }
        // Build Hermitianized matrix from chosen triangle (default: upper).
        const bool useLower = (triFlag == TriFlag::Lower);
        keel::Array<T> H(n * n, scratch.allocator());
        for (keel::usize j = 0; j < n; ++j) {
            const T djj = A[j + j * n];
            if constexpr (kIsComplex<T>) {
                if (djj.imag() != 0.0 || djj.real() <= 0.0 || !elemIsFinite(djj)) {
                    throw Error("decomposition: Cholesky decomposition requires a Hermitian positive-definite matrix",
                                0, 0, "decomposition", "", "numkit:decomposition:InvalidAForCholSPD");
                }
            } else {
                if (djj <= 0.0 || !elemIsFinite(djj)) {
                    throw Error("decomposition: Cholesky decomposition requires a symmetric positive-definite matrix",
                                0, 0, "decomposition", "", "numkit:decomposition:InvalidAForCholSPD");
                }
            }
            H[j + j * n] = djj;
            for (keel::usize i = j + 1; i < n; ++i) {
                const T val = useLower ? A[i + j * n] : elemConj(A[j + i * n]);
                H[i + j * n] = val;
                H[j + i * n] = elemConj(val);
            }
        }
        if (keel::linalg::cholesky_upper(H.data(), d1.data(), n) != 0) {
            throw Error("decomposition: Cholesky decomposition requires a symmetric positive-definite matrix",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidAForCholSPD");
        }
        bool hasNaN = false;
        const keel::f64 anorm = oneNormMat<T>(H.data(), n, n, hasNaN);
        f->hasRcond = true;
        f->rcondVal = estimateRcondSquare<T>(*f, anorm, hasNaN, scratch.allocator());
        return f;
    }

    case DecompKind::LDL: {
        if (m != n) {
            throw Error("decomposition: LDL decomposition requires a square matrix",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidAForLDLSquare");
        }
        if (n == 0) {
            f->hasRcond = true;
            f->rcondVal = keel::Limits<keel::f64>::infinity();
            return f;
        }
        keel::Array<T> H(n * n, scratch.allocator());
        if (triFlag == TriFlag::None) {
            if (!isHermitianBuf(A, n)) {
                throw Error("decomposition: LDL decomposition requires a symmetric/Hermitian matrix",
                            0, 0, "decomposition", "", "numkit:decomposition:InvalidAForLDL");
            }
            keel::copy(A, A + n * n, H.begin());
        } else {
            const bool useUpper = (triFlag == TriFlag::Upper);
            for (keel::usize j = 0; j < n; ++j) {
                if constexpr (kIsComplex<T>)
                    H[j + j * n] = keel::c64(A[j + j * n].real(), 0.0);
                else
                    H[j + j * n] = A[j + j * n];
                for (keel::usize i = j + 1; i < n; ++i) {
                    const T val = useUpper ? elemConj(A[j + i * n]) : A[i + j * n];
                    H[i + j * n] = val;
                    H[j + i * n] = elemConj(val);
                }
            }
        }

        d1.resize(n * n);
        f->vecR.resize(n);
        const keel::usize failIdx = keel::linalg::ldl_factor(H.data(), n, d1.data(), f->vecR.data(), scratch.allocator());
        if (failIdx != 0) {
            // Indefinite matrix requiring 2x2 Bunch-Kaufman pivot — fall back cleanly to LU on H.
            f->ldlUsesLUFallback = true;
            f->vecR.clear();
            factorLU(H.data());
        } else {
            for (keel::usize i = 0; i < n; ++i) {
                if (f->vecR[i] == 0.0)
                    f->isSingular = true;
            }
        }
        bool hasNaN = false;
        const keel::f64 anorm = oneNormMat<T>(H.data(), n, n, hasNaN);
        f->hasRcond = true;
        f->rcondVal = estimateRcondSquare<T>(*f, anorm, hasNaN, scratch.allocator());
        return f;
    }

    case DecompKind::Triangular: {
        if (m != n) {
            throw Error("decomposition: triangular decomposition requires a square matrix",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidAForTriangular");
        }
        if (n == 0) {
            f->hasRcond = true;
            f->rcondVal = keel::Limits<keel::f64>::infinity();
            return f;
        }
        bool upper = true;
        if (triFlag == TriFlag::Upper) {
            upper = true;
        } else if (triFlag == TriFlag::Lower) {
            upper = false;
        } else {
            if (isUpperTriBuf(A, n)) {
                upper = true;
            } else if (isLowerTriBuf(A, n)) {
                upper = false;
            } else {
                throw Error("decomposition: input matrix must be square triangular (or specify 'upper'/'lower')",
                            0, 0, "decomposition", "", "numkit:decomposition:InvalidAForTriangular");
            }
        }
        f->isUpper = upper;
        d1.resize(n * n);
        keel::fill(d1.begin(), d1.end(), T(0));
        for (keel::usize j = 0; j < n; ++j) {
            const keel::usize rStart = upper ? 0 : j;
            const keel::usize rEnd   = upper ? (j + 1) : n;
            for (keel::usize i = rStart; i < rEnd; ++i)
                d1[i + j * n] = A[i + j * n];
            if (elemIsZero(d1[j + j * n]))
                f->isSingular = true;
        }
        bool hasNaN = false;
        const keel::f64 anorm = oneNormMat<T>(d1.data(), n, n, hasNaN);
        f->hasRcond = true;
        f->rcondVal = estimateRcondSquare<T>(*f, anorm, hasNaN, scratch.allocator());
        return f;
    }

    case DecompKind::PermutedTriangular: {
        if (m != n) {
            throw Error("decomposition: permutedTriangular decomposition requires a square matrix",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidAForPermTriang");
        }
        if (!factorPermutedTriangular<T>(A, n, f->perm1, f->perm2, d1, scratch.allocator())) {
            throw Error("decomposition: input matrix must be a permutation of a square triangular matrix",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidAForPermTriang");
        }
        for (keel::usize i = 0; i < n; ++i) {
            if (elemIsZero(d1[i + i * n]))
                f->isSingular = true;
        }
        bool hasNaN = false;
        const keel::f64 anorm = oneNormMat<T>(A, n, n, hasNaN);
        f->hasRcond = true;
        f->rcondVal = estimateRcondSquare<T>(*f, anorm, hasNaN, scratch.allocator());
        return f;
    }

    case DecompKind::Diagonal: {
        if (m != n || !isDiagonalBuf(A, n)) {
            throw Error("decomposition: input matrix must be square diagonal",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidAForDiagonal");
        }
        auto &diag = [&]() -> keel::Array<T> & {
            if constexpr (kIsComplex<T>) return f->vecC;
            else return f->vecR;
        }();
        diag.resize(n);
        for (keel::usize i = 0; i < n; ++i) {
            diag[i] = A[i + i * n];
            if (elemIsZero(diag[i]))
                f->isSingular = true;
        }
        bool hasNaN = false;
        const keel::f64 anorm = oneNormMat<T>(A, n, n, hasNaN);
        f->hasRcond = true;
        f->rcondVal = estimateRcondSquare<T>(*f, anorm, hasNaN, scratch.allocator());
        return f;
    }

    case DecompKind::QR: {
        f->hasRank = true;
        const keel::usize k = keel::min(m, n);
        d1.resize(m * m);
        d2.resize(m * n);
        f->perm1.resize(n);
        if (k == 0) {
            for (keel::usize j = 0; j < n; ++j)
                f->perm1[j] = j;
            f->numRank = 0;
            f->rankTol = hasUserRankTol ? userRankTol : 0.0;
            return f;
        }
        keel::linalg::qr_pivoted(A, m, n, d1.data(), d2.data(), f->perm1.data(), scratch.allocator());
        const keel::f64 r00 = elemAbs(d2[0]);
        const keel::f64 tol = hasUserRankTol
            ? userRankTol
            : (keel::as<keel::f64>(keel::max(m, n)) * eps * r00);
        f->rankTol = tol;
        keel::usize r = 0;
        for (keel::usize i = 0; i < k; ++i) {
            if (elemAbs(d2[i + i * m]) > tol)
                ++r;
        }
        f->numRank = r;
        return f;
    }

    case DecompKind::COD: {
        f->hasRank = true;
        const keel::usize k = keel::min(m, n);
        d1.resize(m * m);
        d2.resize(n * n);
        f->vecR.resize(k);
        if (k == 0) {
            f->numRank = 0;
            f->rankTol = hasUserRankTol ? userRankTol : 0.0;
            return f;
        }
        keel::linalg::svd_full(A, m, n, d1.data(), f->vecR.data(), d2.data(), scratch.allocator());
        const keel::f64 s0 = f->vecR[0];
        const keel::f64 tol = hasUserRankTol
            ? userRankTol
            : (keel::as<keel::f64>(keel::max(m, n)) * eps * s0);
        f->rankTol = tol;
        keel::usize r = 0;
        for (keel::usize i = 0; i < k; ++i) {
            if (f->vecR[i] > tol)
                ++r;
        }
        f->numRank = r;
        return f;
    }
    }
    return f;
}

// ---------------------------------------------------------------------------
// Constructor argument parsing
// ---------------------------------------------------------------------------

bool tryParseDecompType(keel::StringRef s, DecompKind &outKind, bool &isAuto) noexcept
{
    isAuto = false;
    if (asciiEqualNoCase(s, "auto"))               { isAuto = true; return true; }
    if (asciiEqualNoCase(s, "lu"))                 { outKind = DecompKind::LU; return true; }
    if (asciiEqualNoCase(s, "chol"))               { outKind = DecompKind::Chol; return true; }
    if (asciiEqualNoCase(s, "ldl"))                { outKind = DecompKind::LDL; return true; }
    if (asciiEqualNoCase(s, "qr"))                 { outKind = DecompKind::QR; return true; }
    if (asciiEqualNoCase(s, "cod"))                { outKind = DecompKind::COD; return true; }
    if (asciiEqualNoCase(s, "triangular"))         { outKind = DecompKind::Triangular; return true; }
    if (asciiEqualNoCase(s, "permutedTriangular")) { outKind = DecompKind::PermutedTriangular; return true; }
    if (asciiEqualNoCase(s, "banded"))             { outKind = DecompKind::Banded; return true; }
    if (asciiEqualNoCase(s, "hessenberg"))         { outKind = DecompKind::Hessenberg; return true; }
    if (asciiEqualNoCase(s, "diagonal"))           { outKind = DecompKind::Diagonal; return true; }
    return false;
}

bool isKnownOptionName(keel::StringRef s) noexcept
{
    return asciiEqualNoCase(s, "CheckCondition")
        || asciiEqualNoCase(s, "RankTolerance")
        || asciiEqualNoCase(s, "BandDensity")
        || asciiEqualNoCase(s, "LDLPivotTolerance")
        || asciiEqualNoCase(s, "LUPivotTolerance");
}

bool extractLogicalScalar(const Value &v, const char *optName, const char *caller)
{
    if (v.numel() != 1) {
        auto msg = keel::format("{}: '{}' must be a scalar logical value", caller, optName);
        throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidOption");
    }
    if (v.isLogical())
        return v.logicalData()[0] != 0;
    if (v.isNumeric() && !v.isComplex()) {
        const keel::f64 x = v.elemAsDouble(0);
        if (keel::is_nan(x)) {
            auto msg = keel::format("{}: '{}' must not be NaN", caller, optName);
            throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidOption");
        }
        return x != 0.0;
    }
    auto msg = keel::format("{}: '{}' must be a scalar logical value", caller, optName);
    throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidOption");
}

keel::f64 extractRealScalarOption(const Value &v, const char *optName,
                                  keel::f64 lo, keel::f64 hi, const char *caller)
{
    if (v.numel() != 1 || !v.isNumeric() || v.isComplex()) {
        auto msg = keel::format("{}: '{}' must be a real numeric scalar", caller, optName);
        throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidOption");
    }
    const keel::f64 x = v.elemAsDouble(0);
    if (keel::is_nan(x) || x < lo || x > hi) {
        auto msg = keel::format("{}: '{}' out of valid range", caller, optName);
        throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidOption");
    }
    return x;
}

Value constructDecomposition(keel::ArrayRef<Value> args, CallContext &ctx)
{
    keel::Allocator *mr = ctxAllocator(ctx);
    if (args.empty()) {
        auto f = buildFactorsTyped<keel::f64>(nullptr, 0, 0, /*isSingle=*/false,
                                              DecompKind::Triangular, TriFlag::None,
                                              /*hasUserRankTol=*/false, 0.0, mr);
        auto p = keel::make_arc_in<DecompositionPayload>(mr);
        p->factors = keel::move(f);
        return makeDecompValue(keel::move(p), mr);
    }

    const Value &A = args[0];
    const ValueType vt = A.type();
    if ((vt != ValueType::DOUBLE && vt != ValueType::SINGLE && vt != ValueType::COMPLEX)
        || A.dims().ndim() > 2) {
        throw Error("decomposition: first input A must be a 2-D floating-point (double or single) matrix",
                    0, 0, "decomposition", "", "numkit:decomposition:InvalidA");
    }

    const keel::usize m = A.dims().rows();
    const keel::usize n = A.dims().cols();
    const bool isSingle = (vt == ValueType::SINGLE);
    const bool isComplex = (vt == ValueType::COMPLEX);

    bool isAuto = true;
    DecompKind kind = DecompKind::LU;
    TriFlag triFlag = TriFlag::None;
    bool checkCondition = true;
    bool hasUserRankTol = false;
    keel::f64 userRankTol = 0.0;

    keel::usize idx = 1;
    if (idx < args.size()) {
        if (!args[idx].isChar() && !args[idx].isString()) {
            throw Error("decomposition: optional arguments must be type/flag text or Name-Value pairs",
                        0, 0, "decomposition", "", "numkit:decomposition:InvalidOption");
        }
        const keel::String s1 = args[idx].toString();
        if (tryParseDecompType(s1, kind, isAuto)) {
            ++idx;
            if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
                const keel::String s2 = args[idx].toString();
                if (asciiEqualNoCase(s2, "upper") || asciiEqualNoCase(s2, "lower")) {
                    if (isAuto || (kind != DecompKind::Triangular && kind != DecompKind::Chol && kind != DecompKind::LDL)) {
                        auto msg = keel::format(
                            "decomposition: '{}' flag is only supported for 'triangular', 'chol', or 'ldl'",
                            s2);
                        throw Error(msg.c_str(), 0, 0, "decomposition", "",
                                    "numkit:decomposition:unrecognizedStringChoice");
                    }
                    triFlag = asciiEqualNoCase(s2, "upper") ? TriFlag::Upper : TriFlag::Lower;
                    ++idx;
                }
            }
        } else if (!isKnownOptionName(s1)) {
            auto msg = keel::format("decomposition: unrecognized decomposition type or option '{}'", s1);
            throw Error(msg.c_str(), 0, 0, "decomposition", "",
                        "numkit:decomposition:unrecognizedStringChoice");
        }
    }

    if ((args.size() - idx) % 2 != 0) {
        throw Error("decomposition: Name-Value arguments must appear in pairs",
                    0, 0, "decomposition", "", "numkit:decomposition:invalidNameValue");
    }

    for (; idx < args.size(); idx += 2) {
        if (!args[idx].isChar() && !args[idx].isString()) {
            throw Error("decomposition: Name-Value parameter name must be text",
                        0, 0, "decomposition", "", "numkit:decomposition:invalidNameValue");
        }
        const keel::String name = args[idx].toString();
        const Value &val = args[idx + 1];
        if (asciiEqualNoCase(name, "CheckCondition")) {
            checkCondition = extractLogicalScalar(val, "CheckCondition", "decomposition");
        } else if (asciiEqualNoCase(name, "RankTolerance")) {
            userRankTol = extractRealScalarOption(val, "RankTolerance", 0.0,
                                                  keel::Limits<keel::f64>::infinity(), "decomposition");
            hasUserRankTol = true;
        } else if (asciiEqualNoCase(name, "BandDensity")) {
            (void)extractRealScalarOption(val, "BandDensity", 0.0, 1.0, "decomposition");
        } else if (asciiEqualNoCase(name, "LDLPivotTolerance")) {
            (void)extractRealScalarOption(val, "LDLPivotTolerance", 0.0, 0.5, "decomposition");
        } else if (asciiEqualNoCase(name, "LUPivotTolerance")) {
            (void)extractRealScalarOption(val, "LUPivotTolerance", 0.0, 1.0, "decomposition");
        } else {
            auto msg = keel::format("decomposition: unknown parameter '{}'", name);
            throw Error(msg.c_str(), 0, 0, "decomposition", "", "numkit:decomposition:unknownParameter");
        }
    }

    // Resolve 'auto' type based on matrix shape & structure.
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    if (isComplex) {
        const keel::c64 *data = A.complexData();
        if (isAuto) {
            if (m != n) {
                kind = DecompKind::QR;
            } else if (m == 0 || isUpperTriBuf(data, n) || isLowerTriBuf(data, n)) {
                kind = DecompKind::Triangular;
            } else {
                kind = DecompKind::LU;
            }
        }
        auto f = buildFactorsTyped<keel::c64>(data, m, n, isSingle, kind, triFlag,
                                              hasUserRankTol, userRankTol, mr);
        auto p = keel::make_arc_in<DecompositionPayload>(mr);
        p->factors = keel::move(f);
        p->checkCondition = checkCondition;
        return makeDecompValue(keel::move(p), mr);
    }

    const keel::f64 *realPtr = nullptr;
    keel::Array<keel::f64> widened(scratch.allocator());
    if (vt == ValueType::DOUBLE) {
        realPtr = A.doubleData();
    } else {
        // SINGLE -> widen to f64 for computation.
        widened.resize(m * n);
        const keel::f32 *sp = A.singleData();
        for (keel::usize i = 0; i < m * n; ++i)
            widened[i] = keel::as<keel::f64>(sp[i]);
        realPtr = widened.data();
    }

    if (isAuto) {
        if (m != n) {
            kind = DecompKind::QR;
        } else if (m == 0 || isUpperTriBuf(realPtr, n) || isLowerTriBuf(realPtr, n)) {
            kind = DecompKind::Triangular;
        } else {
            kind = DecompKind::LU;
        }
    }

    auto f = buildFactorsTyped<keel::f64>(realPtr, m, n, isSingle, kind, triFlag,
                                          hasUserRankTol, userRankTol, mr);
    auto p = keel::make_arc_in<DecompositionPayload>(mr);
    p->factors = keel::move(f);
    p->checkCondition = checkCondition;
    return makeDecompValue(keel::move(p), mr);
}

// ---------------------------------------------------------------------------
// High-level solve drivers (mldivide / mrdivide)
// ---------------------------------------------------------------------------

void emitIllConditionedWarningIfNeeded(const DecompositionPayload &p, CallContext &ctx)
{
    if (!p.checkCondition || !ctx.engine)
        return;
    const auto &f = *p.factors;
    if (!isIllConditionedFactors(f))
        return;
    auto *eng = static_cast<Engine *>(ctx.engine);
    if (f.hasRank) {
        auto msg = keel::format("Rank deficient, rank = {}, tol = {:.6e}.", f.numRank, f.rankTol);
        eng->setLastWarn(msg, "numkit:decomposition:rankDeficientMatrix");
    } else {
        auto msg = keel::format("Matrix is close to singular or badly scaled. Results may be inaccurate. RCOND = {:.6e}.",
                                f.rcondVal);
        eng->setLastWarn(msg, "numkit:decomposition:nearlySingularMatrix");
    }
}

// Solve op(A) * X = B (without applying ScaleFactor yet).
// B is (opRows x nrhs), returns X (opCols x nrhs) in raw buffer or Value.
Value solveRawValue(const DecompositionPayload &p, bool effectiveConjTrans,
                    const Value &B, bool wantComplex, bool wantSingle,
                    keel::Allocator *mr)
{
    const auto &f = *p.factors;
    const keel::usize opRows = effectiveConjTrans ? f.cols : f.rows;
    const keel::usize opCols = effectiveConjTrans ? f.rows : f.cols;
    const keel::usize nrhs = B.dims().cols();

    if (opCols == 0 || nrhs == 0) {
        const ValueType outType = wantComplex ? ValueType::COMPLEX
                                : (wantSingle ? ValueType::SINGLE : ValueType::DOUBLE);
        return Value::matrix(opCols, nrhs, outType, mr);
    }
    if (opRows == 0) {
        const ValueType outType = wantComplex ? ValueType::COMPLEX
                                : (wantSingle ? ValueType::SINGLE : ValueType::DOUBLE);
        Value out = Value::matrix(opCols, nrhs, outType, mr);
        if (outType == ValueType::COMPLEX) {
            keel::fill(out.complexDataMut(), out.complexDataMut() + opCols * nrhs, keel::c64(0.0, 0.0));
        } else if (outType == ValueType::SINGLE) {
            keel::fill(out.singleDataMut(), out.singleDataMut() + opCols * nrhs, keel::f32(0.0f));
        } else {
            keel::fill(out.doubleDataMut(), out.doubleDataMut() + opCols * nrhs, 0.0);
        }
        return out;
    }

    keel::ArenaTemp scratch = keel::get_scratch(mr);

    if (!f.isComplex && !B.isComplex()) {
        // Real factors + real RHS.
        keel::Array<keel::f64> Bbuf(opRows * nrhs, scratch.allocator());
        if (B.type() == ValueType::DOUBLE) {
            keel::copy(B.doubleData(), B.doubleData() + opRows * nrhs, Bbuf.begin());
        } else {
            for (keel::usize i = 0; i < opRows * nrhs; ++i)
                Bbuf[i] = B.elemAsDouble(i);
        }
        keel::Array<keel::f64> Xbuf(opCols * nrhs, scratch.allocator());
        solveFactorsTyped<keel::f64>(f, effectiveConjTrans, Bbuf.data(), nrhs, Xbuf.data(), scratch.allocator());

        if (wantComplex) {
            Value out = Value::complexMatrix(opCols, nrhs, mr);
            keel::c64 *dst = out.complexDataMut();
            for (keel::usize i = 0; i < opCols * nrhs; ++i)
                dst[i] = keel::c64(Xbuf[i], 0.0);
            return out;
        }
        if (wantSingle) {
            Value out = Value::matrix(opCols, nrhs, ValueType::SINGLE, mr);
            keel::f32 *dst = out.singleDataMut();
            for (keel::usize i = 0; i < opCols * nrhs; ++i)
                dst[i] = keel::as<keel::f32>(Xbuf[i]);
            return out;
        }
        Value out = Value::matrix(opCols, nrhs, ValueType::DOUBLE, mr);
        keel::copy(Xbuf.begin(), Xbuf.end(), out.doubleDataMut());
        return out;
    }

    if (!f.isComplex && B.isComplex()) {
        // Real factors + complex RHS: solve real and imag parts separately with real factors!
        keel::Array<keel::f64> Bre(opRows * nrhs, scratch.allocator());
        keel::Array<keel::f64> Bim(opRows * nrhs, scratch.allocator());
        const keel::c64 *bp = B.complexData();
        for (keel::usize i = 0; i < opRows * nrhs; ++i) {
            Bre[i] = bp[i].real();
            Bim[i] = bp[i].imag();
        }
        keel::Array<keel::f64> Xre(opCols * nrhs, scratch.allocator());
        keel::Array<keel::f64> Xim(opCols * nrhs, scratch.allocator());
        solveFactorsTyped<keel::f64>(f, effectiveConjTrans, Bre.data(), nrhs, Xre.data(), scratch.allocator());
        solveFactorsTyped<keel::f64>(f, effectiveConjTrans, Bim.data(), nrhs, Xim.data(), scratch.allocator());

        Value out = Value::complexMatrix(opCols, nrhs, mr);
        keel::c64 *dst = out.complexDataMut();
        for (keel::usize i = 0; i < opCols * nrhs; ++i)
            dst[i] = keel::c64(Xre[i], Xim[i]);
        return out;
    }

    // Complex factors (with real or complex RHS).
    keel::Array<keel::c64> Bbuf(opRows * nrhs, scratch.allocator());
    if (B.isComplex()) {
        keel::copy(B.complexData(), B.complexData() + opRows * nrhs, Bbuf.begin());
    } else {
        for (keel::usize i = 0; i < opRows * nrhs; ++i)
            Bbuf[i] = keel::c64(B.elemAsDouble(i), 0.0);
    }
    Value out = Value::complexMatrix(opCols, nrhs, mr);
    solveFactorsTyped<keel::c64>(f, effectiveConjTrans, Bbuf.data(), nrhs,
                                 out.complexDataMut(), scratch.allocator());
    return out;
}

void applyInverseScaleInPlace(Value &X, keel::c64 alpha, bool alphaIsComplex, bool wantSingle)
{
    if (!alphaIsComplex && alpha.real() == 1.0)
        return;
    const keel::usize n = X.numel();
    if (n == 0)
        return;
    if (X.isComplex()) {
        keel::c64 *p = X.complexDataMut();
        for (keel::usize i = 0; i < n; ++i)
            p[i] = p[i] / alpha;
        return;
    }
    if (wantSingle && X.type() == ValueType::SINGLE) {
        keel::f32 *p = X.singleDataMut();
        const keel::f64 a = alpha.real();
        for (keel::usize i = 0; i < n; ++i)
            p[i] = keel::as<keel::f32>(keel::as<keel::f64>(p[i]) / a);
        return;
    }
    keel::f64 *p = X.doubleDataMut();
    const keel::f64 a = alpha.real();
    for (keel::usize i = 0; i < n; ++i)
        p[i] /= a;
}

Value conjTransposeMatrix(const Value &M, keel::Allocator *mr)
{
    const keel::usize r = M.dims().rows();
    const keel::usize c = M.dims().cols();
    if (M.isComplex()) {
        Value out = Value::complexMatrix(c, r, mr);
        const keel::c64 *src = M.complexData();
        keel::c64 *dst = out.complexDataMut();
        for (keel::usize j = 0; j < c; ++j) {
            for (keel::usize i = 0; i < r; ++i)
                dst[j + i * c] = keel::conj(src[i + j * r]);
        }
        return out;
    }
    if (M.type() == ValueType::SINGLE) {
        Value out = Value::matrix(c, r, ValueType::SINGLE, mr);
        const keel::f32 *src = M.singleData();
        keel::f32 *dst = out.singleDataMut();
        for (keel::usize j = 0; j < c; ++j) {
            for (keel::usize i = 0; i < r; ++i)
                dst[j + i * c] = src[i + j * r];
        }
        return out;
    }
    Value out = Value::matrix(c, r, ValueType::DOUBLE, mr);
    keel::f64 *dst = out.doubleDataMut();
    if (M.type() == ValueType::DOUBLE) {
        const keel::f64 *src = M.doubleData();
        for (keel::usize j = 0; j < c; ++j) {
            for (keel::usize i = 0; i < r; ++i)
                dst[j + i * c] = src[i + j * r];
        }
    } else {
        for (keel::usize j = 0; j < c; ++j) {
            for (keel::usize i = 0; i < r; ++i)
                dst[j + i * c] = M.elemAsDouble(i + j * r);
        }
    }
    return out;
}

void validateRhsMatrix(const Value &B, const char *caller)
{
    if (isDecompObject(B)) {
        auto msg = keel::format("{}: cannot divide a decomposition by another decomposition", caller);
        throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidSolve");
    }
    if (!B.isNumeric() && !B.isLogical()) {
        auto msg = keel::format("{}: matrix operand must be numeric or logical", caller);
        throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidRHS");
    }
    if (B.dims().ndim() > 2) {
        auto msg = keel::format("{}: matrix operand must be 2-D", caller);
        throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:Not2D");
    }
}

void extractScaleScalar(const Value &s, keel::c64 &valOut, bool &isComplexOut,
                        const char *caller, const char *errId)
{
    if (isDecompObject(s) || (!s.isNumeric() && !s.isLogical()) || s.numel() != 1) {
        auto msg = keel::format("{}: decomposition scaling requires a numeric scalar", caller);
        throw Error(msg.c_str(), 0, 0, caller, "", errId);
    }
    if (s.isComplex()) {
        valOut = s.complexData()[0];
        isComplexOut = (valOut.imag() != 0.0);
    } else {
        valOut = keel::c64(s.elemAsDouble(0), 0.0);
        isComplexOut = false;
    }
}

Value doMldivide(const Value &lhs, const Value &rhs, CallContext &ctx)
{
    if (!isDecompObject(lhs)) {
        throw Error("mldivide: left operand must be a decomposition object",
                    0, 0, "mldivide", "", "numkit:decomposition:InvalidSolve");
    }
    validateRhsMatrix(rhs, "mldivide");
    const auto &p = requireDecompConst(lhs, "mldivide");
    const auto &f = *p.factors;

    if (f.kind == DecompKind::QR && p.isConjTrans) {
        throw Error("mldivide: backslash is not supported for conjugate-transposed 'qr' decomposition; use 'cod' or construct decomposition of the transposed matrix",
                    0, 0, "mldivide", "", "numkit:decomposition:QRmldivideTransp");
    }

    const keel::usize opRows = p.isConjTrans ? f.cols : f.rows;
    if (rhs.dims().rows() != opRows) {
        auto msg = keel::format("mldivide: matrix dimensions must agree (expected {} rows, got {})",
                                opRows, rhs.dims().rows());
        throw Error(msg.c_str(), 0, 0, "mldivide", "", "numkit:decomposition:DimMismatch");
    }

    emitIllConditionedWarningIfNeeded(p, ctx);

    const bool wantComplex = f.isComplex || p.scaleIsComplex || rhs.isComplex();
    const bool wantSingle = !wantComplex && (f.isSingle || rhs.type() == ValueType::SINGLE);
    Value X = solveRawValue(p, p.isConjTrans, rhs, wantComplex, wantSingle, ctxAllocator(ctx));
    const keel::c64 alpha = p.isConjTrans ? keel::conj(p.scaleFactor) : p.scaleFactor;
    applyInverseScaleInPlace(X, alpha, p.scaleIsComplex, wantSingle);
    return X;
}

Value doMrdivide(const Value &lhs, const Value &rhs, CallContext &ctx)
{
    keel::Allocator *mr = ctxAllocator(ctx);
    if (isDecompObject(lhs)) {
        // d / scalar
        const auto &p = requireDecompConst(lhs, "mrdivide");
        keel::c64 s{1.0, 0.0};
        bool sCpx = false;
        extractScaleScalar(rhs, s, sCpx, "mrdivide", "numkit:decomposition:InvalidDiv");
        const keel::c64 ns = p.scaleFactor / s;
        return cloneWithChanges(p, p.isConjTrans, ns, (ns.imag() != 0.0), p.checkCondition, mr);
    }

    // B / d: solve X * (alpha * op(A)) = B  <=>  op(A)' * Y = B', X = Y' / alpha
    validateRhsMatrix(lhs, "mrdivide");
    const auto &p = requireDecompConst(rhs, "mrdivide");
    const auto &f = *p.factors;

    if (f.kind == DecompKind::QR && !p.isConjTrans) {
        throw Error("mrdivide: forward slash is supported only for transposed 'qr' decomposition; use 'cod' or construct decomposition of the transposed matrix",
                    0, 0, "mrdivide", "", "numkit:decomposition:QRmrdivideTransp");
    }

    const keel::usize opCols = p.isConjTrans ? f.rows : f.cols;
    if (lhs.dims().cols() != opCols) {
        auto msg = keel::format("mrdivide: matrix dimensions must agree (expected {} columns, got {})",
                                opCols, lhs.dims().cols());
        throw Error(msg.c_str(), 0, 0, "mrdivide", "", "numkit:decomposition:DimMismatch");
    }

    emitIllConditionedWarningIfNeeded(p, ctx);

    const bool wantComplex = f.isComplex || p.scaleIsComplex || lhs.isComplex();
    const bool wantSingle = !wantComplex && (f.isSingle || lhs.type() == ValueType::SINGLE);
    Value Bh = conjTransposeMatrix(lhs, mr);
    Value Y = solveRawValue(p, !p.isConjTrans, Bh, wantComplex, wantSingle, mr);
    Value X = conjTransposeMatrix(Y, mr);
    const keel::c64 alpha = p.isConjTrans ? keel::conj(p.scaleFactor) : p.scaleFactor;
    applyInverseScaleInPlace(X, alpha, p.scaleIsComplex, wantSingle);
    return X;
}

Value doScaleMul(const Value &lhs, const Value &rhs, const char *caller, CallContext &ctx)
{
    const bool lhsIsD = isDecompObject(lhs);
    const bool rhsIsD = isDecompObject(rhs);
    if (lhsIsD && rhsIsD) {
        auto msg = keel::format("{}: cannot multiply two decomposition objects", caller);
        throw Error(msg.c_str(), 0, 0, caller, "", "numkit:decomposition:InvalidMult");
    }
    const Value &dVal = lhsIsD ? lhs : rhs;
    const Value &sVal = lhsIsD ? rhs : lhs;
    const auto &p = requireDecompConst(dVal, caller);
    keel::c64 s{1.0, 0.0};
    bool sCpx = false;
    extractScaleScalar(sVal, s, sCpx, caller, "numkit:decomposition:InvalidMult");
    const keel::c64 ns = p.scaleFactor * s;
    return cloneWithChanges(p, p.isConjTrans, ns, (ns.imag() != 0.0), p.checkCondition, ctxAllocator(ctx));
}

Value doRdivide(const Value &lhs, const Value &rhs, CallContext &ctx)
{
    if (!isDecompObject(lhs)) {
        throw Error("rdivide: element-wise division requires decomposition on the left and a scalar on the right",
                    0, 0, "rdivide", "", "numkit:decomposition:InvalidDiv");
    }
    const auto &p = requireDecompConst(lhs, "rdivide");
    keel::c64 s{1.0, 0.0};
    bool sCpx = false;
    extractScaleScalar(rhs, s, sCpx, "rdivide", "numkit:decomposition:InvalidDiv");
    const keel::c64 ns = p.scaleFactor / s;
    return cloneWithChanges(p, p.isConjTrans, ns, (ns.imag() != 0.0), p.checkCondition, ctxAllocator(ctx));
}

Value doLdivide(const Value &lhs, const Value &rhs, CallContext &ctx)
{
    if (!isDecompObject(rhs)) {
        throw Error("ldivide: element-wise left division requires a scalar on the left and decomposition on the right",
                    0, 0, "ldivide", "", "numkit:decomposition:InvalidDiv");
    }
    const auto &p = requireDecompConst(rhs, "ldivide");
    keel::c64 s{1.0, 0.0};
    bool sCpx = false;
    extractScaleScalar(lhs, s, sCpx, "ldivide", "numkit:decomposition:InvalidDiv");
    const keel::c64 ns = p.scaleFactor / s;
    return cloneWithChanges(p, p.isConjTrans, ns, (ns.imag() != 0.0), p.checkCondition, ctxAllocator(ctx));
}

// ---------------------------------------------------------------------------
// Property accessors & display
// ---------------------------------------------------------------------------

Value makeScaleFactorValue(const DecompositionPayload &p, keel::Allocator *mr)
{
    if (p.scaleIsComplex)
        return Value::complexScalar(p.scaleFactor, mr);
    if (p.factors->isSingle) {
        Value out = Value::matrix(1, 1, ValueType::SINGLE, mr);
        out.singleDataMut()[0] = keel::as<keel::f32>(p.scaleFactor.real());
        return out;
    }
    return Value::scalar(p.scaleFactor.real(), mr);
}

bool propGetDecomp(const Value &self, keel::StringRef name, Value &out, CallContext &ctx)
{
    const auto &p = requireDecompConst(self, "decomposition");
    const auto &f = *p.factors;
    keel::Allocator *mr = ctxAllocator(ctx);

    if (name == "Type") {
        out = Value::fromString(kindName(f.kind), mr);
        return true;
    }
    if (name == "ScaleFactor") {
        out = makeScaleFactorValue(p, mr);
        return true;
    }
    if (name == "IsSparse") {
        out = Value::logicalScalar(false, mr);
        return true;
    }
    if (name == "IsReal") {
        out = Value::logicalScalar(!f.isComplex, mr);
        return true;
    }
    if (name == "MatrixSize") {
        Value sz = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        sz.doubleDataMut()[0] = keel::as<keel::f64>(f.rows);
        sz.doubleDataMut()[1] = keel::as<keel::f64>(f.cols);
        out = keel::move(sz);
        return true;
    }
    if (name == "IsConjugateTransposed") {
        out = Value::logicalScalar(p.isConjTrans, mr);
        return true;
    }
    if (name == "Datatype") {
        out = Value::fromString(f.isSingle ? "single" : "double", mr);
        return true;
    }
    if (name == "CheckCondition") {
        out = Value::logicalScalar(p.checkCondition, mr);
        return true;
    }
    return false;
}

bool propSetDecomp(Value &self, keel::StringRef name, const Value &val, CallContext &ctx)
{
    (void)ctx;
    if (name == "CheckCondition") {
        const bool b = extractLogicalScalar(val, "CheckCondition", "decomposition");
        auto &p = requireDecompMut(self, "decomposition");
        p.checkCondition = b;
        return true;
    }
    if (name == "Type" || name == "ScaleFactor" || name == "IsSparse" || name == "IsReal"
        || name == "MatrixSize" || name == "IsConjugateTransposed" || name == "Datatype") {
        auto msg = keel::format("You cannot set the read-only property '{}' of decomposition.", name);
        throw Error(msg.c_str(), 0, 0, "decomposition", "", "numkit:decomposition:ReadOnlyProperty");
    }
    return false;
}

keel::String dispDecomp(const Value &self)
{
    if (self.objectCount() != 1)
        return {};
    const auto &p = requireDecompConst(self, "decomposition");
    const auto &f = *p.factors;
    keel::String scaleStr = p.scaleIsComplex
        ? keel::format("{:.6g}{:+.6g}i", p.scaleFactor.real(), p.scaleFactor.imag())
        : keel::format("{:.6g}", p.scaleFactor.real());
    return keel::format(
        "  decomposition with properties:\n\n"
        "                     Type: '{}'\n"
        "              ScaleFactor: {}\n"
        "                 IsSparse: 0\n"
        "                   IsReal: {}\n"
        "               MatrixSize: [{} {}]\n"
        "    IsConjugateTransposed: {}\n"
        "                 Datatype: '{}'\n"
        "           CheckCondition: {}\n",
        kindName(f.kind),
        scaleStr,
        f.isComplex ? 0 : 1,
        f.rows, f.cols,
        p.isConjTrans ? 1 : 0,
        f.isSingle ? "single" : "double",
        p.checkCondition ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Methods: rcond, rank, isIllConditioned, unary/binary operator methods
// ---------------------------------------------------------------------------

void methodRcond(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 0, 0, "rcond");
    const auto &p = requireDecompConst(self, "rcond");
    const auto &f = *p.factors;
    if (!f.hasRcond) {
        auto msg = keel::format("rcond: reciprocal condition number is not supported for decomposition type '{}'",
                                kindName(f.kind));
        throw Error(msg.c_str(), 0, 0, "rcond", "", "numkit:decomposition:RcondNotSupported");
    }
    keel::Allocator *mr = ctxAllocator(ctx);
    if (f.isSingle) {
        Value out = Value::matrix(1, 1, ValueType::SINGLE, mr);
        out.singleDataMut()[0] = keel::as<keel::f32>(f.rcondVal);
        outs[0] = keel::move(out);
    } else {
        outs[0] = Value::scalar(f.rcondVal, mr);
    }
}

void methodRank(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 0, 0, "rank");
    const auto &p = requireDecompConst(self, "rank");
    const auto &f = *p.factors;
    if (!f.hasRank) {
        auto msg = keel::format("rank: numerical rank is not supported for decomposition type '{}' (supported for 'qr' and 'cod')",
                                kindName(f.kind));
        throw Error(msg.c_str(), 0, 0, "rank", "", "numkit:decomposition:RankNotSupported");
    }
    outs[0] = Value::scalar(keel::as<keel::f64>(f.numRank), ctxAllocator(ctx));
}

void methodIsIllConditioned(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                            keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 0, 0, "isIllConditioned");
    const auto &p = requireDecompConst(self, "isIllConditioned");
    outs[0] = Value::logicalScalar(isIllConditionedFactors(*p.factors), ctxAllocator(ctx));
}

void opCtranspose(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 0, 0, "ctranspose");
    const auto &p = requireDecompConst(self, "ctranspose");
    outs[0] = cloneWithChanges(p, !p.isConjTrans, p.scaleFactor, p.scaleIsComplex,
                               p.checkCondition, ctxAllocator(ctx));
}

void opTranspose(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)self;
    (void)args;
    (void)nargout;
    (void)outs;
    (void)ctx;
    throw Error("Transpose operator .' not supported for decomposition. Use the complex conjugate transpose operator ' instead.",
                0, 0, "transpose", "", "numkit:decomposition:TransposeNotSupported");
}

void opUminus(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
              keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 0, 0, "uminus");
    const auto &p = requireDecompConst(self, "uminus");
    const keel::c64 ns(-p.scaleFactor.real(), -p.scaleFactor.imag());
    outs[0] = cloneWithChanges(p, p.isConjTrans, ns, p.scaleIsComplex,
                               p.checkCondition, ctxAllocator(ctx));
}

void opUplus(Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
             keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 0, 0, "uplus");
    const auto &p = requireDecompConst(self, "uplus");
    outs[0] = cloneWithChanges(p, p.isConjTrans, p.scaleFactor, p.scaleIsComplex,
                               p.checkCondition, ctxAllocator(ctx));
}

} // anonymous namespace

void register_decomposition(Engine &engine)
{
    BuiltinClass cls;
    cls.name = keel::String(kDecompClass);
    cls.isHandle = false;
    cls.isSealed = true;
    cls.construct = &constructDecomposition;
    cls.propGet = &propGetDecomp;
    cls.propSet = &propSetDecomp;
    cls.dispText = &dispDecomp;

    const char *kProps[] = {
        "Type",
        "ScaleFactor",
        "IsSparse",
        "IsReal",
        "MatrixSize",
        "IsConjugateTransposed",
        "Datatype",
        "CheckCondition",
    };
    for (const char *pn : kProps) {
        cls.propNames.push_back(keel::String(pn));
        PropMeta pm;
        pm.name = keel::String(pn);
        pm.getAccess = keel::String("public");
        pm.setAccess = keel::String((keel::StringRef(pn) == "CheckCondition") ? "public" : "private");
        cls.propMeta.push_back(keel::move(pm));
    }

    // Operators (`cls.ops` receives binary operands as args[0]=lhs, args[1]=rhs; unary as self).
    cls.ops.put(keel::String("mldivide"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)self; (void)nargout;
            argcheck::checkNargin(args.size(), 2, 2, "mldivide");
            outs[0] = doMldivide(args[0], args[1], ctx);
        });
    cls.ops.put(keel::String("mrdivide"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)self; (void)nargout;
            argcheck::checkNargin(args.size(), 2, 2, "mrdivide");
            outs[0] = doMrdivide(args[0], args[1], ctx);
        });
    cls.ops.put(keel::String("mtimes"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)self; (void)nargout;
            argcheck::checkNargin(args.size(), 2, 2, "mtimes");
            outs[0] = doScaleMul(args[0], args[1], "mtimes", ctx);
        });
    cls.ops.put(keel::String("times"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)self; (void)nargout;
            argcheck::checkNargin(args.size(), 2, 2, "times");
            outs[0] = doScaleMul(args[0], args[1], "times", ctx);
        });
    cls.ops.put(keel::String("rdivide"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)self; (void)nargout;
            argcheck::checkNargin(args.size(), 2, 2, "rdivide");
            outs[0] = doRdivide(args[0], args[1], ctx);
        });
    cls.ops.put(keel::String("ldivide"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)self; (void)nargout;
            argcheck::checkNargin(args.size(), 2, 2, "ldivide");
            outs[0] = doLdivide(args[0], args[1], ctx);
        });
    cls.ops.put(keel::String("ctranspose"), &opCtranspose);
    cls.ops.put(keel::String("transpose"),  &opTranspose);
    cls.ops.put(keel::String("uminus"),     &opUminus);
    cls.ops.put(keel::String("uplus"),      &opUplus);

    // Methods (`cls.methods` receives `self` as arg 0, remaining args in `args`).
    cls.methods.put(keel::String("rcond"),            &methodRcond);
    cls.methods.put(keel::String("rank"),             &methodRank);
    cls.methods.put(keel::String("isIllConditioned"), &methodIsIllConditioned);
    cls.methods.put(keel::String("ctranspose"),       &opCtranspose);
    cls.methods.put(keel::String("transpose"),        &opTranspose);
    cls.methods.put(keel::String("uminus"),           &opUminus);
    cls.methods.put(keel::String("uplus"),            &opUplus);
    cls.methods.put(keel::String("mldivide"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)nargout;
            argcheck::checkNargin(args.size(), 1, 1, "mldivide");
            outs[0] = doMldivide(self, args[0], ctx);
        });
    cls.methods.put(keel::String("mrdivide"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)nargout;
            argcheck::checkNargin(args.size(), 1, 1, "mrdivide");
            outs[0] = doMrdivide(self, args[0], ctx);
        });
    cls.methods.put(keel::String("mtimes"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)nargout;
            argcheck::checkNargin(args.size(), 1, 1, "mtimes");
            outs[0] = doScaleMul(self, args[0], "mtimes", ctx);
        });
    cls.methods.put(keel::String("times"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)nargout;
            argcheck::checkNargin(args.size(), 1, 1, "times");
            outs[0] = doScaleMul(self, args[0], "times", ctx);
        });
    cls.methods.put(keel::String("rdivide"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)nargout;
            argcheck::checkNargin(args.size(), 1, 1, "rdivide");
            outs[0] = doRdivide(self, args[0], ctx);
        });
    cls.methods.put(keel::String("ldivide"),
        [](Value &self, keel::ArrayRef<Value> args, keel::usize nargout,
           keel::MutArrayRef<Value> outs, CallContext &ctx) {
            (void)nargout;
            argcheck::checkNargin(args.size(), 1, 1, "ldivide");
            outs[0] = doLdivide(self, args[0], ctx);
        });

    const char *kMethods[] = {
        "rcond", "rank", "isIllConditioned",
        "mldivide", "mrdivide", "mtimes", "times", "rdivide", "ldivide",
        "ctranspose", "transpose", "uminus", "uplus",
    };
    for (const char *mn : kMethods) {
        MethodMeta mm;
        mm.name = keel::String(mn);
        cls.methodMeta.push_back(keel::move(mm));
    }

    engine.registerClass(keel::move(cls));
}

} // namespace numkit::linalg::detail
