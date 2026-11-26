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

INSTANTIATE_DUAL(GraphicsSceneTest);
