// src/bundle/standard_library.cpp
//
// StandardLibrary::install — wires language runtime, builtins, and SIMD fusion rules into an Engine.
// Toolboxes (signal, stats, etc.) are loaded dynamically as plugins via PluginLoader.

#include <numkit/bundle/standard_library.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/runtime/runtime.hpp>
#include <numkit/bundle/builtin_library.hpp>

namespace numkit {

// Defined in register/fusion/fused_rules.cpp — registers element-wise fusion
// rules (idiom -> ops kernel) on the engine's FusionRule registry.
void registerFusionRules(Engine &engine);

void StandardLibrary::install(Engine &engine)
{
    // 1. Language runtime
    RuntimeLibrary::install(engine);

    // 2. Builtin standard language algorithms
    BuiltinLibrary::install(engine);

    // 3. Element-wise SIMD fusion rules — registered after builtins exist
    registerFusionRules(engine);
}

std::unique_ptr<Engine> makeStandardEngine(std::pmr::memory_resource *mr)
{
    auto engine = std::make_unique<Engine>(mr);
    StandardLibrary::install(*engine);
    return engine;
}

} // namespace numkit
