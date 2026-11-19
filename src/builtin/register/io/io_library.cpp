// toolboxes/io/src/library.cpp
//
// Registration hub for the Data Import and Export library.
// Namespace layout (namespace_design.md §5, §9.4):
//   file_io/   → io.file_io.<fn>   (fopen, fclose, fread, fwrite, ...)
//   text/      → io.text.<fn>      (csvread, csvwrite, ...)
//   paths/     → io.paths.<fn>     (filesep, fullfile, fileparts, ...)
// (save / load moved to runtime — they are workspace runtime,
//  not data import/export, and register bare via registerWorkspaceRuntime.)
// Each function is also aliased into `compat.<fn>` so MATLAB-style
// scripts call them flat via the bare-name resolver.

#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/io/library.hpp>

#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>

namespace numkit::io::detail {
// text/csv.cpp
void csvread_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void csvwrite_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void dlmwrite_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);

// file_io/fileio.cpp
void fopen_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void fclose_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void fgetl_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void fgets_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void feof_reg     (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void ferror_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void ftell_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void fseek_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void frewind_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void fread_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void fwrite_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);

// text/extras.cpp (C1 — modern text helpers)
void fileread_reg    (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void readlines_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void writelines_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void readmatrix_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void writematrix_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void type_reg        (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);

// paths/paths.cpp (C2 — file-name construction)
void filesep_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void fullfile_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void fileparts_reg (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void tempdir_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void tempname_reg  (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
void genpath_reg   (keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
} // namespace numkit::io::detail

namespace numkit {

void IoLibrary::install(Engine &engine)
{
    // io is a MATLAB-mirror library — every function dual-registered
    // (io.<sub>.<name> + compat.<name>).
    auto reg = [&](const char *sub, const char *name, ExternalFunc fn) {
        keel::SmallString<64> ns("io.");
        ns.append(sub);
        engine.registerFunction(ns, name, keel::move(fn));
        // compat.* registration removed — bare-name resolver replaces it
    };

    reg("file_io", "fopen",   &io::detail::fopen_reg);
    reg("file_io", "fclose",  &io::detail::fclose_reg);
    reg("file_io", "fgetl",   &io::detail::fgetl_reg);
    reg("file_io", "fgets",   &io::detail::fgets_reg);
    reg("file_io", "feof",    &io::detail::feof_reg);
    reg("file_io", "ferror",  &io::detail::ferror_reg);
    reg("file_io", "ftell",   &io::detail::ftell_reg);
    reg("file_io", "fseek",   &io::detail::fseek_reg);
    reg("file_io", "frewind", &io::detail::frewind_reg);
    reg("file_io", "fread",   &io::detail::fread_reg);
    reg("file_io", "fwrite",  &io::detail::fwrite_reg);

    reg("text", "csvread",     &io::detail::csvread_reg);
    reg("text", "csvwrite",    &io::detail::csvwrite_reg);
    reg("text", "dlmwrite",    &io::detail::dlmwrite_reg);
    reg("text", "fileread",    &io::detail::fileread_reg);
    reg("text", "readlines",   &io::detail::readlines_reg);
    reg("text", "writelines",  &io::detail::writelines_reg);
    reg("text", "readmatrix",  &io::detail::readmatrix_reg);
    reg("text", "writematrix", &io::detail::writematrix_reg);
    reg("text", "type",        &io::detail::type_reg);

    reg("paths", "filesep",   &io::detail::filesep_reg);
    reg("paths", "fullfile",  &io::detail::fullfile_reg);
    reg("paths", "fileparts", &io::detail::fileparts_reg);
    reg("paths", "tempdir",   &io::detail::tempdir_reg);
    reg("paths", "tempname",  &io::detail::tempname_reg);
    reg("paths", "genpath",   &io::detail::genpath_reg);
}

} // namespace numkit
