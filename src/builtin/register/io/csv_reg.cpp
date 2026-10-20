// bundle/src/register/io/csv_reg.cpp
// CallContext adapters for io.text csvread/csvwrite. Compute is Engine-free
// in toolboxes/io/src/text/csv.cpp (FsContext& + mr); these bridge via
// engine.fsContext()/resource(). IoLibrary::install registers them by name.
#include <numkit/io/text/csv.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/value.hpp>

namespace numkit::io::detail {

void csvread_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    (void)nargout;
    argcheck::checkNargin(args.size(), 1, 4, "csvread");
    outs[0] = csvread(ctx.engine->fsContext(), args, ctx.engine->resource());
}

void csvwrite_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    (void)nargout;
    (void)outs;
    argcheck::checkNargin(args.size(), 2, 4, "csvwrite");
    csvwrite(ctx.engine->fsContext(), args);
}

void dlmwrite_reg(Span<const Value> args, size_t nargout, Span<Value> outs, CallContext &ctx)
{
    (void)nargout;
    (void)outs;
    argcheck::checkNargin(args.size(), 2, 10, "dlmwrite");
    csvwrite(ctx.engine->fsContext(), Span<const Value>(args.data(), 2));
}


} // namespace numkit::io::detail
