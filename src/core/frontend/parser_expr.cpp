// src/core/frontend/parser_expr.cpp
// Expression precedence climbing, postfix/primary, anonymous function, and array literal parsing.
#include <keel/core.h>
#include <keel/format.h>
#include <keel/number.h>
#include <keel/utf8.h>
#include <numkit/core/parser.hpp>
#include <numkit/value/error.hpp>

namespace numkit {

ASTNodePtr Parser::parseExpression()
{
    NestingGuard guard(*this);
    return parseShortCircuitOr();
}

// || (short-circuit OR) — самый низкий приоритет
ASTNodePtr Parser::parseShortCircuitOr()
{
    auto left = parseShortCircuitAnd();
    while (check(TokenType::OR_SHORT)) {
        auto [ln, cl] = loc();
        keel::String op = current().value.clone();
        advance();
        auto n = makeNode(NodeType::BINARY_OP, ln, cl);
        n->strValue = keel::move(op);
        n->children.push_back(keel::move(left));
        n->children.push_back(parseShortCircuitAnd());
        left = keel::move(n);
    }
    return left;
}

// && (short-circuit AND)
ASTNodePtr Parser::parseShortCircuitAnd()
{
    auto left = parseElementOr();
    while (check(TokenType::AND_SHORT)) {
        auto [ln, cl] = loc();
        keel::String op = current().value.clone();
        advance();
        auto n = makeNode(NodeType::BINARY_OP, ln, cl);
        n->strValue = keel::move(op);
        n->children.push_back(keel::move(left));
        n->children.push_back(parseElementOr());
        left = keel::move(n);
    }
    return left;
}

// | (element-wise OR)
ASTNodePtr Parser::parseElementOr()
{
    auto left = parseElementAnd();
    while (check(TokenType::OR)) {
        auto [ln, cl] = loc();
        keel::String op = current().value.clone();
        advance();
        auto n = makeNode(NodeType::BINARY_OP, ln, cl);
        n->strValue = keel::move(op);
        n->children.push_back(keel::move(left));
        n->children.push_back(parseElementAnd());
        left = keel::move(n);
    }
    return left;
}

// & (element-wise AND)
ASTNodePtr Parser::parseElementAnd()
{
    auto left = parseComparison();
    while (check(TokenType::AND)) {
        auto [ln, cl] = loc();
        keel::String op = current().value.clone();
        advance();
        auto n = makeNode(NodeType::BINARY_OP, ln, cl);
        n->strValue = keel::move(op);
        n->children.push_back(keel::move(left));
        n->children.push_back(parseComparison());
        left = keel::move(n);
    }
    return left;
}

ASTNodePtr Parser::parseComparison()
{
    auto left = parseColon();
    while (check(TokenType::EQ) || check(TokenType::NEQ) || check(TokenType::LT)
           || check(TokenType::GT) || check(TokenType::LEQ) || check(TokenType::GEQ)) {
        auto [ln, cl] = loc();
        keel::String op = current().value.clone();
        advance();
        auto n = makeNode(NodeType::BINARY_OP, ln, cl);
        n->strValue = keel::move(op);
        n->children.push_back(keel::move(left));
        n->children.push_back(parseColon());
        left = keel::move(n);
    }
    return left;
}

ASTNodePtr Parser::parseColon()
{
    auto start = parseAddSub();
    if (check(TokenType::COLON)) {
        auto [ln, cl] = loc();
        advance();
        auto second = parseAddSub();
        if (check(TokenType::COLON)) {
            advance();
            auto n = makeNode(NodeType::COLON_EXPR, ln, cl);
            n->children.push_back(keel::move(start));
            n->children.push_back(keel::move(second));
            n->children.push_back(parseAddSub());
            return n;
        }
        auto n = makeNode(NodeType::COLON_EXPR, ln, cl);
        n->children.push_back(keel::move(start));
        n->children.push_back(keel::move(second));
        return n;
    }
    return start;
}

ASTNodePtr Parser::parseAddSub()
{
    auto left = parseMulDiv();
    while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
        auto [ln, cl] = loc();
        keel::String op = current().value.clone();
        advance();
        auto n = makeNode(NodeType::BINARY_OP, ln, cl);
        n->strValue = keel::move(op);
        n->children.push_back(keel::move(left));
        n->children.push_back(parseMulDiv());
        left = keel::move(n);
    }
    return left;
}

