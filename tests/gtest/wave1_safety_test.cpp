// tests/gtest/wave1_safety_test.cpp
//
// Regression tests for Wave 1 memory safety, concurrency, security, and
// numeric correctness fixes across value, ops, sys, and numkit core.

#include "dual_engine_fixture.hpp"

#include <numkit/ops/blas.hpp>
#include <numkit/ops/deflate.hpp>
#include <numkit/ops/matlab_mt19937.hpp>
#include <numkit/ops/rng.hpp>
#include <numkit/ops/thread_pool.hpp>
#include <numkit/pkg/package_manager.hpp>
#include <numkit/sys/process.hpp>

#include <atomic>
#include <climits>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace m_test;

class Wave1SafetyTest : public DualEngineTest {};

// 1. Value::view COW detach on 1D and 2D indexed mutation
TEST_P(Wave1SafetyTest, ValueViewCowDetach1DAnd2D)
{
    double raw1d[4] = {10.0, 20.0, 30.0, 40.0};
    engine.setVariable("v1", Value::view(raw1d, ValueType::DOUBLE, Dims(1, 4)));
    eval("v1(2) = 999;");
    EXPECT_DOUBLE_EQ(raw1d[0], 10.0);
    EXPECT_DOUBLE_EQ(raw1d[1], 20.0);
    EXPECT_DOUBLE_EQ(raw1d[2], 30.0);
    EXPECT_DOUBLE_EQ(raw1d[3], 40.0);
    EXPECT_DOUBLE_EQ(evalScalar("v1(2)"), 999.0);

    double raw2d[4] = {1.0, 2.0, 3.0, 4.0};
    engine.setVariable("v2", Value::view(raw2d, ValueType::DOUBLE, Dims(2, 2)));
    eval("v2(2, 1) = -77;");
    EXPECT_DOUBLE_EQ(raw2d[0], 1.0);
    EXPECT_DOUBLE_EQ(raw2d[1], 2.0);
    EXPECT_DOUBLE_EQ(raw2d[2], 3.0);
    EXPECT_DOUBLE_EQ(raw2d[3], 4.0);
    EXPECT_DOUBLE_EQ(evalScalar("v2(2, 1)"), -77.0);
}

// 2. appendScalar slack capacity + reshape must copy logical bytes, not capacity
TEST_P(Wave1SafetyTest, AppendScalarSlackCapacityReshape)
{
    eval("x = 1; x(end+1) = 2; y = reshape(x, 2, 1);");
    auto *x = getVarPtr("x");
    auto *y = getVarPtr("y");
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(x->rawBytes(), 2 * sizeof(double));
    EXPECT_EQ(y->rawBytes(), 2 * sizeof(double));
    EXPECT_DOUBLE_EQ((*y)(0), 1.0);
    EXPECT_DOUBLE_EQ((*y)(1), 2.0);
}

