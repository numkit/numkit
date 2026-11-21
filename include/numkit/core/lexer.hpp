#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/ref.h>
#include <keel/string.h>

namespace numkit {

enum class TokenType {
    NUMBER,
    IMAG_NUMBER,
    STRING,
    IDENTIFIER,
    PLUS,
    MINUS,
    STAR,
    SLASH,
    BACKSLASH,
    DOT_BACKSLASH,
    CARET,
    DOT_STAR,
    DOT_SLASH,
    DOT_CARET,
    DOT_APOSTROPHE,
    EQ,
    NEQ,
    LT,
    GT,
    LEQ,
    GEQ,
    AND,
    OR,
    TILDE,
    AND_SHORT,
    OR_SHORT,
    ASSIGN,
    LPAREN,
    RPAREN,
    LBRACKET,
    RBRACKET,
    LBRACE,
    RBRACE,
    COMMA,
    SEMICOLON,
    COLON,
    DOT,
    NEWLINE,
    APOSTROPHE,
    AT,
    ELLIPSIS,
    KW_IF,
    KW_ELSEIF,
    KW_ELSE,
    KW_END,
    KW_FOR,
    KW_WHILE,
    KW_BREAK,
    KW_CONTINUE,
    KW_RETURN,
    KW_FUNCTION,
    KW_TRUE,
    KW_FALSE,
    KW_SWITCH,
    KW_CASE,
    KW_OTHERWISE,
    KW_TRY,
    KW_CATCH,
    KW_GLOBAL,
    KW_PERSISTENT,
    KW_CLASSDEF,
    DQSTRING,
    // `%` line comments and `%{ ... %}` block comments — emitted by
    // the lexer so downstream consumers (tools, formatters, the
    // script-graph IDE viewer) can position themselves precisely
    // around them. The parser silently skips COMMENT tokens through
    // its centralized advance() helper, so script-level grammar is
    // unaffected.
    COMMENT,
    END_OF_INPUT
};

struct Token
{
    TokenType type = TokenType::END_OF_INPUT;
    keel::String value;
    keel::i32 line = 0;
    keel::i32 col = 0;
    keel::i32 endLine = 0;
    keel::i32 endCol = 0;

    Token clone(keel::Allocator *alloc = nullptr) const
    {
        Token cp;
        cp.type = type;
        cp.value = value.clone(alloc);
        cp.line = line;
        cp.col = col;
        cp.endLine = endLine;
        cp.endCol = endCol;
        return cp;
    }
};

class Lexer
{
public:
    explicit Lexer(keel::StringRef source);
    keel::Array<Token> tokenize();

private:
    keel::String src_;
    keel::usize pos_ = 0;
    keel::i32 line_ = 1;
    keel::i32 col_ = 1;
    keel::Array<Token> tokens_;
    keel::Array<char> bracketStack_;

    char peek() const;
    char peek(keel::i32 offset) const;
    char advance();

    keel::i32 bracketDepth() const;
    bool inMatrixContext() const;
    void pushBracket(char open);
    void popBracket(char expected);

    bool isAtLineStart() const;
    bool isAtLineStart(keel::usize position) const;

    void skipSpacesAndComments();
    void skipBlockComment();
    void addToken(TokenType type, keel::StringRef val, keel::i32 line, keel::i32 col);
    bool isValueToken(TokenType t) const;
    bool isTransposeContext() const;

    void readNumber();
    void validateUnderscores(keel::usize start, keel::usize end, keel::i32 startLine, keel::i32 startCol);
    void readString(keel::i32 startLine, keel::i32 startCol);
    void readDoubleQuotedString(keel::i32 startLine, keel::i32 startCol);
    void readIdentifier();
    bool readOperator();

    void insertImplicitComma();

    [[noreturn]] void error(keel::StringRef msg);
    [[noreturn]] void error(keel::StringRef msg, keel::i32 line, keel::i32 col);

    static bool isDigit(char c);
    static bool isAlpha(char c);
    static bool isAlnum(char c);
    static bool isXDigit(char c);
    static char closingFor(char open);
};

} // namespace numkit
