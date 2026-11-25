// src/bundle/src/register/builtin/iofun_reg.cpp

#include <keel/array.h>
#include <keel/format.h>
#include <keel/http.h>
#include <keel/math.h>
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/builtin/iofun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/sys/fs_context.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>

// Each registered adapter in iofun/*_reg.cpp enforces argcheck::checkNargin.
namespace numkit::builtin::detail {
void disp_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void display_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void fprintf_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void fscanf_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void sprintf_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void sscanf_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void textscan_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void textread_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
} // namespace numkit::builtin::detail
#include <keel/number.h>

namespace numkit::builtin::detail {

static void jsonEscapeAppend(keel::StringRef s, keel::String &out) {
    out.push_back('\"');
    for (keel::u8 c : s) {
        if (c == '\"') out.append("\\\"");
        else if (c == '\\') out.append("\\\\");
        else if (c == '\n') out.append("\\n");
        else if (c == '\r') out.append("\\r");
        else if (c == '\t') out.append("\\t");
        else out.push_back(keel::as<char>(c));
    }
    out.push_back('\"');
}

static void jsonEncodeValAppend(const Value &v, keel::String &out) {
    if (v.isChar() || v.isString()) {
        jsonEscapeAppend(v.toString(), out);
        return;
    }
    if (v.isLogical() && v.numel() == 1) {
        out.append(v.toBool() ? "true" : "false");
        return;
    }
    if (v.isCell()) {
        out.push_back('[');
        for (keel::usize i = 0; i < v.numel(); ++i) {
            if (i > 0) out.push_back(',');
            jsonEncodeValAppend(v.cellAt(i), out);
        }
        out.push_back(']');
        return;
    }
    if (v.isStruct() && v.numel() == 1) {
        out.push_back('{');
        const auto &fields = v.fieldNamesInOrder();
        for (keel::usize i = 0; i < fields.size(); ++i) {
            if (i > 0) out.push_back(',');
            jsonEscapeAppend(fields[i], out);
            out.push_back(':');
            jsonEncodeValAppend(v.field(fields[i]), out);
        }
        out.push_back('}');
        return;
    }
    if (v.isNumeric() || v.isLogical()) {
        if (v.isEmpty()) {
            out.append("[]");
            return;
        }
        if (v.numel() == 1) {
            keel::f64 d = v.elemAsDouble(0);
            if (!keel::is_finite(d)) {
                out.append("null");
                return;
            }
            char buf[64];
            out.append(keel::format_f64_general_to(buf, d, 15));
            return;
        }
        keel::usize R = v.dims().rows();
        keel::usize C = v.dims().cols();
        if (R == 1 || C == 1) {
            out.push_back('[');
            for (keel::usize i = 0; i < v.numel(); ++i) {
                if (i > 0) out.push_back(',');
                keel::f64 d = v.elemAsDouble(i);
                if (!keel::is_finite(d)) out.append("null");
                else {
                    char buf[64];
                    out.append(keel::format_f64_general_to(buf, d, 15));
                }
            }
            out.push_back(']');
            return;
        }
        out.push_back('[');
        for (keel::usize r = 0; r < R; ++r) {
            if (r > 0) out.push_back(',');
            out.push_back('[');
            for (keel::usize c = 0; c < C; ++c) {
                if (c > 0) out.push_back(',');
                keel::f64 d = v.elemAsDouble(r + c * R);
                if (!keel::is_finite(d)) out.append("null");
                else {
                    char buf[64];
                    out.append(keel::format_f64_general_to(buf, d, 15));
                }
            }
            out.push_back(']');
        }
        out.push_back(']');
        return;
    }
    out.append("null");
}

static keel::String jsonEncodeVal(const Value &v, keel::Allocator *mr) {
    keel::String out(mr);
    jsonEncodeValAppend(v, out);
    return out;
}

struct JsonParser {
    keel::StringRef s;
    keel::usize pos = 0;
    keel::Allocator *mr;

