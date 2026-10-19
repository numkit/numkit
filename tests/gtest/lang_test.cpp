#include <gtest/gtest.h>
#include <numkit/builtin/lang.hpp>
#include <numkit/core/standard_library.hpp>
#include <numkit/core/engine.hpp>

using namespace numkit;

class LangTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = makeStandardEngine();
    }

    std::unique_ptr<Engine> engine;
};

TEST_F(LangTest, Eval) {
    Value e = engine->eval("eval('2 + 3');");
    EXPECT_DOUBLE_EQ(e.toScalar(), 5.0);
}

TEST_F(LangTest, KeywordsAndVarNames) {
    Value kw = engine->eval("iskeyword('for');");
    EXPECT_TRUE(kw.toBool());

    Value nkw = engine->eval("iskeyword('my_var');");
    EXPECT_FALSE(nkw.toBool());

    Value vn = engine->eval("isvarname('valid_123');");
    EXPECT_TRUE(vn.toBool());

    Value nvn = engine->eval("isvarname('123_invalid');");
    EXPECT_FALSE(nvn.toBool());
}

TEST_F(LangTest, DirectCppLangAPI) {
    // Keywords
    EXPECT_TRUE(builtin::iskeyword("for"));
    EXPECT_TRUE(builtin::iskeyword("while"));
    EXPECT_TRUE(builtin::iskeyword("function"));
    EXPECT_FALSE(builtin::iskeyword("my_custom_var"));

    const auto &kwList = builtin::keywords();
    EXPECT_GT(kwList.size(), 15u);
    EXPECT_NE(std::find(kwList.begin(), kwList.end(), "for"), kwList.end());

    // iskeyword Span overload returning cell array
    Value allKw = builtin::iskeyword(Span<const Value>{});
    EXPECT_TRUE(allKw.isCell());
    EXPECT_EQ(allKw.numel(), kwList.size());

    // Variable names
    EXPECT_TRUE(builtin::isvarname("valid_var_123"));
    EXPECT_TRUE(builtin::isvarname("A"));
    EXPECT_FALSE(builtin::isvarname("123_invalid"));
    EXPECT_FALSE(builtin::isvarname(""));
    EXPECT_FALSE(builtin::isvarname("invalid-dash"));
    EXPECT_FALSE(builtin::isvarname("for")); // keyword is not a valid var name

    // isvarname Value overload
    EXPECT_TRUE(builtin::isvarname(Value::fromString("validName")).toBool());
    EXPECT_FALSE(builtin::isvarname(Value::scalar(123.0)).toBool());

    // Environment variables
    builtin::setenv("NUMKIT_CPP_API_TEST", "active_42");
    EXPECT_EQ(builtin::getenv("NUMKIT_CPP_API_TEST"), "active_42");

    Value envVal = builtin::getenv(Value::fromString("NUMKIT_CPP_API_TEST"));
    EXPECT_EQ(envVal.toString(), "active_42");

    builtin::setenv("NUMKIT_CPP_API_TEST", "");
    EXPECT_EQ(builtin::getenv("NUMKIT_CPP_API_TEST"), "");
}

