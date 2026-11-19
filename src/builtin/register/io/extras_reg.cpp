// bundle/src/register/io/extras_reg.cpp
// CallContext adapters for io.text fileread/readlines/writelines/readmatrix/
// writematrix. Compute is Engine-free in toolboxes/io/src/text/extras.cpp
// (FsContext&); type lives in its own Engine-coupled TU (type.cpp).
#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/io/text/extras.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::io::detail {

void fileread_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "fileread");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("fileread: requires a filename string",
                     0, 0, "fileread", "", "numkit:fileread:nargin");
    const keel::String filename = args[0].toString();
    outs[0] = fileread(ctx.engine->fsContext(), filename, ctx.engine->allocator());
}

void readlines_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "readlines");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("readlines: requires a filename string",
                     0, 0, "readlines", "", "numkit:readlines:nargin");
    const keel::String filename = args[0].toString();
    outs[0] = readlines(ctx.engine->fsContext(), filename, ctx.engine->allocator());
}

void writelines_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 2, "writelines");
    if (!args[1].isChar() && !args[1].isString())
        throw Error("writelines: filename must be a string",
                     0, 0, "writelines", "", "numkit:writelines:badFilename");
    const keel::String filename = args[1].toString();
    writelines(ctx.engine->fsContext(), args[0], filename);
    outs[0] = Value();
}

void readmatrix_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "readmatrix");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("readmatrix: requires a filename string",
                     0, 0, "readmatrix", "", "numkit:readmatrix:nargin");
    const keel::String filename = args[0].toString();
    outs[0] = readmatrix(ctx.engine->fsContext(), filename, ctx.engine->allocator());
}

void writematrix_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, 2, "writematrix");
    if (!args[1].isChar() && !args[1].isString())
        throw Error("writematrix: filename must be a string",
                     0, 0, "writematrix", "", "numkit:writematrix:badFilename");
    const keel::String filename = args[1].toString();
    writematrix(ctx.engine->fsContext(), args[0], filename);
    outs[0] = Value();
}

} // namespace numkit::io::detail