ASTNodePtr Parser::parseMulDiv()
{
    auto left = parseUnary();
    while (check(TokenType::STAR) || check(TokenType::SLASH) || check(TokenType::BACKSLASH)
           || check(TokenType::DOT_BACKSLASH) || check(TokenType::DOT_STAR)
           || check(TokenType::DOT_SLASH)) {
        auto [ln, cl] = loc();
        keel::String op = current().value.clone();
        advance();
        auto n = makeNode(NodeType::BINARY_OP, ln, cl);
        n->strValue = keel::move(op);
        n->children.push_back(keel::move(left));
        n->children.push_back(parseUnary());
        left = keel::move(n);
    }
    return left;
}

// FIX #4: unary plus создаёт узел AST (для совместимости с uplus)
ASTNodePtr Parser::parseUnary()
{
    if (check(TokenType::MINUS)) {
        auto [ln, cl] = loc();
        advance();
        auto n = makeNode(NodeType::UNARY_OP, ln, cl);
        n->strValue.assign("-");
        n->children.push_back(parseUnary());
        return n;
    }
    if (check(TokenType::TILDE)) {
        auto [ln, cl] = loc();
        advance();
        auto n = makeNode(NodeType::UNARY_OP, ln, cl);
        n->strValue.assign("~");
        n->children.push_back(parseUnary());
        return n;
    }
    if (check(TokenType::PLUS)) {
        auto [ln, cl] = loc();
        advance();
        auto n = makeNode(NodeType::UNARY_OP, ln, cl);
        n->strValue.assign("+");
        n->children.push_back(parseUnary());
        return n;
    }
    return parsePower();
}

ASTNodePtr Parser::parsePower()
{
    auto left = parsePostfix();
    if (check(TokenType::CARET) || check(TokenType::DOT_CARET)) {
        auto [ln, cl] = loc();
        keel::String op = current().value.clone();
        advance();
        auto n = makeNode(NodeType::BINARY_OP, ln, cl);
        n->strValue = keel::move(op);
        n->children.push_back(keel::move(left));
        n->children.push_back(parseUnary()); // правоассоциативность
        return n;
    }
    return left;
}

ASTNodePtr Parser::parsePostfix()
{
    auto node = parsePrimary();
    while (true) {
        keel::usize before = pos_;

        if (check(TokenType::LPAREN)) {
            // В MATLAB f(args) может быть как вызовом функции, так и
            // индексацией массива. Различение происходит на этапе интерпретации.
            auto [ln, cl] = loc();
            advance();
            auto cn = makeNode(NodeType::CALL, ln, cl);
            cn->children.push_back(keel::move(node));
            auto parseCallArg = [&]() {
                if (check(TokenType::IDENTIFIER) && peekToken(1).type == TokenType::ASSIGN) {
                    auto [aln, acl] = loc();
                    auto k = makeNode(NodeType::STRING_LITERAL, aln, acl);
                    k->strValue = current().value.clone();
                    advance(); // IDENT
                    advance(); // '='
                    cn->children.push_back(keel::move(k));
                }
                cn->children.push_back(parseExpression());
            };
            if (!check(TokenType::RPAREN)) {
                parseCallArg();
                while (match(TokenType::COMMA))
                    parseCallArg();
            }
            consume(TokenType::RPAREN, ")");
            node = keel::move(cn);
        } else if (check(TokenType::LBRACE)) {
            auto [ln, cl] = loc();
            advance();
            auto cn = makeNode(NodeType::CELL_INDEX, ln, cl);
            cn->children.push_back(keel::move(node));
            cn->children.push_back(parseExpression());
            while (match(TokenType::COMMA))
                cn->children.push_back(parseExpression());
            consume(TokenType::RBRACE, "}");
            node = keel::move(cn);
        } else if (check(TokenType::AT) && peekToken(1).type == TokenType::IDENTIFIER) {
            // Superclass-qualified reference `lhs@Base` (classdef super-call).
            auto [ln, cl] = loc();
            advance(); // @
            auto sn = makeNode(NodeType::SUPERCLASS_REF, ln, cl);
            sn->strValue = consume(TokenType::IDENTIFIER, "superclass").value;
            sn->children.push_back(keel::move(node));
            node = keel::move(sn);
        } else if (check(TokenType::DOT)
                   && (peekToken(1).type == TokenType::IDENTIFIER
                       || (!peekToken(1).value.empty()
                           && keel::is_ascii_alpha(peekToken(1).value[0])))) {
            auto [ln, cl] = loc();
            advance();
            auto fn = makeNode(NodeType::FIELD_ACCESS, ln, cl);
            fn->strValue = current().value.clone();
            advance();
            fn->children.push_back(keel::move(node));
            node = keel::move(fn);
        } else if (check(TokenType::DOT) && peekToken(1).type == TokenType::LPAREN) {
            // s.(expr) — dynamic field access
            auto [ln, cl] = loc();
            advance(); // consume DOT
            advance(); // consume LPAREN
            auto fn = makeNode(NodeType::DYNAMIC_FIELD_ACCESS, ln, cl);
            fn->children.push_back(keel::move(node));   // child[0] = object
            fn->children.push_back(parseExpression()); // child[1] = field name expr
            consume(TokenType::RPAREN, ")");
            node = keel::move(fn);
        } else if (check(TokenType::APOSTROPHE)) {
            auto [ln, cl] = loc();
            advance();
            auto tn = makeNode(NodeType::UNARY_OP, ln, cl);
            tn->strValue.assign("'");
            tn->children.push_back(keel::move(node));
            node = keel::move(tn);
        } else if (check(TokenType::DOT_APOSTROPHE)) {
            auto [ln, cl] = loc();
            advance();
            auto tn = makeNode(NodeType::UNARY_OP, ln, cl);
            tn->strValue.assign(".'");
            tn->children.push_back(keel::move(node));
            node = keel::move(tn);
        } else {
            break;
        }

        if (pos_ == before)
            break;
    }
    return node;
}