TEST_F(LangTest, DualEngineEmptyTransposeAndLogicalFalseAssign) {
    for (Engine::Backend mode : {Engine::Backend::TreeWalker, Engine::Backend::VM}) {
        auto eng = makeStandardEngine();
        eng->setBackend(mode);
        // 1. = []' parser regression
        Value v = eng->eval("x = []'; size(x, 1) + size(x, 2)");
        EXPECT_DOUBLE_EQ(v.toScalar(), 0.0);

        // 2. s(false) = a(false) 0xC0000005 underflow regression
        Value s = eng->eval("a = [10, 20]; s = [1, 2]; idx = (1 > 2); s(idx) = a(idx); s");
        EXPECT_EQ(s.dims().rows(), 1u);
        EXPECT_EQ(s.dims().cols(), 2u);
        EXPECT_DOUBLE_EQ(s.elemAsDouble(0), 1.0);
        EXPECT_DOUBLE_EQ(s.elemAsDouble(1), 2.0);

        // 3. Empty-target 2D colon assignment with scalar and 3D (1x1xN) RHS
        Value uySz = eng->eval("clear uy; for m = 1:4, uy(m, :) = m + 1i; end; size(uy)");
        EXPECT_DOUBLE_EQ(uySz.elemAsDouble(0), 4.0);
        EXPECT_DOUBLE_EQ(uySz.elemAsDouble(1), 1.0);

        Value mseSz = eng->eval(
            "clear mse_ol; e_ol = reshape(1:60, 4, 3, 5); "
            "for m = 1:4, mse_ol(m, :) = mean(e_ol(m, :, :), 2); end; size(mse_ol)");
        EXPECT_DOUBLE_EQ(mseSz.elemAsDouble(0), 4.0);
        EXPECT_DOUBLE_EQ(mseSz.elemAsDouble(1), 5.0);

        // 4. 1D linear indexing of matrix/scalar by row/column vector
        Value rSz = eng->eval("A = reshape(1:12, 3, 4); size(A(1:3))");
        EXPECT_DOUBLE_EQ(rSz.elemAsDouble(0), 1.0);
        EXPECT_DOUBLE_EQ(rSz.elemAsDouble(1), 3.0);

        Value cSz = eng->eval("A = reshape(1:12, 3, 4); size(A([1; 2; 3]))");
        EXPECT_DOUBLE_EQ(cSz.elemAsDouble(0), 3.0);
        EXPECT_DOUBLE_EQ(cSz.elemAsDouble(1), 1.0);

        Value scSz = eng->eval("s = 7; size(s([1; 1; 1; 1]))");
        EXPECT_DOUBLE_EQ(scSz.elemAsDouble(0), 4.0);
        EXPECT_DOUBLE_EQ(scSz.elemAsDouble(1), 1.0);
    }
}

