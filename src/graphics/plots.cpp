// nk/graphics â€” plots.cpp
//
// Registration hub for the graphics library. buildPlotTable() assembles the
// full plotting table by dispatching to the per-family builders; each appends
// its graphics.<sub>.<name> (+ compat.<name>) entries to the table. The
// bundle-side installer (library.cpp) wraps each entry's core-free GraphicsFn
// and registers it.
//
// One .cpp per plot family (namespace_design.md Â§5, Â§9.5):
//   plots/layout.cpp   â€” figure / subplot / hold / axes / labels / limits / legend / ticks
//   plots/line.cpp     â€” plot / plot3 / stem / stairs / area / errorbar / semilog* / fplot / xline
//   plots/bar.cpp      â€” bar / barh / scatter / hist / histogram / pie / boxplot / patch / heatmap
//   plots/polar.cpp    â€” polarplot / polarscatter / polarhistogram / rlim / thetalim / r*ticks
//   plots/image.cpp    â€” imagesc / pcolor / imshow
//   plots/contour.cpp  â€” contour / contourf
//   plots/surface.cpp  â€” surf / mesh / slice / isosurface / coneplot / streamtube / lighting
//   plots/helpers.cpp  â€” pure JSON/parse helpers (numkit::detail)
//   plots/shared.cpp   â€” plot bodies shared by more than one family (numkit::detail)

#include <numkit/graphics/graphics_context.hpp>

#include "plots/plot_internal.hpp"

#include <vector>

namespace numkit {

void buildPlotTable(std::vector<PlotEntry> &table)
{
    buildLayoutPlots(table);
    buildLinePlots(table);
    buildBarPlots(table);
    buildPolarPlots(table);
    buildImagePlots(table);
    buildContourPlots(table);
    buildSurfacePlots(table);
}

}  // namespace numkit
