// bundle/src/register/io/type_reg.cpp
//
// Engine-registration adapter for `type(filename)`. The core-free compute body
// numkit::io::type(FsContext&, sink, filename) lives in toolboxes/io; this glue
// unwraps the CallContext and binds the output sink to engine.outputText.

#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/io/text/extras.hpp>

#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/value.hpp>

namespace numkit::io::detail {

void type_reg(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "type");
    if (!args[0].isChar() && !args[0].isString())
        throw Error("type: requires a filename string",
                     0, 0, "type", "", "numkit:type:nargin");
    auto sink = [&ctx](keel::StringRef s) { coreEngine(ctx).outputText(s); };
    const keel::String filename = args[0].toString();
    type(coreEngine(ctx).fsContext(), sink, filename);
    outs[0] = Value();
}

} // namespace numkit::io::detail
