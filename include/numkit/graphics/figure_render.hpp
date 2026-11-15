/// @file figure_render.hpp
/// @ingroup group_graphics
///
/// Software figure renderer declarations (SVG / EPS / PDF / RGB raster).
#pragma once

#include <keel/core.h>
#include <numkit/core/figure_manager.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace numkit {
namespace figrender {

const keel::u8 *namedPalette(const std::string &name, int &n);

void parseNestedDoubles(const std::string &j, std::vector<keel::f64> &out,
                        keel::usize &rows, keel::usize &cols);

std::vector<keel::f64> niceTicks(keel::f64 mn, keel::f64 mx, int target = 10);
std::vector<keel::f64> logTicks(keel::f64 mn, keel::f64 mx);
std::string tickLabel(keel::f64 v);

using SvgImageEmbedder =
    std::function<std::string(const std::vector<keel::u8> &, int, int)>;

std::string renderFigureSvg(const FigureState &fig, int W, int H,
                            const SvgImageEmbedder &embed = {});
std::string renderFigureEps(const FigureState &fig, int W, int H);
std::string renderFigurePdf(const FigureState &fig, int W, int H);
std::vector<keel::u8> renderFigureRgb(const FigureState &fig, int W, int H);

} // namespace figrender
} // namespace numkit
