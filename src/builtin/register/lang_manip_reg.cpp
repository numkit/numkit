// toolboxes/signal/src/language/arrays/manip_reg.cpp
//
// CallContext register half of language/arrays/manip.cpp (Phase 2b compute/register split).
// Engine-coupled glue: marshals CallContext args/outs into the engine-free
// compute API declared in the headers below. See project_layering_refactor.
#include <numkit/core/engine.hpp>
#include <numkit/builtin/elmat.hpp>
#include <numkit/value/argcheck.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/shape_ops.hpp>
#include <numkit/value/value.hpp>
#include <numkit/ops/helpers.hpp>
#include <numkit/builtin/detail/elmat/manip_detail.hpp>
#include <numkit/value/error.hpp>
#include <numkit/value/scratch.hpp>
#include <numkit/value/span.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace numkit::builtin {
using namespace numkit::builtin;  // C4c localized (umbrella removed)

namespace detail {

void repmat_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "repmat");
    auto *mr = ctx.engine->resource();

    // Forms:
    //   repmat(A, n)            → m=n=arg
    //   repmat(A, [m n])        → vector
    //   repmat(A, [m n p ...])  → vector (any length ≥ 1)
    //   repmat(A, m, n)         → two scalars
    //   repmat(A, m, n, p, ...) → ≥ 2 scalars
    ScratchArena scratch(mr);
    auto tiles = ScratchVec<size_t>(&scratch);
    if (args.size() == 2) {
        const Value &v = args[1];
        const size_t k = v.numel();
        if (k == 0) {
            throw Error("repmat: tile vector must not be empty",
                         0, 0, "repmat", "", "numkit:repmat:badTileVec");
        }
        auto tileOf = [&](double d) {
            argcheck::expectedNonNegative(
                argcheck::expectedInteger(d, 1, "tile", "repmat"), 1, "tile", "repmat");
            return static_cast<size_t>(d);
        };
        if (k == 1) {
            const size_t s = tileOf(v.toScalar());
            tiles.assign({s, s});
        } else {
            argcheck::arrayDimReal(v, "repmat");
            tiles.reserve(k);
            for (size_t i = 0; i < k; ++i)
                tiles.push_back(tileOf(v.doubleData()[i]));
        }
    } else {
        tiles.reserve(args.size() - 1);
        for (size_t i = 1; i < args.size(); ++i) {
            const double d = argcheck::expectedReal(args[i], int(i) + 1, "tile", "repmat");
            argcheck::expectedInteger(d, int(i) + 1, "tile", "repmat");
            argcheck::expectedNonNegative(d, int(i) + 1, "tile", "repmat");
            tiles.push_back(static_cast<size_t>(d));
        }
    }

    // Fast path: rank ≤ 3 + tile vector ≤ 3 + DOUBLE → existing 2D/3D
    // kernel. Anything else (higher rank, longer tile vector, or non-
    // DOUBLE type) goes through repmatND.
    const auto &inDims = args[0].dims();
    const int outNdim = std::max(inDims.ndim(), static_cast<int>(tiles.size()));
    if (outNdim <= 3 && tiles.size() <= 3 && args[0].type() == ValueType::DOUBLE) {
        const size_t m = tiles[0];
        const size_t n = tiles.size() >= 2 ? tiles[1] : 1;
        const size_t p = tiles.size() >= 3 ? tiles[2] : 1;
        outs[0] = numkit::narrowComplex(repmat(args[0], m, n, p, mr), ctx.engine->resource());
    } else {
        outs[0] = numkit::narrowComplex(repmatND(args[0], Span<const size_t>(tiles.data(), tiles.size()), mr), ctx.engine->resource());
    }
}

#define NUMKIT_FLIP_REG(name)                                                      \
    void name##_reg(Span<const Value> args, size_t /*nargout*/,               \
                    Span<Value> outs, CallContext &ctx)                       \
    {                                                                          \
        argcheck::checkNargin(args.size(), 1, 1, #name); \
        outs[0] = numkit::narrowComplex(name(args[0], ctx.engine->resource()), ctx.engine->resource());                      \
    }

NUMKIT_FLIP_REG(fliplr)
NUMKIT_FLIP_REG(flipud)

#undef NUMKIT_FLIP_REG

void rot90_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
               CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "rot90");
    int k = (args.size() >= 2 && !args[1].isEmpty())
                ? argcheck::expectedInteger(
                      argcheck::expectedReal(args[1], 2, "k", "rot90"), 2, "k", "rot90")
                : 1;
    outs[0] = numkit::narrowComplex(rot90(args[0], k, ctx.engine->resource()), ctx.engine->resource());
}

