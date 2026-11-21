// toolboxes/io/src/text/extras.cpp
//
// fileread / readlines / writelines / readmatrix / writematrix / type.

#include <keel/arena.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/fs.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/io/text/extras.hpp>

#include <numkit/value/error.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/sys/fs_context.hpp>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace numkit::io {

// Resolve via the FsContext VFS and read the whole file as a string. Public
// (declared in extras.hpp) so the Engine-coupled `type` (type.cpp) can reuse it.
keel::String slurpFile(FsContext &fs, keel::StringRef filename, const char *fnName)
{
    FsContext::ResolvedPath resolved{};
    try {
        resolved = fs.resolvePath(filename);
    } catch (const std::runtime_error &e) {
        throw Error(e.what());
    }
    if (!resolved.fs)
        throw Error(keel::format("{}: cannot resolve filesystem for '{}'", fnName, filename),
                    0, 0, fnName, "", keel::format("numkit:{}:noFs", fnName));
    try {
        return resolved.fs->readFile(resolved.path.as_ref());
    } catch (const std::exception &e) {
        throw Error(keel::format("{}: {}", fnName, e.what()),
                    0, 0, fnName, "", keel::format("numkit:{}:readFailed", fnName));
    }
}

namespace {

void spitFile(FsContext &fs, keel::StringRef filename, keel::StringRef content,
              const char *fnName)
{
    FsContext::ResolvedPath resolved{};
    try {
        resolved = fs.resolvePath(filename);
    } catch (const std::runtime_error &e) {
        throw Error(e.what());
    }
    if (!resolved.fs)
        throw Error(keel::format("{}: cannot resolve filesystem for '{}'", fnName, filename),
                    0, 0, fnName, "", keel::format("numkit:{}:noFs", fnName));
    try {
        resolved.fs->writeFile(resolved.path.as_ref(), content);
    } catch (const std::exception &e) {
        throw Error(keel::format("{}: {}", fnName, e.what()),
                    0, 0, fnName, "", keel::format("numkit:{}:writeFailed", fnName));
    }
}

Value charRow(keel::Allocator *mr, keel::StringRef s)
{
    auto v = Value::matrix(1, s.size(), ValueType::CHAR, mr);
    for (keel::usize i = 0; i < s.size(); ++i)
        v.charDataMut()[i] = s[i];
    return v;
}

// Split content by '\n', trimming '\r' from each line. Drop one trailing
// empty line that comes from a terminal newline.
keel::Array<keel::StringRef> splitLines(keel::StringRef content, keel::Allocator *alloc = nullptr)
{
    keel::Array<keel::StringRef> out(alloc);
    keel::usize pos = 0;
    while (pos <= content.size()) {
        keel::usize nl = content.find('\n', pos);
        keel::usize end = (nl == keel::kInvalidIndex) ? content.size() : nl;
        keel::StringRef line = content.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        out.push_back(line);
        if (nl == keel::kInvalidIndex)
            break;
        pos = nl + 1;
    }
    // Strip trailing empty element produced by a terminal newline.
    if (!out.empty() && out.back().empty())
        out.pop_back();
    return out;
}

keel::StringRef trimWs(keel::StringRef s)
{
    keel::usize a = 0, b = s.size();
    while (a < b && keel::is_ascii_space(s[a])) ++a;
    while (b > a && keel::is_ascii_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool tryParseDouble(keel::StringRef tok, double &out)
{
    if (tok.empty()) { out = 0.0; return true; }   // empty cell → 0
    keel::SmallString<128> buf(tok);
    char *endp = nullptr;
    errno = 0;
    double val = std::strtod(buf.c_str(), &endp);
    if (endp == buf.c_str() || errno == ERANGE)
        return false;
    // Trailing junk → not a clean number.
    while (*endp != '\0' && keel::is_ascii_space(*endp))
        ++endp;
    if (*endp != '\0')
        return false;
    out = val;
    return true;
}

// Comma / tab / semicolon delimited row.
keel::Array<keel::StringRef> splitRow(keel::StringRef line, keel::Allocator *alloc = nullptr)
{
    keel::Array<keel::StringRef> tokens(alloc);
    keel::usize start = 0;
    for (keel::usize i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == ',' || c == ';' || c == '\t') {
            tokens.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    tokens.push_back(line.substr(start));
    return tokens;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
// Engine-free PARSE / SERIALIZE cores (pure text ↔ Value — no Engine/VFS)
//
// Mirrors csvreadFromString / csvwriteToString: a C++ embedder can turn a
// delimited text blob into a numeric matrix (and back) with no Engine. The
// MATLAB-facing readmatrix / writematrix add VFS read/write on top.
// ════════════════════════════════════════════════════════════════════════

Value readmatrixFromString(keel::StringRef content, keel::Allocator *mr)
{
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto lines = splitLines(content, scratch.allocator());

    // Skip leading non-numeric (header) rows: a row whose tokens are
    // all non-numeric (or which has any unparseable token) is considered
    // a header. Common case: header on row 1, numeric on row 2+.
    keel::usize skip = 0;
    for (; skip < lines.size(); ++skip) {
        if (trimWs(lines[skip]).empty()) continue;
        auto toks = splitRow(lines[skip], scratch.allocator());
        bool anyNum = false;
        for (keel::StringRef t : toks) {
            double dummy;
            if (tryParseDouble(trimWs(t), dummy)) { anyNum = true; break; }
        }
        if (anyNum) break;
    }

    keel::Array<keel::Array<double>> rows(scratch.allocator());
    keel::usize maxCols = 0;
    for (keel::usize i = skip; i < lines.size(); ++i) {
        if (trimWs(lines[i]).empty()) continue;
        auto toks = splitRow(lines[i], scratch.allocator());
        keel::Array<double> row(scratch.allocator());
        row.reserve(toks.size());
        for (keel::StringRef t : toks) {
            double v = 0.0;
            tryParseDouble(trimWs(t), v);     // unparseable → 0 (MATLAB compat)
            row.push_back(v);
        }
        if (row.size() > maxCols) maxCols = row.size();
        rows.push_back(keel::move(row));
    }

    const keel::usize R = rows.size();
    auto out = Value::matrix(R, maxCols, ValueType::DOUBLE, mr);
    double *dst = out.doubleDataMut();
    for (keel::usize r = 0; r < R; ++r) {
        for (keel::usize c = 0; c < maxCols; ++c) {
            const double v = (c < rows[r].size()) ? rows[r][c] : 0.0;
            dst[c * R + r] = v;     // column-major
        }
    }
    return out;
}

keel::String writematrixToString(const Value &m)
{
    if (m.isComplex())
        throw Error("writematrix: complex matrices are not supported",
                    0, 0, "writematrix", "", "numkit:writematrix:complex");
    const auto &d = m.dims();
    if (d.is3D())
        throw Error("writematrix: 3-D arrays are not supported",
                    0, 0, "writematrix", "", "numkit:writematrix:nd");
    const keel::usize R = d.rows(), C = d.cols();

    keel::String out;
    for (keel::usize r = 0; r < R; ++r) {
        for (keel::usize c = 0; c < C; ++c) {
            if (c) out.push_back(',');
            const double v = m.elemAsDouble(c * R + r);
            if (std::isfinite(v) && std::floor(v) == v && keel::abs(v) < 1e16) {
                keel::format_into(out, "{}", keel::as<long long>(v));
            } else {
                char buf[64];
                int len = std::snprintf(buf, sizeof(buf), "%.15g", v);
                if (len > 0)
                    out.append(keel::StringRef(buf, keel::as<keel::usize>(len)));
            }
        }
        out.push_back('\n');
    }
    return out;
}

// ── fileread ──────────────────────────────────────────────────────────
Value fileread(FsContext &fs, keel::StringRef filename,
               keel::Allocator *mr)
{
    keel::String content = slurpFile(fs, filename, "fileread");
    return charRow(mr, content.as_ref());
}

// ── readlines ─────────────────────────────────────────────────────────
// Returns a STRING array (column vector), N×1.
Value readlines(FsContext &fs, keel::StringRef filename,
                keel::Allocator *mr)
{
    keel::String content = slurpFile(fs, filename, "readlines");
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto lines = splitLines(content.as_ref(), scratch.allocator());
    auto out = Value::stringArray(lines.size(), 1, mr);
    for (keel::usize i = 0; i < lines.size(); ++i)
        out.stringElemSet(i, lines[i]);
    return out;
}

// ── writelines ────────────────────────────────────────────────────────
void writelines(FsContext &fs, const Value &lines, keel::StringRef filename)
{
    keel::String out;
    auto append = [&](keel::StringRef s) {
        out.append(s);
        out.append(keel::fs_line_ending());
    };

    if (lines.isString() && lines.numel() != 1) {
        // String ARRAY → one line per element (MATLAB writelines contract).
        // Must precede the scalar branch: Value::toString() on a string array
        // returns only element [0], so the old (isChar||isString) catch-all
        // silently wrote just the first line. See bugs/io/writelines.md.
        const keel::usize n = lines.numel();
        for (keel::usize i = 0; i < n; ++i)
            append(lines.stringElemRef(i));
    } else if (lines.isChar() || lines.isString()) {
        // Single char row / scalar string → one line.
        append(lines.asStringRef());
    } else if (lines.isCell()) {
        const keel::usize n = lines.numel();
        for (keel::usize i = 0; i < n; ++i) {
            const Value &cell = lines.cellAt(i);
            if (!cell.isChar() && !cell.isString())
                throw Error("writelines: each cell element must be a string",
                            0, 0, "writelines", "", "numkit:writelines:badCell");
            append(cell.toString());
        }
    } else {
        throw Error("writelines: lines must be a string, string array, or cell of strings",
                    0, 0, "writelines", "", "numkit:writelines:badArg");
    }
    spitFile(fs, filename, out.as_ref(), "writelines");
}

// ── readmatrix ────────────────────────────────────────────────────────
Value readmatrix(FsContext &fs, keel::StringRef filename,
                 keel::Allocator *mr)
{
    keel::String content = slurpFile(fs, filename, "readmatrix");
    return readmatrixFromString(content.as_ref(), mr);
}

// ── writematrix ───────────────────────────────────────────────────────
void writematrix(FsContext &fs, const Value &m, keel::StringRef filename)
{
    keel::String content = writematrixToString(m);
    spitFile(fs, filename, content.as_ref(), "writematrix");
}


} // namespace numkit::io
