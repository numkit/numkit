// src/bundle/src/register/builtin/iofun_reg.cpp

#include <numkit/builtin/iofun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>
#include <numkit/value/span.hpp>

// Each registered adapter in iofun/*_reg.cpp enforces argcheck::checkNargin.
namespace numkit::builtin::detail {
void disp_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void display_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void fprintf_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void fscanf_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void sprintf_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void sscanf_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
void textscan_reg(Span<const Value>, size_t, Span<Value>, CallContext&);
} // namespace numkit::builtin::detail

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace numkit::builtin::detail {

static std::string jsonEscape(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '\"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back((char)c);
    }
    out += "\"";
    return out;
}

static std::string jsonEncodeVal(const Value &v) {
    if (v.isChar() || v.isString()) {
        return jsonEscape(v.toString());
    }
    if (v.isLogical() && v.numel() == 1) {
        return v.toBool() ? "true" : "false";
    }
    if (v.isCell()) {
        std::string out = "[";
        for (size_t i = 0; i < v.numel(); ++i) {
            if (i > 0) out += ",";
            out += jsonEncodeVal(v.cellAt(i));
        }
        out += "]";
        return out;
    }
    if (v.isStruct() && v.numel() == 1) {
        std::string out = "{";
        const auto &fields = v.fieldNamesInOrder();
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) out += ",";
            out += jsonEscape(fields[i]) + ":" + jsonEncodeVal(v.field(fields[i]));
        }
        out += "}";
        return out;
    }
    if (v.isNumeric() || v.isLogical()) {
        if (v.isEmpty()) return "[]";
        if (v.numel() == 1) {
            double d = v.elemAsDouble(0);
            if (!std::isfinite(d)) return "null";
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", d);
            return buf;
        }
        size_t R = v.dims().rows();
        size_t C = v.dims().cols();
        if (R == 1 || C == 1) {
            std::string out = "[";
            for (size_t i = 0; i < v.numel(); ++i) {
                if (i > 0) out += ",";
                double d = v.elemAsDouble(i);
                if (!std::isfinite(d)) out += "null";
                else {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.15g", d);
                    out += buf;
                }
            }
            out += "]";
            return out;
        }
        std::string out = "[";
        for (size_t r = 0; r < R; ++r) {
            if (r > 0) out += ",";
            out += "[";
            for (size_t c = 0; c < C; ++c) {
                if (c > 0) out += ",";
                double d = v.elemAsDouble(r + c * R);
                if (!std::isfinite(d)) out += "null";
                else {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.15g", d);
                    out += buf;
                }
            }
            out += "]";
        }
        out += "]";
        return out;
    }
    return "null";
}

struct JsonParser {
    const std::string &s;
    size_t pos = 0;
    std::pmr::memory_resource *mr;

    void skipWs() {
        while (pos < s.size() && std::isspace((unsigned char)s[pos])) ++pos;
    }

    Value parseValue() {
        skipWs();
        if (pos >= s.size()) return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        char c = s[pos];
        if (c == '"') return Value::fromString(parseString(), mr);
        if (c == '[') return parseArray();
        if (c == '{') return parseObject();
        if (s.compare(pos, 4, "true") == 0) { pos += 4; return Value::logicalScalar(true, mr); }
        if (s.compare(pos, 5, "false") == 0) { pos += 5; return Value::logicalScalar(false, mr); }
        if (s.compare(pos, 4, "null") == 0) { pos += 4; return Value::matrix(0, 0, ValueType::DOUBLE, mr); }
        return parseNumber();
    }

    std::string parseString() {
        if (pos < s.size() && s[pos] == '"') ++pos;
        std::string out;
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
        size_t start = pos;
        while (pos < s.size() && (std::isdigit((unsigned char)s[pos]) || s[pos] == '-' || s[pos] == '+' || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E')) {
            ++pos;
        }
        double d = 0.0;
        try { d = std::stod(s.substr(start, pos - start)); } catch (...) {}
        return Value::scalar(d, mr);
    }

    Value parseArray() {
        ++pos; // '['
        skipWs();
        std::vector<Value> elems;
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
            for (size_t i = 0; i < elems.size(); ++i) v.doubleDataMut()[i] = elems[i].toScalar();
            return v;
        }
        bool allVecNumSameLen = !elems.empty() && elems[0].isNumeric() && elems[0].numel() >= 1 && (elems[0].dims().rows() == 1 || elems[0].dims().cols() == 1);
        size_t C = allVecNumSameLen ? elems[0].numel() : 0;
        for (const auto &e : elems) {
            if (!(e.isNumeric() && e.numel() == C && (e.dims().rows() == 1 || e.dims().cols() == 1))) {
                allVecNumSameLen = false;
                break;
            }
        }
        if (allVecNumSameLen && C > 0) {
            size_t R = elems.size();
            Value v = Value::matrix(R, C, ValueType::DOUBLE, mr);
            for (size_t r = 0; r < R; ++r)
                for (size_t c = 0; c < C; ++c)
                    v.doubleDataMut()[r + c * R] = elems[r].elemAsDouble(c);
            return v;
        }
        Value cell = Value::cell(elems.size(), 1, mr);
        for (size_t i = 0; i < elems.size(); ++i) cell.cellAt(i) = std::move(elems[i]);
        return cell;
    }

    Value parseObject() {
        ++pos; // '{'
        skipWs();
        std::vector<std::string> fields;
        std::vector<Value> vals;
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return Value::structure(mr);
        }
        while (pos < s.size()) {
            skipWs();
            std::string key = parseString();
            skipWs();
            if (pos < s.size() && s[pos] == ':') ++pos;
            Value val = parseValue();
            fields.push_back(key);
            vals.push_back(std::move(val));
            skipWs();
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == '}') { ++pos; break; }
            break;
        }
        Value st = Value::structure(mr);
        for (size_t i = 0; i < fields.size(); ++i) st.setField(0, fields[i], std::move(vals[i]));
        return st;
    }
};

static void jsonencode_reg(Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 3, "jsonencode");
    outs[0] = Value::fromString(jsonEncodeVal(args[0]), ctx.engine->resource());
}

static void jsondecode_reg(Span<const Value> args, size_t, Span<Value> outs, CallContext &ctx) {
    argcheck::checkNargin(args.size(), 1, 3, "jsondecode");
    std::string s = args[0].toString();
    JsonParser jp{s, 0, ctx.engine->resource()};
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