TEST_F(LangTest, TextbookBatch2Builtins) {
    // 1. 2-arg polygamma psi(k, X)
    EXPECT_NEAR(engine->eval("psi(0, 1)").toScalar(), -0.57721566490153286, 1e-10);
    EXPECT_NEAR(engine->eval("psi(1, 1)").toScalar(), 1.6449340668482264, 1e-12);
    EXPECT_NEAR(engine->eval("psi(2, 2)").toScalar(), -0.40411380631918857, 1e-12);

    // 2. spdiags (4-arg and 2-arg)
    Value spd = engine->eval("B = [10 20; 30 40; 50 60]; A = spdiags(B, [-1 1], 3, 3); A");
    EXPECT_DOUBLE_EQ(spd.elemAsDouble(1 + 0 * 3), 10.0); // (2,1)
    EXPECT_DOUBLE_EQ(spd.elemAsDouble(2 + 1 * 3), 30.0); // (3,2)
    EXPECT_DOUBLE_EQ(spd.elemAsDouble(0 + 1 * 3), 40.0); // (1,2)
    EXPECT_DOUBLE_EQ(spd.elemAsDouble(1 + 2 * 3), 60.0); // (2,3)

    // 3. Generalized eig(A, B) and eigs(A, B, k, 'sm')
    Value geigErr = engine->eval(
        "K = [4 -1 0; -1 4 -1; 0 -1 4]; M = [2 0 0; 0 3 0; 0 0 4]; "
        "[V, D] = eig(K, M); norm(K * V - M * V * D)");
    EXPECT_LT(geigErr.toScalar(), 1e-12);

    Value geigsD = engine->eval(
        "K = [4 -1 0; -1 4 -1; 0 -1 4]; M = [2 0 0; 0 3 0; 0 0 4]; "
        "d1 = sort(eig(K, M)); d2 = sort(eigs(K, M, 2, 'sm')); max(abs(d1(1:2) - d2))");
    EXPECT_LT(geigsD.toScalar(), 1e-12);

    // 4. setdiff orientation preservation
    Value sdCol = engine->eval("size(setdiff([1; 2; 3; 4], [2; 4]))");
    EXPECT_DOUBLE_EQ(sdCol.elemAsDouble(0), 2.0);
    EXPECT_DOUBLE_EQ(sdCol.elemAsDouble(1), 1.0);
    Value sdRow = engine->eval("size(setdiff([1 2 3 4], [2; 4]))");
    EXPECT_DOUBLE_EQ(sdRow.elemAsDouble(0), 1.0);
    EXPECT_DOUBLE_EQ(sdRow.elemAsDouble(1), 2.0);

    // 5. dist and latc2tf
    Value dVal = engine->eval("W = [1 2 3; 4 5 6]; P = [1 0; 2 1; 3 2]; dist(W, P)");
    EXPECT_NEAR(dVal.elemAsDouble(0), 0.0, 1e-12);
    EXPECT_NEAR(dVal.elemAsDouble(1), std::sqrt(27.0), 1e-12);
    EXPECT_NEAR(dVal.elemAsDouble(2), std::sqrt(3.0), 1e-12);
    EXPECT_NEAR(dVal.elemAsDouble(3), std::sqrt(48.0), 1e-12);

    Value lVal = engine->eval("[n, d] = latc2tf([0.1; 0.2; 0.3], [1; 2; 3; 4]); [n, d]");
    EXPECT_NEAR(lVal.elemAsDouble(0), 3.0, 1e-12);
    EXPECT_NEAR(lVal.elemAsDouble(1), 3.304, 1e-12);
    EXPECT_NEAR(lVal.elemAsDouble(2), 3.72, 1e-12);
    EXPECT_NEAR(lVal.elemAsDouble(3), 4.0, 1e-12);
    EXPECT_NEAR(lVal.elemAsDouble(4), 1.0, 1e-12);
    EXPECT_NEAR(lVal.elemAsDouble(5), 0.18, 1e-12);
    EXPECT_NEAR(lVal.elemAsDouble(6), 0.236, 1e-12);
    EXPECT_NEAR(lVal.elemAsDouble(7), 0.3, 1e-12);

    // 6. Complex multi-RHS mldivide (nrhs >= 4 fastpath backward substitution)
    Value cSolveRes = engine->eval(
        "R = toeplitz([1, 0.7+0.3i, 0.4+0.5i, 0.1+0.6i]); "
        "A = 10*R + eye(4); X = A \\ R; norm(A * X - R, 'fro')");
    EXPECT_LT(cSolveRes.toScalar(), 1e-12);

    // 7. Complex Hermitian eig returns real ascending eigenvalues and unitary eigenvectors
    Value hEigVals = engine->eval(
        "R = toeplitz([1, 0.7+0.3i, 0.4+0.5i, 0.1+0.6i]); e = eig(R); e");
    EXPECT_FALSE(hEigVals.isComplex());
    EXPECT_LE(hEigVals.elemAsDouble(0), hEigVals.elemAsDouble(1));
    EXPECT_LE(hEigVals.elemAsDouble(1), hEigVals.elemAsDouble(2));
    EXPECT_LE(hEigVals.elemAsDouble(2), hEigVals.elemAsDouble(3));
    Value hEigVD = engine->eval(
        "R = toeplitz([1, 0.7+0.3i, 0.4+0.5i, 0.1+0.6i]); [V, D] = eig(R); "
        "max(norm(R * V - V * D, 'fro'), norm(V' * V - eye(4), 'fro'))");
    EXPECT_LT(hEigVD.toScalar(), 1e-12);
}

