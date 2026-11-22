#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/fs.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/core/figure_manager.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/core/vm.hpp>
#include <numkit/runtime/runtime.hpp>
#include <numkit/graphics/graphics_handle.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <numkit/core/build_info.hpp>

namespace numkit::builtin::detail {
void addpath_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void cd_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void clc_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void delete_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void dir_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void format_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void formatteddisplaytext_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void freqspace_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void home_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void inmem_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void ls_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void mkdir_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void optimget_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void optimset_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void path_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void pathsep_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void pwd_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void rehash_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void rmdir_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void rmpath_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void which_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
}

namespace numkit::builtin::detail {

using namespace ::numkit::builtin;

namespace {

keel::String getActualCwd(Engine &engine, keel::Allocator *mr)
{
    keel::String c = engine.cwd().clone(mr);
    if (c.empty()) {
        try {
            auto rp = engine.resolvePath(".");
            if (rp.fs) c = rp.fs->cwd();
        } catch (...) {}
        if (c.empty()) {
            if (auto *fs = engine.findVirtualFS("native"))
                c = fs->cwd();
        }
    }
    return c;
}

bool globMatch(keel::StringRef pat, keel::StringRef str)
{
    keel::usize p = 0, s = 0, starIdx = keel::kInvalidIndex, matchIdx = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++p; ++s;
        } else if (p < pat.size() && pat[p] == '*') {
            starIdx = p++;
            matchIdx = s;
        } else if (starIdx != keel::kInvalidIndex) {
            p = starIdx + 1;
            s = ++matchIdx;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

keel::usize lastSepPos(keel::StringRef s)
{
    for (keel::usize i = s.size(); i > 0; --i) {
        if (s[i - 1] == '/' || s[i - 1] == '\\') return i - 1;
    }
    return keel::kInvalidIndex;
}

void checkFsKind(keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs,
                 CallContext &ctx, const char *fnName, FileStat::Kind want)
{
    argcheck::checkNargin(args.size(), 1, 1, fnName);
    auto *mr = ctx.engine->allocator();
    auto checkOne = [&](keel::StringRef p) -> bool {
        try {
            auto rp = ctx.engine->resolvePath(p);
            if (!rp.fs) return false;
            auto st = rp.fs->stat(rp.path);
            return st && st->kind == want;
        } catch (...) {
            return false;
        }
    };
    if (args[0].isChar()) {
        outs[0] = Value::logicalScalar(checkOne(args[0].toString()), mr);
    } else if (args[0].isString()) {
        Value res = Value::matrix(args[0].dims().rows(), args[0].dims().cols(), ValueType::LOGICAL, mr);
        for (keel::usize i = 0; i < args[0].numel(); ++i)
            res.logicalDataMut()[i] = checkOne(args[0].stringElem(i)) ? 1 : 0;
        outs[0] = std::move(res);
    } else if (args[0].isCell()) {
        Value res = Value::matrix(args[0].dims().rows(), args[0].dims().cols(), ValueType::LOGICAL, mr);
        for (keel::usize i = 0; i < args[0].numel(); ++i)
            res.logicalDataMut()[i] = checkOne(args[0].cellAt(i).toString()) ? 1 : 0;
        outs[0] = std::move(res);
    } else {
        outs[0] = Value::logicalScalar(false, mr);
    }
}

void clc_impl_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    ctx.engine->outputText("__CLEAR__\n");
    outs[0] = Value();
}

void web_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (!args.empty() && !args[0].isChar() && !args[0].isString())
        throw Error("web: requires a string URL", 0, 0, "web", "",
                    "numkit:web:nargin");
    if (!args.empty())
        ctx.engine->outputText(
            keel::format("Warning: web: no browser in the nk CLI — URL '{}' not opened.\n",
                         args[0].toString()));
    else
        ctx.engine->outputText(
            "Warning: web: no browser in the nk CLI.\n");
    outs[0] = Value::scalar(0.0);
}

void noop_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &)
{
    outs[0] = Value();
}

