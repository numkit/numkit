// src/bundle/src/register/graphics/export_reg.cpp
//
// saveas / exportgraphics / print — engine-side figure export (portion
// 32). The renderer itself is core-free (figure/figure_render.hpp: a
// pixel-space display list + SVG and RGB backends); this reg glue owns
// the MATLAB argument forms, turns the RGB buffer into encoded image
// bytes via the image toolbox, and writes through the engine fs.
//
// Supported output: .svg (vector) + everything imwriteToBytes encodes
// (.png/.jpg/.bmp/.tga/.pnm/.tif). Unsupported documented MATLAB
// formats (.pdf/.eps/.fig/.emf) throw a clear "not supported" — filed
// in bugs/opened/graphics/figure-export-gaps.md, never silent.

#include <keel/array.h>
#include <keel/algo.h>
#include <keel/math.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <keel/utf8.h>
#include <numkit/core/engine.hpp>
#include <numkit/value/error.hpp>
#include <numkit/core/types.hpp>
#include <numkit/graphics/figure_manager.hpp>
#include <numkit/graphics/figure_render.hpp>
#include <numkit/graphics/graphics_handle.hpp>
#include <numkit/image/io/io.hpp>
#include <numkit/value/argcheck.hpp>

#include <keel/number.h>

namespace numkit {

namespace {

keel::String lowerExtOf(keel::StringRef path)
{
    keel::usize dot = path.rfind('.');
    if (dot == keel::kInvalidIndex)
        return keel::String{};
    keel::String e = keel::String::from(path.substr(dot + 1));
    e.to_ascii_lower_in_place();
    return e;
}

// PNG + base64 data-URL embedder for SVG <image> elements. The buffer
// arrives top-down row-major from figrender::imageCmdRgb; it goes to
// imwriteToBytes as a column-major uint8 H×W×3 Value (the transpose is
// the same column-major contract the raster export path uses).
const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
figrender::SvgImageEmbedder svgPngEmbedder(
    keel::Allocator *mr)
{
    return [mr](keel::ArrayRef<keel::u8> rgb, keel::i32 w, keel::i32 h) -> keel::String {
        const keel::usize Hp = (keel::usize)h, Wp = (keel::usize)w;
        Value A = Value::matrix3d(Hp, Wp, 3, ValueType::UINT8, mr);
        keel::u8 *d = A.uint8DataMut();
        for (keel::usize r = 0; r < Hp; ++r)
            for (keel::usize c = 0; c < Wp; ++c)
                for (keel::usize ch = 0; ch < 3; ++ch)
                    d[r + c * Hp + ch * Hp * Wp] =
                        rgb[(r * Wp + c) * 3 + ch];
        keel::String png = image::imwriteToBytes(A, "png", mr);
        keel::String b64 = keel::String::from("data:image/png;base64,");
        b64.reserve(b64.size() + (png.size() / 3 + 1) * 4);
        keel::usize i = 0;
        for (; i + 2 < png.size(); i += 3) {
            // Cast through uint8_t FIRST: png[i] is signed char, so
            // (char)0xFF << 16 sign-extends and corrupts every byte
            // >= 0x80 (most of any PNG).
            keel::u32 v = ((keel::u32)(keel::u8)png[i] << 16)
                            | ((keel::u32)(keel::u8)png[i + 1] << 8)
                            | (keel::u8)png[i + 2];
            b64.push_back(kBase64[(v >> 18) & 63]);
            b64.push_back(kBase64[(v >> 12) & 63]);
            b64.push_back(kBase64[(v >> 6) & 63]);
            b64.push_back(kBase64[v & 63]);
        }
        keel::usize rem = png.size() - i;
        keel::u8 b0 = rem >= 1 ? (keel::u8)png[i] : 0;
        keel::u8 b1 = rem >= 2 ? (keel::u8)png[i + 1] : 0;
        if (rem == 1) {
            b64.push_back(kBase64[b0 >> 2]);
            b64.push_back(kBase64[(b0 & 3) << 4]);
            b64.append("==");
        } else if (rem == 2) {
            b64.push_back(kBase64[b0 >> 2]);
            b64.push_back(kBase64[((b0 & 3) << 4) | (b1 >> 4)]);
            b64.push_back(kBase64[((b1 & 15) << 2)]);
            b64.push_back('=');
        }
        return b64;
    };
}

// Resolve the TARGET FIGURE from any graphics handle / figure number.
// exportgraphics(gca)/saveas(chart) scope to the axes in MATLAB; the
// portion-32 renderer works per-figure, so axes/chart targets map to
// their containing figure (gap noted in figure-export-gaps.md).
const FigureState *targetFigure(Engine &engine, const Value &v)
{
    auto &fm = engine.figureManager();
    const keel::i32 hid = graphicsHandleIdOf(v);
    if (hid >= 0) {
        auto *rec = fm.findHandle(hid);
        if (!rec)
            throw numkit::Error("Invalid or deleted object.");
        return fm.figureOf(rec->figureId);
    }
    if (v.isScalar())
        return fm.figureOf((keel::i32)v.toScalar());
    return nullptr;
}

struct ExportArgs {
    const FigureState *fig = nullptr;
    keel::String path;
    keel::String ext;       // from filename or explicit format
    keel::i32 dpi = 150;          // MATLAB png default; scales the 840×630 base
};

// Shared argument walk for all three entry points: a handle/number
// target, one filename, format flags ('-dpng', 'png'), '-r###'
// resolution, and (ignored, accepted) Name-Value pairs.
ExportArgs parseExportArgs(Engine &engine, keel::ArrayRef<Value> args,
                            const char *who, bool needTarget)
{
    ExportArgs out;
    bool havePath = false, haveTarget = false;
    for (keel::usize i = 0; i < args.size(); ++i) {
        const Value &a = args[i];
        if (a.isObject() || (a.isScalar() && !a.isChar())) {
            if (!haveTarget) {
                out.fig = targetFigure(engine, a);
                if (!out.fig)
                    throw numkit::Error(keel::format(
                        "{}: first argument must be a graphics handle or figure number",
                        who).c_str());
                haveTarget = true;
            }
            continue;
        }
        if (!a.isChar() && !a.isString())
            continue;   // numeric noise (e.g. ignored Name-Value values)
        keel::String s = a.toString();
        keel::String sl = s.clone();
        sl.to_ascii_lower_in_place();
        if (!s.empty() && s[0] == '-') {
            // print-style flags: -dpng / -dsvg / -r150 / -ochr…
            if (sl.size() > 2 && sl[1] == 'd')
                out.ext = keel::String::from(sl.substr(2));
            else if (sl.size() > 2 && sl[1] == 'r') {
                keel::i32 dpiVal = 0;
                if (auto parsed = keel::parse_i64(sl.substr(2)); parsed.is_ok())
                    dpiVal = keel::as<keel::i32>(parsed.value());
                out.dpi = keel::max(30, dpiVal);
            }
            // other print options (-painters, -opengl, -bestfit, …)
            // accepted and ignored
            continue;
        }
        // A char with an extension that is not a known bare FORMAT is
        // the filename; a bare known format is a format override.
        static constexpr keel::StringRef kFormats[] = {
            "png", "svg", "jpg", "jpeg", "bmp", "tga", "pnm", "ppm",
            "tif", "tiff", "pdf", "eps", "epsc", "eps2", "epsc2",
        };
        const bool isBareFormat = sl.is_one_of(keel::ArrayRef<keel::StringRef>(kFormats));
        if (!havePath && !isBareFormat) {
            out.path = keel::move(s);
            havePath = true;
        } else if (isBareFormat) {
            out.ext = keel::move(sl);
        }
    }
    if (needTarget && !out.fig)
        out.fig = &engine.figureManager().current();
    if (!havePath)
        throw numkit::Error(keel::format("{}: a filename is required", who).c_str());
    if (out.ext.empty())
        out.ext = lowerExtOf(out.path);
    if (out.ext.empty())
        throw numkit::Error(keel::format(
            "{}: cannot infer the output format from '{}'", who, out.path).c_str());
    return out;
}

void runExport(Engine &engine, const ExportArgs &ea, const char *who)
{
    if (!ea.fig)
        throw numkit::Error(keel::format("{}: no figure to export", who).c_str());
    // Base canvas 840×630 (MATLAB's 560×420 pt @150dpi ≈ 825×619);
    // Resolution scales it linearly.
    const keel::i32 W = keel::max(64, (keel::i32)(840.0 * ea.dpi / 150.0));
    const keel::i32 H = keel::max(64, (keel::i32)(630.0 * ea.dpi / 150.0));

    auto rp = engine.resolvePath(ea.path);
    if (!rp.fs)
        throw numkit::Error(keel::format("{}: cannot resolve '{}'", who, ea.path).c_str());

    if (ea.ext == "svg") {
        rp.fs->writeFile(rp.path,
                         figrender::renderFigureSvg(*ea.fig, W, H,
                                                     svgPngEmbedder(
                                                         engine.allocator())));
        return;
    }
    if (ea.ext == "pdf") {
        rp.fs->writeFileBytes(rp.path,
                              figrender::renderFigurePdf(*ea.fig, W, H));
        return;
    }
    if (ea.ext.is_one_of({"eps", "epsc", "eps2", "epsc2"})) {
        rp.fs->writeFile(rp.path,
                         figrender::renderFigureEps(*ea.fig, W, H));
        return;
    }
    if (ea.ext.is_one_of({"fig", "emf", "hdf"})) {
        throw numkit::Error(keel::format(
            "{}: format '.{}' is not supported yet (supported: .png .svg .jpg "
            ".bmp .tga .pnm .tif) — bugs/opened/graphics/figure-export-gaps.md",
            who, ea.ext).c_str());
    }
    // Raster formats → RGB buffer → image toolbox encoder.
    keel::Array<keel::u8> rgb =
        figrender::renderFigureRgb(*ea.fig, W, H);
    const keel::usize Hp = (keel::usize)H, Wp = (keel::usize)W;
    Value A = Value::matrix3d(Hp, Wp, 3, ValueType::UINT8,
                              engine.allocator());
    keel::u8 *d = A.uint8DataMut();
    // Value is COLUMN-MAJOR (MATLAB layout): d[r + c*H + ch*H*W]. The
    // raster buffer is row-major top-down RGB — transpose into place
    // (filling it linearly scrambles the image into crisscrossing
    // diagonal noise).
    for (keel::usize r = 0; r < Hp; ++r)
        for (keel::usize c = 0; c < Wp; ++c)
            for (keel::usize ch = 0; ch < 3; ++ch)
                d[r + c * Hp + ch * Hp * Wp] =
                    rgb[(r * Wp + c) * 3 + ch];
    keel::String bytes = image::imwriteToBytes(A, ea.ext, engine.allocator());
    rp.fs->writeFileBytes(rp.path, bytes);
}

void saveas_reg(keel::ArrayRef<Value> args, keel::usize,
                keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 3, "saveas");
    // saveas(filename) targets the current figure.
    bool firstIsTarget = args[0].isObject()
                         || (args[0].isScalar() && !args[0].isChar());
    ExportArgs ea = parseExportArgs(
        coreEngine(ctx), args, "saveas", /*needTarget=*/!firstIsTarget);
    runExport(coreEngine(ctx), ea, "saveas");
    outs[0] = Value();
}

void exportgraphics_reg(keel::ArrayRef<Value> args, keel::usize,
                        keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 2, "exportgraphics");
    ExportArgs ea = parseExportArgs(coreEngine(ctx), args,
                                    "exportgraphics",
                                    /*needTarget=*/false);
    runExport(coreEngine(ctx), ea, "exportgraphics");
    outs[0] = Value();
}

