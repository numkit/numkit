// toolboxes/builtin/src/data_io/fileio.cpp
//
// Simple file-I/O builtins (fopen/fclose/fgetl/fgets/feof/ferror/
// ftell/fseek/frewind). fread/fwrite + scan family live elsewhere
// (6c.8.4b / 6c.8.5) because they need the shared precision / size
// helpers.

#include <keel/arena.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/io/file_io/fileio.hpp>

#include <numkit/sys/fs_context.hpp>
#include <numkit/value/error.hpp>

#include <numkit/ops/io_helpers.hpp>

#include <cstdint>
#include <cstring>

namespace numkit::io {
using namespace ::numkit::ops;

// ════════════════════════════════════════════════════════════════════════
// Shared small helper
// ════════════════════════════════════════════════════════════════════════

namespace {

FsContext::OpenFile *requireReadFid(FsContext &fs, keel::ArrayRef<Value> args, const char *fn)
{
    if (args.empty() || !args[0].isScalar())
        throw Error(keel::format("{}: file identifier required", fn));
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = fs.findFile(fid);
    if (!f || !f->forRead)
        throw Error(keel::format("{}: invalid file identifier", fn));
    return f;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════
// Public API
// ════════════════════════════════════════════════════════════════════════

void fopen(FsContext &fs, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    if (args.empty() || !args[0].isChar())
        throw Error("fopen: filename must be a char array");

    // `fopen('all')` — only as the sole argument — returns a row vector
    // of every user-opened fid. With a mode arg, 'all' becomes a literal
    // filename instead.
    if (args.size() == 1 && args[0].toString() == "all") {
        auto ids = fs.openFileIds();
        if (ids.empty()) {
            outs[0] = Value::matrix(1, 0, ValueType::DOUBLE, mr);
        } else {
            auto row = Value::matrix(1, ids.size(), ValueType::DOUBLE, mr);
            keel::f64 *d = row.doubleDataMut();
            for (keel::usize i = 0; i < ids.size(); ++i)
                d[i] = keel::as<keel::f64>(ids[i]);
            outs[0] = keel::move(row);
        }
        return;
    }

    keel::String path = args[0].toString();
    keel::String mode = (args.size() >= 2 && args[1].isChar())
                            ? args[1].toString()
                            : keel::String::from("r");
    bool be = false;
    if (args.size() >= 3 && (args[2].isChar() || args[2].isString())) {
        be = numkit::ops::parseEndian(args[2].toString().as_ref(), "fopen");
    }
    keel::i32 fid = fs.openFile(path.as_ref(), mode.as_ref());
    if (fid >= 0 && be) {
        if (auto *f = fs.findFile(fid))
            f->mode.append(";be");
    }
    outs[0] = Value::scalar(keel::as<keel::f64>(fid), mr);
    // [fid, errmsg] = fopen(...) — errmsg is '' on success.
    if (nargout > 1)
        outs[1] = Value::fromString(fid < 0 ? fs.lastFopenError().as_ref() : keel::StringRef(""), mr);
}

void fclose(FsContext &fs, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    if (args.empty())
        throw Error("fclose: requires a file identifier or 'all'");

    if (args[0].isChar() && args[0].toString() == "all") {
        fs.closeAllFiles();
        outs[0] = Value::scalar(0.0, mr);
        return;
    }

    if (!args[0].isScalar())
        throw Error("fclose: argument must be a numeric fid or 'all'");
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    bool ok = fs.closeFile(fid);
    outs[0] = Value::scalar(ok ? 0.0 : -1.0, mr);
}

void fgetl(FsContext &fs, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    auto *f = requireReadFid(fs, args, "fgetl");

    if (f->cursor >= f->buffer.size()) {
        f->lastError = keel::String::from("End of file reached.");
        outs[0] = Value::scalar(-1.0, mr);
        return;
    }
    keel::usize start = f->cursor;
    keel::usize nl = f->buffer.find('\n', start);
    keel::usize end = (nl == keel::kInvalidIndex) ? f->buffer.size() : nl;
    // Strip trailing \r so CRLF files don't leak the carriage return —
    // MATLAB does the same.
    keel::usize trimEnd = end;
    if (trimEnd > start && f->buffer[trimEnd - 1] == '\r')
        --trimEnd;
    keel::StringRef line = f->buffer.substr(start, trimEnd - start);
    f->cursor = (nl == keel::kInvalidIndex) ? f->buffer.size() : nl + 1;
    outs[0] = Value::fromString(line, mr);
}

void fgets(FsContext &fs, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    auto *f = requireReadFid(fs, args, "fgets");

    if (f->cursor >= f->buffer.size()) {
        f->lastError = keel::String::from("End of file reached.");
        outs[0] = Value::scalar(-1.0, mr);
        return;
    }
    keel::usize start = f->cursor;
    keel::usize remaining = f->buffer.size() - start;
    keel::usize nchar = (args.size() >= 2 && args[1].isScalar())
                       ? keel::as<keel::usize>(args[1].toScalar())
                       : remaining;
    keel::usize nlPos = f->buffer.find('\n', start);
    keel::usize end;
    if (nlPos != keel::kInvalidIndex && nlPos < start + nchar)
        end = nlPos + 1; // include newline
    else
        end = keel::min(start + nchar, f->buffer.size());
    keel::StringRef line = f->buffer.substr(start, end - start);
    f->cursor = end;
    outs[0] = Value::fromString(line, mr);
}

void feof(FsContext &fs, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    if (args.empty() || !args[0].isScalar())
        throw Error("feof: file identifier required");
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = fs.findFile(fid);
    if (!f)
        throw Error("feof: invalid file identifier");
    outs[0] = Value::logicalScalar(f->cursor >= f->buffer.size(), mr);
}

void ferror(FsContext &fs, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    if (args.empty() || !args[0].isScalar())
        throw Error("ferror: file identifier required");
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = fs.findFile(fid);
    if (!f)
        throw Error("ferror: invalid file identifier");

    bool clear = args.size() >= 2 && args[1].isChar() && args[1].toString() == "clear";
    keel::String msg = f->lastError.clone(mr);
    if (clear)
        f->lastError.clear();

    outs[0] = Value::fromString(msg.as_ref(), mr);
    if (nargout > 1)
        outs[1] = Value::scalar(msg.empty() ? 0.0 : -1.0, mr);
}

void ftell(FsContext &fs, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    if (args.empty() || !args[0].isScalar())
        throw Error("ftell: file identifier required");
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = fs.findFile(fid);
    if (!f) {
        outs[0] = Value::scalar(-1.0, mr);
        return;
    }
    // Write-mode: report end-of-buffer (where next append lands).
    keel::usize pos = f->forWrite ? f->buffer.size() : f->cursor;
    outs[0] = Value::scalar(keel::as<keel::f64>(pos), mr);
}

void fseek(FsContext &fs, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    auto fail = [&]() { outs[0] = Value::scalar(-1.0, mr); };

    if (args.size() < 2 || !args[0].isScalar() || !args[1].isScalar())
        return fail();

    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = fs.findFile(fid);
    if (!f || !f->forRead)
        return fail();

    keel::i64 offset = keel::as<keel::i64>(args[1].toScalar());

    // Origin: defaults to 'bof' when omitted. Accepts either a string
    // ('bof'/'cof'/'eof') or the int codes -1/0/1 used by some
    // MATLAB-equivalent APIs.
    enum { BOF, COF, EOFO } origin = BOF;
    if (args.size() >= 3) {
        if (args[2].isChar()) {
            keel::String o = args[2].toString();
            if (o == "bof") origin = BOF;
            else if (o == "cof") origin = COF;
            else if (o == "eof") origin = EOFO;
            else return fail();
        } else if (args[2].isScalar()) {
            keel::i32 o = keel::as<keel::i32>(args[2].toScalar());
            if (o == -1) origin = BOF;
            else if (o == 0) origin = COF;
            else if (o == 1) origin = EOFO;
            else return fail();
        } else {
            return fail();
        }
    }

    keel::i64 base = 0;
    if (origin == BOF) base = 0;
    else if (origin == COF) base = keel::as<keel::i64>(f->cursor);
    else base = keel::as<keel::i64>(f->buffer.size());

    keel::i64 target = base + offset;
    if (target < 0 || keel::as<keel::usize>(target) > f->buffer.size())
        return fail();

    f->cursor = keel::as<keel::usize>(target);
    outs[0] = Value::scalar(0.0, mr);
}

void frewind(FsContext &fs, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, keel::Allocator *mr)
{
    (void)mr;
    if (args.empty() || !args[0].isScalar())
        throw Error("frewind: file identifier required");
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = fs.findFile(fid);
    if (!f || !f->forRead)
        throw Error("frewind: invalid file identifier");
    f->cursor = 0;
}

// ── Binary I/O ──────────────────────────────────────────────────────────

void fread(FsContext &fs, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    if (args.empty() || !args[0].isScalar())
        throw Error("fread: file identifier required");
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = fs.findFile(fid);
    if (!f || !f->forRead)
        throw Error("fread: invalid file identifier");

    numkit::ops::SizeSpec sz{numkit::ops::SizeSpec::Kind::Flat, SIZE_MAX, 0, 0};
    if (args.size() >= 2)
        sz = numkit::ops::parseReadSize(args[1], "fread");

    keel::String precStr = keel::String::from("uint8");
    if (args.size() >= 3) {
        if (!args[2].isChar())
            throw Error("fread: precision must be a char array");
        precStr = args[2].toString();
    }
    auto precOpt = numkit::ops::parsePrecision(precStr.as_ref());
    if (!precOpt)
        throw Error(keel::format("fread: unsupported precision '{}'", precStr));
    keel::i32 kind = precOpt->first;
    keel::usize bsize = precOpt->second;

    keel::usize skip = 0;
    bool be = (f->mode.find(";be") != keel::kInvalidIndex);
    if (args.size() >= 4) {
        if (args[3].isChar() || args[3].isString()) {
            be = numkit::ops::parseEndian(args[3].toString().as_ref(), "fread");
        } else if (args[3].isScalar() || args[3].isEmpty()) {
            if (args[3].isScalar())
                skip = keel::as<keel::usize>(keel::max(0.0, args[3].toScalar()));
        } else {
            throw Error("fread: machine format must be a char array");
        }
    }
    if (args.size() >= 5) {
        if (!args[4].isChar() && !args[4].isString())
            throw Error("fread: machine format must be a char array");
        be = numkit::ops::parseEndian(args[4].toString().as_ref(), "fread");
    }

    keel::usize available = f->buffer.size() - f->cursor;
    keel::usize stride = bsize + skip;
    keel::usize maxElems = (available < bsize) ? 0 : (1 + (available - bsize) / stride);
    keel::usize n = keel::min(sz.limit, maxElems);
    if (sz.limit != SIZE_MAX && n < sz.limit)
        f->lastError = keel::String::from("End of file reached.");

    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto values = keel::Array<keel::f64>(n, scratch.allocator());
    for (keel::usize i = 0; i < n; ++i) {
        // Local copy lets us byte-swap safely without mutating f->buffer.
        char tmp[8];
        std::memcpy(tmp, f->buffer.data() + f->cursor + i * stride, bsize);
        if (be && bsize > 1) numkit::ops::byteSwap(tmp, bsize);

        keel::f64 v = 0.0;
        if (kind == 0) {
            switch (bsize) {
            case 1: { keel::u8  x; std::memcpy(&x, tmp, 1); v = x; break; }
            case 2: { keel::u16 x; std::memcpy(&x, tmp, 2); v = x; break; }
            case 4: { keel::u32 x; std::memcpy(&x, tmp, 4); v = x; break; }
            case 8: { keel::u64 x; std::memcpy(&x, tmp, 8); v = keel::as<keel::f64>(x); break; }
            }
        } else if (kind == 1) {
            switch (bsize) {
            case 1: { keel::i8  x; std::memcpy(&x, tmp, 1); v = x; break; }
            case 2: { keel::i16 x; std::memcpy(&x, tmp, 2); v = x; break; }
            case 4: { keel::i32 x; std::memcpy(&x, tmp, 4); v = x; break; }
            case 8: { keel::i64 x; std::memcpy(&x, tmp, 8); v = keel::as<keel::f64>(x); break; }
            }
        } else {
            if (bsize == 4) { keel::f32 x; std::memcpy(&x, tmp, 4); v = x; }
            else            {                     std::memcpy(&v, tmp, 8); }
        }
        values[i] = v;
    }
    if (n > 0) {
        keel::usize adv = (n - 1) * stride + bsize;
        f->cursor = keel::min(f->buffer.size(), f->cursor + adv);
    }
    outs[0] = numkit::ops::shapeFreadOutput(values.data(), values.size(), sz, mr);
    if (nargout > 1)
        outs[1] = Value::scalar(keel::as<keel::f64>(n), mr);
}

void fwrite(FsContext &fs, keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, keel::Allocator *mr)
{
    if (args.size() < 2 || !args[0].isScalar())
        throw Error("fwrite: requires (fid, array [, precision [, machineformat]])");
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = fs.findFile(fid);
    if (!f || !f->forWrite)
        throw Error("fwrite: invalid file identifier");

    keel::String precStr = keel::String::from("uint8");
    if (args.size() >= 3) {
        if (!args[2].isChar())
            throw Error("fwrite: precision must be a char array");
        precStr = args[2].toString();
    }
    auto precOpt = numkit::ops::parsePrecision(precStr.as_ref());
    if (!precOpt)
        throw Error(keel::format("fwrite: unsupported precision '{}'", precStr));
    keel::i32 kind = precOpt->first;
    keel::usize bsize = precOpt->second;

    bool be = (f->mode.find(";be") != keel::kInvalidIndex);
    if (args.size() >= 4) {
        if (!args[3].isChar() && !args[3].isString())
            throw Error("fwrite: machine format must be a char array");
        be = numkit::ops::parseEndian(args[3].toString().as_ref(), "fwrite");
    }

    const Value &A = args[1];
    if (A.isObject() || A.isStruct() || A.isCell() || A.isFuncHandle())
        throw Error("fwrite: cannot write value of unsupported class",
                    0, 0, "fwrite", "", "numkit:fwrite:unsupportedClass");
    keel::usize numel = A.numel();

    // Accept any numeric element type — `Value::elemAsDouble` handles
    // DOUBLE/SINGLE/LOGICAL/CHAR/INT*/UINT* uniformly. The on-disk
    // precision is governed by `precStr`, not by the input dtype, so
    // MATLAB-style `fwrite(fid, uint8([...]), 'uint8')` works.
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::Array<char> bytes(numel * bsize, '\0', scratch.allocator());
    char *dst = bytes.data();
    for (keel::usize i = 0; i < numel; ++i) {
        keel::f64 v = A.elemAsDouble(i);
        if (kind == 0) {
            switch (bsize) {
            case 1: { keel::u8  x = keel::as<keel::u8>(v); std::memcpy(dst, &x, 1); break; }
            case 2: { keel::u16 x = keel::as<keel::u16>(v); std::memcpy(dst, &x, 2); break; }
            case 4: { keel::u32 x = keel::as<keel::u32>(v); std::memcpy(dst, &x, 4); break; }
            case 8: { keel::u64 x = keel::as<keel::u64>(v); std::memcpy(dst, &x, 8); break; }
            }
        } else if (kind == 1) {
            switch (bsize) {
            case 1: { keel::i8  x = keel::as<keel::i8>(v); std::memcpy(dst, &x, 1); break; }
            case 2: { keel::i16 x = keel::as<keel::i16>(v); std::memcpy(dst, &x, 2); break; }
            case 4: { keel::i32 x = keel::as<keel::i32>(v); std::memcpy(dst, &x, 4); break; }
            case 8: { keel::i64 x = keel::as<keel::i64>(v); std::memcpy(dst, &x, 8); break; }
            }
        } else {
            if (bsize == 4) { keel::f32 x = keel::as<keel::f32>(v); std::memcpy(dst, &x, 4); }
            else            {                                  std::memcpy(dst, &v, 8); }
        }
        if (be && bsize > 1)
            numkit::ops::byteSwap(dst, bsize);
        dst += bsize;
    }

    // Same cursor-based write contract as fprintf — appendOnly snaps to
    // the end regardless of any prior seek.
    keel::usize writePos = f->appendOnly ? f->buffer.size() : f->cursor;
    if (writePos + bytes.size() > f->buffer.size())
        f->buffer.resize(writePos + bytes.size());
    if (!bytes.empty())
        std::memcpy(f->buffer.data() + writePos, bytes.data(), bytes.size());
    f->cursor = writePos + bytes.size();
    outs[0] = Value::scalar(keel::as<keel::f64>(numel), mr);
}


} // namespace numkit::io
