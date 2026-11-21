// src/bundle/src/register/builtin/iofun_reg.cpp

#include <keel/array.h>
#include <keel/core.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/builtin/iofun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
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
} // namespace numkit::builtin::detail
#include <cmath>
#include <cstdio>
#include <utility>

namespace numkit::builtin::detail {

static void jsonEscapeAppend(keel::StringRef s, keel::String &out) {
    out.push_back('\"');
    for (unsigned char c : s) {
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
            if (!std::isfinite(d)) {
                out.append("null");
                return;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", d);
            out.append(buf);
            return;
        }
        keel::usize R = v.dims().rows();
        keel::usize C = v.dims().cols();
        if (R == 1 || C == 1) {
            out.push_back('[');
            for (keel::usize i = 0; i < v.numel(); ++i) {
                if (i > 0) out.push_back(',');
                keel::f64 d = v.elemAsDouble(i);
                if (!std::isfinite(d)) out.append("null");
                else {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.15g", d);
                    out.append(buf);
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
                if (!std::isfinite(d)) out.append("null");
                else {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.15g", d);
                    out.append(buf);
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
        Value cell = Value::cell(elems.size(), 1, mr);
        for (keel::usize i = 0; i < elems.size(); ++i) cell.cellAt(i) = std::move(elems[i]);
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
            vals.push_back(std::move(val));
            skipWs();
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == '}') { ++pos; break; }
            break;
        }
        Value st = Value::structure(mr);
        for (keel::usize i = 0; i < fields.size(); ++i) st.setField(0, fields[i], std::move(vals[i]));
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

void register_iofun(Engine &engine) {
    engine.registerFunction("sprintf",    &::numkit::builtin::detail::sprintf_reg);
    engine.registerFunction("disp",       &::numkit::builtin::detail::disp_reg);
    engine.registerFunction("display",    &::numkit::builtin::detail::display_reg);
    engine.registerFunction("fprintf",    &::numkit::builtin::detail::fprintf_reg);
    engine.registerFunction("fscanf",     &::numkit::builtin::detail::fscanf_reg);
    engine.registerFunction("sscanf",     &::numkit::builtin::detail::sscanf_reg);
    engine.registerFunction("textscan",   &::numkit::builtin::detail::textscan_reg);
    engine.registerFunction("jsonencode", &::numkit::builtin::detail::jsonencode_reg);
    engine.registerFunction("jsondecode", &::numkit::builtin::detail::jsondecode_reg);
}

} // namespace numkit::builtin::detail
