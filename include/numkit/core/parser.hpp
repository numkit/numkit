#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/ref.h>
#include <keel/string.h>
#include <numkit/core/ast.hpp>
#include <numkit/value/error.hpp>
#include <numkit/core/lexer.hpp>
#include <initializer_list>

namespace numkit {

class Parser
{
public:
    explicit Parser(keel::ArrayRef<Token> tokens);
    explicit Parser(keel::Array<Token> &&tokens);
    ASTNodePtr parse();

private:
    keel::Array<Token> tokens_;
    keel::usize pos_ = 0;
    keel::usize nestingDepth_ = 0;
    static constexpr keel::usize MAX_NESTING_DEPTH = 200;
    // Statement ordinal within this unit (increments at every
    // parseStatement entry, nested blocks included). classdef is only
    // legal as the very first statement — MATLAB rejects it anywhere
    // else with "Illegal use of reserved keyword" (parser_test:
    // ClassdefMidScriptRejected).
    keel::usize stmtOrdinal_ = 0;

    struct NestingGuard
    {
        Parser &p;
        explicit NestingGuard(Parser &parser);
        ~NestingGuard() { --p.nestingDepth_; }
    };

    // — Позиция в исходнике —
    struct SourceLoc
    {
        keel::i32 line;
        keel::i32 col;
    };
    SourceLoc loc() const;

    // — Утилиты навигации по токенам —
    const Token &current() const;
    const Token &peekToken(keel::i32 off = 0) const;
    bool isAtEnd() const;
    bool check(TokenType t) const;
    bool match(TokenType t);
    Token consume(TokenType t, keel::StringRef msg = "");
    void skipNewlines();
    void skipTerminators();
    // Consume a statement-terminating SEMICOLON or COMMA token (if
    // present), recording its position into node.endLine / node.endCol
    // so downstream tools (the graph viewer's sourceText slicer) can
    // bound the statement's source range precisely. Returns true iff
    // a SEMICOLON was consumed (i.e. suppress-output is set); COMMA
    // returns false (it terminates the stmt but doesn't suppress).
    bool consumeStmtTerminator(ASTNode &node);

    // Single chokepoint for token-position changes. After moving pos_
    // forward, skips past any COMMENT tokens so the parser never has
    // to deal with them — COMMENT is purely lexical metadata for
    // downstream tools (script-graph viewer, formatters, doc
    // extractors). All `pos_++` sites in parser.cpp go through this
    // helper, plus the constructor invokes skipPastComments() once
    // to handle a leading comment.
    void advance();
    void skipPastComments();

    // — Проверка терминаторов —
    bool isTerminator(std::initializer_list<TokenType> terminators) const;

    // — Безопасный парсинг числа —
    static keel::f64 parseDouble(keel::StringRef text, keel::i32 line, keel::i32 col);

    // — Command-style calls (clear all, grid on, cd dir, etc.) —
    bool isCommandStyleCall() const;
    ASTNodePtr parseCommandStyleCall();

    // — Statements —
    ASTNodePtr parseStatement();
    ASTNodePtr parseExpressionStatement();
    ASTNodePtr tryMultiAssign();
    ASTNodePtr parseIf();
    ASTNodePtr parseFor();
    ASTNodePtr parseWhile();
    ASTNodePtr parseSwitch();
    ASTNodePtr parseFunctionDef();
    ASTNodePtr parseClassDef();
    ASTNodePtr parseTryCatch();
    ASTNodePtr parseGlobalPersistent();
    ASTNodePtr parseBlock(std::initializer_list<TokenType> terminators);

    // — Expressions (MATLAB precedence, low to high) —
    ASTNodePtr parseExpression();
    ASTNodePtr parseShortCircuitOr();  // ||
    ASTNodePtr parseShortCircuitAnd(); // &&
    ASTNodePtr parseElementOr();       // |
    ASTNodePtr parseElementAnd();      // &
    ASTNodePtr parseComparison();      // == ~= < > <= >=
    ASTNodePtr parseColon();           // :
    ASTNodePtr parseAddSub();          // + -
    ASTNodePtr parseMulDiv();          // * / .* ./
    ASTNodePtr parseUnary();           // - ~ +
    ASTNodePtr parsePower();           // ^ .^
    ASTNodePtr parsePostfix();         // () {} . ' .'
    ASTNodePtr parsePrimary();

    // — Литералы —
    ASTNodePtr parseArrayLiteral(TokenType open, TokenType close, NodeType nodeType);
    ASTNodePtr parseMatrixLiteral();
    ASTNodePtr parseCellLiteral();
    ASTNodePtr parseAnonFunc();
    // Reconstruct anonymous-function source from the token span [from, to)
    // (for func2str). MATLAB-style: no inter-token whitespace, literals re-quoted.
    keel::String reconstructAnonSource(keel::usize from, keel::usize to) const;

    // — Lookahead для function def —
    bool probeHasOutputSignature() const;
};

} // namespace numkit