void print_reg(keel::ArrayRef<Value> args, keel::usize,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, "print");
    ExportArgs ea = parseExportArgs(coreEngine(ctx), args, "print",
                                    /*needTarget=*/true);
    runExport(coreEngine(ctx), ea, "print");
    outs[0] = Value();
}

void graphicsNoop_reg(keel::ArrayRef<Value>, keel::usize,
                      keel::MutArrayRef<Value> outs, CallContext &)
{
    if (!outs.empty()) outs[0] = Value();
}

void light_reg(keel::ArrayRef<Value>, keel::usize,
               keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    if (!outs.empty()) outs[0] = Value::scalar(1.0, ctx.engine->allocator());
}

void getframe_reg(keel::ArrayRef<Value> args, keel::usize,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 2, "getframe");
    auto *mr = ctx.engine->allocator();
    const FigureState *fig = nullptr;
    if (!args.empty()) fig = targetFigure(coreEngine(ctx), args[0]);
    if (!fig) fig = &ctx.engine->figureManager().current();
    const keel::i32 W = 560, H = 420;
    keel::Array<keel::u8> rgb = figrender::renderFigureRgb(*fig, W, H);
    const keel::usize Hp = (keel::usize)H, Wp = (keel::usize)W;
    Value A = Value::matrix3d(Hp, Wp, 3, ValueType::UINT8, mr);
    keel::u8 *d = A.uint8DataMut();
    for (keel::usize r = 0; r < Hp; ++r)
        for (keel::usize c = 0; c < Wp; ++c)
            for (keel::usize ch = 0; ch < 3; ++ch)
                d[r + c * Hp + ch * Hp * Wp] = rgb[(r * Wp + c) * 3 + ch];
    Value s = Value::structure(mr);
    s.field("cdata") = keel::move(A);
    s.field("colormap") = Value::Empty;
    if (!outs.empty()) outs[0] = keel::move(s);
}