void which_impl_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                    CallContext &ctx)
{
    if (args.empty())
        throw std::runtime_error("which requires a name argument");
    auto *mr = ctx.engine->allocator();
    keel::String qname = (args[0].isChar() || args[0].isString())
                             ? args[0].toString()
                             : keel::String(mr);
    keel::StringRef baseName = qname.as_ref();
    if (baseName.size() > 2 && baseName.ends_with(".m"))
        baseName = baseName.substr(0, baseName.size() - 2);
    auto *env = ctx.env;

    keel::String res(mr);
    bool isFound = true;
    if (env->getLocal(baseName)
        || (env->isGlobal(baseName) && env->globalsEnv()
            && env->globalsEnv()->get(baseName)))
        res.assign("variable");
    else if (ctx.engine->hasUserFunction(baseName))
        res = keel::format("{} is a user-defined function.", baseName);
    else if (ctx.engine->hasExternalFunction(baseName))
        res = keel::format("built-in ({})", baseName);
    else {
        auto *ext = ctx.engine->findExternal(baseName, env);
        if (ext) {
            auto qualified = ctx.engine->bareNameSource(baseName);
            if (qualified.empty())
                res = keel::format("built-in ({})", baseName);
            else
                res.assign(qualified);
        } else {
            bool found = false;
            const bool hasNonMExt = (qname.find('.') != keel::kInvalidIndex && qname.as_ref() == baseName);
            const keel::String candName = hasNonMExt ? qname.clone(mr) : keel::format("{}.m", baseName);
            try {
                auto rp = ctx.engine->resolvePath(candName);
                if (rp.fs && rp.fs->exists(rp.path)) {
                    res.assign(rp.path);
                    found = true;
                }
            } catch (...) {}
            if (!found) {
                for (const auto &dir : ctx.engine->path()) {
                    keel::String p = dir.clone(mr);
                    if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
                    p.append(candName);
                    try {
                        auto rp = ctx.engine->resolvePath(p);
                        if (rp.fs && rp.fs->exists(rp.path)) {
                            res.assign(rp.path);
                            found = true;
                            break;
                        }
                    } catch (...) {}
                }
            }
            if (!found) {
                isFound = false;
                res = keel::format("'{}' not found.", qname);
            }
        }
    }

    if (nargout == 0) {
        res.push_back('\n');
        ctx.engine->outputText(res);
        outs[0] = Value();
    } else {
        outs[0] = Value::fromString(isFound ? res.as_ref() : keel::StringRef(""), mr);
    }
}

void addpath_impl_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                      CallContext &ctx)
{
    for (const auto &a : args) {
        if (a.isChar()) ctx.engine->addPath(a.toString());
    }
    outs[0] = Value();
}

void rmpath_impl_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                     CallContext &ctx)
{
    for (const auto &a : args) {
        if (a.isChar()) ctx.engine->rmPath(a.toString());
    }
    outs[0] = Value();
}

void path_impl_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                   CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        const auto &paths = ctx.engine->path();
        if (nargout == 0) {
            keel::String out(mr);
            for (const auto &p : paths) {
                out.append(p);
                out.push_back('\n');
            }
            ctx.engine->outputText(out);
            outs[0] = Value();
        } else {
            // Return as a single newline-joined char vector
            keel::String out(mr);
            for (keel::usize i = 0; i < paths.size(); ++i) {
                if (i) out.push_back('\n');
                out.append(paths[i]);
            }
            outs[0] = Value::fromString(out, mr);
        }
        return;
    }
    // path(p1, p2, ...) — replace path with the given list.
    auto current = ctx.engine->path().clone(mr);
    for (const auto &p : current) ctx.engine->rmPath(p);
    for (const auto &a : args) {
        if (a.isChar()) ctx.engine->addPath(a.toString());
    }
    outs[0] = Value();
}

void rehash_impl_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                     CallContext &ctx)
{
    ctx.engine->rehashMFiles();
    outs[0] = Value();
}

void pwd_impl_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    keel::String c = getActualCwd(*ctx.engine, mr);
    outs[0] = Value::fromString(c, mr);
}

