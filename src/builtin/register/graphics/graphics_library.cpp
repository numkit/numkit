// src/graphics/src/library.cpp
//
// Engine-coupled installer for the graphics plotting service â€” the SOLE
// graphics TU that includes <nk/core>. It is registration glue only: it
// asks plots.cpp (core-free) for the table of GraphicsFn plotting bodies, then
// wraps each in a CallContextâ†’GraphicsContext adapter and registers it under
//   graphics.<sub>.<name>   +   compat.<name>   (+ bare core name when core=true)
//
// The adapter is the one place graphics' two runtime escape hatches are bound
// to the live Engine: callBuiltin (forward to another registered plot builtin
// by name â€” surfcâ†’surf, fcontourâ†’contour, geoplotâ†’plot, â€¦) and callHandle
// (evaluate a user @(x) function handle for fplot/fsurf sampling). Everything
// else a plotting body needs flows through the figure session (FigureManager)
// and a scratch arena, both core-free.
//
// This mirrors the toolbox convention: compute is core-free, the install hub is
// the lone Engine-coupled file (here it also hosts the generic adapter, since
// graphics has one uniform adapter rather than per-function _reg bridges).
//
// â”€â”€ Graphics handles (bugs/closed/graphics/plot-family-no-return-value.md) â”€â”€
// This TU is also the handle ACCESSOR layer:
//   â€¢ the adapter post-hook binds handle objects for chart builtins
//     (h = plot(...)) and for figure/gca/gcf/axes;
//   â€¢ handle-aware set/get/isgraphics/ishandle/isvalid are registered here
//     (delete lives in general_reg.cpp â€” one core name, type-dispatched);
//   â€¢ one BuiltinClass per MATLAB class name routes dot-syntax
//     (h.LineWidth, h.LineWidth = 2) through the FigureManager registry.

#include <numkit/graphics/library.hpp>
#include <numkit/graphics/graphics_context.hpp>
#include <numkit/graphics/graphics_handle.hpp>

#include <numkit/core/engine.hpp>  // Engine, CallContext, ExternalFunc, Span, Value
#include <numkit/core/figure_manager.hpp>
#include <numkit/core/figure_render.hpp>
#include <numkit/value/object.hpp>

#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace numkit {

// export_reg.cpp â€” saveas/exportgraphics/print (portion 32).
void registerFigureExport(Engine &engine);

namespace {

constexpr const char *kFigureClass = "matlab.ui.Figure";
constexpr const char *kAxesClass   = "matlab.graphics.axis.Axes";
constexpr const char *kTextClass   = "matlab.graphics.primitive.Text";
constexpr const char *kLegendClass = "matlab.graphics.illustration.Legend";
constexpr const char *kRootClass   = "matlab.ui.Root";

// Builtin name â†’ Text slot for the title/label family (adapter wrap).
const std::string &textSlotOf(const std::string &fnName)
{
    static const std::map<std::string, std::string> m = {
        {"title", "title"}, {"subtitle", "subtitle"}, {"sgtitle", "sgtitle"},
        {"xlabel", "xlabel"}, {"ylabel", "ylabel"}, {"zlabel", "zlabel"},
    };
    static const std::string kEmpty;
    auto it = m.find(fnName);
    return it == m.end() ? kEmpty : it->second;
}

// Axes PROPERTY name â†’ Text slot (get(gca,'Title') family â€” capitalized).
const std::string &textSlotOfProp(const std::string &prop)
{
    static const std::map<std::string, std::string> m = {
        {"Title", "title"}, {"Subtitle", "subtitle"},
        {"XLabel", "xlabel"}, {"YLabel", "ylabel"}, {"ZLabel", "zlabel"},
    };
    static const std::string kEmpty;
    auto it = m.find(prop);
    return it == m.end() ? kEmpty : it->second;
}

// Which AxesState field a Text slot reads/writes ("sgtitle" â†’ the
// FigureState instead). ylabel under yyaxis-right routes to ylabel2.
std::string *textFieldOf(FigureManager &fm, FigureManager::HandleRecord &rec)
{
    if (rec.slot == "sgtitle") {
        FigureState *fig = fm.figureOf(rec.figureId);
        return fig ? &fig->superTitle : nullptr;
    }
    AxesState *ax = fm.axesOf(rec);
    if (!ax)
        return nullptr;
    if (rec.slot == "title")    return &ax->title;
    if (rec.slot == "subtitle") return &ax->subtitle;
    if (rec.slot == "xlabel")   return &ax->xlabel;
    if (rec.slot == "ylabel") {
        if (ax->yyEnabled && ax->activeYside == "right")
            return &ax->ylabel2;
        return &ax->ylabel;
    }
    if (rec.slot == "zlabel")   return &ax->zlabel;
    return nullptr;
}

// Builtin name â†’ (type, MATLAB class name)
const std::unordered_map<std::string, std::pair<std::string, std::string>> &
handleTypes()
{
    static const std::unordered_map<std::string,
        std::pair<std::string, std::string>> m = {
        {"plot",        {"line",    "matlab.graphics.chart.primitive.Line"}},
        {"plot3",       {"line",    "matlab.graphics.chart.primitive.Line"}},
        {"line",        {"line",    "matlab.graphics.chart.primitive.Line"}},
        {"scatter",     {"scatter", "matlab.graphics.chart.scatter.Scatter"}},
        {"scatter3",    {"scatter", "matlab.graphics.chart.scatter.Scatter"}},
        {"stem",        {"stem",    "matlab.graphics.chart.primitive.Stem"}},
        {"stem3",       {"stem",    "matlab.graphics.chart.primitive.Stem"}},
        {"stairs",      {"stairs",  "matlab.graphics.chart.primitive.Stair"}},
        {"bar",         {"bar",     "matlab.graphics.chart.primitive.Bar"}},
        {"barh",        {"bar",     "matlab.graphics.chart.primitive.Bar"}},
        {"errorbar",    {"errorbar","matlab.graphics.chart.primitive.ErrorBar"}},
        {"area",        {"area",    "matlab.graphics.chart.primitive.Area"}},
        {"pie",         {"pie",     "matlab.graphics.chart.primitive.Pie"}},
        {"compass",     {"compass", "matlab.graphics.chart.primitive.Compass"}},
        {"polarplot",   {"line",    "matlab.graphics.chart.primitive.Line"}},
        {"polar",       {"line",    "matlab.graphics.chart.primitive.Line"}},
        {"patch",       {"polygon", "matlab.graphics.primitive.Patch"}},
        {"fill",        {"polygon", "matlab.graphics.primitive.Patch"}},
        {"surf",        {"surface", "matlab.graphics.chart.surface.Surface"}},
        {"surface",     {"surface", "matlab.graphics.primitive.Surface"}},
        {"pcolor",      {"surface", "matlab.graphics.primitive.Surface"}},
        {"mesh",        {"surface", "matlab.graphics.chart.surface.Surface"}},
        {"waterfall",   {"surface", "matlab.graphics.chart.surface.Waterfall"}},
        {"contour",     {"contour", "matlab.graphics.chart.contour.Contour"}},
        {"contour3",    {"contour", "matlab.graphics.chart.contour.Contour"}},
        {"contourf",    {"contour", "matlab.graphics.chart.contour.Contour"}},
        {"imagesc",     {"image",   "matlab.graphics.image.Image"}},
        {"image",       {"image",   "matlab.graphics.image.Image"}},
        {"quiver",      {"quiver",  "matlab.graphics.chart.primitive.Quiver"}},
        {"feather",     {"feather", "matlab.graphics.chart.primitive.Feather"}},
        {"histogram",   {"histogram","matlab.graphics.chart.histogram.Histogram"}},
        {"fplot",       {"line",    "matlab.graphics.chart.primitive.Line"}},
        {"semilogx",    {"line",    "matlab.graphics.chart.primitive.Line"}},
        {"semilogy",    {"line",    "matlab.graphics.chart.primitive.Line"}},
        {"loglog",      {"line",    "matlab.graphics.chart.primitive.Line"}},
    };
    return m;
}

// â”€â”€ Property encoding â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// rec->props maps name â†’ string form (figure layer is Value-free).
// Numerics use %.17g so decodeProp's stod round-trips bit-exactly
// (std::to_string's 6 digits would silently truncate user values).
std::string formatPropDouble(double d)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return buf;
}

std::string encodeProp(const Value &v)
{
    if (v.isChar() || v.isString())
        return v.toString();
    if (v.isNumeric() && v.isScalar())
        return formatPropDouble(v.toScalar());
    if (v.isLogical() && v.numel() == 1)
        return v.toBool() ? "1" : "0";
    if (v.isNumeric() && v.dims().rows() > 1 && v.dims().cols() > 1) {
        const size_t R = v.dims().rows();
        const size_t C = v.dims().cols();
        std::string s = "[";
        for (size_t r = 0; r < R; ++r) {
            if (r) s += ",";
            s += "[";
            for (size_t c = 0; c < C; ++c) {
                if (c) s += ",";
                const double x = v.elemAsDouble(c * R + r);
                if (std::isfinite(x)) s += formatPropDouble(x);
                else s += "null";
            }
            s += "]";
        }
        return s + "]";
    }
    if (v.isNumeric() && v.numel() >= 2) {
        std::string s = "[";
        for (size_t i = 0; i < v.numel(); ++i) {
            if (i) s += ",";
            s += formatPropDouble(v.elemAsDouble(i));
        }
        return s + "]";
    }
    return "";
}

Value decodeProp(const std::string &s, std::pmr::memory_resource *mr)
{
    if (s.size() >= 2 && s[0] == '[' && s[1] == '[') {
        std::vector<double> zf;
        size_t rows = 0, cols = 0;
        figrender::parseNestedDoubles(s, zf, rows, cols);
        if (rows > 0 && cols > 0 && zf.size() >= rows * cols) {
            auto r = Value::matrix(rows, cols, ValueType::DOUBLE, mr);
            double *rd = r.doubleDataMut();
            for (size_t rr = 0; rr < rows; ++rr)
                for (size_t cc = 0; cc < cols; ++cc)
                    rd[cc * rows + rr] = zf[rr * cols + cc];
            return r;
        }
    }
    if (!s.empty() && s[0] == '[') {
        std::vector<double> vals;
        std::string num;
        for (size_t i = 1; i < s.size(); ++i) {
            if (s[i] == ',' || s[i] == ']') {
                if (!num.empty()) {
                    try { vals.push_back(std::stod(num)); } catch (...) {}
                    num.clear();
                }
            } else num += s[i];
        }
        if (vals.size() == 1)
            return Value::scalar(vals[0], mr);
        if (!vals.empty()) {
            auto r = Value::matrix(1, vals.size(), ValueType::DOUBLE, mr);
            for (size_t i = 0; i < vals.size(); ++i)
                r.doubleDataMut()[i] = vals[i];
            return r;
        }
    }
    try { return Value::scalar(std::stod(s), mr); } catch (...) {}
    return Value::fromString(s, mr);
}

// â”€â”€ Chart property defaults (MATLAB R2025b documented values) â”€â”€â”€â”€â”€â”€â”€â”€
// Introspection reports MATLAB's defaults even where the nk renderer
// draws its own palette (get before set); once the user sets a property,
// both get() and the render use the user value.
Value chartDefaultProp(const std::string &type, const std::string &name,
                       std::pmr::memory_resource *mr)
{
    auto str = [&](const char *s) { return Value::fromString(s, mr); };
    auto num = [&](double d) { return Value::scalar(d, mr); };
    auto rgb = [&](double r, double g, double b) {
        auto v = Value::matrix(1, 3, ValueType::DOUBLE, mr);
        v.doubleDataMut()[0] = r; v.doubleDataMut()[1] = g; v.doubleDataMut()[2] = b;
        return v;
    };
    if (name == "Type")       return str(type.c_str());
    if (name == "Visible")    return str("on");
    if (name == "LineWidth")  return num(0.5);
    if (name == "MarkerSize") return num(6.0);
    if (name == "Marker")     return str(type == "scatter" ? "o" : "none");
    if (name == "LineStyle")  return str(type == "scatter" ? "none" : "-");
    if (name == "DisplayName")return str("");
    if (name == "Color") {
        if (type == "scatter") return rgb(0.2902, 0.0627, 0.5294);
        return rgb(0, 0.4470, 0.7410);
    }
    if (name == "SizeData" && type == "scatter") return num(36.0);
    if (name == "BaseValue" && (type == "stem" || type == "area" || type == "bar"))
        return num(0.0);
    if (name == "BarWidth" && type == "bar") return num(0.8);
    if (name == "Children")   return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    return Value();   // unknown â†’ caller decides (empty â‡’ error/blank)
}