TEST_F(LangTest, TextbookBatch3Builtins) {
    for (Engine::Backend mode : {Engine::Backend::TreeWalker, Engine::Backend::VM}) {
        auto eng = makeStandardEngine();
        eng->setBackend(mode);
        // 1. Line-end comment inside matrix literal preserves row separator
        Value matCmt = eng->eval("M = [[1 2] % row 1\n [3 4]]; size(M)");
        EXPECT_DOUBLE_EQ(matCmt.elemAsDouble(0), 2.0);
        EXPECT_DOUBLE_EQ(matCmt.elemAsDouble(1), 2.0);

        // 2. Consecutive statement separators ;,
        Value sepVal = eng->eval("a = 10;, b = 20;, a + b");
        EXPECT_DOUBLE_EQ(sepVal.toScalar(), 30.0);

        // 3. Top-level & in if/while condition short-circuits like &&
        Value scVal = eng->eval(
            "arr = [10]; k = 1; r = 0; "
            "if k > 1 & arr(k - 1) > 0, r = 1; else, r = 2; end; r");
        EXPECT_DOUBLE_EQ(scVal.toScalar(), 2.0);

        // 4. Ignored parameter ~ in anonymous function and function definition
        Value tildeAnon = eng->eval("f = @(~, y) y * 3; f(999, 7)");
        EXPECT_DOUBLE_EQ(tildeAnon.toScalar(), 21.0);

        // 5. Non-scalar and empty operands to colon operator
        Value colNonScalar = eng->eval("v = 1:[4 2]; numel(v)");
        EXPECT_DOUBLE_EQ(colNonScalar.toScalar(), 4.0);
        Value colEmpty = eng->eval("v = 1:[]; isempty(v)");
        EXPECT_TRUE(colEmpty.toBool());

        // 6. 3D slice assignment S(:,:,1) = 0.2 on unset variable
        Value slice3d = eng->eval("clear S3; S3(:,:,1) = [0.2]; S3(:,:,2) = [0.5]; S3(1,1,2)");
        EXPECT_DOUBLE_EQ(slice3d.toScalar(), 0.5);

        // 7. Scoped eval inside function indexes caller register variables
        Value evalFn = eng->eval(
            "function y = test_eval_idx(c1, idx)\n"
            "  eval('y = sum(c1(idx, :));');\n"
            "end\n"
            "test_eval_idx([1 2 3; 4 5 6], 2)");
        EXPECT_DOUBLE_EQ(evalFn.toScalar(), 15.0);
    }

    // 7. Matrix power mpower: negative integer, fractional (-0.5), and complex
    Value mpNeg = engine->eval("A = [2 1; 1 1]; norm(A^(-2) - inv(A*A), 'fro')");
    EXPECT_LT(mpNeg.toScalar(), 1e-12);
    Value mpHalf = engine->eval("A = [4 1; 1 3]; B = A^(-0.5); norm(B * B * A - eye(2), 'fro')");
    EXPECT_LT(mpHalf.toScalar(), 1e-12);

    // 8. inv on singular matrix returns Inf
    Value singInv = engine->eval("B = inv([1 2; 2 4]); all(isinf(B(:)))");
    EXPECT_TRUE(singInv.toBool());

    // 9. repmat on struct
    Value repSt = engine->eval("s.x = 5; r = repmat(s, 2, 3); numel(r)");
    EXPECT_DOUBLE_EQ(repSt.toScalar(), 6.0);

    // 10. gradient with vector coordinates and heaviside
    Value gradVec = engine->eval("x = [0 0.5 1.5 3.0]; f = 2 * x; max(abs(gradient(f, x) - 2))");
    EXPECT_LT(gradVec.toScalar(), 1e-12);
    Value hvVal = engine->eval("heaviside([-2 0 3])");
    EXPECT_DOUBLE_EQ(hvVal.elemAsDouble(0), 0.0);
    EXPECT_DOUBLE_EQ(hvVal.elemAsDouble(1), 0.5);
    EXPECT_DOUBLE_EQ(hvVal.elemAsDouble(2), 1.0);
}


