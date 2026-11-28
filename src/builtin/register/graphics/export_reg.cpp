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
#include <numkit/graphics/figure_render_state.hpp>
#include <numkit/graphics/figure_render.hpp>
#include <numkit/graphics/graphics_handle.hpp>
#include <numkit/graphics/graphics_scene.hpp>
#include <numkit/image/io/io.hpp>
#include <numkit/value/argcheck.hpp>

#include <keel/number.h>

namespace numkit::gfx {
bool projectSceneFigureToRenderState(const GraphicsScene &scene, NodeId figId,
                                     FigureState &out, keel::Allocator *mr);
} // namespace numkit::gfx

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

struct ExportArgs {
    const FigureState *fig = nullptr;
    FigureState projectedFig;
    bool useProjected = false;
    bool rgbImage = false;
    keel::String path;
    keel::String ext;       // from filename or explicit format
    keel::i32 dpi = 150;          // default raster resolution; scales the 840x630 base

    const FigureState *resolvedFigure() const noexcept
    {
        return useProjected ? &projectedFig : fig;
    }
};

bool resolveTargetFigure(Engine &engine, const Value &v, ExportArgs &out)
{
    auto &scene = engine.graphicsScene();
    if (isSceneGraphicsHandle(v)) {
        const keel::i32 hid = graphicsHandleIdOf(v);
        const auto *node = scene.findNode(hid);
        if (!node)
            throw numkit::Error("Invalid or deleted object.");
        const gfx::NodeId figId = (node->classId == gfx::ClassId::Figure)
                                      ? hid : scene.containingFigure(hid);
        if (figId != gfx::kInvalidNodeId
            && gfx::projectSceneFigureToRenderState(scene, figId, out.projectedFig, engine.allocator())) {
            out.useProjected = true;
            return true;
        }
        return false;
    }
    if (v.isScalar()) {
        const keel::i32 num = keel::as<keel::i32>(v.toScalar());
        const gfx::NodeId figId = scene.findFigureByNumber(num);
        if (figId != gfx::kInvalidNodeId
            && gfx::projectSceneFigureToRenderState(scene, figId, out.projectedFig, engine.allocator())) {
            out.useProjected = true;
            return true;
        }
        return false;
    }
    return false;
}

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
                if (!resolveTargetFigure(engine, a, out))
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
            // print-style flags: -dpng / -dsvg / -r150 / -r0 / -RGBImage / -ochr…
            if (sl == "-rgbimage") {
                out.rgbImage = true;
            } else if (sl.size() > 2 && sl[1] == 'd') {
                out.ext = keel::String::from(sl.substr(2));
            } else if (sl.size() > 2 && sl[1] == 'r') {
                keel::i32 dpiVal = 0;
                if (auto parsed = keel::parse_i64(sl.substr(2)); parsed.is_ok())
                    dpiVal = keel::as<keel::i32>(parsed.value());
                out.dpi = (dpiVal == 0) ? 96 : keel::max(30, dpiVal);
            }
            // other print options (-painters, -opengl, -bestfit, …)
            // accepted and ignored
            continue;
        }
        // Name-Value options for exportgraphics (e.g. 'Resolution', 400).
        if (havePath && i + 1 < args.size()) {
            if (sl == "resolution" && args[i + 1].isScalar()) {
                const keel::i32 dpiVal = keel::as<keel::i32>(args[i + 1].toScalar());
                out.dpi = keel::max(30, dpiVal);
                ++i;
                continue;
            }
            if (sl == "backgroundcolor" || sl == "contenttype" || sl == "append"
                || sl == "colorspace" || sl == "preserveaspectratio") {
                ++i;
                continue;
            }
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
    if (needTarget && !out.resolvedFigure()) {
        const gfx::NodeId curFig = engine.graphicsScene().ensureCurrentFigure();
        if (curFig != gfx::kInvalidNodeId
            && gfx::projectSceneFigureToRenderState(engine.graphicsScene(), curFig, out.projectedFig, engine.allocator())) {
            out.useProjected = true;
        }
    }
    if (out.rgbImage)
        return out;
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
    const FigureState *targetFig = ea.resolvedFigure();
    if (!targetFig)
        throw numkit::Error(keel::format("{}: no figure to export", who).c_str());
    // Base canvas scales from Figure.Position (default 560×420 → 840×630 @150dpi;
    // custom Position e.g. 1280×760 scales proportionally with dpi/100).
    const keel::f64 figW = (targetFig->figWidth > 0.0) ? targetFig->figWidth : 560.0;
    const keel::f64 figH = (targetFig->figHeight > 0.0) ? targetFig->figHeight : 420.0;
    const keel::i32 W = keel::max(64, (keel::i32)keel::round_i64(figW * ea.dpi / 100.0));
    const keel::i32 H = keel::max(64, (keel::i32)keel::round_i64(figH * ea.dpi / 100.0));

    auto rp = engine.resolvePath(ea.path);
    if (!rp.fs)
        throw numkit::Error(keel::format("{}: cannot resolve '{}'", who, ea.path).c_str());

    if (ea.ext == "svg") {
        rp.fs->writeFile(rp.path,
                         figrender::renderFigureSvg(*targetFig, W, H,
                                                     svgPngEmbedder(
                                                         engine.allocator())));
        return;
    }
    if (ea.ext == "pdf") {
        rp.fs->writeFileBytes(rp.path,
                              figrender::renderFigurePdf(*targetFig, W, H));
        return;
    }
    if (ea.ext.is_one_of({"eps", "epsc", "eps2", "epsc2"})) {
        rp.fs->writeFile(rp.path,
                         figrender::renderFigureEps(*targetFig, W, H));
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
        figrender::renderFigureRgb(*targetFig, W, H);
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
    auto &eng = coreEngine(ctx);
    ExportArgs ea = parseExportArgs(eng, args, "print",
                                    /*needTarget=*/true);
    if (ea.rgbImage) {
        const FigureState *targetFig = ea.resolvedFigure();
        if (!targetFig)
            throw numkit::Error("print: no figure to export");
        const keel::i32 W = keel::max(64, (keel::i32)(840.0 * ea.dpi / 150.0));
        const keel::i32 H = keel::max(64, (keel::i32)(630.0 * ea.dpi / 150.0));
        keel::Array<keel::u8> rgb = figrender::renderFigureRgb(*targetFig, W, H);
        const keel::usize Hp = (keel::usize)H, Wp = (keel::usize)W;
        Value A = Value::matrix3d(Hp, Wp, 3, ValueType::UINT8, eng.allocator());
        keel::u8 *d = A.uint8DataMut();
        for (keel::usize r = 0; r < Hp; ++r)
            for (keel::usize c = 0; c < Wp; ++c)
                for (keel::usize ch = 0; ch < 3; ++ch)
                    d[r + c * Hp + ch * Hp * Wp] = rgb[(r * Wp + c) * 3 + ch];
        outs[0] = keel::move(A);
        return;
    }
    runExport(eng, ea, "print");
    outs[0] = Value();
}

void graphicsNoop_reg(keel::ArrayRef<Value>, keel::usize,
                      keel::MutArrayRef<Value> outs, CallContext &)
{
    if (!outs.empty()) outs[0] = Value();
}

void newplot_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                 keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    auto &scene = coreEngine(ctx).graphicsScene();
    auto *mr = ctx.engine->allocator();
    gfx::NodeId axId = gfx::kInvalidNodeId;
    if (!args.empty() && isSceneGraphicsHandle(args[0])) {
        const gfx::NodeId hid = graphicsHandleIdOf(args[0]);
        if (const auto *node = scene.findNode(hid)) {
            if (node->classId == gfx::ClassId::Axes || node->classId == gfx::ClassId::PolarAxes)
                axId = hid;
            else if (node->classId == gfx::ClassId::Figure)
                axId = scene.ensureCurrentAxes(hid);
        }
    }
    if (axId == gfx::kInvalidNodeId)
        axId = scene.ensureCurrentAxes();
    const keel::String np = scene.getProperty(axId, "NextPlot", mr).toString();
    if (np.equals_ignore_ascii_case("replace"))
        scene.clearAxes(axId, true);
    else if (np.equals_ignore_ascii_case("replacechildren"))
        scene.clearAxes(axId, false);
    outs[0] = (nargout >= 1) ? scene.makeHandleValue(axId, mr) : Value();
}

