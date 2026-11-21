// toolboxes/io/src/text/csv.cpp
//
// CSV text I/O (csvread / csvwrite). The PARSE / SERIALIZE cores are Engine-free
// (csvreadFromString / csvwriteToString — pure text↔Value, usable from the C++
// library API with no Engine, mirroring image::imreadFromBytes). The MATLAB-facing
// entries (csvread/csvwrite + the detail reg adapters) add VFS path resolution +
// read/write via the Engine.

#include <keel/arena.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/io/text/csv.hpp>

#include <numkit/value/error.hpp>
#include <numkit/sys/fs_context.hpp>

#include <cstdio>
#include <cstdlib>

namespace numkit::io {

// ════════════════════════════════════════════════════════════════════════
// Private CSV helpers
// ════════════════════════════════════════════════════════════════════════

namespace {

// Auto-append ".csv" when the filename's basename has no extension.
// Matches MATLAB's quiet behaviour so short names like "out" still DTRT.
keel::String resolveCsvPath(keel::StringRef rawPath)
{
    keel::String path = keel::String::from(rawPath);
    keel::usize slash = keel::kInvalidIndex;
    for (keel::usize i = path.size(); i > 0; --i) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') {
            slash = i - 1;
            break;
        }
    }
    keel::usize fromIdx = (slash == keel::kInvalidIndex) ? 0 : slash + 1;
    if (path.find('.', fromIdx) == keel::kInvalidIndex)
        path.append(".csv");
    return path;
}

// Parse one CSV line into a vector of doubles. Empty / non-numeric
// cells map to 0.0 (matching MATLAB's csvread contract). Trailing \r
// is stripped so CRLF files don't leak a carriage return into the last
// cell's parse range.
keel::Array<keel::f64> parseCsvLine(keel::Allocator *mr, keel::StringRef raw)
{
    keel::Array<keel::f64> out(mr);
    keel::usize len = raw.size();
    if (len && raw[len - 1] == '\r')
        --len;

    keel::usize start = 0;
    for (keel::usize i = 0; i <= len; ++i) {
        if (i == len || raw[i] == ',') {
            const char *p = raw.data() + start;
            keel::usize n = i - start;
            while (n > 0 && (*p == ' ' || *p == '\t')) {
                ++p;
                --n;
            }
            while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
                --n;
            keel::f64 v = 0.0;
            if (n > 0) {
                keel::SmallString<64> cell(keel::StringRef(p, n), mr);
                char *endp = nullptr;
                keel::f64 parsed = std::strtod(cell.c_str(), &endp);
                if (endp != cell.c_str())
                    v = parsed;
            }
            out.push_back(v);
            start = i + 1;
        }
    }
    return out;
}

Value csvreadFromStringImpl(keel::StringRef content, keel::usize skipRows, keel::usize skipCols,
                            bool haveRange, keel::usize lastRow, keel::usize lastCol,
                            keel::Allocator *mr)
{
    const keel::usize r1 = skipRows, c1 = skipCols, r2 = lastRow, c2 = lastCol;
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    // Outer + inner ScratchVec — uses-allocator construction propagates
    // the arena into each inner row that we move-construct from
    // `parseCsvLine`'s sliced result below.
    keel::Array<keel::Array<keel::f64>> rows(scratch.allocator());
    keel::usize lineNo = 0;
    keel::usize pos = 0;
    while (pos <= content.size()) {
        keel::usize nl = content.find('\n', pos);
        keel::usize end = (nl == keel::kInvalidIndex) ? content.size() : nl;
        // Stop cleanly on a trailing newline with nothing after it —
        // otherwise we'd produce one bogus empty row.
        if (end == pos && nl == keel::kInvalidIndex)
            break;
        keel::StringRef line = content.substr(pos, end - pos);
        pos = (nl == keel::kInvalidIndex) ? content.size() + 1 : nl + 1;

        if (lineNo < r1) {
            ++lineNo;
            continue;
        }
        if (haveRange && lineNo > r2)
            break;
        ++lineNo;

        keel::Array<keel::f64> cells = parseCsvLine(scratch.allocator(), line);
        keel::Array<keel::f64> row(scratch.allocator());
        keel::usize endCol = haveRange ? keel::min(c2 + 1, cells.size()) : cells.size();
        if (c1 < endCol)
            row.assign(cells.slice(c1, endCol - c1));
        rows.push_back(keel::move(row));
    }

    keel::usize R, C;
    if (haveRange) {
        R = r2 - r1 + 1;
        C = c2 - c1 + 1;
        while (rows.size() < R)
            rows.emplace_back(scratch.allocator());
    } else {
        R = rows.size();
        C = 0;
        for (auto &r : rows)
            if (r.size() > C)
                C = r.size();
    }

    if (R == 0 || C == 0)
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);

    auto M = Value::matrix(R, C, ValueType::DOUBLE, mr);
    keel::f64 *data = M.doubleDataMut();
    for (keel::usize r = 0; r < R; ++r) {
        const auto &row = rows[r];
        keel::usize n = keel::min(C, row.size());
        for (keel::usize c = 0; c < n; ++c)
            data[c * R + r] = row[c];
    }
    return M;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