// â”€â”€ Style-string rebuild (render-effect for LineStyle/Marker/Color) â”€â”€
// ds.style speaks two dialects (renderer parseLineSpec): the compact
// MATLAB linespec ("r--o") and the kv extras ("color=#hex;lineStyle=--").
// A property set rewrites the string into the kv form, preserving the
// components the user did not change.
struct StyleParts { std::string colorHex, lineStyle, marker; };

StyleParts parseStyle(const std::string &s)
{
    StyleParts p;
    if (s.empty()) return p;
    if (s.find('=') != std::string::npos) {
        for (size_t i = 0; i < s.size();) {
            size_t eq = s.find('=', i);
            if (eq == std::string::npos) break;
            size_t semi = s.find(';', eq);
            std::string key = s.substr(i, eq - i);
            std::string val = s.substr(eq + 1,
                (semi == std::string::npos ? s.size() : semi) - eq - 1);
            if (key == "color") p.colorHex = val;
            else if (key == "lineStyle") p.lineStyle = val;
            else if (key == "marker") p.marker = val;
            i = semi == std::string::npos ? s.size() : semi + 1;
        }
        return p;
    }
    // Compact dialect â€” mirrors the renderer's STYLE_COLOR map so a
    // rebuilt style keeps the same visual colour the plot was drawn with.
    static const std::map<char, const char *> kCharColor = {
        {'r', "#f07070"}, {'g', "#6ee7a0"}, {'b', "#60d0f0"},
        {'k', "#d4d4f0"}, {'m', "#e070c0"}, {'c', "#60d0f0"},
        {'y', "#e8d060"}, {'w', "#ffffff"},
    };
    size_t i = 0;
    while (i < s.size()) {
        if (p.lineStyle.empty() && s.compare(i, 2, "--") == 0) { p.lineStyle = "--"; i += 2; continue; }
        if (p.lineStyle.empty() && s.compare(i, 2, "-.") == 0)  { p.lineStyle = "-.";  i += 2; continue; }
        char c = s[i];
        if (p.lineStyle.empty() && (c == '-' || c == ':')) { p.lineStyle = std::string(1, c); ++i; continue; }
        if (p.colorHex.empty()) {
            auto hit = kCharColor.find(c);
            if (hit != kCharColor.end()) { p.colorHex = hit->second; ++i; continue; }
        }
        if (p.marker.empty() && std::string("o+*.xsd^v<>ph").find(c) != std::string::npos) {
            p.marker = std::string(1, c); ++i; continue;
        }
        ++i;
    }
    return p;
}

std::string rgbToHex(const Value &v)
{
    auto clamp255 = [](double d) {
        int i = static_cast<int>(d * 255.0 + 0.5);
        if (i < 0) i = 0;
        if (i > 255) i = 255;
        return i;
    };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x",
                  clamp255(v.numel() > 0 ? v.elemAsDouble(0) : 0),
                  clamp255(v.numel() > 1 ? v.elemAsDouble(1) : 0),
                  clamp255(v.numel() > 2 ? v.elemAsDouble(2) : 0));
    return buf;
}

// â”€â”€ Render-effect: apply one property to the backing DatasetInfo â”€â”€â”€â”€â”€
void applyChartProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                    const std::string &name, const Value &val)
{
    DatasetInfo *ds = fm.datasetOf(*rec);
    if (!ds)
        return;
    bool touched = true;
    if (name == "LineWidth" && val.isNumeric() && val.numel() == 1) {
        double w = val.toScalar();
        ds->lineWidth = w > 0 ? w : 0.0;   // 0/negative = default
    } else if (name == "MarkerSize" && val.isNumeric() && val.numel() == 1) {
        double m = val.toScalar();
        ds->markerSize = m > 0 ? m : 0.0;
    } else if (name == "LineStyle" || name == "Marker" || name == "Color") {
        StyleParts p = parseStyle(ds->style);
        if (name == "LineStyle") p.lineStyle = val.toString();
        else if (name == "Marker") p.marker = val.toString();
        else p.colorHex = rgbToHex(val);
        std::string out;
        auto append = [&](const char *k, const std::string &v) {
            if (v.empty() || v == "none") return;
            if (!out.empty()) out += ";";
            out += std::string(k) + "=" + v;
        };
        append("color", p.colorHex);
        append("lineStyle", p.lineStyle);
        append("marker", p.marker);
        ds->style = out;   // empty â‡’ renderer defaults (palette line, no marker)
    } else if (name == "Visible") {
        ds->visible = !(val.isChar() || val.isString()) || val.toString() != "off";
    } else if (name == "DisplayName") {
        ds->label = val.isChar() || val.isString() ? val.toString() : "";
    } else if ((name == "XData" || name == "YData") && val.isNumeric()) {
        // Replace the series. Huge series: drop the downsample state and
        // re-decide via maybeDownsampleSeries (rare path, correctness first).
        std::vector<double> xs(val.numel());
        for (size_t i = 0; i < val.numel(); ++i)
            xs[i] = val.elemAsDouble(i);
        ds->xRaw.clear(); ds->yRaw.clear(); ds->seriesPyramid.clear();
        ds->seriesDownsampled = false; ds->seriesN = 0;
        (name == "XData" ? ds->xJson : ds->yJson) =
            figdetail::serializeFlatDoubles(xs);
        if (ds->xJson.empty()) ds->xJson = "[]";
        if (ds->yJson.empty()) ds->yJson = "[]";
        fm.maybeDownsampleSeries(*ds);
    } else if (name == "ZData" && val.isNumeric()) {
        if (val.dims().rows() > 1 && val.dims().cols() > 1
            && (ds->type == "surf" || ds->type == "waterfall"
                || ds->type == "contour3")) {
            ds->zJson = encodeProp(val);
        } else if (val.dims().rows() == 1 || val.dims().cols() == 1) {
            std::vector<double> zs(val.numel());
            for (size_t i = 0; i < val.numel(); ++i)
                zs[i] = val.elemAsDouble(i);
            ds->zJson = figdetail::serializeFlatDoubles(zs);
        } else {
            touched = false;
        }
    } else {
        touched = false;   // stored in rec->props only
    }
    if (touched)
        fm.markFigureModified(rec->figureId);
}

// Auto tick positions for get(gca,'XTick') in 'auto' mode â€” the SAME
// niceTicks the renderer uses, over the explicit xlim if set, else the
// data range (portion 34; MATLAB returns the drawn ticks).
std::vector<double> axesAutoTicks(FigureManager &fm,
                                  FigureManager::HandleRecord *rec, char axis)
{
    AxesState *ax = fm.axesOf(*rec);
    if (!ax)
        return {};
    const bool log = (axis == 'x' ? ax->xscale : ax->yscale) == "log";
    double lo = 0, hi = 1;
    const std::string &lim = axis == 'x' ? ax->xlimJson : ax->ylimJson;
    if (!lim.empty()) {
        auto v = figdetail::parseFlatDoubles(lim);
        if (v.size() == 2) {
            lo = v[0];
            hi = v[1];
        }
    } else {
        lo = +INFINITY;
        hi = -INFINITY;
        int imgDatasets = 0, seriesDatasets = 0;
        for (const auto &ds : ax->datasets) {
            if (!ds.visible)
                continue;
            if (ds.type == "imagesc" || ds.type == "image-rgb") {
                ++imgDatasets;
                size_t cells = (axis == 'x')
                                   ? (ds.originalCols ? ds.originalCols : 1)
                                   : (ds.originalRows ? ds.originalRows : 1);
                auto v = figdetail::parseFlatDoubles(axis == 'x' ? ds.xJson : ds.yJson);
                if (v.size() < 2 || cells == 0) {
                    lo = std::min(lo, 0.5);
                    hi = std::max(hi, cells > 0 ? cells + 0.5 : 1.5);
                } else {
                    double span = v.back() - v.front();
                    double sp = cells > 1 ? span / (double)(cells - 1) : (span > 0 ? span : 1.0);
                    lo = std::min(lo, v.front() - sp / 2);
                    hi = std::max(hi, v.back() + sp / 2);
                }
                continue;
            }
            if (ds.type == "text")
                continue;
            ++seriesDatasets;
            const std::string &j = axis == 'x' ? ds.xJson : ds.yJson;
            for (double v : figdetail::parseFlatDoubles(j))
                if (std::isfinite(v)) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
        }
        if (!(hi > lo)) {
            lo = 0;
            hi = 1;
        }
        bool imageOnly = imgDatasets > 0 && seriesDatasets == 0;
        if (!imageOnly) {
            double d = (hi - lo) * 0.05;
            lo -= d;
            hi += d;
        }
    }
    if (log) {
        if (!(lo > 0) || !(hi > lo))
            return {};
        return figrender::logTicks(lo, hi);
    }
    return figrender::niceTicks(lo, hi);
}

// MATLAB R2025b (probe-verified): setting the AXES font RESETS even an
// explicitly-set label-Text font â€” the texts fall back to the axes
// values (FontSize Ã—1.1). Drop their stored font props.
void resetAxesTextFonts(FigureManager &fm, FigureManager::HandleRecord *rec)
{
    for (auto &[id, r] : fm.handles())
        if (!r.deleted && r.type == "text" && r.figureId == rec->figureId
            && r.axesIndex == rec->axesIndex) {
            r.props.erase("FontSize");
            r.props.erase("FontWeight");
            r.props.erase("FontName");
        }
}

// tick vector Value â† json "[a,b,c]"
Value decodeTickVec(const std::string &json, std::pmr::memory_resource *mr)
{
    auto v = figdetail::parseFlatDoubles(json);
    if (v.empty())
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    auto out = Value::matrix(1, v.size(), ValueType::DOUBLE, mr);
    for (size_t i = 0; i < v.size(); ++i)
        out.doubleDataMut()[i] = v[i];
    return out;
}

