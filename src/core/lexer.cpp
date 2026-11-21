#include <keel/core.h>
#include <keel/format.h>
#include <keel/utf8.h>
#include <numkit/core/lexer.hpp>
#include <stdexcept>

namespace numkit {

namespace {

TokenType lookupKeyword(keel::StringRef word) noexcept
{
    // Note: `true` and `false` are NOT keywords. MATLAB treats them
    // as built-in functions (registered in toolboxes/builtin), so the
    // lexer emits IDENTIFIER for them. Bare `true` resolves to a
    // 0-arg function call → logical scalar 1; `true(M, N)` is a
    // proper function call → MxN logical array. See BUGS.md #30.
    //
    // `classdef` is reserved in MATLAB. `properties`/`methods`/`events`/
    // `enumeration` are NOT keywords (they're introspection builtins);
    // the classdef parser recognises them contextually as section heads.
    switch (word.size()) {
    case 2:
        if (word == "if")
            return TokenType::KW_IF;
        break;
    case 3:
        if (word == "end")
            return TokenType::KW_END;
        if (word == "for")
            return TokenType::KW_FOR;
        if (word == "try")
            return TokenType::KW_TRY;
        break;
    case 4:
        if (word == "else")
            return TokenType::KW_ELSE;
        if (word == "case")
            return TokenType::KW_CASE;
        break;
    case 5:
        if (word == "while")
            return TokenType::KW_WHILE;
        if (word == "break")
            return TokenType::KW_BREAK;
        if (word == "catch")
            return TokenType::KW_CATCH;
        break;
    case 6:
        if (word == "elseif")
            return TokenType::KW_ELSEIF;
        if (word == "parfor")
            return TokenType::KW_FOR;
        if (word == "return")
            return TokenType::KW_RETURN;
        if (word == "switch")
            return TokenType::KW_SWITCH;
        if (word == "global")
            return TokenType::KW_GLOBAL;
        break;
    case 8:
        if (word == "continue")
            return TokenType::KW_CONTINUE;
        if (word == "function")
            return TokenType::KW_FUNCTION;
        if (word == "classdef")
            return TokenType::KW_CLASSDEF;
        break;
    case 9:
        if (word == "otherwise")
            return TokenType::KW_OTHERWISE;
        break;
    case 10:
        if (word == "persistent")
            return TokenType::KW_PERSISTENT;
        break;
    default:
        break;
    }
    return TokenType::IDENTIFIER;
}

} // namespace

// ─── safe ctype wrappers ────────────────────────────────────────────────

bool Lexer::isDigit(char c)
{
    return keel::is_ascii_digit(c);
}

bool Lexer::isAlpha(char c)
{
    return keel::is_ascii_alpha(c);
}

bool Lexer::isAlnum(char c)
{
    return keel::is_ascii_alnum(c);
}

bool Lexer::isXDigit(char c)
{
    return keel::is_ascii_hex_digit(c);
}

// ─── error helpers ──────────────────────────────────────────────────────

void Lexer::error(keel::StringRef msg)
{
    error(msg, line_, col_);
}

void Lexer::error(keel::StringRef msg, int line, int col)
{
    keel::String err = keel::format("{} at line {} col {}", msg, line, col);
    throw std::runtime_error(err.c_str());
}

// ─── constructor / basic methods ────────────────────────────────────────

Lexer::Lexer(keel::StringRef source)
{
    if (source.size() >= 3
        && keel::as<unsigned char>(source[0]) == 0xEF
        && keel::as<unsigned char>(source[1]) == 0xBB
        && keel::as<unsigned char>(source[2]) == 0xBF) {
        source = source.substr(3);
    }
    if (source.find('\r') != keel::kInvalidIndex) {
        src_.reserve(source.size());
        for (keel::usize i = 0; i < source.size(); ++i) {
            if (source[i] == '\r') {
                src_.push_back('\n');
                if (i + 1 < source.size() && source[i + 1] == '\n')
                    ++i;
            } else {
                src_.push_back(source[i]);
            }
        }
    } else {
        src_.assign(source);
    }
}

char Lexer::peek() const
{
    return pos_ < src_.size() ? src_[pos_] : '\0';
}

char Lexer::peek(int offset) const
{
    long long p = keel::as<long long>(pos_) + offset;
    if (p < 0 || keel::as<keel::usize>(p) >= src_.size())
        return '\0';
    return src_[keel::as<keel::usize>(p)];
}

char Lexer::advance()
{
    if (pos_ >= src_.size())
        return '\0';
    char c = src_[pos_++];
    if (c == '\n') {
        line_++;
        col_ = 1;
    } else {
        col_++;
    }
    return c;
}

void Lexer::addToken(TokenType type, keel::StringRef val, int line, int col)
{
    tokens_.push_back(Token{type, keel::String::from(val), line, col, line_, col_});
}

// ─── bracket tracking ──────────────────────────────────────────────────

int Lexer::bracketDepth() const
{
    return keel::as<int>(bracketStack_.size());
}

bool Lexer::inMatrixContext() const
{
    return !bracketStack_.empty() && (bracketStack_.back() == '[' || bracketStack_.back() == '{');
}

char Lexer::closingFor(char open)
{
    switch (open) {
    case '(':
        return ')';
    case '[':
        return ']';
    case '{':
        return '}';
    default:
        return '?';
    }
}

void Lexer::pushBracket(char open)
{
    bracketStack_.push_back(open);
}

void Lexer::popBracket(char expected)
{
    if (bracketStack_.empty()) {
        keel::String msg = keel::format("Unexpected closing '{}' without matching open", expected);
        error(msg);
    }
    char open = bracketStack_.back();
    char expectedClosing = closingFor(open);
    if (expected != expectedClosing) {
        keel::String msg = keel::format("Mismatched bracket: expected '{}' but found '{}'",
                                        expectedClosing, expected);
        error(msg);
    }
    bracketStack_.pop_back();
}

// ─── line-start check ───────────────────────────────────────────────────

bool Lexer::isAtLineStart() const
{
    return isAtLineStart(pos_);
}

bool Lexer::isAtLineStart(keel::usize position) const
{
    if (position == 0)
        return true;

    for (keel::usize i = position; i > 0; i--) {
        char ch = src_[i - 1];
        if (ch == '\n')
            return true;
        if (ch != ' ' && ch != '\t')
            return false;
    }
    return true;
}

// ─── context checks ────────────────────────────────────────────────────

bool Lexer::isValueToken(TokenType t) const
{
    switch (t) {
    case TokenType::NUMBER:
    case TokenType::IMAG_NUMBER:
    case TokenType::STRING:
    case TokenType::DQSTRING:
    case TokenType::IDENTIFIER:
    case TokenType::RPAREN:
    case TokenType::RBRACKET:
    case TokenType::RBRACE:
    case TokenType::APOSTROPHE:
    case TokenType::DOT_APOSTROPHE:
    case TokenType::KW_END:
    case TokenType::KW_TRUE:
    case TokenType::KW_FALSE:
        return true;
    default:
        return false;
    }
}

bool Lexer::isTransposeContext() const
{
    if (tokens_.empty())
        return false;
    const Token &prev = tokens_.back();
    if (!isValueToken(prev.type))
        return false;
    // MATLAB: the transpose operators are GLUED to their operand - x' is
    // transpose, while "x '" (spaced) is a parse error in MATLAB and a
    // quote after whitespace starts a STRING (command syntax: disp 'abc').
    // Whitespace = a gap between the previous token's end column and this
    // quote's column (verified on R2025b: y = x ' -> Invalid expression).
    return line_ == prev.endLine && col_ == prev.endCol;
}

// ─── implicit comma inside [] and {} ────────────────────────────────────

void Lexer::insertImplicitComma()
{
    if (!inMatrixContext() || tokens_.empty())
        return;

    int prevIdx = keel::as<int>(tokens_.size()) - 1;
    while (prevIdx >= 0 && tokens_[prevIdx].type == TokenType::COMMENT)
        --prevIdx;
    if (prevIdx < 0)
        return;

    auto prev = tokens_[prevIdx].type;
    if (!isValueToken(prev))
        return;

    if (prev == TokenType::RPAREN) {
        int depth = 0;
        for (int i = prevIdx; i >= 0; --i) {
            if (tokens_[i].type == TokenType::RPAREN)
                depth++;
            else if (tokens_[i].type == TokenType::LPAREN)
                depth--;
            if (depth == 0) {
                if (i > 0 && tokens_[i - 1].type == TokenType::AT)
                    return;
                break;
            }
        }
    }

    keel::usize scanPos = pos_;

    while (scanPos < src_.size()) {
        if (src_[scanPos] == ' ' || src_[scanPos] == '\t') {
            scanPos++;
        } else if (src_[scanPos] == '.' && scanPos + 2 < src_.size()
                   && src_[scanPos + 1] == '.' && src_[scanPos + 2] == '.') {
            while (scanPos < src_.size() && src_[scanPos] != '\n')
                scanPos++;
            if (scanPos < src_.size())
                scanPos++;
        } else {
            break;
        }
    }

    if (scanPos >= src_.size())
        return;

    char next = src_[scanPos];

    if (next == '+' || next == '-') {
        keel::usize afterOp = scanPos + 1;
        bool spaceAfterOp = (afterOp < src_.size()
                             && (src_[afterOp] == ' ' || src_[afterOp] == '\t'));
        if (!spaceAfterOp) {
            addToken(TokenType::COMMA, ",", line_, col_);
        }
        return;
    }

    if (next == '.') {
        char afterDot = (scanPos + 1 < src_.size()) ? src_[scanPos + 1] : '\0';
        if (!isDigit(afterDot))
            return;
    }

    bool nextIsValue = isDigit(next) || isAlpha(next) || next == '(' || next == '[' || next == '{'
                       || next == '\'' || next == '"' || next == '~' || next == '@' || next == '.';

    if (nextIsValue) {
        addToken(TokenType::COMMA, ",", line_, col_);
    }
}

// ─── block comment %{ ... %} ───────────────────────────────────────────

void Lexer::skipBlockComment()
{
    // Capture start position + raw text so we can emit a single
    // COMMENT token spanning the whole block. The parser silently
    // skips COMMENT tokens; downstream tools (script-graph viewer,
    // formatters, doc extractors) recover the original text.
    int startLine = line_, startCol = col_;
    keel::usize startPos = pos_;

    advance(); // '%'
    advance(); // '{'

    while (pos_ < src_.size() && peek() != '\n')
        advance();
    if (pos_ < src_.size())
        advance(); // '\n'

    int depth = 1;
    while (pos_ < src_.size() && depth > 0) {
        if (peek() == '%' && peek(1) == '}' && isAtLineStart(pos_)) {
            depth--;
            advance(); // '%'
            advance(); // '}'
        } else if (peek() == '%' && peek(1) == '{' && isAtLineStart(pos_)) {
            depth++;
            advance(); // '%'
            advance(); // '{'
            while (pos_ < src_.size() && peek() != '\n')
                advance();
            if (pos_ < src_.size())
                advance(); // '\n'
        } else {
            advance();
        }
    }

    if (depth > 0)
        error("Unterminated block comment");

    while (pos_ < src_.size() && peek() != '\n')
        advance();

    addToken(TokenType::COMMENT,
             src_.substr(startPos, pos_ - startPos),
             startLine, startCol);
}

// ─── skip spaces, comments, line continuation ──────────────────────────

void Lexer::skipSpacesAndComments()
{
    while (pos_ < src_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\x1a') {
            if (inMatrixContext() && (c == ' ' || c == '\t')) {
                while (pos_ < src_.size() && (peek() == ' ' || peek() == '\t'))
                    advance();
                insertImplicitComma();
                continue;
            }
            advance();
        } else if (c == '%') {
            if (peek(1) == '{' && isAtLineStart()) {
                skipBlockComment();
            } else {
                // Line comment `% ...` — emit as a COMMENT token so
                // downstream consumers (script-graph viewer, future
                // formatters) can position around it. Parser ignores.
                int startLine = line_, startCol = col_;
                keel::usize startPos = pos_;
                while (pos_ < src_.size() && peek() != '\n')
                    advance();
                addToken(TokenType::COMMENT,
                         src_.substr(startPos, pos_ - startPos),
                         startLine, startCol);
            }
        } else if (c == '.' && peek(1) == '.' && peek(2) == '.') {
            // Line continuation — stays silent (syntactic, not lexical
            // info; consumers that care can recover it from the source
            // text directly).
            while (pos_ < src_.size() && peek() != '\n')
                advance();
            if (pos_ < src_.size())
                advance();
            if (inMatrixContext()) {
                while (pos_ < src_.size() && (peek() == ' ' || peek() == '\t'))
                    advance();
                insertImplicitComma();
            }
        } else {
            break;
        }
    }
}

