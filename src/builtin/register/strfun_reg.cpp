// src/bundle/src/register/builtin/strfun_reg.cpp

#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/builtin/strfun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>

#include <utility>

namespace numkit::builtin::detail {

void append_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void base2dec_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void bin2dec_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void blanks_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void char_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void compose_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void contains_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void convertCharsToStrings_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void convertStringsToChars_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void count_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void deblank_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void dec2base_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void dec2bin_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void dec2hex_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void endsWith_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void erase_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void eraseBetween_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void extract_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void extractAfter_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void extractBefore_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void extractBetween_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void hex2dec_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void hex2num_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void insertAfter_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void insertBefore_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void int2str_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isletter_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isspace_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isstringscalar_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void isstrprop_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void join_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void lower_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void mat2str_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void matches_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void newline_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void num2hex_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void num2str_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void pad_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void rat_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void rats_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void regexp_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void regexpi_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void regexprep_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void regexptranslate_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void replace_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void replaceBetween_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void reverse_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void split_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void splitlines_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void startsWith_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void str2double_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void str2num_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strcat_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strcmp_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strcmpi_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strfind_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void string_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strings_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strip_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strjoin_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strjust_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strlength_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strncmp_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strncmpi_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strrep_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strsplit_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strtok_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void strtrim_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void upper_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);
void validatestring_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext&);

} // namespace numkit::builtin::detail