// ["a","b"] json â†’ 1Ã—N cell of char.
Value decodeTickLabels(const std::string &json, std::pmr::memory_resource *mr)
{
    std::vector<std::string> labels;
    size_t i = 0;
    while (i < json.size()) {
        size_t q1 = json.find('"', i);
        if (q1 == std::string::npos)
            break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos)
            break;
        labels.push_back(json.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    if (labels.empty())
        return Value::cell(0, 0, mr);
    Value c = Value::cell(1, labels.size(), mr);
    for (size_t k = 0; k < labels.size(); ++k)
        c.cellAt(k) = Value::fromString(labels[k], mr);
    return c;
}

// cell/string-array/char â†’ ["a","b"] json.
std::string encodeTickLabels(const Value &v)
{
    std::string out = "[";
    for (size_t i = 0; i < v.numel(); ++i) {
        if (i)
            out += ",";
        out += "\"" + v.cellAt(i).toString() + "\"";
    }
    return out + "]";
}

// â”€â”€ Axes / figure property mapping (render-effect) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
bool applyAxesProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                   const std::string &name, const Value &val)
{
    AxesState *ax = fm.axesOf(*rec);
    if (!ax)
        return false;
    std::vector<double> v(val.isNumeric() ? val.numel() : 0);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = val.elemAsDouble(i);
    auto limJson = [](const std::vector<double> &w) {
        if (w.size() != 2) return std::string();
        return "[" + formatPropDouble(w[0]) + "," + formatPropDouble(w[1]) + "]";
    };
    bool touched = true;
    if      (name == "XLim")  ax->xlimJson  = limJson(v);
    else if (name == "YLim")  ax->ylimJson  = limJson(v);
    else if (name == "ZLim")  ax->zlimJson  = limJson(v);
    else if (name == "XScale") ax->xscale = val.toString();
    else if (name == "YScale") ax->yscale = val.toString();
    else if (name == "XGrid") ax->gridMajor = val.toString() == "on";
    else if (name == "YGrid") ax->gridMajor = val.toString() == "on";
    else if (name == "XLabel") ax->xlabel = val.toString();
    else if (name == "YLabel") ax->ylabel = val.toString();
    else if (name == "ZLabel") ax->zlabel = val.toString();
    else if (name == "Title")  ax->title  = val.toString();
    // â”€â”€ Portion 34 (MATLAB R2025b probe-verified semantics) â”€â”€
    else if (name == "FontSize" && val.isNumeric() && v.size() == 1) {
        ax->fontSize = v[0];
        resetAxesTextFonts(fm, rec);   // label Texts follow the axes font
    }
    else if (name == "FontWeight") {
        ax->fontWeight = val.toString();
        resetAxesTextFonts(fm, rec);
    }
    else if (name == "FontName") {
        ax->fontName = val.toString();
        resetAxesTextFonts(fm, rec);
    }
    else if (name == "LineWidth" && val.isNumeric() && v.size() == 1)
        ax->lineWidth = v[0];   // axes frame; series LineWidth is per-chart
    else if ((name == "XTick" || name == "YTick" || name == "ZTick")
             && (val.isNumeric() || val.isEmpty())) {
        std::ostringstream os;
        os << '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) os << ',';
            os << formatPropDouble(v[i]);
        }
        os << ']';
        (name == "XTick" ? ax->xTicksJson
         : name == "YTick" ? ax->yTicksJson : ax->zTicksJson) = os.str();
        (name == "XTick" ? ax->xTicksAuto
         : name == "YTick" ? ax->yTicksAuto : ax->zTicksAuto) = false;
    }
    else if ((name == "XTickLabel" || name == "YTickLabel"
              || name == "ZTickLabel")
             && (val.isCell() || val.isChar() || val.isString())) {
        std::string json = val.isCell() ? encodeTickLabels(val)
                                        : "[\"" + val.toString() + "\"]";
        (name == "XTickLabel" ? ax->xTickLabelsJson
         : name == "YTickLabel" ? ax->yTickLabelsJson
                                : ax->zTickLabelsJson) = json;
    }
    else if (name == "XTickMode" || name == "YTickMode"
             || name == "ZTickMode") {
        bool auto_ = val.toString() == "auto";
        if (name == "XTickMode") { ax->xTicksAuto = auto_; if (auto_) ax->xTicksJson.clear(); }
        if (name == "YTickMode") { ax->yTicksAuto = auto_; if (auto_) ax->yTicksJson.clear(); }
        if (name == "ZTickMode") { ax->zTicksAuto = auto_; if (auto_) ax->zTicksJson.clear(); }
    }
    else if (name == "XTickLabelMode" || name == "YTickLabelMode"
             || name == "ZTickLabelMode") {
        bool auto_ = val.toString() == "auto";
        if (name == "XTickLabelMode" && auto_) ax->xTickLabelsJson.clear();
        if (name == "YTickLabelMode" && auto_) ax->yTickLabelsJson.clear();
        if (name == "ZTickLabelMode" && auto_) ax->zTickLabelsJson.clear();
    }
    else if (name == "CLim" && v.size() == 2)
        ax->climJson = limJson(v);
    else touched = false;
    if (touched)
        fm.markFigureModified(rec->figureId);
    return touched;
}

Value axesGetProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                  const std::string &name, std::pmr::memory_resource *mr)
{
    auto str = [&](const char *s) { return Value::fromString(s, mr); };
    auto num = [&](double d) { return Value::scalar(d, mr); };
    if (name == "Type") return str("axes");
    AxesState *ax = fm.axesOf(*rec);
    if (!ax)
        return Value();
    auto autoLim = [&](char axisChar) {
        double lo = 0.0, hi = 1.0;
        double dmin = 1e300, dmax = -1e300;
        for (const auto &ds : ax->datasets) {
            if (!ds.visible) continue;
            const std::string &j = (axisChar == 'x') ? ds.xJson : (axisChar == 'y' ? ds.yJson : ds.zJson);
            for (double v : figdetail::parseFlatDoubles(j)) {
                if (std::isfinite(v)) {
                    if (v < dmin) dmin = v;
                    if (v > dmax) dmax = v;
                }
            }
        }
        if (dmin <= dmax) {
            if (dmax > dmin) {
                const double span = dmax - dmin;
                const double p10 = std::pow(10.0, std::floor(std::log10(span)));
                const double s = span / p10;
                const double step = (s <= 1.5) ? 0.2 * p10
                                  : (s <= 3.0) ? 0.5 * p10
                                  : (s <= 7.0 || s >= 9.5) ? 1.0 * p10
                                               : 2.0 * p10;
                lo = std::floor(dmin / step + 1e-12) * step;
                hi = std::ceil(dmax / step - 1e-12) * step;
            } else {
                lo = dmin - 1.0;
                hi = dmin + 1.0;
            }
        }
        Value v = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        v.doubleDataMut()[0] = lo;
        v.doubleDataMut()[1] = hi;
        return v;
    };
    if (name == "XLim") {
        if (!ax->xlimJson.empty()) return decodeProp(ax->xlimJson, mr);
        return autoLim('x');
    }
    if (name == "YLim") {
        if (!ax->ylimJson.empty()) return decodeProp(ax->ylimJson, mr);
        return autoLim('y');
    }
    if (name == "ZLim") {
        if (!ax->zlimJson.empty()) return decodeProp(ax->zlimJson, mr);
        return autoLim('z');
    }
    if (name == "Position" || name == "OuterPosition") {
        auto it = rec->props.find(name);
        if (it != rec->props.end())
            return decodeProp(it->second, mr);
        const FigureState *fig = fm.figureOf(rec->figureId);
        int m = (fig && fig->subplotRows >= 1) ? fig->subplotRows : 1;
        int n = (fig && fig->subplotCols >= 1) ? fig->subplotCols : 1;
        int p = (ax && ax->subplotIndex >= 1) ? ax->subplotIndex : 1;
        double left = 0.13, bottom = 0.11, width = 0.775, height = 0.815;
        if (m > 1 || n > 1) {
            int r = (p - 1) / n;
            int c = (p - 1) % n;
            double stepX = 0.775 / (n - 0.24);
            double stepY = 0.815 / (m - 0.28);
            width  = (n == 1) ? 0.775 : 0.76 * stepX;
            height = (m == 1) ? 0.815 : 0.72 * stepY;
            left   = 0.13 + c * stepX;
            bottom = 0.11 + (m - 1 - r) * stepY;
        }
        Value v = Value::matrix(1, 4, ValueType::DOUBLE, mr);
        v.doubleDataMut()[0] = left;
        v.doubleDataMut()[1] = bottom;
        v.doubleDataMut()[2] = width;
        v.doubleDataMut()[3] = height;
        return v;
    }
    if (name == "XScale") return str(ax->xscale.c_str());
    if (name == "YScale") return str(ax->yscale.c_str());
    if (name == "XLabel") return str(ax->xlabel.c_str());
    if (name == "YLabel") return str(ax->ylabel.c_str());
    if (name == "ZLabel") return str(ax->zlabel.c_str());
    if (name == "Title")  return str(ax->title.c_str());
    // â”€â”€ Portion 34 live reads (MATLAB R2025b probe-verified) â”€â”€
    if (name == "FontSize")   return num(ax->fontSize);
    if (name == "FontWeight") return str(ax->fontWeight.c_str());
    if (name == "FontName")   return str(ax->fontName.c_str());
    if (name == "LineWidth")  return num(ax->lineWidth);
    if (name == "XTick") {
        if (!ax->xTicksAuto)
            return decodeTickVec(ax->xTicksJson, mr);
        auto v = axesAutoTicks(fm, rec, 'x');
        if (v.empty())
            return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        auto out = Value::matrix(1, v.size(), ValueType::DOUBLE, mr);
        for (size_t i = 0; i < v.size(); ++i)
            out.doubleDataMut()[i] = v[i];
        return out;
    }
    if (name == "YTick") {
        if (!ax->yTicksAuto)
            return decodeTickVec(ax->yTicksJson, mr);
        auto v = axesAutoTicks(fm, rec, 'y');
        if (v.empty())
            return Value::matrix(0, 0, ValueType::DOUBLE, mr);
        auto out = Value::matrix(1, v.size(), ValueType::DOUBLE, mr);
        for (size_t i = 0; i < v.size(); ++i)
            out.doubleDataMut()[i] = v[i];
        return out;
    }
    if (name == "XTickMode")
        return str(ax->xTicksAuto ? "auto" : "manual");
    if (name == "YTickMode")
        return str(ax->yTicksAuto ? "auto" : "manual");
    if (name == "XTickLabelMode")
        return str(ax->xTickLabelsJson.empty() ? "auto" : "manual");
    if (name == "YTickLabelMode")
        return str(ax->yTickLabelsJson.empty() ? "auto" : "manual");
    if (name == "XTickLabel") {
        if (!ax->xTickLabelsJson.empty())
            return decodeTickLabels(ax->xTickLabelsJson, mr);
        auto v = !ax->xTicksAuto ? figdetail::parseFlatDoubles(ax->xTicksJson)
                                 : axesAutoTicks(fm, rec, 'x');
        Value c = Value::cell(v.size(), 1, mr);
        for (size_t i = 0; i < v.size(); ++i)
            c.cellAt(i) = Value::fromString(figrender::tickLabel(v[i]), mr);
        return c;
    }
    if (name == "YTickLabel") {
        if (!ax->yTickLabelsJson.empty())
            return decodeTickLabels(ax->yTickLabelsJson, mr);
        auto v = !ax->yTicksAuto ? figdetail::parseFlatDoubles(ax->yTicksJson)
                                 : axesAutoTicks(fm, rec, 'y');
        Value c = Value::cell(v.size(), 1, mr);
        for (size_t i = 0; i < v.size(); ++i)
            c.cellAt(i) = Value::fromString(figrender::tickLabel(v[i]), mr);
        return c;
    }
    if (name == "CLim") {
        if (!ax->climJson.empty())
            return decodeProp(ax->climJson, mr);
        // auto: imagesc cmin/cmax if present, else MATLAB's [0 1].
        for (const auto &ds : ax->datasets)
            if (!ds.zQuantized.empty()) {
                auto c = Value::matrix(1, 2, ValueType::DOUBLE, mr);
                c.doubleDataMut()[0] = ds.cminOrig;
                c.doubleDataMut()[1] = ds.cmaxOrig;
                return c;
            }
        auto c = Value::matrix(1, 2, ValueType::DOUBLE, mr);
        c.doubleDataMut()[0] = 0;
        c.doubleDataMut()[1] = 1;
        return c;
    }
    if (name == "CLimMode")
        return str(ax->climJson.empty() ? "auto" : "manual");
    if (name == "YDir")
        return str(ax->yDir.empty() ? "normal" : ax->yDir.c_str());
    if (name == "XDir")
        return str(ax->xDir.empty() ? "normal" : ax->xDir.c_str());
    return Value();
}

