/// @file figure_render.hpp
/// @ingroup group_graphics
///
/// Software figure renderer declarations (SVG / EPS / PDF / RGB raster).
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/function.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/figure_manager.hpp>

#include <cstddef>
#include <cstdint>

namespace numkit {
namespace figrender {

const keel::u8 *namedPalette(keel::StringRef name, int &n);

void parseNestedDoubles(keel::StringRef j, keel::Array<keel::f64> &out,
                        keel::usize &rows, keel::usize &cols);

keel::Array<keel::f64> niceTicks(keel::f64 mn, keel::f64 mx, int target = 10);
keel::Array<keel::f64> logTicks(keel::f64 mn, keel::f64 mx);
keel::String tickLabel(keel::f64 v);

using SvgImageEmbedder =
    keel::Function<keel::String(keel::ArrayRef<keel::u8>, int, int)>;

keel::String renderFigureSvg(const FigureState &fig, int W, int H,
                             const SvgImageEmbedder &embed = {});
keel::String renderFigureEps(const FigureState &fig, int W, int H);
keel::String renderFigurePdf(const FigureState &fig, int W, int H);
keel::Array<keel::u8> renderFigureRgb(const FigureState &fig, int W, int H);

} // namespace figrender
} // namespace numkit