    void skipWs() {
        while (pos < s.size() && keel::is_ascii_space(s[pos])) ++pos;
    }

    Value parseValue() {
        skipWs();
        if (pos >= s.size()) return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        char c = s[pos];
        if (c == '"') return Value::fromString(parseString(), mr);
        if (c == '[') return parseArray();
        if (c == '{') return parseObject();
        if (pos + 4 <= s.size() && s.substr(pos, 4) == "true") { pos += 4; return Value::logicalScalar(true, mr); }
        if (pos + 5 <= s.size() && s.substr(pos, 5) == "false") { pos += 5; return Value::logicalScalar(false, mr); }
        if (pos + 4 <= s.size() && s.substr(pos, 4) == "null") { pos += 4; return Value::matrix(0, 0, ValueType::DOUBLE, mr); }
        return parseNumber();
    }

    keel::String parseString() {
        if (pos < s.size() && s[pos] == '"') ++pos;
        keel::String out(mr);
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < s.size()) {
                ++pos;
                char e = s[pos++];
                if (e == 'n') out.push_back('\n');
                else if (e == 'r') out.push_back('\r');
                else if (e == 't') out.push_back('\t');
                else out.push_back(e);
            } else {
                out.push_back(s[pos++]);
            }
        }
        if (pos < s.size() && s[pos] == '"') ++pos;
        return out;
    }

    Value parseNumber() {
        keel::usize start = pos;
        while (pos < s.size() && (keel::is_ascii_digit(s[pos]) || s[pos] == '-' || s[pos] == '+' || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
        }
        keel::f64 d = keel::parse_f64(s.substr(start, pos - start)).value_or(0.0);
        return Value::scalar(d, mr);
    }

    Value parseArray() {
        ++pos; // '['
        skipWs();
        keel::Array<Value> elems(mr);
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        }
        while (pos < s.size()) {
            elems.push_back(parseValue());
            skipWs();
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == ']') { ++pos; break; }
            break;
        }
        bool allScalarNum = !elems.empty();
        for (const auto &e : elems) {
            if (!(e.isNumeric() && e.numel() == 1)) { allScalarNum = false; break; }
        }
        if (allScalarNum) {
            Value v = Value::matrix(elems.size(), 1, ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < elems.size(); ++i) v.doubleDataMut()[i] = elems[i].toScalar();
            return v;
        }
        bool allVecNumSameLen = !elems.empty() && elems[0].isNumeric() && elems[0].numel() >= 1 && (elems[0].dims().rows() == 1 || elems[0].dims().cols() == 1);
        keel::usize C = allVecNumSameLen ? elems[0].numel() : 0;
        for (const auto &e : elems) {
            if (!(e.isNumeric() && e.numel() == C && (e.dims().rows() == 1 || e.dims().cols() == 1))) {
                allVecNumSameLen = false;
                break;
            }
        }
        if (allVecNumSameLen && C > 0) {
            keel::usize R = elems.size();
            Value v = Value::matrix(R, C, ValueType::DOUBLE, mr);
            for (keel::usize r = 0; r < R; ++r)
                for (keel::usize c = 0; c < C; ++c)
                    v.doubleDataMut()[r + c * R] = elems[r].elemAsDouble(c);
            return v;
        }
        bool allSameStruct = !elems.empty() && elems[0].isStruct() && elems[0].numel() == 1;
        if (allSameStruct) {
            keel::Array<keel::String> f0 = elems[0].fieldNamesInOrder(mr);
            for (keel::usize i = 1; i < elems.size(); ++i) {
                if (!elems[i].isStruct() || elems[i].numel() != 1
                    || elems[i].structFields().size() != f0.size()) {
                    allSameStruct = false;
                    break;
                }
                for (keel::usize k = 0; k < f0.size(); ++k) {
                    if (!elems[i].hasField(f0[k])) {
                        allSameStruct = false;
                        break;
                    }
                }
                if (!allSameStruct) break;
            }
            if (allSameStruct) {
                Value sa = Value::structArray(elems.size(), 1, mr);
                for (keel::usize i = 0; i < elems.size(); ++i) {
                    for (keel::usize k = 0; k < f0.size(); ++k)
                        sa.setField(i, f0[k], elems[i].field(f0[k]));
                }
                return sa;
            }
        }
        Value cell = Value::cell(elems.size(), 1, mr);
        for (keel::usize i = 0; i < elems.size(); ++i) cell.cellAt(i) = keel::move(elems[i]);
        return cell;
    }

    Value parseObject() {
        ++pos; // '{'
        skipWs();
        keel::Array<keel::String> fields(mr);
        keel::Array<Value> vals(mr);
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return Value::structure(mr);
        }
        while (pos < s.size()) {
            skipWs();
            keel::String key = parseString();
            skipWs();
            if (pos < s.size() && s[pos] == ':') ++pos;
            Value val = parseValue();
            fields.push_back(keel::move(key));
            vals.push_back(keel::move(val));
            skipWs();
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == '}') { ++pos; break; }
            break;
        }
        Value st = Value::structure(mr);
        for (keel::usize i = 0; i < fields.size(); ++i) st.setField(0, fields[i], keel::move(vals[i]));
        return st;
    }
};

