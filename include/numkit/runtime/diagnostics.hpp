/// @file diagnostics.hpp
/// @ingroup group_lang
// toolboxes/builtin/include/numkit/builtin/programming/errors/diagnostics.hpp
#pragma once

#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/value/value.hpp>

namespace numkit {
class Engine;
}

namespace numkit::runtime {

/// @addtogroup group_lang
/// @{

void registerDiagnosticsRuntime(Engine &engine);

// ── error() ───────────────────────────────────────────────────

/// @brief Throw an Error (`error(...)`).
///
/// Accepts any of:
/// - `error()`                  → generic `"Error"`
/// - `error(msg)`               → `msg` literal
/// - `error(msg, arg1, ...)`    → sprintf-formatted message
/// - `error(id, msg, ...)`      → identifier + formatted message
///   (`id` must contain `:`)
/// - `error(MException-struct)` → rethrow `identifier` / `message`
///   fields from the struct
///
/// @param args  Arguments per the forms above.
/// @throws Error  Always (never returns).
[[noreturn]] void error(keel::ArrayRef<Value> args);

/// @brief Engine-stateful warning (`warning(...)`).
///
/// Same form-dispatch as @ref error but does not throw — writes
/// `"Warning: ...\n"` via `engine.outputText()` and updates the
/// engine's most-recent-warning state (`engine.setLastWarn`).
///
/// @param engine  Engine whose output stream receives the message.
/// @param args    Form-dispatched as for @ref error.
void warning(Engine &engine, keel::ArrayRef<Value> args);  // lint: engine-io

/// @brief Build an MException-like struct (`me = MException(id, msg, args...)`).
///
/// Returns a struct with `identifier` and `message` fields. The
/// message is formatted via sprintf-style argument substitution.
///
/// @param args  `(id, msg, arg1, ...)`. Throws on fewer than 2 args.
/// @param mr    Memory resource (nullptr → process default).
/// @return      Struct with `{identifier, message}` fields.
/// @throws Error  Bad argument count (`m:MException:nargin`).
Value mexception(keel::ArrayRef<Value> args, keel::Allocator *mr = nullptr);

// ── rethrow(ME) / throw(ME) ──────────────────────────────────────────

/// @brief Rethrow from an MException struct (`rethrow(me)` / `throw(me)`).
///
/// Extracts `message` + `identifier` from the struct and throws Error.
/// Used by both `rethrow()` and `throw()` (they are aliases).
///
/// @param me  MException-like struct.
/// @throws Error  Always; reconstructed from `me.identifier` / `me.message`.
[[noreturn]] void rethrowStruct(const Value &me);

/// @brief Throw on a false condition (`assert(...)`).
///
/// Forms:
/// - `assert(cond)`                      → generic message on failure
/// - `assert(cond, msg, arg1, ...)`      → sprintf-formatted message
/// - `assert(cond, id, msg, arg1, ...)`  → identifier + formatted message
///   (`id` must contain `:`)
/// - `assert(cond, MException-struct)`   → use struct fields
///
/// Returns normally if `cond` is non-zero.
///
/// @param args  Arguments per the forms above.
/// @throws Error  Condition is false (form-dependent identifier / message).
void assertCond(keel::ArrayRef<Value> args);


/// @}
} // namespace numkit::runtime
