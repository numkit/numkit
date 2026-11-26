// tests/gtest/graphics_scene_test.cpp — Phase 1 Model-First Handle Graphics tests
// Dual-engine (TreeWalker + Bytecode VM): Root (16), Figure (68), Theme (2),
// Placeholder/gobjects, cascading defaults, paired Mode properties, vector get/set,
// findobj/findall/allchild/ancestor/copyobj/reset, and SceneChangeLog.

#include <keel/core.h>
#include <numkit/graphics/graphics_scene.hpp>
#include "dual_engine_fixture.hpp"

using namespace m_test;

class GraphicsSceneTest : public DualEngineTest {};

TEST_P(GraphicsSceneTest, RootAll16PropertiesAndBasics)
{
    eval("g = groot;"
         "p = properties(g);"
         "s = get(g);"
         "fns = fieldnames(s);");
    EXPECT_EQ(evalString("class(g)"), "matlab.ui.Root");
    EXPECT_EQ(evalString("g.Type"), "root");
    EXPECT_EQ(evalString("get(0, 'Type')"), "root");
    EXPECT_EQ(evalScalar("numel(p)"), 16.0);
    EXPECT_EQ(evalScalar("numel(fns)"), 16.0);
    EXPECT_EQ(evalString("g.Units"), "pixels");
    EXPECT_DOUBLE_EQ(evalScalar("g.ScreenDepth"), 32.0);
    EXPECT_DOUBLE_EQ(evalScalar("g.ScreenPixelsPerInch"), 96.0);
    EXPECT_EQ(evalString("g.FixedWidthFontName"), "Courier New");
    EXPECT_EQ(evalString("g.ShowHiddenHandles"), "off");
    EXPECT_EQ(evalString("g.HandleVisibility"), "on");
    EXPECT_TRUE(evalBool("isempty(g.CurrentFigure)"));
    EXPECT_TRUE(evalBool("isempty(g.Children)"));
    EXPECT_TRUE(evalBool("isempty(g.Parent)"));
    EXPECT_TRUE(evalBool("isempty(g.CallbackObject)"));
    EXPECT_TRUE(evalBool("isprop(g, 'ScreenSize')"));
    EXPECT_FALSE(evalBool("isprop(g, 'NoSuchProp')"));

    // Read-only properties on Root must reject assignment
    EXPECT_THROW(eval("g.Type = 'figure';"), numkit::Error);
    EXPECT_THROW(eval("g.MonitorPositions = [1 1 800 600];"), numkit::Error);
    EXPECT_THROW(eval("delete(g);"), numkit::Error);

    // Enum unique prefix & OnOff logical/numeric acceptance
    eval("g.Units = 'norm'; g.ShowHiddenHandles = true; g.Tag = 'myroot'; g.UserData = [1 2 3];");
    EXPECT_EQ(evalString("g.Units"), "normalized");
    EXPECT_EQ(evalString("g.ShowHiddenHandles"), "on");
    EXPECT_EQ(evalString("g.Tag"), "myroot");
    EXPECT_DOUBLE_EQ(evalScalar("g.UserData(3)"), 3.0);
}