static void jsonencode_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 3, "jsonencode");
    auto *mr = ctx.engine->allocator();
    outs[0] = Value::fromString(jsonEncodeVal(args[0], mr), mr);
}

static void jsondecode_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 3, "jsondecode");
    keel::String raw = args[0].toString();
    JsonParser jp{raw.as_ref(), 0, ctx.engine->allocator()};
    outs[0] = jp.parseValue();
}

static keel::String urlPercentEncode(keel::StringRef s) {
    static constexpr const char kHex[] = "0123456789ABCDEF";
    keel::String out;
    out.reserve(s.size());
    for (keel::usize i = 0; i < s.size(); ++i) {
        const keel::u8 c = keel::as<keel::u8>(s[i]);
        const bool unreserved = keel::is_ascii_alnum(c)
                             || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(keel::as<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4u) & 0x0Fu]);
            out.push_back(kHex[c & 0x0Fu]);
        }
    }
    return out;
}

static keel::String encodeQueryPairs(const Value &cell) {
    keel::String q;
    const keel::usize n = cell.numel();
    for (keel::usize i = 0; i + 1 < n; i += 2) {
        if (!q.empty()) q.push_back('&');
        q.append(urlPercentEncode(cell.cellAt(i).toString().as_ref()).as_ref());
        q.push_back('=');
        q.append(urlPercentEncode(cell.cellAt(i + 1).toString().as_ref()).as_ref());
    }
    return q;
}