// XData/YData/ZData for chart records read from the live dataset (xRaw holds
// the full series when it was downsampled; xJson only the preview; zJson
// carries nested 2-D rows for surface/waterfall/contour3 or flat 1-D values).
Value chartDataProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                    const std::string &name, std::pmr::memory_resource *mr)
{
    DatasetInfo *ds = fm.datasetOf(*rec);
    if (rec->type == "histogram" && ds) {
        if (name == "BinEdges") {
            auto ev = figdetail::parseFlatDoubles(ds->zJson);
            auto v = Value::matrix(1, ev.size(), ValueType::DOUBLE, mr);
            for (size_t i = 0; i < ev.size(); ++i) v.doubleDataMut()[i] = ev[i];
            return v;
        }
        if (name == "Values" || name == "BinCounts") {
            auto cv = figdetail::parseFlatDoubles(ds->yJson);
            auto v = Value::matrix(1, cv.size(), ValueType::DOUBLE, mr);
            for (size_t i = 0; i < cv.size(); ++i) v.doubleDataMut()[i] = cv[i];
            return v;
        }
        if (name == "NumBins") {
            auto cv = figdetail::parseFlatDoubles(ds->yJson);
            return Value::scalar(static_cast<double>(cv.size()), mr);
        }
        if (name == "BinWidth") {
            auto ev = figdetail::parseFlatDoubles(ds->zJson);
            return Value::scalar(ev.size() >= 2 ? ev[1] - ev[0] : 1.0, mr);
        }
        if (name == "BinLimits") {
            auto ev = figdetail::parseFlatDoubles(ds->zJson);
            auto v = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            v.doubleDataMut()[0] = ev.empty() ? 0.0 : ev.front();
            v.doubleDataMut()[1] = ev.empty() ? 1.0 : ev.back();
            return v;
        }
        if (name == "Data") {
            auto dv = figdetail::parseFlatDoubles(ds->uJson);
            bool col = (ds->vJson == "col");
            auto v = Value::matrix(col ? dv.size() : 1, col ? 1 : dv.size(), ValueType::DOUBLE, mr);
            for (size_t i = 0; i < dv.size(); ++i) v.doubleDataMut()[i] = dv[i];
            return v;
        }
    }
    if (name != "XData" && name != "YData" && name != "ZData")
        return Value();
    if (!ds)
        return Value();
    if (name == "ZData") {
        if (ds->zJson.empty())
            return Value::matrix(1, 0, ValueType::DOUBLE, mr);
        if (ds->zJson.size() >= 2 && ds->zJson[0] == '[' && ds->zJson[1] == '[')
            return decodeProp(ds->zJson, mr);
        std::vector<double> zvals = figdetail::parseFlatDoubles(ds->zJson);
        if (zvals.empty())
            return Value::matrix(1, 0, ValueType::DOUBLE, mr);
        auto v = Value::matrix(1, zvals.size(), ValueType::DOUBLE, mr);
        for (size_t i = 0; i < zvals.size(); ++i)
            v.doubleDataMut()[i] = zvals[i];
        return v;
    }
    const std::vector<double> &raw = (name == "XData") ? ds->xRaw : ds->yRaw;
    std::vector<double> vals;
    if (!raw.empty()) {
        vals = raw;
    } else {
        vals = figdetail::parseFlatDoubles(
            (name == "XData") ? ds->xJson : ds->yJson);
    }
    if (vals.empty())
        return Value();
    auto v = Value::matrix(1, vals.size(), ValueType::DOUBLE, mr);
    for (size_t i = 0; i < vals.size(); ++i)
        v.doubleDataMut()[i] = vals[i];
    return v;
}

// â”€â”€ Handle record lookup helpers (create-or-reuse) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
int handleIdOfRecord(FigureManager &fm, FigureManager::HandleRecord *rec)
{
    for (auto &[id, r] : fm.handles())
        if (&r == rec)
            return id;
    return -1;
}

FigureManager::HandleRecord *findRecordBy(FigureManager &fm, const char *type,
                                          int figureId, size_t axesIndex)
{
    for (auto &[id, rec] : fm.handles())
        if (!rec.deleted && rec.type == type && rec.figureId == figureId
            && rec.axesIndex == axesIndex)
            return &rec;
    return nullptr;
}

Value makeAxesHandle(FigureManager &fm, std::pmr::memory_resource *mr)
{
    FigureState &fig = fm.current();
    const int fid = fm.currentFigureId();
    const size_t aidx = static_cast<size_t>(fig.currentAxes);
    if (auto *rec = findRecordBy(fm, "axes", fid, aidx))
        return makeGraphicsHandleValue(kAxesClass, handleIdOfRecord(fm, rec), mr);
    const int id = fm.createHandle("axes", kAxesClass);
    return makeGraphicsHandleValue(kAxesClass, id, mr);
}

Value makeFigureHandleFor(FigureManager &fm, int figureId,
                          std::pmr::memory_resource *mr)
{
    if (auto *rec = findRecordBy(fm, "figure", figureId, 0))
        return makeGraphicsHandleValue(kFigureClass, handleIdOfRecord(fm, rec), mr, figureId);
    // createHandle stamps the CURRENT figure â€” switch, create, switch back.
    const int saved = fm.currentFigureId();
    if (saved != figureId)
        fm.setFigure(figureId);
    const int id = fm.createHandle("figure", kFigureClass);
    if (saved != figureId)
        fm.setFigure(saved);
    return makeGraphicsHandleValue(kFigureClass, id, mr, figureId);
}

Value makeFigureHandle(FigureManager &fm, std::pmr::memory_resource *mr)
{
    fm.current();   // ensure the state exists
    return makeFigureHandleFor(fm, fm.currentFigureId(), mr);
}

// Axes handle at an EXPLICIT index (gcf.Children addresses non-current
// subplot cells too).
Value makeAxesHandleFor(FigureManager &fm, int figureId, size_t axesIndex,
                        std::pmr::memory_resource *mr)
{
    if (auto *rec = findRecordBy(fm, "axes", figureId, axesIndex))
        return makeGraphicsHandleValue(kAxesClass, handleIdOfRecord(fm, rec), mr);
    const int id = fm.createHandleAt(figureId, axesIndex, "axes", kAxesClass);
    return makeGraphicsHandleValue(kAxesClass, id, mr);
}

// Text handle bound to a label slot ("sgtitle" is figure-scoped: kNoAxes
// so scoped axes sweeps keep it alive while clf/close invalidate it).
Value makeTextHandleFor(FigureManager &fm, int figureId, size_t axesIndex,
                        const std::string &slot, std::pmr::memory_resource *mr)
{
    for (auto &[id, rec] : fm.handles())
        if (!rec.deleted && rec.type == "text" && rec.figureId == figureId
            && rec.slot == slot && rec.axesIndex == axesIndex)
            return makeGraphicsHandleValue(kTextClass, id, mr);
    const int id = fm.createHandleAt(figureId, axesIndex, "text", kTextClass,
                                     slot);
    return makeGraphicsHandleValue(kTextClass, id, mr);
}

Value makeLegendHandleFor(FigureManager &fm, int figureId, size_t axesIndex,
                          std::pmr::memory_resource *mr)
{
    if (auto *rec = findRecordBy(fm, "legend", figureId, axesIndex))
        return makeGraphicsHandleValue(kLegendClass, handleIdOfRecord(fm, rec), mr);
    const int id = fm.createHandleAt(figureId, axesIndex, "legend", kLegendClass);
    return makeGraphicsHandleValue(kLegendClass, id, mr);
}

// Root (groot) â€” ONE global record (figureId 0 never matches a real
// figure, so figure sweeps leave it alone).
Value makeRootHandle(FigureManager &fm, std::pmr::memory_resource *mr)
{
    for (auto &[id, rec] : fm.handles())
        if (!rec.deleted && rec.type == "root")
            return makeGraphicsHandleValue(kRootClass, id, mr);
    const int id = fm.createHandleAt(0, FigureManager::HandleRecord::kNoAxes,
                                     "root", kRootClass);
    return makeGraphicsHandleValue(kRootClass, id, mr);
}

// Array of handles (gcf.Children / groot.Children).
Value makeHandleArray(const std::string &className,
                      const std::vector<int> &ids,
                      std::pmr::memory_resource *mr)
{
    if (ids.empty())
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    std::vector<std::shared_ptr<ObjectState>> states;
    states.reserve(ids.size());
    for (int id : ids) {
        auto payload = std::make_shared<GraphicsHandlePayload>();
        payload->handleId = id;
        auto st = std::make_shared<ObjectState>(mr);
        st->native = std::move(payload);
        states.push_back(std::move(st));
    }
    Dims d{1, ids.size()};
    return Value::objectArray(className, d, std::move(states),
                              /*isHandle=*/true, mr);
}

// â”€â”€ Unified record property access â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// ONE body shared by get(h,'Prop') / set(h,'Prop',v) AND the dot-syntax
// h.Prop / h.Prop = v (BuiltinClass propGet/propSet). Live state fields
// (Text.String, Legend.Location/Box/Visible, chart XData) read from the
// figure state; everything user-set lives in rec->props; documented
// MATLAB defaults fill the rest.
std::string canonicalPropName(const std::string &raw)
{
    std::string low = raw;
    for (char &ch : low)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    static const std::pair<const char *, const char *> kNames[] = {
        {"xlim", "XLim"}, {"ylim", "YLim"}, {"zlim", "ZLim"}, {"clim", "CLim"},
        {"xtick", "XTick"}, {"ytick", "YTick"}, {"ztick", "ZTick"},
        {"xticklabel", "XTickLabel"}, {"yticklabel", "YTickLabel"}, {"zticklabel", "ZTickLabel"},
        {"xticklabels", "XTickLabel"}, {"yticklabels", "YTickLabel"}, {"zticklabels", "ZTickLabel"},
        {"xtickmode", "XTickMode"}, {"ytickmode", "YTickMode"}, {"ztickmode", "ZTickMode"},
        {"xticklabelmode", "XTickLabelMode"}, {"yticklabelmode", "YTickLabelMode"}, {"zticklabelmode", "ZTickLabelMode"},
        {"xscale", "XScale"}, {"yscale", "YScale"}, {"zscale", "ZScale"},
        {"xgrid", "XGrid"}, {"ygrid", "YGrid"}, {"zgrid", "ZGrid"},
        {"xlabel", "XLabel"}, {"ylabel", "YLabel"}, {"zlabel", "ZLabel"},
        {"xdir", "XDir"}, {"ydir", "YDir"}, {"zdir", "ZDir"},
        {"climmode", "CLimMode"}, {"title", "Title"},
        {"fontsize", "FontSize"}, {"fontweight", "FontWeight"}, {"fontname", "FontName"},
        {"linewidth", "LineWidth"}, {"linestyle", "LineStyle"},
        {"marker", "Marker"}, {"markersize", "MarkerSize"},
        {"color", "Color"}, {"visible", "Visible"}, {"displayname", "DisplayName"},
        {"xdata", "XData"}, {"ydata", "YData"}, {"zdata", "ZData"},
        {"sizedata", "SizeData"}, {"barwidth", "BarWidth"}, {"basevalue", "BaseValue"},
        {"binedges", "BinEdges"}, {"values", "Values"}, {"bincounts", "BinCounts"},
        {"numbins", "NumBins"}, {"binwidth", "BinWidth"}, {"binlimits", "BinLimits"},
        {"data", "Data"}, {"type", "Type"}, {"parent", "Parent"}, {"children", "Children"},
        {"number", "Number"}, {"currentaxes", "CurrentAxes"},
        {"position", "Position"}, {"outerposition", "OuterPosition"},
        {"string", "String"}, {"location", "Location"}, {"box", "Box"},
        {"units", "Units"}, {"paperunits", "PaperUnits"}, {"papersize", "PaperSize"},
        {"paperposition", "PaperPosition"}, {"papertype", "PaperType"},
        {"numbertitle", "NumberTitle"}, {"name", "Name"},
    };
    for (const auto &kv : kNames)
        if (low == kv.first)
            return kv.second;
    return raw;
}

