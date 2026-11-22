// nk/graphics — layout.cpp
//
// graphics.layout.* builders (figure / subplot / axes / labels / limits / legend / ticks / colormap), carved out of plots.cpp. Core-free bodies (GraphicsContext); shared
// JSON/parse helpers come from plot_internal.hpp.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/math.h>
#include <keel/number.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include "plot_internal.hpp"
#include <numkit/graphics/graphics_context.hpp>
#include <numkit/graphics/graphics_handle.hpp>

#include <algorithm>
#include <keel/static_array.h>
#include <climits>
#define _USE_MATH_DEFINES
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace numkit {

namespace {

using namespace numkit::detail;

void figure_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto *mr = gc.mr;
    auto &fm = gc.fm;
    keel::i32 id;
    if (args.empty() || args[0].isChar() || args[0].isString()) {
        id = fm.newFigure();
    } else if (const keel::i32 hid = graphicsHandleIdOf(args[0]); hid >= 0) {
        // figure(f) with a Figure handle — make its figure current.
        auto *rec = fm.findHandle(hid);
        if (!rec || rec->type != "figure")
            throw std::runtime_error(
                "figure: value must be a Figure handle or a number");
        id = rec->figureId;
        fm.setFigure(id);
    } else {
        id = keel::as<keel::i32>(args[0].toScalar());
        fm.setFigure(id);
    }
    fm.current().modified = true;
    fm.emitModified();
    // The bundle adapter wraps this id into a matlab.ui.Figure
    // handle object (kHandleReturning wrap). Always emit the id —
    // MATLAB binds a bare `figure` statement's Figure into `ans`.
    outs[0] = Value::scalar(keel::as<keel::f64>(id), mr);
}
void close_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    if (args.empty()) {
        fm.closeCurrentNotify();
    } else if ((args[0].isChar() || args[0].isString()) && args[0].toString() == "all") {
        fm.closeAllNotify();
    } else if (args[0].isObject()) {
        for (keel::usize k = 0; k < args[0].objectCount(); ++k) {
            Value elem = args[0].objectSubArray({k}, gc.mr);
            const keel::i32 hid = graphicsHandleIdOf(elem);
            if (hid < 0) continue;
            auto *rec = fm.findHandle(hid);
            if (!rec)
                throw std::runtime_error("Invalid or deleted object.");
            if (rec->type == "figure")
                fm.closeFigureNotify(rec->figureId);
            else if (rec->type == "waitbar")
                rec->deleted = true;
        }
    } else if (args[0].isNumeric()) {
        for (keel::usize k = 0; k < args[0].numel(); ++k) {
            keel::i32 id = keel::as<keel::i32>(args[0].elemAsDouble(k));
            fm.closeFigureNotify(id);
        }
    }
    outs[0] = Value();
}
void waitbar_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)args;
    if (nargout >= 1) {
        const keel::i32 hid = gc.fm.createHandleAt(
            0, FigureManager::HandleRecord::kNoAxes, "waitbar", "matlab.ui.Figure");
        outs[0] = makeGraphicsHandleValue("matlab.ui.Figure", hid, gc.mr);
    } else {
        outs[0] = Value();
    }
}
void whitebg_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &)
{
    (void)args;
    (void)nargout;
    outs[0] = Value();
}
void clf_plot(keel::ArrayRef<Value> /*args*/, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &fm = gc.fm;
    auto &fig = fm.current();
    // Everything drawn in this figure dies — stale handles must
    // read as deleted (MATLAB semantics over a cleared figure).
    // The Figure itself survives clf; the super title is figure
    // CONTENT, so it dies too (MATLAB clf clears sgtitle).
    fm.invalidateHandles(fig.id, -1,
                         /*axes records*/ true, /*figure record*/ false);
    fig.superTitle.clear();
    fig.axes.clear();
    fig.axes.push_back(AxesState{});
    fig.currentAxes = 0;
    fig.subplotRows = 0;
    fig.subplotCols = 0;
    fig.modified = true;
    fm.emitModified();
    outs[0] = Value();
}
void queryOrSetLim(FigureManager &fm, keel::String &limJson,
                        keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs,
                        char axisChar)
{
    keel::usize idx = (args.size() >= 1 && graphicsHandleIdOf(args[0]) >= 0) ? 1 : 0;
    if (idx < args.size() && args[idx].isNumeric() && args[idx].numel() >= 2) {
        limJson = vecToJson(args[idx]);
        fm.current().modified = true;
        fm.emitModified();
        outs[0] = Value();
        return;
    }
    if (idx < args.size() && (args[idx].isChar() || args[idx].isString())) {
        keel::String mode = argStr(args[idx]);
        if (mode == "auto") {
            limJson.clear();
            fm.current().modified = true;
            fm.emitModified();
        }
        if (nargout == 0) {
            outs[0] = Value();
            return;
        }
    }
    keel::f64 lo = 0.0, hi = 1.0;
    bool parsed = false;
    if (!limJson.empty()) {
        keel::usize b = limJson.find('[');
        keel::usize c = limJson.find(',');
        keel::usize e = limJson.find(']');
        if (b != keel::kInvalidIndex && c != keel::kInvalidIndex && e != keel::kInvalidIndex && b < c && c < e) {
            auto rlo = keel::parse_f64(limJson.substr(b + 1, c - b - 1).trim());
            auto rhi = keel::parse_f64(limJson.substr(c + 1, e - c - 1).trim());
            if (rlo.is_ok() && rhi.is_ok()) {
                lo = rlo.value();
                hi = rhi.value();
                parsed = true;
            }
        }
    }
    if (!parsed) {
        keel::f64 dmin = 1e300, dmax = -1e300;
        for (const auto &ds : fm.currentAxes().datasets) {
            if (axisChar == 'y'
                && (ds.type == "bar" || ds.type == "histogram" || ds.type == "area")) {
                if (0.0 < dmin) dmin = 0.0;
                if (0.0 > dmax) dmax = 0.0;
            }
            const keel::Array<keel::f64> *raw =
                (axisChar == 'x') ? &ds.xRaw : (axisChar == 'y' ? &ds.yRaw : nullptr);
            if (raw && !raw->empty()) {
                for (keel::f64 v : *raw) {
                    if (std::isfinite(v)) {
                        if (v < dmin) dmin = v;
                        if (v > dmax) dmax = v;
                    }
                }
                continue;
            }
            const keel::String &j = (axisChar == 'x') ? ds.xJson : (axisChar == 'y' ? ds.yJson : ds.zJson);
            const char *base = j.c_str();
            keel::usize pos = 0;
            while (pos < j.size()) {
                if (keel::is_ascii_digit(j[pos]) || j[pos] == '-' || j[pos] == '+') {
                    char *endptr = nullptr;
                    keel::f64 v = std::strtod(base + pos, &endptr);
                    if (endptr && endptr > base + pos && std::isfinite(v)) {
                        if (v < dmin) dmin = v;
                        if (v > dmax) dmax = v;
                        pos = keel::as<keel::usize>(endptr - base);
                        continue;
                    }
                }
                ++pos;
            }
        }
        if (dmin <= dmax) {
            if (dmax > dmin) {
                if (fm.currentAxes().axisMode == "tight") {
                    lo = dmin;
                    hi = dmax;
                } else {
                    const keel::f64 span = dmax - dmin;
                    const keel::f64 p10 = std::pow(10.0, std::floor(std::log10(span)));
                    const keel::f64 s = span / p10;
                    const keel::f64 step = (s <= 1.5) ? 0.2 * p10
                                      : (s <= 3.0) ? 0.5 * p10
                                      : (s <= 7.0 || s >= 9.5) ? 1.0 * p10
                                                   : 2.0 * p10;
                    lo = std::floor(dmin / step + 1e-12) * step;
                    hi = std::ceil(dmax / step - 1e-12) * step;
                }
            } else {
                lo = dmin - 1.0;
                hi = dmin + 1.0;
            }
        }
    }
    Value res = Value::matrix(1, 2, ValueType::DOUBLE);
    res.doubleDataMut()[0] = lo;
    res.doubleDataMut()[1] = hi;
    outs[0] = std::move(res);
}
void hold_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &ax = gc.fm.currentAxes();
    keel::usize idx = (args.size() >= 2 && graphicsHandleIdOf(args[0]) >= 0) ? 1 : 0;
    if (idx >= args.size())
        ax.holdOn = !ax.holdOn;
    else {
        keel::String s = argStr(args[idx]);
        ax.holdOn = (s == "on" || s == "all");
    }
    outs[0] = Value();
}
void ishold_plot(keel::ArrayRef<Value> /*args*/, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    outs[0] = Value::logicalScalar(gc.fm.currentAxes().holdOn);
}
void colordef_plot(keel::ArrayRef<Value> /*args*/, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (nargout >= 1)
        outs[0] = Value::scalar(keel::as<keel::f64>(gc.fm.current().id));
    else
        outs[0] = Value();
}
void subplot_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    if (args.size() == 1 && !args[0].isEmpty()) {
        if (const keel::i32 hid = graphicsHandleIdOf(args[0]); hid >= 0) {
            if (auto *rec = fm.findHandle(hid)) {
                if (rec->figureId > 0) fm.setFigure(rec->figureId);
                if (rec->axesIndex < fm.current().axes.size()) {
                    fm.current().currentAxes = keel::as<keel::i32>(rec->axesIndex);
                }
            }
            outs[0] = Value();
            return;
        }
        keel::i32 mnp = 0;
        if (args[0].isChar() || args[0].isString()) {
            auto r = keel::parse_i64(args[0].toString().as_ref().trim());
            if (r.is_ok()) mnp = keel::as<keel::i32>(r.value());
        } else {
            mnp = keel::as<keel::i32>(args[0].elemAsDouble(0));
        }
        if (mnp >= 111 && mnp <= 999) {
            keel::i32 m = mnp / 100;
            keel::i32 n = (mnp / 10) % 10;
            keel::i32 p = mnp % 10;
            if (m >= 1 && n >= 1 && p >= 1 && p <= m * n) {
                fm.setSubplot(m, n, p);
            }
        }
        outs[0] = Value();
        return;
    }
    if (args.size() < 3 || args[0].isEmpty() || args[1].isEmpty() || args[2].isEmpty()) {
        outs[0] = Value();
        return;
    }
    keel::i32 m = keel::as<keel::i32>(args[0].elemAsDouble(0));
    keel::i32 n = keel::as<keel::i32>(args[1].elemAsDouble(0));
    keel::i32 p = keel::as<keel::i32>(args[2].elemAsDouble(0));
    fm.setSubplot(m, n, p);
    outs[0] = Value();
}
// tiledlayout(m, n[, ...]) — modern alternative to subplot. We
// store the grid shape on the FigureState so subsequent nexttile
// calls can step through cells. Trailing N-V pairs ('Padding',
// 'TileSpacing', 'TileIndexing') are accepted but currently no-op
// (the IDE always renders subplot cells with the same fixed gap).
void tiledlayout_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    keel::i32 m = 1, n = 1;
    if (args.size() >= 1 && !args[0].isChar()) m = (keel::i32)args[0].toScalar();
    if (args.size() >= 2 && !args[1].isChar()) n = (keel::i32)args[1].toScalar();
    if (m < 1) m = 1;
    if (n < 1) n = 1;
    auto &fm = gc.fm;
    // setSubplot(m, n, 1) reserves the grid + activates cell 1.
    fm.setSubplot(m, n, 1);
    outs[0] = Value();
}
// nexttile([span]) — bumps the active subplot cell index by 1
// (or by `span` if given a numeric arg). When the figure has no
// tiledlayout grid yet, the call is a no-op.
void nexttile_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    auto &fig = fm.current();
    if (fig.subplotRows <= 0 || fig.subplotCols <= 0) {
        // No tiledlayout active; default to a 1x1 grid so
        // the first nexttile creates a single cell.
        fm.setSubplot(1, 1, 1);
        outs[0] = Value();
        return;
    }
    const keel::i32 total = fig.subplotRows * fig.subplotCols;
    // Determine target cell. With a numeric arg, jump to that
    // cell (1-based); else advance by 1 from the current.
    keel::i32 target = fig.currentAxes + 2;   // (currentAxes is 0-based)
    if (!args.empty() && !args[0].isChar()) {
        target = (keel::i32)args[0].toScalar();
    }
    if (target < 1) target = 1;
    if (target > total) target = total;
    fm.setSubplot(fig.subplotRows, fig.subplotCols, target);
    outs[0] = Value();
}
void title_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    keel::usize idx = (args.size() >= 2 && graphicsHandleIdOf(args[0]) >= 0) ? 1 : 0;
    if (idx < args.size()) {
        auto &fm = gc.fm;
        fm.currentAxes().title = argStr(args[idx]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}
void subtitle_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    keel::usize idx = (args.size() >= 2 && graphicsHandleIdOf(args[0]) >= 0) ? 1 : 0;
    if (idx < args.size()) {
        auto &fm = gc.fm;
        fm.currentAxes().subtitle = argStr(args[idx]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}
void sgtitle_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (!args.empty()) {
        auto &fm = gc.fm;
        fm.current().superTitle = argStr(args[0]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}
void xlabel_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    keel::usize idx = (args.size() >= 2 && graphicsHandleIdOf(args[0]) >= 0) ? 1 : 0;
    if (idx < args.size()) {
        auto &fm = gc.fm;
        fm.currentAxes().xlabel = argStr(args[idx]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}
// text(x, y, str, ...) — annotation overlay. Each call appends ONE
// text dataset (single-point) to the current axes. For arrays, the
// user iterates via for-loop in script. Trailing name-value pairs
// (Color, FontSize) parsed minimally — extra pairs are ignored.
// The IDE renders text overlays after the image / line layers so
// labels stay on top of imagesc / scatter.
// annotation(type, x, y [, Name-Value]) — figure-level overlays in
// NORMALIZED FIGURE coordinates [0,1] (MATLAB convention; probes
// R2025b: Color (0.1294)^3, HeadSize 10, LineWidth 0.5; textarrow
// text sits at the TAIL). Accepted types: line / arrow / textarrow /
// ellipse / rectangle / textbox. NV: String, Color (name / short /
// 1x3 triplet), FaceColor, FontSize. Attached to the CURRENT
// figure; clf clears them (same lifecycle as superTitle). No
// handle object yet — deferred with the annotation IDE rendering
// (figure-export-gaps / handle-surface-gaps).
void annotation_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (args.empty() || !args[0].isChar()) { outs[0] = Value(); return; }
    keel::String type = args[0].toString();
    for (auto &c : type) c = keel::to_ascii_lower(c);
    static const keel::StaticArray<const char *, 6> kTypes = {
        "line", "arrow", "textarrow", "ellipse", "rectangle",
        "textbox"};
    bool okType = false;
    for (const char *t : kTypes)
        if (type == t)
            okType = true;
    if (!okType) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    auto &fig = fm.current();
    FigureState::Annotation an;
    an.type = keel::move(type);
    if (args.size() >= 2 && args[1].isNumeric()
        && args[1].numel() >= 4
        && !(args.size() >= 3 && args[2].isNumeric())) {
        // Position form (ellipse / rectangle / textbox):
        // annotation(type, [x y w h]) — one vector.
        an.x1 = args[1].elemAsDouble(0);
        an.y1 = args[1].elemAsDouble(1);
        an.x2 = an.x1 + args[1].elemAsDouble(2);
        an.y2 = an.y1 + args[1].elemAsDouble(3);
    } else {
        // Two-vector form (line / arrow / textarrow):
        // annotation(type, x, y) with 2-element normalized x/y.
        if (args.size() >= 2 && args[1].isNumeric()
            && args[1].numel() >= 2) {
            an.x1 = args[1].elemAsDouble(0);
            an.x2 = args[1].elemAsDouble(1);
        }
        if (args.size() >= 3 && args[2].isNumeric()
            && args[2].numel() >= 2) {
            an.y1 = args[2].elemAsDouble(0);
            an.y2 = args[2].elemAsDouble(1);
        }
    }
    // Name-Value walk.
    auto colorToHex = [](const Value &cv, keel::String &hexOut) -> bool {
        char buf[8];
        if (cv.isChar()) {
            keel::String s = cv.toString();
            for (auto &ch : s) ch = keel::to_ascii_lower(ch);
            static const std::pair<const char *, const char *> kMap[] = {
                {"r", "#ff0000"}, {"g", "#008000"}, {"b", "#0000ff"},
                {"k", "#000000"}, {"w", "#ffffff"}, {"y", "#ffff00"},
                {"m", "#ff00ff"}, {"c", "#00ffff"},
                {"red", "#ff0000"}, {"green", "#008000"},
                {"blue", "#0000ff"}, {"black", "#000000"},
                {"white", "#ffffff"}, {"none", ""},
            };
            for (const auto &kv : kMap)
                if (s == kv.first) {
                    hexOut.assign(kv.second);
                    return true;
                }
            return false;
        }
        if (cv.isNumeric() && cv.numel() >= 3) {
            std::snprintf(buf, sizeof buf, "#%02x%02x%02x",
                          (keel::u32)(cv.elemAsDouble(0) * 255 + 0.5),
                          (keel::u32)(cv.elemAsDouble(1) * 255 + 0.5),
                          (keel::u32)(cv.elemAsDouble(2) * 255 + 0.5));
            hexOut.assign(buf);
            return true;
        }
        return false;
    };
    // Name-Value scan: keys are CHAR args anywhere after the
    // geometry (fixed i+=2 desyncs when the position vector
    // shifts the parity).
    for (keel::usize i = 1; i + 1 < args.size(); ++i) {
        if (!args[i].isChar())
            continue;
        keel::String key = args[i].toString();
        for (auto &c : key) c = keel::to_ascii_lower(c);
        if (key == "string" && args[i + 1].isChar()) {
            an.text = args[i + 1].toString();
        } else if (key == "color") {
            colorToHex(args[i + 1], an.colorHex);
        } else if (key == "facecolor") {
            colorToHex(args[i + 1], an.faceHex);
        } else if (key == "fontsize" && args[i + 1].isNumeric()) {
            an.fontSize = args[i + 1].elemAsDouble(0);
        }
    }
    fig.annotations.push_back(keel::move(an));
    fig.modified = true;
    fm.emitModified();
    outs[0] = Value();
}
void text_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (args.size() < 3) { outs[0] = Value(); return; }
    const Value &xv = args[0];
    const Value &yv = args[1];
    if (!xv.numel() || !yv.numel()) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    // text() does NOT call prepareForPlot — it's an annotation,
    // it should append to whatever's already there even without
    // explicit `hold on`.
    DatasetInfo ds;
    ds.type.assign("text");
    StrBuf xs, ys;
    xs << "[" << xv.elemAsDouble(0) << "]";
    ys << "[" << yv.elemAsDouble(0) << "]";
    ds.xJson = xs.take();
    ds.yJson = ys.take();
    keel::usize strIdx = (args.size() >= 4 && args[2].isNumeric() &&
                     (args[3].isChar() || args[3].isString() || args[3].isCell())) ? 3 : 2;
    ds.label = argStr(args[strIdx]);
    keel::String extras;
    auto addExtra = [&](keel::StringRef kv) {
        if (!extras.empty()) extras.push_back(';');
        extras.append(kv);
    };
    for (keel::usize i = strIdx + 1; i + 1 < args.size(); i += 2) {
        keel::String key = argStr(args[i]);
        for (auto &c : key) c = keel::to_ascii_lower(c);
        const Value &val = args[i + 1];
        if (key == "color") {
            if (val.isChar() || val.isString()) {
                keel::String kv = keel::format("color={}", argStr(val));
                addExtra(kv.as_ref());
            } else if (val.isNumeric() && val.numel() >= 3) {
                StrBuf os2;
                os2 << "rgb=" << val.elemAsDouble(0) << ","
                    << val.elemAsDouble(1) << "," << val.elemAsDouble(2);
                addExtra(os2.as_ref());
            }
        } else if (key == "fontsize" && val.isNumeric()) {
            StrBuf fs;
            fs << "fontSize=" << val.elemAsDouble(0);
            addExtra(fs.as_ref());
        } else if (key == "horizontalalignment" && val.isChar()) {
            keel::String a = argStr(val);
            for (auto &c : a) c = keel::to_ascii_lower(c);
            if (a == "left" || a == "center" || a == "right") {
                keel::String kv = keel::format("halign={}", a);
                addExtra(kv.as_ref());
            }
        }
    }
    ds.style = keel::move(extras);
    fm.pushDataset(keel::move(ds));
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
void ylabel_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    keel::usize idx = (args.size() >= 2 && (graphicsHandleIdOf(args[0]) >= 0 || args[0].isNumeric())) ? 1 : 0;
    if (idx < args.size()) {
        auto &fm = gc.fm;
        auto &ax = fm.currentAxes();
        if (ax.yyEnabled && ax.activeYside == "right")
            ax.ylabel2 = argStr(args[idx]);
        else
            ax.ylabel = argStr(args[idx]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}
void xlim_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    queryOrSetLim(gc.fm, gc.fm.currentAxes().xlimJson, args, nargout, outs, 'x');
}
void ylim_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &ax = gc.fm.currentAxes();
    keel::String &lim = (ax.yyEnabled && ax.activeYside == "right") ? ax.ylim2Json : ax.ylimJson;
    queryOrSetLim(gc.fm, lim, args, nargout, outs, 'y');
}
void grid_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    // MATLAB semantics:
    //   grid          → toggle MAJOR (minor untouched)
    //   grid on       → major on
    //   grid off      → both off
    //   grid minor    → toggle MINOR (major untouched)
    // Major and minor are independent booleans; the script can
    // hold "minor only" (rare but valid in MATLAB).
    keel::usize idx = (args.size() >= 2 && graphicsHandleIdOf(args[0]) >= 0) ? 1 : 0;
    if (idx >= args.size()) {
        ax.gridMajor = !ax.gridMajor;
    } else if (args[idx].isChar() || args[idx].isString()) {
        keel::String arg = argStr(args[idx]);
        for (auto &c : arg) c = keel::to_ascii_lower(c);
        if      (arg == "on")    { ax.gridMajor = true; }
        else if (arg == "off")   { ax.gridMajor = false; ax.gridMinor = false; }
        else if (arg == "minor") { ax.gridMinor = !ax.gridMinor; }
    }
    ax.gridUserTouched = true;
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
// Normalise a Location string ("NorthWest" → "northwest"). MATLAB
// accepts mixed case; we always lowercase. Empty / unrecognised
// strings clear the field so the renderer falls back to its default.
keel::String normLocation(keel::StringRef raw)
{
    keel::String s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back(keel::to_ascii_lower(c));
    // Allowed values, mirroring MATLAB:
    //   north, south, east, west, best, none,
    //   northeast, northwest, southeast, southwest,
    //   plus the same with `outside` suffix.
    static const keel::StaticArray<const char *, 19> kAllowed{
        "north", "south", "east", "west", "best", "none",
        "northeast", "northwest", "southeast", "southwest",
        "northoutside", "southoutside", "eastoutside", "westoutside",
        "northeastoutside", "northwestoutside",
        "southeastoutside", "southwestoutside",
        "bestoutside",
    };
    for (auto p : kAllowed)
        if (s == p) return s;
    return keel::String{};
}
void legend_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    // Walk args. Plain strings become legend labels; a `'Location'`
    // / 'location' marker captures the next arg as the location.
    // Other (Name, Value) pairs (FontSize, NumColumns, ...) are
    // not yet honoured but parsed-and-skipped so they don't
    // pollute legendLabels.
    // Pure-visibility args ('boxoff'/'boxon'/'show'/'on'/
    // 'hide'/'off') must NOT clear existing labels — they toggle
    // frame/visibility only (MATLAB semantics). Detect up front
    // before zapping legendLabels.
    bool visibilityOnly = !args.empty();
    for (keel::usize i = 0; i < args.size() && visibilityOnly; ++i) {
        if (!args[i].isChar()) { visibilityOnly = false; break; }
        keel::String s = args[i].toString();
        keel::String sl;
        sl.reserve(s.size());
        for (char c : s) sl.push_back(keel::to_ascii_lower(c));
        if (sl != "boxoff" && sl != "boxon" && sl != "show"
            && sl != "on" && sl != "hide" && sl != "off") {
            visibilityOnly = false;
            break;
        }
    }
    if (!visibilityOnly) ax.legendLabels.clear();
    // Don't reset legendLocation if the user calls bare `legend(...)`
    // again — but DO accept an explicit override.
    bool sawLocation = false;
    keel::String newLoc;
    for (keel::usize i = 0; i < args.size(); ++i) {
        // Cell of labels — the canonical multi-legend form
        // legend({'sine','cosine'}, 'Location', ...) — decompose
        // into individual labels
        // (bugs/closed/graphics/legend-cell-and-name-value.md).
        if (args[i].isCell()) {
            for (keel::usize c = 0; c < args[i].numel(); ++c)
                ax.legendLabels.push_back(argStr(args[i].cellAt(c)));
            continue;
        }
        if (!args[i].isChar()) {
            if (i == 0 && (args[i].isObject() || args[i].isNumeric()))
                continue;
            if (i > 0 && (args[i].isNumeric() || args[i].isLogical()))
                throw std::runtime_error("legend: invalid argument (numeric location syntax is not supported)");
            ax.legendLabels.push_back(argStr(args[i]));
            continue;
        }
        keel::String s = args[i].toString();
        keel::String sLower;
        sLower.reserve(s.size());
        for (char c : s) sLower.push_back(keel::to_ascii_lower(c));
        if (sLower == "location" && i + 1 < args.size() && (args[i + 1].isChar() || args[i + 1].isString())) {
            sawLocation = true;
            newLoc = normLocation(args[i + 1].toString());
            ++i;  // skip the value
            continue;
        }
        static const keel::StaticArray<const char *, 23> kLegendProps{
            "orientation", "numcolumns", "numcolumnsmode",
            "fontsize", "fontname", "fontweight", "fontangle", "fontunits",
            "interpreter", "box", "color", "edgecolor", "textcolor",
            "linewidth", "autoupdate", "position", "units", "visible",
            "string", "title", "itemhitfcn", "tag", "userdata",
        };
        bool isLegendProp = false;
        for (const char *p : kLegendProps) {
            if (sLower == p) {
                isLegendProp = true;
                break;
            }
        }
        if (isLegendProp && i + 1 < args.size()) {
            if (sLower == "box" && (args[i + 1].isChar() || args[i + 1].isString())) {
                keel::String bv = args[i + 1].toString();
                for (char &c : bv) c = keel::to_ascii_lower(c);
                ax.legendBoxOn = (bv != "off");
            }
            ++i;  // skip property value
            continue;
        }
        // MATLAB 'show'/'hide'/'off' as the first arg: visibility
        // toggles that PRESERVE labels + location (Visible prop).
        if (i == 0 && (sLower == "off" || sLower == "hide")) {
            ax.legendHidden = true;
            fm.current().modified = true;
            fm.emitModified();
            outs[0] = Value();
            return;
        }
        if (i == 0 && (sLower == "show" || sLower == "on")) {
            ax.legendHidden = false;
            // No labels passed — keep whatever was set previously.
            continue;
        }
        // 'boxoff' / 'boxon' — toggle the legend frame.
        if (sLower == "boxoff") {
            ax.legendBoxOn = false;
            continue;
        }
        if (sLower == "boxon") {
            ax.legendBoxOn = true;
            continue;
        }
        ax.legendLabels.push_back(keel::move(s));
    }
    if (sawLocation)
        ax.legendLocation = keel::move(newLoc);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
void colorbar_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    // Default: presence of the call enables the bar at the
    // renderer's default location ('east'). Subsequent calls
    // can move it via 'Location', '<pos>'.
    if (ax.colorbarLocation.empty())
        ax.colorbarLocation.assign("east");
    for (keel::usize i = 0; i < args.size(); ++i) {
        if (!args[i].isChar()) continue;
        keel::String s = args[i].toString();
        keel::String sLower;
        sLower.reserve(s.size());
        for (char c : s) sLower.push_back(keel::to_ascii_lower(c));
        if (sLower == "location" && i + 1 < args.size() && args[i + 1].isChar()) {
            auto loc = normLocation(args[i + 1].toString());
            if (!loc.empty()) ax.colorbarLocation = keel::move(loc);
            ++i;
            continue;
        }
        // Positional location: colorbar('southoutside') etc.
        // (probe: MATLAB accepts it; the old parser silently
        // dropped it and kept the 'east' default).
        if (sLower != "off" && sLower != "hide" && sLower != "on") {
            auto loc = normLocation(s);
            if (!loc.empty()) {
                ax.colorbarLocation = keel::move(loc);
                continue;
            }
        }
        if (sLower == "off" || sLower == "hide") {
            ax.colorbarLocation.clear();
            fm.current().modified = true;
            fm.emitModified();
            outs[0] = Value();
            return;
        }
    }
    fm.current().modified = true;
    fm.emitModified();
    if (nargout >= 1) {
        Value s = Value::structure(gc.mr);
        s.setField(0, "Layout", Value::structure(gc.mr));
        s.setField(0, "Label", Value::structure(gc.mr));
        outs[0] = std::move(s);
    } else {
        outs[0] = Value();
    }
}
void axis_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    if (!args.empty() && (args[0].isChar() || args[0].isString())) {
        keel::String mode = args[0].toString();
        if (mode == "off") {
            ax.axisVisible = false;
        } else if (mode == "on") {
            ax.axisVisible = true;
        } else {
            if (mode == "ij") ax.yDir.assign("reverse");
            else if (mode == "xy") ax.yDir.assign("normal");
            else if (mode == "tight" || mode == "auto") {
                ax.xlimJson.clear();
                ax.ylimJson.clear();
                ax.zlimJson.clear();
            }
            ax.axisMode = keel::move(mode);
        }
        fm.current().modified = true;
        fm.emitModified();
        outs[0] = Value();
        return;
    }
    if (!args.empty() && args[0].isNumeric() && args[0].numel() >= 4) {
        StrBuf xs, ys;
        xs << "[" << args[0].elemAsDouble(0) << "," << args[0].elemAsDouble(1) << "]";
        ys << "[" << args[0].elemAsDouble(2) << "," << args[0].elemAsDouble(3) << "]";
        ax.xlimJson = xs.take();
        ax.ylimJson = ys.take();
        if (args[0].numel() >= 6) {
            StrBuf zs;
            zs << "[" << args[0].elemAsDouble(4) << "," << args[0].elemAsDouble(5) << "]";
            ax.zlimJson = zs.take();
        }
        fm.current().modified = true;
        fm.emitModified();
        outs[0] = Value();
        return;
    }
    Value xl, yl;
    queryOrSetLim(fm, ax.xlimJson, keel::ArrayRef<Value>{}, 1, keel::MutArrayRef<Value>(&xl, 1), 'x');
    queryOrSetLim(fm, ax.ylimJson, keel::ArrayRef<Value>{}, 1, keel::MutArrayRef<Value>(&yl, 1), 'y');
    Value out = Value::matrix(1, 4, ValueType::DOUBLE, gc.mr);
    out.doubleDataMut()[0] = xl.elemAsDouble(0);
    out.doubleDataMut()[1] = xl.elemAsDouble(1);
    out.doubleDataMut()[2] = yl.elemAsDouble(0);
    out.doubleDataMut()[3] = yl.elemAsDouble(1);
    outs[0] = std::move(out);
}
// xdir / ydir — direct setters. MATLAB also accepts
// set(gca, 'XDir', 'reverse'); we ship the direct form here,
// and `set` is on the BACKLOG.
void xdir_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (!args.empty() && args[0].isChar()) {
        auto &fm = gc.fm;
        keel::String v = args[0].toString();
        if (v == "reverse" || v == "normal") {
            fm.currentAxes().xDir = keel::move(v);
            fm.current().modified = true;
            fm.emitModified();
        }
    }
    outs[0] = Value();
}
void ydir_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (!args.empty() && args[0].isChar()) {
        auto &fm = gc.fm;
        keel::String v = args[0].toString();
        if (v == "reverse" || v == "normal") {
            fm.currentAxes().yDir = keel::move(v);
            fm.current().modified = true;
            fm.emitModified();
        }
    }
    outs[0] = Value();
}
// ── Color limits & colormap — graphics.layout ─────────────────────
void caxis_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (!args.empty() && args[0].numel() >= 2) {
        auto &fm = gc.fm;
        fm.currentAxes().climJson = vecToJson(args[0]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}
void clim_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (!args.empty() && args[0].numel() >= 2) {
        auto &fm = gc.fm;
        fm.currentAxes().climJson = vecToJson(args[0]);
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
}
void colormap_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    if (args.empty()) {
        auto cm = Value::matrix(64, 3, ValueType::DOUBLE, gc.mr);
        keel::f64 *p = cm.doubleDataMut();
        for (keel::usize i = 0; i < 64; ++i) {
            const keel::f64 v = keel::as<keel::f64>(i) / 63.0;
            p[i] = v;
            p[64 + i] = v;
            p[128 + i] = v;
        }
        outs[0] = std::move(cm);
        return;
    }
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    if (args[0].isChar()) {
        keel::String name = args[0].toString();
        for (auto &c : name) c = keel::to_ascii_lower(c);
        ax.colormapName = keel::move(name);
        ax.customColormapJson.clear();
    } else {
        // Numeric M×3 matrix → encode as JSON array of triplets
        // and clear the named colormap.
        const auto &M = args[0];
        const keel::usize R = M.dims().rows();
        const keel::usize C = M.dims().cols();
        if (C != 3 || R == 0) {
            outs[0] = Value();
            return;
        }
        StrBuf os;
        os << "[";
        for (keel::usize r = 0; r < R; ++r) {
            if (r) os << ",";
            // Column-major: M(r, c) = data[c * R + r].
            const keel::f64 rv = M.elemAsDouble(0 * R + r);
            const keel::f64 gv = M.elemAsDouble(1 * R + r);
            const keel::f64 bv = M.elemAsDouble(2 * R + r);
            os << "[" << rv << "," << gv << "," << bv << "]";
        }
        os << "]";
        ax.customColormapJson = os.take();
        if (ax.colormapName.empty()) ax.colormapName.assign("custom");
    }
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = (nargout >= 1 && !args.empty() && args[0].isNumeric()) ? args[0] : Value();
}
// colorscale('log' | 'linear') — must be called BEFORE imagesc(M).
// Sets the axes' colorScale state, which survives prepareForPlot so
// the next imagesc bakes log10 into its quantization (one-shot —
// toggling after imagesc does nothing because the data is already
// quantized).
void colorscale_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &fm = gc.fm;
    if (args.empty()) {
        fm.currentAxes().colorScale.assign("linear");
    } else if (args[0].isChar()) {
        keel::String mode = args[0].toString();
        for (auto &c : mode) c = keel::to_ascii_lower(c);
        if (mode == "log" || mode == "linear") {
            fm.currentAxes().colorScale = keel::move(mode);
        }
    }
    outs[0] = Value();
}
// xscale / yscale('log' | 'linear') — set the axis scale of the current
// axes. Mirrors `set(gca, 'XScale', 'log')` from MATLAB. Unlike colorscale
// these do NOT survive prepareForPlot — they're meant to be set AFTER
// the plot, like in MATLAB. The IDE-side renderer (Heatmap, Interactive-
// Plot) reads figure.xscale / figure.yscale to drive log-axis display.
void axisScaleImpl(const char *fnName, keel::String AxesState::*field,
                   keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs,
                   GraphicsContext &gc)
{
auto &fm = gc.fm;
keel::String mode = keel::String::from("linear");
if (!args.empty() && args[0].isChar()) {
    keel::String m = args[0].toString();
    for (auto &c : m) c = keel::to_ascii_lower(c);
    if (m == "log" || m == "linear") mode = keel::move(m);
}
auto &ax = fm.currentAxes();
// yyaxis: yscale routes to yscale2 when right is active.
if (keel::StringRef(fnName) == "yscale"
    && ax.yyEnabled && ax.activeYside == "right")
    ax.yscale2 = keel::move(mode);
else
    ax.*field = keel::move(mode);
fm.current().modified = true;
fm.emitModified();
outs[0] = Value();
        }

