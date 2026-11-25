// runtime/containers/table_io.cpp
//
// Spreadsheet (XLSX) and delimited text (CSV/TSV) table I/O plus identifier
// sanitization (`makeValidName`).
#include "table_internal.hpp"

#include <keel/algo.h>
#include <keel/array.h>
#include <keel/compress.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/utf8.h>

#include <numkit/core/engine.hpp>
#include <numkit/ops/deflate.hpp>
#include <numkit/sys/fs_context.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/object.hpp>

namespace numkit::runtime::containers_detail {

namespace {

// ── Identifier sanitization + Spreadsheet / Delimited Table I/O ──────────
static auto makeValidIdStr = [](keel::StringRef raw) -> keel::String {
    keel::usize a = 0, b = raw.size();
    while (a < b && keel::is_ascii_space(raw[a])) ++a;
    while (b > a && keel::is_ascii_space(raw[b - 1])) --b;
    keel::String s;
    s.reserve(b - a + 1);
    for (keel::usize i = a; i < b; ++i) {
        keel::u8 ch = keel::as<keel::u8>(raw[i]);
        if (keel::is_ascii_alnum(ch) || ch == '_')
            s.push_back(keel::as<char>(ch));
        else
            s.push_back('_');
    }
    if (s.empty() || !keel::is_ascii_alpha(s[0])) {
        keel::String prefixed;
        prefixed.reserve(s.size() + 1);
        prefixed.push_back('x');
        prefixed.append(s.as_ref());
        return prefixed;
    }
    return s;
};

void makeValidName_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 5, "makeValidName");
    auto *mr = ctx.engine->allocator();
    const Value &in = args[0];
    bool modified = false;
    auto convOne = [&](keel::StringRef sr) -> keel::String {
        keel::String v = makeValidIdStr(sr);
        if (v != sr) modified = true;
        return v;
    };
    if (in.isChar()) {
        outs[0] = Value::fromString(convOne(in.toString()), mr);
    } else if (in.isString()) {
        Value out = Value::stringArray(in.dims().rows(), in.dims().cols(), mr);
        for (keel::usize i = 0; i < in.numel(); ++i)
            out.stringElemSet(i, convOne(in.stringElem(i)));
        outs[0] = keel::move(out);
    } else if (in.isCell()) {
        Value out = Value::cell(in.dims().rows(), in.dims().cols(), mr);
        for (keel::usize i = 0; i < in.numel(); ++i)
            out.cellAt(i) = Value::fromString(convOne(in.cellAt(i).toString()), mr);
        outs[0] = keel::move(out);
    } else {
        throw Error("makeValidName: input must be a char vector, string array, or cell array of char vectors",
                    0, 0, "makeValidName", "", "numkit:makeValidName:invalidInput");
    }
    if (nargout >= 2 && outs.size() >= 2)
        outs[1] = Value::logicalScalar(modified, mr);
}


