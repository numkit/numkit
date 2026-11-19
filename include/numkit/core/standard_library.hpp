/// @file standard_library.hpp
/// @ingroup group_lang
// include/numkit/core/standard_library.hpp
//
// The composition layer for NumKit. StandardLibrary::install wires all
// standard subsystems (Runtime, Builtin) into an Engine instance.
#pragma once

#include <keel/memory.h>
#include <keel/smart_ptr.h>

namespace numkit {

/// @addtogroup group_lang
/// @{


class Engine;

class StandardLibrary {
public:
    static void install(Engine &engine);
};

/// @brief Forwarder for backward compatibility.
inline void installStandardLibrary(Engine &engine) {
    StandardLibrary::install(engine);
}

// Convenience: construct an Engine on the heap with the standard library
// already installed. Returned by UniquePtr because Engine is non-movable.
keel::UniquePtr<Engine> makeStandardEngine(keel::Allocator *mr = nullptr);


/// @}
} // namespace numkit