void xscale_plot(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{ axisScaleImpl("xscale", &AxesState::xscale, args, outs, gc); }
void yscale_plot(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{ axisScaleImpl("yscale", &AxesState::yscale, args, outs, gc); }
// yyaxis(side) — switch the active Y-side. MATLAB: yyaxis left|right.
// First call enables the dual-axis state and stays enabled for the
// life of the current axes; toggling only changes which side new
// plots / ylim / ylabel / yscale calls write to.
void yyaxis_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    keel::String side = keel::String::from("left");
    if (!args.empty() && args[0].isChar()) {
        keel::String s = args[0].toString();
        for (auto &c : s) c = keel::to_ascii_lower(c);
        if (s == "left" || s == "right") side = keel::move(s);
    }
    ax.yyEnabled = true;
    ax.activeYside = keel::move(side);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
void noop(keel::ArrayRef<Value> /*args*/, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &/*gc*/)
{
    outs[0] = Value();
}
void noop_ret1(keel::ArrayRef<Value> /*args*/, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    outs[0] = Value::scalar(1.0, gc.mr);
}
// linkaxes — connect all subplot cells in the current figure for
// synchronised pan/zoom. MATLAB:
//   linkaxes(h)            — link on x and y (alias for 'xy')
//   linkaxes(h, 'x'|'y'|'xy'|'off')
// nk doesn't model graphics handles, so we ignore the first
// argument and link every subplot cell in the current figure
// unconditionally. The mode is stored on the FigureState and
// applied by the SubplotGrid renderer.
void linkaxes_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto &fm = gc.fm;
    keel::String mode = keel::String::from("xy");  // MATLAB default when no mode arg
    for (auto &a : args) {
        if (a.isChar()) {
            keel::String s = a.toString();
            keel::String sl;
            sl.reserve(s.size());
            for (char c : s) sl.push_back(keel::to_ascii_lower(c));
            if (sl == "x" || sl == "y" || sl == "xy" || sl == "off") {
                mode = keel::move(sl);
                break;  // only the first string arg is the mode
            }
        }
    }
    if (mode == "off")
        fm.current().linkMode.clear();
    else
        fm.current().linkMode = keel::move(mode);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
// rotate3d / pan3d / zoom3d (on|off) — toggle the OrbitControls
// axis-of-interaction for the 3-D renderer. MATLAB also accepts
// these as no-arg toggles but the e2e parity tests pass strings.
void interactionToggle(keel::String AxesState::*field,
                            keel::ArrayRef<Value> args, keel::MutArrayRef<Value> outs,
                            GraphicsContext &gc)
{
    keel::String mode = keel::String::from("on");
    if (!args.empty() && args[0].isChar()) {
        keel::String s = args[0].toString();
        for (auto &c : s) c = keel::to_ascii_lower(c);
        if (s == "on" || s == "off") mode = keel::move(s);
    }
    gc.fm.currentAxes().*field = keel::move(mode);
    gc.fm.current().modified = true;
    gc.fm.emitModified();
    outs[0] = Value();
}
void rotate3d_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    interactionToggle(&AxesState::rotate3dMode, a, o, gc);
}
void pan3d_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    interactionToggle(&AxesState::pan3dMode, a, o, gc);
}
void zoom3d_plot(keel::ArrayRef<Value> a, keel::usize, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{
    interactionToggle(&AxesState::zoom3dMode, a, o, gc);
}
// xticks / yticks / zticks — set custom tick positions. Accept a
// numeric vector OR the string "auto" to clear. With no args,
// the v1 returns an empty value (true MATLAB returns the
// auto-generated tick set; needs renderer-side query plumbing).
void ticksImpl(keel::String AxesState::*field, bool AxesState::*autoFlag,
               keel::ArrayRef<Value> args, keel::usize nargout,
               keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
(void)nargout;
auto &fm = gc.fm;
auto &ax = fm.currentAxes();
if (args.empty()) {
    outs[0] = Value();
    return;
}
if (args[0].isChar()) {
    keel::String s = args[0].toString();
    for (auto &c : s) c = keel::to_ascii_lower(c);
    if (s == "auto") {
        (ax.*field).clear();
        ax.*autoFlag = true;   // MATLAB XTickMode 'auto'
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
    return;
}
StrBuf os;
os << '[';
const keel::usize n = args[0].numel();
for (keel::usize i = 0; i < n; ++i) {
    if (i) os << ',';
    os << args[0].elemAsDouble(i);
}
os << ']';
ax.*field = os.take();
ax.*autoFlag = false;   // explicit ticks freeze the mode
fm.current().modified = true;
fm.emitModified();
outs[0] = Value();
    }

void xticks_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ ticksImpl(&AxesState::xTicksJson, &AxesState::xTicksAuto, a, n, o, gc); }
void yticks_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ ticksImpl(&AxesState::yTicksJson, &AxesState::yTicksAuto, a, n, o, gc); }
void zticks_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ ticksImpl(&AxesState::zTicksJson, &AxesState::zTicksAuto, a, n, o, gc); }
// xticklabels / yticklabels / zticklabels — set custom tick label
// strings. Accept a cell of strings, a string array, or a single
// multiline string. v1: cell-of-chars (most common) supported via
// per-element toString; "auto" clears.
void tickLabelsImpl(keel::String AxesState::*field,
                    keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
(void)nargout;
auto &fm = gc.fm;
auto &ax = fm.currentAxes();
if (args.empty()) { outs[0] = Value(); return; }
if (args[0].isChar()) {
    keel::String s = args[0].toString();
    keel::String sl;
    sl.reserve(s.size());
    for (char c : s) sl.push_back(keel::to_ascii_lower(c));
    if (sl == "auto") {
        (ax.*field).clear();
        fm.current().modified = true;
        fm.emitModified();
    }
    outs[0] = Value();
    return;
}
// Build "[\"a\",\"b\",...]" from cell/string-array.
StrBuf os;
os << '[';
const keel::usize n = args[0].numel();
for (keel::usize i = 0; i < n; ++i) {
    if (i) os << ',';
    Value elem = args[0].elemAt(i, gc.mr);
    keel::String s = elem.isChar() ? elem.toString() : keel::String{};
    os << '"';
    for (char c : s) {
        if (c == '"' || c == '\\') os << '\\';
        os << c;
    }
    os << '"';
}
os << ']';
ax.*field = os.take();
fm.current().modified = true;
fm.emitModified();
outs[0] = Value();
    }

