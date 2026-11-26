// builtin/register/web_reg.cpp
//
// HTTP/HTTPS web client builtins (weboptions, urlread, webread, websave)
// backed by keel::HttpClient.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/http.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/sys/fs_context.hpp>
#include <numkit/sys/vfs.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::builtin::detail {

Value parseJsonString(keel::StringRef raw, keel::Allocator *mr);

namespace {

static keel::String urlPercentEncode(keel::StringRef s)
{
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

static keel::String encodeQueryPairs(const Value &cell)
{
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

static void applyWebOptions(const Value &opts, keel::HttpRequest &req, keel::String &contentTypeOut)
{
    if (!opts.isStruct()) return;
    if (opts.hasField("Timeout") && opts.field("Timeout").isNumeric() && !opts.field("Timeout").isEmpty()) {
        const keel::f64 sec = opts.field("Timeout").elemAsDouble(0);
        if (keel::is_finite(sec) && sec > 0.0) {
            const keel::f64 msF = sec * 1000.0;
            const keel::f64 maxU32 = keel::as<keel::f64>(keel::Limits<keel::u32>::kMax);
            const keel::u32 ms = (msF >= maxU32) ? keel::Limits<keel::u32>::kMax : keel::as<keel::u32>(msF);
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

static void weboptions_reg(keel::ArrayRef<Value> args, keel::usize,
                           keel::MutArrayRef<Value> outs, CallContext &ctx)
{
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

static void urlread_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
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
            const keel::f64 sec = args[i + 1].elemAsDouble(0);
            if (keel::is_finite(sec) && sec > 0.0) {
                const keel::f64 msF = sec * 1000.0;
                const keel::f64 maxU32 = keel::as<keel::f64>(keel::Limits<keel::u32>::kMax);
                const keel::u32 ms = (msF >= maxU32) ? keel::Limits<keel::u32>::kMax : keel::as<keel::u32>(msF);
                req.connect_timeout_ms = ms;
                req.read_timeout_ms = ms;
            }
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

static void webread_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
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
        outs[0] = parseJsonString(resp.body_as_text(), mr);
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

static void websave_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
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

} // namespace

void register_web_io(Engine &engine)
{
    engine.registerFunction("weboptions", &weboptions_reg);
    engine.registerFunction("urlread",    &urlread_reg);
    engine.registerFunction("webread",    &webread_reg);
    engine.registerFunction("websave",    &websave_reg);
}

} // namespace numkit::builtin::detail