void circshift_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                   CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "circshift");
    argcheck::checkNargin(args.size(), 2, 3, "circshift");
    const Value &k = args[1];
    auto *mr = ctx.engine->resource();
    const size_t nk = k.numel();
    if (nk == 0)
        throw Error("circshift: shift vector must not be empty",
                     0, 0, "circshift", "", "numkit:circshift:badShift");

    if (nk == 1) {
        const int64_t kk = static_cast<int64_t>(k.toScalar());
        // circshift(X, K, dim): shift by K ONLY along dimension `dim`. The
        // previous code ignored args[2] and always shifted dim 1.
        if (args.size() >= 3 && !args[2].isEmpty()) {
            const int dim = argcheck::dimArg(args[2], "circshift");
            if (dim < 1)
                throw Error("circshift: dim must be a positive integer",
                             0, 0, "circshift", "", "numkit:circshift:badDim");
            ScratchArena scratch(mr);
            auto shifts = ScratchVec<int64_t>(static_cast<size_t>(dim), &scratch);
            for (int i = 0; i < dim; ++i) shifts[i] = 0;
            shifts[dim - 1] = kk;
            outs[0] = numkit::narrowComplex(circshiftND(args[0],
                                  Span<const int64_t>(shifts.data(), dim), mr), ctx.engine->resource());
            return;
        }
        outs[0] = numkit::narrowComplex(circshift(args[0], kk, mr), ctx.engine->resource());
        return;
    }
    if (nk == 2 && args[0].dims().ndim() <= 3) {
        outs[0] = numkit::narrowComplex(circshift(args[0], static_cast<int64_t>(k.doubleData()[0]), static_cast<int64_t>(k.doubleData()[1]), mr), ctx.engine->resource());
        return;
    }
    // ND path: shift vector ≥ 3 entries OR input rank ≥ 4.
    ScratchArena scratch(mr);
    auto shifts = ScratchVec<int64_t>(nk, &scratch);
    for (size_t i = 0; i < nk; ++i)
        shifts[i] = static_cast<int64_t>(k.doubleData()[i]);
    outs[0] = numkit::narrowComplex(circshiftND(args[0], Span<const int64_t>(shifts.data(), nk), mr), ctx.engine->resource());
}

#define NUMKIT_TRI_REG(name)                                                       \
    void name##_reg(Span<const Value> args, size_t /*nargout*/,               \
                    Span<Value> outs, CallContext &ctx)                       \
    {                                                                          \
        argcheck::checkNargin(args.size(), 1, 2, #name);                      \
        int k = (args.size() >= 2 && !args[1].isEmpty())                       \
                    ? argcheck::expectedInteger(                              \
                          argcheck::expectedReal(args[1], 2, "k", #name),      \
                          2, "k", #name)                                       \
                    : 0;                                                        \
        outs[0] = numkit::narrowComplex(name(args[0], k, ctx.engine->resource()), ctx.engine->resource());                   \
    }

NUMKIT_TRI_REG(tril)
NUMKIT_TRI_REG(triu)

#undef NUMKIT_TRI_REG

void flip_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
              CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "flip");
    int dim = (args.size() >= 2 && !args[1].isEmpty())
                  ? argcheck::dimArg(args[1], "flip")
                  : 0;
    outs[0] = numkit::narrowComplex(flip(args[0], dim, ctx.engine->resource()), ctx.engine->resource());
}

void repelem_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "repelem");
    auto *mr = ctx.engine->resource();
    if (args.size() == 2) {
        // counts may be a scalar or a per-element vector — the Value
        // overload dispatches internally.
        outs[0] = numkit::narrowComplex(repelem(args[0], args[1], mr), ctx.engine->resource());
        return;
    }
    // r / c may each be a scalar or a per-row / per-column vector.
    outs[0] = numkit::narrowComplex(repelem(args[0], args[1], args[2], mr), ctx.engine->resource());
}

// sub2ind(siz, i1, i2, ...) → linear index. Column-major, 1-based.
void sub2ind_reg(Span<const Value> args, size_t /*nargout*/, Span<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "sub2ind");
    outs[0] = sub2ind(args[0], args.subspan(1), ctx.engine->resource());
}

// paddata / trimdata / resize adapters.
struct ResizeOpts {
    int dim = 0;
    std::string side = "trailing";
    double fillValue = 0.0;
};