void cd_impl_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                 CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    if (args.empty()) {
        keel::String c = getActualCwd(*ctx.engine, mr);
        if (nargout == 0) {
            c.push_back('\n');
            ctx.engine->outputText(c);
            outs[0] = Value();
        } else {
            outs[0] = Value::fromString(c, mr);
        }
        return;
    }
    if (!args[0].isChar())
        throw std::runtime_error("cd: directory must be a string");
    keel::String target = args[0].toString();
    auto rp = ctx.engine->resolvePath(target);
    if (!rp.fs)
        throw std::runtime_error(keel::format("cd: cannot resolve '{}'", target).c_str());
    auto st = rp.fs->stat(rp.path);
    if (!st || st->kind != FileStat::Kind::Directory)
        throw std::runtime_error(keel::format("cd: not a directory: {}", target).c_str());
    keel::String prev = getActualCwd(*ctx.engine, mr);
    ctx.engine->setCwd(rp.path);
    // With output, return the previous cwd.
    if (nargout > 0)
        outs[0] = Value::fromString(prev, mr);
    else
        outs[0] = Value();
}

void version_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                 CallContext &ctx)
{
    outs[0] = Value::fromString(numkit::buildTimestamp(), ctx.engine->allocator());
}

void mkdir_impl_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                    CallContext &ctx)
{
    if (args.empty() || !args[0].isChar())
        throw std::runtime_error("mkdir requires a directory name");
    auto *mr = ctx.engine->allocator();
    keel::String p = args[0].toString();
    if (args.size() >= 2 && args[1].isChar()) {
        // mkdir(parent, name) — concatenate.
        keel::String name = args[1].toString();
        if (!p.empty() && p.back() != '/' && p.back() != '\\')
            p.push_back('/');
        p.append(name);
    }
    auto rp = ctx.engine->resolvePath(p);
    if (!rp.fs)
        throw std::runtime_error(keel::format("mkdir: cannot resolve '{}'", p).c_str());
    bool ok = true;
    keel::String msg(mr);
    try {
        rp.fs->mkdir(rp.path);
    } catch (const std::exception &e) {
        ok = false;
        msg.assign(e.what());
    }
    if (nargout > 0)
        outs[0] = Value::logicalScalar(ok, mr);
    else if (!ok)
        throw std::runtime_error(keel::format("mkdir: {}", msg).c_str());
    else
        outs[0] = Value();
    if (nargout > 1)
        outs[1] = Value::fromString(msg, mr);
}

void rmdir_impl_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                    CallContext &ctx)
{
    if (args.empty() || !args[0].isChar())
        throw std::runtime_error("rmdir requires a directory name");
    auto *mr = ctx.engine->allocator();
    keel::String p = args[0].toString();
    auto rp = ctx.engine->resolvePath(p);
    if (!rp.fs)
        throw std::runtime_error(keel::format("rmdir: cannot resolve '{}'", p).c_str());
    bool ok = true;
    keel::String msg(mr);
    try {
        rp.fs->rmdir(rp.path);
    } catch (const std::exception &e) {
        ok = false;
        msg.assign(e.what());
    }
    if (nargout > 0)
        outs[0] = Value::logicalScalar(ok, mr);
    else if (!ok)
        throw std::runtime_error(keel::format("rmdir: {}", msg).c_str());
    else
        outs[0] = Value();
    if (nargout > 1)
        outs[1] = Value::fromString(msg, mr);
}

void delete_impl_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                     CallContext &ctx)
{
    auto &fm = ctx.engine->figureManager();
    bool anyFigureClosed = false;
    for (const auto &a : args) {
        const keel::i32 hid = graphicsHandleIdOf(a);
        if (hid >= 0) {
            auto *rec = fm.findHandle(hid);
            if (!rec)
                continue;   // already deleted — idempotent
            if (rec->type == "figure") {
                fm.closeFigureNotify(rec->figureId);
                anyFigureClosed = true;
            } else if (rec->type == "axes") {
                fm.removeAxes(rec->figureId, rec->axesIndex);
                fm.markFigureModified(rec->figureId);
            } else {
                fm.deleteHandle(hid);
            }
            continue;
        }
        if (a.isNumeric() || a.isEmpty())
            continue;
        if (!a.isChar() && !a.isString())
            throw std::runtime_error("delete: filename must be a string");
        keel::String p = a.toString();
        auto rp = ctx.engine->resolvePath(p);
        if (!rp.fs)
            throw std::runtime_error(keel::format("delete: cannot resolve '{}'", p).c_str());
        rp.fs->unlink(rp.path);
    }
    if (!anyFigureClosed)
        fm.emitModified();
    outs[0] = Value();
}