TEST_P(GraphicsSceneTest, FigureAll68PropertiesSyncedGroupsAndTheme)
{
    eval("f = figure('Visible', 'off', 'Name', 'MainFig', 'Tag', 'fig1');"
         "p = properties(f);"
         "s = get(f);"
         "fns = fieldnames(s);");
    EXPECT_EQ(evalString("class(f)"), "matlab.ui.Figure");
    EXPECT_EQ(evalString("f.Type"), "figure");
    EXPECT_EQ(evalScalar("numel(p)"), 68.0);
    EXPECT_EQ(evalScalar("numel(fns)"), 68.0);
    EXPECT_EQ(evalString("f.Visible"), "off");
    EXPECT_EQ(evalString("f.Name"), "MainFig");
    EXPECT_EQ(evalString("f.Tag"), "fig1");
    EXPECT_DOUBLE_EQ(evalScalar("f.Number"), 1.0);
    EXPECT_EQ(evalString("f.NumberTitle"), "on");
    EXPECT_EQ(evalString("f.IntegerHandle"), "on");
    EXPECT_EQ(evalString("f.NextPlot"), "add");
    EXPECT_EQ(evalString("f.BeingDeleted"), "off");
    EXPECT_EQ(evalScalar("size(f.Colormap, 1)"), 256.0);
    EXPECT_EQ(evalScalar("size(f.Colormap, 2)"), 3.0);
    EXPECT_EQ(evalScalar("numel(f.Alphamap)"), 64.0);
    EXPECT_EQ(evalScalar("size(f.PointerShapeCData, 1)"), 16.0);
    EXPECT_EQ(evalScalar("size(f.PointerShapeCData, 2)"), 16.0);

    // Paired Mode properties: Renderer/RendererMode, PaperPosition/PaperPositionMode
    EXPECT_EQ(evalString("f.RendererMode"), "auto");
    EXPECT_EQ(evalString("f.PaperPositionMode"), "auto");
    eval("f.Renderer = 'painters'; f.PaperPosition = [1 1 6 4];");
    EXPECT_EQ(evalString("f.Renderer"), "painters");
    EXPECT_EQ(evalString("f.RendererMode"), "manual");
    EXPECT_EQ(evalString("f.PaperPositionMode"), "manual");

    // Position / InnerPosition / OuterPosition synchronization
    eval("f.Position = [100 120 800 600];");
    EXPECT_DOUBLE_EQ(evalScalar("f.InnerPosition(3)"), 800.0);
    EXPECT_DOUBLE_EQ(evalScalar("f.OuterPosition(4)"), 600.0);

    // PaperType <-> PaperSize <-> PaperOrientation synchronization
    eval("f.PaperType = 'A4';");
    EXPECT_NEAR(evalScalar("f.PaperSize(1)"), 8.2677165, 1e-4);
    EXPECT_NEAR(evalScalar("f.PaperSize(2)"), 11.692913, 1e-4);
    eval("f.PaperOrientation = 'landscape';");
    EXPECT_NEAR(evalScalar("f.PaperSize(1)"), 11.692913, 1e-4);
    EXPECT_NEAR(evalScalar("f.PaperSize(2)"), 8.2677165, 1e-4);
    eval("f.PaperSize = [10 20];");
    EXPECT_EQ(evalString("f.PaperType"), "<custom>");
    EXPECT_DOUBLE_EQ(evalScalar("f.PaperSize(1)"), 10.0);

    // ColorSpec: short name, long name, hex, RGB, 'none'
    eval("f.Color = 'r';");
    EXPECT_DOUBLE_EQ(evalScalar("f.Color(1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("f.Color(2)"), 0.0);
    eval("f.Color = '#336699';");
    EXPECT_NEAR(evalScalar("f.Color(1)"), 51.0 / 255.0, 1e-12);
    EXPECT_NEAR(evalScalar("f.Color(2)"), 102.0 / 255.0, 1e-12);
    EXPECT_NEAR(evalScalar("f.Color(3)"), 153.0 / 255.0, 1e-12);
    eval("f.Color = 'none';");
    EXPECT_EQ(evalString("f.Color"), "none");

    // Theme object (Name read-only, BaseColorStyle read-write) & Figure.Theme shorthand
    EXPECT_EQ(evalString("class(f.Theme)"), "matlab.graphics.theme.GraphicsTheme");
    EXPECT_EQ(evalString("f.Theme.BaseColorStyle"), "light");
    EXPECT_EQ(evalString("f.Theme.Name"), "Light Theme");
    eval("f.Theme = 'dark';");
    EXPECT_EQ(evalString("f.Theme.BaseColorStyle"), "dark");
    EXPECT_EQ(evalString("f.Theme.Name"), "Dark Theme");
    EXPECT_EQ(evalString("f.ThemeMode"), "manual");
    EXPECT_THROW(eval("f.Theme.Name = 'Custom';"), numkit::Error);

    // Callback storage (@fn, cell, char, [])
    eval("f.CloseRequestFcn = @(src, evt) disp('closing');"
         "f.KeyPressFcn = {@(a, b) a, 42};"
         "f.DeleteFcn = [];");
    EXPECT_EQ(evalString("f.DeleteFcn"), "");

    // Read-only & validation rejections
    EXPECT_THROW(eval("f.Number = 99;"), numkit::Error);
    EXPECT_THROW(eval("f.Type = 'axes';"), numkit::Error);
    EXPECT_THROW(eval("f.BeingDeleted = 'on';"), numkit::Error);
    EXPECT_THROW(eval("f.Renderer = 'vulkan';"), numkit::Error);
    EXPECT_THROW(eval("f.Color = [2 0 0];"), numkit::Error);
    EXPECT_THROW(eval("f.Colormap = [1 2];"), numkit::Error);
    EXPECT_THROW(eval("f.NoSuchProperty;"), numkit::Error);
}

TEST_P(GraphicsSceneTest, NumberingChildrenOrderIntegerHandleAndDefaults)
{
    eval("f1 = figure('Visible', 'off');"
         "f2 = figure('Visible', 'off');"
         "f5 = figure(5); set(f5, 'Visible', 'off');"
         "f3 = figure('Visible', 'off');"
         "ch = get(groot, 'Children');");
    EXPECT_DOUBLE_EQ(evalScalar("f1.Number"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("f2.Number"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("f5.Number"), 5.0);
    EXPECT_DOUBLE_EQ(evalScalar("f3.Number"), 3.0);
    // Children order is newest-first: [3 5 2 1]
    EXPECT_DOUBLE_EQ(evalScalar("numel(ch)"), 4.0);
    EXPECT_DOUBLE_EQ(evalScalar("ch(1).Number"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("ch(2).Number"), 5.0);
    EXPECT_DOUBLE_EQ(evalScalar("ch(3).Number"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("ch(4).Number"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("get(groot, 'CurrentFigure').Number"), 3.0);

    // IntegerHandle toggle
    eval("f3.IntegerHandle = 'off';");
    EXPECT_TRUE(evalBool("isempty(f3.Number)"));
    eval("f3.IntegerHandle = 'on';");
    EXPECT_DOUBLE_EQ(evalScalar("f3.Number"), 3.0);

    // Cascading defaults: defaultFigureColor, 'default', 'factory', 'remove'
    eval("set(groot, 'defaultFigureColor', [0.2 0.3 0.4]);"
         "fd = figure('Visible', 'off');"
         "c_inh = fd.Color;"
         "set(fd, 'Color', 'r');"
         "c_red = fd.Color;"
         "set(fd, 'Color', 'default');"
         "c_def = fd.Color;"
         "set(fd, 'Color', 'factory');"
         "c_fac = fd.Color;"
         "set(groot, 'defaultFigureColor', 'remove');"
         "fd2 = figure('Visible', 'off');"
         "c_rem = fd2.Color;");
    EXPECT_NEAR(evalScalar("c_inh(1)"), 0.2, 1e-12);
    EXPECT_NEAR(evalScalar("c_red(1)"), 1.0, 1e-12);
    EXPECT_NEAR(evalScalar("c_def(1)"), 0.2, 1e-12);
    EXPECT_NEAR(evalScalar("c_fac(1)"), 0.94, 1e-12);
    EXPECT_NEAR(evalScalar("c_rem(1)"), 0.94, 1e-12);
}

TEST_P(GraphicsSceneTest, HandleOpsFindobjVectorSetGetCopyResetAndGobjects)
{
    eval("f1 = figure('Visible', 'off', 'Tag', 'alpha', 'Name', 'N1');"
         "f2 = figure('Visible', 'off', 'Tag', 'beta', 'Name', 'N2');"
         "fh = figure('Visible', 'off', 'HandleVisibility', 'off', 'Tag', 'secret');");

    // HandleVisibility filtering: findobj vs findall, groot.Children vs allchild(groot)
    EXPECT_DOUBLE_EQ(evalScalar("numel(findobj(groot, 'Tag', 'secret'))"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(findall(groot, 'Tag', 'secret'))"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(get(groot, 'Children'))"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(allchild(groot))"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(findobj(groot, 'Tag', 'alpha', '-or', 'Tag', 'beta'))"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(findobj(groot, '-regexp', 'Tag', '^al'))"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(findobj(groot, '-isa', 'matlab.ui.Figure'))"), 2.0);

    // ancestor
    eval("anc = ancestor(f1, 'root');");
    EXPECT_TRUE(evalBool("anc == groot"));

    // copyobj
    eval("fc = copyobj(f1, groot);");
    EXPECT_EQ(evalString("fc.Tag"), "alpha");
    EXPECT_TRUE(evalBool("fc ~= f1"));

    // Vector get / set and cell set
    eval("arr = [f1; f2];"
         "set(arr, 'Tag', 'batch');"
         "gv = get(arr, 'Tag');"
         "set(arr, {'Tag', 'Name'}, {'t1', 'm1'; 't2', 'm2'});"
         "gv2 = get(arr, {'Tag', 'Name'});");
    EXPECT_EQ(evalString("gv{1}"), "batch");
    EXPECT_EQ(evalString("gv{2}"), "batch");
    EXPECT_EQ(evalString("gv2{1, 1}"), "t1");
    EXPECT_EQ(evalString("gv2{2, 2}"), "m2");

    // reset(f) preserves Position and Units, resets Tag and Name
    eval("f1.Position = [11 22 333 444]; f1.Units = 'points'; reset(f1);");
    EXPECT_EQ(evalString("f1.Tag"), "");
    EXPECT_EQ(evalString("f1.Name"), "");
    EXPECT_EQ(evalString("f1.Units"), "points");
    EXPECT_DOUBLE_EQ(evalScalar("f1.Position(3)"), 333.0);

    // gobjects & GraphicsPlaceholder: isvalid==true, isgraphics==false, ishandle==false
    eval("gp = gobjects(2, 3);");
    EXPECT_EQ(evalString("class(gp)"), "matlab.graphics.GraphicsPlaceholder");
    EXPECT_TRUE(evalBool("all(isvalid(gp(:)))"));
    EXPECT_FALSE(evalBool("any(isgraphics(gp(:)))"));
    EXPECT_FALSE(evalBool("any(ishandle(gp(:)))"));
    EXPECT_TRUE(evalBool("isvalid(f1) && isgraphics(f1, 'figure') && ishandle(f1) && ishandle(0)"));

    // close all leaves hidden figure; close all force closes hidden too
    eval("close('all');");
    EXPECT_DOUBLE_EQ(evalScalar("numel(get(groot, 'Children'))"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(allchild(groot))"), 1.0);
    EXPECT_FALSE(evalBool("isvalid(f1)"));
    EXPECT_TRUE(evalBool("isvalid(fh)"));
    eval("close('all', 'force');");
    EXPECT_DOUBLE_EQ(evalScalar("numel(allchild(groot))"), 0.0);
    EXPECT_FALSE(evalBool("isvalid(fh)"));
}

TEST_P(GraphicsSceneTest, SceneChangeLogRecordsLifecycleEvents)
{
    auto &scene = engine.graphicsScene();
    const keel::u64 g0 = scene.generation();
    eval("f = figure('Visible', 'off', 'Name', 'Obs'); f.Tag = 't'; close(f);");
    auto evs = scene.pollEvents(g0);
    EXPECT_GT(evs.size(), 3u);
    bool sawCreated = false, sawProp = false, sawDeleted = false;
    for (const auto &ev : evs) {
        if (ev.kind == numkit::gfx::SceneEventKind::NodeCreated) sawCreated = true;
        if (ev.kind == numkit::gfx::SceneEventKind::PropertyChanged && ev.propName == "Tag") sawProp = true;
        if (ev.kind == numkit::gfx::SceneEventKind::NodeDeleted) sawDeleted = true;
    }
    EXPECT_TRUE(sawCreated);
    EXPECT_TRUE(sawProp);
    EXPECT_TRUE(sawDeleted);
}

TEST_P(GraphicsSceneTest, Phase2All8ClassesExactPropertyCounts)
{
    eval("f = figure('Visible', 'off');"
         "ax = axes(f);"
         "xr = ax.XAxis;"
         "tx = text(ax, 0.5, 0.5, 'hi');"
         "lg = legend(ax, {'a', 'b'});"
         "cb = colorbar(ax);"
         "cl = xline(ax, 0.5, '--r', 'mid');"
         "f2 = figure('Visible', 'off');"
         "pax = polaraxes(f2);"
         "f3 = figure('Visible', 'off');"
         "tl = tiledlayout(f3, 2, 2);");

    EXPECT_EQ(evalString("class(ax)"),  "matlab.graphics.axis.Axes");
    EXPECT_EQ(evalString("class(xr)"),  "matlab.graphics.axis.decorator.NumericRuler");
    EXPECT_EQ(evalString("class(pax)"), "matlab.graphics.axis.PolarAxes");
    EXPECT_EQ(evalString("class(tl)"),  "matlab.graphics.layout.TiledChartLayout");
    EXPECT_EQ(evalString("class(tx)"),  "matlab.graphics.primitive.Text");
    EXPECT_EQ(evalString("class(lg)"),  "matlab.graphics.illustration.Legend");
    EXPECT_EQ(evalString("class(cb)"),  "matlab.graphics.illustration.ColorBar");
    EXPECT_EQ(evalString("class(cl)"),  "matlab.graphics.chart.decoration.ConstantLine");

    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(ax))"),  156.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(ax)))"), 156.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(xr))"),  35.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(xr)))"), 35.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(pax))"), 110.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(pax)))"), 110.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(tl))"),  30.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(tl)))"), 30.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(tx))"),  44.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(tx)))"), 44.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(lg))"),  44.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(lg)))"), 44.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(cb))"),  42.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(cb)))"), 42.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(cl))"),  41.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(cl)))"), 41.0);
}

