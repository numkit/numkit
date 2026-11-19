// toolboxes/signal/src/language/strings/regex_reg.cpp
//
// CallContext register half of language/strings/regex.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <keel/core.h>
#include <keel/format.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/builtin/strfun.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>
#include <numkit/builtin/detail/strfun/regex_detail.hpp>
#include <numkit/value/error.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <utility>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

// The default positional output order of regexp/regexpi when no option
// string is given: [start, end, tokenExtents, match, tokens, names, split].
inline void regexpDispatch(keel::ArrayRef<Value> args, keel::usize nargout,
                           keel::MutArrayRef<Value> outs, bool ignoreCase, const char *fn,
                           CallContext &ctx)
{
    if (args.size() < 2)
        throw Error(keel::format("{}: requires at least 2 arguments (s, pat)", fn),
                     0, 0, fn, "", keel::format("numkit:{}:nargin", fn));
    auto *mr = ctx.engine->allocator();
    // Trailing option strings. 'once' is a modifier (match first occurrence,
    // return the scalarised form); the first non-'once' string selects the
    // output (as before). Additional output selectors are ignored, matching
    // the pre-existing single-option behaviour.
    keel::String opt(mr);
    bool once = false;
    for (keel::usize i = 2; i < args.size(); ++i) {
        if (!args[i].isChar() && !args[i].isString())
            throw Error(keel::format("{}: option must be a string", fn),
                         0, 0, fn, "", keel::format("numkit:{}:badOption", fn));
        keel::String o = args[i].toString();
        keel::String lo = o.clone();
        for (keel::usize j = 0; j < lo.size(); ++j)
            lo[j] = static_cast<char>(std::tolower(static_cast<unsigned char>(lo[j])));
        if (lo == "once") { once = true; continue; }
        if (opt.empty()) opt = std::move(o);
    }
    if (once) {
        // 'once' forces a single scalarised output (opt may be "" → 'start').
        outs[0] = regexpFindOnce(args[0], args[1], opt, ignoreCase, mr);
        return;
    }
    if (!opt.empty() || nargout <= 1) {
        outs[0] = regexpFind(args[0], args[1], opt, ignoreCase, mr);
        return;
    }
    static const char *order[] = {"start", "end", "tokenExtents",
                                  "match", "tokens", "names", "split"};
    const keel::usize k = std::min<keel::usize>(nargout, 7);
    for (keel::usize i = 0; i < k; ++i)
        outs[i] = regexpFind(args[0], args[1], order[i], ignoreCase, mr);
}

void regexp_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    regexpDispatch(args, nargout, outs, /*ignoreCase=*/false, "regexp", ctx);
}

void regexpi_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    regexpDispatch(args, nargout, outs, /*ignoreCase=*/true, "regexpi", ctx);
}

void regexprep_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 3, "regexprep");
    // Trailing option strings: recognise 'ignorecase' and 'once' (other
    // documented options — 'preservecase', 'lineanchors', … — are not yet
    // parsed and are left to their default behaviour).
    bool ignoreCase = false;
    bool once = false;
    for (keel::usize i = 3; i < args.size(); ++i) {
        if (args[i].isChar() || args[i].isString()) {
            keel::String o = args[i].toString();
            for (keel::usize j = 0; j < o.size(); ++j)
                o[j] = static_cast<char>(std::tolower(static_cast<unsigned char>(o[j])));
            if      (o == "ignorecase") ignoreCase = true;
            else if (o == "once")       once = true;
        }
    }
    outs[0] = regexprep(args[0], args[1], args[2], ignoreCase, once, ctx.engine->allocator());
}

void regexptranslate_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                         CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "regexptranslate");
    argcheck::checkNargin(args.size(), 2, 3, "regexptranslate");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("regexptranslate: op must be a char or string",
                     0, 0, "regexptranslate", "", "numkit:regexptranslate:badOp");
    if (!args[1].isChar() && !args[1].isString())
        throw Error("regexptranslate: str must be a char or string",
                     0, 0, "regexptranslate", "", "numkit:regexptranslate:badStr");
    outs[0] = regexptranslate(args[0].toString(), args[1].toString(), ctx.engine->allocator());
}

} // namespace detail

} // namespace numkit::builtin