void dir_impl_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    keel::String target = args.empty() ? keel::String(".", mr)
                                       : args[0].toString();
    keel::Array<DirEntry> entries(mr);
    Engine::ResolvedPath rp;
    const bool hasWild = target.find('*') != keel::kInvalidIndex
                      || target.find('?') != keel::kInvalidIndex;
    if (hasWild) {
        keel::usize sep = lastSepPos(target);
        keel::StringRef dirPart = (sep == keel::kInvalidIndex) ? keel::StringRef(".")
                                : (sep == 0 ? keel::StringRef("/") : target.as_ref().substr(0, sep));
        keel::StringRef patPart = (sep == keel::kInvalidIndex) ? target.as_ref() : target.as_ref().substr(sep + 1);
        rp = ctx.engine->resolvePath(dirPart);
        if (!rp.fs)
            throw std::runtime_error(keel::format("dir: cannot resolve '{}'", target).c_str());
        auto all = rp.fs->listDir(rp.path);
        for (auto &e : all) {
            if ((e.name == "." || e.name == "..") && (patPart.empty() || patPart[0] != '.'))
                continue;
            if (globMatch(patPart, e.name))
                entries.push_back(keel::move(e));
        }
        std::sort(entries.begin(), entries.end(),
                  [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    } else {
        rp = ctx.engine->resolvePath(target);
        if (!rp.fs)
            throw std::runtime_error(keel::format("dir: cannot resolve '{}'", target).c_str());
        auto st = rp.fs->stat(rp.path);
        if (st && st->kind == FileStat::Kind::File) {
            DirEntry e;
            keel::StringRef pv = rp.path.as_ref();
            keel::usize sep = lastSepPos(pv);
            e.name = keel::String(
                (sep == keel::kInvalidIndex) ? pv : pv.substr(sep + 1),
                mr);
            e.isDirectory = false;
            entries.push_back(keel::move(e));
        } else {
            entries = rp.fs->listDir(rp.path);
        }
    }

    if (nargout == 0) {
        keel::String out(mr);
        for (const auto &e : entries) {
            out.append(e.name);
            if (e.isDirectory) out.push_back('/');
            out.push_back('\n');
        }
        ctx.engine->outputText(out);
        outs[0] = Value();
        return;
    }

    if (entries.empty()) {
        outs[0] = Value::structure(mr);
        return;
    }
    Value arr = Value::structArray(entries.size(), 1, mr);
    for (keel::usize i = 0; i < entries.size(); ++i) {
        auto &fields = arr.structArrayElem(i);
        fields.put(keel::String("name", mr),    Value::fromString(entries[i].name, mr));
        fields.put(keel::String("folder", mr),  Value::fromString(rp.path, mr));
        fields.put(keel::String("isdir", mr),   Value::logicalScalar(entries[i].isDirectory, mr));
        keel::String full = rp.path.clone(mr);
        if (!full.empty() && full.back() != '/' && full.back() != '\\')
            full.push_back('/');
        full.append(entries[i].name);
        auto est = rp.fs->stat(full);
        fields.put(keel::String("bytes", mr),   Value::scalar(est ? keel::f64(est->size) : 0.0, mr));
        fields.put(keel::String("datenum", mr), Value::scalar(est ? keel::f64(est->mtime) : 0.0, mr));
        fields.put(keel::String("date", mr),    Value::fromString("", mr));
    }
    outs[0] = std::move(arr);
}

void ls_impl_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                 CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    keel::String target = args.empty() ? keel::String(".", mr)
                                       : args[0].toString();
    auto rp = ctx.engine->resolvePath(target);
    if (!rp.fs)
        throw std::runtime_error(keel::format("ls: cannot resolve '{}'", target).c_str());
    auto entries = rp.fs->listDir(rp.path);
    if (nargout == 0) {
        keel::String out(mr);
        for (const auto &e : entries) {
            out.append(e.name);
            out.push_back('\n');
        }
        ctx.engine->outputText(out);
        outs[0] = Value();
    } else {
        keel::String out(mr);
        for (keel::usize i = 0; i < entries.size(); ++i) {
            if (i) out.append("  ");
            out.append(entries[i].name);
        }
        outs[0] = Value::fromString(out, mr);
    }
}

