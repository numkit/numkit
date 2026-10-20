// src/bundle/src/register/graphics/export_reg.cpp
//
// saveas / exportgraphics / print â€” engine-side figure export (portion
// 32). The renderer itself is core-free (figure/figure_render.hpp: a
// pixel-space display list + SVG and RGB backends); this reg glue owns
// the MATLAB argument forms, turns the RGB buffer into encoded image
// bytes via the image toolbox, and writes through the engine fs.
//
// Supported output: .svg (vector) + everything imwriteToBytes encodes
// (.png/.jpg/.bmp/.tga/.pnm/.tif). Unsupported documented MATLAB
// formats (.pdf/.eps/.fig/.emf) throw a clear "not supported" â€” filed
// in bugs/opened/graphics/figure-export-gaps.md, never silent.

#include <numkit/core/engine.hpp>
#include <numkit/core/types.hpp>
#include <numkit/core/figure_manager.hpp>
#include <numkit/core/figure_render.hpp>
#include <numkit/graphics/graphics_handle.hpp>
#include <numkit/image/io/io.hpp>
#include <numkit/value/argcheck.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace numkit {

namespace {

std::string lowerExtOf(const std::string &path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "";
    std::string e = path.substr(dot + 1);
    for (char &c : e)
        c = (char)std::tolower((unsigned char)c);
    return e;
}

// PNG + base64 data-URL embedder for SVG <image> elements. The buffer
// arrives top-down row-major from figrender::imageCmdRgb; it goes to
// imwriteToBytes as a column-major uint8 HÃ—WÃ—3 Value (the transpose is
// the same column-major contract the raster export path uses).
const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
figrender::SvgImageEmbedder svgPngEmbedder(
    std::pmr::memory_resource *mr)
{
    return [mr](const std::vector<std::uint8_t> &rgb, int w, int h) {
        const size_t Hp = (size_t)h, Wp = (size_t)w;
        Value A = Value::matrix3d(Hp, Wp, 3, ValueType::UINT8, mr);
        std::uint8_t *d = A.uint8DataMut();
        for (size_t r = 0; r < Hp; ++r)
            for (size_t c = 0; c < Wp; ++c)
                for (size_t ch = 0; ch < 3; ++ch)
                    d[r + c * Hp + ch * Hp * Wp] =
                        rgb[(r * Wp + c) * 3 + ch];
        std::string png = image::imwriteToBytes(A, "png", mr);
        std::string b64 = "data:image/png;base64,";
        b64.reserve(b64.size() + (png.size() / 3 + 1) * 4);
        size_t i = 0;
        for (; i + 2 < png.size(); i += 3) {
            // Cast through uint8_t FIRST: png[i] is signed char, so
            // (char)0xFF << 16 sign-extends and corrupts every byte
            // >= 0x80 (most of any PNG).
            std::uint32_t v = ((std::uint32_t)(std::uint8_t)png[i] << 16)
                            | ((std::uint32_t)(std::uint8_t)png[i + 1] << 8)
                            | (std::uint8_t)png[i + 2];
            b64 += kBase64[(v >> 18) & 63];
            b64 += kBase64[(v >> 12) & 63];
            b64 += kBase64[(v >> 6) & 63];
            b64 += kBase64[v & 63];
        }
        size_t rem = png.size() - i;
        std::uint8_t b0 = rem >= 1 ? (std::uint8_t)png[i] : 0;
        std::uint8_t b1 = rem >= 2 ? (std::uint8_t)png[i + 1] : 0;
        if (rem == 1) {
            b64 += kBase64[b0 >> 2];
            b64 += kBase64[(b0 & 3) << 4];
            b64 += "==";
        } else if (rem == 2) {
            b64 += kBase64[b0 >> 2];
            b64 += kBase64[((b0 & 3) << 4) | (b1 >> 4)];
            b64 += kBase64[((b1 & 15) << 2)];
            b64 += '=';
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
    const int hid = graphicsHandleIdOf(v);
    if (hid >= 0) {
        auto *rec = fm.findHandle(hid);
        if (!rec)
            throw std::runtime_error("Invalid or deleted object.");
        return fm.figureOf(rec->figureId);
    }
    if (v.isScalar())
        return fm.figureOf((int)v.toScalar());
    return nullptr;
}

struct ExportArgs {
    const FigureState *fig = nullptr;
    std::string path;
    std::string ext;        // from filename or explicit format
    int dpi = 150;          // MATLAB png default; scales the 840Ã—630 base
};

// Shared argument walk for all three entry points: a handle/number
// target, one filename, format flags ('-dpng', 'png'), '-r###'
// resolution, and (ignored, accepted) Name-Value pairs.
ExportArgs parseExportArgs(Engine &engine, Span<const Value> args,
                            const char *who, bool needTarget)
{
    ExportArgs out;
    bool havePath = false, haveTarget = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const Value &a = args[i];
        if (a.isObject() || (a.isScalar() && !a.isChar())) {
            if (!haveTarget) {
                out.fig = targetFigure(engine, a);
                if (!out.fig)
                    throw std::runtime_error(std::string(who)
                        + ": first argument must be a graphics handle or figure number");
                haveTarget = true;
            }
            continue;
        }
        if (!a.isChar() && !a.isString())
            continue;   // numeric noise (e.g. ignored Name-Value values)
        std::string s = a.toString();
        std::string sl = s;
        for (char &c : sl)
            c = (char)std::tolower((unsigned char)c);
        if (!s.empty() && s[0] == '-') {
            // print-style flags: -dpng / -dsvg / -r150 / -ochrâ€¦
            if (sl.size() > 2 && sl[1] == 'd')
                out.ext = sl.substr(2);
            else if (sl.size() > 2 && sl[1] == 'r')
                out.dpi = std::max(30, std::atoi(sl.c_str() + 2));
            // other print options (-painters, -opengl, -bestfit, â€¦)
            // accepted and ignored
            continue;
        }
        // A char with an extension that is not a known bare FORMAT is
        // the filename; a bare known format is a format override.
        static const char *kFormats[] = {"png", "svg", "jpg", "jpeg",
                                         "bmp", "tga", "pnm", "ppm",
                                         "tif", "tiff", "pdf", "eps",
                                         "epsc", "eps2", "epsc2"};
        bool isBareFormat = false;
        for (const char *f : kFormats)
            if (sl == f) {
                isBareFormat = true;
                break;
            }
        if (!havePath && !isBareFormat) {
            out.path = s;
            havePath = true;
        } else if (isBareFormat) {
            out.ext = sl;
        }
    }
    if (needTarget && !out.fig)
        out.fig = &engine.figureManager().current();
    if (!havePath)
        throw std::runtime_error(std::string(who)
            + ": a filename is required");
    if (out.ext.empty())
        out.ext = lowerExtOf(out.path);
    if (out.ext.empty())
        throw std::runtime_error(std::string(who)
            + ": cannot infer the output format from '" + out.path + "'");
    return out;
}

void runExport(Engine &engine, const ExportArgs &ea, const char *who)
{
    if (!ea.fig)
        throw std::runtime_error(std::string(who) + ": no figure to export");
    // Base canvas 840Ã—630 (MATLAB's 560Ã—420 pt @150dpi â‰ˆ 825Ã—619);
    // Resolution scales it linearly.
    const int W = std::max(64, (int)(840.0 * ea.dpi / 150.0));
    const int H = std::max(64, (int)(630.0 * ea.dpi / 150.0));

    auto rp = engine.resolvePath(ea.path);
    if (!rp.fs)
        throw std::runtime_error(std::string(who) + ": cannot resolve '"
                                 + ea.path + "'");

    if (ea.ext == "svg") {
        rp.fs->writeFile(rp.path,
                         figrender::renderFigureSvg(*ea.fig, W, H,
                                                     svgPngEmbedder(
                                                         engine.resource())));
        return;
    }
    if (ea.ext == "pdf") {
        rp.fs->writeFileBytes(rp.path,
                              figrender::renderFigurePdf(*ea.fig, W, H));
        return;
    }
    if (ea.ext == "eps" || ea.ext == "epsc" || ea.ext == "eps2" || ea.ext == "epsc2") {
        rp.fs->writeFile(rp.path,
                         figrender::renderFigureEps(*ea.fig, W, H));
        return;
    }
    if (ea.ext == "fig" || ea.ext == "emf" || ea.ext == "hdf") {
        throw std::runtime_error(std::string(who) + ": format '."
            + ea.ext + "' is not supported yet (supported: .png .svg .jpg "
              ".bmp .tga .pnm .tif) â€” bugs/opened/graphics/"
              "figure-export-gaps.md");
    }
    // Raster formats â†’ RGB buffer â†’ image toolbox encoder.
    std::vector<std::uint8_t> rgb =
        figrender::renderFigureRgb(*ea.fig, W, H);
    const size_t Hp = (size_t)H, Wp = (size_t)W;
    Value A = Value::matrix3d(Hp, Wp, 3, ValueType::UINT8,
                              engine.resource());
    std::uint8_t *d = A.uint8DataMut();
    // Value is COLUMN-MAJOR (MATLAB layout): d[r + c*H + ch*H*W]. The
    // raster buffer is row-major top-down RGB â€” transpose into place
    // (filling it linearly scrambles the image into crisscrossing
    // diagonal noise).
    for (size_t r = 0; r < Hp; ++r)
        for (size_t c = 0; c < Wp; ++c)
            for (size_t ch = 0; ch < 3; ++ch)
                d[r + c * Hp + ch * Hp * Wp] =
                    rgb[(r * Wp + c) * 3 + ch];
    std::string bytes = image::imwriteToBytes(A, ea.ext, engine.resource());
    rp.fs->writeFileBytes(rp.path, bytes);
}

} // anonymous namespace

void registerFigureExport(Engine &engine)
{
    // saveas(h, filename [, fmt])
    {
        ExternalFunc fn = [&engine](Span<const Value> args, size_t,
                                    Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 3, "saveas");
            // saveas(filename) targets the current figure.
            bool firstIsTarget = args[0].isObject()
                                 || (args[0].isScalar() && !args[0].isChar());
            ExportArgs ea = parseExportArgs(
                *ctx.engine, args, "saveas", /*needTarget=*/!firstIsTarget);
            runExport(*ctx.engine, ea, "saveas");
            outs[0] = Value();
        };
        engine.registerFunction("", "saveas", std::move(fn));
    }
    // exportgraphics(obj, filename [, Name,Value])
    {
        ExternalFunc fn = [](Span<const Value> args, size_t,
                             Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 2, "exportgraphics");
            ExportArgs ea = parseExportArgs(*ctx.engine, args,
                                            "exportgraphics",
                                            /*needTarget=*/false);
            runExport(*ctx.engine, ea, "exportgraphics");
            outs[0] = Value();
        };
        engine.registerFunction("", "exportgraphics", std::move(fn));
    }
    // print(fig, filename, '-dpng' [, '-r###' â€¦]) and the command form
    // `print -dpng file.png` (glue args arrive the same way).
    {
        ExternalFunc fn = [&engine](Span<const Value> args, size_t,
                                    Span<Value> outs, CallContext &ctx) {
            (void)engine;
            argcheck::checkNargin(args.size(), 1, "print");
            ExportArgs ea = parseExportArgs(*ctx.engine, args, "print",
                                            /*needTarget=*/true);
            runExport(*ctx.engine, ea, "print");
            outs[0] = Value();
        };
        engine.registerFunction("", "print", std::move(fn));
    }
    // shading('interp'|'flat'|'faceted') / lighting / material / movie
    {
        ExternalFunc fn = [](Span<const Value>, size_t,
                             Span<Value> outs, CallContext &) {
            if (!outs.empty()) outs[0] = Value();
        };
        engine.registerFunction("", "shading", fn);
        engine.registerFunction("", "lighting", fn);
        engine.registerFunction("", "material", fn);
        engine.registerFunction("", "movie", fn);
    }
    // light(...) -> returns handle or scalar
    {
        ExternalFunc fn = [](Span<const Value>, size_t,
                             Span<Value> outs, CallContext &ctx) {
            if (!outs.empty()) outs[0] = Value::scalar(1.0, ctx.engine->resource());
        };
        engine.registerFunction("", "light", std::move(fn));
    }
    // getframe([h]) -> returns struct('cdata', HxWx3 uint8, 'colormap', [])
    {
        ExternalFunc fn = [](Span<const Value> args, size_t,
                             Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 0, 2, "getframe");
            auto *mr = ctx.engine->resource();
            const FigureState *fig = nullptr;
            if (!args.empty()) fig = targetFigure(*ctx.engine, args[0]);
            if (!fig) fig = &ctx.engine->figureManager().current();
            const int W = 560, H = 420;
            std::vector<std::uint8_t> rgb = figrender::renderFigureRgb(*fig, W, H);
            const size_t Hp = (size_t)H, Wp = (size_t)W;
            Value A = Value::matrix3d(Hp, Wp, 3, ValueType::UINT8, mr);
            std::uint8_t *d = A.uint8DataMut();
            for (size_t r = 0; r < Hp; ++r)
                for (size_t c = 0; c < Wp; ++c)
                    for (size_t ch = 0; ch < 3; ++ch)
                        d[r + c * Hp + ch * Hp * Wp] = rgb[(r * Wp + c) * 3 + ch];
            Value s = Value::structure(mr);
            s.field("cdata") = std::move(A);
            s.field("colormap") = Value::Empty;
            if (!outs.empty()) outs[0] = std::move(s);
        };
        engine.registerFunction("", "getframe", std::move(fn));
    }
    // frame2im(F) -> [X, Map] and im2frame(X [, map]) -> F
    {
        ExternalFunc f2i = [](Span<const Value> args, size_t nargout,
                              Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 1, "frame2im");
            auto *mr = ctx.engine->resource();
            const Value &f = args[0];
            Value cdata = Value::matrix3d(1, 1, 3, ValueType::UINT8, mr);
            Value cmap = Value::Empty;
            if (f.isStruct() && f.numel() >= 1) {
                if (f.hasField("cdata"))
                    cdata = f.field("cdata");
                if (f.hasField("colormap"))
                    cmap = f.field("colormap");
            }
            if (!outs.empty()) outs[0] = std::move(cdata);
            if (nargout >= 2 && outs.size() >= 2) outs[1] = std::move(cmap);
        };
        engine.registerFunction("", "frame2im", std::move(f2i));

        ExternalFunc i2f = [](Span<const Value> args, size_t,
                              Span<Value> outs, CallContext &ctx) {
            argcheck::checkNargin(args.size(), 1, 2, "im2frame");
            auto *mr = ctx.engine->resource();
            Value s = Value::structure(mr);
            s.field("cdata") = args[0];
            s.field("colormap") = (args.size() >= 2) ? args[1] : Value::Empty;
            if (!outs.empty()) outs[0] = std::move(s);
        };
        engine.registerFunction("", "im2frame", std::move(i2f));
    }
    // High-level chart & viewer helpers: imtool, implay, boxchart, heatmap, probplot, dendrogram
    {
        ExternalFunc chartNoop = [](Span<const Value>, size_t nargout,
                                    Span<Value> outs, CallContext &ctx) {
            ctx.engine->figureManager().current();
            if (!outs.empty())
                outs[0] = (nargout >= 1) ? Value::scalar(1.0, ctx.engine->resource()) : Value();
        };
        engine.registerFunction("", "imtool", chartNoop);
        engine.registerFunction("", "implay", chartNoop);
        engine.registerFunction("", "boxchart", chartNoop);
        engine.registerFunction("", "heatmap", chartNoop);
        engine.registerFunction("", "probplot", chartNoop);
        engine.registerFunction("", "newplot", chartNoop);
        engine.registerFunction("", "zoom", chartNoop);
        engine.registerFunction("", "pan", chartNoop);
        engine.registerFunction("", "rotate3d", chartNoop);

        ExternalFunc dendroFn = [](Span<const Value> args, size_t nargout,
                                   Span<Value> outs, CallContext &ctx) {
            ctx.engine->figureManager().current();
            auto *mr = ctx.engine->resource();
            size_t m = (!args.empty() && args[0].dims().rows() > 0) ? args[0].dims().rows() : 1;
            if (nargout == 0) {
                if (!outs.empty()) outs[0] = Value();
                return;
            }
            Value H = Value::matrix(m, 1, ValueType::DOUBLE, mr);
            for (size_t i = 0; i < m; ++i) H.doubleDataMut()[i] = 1.0;
            outs[0] = std::move(H);
            if (nargout >= 2 && outs.size() >= 2) {
                Value T = Value::matrix(m + 1, 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i <= m; ++i) T.doubleDataMut()[i] = static_cast<double>(i + 1);
                outs[1] = std::move(T);
            }
            if (nargout >= 3 && outs.size() >= 3) {
                Value P = Value::matrix(1, m + 1, ValueType::DOUBLE, mr);
                for (size_t i = 0; i <= m; ++i) P.doubleDataMut()[i] = static_cast<double>(i + 1);
                outs[2] = std::move(P);
            }
        };
        engine.registerFunction("", "dendrogram", std::move(dendroFn));
    }
    // Colormap palette functions: parula([m]), jet([m]), hot([m]), etc. -> m x 3 double [0,1]
    {
        static const char *kCmapFns[] = {
            "parula", "turbo", "jet", "hot", "cool", "gray", "bone",
            "copper", "spring", "summer", "autumn", "winter", "hsv", "viridis",
            "pink", "lines"
        };
        for (const char *cname : kCmapFns) {
            std::string paletteName = cname;
            ExternalFunc fn = [paletteName](Span<const Value> args, size_t,
                                            Span<Value> outs, CallContext &ctx) {
                argcheck::checkNargin(args.size(), 0, 1, paletteName.c_str());
                auto *mr = ctx.engine->resource();
                size_t m = 256;
                if (!args.empty() && args[0].isScalar() && !args[0].isChar() && !args[0].isString()) {
                    double mv = args[0].toScalar();
                    m = (mv >= 0.0) ? static_cast<size_t>(mv) : 0;
                }
                Value C = Value::matrix(m, 3, ValueType::DOUBLE, mr);
                if (m > 0) {
                    double *d = C.doubleDataMut();
                    if (paletteName == "jet") {
                        std::fill(d, d + 3 * m, 0.0);
                        const int mi = static_cast<int>(m);
                        const int n = (mi + 3) / 4;
                        const int ulen = n + (n - 1) + n;
                        auto u_at = [&](int idx1) -> double {
                            if (idx1 <= n) return double(idx1) / double(n);
                            if (idx1 <= 2 * n - 1) return 1.0;
                            return double(2 * n - 1 + n - (idx1 - 1)) / double(n);
                        };
                        const int gbase = (n + 1) / 2 - (mi % 4 == 1 ? 1 : 0);
                        int rcount = 0, gcount = 0, bcount = 0;
                        for (int k = 1; k <= ulen; ++k) {
                            const int g = gbase + k;
                            const int r = g + n;
                            const int b = g - n;
                            if (r >= 1 && r <= mi) {
                                ++rcount;
                                d[0 * m + static_cast<size_t>(r - 1)] = u_at(rcount);
                            }
                            if (g >= 1 && g <= mi) {
                                ++gcount;
                                d[1 * m + static_cast<size_t>(g - 1)] = u_at(gcount);
                            }
                            if (b >= 1 && b <= mi) ++bcount;
                        }
                        int bsofar = 0;
                        for (int k = 1; k <= ulen; ++k) {
                            const int g = gbase + k;
                            const int b = g - n;
                            if (b >= 1 && b <= mi) {
                                ++bsofar;
                                d[2 * m + static_cast<size_t>(b - 1)] = u_at(ulen - bcount + bsofar);
                            }
                        }
                    } else if (paletteName == "gray") {
                        for (size_t i = 0; i < m; ++i) {
                            double g = (m == 1) ? 0.0 : double(i) / double(m - 1);
                            d[i] = d[i + m] = d[i + 2 * m] = g;
                        }
                    } else if (paletteName == "cool") {
                        for (size_t i = 0; i < m; ++i) {
                            double r = (m == 1) ? 0.0 : double(i) / double(m - 1);
                            d[i] = r; d[i + m] = 1.0 - r; d[i + 2 * m] = 1.0;
                        }
                    } else if (paletteName == "hot") {
                        const int mi = static_cast<int>(m);
                        const int n = (3 * mi) / 8;
                        for (int i = 0; i < mi; ++i) {
                            d[i]         = (i < n) ? double(i + 1) / double(n) : 1.0;
                            d[i + m]     = (i < n) ? 0.0 : (i < 2 * n ? double(i - n + 1) / double(n) : 1.0);
                            d[i + 2 * m] = (i < 2 * n) ? 0.0 : double(i - 2 * n + 1) / double(mi - 2 * n);
                        }
                    } else {
                        int tableN = 256;
                        const std::uint8_t *tbl = figrender::namedPalette(paletteName, tableN);
                        for (size_t i = 0; i < m; ++i) {
                            double t = (m == 1) ? 0.0 : static_cast<double>(i) / static_cast<double>(m - 1);
                            int idx = std::min(tableN - 1, std::max(0, static_cast<int>(std::lround(t * (tableN - 1)))));
                            d[i]         = tbl[idx * 3 + 0] / 255.0;
                            d[i + m]     = tbl[idx * 3 + 1] / 255.0;
                            d[i + 2 * m] = tbl[idx * 3 + 2] / 255.0;
                        }
                    }
                }
                if (!outs.empty()) outs[0] = std::move(C);
            };
            engine.registerFunction("", cname, std::move(fn));
        }
    }
}

} // namespace numkit
