#pragma once
// nk/graphics — plots/plot_internal.hpp
//
// Private (src-only) header shared by the per-family plot-table builders carved
// out of plots.cpp. NOT installed — only the graphics .cpp TUs include it.
//
// It declares (1) the pure JSON/parse helpers (numkit::detail) that many plot
// bodies share, and (2) the per-family build*Plots(table) entry points that
// buildPlotTable() dispatches to.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/graphics/graphics_context.hpp>  // GraphicsContext, PlotEntry, GraphicsFn
#include <numkit/core/figure_manager.hpp>      // DatasetInfo
#include <numkit/value/value.hpp>                // Value

#include <cstddef>
#include <cstdio>

namespace numkit {

namespace detail {

// Lightweight keel::String stream builder replacing std::ostringstream for
// JSON / style assembly inside the core-free plot builders.
struct StrBuf {
    keel::String buf;

    void reserve(keel::usize n) { buf.reserve(n); }
    void clear() { buf.clear(); }
    bool empty() const { return buf.empty(); }
    keel::StringRef as_ref() const { return buf.as_ref(); }
    keel::String str() const { return buf.clone(); }
    keel::String take() { return keel::move(buf); }

    StrBuf &operator<<(char c)
    {
        buf.push_back(c);
        return *this;
    }
    StrBuf &operator<<(const char *s)
    {
        if (s)
            buf.append(keel::StringRef(s));
        return *this;
    }
    StrBuf &operator<<(keel::StringRef s)
    {
        buf.append(s);
        return *this;
    }
    StrBuf &operator<<(const keel::String &s)
    {
        buf.append(s.as_ref());
        return *this;
    }
    StrBuf &operator<<(int v)
    {
        char tmp[32];
        int n = std::snprintf(tmp, sizeof(tmp), "%d", v);
        if (n > 0)
            buf.append(keel::StringRef(tmp, keel::as<keel::usize>(n)));
        return *this;
    }
    StrBuf &operator<<(unsigned int v)
    {
        char tmp[32];
        int n = std::snprintf(tmp, sizeof(tmp), "%u", v);
        if (n > 0)
            buf.append(keel::StringRef(tmp, keel::as<keel::usize>(n)));
        return *this;
    }
    StrBuf &operator<<(long v)
    {
        char tmp[32];
        int n = std::snprintf(tmp, sizeof(tmp), "%ld", v);
        if (n > 0)
            buf.append(keel::StringRef(tmp, keel::as<keel::usize>(n)));
        return *this;
    }
    StrBuf &operator<<(unsigned long v)
    {
        char tmp[32];
        int n = std::snprintf(tmp, sizeof(tmp), "%lu", v);
        if (n > 0)
            buf.append(keel::StringRef(tmp, keel::as<keel::usize>(n)));
        return *this;
    }
    StrBuf &operator<<(long long v)
    {
        char tmp[32];
        int n = std::snprintf(tmp, sizeof(tmp), "%lld", v);
        if (n > 0)
            buf.append(keel::StringRef(tmp, keel::as<keel::usize>(n)));
        return *this;
    }
    StrBuf &operator<<(unsigned long long v)
    {
        char tmp[32];
        int n = std::snprintf(tmp, sizeof(tmp), "%llu", v);
        if (n > 0)
            buf.append(keel::StringRef(tmp, keel::as<keel::usize>(n)));
        return *this;
    }
    StrBuf &operator<<(float v)
    {
        return (*this) << keel::as<double>(v);
    }
    StrBuf &operator<<(double v)
    {
        char tmp[64];
        int n = std::snprintf(tmp, sizeof(tmp), "%.6g", v);
        if (n > 0)
            buf.append(keel::StringRef(tmp, keel::as<keel::usize>(n)));
        return *this;
    }
};

// Render a numeric/complex Value as a JSON number array ("[1,2,3]"). Complex
// uses magnitude; NaN -> null; +/-Inf -> +/-1e308.
keel::String vecToJson(const Value &v);
keel::String vecToJson(keel::ArrayRef<double> v);

// "[1,2,...,n]" — the 1-based implicit-X axis for a plot given only Y.
keel::String makeIndexJson(keel::usize n);

// A char Value as its raw string (title / label text).
keel::String argStr(const Value &v);

// Parse trailing 'LineWidth' / 'MarkerSize' N-V pairs from args[startIdx..].
void parsePlotArgs(keel::ArrayRef<Value> args, keel::usize startIdx, DatasetInfo &ds);

// Parse the leading (x,y[,style]) / (y[,style]) of a plot call into ds and
// return the index where trailing N-V pairs begin.
keel::usize parsePlotXYStyle(keel::ArrayRef<Value> args, DatasetInfo &ds);

// Append a double to os as JSON (NaN -> null; +/-Inf -> +/-1e308).
void doubleToJson(StrBuf &os, double val);


// Plot bodies shared by >1 family (defined in plots/shared.cpp).
void heatmapImpl(const char *typeName, bool axisIj, keel::ArrayRef<Value> args,
                 keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc);
void geoForward(const char *target, keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs,
                GraphicsContext &gc);
void delegateTo(const char *target, keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs,
                GraphicsContext &gc);

}  // namespace detail

// Per-family plot-table builders (carved out of plots.cpp). Each appends its
// graphics.<sub>.<name> entries to `table`; buildPlotTable() calls them in turn.
void buildContourPlots(keel::Array<PlotEntry> &table);
void buildPolarPlots(keel::Array<PlotEntry> &table);
void buildSurfacePlots(keel::Array<PlotEntry> &table);
void buildLayoutPlots(keel::Array<PlotEntry> &table);
void buildImagePlots(keel::Array<PlotEntry> &table);
void buildLinePlots(keel::Array<PlotEntry> &table);
void buildBarPlots(keel::Array<PlotEntry> &table);
// BUILDER-DECLS — the family split appends new declarations directly above here.

}  // namespace numkit
