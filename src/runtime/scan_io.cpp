// runtime/src/language/strings/scan_io.cpp
//
// Engine-coupled scan builtins — fscanf / textscan. Relocated from
// lang/src/strings/scan.cpp in C6c-2b: both read through the engine fid table
// (findFile / OpenFile), so they belong in the core-aware runtime layer. The
// pure parse+shape core (scanfEmit) and the pure sscanf stay in lang; fscanf
// reads the file buffer then delegates to numkit::builtin::detail::scanfEmit.
// Namespace stays numkit::lang (cell/struct precedent: runtime-located,
// language-namespaced builtins). textscan + its format parser are textscan-
// only, so they ride along here whole.

#include <keel/arena.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/runtime/io.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/builtin/scan_core.hpp>     // detail::scanfEmit + ScanfOut
#include <numkit/core/engine.hpp>          // Engine, findFile, OpenFile

#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/io_helpers.hpp>       // SizeSpec / parseReadSize (numkit::builtin::detail)
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace numkit::runtime {

using numkit::builtin::detail::scanfEmit;
using numkit::builtin::detail::ScanfOut;

void fscanf(Engine &engine, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs)
{
    keel::Allocator *mr = engine.allocator();
    if (args.size() < 2 || !args[0].isScalar() || !args[1].isChar())
        throw Error("fscanf: requires (fid, format [, size])");
    keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
    auto *f = engine.findFile(fid);
    if (!f || !f->forRead)
        throw Error("fscanf: invalid file identifier");

    numkit::ops::SizeSpec sz{numkit::ops::SizeSpec::Kind::Flat, SIZE_MAX, 0, 0};
    if (args.size() >= 3)
        sz = numkit::ops::parseReadSize(args[2], "fscanf");

    keel::StringRef input(f->buffer.data() + f->cursor, f->buffer.size() - f->cursor);
    keel::String fmt = args[1].toString();

    ScanfOut r{0, 0};
    scanfEmit(input, fmt, sz, nargout, outs, r, mr);
    f->cursor += r.bytesConsumed;

    // Populate ferror on a partial match so scripts can distinguish
    // "input exhausted" from "fewer items matched than requested".
    if (f->cursor >= f->buffer.size())
        f->lastError = keel::String::from("End of file reached.");
    else if (sz.limit != SIZE_MAX && r.count < sz.limit)
        f->lastError = keel::String::from("Matching failure.");
}

namespace {

struct TextscanOptions
{
    explicit TextscanOptions(keel::Allocator *mr)
        : delimiters(mr),
          endOfLine(keel::String::from("\n", mr)),
          commentStyle(mr),
          treatAsEmpty(mr)
    {}

    // Empty → whitespace-default mode (runs of space/tab/\f/\v + EOL
    // count as a single field boundary). Non-empty → explicit-delim
    // mode: ONE char in this set is one field boundary, and EOL
    // chars are still separate record boundaries that always
    // collapse.
    keel::String delimiters;
    keel::String endOfLine;
    keel::usize headerLines = 0;
    // Line-comment marker. When non-empty, anything from its first
    // occurrence up to the next EndOfLine char is ignored.
    keel::String commentStyle;
    // Tokens whose text equals any entry in this set are coerced to
    // "empty" (numeric → EmptyValue, %s → ''). Vector body and
    // string elements live on the per-call arena.
    keel::Array<keel::String> treatAsEmpty;
    // In explicit-delim mode only: when true, runs of delim chars
    // collapse to one boundary (no empty fields emitted). Default
    // false, matching MATLAB; whitespace-default mode always
    // collapses regardless.
    bool multipleDelimsAsOne = false;
    // Value substituted for empty numeric fields. MATLAB default NaN.
    keel::f64 emptyValue = std::numeric_limits<keel::f64>::quiet_NaN();
};

struct TextscanConv { char spec; bool suppress; keel::i32 width; };

struct StagedNum { keel::usize col; keel::f64 val; };
struct StagedStr { keel::usize col; keel::String val; };

// Parse formatSpec into an ordered conversion list. Unrecognised
// % codes throw before any scanning happens.
keel::Array<TextscanConv> parseTextscanFormat(keel::StringRef fmt, keel::Allocator *mr)
{
    keel::Array<TextscanConv> out(mr);
    for (keel::usize i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%')
            continue;                     // literal chars between specs are ignored
        ++i;
        bool suppress = false;
        if (i < fmt.size() && fmt[i] == '*') { suppress = true; ++i; }
        keel::i32 width = -1;
        while (i < fmt.size() && keel::is_ascii_digit(fmt[i])) {
            if (width < 0) width = 0;
            width = width * 10 + (fmt[i] - '0');
            ++i;
        }
        if (i >= fmt.size())
            throw Error("textscan: truncated format specifier");
        char spec = fmt[i];
        switch (spec) {
        case 'd': case 'i': case 'u':
        case 'f': case 'e': case 'g': case 'E': case 'G':
        case 'x': case 'X': case 'o':
        case 's':
            break;
        default:
            throw Error(keel::format("textscan: unsupported conversion '%{}'", spec));
        }
        out.push_back({spec, suppress, width});
    }
    if (out.empty())
        throw Error("textscan: format must contain at least one conversion");
    return out;
}

} // namespace

void textscan(Engine &engine, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs)
{
    (void)nargout;
    keel::Allocator *mr = engine.allocator();
    if (args.size() < 2 || !args[1].isChar())
        throw Error("textscan: requires (source, format [, N] [, opt, value …])");

    // Source — fid scalar or char array.
    keel::String ownedInput;
    keel::StringRef input;
    Engine::OpenFile *srcFile = nullptr;
    if (args[0].isChar()) {
        ownedInput = args[0].toString();
        input = ownedInput.as_ref();
    } else if (args[0].isScalar()) {
        keel::i32 fid = keel::as<keel::i32>(args[0].toScalar());
        srcFile = engine.findFile(fid);
        if (!srcFile || !srcFile->forRead)
            throw Error("textscan: invalid file identifier");
        input = keel::StringRef(srcFile->buffer.data() + srcFile->cursor,
                                srcFile->buffer.size() - srcFile->cursor);
    } else {
        throw Error("textscan: source must be a file identifier or char array");
    }

    keel::String fmt = args[1].toString();
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    auto convs = parseTextscanFormat(fmt, scratch.allocator());

    // Optional positional N, then name-value pairs.
    keel::usize argIdx = 2;
    keel::usize cycleCap = SIZE_MAX;
    if (argIdx < args.size() && args[argIdx].isScalar() && !args[argIdx].isChar()) {
        keel::f64 d = args[argIdx].toScalar();
        if (!std::isinf(d)) {
            if (d < 0 || !std::isfinite(d))
                throw Error("textscan: N must be Inf or a non-negative integer");
            cycleCap = keel::as<keel::usize>(d);
        }
        ++argIdx;
    }

    // Parse name/value options into a TextscanOptions struct.
    TextscanOptions opts{scratch.allocator()};
    auto lower = [](keel::StringRef sr) {
        keel::String s = keel::String::from(sr);
        s.to_ascii_lower_in_place();
        return s;
    };
    while (argIdx + 1 < args.size()) {
        if (!args[argIdx].isChar())
            throw Error("textscan: option name must be a char array");
        keel::String name = lower(args[argIdx].toString());
        const Value &val = args[argIdx + 1];
        if (name == "delimiter") {
            if (val.isChar()) {
                opts.delimiters.assign(val.toString());
            } else if (val.isCell()) {
                // Concatenate every char in the cells into one delimiter set.
                opts.delimiters.clear();
                for (keel::usize i = 0; i < val.numel(); ++i) {
                    const Value &d = val.cellAt(i);
                    if (d.isChar()) opts.delimiters.append(d.toString());
                }
            } else {
                throw Error("textscan: 'Delimiter' must be a char array or cell");
            }
        } else if (name == "endofline") {
            if (!val.isChar())
                throw Error("textscan: 'EndOfLine' must be a char array");
            opts.endOfLine.assign(val.toString());
        } else if (name == "headerlines") {
            keel::f64 d = val.toScalar();
            if (d < 0 || !std::isfinite(d))
                throw Error("textscan: 'HeaderLines' must be a non-negative integer");
            opts.headerLines = keel::as<keel::usize>(d);
        } else if (name == "commentstyle") {
            if (!val.isChar())
                throw Error("textscan: 'CommentStyle' must be a char array");
            opts.commentStyle.assign(val.toString());
        } else if (name == "treatasempty") {
            if (val.isChar()) {
                opts.treatAsEmpty.push_back(keel::String::from(val.toString(), scratch.allocator()));
            } else if (val.isCell()) {
                for (keel::usize i = 0; i < val.numel(); ++i) {
                    const Value &e = val.cellAt(i);
                    if (e.isChar())
                        opts.treatAsEmpty.push_back(keel::String::from(e.toString(), scratch.allocator()));
                }
            } else {
                throw Error("textscan: 'TreatAsEmpty' must be a char array or cell");
            }
        } else if (name == "multipledelimsasone") {
            if (!val.isScalar() && !val.isLogical())
                throw Error("textscan: 'MultipleDelimsAsOne' must be logical/numeric");
            opts.multipleDelimsAsOne = (val.toScalar() != 0.0);
        } else if (name == "emptyvalue") {
            if (!val.isScalar())
                throw Error("textscan: 'EmptyValue' must be a numeric scalar");
            opts.emptyValue = val.toScalar();
        } else {
            throw Error(keel::format("textscan: unsupported option '{}'", args[argIdx].toString()));
        }
        argIdx += 2;
    }

    // Character classification. Whitespace-default mode (no
    // Delimiter supplied) treats whitespace as a collapsing
    // field separator; explicit-delim mode reserves whitespace
    // for trimming around fields instead.
    const bool useWsDefault = opts.delimiters.empty();
    keel::usize pos = 0;

    auto isEol = [&opts](char c) {
        return opts.endOfLine.find(c) != keel::kInvalidIndex;
    };
    auto isWs = [](char c) {
        return c == ' ' || c == '\t' || c == '\f' || c == '\v';
    };
    auto isExplicitDelim = [&opts](char c) {
        return opts.delimiters.find(c) != keel::kInvalidIndex;
    };
    auto atComment = [&opts, &input, &pos]() -> bool {
        return !opts.commentStyle.empty() &&
               input.substr(pos).starts_with(opts.commentStyle.as_ref());
    };
    auto isEmpty = [&opts](keel::StringRef tok) {
        for (const auto &e : opts.treatAsEmpty)
            if (e == tok) return true;
        return false;
    };

    // Skip header lines using the configured EndOfLine chars.
    for (keel::usize h = 0; h < opts.headerLines && pos < input.size(); ++h) {
        while (pos < input.size() && !isEol(input[pos])) ++pos;
        if (pos < input.size()) ++pos;  // consume the EOL char itself
    }

    // skipLeading: eat all whitespace / EOL / comment chars before
    // the very first field of the whole run. Called once, before
    // the cycle loop.
    auto skipLeading = [&]() {
        while (pos < input.size()) {
            if (atComment()) {
                while (pos < input.size() && !isEol(input[pos])) ++pos;
                continue;
            }
            char c = input[pos];
            if (isEol(c) || isWs(c)) { ++pos; continue; }
            break;
        }
    };

    // consumeBoundary: eat exactly ONE field/record boundary
    // between two adjacent fields. Returns false when there is
    // no boundary to consume (EOF or unexpected input), which
    // signals the caller to end the cycle.
    //
    // Whitespace-default: runs of whitespace+EOL+comments all
    // collapse to one boundary — MATLAB's documented contract.
    // Explicit-delim: EOL still collapses, but a single delim
    // char counts as ONE boundary, so "1,,3" yields three
    // fields with the middle one empty (subject to
    // MultipleDelimsAsOne, which collapses runs of the
    // explicit delim back together).
    auto consumeBoundary = [&]() -> bool {
        if (useWsDefault) {
            keel::usize before = pos;
            while (pos < input.size()) {
                if (atComment()) {
                    while (pos < input.size() && !isEol(input[pos])) ++pos;
                    continue;
                }
                char c = input[pos];
                if (isWs(c) || isEol(c)) { ++pos; continue; }
                break;
            }
            return pos > before;
        }

        // Explicit-delim mode. Trim leading whitespace around
        // the boundary (ws is always stripped from fields, not
        // treated as a delim itself).
        while (pos < input.size() && isWs(input[pos])) ++pos;
        if (atComment()) {
            while (pos < input.size() && !isEol(input[pos])) ++pos;
        }
        if (pos >= input.size()) return false;

        char c = input[pos];
        if (isEol(c)) {
            // EOL runs always collapse, even in explicit-delim mode.
            while (pos < input.size() && isEol(input[pos])) ++pos;
            while (pos < input.size() && isWs(input[pos])) ++pos;
            return true;
        }
        if (!isExplicitDelim(c)) return false;

        ++pos;  // consume exactly one delim
        if (opts.multipleDelimsAsOne) {
            while (pos < input.size() && isExplicitDelim(input[pos])) ++pos;
        }
        while (pos < input.size() && isWs(input[pos])) ++pos;
        return true;
    };

    // readField: consume up to the next field/record boundary.
    // In explicit-delim mode, trailing whitespace inside the
    // field is trimmed (so "1 , 2" with delim ',' yields "1"
    // and "2", not "1 " and " 2").
    auto readField = [&]() -> keel::StringRef {
        keel::usize start = pos;
        while (pos < input.size()) {
            char c = input[pos];
            if (atComment()) break;
            if (isEol(c)) break;
            if (useWsDefault) {
                if (isWs(c)) break;
            } else {
                if (isExplicitDelim(c)) break;
            }
            ++pos;
        }
        keel::usize end = pos;
        if (!useWsDefault) {
            while (end > start && isWs(input[end - 1])) --end;
        }
        return input.substr(start, end - start);
    };

    // Per-column accumulators. Both outer and inner arrays allocate from the arena.
    keel::Array<keel::Array<keel::f64>> numCols(scratch.allocator());
    keel::Array<keel::Array<keel::String>> strCols(scratch.allocator());
    numCols.reserve(convs.size());
    strCols.reserve(convs.size());
    for (keel::usize ci = 0; ci < convs.size(); ++ci) {
        numCols.emplace_back(scratch.allocator());
        strCols.emplace_back(scratch.allocator());
    }
    // Staging buffers are HOISTED out of the cycle loop and clear()-ed
    // each iteration: re-creating them inside the loop would bump-leak
    // under the monotonic resource (footgun #c in scratch.hpp).
    // After a few iterations the capacity stabilises and clear() reuses
    // it — total arena footprint stays bounded.
    keel::Array<StagedNum> stageNum(scratch.allocator());
    keel::Array<StagedStr> stageStr(scratch.allocator());
    keel::String tokBuf(scratch.allocator());
    keel::usize cycles = 0;

    skipLeading();                         // past leading ws/EOL/comments once
    bool atFirstField = true;              // no boundary-consume before the very first field

    while (cycles < cycleCap) {
        if (pos >= input.size()) break;

        keel::usize savedPos = pos;
        bool savedFirst = atFirstField;
        stageNum.clear();
        stageStr.clear();
        bool fullCycle = true;

        for (keel::usize i = 0; i < convs.size(); ++i) {
            if (!atFirstField) {
                if (!consumeBoundary()) { fullCycle = false; break; }
            }
            atFirstField = false;

            // Note: we DO NOT bail on pos >= input.size() here.
            // In explicit-delim mode a trailing separator creates
            // a legitimate final empty field ("a,b," → 3 fields),
            // and readField returning "" below gets routed into
            // the empty-field branch correctly. In whitespace-
            // default mode, the same empty-tok path distinguishes
            // "no more data" and triggers fullCycle = false.
            keel::StringRef tok = readField();

            // Two kinds of "empty token" can flow out of readField:
            //
            //   1. The raw token was non-empty but matches one of
            //      the TreatAsEmpty markers → coerce to empty,
            //      treat as an explicitly empty field.
            //   2. readField genuinely returned "" because we hit
            //      EOL/EOF → partial cycle in ws-default mode, or
            //      a real empty field between two explicit delims.
            //
            // The coercedEmpty flag keeps these apart so a
            // TreatAsEmpty hit in whitespace-default mode still
            // yields EmptyValue instead of rolling back.
            bool coercedEmpty = !tok.empty() && isEmpty(tok);
            if (coercedEmpty) tok = {};

            const TextscanConv &c = convs[i];
            if (tok.empty()) {
                if (!coercedEmpty && useWsDefault) {
                    fullCycle = false;
                    break;
                }
                if (c.suppress) continue;
                if (c.spec == 's')
                    stageStr.push_back({i, keel::String(scratch.allocator())});
                else
                    stageNum.push_back({i, opts.emptyValue});
                continue;
            }

            if (c.width > 0 && keel::as<keel::i32>(tok.size()) > c.width)
                tok = tok.substr(0, keel::as<keel::usize>(c.width));

            if (c.spec == 's') {
                if (!c.suppress)
                    stageStr.push_back({i, keel::String::from(tok, scratch.allocator())});
                continue;
            }

            tokBuf.assign(tok);
            const char *start = tokBuf.c_str();
            char *endp = nullptr;
            keel::f64 v = 0.0;
            switch (c.spec) {
            case 'd': case 'i': {
                keel::i64 iv = std::strtoll(start, &endp, 10);
                v = keel::as<keel::f64>(iv);
                break;
            }
            case 'u': {
                keel::u64 uv = std::strtoull(start, &endp, 10);
                v = keel::as<keel::f64>(uv);
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G':
                v = std::strtod(start, &endp); break;
            case 'x': case 'X': {
                keel::u64 uv = std::strtoull(start, &endp, 16);
                v = keel::as<keel::f64>(uv);
                break;
            }
            case 'o': {
                keel::u64 uv = std::strtoull(start, &endp, 8);
                v = keel::as<keel::f64>(uv);
                break;
            }
            }
            if (endp == start) { fullCycle = false; break; }
            if (!c.suppress) stageNum.push_back({i, v});
        }

        if (!fullCycle) {
            pos = savedPos;
            atFirstField = savedFirst;
            break;
        }

        for (auto &p : stageNum) numCols[p.col].push_back(p.val);
        for (auto &p : stageStr) strCols[p.col].push_back(keel::move(p.val));
        ++cycles;
    }

    if (srcFile)
        srcFile->cursor += pos;

    // Build the outer cell array — one slot per non-suppressed
    // conversion. Suppressed conversions contribute no column.
    keel::usize nonSup = 0;
    for (auto &c : convs) if (!c.suppress) ++nonSup;
    Value result = Value::cell(1, nonSup);
    keel::usize slot = 0;
    for (keel::usize i = 0; i < convs.size(); ++i) {
        if (convs[i].suppress) continue;
        if (convs[i].spec == 's') {
            keel::usize k = strCols[i].size();
            Value inner = Value::cell(k, 1);
            for (keel::usize j = 0; j < k; ++j)
                inner.cellAt(j) = Value::fromString(strCols[i][j], mr);
            result.cellAt(slot++) = keel::move(inner);
        } else {
            keel::usize k = numCols[i].size();
            Value col = (k == 0)
                             ? Value::matrix(0, 0, ValueType::DOUBLE, mr)
                             : Value::matrix(k, 1, ValueType::DOUBLE, mr);
            if (k > 0)
                std::memcpy(col.doubleDataMut(), numCols[i].data(),
                            k * sizeof(keel::f64));
            result.cellAt(slot++) = keel::move(col);
        }
    }
    outs[0] = keel::move(result);
}

} // namespace numkit::runtime