Value recordGetProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                    const std::string &rawName, std::pmr::memory_resource *mr,
                    bool strict = false)
{
    const std::string name = canonicalPropName(rawName);
    auto str = [&](const char *s) { return Value::fromString(s, mr); };
    auto num = [&](double d) { return Value::scalar(d, mr); };
    auto rgb = [&](double r, double g, double b) {
        auto v = Value::matrix(1, 3, ValueType::DOUBLE, mr);
        v.doubleDataMut()[0] = r; v.doubleDataMut()[1] = g; v.doubleDataMut()[2] = b;
        return v;
    };
    if (rec->type == "root") {
        if (name == "Type")   return str("root");
        if (name == "Children") {
            std::vector<int> ids;
            for (const auto &[fid, fig] : fm.figures()) {
                (void)fig;
                int hid = -1;
                for (auto &[id, r] : fm.handles())
                    if (!r.deleted && r.type == "figure" && r.figureId == fid) {
                        hid = id;
                        break;
                    }
                if (hid < 0)
                    hid = fm.createHandleAt(fid, 0, "figure", kFigureClass);
                ids.push_back(hid);
            }
            return makeHandleArray(kFigureClass, ids, mr);
        }
        auto it = rec->props.find(name);
        return it != rec->props.end() ? decodeProp(it->second, mr) : Value();
    }
    if (rec->type == "text") {
        // Live String from the label slot (the render source of truth).
        if (name == "String") {
            std::string *f = textFieldOf(fm, *rec);
            return str(f ? f->c_str() : "");
        }
        if (name == "Type")       return str("text");
        if (name == "Parent") {
            // sgtitle's parent is the Figure; other slots the Axes.
            if (rec->slot == "sgtitle")
                return makeFigureHandleFor(fm, rec->figureId, mr);
            return makeAxesHandleFor(fm, rec->figureId, rec->axesIndex, mr);
        }
        auto it = rec->props.find(name);
        if (it != rec->props.end())
            return decodeProp(it->second, mr);
        // MATLAB R2025b (probe-verified): label Texts inherit the AXES
        // font â€” FontSize at 1.1Ã— â€” unless the axes font was set (which
        // RESETS any explicit Text font, see resetAxesTextFonts).
        AxesState *ax = rec->slot == "sgtitle" ? nullptr
                                               : fm.axesOf(*rec);
        if (name == "FontSize")
            return num(ax ? ax->fontSize * 1.1 : 11.0);
        if (name == "FontWeight")
            return str(ax ? ax->fontWeight.c_str() : "normal");
        if (name == "FontName")
            return str(ax ? ax->fontName.c_str() : "Helvetica");
        if (name == "Visible")    return str("on");
        if (name == "Color")      return rgb(0.15, 0.15, 0.15);
        return Value();
    }
    if (rec->type == "legend") {
        AxesState *ax = fm.axesOf(*rec);
        if (name == "String") {
            size_t n = ax ? ax->legendLabels.size() : 0;
            if (!n)
                return Value::cell(0, 0, mr);
            Value c = Value::cell(1, n, mr);
            for (size_t i = 0; i < n; ++i)
                c.cellAt(i) = Value::fromString(ax->legendLabels[i], mr);
            return c;
        }
        if (name == "Type") return str("legend");
        if (name == "Parent")
            return makeAxesHandleFor(fm, rec->figureId, rec->axesIndex, mr);
        // Live render state first â€” a stored prop would shadow later
        // command-form changes (`legend show` after set(hL,'Visible','off')).
        if (name == "Location")
            return str(ax && !ax->legendLocation.empty()
                           ? ax->legendLocation.c_str() : "northeast");
        if (name == "Box")  return str(!ax || ax->legendBoxOn ? "on" : "off");
        if (name == "Visible")
            return str(!ax || !ax->legendHidden ? "on" : "off");
        auto it = rec->props.find(name);
        if (it != rec->props.end())
            return decodeProp(it->second, mr);
        return Value();
    }
    if (rec->type == "figure") {
        if (name == "Type")   return str("figure");
        if (name == "Number") return num(rec->figureId);
        if (name == "Parent") return makeRootHandle(fm, mr);
        if (name == "CurrentAxes")
            return makeAxesHandle(fm, mr);
        if (name == "Children") {
            FigureState *fig = fm.figureOf(rec->figureId);
            const size_t n = fig ? fig->axes.size() : 0;
            std::vector<int> ids;
            for (size_t k = 0; k < n; ++k) {
                int hid = -1;
                for (auto &[id, r] : fm.handles())
                    if (!r.deleted && r.type == "axes"
                        && r.figureId == rec->figureId && r.axesIndex == k) {
                        hid = id;
                        break;
                    }
                if (hid < 0)
                    hid = fm.createHandleAt(rec->figureId, k, "axes", kAxesClass);
                ids.push_back(hid);
            }
            return makeHandleArray(kAxesClass, ids, mr);
        }
        auto it = rec->props.find(name);
        if (it != rec->props.end())
            return decodeProp(it->second, mr);
        std::string lowName = name;
        for (char &ch : lowName)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        for (const auto &[k, v] : rec->props) {
            std::string lk = k;
            for (char &ch : lk)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if (lk == lowName)
                return decodeProp(v, mr);
        }
        if (lowName == "papersize") {
            auto v = Value::matrix(1, 2, ValueType::DOUBLE, mr);
            v.doubleDataMut()[0] = 20.999998;
            v.doubleDataMut()[1] = 29.699999;
            return v;
        }
        if (lowName == "paperposition") {
            auto v = Value::matrix(1, 4, ValueType::DOUBLE, mr);
            v.doubleDataMut()[0] = 0.6345;
            v.doubleDataMut()[1] = 6.3452;
            v.doubleDataMut()[2] = 20.3046;
            v.doubleDataMut()[3] = 15.2284;
            return v;
        }
        if (lowName == "position" || lowName == "outerposition") {
            auto v = Value::matrix(1, 4, ValueType::DOUBLE, mr);
            v.doubleDataMut()[0] = 100.0;
            v.doubleDataMut()[1] = 100.0;
            v.doubleDataMut()[2] = 560.0;
            v.doubleDataMut()[3] = 420.0;
            return v;
        }
        if (lowName == "units")
            return str("pixels");
        if (lowName == "paperunits")
            return str("centimeters");
        if (lowName == "papertype")
            return str("A4");
        if (lowName == "color")
            return rgb(1.0, 1.0, 1.0);
        if (lowName == "visible")
            return str("on");
        return strict ? Value() : Value::fromString("", mr);
    }
    if (rec->type == "axes") {
        if (name == "Parent")
            return makeFigureHandleFor(fm, rec->figureId, mr);
        // Label slots return Text handles (MATLAB), not strings.
        const std::string &slot = textSlotOfProp(name);
        if (!slot.empty())
            return makeTextHandleFor(fm, rec->figureId, rec->axesIndex, slot, mr);
        Value v = axesGetProp(fm, rec, name, mr);
        if (!v.isEmpty())
            return v;
        auto it = rec->props.find(name);
        return it != rec->props.end()
                   ? decodeProp(it->second, mr)
                   : (strict ? Value() : Value::fromString("", mr));
    }
    // Chart records.
    if (name == "Parent")
        return makeAxesHandleFor(fm, rec->figureId, rec->axesIndex, mr);
    auto it = rec->props.find(name);
    if (it != rec->props.end())
        return decodeProp(it->second, mr);
    {
        Value v = chartDataProp(fm, rec, name, mr);
        if (!v.isEmpty())
            return v;
    }
    // An EXPLICIT linespec from the plot call (plot(x,y,'r--o')) is
    // user intent and reads back MATLAB-exact â€” unlike the soft series
    // palette the renderer draws. Colors map to MATLAB's table
    // ('r'==[1 0 0]), kv hex to /255.
    if (name == "Color" || name == "LineStyle" || name == "Marker") {
        DatasetInfo *ds = fm.datasetOf(*rec);
        if (ds && !ds->style.empty()) {
            const std::string &s = ds->style;
            auto rgb = [&](double r, double g, double b) {
                auto v = Value::matrix(1, 3, ValueType::DOUBLE, mr);
                v.doubleDataMut()[0] = r;
                v.doubleDataMut()[1] = g;
                v.doubleDataMut()[2] = b;
                return v;
            };
            auto strv = [&](const char *t) {
                return Value::fromString(t, mr);
            };
            if (name == "LineStyle") {
                if (s.find("--") != std::string::npos)
                    return strv("--");
                if (s.find("-.") != std::string::npos)
                    return strv("-.");
                if (s.find(':') != std::string::npos)
                    return strv(":");
            } else if (name == "Marker") {
                static const char *kMarkers = "o+*.xsd^v<>ph";
                for (char mc : s)
                    if (mc && std::strchr(kMarkers, mc))
                        return strv(std::string(1, mc).c_str());
            } else {
                struct ColEntry { char c; double rgb[3]; };
                static const ColEntry kCol[] = {
                    {'r', {1, 0, 0}},       {'g', {0, 0.5, 0}},
                    {'b', {0, 0, 1}},       {'k', {0, 0, 0}},
                    {'w', {1, 1, 1}},       {'y', {1, 1, 0}},
                    {'m', {1, 0, 1}},       {'c', {0, 1, 1}},
                };
                size_t hp = s.find("color=#");
                if (hp != std::string::npos) {
                    unsigned R = 0, G = 0, B = 0;
                    if (std::sscanf(s.c_str() + hp + 7, "%02x%02x%02x", &R,
                                    &G, &B)
                        == 3)
                        return rgb(R / 255.0, G / 255.0, B / 255.0);
                } else {
                    // kv color= segment or a bare linespec letter; the
                    // single-letter form must not collide with markers
                    // first (handled above), so scan for a colour char.
                    size_t cp = s.find("color=");
                    std::string cand;
                    if (cp != std::string::npos) {
                        size_t e = s.find(';', cp);
                        cand = s.substr(cp + 6,
                                        e == std::string::npos
                                            ? std::string::npos
                                            : e - cp - 6);
                    } else if (s.size() == 1) {
                        cand = s;
                    } else {
                        for (char ch : s)
                            if (std::strchr("rgbcymwkv", ch)) {
                                cand = std::string(1, ch);
                                break;
                            }
                    }
                    if (cand.size() == 1) {
                        for (const auto &cc : kCol)
                            if (cand[0] == cc.c)
                                return rgb(cc.rgb[0], cc.rgb[1],
                                           cc.rgb[2]);
                    }
                    if (cand.size() > 1 && cand[0] == '#'
                        && cand.size() >= 7) {
                        unsigned R = 0, G = 0, B = 0;
                        if (std::sscanf(cand.c_str() + 1, "%02x%02x%02x",
                                        &R, &G, &B)
                            == 3)
                            return rgb(R / 255.0, G / 255.0, B / 255.0);
                    }
                }
            }
        }
    }
    Value d = chartDefaultProp(rec->type, name, mr);
    if (!d.isEmpty())
        return d;
    return strict ? Value() : Value::fromString("", mr);
}

// Returns true when the property is known/accepted (stored at minimum).
bool recordSetProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                   const std::string &rawName, const Value &val)
{
    const std::string name = canonicalPropName(rawName);
    if (name == "Parent" || val.isObject())
        return true;
    if (name == "XData" || name == "YData") {
        rec->props.erase(name);
    } else if (!(val.isNumeric() && val.numel() > 1024 && name != "ZData")) {
        rec->props[name] = encodeProp(val);
    }
    if (rec->type == "text") {
        if (name == "String") {
            std::string *f = textFieldOf(fm, *rec);
            if (f) {
                *f = (val.isChar() || val.isString()) ? val.toString() : "";
                fm.markFigureModified(rec->figureId);
            }
        }
        return true;   // FontSize/Color/â€¦ store-only (renderer gap filed)
    }
    if (rec->type == "legend") {
        AxesState *ax = fm.axesOf(*rec);
        if (!ax)
            return true;
        if (name == "Location") {
            ax->legendLocation = val.toString();
            fm.markFigureModified(rec->figureId);
        } else if (name == "Box") {
            ax->legendBoxOn = val.toString() != "off";
            fm.markFigureModified(rec->figureId);
        } else if (name == "Visible") {
            ax->legendHidden = val.toString() == "off";
            fm.markFigureModified(rec->figureId);
        } else if (name == "String" && val.isCell()) {
            ax->legendLabels.clear();
            for (size_t i = 0; i < val.numel(); ++i)
                ax->legendLabels.push_back(val.cellAt(i).toString());
            fm.markFigureModified(rec->figureId);
        }
        return true;
    }
    if (rec->type == "root")
        return true;   // root properties are read-only at this layer
    if (rec->type == "axes")
        applyAxesProp(fm, rec, name, val);
    else
        applyChartProp(fm, rec, name, val);
    return true;
}

} // anonymous namespace

