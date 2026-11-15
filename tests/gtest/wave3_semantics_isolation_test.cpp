// tests/gtest/wave3_semantics_isolation_test.cpp
//
// Differential & concurrency regression tests for Wave 3:
//   - Per-function isolated `persistent` store (decoupled from globalsEnv_)
//   - Lexical nested functions, multi-level upvalue cells, and escaping @inner closures
//   - Live VM frame <-> Environment synchronization (eval, evalin, assignin, load, clearvars)
//   - Lexer & parser fixes ("abc"' transpose, [1 ...\n -2] continuation, --x unary, command disambiguation)
//   - OOP classdef accessor recursion guard, Constant/undeclared property enforcement, predicates, exist
//   - Multi-Engine concurrent isolation (lastwarn, setenv/getenv, persistent, closures, paused callbacks)

#include "dual_engine_fixture.hpp"
#include <keel/fs.h>

#include <atomic>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace m_test;

class Wave3SemanticsIsolationTest : public DualEngineTest {};

// 1. Per-function persistent isolation, global decoupling, whos attribute, and clear reset
TEST_P(Wave3SemanticsIsolationTest, PersistentIsolationAndLifecycle)
{
    eval(
        "function r = counter_alpha()\n"
        "  persistent count\n"
        "  if isempty(count)\n"
        "    count = 0;\n"
        "  end\n"
        "  count = count + 1;\n"
        "  r = count;\n"
        "end\n"
        "function r = counter_beta()\n"
        "  persistent count\n"
        "  if isempty(count)\n"
        "    count = 100;\n"
        "  end\n"
        "  count = count + 10;\n"
        "  r = count;\n"
        "end\n"
        "function [isP, isG] = inspect_pers()\n"
        "  persistent pvar\n"
        "  pvar = 42;\n"
        "  info = whos('pvar');\n"
        "  isP = info.persistent;\n"
        "  isG = info.global;\n"
        "end\n");

    // Two functions with same persistent variable name must not collide
    EXPECT_DOUBLE_EQ(evalScalar("counter_alpha()"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("counter_alpha()"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("counter_beta()"), 110.0);
    EXPECT_DOUBLE_EQ(evalScalar("counter_alpha()"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("counter_beta()"), 120.0);

    // Persistent variable must not leak into global variable of same name
    eval("global count; count = -999;");
    EXPECT_DOUBLE_EQ(evalScalar("counter_alpha()"), 4.0);
    EXPECT_DOUBLE_EQ(getVar("count"), -999.0);

    // whos reports persistent=true and global=false
    eval("[pFlag, gFlag] = inspect_pers();");
    EXPECT_TRUE(evalBool("pFlag"));
    EXPECT_FALSE(evalBool("gFlag"));

    // Top-level persistent declaration must throw
    EXPECT_ANY_THROW(eval("persistent bad_top_level_var"));

    // `clear counter_alpha` resets only counter_alpha's persistent state, leaving counter_beta intact
    eval("clear counter_alpha");
    EXPECT_DOUBLE_EQ(evalScalar("counter_alpha()"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("counter_beta()"), 130.0);
}

// 2. Lexical nested functions, sibling mutual recursion, 3-level upvalues, and @inner closures
TEST_P(Wave3SemanticsIsolationTest, NestedFunctionsLexicalUpvaluesAndClosures)
{
    // 2a. Direct nested function + variable assigned ONLY inside nested function and read in outer
    eval(
        "function out = outer_init_in_nested(a)\n"
        "  populate();\n"
        "  out = a + initialized_by_inner;\n"
        "  function populate()\n"
        "    initialized_by_inner = a * 3;\n"
        "    a = a + 1;\n"
        "  end\n"
        "end\n");
    // a starts at 5 -> populate sets initialized_by_inner=15, a=6 -> out = 21
    EXPECT_DOUBLE_EQ(evalScalar("outer_init_in_nested(5)"), 21.0);

    // 2b. Sibling mutual recursion sharing outer state
    eval(
        "function [flag, steps] = mutual_even_odd(n)\n"
        "  steps = 0;\n"
        "  flag = is_even_n(n);\n"
        "  function r = is_even_n(k)\n"
        "    steps = steps + 1;\n"
        "    if k == 0\n"
        "      r = 1;\n"
        "    else\n"
        "      r = is_odd_n(k - 1);\n"
        "    end\n"
        "  end\n"
        "  function r = is_odd_n(k)\n"
        "    steps = steps + 1;\n"
        "    if k == 0\n"
        "      r = 0;\n"
        "    else\n"
        "      r = is_even_n(k - 1);\n"
        "    end\n"
        "  end\n"
        "end\n");
    eval("[eflag, esteps] = mutual_even_odd(6);");
    EXPECT_DOUBLE_EQ(getVar("eflag"), 1.0);
    EXPECT_DOUBLE_EQ(getVar("esteps"), 7.0);

    // 2c. 3-level nested functions mutating outer & middle scopes without leaking deep locals
    eval(
        "function res = three_level(x)\n"
        "  a = x;\n"
        "  m_res = middle(2);\n"
        "  res = a + m_res;\n"
        "  function r = middle(y)\n"
        "    b = y * 10;\n"
        "    deep(5);\n"
        "    r = b;\n"
        "    function deep(z)\n"
        "      d_only = z * 100;\n"
        "      a = a + z;\n"
        "      b = b + d_only / 100;\n"
        "    end\n"
        "  end\n"
        "end\n");
    // x=3: a=3 -> middle(2): b=20 -> deep(5): a=8, b=25 -> m_res=25 -> res=33
    EXPECT_DOUBLE_EQ(evalScalar("three_level(3)"), 33.0);

    // 2d. Escaping @inner closures with independent lexical cells + func2str + survival across clear
    eval(
        "function h = make_acc(startVal)\n"
        "  acc = startVal;\n"
        "  h = @step;\n"
        "  function cur = step(delta)\n"
        "    acc = acc + delta;\n"
        "    cur = acc;\n"
        "  end\n"
        "end\n");
    eval("c1 = make_acc(10); c2 = make_acc(100);");
    EXPECT_EQ(eval("func2str(c1)").toString(), "make_acc/step");
    EXPECT_DOUBLE_EQ(evalScalar("c1(5)"), 15.0);
    EXPECT_DOUBLE_EQ(evalScalar("c2(7)"), 107.0);
    eval("clear make_acc");
    EXPECT_DOUBLE_EQ(evalScalar("c1(3)"), 18.0);
    EXPECT_DOUBLE_EQ(evalScalar("c2(2)"), 109.0);

    // 2e. Anonymous function inside outer calling nested function + snapshotting local by value
    eval(
        "function fn = make_combo(base)\n"
        "  live = 0;\n"
        "  snap = base;\n"
        "  fn = @(d) bump(d) + snap;\n"
        "  snap = 9999;\n"
        "  function v = bump(d)\n"
        "    live = live + d;\n"
        "    v = live;\n"
        "  end\n"
        "end\n");
    eval("cf = make_combo(10);");
    EXPECT_DOUBLE_EQ(evalScalar("cf(4)"), 14.0);
    EXPECT_DOUBLE_EQ(evalScalar("cf(6)"), 20.0);
}

// 3. VM register synchronization with eval, evalin, assignin, load, and clearvars
TEST_P(Wave3SemanticsIsolationTest, DynamicWorkspaceSyncEvalAssigninLoadClearvars)
{
    // 3a. Dynamic variable injection & builtin shadowing via eval() inside a function
    eval(
        "function [r1, r2] = dyn_eval_fn()\n"
        "  a = 10;\n"
        "  eval('b = a + 32;');\n"
        "  eval('sum = [100, 200, 300];');\n"
        "  r1 = b;\n"
        "  r2 = sum(2);\n"
        "end\n");
    eval("[de1, de2] = dyn_eval_fn();");
    EXPECT_DOUBLE_EQ(getVar("de1"), 42.0);
    EXPECT_DOUBLE_EQ(getVar("de2"), 200.0);

    // 3b. assignin('caller', ...) and evalin('caller', ...) updating live caller registers
    eval(
        "function poke_caller()\n"
        "  assignin('caller', 'x_cal', 50);\n"
        "  evalin('caller', 'x_cal = x_cal + 7; y_dyn = 9;');\n"
        "end\n"
        "function out = caller_host()\n"
        "  x_cal = 1;\n"
        "  poke_caller();\n"
        "  out = x_cal + y_dyn;\n"
        "end\n");
    EXPECT_DOUBLE_EQ(evalScalar("caller_host()"), 66.0);

    // 3c. assignin('base', ...) updating live base script variable mid-script
    eval(
        "function write_base(v)\n"
        "  assignin('base', 'base_slot', v);\n"
        "end\n");
    eval("base_slot = 3; write_base(88); after_base = base_slot + 2;");
    EXPECT_DOUBLE_EQ(getVar("after_base"), 90.0);

    // 3d. load (both MAT and -ascii) inside a function populating local scope cleanly
    auto tmpBase = keel::fs_temp_dir();
    ASSERT_TRUE(tmpBase.is_ok());
    keel::String tmpDir = keel::path_join(tmpBase.value(), "numkit_wave3_load_test");
    ASSERT_TRUE(keel::fs_create_dirs(tmpDir).is_ok());
    keel::String matPathK = keel::path_join(tmpDir, "w3vars.mat");
    keel::String ascPathK = keel::path_join(tmpDir, "myvec.txt");
    std::string matPath(matPathK.data(), matPathK.size());
    std::string ascPath(ascPathK.data(), ascPathK.size());
    ASSERT_TRUE(keel::fs_write_text(ascPathK, "11 22 33\n").is_ok());
    eval("saved_val = [4, 5, 6]; save('" + matPath + "', 'saved_val'); clear saved_val;");
    eval(
        "function [s1, s2] = load_in_fn(mp, ap)\n"
        "  load(mp);\n"
        "  load(ap, '-ascii');\n"
        "  s1 = saved_val(2);\n"
        "  s2 = myvec(3);\n"
        "end\n");
    eval("[lv1, lv2] = load_in_fn('" + matPath + "', '" + ascPath + "');");
    EXPECT_DOUBLE_EQ(getVar("lv1"), 5.0);
    EXPECT_DOUBLE_EQ(getVar("lv2"), 33.0);
    (void)keel::fs_remove_all(tmpDir);

    // 3e. clearvars & clearvars -except inside function and top-level (preserving globals)
    eval("global g_keep; g_keep = 77;");
    eval(
        "function [hasA, hasB, gVal] = test_clearvars()\n"
        "  global g_keep\n"
        "  a = 1;\n"
        "  b = 2;\n"
        "  clearvars -except b\n"
        "  hasA = exist('a', 'var');\n"
        "  hasB = (exist('b', 'var') == 1) && (b == 2);\n"
        "  gVal = g_keep;\n"
        "end\n");
    eval("[cvA, cvB, cvG] = test_clearvars();");
    EXPECT_DOUBLE_EQ(getVar("cvA"), 0.0);
    EXPECT_TRUE(evalBool("cvB"));
    EXPECT_DOUBLE_EQ(getVar("cvG"), 77.0);
}

// 4. Lexer and parser bug fixes
TEST_P(Wave3SemanticsIsolationTest, LexerParserFixes)
{
    // 4a. Double-quoted string transpose ("abc"', ["a", "b"]', escaped quotes)
    eval("st1 = \"abc\"'; st2 = [\"a\", \"b\"]'; st3 = \"x\"\"y\"';");
    EXPECT_EQ(eval("st1").stringElem(0), "abc");
    EXPECT_DOUBLE_EQ(evalScalar("size(st2, 1)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("size(st2, 2)"), 1.0);
    EXPECT_EQ(eval("st3").stringElem(0), "x\"y");

    // 4b. Implicit comma across `...` line continuations inside brackets and braces
    eval("arr_cont = [1 ...\n -2]; arr_adj = [1...\n2]; cell_cont = {10 ...\n -20};");
    EXPECT_DOUBLE_EQ(evalScalar("numel(arr_cont)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("arr_cont(1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("arr_cont(2)"), -2.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(arr_adj)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("arr_adj(2)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(cell_cont)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("cell_cont{2}"), -20.0);

    // 4c. Chained unary operators (--x, ~~x, ~-1, -+x)
    EXPECT_DOUBLE_EQ(evalScalar("--7"), 7.0);
    EXPECT_TRUE(evalBool("~~true"));
    EXPECT_FALSE(evalBool("~-1"));
    EXPECT_DOUBLE_EQ(evalScalar("-+4"), -4.0);

    // 4d. Variable-headed command disambiguation (`x -1`, `x /2`, `x -y`, `x -1e1`) in functions
    eval(
        "function [a1, a2, a3, a4] = cmd_var_disambig(x, y)\n"
        "  x -3;\n"
        "  a1 = ans;\n"
        "  x /2;\n"
        "  a2 = ans;\n"
        "  x -y;\n"
        "  a3 = ans;\n"
        "  x -1e1;\n"
        "  a4 = ans;\n"
        "end\n");
    eval("[ca1, ca2, ca3, ca4] = cmd_var_disambig(10, 4);");
    EXPECT_DOUBLE_EQ(getVar("ca1"), 7.0);
    EXPECT_DOUBLE_EQ(getVar("ca2"), 5.0);
    EXPECT_DOUBLE_EQ(getVar("ca3"), 6.0);
    EXPECT_DOUBLE_EQ(getVar("ca4"), 0.0);
}

// 5. classdef get.Prop / set.Prop recursion guard, Constant/undeclared props, exist & predicates
TEST_P(Wave3SemanticsIsolationTest, ClassdefAccessorsConstantDynamicPropsAndPredicates)
{
    eval(
        "classdef GuardedBox\n"
        "  properties (Constant)\n"
        "     TAG = 42\n"
        "  end\n"
        "  properties\n"
        "    Val = 5\n"
        "  end\n"
        "  methods\n"
        "    function v = get.Val(obj)\n"
        "      v = obj.Val * 2;\n"
        "    end\n"
        "    function obj = set.Val(obj, v)\n"
        "      obj.Val = v + 1;\n"
        "    end\n"
        "  end\n"
        "end\n");
    eval(
        "classdef OpenBox < dynamicprops\n"
        "  properties\n"
        "    Base = 1\n"
        "  end\n"
        "end\n");

    // get.Val reads raw stored Val=5 -> returns 10 without infinite recursion!
    // set.Val(7) stores 8 -> get.Val returns 16 without infinite recursion!
    eval("gb = GuardedBox(); v_init = gb.Val; gb.Val = 7; v_after = gb.Val;");
    EXPECT_DOUBLE_EQ(getVar("v_init"), 10.0);
    EXPECT_DOUBLE_EQ(getVar("v_after"), 16.0);
    EXPECT_DOUBLE_EQ(evalScalar("GuardedBox.TAG"), 42.0);
    EXPECT_DOUBLE_EQ(evalScalar("gb.TAG"), 42.0);

    // Writing a Constant property or an undeclared property on a normal class must throw
    EXPECT_ANY_THROW(eval("gb.TAG = 99;"));
    EXPECT_ANY_THROW(eval("gb.UndeclaredProp = 123;"));

    // Subclass of dynamicprops allows dynamic property assignment
    eval("ob = OpenBox(); ob.DynamicExtra = 77; dval = ob.DynamicExtra;");
    EXPECT_DOUBLE_EQ(getVar("dval"), 77.0);

    // exist('ClassName', 'class') and unfiltered exist('ClassName') return 8
    EXPECT_DOUBLE_EQ(evalScalar("exist('GuardedBox', 'class')"), 8.0);
    EXPECT_DOUBLE_EQ(evalScalar("exist('GuardedBox')"), 8.0);
    EXPECT_DOUBLE_EQ(evalScalar("exist('NonExistentClass_XYZ', 'class')"), 0.0);

    // Table vs Timetable predicates
    eval("tbl = table([1; 2], [10; 20]); ttbl = timetable([1; 2], [10; 20]);");
    EXPECT_TRUE(evalBool("istable(tbl)"));
    EXPECT_FALSE(evalBool("istable(ttbl)"));
    EXPECT_TRUE(evalBool("istimetable(ttbl)"));
    EXPECT_FALSE(evalBool("istimetable(tbl)"));
    EXPECT_TRUE(evalBool("istabular(tbl)"));
    EXPECT_TRUE(evalBool("istabular(ttbl)"));
    EXPECT_FALSE(evalBool("iscategorical(tbl)"));
}

INSTANTIATE_DUAL(Wave3SemanticsIsolationTest);

// 6. Multi-Engine concurrent isolation across threads & backends
TEST(Wave3MultiEngineIsolation, ParallelEnginesIndependentState)
{
    constexpr int kThreads = 6;
    constexpr int kIters = 25;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int tid = 0; tid < kThreads; ++tid) {
        workers.emplace_back([tid, &failures]() {
            try {
                StandardEngine eng;
                eng.setOutputFunc([](const std::string &) {});
                eng.setBackend((tid % 2 == 0) ? Engine::Backend::VM
                                              : Engine::Backend::TreeWalker);

                eng.eval(
                    "function r = pcount()\n"
                    "  persistent c\n"
                    "  if isempty(c), c = 0; end\n"
                    "  c = c + 1;\n"
                    "  r = c;\n"
                    "end\n"
                    "function h = mkclos(seed)\n"
                    "  s = seed;\n"
                    "  h = @add;\n"
                    "  function y = add(v)\n"
                    "    s = s + v;\n"
                    "    y = s;\n"
                    "  end\n"
                    "end\n");

                const std::string envVal = "val_thread_" + std::to_string(tid);
                eng.eval("setenv('NUMKIT_W3_ISO_KEY', '" + envVal + "');");
                eng.eval("clos = mkclos(" + std::to_string(tid * 100) + ");");

                for (int it = 1; it <= kIters; ++it) {
                    const std::string wmsg = "warn_" + std::to_string(tid) + "_" + std::to_string(it);
                    const std::string wid = "w3:t" + std::to_string(tid);
                    eng.eval("warning('" + wid + "', '" + wmsg + "');");

                    eng.eval("[m, id] = lastwarn();");
                    if (eng.getVariable("m")->toString() != wmsg
                        || eng.getVariable("id")->toString() != wid) {
                        failures.fetch_add(1);
                        return;
                    }
                    if (eng.eval("getenv('NUMKIT_W3_ISO_KEY')").toString() != envVal) {
                        failures.fetch_add(1);
                        return;
                    }
                    if (eng.eval("pcount()").toScalar() != static_cast<double>(it)) {
                        failures.fetch_add(1);
                        return;
                    }
                    double expectedClos = static_cast<double>(tid * 100 + it * (it + 1) / 2);
                    if (eng.eval("clos(" + std::to_string(it) + ")").toScalar() != expectedClos) {
                        failures.fetch_add(1);
                        return;
                    }
                    // Also exercise higher-order callback state on every iteration
                    double cfSum = eng.eval("sum(cellfun(@(x) x + " + std::to_string(tid)
                                            + ", {1, 2, 3}))").toScalar();
                    if (cfSum != static_cast<double>(6 + 3 * tid)) {
                        failures.fetch_add(1);
                        return;
                    }
                }
            } catch (...) {
                failures.fetch_add(1);
            }
        });
    }

    for (auto &t : workers)
        t.join();

    EXPECT_EQ(failures.load(), 0);
}