void getframe_reg(keel::ArrayRef<Value> args, keel::usize,
                  keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    argcheck::checkNargin(args.size(), 0, 2, "getframe");
    auto *mr = ctx.engine->allocator();
    auto &eng = coreEngine(ctx);
    ExportArgs ea;
    if (!args.empty())
        resolveTargetFigure(eng, args[0], ea);
    const FigureState *fig = ea.resolvedFigure();
    if (!fig) {
        const gfx::NodeId curFig = eng.graphicsScene().ensureCurrentFigure();
        if (curFig != gfx::kInvalidNodeId
            && gfx::projectSceneFigureToRenderState(eng.graphicsScene(), curFig, ea.projectedFig, mr)) {
            ea.useProjected = true;
            fig = ea.resolvedFigure();
        }
    }
    if (!fig) {
        ea.useProjected = true;
        fig = ea.resolvedFigure();
    }
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
    coreEngine(ctx).graphicsScene().ensureCurrentFigure();
    if (!outs.empty())
        outs[0] = (nargout >= 1) ? Value::scalar(1.0, ctx.engine->allocator()) : Value();
}

void dendrogram_reg(keel::ArrayRef<Value> args, keel::usize nargout,
                    keel::MutArrayRef<Value> outs, CallContext &ctx)
{
    coreEngine(ctx).graphicsScene().ensureCurrentAxes();
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
        } else if (paletteName == "spring") {
            const keel::f64 inv = (m == 1) ? 0.0 : 1.0 / keel::as<keel::f64>(m - 1);
            for (keel::usize i = 0; i < m; ++i) {
                const keel::f64 g = (m == 1) ? 0.0 : keel::as<keel::f64>(i) * inv;
                d[i] = 1.0; d[i + m] = g; d[i + 2 * m] = 1.0 - g;
            }
        } else if (paletteName == "summer") {
            const keel::f64 inv = (m == 1) ? 0.0 : 1.0 / keel::as<keel::f64>(m - 1);
            for (keel::usize i = 0; i < m; ++i) {
                const keel::f64 r = (m == 1) ? 0.0 : keel::as<keel::f64>(i) * inv;
                d[i] = r; d[i + m] = 0.5 + 0.5 * r; d[i + 2 * m] = 0.4;
            }
        } else if (paletteName == "autumn") {
            const keel::f64 inv = (m == 1) ? 0.0 : 1.0 / keel::as<keel::f64>(m - 1);
            for (keel::usize i = 0; i < m; ++i) {
                d[i] = 1.0;
                d[i + m] = (m == 1) ? 0.0 : keel::as<keel::f64>(i) * inv;
                d[i + 2 * m] = 0.0;
            }
        } else if (paletteName == "winter") {
            const keel::f64 inv = (m == 1) ? 0.0 : 1.0 / keel::as<keel::f64>(m - 1);
            for (keel::usize i = 0; i < m; ++i) {
                const keel::f64 g = (m == 1) ? 0.0 : keel::as<keel::f64>(i) * inv;
                d[i] = 0.0; d[i + m] = g; d[i + 2 * m] = 1.0 - 0.5 * g;
            }
        } else if (paletteName == "copper") {
            const keel::f64 inv = (m == 1) ? 0.0 : 1.0 / keel::as<keel::f64>(m - 1);
            for (keel::usize i = 0; i < m; ++i) {
                const keel::f64 x = (m == 1) ? 0.0 : keel::as<keel::f64>(i) * inv;
                keel::f64 r = 1.25 * x;
                if (r > 1.0) r = 1.0;
                d[i] = r; d[i + m] = 0.7812 * x; d[i + 2 * m] = 0.4975 * x;
            }
        } else if (paletteName == "hsv") {
            if (m == 1) {
                d[0] = 1.0; d[1] = 0.0; d[2] = 0.0;
            } else {
                const keel::f64 inv = 1.0 / keel::as<keel::f64>(m);
                for (keel::usize i = 0; i < m; ++i) {
                    const keel::f64 h6 = 6.0 * keel::as<keel::f64>(i) * inv;
                    const keel::i32 sector = keel::as<keel::i32>(keel::floor(h6)) % 6;
                    const keel::f64 f = h6 - keel::floor(h6);
                    keel::f64 r = 0.0, g = 0.0, b = 0.0;
                    if (sector == 0)      { r = 1.0; g = f;       b = 0.0;     }
                    else if (sector == 1) { r = 1.0 - f; g = 1.0; b = 0.0;     }
                    else if (sector == 2) { r = 0.0; g = 1.0;     b = f;       }
                    else if (sector == 3) { r = 0.0; g = 1.0 - f; b = 1.0;     }
                    else if (sector == 4) { r = f;   g = 0.0;     b = 1.0;     }
                    else                  { r = 1.0; g = 0.0;     b = 1.0 - f; }
                    d[i] = r; d[i + m] = g; d[i + 2 * m] = b;
                }
            }
        } else if (paletteName == "lines") {
            if (m == 1) {
                d[0] = 0.0; d[1] = 0.0; d[2] = 1.0;
            } else {
                static constexpr keel::f64 kLines[7][3] = {
                    {0.066, 0.443, 0.745},
                    {0.866, 0.329, 0.000},
                    {0.929, 0.694, 0.125},
                    {0.521, 0.086, 0.819},
                    {0.231, 0.666, 0.196},
                    {0.184, 0.745, 0.937},
                    {0.819, 0.015, 0.545}
                };
                for (keel::usize i = 0; i < m; ++i) {
                    const keel::usize k = i % 7;
                    d[i] = kLines[k][0]; d[i + m] = kLines[k][1]; d[i + 2 * m] = kLines[k][2];
                }
            }
        } else if (paletteName == "bone") {
            const keel::i32 mi = keel::as<keel::i32>(m);
            if (mi == 1) {
                d[0] = 0.125; d[1] = 0.125; d[2] = 0.125;
            } else if (mi == 2) {
                d[0] = 1.0 / 16.0; d[2] = 0.125; d[4] = 0.125;
                d[1] = 1.0;        d[3] = 1.0;   d[5] = 1.0;
            } else {
                const keel::f64 inv = 1.0 / keel::as<keel::f64>(mi - 1);
                auto xv = [&](keel::i32 i) { return keel::as<keel::f64>(i) * inv; };
                keel::Array<keel::f64> r(m, mr), g(m, mr), b(m, mr);
                const keel::i32 idxR = keel::as<keel::i32>(keel::floor(0.75 * mi));
                const keel::i32 nelR = mi - idxR + 1;
                const keel::i32 rem8 = mi % 8;
                keel::f64 baseR = 0.0;
                if (rem8 == 2 || rem8 == 4)      baseR = 1.0 / (16.0 + 2.0 * (mi - rem8));
                else if (rem8 == 5 || rem8 == 7) baseR = 1.0 / (24.0 + 2.0 * (mi - rem8));
                for (keel::i32 i = 0; i < idxR; ++i) r[keel::as<keel::usize>(i)] = (7.0 / 8.0) * xv(i);
                const keel::f64 aR = (7.0 / 8.0) * xv(idxR - 1) + baseR;
                for (keel::i32 j = 0; j < nelR; ++j) {
                    const keel::i32 pos = idxR - 1 + j;
                    if (pos >= 0 && pos < mi)
                        r[keel::as<keel::usize>(pos)] = aR + (1.0 - aR) * keel::as<keel::f64>(j) / keel::as<keel::f64>(nelR - 1);
                }
                const keel::i32 idxG = keel::as<keel::i32>(keel::floor(0.375 * mi));
                for (keel::i32 i = 0; i < idxG; ++i) g[keel::as<keel::usize>(i)] = (7.0 / 8.0) * xv(i);
                const keel::i32 lenG = idxG + 1;
                const keel::f64 aG = (7.0 / 8.0) * xv(idxG - 1);
                const keel::f64 bG = (7.0 / 8.0) * xv(2 * idxG - 1) + 0.125;
                for (keel::i32 j = 0; j < lenG; ++j) {
                    const keel::i32 pos = idxG - 1 + j;
                    if (pos >= 0 && pos < mi)
                        g[keel::as<keel::usize>(pos)] = aG + (bG - aG) * keel::as<keel::f64>(j) / keel::as<keel::f64>(lenG - 1);
                }
                for (keel::i32 i = 2 * idxG; i < mi; ++i) g[keel::as<keel::usize>(i)] = (7.0 / 8.0) * xv(i) + 0.125;
                const keel::f64 baseB = 1.0 / (8.0 * idxG);
                const keel::f64 bB = (7.0 / 8.0) * xv(idxG - 1) + 0.125;
                for (keel::i32 j = 0; j < idxG; ++j) {
                    if (j < mi)
                        b[keel::as<keel::usize>(j)] = (idxG == 1)
                            ? baseB
                            : baseB + (bB - baseB) * keel::as<keel::f64>(j) / keel::as<keel::f64>(idxG - 1);
                }
                for (keel::i32 i = idxG - 1; i < mi; ++i) b[keel::as<keel::usize>(i)] = (7.0 / 8.0) * xv(i) + 0.125;
                for (keel::usize i = 0; i < m; ++i) { d[i] = r[i]; d[i + m] = g[i]; d[i + 2 * m] = b[i]; }
            }
        } else if (paletteName == "pink") {
            const keel::i32 mi = keel::as<keel::i32>(m);
            if (mi == 1) {
                const keel::f64 v = keel::sqrt(1.0 / 3.0);
                d[0] = v; d[1] = v; d[2] = v;
            } else if (mi == 2) {
                const keel::f64 s = keel::sqrt(1.0 / 3.0);
                const keel::f64 s2 = keel::sqrt(1.0 / 6.0);
                d[0] = s;   d[2] = s;   d[4] = s2;
                d[1] = 1.0; d[3] = 1.0; d[5] = 1.0;
            } else {
                const keel::i32 idx = keel::as<keel::i32>(keel::floor(3.0 / 8.0 * mi));
                const keel::f64 base = 1.0 / (3.0 * idx);
                const keel::f64 inv = 1.0 / keel::as<keel::f64>(mi - 1);
                keel::Array<keel::f64> r(m, mr), g(m, mr), b(m, mr);
                const keel::f64 rEnd = (2.0 / 3.0) * keel::as<keel::f64>(idx - 1) * inv + 1.0 / 3.0;
                if (idx == 1) {
                    r[0] = base;
                } else {
                    for (keel::i32 i = 0; i < idx; ++i)
                        r[keel::as<keel::usize>(i)] = base + (rEnd - base) * keel::as<keel::f64>(i) / keel::as<keel::f64>(idx - 1);
                }
                for (keel::i32 i = idx; i < mi; ++i)
                    r[keel::as<keel::usize>(i)] = (2.0 / 3.0) * keel::as<keel::f64>(i) * inv + 1.0 / 3.0;
                for (keel::i32 i = 0; i < idx; ++i)
                    g[keel::as<keel::usize>(i)] = (2.0 / 3.0) * keel::as<keel::f64>(i) * inv;
                const keel::f64 aG = (2.0 / 3.0) * keel::as<keel::f64>(idx - 1) * inv;
                const keel::f64 bG = (2.0 / 3.0) * keel::as<keel::f64>(2 * idx - 1) * inv + 1.0 / 3.0;
                const keel::i32 lenG = idx + 1;
                for (keel::i32 j = 0; j < lenG; ++j) {
                    const keel::i32 pos = idx - 1 + j;
                    if (pos >= 0 && pos < mi)
                        g[keel::as<keel::usize>(pos)] = aG + (bG - aG) * keel::as<keel::f64>(j) / keel::as<keel::f64>(lenG - 1);
                }
                for (keel::i32 i = 2 * idx; i < mi; ++i)
                    g[keel::as<keel::usize>(i)] = (2.0 / 3.0) * keel::as<keel::f64>(i) * inv + 1.0 / 3.0;
                const keel::i32 upto = keel::min(2 * idx, mi);
                for (keel::i32 i = 0; i < upto; ++i)
                    b[keel::as<keel::usize>(i)] = (2.0 / 3.0) * keel::as<keel::f64>(i) * inv;
                const keel::i32 bStart = 2 * idx - 1;
                if (bStart >= 0 && bStart < mi) {
                    const keel::i32 lenB = mi - 2 * idx + 1;
                    const keel::f64 aB = (2.0 / 3.0) * keel::as<keel::f64>(bStart) * inv;
                    for (keel::i32 j = 0; j < lenB; ++j) {
                        const keel::i32 pos = bStart + j;
                        if (pos >= mi) break;
                        b[keel::as<keel::usize>(pos)] = aB + (1.0 - aB) * keel::as<keel::f64>(j) / keel::as<keel::f64>(lenB - 1);
                    }
                }
                for (keel::usize i = 0; i < m; ++i) {
                    d[i]         = keel::sqrt(r[i]);
                    d[i + m]     = keel::sqrt(g[i]);
                    d[i + 2 * m] = keel::sqrt(b[i]);
                }
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
    {"movie",          &graphicsNoop_reg},
    {"getframe",       &getframe_reg},
    {"frame2im",       &frame2im_reg},
    {"im2frame",       &im2frame_reg},
    {"imtool",         &chartNoop_reg},
    {"implay",         &chartNoop_reg},
    {"newplot",        &newplot_reg},
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