ASTNodePtr Parser::parsePrimary()
{
    NestingGuard guard(*this);
    auto [ln, cl] = loc();

    switch (current().type) {
    case TokenType::NUMBER: {
        auto n = makeNode(NodeType::NUMBER_LITERAL, ln, cl);
        n->numValue = parseDouble(current().value, ln, cl);
        n->strValue = current().value.clone();
        advance();
        return n;
    }
    case TokenType::IMAG_NUMBER: {
        auto n = makeNode(NodeType::IMAG_LITERAL, ln, cl);
        n->numValue = parseDouble(current().value, ln, cl);
        n->strValue = current().value.clone();
        advance();
        return n;
    }
    case TokenType::STRING: {
        auto n = makeNode(NodeType::STRING_LITERAL, ln, cl);
        n->strValue = current().value.clone();
        advance();
        return n;
    }
    case TokenType::DQSTRING: {
        auto n = makeNode(NodeType::DQSTRING_LITERAL, ln, cl);
        n->strValue = current().value.clone();
        advance();
        return n;
    }
    case TokenType::KW_TRUE: {
        advance();
        auto n = makeNode(NodeType::BOOL_LITERAL, ln, cl);
        n->boolValue = true;
        return n;
    }
    case TokenType::KW_FALSE: {
        advance();
        auto n = makeNode(NodeType::BOOL_LITERAL, ln, cl);
        n->boolValue = false;
        return n;
    }
    case TokenType::KW_END: {
        advance();
        return makeNode(NodeType::END_VAL, ln, cl);
    }
    case TokenType::IDENTIFIER: {
        auto n = makeNode(NodeType::IDENTIFIER, ln, cl);
        n->strValue = current().value.clone();
        advance();
        return n;
    }
    case TokenType::AT:
        return parseAnonFunc();
    case TokenType::LPAREN: {
        advance();
        auto e = parseExpression();
        // Поддержка (x = expr) — присваивание как выражение.
        // Используется в short-circuit: if cond && (x = 1)
        if (check(TokenType::ASSIGN) && e->type == NodeType::IDENTIFIER) {
            advance();
            auto rhs = parseExpression();
            auto assignNode = makeNode(NodeType::ASSIGN, e->line, e->col);
            assignNode->children.push_back(keel::move(e));
            assignNode->children.push_back(keel::move(rhs));
            assignNode->suppressOutput = true;
            consume(TokenType::RPAREN, ")");
            return assignNode;
        }
        consume(TokenType::RPAREN, ")");
        return e;
    }
    case TokenType::LBRACKET:
        return parseMatrixLiteral();
    case TokenType::LBRACE:
        return parseCellLiteral();
    case TokenType::COLON: {
        advance();
        return makeNode(NodeType::COLON_EXPR, ln, cl);
    }
    default: {
        keel::String err = keel::format("Unexpected token '{}' at line {} col {}",
                                        current().value, ln, cl);
        throw numkit::Error(err.c_str());
    }
    }
}

// ============================================================
// Anonymous functions
// ============================================================