// ─── underscore validation in numbers ───────────────────────────────────

void Lexer::validateUnderscores(keel::usize start, keel::usize end, int startLine, int startCol)
{
    if (start >= end)
        return;

    if (src_[start] == '_')
        error("Number literal cannot start digit group with underscore", startLine, startCol);

    if (src_[end - 1] == '_')
        error("Number literal cannot end with underscore", startLine, startCol);

    for (keel::usize i = start; i + 1 < end; i++) {
        if (src_[i] == '_' && src_[i + 1] == '_')
            error("Number literal cannot have consecutive underscores", startLine, startCol);
    }
}

// ─── read number ────────────────────────────────────────────────────────

void Lexer::readNumber()
{
    int startLine = line_;
    int startCol = col_;
    keel::usize start = pos_;

    // ── Hex: 0x... ──
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance();
        advance();
        if (pos_ >= src_.size() || !isXDigit(peek()))
            error("Invalid hex literal", startLine, startCol);
        keel::usize digitStart = pos_;
        while (pos_ < src_.size() && (isXDigit(peek()) || peek() == '_'))
            advance();
        validateUnderscores(digitStart, pos_, startLine, startCol);
        if (pos_ < src_.size() && (peek() == 'i' || peek() == 'j')) {
            char afterSuffix = peek(1);
            if (!isAlnum(afterSuffix) && afterSuffix != '_') {
                advance();
                addToken(TokenType::IMAG_NUMBER,
                         src_.substr(start, pos_ - start),
                         startLine,
                         startCol);
                return;
            }
        }
        addToken(TokenType::NUMBER, src_.substr(start, pos_ - start), startLine, startCol);
        return;
    }

    // ── Binary: 0b... ──
    if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        advance();
        advance();
        if (pos_ >= src_.size() || (peek() != '0' && peek() != '1'))
            error("Invalid binary literal", startLine, startCol);
        keel::usize digitStart = pos_;
        while (pos_ < src_.size() && (peek() == '0' || peek() == '1' || peek() == '_'))
            advance();
        validateUnderscores(digitStart, pos_, startLine, startCol);
        if (pos_ < src_.size() && (peek() == 'i' || peek() == 'j')) {
            char afterSuffix = peek(1);
            if (!isAlnum(afterSuffix) && afterSuffix != '_') {
                advance();
                addToken(TokenType::IMAG_NUMBER,
                         src_.substr(start, pos_ - start),
                         startLine,
                         startCol);
                return;
            }
        }
        addToken(TokenType::NUMBER, src_.substr(start, pos_ - start), startLine, startCol);
        return;
    }

    // ── Octal: 0o... ──
    if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
        advance();
        advance();
        if (pos_ >= src_.size() || peek() < '0' || peek() > '7')
            error("Invalid octal literal", startLine, startCol);
        keel::usize digitStart = pos_;
        while (pos_ < src_.size() && ((peek() >= '0' && peek() <= '7') || peek() == '_'))
            advance();
        validateUnderscores(digitStart, pos_, startLine, startCol);
        if (pos_ < src_.size() && (peek() == 'i' || peek() == 'j')) {
            char afterSuffix = peek(1);
            if (!isAlnum(afterSuffix) && afterSuffix != '_') {
                advance();
                addToken(TokenType::IMAG_NUMBER,
                         src_.substr(start, pos_ - start),
                         startLine,
                         startCol);
                return;
            }
        }
        addToken(TokenType::NUMBER, src_.substr(start, pos_ - start), startLine, startCol);
        return;
    }

    // ── Decimal / float ──
    bool hasDigits = false;

    keel::usize intStart = pos_;
    while (pos_ < src_.size() && (isDigit(peek()) || (peek() == '_' && !isAlpha(peek(1))))) {
        if (peek() != '_')
            hasDigits = true;
        advance();
    }
    if (pos_ > intStart)
        validateUnderscores(intStart, pos_, startLine, startCol);

    // fractional part
    if (pos_ < src_.size() && peek() == '.') {
        char next = peek(1);

        bool isDotOperator = (next == '*' || next == '/' || next == '^' || next == '\''
                              || next == '\\' || next == '.');

        bool isExponent = (next == 'e' || next == 'E')
                          && (isDigit(peek(2))
                              || ((peek(2) == '+' || peek(2) == '-') && isDigit(peek(3))));
        bool isFieldAccess = isAlpha(next) && !isExponent;

        if (!isDotOperator && !isFieldAccess) {
            advance(); // '.'
            keel::usize fracStart = pos_;
            while (pos_ < src_.size() && (isDigit(peek()) || (peek() == '_' && !isAlpha(peek(1))))) {
                if (peek() != '_')
                    hasDigits = true;
                advance();
            }
            if (pos_ > fracStart)
                validateUnderscores(fracStart, pos_, startLine, startCol);
        }
    }

    if (!hasDigits)
        error("Invalid number literal", startLine, startCol);

    // exponent
    if (pos_ < src_.size() && (peek() == 'e' || peek() == 'E')) {
        advance();
        if (pos_ < src_.size() && (peek() == '+' || peek() == '-'))
            advance();
        if (pos_ >= src_.size() || !isDigit(peek()))
            error("Invalid number exponent", startLine, startCol);
        keel::usize expStart = pos_;
        while (pos_ < src_.size() && (isDigit(peek()) || (peek() == '_' && !isAlpha(peek(1)))))
            advance();
        if (pos_ > expStart)
            validateUnderscores(expStart, pos_, startLine, startCol);
    }

    // imaginary suffix
    if (pos_ < src_.size() && (peek() == 'i' || peek() == 'j')) {
        char afterSuffix = peek(1);
        if (!isAlnum(afterSuffix) && afterSuffix != '_') {
            advance();
            addToken(TokenType::IMAG_NUMBER, src_.substr(start, pos_ - start), startLine, startCol);
            return;
        }
    }

    addToken(TokenType::NUMBER, src_.substr(start, pos_ - start), startLine, startCol);
}

