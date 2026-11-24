// bundle/src/register/io/paths_reg.cpp
// CallContext adapters for io.paths filesep/fullfile/fileparts/tempdir/tempname.
// Compute is Engine-free in toolboxes/io/src/paths/paths.cpp; these bridge via
// engine.fsContext()/resource(). IoLibrary::install registers them by name.
#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/io/paths/paths.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::io::detail {

void filesep_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 0, "filesep");
    outs[0] = filesep(ctx.engine->allocator());
}

void fullfile_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty()) {
        outs[0] = Value::matrix(1, 0, ValueType::CHAR, ctx.engine->allocator());
        return;
    }
    keel::Array<keel::String> parts;
    parts.reserve(args.size());
    for (const auto &a : args) {
        if (!a.isChar() && !a.isString())
            throw Error("fullfile: all arguments must be strings",
                         0, 0, "fullfile", "", "numkit:fullfile:badArg");
        parts.push_back(a.toString());
    }
    outs[0] = fullfile(keel::ArrayRef<keel::String>(parts.data(), parts.size()), ctx.engine->allocator());
}

void fileparts_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "fileparts");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("fileparts: requires a string path",
                     0, 0, "fileparts", "", "numkit:fileparts:nargin");
    const keel::String path = args[0].toString();
    auto [folder, name, ext] = fileparts(path, ctx.engine->allocator());
    outs[0] = keel::move(folder);
    if (nargout > 1) outs[1] = keel::move(name);
    if (nargout > 2) outs[2] = keel::move(ext);
}

void tempdir_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 0, "tempdir");
    outs[0] = tempdir(coreEngine(ctx).fsContext(), ctx.engine->allocator());
}

void tempname_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 1, "tempname");
    outs[0] = tempname(coreEngine(ctx).fsContext(), ctx.engine->allocator());
}

// ── genpath: recursive path string from a directory tree ────────────
// MATLAB: recursive DFS; skips @class folders, +package folders,
// `private`, and hidden dot-directories (.git etc.). Returns
// "root;root/sub1;root/sub1/sub2;..." as a semicolon-joined string.
namespace {
void genpathCollect(Engine &eng, keel::StringRef dir, keel::String &out)
{
    if (!out.empty())
        out.push_back(';');
    out.append(dir);
    try {
        auto rp = eng.resolvePath(dir);
        if (!rp.fs) return;
        auto entries = rp.fs->listDir(rp.path);
        for (const auto &e : entries) {
            if (!e.isDirectory || e.name.empty()) continue;
            // MATLAB genpath filters: @class, +package, private, .hidden
            if (e.name[0] == '@' || e.name[0] == '+' || e.name[0] == '.'
                || e.name == "private")
                continue;
            keel::String sub(dir);
            sub.push_back('/');
            sub.append(e.name);
            genpathCollect(eng, sub, out);
        }
    } catch (...) {
        // Path not listable — just include the root.
    }
}
} // namespace

void genpath_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "genpath");
    if (!args[0].isChar())
        throw Error("genpath: requires a string directory name",
                     0, 0, "genpath", "", "numkit:genpath:nargin");
    keel::String result;
    const keel::String root = args[0].toString();
    genpathCollect(coreEngine(ctx), root, result);
    outs[0] = Value::fromString(result, ctx.engine->allocator());
}

} // namespace numkit::io::detail