// ============================================================
// Handle-aware accessor registration (set/get/isgraphics/ishandle/
// isvalid â€” delete lives in general_reg.cpp, type-dispatched)
// ============================================================
namespace {

void registerHandleAccessors(Engine &engine)
{
    auto settableCatalogFor = [](const std::string &type)
        -> std::map<std::string, std::vector<std::string>> {
        if (type == "figure") {
            return {
                {"Color", {}},
                {"Name", {}},
                {"NumberTitle", {"on", "off"}},
                {"Position", {}},
                {"Units", {"pixels", "normalized", "inches", "centimeters", "points"}},
                {"Visible", {"on", "off"}},
            };
        }
        if (type == "axes") {
            return {
                {"Box", {"on", "off"}},
                {"CLim", {}},
                {"CLimMode", {"auto", "manual"}},
                {"FontName", {}},
                {"FontSize", {}},
                {"FontWeight", {"normal", "bold"}},
                {"LineWidth", {}},
                {"Title", {}},
                {"Visible", {"on", "off"}},
                {"XDir", {"normal", "reverse"}},
                {"XGrid", {"on", "off"}},
                {"XLabel", {}},
                {"XLim", {}},
                {"XScale", {"linear", "log"}},
                {"XTick", {}},
                {"XTickLabel", {}},
                {"XTickLabelMode", {"auto", "manual"}},
                {"XTickMode", {"auto", "manual"}},
                {"YDir", {"normal", "reverse"}},
                {"YGrid", {"on", "off"}},
                {"YLabel", {}},
                {"YLim", {}},
                {"YScale", {"linear", "log"}},
                {"YTick", {}},
                {"YTickLabel", {}},
                {"YTickLabelMode", {"auto", "manual"}},
                {"YTickMode", {"auto", "manual"}},
                {"ZLabel", {}},
                {"ZLim", {}},
                {"ZTick", {}},
            };
        }
        if (type == "text") {
            return {
                {"Color", {}},
                {"FontName", {}},
                {"FontSize", {}},
                {"FontWeight", {"normal", "bold"}},
                {"String", {}},
                {"Visible", {"on", "off"}},
            };
        }
        if (type == "legend") {
            return {
                {"Box", {"on", "off"}},
                {"Location", {"north", "south", "east", "west", "northeast",
                              "northwest", "southeast", "southwest", "best"}},
                {"String", {}},
                {"Visible", {"on", "off"}},
            };
        }
        if (type == "root") {
            return {};
        }
        // Chart records (line, scatter, bar, surface, contour, ...).
        std::map<std::string, std::vector<std::string>> m = {
            {"Color", {}},
            {"DisplayName", {}},
            {"LineStyle", {"-", "--", ":", "-.", "none"}},
            {"LineWidth", {}},
            {"Marker", {"o", "+", "*", ".", "x", "square", "diamond",
                        "^", "v", ">", "<", "pentagram", "hexagram", "none"}},
            {"MarkerSize", {}},
            {"Visible", {"on", "off"}},
            {"XData", {}},
            {"YData", {}},
            {"ZData", {}},
        };
        if (type == "scatter")
            m["SizeData"] = {};
        if (type == "bar") {
            m["BarWidth"] = {};
            m["BaseValue"] = {};
        }
        if (type == "stem" || type == "area")
            m["BaseValue"] = {};
        return m;
    };

    // set(h, ...)
    {
        ExternalFunc fn = [&engine, settableCatalogFor](
                Span<const Value> args, size_t nargout,
                Span<Value> outs, CallContext &ctx) {
            if (args.empty()) { outs[0] = Value(); return; }
            const int id = graphicsHandleIdOf(args[0]);
            if (id < 0) { outs[0] = Value(); return; }
            auto *rec = engine.figureManager().findHandle(id);
            if (!rec)
                throw std::runtime_error("Invalid or deleted object.");
            auto *mr = ctx.engine->resource();
            if (args.size() == 1) {
                const auto cat = settableCatalogFor(rec->type);
                if (nargout == 0) {
                    std::string text;
                    for (const auto &[k, choices] : cat) {
                        text += "    " + k;
                        if (!choices.empty()) {
                            text += ": [ ";
                            for (size_t i = 0; i < choices.size(); ++i) {
                                if (i) text += " | ";
                                if (i == 0) text += "{" + choices[i] + "}";
                                else text += choices[i];
                            }
                            text += " ]";
                        }
                        text += "\n";
                    }
                    ctx.engine->outputText(text);
                    outs[0] = Value();
                    return;
                }
                Value st = Value::structure(mr);
                for (const auto &[k, choices] : cat) {
                    if (choices.empty()) {
                        st.setFieldAll(k, Value::cell(1, 0, mr));
                    } else {
                        Value c = Value::cell(choices.size(), 1, mr);
                        for (size_t i = 0; i < choices.size(); ++i)
                            c.cellAt(i) = Value::fromString(choices[i], mr);
                        st.setFieldAll(k, std::move(c));
                    }
                }
                outs[0] = std::move(st);
                return;
            }
            if (args.size() == 2 && (args[1].isChar() || args[1].isString())) {
                const std::string prop = args[1].toString();
                const auto cat = settableCatalogFor(rec->type);
                auto it = cat.find(prop);
                const std::vector<std::string> emptyChoices;
                const auto &choices = (it != cat.end()) ? it->second : emptyChoices;
                if (nargout == 0) {
                    std::string text;
                    if (!choices.empty()) {
                        text += "[ ";
                        for (size_t i = 0; i < choices.size(); ++i) {
                            if (i) text += " | ";
                            if (i == 0) text += "{" + choices[i] + "}";
                            else text += choices[i];
                        }
                        text += " ]\n";
                    } else {
                        text += "Property does not have a fixed set of property values.\n";
                    }
                    ctx.engine->outputText(text);
                    outs[0] = Value();
                    return;
                }
                if (choices.empty()) {
                    outs[0] = Value::cell(1, 0, mr);
                } else {
                    Value c = Value::cell(choices.size(), 1, mr);
                    for (size_t i = 0; i < choices.size(); ++i)
                        c.cellAt(i) = Value::fromString(choices[i], mr);
                    outs[0] = std::move(c);
                }
                return;
            }
            if (args[0].isObject() && args[0].objectCount() > 1) {
                for (size_t k = 0; k < args[0].objectCount(); ++k) {
                    const ObjectState *st = args[0].objectStateAt(k);
                    if (!st || !st->native) continue;
                    auto *p = dynamic_cast<const GraphicsHandlePayload *>(st->native.get());
                    if (!p) continue;
                    auto *r = engine.figureManager().findHandle(p->handleId);
                    if (!r) continue;
                    for (size_t i = 1; i + 1 < args.size(); i += 2) {
                        if (!args[i].isChar() && !args[i].isString()) continue;
                        recordSetProp(engine.figureManager(), r,
                                      args[i].toString(), args[i + 1]);
                    }
                }
            } else {
                for (size_t i = 1; i + 1 < args.size(); i += 2) {
                    if (!args[i].isChar() && !args[i].isString()) continue;
                    recordSetProp(engine.figureManager(), rec,
                                  args[i].toString(), args[i + 1]);
                }
            }
            engine.figureManager().emitModified();
            outs[0] = Value();
        };
        engine.registerFunction("", "set", std::move(fn));
    }
    // get(h [, 'Prop']) — without a property name: struct of all
    {
        ExternalFunc fn = [&engine](
                Span<const Value> args, size_t,
                Span<Value> outs, CallContext &) {
            if (args.empty()) { outs[0] = Value(); return; }
            const int id = graphicsHandleIdOf(args[0]);
            if (id < 0) {
                if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
                    std::string prop = args[1].toString();
                    std::string lprop = prop;
                    for (char &ch : lprop)
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (lprop == "pointerlocation") {
                        Value v = Value::matrix(1, 2, ValueType::DOUBLE, engine.resource());
                        v.doubleDataMut()[0] = 0.0; v.doubleDataMut()[1] = 0.0;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop == "position" || lprop == "outerposition" || lprop == "screensize") {
                        Value v = Value::matrix(1, 4, ValueType::DOUBLE, engine.resource());
                        v.doubleDataMut()[0] = 0.13; v.doubleDataMut()[1] = 0.11;
                        v.doubleDataMut()[2] = 0.775; v.doubleDataMut()[3] = 0.815;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop == "nextplot") {
                        outs[0] = Value::fromString("replace", engine.resource());
                        return;
                    }
                    if (lprop == "xcolor" || lprop == "ycolor" || lprop == "zcolor" || lprop == "color") {
                        outs[0] = Value::fromString("k", engine.resource());
                        return;
                    }
                    if (lprop == "xlim" || lprop == "ylim" || lprop == "zlim" || lprop == "clim") {
                        Value v = Value::matrix(1, 2, ValueType::DOUBLE, engine.resource());
                        v.doubleDataMut()[0] = 0.0; v.doubleDataMut()[1] = 1.0;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop == "xtick" || lprop == "ytick" || lprop == "ztick") {
                        Value v = Value::matrix(1, 3, ValueType::DOUBLE, engine.resource());
                        v.doubleDataMut()[0] = 0.0; v.doubleDataMut()[1] = 0.5; v.doubleDataMut()[2] = 1.0;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop.find("fontangle") != std::string::npos || lprop.find("fontweight") != std::string::npos) {
                        outs[0] = Value::fromString("normal", engine.resource());
                        return;
                    }
                    if (lprop.find("fontname") != std::string::npos) {
                        outs[0] = Value::fromString("Helvetica", engine.resource());
                        return;
                    }
                    if (lprop.find("fontsize") != std::string::npos) {
                        outs[0] = Value::scalar(10.0, engine.resource());
                        return;
                    }
                }
                outs[0] = Value::scalar(1.0, engine.resource());
                return;
            }
            auto *rec = engine.figureManager().findHandle(id);
            if (!rec)
                throw std::runtime_error("Invalid or deleted object.");
            if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
                outs[0] = recordGetProp(engine.figureManager(), rec,
                                        args[1].toString(), engine.resource());
                return;
            }
            // get(h) → struct: stored props ∪ documented defaults.
            auto *mr = engine.resource();
            Value st = Value::structure(mr);
            std::map<std::string, std::string> fields(rec->props);
            static const char *kCommon[] = {
                "Type", "Color", "LineStyle", "LineWidth", "Marker",
                "MarkerSize", "Visible", "Parent", "Children",
            };
            for (const char *p : kCommon) {
                if (fields.count(p)) continue;
                Value d = rec->isChart() ? chartDefaultProp(rec->type, p, mr)
                                         : Value();
                if (!d.isEmpty())
                    fields[p] = encodeProp(d);
            }
            if (rec->isChart()) {
                for (const char *dp : {"XData", "YData", "ZData"}) {
                    if (fields.count(dp)) continue;
                    Value dv = chartDataProp(engine.figureManager(), rec, dp, mr);
                    if (!dv.isEmpty())
                        fields[dp] = encodeProp(dv);
                }
            }
            if (rec->type == "figure") {
                fields["Number"] = std::to_string(rec->figureId);
                fields["Type"] = "figure";
            } else if (rec->type == "axes") {
                fields["Type"] = "axes";
                static const char *kLayout[] = {
                    "XLim", "YLim", "XScale", "YScale", "XLabel", "YLabel",
                    "Title",
                };
                for (const char *p : kLayout) {
                    Value v = axesGetProp(engine.figureManager(), rec, p, mr);
                    if (!v.isEmpty())
                        fields[p] = encodeProp(v);
                }
            } else if (rec->type == "text") {
                fields["Type"] = "text";
                Value v = recordGetProp(engine.figureManager(), rec, "String", mr);
                if (!v.isEmpty())
                    fields["String"] = encodeProp(v);
            } else if (rec->type == "legend") {
                fields["Type"] = "legend";
                fields["Location"] = encodeProp(
                    recordGetProp(engine.figureManager(), rec, "Location", mr));
                fields["Visible"] = encodeProp(
                    recordGetProp(engine.figureManager(), rec, "Visible", mr));
            }
            for (const auto &[k, v] : fields)
                st.setFieldAll(k, decodeProp(v, mr));
            outs[0] = std::move(st);
        };
        engine.registerFunction("", "get", std::move(fn));
    }
    // findobj([h][, 'Prop', value, ...] | '-property', Name)
    {
        ExternalFunc fn = [&engine](
                Span<const Value> args, size_t, Span<Value> outs,
                CallContext &ctx) {
            auto &fm = ctx.engine->figureManager();
            auto *mr = ctx.engine->resource();

            // Scope: an optional leading handle narrows the search.
            int scopeFig = -1;      // -1 = root scope
            int scopeAxes = -1;
            int scopeSelf = -1;      // chart-handle self-scope
            size_t first = 0;
            if (!args.empty()) {
                int hid = graphicsHandleIdOf(args[0]);
                if (hid >= 0) {
                    auto *rec = fm.findHandle(hid);
                    if (rec) {
                        if (rec->type == "figure")
                            scopeFig = rec->figureId;
                        else if (rec->type == "axes") {
                            scopeFig = rec->figureId;
                            scopeAxes = (int)rec->axesIndex;
                        } else {
                            // chart/text/legend handle: returns
                            // the handle itself (no children modeled).
                            if (!args.empty() && args.size() > 1
                                && args[1].isChar()) {
                                // has filters — evaluate them on itself
                                scopeFig = rec->figureId;
                                scopeAxes = (int)rec->axesIndex;
                                scopeSelf = hid;
                            } else {
                                outs[0] = args[0];
                                return;
                            }
                        }
                        first = 1;
                    }
                }
            }

            // Filters.
            struct Filt {
                std::string prop;   // "Type" or a property name
                Value want;
                bool isPropTest;    // '-property' Name
            };
            std::vector<Filt> filts;
            for (size_t i = first; i < args.size(); ++i) {
                if (!args[i].isChar())
                    continue;
                std::string key = args[i].toString();
                for (auto &c : key)
                    c = (char)std::tolower((unsigned char)c);
                if (key == "-property" && i + 1 < args.size()
                    && args[i + 1].isChar()) {
                    Filt f;
                    f.isPropTest = true;
                    f.prop = args[++i].toString();
                    filts.push_back(std::move(f));
                } else if (key == "-regexp" || key == "-depth") {
                    // skip their option payload; unsupported, ignored
                    ++i;
                } else if (i + 1 < args.size()) {
                    Filt f;
                    f.isPropTest = false;
                    f.prop = args[i].toString();
                    f.want = args[i + 1];
                    ++i;
                    filts.push_back(std::move(f));
                }
            }

            auto matlabType = [](const std::string &t) {
                if (t == "figure" || t == "axes" || t == "text"
                    || t == "line" || t == "legend" || t == "image")
                    return t;
                // chart records carry dataset-ish types (scatter, bar…)
                return std::string("line");
            };

            std::vector<int> hits;
            for (auto &kv : fm.handles()) {
                auto *rec = &kv.second;
                if (!rec || rec->deleted)
                    continue;
                // Hide slot-bound title/label text objects from findobj;
                // user text() stays findable.
                if (rec->type == "text" && !rec->slot.empty())
                    continue;
                if (scopeFig >= 0 && rec->figureId != scopeFig)
                    continue;
                if (scopeSelf >= 0) {
                    if (kv.first != scopeSelf)
                        continue;
                } else if (scopeAxes >= 0
                           && rec->axesIndex != (size_t)scopeAxes
                           && !(rec->type == "figure")) {
                    continue;
                }
                bool pass = true;
                for (const auto &f : filts) {
                    if (f.isPropTest) {
                        Value v = recordGetProp(fm, rec, f.prop, mr);
                        if (v.isUnset() || v.isEmpty()) {
                            pass = false;
                            break;
                        }
                        continue;
                    }
                    if (f.prop == "Type") {
                        if (!f.want.isChar()
                            || matlabType(rec->type)
                                   != f.want.toString()) {
                            pass = false;
                            break;
                        }
                        continue;
                    }
                    Value v = recordGetProp(fm, rec, f.prop, mr);
                    if (v.isUnset() || v.isEmpty()) {
                        pass = false;
                        break;
                    }
                    // Colour shorthand: compares findobj('Color','r')
                    // against the RGB triplet [1 0 0].
                    if (f.want.isChar() && !v.isChar()
                        && v.numel() == 3
                        && (f.prop == "Color" || f.prop == "EdgeColor"
                            || f.prop == "FaceColor")) {
                        struct ColEntry { const char *n; double rgb[3]; };
                        static const ColEntry kCol[] = {
                            {"r", {1, 0, 0}},     {"g", {0, 0.5, 0}},
                            {"b", {0, 0, 1}},     {"k", {0, 0, 0}},
                            {"w", {1, 1, 1}},     {"y", {1, 1, 0}},
                            {"m", {1, 0, 1}},     {"c", {0, 1, 1}},
                            {"red", {1, 0, 0}},   {"green", {0, 0.5, 0}},
                            {"blue", {0, 0, 1}},  {"black", {0, 0, 0}},
                            {"white", {1, 1, 1}},
                        };
                        std::string want = f.want.toString();
                        bool matched = false;
                        for (const auto &cc : kCol) {
                            if (want != cc.n)
                                continue;
                            matched = std::abs(v.elemAsDouble(0) - cc.rgb[0]) < 1e-9
                                   && std::abs(v.elemAsDouble(1) - cc.rgb[1]) < 1e-9
                                   && std::abs(v.elemAsDouble(2) - cc.rgb[2]) < 1e-9;
                            break;
                        }
                        if (!matched) {
                            pass = false;
                            break;
                        }
                        continue;
                    }
                    if (v.isChar() != f.want.isChar()) {
                        pass = false;
                        break;
                    }
                    if (v.isChar()) {
                        if (v.toString() != f.want.toString()) {
                            pass = false;
                            break;
                        }
                    } else {
                        if (v.numel() != f.want.numel()) {
                            pass = false;
                            break;
                        }
                        bool near = true;
                        for (size_t k = 0; k < v.numel(); ++k)
                            if (std::abs(v.elemAsDouble(k)
                                         - f.want.elemAsDouble(k))
                                > 1e-9)
                                near = false;
                        if (!near) {
                            pass = false;
                            break;
                        }
                    }
                }
                if (pass)
                    hits.push_back(kv.first);   // registry key = handle id
            }
            std::sort(hits.begin(), hits.end());   // creation order
            std::string cls = hits.empty()
                ? std::string()
                : fm.findHandle(hits[0])->className;
            outs[0] = makeHandleArray(cls, hits, mr);
            (void)engine;
        };
        engine.registerFunction("", "findobj", fn);
        engine.registerFunction("", "findall", std::move(fn));
    }
    // isgraphics(h [, type])
    {
        ExternalFunc fn = [&engine](
                Span<const Value> args, size_t,
                Span<Value> outs, CallContext &) {
            if (args.empty()) { outs[0] = Value(); return; }
            auto &fm = engine.figureManager();
            const int id = graphicsHandleIdOf(args[0]);
            auto *rec = id >= 0 ? fm.findHandle(id) : nullptr;
            bool ok = rec != nullptr;
            if (ok && args.size() >= 2
                && (args[1].isChar() || args[1].isString())) {
                const std::string want = args[1].toString();
                ok = rec->type == want || want == "chart"
                     || want == "graphics" || want == "axes"
                     || want == "figure";
            }
            outs[0] = Value::logicalScalar(ok);
        };
        engine.registerFunction("", "isgraphics", std::move(fn));
    }
    // groot — the root graphics object (figure.Parent).
    {
        ExternalFunc fn = [&engine](
                Span<const Value>, size_t,
                Span<Value> outs, CallContext &) {
            outs[0] = makeRootHandle(engine.figureManager(),
                                     engine.resource());
        };
        engine.registerFunction("", "groot", std::move(fn));
    }
    // gobjects([m, n, ...]) ? array of placeholder graphics objects.
    {
        ExternalFunc fn = [](Span<const Value> args, size_t,
                             Span<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->resource();
            size_t r = 1, c = 1;
            if (args.size() == 1 && args[0].isNumeric()) {
                if (args[0].numel() == 1) {
                    r = c = static_cast<size_t>(std::max(0.0, args[0].toScalar()));
                } else if (args[0].numel() >= 2) {
                    r = static_cast<size_t>(std::max(0.0, args[0].elemAsDouble(0)));
                    c = static_cast<size_t>(std::max(0.0, args[0].elemAsDouble(1)));
                }
            } else if (args.size() >= 2 && args[0].isNumeric() && args[1].isNumeric()) {
                r = static_cast<size_t>(std::max(0.0, args[0].toScalar()));
                c = static_cast<size_t>(std::max(0.0, args[1].toScalar()));
            }
            size_t n = r * c;
            std::vector<std::shared_ptr<ObjectState>> states(n);
            for (size_t i = 0; i < n; ++i) {
                auto payload = std::make_shared<GraphicsHandlePayload>();
                payload->handleId = 0;
                auto st = std::make_shared<ObjectState>(mr);
                st->native = std::move(payload);
                states[i] = std::move(st);
            }
            outs[0] = Value::objectArray("matlab.graphics.GraphicsPlaceholder",
                                         Dims(r, c), std::move(states),
                                         /*isHandle=*/true, mr);
        };
        engine.registerFunction("", "gobjects", std::move(fn));
    }
    // ishandle(h) / isvalid(h)
    {
        ExternalFunc fn = [&engine](
                Span<const Value> args, size_t,
                Span<Value> outs, CallContext &) {
            if (args.empty()) { outs[0] = Value(); return; }
            auto &fm = engine.figureManager();
            const int id = graphicsHandleIdOf(args[0]);
            outs[0] = Value::logicalScalar(
                id >= 0 && fm.findHandle(id) != nullptr);
        };
        engine.registerFunction("", "ishandle", fn);
        engine.registerFunction("", "isvalid", std::move(fn));
    }
}

