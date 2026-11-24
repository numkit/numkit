#pragma once

// Shared AST lexical-scope analysis utilities used by both Compiler and TreeWalker
// so nested-function discovery, assigned/identifier scanning, and global/persistent
// collection follow a single source of truth across both execution backends.

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_set.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/ast.hpp>

namespace numkit::scope {

inline bool isPseudoScopeVar(keel::StringRef name) noexcept
{
    return name == "nargin" || name == "nargout"
           || name == "varargin" || name == "varargout";
}

inline const ASTNode *rootTargetIdentifier(const ASTNode *lhs) noexcept
{
    const ASTNode *cur = lhs;
    while (cur && !cur->children.empty()
           && (cur->type == NodeType::CALL || cur->type == NodeType::FIELD_ACCESS
               || cur->type == NodeType::DYNAMIC_FIELD_ACCESS
               || cur->type == NodeType::CELL_INDEX)) {
        cur = cur->children[0].get();
    }
    return (cur && cur->type == NodeType::IDENTIFIER) ? cur : nullptr;
}

inline void collectDirectNestedDefs(const ASTNode *node, keel::Array<const ASTNode *> &out)
{
    if (!node || node->type == NodeType::ANON_FUNC || node->type == NodeType::CLASSDEF_DEF)
        return;
    if (node->type == NodeType::FUNCTION_DEF) {
        out.push_back(node);
        return;
    }
    for (const auto &ch : node->children)
        collectDirectNestedDefs(ch.get(), out);
    for (const auto &b : node->branches) {
        collectDirectNestedDefs(b.cond.get(), out);
        collectDirectNestedDefs(b.body.get(), out);
    }
    if (node->elseBranch)
        collectDirectNestedDefs(node->elseBranch.get(), out);
}

inline void collectScopeGlobalPersistentDecls(const ASTNode *node,
                                              keel::Array<keel::String> &globals,
                                              keel::Array<keel::String> &persistents)
{
    if (!node || node->type == NodeType::FUNCTION_DEF
        || node->type == NodeType::ANON_FUNC
        || node->type == NodeType::CLASSDEF_DEF)
        return;
    if (node->type == NodeType::GLOBAL_STMT) {
        for (const auto &nm : node->paramNames) {
            bool found = false;
            for (const auto &g : globals) {
                if (g == nm) {
                    found = true;
                    break;
                }
            }
            if (!found)
                globals.push_back(nm.clone());
        }
        return;
    }
    if (node->type == NodeType::PERSISTENT_STMT) {
        for (const auto &nm : node->paramNames) {
            bool found = false;
            for (const auto &p : persistents) {
                if (p == nm) {
                    found = true;
                    break;
                }
            }
            if (!found)
                persistents.push_back(nm.clone());
        }
        return;
    }
    for (const auto &ch : node->children)
        collectScopeGlobalPersistentDecls(ch.get(), globals, persistents);
    for (const auto &b : node->branches) {
        collectScopeGlobalPersistentDecls(b.cond.get(), globals, persistents);
        collectScopeGlobalPersistentDecls(b.body.get(), globals, persistents);
    }
    if (node->elseBranch)
        collectScopeGlobalPersistentDecls(node->elseBranch.get(), globals, persistents);
}

inline void scanBodyLexical(const ASTNode *node,
                            keel::HashSet<keel::String> &assigned,
                            keel::HashSet<keel::String> &identifiers,
                            keel::HashSet<keel::String> &ownGlobPers)
{
    if (!node || node->type == NodeType::FUNCTION_DEF || node->type == NodeType::CLASSDEF_DEF)
        return;
    switch (node->type) {
    case NodeType::IDENTIFIER:
        if (!node->strValue.empty())
            identifiers.insert(node->strValue.clone());
        break;
    case NodeType::ASSIGN:
    case NodeType::DELETE_ASSIGN:
        if (!node->children.empty()) {
            if (const ASTNode *id = rootTargetIdentifier(node->children[0].get()))
                assigned.insert(id->strValue.clone());
        }
        break;
    case NodeType::MULTI_ASSIGN:
        for (const auto &rn : node->returnNames) {
            if (!rn.empty() && rn != "~")
                assigned.insert(rn.clone());
        }
        for (const auto &lt : node->lhsTargets) {
            if (const ASTNode *id = rootTargetIdentifier(lt.get()))
                assigned.insert(id->strValue.clone());
        }
        break;
    case NodeType::FOR_STMT:
        if (!node->strValue.empty())
            assigned.insert(node->strValue.clone());
        break;
    case NodeType::TRY_STMT:
        if (!node->strValue.empty() && node->children.size() > 1)
            assigned.insert(node->strValue.clone());
        break;
    case NodeType::GLOBAL_STMT:
    case NodeType::PERSISTENT_STMT:
        for (const auto &nm : node->paramNames)
            ownGlobPers.insert(nm.clone());
        break;
    default:
        break;
    }
    for (const auto &lt : node->lhsTargets)
        scanBodyLexical(lt.get(), assigned, identifiers, ownGlobPers);
    for (const auto &ch : node->children)
        scanBodyLexical(ch.get(), assigned, identifiers, ownGlobPers);
    for (const auto &b : node->branches) {
        scanBodyLexical(b.cond.get(), assigned, identifiers, ownGlobPers);
        scanBodyLexical(b.body.get(), assigned, identifiers, ownGlobPers);
    }
    if (node->elseBranch)
        scanBodyLexical(node->elseBranch.get(), assigned, identifiers, ownGlobPers);
}

} // namespace numkit::scope