void xticklabels_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ tickLabelsImpl(&AxesState::xTickLabelsJson, a, n, o, gc); }
void yticklabels_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ tickLabelsImpl(&AxesState::yTickLabelsJson, a, n, o, gc); }
void zticklabels_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ tickLabelsImpl(&AxesState::zTickLabelsJson, a, n, o, gc); }
// xtickformat / ytickformat / ztickformat — set a sprintf-style
// format string ("%.2f", "%.0e", etc.). 'auto' clears.
void tickFormatImpl(keel::String AxesState::*field,
                    keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
(void)nargout;
auto &fm = gc.fm;
auto &ax = fm.currentAxes();
if (args.empty() || !args[0].isChar()) {
    outs[0] = Value();
    return;
}
keel::String s = args[0].toString();
keel::String sl;
sl.reserve(s.size());
for (char c : s) sl.push_back(keel::to_ascii_lower(c));
if (sl == "auto") {
    (ax.*field).clear();
} else {
    ax.*field = keel::move(s);
}
fm.current().modified = true;
fm.emitModified();
outs[0] = Value();
    }

void xtickformat_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ tickFormatImpl(&AxesState::xTickFormat, a, n, o, gc); }
void ytickformat_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ tickFormatImpl(&AxesState::yTickFormat, a, n, o, gc); }
void ztickformat_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ tickFormatImpl(&AxesState::zTickFormat, a, n, o, gc); }
// xtickangle / ytickangle — rotation of tick labels in degrees.
void tickAngleImpl(keel::f64 AxesState::*field,
                   keel::ArrayRef<Value> args, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
(void)nargout;
auto &fm = gc.fm;
if (args.empty()) { outs[0] = Value(); return; }
fm.currentAxes().*field = args[0].toScalar();
fm.current().modified = true;
fm.emitModified();
outs[0] = Value();
    }