// ============================================================
// BuiltinClass per graphics class — construct + dot-syntax + isa + eq
// ============================================================
void registerGraphicsClasses(Engine &engine)
{
    // Distinct class names across the chart table + axes + figure
    // + text/legend/root records, plus class -> internal type mapping.
    std::unordered_map<std::string, std::string> classToType = {
        {kFigureClass, "figure"},
        {kAxesClass,   "axes"},
        {kTextClass,   "text"},
        {kLegendClass, "legend"},
        {kRootClass,   "root"},
        {"matlab.graphics.Graphics", "line"},
        {"matlab.graphics.GraphicsPlaceholder", "line"},
    };
    for (const auto &kv : handleTypes())
        classToType.emplace(kv.second.second, kv.second.first);

    for (const auto &[clsName, recType] : classToType) {
        BuiltinClass cls;
        cls.name = clsName;
        cls.isHandle = true;
        cls.superclasses = {"matlab.mixin.Handle", "handle"};
        cls.construct = [&engine, clsName, recType](
                Span<const Value> args, CallContext &ctx) -> Value {
            auto &fm = ctx.engine->figureManager();
            auto *mr = ctx.engine->resource();
            Value h;
            if (recType == "figure") {
                h = makeFigureHandle(fm, mr);
            } else if (recType == "axes") {
                h = makeAxesHandle(fm, mr);
            } else if (recType == "text") {
                const int fid = fm.currentFigureId();
                const size_t aidx = static_cast<size_t>(fm.current().currentAxes);
                const int id = fm.createHandleAt(fid, aidx, "text", kTextClass);
                h = makeGraphicsHandleValue(kTextClass, id, mr);
            } else if (recType == "legend") {
                const int fid = fm.currentFigureId();
                const size_t aidx = static_cast<size_t>(fm.current().currentAxes);
                h = makeLegendHandleFor(fm, fid, aidx, mr);
            } else if (recType == "root") {
                h = makeRootHandle(fm, mr);
            } else {
                AxesState &ax = fm.currentAxes();
                const size_t dsIdx = ax.datasets.size();
                DatasetInfo ds;
                if (recType == "surface") {
                    ds.type = "surf";
                    ds.xJson = "[1,2,3]";
                    ds.yJson = "[1,2,3]";
                    ds.zJson = "[[1,0,0],[0,1,0],[0,0,1]]";
                } else {
                    ds.type = recType;
                    ds.xJson = "[0,1]";
                    ds.yJson = "[0,1]";
                }
                ax.datasets.push_back(std::move(ds));
                const int id = fm.createHandle(recType, clsName, dsIdx);
                h = makeGraphicsHandleValue(clsName, id, mr);
            }
            const int hid = graphicsHandleIdOf(h);
            if (auto *rec = fm.findHandle(hid)) {
                for (size_t i = 0; i + 1 < args.size(); i += 2) {
                    if (args[i].isChar() || args[i].isString())
                        recordSetProp(fm, rec, args[i].toString(), args[i + 1]);
                }
            }
            fm.emitModified();
            (void)engine;
            return h;
        };
        // propGet / propSet mirror set()/get() exactly — one shared body
        // (recordGetProp / recordSetProp), same registry, same defaults,
        // same render-effect.
        cls.propGet = [&engine](const Value &self, const std::string &name,
                                Value &out, CallContext &) -> bool {
            const int id = graphicsHandleIdOf(self);
            if (id < 0)
                return false;
            auto *rec = engine.figureManager().findHandle(id);
            if (!rec)
                throw std::runtime_error("Invalid or deleted object.");
            Value v = recordGetProp(engine.figureManager(), rec, name,
                                    engine.resource(), /*strict=*/true);
            if (v.isEmpty())
                return false;   // unknown prop → engine's error
            out = std::move(v);
            return true;
        };
        cls.propSet = [&engine](Value &self, const std::string &name,
                                const Value &val, CallContext &) -> bool {
            const int id = graphicsHandleIdOf(self);
            if (id < 0)
                return false;
            auto *rec = engine.figureManager().findHandle(id);
            if (!rec)
                throw std::runtime_error("Invalid or deleted object.");
            recordSetProp(engine.figureManager(), rec, name, val);
            engine.figureManager().emitModified();
            return true;
        };
        // Handle identity comparison (h1 == h2 ⇔ same registry record).
        // args carries (lhs, rhs) in source order — self is a copy of the
        // dominant operand, NOT an extra argument.
        cls.ops["eq"] = [](Value &, Span<const Value> args, size_t,
                           Span<Value> outs, CallContext &) {
            bool eq = args.size() >= 2
                      && graphicsHandleIdOf(args[0]) >= 0
                      && graphicsHandleIdOf(args[0]) == graphicsHandleIdOf(args[1]);
            outs[0] = Value::logicalScalar(eq);
        };
        cls.ops["ne"] = [](Value &, Span<const Value> args, size_t,
                           Span<Value> outs, CallContext &) {
            bool eq = args.size() >= 2
                      && graphicsHandleIdOf(args[0]) >= 0
                      && graphicsHandleIdOf(args[0]) == graphicsHandleIdOf(args[1]);
            outs[0] = Value::logicalScalar(!eq);
        };
        cls.dispText = [&engine](const Value &self) -> std::string {
            const int id = graphicsHandleIdOf(self);
            auto *rec = id >= 0 ? engine.figureManager().findHandle(id) : nullptr;
            if (!rec)
                return "  deleted handle\n";
            const std::string shortName = rec->className.substr(
                rec->className.find_last_of('.') + 1);
            std::string s = "  " + shortName + " with properties:\n\n";
            auto line = [&](const char *k, const std::string &v) {
                s += std::string("    ") + k + ": " + v + "\n";
            };
            for (const char *p : {"Color", "LineStyle", "LineWidth",
                                  "Marker", "Visible"}) {
                auto it = rec->props.find(p);
                if (it != rec->props.end()) {
                    line(p, it->second);
                    continue;
                }
                Value d = rec->isChart()
                              ? chartDefaultProp(rec->type, p, engine.resource())
                              : Value();
                if (!d.isEmpty())
                    line(p, encodeProp(d));
            }
            return s;
        };
        engine.registerClass(std::move(cls));
    }
}

} // namespace

