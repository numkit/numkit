#include <keel/core.h>
#include <keel/smart_ptr.h>
#include "dual_engine_fixture.hpp"
#include <numkit/sys/vfs.hpp>

#include <chrono>
#include <cmath>
#include <unordered_map>

namespace {

using namespace numkit;

class CountingMemoryFS : public VirtualFS
{
public:
    struct Entry {
        std::string content;
        keel::i64 mtime = 1000;
        keel::i64 mtimeNs = 1000000000000LL;
    };

    explicit CountingMemoryFS(std::string fsName) : name_(std::move(fsName)) {}

    keel::String name() const override { return keel::String::from(name_); }

    void put(const std::string &path, std::string content, keel::i64 mtime, keel::i64 mtimeNs)
    {
        files_[norm(path)] = Entry{std::move(content), mtime, mtimeNs};
    }

    keel::String readFile(keel::StringRef path) override
    {
        const std::string p(path);
        auto it = files_.find(norm(p));
        if (it == files_.end())
            throw std::runtime_error("file not found: " + p);
        ++readCount;
        return keel::String::from(it->second.content);
    }

    void writeFile(keel::StringRef path, keel::StringRef content) override
    {
        auto &e = files_[norm(std::string(path))];
        e.content.assign(content.data(), content.size());
        ++e.mtimeNs;
    }

