// src/bundle/src/help/help.cpp
//
// Help system C++ API and Engine registration adapters for NK bundle.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/memory.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/help.hpp>
#include "core/help/help_catalog.hpp"
#include <numkit/core/engine.hpp>
#include <numkit/value/value.hpp>

namespace numkit::bundle {

// ════════════════════════════════════════════════════════════════════════
// C++ API
// ════════════════════════════════════════════════════════════════════════

keel::String help(keel::StringRef query)
{
    const auto &catalog = HelpCatalog::instance();
    if (query.empty()) {
        return catalog.formatAllCategories();
    }
    const HelpCategory *cat = catalog.findCategory(query);
    if (cat) {
        return catalog.formatCategory(cat->name);
    }
    const HelpEntry *func = catalog.findFunction(query);
    if (func) {
        return catalog.formatFunction(func->name);
    }
    return keel::format("'{}' not found. Type 'help' for a list of topics.\n", query);
}

Value help(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    keel::String query = args.empty() ? keel::String{} : args[0].toString();
    return Value::fromString(help(query), mr);
}

keel::Array<keel::String> what(keel::StringRef category)
{
    const auto &catalog = HelpCatalog::instance();
    const HelpCategory *cat = catalog.findCategory(category);
    if (cat) {
        return catalog.getCategoryFunctions(cat->name);
    }
    return catalog.getCategoryFunctions(category);
}

Value what(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    const auto &catalog = HelpCatalog::instance();
    keel::String topic = args.empty() ? keel::String::from("elmat") : args[0].toString();
    const HelpCategory *cat = catalog.findCategory(topic);
    keel::Array<keel::String> funcs = what(topic);

    Value cellM = Value::cell(funcs.size(), 1, mr);
    for (keel::usize i = 0; i < funcs.size(); ++i) {
        cellM.cellAt(i) = Value::fromString(funcs[i], mr);
    }
    Value st = Value::structure(mr);
    st.structFields().put(keel::String::from("path"),
                          Value::fromString(cat ? cat->name.as_ref() : topic.as_ref(), mr));
    st.structFields().put(keel::String::from("m"), keel::move(cellM));
    st.structFields().put(keel::String::from("classes"), Value::cell(0, 1, mr));
    st.structFields().put(keel::String::from("packages"), Value::cell(0, 1, mr));
    return st;
}

keel::Array<keel::String> builtins(keel::StringRef category)
{
    const auto &catalog = HelpCatalog::instance();
    if (category.empty()) {
        return catalog.getAllFunctions();
    }
    return catalog.getCategoryFunctions(category);
}

Value builtins(keel::ArrayRef<Value> args, keel::Allocator *mr)
{
    keel::String topic = args.empty() ? keel::String{} : args[0].toString();
    keel::Array<keel::String> funcs = builtins(topic);
    Value cellOut = Value::cell(funcs.size(), 1, mr);
    for (keel::usize i = 0; i < funcs.size(); ++i) {
        cellOut.cellAt(i) = Value::fromString(funcs[i], mr);
    }
    return cellOut;
}

keel::Array<keel::String> categories()
{
    const auto &catalog = HelpCatalog::instance();
    keel::Array<keel::String> cats;
    cats.reserve(catalog.categories().size());
    for (const auto &c : catalog.categories()) {
        cats.push_back(c.name.clone());
    }
    return cats;
}

Value categories(keel::Allocator *mr)
{
    const auto &catalog = HelpCatalog::instance();
    Value cellOut = Value::cell(catalog.categories().size(), 1, mr);
    for (keel::usize i = 0; i < catalog.categories().size(); ++i) {
        cellOut.cellAt(i) = Value::fromString(catalog.categories()[i].name, mr);
    }
    return cellOut;
}

// ════════════════════════════════════════════════════════════════════════
// Engine Registration Adapters
// ════════════════════════════════════════════════════════════════════════

namespace detail {

static void help_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    const auto &catalog = HelpCatalog::instance();
    keel::String text;

