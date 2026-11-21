// src/graphics/src/library.cpp
//
// Engine-coupled installer for the graphics plotting service — the SOLE
// graphics TU that includes <nk/core>. It is registration glue only: it
// asks plots.cpp (core-free) for the table of GraphicsFn plotting bodies, then
// wraps each in a CallContext→GraphicsContext adapter and registers it under
//   graphics.<sub>.<name>   +   compat.<name>   (+ bare core name when core=true)
//
// The adapter is the one place graphics' two runtime escape hatches are bound
// to the live Engine: callBuiltin (forward to another registered plot builtin
// by name — surfc→surf, fcontour→contour, geoplot→plot, …) and callHandle
// (evaluate a user @(x) function handle for fplot/fsurf sampling). Everything
// else a plotting body needs flows through the figure session (FigureManager)
// and a scratch arena, both core-free.
//
// This mirrors the toolbox convention: compute is core-free, the install hub is
// the lone Engine-coupled file (here it also hosts the generic adapter, since
// graphics has one uniform adapter rather than per-function _reg bridges).
//
// ── Graphics handles (bugs/closed/graphics/plot-family-no-return-value.md) ──
// This TU is also the handle ACCESSOR layer:
//   • the adapter post-hook binds handle objects for chart builtins
//     (h = plot(...)) and for figure/gca/gcf/axes;
//   • handle-aware set/get/isgraphics/ishandle/isvalid are registered here
//     (delete lives in general_reg.cpp — one core name, type-dispatched);
//   • one BuiltinClass per MATLAB class name routes dot-syntax
//     (h.LineWidth, h.LineWidth = 2) through the FigureManager registry.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/math.h>
#include <keel/memory.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/graphics/library.hpp>
#include <numkit/graphics/graphics_context.hpp>
#include <numkit/graphics/graphics_handle.hpp>

#include <numkit/core/engine.hpp>  // Engine, CallContext, ExternalFunc, Span, Value
#include <numkit/core/figure_manager.hpp>
#include <numkit/graphics/figure_render.hpp>
#include <numkit/value/object.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#ifdef _WIN32
extern "C" {
__declspec(dllimport) void *__stdcall GetDC(void *hWnd);
__declspec(dllimport) int __stdcall ReleaseDC(void *hWnd, void *hDC);
__declspec(dllimport) int __stdcall GetDeviceCaps(void *hdc, int index);
}
#endif