static auto unescapeXmlStr = [](keel::StringRef s) -> keel::String {
    keel::String out;
    out.reserve(s.size());
    auto matchAt = [&](keel::usize pos, keel::StringRef tok) -> bool {
        return pos + tok.size() <= s.size() && s.substr(pos, tok.size()) == tok;
    };
    for (keel::usize i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (matchAt(i, "&amp;"))  { out.push_back('&');  i += 4; continue; }
            if (matchAt(i, "&lt;"))   { out.push_back('<');  i += 3; continue; }
            if (matchAt(i, "&gt;"))   { out.push_back('>');  i += 3; continue; }
            if (matchAt(i, "&quot;")) { out.push_back('"');  i += 5; continue; }
            if (matchAt(i, "&apos;")) { out.push_back('\''); i += 5; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
};


static auto parseXlsxGrid = [](keel::StringRef bytes, keel::StringRef sheetSpec,
                               keel::i32 sheetNumSpec) -> keel::Array<keel::Array<GridCell>> {
    keel::ArrayRef<keel::u8> rawBytes(reinterpret_cast<const keel::u8 *>(bytes.data()), bytes.size());
    auto zrRes = keel::compress::ZipReader::open(rawBytes);
    if (zrRes.is_err())
        return {};
    const auto &zr = zrRes.value();
    auto readEntry = [&](keel::StringRef name) -> keel::String {
        auto ex = zr.extract(name);
        if (ex.is_err()) return {};
        return keel::String::from(keel::StringRef(
            reinterpret_cast<const char *>(ex->data()), ex->size()));
    };

    keel::Array<keel::String> sst;
    keel::String sstXml = readEntry("xl/sharedStrings.xml");
    if (!sstXml.empty()) {
        keel::usize pos = 0;
        while (pos < sstXml.size()) {
            keel::usize siStart = sstXml.find("<si", pos);
            if (siStart == keel::kInvalidIndex) break;
            keel::usize siOpenEnd = sstXml.find('>', siStart);
            keel::usize siClose = sstXml.find("</si>", siStart);
            if (siOpenEnd == keel::kInvalidIndex || siClose == keel::kInvalidIndex) break;
            keel::String accum;
            keel::usize tPos = siOpenEnd + 1;
            while (tPos < siClose) {
                keel::usize tStart = sstXml.find("<t", tPos);
                if (tStart == keel::kInvalidIndex || tStart >= siClose) break;
                char nextCh = (tStart + 2 < sstXml.size()) ? sstXml[tStart + 2] : '\0';
                if (nextCh != '>' && nextCh != ' ' && nextCh != '/') {
                    tPos = tStart + 2;
                    continue;
                }
                keel::usize tOpenEnd = sstXml.find('>', tStart);
                if (tOpenEnd == keel::kInvalidIndex || tOpenEnd >= siClose) break;
                if (sstXml[tOpenEnd - 1] == '/') {
                    tPos = tOpenEnd + 1;
                    continue;
                }
                keel::usize tClose = sstXml.find("</t>", tOpenEnd + 1);
                if (tClose == keel::kInvalidIndex || tClose > siClose) break;
                accum.append(unescapeXmlStr(sstXml.substr(tOpenEnd + 1, tClose - (tOpenEnd + 1))).as_ref());
                tPos = tClose + 4;
            }
            sst.push_back(keel::move(accum));
            pos = siClose + 5;
        }
    }

    keel::i32 sheetIdx = (sheetNumSpec >= 1) ? sheetNumSpec : 1;
    if (!sheetSpec.empty()) {
        keel::String wbXml = readEntry("xl/workbook.xml");
        keel::usize pos = 0;
        keel::i32 idx = 1;
        while (pos < wbXml.size()) {
            keel::usize shStart = wbXml.find("<sheet ", pos);
            if (shStart == keel::kInvalidIndex) break;
            keel::usize shEnd = wbXml.find('>', shStart);
            if (shEnd == keel::kInvalidIndex) break;
            keel::StringRef tag = wbXml.substr(shStart, shEnd - shStart);
            keel::usize nmPos = tag.find("name=\"");
            if (nmPos != keel::kInvalidIndex) {
                keel::usize nmEnd = tag.find('"', nmPos + 6);
                if (nmEnd != keel::kInvalidIndex) {
                    keel::String nm = unescapeXmlStr(tag.substr(nmPos + 6, nmEnd - (nmPos + 6)));
                    if (nm == sheetSpec) {
                        sheetIdx = idx;
                        break;
                    }
                }
            }
            ++idx;
            pos = shEnd + 1;
        }
    }

    keel::String wsXml = readEntry(keel::format("xl/worksheets/sheet{}.xml", sheetIdx));
    if (wsXml.empty() && sheetIdx != 1)
        wsXml = readEntry("xl/worksheets/sheet1.xml");
    if (wsXml.empty())
        return {};

    struct RawCell { keel::usize r, c; bool isStr; keel::String text; };
    keel::Array<RawCell> cells;
    keel::usize maxR = 0, maxC = 0;
    keel::usize pos = 0;
    while (pos < wsXml.size()) {
        keel::usize cStart = wsXml.find("<c ", pos);
        if (cStart == keel::kInvalidIndex) break;
        keel::usize cOpenEnd = wsXml.find('>', cStart);
        if (cOpenEnd == keel::kInvalidIndex) break;
        if (wsXml[cOpenEnd - 1] == '/') {
            pos = cOpenEnd + 1;
            continue;
        }
        keel::usize cClose = wsXml.find("</c>", cOpenEnd + 1);
        if (cClose == keel::kInvalidIndex) break;
        keel::StringRef openTag = wsXml.substr(cStart, cOpenEnd - cStart);
        keel::StringRef body = wsXml.substr(cOpenEnd + 1, cClose - (cOpenEnd + 1));
        pos = cClose + 4;

        auto getAttr = [&](const char *attr) -> keel::StringRef {
            keel::String key = keel::format(" {}=\"", attr);
            keel::usize p = openTag.find(key.as_ref());
            if (p == keel::kInvalidIndex) return {};
            p += key.size();
            keel::usize e = openTag.find('"', p);
            return (e == keel::kInvalidIndex) ? keel::StringRef{} : openTag.substr(p, e - p);
        };
        keel::StringRef ref = getAttr("r");
        keel::StringRef tAttr = getAttr("t");
        if (ref.empty() || tAttr == "e")
            continue;

        keel::usize colNum = 0, rowNum = 0, k = 0;
        while (k < ref.size() && keel::is_ascii_alpha(ref[k])) {
            colNum = colNum * 26 + (keel::to_ascii_upper(ref[k]) - 'A' + 1);
            ++k;
        }
        while (k < ref.size() && keel::is_ascii_digit(ref[k])) {
            rowNum = rowNum * 10 + (ref[k] - '0');
            ++k;
        }
        if (colNum == 0 || rowNum == 0)
            continue;

        keel::String valText;
        bool isStr = false;
        if (tAttr == "inlineStr") {
            keel::usize tOpen = body.find("<t");
            if (tOpen != keel::kInvalidIndex) {
                keel::usize tGt = body.find('>', tOpen);
                keel::usize tEnd = (tGt != keel::kInvalidIndex) ? body.find("</t>", tGt) : keel::kInvalidIndex;
                if (tGt != keel::kInvalidIndex && tEnd != keel::kInvalidIndex) {
                    valText = unescapeXmlStr(body.substr(tGt + 1, tEnd - (tGt + 1)));
                    isStr = true;
                }
            }
        } else {
            keel::usize vOpen = body.find("<v>");
            keel::usize vBodyStart = (vOpen != keel::kInvalidIndex) ? (vOpen + 3) : keel::kInvalidIndex;
            if (vBodyStart == keel::kInvalidIndex) {
                keel::usize vTag = body.find("<v ");
                if (vTag != keel::kInvalidIndex) {
                    keel::usize vGt = body.find('>', vTag);
                    if (vGt != keel::kInvalidIndex && body[vGt - 1] != '/')
                        vBodyStart = vGt + 1;
                }
            }
            if (vBodyStart == keel::kInvalidIndex)
                continue;
            keel::usize vClose = body.find("</v>", vBodyStart);
            if (vClose == keel::kInvalidIndex)
                continue;
            keel::String rawV = keel::String::from(body.substr(vBodyStart, vClose - vBodyStart));
            if (rawV.empty())
                continue;
            if (tAttr == "s") {
                keel::StringRef rawTrim = rawV.as_ref();
                while (!rawTrim.empty() && keel::is_ascii_space(rawTrim[0]))
                    rawTrim = rawTrim.substr(1);
                while (!rawTrim.empty() && keel::is_ascii_space(rawTrim[rawTrim.size() - 1]))
                    rawTrim = rawTrim.substr(0, rawTrim.size() - 1);
                auto parsedOpt = keel::parse_i64(rawTrim);
                keel::i32 sidx = parsedOpt.is_ok() ? keel::as<keel::i32>(parsedOpt.value()) : -1;
                if (sidx >= 0 && keel::as<keel::usize>(sidx) < sst.size())
                    valText = sst[keel::as<keel::usize>(sidx)].clone();
                isStr = true;
            } else {
                valText = unescapeXmlStr(rawV.as_ref());
                isStr = (tAttr == "str");
            }
        }
        keel::usize r0 = rowNum - 1, c0 = colNum - 1;
        cells.push_back(RawCell{r0, c0, isStr, keel::move(valText)});
        if (r0 + 1 > maxR) maxR = r0 + 1;
        if (c0 + 1 > maxC) maxC = c0 + 1;
    }
    keel::Array<keel::Array<GridCell>> grid;
    grid.reserve(maxR);
    for (keel::usize r = 0; r < maxR; ++r) {
        keel::Array<GridCell> row;
        row.resize(maxC);
        grid.push_back(keel::move(row));
    }
    for (auto &cell : cells)
        grid[cell.r][cell.c] = GridCell{true, cell.isStr, keel::move(cell.text)};
    return grid;
};

static auto parseCsvGrid = [](keel::StringRef content) -> keel::Array<keel::Array<GridCell>> {
    keel::Array<keel::Array<GridCell>> rows;
    keel::usize maxC = 0;
    keel::usize pos = 0;
    while (pos < content.size()) {
        keel::usize nl = content.find('\n', pos);
        keel::usize end = (nl == keel::kInvalidIndex) ? content.size() : nl;
        keel::StringRef line = content.substr(pos, end - pos);
        if (!line.empty() && line[line.size() - 1] == '\r')
            line = line.substr(0, line.size() - 1);
        pos = (nl == keel::kInvalidIndex) ? content.size() : (nl + 1);
        bool allWs = true;
        for (keel::usize ci = 0; ci < line.size(); ++ci)
            if (!keel::is_ascii_space(line[ci])) { allWs = false; break; }
        if (allWs)
            continue;

        keel::Array<GridCell> row;
        keel::usize i = 0;
        while (i <= line.size()) {
            if (i == line.size()) {
                if (!line.empty() && (line[line.size() - 1] == ',' || line[line.size() - 1] == '\t'))
                    row.push_back(GridCell{false, false, keel::String()});
                break;
            }
            if (line[i] == '"') {
                keel::String tok;
                ++i;
                while (i < line.size()) {
                    if (line[i] == '"') {
                        if (i + 1 < line.size() && line[i + 1] == '"') {
                            tok.push_back('"');
                            i += 2;
                        } else {
                            ++i;
                            break;
                        }
                    } else {
                        tok.push_back(line[i++]);
                    }
                }
                while (i < line.size() && line[i] != ',' && line[i] != '\t')
                    ++i;
                if (i < line.size()) ++i;
                bool present = !tok.empty();
                row.push_back(GridCell{present, true, keel::move(tok)});
            } else {
                keel::usize nextSep = i;
                while (nextSep < line.size() && line[nextSep] != ',' && line[nextSep] != '\t')
                    ++nextSep;
                keel::StringRef rawTok = line.substr(i, nextSep - i);
                keel::usize a = 0, b = rawTok.size();
                while (a < b && keel::is_ascii_space(rawTok[a])) ++a;
                while (b > a && keel::is_ascii_space(rawTok[b - 1])) --b;
                keel::String tok = keel::String::from(rawTok.substr(a, b - a));
                bool present = !tok.empty();
                row.push_back(GridCell{present, false, keel::move(tok)});
                if (nextSep == line.size())
                    break;
                i = nextSep + 1;
            }
        }
        if (row.size() > maxC) maxC = row.size();
        rows.push_back(keel::move(row));
    }
    for (auto &r : rows)
        r.resize(maxC);
    return rows;
};

static auto tryParseCellDouble = [](const keel::String &s, keel::f64 &out) -> bool {
    keel::StringRef sr = s.as_ref();
    while (!sr.empty() && keel::is_ascii_space(sr[0]))
        sr = sr.substr(1);
    while (!sr.empty() && keel::is_ascii_space(sr[sr.size() - 1]))
        sr = sr.substr(0, sr.size() - 1);
    if (sr.empty()) return false;
    auto parsed = keel::parse_f64(sr);
    if (!parsed.is_ok()) return false;
    out = parsed.value();
    return true;
};

static auto readRawFileBytes = [](CallContext &ctx, keel::StringRef filename,
                                  const char *fnName) -> keel::String {
    FsContext::ResolvedPath resolved{};
    try {
        resolved = ctx.engine->resolvePath(filename);
    } catch (const std::exception &e) {
        throw Error(e.what());
    }
    if (!resolved.fs)
        throw Error(keel::format("{}: cannot resolve '{}'", fnName, filename),
                    0, 0, fnName, "", keel::format("numkit:{}:noFs", fnName));
    try {
        return resolved.fs->readFile(resolved.path);
    } catch (const std::exception &e) {
        throw Error(keel::format("{}: {}", fnName, e.what()),
                    0, 0, fnName, "", keel::format("numkit:{}:readFailed", fnName));
    }
};

static keel::Array<keel::Array<GridCell>> parseSpreadsheetOrCsvGrid(
    keel::StringRef raw, keel::StringRef sheetName, keel::i32 sheetNum)
{
    const auto *b = reinterpret_cast<const keel::u8 *>(raw.data());
    const bool isZip = raw.size() >= 4
                    && b[0] == 0x50u && b[1] == 0x4Bu && b[2] == 0x03u && b[3] == 0x04u;
    if (isZip) return parseXlsxGrid(raw, sheetName, sheetNum);
    const bool isOle2 = raw.size() >= 8
                     && b[0] == 0xD0u && b[1] == 0xCFu && b[2] == 0x11u && b[3] == 0xE0u
                     && b[4] == 0xA1u && b[5] == 0xB1u && b[6] == 0x1Au && b[7] == 0xE1u;
    if (isOle2) return parseXlsBiff8Grid(raw, sheetName, sheetNum);
    return parseCsvGrid(raw);
}

void detectImportOptions_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 15, "detectImportOptions");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    keel::String filename = args[0].toString();
    keel::String sheetName(alloc);
    keel::i32 sheetNum = 1;
    bool preserveNames = false;
    for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString()) continue;
        keel::String k = args[i].toString();
        if (iequalsTableOpt(k, "Sheet")) {
            if (args[i + 1].isNumeric()) sheetNum = keel::as<keel::i32>(args[i + 1].elemAsDouble(0));
            else sheetName = args[i + 1].toString();
        } else if (iequalsTableOpt(k, "VariableNamingRule")) {
            preserveNames = iequalsTableOpt(args[i + 1].toString(), "preserve");
        }
    }
    keel::String raw = readRawFileBytes(ctx, filename, "detectImportOptions");
    auto grid = parseSpreadsheetOrCsvGrid(raw.as_ref(), sheetName.as_ref(), sheetNum);
    keel::Array<keel::String> varNames(alloc);
    auto hasVarName = [&](keel::StringRef nm) -> bool {
        for (const auto &v : varNames)
            if (v == nm) return true;
        return false;
    };
    if (!grid.empty()) {
        for (keel::usize c = 0; c < grid[0].size(); ++c) {
            keel::String nm = grid[0][c].text.clone(alloc);
            if (!preserveNames && !nm.empty()) nm = makeValidIdStr(nm.as_ref());
            if (nm.empty()) nm = keel::format_alloc(alloc, "Var{}", c + 1);
            keel::String base = nm.clone(alloc);
            keel::i32 suffix = 1;
            while (hasVarName(nm.as_ref()))
                nm = keel::format_alloc(alloc, "{}_{}", base, suffix++);
            varNames.push_back(keel::move(nm));
        }
    }
    Value vnCell = Value::cell(1, varNames.size(), mr);
    Value vtCell = Value::cell(1, varNames.size(), mr);
    for (keel::usize c = 0; c < varNames.size(); ++c) {
        vnCell.cellAt(c) = Value::fromString(varNames[c], mr);
        vtCell.cellAt(c) = Value::fromString("double", mr);
    }
    Value vnOrigCell = Value::cell(1, grid.empty() ? 0 : grid[0].size(), mr);
    if (!grid.empty()) {
        for (keel::usize c = 0; c < grid[0].size(); ++c)
            vnOrigCell.cellAt(c) = Value::fromString(grid[0][c].text, mr);
    }
    Value opts = Value::structure(mr);
    opts.field("VariableNames") = vnCell;
    opts.field("OriginalVariableNames") = keel::move(vnOrigCell);
    opts.field("SelectedVariableNames") = vnCell;
    opts.field("VariableTypes") = keel::move(vtCell);
    opts.field("Sheet") = Value::fromString(sheetName, mr);
    opts.field("SheetNum") = Value::scalar(keel::as<keel::f64>(sheetNum), mr);
    opts.field("PreserveVariableNames") = Value::logicalScalar(preserveNames, mr);
    opts.field("VariableNamingRule") = Value::fromString(preserveNames ? "preserve" : "modify", mr);
    opts.field("TreatAsMissing") = Value::cell(0, 0, mr);
    outs[0] = keel::move(opts);
}