void xtickangle_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ tickAngleImpl(&AxesState::xTickAngle, a, n, o, gc); }
void ytickangle_plot(keel::ArrayRef<Value> a, keel::usize n, keel::MutArrayRef<Value> o, GraphicsContext &gc)
{ tickAngleImpl(&AxesState::yTickAngle, a, n, o, gc); }
// daspect([dx dy dz]) / pbaspect([px py pz]) — data + plot box
// aspect ratios. v1 maps the canonical [1 1 1] case to
// axisMode='equal' (equivalent visual). Other ratios accepted but
// currently no-op — full anisotropic stretching is BACKLOG.
void aspectImpl(keel::ArrayRef<Value> args, keel::usize nargout,
                     keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    if (args.empty()) { outs[0] = Value(); return; }
    if (args[0].isChar()) {
        keel::String s = args[0].toString();
        for (auto &c : s) c = keel::to_ascii_lower(c);
        if (s == "auto") {
            fm.currentAxes().axisMode.clear();
            fm.current().modified = true;
            fm.emitModified();
        }
        outs[0] = Value();
        return;
    }
    if (args[0].numel() >= 3) {
        const keel::f64 dx = args[0].elemAsDouble(0);
        const keel::f64 dy = args[0].elemAsDouble(1);
        const keel::f64 dz = args[0].elemAsDouble(2);
        if (keel::abs(dx - dy) < 1e-9 && keel::abs(dy - dz) < 1e-9
            && fm.currentAxes().axisMode.empty()) {
            fm.currentAxes().axisMode.assign("equal");
            fm.current().modified = true;
            fm.emitModified();
        }
    }
    outs[0] = Value();
}


