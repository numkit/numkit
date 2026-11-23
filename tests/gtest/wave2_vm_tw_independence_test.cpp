// tests/gtest/wave2_vm_tw_independence_test.cpp
//
// Differential & stress regression tests for Wave 2:
//   - 16-byte wide Instruction format (uint16_t regs, int32_t pool/jump offsets)
//   - Dynamic VM register stack growth (regBase indexing, callback RegDestContinuation)
//   - Complete independence of Backend::VM and Backend::TreeWalker
//   - Semantic parity across && / ||, NaN logical conversion, empty conditionals,
//     bare 'end', d(end).field, break/continue inside try/catch, typed colon
//     for-loops, multi-output classdef methods, and mid-session backend switching.

#include "dual_engine_fixture.hpp"

#include <numkit/core/bytecode.hpp>
#include <numkit/core/compiler.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace m_test;

static_assert(sizeof(numkit::Instruction) == 16, "Instruction must be 16 bytes");
static_assert(numkit::kInvalidReg == 0xFFFF, "kInvalidReg must be 0xFFFF");

class Wave2VmTwIndependenceTest : public DualEngineTest {};

// 1. >300 local variables/registers in both script and user function
TEST_P(Wave2VmTwIndependenceTest, Over350LocalRegistersNativeExecution)
{
    constexpr int kVars = 360;
    std::ostringstream script;
    for (int i = 1; i <= kVars; ++i)
        script << "v" << i << " = " << i << ";\n";
    script << "total = v1";
    for (int i = 2; i <= kVars; ++i)
        script << " + v" << i;
    script << ";\n";

    eval(script.str());
    EXPECT_DOUBLE_EQ(getVar("total"), static_cast<double>(kVars * (kVars + 1) / 2));

    std::ostringstream fn;
    fn << "function out = big_locals_fn(seed)\n";
    for (int i = 1; i <= kVars; ++i)
        fn << "  loc" << i << " = seed + " << i << ";\n";
    fn << "  out = loc1";
    for (int i = 2; i <= kVars; ++i)
        fn << " + loc" << i;
    fn << ";\nend\n";
    eval(fn.str());

    if (GetParam() == BackendParam::VM) {
        ASSERT_NE(engine.compilerPtr(), nullptr);
        const BytecodeChunk *chunk = engine.compilerPtr()->findCompiled("big_locals_fn");
        ASSERT_NE(chunk, nullptr);
        EXPECT_GT(chunk->numRegisters, 350u);
    }

    const double expected = static_cast<double>(kVars) * 10.0
                            + static_cast<double>(kVars * (kVars + 1) / 2);
    EXPECT_DOUBLE_EQ(evalScalar("big_locals_fn(10)"), expected);
}

// 2. >260 parameters and >70 return values (exceeding old uint8_t & 64-slot returnBuf_)
TEST_P(Wave2VmTwIndependenceTest, Over260ParamsAndOver70ReturnValues)
{
    constexpr int kParams = 270;
    constexpr int kOuts = 75;

    std::ostringstream fn;
    fn << "function [";
    for (int i = 1; i <= kOuts; ++i) {
        if (i > 1) fn << ", ";
        fn << "r" << i;
    }
    fn << "] = wide_sig(";
    for (int i = 1; i <= kParams; ++i) {
        if (i > 1) fn << ", ";
        fn << "p" << i;
    }
    fn << ")\n";
    fn << "  s = p1 + p" << kParams << ";\n";
    for (int i = 1; i <= kOuts; ++i)
        fn << "  r" << i << " = s + " << i << ";\n";
    fn << "end\n";
    eval(fn.str());

    std::ostringstream call;
    call << "[";
    for (int i = 1; i <= kOuts; ++i) {
        if (i > 1) call << ", ";
        call << "o" << i;
    }
    call << "] = wide_sig(";
    for (int i = 1; i <= kParams; ++i) {
        if (i > 1) call << ", ";
        call << i;
    }
    call << ");";
    eval(call.str());

    // s = 1 + 270 = 271
    EXPECT_DOUBLE_EQ(getVar("o1"), 272.0);
    EXPECT_DOUBLE_EQ(getVar("o40"), 311.0);
    EXPECT_DOUBLE_EQ(getVar("o75"), 346.0);
}