void frame2im_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 1, "frame2im");
    auto *mr = ctx.engine->allocator();
    const Value &f = args[0];
    Value cdata = Value::matrix3d(1, 1, 3, ValueType::UINT8, mr);
    Value cmap = Value::Empty;
    if (f.isStruct() && f.numel() >= 1) {
        if (f.hasField("cdata"))
            cdata = f.field("cdata");
        if (f.hasField("colormap"))
            cmap = f.field("colormap");
    }
    if (!outs.empty()) outs[0] = keel::move(cdata);
    if (nargout >= 2 && outs.size() >= 2) outs[1] = keel::move(cmap);
}

void im2frame_reg(keel::ArrayRef<Value> args, keel::usize,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 1, 2, "im2frame");
    auto *mr = ctx.engine->allocator();
    Value s = Value::structure(mr);
    s.field("cdata") = args[0];
    s.field("colormap") = (args.size() >= 2) ? args[1] : Value::Empty;
    if (!outs.empty()) outs[0] = keel::move(s);
}

void chartNoop_reg(keel::ArrayRef<Value>, keel::usize nargout,
                   keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    ctx.engine->figureManager().current();
    if (!outs.empty())
        outs[0] = (nargout >= 1) ? Value::scalar(1.0, ctx.engine->allocator()) : Value();
}