// box(on|off|toggle) — show/hide the axis frame rectangle (the
// full closed box vs. just left + bottom edges).
void box_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    if (args.empty()) {
        ax.boxOn = !ax.boxOn;
    } else if (args[0].isChar()) {
        keel::String s = args[0].toString();
        for (auto &c : s) c = keel::to_ascii_lower(c);
        if      (s == "on")  ax.boxOn = true;
        else if (s == "off") ax.boxOn = false;
    }
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
// gca / gcf / axes — ensure the target exists; the bundle adapter
// wraps the result into an axes/figure handle object
// (matlab.graphics.axis.Axes / matlab.ui.Figure).
void axes_plot(keel::ArrayRef<Value> /*args*/, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    // MATLAB axes(...) with no output makes the (new) axes
    // current. nk has one axes per non-subplot figure, so
    // this reduces to ensuring it exists (Position etc. no-op).
    gc.fm.currentAxes();
    outs[0] = Value();
}
void gca_plot(keel::ArrayRef<Value> /*args*/, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    gc.fm.currentAxes();   // create-on-demand, like MATLAB
    outs[0] = Value();
}
void gcf_plot(keel::ArrayRef<Value> /*args*/, keel::usize /*nargout*/, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    gc.fm.current();       // create-on-demand, like MATLAB
    outs[0] = Value();
}
// cla([reset]) — clear the current axes' datasets. With 'reset'
// also clears the per-axis config (title, xlabel, etc.). MATLAB's
// 'reset' is opt-in; default cla preserves axes properties.
void cla_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    ax.datasets.clear();
    ax.animatedDatasetIdx = -1;
    // 'reset' arg: also clear axis config + scales.
    if (!args.empty() && args[0].isChar()) {
        keel::String s = args[0].toString();
        for (auto &c : s) c = keel::to_ascii_lower(c);
        if (s == "reset") {
            ax.title.clear(); ax.subtitle.clear();
            ax.xlabel.clear(); ax.ylabel.clear();
            ax.xlimJson.clear(); ax.ylimJson.clear();
            ax.legendLabels.clear(); ax.legendLocation.clear();
            ax.gridMajor = false; ax.gridMinor = false;
            ax.gridUserTouched = false;
            ax.colormapName.clear();
            ax.axisMode.clear();
            ax.axisVisible = true;
            ax.xscale.assign("linear"); ax.yscale.assign("linear");
            ax.xTicksJson.clear(); ax.yTicksJson.clear();
            ax.xTickLabelsJson.clear(); ax.yTickLabelsJson.clear();
            ax.xTickFormat.clear(); ax.yTickFormat.clear();
        }
    }
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
// linkprop / linkdata — handle-based property linking. We don't
// model graphics handles, so these accept any args and return an
// opaque scalar handle so user scripts that store the return value
// (h = linkprop(...);) don't break. Real synchronised state is
// BACKLOG.


