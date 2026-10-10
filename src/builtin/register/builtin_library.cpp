// src/builtin/register/builtin_library.cpp
//
// Registration hub for the Standard Builtin Library on an Engine instance.

#include <numkit/builtin/library.hpp>

namespace numkit::builtin::detail {

void register_ops(Engine &engine);
void register_elfun(Engine &engine);
void register_elmat(Engine &engine);
void register_matfun(Engine &engine);
void register_datafun(Engine &engine);
void register_specfun(Engine &engine);
void register_polyfun(Engine &engine);
void register_strfun(Engine &engine);
void register_timefun(Engine &engine);
void register_datatypes(Engine &engine);
void register_iofun(Engine &engine);
void register_general(Engine &engine);
void register_lang(Engine &engine);

} // namespace numkit::builtin::detail

namespace numkit {

void BuiltinLibrary::install(Engine &engine) {
    builtin::detail::register_ops(engine);
    builtin::detail::register_elfun(engine);
    builtin::detail::register_elmat(engine);
    builtin::detail::register_matfun(engine);
    builtin::detail::register_datafun(engine);
    builtin::detail::register_specfun(engine);
    builtin::detail::register_polyfun(engine);
    builtin::detail::register_strfun(engine);
    builtin::detail::register_timefun(engine);
    builtin::detail::register_datatypes(engine);
    builtin::detail::register_iofun(engine);
    builtin::detail::register_general(engine);
    builtin::detail::register_lang(engine);
}

void BuiltinLibrary::registerWorkspaceBuiltins(Engine &) {
    // Handled modularly via register_general, register_datatypes, register_timefun, etc.
}

} // namespace numkit