TEST_P(GraphicsSceneTest, AxesRulerBidirectionalSyncAutoTicksAndFontCascade)
{
    eval("f = figure('Visible', 'off'); ax = axes(f);");
    EXPECT_EQ(evalString("ax.XLimMode"), "auto");
    EXPECT_EQ(evalString("ax.XTickMode"), "auto");

    // Axes -> Ruler sync + auto nice-ticks recomputation
    eval("ax.XLim = [0 10];");
    EXPECT_EQ(evalString("ax.XLimMode"), "manual");
    EXPECT_DOUBLE_EQ(evalScalar("ax.XAxis.Limits(1)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.XAxis.Limits(2)"), 10.0);
    EXPECT_EQ(evalString("ax.XAxis.LimitsMode"), "manual");
    EXPECT_DOUBLE_EQ(evalScalar("ax.XTick(1)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.XTick(end)"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.XAxis.TickValues(end)"), 10.0);

    // Ruler -> Axes bidirectional sync
    eval("ax.XAxis.Limits = [-5 5]; ax.XAxis.Direction = 'reverse'; ax.XAxis.Scale = 'log';");
    EXPECT_DOUBLE_EQ(evalScalar("ax.XLim(1)"), -5.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.XLim(2)"), 5.0);
    EXPECT_EQ(evalString("ax.XDir"), "reverse");
    EXPECT_EQ(evalString("ax.XScale"), "log");

    // Manual tick override switches XTickMode to manual; reverting to auto regenerates ticks
    eval("ax.XScale = 'linear'; ax.XLim = [0 10]; ax.XTick = [0 3 7 10];");
    EXPECT_EQ(evalString("ax.XTickMode"), "manual");
    EXPECT_DOUBLE_EQ(evalScalar("numel(ax.XTick)"), 4.0);
    eval("ax.XTickMode = 'auto';");
    EXPECT_EQ(evalString("ax.XTickMode"), "auto");
    EXPECT_GT(evalScalar("numel(ax.XTick)"), 4.0);

    // FontSize cascade to Title (1.1x), XLabel (1.1x), YLabel (1.1x), XAxis (1x)
    eval("ax.FontSize = 14;");
    EXPECT_NEAR(evalScalar("ax.Title.FontSize"), 15.4, 1e-12);
    EXPECT_NEAR(evalScalar("ax.XLabel.FontSize"), 15.4, 1e-12);
    EXPECT_NEAR(evalScalar("ax.YLabel.FontSize"), 15.4, 1e-12);
    EXPECT_DOUBLE_EQ(evalScalar("ax.XAxis.FontSize"), 14.0);

    // Invalid limits (lo >= hi) must throw
    EXPECT_THROW(eval("ax.XLim = [5 5];"), numkit::Error);
    EXPECT_THROW(eval("ax.YLim = [10 2];"), numkit::Error);
}

TEST_P(GraphicsSceneTest, HoldClaYYAxisLinkAxesAndSubplotTiledLayout)
{
    eval("f = figure('Visible', 'off'); ax = axes(f);");
    EXPECT_FALSE(evalBool("ishold(ax)"));
    eval("hold(ax, 'on');");
    EXPECT_TRUE(evalBool("ishold(ax)"));
    EXPECT_EQ(evalString("ax.NextPlot"), "add");
    eval("hold(ax, 'off');");
    EXPECT_FALSE(evalBool("ishold(ax)"));
    EXPECT_EQ(evalString("ax.NextPlot"), "replace");

    // yyaxis left/right creates 2x1 YAxis with independent limits and labels
    eval("yyaxis(ax, 'left'); ylim(ax, [0 10]); ylabel(ax, 'LeftY');"
         "yyaxis(ax, 'right'); ylim(ax, [100 500]); ylabel(ax, 'RightY');");
    EXPECT_DOUBLE_EQ(evalScalar("numel(ax.YAxis)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.YLim(2)"), 500.0);
    EXPECT_EQ(evalString("ax.YLabel.String"), "RightY");
    eval("yyaxis(ax, 'left');");
    EXPECT_DOUBLE_EQ(evalScalar("ax.YLim(2)"), 10.0);
    EXPECT_EQ(evalString("ax.YLabel.String"), "LeftY");

    // cla clears children; cla('reset') also clears labels and resets properties
    eval("text(ax, 0.2, 0.2, 'tmp'); xline(ax, 1.0);");
    EXPECT_DOUBLE_EQ(evalScalar("numel(ax.Children)"), 2.0);
    eval("cla(ax);");
    EXPECT_DOUBLE_EQ(evalScalar("numel(ax.Children)"), 0.0);
    EXPECT_EQ(evalString("ax.YLabel.String"), "LeftY");
    eval("cla(ax, 'reset');");
    EXPECT_EQ(evalString("ax.YLabel.String"), "");

    // subplot + linkaxes
    eval("f2 = figure('Visible', 'off');"
         "a1 = subplot(2, 1, 1); xlim(a1, [0 5]);"
         "a2 = subplot(2, 1, 2); xlim(a2, [2 10]);"
         "linkaxes([a1 a2], 'x');");
    // Union on link -> [0 10] on both
    EXPECT_DOUBLE_EQ(evalScalar("a1.XLim(1)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("a1.XLim(2)"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("a2.XLim(2)"), 10.0);
    // Changing one propagates to the other
    eval("xlim(a1, [-3 7]);");
    EXPECT_DOUBLE_EQ(evalScalar("a2.XLim(1)"), -3.0);
    EXPECT_DOUBLE_EQ(evalScalar("a2.XLim(2)"), 7.0);
    // Unlink breaks propagation
    eval("linkaxes([a1 a2], 'off'); xlim(a1, [1 2]);");
    EXPECT_DOUBLE_EQ(evalScalar("a2.XLim(2)"), 7.0);

    // tiledlayout + nexttile
    eval("f3 = figure('Visible', 'off');"
         "tl = tiledlayout(f3, 2, 3, 'TileSpacing', 'compact', 'Padding', 'tight');"
         "t1 = nexttile(tl); t2 = nexttile(tl, 4, [1 2]);");
    EXPECT_DOUBLE_EQ(evalScalar("tl.GridSize(1)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("tl.GridSize(2)"), 3.0);
    EXPECT_EQ(evalString("tl.TileSpacing"), "compact");
    EXPECT_EQ(evalString("tl.Padding"), "tight");
    EXPECT_DOUBLE_EQ(evalScalar("t1.Layout.Tile"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("t2.Layout.Tile"), 4.0);
    EXPECT_DOUBLE_EQ(evalScalar("t2.Layout.TileSpan(2)"), 2.0);
    EXPECT_TRUE(evalBool("t1.Parent == tl"));
}

TEST_P(GraphicsSceneTest, PolarAxesDecorationsAndCascadingDefaults)
{
    // Cascading defaults via groot & Figure
    eval("set(groot, 'defaultAxesFontSize', 12);"
         "f = figure('Visible', 'off');"
         "set(f, 'defaultTextColor', 'r');"
         "ax = axes(f);"
         "tx = text(ax, 1, 2, 'hello');"
         "set(groot, 'defaultAxesFontSize', 'remove');");
    EXPECT_DOUBLE_EQ(evalScalar("ax.FontSize"), 12.0);
    EXPECT_DOUBLE_EQ(evalScalar("tx.Color(1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("tx.Color(2)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("tx.Position(1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("tx.Position(2)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("tx.Position(3)"), 0.0);

    // Legend, ColorBar (CLim sync), xline/yline (vector creation)
    eval("lg = legend(ax, {'s1', 's2'}, 'Location', 'northwest', 'Box', 'off');"
         "lg.Title.String = 'Curves';"
         "cb = colorbar(ax, 'southoutside');"
         "cb.Label.String = 'Intensity';"
         "clim(ax, [-2 8]);"
         "xls = xline(ax, [1 2 3], '--b', 'cut');");
    EXPECT_EQ(evalString("lg.Location"), "northwest");
    EXPECT_EQ(evalString("lg.Box"), "off");
    EXPECT_EQ(evalString("lg.Title.String"), "Curves");
    EXPECT_TRUE(evalBool("ax.Legend == lg"));
    EXPECT_EQ(evalString("cb.Location"), "southoutside");
    EXPECT_EQ(evalString("cb.Label.String"), "Intensity");
    EXPECT_DOUBLE_EQ(evalScalar("cb.Limits(1)"), -2.0);
    EXPECT_DOUBLE_EQ(evalScalar("cb.Limits(2)"), 8.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(xls)"), 3.0);
    EXPECT_EQ(evalString("xls(2).InterceptAxis"), "x");
    EXPECT_DOUBLE_EQ(evalScalar("xls(2).Value"), 2.0);
    EXPECT_EQ(evalString("xls(2).LineStyle"), "--");
    EXPECT_EQ(evalString("xls(2).Label"), "cut");

    // PolarAxes & polar limit/tick builtins
    eval("fp = figure('Visible', 'off');"
         "pax = polaraxes(fp);"
         "rlim(pax, [0 5]);"
         "thetalim(pax, [0 180]);"
         "rticks(pax, [0 2.5 5]);"
         "thetaticks(pax, [0 90 180]);"
         "grid(pax, 'off');");
    EXPECT_DOUBLE_EQ(evalScalar("pax.RLim(2)"), 5.0);
    EXPECT_DOUBLE_EQ(evalScalar("pax.ThetaLim(2)"), 180.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(pax.RTick)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(pax.ThetaTick)"), 3.0);
    EXPECT_EQ(evalString("pax.RGrid"), "off");
    EXPECT_EQ(evalString("pax.ThetaGrid"), "off");
    EXPECT_TRUE(evalBool("isgraphics(pax.RAxis) && isgraphics(pax.ThetaAxis)"));
}

TEST_P(GraphicsSceneTest, Phase3All15ChartClassesExactPropertyCounts)
{
    eval("f = figure('Visible', 'off'); ax = axes(f); hold(ax, 'on');"
         "h_ln = plot(ax, [0 1], [0 1]);"
         "h_sc = scatter(ax, [1 2], [3 4]);"
         "h_br = bar(ax, [1 2 3]);"
         "h_sf = surf(ax, [1 2; 3 4]);"
         "h_pt = patch(ax, [0 1 0], [0 0 1], 'r');"
         "h_qv = quiver(ax, [0 1], [0 1], [1 0], [0 1]);"
         "[~, h_ct] = contour(ax, peaks(6));"
         "h_st = stem(ax, [1 2 3]);"
         "h_eb = errorbar(ax, [1 2], [3 4], [0.5 0.5]);"
         "h_hg = histogram(ax, [1 2 2 3 3 3]);"
         "h_ar = area(ax, [1 2 3]);"
         "h_im = image(ax, [1 2; 3 4]);"
         "h_sr = stairs(ax, [1 2 3]);"
         "h_al = animatedline(ax);"
         "h_tb = annotation(f, 'textbox', [0.2 0.2 0.3 0.1], 'String', 'box');");

    EXPECT_EQ(evalString("class(h_ln)"), "matlab.graphics.chart.primitive.Line");
    EXPECT_EQ(evalString("class(h_sc)"), "matlab.graphics.chart.primitive.Scatter");
    EXPECT_EQ(evalString("class(h_br)"), "matlab.graphics.chart.primitive.Bar");
    EXPECT_EQ(evalString("class(h_sf)"), "matlab.graphics.chart.primitive.Surface");
    EXPECT_EQ(evalString("class(h_pt)"), "matlab.graphics.primitive.Patch");
    EXPECT_EQ(evalString("class(h_qv)"), "matlab.graphics.chart.primitive.Quiver");
    EXPECT_EQ(evalString("class(h_ct)"), "matlab.graphics.chart.primitive.Contour");
    EXPECT_EQ(evalString("class(h_st)"), "matlab.graphics.chart.primitive.Stem");
    EXPECT_EQ(evalString("class(h_eb)"), "matlab.graphics.chart.primitive.ErrorBar");
    EXPECT_EQ(evalString("class(h_hg)"), "matlab.graphics.chart.primitive.Histogram");
    EXPECT_EQ(evalString("class(h_ar)"), "matlab.graphics.chart.primitive.Area");
    EXPECT_EQ(evalString("class(h_im)"), "matlab.graphics.primitive.Image");
    EXPECT_EQ(evalString("class(h_sr)"), "matlab.graphics.chart.primitive.Stair");
    EXPECT_EQ(evalString("class(h_al)"), "matlab.graphics.animation.AnimatedLine");
    EXPECT_EQ(evalString("class(h_tb)"), "matlab.graphics.shape.TextBox");

    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_ln))"), 50.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(fieldnames(get(h_ln)))"), 50.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_sc))"), 63.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_br))"), 57.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_sf))"), 60.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_pt))"), 58.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_qv))"), 54.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_ct))"), 50.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_st))"), 49.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_eb))"), 48.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_hg))"), 41.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_ar))"), 39.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_im))"), 27.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_sr))"), 42.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_al))"), 34.0);
    EXPECT_DOUBLE_EQ(evalScalar("numel(properties(h_tb))"), 38.0);
    EXPECT_EQ(evalString("h_ln.Visible"), "on");
}