// ────────────────────────────────────────────────────────────────
// animatedline — incremental line plot. MATLAB:
//   h = animatedline;          — empty line on the current axes
//   h = animatedline(x0, y0);  — initial points
//   addpoints(h, x, y);
//   clearpoints(h);
//   [x, y] = getpoints(h);
//   drawnow;                   — flush
// nk doesn't model graphics handles, so animatedline-cluster
// calls target the most recent animated dataset on the current
// axes (axes::animatedDatasetIdx). animatedline returns an
// opaque scalar (1-based index) for script compat, but every
// op walks animatedDatasetIdx.
// Wire: the dataset uses ds.type='line' with isAnimated=true and
// animatedX/animatedY holding the raw points. xJson/yJson are
// rebuilt on every push.
// ────────────────────────────────────────────────────────────────
void rebuildAnimatedJson(DatasetInfo &ds)
{
    StrBuf xs, ys;
    xs << '[';
    for (keel::usize i = 0; i < ds.animatedX.size(); ++i) {
        if (i) xs << ',';
        xs << ds.animatedX[i];
    }
    xs << ']';
    ys << '[';
    for (keel::usize i = 0; i < ds.animatedY.size(); ++i) {
        if (i) ys << ',';
        ys << ds.animatedY[i];
    }
    ys << ']';
    ds.xJson = xs.take();
    ds.yJson = ys.take();
}
void animatedline_plot(keel::ArrayRef<Value> args, keel::usize /*nargout*/,
                          keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    auto *mr = gc.mr;
    auto &fm = gc.fm;
    fm.prepareForPlot();
    DatasetInfo ds;
    ds.type.assign("line");
    ds.isAnimated = true;
    // Optional initial points.
    if (args.size() >= 2 && !args[0].isChar() && !args[1].isChar()) {
        const auto &xa = args[0];
        const auto &ya = args[1];
        const keel::usize n = std::min(xa.numel(), ya.numel());
        ds.animatedX.reserve(n);
        ds.animatedY.reserve(n);
        for (keel::usize i = 0; i < n; ++i) {
            ds.animatedX.push_back(xa.elemAsDouble(i));
            ds.animatedY.push_back(ya.elemAsDouble(i));
        }
    }
    rebuildAnimatedJson(ds);
    auto &ax = fm.currentAxes();
    ax.datasets.push_back(std::move(ds));
    ax.animatedDatasetIdx = (keel::i32)ax.datasets.size() - 1;
    fm.current().modified = true;
    fm.emitModified();
    // Return 1-based dataset index as a scalar handle.
    outs[0] = Value::scalar((keel::f64)(ax.animatedDatasetIdx + 1), mr);
}
void addpoints_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    if (ax.animatedDatasetIdx < 0
        || (keel::usize)ax.animatedDatasetIdx >= ax.datasets.size()) {
        outs[0] = Value();
        return;
    }
    // Skip args[0] (handle) — addpoints(h, x, y).
    if (args.size() < 3) { outs[0] = Value(); return; }
    const auto &xa = args[1];
    const auto &ya = args[2];
    const keel::usize n = std::min(xa.numel(), ya.numel());
    auto &ds = ax.datasets[(keel::usize)ax.animatedDatasetIdx];
    ds.animatedX.reserve(ds.animatedX.size() + n);
    ds.animatedY.reserve(ds.animatedY.size() + n);
    for (keel::usize i = 0; i < n; ++i) {
        ds.animatedX.push_back(xa.elemAsDouble(i));
        ds.animatedY.push_back(ya.elemAsDouble(i));
    }
    rebuildAnimatedJson(ds);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
