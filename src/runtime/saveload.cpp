// runtime/src/saveload.cpp
//
// Workspace-persistence builtins (save / load). Dispatches between the
// ASCII backend (saveload_ascii.cpp) and the binary .mat backend
// (saveload_mat.cpp) based on flags and the filename's extension —
// matching MATLAB's defaults (binary .mat unless `-ascii` is given).

#include <keel/arena.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/runtime/saveload.hpp>

#include <numkit/core/engine.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>
#include <numkit/sys/vfs.hpp>

#include <cctype>

namespace numkit::runtime {

// ── Backends ─────────────────────────────────────────────────────────

void saveAscii(Engine &engine, Environment &env,
               keel::StringRef filename,
               keel::ArrayRef<keel::String> varnames);

void loadAscii(Engine &engine, Environment &env,
               keel::StringRef filename,
               keel::usize nargout, keel::MutArrayRef<Value> outs);

void saveMat(Engine &engine, Environment &env,
             keel::StringRef filename,
             keel::ArrayRef<keel::String> varnames,
             int matVersion);

void loadMat(Engine &engine, Environment &env,
             keel::StringRef filename,
             keel::usize nargout, keel::MutArrayRef<Value> outs);

namespace {

bool endsWithIgnoreCase(keel::StringRef s, keel::StringRef suffix)
{
    if (s.size() < suffix.size()) return false;
    for (keel::usize i = 0; i < suffix.size(); ++i) {
        char a = s[s.size() - suffix.size() + i];
        char b = suffix[i];
        if (std::tolower(static_cast<unsigned char>(a))
            != std::tolower(static_cast<unsigned char>(b)))
            return false;
    }
    return true;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
// save / load Entry Points
// ════════════════════════════════════════════════════════════════════════

void save(Engine &engine, Environment &env, keel::ArrayRef<Value> args)
{
    if (args.empty() || !args[0].isChar())
        throw Error("save: filename required");
    engine.syncFrameToEnv(&env);
    keel::String filename = args[0].toString();

    // Three-way mode pick: explicit flag wins; otherwise extension hint;
    // otherwise MATLAB default (binary mat).
    enum class Mode { Auto, Ascii, Mat };
    Mode mode = Mode::Auto;
    // matVersion follows MATLAB-flag convention: 4 = MAT4, 5/6 = MAT5,
    // 7 = MAT5 + zlib. `-mat` alone (no version suffix) defaults to 5.
    // Whichever `-vN` appears last wins (mirrors MATLAB).
    int matVersion = 5;

    keel::ArenaTemp scratch = keel::get_scratch(engine.allocator());
    keel::Array<keel::String> varnames(scratch.allocator());
    for (keel::usize i = 1; i < args.size(); ++i) {
        if (!args[i].isChar())
            continue;
        keel::String s = args[i].toString();
        if (s == "-ascii") { mode = Mode::Ascii; continue; }
        if (s == "-mat") { mode = Mode::Mat; continue; }
        if (s == "-v4") { mode = Mode::Mat; matVersion = 4; continue; }
        if (s == "-v6") { mode = Mode::Mat; matVersion = 6; continue; }
        if (s == "-v7") { mode = Mode::Mat; matVersion = 7; continue; }
        if (s == "-v7.3")
            throw Error("save: -v7.3 (HDF5) is not supported in this build");
        if (s == "-append" || s == "-nocompression" || s == "-struct") {
            // accepted no-ops for now; -append is binary-only in MATLAB.
            if (s == "-append") mode = Mode::Mat;
            continue;
        }
        if (!s.empty() && s.front() == '-')
            throw Error(keel::format("save: unsupported flag '{}'", s));
        varnames.push_back(keel::move(s));
    }

    if (mode == Mode::Auto) {
        if (endsWithIgnoreCase(filename.as_ref(), ".mat")) {
            mode = Mode::Mat;
        } else if (filename.find('.') == keel::kInvalidIndex) {
            filename.append(".mat");
            mode = Mode::Mat;
        } else {
            mode = Mode::Ascii;
        }
    }

    if (mode == Mode::Mat) {
        saveMat(engine, env, filename.as_ref(), varnames.as_ref(), matVersion);
    } else {
        saveAscii(engine, env, filename.as_ref(), varnames.as_ref());
    }
}

void load(Engine &engine, Environment &env, keel::ArrayRef<Value> args,
          keel::usize nargout, keel::MutArrayRef<Value> outs)
{
    if (args.empty() || !args[0].isChar())
        throw Error("load: filename required");
    keel::String filename = args[0].toString();

    enum class Mode { Auto, Ascii, Mat };
    Mode mode = Mode::Auto;
    for (keel::usize i = 1; i < args.size(); ++i) {
        if (!args[i].isChar()) continue;
        keel::String s = args[i].toString();
        if (s == "-ascii") { mode = Mode::Ascii; continue; }
        if (s == "-mat" || s == "-v4" || s == "-v6" || s == "-v7") {
            mode = Mode::Mat; continue;
        }
        if (s == "-v7.3")
            throw Error("load: -v7.3 (HDF5) is not supported in this build");
        if (!s.empty() && s.front() == '-')
            throw Error(keel::format("load: unsupported flag '{}'", s));
    }

    auto fileExists = [&](keel::StringRef cand) -> bool {
        try {
            auto rp = engine.resolvePath(cand);
            if (!rp.fs || !rp.fs->exists(rp.path.as_ref())) return false;
            auto st = rp.fs->stat(rp.path.as_ref());
            return st && st->kind == FileStat::Kind::File;
        } catch (...) {
            return false;
        }
    };

    const bool hasExt = filename.find('.') != keel::kInvalidIndex;
    keel::String withMat = keel::format("{}.mat", filename);
    if (!hasExt && fileExists(withMat.as_ref())) {
        filename = keel::move(withMat);
    } else if (!fileExists(filename.as_ref())) {
        if (!endsWithIgnoreCase(filename.as_ref(), ".mat") && fileExists(withMat.as_ref())) {
            filename = keel::move(withMat);
        } else {
            for (const auto &dir : engine.path()) {
                keel::String prefix = dir.clone();
                if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\')
                    prefix.push_back('/');
                if (!endsWithIgnoreCase(filename.as_ref(), ".mat")) {
                    keel::String candMat = keel::format("{}{}.mat", prefix, filename);
                    if (fileExists(candMat.as_ref())) {
                        filename = keel::move(candMat);
                        break;
                    }
                }
                keel::String cand = keel::format("{}{}", prefix, filename);
                if (fileExists(cand.as_ref())) {
                    filename = keel::move(cand);
                    break;
                }
            }
        }
    }

    if (mode == Mode::Auto)
        mode = endsWithIgnoreCase(filename.as_ref(), ".mat") ? Mode::Mat : Mode::Ascii;

    if (mode == Mode::Mat) {
        loadMat(engine, env, filename.as_ref(), nargout, outs);
    } else {
        loadAscii(engine, env, filename.as_ref(), nargout, outs);
    }
}

// ════════════════════════════════════════════════════════════════════════
// Adapters
// ════════════════════════════════════════════════════════════════════════

namespace detail {

void save_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    (void)nargout;
    (void)outs;
    save(*ctx.engine, *ctx.env, args);
}

void load_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    load(*ctx.engine, *ctx.env, args, nargout, outs);
}

} // namespace detail

} // namespace numkit::runtime