// 3. Value::stringElem returns independent std::string values + table sortrows
TEST_P(Wave1SafetyTest, StringElemNoAliasingAndTableSortrows)
{
    Value sa = Value::stringArray(1, 2);
    sa.stringElemSet(0, "first_element");
    sa.stringElemSet(1, "second_element");
    std::string s0 = sa.stringElem(0);
    std::string s1 = sa.stringElem(1);
    EXPECT_EQ(s0, "first_element");
    EXPECT_EQ(s1, "second_element");

    eval("T = table([\"zeta\"; \"alpha\"; \"mu\"], [3; 1; 2], 'VariableNames', {'Name', 'Score'});");
    eval("Ts = sortrows(T, 'Name');");
    EXPECT_DOUBLE_EQ(evalScalar("Ts.Score(1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("Ts.Score(2)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("Ts.Score(3)"), 3.0);
}

// 4. 4D+ horzcat and vertcat preserve higher dimensions
TEST_P(Wave1SafetyTest, HorzcatVertcat4DPreservesShape)
{
    eval("A = ones(2, 3, 2, 2); H = [A, 2*A]; V = [A; 3*A];");
    auto *H = getVarPtr("H");
    auto *V = getVarPtr("V");
    ASSERT_NE(H, nullptr);
    ASSERT_NE(V, nullptr);
    EXPECT_EQ(H->dims().ndim(), 4);
    EXPECT_EQ(H->dims().dim(0), 2u);
    EXPECT_EQ(H->dims().dim(1), 6u);
    EXPECT_EQ(H->dims().dim(2), 2u);
    EXPECT_EQ(H->dims().dim(3), 2u);
    EXPECT_DOUBLE_EQ(evalScalar("H(1, 4, 2, 2)"), 2.0);

    EXPECT_EQ(V->dims().ndim(), 4);
    EXPECT_EQ(V->dims().dim(0), 4u);
    EXPECT_EQ(V->dims().dim(1), 3u);
    EXPECT_EQ(V->dims().dim(2), 2u);
    EXPECT_EQ(V->dims().dim(3), 2u);
    EXPECT_DOUBLE_EQ(evalScalar("V(3, 2, 2, 2)"), 3.0);
}

// 5. VM 2D indexing bounds checks & zero/negative/huge subscript rejection
TEST_P(Wave1SafetyTest, Index2DBoundsAndInvalidSubscriptsRejected)
{
    eval("Z = magic(3);");
    EXPECT_THROW(eval("Z(0, 1) = 5;"), std::exception);
    EXPECT_THROW(eval("Z(1, 0) = 5;"), std::exception);
    EXPECT_THROW(eval("Z(-2, 1) = 5;"), std::exception);
    EXPECT_THROW(eval("Z(1.5, 1) = 5;"), std::exception);
    EXPECT_THROW(eval("Z(NaN, 1) = 5;"), std::exception);
    EXPECT_THROW(eval("q = Z(4, 1);"), std::exception);
    EXPECT_THROW(eval("q = Z(1, 4);"), std::exception);
    EXPECT_THROW(eval("q = Z(0, 1);"), std::exception);
    EXPECT_THROW(eval("Z(1e20) = 1;"), std::exception);
}

// 6. For-loop supports single, integer, logical, and string arrays
TEST_P(Wave1SafetyTest, ForLoopAllValueTypes)
{
    eval("acc1 = single(0); for x = single([2, 3, 5]), acc1 = acc1 + x; end");
    EXPECT_DOUBLE_EQ(evalScalar("double(acc1)"), 10.0);

    eval("acc2 = 0; for col = int32([1, 2, 3; 4, 5, 6]), acc2 = acc2 + double(col(1)) + double(col(2)); end");
    EXPECT_DOUBLE_EQ(evalScalar("acc2"), 21.0);

    eval("cnt = 0; for b = [true, false, true], if b, cnt = cnt + 1; end; end");
    EXPECT_DOUBLE_EQ(evalScalar("cnt"), 2.0);

    eval("joined = \"\"; for s = [\"ab\", \"cd\"], joined = joined + s; end");
    EXPECT_EQ(evalString("char(joined)"), "abcd");
}

// 7. Arrayfun 3D and Cellfun 4D preserve N-D output shapes
TEST_P(Wave1SafetyTest, Arrayfun3DAndCellfun4DPreserveShape)
{
    eval("X = reshape(1:24, 2, 3, 4); Y1 = arrayfun(@(v) v * 2, X); Y2 = arrayfun(@sin, X);");
    auto *Y1 = getVarPtr("Y1");
    auto *Y2 = getVarPtr("Y2");
    ASSERT_NE(Y1, nullptr);
    ASSERT_NE(Y2, nullptr);
    EXPECT_EQ(Y1->dims().ndim(), 3);
    EXPECT_EQ(Y1->dims().dim(0), 2u);
    EXPECT_EQ(Y1->dims().dim(1), 3u);
    EXPECT_EQ(Y1->dims().dim(2), 4u);
    EXPECT_DOUBLE_EQ(evalScalar("Y1(2, 3, 4)"), 48.0);
    EXPECT_EQ(Y2->dims().ndim(), 3);
    EXPECT_EQ(Y2->numel(), 24u);

    eval("C = num2cell(ones(2, 2, 2, 2)); R = cellfun(@(v) v + 4, C);");
    auto *R = getVarPtr("R");
    ASSERT_NE(R, nullptr);
    EXPECT_EQ(R->dims().ndim(), 4);
    EXPECT_EQ(R->numel(), 16u);
    EXPECT_DOUBLE_EQ(evalScalar("R(2, 2, 2, 2)"), 5.0);
}

// 8. Non-global workspace variable is not clobbered by unrelated function global
TEST_P(Wave1SafetyTest, WorkspaceLocalNotClobberedByFunctionGlobal)
{
    eval("function set_glob()\n  global shared_g;\n  shared_g = 777;\nend\nset_glob();");
    eval("shared_g = 42;");
    EXPECT_DOUBLE_EQ(evalScalar("shared_g"), 42.0);
}

INSTANTIATE_DUAL(Wave1SafetyTest);

// 9. BLAS GEMM tail panel (N = 2050 > nc_block = 2048) + Trans / ConjTrans
TEST(Wave1OpsSafetyTest, GemmTailPanelAndTransposes)
{
    // Exercise N = 2050, K = 260 (crosses nc_block = 2048 and kc_block = 256)
    const int M = 4, N = 2050, K = 260;
    std::vector<double> A(static_cast<size_t>(M * K), 1.0);
    std::vector<double> B(static_cast<size_t>(K * N), 2.0);
    std::vector<double> C(static_cast<size_t>(M * N), 0.0);
    numkit::ops::gemm(numkit::ops::MatrixTranspose::NoTrans,
                      numkit::ops::MatrixTranspose::NoTrans,
                      M, N, K, 1.0, A.data(), M, B.data(), K, 0.0, C.data(), M);
    EXPECT_DOUBLE_EQ(C[0], 520.0);
    EXPECT_DOUBLE_EQ(C[static_cast<size_t>(M * N - 1)], 520.0);

    // Real Trans: A^T (2x3 -> 3x2) * B^T (2x2 -> 2x2)
    // A (2x3 col-major): [1 3 5; 2 4 6], so A^T is 3x2: [1 2; 3 4; 5 6]
    double At[6] = {1, 2, 3, 4, 5, 6};
    double Bt[4] = {1, 0, 2, 1}; // [1 2; 0 1], so B^T is [1 0; 2 1]
    double Ct[6] = {0};
    numkit::ops::gemm(numkit::ops::MatrixTranspose::Trans,
                      numkit::ops::MatrixTranspose::Trans,
                      3, 2, 2, 1.0, At, 2, Bt, 2, 0.0, Ct, 3);
    // Row 0 of A^T is [1, 2]; B^T cols are [1;2] and [0;1] -> [5, 2]
    EXPECT_DOUBLE_EQ(Ct[0], 5.0);
    EXPECT_DOUBLE_EQ(Ct[3], 2.0);

    // Complex ConjTrans: A^H * A for A = [1+2i] -> (1-2i)*(1+2i) = 5 + 0i
    std::complex<double> zA[1] = {{1.0, 2.0}};
    std::complex<double> zC[1] = {{0.0, 0.0}};
    numkit::ops::gemm(numkit::ops::MatrixTranspose::ConjTrans,
                      numkit::ops::MatrixTranspose::NoTrans,
                      1, 1, 1, {1.0, 0.0}, zA, 1, zA, 1, {0.0, 0.0}, zC, 1);
    EXPECT_DOUBLE_EQ(zC[0].real(), 5.0);
    EXPECT_DOUBLE_EQ(zC[0].imag(), 0.0);
}

// 10. ThreadPool alternating max_workers stress test
TEST(Wave1OpsSafetyTest, ThreadPoolAlternatingWorkerSubsetRace)
{
    auto &pool = numkit::detail::ThreadPool::global();
    for (int iter = 0; iter < 1500; ++iter) {
        int maxW = (iter % 2 == 0) ? 2 : 16;
        std::atomic<int> sum{0};
        pool.run(32, [&](std::size_t start, std::size_t end) {
            for (std::size_t i = start; i < end; ++i) {
                sum.fetch_add(static_cast<int>(i + 1), std::memory_order_relaxed);
            }
        }, maxW);
        ASSERT_EQ(sum.load(), 32 * 33 / 2);
    }
}

// 11. RNG state bounds & full-64-bit integer span safety
TEST(Wave1OpsSafetyTest, RngCorruptStateAndWideUniformIntSafe)
{
    numkit::ops::MatlabMT19937 mt;
    std::uint32_t allZero[624] = {0};
    mt.setState(allZero, -42); // negative index + all-zero state
    std::uint32_t u = mt.genUint32();
    double r = mt.genRes53();
    (void)u;
    EXPECT_GE(r, 0.0);
    EXPECT_LT(r, 1.0);

    numkit::ops::RngContext ctx;
    Value ri = numkit::ops::randi(ctx, INT64_MIN, INT64_MAX, 2, 4);
    EXPECT_EQ(ri.numel(), 8u);
}

// 12. Malformed MAT5 / MAT4 inputs & Deflate bomb cap
TEST(Wave1SysAndMatSafetyTest, MalformedMatFilesAndDeflateCap)
{
    namespace fs = std::filesystem;
    fs::path tmpDir = fs::temp_directory_path() / "numkit_wave1_mat_test";
    fs::create_directories(tmpDir);

    StandardEngine eng;

    // 12a. MAT5 with 0xFFFFFFFC byteCount (32-bit padding wraparound regression)
    {
        fs::path bad5 = tmpDir / "wrap_tag.mat";
        std::string bytes(128, ' ');
        bytes[126] = 'I';
        bytes[127] = 'M';
        const std::uint8_t tag[8] = {0x0E, 0x00, 0x00, 0x00, 0xFC, 0xFF, 0xFF, 0xFF};
        bytes.append(reinterpret_cast<const char *>(tag), 8);
        {
            std::ofstream out(bad5, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        EXPECT_THROW(eng.eval("load('" + bad5.generic_string() + "');"), std::exception);
    }

    // 12b. MAT4 with non-NUL-terminated variable name
    {
        fs::path bad4 = tmpDir / "unterm_mat4.mat";
        std::vector<std::uint8_t> bytes(24, 0);
        // mType=0, rows=1, cols=1, imagf=0, namlen=4
        bytes[4] = 1;
        bytes[8] = 1;
        bytes[16] = 4;
        bytes[20] = 'a';
        bytes[21] = 'b';
        bytes[22] = 'c';
        bytes[23] = 'd'; // missing NUL!
        {
            std::ofstream out(bad4, std::ios::binary);
            out.write(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        EXPECT_THROW(eng.eval("load('" + bad4.generic_string() + "');"), std::exception);
    }

    // 12c. Deflate output byte cap
    {
        std::vector<std::uint8_t> bigZeros(8192, 0);
        auto compressed = numkit::ops::zlibCompress(bigZeros.data(), bigZeros.size(), 6);
        EXPECT_THROW(numkit::ops::zlibDecompress(compressed.data(), compressed.size(), 0, 1024),
                     std::runtime_error);
    }

    std::error_code ec;
    fs::remove_all(tmpDir, ec);
}

// 13. PackageManager path traversal & process shell metacharacter rejection
TEST(Wave1SysAndMatSafetyTest, PackageManagerTraversalAndCommandInjectionBlocked)
{
    namespace fs = std::filesystem;
    fs::path tmpRoot = fs::temp_directory_path() / "numkit_wave1_pkg_test";
    fs::path modsDir = tmpRoot / "modules";
    fs::path victimDir = tmpRoot / "victim";
    fs::create_directories(modsDir);
    fs::create_directories(victimDir);
    {
        std::ofstream f(victimDir / "keep.txt");
        f << "do not delete";
    }

    numkit::pkg::PackageManager pm(modsDir.string());
    EXPECT_NE(pm.remove("../victim"), 0);
    EXPECT_NE(pm.remove(".."), 0);
    EXPECT_NE(pm.remove("."), 0);
    EXPECT_TRUE(fs::exists(victimDir / "keep.txt"));

    fs::path marker = tmpRoot / "injected_marker";
    EXPECT_FALSE(numkit::sys::isCommandAvailable("sh; touch " + marker.string()));
    EXPECT_FALSE(fs::exists(marker));

    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
}

// 14. Canonical Size/Index conversion boundary parity (VM + TreeWalker)
TEST_P(Wave1SafetyTest, CanonicalIndexAndDimBoundsParity)
{
    eval("a = [10, 20, 30];");
    EXPECT_DOUBLE_EQ(evalScalar("a(1)"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("a(3)"), 30.0);

    // Non-positive, fractional, non-finite, flintmax (2^53), and huge doubles
    // are rejected without UB across both literal and variable index paths.
    for (const char *expr : {
             "a(0);",
             "a(-1);",
             "a(1.5);",
             "a(NaN);",
             "a(Inf);",
             "a(9007199254740992);",
             "a(1e300);",
             "k = 9007199254740992; a(k);",
             "k = 1e300; a(k);",
             "zeros(0, 9007199254740992);",
             "zeros(0, 1e300);",
         }) {
        EXPECT_THROW(eval(expr), std::exception) << expr;
    }

    if constexpr (sizeof(numkit::Size) >= 8) {
        // Largest contiguous exact double integer (2^53 - 1) is accepted as an
        // empty-array dimension without allocating storage.
        EXPECT_DOUBLE_EQ(evalScalar("size(zeros(0, 9007199254740991), 2)"),
                         9007199254740991.0);
    }
}