void clearpoints_plot(keel::ArrayRef<Value> args, keel::usize nargout,
                          keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    (void)args;
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    if (ax.animatedDatasetIdx < 0
        || (keel::usize)ax.animatedDatasetIdx >= ax.datasets.size()) {
        outs[0] = Value();
        return;
    }
    auto &ds = ax.datasets[(keel::usize)ax.animatedDatasetIdx];
    ds.animatedX.clear();
    ds.animatedY.clear();
    rebuildAnimatedJson(ds);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
void getpoints_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)args;
    auto *mr = gc.mr;
    auto &fm = gc.fm;
    auto &ax = fm.currentAxes();
    if (ax.animatedDatasetIdx < 0
        || (keel::usize)ax.animatedDatasetIdx >= ax.datasets.size()) {
        outs[0] = Value();
        if (nargout >= 2) outs[1] = Value();
        return;
    }
    const auto &ds = ax.datasets[(keel::usize)ax.animatedDatasetIdx];
    const keel::usize n = ds.animatedX.size();
    auto xv = Value::matrix(1, n, ValueType::DOUBLE, mr);
    auto yv = Value::matrix(1, n, ValueType::DOUBLE, mr);
    if (n > 0) {
        std::memcpy(xv.doubleDataMut(), ds.animatedX.data(),
                    n * sizeof(keel::f64));
        std::memcpy(yv.doubleDataMut(), ds.animatedY.data(),
                    n * sizeof(keel::f64));
    }
    outs[0] = std::move(xv);
    if (nargout >= 2) outs[1] = std::move(yv);
}
// drawnow — flush figure state. Engine emits figure JSON on
// every modification; drawnow just bumps modified + emits to be
// safe (idempotent if already up-to-date).
void drawnow_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)args; (void)nargout;
    auto &fm = gc.fm;
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
// zlabel(text) — 3-D Z-axis label.
void zlabel_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    keel::usize idx = (args.size() >= 2 && graphicsHandleIdOf(args[0]) >= 0) ? 1 : 0;
    if (idx >= args.size()) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    fm.currentAxes().zlabel = argStr(args[idx]);
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}
// zlim([z0, z1]) — 3-D Z-axis limits.
void zlim_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    queryOrSetLim(gc.fm, gc.fm.currentAxes().zlimJson, args, nargout, outs, 'z');
}
// view(az, el) — set the 3-D camera azimuth/elevation in degrees.
// Both args required for the (az, el) form; 2-element vector also
// accepted (view([az, el])). The renderer reads cfg.view on mount
// of Composite3DPlot. Stash on AxesState so it survives prepareForPlot.
void view_plot(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, GraphicsContext &gc)
{
    (void)nargout;
    if (args.empty()) { outs[0] = Value(); return; }
    auto &fm = gc.fm;
    keel::f64 az = 0, el = 0;
    bool ok = false;
    // view(2) / view(3) — preset top-down / default 3-D views.
    keel::usize idx = (args.size() >= 2 && graphicsHandleIdOf(args[0]) >= 0) ? 1 : 0;
    if (idx >= args.size()) { outs[0] = Value(); return; }
    if (args.size() - idx == 1 && args[idx].numel() == 1 && !args[idx].isChar()) {
        const keel::i32 n = (keel::i32)args[idx].toScalar();
        if (n == 2) { az = 0;     el = 90; ok = true; }    // top-down
        else if (n == 3) { az = -37.5; el = 30; ok = true; } // default 3-D
    }
    if (!ok && args.size() - idx >= 2 && args[idx].numel() == 1 && args[idx + 1].numel() == 1) {
        az = args[idx].toScalar();
        el = args[idx + 1].toScalar();
        ok = true;
    } else if (!ok && args[idx].numel() >= 2) {
        az = args[idx].elemAsDouble(0);
        el = args[idx].elemAsDouble(1);
        ok = true;
    }
    if (!ok) { outs[0] = Value(); return; }
    StrBuf os;
    os << "[" << az << "," << el << "]";
    fm.currentAxes().viewJson = os.take();
    fm.current().modified = true;
    fm.emitModified();
    outs[0] = Value();
}