namespace numkit::builtin::detail {

void register_strfun(Engine &engine) {
    engine.registerFunction("convertContainedStringsToChars",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "convertContainedStringsToChars");
            outs[0] = numkit::builtin::convertContainedStringsToChars(args[0], ctx.engine->allocator());
        });

    engine.registerFunction("num2str",    &::numkit::builtin::detail::num2str_reg);
    engine.registerFunction("int2str",    &::numkit::builtin::detail::int2str_reg);
    engine.registerFunction("validatestring", &::numkit::builtin::detail::validatestring_reg);
    engine.registerFunction("str2num",    &::numkit::builtin::detail::str2num_reg);
    engine.registerFunction("str2double", &::numkit::builtin::detail::str2double_reg);
    engine.registerFunction("string",     &::numkit::builtin::detail::string_reg);
    engine.registerFunction("char",       &::numkit::builtin::detail::char_reg);
    engine.registerFunction("strcmp",     &::numkit::builtin::detail::strcmp_reg);
    engine.registerFunction("strcmpi",    &::numkit::builtin::detail::strcmpi_reg);
    engine.registerFunction("strmatch",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 3, "strmatch");
            auto *mr = ctx.engine->allocator();
            keel::String pat = args[0].toString();
            bool exact = (args.size() >= 3 && (args[2].isChar() || args[2].isString()) && args[2].toString() == "exact");
            keel::Array<double> hits(mr);
            const Value &arr = args[1];
            if (arr.isCell() || arr.isString()) {
                for (keel::usize i = 0; i < arr.numel(); ++i) {
                    keel::String s = arr.isCell() ? arr.cellAt(i).toString() : arr.stringElem(i);
                    bool m = exact ? (s == pat) : s.as_ref().starts_with(pat);
                    if (m) hits.push_back(keel::as<double>(i + 1));
                }
            } else if (arr.isChar()) {
                keel::usize R = arr.dims().rows(), C = arr.dims().cols();
                for (keel::usize r = 0; r < R; ++r) {
                    keel::String s(mr);
                    s.reserve(C);
                    for (keel::usize c = 0; c < C; ++c)
                        s.push_back(keel::as<char>(arr.charData()[c * R + r]));
                    while (exact && !s.empty() && s.back() == ' ') s.pop_back();
                    bool m = exact ? (s == pat) : s.as_ref().starts_with(pat);
                    if (m) hits.push_back(keel::as<double>(r + 1));
                }
            }
            if (hits.empty()) {
                outs[0] = Value::matrix(0, 0, ValueType::DOUBLE, mr);
            } else {
                auto out = Value::matrix(hits.size(), 1, ValueType::DOUBLE, mr);
                for (keel::usize i = 0; i < hits.size(); ++i) out.doubleDataMut()[i] = hits[i];
                outs[0] = std::move(out);
            }
        });
    engine.registerFunction("upper",      &::numkit::builtin::detail::upper_reg);
    engine.registerFunction("lower",      &::numkit::builtin::detail::lower_reg);
    engine.registerFunction("strtrim",    &::numkit::builtin::detail::strtrim_reg);
    engine.registerFunction("strsplit",   &::numkit::builtin::detail::strsplit_reg);
    engine.registerFunction("strcat",     &::numkit::builtin::detail::strcat_reg);
    engine.registerFunction("strlength",  &::numkit::builtin::detail::strlength_reg);
    engine.registerFunction("strrep",     &::numkit::builtin::detail::strrep_reg);
    engine.registerFunction("contains",   &::numkit::builtin::detail::contains_reg);
    engine.registerFunction("startsWith", &::numkit::builtin::detail::startsWith_reg);
    engine.registerFunction("endsWith",   &::numkit::builtin::detail::endsWith_reg);
    engine.registerFunction("extractafter",   &::numkit::builtin::detail::extractAfter_reg);
    engine.registerFunction("extractbefore",  &::numkit::builtin::detail::extractBefore_reg);
    engine.registerFunction("extractbetween", &::numkit::builtin::detail::extractBetween_reg);
    engine.registerFunction("erasebetween",   &::numkit::builtin::detail::eraseBetween_reg);
    engine.registerFunction("insertafter",    &::numkit::builtin::detail::insertAfter_reg);
    engine.registerFunction("insertbefore",   &::numkit::builtin::detail::insertBefore_reg);
    engine.registerFunction("replacebetween", &::numkit::builtin::detail::replaceBetween_reg);
    engine.registerFunction("strncmp",    &::numkit::builtin::detail::strncmp_reg);
    engine.registerFunction("strncmpi",   &::numkit::builtin::detail::strncmpi_reg);
    engine.registerFunction("strfind",    &::numkit::builtin::detail::strfind_reg);
    engine.registerFunction("blanks",     &::numkit::builtin::detail::blanks_reg);
    engine.registerFunction("newline",    &::numkit::builtin::detail::newline_reg);
    engine.registerFunction("strings",    &::numkit::builtin::detail::strings_reg);
    engine.registerFunction("compose",    &::numkit::builtin::detail::compose_reg);
    engine.registerFunction("strjust",    &::numkit::builtin::detail::strjust_reg);
    engine.registerFunction("extract",    &::numkit::builtin::detail::extract_reg);
    engine.registerFunction("split",      &::numkit::builtin::detail::split_reg);
    engine.registerFunction("join",       &::numkit::builtin::detail::join_reg);
    engine.registerFunction("deblank",    &::numkit::builtin::detail::deblank_reg);
    engine.registerFunction("mat2str",    &::numkit::builtin::detail::mat2str_reg);
    engine.registerFunction("strjoin",    &::numkit::builtin::detail::strjoin_reg);
    engine.registerFunction("strtok",     &::numkit::builtin::detail::strtok_reg);
    engine.registerFunction("append",     &::numkit::builtin::detail::append_reg);
    engine.registerFunction("count",      &::numkit::builtin::detail::count_reg);
    engine.registerFunction("erase",      &::numkit::builtin::detail::erase_reg);
    engine.registerFunction("replace",    &::numkit::builtin::detail::replace_reg);
    engine.registerFunction("reverse",    &::numkit::builtin::detail::reverse_reg);
    engine.registerFunction("splitlines", &::numkit::builtin::detail::splitlines_reg);
    engine.registerFunction("pad",        &::numkit::builtin::detail::pad_reg);
    engine.registerFunction("strip",      &::numkit::builtin::detail::strip_reg);
    engine.registerFunction("matches",    &::numkit::builtin::detail::matches_reg);
    engine.registerFunction("convertCharsToStrings",
                                          &::numkit::builtin::detail::convertCharsToStrings_reg);
    engine.registerFunction("convertStringsToChars",
                                          &::numkit::builtin::detail::convertStringsToChars_reg);
    engine.registerFunction("isstringscalar",
                                          &::numkit::builtin::detail::isstringscalar_reg);
    engine.registerFunction("isStringScalar",
                                          &::numkit::builtin::detail::isstringscalar_reg);
    engine.registerFunction("isstrprop",  &::numkit::builtin::detail::isstrprop_reg);
    engine.registerFunction("isletter",   &::numkit::builtin::detail::isletter_reg);
    engine.registerFunction("isspace",    &::numkit::builtin::detail::isspace_reg);
    engine.registerFunction("extractAfter",   &::numkit::builtin::detail::extractAfter_reg);
    engine.registerFunction("extractBefore",  &::numkit::builtin::detail::extractBefore_reg);
    engine.registerFunction("extractBetween", &::numkit::builtin::detail::extractBetween_reg);
    engine.registerFunction("insertAfter",    &::numkit::builtin::detail::insertAfter_reg);
    engine.registerFunction("insertBefore",   &::numkit::builtin::detail::insertBefore_reg);
    engine.registerFunction("eraseBetween",   &::numkit::builtin::detail::eraseBetween_reg);
    engine.registerFunction("replaceBetween", &::numkit::builtin::detail::replaceBetween_reg);
    engine.registerFunction("dec2bin",    &::numkit::builtin::detail::dec2bin_reg);
    engine.registerFunction("dec2hex",    &::numkit::builtin::detail::dec2hex_reg);
    engine.registerFunction("dec2base",   &::numkit::builtin::detail::dec2base_reg);
    engine.registerFunction("base2dec",   &::numkit::builtin::detail::base2dec_reg);
    engine.registerFunction("bin2dec",    &::numkit::builtin::detail::bin2dec_reg);
    engine.registerFunction("hex2dec",    &::numkit::builtin::detail::hex2dec_reg);
    engine.registerFunction("hex2num",    &::numkit::builtin::detail::hex2num_reg);
    engine.registerFunction("num2hex",    &::numkit::builtin::detail::num2hex_reg);
    engine.registerFunction("rat",        &::numkit::builtin::detail::rat_reg);
    engine.registerFunction("rats",       &::numkit::builtin::detail::rats_reg);
    engine.registerFunction("regexp",     &::numkit::builtin::detail::regexp_reg);
    engine.registerFunction("regexpi",    &::numkit::builtin::detail::regexpi_reg);
    engine.registerFunction("regexprep",  &::numkit::builtin::detail::regexprep_reg);
    engine.registerFunction("regexptranslate", &::numkit::builtin::detail::regexptranslate_reg);
    engine.registerFunction("setstr",     &::numkit::builtin::detail::char_reg);
    engine.registerFunction("strvcat",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            keel::Array<Value> nonEmpty(ctx.engine->allocator());
            for (const auto &a : args) {
                if (!a.isEmpty())
                    nonEmpty.push_back(a);
            }
            if (nonEmpty.empty()) {
                outs[0] = Value::matrix(0, 0, ValueType::CHAR, ctx.engine->allocator());
                return;
            }
            char_reg(keel::ArrayRef<Value>(nonEmpty.data(), nonEmpty.size()), nargout, outs, ctx);
        });
    engine.registerFunction("findstr",
        [](keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, 2, "findstr");
            if (args[0].numel() < args[1].numel()) {
                Value swapped[2] = {args[1], args[0]};
                strfind_reg(keel::ArrayRef<Value>(swapped, 2), nargout, outs, ctx);
            } else {
                strfind_reg(args, nargout, outs, ctx);
            }
        });
}

} // namespace numkit::builtin::detail
