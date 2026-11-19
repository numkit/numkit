/// @file help.hpp
/// @ingroup group_lang
#pragma once

#include <keel/array.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/help/help_catalog.hpp>
#include <numkit/value/value.hpp>

namespace numkit {
class Engine;
}

namespace numkit::bundle {

/// @addtogroup group_lang
/// @{

/// @brief Installs MATLAB-compatible help and documentation commands into the script Engine.
class HelpLibrary {
public:
    /// @brief Registers `help`, `doc`, `what`, `builtins`, `categories`, and `inmem` in the given engine.
    /// @param engine Engine instance.
    static void install(Engine &engine);
};

/// @brief Convenience alias to install help system into an engine.
/// @param engine Engine instance.
inline void registerHelpLibrary(Engine &engine)
{
    HelpLibrary::install(engine);
}

// ── C++ Public API ──────────────────────────────────────────────────────────

/// @brief Formats help text or catalog index for given function or topic name (`help query`).
/// @param query Function, category, or empty string to display root catalog index.
/// @return Formatted help string.
/// @see what, builtins, categories
keel::String help(keel::StringRef query = "");

/// @brief MATLAB script engine wrapper for `help`.
/// @param args Command-line arguments.
/// @param mr Memory resource for output allocation.
/// @return Help string value.
Value help(keel::ArrayRef<Value> args, keel::Allocator *mr = nullptr);

/// @brief Lists all functions in specified category (`what category`).
/// @param category Category name (e.g. `"elmat"`, `"signal"`, `"stats"`).
/// @return Array of function names in category.
/// @see help, builtins, categories
keel::Array<keel::String> what(keel::StringRef category = "elmat");

/// @brief MATLAB script engine wrapper for `what`.
/// @param args Category argument.
/// @param mr Memory resource for output allocation.
/// @return Cell array of strings.
Value what(keel::ArrayRef<Value> args, keel::Allocator *mr = nullptr);

/// @brief Lists all built-in functions registered in the system or specified category (`builtins`).
/// @param category Category name (or empty string for all built-ins).
/// @return Array of registered builtin function names.
/// @see help, what, categories
keel::Array<keel::String> builtins(keel::StringRef category = "");

/// @brief MATLAB script engine wrapper for `builtins`.
/// @param args Category argument.
/// @param mr Memory resource for output allocation.
/// @return Cell array of strings.
Value builtins(keel::ArrayRef<Value> args, keel::Allocator *mr = nullptr);

/// @brief Lists all registered documentation categories (`categories`).
/// @return Array of category identifiers (e.g. `"elmat"`, `"elfun"`, `"signal"`, ...).
/// @see help, what, builtins
keel::Array<keel::String> categories();

/// @brief Returns all registered categories as a cell array of strings.
/// @param mr Memory resource for output allocation.
/// @return Cell array of category name strings.
Value categories(keel::Allocator *mr);


/// @}
} // namespace numkit::bundle