void dendrogram_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    ctx.engine->figureManager().current();
    auto *mr = ctx.engine->allocator();
    keel::usize m = (!args.empty() && args[0].dims().rows() > 0) ? args[0].dims().rows() : 1;
    if (nargout == 0) {
        if (!outs.empty()) outs[0] = Value();
        return;
    }
    Value H = Value::matrix(m, 1, ValueType::DOUBLE, mr);
    for (keel::usize i = 0; i < m; ++i) H.doubleDataMut()[i] = 1.0;
    outs[0] = keel::move(H);
    if (nargout >= 2 && outs.size() >= 2) {
        Value T = Value::matrix(m + 1, 1, ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i <= m; ++i) T.doubleDataMut()[i] = keel::as<keel::f64>(i + 1);
        outs[1] = keel::move(T);
    }
    if (nargout >= 3 && outs.size() >= 3) {
        Value P = Value::matrix(1, m + 1, ValueType::DOUBLE, mr);
        for (keel::usize i = 0; i <= m; ++i) P.doubleDataMut()[i] = keel::as<keel::f64>(i + 1);
        outs[2] = keel::move(P);
    }
}

void evalColormap(const char *cname, keel::ArrayRef<Value> args,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    const keel::StringRef paletteName(cname);
    argcheck::checkNargin(args.size(), 0, 1, cname);
    auto *mr = ctx.engine->allocator();
    keel::usize m = 256;
    if (!args.empty() && args[0].isScalar() && !args[0].isChar() && !args[0].isString()) {
        keel::f64 mv = args[0].toScalar();
        m = (mv >= 0.0) ? keel::as<keel::usize>(mv) : 0;
    }
    Value C = Value::matrix(m, 3, ValueType::DOUBLE, mr);
    if (m > 0) {
        keel::f64 *d = C.doubleDataMut();
        if (paletteName == "jet") {
            keel::fill(d, d + 3 * m, 0.0);
            const keel::i32 mi = keel::as<keel::i32>(m);
            const keel::i32 n = (mi + 3) / 4;
            const keel::i32 ulen = n + (n - 1) + n;
            auto u_at = [&](keel::i32 idx1) -> keel::f64 {
                if (idx1 <= n) return keel::f64(idx1) / keel::f64(n);
                if (idx1 <= 2 * n - 1) return 1.0;
                return keel::f64(2 * n - 1 + n - (idx1 - 1)) / keel::f64(n);
            };
            const keel::i32 gbase = (n + 1) / 2 - (mi % 4 == 1 ? 1 : 0);
            keel::i32 rcount = 0, gcount = 0, bcount = 0;
            for (keel::i32 k = 1; k <= ulen; ++k) {
                const keel::i32 g = gbase + k;
                const keel::i32 r = g + n;
                const keel::i32 b = g - n;
                if (r >= 1 && r <= mi) {
                    ++rcount;
                    d[0 * m + keel::as<keel::usize>(r - 1)] = u_at(rcount);
                }
                if (g >= 1 && g <= mi) {
                    ++gcount;
                    d[1 * m + keel::as<keel::usize>(g - 1)] = u_at(gcount);
                }
                if (b >= 1 && b <= mi) ++bcount;
            }
            keel::i32 bsofar = 0;
            for (keel::i32 k = 1; k <= ulen; ++k) {
                const keel::i32 g = gbase + k;
                const keel::i32 b = g - n;
                if (b >= 1 && b <= mi) {
                    ++bsofar;
                    d[2 * m + keel::as<keel::usize>(b - 1)] = u_at(ulen - bcount + bsofar);
                }
            }
        } else if (paletteName == "gray") {
            for (keel::usize i = 0; i < m; ++i) {
                keel::f64 g = (m == 1) ? 0.0 : keel::f64(i) / keel::f64(m - 1);
                d[i] = d[i + m] = d[i + 2 * m] = g;
            }
        } else if (paletteName == "cool") {
            for (keel::usize i = 0; i < m; ++i) {
                keel::f64 r = (m == 1) ? 0.0 : keel::f64(i) / keel::f64(m - 1);
                d[i] = r; d[i + m] = 1.0 - r; d[i + 2 * m] = 1.0;
            }
        } else if (paletteName == "hot") {
            const keel::i32 mi = keel::as<keel::i32>(m);
            const keel::i32 n = (3 * mi) / 8;
            for (keel::i32 i = 0; i < mi; ++i) {
                d[i]         = (i < n) ? keel::f64(i + 1) / keel::f64(n) : 1.0;
                d[i + m]     = (i < n) ? 0.0 : (i < 2 * n ? keel::f64(i - n + 1) / keel::f64(n) : 1.0);
                d[i + 2 * m] = (i < 2 * n) ? 0.0 : keel::f64(i - 2 * n + 1) / keel::f64(mi - 2 * n);
            }
        } else {
            keel::i32 tableN = 256;
            const keel::u8 *tbl = figrender::namedPalette(paletteName, tableN);
            for (keel::usize i = 0; i < m; ++i) {
                keel::f64 t = (m == 1) ? 0.0 : keel::as<keel::f64>(i) / keel::as<keel::f64>(m - 1);
                keel::i32 idx = keel::min(tableN - 1, keel::max(0, keel::as<keel::i32>(keel::round_i64(t * (tableN - 1)))));
                d[i]         = tbl[idx * 3 + 0] / 255.0;
                d[i + m]     = tbl[idx * 3 + 1] / 255.0;
                d[i + 2 * m] = tbl[idx * 3 + 2] / 255.0;
            }
        }
    }
    if (!outs.empty()) outs[0] = keel::move(C);
}

