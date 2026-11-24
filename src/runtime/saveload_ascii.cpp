// runtime/src/saveload_ascii.cpp
//
// Workspace-persistence ASCII backend (save -ascii / load ASCII matrices).

#include <keel/arena.h>
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/runtime/saveload.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>
#include <keel/number.h>

namespace numkit::runtime {

void saveAscii(Engine &engine, Environment &env,
               keel::StringRef filename,
               keel::ArrayRef<keel::String> varnames)
{
    if (varnames.empty())
        throw Error("save: at least one variable name is required");

    keel::String out;
    for (keel::usize vi = 0; vi < varnames.size(); ++vi) {
        Value *v = env.get(varnames[vi].as_ref());
        if (!v)
            throw Error(keel::format("save: variable '{}' not found", varnames[vi]));
        if (v->type() != ValueType::DOUBLE)
            throw Error("save: only numeric (double) variables supported in ascii mode");
        auto d = v->dims();
        keel::usize rows = d.rows();
        keel::usize cols = d.cols();
        if (vi > 0) out.push_back('\n');
        if (rows == 0 || cols == 0) continue;
        for (keel::usize r = 0; r < rows; ++r) {
            for (keel::usize c = 0; c < cols; ++c) {
                if (c > 0) out.push_back(' ');
                char buf[64];
                out.append(keel::format_f64_general_to(buf, (*v)(r, c), 17));
            }
            out.push_back('\n');
        }
    }

    auto resolved = engine.resolvePath(filename);
    try {
        resolved.fs->writeFile(resolved.path.as_ref(), out.as_ref());
    } catch (const std::exception &e) {
        throw Error(keel::format("save: {}", e.what()));
    }
}

void loadAscii(Engine &engine, Environment &env,
               keel::StringRef filename,
               keel::usize nargout, keel::MutArrayRef<Value> outs)
{
    keel::Allocator *mr = engine.allocator();
    auto resolved = engine.resolvePath(filename);
    keel::String content;
    try {
        content = resolved.fs->readFile(resolved.path.as_ref());
    } catch (const std::exception &e) {
        throw Error(keel::format("load: {}", e.what()));
    }

    // Parse each non-empty, non-comment line as whitespace-separated
    // doubles. MATLAB ignores '%' and '#' line comments.
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::Array<keel::Array<keel::f64>> rows(scratch.allocator());
    keel::String line(scratch.allocator());
    keel::usize p = 0;
    while (p <= content.size()) {
        keel::usize nl = content.find('\n', p);
        keel::usize end = (nl == keel::kInvalidIndex) ? content.size() : nl;
        if (end == p && nl == keel::kInvalidIndex) break;
        line.assign(content.substr(p, end - p));
        p = (nl == keel::kInvalidIndex) ? content.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        keel::usize ws = 0;
        while (ws < line.size() && (line[ws] == ' ' || line[ws] == '\t'))
            ++ws;
        if (ws == line.size()) continue;
        if (line[ws] == '%' || line[ws] == '#') continue;

        keel::Array<keel::f64> row(scratch.allocator());
        keel::usize q = ws;
        while (q < line.size()) {
            while (q < line.size() && (keel::is_ascii_space(line[q]) || line[q] == ',')) ++q;
            if (q >= line.size()) break;
            keel::usize tstart = q;
            while (q < line.size() && !keel::is_ascii_space(line[q]) && line[q] != ',') ++q;
            keel::StringRef tok = line.as_ref().substr(tstart, q - tstart);
            auto parsed = keel::parse_f64(tok);
            if (!parsed.is_ok())
                throw Error(keel::format("load: parse error near '{}'", line.substr(tstart)));
            row.push_back(parsed.value());
        }
        rows.push_back(keel::move(row));
    }

    if (rows.empty())
        throw Error("load: no numeric data found");
    keel::usize cols = rows[0].size();
    for (auto &r : rows) {
        if (r.size() != cols)
            throw Error("load: inconsistent column count across rows");
    }
    keel::usize nrows = rows.size();

    Value M;
    if (nrows == 1 && cols == 1) {
        M = Value::scalar(rows[0][0], mr);
    } else {
        M = Value::matrix(nrows, cols, ValueType::DOUBLE, mr);
        keel::f64 *data = M.doubleDataMut();
        for (keel::usize r = 0; r < nrows; ++r)
            for (keel::usize c = 0; c < cols; ++c)
                data[c * nrows + r] = rows[r][c];
    }

    if (nargout > 0) {
        outs[0] = keel::move(M);
        return;
    }

    // No LHS — MATLAB assigns to a variable named after the file's
    // stem (basename without directory and extension).
    keel::StringRef stem = filename;
    for (keel::usize i = stem.size(); i > 0; --i) {
        char c = stem[i - 1];
        if (c == '/' || c == '\\' || c == ':') {
            stem = stem.substr(i);
            break;
        }
    }
    keel::usize dot = stem.rfind('.');
    if (dot != keel::kInvalidIndex && dot > 0)
        stem = stem.substr(0, dot);
    if (stem.empty() || !(keel::is_ascii_alpha(stem[0]) || stem[0] == '_'))
        throw Error("load: cannot derive a valid variable name from filename");
    env.set(stem, M);
    engine.assignToCaller(0, stem, keel::move(M));
}

} // namespace numkit::runtime