// ─── read single-quoted string ──────────────────────────────────────────

void Lexer::readString(int startLine, int startCol)
{
    advance();
    keel::String s;
    while (pos_ < src_.size()) {
        if (peek() == '\'') {
            if (peek(1) == '\'') {
                s.push_back('\'');
                advance();
                advance();
            } else {
                advance();
                addToken(TokenType::STRING, s, startLine, startCol);
                return;
            }
        } else if (peek() == '\n') {
            error("Unterminated string literal", startLine, startCol);
        } else {
            s.push_back(advance());
        }
    }
    error("Unterminated string literal", startLine, startCol);
}

// ─── read double-quoted string (with escape sequences) ──────────────────

void Lexer::readDoubleQuotedString(int startLine, int startCol)
{
    advance();
    keel::String s;
    while (pos_ < src_.size()) {
        if (peek() == '"') {
            if (peek(1) == '"') {
                s.push_back('"');
                advance();
                advance();
            } else {
                advance();
                addToken(TokenType::DQSTRING, s, startLine, startCol);
                return;
            }
        } else if (peek() == '\\') {
            advance();
            if (pos_ >= src_.size())
                error("Unterminated string literal", startLine, startCol);
            char esc = advance();
            switch (esc) {
            case 'n':
                s.push_back('\n');
                break;
            case 't':
                s.push_back('\t');
                break;
            case 'r':
                s.push_back('\r');
                break;
            case '\\':
                s.push_back('\\');
                break;
            case '"':
                s.push_back('"');
                break;
            case '0':
                s.push_back('\0');
                break;
            case 'a':
                s.push_back('\a');
                break;
            case 'b':
                s.push_back('\b');
                break;
            case 'f':
                s.push_back('\f');
                break;
            case 'v':
                s.push_back('\v');
                break;
            default:
                s.push_back('\\');
                s.push_back(esc);
                break;
            }
        } else if (peek() == '\n') {
            error("Unterminated string literal", startLine, startCol);
        } else {
            s.push_back(advance());
        }
    }
    error("Unterminated string literal", startLine, startCol);
}

