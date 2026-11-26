// tests/c_api/engine_c_api.cpp
//
// Standalone C-ABI tests for `numkit/numkit` (`numkit_c` Full Engine).
#include <numkit/core/c_api.h>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {

int doubleHostFn(void * /*userData*/,
                 const numkit_val *const *args,
                 size_t nargs,
                 size_t /*nargout*/,
                 numkit_val **outs,
                 numkit_error *err)
{
    if (nargs < 1) return 1;
    double v = numkit_val_as_double(args[0], err);
    if (err->code != 0) return 1;
    if (outs != nullptr) {
        outs[0] = numkit_val_scalar(2.0 * v);
    }
    return 0;
}

} // namespace

TEST(NumkitEngineCApi, EvalWorkspaceCallbackAndResolve)
{
    const int64_t base = numkit_debug_live_values();
    numkit_error err{};
    numkit_engine_ctx *ctx = numkit_engine_create(&err);
    ASSERT_NE(ctx, nullptr) << err.message;

    numkit_val *res = numkit_engine_eval(ctx, "M = magic(4); sum(diag(M))", &err);
    ASSERT_NE(res, nullptr) << err.message;
    EXPECT_DOUBLE_EQ(numkit_val_as_double(res, &err), 34.0);
    numkit_val_release(res);

    numkit_val *mVar = numkit_engine_workspace_get(ctx, "M", &err);
    ASSERT_NE(mVar, nullptr) << err.message;
    EXPECT_EQ(numkit_val_numel(mVar), 16u);
    numkit_val_release(mVar);

    numkit_val *kVal = numkit_val_scalar(3.5);
    ASSERT_EQ(numkit_engine_workspace_set(ctx, "k", kVal, &err), 0);
    numkit_val_release(kVal);

    ASSERT_EQ(numkit_engine_register_fn(ctx, nullptr, "host_twice", &doubleHostFn, nullptr, &err), 0);
    numkit_val *cbRes = numkit_engine_eval(ctx, "host_twice(k + 1.5)", &err);
    ASSERT_NE(cbRes, nullptr) << err.message;
    EXPECT_DOUBLE_EQ(numkit_val_as_double(cbRes, &err), 10.0);
    numkit_val_release(cbRes);

    numkit_val *cbAssigned = numkit_engine_eval(ctx, "y = host_twice(k + 2.5); y", &err);
    ASSERT_NE(cbAssigned, nullptr) << err.message;
    EXPECT_DOUBLE_EQ(numkit_val_as_double(cbAssigned, &err), 12.0);
    numkit_val_release(cbAssigned);

    const numkit_fn *sinFn = numkit_engine_resolve(ctx, nullptr, "sin");
    ASSERT_NE(sinFn, nullptr);
    numkit_val *zero = numkit_val_scalar(0.0);
    const numkit_val *in[1] = {zero};
    numkit_val *sinOut = nullptr;
    ASSERT_EQ(numkit_engine_invoke(ctx, sinFn, in, 1, 1, &sinOut, &err), 0);
    EXPECT_DOUBLE_EQ(numkit_val_as_double(sinOut, &err), 0.0);
    numkit_val_release(sinOut);
    numkit_val_release(zero);

    numkit_engine_destroy(ctx);
    EXPECT_EQ(numkit_debug_live_values(), base);
}

TEST(NumkitEngineCApi, CooperativeCancelFromHostThread)
{
    const int64_t base = numkit_debug_live_values();
    numkit_error err{};
    numkit_engine_ctx *ctx = numkit_engine_create(&err);
    ASSERT_NE(ctx, nullptr) << err.message;
    EXPECT_EQ(numkit_engine_is_cancel_requested(ctx), 0);

    std::thread canceller([ctx]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        numkit_engine_request_cancel(ctx);
    });
    numkit_val *interrupted = numkit_engine_eval(ctx, "x = 0; while true; x = x + 1; end", &err);
    canceller.join();

    EXPECT_EQ(interrupted, nullptr);
    EXPECT_NE(err.code, 0);
    EXPECT_EQ(numkit_engine_is_cancel_requested(ctx), 1);

    numkit_engine_clear_cancel(ctx);
    EXPECT_EQ(numkit_engine_is_cancel_requested(ctx), 0);

    numkit_val *ok = numkit_engine_eval(ctx, "6 * 7", &err);
    ASSERT_NE(ok, nullptr) << err.message;
    EXPECT_DOUBLE_EQ(numkit_val_as_double(ok, &err), 42.0);
    numkit_val_release(ok);

    numkit_engine_destroy(ctx);
    EXPECT_EQ(numkit_debug_live_values(), base);
}

TEST(NumkitEngineCApi, GraphicsSceneModelAndEvents)
{
    const int64_t base = numkit_debug_live_values();
    numkit_error err{};
    numkit_engine_ctx *ctx = numkit_engine_create(&err);
    ASSERT_NE(ctx, nullptr) << err.message;

    const int32_t rootId = numkit_gfx_root(ctx);
    EXPECT_EQ(rootId, 1);
    EXPECT_EQ(numkit_gfx_node_valid(ctx, rootId), 1);

    char clsBuf[64]{};
    EXPECT_GT(numkit_gfx_node_class(ctx, rootId, clsBuf, sizeof(clsBuf)), 0u);
    EXPECT_STREQ(clsBuf, "matlab.ui.Root");

    const uint64_t g0 = numkit_gfx_generation(ctx);
    numkit_val *r = numkit_engine_eval(ctx, "f = figure('Visible','off','Name','GuiFig');", &err);
    ASSERT_NE(r, nullptr) << err.message;
    numkit_val_release(r);

    int32_t kids[8]{};
    const size_t nKids = numkit_gfx_node_children(ctx, rootId, 0, kids, 8);
    ASSERT_EQ(nKids, 1u);
    const int32_t figId = kids[0];
    EXPECT_EQ(numkit_gfx_node_parent(ctx, figId), rootId);
    EXPECT_GT(numkit_gfx_node_class(ctx, figId, clsBuf, sizeof(clsBuf)), 0u);
    EXPECT_STREQ(clsBuf, "matlab.ui.Figure");

    numkit_val *nameVal = numkit_gfx_get_prop(ctx, figId, "Name", &err);
    ASSERT_NE(nameVal, nullptr) << err.message;
    char fmtBuf[64]{};
    EXPECT_GT(numkit_engine_format(ctx, nameVal, fmtBuf, sizeof(fmtBuf)), 0u);
    EXPECT_STREQ(fmtBuf, "GuiFig");
    numkit_val_release(nameVal);

    numkit_val *newTag = numkit_engine_eval(ctx, "'from_gui'", &err);
    ASSERT_NE(newTag, nullptr) << err.message;
    EXPECT_EQ(numkit_gfx_set_prop(ctx, figId, "Tag", newTag, &err), 0) << err.message;
    numkit_val_release(newTag);

    numkit_val *evCheck = numkit_engine_eval(ctx, "strcmp(f.Tag, 'from_gui')", &err);
    ASSERT_NE(evCheck, nullptr) << err.message;
    EXPECT_DOUBLE_EQ(numkit_val_as_double(evCheck, &err), 1.0);
    numkit_val_release(evCheck);

    numkit_gfx_event evs[32]{};
    const size_t nEvs = numkit_gfx_poll_events(ctx, g0, evs, 32);
    EXPECT_GT(nEvs, 2u);
    numkit_gfx_clear_events(ctx);
    EXPECT_EQ(numkit_gfx_poll_events(ctx, 0, evs, 32), 0u);

    numkit_engine_destroy(ctx);
    EXPECT_EQ(numkit_debug_live_values(), base);
}
