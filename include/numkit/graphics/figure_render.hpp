/// @file figure_render.hpp
/// @ingroup group_graphics
///
/// Software figure renderer declarations (SVG / EPS / PDF / RGB raster).
#pragma once

#include <numkit/core/figure_manager.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace numkit {
namespace figrender {

const std::uint8_t *namedPalette(const std::string &name, int &n);

void parseNestedDoubles(const std::string &j, std::vector<double> &out,
                        size_t &rows, size_t &cols);

std::vector<double> niceTicks(double mn, double mx, int target = 10);
std::vector<double> logTicks(double mn, double mx);
std::string tickLabel(double v);

using SvgImageEmbedder =
    std::function<std::string(const std::vector<std::uint8_t> &, int, int)>;

std::string renderFigureSvg(const FigureState &fig, int W, int H,
                            const SvgImageEmbedder &embed = {});
std::string renderFigureEps(const FigureState &fig, int W, int H);
std::string renderFigurePdf(const FigureState &fig, int W, int H);
std::vector<std::uint8_t> renderFigureRgb(const FigureState &fig, int W, int H);

} // namespace figrender
} // namespace numkit