void setvaropts_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, 15, "setvaropts");
    auto *mr = ctx.engine->allocator();
    Value opts = args[0];
    if (!opts.isStruct()) {
        outs[0] = opts;
        return;
    }
    keel::usize kvStart = 1;
    if (args.size() % 2 == 0)
        kvStart = 2;
    for (keel::usize i = kvStart; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString()) continue;
        keel::String k = args[i].toString();
        if (iequalsTableOpt(k, "TreatAsMissing")) {
            auto tokens = TablePayload::extractNames(args[i + 1], "setvaropts", "TreatAsMissing");
            Value c = Value::cell(1, tokens.size(), mr);
            for (keel::usize j = 0; j < tokens.size(); ++j)
                c.cellAt(j) = Value::fromString(tokens[j], mr);
            opts.field("TreatAsMissing") = keel::move(c);
        }
    }
    outs[0] = keel::move(opts);
}

void readtable_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 15, "readtable");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("readtable: first argument must be a filename",
                    0, 0, "readtable", "", "numkit:readtable:invalidFilename");
    auto *mr = ctx.engine->allocator();
    auto *alloc = mr ? mr : keel::default_allocator();
    keel::String filename = args[0].toString();
    keel::String sheetName(alloc);
    keel::i32 sheetNum = 1;
    bool preserveNames = false;
    keel::Array<keel::String> treatAsMissing(alloc);
    keel::usize kvStart = 1;
    if (args.size() >= 2 && args[1].isStruct()) {
        const Value &opts = args[1];
        if (opts.hasField("Sheet") && !opts.field("Sheet").toString().empty())
            sheetName = opts.field("Sheet").toString();
        if (opts.hasField("SheetNum") && opts.field("SheetNum").isNumeric())
            sheetNum = keel::as<keel::i32>(opts.field("SheetNum").elemAsDouble(0));
        if (opts.hasField("PreserveVariableNames") && opts.field("PreserveVariableNames").toBool())
            preserveNames = true;
        if (opts.hasField("VariableNamingRule")
            && iequalsTableOpt(opts.field("VariableNamingRule").toString(), "preserve"))
            preserveNames = true;
        if (opts.hasField("TreatAsMissing") && !opts.field("TreatAsMissing").isEmpty())
            treatAsMissing = TablePayload::extractNames(opts.field("TreatAsMissing"), "readtable", "TreatAsMissing", alloc);
        kvStart = 2;
    }
    for (keel::usize i = kvStart; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString())
            continue;
        keel::String k = args[i].toString();
        if (iequalsTableOpt(k, "Sheet")) {
            if (args[i + 1].isNumeric())
                sheetNum = keel::as<keel::i32>(args[i + 1].elemAsDouble(0));
            else
                sheetName = args[i + 1].toString();
        } else if (iequalsTableOpt(k, "VariableNamingRule")) {
            preserveNames = iequalsTableOpt(args[i + 1].toString(), "preserve");
        } else if (iequalsTableOpt(k, "TreatAsMissing")) {
            treatAsMissing = TablePayload::extractNames(args[i + 1], "readtable", "TreatAsMissing", alloc);
        }
    }
    auto isMissingToken = [&](keel::StringRef tok) -> bool {
        for (const auto &m : treatAsMissing)
            if (tok == m) return true;
        return false;
    };

    keel::String raw = readRawFileBytes(ctx, filename, "readtable");
    auto grid = parseSpreadsheetOrCsvGrid(raw.as_ref(), sheetName.as_ref(), sheetNum);
    auto p = keel::make_arc_in<TablePayload>(alloc, alloc);
    if (grid.empty()) {
        outs[0] = makeTableValue(keel::move(p), mr);
        return;
    }
    const keel::usize maxR = grid.size();
    const keel::usize maxC = grid[0].size();
    const keel::usize nrows = (maxR > 1) ? (maxR - 1) : 0;
    p->nrows = nrows;

    for (keel::usize c = 0; c < maxC; ++c) {
        keel::String nm = grid[0][c].text.clone(alloc);
        if (!preserveNames && !nm.empty())
            nm = makeValidIdStr(nm.as_ref());
        if (nm.empty())
            nm = keel::format_alloc(alloc, "Var{}", c + 1);
        keel::String base = nm.clone(alloc);
        keel::i32 suffix = 1;
        while (p->findVar(nm) >= 0) {
            nm = keel::format_alloc(alloc, "{}_{}", base, suffix++);
        }
        p->varNames.push_back(keel::move(nm));

        keel::usize numCount = 0, strCount = 0;
        for (keel::usize r = 1; r < maxR; ++r) {
            const auto &cell = grid[r][c];
            if (!cell.present || cell.text.empty() || isMissingToken(cell.text))
                continue;
            keel::f64 dummy = 0.0;
            if (!cell.isStr && tryParseCellDouble(cell.text, dummy))
                ++numCount;
            else
                ++strCount;
        }
        bool isNumCol = (numCount > 0 && numCount >= strCount)
                     || (numCount == 0 && strCount == 0);
        if (isNumCol) {
            Value col = Value::matrix(nrows, 1, ValueType::DOUBLE, mr);
            keel::f64 *dst = col.doubleDataMut();
            const keel::f64 nan = keel::Limits<keel::f64>::quiet_nan();
            for (keel::usize r = 1; r < maxR; ++r) {
                const auto &cell = grid[r][c];
                keel::f64 d = nan;
                if (cell.present && !cell.isStr && !cell.text.empty() && !isMissingToken(cell.text))
                    tryParseCellDouble(cell.text, d);
                dst[r - 1] = d;
            }
            p->cols.push_back(keel::move(col));
        } else {
            Value col = Value::cell(nrows, 1, mr);
            for (keel::usize r = 1; r < maxR; ++r)
                col.cellAt(r - 1) = Value::fromString(grid[r][c].text, mr);
            p->cols.push_back(keel::move(col));
        }
    }
    outs[0] = makeTableValue(keel::move(p), mr);
}


