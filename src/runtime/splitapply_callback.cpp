// src/runtime/src/language/splitapply_callback.cpp
//
// VM State-machine splitapply callback implementation.

#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/core/callback_builtin.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/vm.hpp>
#include <numkit/value/value.hpp>

#include <map>
#include <memory>
#include <vector>

namespace numkit::runtime {

namespace detail {

struct SplitapplyCallbackBuiltin : CallbackBuiltin
{
    std::shared_ptr<VmContinuation> tryStart(keel::ArrayRef<Value> args, keel::usize nargout,
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
        std::map<int, std::vector<keel::usize>> buckets;
        for (keel::usize i = 0; i < n; ++i)
            buckets[(int)G.elemAsDouble(i)].push_back(i);
        auto groups = std::make_shared<std::vector<std::vector<keel::usize>>>();
        for (auto &kv : buckets)
            groups->push_back(std::move(kv.second));
        auto inputs = std::make_shared<std::vector<Value>>();
        for (keel::usize k = 0; k < nIn; ++k)
            inputs->push_back(args[1 + k]);
        auto cont = std::make_shared<LoopContinuation>();
        cont->handle = args[0];
        cont->n = groups->size();
        cont->dest = dest;
        cont->makeArgs = [inputs, groups, mr](keel::usize i) -> std::vector<Value> {
            const auto &idxs = (*groups)[i];
            std::vector<Value> callArgs(inputs->size());
            for (keel::usize k = 0; k < inputs->size(); ++k) {
                auto sub = Value::matrix(idxs.size(), 1, ValueType::DOUBLE, mr);
                double *sd = sub.doubleDataMut();
                for (keel::usize j = 0; j < idxs.size(); ++j)
                    sd[j] = (*inputs)[k].elemAsDouble(idxs[j]);
                callArgs[k] = std::move(sub);
            }
            return callArgs;
        };
        cont->pack = [mr](std::vector<Value> &results) -> Value {
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
                                   std::make_shared<detail::SplitapplyCallbackBuiltin>());
}

} // namespace numkit::runtime