static void applyWebOptions(const Value &opts, keel::HttpRequest &req, keel::String &contentTypeOut) {
    if (!opts.isStruct()) return;
    if (opts.hasField("Timeout") && opts.field("Timeout").isNumeric() && !opts.field("Timeout").isEmpty()) {
        const keel::f64 sec = opts.field("Timeout").elemAsDouble(0);
        if (keel::is_finite(sec) && sec > 0.0) {
            const keel::u32 ms = keel::as<keel::u32>(sec * 1000.0);
            req.connect_timeout_ms = ms;
            req.read_timeout_ms = ms;
        }
    }
    if (opts.hasField("UserAgent") && (opts.field("UserAgent").isChar() || opts.field("UserAgent").isString())) {
        req.user_agent = opts.field("UserAgent").toString();
    }
    if (opts.hasField("ContentType") && (opts.field("ContentType").isChar() || opts.field("ContentType").isString())) {
        contentTypeOut = opts.field("ContentType").toString();
        contentTypeOut.to_ascii_lower_in_place();
    }
    if (opts.hasField("RequestMethod") && (opts.field("RequestMethod").isChar() || opts.field("RequestMethod").isString())) {
        keel::String m = opts.field("RequestMethod").toString();
        m.to_ascii_lower_in_place();
        if (m == "post") req.method = keel::HttpMethod::Post;
        else if (m == "put") req.method = keel::HttpMethod::Put;
        else if (m == "delete") req.method = keel::HttpMethod::Delete;
        else if (m == "patch") req.method = keel::HttpMethod::Patch;
        else req.method = keel::HttpMethod::Get;
    }
    if (opts.hasField("HeaderFields") && opts.field("HeaderFields").isCell()) {
        const Value &hf = opts.field("HeaderFields");
        const keel::usize rows = hf.dims().rows();
        const keel::usize cols = hf.dims().cols();
        if (cols == 2) {
            for (keel::usize r = 0; r < rows; ++r) {
                keel::HttpHeader hdr;
                hdr.name = hf.cellAt(r).toString();
                hdr.value = hf.cellAt(rows + r).toString();
                req.headers.push_back(keel::move(hdr));
            }
        }
    }
}

static void weboptions_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 0, 32, "weboptions");
    auto *mr = ctx.engine->allocator();
    Value opts = Value::structure(mr);
    opts.field("CharacterEncoding") = Value::fromString("auto", mr);
    opts.field("UserAgent") = Value::fromString("NumKit/1.0", mr);
    opts.field("Timeout") = Value::scalar(15.0, mr);
    opts.field("Username") = Value::fromString("", mr);
    opts.field("Password") = Value::fromString("", mr);
    opts.field("KeyName") = Value::fromString("", mr);
    opts.field("KeyValue") = Value::fromString("", mr);
    opts.field("ContentType") = Value::fromString("auto", mr);
    opts.field("MediaType") = Value::fromString("auto", mr);
    opts.field("RequestMethod") = Value::fromString("auto", mr);
    opts.field("ArrayFormat") = Value::fromString("csv", mr);
    opts.field("HeaderFields") = Value::cell(0, 0, mr);
    opts.field("CertificateFilename") = Value::fromString("default", mr);
    for (keel::usize i = 0; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString())
            throw Error("weboptions: parameter names must be strings",
                        0, 0, "weboptions", "", "numkit:weboptions:badParamName");
        opts.field(args[i].toString()) = args[i + 1];
    }
    outs[0] = keel::move(opts);
}