ASTNodePtr Parser::parseAnonFunc()
{
    auto [ln, cl] = loc();
    const keel::usize atPos = pos_;   // index of '@' — start of the source span
    consume(TokenType::AT, "@");

    // @funcName — хэндл на существующую функцию
    if (check(TokenType::IDENTIFIER) && peekToken(1).type != TokenType::LPAREN) {
        auto n = makeNode(NodeType::ANON_FUNC, ln, cl);
        n->strValue = current().value.clone();
        advance();
        return n;
    }

    // @(params) expr — анонимная функция
    auto n = makeNode(NodeType::ANON_FUNC, ln, cl);
    consume(TokenType::LPAREN, "(");
    auto consumeAnonParam = [&]() -> keel::String {
        if (match(TokenType::TILDE))
            return keel::format("__unused_{}", n->paramNames.size());
        return consume(TokenType::IDENTIFIER, "param").value;
    };
    if (!check(TokenType::RPAREN)) {
        n->paramNames.push_back(consumeAnonParam());
        while (match(TokenType::COMMA))
            n->paramNames.push_back(consumeAnonParam());
    }
    consume(TokenType::RPAREN, ")");
    n->children.push_back(parseExpression());
    // Reconstruct the source text from the token span [atPos, pos_) so func2str
    // can return it (e.g. "@(x)x+1"). Stored in strValue: the @funcName
    // discriminator (strValue non-empty AND no children) still holds because an
    // anonymous node always carries a body child.
    n->strValue = reconstructAnonSource(atPos, pos_);
    return n;
}

// Reconstruct anonymous-function source from the token stream, MATLAB-style:
// tokens concatenated with NO separators (insignificant whitespace dropped),
// char/string literals re-quoted (internal quotes doubled). This reproduces
// MATLAB's func2str normalization (`@(x) x + 1` -> `@(x)x+1`, while
// `' world'` keeps its interior space).
keel::String Parser::reconstructAnonSource(keel::usize from, keel::usize to) const
{
    keel::String s;
    for (keel::usize i = from; i < to && i < tokens_.size(); ++i) {
        const Token &t = tokens_[i];
        switch (t.type) {
        case TokenType::STRING:           // 'char literal'
            s.push_back('\'');
            for (char c : t.value) {
                if (c == '\'')
                    s.append("''");
                else
                    s.push_back(c);
            }
            s.push_back('\'');
            break;
        case TokenType::DQSTRING:         // "string literal"
            s.push_back('"');
            for (char c : t.value) {
                if (c == '"')
                    s.append("\"\"");
                else
                    s.push_back(c);
            }
            s.push_back('"');
            break;
        case TokenType::COMMENT:
        case TokenType::NEWLINE:
            break;                        // not part of the expression text
        default:
            s.append(t.value);            // operators / identifiers / numbers verbatim
        }
    }
    return s;
}

// ============================================================
// Array / Cell literals (общий код)
// ============================================================

ASTNodePtr Parser::parseArrayLiteral(TokenType open, TokenType close, NodeType nodeType)
{
    NestingGuard guard(*this);
    auto [ln, cl] = loc();
    const char *openStr = (open == TokenType::LBRACKET) ? "[" : "{";
    const char *closeStr = (close == TokenType::RBRACKET) ? "]" : "}";
    consume(open, openStr);

    auto node = makeNode(nodeType, ln, cl);

    if (check(close)) {
        advance();
        return node;
    }

    auto row = makeNode(NodeType::BLOCK, current().line, current().col);
    row->children.push_back(parseExpression());

    while (!check(close) && !isAtEnd()) {
        keel::usize beforeIteration = pos_;

        if (check(TokenType::SEMICOLON) || check(TokenType::NEWLINE)) {
            node->children.push_back(keel::move(row));
            row = makeNode(NodeType::BLOCK, current().line, current().col);
            advance();
            while (!isAtEnd() && (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON)))
                advance();
            if (check(close))
                break;
        } else if (check(TokenType::COMMA)) {
            advance();
        }

        if (!check(close) && !check(TokenType::SEMICOLON) && !check(TokenType::NEWLINE))
            row->children.push_back(parseExpression());

        if (pos_ == beforeIteration) {
            keel::String err = keel::format(
                "Parse error: stuck in array literal at line {} col {}",
                current().line, current().col);
            throw numkit::Error(err.c_str());
        }
    }

    if (!row->children.empty())
        node->children.push_back(keel::move(row));

    consume(close, closeStr);
    return node;
}

ASTNodePtr Parser::parseMatrixLiteral()
{
    NestingGuard guard(*this);
    return parseArrayLiteral(TokenType::LBRACKET, TokenType::RBRACKET, NodeType::MATRIX_LITERAL);
}

ASTNodePtr Parser::parseCellLiteral()
{
    NestingGuard guard(*this);
    return parseArrayLiteral(TokenType::LBRACE, TokenType::RBRACE, NodeType::CELL_LITERAL);
}

} // namespace numkit
