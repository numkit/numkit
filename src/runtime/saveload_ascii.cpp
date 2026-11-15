// runtime/src/saveload_ascii.cpp
//
// Workspace-persistence ASCII backend (save -ascii / load ASCII matrices).

#include <keel/core.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/arena.h>
#include <keel/array.h>
#include <numkit/runtime/saveload.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/environment.hpp>
#include <numkit/core/types.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace numkit::runtime {

void saveAscii(Engine &engine, Environment &env,
               const std::string &filename,
               const std::vector<std::string> &varnames)
{
    if (varnames.empty())
        throw Error("save: at least one variable name is required");

    std::ostringstream out;
    for (keel::usize vi = 0; vi < varnames.size(); ++vi) {
        Value *v = env.get(varnames[vi]);
        if (!v)
            throw Error("save: variable '" + varnames[vi] + "' not found");
        if (v->type() != ValueType::DOUBLE)
            throw Error("save: only numeric (double) variables supported in ascii mode");
        auto d = v->dims();
        keel::usize rows = d.rows();
        keel::usize cols = d.cols();
        if (vi > 0) out << "\n";
        if (rows == 0 || cols == 0) continue;
        for (keel::usize r = 0; r < rows; ++r) {
            for (keel::usize c = 0; c < cols; ++c) {
                if (c > 0) out << " ";
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", (*v)(r, c));
                out << buf;
            }
            out << "\n";
        }
    }

    auto resolved = engine.resolvePath(filename);
    try {
        resolved.fs->writeFile(resolved.path, out.str());
    } catch (const std::exception &e) {
        throw Error(std::string("save: ") + e.what());
    }
}

void loadAscii(Engine &engine, Environment &env,
               const std::string &filename,
               keel::usize nargout, keel::MutArrayRef<Value> outs)
{
    keel::Allocator *mr = engine.allocator();
    auto resolved = engine.resolvePath(filename);
    std::string content;
    try {
        content = resolved.fs->readFile(resolved.path);
    } catch (const std::exception &e) {
        throw Error(std::string("load: ") + e.what());
    }

    // Parse each non-empty, non-comment line as whitespace-separated
    // doubles. MATLAB ignores '%' and '#' line comments.
    keel::ArenaTemp scratch = keel::get_scratch(mr);
    keel::Array<keel::Array<double>> rows(scratch.allocator());
    keel::usize p = 0;
    while (p <= content.size()) {
        keel::usize nl = content.find('\n', p);
        keel::usize end = (nl == std::string::npos) ? content.size() : nl;
        if (end == p && nl == std::string::npos) break;
        std::string line = content.substr(p, end - p);
        p = (nl == std::string::npos) ? content.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        keel::usize ws = line.find_first_not_of(" \t");
        if (ws == std::string::npos) continue;
        if (line[ws] == '%' || line[ws] == '#') continue;

        keel::Array<double> row(scratch.allocator());
        keel::usize q = ws;
        while (q < line.size()) {
            while (q < line.size() && (std::isspace(static_cast<unsigned char>(line[q])) || line[q] == ',')) ++q;
            if (q >= line.size()) break;
            const char *start = line.c_str() + q;
            char *endp = nullptr;
            double v = std::strtod(start, &endp);
            if (endp == start)
                throw Error("load: parse error near '" + line.substr(q) + "'");
            row.push_back(v);
            q = static_cast<keel::usize>(endp - line.c_str());
        }
        rows.push_back(std::move(row));
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
        double *data = M.doubleDataMut();
        for (keel::usize r = 0; r < nrows; ++r)
            for (keel::usize c = 0; c < cols; ++c)
                data[c * nrows + r] = rows[r][c];
    }

    if (nargout > 0) {
        outs[0] = std::move(M);
        return;
    }

    // No LHS — MATLAB assigns to a variable named after the file's
    // stem (basename without directory and extension).
    std::string stem = filename;
    keel::usize sep = stem.find_last_of("/\\:");
    if (sep != std::string::npos) stem = stem.substr(sep + 1);
    keel::usize dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
    if (stem.empty() || !(std::isalpha(static_cast<unsigned char>(stem[0])) || stem[0] == '_'))
        throw Error("load: cannot derive a valid variable name from filename");
    env.set(stem, M);
    engine.assignToCaller(0, stem, std::move(M));
}

} // namespace numkit::runtime