static void urlread_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 9, "urlread");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("urlread: URL must be a character vector or string",
                    0, 0, "urlread", "", "numkit:urlread:invalidUrl");
    auto *mr = ctx.engine->allocator();
    keel::HttpRequest req;
    req.url = args[0].toString();
    req.connect_timeout_ms = 10000;
    req.read_timeout_ms = 30000;

    keel::usize i = 1;
    if (i + 1 < args.size() && (args[i].isChar() || args[i].isString())) {
        keel::String m = args[i].toString();
        m.to_ascii_lower_in_place();
        if (m == "get" && args[i + 1].isCell()) {
            keel::String q = encodeQueryPairs(args[i + 1]);
            if (!q.empty()) {
                req.url.push_back(req.url.find('?') == keel::kInvalidIndex ? '?' : '&');
                req.url.append(q.as_ref());
            }
            i += 2;
        } else if (m == "post" && args[i + 1].isCell()) {
            req.method = keel::HttpMethod::Post;
            keel::String q = encodeQueryPairs(args[i + 1]);
            req.body.resize(q.size());
            if (!q.empty())
                keel::mem_copy(req.body.data(), q.data(), q.size());
            keel::HttpHeader ct;
            ct.name = keel::String::from("Content-Type");
            ct.value = keel::String::from("application/x-www-form-urlencoded");
            req.headers.push_back(keel::move(ct));
            i += 2;
        }
    }
    for (; i + 1 < args.size(); i += 2) {
        if (!args[i].isChar() && !args[i].isString()) continue;
        keel::String k = args[i].toString();
        k.to_ascii_lower_in_place();
        if (k == "timeout" && args[i + 1].isNumeric() && !args[i + 1].isEmpty()) {
            const keel::u32 ms = keel::as<keel::u32>(args[i + 1].elemAsDouble(0) * 1000.0);
            req.connect_timeout_ms = ms;
            req.read_timeout_ms = ms;
        } else if (k == "useragent" && (args[i + 1].isChar() || args[i + 1].isString())) {
            req.user_agent = args[i + 1].toString();
        }
    }

    keel::HttpClient client("NumKit/1.0");
    keel::HttpResponse resp = client.execute(req);
    if (!resp.succeeded()) {
        if (nargout >= 2 && outs.size() >= 2) {
            outs[0] = Value::fromString("", mr);
            outs[1] = Value::scalar(0.0, mr);
            return;
        }
        keel::String msg = resp.error_message.empty()
            ? keel::format("urlread: HTTP status {} for '{}'", resp.status_code, req.url)
            : keel::format("urlread: {} for '{}'", resp.error_message, req.url);
        throw Error(msg, 0, 0, "urlread", "", "numkit:urlread:requestFailed");
    }
    outs[0] = Value::fromString(resp.body_as_text(), mr);
    if (nargout >= 2 && outs.size() >= 2)
        outs[1] = Value::scalar(1.0, mr);
}

static void webread_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 32, "webread");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("webread: URL must be a character vector or string",
                    0, 0, "webread", "", "numkit:webread:invalidUrl");
    auto *mr = ctx.engine->allocator();
    keel::HttpRequest req;
    req.url = args[0].toString();
    req.connect_timeout_ms = 10000;
    req.read_timeout_ms = 30000;
    keel::String contentType = keel::String::from("auto");

    keel::usize end = args.size();
    if (end >= 2 && args[end - 1].isStruct()) {
        applyWebOptions(args[end - 1], req, contentType);
        --end;
    }
    keel::String q;
    for (keel::usize i = 1; i + 1 < end; i += 2) {
        if (!q.empty()) q.push_back('&');
        q.append(urlPercentEncode(args[i].toString().as_ref()).as_ref());
        q.push_back('=');
        q.append(urlPercentEncode(args[i + 1].toString().as_ref()).as_ref());
    }
    if (!q.empty()) {
        req.url.push_back(req.url.find('?') == keel::kInvalidIndex ? '?' : '&');
        req.url.append(q.as_ref());
    }

    keel::HttpClient client("NumKit/1.0");
    keel::HttpResponse resp = client.execute(req);
    if (!resp.succeeded()) {
        keel::String msg = resp.error_message.empty()
            ? keel::format("webread: HTTP status {} for '{}'", resp.status_code, req.url)
            : keel::format("webread: {} for '{}'", resp.error_message, req.url);
        throw Error(msg, 0, 0, "webread", "", "numkit:webread:requestFailed");
    }

    keel::String ctLower = resp.content_type.clone();
    ctLower.to_ascii_lower_in_place();
    const bool isJson = (contentType == "json")
                     || (contentType == "auto" && ctLower.find("json") != keel::kInvalidIndex);
    if (isJson) {
        JsonParser jp{resp.body_as_text(), 0, mr};
        outs[0] = jp.parseValue();
        return;
    }
    if (contentType == "binary") {
        Value arr = Value::matrix(resp.body.size(), 1, ValueType::UINT8, mr);
        keel::u8 *dst = arr.uint8DataMut();
        if (!resp.body.empty()) keel::mem_copy(dst, resp.body.data(), resp.body.size());
        outs[0] = keel::move(arr);
        return;
    }
    outs[0] = Value::fromString(resp.body_as_text(), mr);
}

