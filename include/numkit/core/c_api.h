/* include/numkit/core/c_api.h
 *
 * Self-contained C-ABI for the Full NumKit Engine (`numkit/numkit`).
 *
 * Provides full MATLAB-compatible `.m` source evaluation (`numkit_engine_eval`),
 * workspace variable read/write, dynamic toolbox plugin loading, host callback
 * registration, and both O(1) pre-resolved (`resolve`/`invoke`) and dynamic (`call`)
 * builtin/toolbox invocation.
 */
#ifndef NUMKIT_CORE_C_API_H
#define NUMKIT_CORE_C_API_H

#include <numkit/value/c_api.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(NUMKIT_C_BUILD)
#    define NUMKIT_ENGINE_API __declspec(dllexport)
#  elif defined(NUMKIT_C_STATIC)
#    define NUMKIT_ENGINE_API
#  else
#    define NUMKIT_ENGINE_API __declspec(dllimport)
#  endif
#else
#  if defined(__GNUC__) && __GNUC__ >= 4
#    define NUMKIT_ENGINE_API __attribute__((visibility("default")))
#  else
#    define NUMKIT_ENGINE_API
#  endif
#endif

/* Opaque handle to a `numkit::StandardEngine` session */
typedef struct numkit_engine_ctx_s numkit_engine_ctx;

/* Create a new isolated Full Engine session with standard builtins installed. */
NUMKIT_ENGINE_API numkit_engine_ctx *numkit_engine_create(numkit_error *err);

/* Destroy an engine session and release all resources. */
NUMKIT_ENGINE_API void numkit_engine_destroy(numkit_engine_ctx *ctx);

/* Seed the engine's random number stream (MATLAB `rng(seed)` semantics). */
NUMKIT_ENGINE_API void numkit_engine_seed_rng(numkit_engine_ctx *ctx, uint64_t seed);

/* Add a directory to search when resolving dynamic toolbox plugins. */
NUMKIT_ENGINE_API void numkit_engine_add_plugin_search_dir(numkit_engine_ctx *ctx,
                                                           const char *dir_path);

/* Load a dynamic toolbox plugin by short name (e.g. "signal") or explicit file path. */
NUMKIT_ENGINE_API int numkit_engine_load_plugin(numkit_engine_ctx *ctx,
                                                const char *name_or_path,
                                                numkit_error *err);

/* Register a host-language callback (`numkit_host_fn`) into the engine. */
NUMKIT_ENGINE_API int numkit_engine_register_fn(numkit_engine_ctx *ctx,
                                                const char *ns,
                                                const char *name,
                                                numkit_host_fn fn,
                                                void *user_data,
                                                numkit_error *err);

/* Resolve a function (`ns` may be NULL/"") into an O(1) pointer (`NULL` if not found). */
NUMKIT_ENGINE_API const numkit_fn *numkit_engine_resolve(numkit_engine_ctx *ctx,
                                                         const char *ns,
                                                         const char *name);

/* Invoke a pre-resolved function pointer directly with zero string lookup overhead. */
NUMKIT_ENGINE_API int numkit_engine_invoke(numkit_engine_ctx *ctx,
                                           const numkit_fn *fn,
                                           const numkit_val *const *args,
                                           size_t nargs,
                                           size_t nargout,
                                           numkit_val **outs,
                                           numkit_error *err);

/* Dynamically invoke builtin, toolbox function, or object method `name`. */
NUMKIT_ENGINE_API int numkit_engine_call(numkit_engine_ctx *ctx,
                                         const char *ns,
                                         const char *name,
                                         const numkit_val *const *args,
                                         size_t nargs,
                                         size_t nargout,
                                         numkit_val **outs,
                                         numkit_error *err);

/* Evaluate MATLAB-compatible `.m` source code and return the result value. */
NUMKIT_ENGINE_API numkit_val *numkit_engine_eval(numkit_engine_ctx *ctx,
                                                 const char *code,
                                                 numkit_error *err);

/* Read or write a variable in the engine's base workspace. */
NUMKIT_ENGINE_API numkit_val *numkit_engine_workspace_get(numkit_engine_ctx *ctx,
                                                          const char *name,
                                                          numkit_error *err);
NUMKIT_ENGINE_API int numkit_engine_workspace_set(numkit_engine_ctx *ctx,
                                                  const char *name,
                                                  const numkit_val *val,
                                                  numkit_error *err);

/* Format a value or class object (`sym`, `tf`, user `classdef`) to UTF-8 text. */
NUMKIT_ENGINE_API size_t numkit_engine_format(numkit_engine_ctx *ctx,
                                              const numkit_val *v,
                                              char *buf,
                                              size_t buf_cap);

#ifdef __cplusplus
}
#endif

#endif /* NUMKIT_CORE_C_API_H */