// Engine-free PARSE / SERIALIZE cores (pure text ↔ Value — no Engine/VFS)
// ════════════════════════════════════════════════════════════════════════

Value csvreadFromString(keel::StringRef content, keel::usize skipRows, keel::usize skipCols,
                        keel::Allocator *mr)
{
    return csvreadFromStringImpl(content, skipRows, skipCols, false, 0, 0, mr);
}

Value csvreadFromString(keel::StringRef content, keel::Allocator *mr)
{
    return csvreadFromStringImpl(content, 0, 0, false, 0, 0, mr);
}

keel::String csvwriteToString(const Value &M, keel::usize offR, keel::usize offC)
{
    keel::usize R = M.dims().rows();
    keel::usize C = M.dims().cols();

    keel::String out;
    for (keel::usize r = 0; r < offR; ++r)
        out.push_back('\n');

    auto writeCell = [&](keel::f64 v) {
        char buf[32];
        int len = std::snprintf(buf, sizeof(buf), "%.15g", v);
        if (len > 0)
            out.append(keel::StringRef(buf, keel::as<keel::usize>(len)));
    };

    for (keel::usize r = 0; r < R; ++r) {
        for (keel::usize c = 0; c < offC; ++c)
            out.push_back(',');
        for (keel::usize c = 0; c < C; ++c) {
            if (c > 0)
                out.push_back(',');
            keel::f64 v = 0.0;
            if (M.type() == ValueType::DOUBLE) {
                v = M.isScalar() ? M.toScalar() : M(r, c);
            } else if (M.isLogical()) {
                if (M.isScalar())
                    v = M.toBool() ? 1.0 : 0.0;
                else
                    v = M.logicalData()[M.dims().sub2ind(r, c)] ? 1.0 : 0.0;
            } else if (M.isComplex()) {
                v = M.isScalar() ? M.toComplex().real() : M.complexElem(r, c).real();
            }
            writeCell(v);
        }
        out.push_back('\n');
    }
    return out;
}

// ════════════════════════════════════════════════════════════════════════
// csvread / csvwrite — MATLAB-facing entries: VFS path resolution + read/write
//
//   csvread(filename)                         → full numeric CSV → matrix
//   csvread(filename, R1, C1)                 → skip first R1 rows / C1 cols
//   csvread(filename, R1, C1, [R1 C1 R2 C2])  → read a rectangular range
//   csvwrite(filename, M[, R, C])             → write M as CSV (with offsets)
//
// Missing cells are read as 0. Filenames with no extension get ".csv".
// ════════════════════════════════════════════════════════════════════════

Value csvread(FsContext &fs, keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    if (args.empty() || !args[0].isChar())
        throw Error("csvread requires a filename as the first argument");

    keel::String filename = resolveCsvPath(args[0].toString().as_ref());

    keel::usize r1 = 0, c1 = 0, r2 = 0, c2 = 0;
    bool haveRange = false;

    if (args.size() >= 2)
        r1 = keel::as<keel::usize>(args[1].toScalar());
    if (args.size() >= 3)
        c1 = keel::as<keel::usize>(args[2].toScalar());
    if (args.size() >= 4) {
        if (args[3].numel() != 4)
            throw Error("csvread: range argument must be [R1 C1 R2 C2]");
        haveRange = true;
        r2 = keel::as<keel::usize>(args[3](2));
        c2 = keel::as<keel::usize>(args[3](3));
        if (r2 < r1 || c2 < c1)
            throw Error("csvread: invalid range [R1 C1 R2 C2]");
    }

    FsContext::ResolvedPath resolved{};
    try {
        resolved = fs.resolvePath(filename.as_ref());
    } catch (const std::runtime_error &e) {
        throw Error(e.what());
    }
    keel::String content;
    try {
        content = resolved.fs->readFile(resolved.path.as_ref());
    } catch (const std::exception &e) {
        throw Error(keel::format("csvread: {}", e.what()));
    }
    return csvreadFromStringImpl(content.as_ref(), r1, c1, haveRange, r2, c2, mr);
}

void csvwrite(FsContext &fs, keel::ArrayRef<Value> args)
{
    if (args.size() < 2)
        throw Error("csvwrite requires filename and matrix arguments");
    if (!args[0].isChar())
        throw Error("csvwrite: first argument must be a filename");

    keel::String filename = resolveCsvPath(args[0].toString().as_ref());
    const Value &M = args[1];

    keel::usize offR = 0, offC = 0;
    if (args.size() >= 3)
        offR = keel::as<keel::usize>(args[2].toScalar());
    if (args.size() >= 4)
        offC = keel::as<keel::usize>(args[3].toScalar());

    const keel::String out = csvwriteToString(M, offR, offC);

    FsContext::ResolvedPath resolved{};
    try {
        resolved = fs.resolvePath(filename.as_ref());
    } catch (const std::runtime_error &e) {
        throw Error(e.what());
    }
    try {
        resolved.fs->writeFile(resolved.path.as_ref(), out.as_ref());
    } catch (const std::exception &e) {
        throw Error(keel::format("csvwrite: {}", e.what()));
    }
}


} // namespace numkit::io