static auto buildXlsxBytes = [](const TablePayload *p) -> keel::String {
    auto xmlEsc = [](keel::StringRef s) -> keel::String {
        keel::String r;
        r.reserve(s.size());
        for (keel::usize i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '&') r.append("&amp;");
            else if (c == '<') r.append("&lt;");
            else if (c == '>') r.append("&gt;");
            else if (c == '"') r.append("&quot;");
            else r.push_back(c);
        }
        return r;
    };
    auto colLetters = [](keel::usize c) -> keel::String {
        keel::String s;
        keel::usize n = c + 1;
        while (n > 0) {
            keel::usize rem = (n - 1) % 26;
            s.push_back(keel::as<char>('A' + rem));
            n = (n - 1) / 26;
        }
        keel::reverse(s.begin(), s.end());
        return s;
    };
    keel::Array<keel::String> shared;
    auto addShared = [&](keel::StringRef s) -> keel::usize {
        keel::usize idx = shared.size();
        shared.push_back(keel::String::from(s));
        return idx;
    };
    keel::String sh;
    sh.append("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
              "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
              "<sheetData>");
    const keel::usize nc = p->width();
    sh.append("<row r=\"1\">");
    for (keel::usize j = 0; j < nc; ++j) {
        keel::usize si = addShared(p->varNames[j]);
        keel::format_into(sh, "<c r=\"{}1\" t=\"s\"><v>{}</v></c>", colLetters(j), si);
    }
    sh.append("</row>");
    for (keel::usize r = 0; r < p->nrows; ++r) {
        keel::usize rowNum = r + 2;
        keel::format_into(sh, "<row r=\"{}\">", rowNum);
        for (keel::usize j = 0; j < nc; ++j) {
            const Value &col = p->cols[j];
            keel::String ref = keel::format("{}{}", colLetters(j), rowNum);
            if (col.isNumeric()) {
                keel::f64 d = col.elemAsDouble(r);
                if (keel::is_finite(d))
                    keel::format_into(sh, "<c r=\"{}\"><v>%.15g</v></c>", ref, d);
            } else {
                keel::String sv;
                if (col.isString()) sv = col.stringElem(r);
                else if (col.isCell()) {
                    const Value &el = col.cellAt(r);
                    if (el.isChar() || el.isString()) sv = el.toString();
                } else sv = formatTableCell(col, r);
                if (!sv.empty()) {
                    keel::usize si = addShared(sv);
                    keel::format_into(sh, "<c r=\"{}\" t=\"s\"><v>{}</v></c>", ref, si);
                }
            }
        }
        sh.append("</row>");
    }
    sh.append("</sheetData></worksheet>");

    keel::String ss;
    keel::format_into(ss,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"{}\" uniqueCount=\"{}\">",
        shared.size(), shared.size());
    for (const auto &s : shared)
        keel::format_into(ss, "<si><t>{}</t></si>", xmlEsc(s));
    ss.append("</sst>");

    struct Entry { keel::String name; keel::String data; };
    keel::Array<Entry> files;
    files.reserve(7);
    files.push_back(Entry{
        keel::String::from("[Content_Types].xml"),
        keel::String::from(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
            "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
            "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
            "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>"
            "</Types>")});
    files.push_back(Entry{
        keel::String::from("_rels/.rels"),
        keel::String::from(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
            "</Relationships>")});
    files.push_back(Entry{
        keel::String::from("xl/workbook.xml"),
        keel::String::from(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
            "<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>")});
    files.push_back(Entry{
        keel::String::from("xl/_rels/workbook.xml.rels"),
        keel::String::from(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
            "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>"
            "</Relationships>")});
    files.push_back(Entry{
        keel::String::from("xl/styles.xml"),
        keel::String::from(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
            "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
            "<fills count=\"1\"><fill><patternFill patternType=\"none\"/></fill></fills>"
            "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
            "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
            "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
            "</styleSheet>")});
    files.push_back(Entry{keel::String::from("xl/sharedStrings.xml"), keel::move(ss)});
    files.push_back(Entry{keel::String::from("xl/worksheets/sheet1.xml"), keel::move(sh)});

    auto crc32Buf = [](keel::StringRef buf) -> keel::u32 {
        keel::u32 crc = 0xFFFFFFFFu;
        for (keel::usize i = 0; i < buf.size(); ++i) {
            keel::u8 b = keel::as<keel::u8>(buf[i]);
            crc ^= b;
            for (keel::i32 k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    };
    keel::String zip;
    auto put16 = [&](keel::u16 v) {
        zip.push_back(keel::as<char>(v & 0xFF));
        zip.push_back(keel::as<char>((v >> 8) & 0xFF));
    };
    auto put32 = [&](keel::u32 v) {
        zip.push_back(keel::as<char>(v & 0xFF));
        zip.push_back(keel::as<char>((v >> 8) & 0xFF));
        zip.push_back(keel::as<char>((v >> 16) & 0xFF));
        zip.push_back(keel::as<char>((v >> 24) & 0xFF));
    };
    keel::Array<keel::u32> offsets, crcs;
    for (const auto &f : files) {
        offsets.push_back(keel::as<keel::u32>(zip.size()));
        keel::u32 crc = crc32Buf(f.data);
        crcs.push_back(crc);
        keel::u32 sz = keel::as<keel::u32>(f.data.size());
        put32(0x04034b50u);
        put16(20); put16(0); put16(0); put16(0); put16(0);
        put32(crc); put32(sz); put32(sz);
        put16(keel::as<keel::u16>(f.name.size())); put16(0);
        zip.append(f.name.as_ref());
        zip.append(f.data.as_ref());
    }
    keel::u32 cdOffset = keel::as<keel::u32>(zip.size());
    for (keel::usize i = 0; i < files.size(); ++i) {
        const auto &f = files[i];
        keel::u32 sz = keel::as<keel::u32>(f.data.size());
        put32(0x02014b50u);
        put16(20); put16(20); put16(0); put16(0); put16(0); put16(0);
        put32(crcs[i]); put32(sz); put32(sz);
        put16(keel::as<keel::u16>(f.name.size())); put16(0); put16(0);
        put16(0); put16(0); put32(0); put32(offsets[i]);
        zip.append(f.name.as_ref());
    }
    keel::u32 cdSize = keel::as<keel::u32>(zip.size()) - cdOffset;
    put32(0x06054b50u);
    put16(0); put16(0);
    put16(keel::as<keel::u16>(files.size()));
    put16(keel::as<keel::u16>(files.size()));
    put32(cdSize); put32(cdOffset); put16(0);
    return zip;
};

void writetable_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 15, "writetable");
    const TablePayload *p = tableCst(args[0]);
    if (!p)
        throw Error("writetable: first argument must be a table",
                    0, 0, "writetable", "", "numkit:writetable:notATable");
    keel::String filename = (args.size() >= 2 && (args[1].isChar() || args[1].isString()))
                          ? args[1].toString()
                          : keel::String::from("table.txt");
    bool isXlsx = filename.size() >= 5
               && iequalsTableOpt(filename.substr(filename.size() - 5), ".xlsx");
    keel::String payload;
    if (isXlsx) {
        payload = buildXlsxBytes(p);
    } else {
        keel::String os;
        const keel::usize nc = p->width();
        for (keel::usize j = 0; j < nc; ++j) {
            if (j > 0) os.push_back(',');
            os.append(p->varNames[j].as_ref());
        }
        os.push_back('\n');
        for (keel::usize r = 0; r < p->nrows; ++r) {
            for (keel::usize j = 0; j < nc; ++j) {
                if (j > 0) os.push_back(',');
                const Value &col = p->cols[j];
                if (col.isNumeric()) {
                    keel::f64 d = col.elemAsDouble(r);
                    if (!keel::is_nan(d)) keel::format_into(os, "%.15g", d);
                } else if (col.isString()) {
                    os.append(col.stringElem(r).as_ref());
                } else if (col.isCell()) {
                    const Value &el = col.cellAt(r);
                    if (el.isChar() || el.isString()) os.append(el.toString().as_ref());
                    else if (el.isNumeric() && el.isScalar() && !keel::is_nan(el.toScalar()))
                        keel::format_into(os, "%.15g", el.toScalar());
                } else {
                    os.append(formatTableCell(col, r).as_ref());
                }
            }
            os.push_back('\n');
        }
        payload = keel::move(os);
    }
    FsContext::ResolvedPath resolved{};
    try {
        resolved = coreEngine(ctx).fsContext().resolvePath(filename);
        if (resolved.fs)
            resolved.fs->writeFile(resolved.path, payload);
    } catch (const std::exception &e) {
        throw Error(keel::format("writetable: {}", e.what()),
                    0, 0, "writetable", "", "numkit:writetable:writeFailed");
    }
    outs[0] = Value();
}