void pathsep_impl_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs,
                      CallContext &ctx)
{
    const char sep[1] = {keel::fs_path_list_sep()};
    outs[0] = Value::fromString(keel::StringRef(sep, 1), ctx.engine->allocator());
}

void formatteddisplaytext_impl_reg(keel::ArrayRef<Value> args, keel::usize,
                                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        throw std::runtime_error("formatteddisplaytext requires 1 argument");
    outs[0] = Value::fromString(args[0].formatDisplay(""), ctx.engine->allocator());
}

void format_impl_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                     CallContext &)
{
    if (!args.empty()) {
        if (!args[0].isChar() && !args[0].isString())
            throw std::runtime_error("format: argument must be a string");
        static constexpr keel::StringRef kKnown[] = {
            "short", "long", "compact", "loose",
            "shortg", "longg", "shorte", "longe",
            "shorteng", "longeng", "rat", "hex", "bank", "+", "default"
        };
        const keel::String s = args[0].toString();
        if (!s.is_one_of_ignore_ascii_case(keel::ArrayRef<keel::StringRef>(kKnown)))
            throw std::runtime_error(
                keel::format("format: unrecognised format spec '{}'", s).c_str());
    }
    // No-op: nk's display already runs at full precision.
    outs[0] = Value();
}

void optimset_impl_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                       CallContext &ctx)
{
    auto *mr = ctx.engine->allocator();
    auto s = Value::structure(mr);
    s.field("Display")     = Value::fromString("notify", mr);
    s.field("MaxFunEvals") = Value::scalar(1000.0, mr);
    s.field("MaxIter")     = Value::scalar(500.0, mr);
    s.field("TolFun")      = Value::scalar(1e-6, mr);
    s.field("TolX")        = Value::scalar(1e-6, mr);
    for (keel::usize i = 0; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString())
            throw std::runtime_error("optimset: option name must be a string");
        s.field(args[i].toString()) = args[i + 1];
    }
    outs[0] = std::move(s);
}

void optimget_impl_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                       CallContext &)
{
    if (args.size() < 2)
        throw std::runtime_error("optimget requires (options, name[, default])");
    const Value &opts = args[0];
    const keel::String name = args[1].toString();
    if (opts.isStruct() && opts.hasField(name)) {
        outs[0] = opts.field(name);
        return;
    }
    if (args.size() >= 3) outs[0] = args[2];
    else outs[0] = Value();
}

void freqspace_impl_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (args.empty())
        throw std::runtime_error("freqspace requires (n[, 'whole'])");
    keel::usize n_rows = 0, n_cols = 0;
    const Value &a0 = args[0];
    if (a0.numel() == 1) {
        n_rows = n_cols = argcheck::arrayDim(a0, "freqspace");
    } else if (a0.numel() == 2) {
        argcheck::arrayDimReal(a0, "freqspace");
        n_rows = argcheck::arrayDim(a0.elemAsDouble(0), "freqspace");
        n_cols = argcheck::arrayDim(a0.elemAsDouble(1), "freqspace");
    } else {
        throw std::runtime_error("freqspace: N must be a scalar or 2-element vector");
    }
    bool whole = false;
    if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
        keel::String s = args[1].toString();
        s.to_ascii_lower_in_place();
        whole = (s == "whole");
    }
    auto *mr = ctx.engine->allocator();

    auto whole_vec = [&](keel::usize n) {
        auto v = Value::matrix(1, n, ValueType::DOUBLE, mr);
        if (n == 0) return v;
        keel::f64 *d = v.doubleDataMut();
        const keel::f64 step = 2.0 / keel::as<keel::f64>(n);
        for (keel::usize i = 0; i < n; ++i)
            d[i] = step * keel::as<keel::f64>(i);
        return v;
    };
    auto centered_vec = [&](keel::usize n) {
        auto v = Value::matrix(1, n, ValueType::DOUBLE, mr);
        if (n == 0) return v;
        keel::f64 *d = v.doubleDataMut();
        const keel::i64 start = (n % 2 == 0) ? -keel::as<keel::i64>(n)
                                             : -keel::as<keel::i64>(n) + 1;
        for (keel::usize i = 0; i < n; ++i)
            d[i] = keel::as<keel::f64>(start + 2 * keel::as<keel::i64>(i))
                 / keel::as<keel::f64>(n);
        return v;
    };
    auto half_vec = [&](keel::usize n) {
        if (n == 0)
            return Value::matrix(1, 0, ValueType::DOUBLE, mr);
        keel::usize m;
        keel::f64 last;
        if (n % 2 == 0) { m = n / 2 + 1; last = 1.0; }
        else            { m = (n + 1) / 2;
                          last = 1.0 - 1.0 / keel::as<keel::f64>(n); }
        auto v = Value::matrix(1, m, ValueType::DOUBLE, mr);
        keel::f64 *d = v.doubleDataMut();
        if (m == 1) d[0] = 0.0;
        else {
            const keel::f64 step = last / keel::as<keel::f64>(m - 1);
            for (keel::usize i = 0; i < m; ++i)
                d[i] = step * keel::as<keel::f64>(i);
        }
        return v;
    };

    if (nargout > 1) {
        if (whole)
            throw std::runtime_error("freqspace: 2-output 'whole' form is not supported");
        outs[0] = centered_vec(n_cols);
        outs[1] = centered_vec(n_rows);
        return;
    }
    if (a0.numel() == 2)
        throw std::runtime_error("freqspace: 2-vec input requires 2 output args");
    outs[0] = whole ? whole_vec(n_rows) : half_vec(n_rows);
}