// ============================================================
// Install hub
// ============================================================
void GraphicsLibrary::install(Engine &engine)
{
    registerHandleAccessors(engine);
    registerGraphicsClasses(engine);
    registerFigureExport(engine);

    std::vector<PlotEntry> table;
    buildPlotTable(table);

    // Handle-accessor names registered above — skip their (noop)
    // table entries (registerFunctionImpl_ throws on duplicates).
    static const std::unordered_set<std::string> kHandleAccessorNames = {
        "set", "get", "isgraphics", "ishandle", "isvalid",
    };
    // Layout functions whose RESULT the adapter wraps into a handle
    // object (the core-free body only prepares the state / label field).
    static const std::unordered_set<std::string> kHandleReturning = {
        "figure", "gcf", "gca", "axes", "polaraxes",
        "title", "subtitle", "sgtitle", "xlabel", "ylabel", "zlabel",
        "legend", "text",
    };

    for (PlotEntry &entry : table) {
        if (kHandleAccessorNames.count(entry.name))
            continue;
        const std::string name(entry.name);
        const bool hasHandle = handleTypes().count(name) > 0;
        const bool wrapHandle = kHandleReturning.count(name) > 0;

        // The core-free plotting body; moved into the adapter closure.
        ExternalFunc ext =
            [fn = std::move(entry.fn), hasHandle, wrapHandle, name](
                Span<const Value> args, std::size_t nargout,
                Span<Value> outs, CallContext &ctx) {
                auto &fm = ctx.engine->figureManager();
                // Chart calls only: snapshot the handle base. Calling this
                // unconditionally would materialise a default figure before
                // `figure`'s own body runs — and the first figure() would
                // then number itself 2 instead of 1 (regression caught by
                // the handle smokes).
                if (hasHandle)
                    fm.beginChartCall();

                // Per-call bridge: bind the figure session + scratch arena, and
                // close the two escape hatches over the live ctx (engine + the
                // name-resolution env).
                GraphicsContext gc{
                    fm,
                    ctx.engine->resource(),
                    // callBuiltin — forward to another registered plot builtin.
                    [&ctx](std::string_view nm, Span<const Value> a,
                           std::size_t no, Span<Value> o) -> bool {
                        const ExternalFunc *cf =
                            ctx.engine->findExternal(std::string(nm), ctx.env);
                        if (!cf)
                            return false;
                        (*cf)(a, no, o, ctx);
                        return true;
                    },
                    // callHandle — evaluate a user function-handle value.
                    [&ctx](const Value &fh, Span<const Value> a) -> Value {
                        return ctx.engine->callFunctionHandle(fh, a);
                    },
                };
                fn(args, nargout, outs, gc);

                // Post-hook: bind the handle object(s) so h = plot(...)
                // works (bugs/closed/graphics/plot-family-no-return-value.md).
                // Statement calls get the handle in `ans`, like the reference.
                if (hasHandle) {
                    const size_t base = fm.chartCallBase();
                    const size_t after = fm.currentAxes().datasets.size();
                    if (after > base) {
                        const auto &types = handleTypes();
                        auto hit = types.find(name);
                        const bool isSurfOrContour =
                            (hit->second.first == "surface"
                             || hit->second.first == "contour");
                        const int nnew = isSurfOrContour
                                             ? 1
                                             : static_cast<int>(after - base);
                        if (nnew == 1) {
                            const int id = fm.createHandle(hit->second.first,
                                                           hit->second.second,
                                                           base);
                            if (isSurfOrContour) {
                                const Value *zArg = nullptr;
                                if (args.size() == 1 || args.size() == 2) {
                                    zArg = &args[0];
                                } else if (args.size() >= 3) {
                                    zArg = (args[2].isNumeric()
                                            && args[2].dims().rows() >= 2
                                            && args[2].dims().cols() >= 2)
                                               ? &args[2]
                                               : &args[0];
                                                                }
                                if (zArg && zArg->isNumeric()
                                    && zArg->dims().rows() >= 1
                                    && zArg->dims().cols() >= 1) {
                                    if (auto *rec = fm.findHandle(id)) {
                                        DatasetInfo *ds = fm.datasetOf(*rec);
                                        if (!ds || ds->zJson.size() < 2
                                            || ds->zJson[0] != '['
                                            || ds->zJson[1] != '[') {
                                            rec->props["ZData"] = encodeProp(*zArg);
                                        }
                                    }
                                }
                            }
                            outs[0] = makeGraphicsHandleValue(
                                hit->second.second, id, ctx.engine->resource());
                        } else {
                            std::vector<std::shared_ptr<ObjectState>> states;
                            for (int i = 0; i < nnew; ++i) {
                                const int id = fm.createHandle(
                                    hit->second.first, hit->second.second,
                                    base + static_cast<size_t>(i));
                                auto payload = std::make_shared<GraphicsHandlePayload>();
                                payload->handleId = id;
                                auto st = std::make_shared<ObjectState>(
                                    ctx.engine->resource());
                                st->native = std::move(payload);
                                states.push_back(std::move(st));
                            }
                            Dims d{1, static_cast<size_t>(nnew)};
                            outs[0] = Value::objectArray(
                                hit->second.second, d, std::move(states),
                                /*isHandle=*/true, ctx.engine->resource());
                        }
                    }
                } else if (wrapHandle) {
                    // figure/gcf → Figure, gca/axes → Axes, the title/
                    // label family → slot-bound Text, legend → Legend.
                    auto *mr = ctx.engine->resource();
                    if (name == "figure" || name == "gcf") {
                        outs[0] = makeFigureHandle(fm, mr);
                        if (name == "figure" && args.size() >= 2 && (args[0].isChar() || args[0].isString())) {
                            int hid = graphicsHandleIdOf(outs[0]);
                            if (auto *rec = fm.findHandle(hid)) {
                                for (size_t i = 0; i + 1 < args.size(); i += 2) {
                                    if (args[i].isChar() || args[i].isString())
                                        recordSetProp(fm, rec, args[i].toString(), args[i + 1]);
                                }
                            }
                        }
                    } else if (const std::string &slot = textSlotOf(name);
                               !slot.empty()) {
                        // sgtitle is figure-scoped (kNoAxes) so scoped
                        // axes sweeps keep it; the rest bind the axes.
                        outs[0] =
                            (name == "sgtitle")
                                ? makeTextHandleFor(
                                      fm, fm.currentFigureId(),
                                      FigureManager::HandleRecord::kNoAxes,
                                      slot, mr)
                                : makeTextHandleFor(
                                      fm, fm.currentFigureId(),
                                      static_cast<size_t>(
                                          fm.current().currentAxes),
                                      slot, mr);
                    } else if (name == "text") {
                        outs[0] = makeTextHandleFor(
                            fm, fm.currentFigureId(),
                            static_cast<size_t>(fm.current().currentAxes),
                            "text", mr);
                    } else if (name == "legend") {
                        outs[0] = makeLegendHandleFor(
                            fm, fm.currentFigureId(),
                            static_cast<size_t>(fm.current().currentAxes),
                            mr);
                    } else {
                        outs[0] = makeAxesHandle(fm, mr);
                    }
                } else if (name == "subplot" && nargout >= 1) {
                    outs[0] = makeAxesHandle(fm, ctx.engine->resource());
                }
            };

        engine.registerFunction(std::string("graphics.") + entry.sub, entry.name, ext);
        if (entry.core)
            engine.registerFunction("", entry.name, ext);  // bare core name
    }
}

} // namespace numkit