struct PlotRegRow {
    const char *sub;
    const char *name;
    bool core;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, GraphicsContext &);
};

constexpr PlotRegRow kPlots[] = {
    {"layout", "figure", true, &figure_plot},
    {"layout", "close", true, &close_plot},
    {"layout", "waitbar", true, &waitbar_plot},
    {"layout", "whitebg", true, &whitebg_plot},
    {"layout", "clf", false, &clf_plot},
    {"layout", "hold", true, &hold_plot},
    {"layout", "ishold", true, &ishold_plot},
    {"layout", "colordef", true, &colordef_plot},
    {"layout", "subplot", false, &subplot_plot},
    {"layout", "tiledlayout", false, &tiledlayout_plot},
    {"layout", "nexttile", false, &nexttile_plot},
    {"layout", "title", false, &title_plot},
    {"layout", "subtitle", false, &subtitle_plot},
    {"layout", "sgtitle", false, &sgtitle_plot},
    {"layout", "xlabel", false, &xlabel_plot},
    {"layout", "annotation", false, &annotation_plot},
    {"layout", "text", false, &text_plot},
    {"layout", "ylabel", false, &ylabel_plot},
    {"layout", "xlim", false, &xlim_plot},
    {"layout", "ylim", false, &ylim_plot},
    {"layout", "grid", false, &grid_plot},
    {"layout", "legend", false, &legend_plot},
    {"layout", "colorbar", false, &colorbar_plot},
    {"layout", "axis", false, &axis_plot},
    {"layout", "xdir", false, &xdir_plot},
    {"layout", "ydir", false, &ydir_plot},
    {"layout", "caxis", false, &caxis_plot},
    {"layout", "clim", false, &clim_plot},
    {"layout", "colormap", false, &colormap_plot},
    {"layout", "colorscale", false, &colorscale_plot},
    {"layout", "yyaxis", false, &yyaxis_plot},
    {"layout", "linkaxes", false, &linkaxes_plot},
    {"layout", "rotate3d", false, &rotate3d_plot},
    {"layout", "pan3d", false, &pan3d_plot},
    {"layout", "zoom3d", false, &zoom3d_plot},
    {"layout", "daspect", false, &aspectImpl},
    {"layout", "pbaspect", false, &aspectImpl},
    {"layout", "box", false, &box_plot},
    {"layout", "axes", false, &axes_plot},
    {"layout", "gca", false, &gca_plot},
    {"layout", "gcf", false, &gcf_plot},
    {"layout", "cla", false, &cla_plot},
    {"layout", "linkprop", false, &noop_ret1},
    {"layout", "linkdata", false, &noop_ret1},
    {"layout", "animatedline", false, &animatedline_plot},
    {"layout", "addpoints", false, &addpoints_plot},
    {"layout", "clearpoints", false, &clearpoints_plot},
    {"layout", "getpoints", false, &getpoints_plot},
    {"layout", "drawnow", false, &drawnow_plot},
    {"layout", "zlabel", false, &zlabel_plot},
    {"layout", "zlim", false, &zlim_plot},
    {"layout", "view", false, &view_plot},
    {"layout", "set", false, &noop},
    {"layout", "get", false, &noop_ret1},
    {"layout", "camzoom", false, &noop},
    {"layout", "camorbit", false, &noop},
    {"layout", "campan", false, &noop},
    {"layout", "camroll", false, &noop},
    {"layout", "camdolly", false, &noop},
    {"layout", "camva", false, &noop},
    {"layout", "camtarget", false, &noop},
    {"layout", "campos", false, &noop},
    {"layout", "camup", false, &noop},
    {"layout", "camproj", false, &noop},
    {"layout", "xscale", false, &xscale_plot},
    {"layout", "yscale", false, &yscale_plot},
    {"layout", "xticks", false, &xticks_plot},
    {"layout", "yticks", false, &yticks_plot},
    {"layout", "zticks", false, &zticks_plot},
    {"layout", "xticklabels", false, &xticklabels_plot},
    {"layout", "yticklabels", false, &yticklabels_plot},
    {"layout", "zticklabels", false, &zticklabels_plot},
    {"layout", "xtickformat", false, &xtickformat_plot},
    {"layout", "ytickformat", false, &ytickformat_plot},
    {"layout", "ztickformat", false, &ztickformat_plot},
    {"layout", "xtickangle", false, &xtickangle_plot},
    {"layout", "ytickangle", false, &ytickangle_plot},
};

} // namespace

void buildLayoutPlots(keel::Array<PlotEntry> &table)
{
    for (const auto &row : kPlots)
        table.push_back(PlotEntry{row.sub, row.name, row.core, row.fn});
}

// ── Reactive-session checkpoints ─────────────────────────────────────
// See figure_manager.hpp for the contract. Truncation only ever
// removes the TAIL of datasets, so surviving chart-handle records
// keep valid indices — records pointing into the removed tail are
// tombstoned without reindexing.

FigureManager::FigureCheckpoint FigureManager::checkpointFigures() const
{
    FigureCheckpoint cp;
    for (const auto &[id, fig] : figures_) {
        keel::Array<AxesCheckpoint> ax;
        ax.reserve(fig.axes.size());
        for (const auto &a : fig.axes)
            ax.push_back(AxesCheckpoint{a.holdOn, a.datasets.size()});
        cp.figures.push_back(FigureAxesCheckpoint{id, keel::move(ax)});
    }
    return cp;
}

void FigureManager::restoreFigures(const FigureCheckpoint &cp)
{
    auto find = [&cp](keel::i32 id) -> const keel::Array<AxesCheckpoint> * {
        for (const auto &p : cp.figures)
            if (p.id == id)
                return &p.axes;
        return nullptr;
    };
    // Figures created AFTER the checkpoint are effects of statements
    // that are about to re-run (a wildcard-tail `figure;` re-executes
    // and would mint a new id on every tick). Delete them and emit
    // close markers so the renderer drops the stale cards; the replay
    // re-creates whatever the script actually needs.
    for (keel::usize idx = 0; idx < figures_.size();) {
        if (!find(figures_[idx].id)) {
            const keel::i32 id = figures_[idx].id;
            for (auto [hid, rec] : handleRegistry_)
                if (rec.figureId == id)
                    rec.deleted = true;
            figures_.remove_at(idx);
            keel::String marker = keel::format("__FIGURE_CLOSE__:{}\n", id);
            emitRawOutput_(marker);
            continue;
        }
        ++idx;
    }
    for (auto &[id, fig] : figures_) {
        const auto *snaps = find(id);
        for (keel::usize i = 0; i < fig.axes.size(); ++i) {
            auto &ax = fig.axes[i];
            if (snaps && i < snaps->size()) {
                const auto &s = (*snaps)[i];
                if (ax.holdOn != s.holdOn) {
                    ax.holdOn = s.holdOn;
                    fig.modified = true;
                }
                if (ax.datasets.size() > s.datasetCount) {
                    for (auto [hid, rec] : handleRegistry_)
                        if (!rec.deleted && rec.figureId == id
                            && rec.axesIndex == i
                            && rec.isChart()
                            && rec.datasetIndex >= s.datasetCount)
                            rec.deleted = true;
                    ax.datasets.resize(s.datasetCount);
                    fig.modified = true;
                }
            } else {
                // Axes created after the checkpoint within a surviving
                // figure: their producing statements re-run — clean slate.
                if (!ax.datasets.empty()) {
                    ax.datasets.clear();
                    fig.modified = true;
                }
                if (ax.holdOn) {
                    ax.holdOn = false;
                    fig.modified = true;
                }
            }
        }
    }
}

}  // namespace numkit