// ─── read identifier / keyword ──────────────────────────────────────────

void Lexer::readIdentifier()
{
    int startLine = line_;
    int startCol = col_;
    keel::usize start = pos_;

    while (pos_ < src_.size() && (isAlnum(peek()) || peek() == '_'))
        advance();

    keel::StringRef word = src_.substr(start, pos_ - start);
    addToken(lookupKeyword(word), word, startLine, startCol);
}

// ─── read operators / punctuation ───────────────────────────────────────

bool Lexer::readOperator()
{
    char c = peek();
    char c2 = peek(1);

    int startLine = line_;
    int startCol = col_;

    auto twoChar = [&](TokenType t, const char *s) {
        advance();
        advance();
        addToken(t, s, startLine, startCol);
        return true;
    };
    auto oneChar = [&](TokenType t, const char *s) {
        advance();
        addToken(t, s, startLine, startCol);
        return true;
    };

    switch (c) {
    case '@':
        return oneChar(TokenType::AT, "@");
    case '.':
        if (c2 == '*')
            return twoChar(TokenType::DOT_STAR, ".*");
        if (c2 == '/')
            return twoChar(TokenType::DOT_SLASH, "./");
        if (c2 == '\\')
            return twoChar(TokenType::DOT_BACKSLASH, ".\\");
        if (c2 == '^')
            return twoChar(TokenType::DOT_CARET, ".^");
        if (c2 == '\'')
            return twoChar(TokenType::DOT_APOSTROPHE, ".'");
        return oneChar(TokenType::DOT, ".");
    case '+':
        return oneChar(TokenType::PLUS, "+");
    case '-':
        return oneChar(TokenType::MINUS, "-");
    case '*':
        return oneChar(TokenType::STAR, "*");
    case '/':
        return oneChar(TokenType::SLASH, "/");
    case '\\':
        return oneChar(TokenType::BACKSLASH, "\\");
    case '^':
        return oneChar(TokenType::CARET, "^");
    case '=':
        if (c2 == '=')
            return twoChar(TokenType::EQ, "==");
        return oneChar(TokenType::ASSIGN, "=");
    case '~':
        if (c2 == '=')
            return twoChar(TokenType::NEQ, "~=");
        return oneChar(TokenType::TILDE, "~");
    case '<':
        if (c2 == '=')
            return twoChar(TokenType::LEQ, "<=");
        return oneChar(TokenType::LT, "<");
    case '>':
        if (c2 == '=')
            return twoChar(TokenType::GEQ, ">=");
        return oneChar(TokenType::GT, ">");
    case '&':
        if (c2 == '&')
            return twoChar(TokenType::AND_SHORT, "&&");
        return oneChar(TokenType::AND, "&");
    case '|':
        if (c2 == '|')
            return twoChar(TokenType::OR_SHORT, "||");
        return oneChar(TokenType::OR, "|");
    case '(':
        pushBracket('(');
        return oneChar(TokenType::LPAREN, "(");
    case ')':
        popBracket(')');
        return oneChar(TokenType::RPAREN, ")");
    case '[':
        pushBracket('[');
        return oneChar(TokenType::LBRACKET, "[");
    case ']':
        popBracket(']');
        return oneChar(TokenType::RBRACKET, "]");
    case '{':
        pushBracket('{');
        return oneChar(TokenType::LBRACE, "{");
    case '}':
        popBracket('}');
        return oneChar(TokenType::RBRACE, "}");
    case ',':
        return oneChar(TokenType::COMMA, ",");
    case ';':
        return oneChar(TokenType::SEMICOLON, ";");
    case ':':
        return oneChar(TokenType::COLON, ":");
    default:
        return false;
    }
}