    if (args.empty()) {
        text = catalog.formatAllCategories();
    } else {
        keel::String query = args[0].isChar() ? args[0].toString() : keel::String{};
        if (query.empty() && args[0].isString()) query = args[0].toString();

        const HelpCategory *cat = catalog.findCategory(query);
        if (cat) {
            text = catalog.formatCategory(cat->name);
        } else {
            const HelpEntry *func = catalog.findFunction(query);
            if (func) {
                text = catalog.formatFunction(func->name);
            } else if (coreEngine(ctx).hasUserFunction(query)) {
                text = keel::format("{} is a user-defined function.\n", query);
            } else if (coreEngine(ctx).hasExternalFunction(query)) {
                text = keel::format("{} is a built-in function.\n", query);
            } else {
                text = keel::format("'{}' not found. Type 'help' for a list of topics.\n", query);
            }
        }
    }

    if (nargout > 0) {
        outs[0] = Value::fromString(text, ctx.engine->allocator());
    } else {
        coreEngine(ctx).outputText(text);
        outs[0] = Value();
    }
}

static void what_reg(keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    if (nargout > 0) {
        outs[0] = what(args, ctx.engine->allocator());
    } else {
        keel::String topic = args.empty() ? keel::String::from("elmat")
                                          : (args[0].isChar() ? args[0].toString() : keel::String{});
        const auto &catalog = HelpCatalog::instance();
        const HelpCategory *cat = catalog.findCategory(topic);
        keel::Array<keel::String> funcs = what(topic);
        keel::StringRef title = cat ? cat->title.as_ref() : topic.as_ref();

        keel::String out = keel::format("Functions in {} ({}):\n\n", topic, title);
        for (keel::usize i = 0; i < funcs.size(); ++i) {
            keel::format_into(out, "{:<16}", funcs[i]);
            if ((i + 1) % 4 == 0 || i + 1 == funcs.size()) out.push_back('\n');
        }
        out.push_back('\n');
        coreEngine(ctx).outputText(out);
        outs[0] = Value();
    }
}

static void builtins_reg(keel::ArrayRef<Value> args, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    outs[0] = builtins(args, ctx.engine->allocator());
}

static void categories_reg(keel::ArrayRef<Value>, keel::usize, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    outs[0] = categories(ctx.engine->allocator());
}

static void inmem_reg(keel::ArrayRef<Value>, keel::usize nargout, keel::MutArrayRef<Value> outs, CallContext &ctx) {
    keel::Array<keel::String> userFuncs;
    keel::Array<keel::String> classes;

    for (const auto &name : coreEngine(ctx).namespaces()) {
        classes.push_back(name.clone());
    }

    Value mCell = Value::cell(userFuncs.size(), 1, ctx.engine->allocator());
    for (keel::usize i = 0; i < userFuncs.size(); ++i) {
        mCell.cellAt(i) = Value::fromString(userFuncs[i], ctx.engine->allocator());
    }
    outs[0] = keel::move(mCell);

    if (nargout > 1) {
        outs[1] = Value::cell(0, 1, ctx.engine->allocator()); // MEX
    }
    if (nargout > 2) {
        Value cCell = Value::cell(classes.size(), 1, ctx.engine->allocator());
        for (keel::usize i = 0; i < classes.size(); ++i) {
            cCell.cellAt(i) = Value::fromString(classes[i], ctx.engine->allocator());
        }
        outs[2] = keel::move(cCell);
    }
}

} // namespace detail

void HelpLibrary::install(Engine &engine)
{
    engine.registerFunction("help",       &detail::help_reg);
    engine.registerFunction("doc",        &detail::help_reg);
    engine.registerFunction("what",       &detail::what_reg);
    engine.registerFunction("builtins",   &detail::builtins_reg);
    engine.registerFunction("categories", &detail::categories_reg);
    engine.registerFunction("inmem",      &detail::inmem_reg);
}

} // namespace numkit::bundle
