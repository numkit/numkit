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


namespace numkit {
namespace figrender {

const keel::u8 *namedPalette(keel::StringRef name, keel::i32 &n);

void parseNestedDoubles(keel::StringRef j, keel::Array<keel::f64> &out,
                        keel::usize &rows, keel::usize &cols);

keel::Array<keel::f64> niceTicks(keel::f64 mn, keel::f64 mx, keel::i32 target = 10);
keel::Array<keel::f64> logTicks(keel::f64 mn, keel::f64 mx);
keel::String tickLabel(keel::f64 v);

using SvgImageEmbedder =
    keel::Function<keel::String(keel::ArrayRef<keel::u8>, keel::i32, keel::i32)>;

keel::String renderFigureSvg(const FigureState &fig, keel::i32 W, keel::i32 H,
                             const SvgImageEmbedder &embed = {});
keel::String renderFigureEps(const FigureState &fig, keel::i32 W, keel::i32 H);
keel::String renderFigurePdf(const FigureState &fig, keel::i32 W, keel::i32 H);
keel::Array<keel::u8> renderFigureRgb(const FigureState &fig, keel::i32 W, keel::i32 H);

} // namespace figrender
} // namespace numkit