// ─── main tokenization loop ────────────────────────────────────────────

keel::Array<Token> Lexer::tokenize()
{
    tokens_.clear();
    bracketStack_.clear();
    pos_ = 0;
    line_ = 1;
    col_ = 1;

    while (pos_ < src_.size()) {
        skipSpacesAndComments();
        if (pos_ >= src_.size())
            break;

        char c = peek();

        if (c == '\n') {
            int nl = line_;
            int nc = col_;

            if (bracketDepth() > 0) {
                if (inMatrixContext()) {
                    int prevIdx = keel::as<int>(tokens_.size()) - 1;
                    while (prevIdx >= 0 && tokens_[prevIdx].type == TokenType::COMMENT)
                        --prevIdx;
                    if (prevIdx >= 0 && isValueToken(tokens_[prevIdx].type)) {
                        addToken(TokenType::SEMICOLON, ";", nl, nc);
                    }
                }
            } else {
                addToken(TokenType::NEWLINE, "\\n", nl, nc);
            }
            advance();
            continue;
        }

        if (isDigit(c)
            || (c == '.' && pos_ + 1 < src_.size() && isDigit(src_[pos_ + 1])
                && !isTransposeContext())) {
            readNumber();
            continue;
        }

        if (c == '\'') {
            if (isTransposeContext()) {
                int al = line_, ac = col_;
                advance();
                addToken(TokenType::APOSTROPHE, "'", al, ac);
                continue;
            }
            readString(line_, col_);
            continue;
        }

        if (c == '"') {
            readDoubleQuotedString(line_, col_);
            continue;
        }

        if (isAlpha(c) || c == '_') {
            readIdentifier();
            continue;
        }

        if (readOperator())
            continue;

        keel::String msg = keel::format("Unexpected character '{}'", c);
        error(msg);
    }

    if (!bracketStack_.empty()) {
        keel::String msg = keel::format("Unclosed bracket '{}'", bracketStack_.back());
        error(msg);
    }

    addToken(TokenType::END_OF_INPUT, "", line_, col_);
    return keel::move(tokens_);
}

} // namespace numkit
