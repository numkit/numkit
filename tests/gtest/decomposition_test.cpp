// tests/gtest/decomposition_test.cpp — Dual-engine tests for `decomposition` BuiltinClass
// Parameterized: runs on both TreeWalker and Bytecode VM backends.

#include <keel/core.h>
#include <keel/math.h>
#include "dual_engine_fixture.hpp"

using namespace m_test;

class DecompositionTest : public DualEngineTest {};

TEST_P(DecompositionTest, DefaultEmptyAndProperties)
{
    eval("d = decomposition();");
    EXPECT_EQ(evalString("class(d)"), "decomposition");
    EXPECT_EQ(evalString("d.Type"), "triangular");
    EXPECT_EQ(evalString("d.Datatype"), "double");
    EXPECT_DOUBLE_EQ(evalScalar("d.ScaleFactor"), 1.0);
    EXPECT_FALSE(evalBool("d.IsSparse"));
    EXPECT_TRUE(evalBool("d.IsReal"));
    EXPECT_FALSE(evalBool("d.IsConjugateTransposed"));
    EXPECT_TRUE(evalBool("d.CheckCondition"));
    eval("sz = d.MatrixSize;");
    auto *sz = getVarPtr("sz");
    ASSERT_NE(sz, nullptr);
    EXPECT_DOUBLE_EQ((*sz)(0, 0), 0.0);
    EXPECT_DOUBLE_EQ((*sz)(0, 1), 0.0);
    EXPECT_TRUE(keel::is_inf(evalScalar("rcond(d)")));
    EXPECT_FALSE(evalBool("isIllConditioned(d)"));
    EXPECT_DOUBLE_EQ(evalScalar("size(d, 1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("size(d, 2)"), 1.0);
}

TEST_P(DecompositionTest, AutoTypeSelection)
{
    EXPECT_EQ(evalString("decomposition([4 1; 2 3]).Type"), "lu");
    EXPECT_EQ(evalString("decomposition([4 1; 0 3]).Type"), "triangular");
    EXPECT_EQ(evalString("decomposition([4 0; 2 3]).Type"), "triangular");
    EXPECT_EQ(evalString("decomposition([4 0; 0 3]).Type"), "triangular");
    EXPECT_EQ(evalString("decomposition([1 2; 3 4; 5 6]).Type"), "qr");
    EXPECT_EQ(evalString("decomposition(zeros(0, 3)).Type"), "qr");
    EXPECT_EQ(evalString("decomposition(zeros(3, 0)).Type"), "qr");
    EXPECT_EQ(evalString("decomposition(zeros(0, 0)).Type"), "triangular");
}

TEST_P(DecompositionTest, LUSolveLeftRightTransposeAndMatrixRHS)
{
    eval("A = [4 1 2; 3 5 1; 2 1 6];"
         "b = [1; 2; 3];"
         "B = [1 4; 2 5; 3 6];"
         "d = decomposition(A, 'lu');"
         "x = d \\ b;"
         "X = d \\ B;"
         "xt = d' \\ b;"
         "xr = b' / d;"
         "xrt = b' / d';"
         "err1 = norm(A * x - b);"
         "err2 = norm(A * X - B);"
         "err3 = norm(A' * xt - b);"
         "err4 = norm(xr * A - b');"
         "err5 = norm(xrt * A' - b');"
         "rc_d = rcond(d);"
         "rc_a = rcond(A);");
    EXPECT_LT(getVar("err1"), 1e-13);
    EXPECT_LT(getVar("err2"), 1e-13);
    EXPECT_LT(getVar("err3"), 1e-13);
    EXPECT_LT(getVar("err4"), 1e-13);
    EXPECT_LT(getVar("err5"), 1e-13);
    EXPECT_NEAR(getVar("rc_d"), getVar("rc_a"), 1e-12);
    EXPECT_FALSE(evalBool("isIllConditioned(d)"));
}

TEST_P(DecompositionTest, CholeskyUpperLowerAndRejection)
{
    eval("A = [4 1 1; 1 5 2; 1 2 6]; b = [1; 2; 3];"
         "d = decomposition(A, 'chol');"
         "x = d \\ b;"
         "err = norm(A * x - b);");
    EXPECT_EQ(evalString("d.Type"), "chol");
    EXPECT_LT(getVar("err"), 1e-13);

    // Lower triangle only ignores strict upper triangle.
    eval("An = [4 100; 2 5];"
         "dl = decomposition(An, 'chol', 'lower');"
         "xl = dl \\ [1; 2];"
         "errl = norm([4 2; 2 5] * xl - [1; 2]);");
    EXPECT_LT(getVar("errl"), 1e-13);

    // Upper triangle only on An has [4 100; 100 5] which is not SPD -> throws.
    EXPECT_THROW(eval("decomposition(An, 'chol', 'upper');"), numkit::Error);
    // Non-SPD matrix -> throws.
    EXPECT_THROW(eval("decomposition([1 2; 2 1], 'chol');"), numkit::Error);
    // Non-square matrix -> throws.
    EXPECT_THROW(eval("decomposition([1 2 3; 4 5 6], 'chol');"), numkit::Error);
}

TEST_P(DecompositionTest, LDLIndefiniteAndTriangles)
{
    // Indefinite symmetric with 0 diagonal (tests LU fallback while preserving Type='ldl').
    eval("Ai = [0 1; 1 0]; b = [3; 5];"
         "di = decomposition(Ai, 'ldl');"
         "xi = di \\ b;"
         "erri = norm(Ai * xi - b);");
    EXPECT_EQ(evalString("di.Type"), "ldl");
    EXPECT_LT(getVar("erri"), 1e-13);

    // Explicit lower/upper triangle symmetrizes from chosen triangle.
    eval("An = [4 100; 2 5];"
         "dl = decomposition(An, 'ldl', 'lower');"
         "du = decomposition(An, 'ldl', 'upper');"
         "errl = norm([4 2; 2 5] * (dl \\ [1; 2]) - [1; 2]);"
         "erru = norm([4 100; 100 5] * (du \\ [1; 2]) - [1; 2]);");
    EXPECT_LT(getVar("errl"), 1e-13);
    EXPECT_LT(getVar("erru"), 1e-13);

    // Non-symmetric without flag -> throws.
    EXPECT_THROW(eval("decomposition([4 100; 2 5], 'ldl');"), numkit::Error);
    // Complex Hermitian succeeds; complex non-Hermitian symmetric throws without flag.
    eval("Ch = [4, 1+2i; 1-2i, 5];"
         "dh = decomposition(Ch, 'ldl');"
         "errh = norm(Ch * (dh \\ [1; 2]) - [1; 2]);");
    EXPECT_LT(getVar("errh"), 1e-13);
    EXPECT_THROW(eval("decomposition([4, 1+2i; 1+2i, 5], 'ldl');"), numkit::Error);
}

TEST_P(DecompositionTest, TriangularDiagonalPermutedBandedHessenberg)
{
    // Triangular with explicit 'upper' / 'lower' ignores opposite triangle.
    eval("M = [2 10; 20 4];"
         "xu = decomposition(M, 'triangular', 'upper') \\ [12; 4];"
         "xl = decomposition(M, 'triangular', 'lower') \\ [2; 24];");
    auto *xu = getVarPtr("xu");
    auto *xl = getVarPtr("xl");
    ASSERT_NE(xu, nullptr);
    ASSERT_NE(xl, nullptr);
    EXPECT_NEAR((*xu)(0, 0), 1.0, 1e-13);
    EXPECT_NEAR((*xu)(1, 0), 1.0, 1e-13);
    EXPECT_NEAR((*xl)(0, 0), 1.0, 1e-13);
    EXPECT_NEAR((*xl)(1, 0), 1.0, 1e-13);
    EXPECT_THROW(eval("decomposition(M, 'triangular');"), numkit::Error);

    // Diagonal
    eval("Ad = diag([2, 4, 8]);"
         "dd = decomposition(Ad, 'diagonal');"
         "xd = dd \\ [4; 12; 16];"
         "rcd = rcond(dd);");
    auto *xd = getVarPtr("xd");
    ASSERT_NE(xd, nullptr);
    EXPECT_EQ(evalString("dd.Type"), "diagonal");
    EXPECT_NEAR((*xd)(0, 0), 2.0, 1e-13);
    EXPECT_NEAR((*xd)(1, 0), 3.0, 1e-13);
    EXPECT_NEAR((*xd)(2, 0), 2.0, 1e-13);
    EXPECT_NEAR(getVar("rcd"), 0.25, 1e-13);
    EXPECT_THROW(eval("decomposition([2 1; 0 3], 'diagonal');"), numkit::Error);

    // Permuted triangular
    eval("Ap = [0 0 2; 3 1 4; 0 5 6];"
         "dp = decomposition(Ap, 'permutedTriangular');"
         "xp = dp \\ [1; 2; 3];"
         "xpt = dp' \\ [1; 2; 3];"
         "errp = norm(Ap * xp - [1; 2; 3]);"
         "errpt = norm(Ap' * xpt - [1; 2; 3]);");
    EXPECT_EQ(evalString("dp.Type"), "permutedTriangular");
    EXPECT_LT(getVar("errp"), 1e-13);
    EXPECT_LT(getVar("errpt"), 1e-13);
    EXPECT_THROW(eval("decomposition([1 2; 3 4], 'permutedTriangular');"), numkit::Error);

    // Banded (accepts any square matrix)
    eval("Ab = [4 1 0; 2 5 1; 0 3 6];"
         "db = decomposition(Ab, 'banded');"
         "errb = norm(Ab * (db \\ [1; 2; 3]) - [1; 2; 3]);");
    EXPECT_EQ(evalString("db.Type"), "banded");
    EXPECT_LT(getVar("errb"), 1e-13);

    // Hessenberg (upper triangular + 1 subdiagonal)
    eval("Ah = [4 1 2; 3 5 1; 0 2 6];"
         "dh = decomposition(Ah, 'hessenberg');"
         "errh = norm(Ah * (dh \\ [1; 2; 3]) - [1; 2; 3]);");
    EXPECT_EQ(evalString("dh.Type"), "hessenberg");
    EXPECT_LT(getVar("errh"), 1e-13);
    EXPECT_THROW(eval("decomposition([4 1 2; 3 5 1; 7 2 6], 'hessenberg');"), numkit::Error);
}

TEST_P(DecompositionTest, QRAndCODRankDeficientAndTransposeRules)
{
    // Rank-deficient tall matrix: QR gives basic solution, COD gives minimum-norm solution.
    eval("Ar = [1 2; 2 4; 3 6]; br = [1; 2; 3];"
         "dqr = decomposition(Ar, 'qr');"
         "dcod = decomposition(Ar, 'cod');"
         "xqr = dqr \\ br;"
         "xcod = dcod \\ br;"
         "rqr = rank(dqr);"
         "rcod = rank(dcod);");
    EXPECT_DOUBLE_EQ(getVar("rqr"), 1.0);
    EXPECT_DOUBLE_EQ(getVar("rcod"), 1.0);
    EXPECT_TRUE(evalBool("isIllConditioned(dqr)"));
    EXPECT_TRUE(evalBool("isIllConditioned(dcod)"));
    auto *xqr = getVarPtr("xqr");
    auto *xcod = getVarPtr("xcod");
    ASSERT_NE(xqr, nullptr);
    ASSERT_NE(xcod, nullptr);
    EXPECT_NEAR((*xqr)(0, 0), 0.0, 1e-12);
    EXPECT_NEAR((*xqr)(1, 0), 0.5, 1e-12);
    EXPECT_NEAR((*xcod)(0, 0), 0.2, 1e-12);
    EXPECT_NEAR((*xcod)(1, 0), 0.4, 1e-12);

    // QR transpose rules: d'\b and b/d throw, b/d' succeeds.
    eval("At = [1 2; 3 4; 5 6]; dt = decomposition(At, 'qr');");
    EXPECT_THROW(eval("dt' \\ [1; 2];"), numkit::Error);
    EXPECT_THROW(eval("[1 2] / dt;"), numkit::Error);
    eval("x_qr_rt = [1 2 3] / dt';");
    auto *x_qr_rt = getVarPtr("x_qr_rt");
    ASSERT_NE(x_qr_rt, nullptr);
    EXPECT_NEAR((*x_qr_rt)(0, 0), 0.0, 1e-12);
    EXPECT_NEAR((*x_qr_rt)(0, 1), 0.5, 1e-12);

    // COD supports both d'\b and b/d.
    eval("dc = decomposition(At, 'cod');"
         "xc_r = [1 2] / dc;"
         "xc_t = dc' \\ [1; 2];");
    auto *xc_r = getVarPtr("xc_r");
    auto *xc_t = getVarPtr("xc_t");
    ASSERT_NE(xc_r, nullptr);
    ASSERT_NE(xc_t, nullptr);
    EXPECT_NEAR((*xc_r)(0, 0), 5.0 / 6.0, 1e-12);
    EXPECT_NEAR((*xc_r)(0, 1), 1.0 / 3.0, 1e-12);
    EXPECT_NEAR((*xc_r)(0, 2), -1.0 / 6.0, 1e-12);
    EXPECT_NEAR((*xc_t)(0, 0), 5.0 / 6.0, 1e-12);
    EXPECT_NEAR((*xc_t)(1, 0), 1.0 / 3.0, 1e-12);
    EXPECT_NEAR((*xc_t)(2, 0), -1.0 / 6.0, 1e-12);

    // RankTolerance option
    eval("d_tol = decomposition([1 0; 0 1e-3], 'cod', 'RankTolerance', 1e-2);");
    EXPECT_DOUBLE_EQ(evalScalar("rank(d_tol)"), 1.0);

    // rcond unsupported on qr/cod; rank unsupported on lu/chol/etc.
    EXPECT_THROW(eval("rcond(dqr);"), numkit::Error);
    EXPECT_THROW(eval("rcond(dcod);"), numkit::Error);
    EXPECT_THROW(eval("rank(decomposition([4 1; 1 3], 'lu'));"), numkit::Error);
}

TEST_P(DecompositionTest, ScalingComplexSingleAndValueSemantics)
{
    // Real & complex scalar scaling + conjugate transpose interaction
    eval("A = [4 1; 1 3]; b = [1; 2];"
         "d = decomposition(A, 'lu');"
         "d2 = 2 * d;"
         "d3 = d * 2;"
         "d4 = d / 2;"
         "d5 = 2 .* d;"
         "d6 = d ./ 2;"
         "d7 = 2 .\\ d;"
         "dn = -d;"
         "dp = +d;"
         "dc = d * (1 + 2i);"
         "e2 = norm((2*A)*(d2 \\ b) - b);"
         "e3 = norm((2*A)*(d3 \\ b) - b);"
         "e4 = norm((A/2)*(d4 \\ b) - b);"
         "e5 = norm((2*A)*(d5 \\ b) - b);"
         "e6 = norm((A/2)*(d6 \\ b) - b);"
         "e7 = norm((A/2)*(d7 \\ b) - b);"
         "en = norm((-A)*(dn \\ b) - b);"
         "ep = norm(A*(dp \\ b) - b);"
         "ec = norm(((1+2i)*A)*(dc \\ b) - b);"
         "ect = norm((((1+2i)*A)')*(dc' \\ b) - b);"
         "ecr = norm((b' / dc)*((1+2i)*A) - b');"
         "ecrt = norm((b' / dc')*(((1+2i)*A)') - b');");
    EXPECT_LT(getVar("e2"), 1e-13);
    EXPECT_LT(getVar("e3"), 1e-13);
    EXPECT_LT(getVar("e4"), 1e-13);
    EXPECT_LT(getVar("e5"), 1e-13);
    EXPECT_LT(getVar("e6"), 1e-13);
    EXPECT_LT(getVar("e7"), 1e-13);
    EXPECT_LT(getVar("en"), 1e-13);
    EXPECT_LT(getVar("ep"), 1e-13);
    EXPECT_LT(getVar("ec"), 1e-13);
    EXPECT_LT(getVar("ect"), 1e-13);
    EXPECT_LT(getVar("ecr"), 1e-13);
    EXPECT_LT(getVar("ecrt"), 1e-13);
    EXPECT_TRUE(evalBool("dc.IsReal"));

    // Single precision preserved
    eval("As = single([4 1; 1 3]); bs = single([1; 2]);"
         "ds = decomposition(As);"
         "xs = ds \\ bs;");
    EXPECT_EQ(evalString("ds.Datatype"), "single");
    EXPECT_EQ(evalString("class(ds.ScaleFactor)"), "single");
    EXPECT_EQ(evalString("class(rcond(ds))"), "single");
    EXPECT_EQ(evalString("class(xs)"), "single");

    // Value semantics: mutating CheckCondition on a copy must NOT mutate original
    eval("dorig = decomposition([4 1; 1 3]);"
         "dcopy = dorig;"
         "dcopy.CheckCondition = false;");
    EXPECT_TRUE(evalBool("dorig.CheckCondition"));
    EXPECT_FALSE(evalBool("dcopy.CheckCondition"));

    // Read-only properties and transpose .' rejection
    EXPECT_THROW(eval("dorig.Type = 'qr';"), numkit::Error);
    EXPECT_THROW(eval("dorig.';"), numkit::Error);
    EXPECT_THROW(eval("dorig * [1 2];"), numkit::Error);
}

TEST_P(DecompositionTest, ValidationErrorsAndEmptyRHS)
{
    // Invalid A (integer, char, 3-D, cell)
    EXPECT_THROW(eval("decomposition(int32([1 2; 3 4]));"), numkit::Error);
    EXPECT_THROW(eval("decomposition('abc');"), numkit::Error);
    EXPECT_THROW(eval("decomposition(ones(2, 2, 2));"), numkit::Error);
    EXPECT_THROW(eval("decomposition({1});"), numkit::Error);
    // Invalid type or triangular flag on LU
    EXPECT_THROW(eval("decomposition([4 1; 1 3], 'bogus');"), numkit::Error);
    EXPECT_THROW(eval("decomposition([4 1; 1 3], 'lu', 'upper');"), numkit::Error);
    // Dimension mismatch on solve
    EXPECT_THROW(eval("decomposition([4 1; 1 3]) \\ [1; 2; 3];"), numkit::Error);
    EXPECT_THROW(eval("[1 2 3] / decomposition([4 1; 1 3]);"), numkit::Error);
    // Empty RHS
    eval("d = decomposition([4 1; 1 3]);"
         "xe = d \\ zeros(2, 0);"
         "xre = zeros(0, 2) / d;");
    auto *xe = getVarPtr("xe");
    auto *xre = getVarPtr("xre");
    ASSERT_NE(xe, nullptr);
    ASSERT_NE(xre, nullptr);
    EXPECT_EQ(rows(*xe), 2u);
    EXPECT_EQ(cols(*xe), 0u);
    EXPECT_EQ(rows(*xre), 0u);
    EXPECT_EQ(cols(*xre), 2u);
}

TEST_P(DecompositionTest, ComplexFactorsLargeRcondAndMethodForms)
{
    // Complex matrices across lu, chol, qr, cod, triangular, diagonal, permutedTriangular
    eval("Ac = [4+1i, 1-2i; 2+1i, 5-1i]; bc = [1+1i; 2-1i];"
         "dlu = decomposition(Ac, 'lu');"
         "e_lu = norm(Ac * (dlu \\ bc) - bc);"
         "e_lut = norm(Ac' * (dlu' \\ bc) - bc);"
         "e_lur = norm((bc' / dlu) * Ac - bc');"
         "Ah = [5, 1+2i; 1-2i, 6];"
         "dch = decomposition(Ah, 'chol');"
         "e_ch = norm(Ah * (dch \\ bc) - bc);"
         "Arc = [1+1i, 2; 3, 4-1i; 5+2i, 6]; b3 = [1; 2+1i; 3];"
         "dqr = decomposition(Arc, 'qr');"
         "dcod = decomposition(Arc, 'cod');"
         "e_qr = norm(Arc' * (Arc * (dqr \\ b3) - b3));"
         "e_cod = norm(Arc' * (Arc * (dcod \\ b3) - b3));"
         "e_codr = norm(([1 2+1i] / dcod) * Arc - [1 2+1i]);"
         "Ad = diag([2+1i, 3-2i]);"
         "dd = decomposition(Ad, 'diagonal');"
         "e_d = norm(Ad * (dd \\ bc) - bc);"
         "e_dt = norm(Ad' * (dd' \\ bc) - bc);"
         "At = [2+1i, 1-1i; 0, 3+2i];"
         "dt = decomposition(At, 'triangular');"
         "e_t = norm(At * (dt \\ bc) - bc);"
         "e_tt = norm(At' * (dt' \\ bc) - bc);");
    EXPECT_FALSE(evalBool("dlu.IsReal"));
    EXPECT_LT(getVar("e_lu"), 1e-12);
    EXPECT_LT(getVar("e_lut"), 1e-12);
    EXPECT_LT(getVar("e_lur"), 1e-12);
    EXPECT_LT(getVar("e_ch"), 1e-12);
    EXPECT_LT(getVar("e_qr"), 1e-12);
    EXPECT_LT(getVar("e_cod"), 1e-12);
    EXPECT_LT(getVar("e_codr"), 1e-12);
    EXPECT_LT(getVar("e_d"), 1e-12);
    EXPECT_LT(getVar("e_dt"), 1e-12);
    EXPECT_LT(getVar("e_t"), 1e-12);
    EXPECT_LT(getVar("e_tt"), 1e-12);

    // Large matrix (n = 70 > 64) exercises Hager-Higham 1-norm rcond estimator
    eval("N = 70; Al = eye(N) * 10 + diag(ones(N-1, 1), 1) + diag(ones(N-1, 1), -1);"
         "dl = decomposition(Al, 'lu');"
         "rc_l = rcond(dl);"
         "bl = (1:N)';"
         "e_large = norm(Al * (dl \\ bl) - bl);");
    EXPECT_GT(getVar("rc_l"), 0.5);
    EXPECT_LE(getVar("rc_l"), 1.0);
    EXPECT_LT(getVar("e_large"), 1e-11);

    // Singular LU & ill-conditioned warning with CheckCondition toggle
    eval("ds = decomposition([1 2; 2 4], 'lu');"
         "rc_s = rcond(ds);"
         "ill_s = isIllConditioned(ds);"
         "disp(ds);");
    EXPECT_DOUBLE_EQ(getVar("rc_s"), 0.0);
    EXPECT_TRUE(evalBool("ill_s"));
    EXPECT_NE(capturedOutput.find("decomposition with properties"), std::string::npos);

    // Function-form method calls & sparse option Name-Value acceptance
    eval("df = decomposition([4 1; 1 3], 'lu', 'CheckCondition', false, 'LUPivotTolerance', 0.1);"
         "xf = mldivide(df, [1; 2]);"
         "e_fn = norm([4 1; 1 3] * xf - [1; 2]);");
    EXPECT_FALSE(evalBool("df.CheckCondition"));
    EXPECT_LT(getVar("e_fn"), 1e-13);
}

INSTANTIATE_DUAL(DecompositionTest);
