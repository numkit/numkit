// toolboxes/io/src/paths/paths.cpp
//
// filesep / fullfile / fileparts / tempdir / tempname.

#include <keel/core.h>
#include <keel/fs.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/time.h>
#include <numkit/io/paths/paths.hpp>

#include <numkit/value/error.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/sys/fs_context.hpp>

#include <atomic>
#include <cstdint>
#include <random>

namespace numkit::io {

namespace {

constexpr char kPathSepChar =
#ifdef _WIN32
    '\\';
#else
    '/';
#endif

bool isPathSep(char c) { return c == '/' || c == '\\'; }

// Strip leading separators from the left edge.
keel::StringRef ltrimSep(keel::StringRef s)
{
    keel::usize start = 0;
    while (start < s.size() && isPathSep(s[start])) ++start;
    return s.substr(start);
}

Value charValue(keel::Allocator *mr, keel::StringRef s)
{
    auto v = Value::matrix(1, s.size(), ValueType::CHAR, mr);
    for (keel::usize i = 0; i < s.size(); ++i)
        v.charDataMut()[i] = s[i];
    return v;
}

template <typename PartT>
Value fullfileImpl(keel::ArrayRef<PartT> parts, keel::Allocator *mr)
{
    const keel::usize n = parts.size();
    if (n == 0)
        return charValue(mr, keel::StringRef{});
    keel::String out = keel::String::from(parts[0], mr);
    for (keel::usize i = 1; i < n; ++i) {
        keel::StringRef p(parts[i]);
        if (p.empty()) continue;
        if (out.empty()) {
            out.assign(p);
            continue;
        }
        while (!out.empty() && isPathSep(out.back()))
            out.pop_back();
        out.push_back(kPathSepChar);
        out.append(ltrimSep(p));
    }
    return charValue(mr, out.as_ref());
}

} // namespace

Value filesep(keel::Allocator *mr)
{
    // (mr-last by convention; only one arg here.)
    const char sep[1] = {kPathSepChar};
    return charValue(mr, keel::StringRef(sep, 1));
}

Value fullfile(keel::ArrayRef<keel::String> parts,
               keel::Allocator *mr)
{
    return fullfileImpl(parts, mr);
}

Value fullfile(keel::ArrayRef<keel::StringRef> parts,
               keel::Allocator *mr)
{
    return fullfileImpl(parts, mr);
}

std::tuple<Value, Value, Value>
fileparts(keel::StringRef path, keel::Allocator *mr)
{
    // Find last separator.
    keel::usize lastSep = keel::kInvalidIndex;
    for (keel::usize i = path.size(); i-- > 0;) {
        if (isPathSep(path[i])) { lastSep = i; break; }
    }
    keel::StringRef folder, base;
    if (lastSep == keel::kInvalidIndex) {
        folder = "";
        base = path;
    } else {
        folder = (lastSep == 0)
                    ? path.substr(0, 1)              // root
                    : path.substr(0, lastSep);
        base = path.substr(lastSep + 1);
    }
    // Find last '.' in base — but ignore a leading dot ('.bashrc').
    keel::StringRef name = base, ext;
    if (!base.empty()) {
        keel::usize dot = base.rfind('.');
        if (dot != keel::kInvalidIndex && dot > 0) {
            name = base.substr(0, dot);
            ext = base.substr(dot);
        }
    }
    return std::make_tuple(charValue(mr, folder),
                           charValue(mr, name),
                           charValue(mr, ext));
}

Value tempdir(FsContext &fs, keel::Allocator *mr)
{
    // Prefer the resolved-FS temp area when available (lets hosts hook
    // a virtual temp area — IDE / WASM use this). Fall back to host OS.
    keel::String td(mr);
    FsContext::ResolvedPath resolved{};
    try {
        resolved = fs.resolvePath(".");
    } catch (const std::runtime_error &e) {
        throw Error(e.what());
    }
    if (resolved.fs) {
        try { td = resolved.fs->tempArea(); } catch (...) { td.clear(); }
    }
    if (td.empty()) {
        auto p = keel::fs_temp_dir(mr);
        if (p.is_ok()) td.assign(p->as_ref());
    }
    if (!td.empty() && !isPathSep(td.back()))
        td.push_back(kPathSepChar);
    return charValue(mr, td.as_ref());
}

Value tempname(FsContext &fs, keel::Allocator *mr)
{
    static std::atomic<keel::u64> counter{0};
    const keel::u64 n = counter.fetch_add(1, std::memory_order_relaxed);
    const keel::u64 ns = keel::monotonic_ns();

    static thread_local std::mt19937_64 rng{
        std::random_device{}() ^ static_cast<keel::u64>(keel::wall_clock_ns())};
    const keel::u64 r = rng();

    keel::String out = tempdir(fs, mr).toString();
    char hexBuf[32];
    out.append("tp");
    out.append(keel::StringRef(hexBuf, keel::format_u64(hexBuf, sizeof(hexBuf), ns, 16, false)));
    out.push_back('_');
    out.append(keel::StringRef(hexBuf, keel::format_u64(hexBuf, sizeof(hexBuf), r, 16, false)));
    out.push_back('_');
    out.append(keel::StringRef(hexBuf, keel::format_u64(hexBuf, sizeof(hexBuf), n, 16, false)));
    return charValue(mr, out.as_ref());
}


} // namespace numkit::io