void xlsread_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 5, "xlsread");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("xlsread: first argument must be a filename",
                    0, 0, "xlsread", "", "numkit:xlsread:invalidFilename");
    auto *mr = ctx.engine->allocator();
    keel::String filename = args[0].toString();
    keel::String sheetName;
    keel::i32 sheetNum = 1;
    if (args.size() >= 2) {
        if (args[1].isNumeric() && !args[1].isEmpty())
            sheetNum = keel::as<keel::i32>(args[1].elemAsDouble(0));
        else if (args[1].isChar() || args[1].isString())
            sheetName = args[1].toString();
    }
    keel::String raw = readRawFileBytes(ctx, filename, "xlsread");
    auto grid = parseSpreadsheetOrCsvGrid(raw.as_ref(), sheetName.as_ref(), sheetNum);
    const keel::usize maxR = grid.size();
    const keel::usize maxC = maxR > 0 ? grid[0].size() : 0;
    keel::usize rMin = maxR, rMax = 0, cMin = maxC, cMax = 0;
    for (keel::usize r = 0; r < maxR; ++r) {
        for (keel::usize c = 0; c < maxC; ++c) {
            keel::f64 d = 0.0;
            if (grid[r][c].present && !grid[r][c].isStr
                && tryParseCellDouble(grid[r][c].text, d)) {
                rMin = keel::min(rMin, r);
                rMax = keel::max(rMax, r + 1);
                cMin = keel::min(cMin, c);
                cMax = keel::max(cMax, c + 1);
            }
        }
    }
    if (rMin < rMax && cMin < cMax) {
        keel::usize nr = rMax - rMin, nc = cMax - cMin;
        Value num = Value::matrix(nr, nc, ValueType::DOUBLE, mr);
        keel::f64 *dst = num.doubleDataMut();
        const keel::f64 nan = keel::Limits<keel::f64>::quiet_nan();
        for (keel::usize c = 0; c < nc; ++c) {
            for (keel::usize r = 0; r < nr; ++r) {
                const auto &cell = grid[rMin + r][cMin + c];
                keel::f64 d = nan;
                if (cell.present && !cell.isStr)
                    tryParseCellDouble(cell.text, d);
                dst[c * nr + r] = d;
            }
        }
        outs[0] = keel::move(num);
    } else {
        outs[0] = Value::matrix(0, 0, ValueType::DOUBLE, mr);
    }
    if (nargout >= 2 && outs.size() >= 2) {
        Value txt = Value::cell(maxR, maxC, mr);
        for (keel::usize c = 0; c < maxC; ++c)
            for (keel::usize r = 0; r < maxR; ++r) {
                keel::f64 d = 0.0;
                bool isNum = grid[r][c].present && !grid[r][c].isStr
                          && tryParseCellDouble(grid[r][c].text, d);
                txt.cellAt(c * maxR + r) = Value::fromString(
                    (grid[r][c].present && !isNum) ? grid[r][c].text.as_ref() : keel::StringRef(""), mr);
            }
        outs[1] = keel::move(txt);
    }
    if (nargout >= 3 && outs.size() >= 3) {
        Value rawOut = Value::cell(maxR, maxC, mr);
        const keel::f64 nan = keel::Limits<keel::f64>::quiet_nan();
        for (keel::usize c = 0; c < maxC; ++c)
            for (keel::usize r = 0; r < maxR; ++r) {
                const auto &cell = grid[r][c];
                keel::f64 d = 0.0;
                if (!cell.present || cell.text.empty())
                    rawOut.cellAt(c * maxR + r) = Value::scalar(nan, mr);
                else if (!cell.isStr && tryParseCellDouble(cell.text, d))
                    rawOut.cellAt(c * maxR + r) = Value::scalar(d, mr);
                else
                    rawOut.cellAt(c * maxR + r) = Value::fromString(cell.text, mr);
            }
        outs[2] = keel::move(rawOut);
    }
}


} // namespace

void registerTableIo(Engine &engine)
{
    engine.registerFunction("matlab.lang", "makeValidName", &makeValidName_reg);
    struct Entry {
        const char *name;
        void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
    };
    static constexpr Entry kTableIoFns[] = {
        {"detectImportOptions", &detectImportOptions_reg},
        {"setvaropts", &setvaropts_reg},
        {"readtable", &readtable_reg},
        {"writetable", &writetable_reg},
        {"xlsread", &xlsread_reg},
        {"makeValidName", &makeValidName_reg},
    };
    for (const auto &entry : kTableIoFns)
        engine.registerFunction(entry.name, entry.fn);
}

} // namespace numkit::runtime::containers_detail