// 3. Deep left-associative binary & short-circuit chains (O(1) C++ stack in TW, >255 temps avoided)
TEST_P(Wave2VmTwIndependenceTest, DeepLeftAssociativeBinaryAndShortCircuitChains)
{
    constexpr int kTerms = 1500;
    std::ostringstream sumExpr;
    sumExpr << "1";
    for (int i = 1; i < kTerms; ++i)
        sumExpr << " + 1";
    EXPECT_DOUBLE_EQ(evalScalar(sumExpr.str()), static_cast<double>(kTerms));

    std::ostringstream andExpr;
    andExpr << "true";
    for (int i = 1; i < 1200; ++i)
        andExpr << " && true";
    andExpr << " && false";
    EXPECT_FALSE(evalBool(andExpr.str()));

    std::ostringstream orExpr;
    orExpr << "false";
    for (int i = 1; i < 1200; ++i)
        orExpr << " || false";
    orExpr << " || true";
    EXPECT_TRUE(evalBool(orExpr.str()));
}

// 4. Short-circuit && / || scalar validation, NaN logical errors, and empty conditionals
TEST_P(Wave2VmTwIndependenceTest, ShortCircuitNaNAndConditionalSemanticsParity)
{
    // Non-scalar and empty arrays must error in && and ||
    EXPECT_THROW(eval("x = [1, 2] && 1;"), std::exception);
    EXPECT_THROW(eval("x = 0 || [1, 2];"), std::exception);
    EXPECT_THROW(eval("x = [] && 1;"), std::exception);
    EXPECT_THROW(eval("x = [] || 0;"), std::exception);

    // Short-circuit skips RHS evaluation when LHS determines outcome
    eval("cnt = 0; function y = bump(); global g_cnt; g_cnt = g_cnt + 1; y = true; end");
    eval("global g_cnt; g_cnt = 0; a = false && bump(); b = true || bump();");
    EXPECT_FALSE(evalBool("a"));
    EXPECT_TRUE(evalBool("b"));
    EXPECT_DOUBLE_EQ(evalScalar("g_cnt"), 0.0);

    // NaN in logical contexts (double & single) must throw
    EXPECT_THROW(eval("x = NaN && true;"), std::exception);
    EXPECT_THROW(eval("x = false || NaN;"), std::exception);
    EXPECT_THROW(eval("x = single(NaN) && true;"), std::exception);
    EXPECT_THROW(eval("if NaN; y = 1; end"), std::exception);
    EXPECT_THROW(eval("if single(NaN); y = 1; end"), std::exception);
    EXPECT_THROW(eval("y = ~NaN;"), std::exception);
    EXPECT_THROW(eval("y = ~single(NaN);"), std::exception);
    EXPECT_THROW(eval("y = single(NaN) & true;"), std::exception);
    EXPECT_THROW(eval("y = [1, single(NaN)] | [0, 1];"), std::exception);

    // Empty arrays in if / while evaluate to false
    eval("if []; branch = 1; else; branch = 2; end");
    EXPECT_DOUBLE_EQ(getVar("branch"), 2.0);
    eval("witers = 0; while []; witers = witers + 1; end");
    EXPECT_DOUBLE_EQ(getVar("witers"), 0.0);
}