    bool exists(keel::StringRef path) override
    {
        const std::string p = norm(std::string(path));
        if (files_.count(p))
            return true;
        const std::string prefix = p + "/";
        for (const auto &kv : files_) {
            if (kv.first.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    keel::Array<DirEntry> listDir(keel::StringRef path) override
    {
        const std::string dir = norm(std::string(path));
        const std::string prefix = dir.empty() ? "" : (dir + "/");
        keel::Array<DirEntry> out;
        for (const auto &kv : files_) {
            if (kv.first.rfind(prefix, 0) == 0) {
                std::string rest = kv.first.substr(prefix.size());
                if (rest.find('/') == std::string::npos && !rest.empty())
                    out.push_back(DirEntry{keel::String::from(rest), false});
            }
        }
        return out;
    }

    keel::Optional<FileStat> stat(keel::StringRef path) override
    {
        ++statCount;
        const std::string p = norm(std::string(path));
        auto it = files_.find(p);
        if (it != files_.end()) {
            FileStat st;
            st.size = static_cast<keel::i64>(it->second.content.size());
            st.mtime = it->second.mtime;
            st.mtimeNs = it->second.mtimeNs;
            st.kind = FileStat::Kind::File;
            return st;
        }
        if (exists(p)) {
            FileStat st;
            st.kind = FileStat::Kind::Directory;
            return st;
        }
        return keel::kNullOpt;
    }

    int readCount = 0;
    int statCount = 0;

private:
    static std::string norm(const std::string &p)
    {
        std::string s = p;
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return s;
    }

    std::string name_;
    std::unordered_map<std::string, Entry> files_;
};

class Wave4PerfTest : public m_test::DualEngineTest {};

TEST_P(Wave4PerfTest, NestedStructAndCellInPlaceMutationLinearTimeAndCowSafe)
{
    // 1. O(N) in-place nested mutation on unshared struct field and cell element
    eval(R"(
        function [s1, s2, s3] = run_nested()
            st = struct('arr', zeros(1, 3000));
            for i = 1:3000
                st.arr(i) = i;
            end
            s1 = sum(st.arr);

            c = {zeros(1, 3000)};
            for i = 1:3000
                c{1}(i) = 2 * i;
            end
            s2 = sum(c{1});

            deep = struct('a', struct('b', zeros(1, 1000)));
            for i = 1:1000
                deep.a.b(i) = i;
            end
            s3 = sum(deep.a.b);
        end
        [ans1, ans2, ans3] = run_nested();
    )");
    EXPECT_DOUBLE_EQ(getVar("ans1"), 3000.0 * 3001.0 / 2.0);
    EXPECT_DOUBLE_EQ(getVar("ans2"), 3000.0 * 3001.0);
    EXPECT_DOUBLE_EQ(getVar("ans3"), 1000.0 * 1001.0 / 2.0);

    // 2. COW preservation when struct, leaf array, or cell is aliased
    eval(R"(
        s = struct('arr', [10 20 30]);
        t = s;
        leaf = s.arr;
        s.arr(2) = 99;

        c = {[4 5 6]};
        d = c;
        cleaf = c{1};
        c{1}(3) = 88;
    )");
    EXPECT_DOUBLE_EQ(evalScalar("s.arr(2)"), 99.0);
    EXPECT_DOUBLE_EQ(evalScalar("t.arr(2)"), 20.0);
    EXPECT_DOUBLE_EQ(evalScalar("leaf(2)"), 20.0);
    EXPECT_DOUBLE_EQ(evalScalar("c{1}(3)"), 88.0);
    EXPECT_DOUBLE_EQ(evalScalar("d{1}(3)"), 6.0);
    EXPECT_DOUBLE_EQ(evalScalar("cleaf(3)"), 6.0);
}

TEST_P(Wave4PerfTest, NestedMutationExceptionRollbackLeavesContainerUntouched)
{
    eval(R"(
        function [len1, v1, len2, v2, clen, cv] = test_rollback()
            s = struct('arr', [10 20 30]);
            try
                s.arr(0) = 99;
            catch
            end
            len1 = numel(s.arr);
            v1 = s.arr(1) + s.arr(2) + s.arr(3);

            try
                s.arr(1:6) = [1 2];
            catch
            end
            len2 = numel(s.arr);
            v2 = s.arr(1) + s.arr(2) + s.arr(3);

            c = {[7 8 9]};
            try
                c{1}(1:5) = [1 2];
            catch
            end
            clen = numel(c{1});
            cv = c{1}(1) + c{1}(2) + c{1}(3);
        end
        [len1, v1, len2, v2, clen, cv] = test_rollback();
    )");
    EXPECT_DOUBLE_EQ(getVar("len1"), 3.0);
    EXPECT_DOUBLE_EQ(getVar("v1"), 60.0);
    EXPECT_DOUBLE_EQ(getVar("len2"), 3.0);
    EXPECT_DOUBLE_EQ(getVar("v2"), 60.0);
    EXPECT_DOUBLE_EQ(getVar("clen"), 3.0);
    EXPECT_DOUBLE_EQ(getVar("cv"), 24.0);
}

TEST_P(Wave4PerfTest, DynamicCellAndStructEndPlusOneGrowthIsAmortizedLinear)
{
    eval(R"(
        function [nc, csum, ns, ssum] = test_grow()
            c = {};
            for i = 1:2500
                c{end+1} = i;
            end
            nc = numel(c);
            csum = c{1} + c{2500};

            s = struct('x', {});
            for i = 1:1500
                s(end+1).x = i * 3;
            end
            ns = numel(s);
            ssum = s(1).x + s(1500).x;
        end
        [nc, csum, ns, ssum] = test_grow();
    )");
    EXPECT_DOUBLE_EQ(getVar("nc"), 2500.0);
    EXPECT_DOUBLE_EQ(getVar("csum"), 2501.0);
    EXPECT_DOUBLE_EQ(getVar("ns"), 1500.0);
    EXPECT_DOUBLE_EQ(getVar("ssum"), 4503.0);
}

TEST_P(Wave4PerfTest, CallMultiInlineCacheAndRedefinitionInvalidation)
{
    eval(R"(
        function [a, b] = pair_fn(x)
            a = x + 1;
            b = x * 2;
        end
        function total = driver(n)
            total = 0;
            for k = 1:n
                [u, v] = pair_fn(k);
                [mx, idx] = max([u, v]);
                total = total + mx + idx;
            end
        end
        r1 = driver(1000);
    )");
    // For k=1: u=2, v=2 -> mx=2, idx=1 (3). For k>=2: v=2*k > k+1 -> mx=2*k, idx=2.
    // Sum_{k=1..1000}: k=1 gives 3; k=2..1000 gives sum(2*k + 2) = 2*(1000*1001/2 - 1) + 2*999 = 1002996.
    // Total = 1002999.
    EXPECT_DOUBLE_EQ(getVar("r1"), 1002999.0);

    // Redefine pair_fn and verify inline cache epoch invalidates stale target!
    eval(R"(
        function [a, b] = pair_fn(x)
            a = x + 10;
            b = x + 20;
        end
        r2 = driver(5);
    )");
    // For k=1..5: b = k+20 > k+10 -> mx = k+20, idx = 2 -> sum_{k=1..5}(k + 22) = 15 + 110 = 125.
    EXPECT_DOUBLE_EQ(getVar("r2"), 125.0);
}

TEST_P(Wave4PerfTest, FuseEwiseEvaluatesSideEffectingOperandExactlyOnce)
{
    eval(R"(
        function y = counted_vec(n)
            persistent calls;
            if isempty(calls)
                calls = 0;
            end
            calls = calls + 1;
            if n == 0
                y = calls;
            else
                y = 1:n;
            end
        end
        function [c_small, c_large, diff_small, diff_large] = run_fuse_check()
            % N = 16 declines fusion (< 1024) and runs fallback path
            a = sin(counted_vec(16)) + cos(1:16);
            c_small = counted_vec(0) - 1;
            ref_a = sin(1:16) + cos(1:16);
            diff_small = max(abs(a - ref_a));

            % N = 2048 triggers fusion kernel
            b = sin(counted_vec(2048)) + cos(1:2048);
            c_large = counted_vec(0) - 3;
            ref_b = sin(1:2048) + cos(1:2048);
            diff_large = max(abs(b - ref_b));
        end
        [c_small, c_large, diff_small, diff_large] = run_fuse_check();
    )");
    EXPECT_DOUBLE_EQ(getVar("c_small"), 1.0);
    EXPECT_DOUBLE_EQ(getVar("c_large"), 1.0);
    EXPECT_LT(getVar("diff_small"), 1e-12);
    EXPECT_LT(getVar("diff_large"), 1e-12);
}

TEST_P(Wave4PerfTest, MFileCacheUsesMtimeWithoutReReadingFileOnEveryEval)
{
    auto fs = keel::make_unique<CountingMemoryFS>("memw4");
    CountingMemoryFS *fsPtr = fs.get();
    fsPtr->put("/pkg/triple_val.m",
               "function y = triple_val(x)\n    y = 3 * x;\nend\n",
               /*mtime=*/1700000000,
               /*mtimeNs=*/1700000000111222333LL);
    engine.registerVirtualFS(std::move(fs));
    engine.addPath("memw4:/pkg");

    EXPECT_DOUBLE_EQ(evalScalar("triple_val(7)"), 21.0);
    EXPECT_EQ(fsPtr->readCount, 1);

    // Repeated top-level eval() calls run refreshStaleMFiles() — must NOT re-read file!
    for (int i = 1; i <= 5; ++i) {
        EXPECT_DOUBLE_EQ(evalScalar("triple_val(" + std::to_string(i) + ")"), 3.0 * i);
    }
    EXPECT_EQ(fsPtr->readCount, 1);

    // Sub-second overwrite (same second mtime, updated mtimeNs, same file length!)
    fsPtr->put("/pkg/triple_val.m",
               "function y = triple_val(x)\n    y = 9 * x;\nend\n",
               /*mtime=*/1700000000,
               /*mtimeNs=*/1700000000111222999LL);
    EXPECT_DOUBLE_EQ(evalScalar("triple_val(7)"), 63.0);
    EXPECT_EQ(fsPtr->readCount, 2);
}

TEST_P(Wave4PerfTest, BareScriptInvocationByName)
{
    auto fs = keel::make_unique<CountingMemoryFS>("memscr");
    fs->put("/scripts/seed_vars.m",
            "a = 17;\nb = 25;\n",
            /*mtime=*/1700000000,
            /*mtimeNs=*/1700000000000000000LL);
    engine.registerVirtualFS(std::move(fs));
    engine.addPath("memscr:/scripts");

    // Calling a .m script by bare statement must execute with nargout=0 and
    // populate the caller's workspace (regression for compiler.cpp compileExprStmt).
    eval("seed_vars;\nc = a + b;");
    EXPECT_DOUBLE_EQ(getVar("c"), 42.0);
    // Calling a script as a function with a return target must still throw.
    EXPECT_THROW(eval("x = seed_vars;"), std::exception);
}

TEST_P(Wave4PerfTest, ComplexAndSingleMtimesPlusFastMatrixPower)
{
    // Complex GEMM (80x70 * 70x65 triggers SIMD tiled GemmComplexKernel)
    eval(R"(
        rng(42);
        A = randn(80, 70) + 1i * randn(80, 70);
        B = randn(70, 65) + 1i * randn(70, 65);
        C = A * B;
        % Verify against real/imag decomposition
        C_ref = (real(A)*real(B) - imag(A)*imag(B)) + 1i * (real(A)*imag(B) + imag(A)*real(B));
        err_cplx = max(abs(C(:) - C_ref(:)));

        As = single(randn(64, 48));
        Bs = single(randn(48, 50));
        Cs = As * Bs;
        Cs_ref = single(double(As) * double(Bs));
        err_sng = double(max(abs(Cs(:) - Cs_ref(:))));

        % O(log n) binary matrix exponentiation: 10^9 shear power
        M = [1, 2; 0, 1]^1000000000;
        m12 = M(1, 2);
        Minv = [2, 0; 0, 4]^(-5);
        minv22 = Minv(2, 2);
    )");
    EXPECT_LT(getVar("err_cplx"), 1e-11);
    EXPECT_LT(getVar("err_sng"), 1e-4);
    EXPECT_DOUBLE_EQ(getVar("m12"), 2000000000.0);
    EXPECT_NEAR(getVar("minv22"), std::pow(4.0, -5.0), 1e-15);
}

TEST_P(Wave4PerfTest, QzDecompositionGeneralAndSingularBForNGe4)
{
    eval(R"(
        rng(123);
        A = randn(5, 5);
        B = randn(5, 5);
        [AA, BB, Q, Z] = qz(A, B);
        res_a = norm(Q * A * Z - AA, 'fro');
        res_b = norm(Q * B * Z - BB, 'fro');
        orth_q = norm(Q * Q' - eye(5), 'fro');
        orth_z = norm(Z * Z' - eye(5), 'fro');
        strict_lower_b = norm(tril(BB, -1), 'fro');
        strict_lower2_a = norm(tril(AA, -2), 'fro');

        % Singular B (row/col zero) for n = 4
        As = [2 1 0 3; -1 4 2 1; 0 1 3 -2; 1 -1 2 5];
        Bs = [1 2 0 1; 0 0 0 0; 0 3 1 -1; 0 1 2 4];
        [AAs, BBs, Qs, Zs] = qz(As, Bs);
        sres_a = norm(Qs * As * Zs - AAs, 'fro');
        sres_b = norm(Qs * Bs * Zs - BBs, 'fro');
        slower_b = norm(tril(BBs, -1), 'fro');
    )");
    EXPECT_LT(getVar("res_a"), 1e-9);
    EXPECT_LT(getVar("res_b"), 1e-9);
    EXPECT_LT(getVar("orth_q"), 1e-10);
    EXPECT_LT(getVar("orth_z"), 1e-10);
    EXPECT_LT(getVar("strict_lower_b"), 1e-9);
    EXPECT_LT(getVar("strict_lower2_a"), 1e-9);

    EXPECT_LT(getVar("sres_a"), 1e-9);
    EXPECT_LT(getVar("sres_b"), 1e-9);
    EXPECT_LT(getVar("slower_b"), 1e-9);
}

TEST_P(Wave4PerfTest, EigRepeatedEigenvaluesGeneralizedSingularBAndPolyAndCholupdate)
{
    eval(R"(
        % Non-symmetric matrix with repeated diagonalizable eigenvalue 5 (multiplicity 2)
        S = [1 2 0 1; 0 3 1 -1; 1 0 2 2; -1 1 0 3];
        A = S * diag([5, 5, -2, 3]) / S;
        [V, D] = eig(A);
        eig_res = norm(A * V - V * D, 'fro');
        eig_rank = rank(V, 1e-7);

        % Generalized eig(A, B) with singular B
        Ag = [4 1 0; 1 3 1; 0 1 2];
        Bg = [2 0 0; 0 0 0; 0 0 4];
        [Vg, Dg] = eig(Ag, Bg);
        % Finite generalized eigenvalues must satisfy Ag*v = lambda*Bg*v
        gen_err = 0;
        finite_cnt = 0;
        for j = 1:3
            lam = Dg(j, j);
            if isfinite(lam)
                finite_cnt = finite_cnt + 1;
                gen_err = max(gen_err, norm(Ag * Vg(:, j) - lam * Bg * Vg(:, j)));
            end
        end

        % O(n^3) characteristic polynomial via eigenvalues
        p = poly(diag(1:10));
        p0 = p(1);
        p1 = p(2);
        p10 = p(11);

        % O(n^2) Givens cholupdate ('+' and '-')
        M = [9 3 1 2; 3 8 2 1; 1 2 7 2; 2 1 2 6];
        R = chol(M);
        x = [1.0; -0.5; 0.8; 0.3];
        Rp = cholupdate(R, x, '+');
        Rm = cholupdate(R, x, '-');
        chol_up_err = norm(Rp' * Rp - (M + x * x'), 'fro');
        chol_dn_err = norm(Rm' * Rm - (M - x * x'), 'fro');
        chol_tri_err = norm(tril(Rm, -1), 'fro');

        chol_threw = 0;
        try
            cholupdate(R, [100; 0; 0; 0], '-');
        catch
            chol_threw = 1;
        end
    )");
    EXPECT_LT(getVar("eig_res"), 1e-10);
    EXPECT_DOUBLE_EQ(getVar("eig_rank"), 4.0);
    EXPECT_DOUBLE_EQ(getVar("finite_cnt"), 2.0);
    EXPECT_LT(getVar("gen_err"), 1e-10);

    EXPECT_NEAR(getVar("p0"), 1.0, 1e-12);
    EXPECT_NEAR(getVar("p1"), -55.0, 1e-9);
    EXPECT_NEAR(getVar("p10"), 3628800.0, 1e-4);

    EXPECT_LT(getVar("chol_up_err"), 1e-11);
    EXPECT_LT(getVar("chol_dn_err"), 1e-11);
    EXPECT_DOUBLE_EQ(getVar("chol_tri_err"), 0.0);
    EXPECT_DOUBLE_EQ(getVar("chol_threw"), 1.0);
}

TEST_P(Wave4PerfTest, CoreEngineDebloatedOfAlienCommAndDspSystemObjects)
{
    EXPECT_EQ(engine.findClass("comm.OFDMModulator"), nullptr);
    EXPECT_EQ(engine.findClass("comm.QPSKModulator"), nullptr);
    EXPECT_EQ(engine.findClass("comm.SymbolSynchronizer"), nullptr);
    EXPECT_EQ(engine.findClass("dsp.FIRInterpolator"), nullptr);
    EXPECT_EQ(engine.findClass("dsp.IIRFilter"), nullptr);
    EXPECT_FALSE(engine.hasFunction("eyediagram"));
    EXPECT_FALSE(engine.hasFunction("scatterplot"));
    EXPECT_FALSE(engine.hasFunction("iqimbal"));
}

INSTANTIATE_DUAL(Wave4PerfTest);

} // namespace