void isfile_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                CallContext &ctx)
{
    checkFsKind(args, outs, ctx, "isfile", FileStat::Kind::File);
}

void isfolder_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                  CallContext &ctx)
{
    checkFsKind(args, outs, ctx, "isfolder", FileStat::Kind::Directory);
}

void profile_reg(keel::ArrayRef<Value>, keel::usize nargout, keel::MutArrayRef<Value> outs,
                 CallContext &ctx)
{
    if (nargout >= 1) {
        auto *mr = ctx.engine->allocator();
        Value s = Value::structure(mr);
        s.field("FunctionTable") = Value::structArray(0, 1, mr);
        s.field("ProfilerStatus") = Value::fromString("off", mr);
        outs[0] = std::move(s);
    } else {
        outs[0] = Value();
    }
}

void lsline_reg(keel::ArrayRef<Value>, keel::usize nargout, keel::MutArrayRef<Value> outs,
                CallContext &ctx)
{
    if (nargout >= 1) {
        const keel::i32 hid = ctx.engine->figureManager().createHandle(
            "line", "matlab.graphics.chart.primitive.Line");
        outs[0] = makeGraphicsHandleValue(
            "matlab.graphics.chart.primitive.Line", hid, ctx.engine->allocator());
    } else {
        outs[0] = Value();
    }
}

struct GeneralRegEntry {
    const char *name;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
};

constexpr GeneralRegEntry kGeneralFns[] = {
    {"clc",                  &clc_impl_reg},
    {"web",                  &web_reg},
    {"edit",                 &noop_reg},
    {"which",                &which_impl_reg},
    {"addpath",              &addpath_impl_reg},
    {"rmpath",               &rmpath_impl_reg},
    {"path",                 &path_impl_reg},
    {"rehash",               &rehash_impl_reg},
    {"pwd",                  &pwd_impl_reg},
    {"cd",                   &cd_impl_reg},
    {"version",              &version_reg},
    {"mkdir",                &mkdir_impl_reg},
    {"rmdir",                &rmdir_impl_reg},
    {"delete",               &delete_impl_reg},
    {"diary",                &noop_reg},
    {"dir",                  &dir_impl_reg},
    {"ls",                   &ls_impl_reg},
    {"pathsep",              &pathsep_impl_reg},
    {"formatteddisplaytext", &formatteddisplaytext_impl_reg},
    {"format",               &format_impl_reg},
    {"home",                 &clc_impl_reg},
    {"optimset",             &optimset_impl_reg},
    {"optimget",             &optimget_impl_reg},
    {"freqspace",            &freqspace_impl_reg},
    {"isfile",               &isfile_reg},
    {"isfolder",             &isfolder_reg},
    {"echo",                 &noop_reg},
    {"profile",              &profile_reg},
    {"lsline",               &lsline_reg},
};

} // namespace

void register_general(Engine &engine)
{
    for (const auto &entry : kGeneralFns)
        engine.registerFunction(entry.name, entry.fn);
}

} // namespace numkit::builtin::detail