ResizeOpts parseResizeOpts(Span<const Value> args, bool allowFill, const char *fn)
{
    ResizeOpts opts;
    if (args.size() <= 2) return opts;
    // Positional form: fn(X, len, dim[, side]) when arg 3 is numeric
    if (!args[2].isChar() && !args[2].isString()) {
        argcheck::checkNargin(args.size(), 3, 4, fn);
        opts.dim = argcheck::dimArg(args[2], fn);
        if (args.size() >= 4) {
            static const char *const kSides[] = {"trailing", "leading", "both", "right", "left"};
            const size_t sideIdx = argcheck::expectedChoice(
                args[3], {"trailing", "leading", "both", "right", "left"},
                4, "Side", fn);
            opts.side = kSides[sideIdx];
        }
        return opts;
    }
    if (((args.size() - 2) % 2) != 0) {
        throw Error(std::string(fn) + ": Name-Value arguments must occur in pairs",
                    0, 0, fn, "", std::string("numkit:") + fn + ":unpairedNameValue");
    }
    for (size_t i = 2; i < args.size(); i += 2) {
        const size_t keyIdx = allowFill
            ? argcheck::expectedChoice(args[i], {"dimension", "side", "fillvalue"},
                                       static_cast<int>(i + 1), "Name", fn)
            : argcheck::expectedChoice(args[i], {"dimension", "side"},
                                       static_cast<int>(i + 1), "Name", fn);
        const Value &val = args[i + 1];
        if (keyIdx == 0) {
            if (val.isChar() || val.isString()) {
                argcheck::expectedChoice(val, {"auto"},
                                         static_cast<int>(i + 2), "Dimension", fn);
                opts.dim = 0;
            } else {
                const double dv = argcheck::expectedReal(val, static_cast<int>(i + 2), "Dimension", fn);
                opts.dim = argcheck::expectedInteger(dv, static_cast<int>(i + 2), "Dimension", fn);
                argcheck::expectedPositive(dv, static_cast<int>(i + 2), "Dimension", fn);
            }
        } else if (keyIdx == 1) {
            static const char *const kSides[] = {"trailing", "leading", "both", "right", "left"};
            const size_t sideIdx = argcheck::expectedChoice(
                val, {"trailing", "leading", "both", "right", "left"},
                static_cast<int>(i + 2), "Side", fn);
            opts.side = kSides[sideIdx];
        } else if (keyIdx == 2) {
            opts.fillValue = argcheck::expectedReal(val, static_cast<int>(i + 2), "FillValue", fn);
        }
    }
    return opts;
}

void paddata_reg(Span<const Value> args, size_t /*nargout*/,
                 Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "paddata");
    const size_t n = argcheck::arrayDim(args[1], "paddata");
    if (args.size() == 2) {
        outs[0] = numkit::narrowComplex(paddata(args[0], n, ctx.engine->resource()), ctx.engine->resource());
        return;
    }
    const auto opts = parseResizeOpts(args, true, "paddata");
    outs[0] = numkit::narrowComplex(
        paddata(args[0], n, opts.dim, opts.side, opts.fillValue, ctx.engine->resource()),
        ctx.engine->resource());
}

void trimdata_reg(Span<const Value> args, size_t /*nargout*/,
                  Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "trimdata");
    const size_t n = argcheck::arrayDim(args[1], "trimdata");
    if (args.size() == 2) {
        outs[0] = numkit::narrowComplex(trimdata(args[0], n, ctx.engine->resource()), ctx.engine->resource());
        return;
    }
    const auto opts = parseResizeOpts(args, false, "trimdata");
    outs[0] = numkit::narrowComplex(
        trimdata(args[0], n, opts.dim, opts.side, ctx.engine->resource()),
        ctx.engine->resource());
}

void resize_reg(Span<const Value> args, size_t /*nargout*/,
                Span<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "resize");
    const size_t n = argcheck::arrayDim(args[1], "resize");
    if (args.size() == 2) {
        outs[0] = numkit::narrowComplex(resize(args[0], n, ctx.engine->resource()), ctx.engine->resource());
        return;
    }
    const auto opts = parseResizeOpts(args, true, "resize");
    outs[0] = numkit::narrowComplex(
        resize(args[0], n, opts.dim, opts.side, opts.fillValue, ctx.engine->resource()),
        ctx.engine->resource());
}

// ind2sub(siz, ind) → multiple outputs (one per dim of siz). When the
// caller requests fewer outputs than siz has dims, the last output
// absorbs trailing dims (column-major linear index of the remainder).
void ind2sub_reg(Span<const Value> args, size_t nargout, Span<Value> outs,
                 CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "ind2sub");
    argcheck::checkNargin(args.size(), 2, 2, "ind2sub");
    std::vector<Value> rs = ind2sub(args[0], args[1],
                                    std::max<size_t>(nargout, 1),
                                    ctx.engine->resource());
    for (size_t i = 0; i < rs.size() && i < outs.size(); ++i)
        outs[i] = std::move(rs[i]);
}

} // namespace detail

} // namespace numkit::builtin
