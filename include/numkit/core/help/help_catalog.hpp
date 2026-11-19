#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/ref.h>
#include <keel/string.h>

namespace numkit::bundle {

struct HelpEntry {
    keel::String name;
    keel::String summary;
    keel::String signature;
    keel::String doc;

    HelpEntry clone(keel::Allocator *alloc = nullptr) const
    {
        HelpEntry cp;
        cp.name = name.clone(alloc);
        cp.summary = summary.clone(alloc);
        cp.signature = signature.clone(alloc);
        cp.doc = doc.clone(alloc);
        return cp;
    }
};

struct HelpSection {
    keel::String title;
    keel::Array<HelpEntry> entries;
};

struct HelpCategory {
    keel::String name;        // e.g. "elmat", "elfun", "images", "signal"
    keel::String title;       // e.g. "Elementary matrices and matrix manipulation."
    keel::Array<HelpSection> sections;
};

class HelpCatalog {
public:
    static const HelpCatalog &instance();

    // List all registered categories in order
    const keel::Array<HelpCategory> &categories() const { return categories_; }

    // Find category by name (case-insensitive, alias-aware e.g. "images" -> "image")
    const HelpCategory *findCategory(keel::StringRef name) const;

    // Find function help by name (case-insensitive)
    const HelpEntry *findFunction(keel::StringRef name) const;

    // Generate formatted MATLAB-style help overview of all categories
    keel::String formatAllCategories() const;

    // Generate formatted MATLAB-style help for a specific category (e.g. "help elmat")
    keel::String formatCategory(keel::StringRef catName) const;

    // Generate formatted help for a specific function (e.g. "help sin")
    keel::String formatFunction(keel::StringRef funcName) const;

    // Return list of function names in a given category
    keel::Array<keel::String> getCategoryFunctions(keel::StringRef catName) const;

    // Return list of all function names across all categories
    keel::Array<keel::String> getAllFunctions() const;

private:
    HelpCatalog();
    void initCategories();

    keel::Array<HelpCategory> categories_;
    keel::HashMap<keel::String, keel::usize> categoryIndex_;
    keel::HashMap<keel::String, HelpEntry> functionIndex_;
};

} // namespace numkit::bundle
