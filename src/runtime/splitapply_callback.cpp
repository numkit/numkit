// src/runtime/src/language/splitapply_callback.cpp
//
// VM State-machine splitapply callback implementation.

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/vm.hpp>
#include <numkit/value/value.hpp>

namespace numkit::runtime {

namespace detail {

struct SplitapplyCallbackBuiltin : CallbackBuiltin
{
    keel::Arc<VmContinuation> tryStart(keel::ArrayRef<Value> args, keel::usize nargout,
                                       Value *dest, Engine &eng) override
    {
        if (args.size() < 3 || nargout > 1)
            return nullptr;
        if (!eng.isUserCodeHandle(args[0]))
            return nullptr;
        const Value &G = args[args.size() - 1];
        const keel::usize nIn = args.size() - 2;
        if (nIn == 0)
            return nullptr;
        const keel::usize n = G.numel();
        for (keel::usize k = 0; k < nIn; ++k)
            if (args[1 + k].numel() != n)
                return nullptr; // shape error -> synchronous path reports it
        auto *mr = eng.allocator();
        // Bucket indices by group id (sorted).
        keel::HashMap<int, keel::Array<keel::usize>> buckets;
        for (keel::usize i = 0; i < n; ++i)
            buckets.get_or_insert(keel::as<int>(G.elemAsDouble(i))).push_back(i);
        keel::Array<int> gids;
        gids.reserve(buckets.size());
        for (auto [k, v] : buckets) {
            (void)v;
            gids.push_back(k);
        }
        keel::sort(gids);

        auto groups = keel::make_arc<keel::Array<keel::Array<keel::usize>>>();
        groups->reserve(gids.size());
        for (int gid : gids) {
            if (auto *arr = buckets.get(gid))
                groups->push_back(keel::move(*arr));
        }

        auto inputs = keel::make_arc<keel::Array<Value>>();
        inputs->reserve(nIn);
        for (keel::usize k = 0; k < nIn; ++k)
            inputs->push_back(args[1 + k]);

        auto cont = keel::make_arc<LoopContinuation>();
        cont->handle = args[0];
        cont->n = groups->size();
        cont->dest = dest;
        cont->makeArgs = [inputs, groups, mr](keel::usize i) -> keel::Array<Value> {
            const auto &idxs = (*groups)[i];
            keel::Array<Value> callArgs(inputs->size(), mr);
            for (keel::usize k = 0; k < inputs->size(); ++k) {
                auto sub = Value::matrix(idxs.size(), 1, ValueType::DOUBLE, mr);
                double *sd = sub.doubleDataMut();
                for (keel::usize j = 0; j < idxs.size(); ++j)
                    sd[j] = (*inputs)[k].elemAsDouble(idxs[j]);
                callArgs[k] = keel::move(sub);
            }
            return callArgs;
        };
        cont->pack = [mr](keel::Array<Value> &results) -> Value {
            auto out = Value::matrix(results.size(), 1, ValueType::DOUBLE, mr);
            double *d = out.doubleDataMut();
            for (keel::usize i = 0; i < results.size(); ++i)
                d[i] = results[i].toScalar();
            return out;
        };
        cont->results.reserve(cont->n);
        return cont;
    }
};

} // namespace detail

void registerSplitapplyCallbackBuiltin(Engine &engine)
{
    engine.registerCallbackBuiltin("splitapply",
                                   keel::make_arc<detail::SplitapplyCallbackBuiltin>());
}

} // namespace numkit::runtime