static void websave_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 2, 32, "websave");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("websave: filename must be a character vector or string",
                    0, 0, "websave", "", "numkit:websave:invalidFilename");
    if (!args[1].isChar() && !args[1].isString())
        throw Error("websave: URL must be a character vector or string",
                    0, 0, "websave", "", "numkit:websave:invalidUrl");
    auto *mr = ctx.engine->allocator();
    keel::String filename = args[0].toString();
    keel::HttpRequest req;
    req.url = args[1].toString();
    req.connect_timeout_ms = 10000;
    req.read_timeout_ms = 30000;
    req.max_response_bytes = 64u * 1024u * 1024u;
    keel::String contentType = keel::String::from("auto");

    keel::usize end = args.size();
    if (end >= 3 && args[end - 1].isStruct()) {
        applyWebOptions(args[end - 1], req, contentType);
        --end;
    }
    keel::String q;
    for (keel::usize i = 2; i + 1 < end; i += 2) {
        if (!q.empty()) q.push_back('&');
        q.append(urlPercentEncode(args[i].toString().as_ref()).as_ref());
        q.push_back('=');
        q.append(urlPercentEncode(args[i + 1].toString().as_ref()).as_ref());
    }
    if (!q.empty()) {
        req.url.push_back(req.url.find('?') == keel::kInvalidIndex ? '?' : '&');
        req.url.append(q.as_ref());
    }

    keel::HttpClient client("NumKit/1.0");
    keel::HttpResponse resp = client.execute(req);
    if (!resp.succeeded()) {
        keel::String msg = resp.error_message.empty()
            ? keel::format("websave: HTTP status {} for '{}'", resp.status_code, req.url)
            : keel::format("websave: {} for '{}'", resp.error_message, req.url);
        throw Error(msg, 0, 0, "websave", "", "numkit:websave:requestFailed");
    }

    FsContext::ResolvedPath resolved = static_cast<Engine *>(ctx.engine)->fsContext().resolvePath(filename);
    if (!resolved.fs)
        throw Error("websave: cannot resolve output path",
                    0, 0, "websave", "", "numkit:websave:writeFailed");
    keel::StringRef payload(reinterpret_cast<const char *>(resp.body.data()), resp.body.size());
    try {
        resolved.fs->writeFile(resolved.path, payload);
    } catch (const std::exception &e) {
        throw Error(keel::format("websave: failed to write '{}': {}", filename, e.what()),
                    0, 0, "websave", "", "numkit:websave:writeFailed");
    }
    outs[0] = Value::fromString(resolved.path, mr);
}

void register_iofun(Engine &engine) {
    engine.registerFunction("sprintf",    &::numkit::builtin::detail::sprintf_reg);
    engine.registerFunction("disp",       &::numkit::builtin::detail::disp_reg);
    engine.registerFunction("display",    &::numkit::builtin::detail::display_reg);
    engine.registerFunction("fprintf",    &::numkit::builtin::detail::fprintf_reg);
    engine.registerFunction("fscanf",     &::numkit::builtin::detail::fscanf_reg);
    engine.registerFunction("sscanf",     &::numkit::builtin::detail::sscanf_reg);
    engine.registerFunction("textscan",   &::numkit::builtin::detail::textscan_reg);
    engine.registerFunction("textread",   &::numkit::builtin::detail::textread_reg);
    engine.registerFunction("jsonencode", &::numkit::builtin::detail::jsonencode_reg);
    engine.registerFunction("jsondecode", &::numkit::builtin::detail::jsondecode_reg);
    engine.registerFunction("weboptions", &::numkit::builtin::detail::weboptions_reg);
    engine.registerFunction("urlread",    &::numkit::builtin::detail::urlread_reg);
    engine.registerFunction("webread",    &::numkit::builtin::detail::webread_reg);
    engine.registerFunction("websave",    &::numkit::builtin::detail::websave_reg);
}

} // namespace numkit::builtin::detail