TEST_P(GraphicsSceneTest, Phase3LiveAutoLimitsColorOrderAndNextPlot)
{
    eval("f = figure('Visible', 'off'); ax = axes(f);"
         "h1 = plot(ax, [0 10], [0 20]);");
    EXPECT_DOUBLE_EQ(evalScalar("ax.XLim(1)"), 0.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.XLim(2)"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.YLim(2)"), 20.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.XAxis.Limits(2)"), 10.0);
    EXPECT_DOUBLE_EQ(evalScalar("h1.SeriesIndex"), 1.0);

    // Live YData mutation automatically updates Axes.YLim when YLimMode == 'auto'
    eval("h1.YData = [0 100];");
    EXPECT_DOUBLE_EQ(evalScalar("ax.YLim(2)"), 100.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.YAxis.Limits(2)"), 100.0);

    // hold on cycles ColorOrderIndex and appends Children
    eval("hold(ax, 'on'); h2 = plot(ax, [0 5], [0 50]);");
    EXPECT_DOUBLE_EQ(evalScalar("numel(ax.Children)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("h2.SeriesIndex"), 2.0);
    EXPECT_TRUE(evalBool("any(h1.Color ~= h2.Color)"));

    // Deleting h1 recomputes auto limits down to h2's range [0 50]
    eval("delete(h1);");
    EXPECT_DOUBLE_EQ(evalScalar("numel(ax.Children)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.YLim(2)"), 50.0);

    // hold off -> new plot replaces previous children and resets SeriesIndex to 1
    eval("hold(ax, 'off'); h3 = plot(ax, [1 2], [3 4]);");
    EXPECT_DOUBLE_EQ(evalScalar("numel(ax.Children)"), 1.0);
    EXPECT_FALSE(evalBool("isvalid(h2)"));
    EXPECT_DOUBLE_EQ(evalScalar("h3.SeriesIndex"), 1.0);
}

TEST_P(GraphicsSceneTest, Phase3BarHistogramPatchAnimatedLineAndAnnotation)
{
    // Histogram live Normalization & NumBins recomputation
    eval("f = figure('Visible', 'off'); ax = axes(f);"
         "h = histogram(ax, [1 1 2 2 2 3 4 4], 4);"
         "v_cnt = h.Values;"
         "h.Normalization = 'probability';"
         "v_prob = h.Values;"
         "h.Normalization = 'cdf';"
         "v_cdf = h.Values;");
    EXPECT_DOUBLE_EQ(evalScalar("sum(v_cnt)"), 8.0);
    EXPECT_NEAR(evalScalar("sum(v_prob)"), 1.0, 1e-12);
    EXPECT_NEAR(evalScalar("v_cdf(end)"), 1.0, 1e-12);

    // Patch bidirectional XData/YData <-> Vertices/Faces sync
    eval("p = patch(ax, [0 2 0], [0 0 3], 'g');");
    EXPECT_DOUBLE_EQ(evalScalar("size(p.Vertices, 1)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("p.Vertices(2, 1)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("p.Vertices(3, 2)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("p.FaceColor(2)"), 1.0);
    eval("p.Vertices = [0 0; 5 0; 5 4; 0 4]; p.Faces = [1 2 3 4];");
    EXPECT_DOUBLE_EQ(evalScalar("max(p.XData(:))"), 5.0);
    EXPECT_DOUBLE_EQ(evalScalar("max(p.YData(:))"), 4.0);

    // AnimatedLine addpoints / getpoints / clearpoints / MaximumNumPoints
    eval("cla(ax, 'reset'); an = animatedline(ax, 'MaximumNumPoints', 3);"
         "addpoints(an, [1 2], [10 20]);"
         "addpoints(an, [3 4], [30 40]);"
         "[axp, ayp] = getpoints(an);");
    EXPECT_DOUBLE_EQ(evalScalar("numel(axp)"), 3.0);
    EXPECT_DOUBLE_EQ(evalScalar("axp(1)"), 2.0);
    EXPECT_DOUBLE_EQ(evalScalar("ayp(3)"), 40.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.YLim(2)"), 40.0);
    eval("clearpoints(an); [axp2, ayp2] = getpoints(an);");
    EXPECT_DOUBLE_EQ(evalScalar("numel(axp2)"), 0.0);

    // imagesc sets YDir='reverse', CDataMapping='scaled', CLim
    eval("im = imagesc(ax, [1 2 3; 4 5 6]);");
    EXPECT_EQ(evalString("ax.YDir"), "reverse");
    EXPECT_EQ(evalString("im.CDataMapping"), "scaled");
    EXPECT_DOUBLE_EQ(evalScalar("ax.CLim(1)"), 1.0);
    EXPECT_DOUBLE_EQ(evalScalar("ax.CLim(2)"), 6.0);

    // fplot & annotation TextBox
    eval("fl = fplot(ax, @(x) x.^2, [0 4]);"
         "tb = annotation(f, 'textbox', [0.1 0.2 0.3 0.4], 'String', 'Hello');");
    EXPECT_NEAR(evalScalar("fl.YData(end)"), 16.0, 1e-12);
    EXPECT_EQ(evalString("tb.String"), "Hello");
    EXPECT_DOUBLE_EQ(evalScalar("tb.Position(3)"), 0.3);
}

INSTANTIATE_DUAL(GraphicsSceneTest);
