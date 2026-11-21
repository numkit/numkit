// src/ast.cpp
#include <numkit/core/ast.hpp>

#include <keel/small_array.h>

namespace numkit {

// ============================================================
// Фабрики узлов
// ============================================================

ASTNodePtr makeNode(NodeType t)
{
    return keel::make_unique<ASTNode>(t);
}

ASTNodePtr makeNode(NodeType t, keel::i32 line, keel::i32 col)
{
    auto node = keel::make_unique<ASTNode>(t);
    node->line = line;
    node->col = col;
    return node;
}

ASTNodePtr cloneNode(const ASTNode *src)
{
    if (!src)
        return nullptr;

    auto dst = keel::make_unique<ASTNode>(src->type);
    dst->line = src->line;
    dst->col = src->col;
    dst->endLine = src->endLine;
    dst->endCol = src->endCol;
    dst->strValue = src->strValue.clone();
    dst->numValue = src->numValue;
    dst->boolValue = src->boolValue;
    dst->suppressOutput = src->suppressOutput;
    dst->paramNames = src->paramNames.clone();
    dst->returnNames = src->returnNames.clone();
    dst->classAttrs = src->classAttrs.clone();

    dst->children.reserve(src->children.size());
    for (const auto &child : src->children)
        dst->children.push_back(cloneNode(child.get()));

    dst->lhsTargets.reserve(src->lhsTargets.size());
    for (const auto &t : src->lhsTargets)
        dst->lhsTargets.push_back(cloneNode(t.get()));

    dst->branches.reserve(src->branches.size());
    for (const auto &[cond, body] : src->branches)
        dst->branches.push_back(ASTBranch{cloneNode(cond.get()), cloneNode(body.get())});

    dst->elseBranch = cloneNode(src->elseBranch.get());

    return dst;
}

keel::String tryBuildQualifiedName(const ASTNode *node, const ASTNode **rootIdent)
{
    if (!node || node->type != NodeType::FIELD_ACCESS)
        return {};
    keel::SmallArray<keel::StringRef, 8> parts;
    const ASTNode *cur = node;
    while (cur && cur->type == NodeType::FIELD_ACCESS && cur->children.size() == 1) {
        parts.push_back(cur->strValue.as_ref());
        cur = cur->children[0].get();
    }
    if (!cur || cur->type != NodeType::IDENTIFIER)
        return {};
    if (rootIdent)
        *rootIdent = cur;
    keel::usize totalLen = cur->strValue.size();
    for (keel::usize i = 0; i < parts.size(); ++i)
        totalLen += 1 + parts[i].size();
    keel::String out;
    out.reserve(totalLen);
    out.append(cur->strValue.as_ref());
    for (keel::usize i = parts.size(); i > 0; --i) {
        out.push_back('.');
        out.append(parts[i - 1]);
    }
    return out;
}

} // namespace numkit