template <const char *Palette>
void colormap_reg(keel::ArrayRef<Value> args, keel::usize,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    evalColormap(Palette, args, outs, ctx);
}

constexpr char kPalParula[]  = "parula";
constexpr char kPalTurbo[]   = "turbo";
constexpr char kPalJet[]     = "jet";
constexpr char kPalHot[]     = "hot";
constexpr char kPalCool[]    = "cool";
constexpr char kPalGray[]    = "gray";
constexpr char kPalBone[]    = "bone";
constexpr char kPalCopper[]  = "copper";
constexpr char kPalSpring[]  = "spring";
constexpr char kPalSummer[]  = "summer";
constexpr char kPalAutumn[]  = "autumn";
constexpr char kPalWinter[]  = "winter";
constexpr char kPalHsv[]     = "hsv";
constexpr char kPalViridis[] = "viridis";
constexpr char kPalPink[]    = "pink";
constexpr char kPalLines[]   = "lines";

struct ExportRegEntry {
    const char *name;
    void (*fn)(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value>, CallContext &);
};

constexpr ExportRegEntry kExportFns[] = {
    {"saveas",         &saveas_reg},
    {"exportgraphics", &exportgraphics_reg},
    {"print",          &print_reg},
    {"shading",        &graphicsNoop_reg},
    {"lighting",       &graphicsNoop_reg},
    {"material",       &graphicsNoop_reg},
    {"movie",          &graphicsNoop_reg},
    {"light",          &light_reg},
    {"getframe",       &getframe_reg},
    {"frame2im",       &frame2im_reg},
    {"im2frame",       &im2frame_reg},
    {"imtool",         &chartNoop_reg},
    {"implay",         &chartNoop_reg},
    {"boxchart",       &chartNoop_reg},
    {"heatmap",        &chartNoop_reg},
    {"probplot",       &chartNoop_reg},
    {"newplot",        &chartNoop_reg},
    {"zoom",           &chartNoop_reg},
    {"pan",            &chartNoop_reg},
    {"rotate3d",       &chartNoop_reg},
    {"dendrogram",     &dendrogram_reg},
    {"parula",         &colormap_reg<kPalParula>},
    {"turbo",          &colormap_reg<kPalTurbo>},
    {"jet",            &colormap_reg<kPalJet>},
    {"hot",            &colormap_reg<kPalHot>},
    {"cool",           &colormap_reg<kPalCool>},
    {"gray",           &colormap_reg<kPalGray>},
    {"bone",           &colormap_reg<kPalBone>},
    {"copper",         &colormap_reg<kPalCopper>},
    {"spring",         &colormap_reg<kPalSpring>},
    {"summer",         &colormap_reg<kPalSummer>},
    {"autumn",         &colormap_reg<kPalAutumn>},
    {"winter",         &colormap_reg<kPalWinter>},
    {"hsv",            &colormap_reg<kPalHsv>},
    {"viridis",        &colormap_reg<kPalViridis>},
    {"pink",           &colormap_reg<kPalPink>},
    {"lines",          &colormap_reg<kPalLines>},
};

} // anonymous namespace

void registerFigureExport(Engine &engine)
{
    for (const auto &entry : kExportFns)
        engine.registerFunction("", entry.name, entry.fn);
}

} // namespace numkit
