// tests/c_api/engine_c_api.cpp
//
// Standalone C-ABI tests for `numkit/numkit` (`numkit_c` Full Engine).
#include <numkit/core/c_api.h>

#include <gtest/gtest.h>

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