namespace numkit {

// export_reg.cpp — saveas/exportgraphics/print (portion 32).
void registerFigureExport(Engine &engine);

namespace {

constexpr const char *kFigureClass = "matlab.ui.Figure";
constexpr const char *kAxesClass   = "matlab.graphics.axis.Axes";
constexpr const char *kTextClass   = "matlab.graphics.primitive.Text";
constexpr const char *kLegendClass = "matlab.graphics.illustration.Legend";
constexpr const char *kRootClass   = "matlab.ui.Root";

// Builtin name → Text slot for the title/label family (adapter wrap).
keel::StringRef textSlotOf(keel::StringRef fnName)
{
    static constexpr std::pair<const char *, const char *> kSlots[] = {
        {"title", "title"}, {"subtitle", "subtitle"}, {"sgtitle", "sgtitle"},
        {"xlabel", "xlabel"}, {"ylabel", "ylabel"}, {"zlabel", "zlabel"},
    };
    for (const auto &kv : kSlots)
        if (fnName == kv.first)
            return keel::StringRef(kv.second);
    return keel::StringRef{};
}

// Axes PROPERTY name → Text slot (get(gca,'Title') family — capitalized).
keel::StringRef textSlotOfProp(keel::StringRef prop)
{
    static constexpr std::pair<const char *, const char *> kSlots[] = {
        {"Title", "title"}, {"Subtitle", "subtitle"},
        {"XLabel", "xlabel"}, {"YLabel", "ylabel"}, {"ZLabel", "zlabel"},
    };
    for (const auto &kv : kSlots)
        if (prop == kv.first)
            return keel::StringRef(kv.second);
    return keel::StringRef{};
}

// Which AxesState field a Text slot reads/writes ("sgtitle" → the
// FigureState instead). ylabel under yyaxis-right routes to ylabel2.
keel::String *textFieldOf(FigureManager &fm, FigureManager::HandleRecord &rec)
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

// Builtin name → (type, MATLAB class name)
struct HandleTypeEntry {
    const char *fnName;
    const char *type;
    const char *className;
};

constexpr HandleTypeEntry kHandleTypes[] = {
    {"plot",        "line",      "matlab.graphics.chart.primitive.Line"},
    {"plot3",       "line",      "matlab.graphics.chart.primitive.Line"},
    {"line",        "line",      "matlab.graphics.chart.primitive.Line"},
    {"scatter",     "scatter",   "matlab.graphics.chart.scatter.Scatter"},
    {"scatter3",    "scatter",   "matlab.graphics.chart.scatter.Scatter"},
    {"stem",        "stem",      "matlab.graphics.chart.primitive.Stem"},
    {"stem3",       "stem",      "matlab.graphics.chart.primitive.Stem"},
    {"stairs",      "stairs",    "matlab.graphics.chart.primitive.Stair"},
    {"bar",         "bar",       "matlab.graphics.chart.primitive.Bar"},
    {"barh",        "bar",       "matlab.graphics.chart.primitive.Bar"},
    {"errorbar",    "errorbar",  "matlab.graphics.chart.primitive.ErrorBar"},
    {"area",        "area",      "matlab.graphics.chart.primitive.Area"},
    {"pie",         "pie",       "matlab.graphics.chart.primitive.Pie"},
    {"compass",     "compass",   "matlab.graphics.chart.primitive.Compass"},
    {"polarplot",   "line",      "matlab.graphics.chart.primitive.Line"},
    {"polar",       "line",      "matlab.graphics.chart.primitive.Line"},
    {"patch",       "polygon",   "matlab.graphics.primitive.Patch"},
    {"fill",        "polygon",   "matlab.graphics.primitive.Patch"},
    {"surf",        "surface",   "matlab.graphics.chart.surface.Surface"},
    {"surface",     "surface",   "matlab.graphics.primitive.Surface"},
    {"pcolor",      "surface",   "matlab.graphics.primitive.Surface"},
    {"mesh",        "surface",   "matlab.graphics.chart.surface.Surface"},
    {"waterfall",   "surface",   "matlab.graphics.chart.surface.Waterfall"},
    {"contour",     "contour",   "matlab.graphics.chart.contour.Contour"},
    {"contour3",    "contour",   "matlab.graphics.chart.contour.Contour"},
    {"contourf",    "contour",   "matlab.graphics.chart.contour.Contour"},
    {"imagesc",     "image",     "matlab.graphics.image.Image"},
    {"image",       "image",     "matlab.graphics.image.Image"},
    {"quiver",      "quiver",    "matlab.graphics.chart.primitive.Quiver"},
    {"feather",     "feather",   "matlab.graphics.chart.primitive.Feather"},
    {"histogram",   "histogram", "matlab.graphics.chart.histogram.Histogram"},
    {"fplot",       "line",      "matlab.graphics.chart.primitive.Line"},
    {"semilogx",    "line",      "matlab.graphics.chart.primitive.Line"},
    {"semilogy",    "line",      "matlab.graphics.chart.primitive.Line"},
    {"loglog",      "line",      "matlab.graphics.chart.primitive.Line"},
};

const HandleTypeEntry *findHandleType(keel::StringRef fnName)
{
    for (const auto &e : kHandleTypes)
        if (fnName == e.fnName)
            return &e;
    return nullptr;
}

// ── Property encoding ────────────────────────────────────────────────
// rec->props maps name → string form (figure layer is Value-free).
// Numerics use %.17g so decodeProp's parse round-trips bit-exactly
// (6 digits would silently truncate user values).
keel::String formatPropDouble(double d)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    return keel::String::from(buf);
}

keel::String encodeProp(const Value &v)
{
    if (v.isChar() || v.isString())
        return v.toString();
    if (v.isNumeric() && v.isScalar())
        return formatPropDouble(v.toScalar());
    if (v.isLogical() && v.numel() == 1)
        return keel::String::from(v.toBool() ? "1" : "0");
    if (v.isNumeric() && v.dims().rows() > 1 && v.dims().cols() > 1) {
        const keel::usize R = v.dims().rows();
        const keel::usize C = v.dims().cols();
        keel::String s = keel::String::from("[");
        for (keel::usize r = 0; r < R; ++r) {
            if (r) s.push_back(',');
            s.push_back('[');
            for (keel::usize c = 0; c < C; ++c) {
                if (c) s.push_back(',');
                const double x = v.elemAsDouble(c * R + r);
                if (std::isfinite(x)) s.append(formatPropDouble(x));
                else s.append("null");
            }
            s.push_back(']');
        }
        s.push_back(']');
        return s;
    }
    if (v.isNumeric() && v.numel() >= 2) {
        keel::String s = keel::String::from("[");
        for (keel::usize i = 0; i < v.numel(); ++i) {
            if (i) s.push_back(',');
            s.append(formatPropDouble(v.elemAsDouble(i)));
        }
        s.push_back(']');
        return s;
    }
    return keel::String{};
}

Value decodeProp(keel::StringRef s, keel::Allocator *mr)
{
    if (s.size() >= 2 && s[0] == '[' && s[1] == '[') {
        keel::Array<double> zf;
        keel::usize rows = 0, cols = 0;
        figrender::parseNestedDoubles(s, zf, rows, cols);
        if (rows > 0 && cols > 0 && zf.size() >= rows * cols) {
            auto r = Value::matrix(rows, cols, ValueType::DOUBLE, mr);
            double *rd = r.doubleDataMut();
            for (keel::usize rr = 0; rr < rows; ++rr)
                for (keel::usize cc = 0; cc < cols; ++cc)
                    rd[cc * rows + rr] = zf[rr * cols + cc];
            return r;
        }
    }
    if (!s.empty() && s[0] == '[') {
        keel::Array<double> vals;
        keel::String num;
        for (keel::usize i = 1; i < s.size(); ++i) {
            if (s[i] == ',' || s[i] == ']') {
                if (!num.empty()) {
                    auto r = keel::parse_f64(num.as_ref().trim());
                    if (r.is_ok()) vals.push_back(r.value());
                    num.clear();
                }
            } else {
                num.push_back(s[i]);
            }
        }
        if (vals.size() == 1)
            return Value::scalar(vals[0], mr);
        if (!vals.empty()) {
            auto r = Value::matrix(1, vals.size(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < vals.size(); ++i)
                r.doubleDataMut()[i] = vals[i];
            return r;
        }
    }
    {
        auto r = keel::parse_f64(s.trim());
        if (r.is_ok())
            return Value::scalar(r.value(), mr);
    }
    return Value::fromString(s, mr);
}

// ── Chart property defaults (MATLAB R2025b documented values) ────────
// Introspection reports MATLAB's defaults even where the nk renderer
// draws its own palette (get before set); once the user sets a property,
// both get() and the render use the user value.
Value chartDefaultProp(keel::StringRef type, keel::StringRef name,
                       keel::Allocator *mr)
{
    auto str = [&](keel::StringRef s) { return Value::fromString(s, mr); };
    auto num = [&](double d) { return Value::scalar(d, mr); };
    auto rgb = [&](double r, double g, double b) {
        auto v = Value::matrix(1, 3, ValueType::DOUBLE, mr);
        v.doubleDataMut()[0] = r; v.doubleDataMut()[1] = g; v.doubleDataMut()[2] = b;
        return v;
    };
    if (name == "Type")       return str(type);
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
    return Value();   // unknown → caller decides (empty ⇒ error/blank)
}

// ── Style-string rebuild (render-effect for LineStyle/Marker/Color) ──
// ds.style speaks two dialects (renderer parseLineSpec): the compact
// MATLAB linespec ("r--o") and the kv extras ("color=#hex;lineStyle=--").
// A property set rewrites the string into the kv form, preserving the
// components the user did not change.
struct StyleParts { keel::String colorHex, lineStyle, marker; };

StyleParts parseStyle(keel::StringRef s)
{
    StyleParts p;
    if (s.empty()) return p;
    if (s.find('=') != keel::kInvalidIndex) {
        for (keel::usize i = 0; i < s.size();) {
            keel::usize eq = s.find('=', i);
            if (eq == keel::kInvalidIndex) break;
            keel::usize semi = s.find(';', eq);
            keel::StringRef key = s.substr(i, eq - i);
            keel::StringRef val = s.substr(eq + 1,
                (semi == keel::kInvalidIndex ? s.size() : semi) - eq - 1);
            if (key == "color") p.colorHex.assign(val);
            else if (key == "lineStyle") p.lineStyle.assign(val);
            else if (key == "marker") p.marker.assign(val);
            i = semi == keel::kInvalidIndex ? s.size() : semi + 1;
        }
        return p;
    }
    // Compact dialect — mirrors the renderer's STYLE_COLOR map so a
    // rebuilt style keeps the same visual colour the plot was drawn with.
    static constexpr std::pair<char, const char *> kCharColor[] = {
        {'r', "#f07070"}, {'g', "#6ee7a0"}, {'b', "#60d0f0"},
        {'k', "#d4d4f0"}, {'m', "#e070c0"}, {'c', "#60d0f0"},
        {'y', "#e8d060"}, {'w', "#ffffff"},
    };
    static constexpr keel::StringRef kMarkers("o+*.xsd^v<>ph");
    keel::usize i = 0;
    while (i < s.size()) {
        if (p.lineStyle.empty() && i + 2 <= s.size() && s.substr(i, 2) == "--") {
            p.lineStyle.assign("--"); i += 2; continue;
        }
        if (p.lineStyle.empty() && i + 2 <= s.size() && s.substr(i, 2) == "-.") {
            p.lineStyle.assign("-."); i += 2; continue;
        }
        char c = s[i];
        if (p.lineStyle.empty() && (c == '-' || c == ':')) {
            p.lineStyle.push_back(c); ++i; continue;
        }
        if (p.colorHex.empty()) {
            bool matched = false;
            for (const auto &kv : kCharColor) {
                if (kv.first == c) {
                    p.colorHex.assign(kv.second);
                    matched = true;
                    break;
                }
            }
            if (matched) { ++i; continue; }
        }
        if (p.marker.empty() && kMarkers.find(c) != keel::kInvalidIndex) {
            p.marker.push_back(c); ++i; continue;
        }
        ++i;
    }
    return p;
}

keel::String rgbToHex(const Value &v)
{
    auto clamp255 = [](double d) {
        int i = keel::as<int>(d * 255.0 + 0.5);
        if (i < 0) i = 0;
        if (i > 255) i = 255;
        return i;
    };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x",
                  clamp255(v.numel() > 0 ? v.elemAsDouble(0) : 0),
                  clamp255(v.numel() > 1 ? v.elemAsDouble(1) : 0),
                  clamp255(v.numel() > 2 ? v.elemAsDouble(2) : 0));
    return keel::String::from(buf);
}

// ── Render-effect: apply one property to the backing DatasetInfo ─────
void applyChartProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                    keel::StringRef name, const Value &val)
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
        keel::String out;
        auto append = [&](const char *k, keel::StringRef v) {
            if (v.empty() || v == "none") return;
            if (!out.empty()) out.push_back(';');
            out.append(k);
            out.push_back('=');
            out.append(v);
        };
        append("color", p.colorHex);
        append("lineStyle", p.lineStyle);
        append("marker", p.marker);
        ds->style = keel::move(out);   // empty ⇒ renderer defaults (palette line, no marker)
    } else if (name == "Visible") {
        ds->visible = !(val.isChar() || val.isString()) || val.toString() != "off";
    } else if (name == "DisplayName") {
        ds->label = (val.isChar() || val.isString()) ? val.toString() : keel::String{};
    } else if ((name == "XData" || name == "YData") && val.isNumeric()) {
        // Replace the series. Huge series: drop the downsample state and
        // re-decide via maybeDownsampleSeries (rare path, correctness first).
        keel::Array<double> xs;
        xs.resize(val.numel());
        for (keel::usize i = 0; i < val.numel(); ++i)
            xs[i] = val.elemAsDouble(i);
        ds->xRaw.clear(); ds->yRaw.clear(); ds->seriesPyramid.clear();
        ds->seriesDownsampled = false; ds->seriesN = 0;
        (name == "XData" ? ds->xJson : ds->yJson) =
            figdetail::serializeFlatDoubles(xs);
        if (ds->xJson.empty()) ds->xJson.assign("[]");
        if (ds->yJson.empty()) ds->yJson.assign("[]");
        fm.maybeDownsampleSeries(*ds);
    } else if (name == "ZData" && val.isNumeric()) {
        if (val.dims().rows() > 1 && val.dims().cols() > 1
            && (ds->type == "surf" || ds->type == "waterfall"
                || ds->type == "contour3")) {
            ds->zJson = encodeProp(val);
        } else if (val.dims().rows() == 1 || val.dims().cols() == 1) {
            keel::Array<double> zs;
            zs.resize(val.numel());
            for (keel::usize i = 0; i < val.numel(); ++i)
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

// Auto tick positions for get(gca,'XTick') in 'auto' mode — the SAME
// niceTicks the renderer uses, over the explicit xlim if set, else the
// data range (portion 34; MATLAB returns the drawn ticks).
keel::Array<double> axesAutoTicks(FigureManager &fm,
                                  FigureManager::HandleRecord *rec, char axis)
{
    AxesState *ax = fm.axesOf(*rec);
    if (!ax)
        return {};
    const bool log = (axis == 'x' ? ax->xscale : ax->yscale) == "log";
    double lo = 0, hi = 1;
    const keel::String &lim = axis == 'x' ? ax->xlimJson : ax->ylimJson;
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
                keel::usize cells = (axis == 'x')
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
            const keel::String &j = axis == 'x' ? ds.xJson : ds.yJson;
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
// explicitly-set label-Text font — the texts fall back to the axes
// values (FontSize ×1.1). Drop their stored font props.
void resetAxesTextFonts(FigureManager &fm, FigureManager::HandleRecord *rec)
{
    for (auto [id, r] : fm.handles())
        if (!r.deleted && r.type == "text" && r.figureId == rec->figureId
            && r.axesIndex == rec->axesIndex) {
            r.props.remove("FontSize");
            r.props.remove("FontWeight");
            r.props.remove("FontName");
        }
}

// tick vector Value ← json "[a,b,c]"
Value decodeTickVec(const keel::String &json, keel::Allocator *mr)
{
    auto v = figdetail::parseFlatDoubles(json);
    if (v.empty())
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    auto out = Value::matrix(1, v.size(), ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < v.size(); ++i)
        out.doubleDataMut()[i] = v[i];
    return out;
}

// ["a","b"] json → 1×N cell of char.
Value decodeTickLabels(keel::StringRef json, keel::Allocator *mr)
{
    keel::Array<keel::String> labels;
    keel::usize i = 0;
    while (i < json.size()) {
        keel::usize q1 = json.find('"', i);
        if (q1 == keel::kInvalidIndex)
            break;
        keel::usize q2 = json.find('"', q1 + 1);
        if (q2 == keel::kInvalidIndex)
            break;
        labels.push_back(keel::String::from(json.substr(q1 + 1, q2 - q1 - 1)));
        i = q2 + 1;
    }
    if (labels.empty())
        return Value::cell(0, 0, mr);
    Value c = Value::cell(1, labels.size(), mr);
    for (keel::usize k = 0; k < labels.size(); ++k)
        c.cellAt(k) = Value::fromString(labels[k], mr);
    return c;
}

// cell/string-array/char → ["a","b"] json.
keel::String encodeTickLabels(const Value &v)
{
    keel::String out = keel::String::from("[");
    for (keel::usize i = 0; i < v.numel(); ++i) {
        if (i)
            out.push_back(',');
        out.push_back('"');
        out.append(v.cellAt(i).toString());
        out.push_back('"');
    }
    out.push_back(']');
    return out;
}

// ── Axes / figure property mapping (render-effect) ───────────────────
bool applyAxesProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                   keel::StringRef name, const Value &val)
{
    AxesState *ax = fm.axesOf(*rec);
    if (!ax)
        return false;
    keel::Array<double> v;
    v.resize(val.isNumeric() ? val.numel() : 0);
    for (keel::usize i = 0; i < v.size(); ++i)
        v[i] = val.elemAsDouble(i);
    auto limJson = [](keel::ArrayRef<double> w) -> keel::String {
        if (w.size() != 2) return keel::String{};
        keel::String out = keel::String::from("[");
        out.append(formatPropDouble(w[0]));
        out.push_back(',');
        out.append(formatPropDouble(w[1]));
        out.push_back(']');
        return out;
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
    // ── Portion 34 (MATLAB R2025b probe-verified semantics) ──
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
        keel::String os = keel::String::from("[");
        for (keel::usize i = 0; i < v.size(); ++i) {
            if (i) os.push_back(',');
            os.append(formatPropDouble(v[i]));
        }
        os.push_back(']');
        (name == "XTick" ? ax->xTicksJson
         : name == "YTick" ? ax->yTicksJson : ax->zTicksJson) = keel::move(os);
        (name == "XTick" ? ax->xTicksAuto
         : name == "YTick" ? ax->yTicksAuto : ax->zTicksAuto) = false;
    }
    else if ((name == "XTickLabel" || name == "YTickLabel"
              || name == "ZTickLabel")
             && (val.isCell() || val.isChar() || val.isString())) {
        keel::String json = val.isCell() ? encodeTickLabels(val)
                                         : keel::format("[\"{}\"]", val.toString());
        (name == "XTickLabel" ? ax->xTickLabelsJson
         : name == "YTickLabel" ? ax->yTickLabelsJson
                                : ax->zTickLabelsJson) = keel::move(json);
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
                  keel::StringRef name, keel::Allocator *mr)
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
            const keel::String &j = (axisChar == 'x') ? ds.xJson : (axisChar == 'y' ? ds.yJson : ds.zJson);
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
        if (const keel::String *hit = rec->props.get(name))
            return decodeProp(*hit, mr);
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
    // ── Portion 34 live reads (MATLAB R2025b probe-verified) ──
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
        for (keel::usize i = 0; i < v.size(); ++i)
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
        for (keel::usize i = 0; i < v.size(); ++i)
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
        for (keel::usize i = 0; i < v.size(); ++i)
            c.cellAt(i) = Value::fromString(figrender::tickLabel(v[i]), mr);
        return c;
    }
    if (name == "YTickLabel") {
        if (!ax->yTickLabelsJson.empty())
            return decodeTickLabels(ax->yTickLabelsJson, mr);
        auto v = !ax->yTicksAuto ? figdetail::parseFlatDoubles(ax->yTicksJson)
                                 : axesAutoTicks(fm, rec, 'y');
        Value c = Value::cell(v.size(), 1, mr);
        for (keel::usize i = 0; i < v.size(); ++i)
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
                    keel::StringRef name, keel::Allocator *mr)
{
    DatasetInfo *ds = fm.datasetOf(*rec);
    if (rec->type == "histogram" && ds) {
        if (name == "BinEdges") {
            auto ev = figdetail::parseFlatDoubles(ds->zJson);
            auto v = Value::matrix(1, ev.size(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < ev.size(); ++i) v.doubleDataMut()[i] = ev[i];
            return v;
        }
        if (name == "Values" || name == "BinCounts") {
            auto cv = figdetail::parseFlatDoubles(ds->yJson);
            auto v = Value::matrix(1, cv.size(), ValueType::DOUBLE, mr);
            for (keel::usize i = 0; i < cv.size(); ++i) v.doubleDataMut()[i] = cv[i];
            return v;
        }
        if (name == "NumBins") {
            auto cv = figdetail::parseFlatDoubles(ds->yJson);
            return Value::scalar(keel::as<double>(cv.size()), mr);
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
            for (keel::usize i = 0; i < dv.size(); ++i) v.doubleDataMut()[i] = dv[i];
            return v;
        }
    }
    keel::StringRef mappedName = name;
    if (mappedName == "XEndPoints") mappedName = "XData";
    if (mappedName == "YEndPoints") mappedName = "YData";
    if (mappedName != "XData" && mappedName != "YData" && mappedName != "ZData")
        return Value();
    if (!ds)
        return Value();
    if (mappedName == "ZData") {
        if (ds->zJson.empty())
            return Value::matrix(1, 0, ValueType::DOUBLE, mr);
        if (ds->zJson.size() >= 2 && ds->zJson[0] == '[' && ds->zJson[1] == '[')
            return decodeProp(ds->zJson, mr);
        keel::Array<double> zvals = figdetail::parseFlatDoubles(ds->zJson);
        if (zvals.empty())
            return Value::matrix(1, 0, ValueType::DOUBLE, mr);
        auto v = Value::matrix(1, zvals.size(), ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i < zvals.size(); ++i)
            v.doubleDataMut()[i] = zvals[i];
        return v;
    }
    const keel::Array<double> &raw = (mappedName == "XData") ? ds->xRaw : ds->yRaw;
    keel::Array<double> vals;
    if (!raw.empty()) {
        vals = raw.clone();
    } else {
        vals = figdetail::parseFlatDoubles(
            (mappedName == "XData") ? ds->xJson : ds->yJson);
    }
    if (vals.empty())
        return Value();
    auto v = Value::matrix(1, vals.size(), ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < vals.size(); ++i)
        v.doubleDataMut()[i] = vals[i];
    return v;
}

// ── Handle record lookup helpers (create-or-reuse) ───────────────────
int handleIdOfRecord(FigureManager &fm, FigureManager::HandleRecord *rec)
{
    for (auto [id, r] : fm.handles())
        if (&r == rec)
            return id;
    return -1;
}

FigureManager::HandleRecord *findRecordBy(FigureManager &fm, const char *type,
                                          int figureId, keel::usize axesIndex)
{
    for (auto [id, rec] : fm.handles())
        if (!rec.deleted && rec.type == type && rec.figureId == figureId
            && rec.axesIndex == axesIndex)
            return &rec;
    return nullptr;
}

Value makeAxesHandle(FigureManager &fm, keel::Allocator *mr)
{
    FigureState &fig = fm.current();
    const int fid = fm.currentFigureId();
    const keel::usize aidx = keel::as<keel::usize>(fig.currentAxes);
    if (auto *rec = findRecordBy(fm, "axes", fid, aidx))
        return makeGraphicsHandleValue(kAxesClass, handleIdOfRecord(fm, rec), mr);
    const int id = fm.createHandle("axes", kAxesClass);
    return makeGraphicsHandleValue(kAxesClass, id, mr);
}

Value makeFigureHandleFor(FigureManager &fm, int figureId,
                          keel::Allocator *mr)
{
    if (auto *rec = findRecordBy(fm, "figure", figureId, 0))
        return makeGraphicsHandleValue(kFigureClass, handleIdOfRecord(fm, rec), figureId, mr);
    // createHandle stamps the CURRENT figure — switch, create, switch back.
    const int saved = fm.currentFigureId();
    if (saved != figureId)
        fm.setFigure(figureId);
    const int id = fm.createHandle("figure", kFigureClass);
    if (saved != figureId)
        fm.setFigure(saved);
    return makeGraphicsHandleValue(kFigureClass, id, figureId, mr);
}

Value makeFigureHandle(FigureManager &fm, keel::Allocator *mr)
{
    fm.current();   // ensure the state exists
    return makeFigureHandleFor(fm, fm.currentFigureId(), mr);
}

// Axes handle at an EXPLICIT index (gcf.Children addresses non-current
// subplot cells too).
Value makeAxesHandleFor(FigureManager &fm, int figureId, keel::usize axesIndex,
                        keel::Allocator *mr)
{
    if (auto *rec = findRecordBy(fm, "axes", figureId, axesIndex))
        return makeGraphicsHandleValue(kAxesClass, handleIdOfRecord(fm, rec), mr);
    const int id = fm.createHandleAt(figureId, axesIndex, "axes", kAxesClass);
    return makeGraphicsHandleValue(kAxesClass, id, mr);
}

// Text handle bound to a label slot ("sgtitle" is figure-scoped: kNoAxes
// so scoped axes sweeps keep it alive while clf/close invalidate it).
Value makeTextHandleFor(FigureManager &fm, int figureId, keel::usize axesIndex,
                        keel::StringRef slot, keel::Allocator *mr)
{
    for (auto [id, rec] : fm.handles())
        if (!rec.deleted && rec.type == "text" && rec.figureId == figureId
            && rec.slot == slot && rec.axesIndex == axesIndex)
            return makeGraphicsHandleValue(kTextClass, id, mr);
    const int id = fm.createHandleAt(figureId, axesIndex, "text", kTextClass,
                                     slot);
    return makeGraphicsHandleValue(kTextClass, id, mr);
}

Value makeLegendHandleFor(FigureManager &fm, int figureId, keel::usize axesIndex,
                          keel::Allocator *mr)
{
    if (auto *rec = findRecordBy(fm, "legend", figureId, axesIndex))
        return makeGraphicsHandleValue(kLegendClass, handleIdOfRecord(fm, rec), mr);
    const int id = fm.createHandleAt(figureId, axesIndex, "legend", kLegendClass);
    return makeGraphicsHandleValue(kLegendClass, id, mr);
}

// Root (groot) — ONE global record (figureId 0 never matches a real
// figure, so figure sweeps leave it alone).
Value makeRootHandle(FigureManager &fm, keel::Allocator *mr)
{
    for (auto [id, rec] : fm.handles())
        if (!rec.deleted && rec.type == "root")
            return makeGraphicsHandleValue(kRootClass, id, mr);
    const int id = fm.createHandleAt(0, FigureManager::HandleRecord::kNoAxes,
                                     "root", kRootClass);
    return makeGraphicsHandleValue(kRootClass, id, mr);
}

// Array of handles (gcf.Children / groot.Children).
Value makeHandleArray(keel::StringRef className,
                      keel::ArrayRef<int> ids,
                      keel::Allocator *mr)
{
    if (ids.empty())
        return Value::matrix(0, 0, ValueType::DOUBLE, mr);
    auto *res = mr ? mr : keel::default_allocator();
    keel::Array<keel::Arc<ObjectState>> states(res);
    states.reserve(ids.size());
    for (int id : ids) {
        auto payload = keel::make_arc_in<GraphicsHandlePayload>(res);
        payload->handleId = id;
        auto st = keel::make_arc_in<ObjectState>(res, res);
        st->native = keel::move(payload);
        states.push_back(keel::move(st));
    }
    Dims d{1, ids.size()};
    return Value::objectArray(className, d, keel::move(states),
                              /*isHandle=*/true, res);
}

// ── Unified record property access ───────────────────────────────────
// ONE body shared by get(h,'Prop') / set(h,'Prop',v) AND the dot-syntax
// h.Prop / h.Prop = v (BuiltinClass propGet/propSet). Live state fields
// (Text.String, Legend.Location/Box/Visible, chart XData) read from the
// figure state; everything user-set lives in rec->props; documented
// MATLAB defaults fill the rest.
keel::String canonicalPropName(keel::StringRef raw)
{
    keel::String low = keel::String::from(raw);
    for (char &ch : low)
        if (ch >= 'A' && ch <= 'Z')
            ch = keel::as<char>(ch - 'A' + 'a');
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
        {"xendpoints", "XEndPoints"}, {"yendpoints", "YEndPoints"},
        {"facecolor", "FaceColor"}, {"edgecolor", "EdgeColor"},
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
            return keel::String::from(kv.second);
    return keel::String::from(raw);
}

Value recordGetProp(FigureManager &fm, FigureManager::HandleRecord *rec,
                    keel::StringRef rawName, keel::Allocator *mr,
                    bool strict = false)
{
    const keel::String name = canonicalPropName(rawName);
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
            keel::Array<int> ids;
            for (const auto &[fid, fig] : fm.figures()) {
                (void)fig;
                int hid = -1;
                for (auto [id, r] : fm.handles())
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
        const keel::String *hit = rec->props.get(name);
        return hit ? decodeProp(*hit, mr) : Value();
    }
    if (rec->type == "text") {
        // Live String from the label slot (the render source of truth).
        if (name == "String") {
            keel::String *f = textFieldOf(fm, *rec);
            return str(f ? f->c_str() : "");
        }
        if (name == "Type")       return str("text");
        if (name == "Parent") {
            // sgtitle's parent is the Figure; other slots the Axes.
            if (rec->slot == "sgtitle")
                return makeFigureHandleFor(fm, rec->figureId, mr);
            return makeAxesHandleFor(fm, rec->figureId, rec->axesIndex, mr);
        }
        if (const keel::String *hit = rec->props.get(name))
            return decodeProp(*hit, mr);
        // MATLAB R2025b (probe-verified): label Texts inherit the AXES
        // font — FontSize at 1.1× — unless the axes font was set (which
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
            keel::usize n = ax ? ax->legendLabels.size() : 0;
            if (!n)
                return Value::cell(0, 0, mr);
            Value c = Value::cell(1, n, mr);
            for (keel::usize i = 0; i < n; ++i)
                c.cellAt(i) = Value::fromString(ax->legendLabels[i], mr);
            return c;
        }
        if (name == "Type") return str("legend");
        if (name == "Parent")
            return makeAxesHandleFor(fm, rec->figureId, rec->axesIndex, mr);
        // Live render state first — a stored prop would shadow later
        // command-form changes (`legend show` after set(hL,'Visible','off')).
        if (name == "Location")
            return str(ax && !ax->legendLocation.empty()
                           ? ax->legendLocation.c_str() : "northeast");
        if (name == "Box")  return str(!ax || ax->legendBoxOn ? "on" : "off");
        if (name == "Visible")
            return str(!ax || !ax->legendHidden ? "on" : "off");
        if (const keel::String *hit = rec->props.get(name))
            return decodeProp(*hit, mr);
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
            const keel::usize n = fig ? fig->axes.size() : 0;
            keel::Array<int> ids;
            for (keel::usize k = 0; k < n; ++k) {
                int hid = -1;
                for (auto [id, r] : fm.handles())
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
        if (const keel::String *hit = rec->props.get(name))
            return decodeProp(*hit, mr);
        keel::String lowName = name.clone();
        for (char &ch : lowName)
            if (ch >= 'A' && ch <= 'Z') ch = keel::as<char>(ch - 'A' + 'a');
        for (auto [k, v] : rec->props) {
            keel::String lk = k.clone();
            for (char &ch : lk)
                if (ch >= 'A' && ch <= 'Z') ch = keel::as<char>(ch - 'A' + 'a');
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
        keel::StringRef slot = textSlotOfProp(name);
        if (!slot.empty())
            return makeTextHandleFor(fm, rec->figureId, rec->axesIndex, slot, mr);
        Value v = axesGetProp(fm, rec, name, mr);
        if (!v.isEmpty())
            return v;
        const keel::String *hit = rec->props.get(name);
        return hit ? decodeProp(*hit, mr)
                   : (strict ? Value() : Value::fromString("", mr));
    }
    // Chart records.
    if (name == "Parent")
        return makeAxesHandleFor(fm, rec->figureId, rec->axesIndex, mr);
    if (const keel::String *hit = rec->props.get(name))
        return decodeProp(*hit, mr);
    {
        Value v = chartDataProp(fm, rec, name, mr);
        if (!v.isEmpty())
            return v;
    }
    // An EXPLICIT linespec from the plot call (plot(x,y,'r--o')) is
    // user intent and reads back MATLAB-exact — unlike the soft series
    // palette the renderer draws. Colors map to MATLAB's table
    // ('r'==[1 0 0]), kv hex to /255.
    if (name == "Color" || name == "LineStyle" || name == "Marker") {
        DatasetInfo *ds = fm.datasetOf(*rec);
        if (ds && !ds->style.empty()) {
            const keel::String &s = ds->style;
            auto rgb = [&](double r, double g, double b) {
                auto v = Value::matrix(1, 3, ValueType::DOUBLE, mr);
                v.doubleDataMut()[0] = r;
                v.doubleDataMut()[1] = g;
                v.doubleDataMut()[2] = b;
                return v;
            };
            auto strv = [&](keel::StringRef t) {
                return Value::fromString(t, mr);
            };
            if (name == "LineStyle") {
                if (s.find("--") != keel::kInvalidIndex)
                    return strv("--");
                if (s.find("-.") != keel::kInvalidIndex)
                    return strv("-.");
                if (s.find(':') != keel::kInvalidIndex)
                    return strv(":");
            } else if (name == "Marker") {
                static const char *kMarkers = "o+*.xsd^v<>ph";
                for (char mc : s)
                    if (mc && std::strchr(kMarkers, mc))
                        return strv(keel::StringRef(&mc, 1));
            } else {
                struct ColEntry { char c; double rgb[3]; };
                static const ColEntry kCol[] = {
                    {'r', {1, 0, 0}},       {'g', {0, 0.5, 0}},
                    {'b', {0, 0, 1}},       {'k', {0, 0, 0}},
                    {'w', {1, 1, 1}},       {'y', {1, 1, 0}},
                    {'m', {1, 0, 1}},       {'c', {0, 1, 1}},
                };
                keel::usize hp = s.find("color=#");
                if (hp != keel::kInvalidIndex) {
                    unsigned R = 0, G = 0, B = 0;
                    if (std::sscanf(s.c_str() + hp + 7, "%02x%02x%02x", &R,
                                    &G, &B)
                        == 3)
                        return rgb(R / 255.0, G / 255.0, B / 255.0);
                } else {
                    // kv color= segment or a bare linespec letter; the
                    // single-letter form must not collide with markers
                    // first (handled above), so scan for a colour char.
                    keel::usize cp = s.find("color=");
                    keel::String cand;
                    if (cp != keel::kInvalidIndex) {
                        keel::usize e = s.find(';', cp);
                        cand = keel::String::from(
                            e == keel::kInvalidIndex
                                ? s.substr(cp + 6)
                                : s.substr(cp + 6, e - cp - 6));
                    } else if (s.size() == 1) {
                        cand = s.clone();
                    } else {
                        for (char ch : s)
                            if (std::strchr("rgbcymwkv", ch)) {
                                cand.push_back(ch);
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
                   keel::StringRef rawName, const Value &val)
{
    const keel::String name = canonicalPropName(rawName);
    if (name == "Parent" || val.isObject())
        return true;
    if (name == "XData" || name == "YData") {
        rec->props.remove(name);
    } else if (!(val.isNumeric() && val.numel() > 1024 && name != "ZData")) {
        rec->props.put(name.clone(), encodeProp(val));
    }
    if (rec->type == "text") {
        if (name == "String") {
            keel::String *f = textFieldOf(fm, *rec);
            if (f) {
                *f = (val.isChar() || val.isString()) ? val.toString() : keel::String{};
                fm.markFigureModified(rec->figureId);
            }
        }
        return true;   // FontSize/Color/… store-only (renderer gap filed)
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
            for (keel::usize i = 0; i < val.numel(); ++i)
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
// isvalid — delete lives in general_reg.cpp, type-dispatched)
// ============================================================
namespace {

void registerHandleAccessors(Engine &engine)
{
    struct SettableEntry {
        const char *key;
        keel::Array<const char *> choices;
    };
    auto settableCatalogFor = [](keel::StringRef type) -> keel::Array<SettableEntry> {
        keel::Array<SettableEntry> out;
        auto add = [&](const char *k, std::initializer_list<const char *> ch = {}) {
            SettableEntry e;
            e.key = k;
            e.choices.reserve(ch.size());
            for (const char *s : ch) e.choices.push_back(s);
            out.push_back(keel::move(e));
        };
        if (type == "figure") {
            add("Color");
            add("Name");
            add("NumberTitle", {"on", "off"});
            add("Position");
            add("Units", {"pixels", "normalized", "inches", "centimeters", "points"});
            add("Visible", {"on", "off"});
            return out;
        }
        if (type == "axes") {
            add("Box", {"on", "off"});
            add("CLim");
            add("CLimMode", {"auto", "manual"});
            add("FontName");
            add("FontSize");
            add("FontWeight", {"normal", "bold"});
            add("LineWidth");
            add("Title");
            add("Visible", {"on", "off"});
            add("XDir", {"normal", "reverse"});
            add("XGrid", {"on", "off"});
            add("XLabel");
            add("XLim");
            add("XScale", {"linear", "log"});
            add("XTick");
            add("XTickLabel");
            add("XTickLabelMode", {"auto", "manual"});
            add("XTickMode", {"auto", "manual"});
            add("YDir", {"normal", "reverse"});
            add("YGrid", {"on", "off"});
            add("YLabel");
            add("YLim");
            add("YScale", {"linear", "log"});
            add("YTick");
            add("YTickLabel");
            add("YTickLabelMode", {"auto", "manual"});
            add("YTickMode", {"auto", "manual"});
            add("ZLabel");
            add("ZLim");
            add("ZTick");
            return out;
        }
        if (type == "text") {
            add("Color");
            add("FontName");
            add("FontSize");
            add("FontWeight", {"normal", "bold"});
            add("String");
            add("Visible", {"on", "off"});
            return out;
        }
        if (type == "legend") {
            add("Box", {"on", "off"});
            add("Location", {"north", "south", "east", "west", "northeast",
                             "northwest", "southeast", "southwest", "best"});
            add("String");
            add("Visible", {"on", "off"});
            return out;
        }
        if (type == "root") {
            return out;
        }
        // Chart records (line, scatter, bar, surface, contour, ...).
        if (type == "bar") {
            add("BarWidth");
            add("BaseValue");
        } else if (type == "stem" || type == "area") {
            add("BaseValue");
        }
        add("Color");
        add("DisplayName");
        add("LineStyle", {"-", "--", ":", "-.", "none"});
        add("LineWidth");
        add("Marker", {"o", "+", "*", ".", "x", "square", "diamond",
                       "^", "v", ">", "<", "pentagram", "hexagram", "none"});
        add("MarkerSize");
        if (type == "scatter")
            add("SizeData");
        add("Visible", {"on", "off"});
        add("XData");
        add("YData");
        add("ZData");
        return out;
    };

    // set(h, ...)
    {
        ExternalFunc fn = [&engine, settableCatalogFor](
                keel::ArrayRef<Value> args, keel::usize nargout,
                keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (args.empty()) { outs[0] = Value(); return; }
            const int id = graphicsHandleIdOf(args[0]);
            if (id < 0) { outs[0] = Value(); return; }
            auto *rec = engine.figureManager().findHandle(id);
            if (!rec)
                throw std::runtime_error("Invalid or deleted object.");
            auto *mr = ctx.engine->allocator();
            if (args.size() == 1) {
                const auto cat = settableCatalogFor(rec->type);
                if (nargout == 0) {
                    keel::String text;
                    for (const auto &entry : cat) {
                        text.append("    ");
                        text.append(entry.key);
                        if (!entry.choices.empty()) {
                            text.append(": [ ");
                            for (keel::usize i = 0; i < entry.choices.size(); ++i) {
                                if (i) text.append(" | ");
                                if (i == 0) {
                                    text.push_back('{');
                                    text.append(entry.choices[i]);
                                    text.push_back('}');
                                } else {
                                    text.append(entry.choices[i]);
                                }
                            }
                            text.append(" ]");
                        }
                        text.push_back('\n');
                    }
                    ctx.engine->outputText(text);
                    outs[0] = Value();
                    return;
                }
                Value st = Value::structure(mr);
                for (const auto &entry : cat) {
                    if (entry.choices.empty()) {
                        st.setFieldAll(entry.key, Value::cell(1, 0, mr));
                    } else {
                        Value c = Value::cell(entry.choices.size(), 1, mr);
                        for (keel::usize i = 0; i < entry.choices.size(); ++i)
                            c.cellAt(i) = Value::fromString(entry.choices[i], mr);
                        st.setFieldAll(entry.key, std::move(c));
                    }
                }
                outs[0] = std::move(st);
                return;
            }
            if (args.size() == 2 && (args[1].isChar() || args[1].isString())) {
                const keel::String prop = args[1].toString();
                const auto cat = settableCatalogFor(rec->type);
                const keel::Array<const char *> *choicesPtr = nullptr;
                for (const auto &entry : cat) {
                    if (prop == entry.key) {
                        choicesPtr = &entry.choices;
                        break;
                    }
                }
                const keel::Array<const char *> emptyChoices;
                const auto &choices = choicesPtr ? *choicesPtr : emptyChoices;
                if (nargout == 0) {
                    keel::String text;
                    if (!choices.empty()) {
                        text.append("[ ");
                        for (keel::usize i = 0; i < choices.size(); ++i) {
                            if (i) text.append(" | ");
                            if (i == 0) {
                                text.push_back('{');
                                text.append(choices[i]);
                                text.push_back('}');
                            } else {
                                text.append(choices[i]);
                            }
                        }
                        text.append(" ]\n");
                    } else {
                        text.append("Property does not have a fixed set of property values.\n");
                    }
                    ctx.engine->outputText(text);
                    outs[0] = Value();
                    return;
                }
                if (choices.empty()) {
                    outs[0] = Value::cell(1, 0, mr);
                } else {
                    Value c = Value::cell(choices.size(), 1, mr);
                    for (keel::usize i = 0; i < choices.size(); ++i)
                        c.cellAt(i) = Value::fromString(choices[i], mr);
                    outs[0] = std::move(c);
                }
                return;
            }
            if (args[0].isObject() && args[0].objectCount() > 1) {
                for (keel::usize k = 0; k < args[0].objectCount(); ++k) {
                    const ObjectState *st = args[0].objectStateAt(k);
                    if (!st || !st->native) continue;
                    auto *p = dynamic_cast<const GraphicsHandlePayload *>(st->native.get());
                    if (!p) continue;
                    auto *r = engine.figureManager().findHandle(p->handleId);
                    if (!r) continue;
                    for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
                        if (!args[i].isChar() && !args[i].isString()) continue;
                        recordSetProp(engine.figureManager(), r,
                                      args[i].toString(), args[i + 1]);
                    }
                }
            } else {
                for (keel::usize i = 1; i + 1 < args.size(); i += 2) {
                    if (!args[i].isChar() && !args[i].isString()) continue;
                    recordSetProp(engine.figureManager(), rec,
                                  args[i].toString(), args[i + 1]);
                }
            }
            engine.figureManager().emitModified();
            outs[0] = Value();
        };
        engine.registerFunction("", "set", keel::move(fn));
    }
    // get(h [, 'Prop']) — without a property name: struct of all
    {
        ExternalFunc fn = [&engine](
                keel::ArrayRef<Value> args, keel::usize,
                keel::MutArrayRef<Value> outs, CallContext &) {
            if (args.empty()) { outs[0] = Value(); return; }
            const int id = graphicsHandleIdOf(args[0]);
            if (id < 0) {
                if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
                    keel::String lprop = args[1].toString();
                    for (char &ch : lprop)
                        ch = keel::to_ascii_lower(ch);
                    if (lprop == "pointerlocation") {
                        Value v = Value::matrix(1, 2, ValueType::DOUBLE, engine.allocator());
                        v.doubleDataMut()[0] = 0.0; v.doubleDataMut()[1] = 0.0;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop == "screensize") {
                        Value v = Value::matrix(1, 4, ValueType::DOUBLE, engine.allocator());
                        v.doubleDataMut()[0] = 1.0; v.doubleDataMut()[1] = 1.0;
                        double w = 1920.0, h = 1080.0;
#ifdef _WIN32
                        if (void *hdc = GetDC(nullptr)) {
                            const int physW = GetDeviceCaps(hdc, 118); // DESKTOPHORZRES
                            const int physH = GetDeviceCaps(hdc, 117); // DESKTOPVERTRES
                            const int logW  = GetDeviceCaps(hdc, 8);   // HORZRES
                            ReleaseDC(nullptr, hdc);
                            if (physW > 0 && physH > 0 && logW > 0) {
                                const double scale = std::round((keel::as<double>(physW) / keel::as<double>(logW)) * 4.0) / 4.0;
                                w = (scale > 0.0) ? (keel::as<double>(physW) / scale) : keel::as<double>(logW);
                                h = (scale > 0.0) ? (keel::as<double>(physH) / scale) : keel::as<double>(physH);
                            }
                        }
#endif
                        v.doubleDataMut()[2] = w;
                        v.doubleDataMut()[3] = h;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop == "position" || lprop == "outerposition") {
                        Value v = Value::matrix(1, 4, ValueType::DOUBLE, engine.allocator());
                        v.doubleDataMut()[0] = 0.13; v.doubleDataMut()[1] = 0.11;
                        v.doubleDataMut()[2] = 0.775; v.doubleDataMut()[3] = 0.815;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop == "nextplot") {
                        outs[0] = Value::fromString("replace", engine.allocator());
                        return;
                    }
                    if (lprop == "xcolor" || lprop == "ycolor" || lprop == "zcolor" || lprop == "color") {
                        outs[0] = Value::fromString("k", engine.allocator());
                        return;
                    }
                    if (lprop == "xlim" || lprop == "ylim" || lprop == "zlim" || lprop == "clim") {
                        Value v = Value::matrix(1, 2, ValueType::DOUBLE, engine.allocator());
                        v.doubleDataMut()[0] = 0.0; v.doubleDataMut()[1] = 1.0;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop == "xtick" || lprop == "ytick" || lprop == "ztick") {
                        Value v = Value::matrix(1, 3, ValueType::DOUBLE, engine.allocator());
                        v.doubleDataMut()[0] = 0.0; v.doubleDataMut()[1] = 0.5; v.doubleDataMut()[2] = 1.0;
                        outs[0] = std::move(v);
                        return;
                    }
                    if (lprop.find("fontangle") != keel::kInvalidIndex || lprop.find("fontweight") != keel::kInvalidIndex) {
                        outs[0] = Value::fromString("normal", engine.allocator());
                        return;
                    }
                    if (lprop.find("fontname") != keel::kInvalidIndex) {
                        outs[0] = Value::fromString("Helvetica", engine.allocator());
                        return;
                    }
                    if (lprop.find("fontsize") != keel::kInvalidIndex) {
                        outs[0] = Value::scalar(10.0, engine.allocator());
                        return;
                    }
                }
                outs[0] = Value::scalar(1.0, engine.allocator());
                return;
            }
            auto *rec = engine.figureManager().findHandle(id);
            if (!rec)
                throw std::runtime_error("Invalid or deleted object.");
            if (args.size() >= 2 && (args[1].isChar() || args[1].isString())) {
                outs[0] = recordGetProp(engine.figureManager(), rec,
                                        args[1].toString(), engine.allocator());
                return;
            }
            // get(h) → struct: stored props ∪ documented defaults.
            auto *mr = engine.allocator();
            Value st = Value::structure(mr);
            keel::HashMap<keel::String, keel::String> fields = rec->props.clone();
            static const char *kCommon[] = {
                "Type", "Color", "LineStyle", "LineWidth", "Marker",
                "MarkerSize", "Visible", "Parent", "Children",
            };
            for (const char *p : kCommon) {
                if (fields.contains(p)) continue;
                Value d = rec->isChart() ? chartDefaultProp(rec->type, p, mr)
                                         : Value();
                if (!d.isEmpty())
                    fields.put(keel::String::from(p), encodeProp(d));
            }
            if (rec->isChart()) {
                for (const char *dp : {"XData", "YData", "ZData"}) {
                    if (fields.contains(dp)) continue;
                    Value dv = chartDataProp(engine.figureManager(), rec, dp, mr);
                    if (!dv.isEmpty())
                        fields.put(keel::String::from(dp), encodeProp(dv));
                }
            }
            if (rec->type == "figure") {
                fields.put(keel::String::from("Number"), keel::format("{}", rec->figureId));
                fields.put(keel::String::from("Type"), keel::String::from("figure"));
            } else if (rec->type == "axes") {
                fields.put(keel::String::from("Type"), keel::String::from("axes"));
                static const char *kLayout[] = {
                    "XLim", "YLim", "XScale", "YScale", "XLabel", "YLabel",
                    "Title",
                };
                for (const char *p : kLayout) {
                    Value v = axesGetProp(engine.figureManager(), rec, p, mr);
                    if (!v.isEmpty())
                        fields.put(keel::String::from(p), encodeProp(v));
                }
            } else if (rec->type == "text") {
                fields.put(keel::String::from("Type"), keel::String::from("text"));
                Value v = recordGetProp(engine.figureManager(), rec, "String", mr);
                if (!v.isEmpty())
                    fields.put(keel::String::from("String"), encodeProp(v));
            } else if (rec->type == "legend") {
                fields.put(keel::String::from("Type"), keel::String::from("legend"));
                fields.put(keel::String::from("Location"), encodeProp(
                    recordGetProp(engine.figureManager(), rec, "Location", mr)));
                fields.put(keel::String::from("Visible"), encodeProp(
                    recordGetProp(engine.figureManager(), rec, "Visible", mr)));
            }
            keel::Array<keel::StringRef> sortedKeys;
            sortedKeys.reserve(fields.size());
            for (auto [k, v] : fields)
                sortedKeys.push_back(k.as_ref());
            std::sort(sortedKeys.begin(), sortedKeys.end());
            for (keel::StringRef k : sortedKeys)
                st.setFieldAll(k, decodeProp(*fields.get(k), mr));
            outs[0] = std::move(st);
        };
        engine.registerFunction("", "get", keel::move(fn));
    }
    // findobj([h][, 'Prop', value, ...] | '-property', Name)
    {
        auto fn = [&engine](
                keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs,
                CallContext &ctx) {
            auto &fm = ctx.engine->figureManager();
            auto *mr = ctx.engine->allocator();

            // Scope: an optional leading handle narrows the search.
            int scopeFig = -1;      // -1 = root scope
            int scopeAxes = -1;
            int scopeSelf = -1;      // chart-handle self-scope
            keel::usize first = 0;
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
                keel::String prop;   // "Type" or a property name
                Value want;
                bool isPropTest;    // '-property' Name
            };
            keel::Array<Filt> filts;
            for (keel::usize i = first; i < args.size(); ++i) {
                if (!args[i].isChar())
                    continue;
                keel::String key = args[i].toString();
                for (auto &c : key)
                    c = keel::to_ascii_lower(c);
                if (key == "-property" && i + 1 < args.size()
                    && args[i + 1].isChar()) {
                    Filt f;
                    f.isPropTest = true;
                    f.prop = args[++i].toString();
                    filts.push_back(keel::move(f));
                } else if (key == "-regexp" || key == "-depth") {
                    // skip their option payload; unsupported, ignored
                    ++i;
                } else if (i + 1 < args.size()) {
                    Filt f;
                    f.isPropTest = false;
                    f.prop = args[i].toString();
                    f.want = args[i + 1];
                    ++i;
                    filts.push_back(keel::move(f));
                }
            }

            auto matlabType = [](keel::StringRef t) -> keel::StringRef {
                if (t == "figure" || t == "axes" || t == "text"
                    || t == "line" || t == "legend" || t == "image")
                    return t;
                // chart records carry dataset-ish types (scatter, bar…)
                return keel::StringRef("line");
            };

            keel::Array<int> hits;
            for (auto kv : fm.handles()) {
                auto *rec = &kv.value;
                if (!rec || rec->deleted)
                    continue;
                // Hide slot-bound title/label text objects from findobj;
                // user text() stays findable.
                if (rec->type == "text" && !rec->slot.empty())
                    continue;
                if (scopeFig >= 0 && rec->figureId != scopeFig)
                    continue;
                if (scopeSelf >= 0) {
                    if (kv.key != scopeSelf)
                        continue;
                } else if (scopeAxes >= 0
                           && rec->axesIndex != (keel::usize)scopeAxes
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
                                   != f.want.toString().as_ref()) {
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
                        keel::String want = f.want.toString();
                        bool matched = false;
                        for (const auto &cc : kCol) {
                            if (want != cc.n)
                                continue;
                            matched = keel::abs(v.elemAsDouble(0) - cc.rgb[0]) < 1e-9
                                   && keel::abs(v.elemAsDouble(1) - cc.rgb[1]) < 1e-9
                                   && keel::abs(v.elemAsDouble(2) - cc.rgb[2]) < 1e-9;
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
                        for (keel::usize k = 0; k < v.numel(); ++k)
                            if (keel::abs(v.elemAsDouble(k)
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
                    hits.push_back(kv.key);   // registry key = handle id
            }
            std::sort(hits.begin(), hits.end());   // creation order
            keel::StringRef cls = hits.empty()
                ? keel::StringRef{}
                : fm.findHandle(hits[0])->className.as_ref();
            outs[0] = makeHandleArray(cls, hits, mr);
            (void)engine;
        };
        engine.registerFunction("", "findobj", fn);
        engine.registerFunction("", "findall", fn);
    }
    // isgraphics(h [, type])
    {
        ExternalFunc fn = [&engine](
                keel::ArrayRef<Value> args, keel::usize,
                keel::MutArrayRef<Value> outs, CallContext &) {
            if (args.empty()) { outs[0] = Value(); return; }
            auto &fm = engine.figureManager();
            const int id = graphicsHandleIdOf(args[0]);
            auto *rec = id >= 0 ? fm.findHandle(id) : nullptr;
            bool ok = rec != nullptr;
            if (ok && args.size() >= 2
                && (args[1].isChar() || args[1].isString())) {
                const keel::String want = args[1].toString();
                ok = rec->type == want || want == "chart"
                     || want == "graphics" || want == "axes"
                     || want == "figure";
            }
            outs[0] = Value::logicalScalar(ok);
        };
        engine.registerFunction("", "isgraphics", keel::move(fn));
    }
    // groot — the root graphics object (figure.Parent).
    {
        ExternalFunc fn = [&engine](
                keel::ArrayRef<Value>, keel::usize,
                keel::MutArrayRef<Value> outs, CallContext &) {
            outs[0] = makeRootHandle(engine.figureManager(),
                                     engine.allocator());
        };
        engine.registerFunction("", "groot", std::move(fn));
    }
    // gobjects([m, n, ...]) ? array of placeholder graphics objects.
    {
        ExternalFunc fn = [](keel::ArrayRef<Value> args, keel::usize,
                             keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            keel::usize r = 1, c = 1;
            if (args.size() == 1 && args[0].isNumeric()) {
                if (args[0].numel() == 1) {
                    r = c = keel::as<keel::usize>(std::max(0.0, args[0].toScalar()));
                } else if (args[0].numel() >= 2) {
                    r = keel::as<keel::usize>(std::max(0.0, args[0].elemAsDouble(0)));
                    c = keel::as<keel::usize>(std::max(0.0, args[0].elemAsDouble(1)));
                }
            } else if (args.size() >= 2 && args[0].isNumeric() && args[1].isNumeric()) {
                r = keel::as<keel::usize>(std::max(0.0, args[0].toScalar()));
                c = keel::as<keel::usize>(std::max(0.0, args[1].toScalar()));
            }
            auto *res = mr ? mr : keel::default_allocator();
            keel::usize n = r * c;
            keel::Array<keel::Arc<ObjectState>> states(res);
            states.reserve(n);
            for (keel::usize i = 0; i < n; ++i) {
                auto payload = keel::make_arc_in<GraphicsHandlePayload>(res);
                payload->handleId = 0;
                auto st = keel::make_arc_in<ObjectState>(res, res);
                st->native = keel::move(payload);
                states.push_back(keel::move(st));
            }
            outs[0] = Value::objectArray("matlab.graphics.GraphicsPlaceholder",
                                         Dims(r, c), keel::move(states),
                                         /*isHandle=*/true, res);
        };
        engine.registerFunction("", "gobjects", std::move(fn));
    }
    // ishandle(h) / isvalid(h)
    {
        auto fn = [&engine](
                keel::ArrayRef<Value> args, keel::usize,
                keel::MutArrayRef<Value> outs, CallContext &) {
            if (args.empty()) { outs[0] = Value(); return; }
            auto &fm = engine.figureManager();
            const int id = graphicsHandleIdOf(args[0]);
            outs[0] = Value::logicalScalar(
                id >= 0 && fm.findHandle(id) != nullptr);
        };
        engine.registerFunction("", "ishandle", fn);
        engine.registerFunction("", "isvalid", fn);
    }
}

// ============================================================
// BuiltinClass per graphics class — construct + dot-syntax + isa + eq
// ============================================================
void registerGraphicsClasses(Engine &engine)
{
    // Distinct class names across the chart table + axes + figure
    // + text/legend/root records, plus class -> internal type mapping.
    struct ClassTypePair {
        const char *className;
        const char *recType;
    };
    keel::Array<ClassTypePair> classToType;
    auto addClass = [&](const char *cn, const char *rt) {
        for (const auto &p : classToType)
            if (keel::StringRef(p.className) == cn)
                return;
        classToType.push_back(ClassTypePair{cn, rt});
    };
    addClass(kFigureClass, "figure");
    addClass(kAxesClass,   "axes");
    addClass(kTextClass,   "text");
    addClass(kLegendClass, "legend");
    addClass(kRootClass,   "root");
    addClass("matlab.graphics.Graphics", "line");
    addClass("matlab.graphics.GraphicsPlaceholder", "line");
    for (const auto &e : kHandleTypes)
        addClass(e.className, e.type);

    for (const auto &pair : classToType) {
        const char *clsName = pair.className;
        const char *recType = pair.recType;
        BuiltinClass cls;
        cls.name = keel::String::from(clsName);
        cls.isHandle = true;
        cls.superclasses.push_back(keel::String::from("matlab.mixin.Handle"));
        cls.superclasses.push_back(keel::String::from("handle"));
        cls.construct = [&engine, clsName, recType](
                keel::ArrayRef<Value> args, CallContext &ctx) -> Value {
            auto &fm = ctx.engine->figureManager();
            auto *mr = ctx.engine->allocator();
            const keel::StringRef recTypeRef(recType);
            Value h;
            if (recTypeRef == "figure") {
                h = makeFigureHandle(fm, mr);
            } else if (recTypeRef == "axes") {
                h = makeAxesHandle(fm, mr);
            } else if (recTypeRef == "text") {
                const int fid = fm.currentFigureId();
                const keel::usize aidx = keel::as<keel::usize>(fm.current().currentAxes);
                const int id = fm.createHandleAt(fid, aidx, "text", kTextClass);
                h = makeGraphicsHandleValue(kTextClass, id, mr);
            } else if (recTypeRef == "legend") {
                const int fid = fm.currentFigureId();
                const keel::usize aidx = keel::as<keel::usize>(fm.current().currentAxes);
                h = makeLegendHandleFor(fm, fid, aidx, mr);
            } else if (recTypeRef == "root") {
                h = makeRootHandle(fm, mr);
            } else {
                AxesState &ax = fm.currentAxes();
                const keel::usize dsIdx = ax.datasets.size();
                DatasetInfo ds;
                if (recTypeRef == "surface") {
                    ds.type.assign("surf");
                    ds.xJson.assign("[1,2,3]");
                    ds.yJson.assign("[1,2,3]");
                    ds.zJson.assign("[[1,0,0],[0,1,0],[0,0,1]]");
                } else {
                    ds.type.assign(recTypeRef);
                    ds.xJson.assign("[0,1]");
                    ds.yJson.assign("[0,1]");
                }
                ax.datasets.push_back(keel::move(ds));
                const int id = fm.createHandle(recTypeRef, clsName, dsIdx);
                h = makeGraphicsHandleValue(clsName, id, mr);
            }
            const int hid = graphicsHandleIdOf(h);
            if (auto *rec = fm.findHandle(hid)) {
                for (keel::usize i = 0; i + 1 < args.size(); i += 2) {
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
        cls.propGet = [&engine](const Value &self, keel::StringRef name,
                                Value &out, CallContext &) -> bool {
            const int id = graphicsHandleIdOf(self);
            if (id < 0)
                return false;
            auto *rec = engine.figureManager().findHandle(id);
            if (!rec)
                throw std::runtime_error("Invalid or deleted object.");
            Value v = recordGetProp(engine.figureManager(), rec, name,
                                    engine.allocator(), /*strict=*/true);
            if (v.isEmpty())
                return false;   // unknown prop → engine's error
            out = std::move(v);
            return true;
        };
        cls.propSet = [&engine](Value &self, keel::StringRef name,
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
        cls.ops.put(keel::String::from("eq"),
                    [](Value &, keel::ArrayRef<Value> args, keel::usize,
                       keel::MutArrayRef<Value> outs, CallContext &) {
            bool eq = args.size() >= 2
                      && graphicsHandleIdOf(args[0]) >= 0
                      && graphicsHandleIdOf(args[0]) == graphicsHandleIdOf(args[1]);
            outs[0] = Value::logicalScalar(eq);
        });
        cls.ops.put(keel::String::from("ne"),
                    [](Value &, keel::ArrayRef<Value> args, keel::usize,
                       keel::MutArrayRef<Value> outs, CallContext &) {
            bool eq = args.size() >= 2
                      && graphicsHandleIdOf(args[0]) >= 0
                      && graphicsHandleIdOf(args[0]) == graphicsHandleIdOf(args[1]);
            outs[0] = Value::logicalScalar(!eq);
        });
        cls.dispText = [&engine](const Value &self) -> keel::String {
            const int id = graphicsHandleIdOf(self);
            auto *rec = id >= 0 ? engine.figureManager().findHandle(id) : nullptr;
            if (!rec)
                return keel::String::from("  deleted handle\n");
            const keel::usize dot = rec->className.rfind('.');
            const keel::StringRef shortName = (dot == keel::kInvalidIndex)
                ? rec->className.as_ref()
                : rec->className.substr(dot + 1);
            keel::String s = keel::format("  {} with properties:\n\n", shortName);
            auto line = [&](const char *k, keel::StringRef v) {
                s.append("    ");
                s.append(k);
                s.append(": ");
                s.append(v);
                s.push_back('\n');
            };
            for (const char *p : {"Color", "LineStyle", "LineWidth",
                                  "Marker", "Visible"}) {
                if (const keel::String *hit = rec->props.get(p)) {
                    line(p, *hit);
                    continue;
                }
                Value d = rec->isChart()
                              ? chartDefaultProp(rec->type, p, engine.allocator())
                              : Value();
                if (!d.isEmpty())
                    line(p, encodeProp(d));
            }
            return s;
        };
        engine.registerClass(keel::move(cls));
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

    keel::Array<PlotEntry> table;
    buildPlotTable(table);

    // Handle-accessor names registered above — skip their (noop)
    // table entries (registerFunctionImpl_ throws on duplicates).
    auto isHandleAccessorName = [](keel::StringRef nm) -> bool {
        static constexpr const char *kNames[] = {
            "set", "get", "isgraphics", "ishandle", "isvalid",
        };
        for (const char *n : kNames)
            if (nm == n) return true;
        return false;
    };
    // Layout functions whose RESULT the adapter wraps into a handle
    // object (the core-free body only prepares the state / label field).
    auto isHandleReturning = [](keel::StringRef nm) -> bool {
        static constexpr const char *kNames[] = {
            "figure", "gcf", "gca", "axes", "polaraxes",
            "title", "subtitle", "sgtitle", "xlabel", "ylabel", "zlabel",
            "legend", "text",
        };
        for (const char *n : kNames)
            if (nm == n) return true;
        return false;
    };

    for (PlotEntry &entry : table) {
        if (isHandleAccessorName(entry.name))
            continue;
        const char *entryName = entry.name;
        const HandleTypeEntry *hitType = findHandleType(entryName);
        const bool hasHandle = hitType != nullptr;
        const bool wrapHandle = isHandleReturning(entryName);

        // The core-free plotting body; shared via Arc so core entries can
        // register under both graphics.<sub>.<name> and the bare core name.
        auto sharedFn = keel::make_arc<GraphicsFn>(keel::move(entry.fn));
        auto ext =
            [fn = keel::move(sharedFn), hitType, hasHandle, wrapHandle, entryName](
                keel::ArrayRef<Value> args, keel::usize nargout,
                keel::MutArrayRef<Value> outs, CallContext &ctx) {
                auto &fm = ctx.engine->figureManager();
                const keel::StringRef name(entryName);
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
                    ctx.engine->allocator(),
                    // callBuiltin — forward to another registered plot builtin.
                    [&ctx](keel::StringRef nm, keel::ArrayRef<Value> a,
                           keel::usize no, keel::MutArrayRef<Value> o) -> bool {
                        const ExternalFunc *cf =
                            ctx.engine->findExternal(nm, ctx.env);
                        if (!cf)
                            return false;
                        (*cf)(a, no, o, ctx);
                        return true;
                    },
                    // callHandle — evaluate a user function-handle value.
                    [&ctx](const Value &fh, keel::ArrayRef<Value> a) -> Value {
                        return ctx.engine->callFunctionHandle(fh, a);
                    },
                };
                (*fn)(args, nargout, outs, gc);

                // Post-hook: bind the handle object(s) so h = plot(...)
                // works (bugs/closed/graphics/plot-family-no-return-value.md).
                // Statement calls get the handle in `ans`, like the reference.
                if (hasHandle) {
                    const keel::usize base = fm.chartCallBase();
                    const keel::usize after = fm.currentAxes().datasets.size();
                    if (after > base) {
                        const keel::StringRef hType(hitType->type);
                        const keel::StringRef hClass(hitType->className);
                        const bool isSurfOrContour =
                            (hType == "surface" || hType == "contour");
                        const int nnew = isSurfOrContour
                                             ? 1
                                             : keel::as<int>(after - base);
                        if (nnew == 1) {
                            const int id = fm.createHandle(hType, hClass, base);
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
                                            rec->props.put(keel::String::from("ZData"),
                                                           encodeProp(*zArg));
                                        }
                                    }
                                }
                            }
                            outs[0] = makeGraphicsHandleValue(
                                hClass, id, ctx.engine->allocator());
                        } else {
                            auto *res = ctx.engine->allocator()
                                            ? ctx.engine->allocator()
                                            : keel::default_allocator();
                            keel::Array<keel::Arc<ObjectState>> states(res);
                            states.reserve(nnew);
                            for (int i = 0; i < nnew; ++i) {
                                const int id = fm.createHandle(
                                    hType, hClass,
                                    base + keel::as<keel::usize>(i));
                                auto payload = keel::make_arc_in<GraphicsHandlePayload>(res);
                                payload->handleId = id;
                                auto st = keel::make_arc_in<ObjectState>(res, res);
                                st->native = keel::move(payload);
                                states.push_back(keel::move(st));
                            }
                            Dims d{1, keel::as<keel::usize>(nnew)};
                            outs[0] = Value::objectArray(
                                hClass, d, keel::move(states),
                                /*isHandle=*/true, res);
                        }
                    }
                } else if (wrapHandle) {
                    // figure/gcf → Figure, gca/axes → Axes, the title/
                    // label family → slot-bound Text, legend → Legend.
                    auto *mr = ctx.engine->allocator();
                    if (name == "figure" || name == "gcf") {
                        outs[0] = makeFigureHandle(fm, mr);
                        if (name == "figure" && args.size() >= 2 && (args[0].isChar() || args[0].isString())) {
                            int hid = graphicsHandleIdOf(outs[0]);
                            if (auto *rec = fm.findHandle(hid)) {
                                for (keel::usize i = 0; i + 1 < args.size(); i += 2) {
                                    if (args[i].isChar() || args[i].isString())
                                        recordSetProp(fm, rec, args[i].toString(), args[i + 1]);
                                }
                            }
                        }
                    } else if (keel::StringRef slot = textSlotOf(name);
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
                                      keel::as<keel::usize>(
                                          fm.current().currentAxes),
                                      slot, mr);
                    } else if (name == "text") {
                        outs[0] = makeTextHandleFor(
                            fm, fm.currentFigureId(),
                            keel::as<keel::usize>(fm.current().currentAxes),
                            "text", mr);
                    } else if (name == "legend") {
                        outs[0] = makeLegendHandleFor(
                            fm, fm.currentFigureId(),
                            keel::as<keel::usize>(fm.current().currentAxes),
                            mr);
                    } else {
                        outs[0] = makeAxesHandle(fm, mr);
                    }
                } else if (name == "subplot" && nargout >= 1) {
                    outs[0] = makeAxesHandle(fm, ctx.engine->allocator());
                }
            };

        keel::String ns = keel::format("graphics.{}", entry.sub);
        if (entry.core) {
            engine.registerFunction(ns, entry.name, ext);
            engine.registerFunction("", entry.name, keel::move(ext));  // bare core name
        } else {
            engine.registerFunction(ns, entry.name, keel::move(ext));
        }
    }

    auto noopGfx = [](keel::ArrayRef<Value>, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
        if (nargout >= 1) outs[0] = Value::scalar(1.0, ctx.engine->allocator());
    };
    engine.registerFunction("", "shg", noopGfx);
    engine.registerFunction("", "datetick", noopGfx);
    engine.registerFunction("", "rectangle", noopGfx);
    engine.registerFunction("", "uicontrol",
        [](keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto &fm = ctx.engine->figureManager();
            const int id = fm.createHandleAt(
                fm.currentFigureId(), FigureManager::HandleRecord::kNoAxes,
                "text", kTextClass);
            outs[0] = makeGraphicsHandleValue(kTextClass, id, ctx.engine->allocator());
        });
    engine.registerFunction("", "VideoWriter",
        [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            Value s = Value::structure(mr);
            keel::String fn = (!args.empty() && (args[0].isChar() || args[0].isString()))
                                  ? args[0].toString() : keel::String::from("output.avi");
            s.setFieldAll("Filename", Value::fromString(fn, mr));
            s.setFieldAll("FrameRate", Value::scalar(30.0, mr));
            s.setFieldAll("Quality", Value::scalar(75.0, mr));
            outs[0] = std::move(s);
        });
    engine.registerFunction("", "open",
        [](keel::ArrayRef<Value>, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            if (nargout >= 1) outs[0] = Value::structure(ctx.engine->allocator());
            else outs[0] = Value();
        });
    engine.registerFunction("", "writeVideo",
        [](keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &) {
            outs[0] = Value();
        });
    auto graphStub = [](keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
        auto *mr = ctx.engine->allocator();
        Value s = Value::structure(mr);
        if (!args.empty()) s.setFieldAll("Adj", args[0]);
        outs[0] = std::move(s);
    };
    engine.registerFunction("", "digraph", graphStub);
    engine.registerFunction("", "graph", graphStub);
    engine.registerFunction("", "plotmatrix", noopGfx);
    engine.registerFunction("", "makedatatip", noopGfx);
    engine.registerFunction("", "soundsc", noopGfx);
    engine.registerFunction("", "zplane", noopGfx);
    engine.registerFunction("", "listfonts",
        [](keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
            auto *mr = ctx.engine->allocator();
            Value c = Value::cell(4, 1, mr);
            c.cellAt(0) = Value::fromString("Helvetica", mr);
            c.cellAt(1) = Value::fromString("Arial", mr);
            c.cellAt(2) = Value::fromString("Times New Roman", mr);
            c.cellAt(3) = Value::fromString("Courier New", mr);
            outs[0] = c;
        });
}

} // namespace numkit