// 5. Bare 'end' outside indexing and struct-array d(end).field assignment parity
TEST_P(Wave2VmTwIndependenceTest, BareEndErrorAndStructArrayEndFieldAssign)
{
    EXPECT_THROW(eval("x = end;"), std::exception);
    eval("function y = fn_with_bare_end()\n  y = end;\nend");
    EXPECT_THROW(eval("fn_with_bare_end()"), std::exception);

    eval("d(1).val = 10; d(2).val = 20; d(3).val = 30; d(end).val = 99;");
    EXPECT_DOUBLE_EQ(evalScalar("d(1).val"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("d(2).val"), 20.0);
    EXPECT_DOUBLE_EQ(evalScalar("d(3).val"), 99.0);
}

// 6. break / continue inside try/catch must pop tryStack_ cleanly
TEST_P(Wave2VmTwIndependenceTest, BreakAndContinueInsideTryDoNotLeakTryHandler)
{
    eval(R"(
        function out = test_break_in_try()
            acc = 0;
            for k = 1:5
                try
                    if k == 3
                        break;
                    end
                    acc = acc + k;
                catch
                    acc = -999;
                end
            end
            error('uncaught_after_break');
            out = acc;
        end
    )");
    // If TRY_END was skipped on break, the stale catch block inside the loop
    // would swallow error('uncaught_after_break') instead of propagating!
    EXPECT_THROW(eval("test_break_in_try()"), std::exception);

    eval(R"(
        function out = test_continue_in_try()
            acc = 0;
            for k = 1:4
                try
                    if mod(k, 2) == 0
                        continue;
                    end
                    acc = acc + k;
                catch
                    acc = -999;
                end
            end
            error('uncaught_after_continue');
            out = acc;
        end
    )");
    EXPECT_THROW(eval("test_continue_in_try()"), std::exception);

    // And without trailing error, verify exact loop result
    eval(R"(
        s = 0;
        for k = 1:5
            try
                if k == 4
                    break;
                end
                if k == 2
                    continue;
                end
                s = s + k;
            catch
                s = -100;
            end
        end
    )");
    EXPECT_DOUBLE_EQ(getVar("s"), 4.0); // 1 + 3
}

// 7. Typed colon for-loops ('a':'d', int32(1):int32(3), invalid mixed integer types)
TEST_P(Wave2VmTwIndependenceTest, TypedColonForLoopParity)
{
    eval("acc = ''; lastIsChar = false; for c = 'a':'d'; acc = [acc, c]; lastIsChar = ischar(c); end");
    EXPECT_EQ(evalString("acc"), "abcd");
    EXPECT_TRUE(evalBool("lastIsChar"));

    eval("cls = ''; total = int32(0); for k = int32(2):int32(2):int32(6); cls = class(k); total = total + k; end");
    EXPECT_EQ(evalString("cls"), "int32");
    EXPECT_DOUBLE_EQ(evalScalar("double(total)"), 12.0);

    EXPECT_THROW(eval("for k = int8(1):uint8(3); end"), std::exception);
}

// 8. classdef property defaults, >16 outputs on method call, and bare superclass ref error
TEST_P(Wave2VmTwIndependenceTest, ClassdefMultiOutPropertyDefaultAndBareSuperError)
{
    eval(R"(
        classdef BaseW2
            properties
                Scale = 3 * 7
            end
            methods
                function obj = BaseW2(v)
                    if nargin > 0
                        obj.Scale = v;
                    end
                end
            end
        end
    )");

    eval(R"(
        classdef SubW2 < BaseW2
            methods
                function obj = SubW2(v)
                    obj@BaseW2(v);
                end
                function badSuper(obj)
                    x = obj@BaseW2;
                end
                function [o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15,o16,o17,o18,o19,o20] = twenty(obj)
                    o1 = obj.Scale + 1;  o2 = obj.Scale + 2;  o3 = obj.Scale + 3;  o4 = obj.Scale + 4;
                    o5 = obj.Scale + 5;  o6 = obj.Scale + 6;  o7 = obj.Scale + 7;  o8 = obj.Scale + 8;
                    o9 = obj.Scale + 9;  o10 = obj.Scale + 10; o11 = obj.Scale + 11; o12 = obj.Scale + 12;
                    o13 = obj.Scale + 13; o14 = obj.Scale + 14; o15 = obj.Scale + 15; o16 = obj.Scale + 16;
                    o17 = obj.Scale + 17; o18 = obj.Scale + 18; o19 = obj.Scale + 19; o20 = obj.Scale + 20;
                end
            end
        end
    )");

    EXPECT_DOUBLE_EQ(evalScalar("b0 = BaseW2(); b0.Scale"), 21.0);
    eval("sub = SubW2(100);");
    eval("[a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20] = sub.twenty();");
    EXPECT_DOUBLE_EQ(getVar("a1"), 101.0);
    EXPECT_DOUBLE_EQ(getVar("a16"), 116.0);
    EXPECT_DOUBLE_EQ(getVar("a20"), 120.0);

    eval("caught = false; try; sub.badSuper(); catch; caught = true; end");
    EXPECT_TRUE(evalBool("caught"));

    // Multi-output anon function forwarding on both backends
    eval("[m1, m2] = (@(x) deal(x + 5, x - 5))(20);");
    EXPECT_DOUBLE_EQ(getVar("m1"), 25.0);
    EXPECT_DOUBLE_EQ(getVar("m2"), 15.0);
}

// 9. Mid-session backend switching & dynamic regStack_ growth inside cellfun callback
TEST(Wave2BackendSwitchAndRegGrowthTest, SwitchBackendsAndGrowRegStackMidCallback)
{
    StandardEngine eng;

    // Define function & closure in TreeWalker, then switch to VM and execute
    eng.setBackend(Engine::Backend::TreeWalker);
    eng.eval("function y = tw_defined(x); y = x * 3 + 1; end");
    eng.eval("cap = 50; fh_tw = @(z) tw_defined(z) + cap;");
    EXPECT_DOUBLE_EQ(eng.eval("fh_tw(4)").toScalar(), 63.0);

    eng.setBackend(Engine::Backend::VM);
    EXPECT_DOUBLE_EQ(eng.eval("tw_defined(10)").toScalar(), 31.0);
    EXPECT_DOUBLE_EQ(eng.eval("fh_tw(4)").toScalar(), 63.0);
    EXPECT_DOUBLE_EQ(eng.eval("sum(cellfun(fh_tw, {1, 2, 3}))").toScalar(),
                     (4.0 + 50.0) + (7.0 + 50.0) + (10.0 + 50.0));

    // Redefine tw_defined in TreeWalker and verify VM picks up the new definition
    eng.setBackend(Engine::Backend::TreeWalker);
    eng.eval("function y = tw_defined(x); y = x * 10; end");
    eng.setBackend(Engine::Backend::VM);
    EXPECT_DOUBLE_EQ(eng.eval("tw_defined(7)").toScalar(), 70.0);

    // Force dynamic regStack_ reallocation inside a callback (cellfun -> recursive 320-reg function)
    std::ostringstream bigRec;
    bigRec << "function out = rec_wide(n)\n";
    for (int i = 1; i <= 320; ++i)
        bigRec << "  r" << i << " = n + " << i << ";\n";
    bigRec << "  if n > 1\n"
           << "    out = r1 + r320 + rec_wide(n - 1);\n"
           << "  else\n"
           << "    out = r1 + r320;\n"
           << "  end\n"
           << "end\n";
    eng.eval(bigRec.str());

    // Initial regStack_ is 65536 slots; depth 210 * ~325 regs > 68,000 slots -> grows mid-cellfun!
    eng.setMaxRecursionDepth(500);
    double r = eng.eval("res = cellfun(@rec_wide, {220, 5}); res(1) + res(2)").toScalar();
    // For each k from 1..N, r1 + r320 = (k + 1) + (k + 320) = 2*k + 321.
    // Sum_{k=1..N} (2*k + 321) = N*(N + 1) + 321*N.
    auto expectedFor = [](int n) -> double {
        return static_cast<double>(n) * static_cast<double>(n + 1) + 321.0 * static_cast<double>(n);
    };
    EXPECT_DOUBLE_EQ(r, expectedFor(220) + expectedFor(5));
}

// ── Stage 1–5 Architecture Invariant Parity Suite (TW + VM) ──────────────

// 9. Pointer-stable symbol tables under mid-call HashMap rehash & clear functions
TEST_P(Wave2VmTwIndependenceTest, SymbolTableRehashMidCallSafety)
{
    eval(R"(
        function out = rehash_driver(n)
            acc = 0;
            for k = 1:n
                f = @(x) x + k;
                acc = acc + f(1);
            end
            % Trigger clear functions while rehash_driver itself is active on the stack.
            clear functions;
            out = acc + 100;
        end
    )");
    // Sum_{k=1..40} (1 + k) = 40 + 820 = 860, plus 100 = 960
    EXPECT_DOUBLE_EQ(evalScalar("rehash_driver(40)"), 960.0);
    EXPECT_EQ(engine.callerDepth(), 0);
}

// 10. End-less subfunctions, file-private isolation, handle escape, rehash eviction,
//     and m-file lazy load inside a script with local functions (scriptDepth_ > 0)
TEST_P(Wave2VmTwIndependenceTest, EndlessSubfunctionsFilePrivateAndEviction)
{
    namespace fs = std::filesystem;
    const auto tag = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tmpDir = fs::temp_directory_path() / ("numkit_s6_mfiles_" + tag);
    fs::create_directories(tmpDir);
    struct DirCleanup {
        fs::path p;
        ~DirCleanup() {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
    } cleanup{tmpDir};

    auto writeFile = [&](const std::string &name, const std::string &body) {
        std::ofstream ofs(tmpDir / name, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(ofs.good());
        ofs << body;
    };

    // End-less main + two end-less subfunctions; variable `t` must not be clobbered.
    writeFile("alpha_s6.m", R"(function [val, fh] = alpha_s6(x)
t = x * 2;
val = helper(t) + extra_step(x);
fh = @helper;

function y = helper(a)
y = a + 100;

function z = extra_step(b)
z = b * 3;
)");

    // Second m-file with a same-named private subfunction `helper` returning +200.
    writeFile("beta_s6.m", R"(function val = beta_s6(x)
val = helper(x);

function y = helper(a)
y = a + 200;
)");

    eval("addpath('" + tmpDir.string() + "');");

    // Both files resolve their own private `helper` without cross-contamination.
    eval("[va, h_alpha] = alpha_s6(5);");
    EXPECT_DOUBLE_EQ(getVar("va"), (5.0 * 2.0 + 100.0) + (5.0 * 3.0)); // 125
    EXPECT_DOUBLE_EQ(evalScalar("beta_s6(5)"), 205.0);

    // File-private `helper` must NOT be callable directly from top-level.
    auto bareHelper = engine.evalSafe("helper(5)");
    EXPECT_FALSE(bareHelper.ok);

    // Returned function handle @helper still invokes alpha_s6's private helper.
    EXPECT_DOUBLE_EQ(evalScalar("h_alpha(7)"), 107.0);

    // Update alpha_s6.m on disk and rehash -> evicts both main and `alpha_s6>helper`.
    writeFile("alpha_s6.m", R"(function [val, fh] = alpha_s6(x)
t = x * 2;
val = helper(t) + extra_step(x);
fh = @helper;

function y = helper(a)
y = a + 1000;

function z = extra_step(b)
z = b * 3;
)");
    engine.rehashMFiles();
    eval("[va2, h_alpha2] = alpha_s6(5);");
    EXPECT_DOUBLE_EQ(getVar("va2"), (5.0 * 2.0 + 1000.0) + (5.0 * 3.0)); // 1025
    EXPECT_DOUBLE_EQ(evalScalar("h_alpha2(7)"), 1007.0);

    // Lazy-loading an m-file inside a script that also defines local functions
    // (scriptDepth_ > 0) must register the m-file globally so it survives script exit.
    writeFile("gamma_s6.m", R"(function y = gamma_s6(x)
y = x + 77;
)");
    eval(R"(
        r_script = local_caller(3);
        function y = local_caller(a)
            y = gamma_s6(a);
        end
    )");
    EXPECT_DOUBLE_EQ(getVar("r_script"), 80.0);
    EXPECT_DOUBLE_EQ(evalScalar("gamma_s6(10)"), 87.0);
}

// 11. mayInjectVars_ zero-arg builtins and strict function scope isolation
TEST_P(Wave2VmTwIndependenceTest, MayInjectVarsAndScopeIsolationParity)
{
    // Zero-arg builtin inside a function containing eval (mayInjectVars_ = true)
    eval(R"(
        function n = test_may_inject()
            eval('z_dyn = 42;');
            h1 = figure;
            h2 = figure;
            n = z_dyn + double(h2.Number > h1.Number);
            close all;
        end
    )");
    EXPECT_DOUBLE_EQ(evalScalar("test_may_inject()"), 43.0);

    // Script struct `s.g` must NEVER leak into a function scope.
    eval(R"(
        s.g = 99;
        function y = isolated_fn()
            y = s.g;
        end
    )");
    auto leaked = engine.evalSafe("isolated_fn()");
    EXPECT_FALSE(leaked.ok);
}

// 12. Unified call stack: MException.stack and EvalResult.errorStack parity
TEST_P(Wave2VmTwIndependenceTest, MExceptionStackAndEvalSafeTracebackParity)
{
    eval(R"(
        function inner_err()
            error('s6:test:boom', 'something broke');
        end
        function mid_err()
            inner_err();
        end
        function outer_err()
            mid_err();
        end
    )");

    // Caught via try/catch -> e.stack has innermost frame first
    eval(R"(
        try
            outer_err();
        catch e
            caught_id = e.identifier;
            caught_msg = e.message;
            stack_len = numel(e.stack);
            f1 = e.stack(1).name;
            f2 = e.stack(2).name;
            f3 = e.stack(3).name;
            l1 = e.stack(1).line;
            l2 = e.stack(2).line;
            l3 = e.stack(3).line;
        end
    )");
    EXPECT_EQ(evalString("caught_id"), "s6:test:boom");
    EXPECT_EQ(evalString("caught_msg"), "something broke");
    EXPECT_GE(getVar("stack_len"), 3.0);
    EXPECT_EQ(evalString("f1"), "inner_err");
    EXPECT_EQ(evalString("f2"), "mid_err");
    EXPECT_EQ(evalString("f3"), "outer_err");
    EXPECT_GT(getVar("l1"), 0.0);
    EXPECT_GT(getVar("l2"), 0.0);
    EXPECT_GT(getVar("l3"), 0.0);

    // Uncaught via evalSafe -> EvalResult::errorStack matches
    auto res = engine.evalSafe("outer_err();");
    ASSERT_FALSE(res.ok);
    EXPECT_EQ(std::string(res.errorId.as_ref()), "s6:test:boom");
    EXPECT_EQ(std::string(res.errorMessage.as_ref()), "something broke");
    ASSERT_GE(res.errorStack.size(), 3u);
    EXPECT_EQ(std::string(res.errorStack[0].name.as_ref()), "inner_err");
    EXPECT_EQ(std::string(res.errorStack[1].name.as_ref()), "mid_err");
    EXPECT_EQ(std::string(res.errorStack[2].name.as_ref()), "outer_err");
    EXPECT_GT(res.errorStack[0].line, 0);

    // rethrow(e) preserves the original stack from inner_err -> mid_err -> outer_err
    auto rethrown = engine.evalSafe(R"(
        try
            outer_err();
        catch e
            rethrow(e);
        end
    )");
    ASSERT_FALSE(rethrown.ok);
    EXPECT_EQ(std::string(rethrown.errorId.as_ref()), "s6:test:boom");
    ASSERT_GE(rethrown.errorStack.size(), 3u);
    EXPECT_EQ(std::string(rethrown.errorStack[0].name.as_ref()), "inner_err");
    EXPECT_EQ(std::string(rethrown.errorStack[1].name.as_ref()), "mid_err");
    EXPECT_EQ(std::string(rethrown.errorStack[2].name.as_ref()), "outer_err");

    // Constructed MException carries an empty stack field
    eval("me = MException('s6:manual', 'manual msg');");
    EXPECT_TRUE(evalBool("isfield(me, 'stack') && isempty(me.stack)"));
    EXPECT_EQ(engine.callerDepth(), 0);
}

// 13. Cooperative cancellation (while + try/catch shield + for loop + clean recovery)
TEST_P(Wave2VmTwIndependenceTest, CooperativeCancellationParity)
{
    // Interrupt an infinite while loop that tries to swallow errors with try/catch
    std::thread canceller1([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        engine.requestCancel();
    });
    auto r1 = engine.evalSafe(R"(
        x = 0;
        while true
            try
                x = x + 1;
            catch
            end
        end
    )");
    canceller1.join();

    EXPECT_FALSE(r1.ok);
    EXPECT_EQ(std::string(r1.errorId.as_ref()), "numkit:interrupted");
    EXPECT_EQ(engine.callerDepth(), 0);

    // After clearCancel(), engine executes normally again
    engine.clearCancel();
    EXPECT_DOUBLE_EQ(evalScalar("1 + 2"), 3.0);

    // Interrupt a huge 1:1e9 for-loop cleanly in O(1) memory on BOTH TW and VM
    std::thread canceller2([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        engine.requestCancel();
    });
    auto r2 = engine.evalSafe(R"(
        s = 0;
        for k = 1:1e9
            s = s + 1;
        end
    )");
    canceller2.join();

    EXPECT_FALSE(r2.ok);
    EXPECT_EQ(std::string(r2.errorId.as_ref()), "numkit:interrupted");
    EXPECT_EQ(engine.callerDepth(), 0);

    engine.clearCancel();
    EXPECT_DOUBLE_EQ(evalScalar("6 * 7"), 42.0);
}

// 14. Unified maxRecursionDepth and RAII re-entrancy recovery across eval/callbacks
TEST_P(Wave2VmTwIndependenceTest, UnifiedMaxRecursionAndReentrantRecovery)
{
    engine.setMaxRecursionDepth(15);
    EXPECT_EQ(engine.maxRecursionDepth(), 15);

    eval(R"(
        function y = rec_sum(n)
            if n <= 1
                y = 1;
            else
                y = n + rec_sum(n - 1);
            end
        end
    )");

    // Depth 14 <= 15 succeeds identically on TW and VM
    EXPECT_DOUBLE_EQ(evalScalar("rec_sum(14)"), 105.0);

    // Depth 20 > 15 triggers numkit:recursionLimit identically on TW and VM
    auto ovf = engine.evalSafe("rec_sum(20)");
    EXPECT_FALSE(ovf.ok);
    EXPECT_EQ(std::string(ovf.errorId.as_ref()), "numkit:recursionLimit");
    EXPECT_EQ(engine.callerDepth(), 0);

    // Re-entrant callback/eval error must leave engine state spotless
    eval(R"(
        function y = boom_cb(x)
            if x > 2
                error('s6:cb:fail', 'cb failed');
            end
            y = x * 10;
        end
    )");
    auto cbErr = engine.evalSafe("cellfun(@boom_cb, {1, 2, 3})");
    EXPECT_FALSE(cbErr.ok);
    EXPECT_EQ(std::string(cbErr.errorId.as_ref()), "s6:cb:fail");
    EXPECT_EQ(engine.callerDepth(), 0);

    auto evalErr = engine.evalSafe("eval('boom_cb(5)')");
    EXPECT_FALSE(evalErr.ok);
    EXPECT_EQ(std::string(evalErr.errorId.as_ref()), "s6:cb:fail");
    EXPECT_EQ(engine.callerDepth(), 0);

    // Subsequent calls work cleanly
    EXPECT_DOUBLE_EQ(evalScalar("rec_sum(10)"), 55.0);
}

// 15. dbstack() and lasterror() parity across TW and VM
TEST_P(Wave2VmTwIndependenceTest, DbstackAndLasterrorParity)
{
    eval(R"(
        function [st, st1, idx] = leaf_stack()
            [st, idx] = dbstack;
            st1 = dbstack(1);
        end
        function [st, st1, idx] = mid_stack()
            [st, st1, idx] = leaf_stack();
        end
    )");
    eval("[st, st1, idx] = mid_stack();");
    EXPECT_DOUBLE_EQ(getVar("idx"), 1.0);
    EXPECT_GE(evalScalar("numel(st)"), 2.0);
    EXPECT_EQ(evalString("st(1).name"), "leaf_stack");
    EXPECT_EQ(evalString("st(2).name"), "mid_stack");
    EXPECT_GT(evalScalar("st(1).line"), 0.0);
    // dbstack(1) omits innermost frame `leaf_stack`
    EXPECT_EQ(evalString("st1(1).name"), "mid_stack");

    // lasterror records caught errors and resets cleanly via lasterror('reset')
    eval(R"(
        lasterror('reset');
        le0 = lasterror;
        try
            error('s6:lasterr:tag', 'captured by lasterror');
        catch
        end
        le1 = lasterror;
        lasterror('reset');
        le2 = lasterror;
    )");
    EXPECT_TRUE(evalBool("isempty(le0.message) && isempty(le0.identifier) && isempty(le0.stack)"));
    EXPECT_EQ(evalString("le1.identifier"), "s6:lasterr:tag");
    EXPECT_EQ(evalString("le1.message"), "captured by lasterror");
    EXPECT_GE(evalScalar("numel(le1.stack)"), 1.0);
    EXPECT_TRUE(evalBool("isempty(le2.message) && isempty(le2.identifier) && isempty(le2.stack)"));
}

INSTANTIATE_DUAL(Wave2VmTwIndependenceTest);
