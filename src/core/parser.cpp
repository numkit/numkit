#include <keel/core.h>
#include <keel/format.h>
#include <keel/hash_map.h>
#include <keel/utf8.h>
#include <numkit/core/parser.hpp>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>

namespace numkit {

// ============================================================
// Конструктор
// ============================================================

Parser::Parser(keel::ArrayRef<Token> tokens)
{
    tokens_.reserve(tokens.size() + 1);
    for (const auto &tok : tokens)
        tokens_.push_back(tok.clone());
    if (tokens_.empty() || tokens_.back().type != TokenType::END_OF_INPUT) {
        Token eof;
        eof.type = TokenType::END_OF_INPUT;
        eof.value.clear();
        eof.line = tokens_.empty() ? 1 : tokens_.back().line;
        eof.col = 0;
        tokens_.push_back(keel::move(eof));
    }
    // Skip past any leading COMMENT tokens so current() never returns
    // one. The advance() helper maintains this invariant from here on.
    skipPastComments();
}

Parser::Parser(keel::Array<Token> &&tokens)
    : tokens_(keel::move(tokens))
{
    if (tokens_.empty() || tokens_.back().type != TokenType::END_OF_INPUT) {
        Token eof;
        eof.type = TokenType::END_OF_INPUT;
        eof.value.clear();
        eof.line = tokens_.empty() ? 1 : tokens_.back().line;
        eof.col = 0;
        tokens_.push_back(keel::move(eof));
    }
    // Skip past any leading COMMENT tokens so current() never returns
    // one. The advance() helper maintains this invariant from here on.
    skipPastComments();
}

// ============================================================
// Навигация по токенам
// ============================================================

Parser::SourceLoc Parser::loc() const
{
    return {current().line, current().col};
}

// Single chokepoint for forward motion through the token stream.
// After advancing pos_ by one, skip past any COMMENT tokens — they
// are pure lexical metadata for downstream tools (script-graph viewer,
// formatters), not part of script grammar.
void Parser::advance()
{
    if (pos_ < tokens_.size()) ++pos_;
    skipPastComments();
}

void Parser::skipPastComments()
{
    while (pos_ < tokens_.size() && tokens_[pos_].type == TokenType::COMMENT)
        ++pos_;
}

const Token &Parser::current() const
{
    if (pos_ >= tokens_.size())
        return tokens_.back();
    return tokens_[pos_];
}

const Token &Parser::peekToken(int off) const
{
    // COMMENT-aware lookahead: COMMENT tokens are transparent at the
    // parser level so peekToken(1) returns the next *real* token,
    // not the comment between current and next. Negative offsets do
    // the mirrored skip-backwards.
    long long p = keel::as<long long>(pos_);
    if (off == 0) {
        if (p < 0 || p >= keel::as<long long>(tokens_.size())) return tokens_.back();
        return tokens_[keel::as<keel::usize>(p)];
    }
    const int dir   = (off > 0) ? 1 : -1;
    const int steps = (off > 0) ? off : -off;
    for (int s = 0; s < steps; ++s) {
        do {
            p += dir;
            if (p < 0 || p >= keel::as<long long>(tokens_.size()))
                return tokens_.back();
        } while (tokens_[keel::as<keel::usize>(p)].type == TokenType::COMMENT);
    }
    return tokens_[keel::as<keel::usize>(p)];
}

bool Parser::isAtEnd() const
{
    return current().type == TokenType::END_OF_INPUT;
}

bool Parser::check(TokenType t) const
{
    return current().type == t;
}

bool Parser::match(TokenType t)
{
    if (check(t)) {
        advance();
        return true;
    }
    return false;
}

Token Parser::consume(TokenType t, keel::StringRef msg)
{
    if (check(t)) {
        Token result = tokens_[pos_].clone();
        advance();
        return result;
    }
    if (msg.empty()) {
        keel::String err = keel::format(
            "Parse error at line {} col {}: expected token type {}, got '{}'",
            current().line, current().col, keel::as<int>(t), current().value);
        throw std::runtime_error(err.c_str());
    }
    keel::String err = keel::format(
        "Parse error at line {} col {}: expected {}, got '{}'",
        current().line, current().col, msg, current().value);
    throw std::runtime_error(err.c_str());
}

void Parser::skipNewlines()
{
    while (!isAtEnd()
           && (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON) || check(TokenType::COMMA)))
        advance();
}

void Parser::skipTerminators()
{
    while (!isAtEnd()
           && (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON) || check(TokenType::COMMA)))
        advance();
}

bool Parser::consumeStmtTerminator(ASTNode &node)
{
    if (check(TokenType::SEMICOLON) || check(TokenType::COMMA)) {
        // Record the terminator's position into the node BEFORE
        // advancing past it. endCol = col + 1 because col is the
        // 1-indexed position OF the terminator char; we want the
        // column one past it (so a slice [startCol, endCol) includes
        // the terminator).
        const Token &tok = current();
        node.endLine = tok.line;
        node.endCol  = tok.col + 1;
        bool isSemi  = tok.type == TokenType::SEMICOLON;
        advance();
        return isSemi;
    }
    return false;
}

bool Parser::isTerminator(std::initializer_list<TokenType> terminators) const
{
    auto cur = current().type;
    for (auto t : terminators) {
        if (cur == t)
            return true;
    }
    return false;
}

// ============================================================
// FIX #2: Полноценный парсинг числовых литералов
// Обрабатывает hex (0x), binary (0b), octal (0o), подчёркивания
// ============================================================

keel::f64 Parser::parseDouble(keel::StringRef text, int line, int col)
{
    // Убираем подчёркивания и суффикс i/j (для мнимых)
    keel::String clean;
    clean.reserve(text.size());
    for (char c : text) {
        if (c != '_')
            clean.push_back(c);
    }

    // Убрать суффикс i/j (IMAG_LITERAL)
    if (!clean.empty() && (clean.back() == 'i' || clean.back() == 'j'))
        clean.pop_back();

    auto throwInvalid = [&]() -> void {
        keel::String err = keel::format(
            "Invalid number literal '{}' at line {} col {}", text, line, col);
        throw std::runtime_error(err.c_str());
    };

    if (clean.empty()) {
        throwInvalid();
    }

    // Binary: 0b...
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B')) {
        const char *start = clean.c_str() + 2;
        if (*start == '-' || *start == '+')
            throwInvalid();
        char *end = nullptr;
        errno = 0;
        unsigned long long val = std::strtoull(start, &end, 2);
        if (end == start || *end != '\0' || errno == ERANGE)
            throwInvalid();
        return keel::as<keel::f64>(val);
    }

    // Octal: 0o...
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'o' || clean[1] == 'O')) {
        const char *start = clean.c_str() + 2;
        if (*start == '-' || *start == '+')
            throwInvalid();
        char *end = nullptr;
        errno = 0;
        unsigned long long val = std::strtoull(start, &end, 8);
        if (end == start || *end != '\0' || errno == ERANGE)
            throwInvalid();
        return keel::as<keel::f64>(val);
    }

    // Hex и decimal — std::strtod обрабатывает оба
    const char *start = clean.c_str();
    char *end = nullptr;
    errno = 0;
    double val = std::strtod(start, &end);
    if (end == start || *end != '\0' || errno == ERANGE)
        throwInvalid();
    return val;
}

// ============================================================
// Точка входа
// ============================================================

ASTNodePtr Parser::parse()
{
    auto [ln, cl] = loc();
    auto block = makeNode(NodeType::BLOCK, ln, cl);
    skipNewlines();
    while (!isAtEnd()) {
        auto stmt = parseStatement();
        if (stmt)
            block->children.push_back(keel::move(stmt));
        skipNewlines();
    }

    // Check for duplicate function definitions
    keel::HashMap<keel::String, int> funcNames;
    for (auto &child : block->children) {
        if (child && child->type == NodeType::FUNCTION_DEF) {
            if (const int *firstLine = funcNames.get(child->strValue)) {
                keel::String err = keel::format(
                    "Duplicate function '{}' (first defined at line {}, redefined at line {})",
                    child->strValue, *firstLine, child->line);
                throw std::runtime_error(err.c_str());
            }
            funcNames.put(child->strValue.clone(), child->line);
        }
    }

    return block;
}

// ============================================================
// Statements
// ============================================================

ASTNodePtr Parser::parseStatement()
{
    NestingGuard guard(*this);
    const bool firstStatement = (stmtOrdinal_ == 0);
    ++stmtOrdinal_;
    switch (current().type) {
    case TokenType::KW_FUNCTION:
        return parseFunctionDef();
    case TokenType::KW_CLASSDEF: {
        // MATLAB: classdef is only legal as the first construct of a
        // file — a script with statements before it (or any classdef
        // nested in a block) is an "Illegal use of reserved keyword"
        // parse error, not a silently-ignored block
        // (bugs/opened/lang/classdef-mid-script-silent.md).
        if (!firstStatement) {
            auto [ln, cl] = loc();
            keel::String err = keel::format(
                "Illegal use of reserved keyword \"classdef\" at line {} col {}", ln, cl);
            throw std::runtime_error(err.c_str());
        }
        return parseClassDef();
    }
    case TokenType::KW_IF:
        return parseIf();
    case TokenType::KW_FOR:
        return parseFor();
    case TokenType::KW_WHILE:
        return parseWhile();
    case TokenType::KW_SWITCH:
        return parseSwitch();
    case TokenType::KW_TRY:
        return parseTryCatch();
    case TokenType::KW_GLOBAL:
    case TokenType::KW_PERSISTENT:
        return parseGlobalPersistent();
    case TokenType::KW_BREAK: {
        auto [ln, cl] = loc();
        auto node = makeNode(NodeType::BREAK_STMT, ln, cl);
        advance();
        consumeStmtTerminator(*node);  // record `;` / `,` pos for graph view
        skipTerminators();
        return node;
    }
    case TokenType::KW_CONTINUE: {
        auto [ln, cl] = loc();
        auto node = makeNode(NodeType::CONTINUE_STMT, ln, cl);
        advance();
        consumeStmtTerminator(*node);
        skipTerminators();
        return node;
    }
    case TokenType::KW_RETURN: {
        // MATLAB return не принимает выражения — просто возврат из функции
        auto [ln, cl] = loc();
        auto node = makeNode(NodeType::RETURN_STMT, ln, cl);
        advance();
        consumeStmtTerminator(*node);
        skipTerminators();
        return node;
    }
    default:
        return parseExpressionStatement();
    }
}

// ============================================================
// Command-style calls
//
// MATLAB позволяет вызывать функции без скобок:
//   clear all          →  clear('all')
//   grid on            →  grid('on')
//   format long        →  format('long')
//   cd /path/to/dir    →  cd('/path/to/dir')
//   load data.mat x y  →  load('data.mat','x','y')
//   disp hello         →  disp('hello')
//
// Правило: IDENTIFIER на позиции statement, за которым на той же
// строке следует токен-аргумент (IDENTIFIER, STRING, NUMBER, …)
// без оператора/скобки между ними.
//
// Представление в AST: COMMAND_CALL
//   strValue   = имя функции
//   children[] = аргументы (STRING_LITERAL)
// ============================================================

bool Parser::isCommandStyleCall() const
{
    if (current().type != TokenType::IDENTIFIER)
        return false;

    const Token &next = peekToken(1);
    if (next.type == TokenType::STAR && next.line == current().line
        && (current().value == "clear" || current().value == "clearvars"
            || current().value == "who" || current().value == "whos"))
        return true;

    // Command args may START with a glued '-' or '/': `which -all sin`,
    // `cd /path`, `web -broswer http://…` (the fieldtest corpus idiom).
    // Distinguished from binary expressions (`x - 1`, `a / b`, `C1-sqrt(C2)`,
    // `N/2`) by requiring whitespace between the command head and the
    // operator AND the operand being glued to the operator.
    if (next.type == TokenType::MINUS || next.type == TokenType::SLASH) {
        const int headEnd = current().endCol > 0 ? current().endCol
                                                 : current().col + keel::as<int>(current().value.size());
        const Token &operand = peekToken(2);
        const int nextEnd = next.endCol > 0 ? next.endCol
                                            : next.col + keel::as<int>(next.value.size());
        return next.line == current().line
            && next.col > headEnd
            && operand.line == next.line
            && operand.col == nextEnd
            && (operand.type == TokenType::IDENTIFIER
                || operand.type == TokenType::NUMBER
                || operand.type == TokenType::STRING
                || operand.type == TokenType::DQSTRING);
    }

    // Relative path in command form: e.g. `load ../sampleEEGdata` or `load ./foo`
    if (next.type == TokenType::DOT || next.type == TokenType::DOT_SLASH
        || next.type == TokenType::DOT_BACKSLASH) {
        const int headEnd = current().endCol > 0 ? current().endCol
                                                 : current().col + keel::as<int>(current().value.size());
        const Token &operand = peekToken(2);
        const int nextEnd = next.endCol > 0 ? next.endCol
                                            : next.col + keel::as<int>(next.value.size());
        return next.line == current().line
            && next.col > headEnd
            && operand.line == next.line
            && operand.col == nextEnd
            && (operand.type == TokenType::DOT
                || operand.type == TokenType::DOT_SLASH
                || operand.type == TokenType::DOT_BACKSLASH
                || operand.type == TokenType::SLASH
                || operand.type == TokenType::BACKSLASH
                || operand.type == TokenType::IDENTIFIER);
    }

    // Если следующий токен — оператор, скобка, присваивание, терминатор
    // или конец файла — это НЕ command-style.
    switch (next.type) {
    // Присваивание и скобочный доступ
    case TokenType::ASSIGN:
    case TokenType::LPAREN:
    case TokenType::LBRACE:
    // Доступ к полю / транспонирование
    case TokenType::DOT:
    case TokenType::APOSTROPHE:
    case TokenType::DOT_APOSTROPHE:
    // Арифметические и логические операторы
    case TokenType::PLUS:
    case TokenType::STAR:
    case TokenType::BACKSLASH:
    case TokenType::CARET:
    case TokenType::DOT_STAR:
    case TokenType::DOT_SLASH:
    case TokenType::DOT_BACKSLASH:
    case TokenType::DOT_CARET:
    // Сравнения
    case TokenType::EQ:
    case TokenType::NEQ:
    case TokenType::LT:
    case TokenType::GT:
    case TokenType::LEQ:
    case TokenType::GEQ:
    // Логические
    case TokenType::AND:
    case TokenType::OR:
    case TokenType::AND_SHORT:
    case TokenType::OR_SHORT:
    // Colon (x:y — range)
    case TokenType::COLON:
    // Терминаторы
    case TokenType::SEMICOLON:
    case TokenType::NEWLINE:
    case TokenType::COMMA:
    case TokenType::RPAREN:
    case TokenType::RBRACKET:
    case TokenType::RBRACE:
    case TokenType::END_OF_INPUT:
        return false;
    default:
        break;
    }

    // Аргумент должен быть на той же строке
    if (next.line != current().line)
        return false;

    // Допустимые типы первого аргумента
    switch (next.type) {
    case TokenType::IDENTIFIER:
    case TokenType::STRING:
    case TokenType::DQSTRING:
    case TokenType::NUMBER:
    case TokenType::KW_TRUE:
    case TokenType::KW_FALSE:
    case TokenType::KW_GLOBAL:
        return true;
    case TokenType::KW_IF:
        return current().value == "dbstop" || current().value == "dbclear";
    default:
        return false;
    }
}

ASTNodePtr Parser::parseCommandStyleCall()
{
    auto [ln, cl] = loc();

    // Имя функции
    keel::String funcName = current().value.clone();
    advance();

    auto node = makeNode(NodeType::COMMAND_CALL, ln, cl);
    node->strValue = keel::move(funcName);

    // Собираем аргументы до конца statement (NEWLINE, SEMICOLON, EOF).
    // MATLAB semantics: everything after the command head up to the
    // statement terminator is split on WHITESPACE into char-literal
    // arguments — no operator interpretation of ':' '/' '.' '-' inside.
    // Tokens don't carry whitespace, but they carry (line, col, value),
    // so a gap between the previous token's end column and the next
    // token's start column reconstructs the space that splits arguments.
    int cmdLine = ln;
    bool haveArg = false;
    keel::String argStr;
    int argLine = 0, argCol = 0;
    int prevEnd = -1; // col just past the source text of the previous token

    auto tokenEndCol = [](const Token &t) {
        if (t.endCol > 0)
            return t.endCol - 1;
        // Quoted strings store the UNQUOTED value but span the quotes in
        // the source (opening quote at t.col, closing after the value).
        if (t.type == TokenType::STRING || t.type == TokenType::DQSTRING)
            return t.col + keel::as<int>(t.value.size()) + 1;
        return t.col + keel::as<int>(t.value.size()) - 1;
    };
    auto flushArg = [&]() {
        if (!haveArg)
            return;
        auto arg = makeNode(NodeType::STRING_LITERAL, argLine, argCol);
        arg->strValue = keel::move(argStr);
        node->children.push_back(keel::move(arg));
        haveArg = false;
        argStr.clear();
    };

    while (!isAtEnd() && !check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON)
           && !check(TokenType::COMMA) && current().line == cmdLine) {
        const Token &tok = current();
        // A whitespace gap before this token starts a new argument.
        if (haveArg && tok.col > prevEnd)
            flushArg();
        if (!haveArg) {
            argLine = tok.line;
            argCol = tok.col;
            haveArg = true;
        }
        argStr.append(tok.value);
        prevEnd = tokenEndCol(tok) + 1;
        advance();
    }
    flushArg();

    node->suppressOutput = consumeStmtTerminator(*node);
    skipNewlines();
    return node;
}

ASTNodePtr Parser::parseExpressionStatement()
{
    // Command-style вызов: clear all, grid on, cd dir, и т.д.
    // Проверяем ДО multi-assign и expression parsing.
    if (isCommandStyleCall())
        return parseCommandStyleCall();

    // Попытка multi-assign: [a, b] = expr  или  [~, b] = expr
    if (check(TokenType::LBRACKET)) {
        keel::usize save = pos_;
        auto ma = tryMultiAssign();
        if (ma)
            return ma;
        pos_ = save;
    }

    auto [startLine, startCol] = loc();
    auto expr = parseExpression();

    if (check(TokenType::ASSIGN)) {
        advance();

        // `[lvalue] = rhs` — a single-element bracketed target is just a
        // plain assignment to that one lvalue. tryMultiAssign() above only
        // accepts bare identifiers, so a non-identifier target like
        // `[r.B] = r.b`, `[a(1)] = x` or `[c{1}] = x` falls through here as
        // an ASSIGN whose LHS is a 1x1 MATRIX_LITERAL — which neither the
        // TreeWalker nor the VM compiler can assign to. Unwrap it to the
        // inner lvalue so both backends see `lvalue = rhs`. MATLAB treats
        // `[X] = Y` (single target) identically to `X = Y`.
        if (expr && expr->type == NodeType::MATRIX_LITERAL
            && expr->children.size() == 1
            && expr->children[0]
            && expr->children[0]->children.size() == 1
            && expr->children[0]->children[0]) {
            NodeType it = expr->children[0]->children[0]->type;
            if (it == NodeType::IDENTIFIER || it == NodeType::FIELD_ACCESS
                || it == NodeType::CALL || it == NodeType::CELL_INDEX
                || it == NodeType::DYNAMIC_FIELD_ACCESS) {
                expr = keel::move(expr->children[0]->children[0]);
            }
        }

        // FIX #12: Distinguish `A(idx) = []` (delete elements, only when `[]`
        // is the entire RHS expression) from `x = []` / `x = []'` (normal
        // expression assignment handled by parseExpression()).
        bool isIndexedLhs = (expr->type == NodeType::CALL || expr->type == NodeType::CELL_INDEX);
        if (isIndexedLhs && check(TokenType::LBRACKET) && peekToken(1).type == TokenType::RBRACKET) {
            TokenType afterEmpty = peekToken(2).type;
            if (afterEmpty == TokenType::SEMICOLON || afterEmpty == TokenType::COMMA
                || afterEmpty == TokenType::NEWLINE || afterEmpty == TokenType::END_OF_INPUT
                || afterEmpty == TokenType::KW_END || afterEmpty == TokenType::KW_ELSE
                || afterEmpty == TokenType::KW_ELSEIF || afterEmpty == TokenType::KW_CATCH
                || afterEmpty == TokenType::KW_CASE || afterEmpty == TokenType::KW_OTHERWISE) {
                advance();  // [
                advance();  // ]
                auto node = makeNode(NodeType::DELETE_ASSIGN, startLine, startCol);
                node->children.push_back(keel::move(expr));
                node->suppressOutput = consumeStmtTerminator(*node);
                skipNewlines();
                return node;
            }
        }
        auto rhs = parseExpression();
        auto node = makeNode(NodeType::ASSIGN, startLine, startCol);
        node->children.push_back(keel::move(expr));
        node->children.push_back(keel::move(rhs));
        node->suppressOutput = consumeStmtTerminator(*node);
        skipNewlines();
        return node;
    }

    auto stmt = makeNode(NodeType::EXPR_STMT, startLine, startCol);
    stmt->children.push_back(keel::move(expr));
    stmt->suppressOutput = consumeStmtTerminator(*stmt);
    skipNewlines();
    return stmt;
}

ASTNodePtr Parser::tryMultiAssign()
{
    // Мягкий разбор — возвращает nullptr без исключений при неудаче.
    // Поддерживает ~ (tilde) для игнорируемых выходов: [~, b] = func()
    if (!check(TokenType::LBRACKET))
        return nullptr;

    auto [startLine, startCol] = loc();
    advance();

    keel::Array<keel::String> names;
    keel::Array<ASTNodePtr> targets;
    bool anyComplex = false;

    // Разбор одного выходного элемента: ~ , bare identifier, или
    // сложный lvalue (s.f / a(i) / c{i} / s.(e) / s.a.b ...).
    // Возвращает false при не-lvalue — caller откатывает pos_ и
    // трактует `[...]` как обычный matrix literal.
    auto parseElem = [&]() -> bool {
        if (check(TokenType::TILDE)) {
            advance();
            names.push_back(keel::String::from("~"));
            targets.push_back(nullptr);
            return true;
        }
        if (!check(TokenType::IDENTIFIER))
            return false;
        auto expr = parsePostfix();
        if (!expr)
            return false;
        switch (expr->type) {
        case NodeType::IDENTIFIER:
            names.push_back(expr->strValue.clone());
            break;
        case NodeType::FIELD_ACCESS:
        case NodeType::DYNAMIC_FIELD_ACCESS:
        case NodeType::CALL:
        case NodeType::INDEX:
        case NodeType::CELL_INDEX:
            anyComplex = true;
            names.emplace_back(); // empty — authoritative target is in lhsTargets
            break;
        default:
            return false; // not an assignable lvalue (e.g. a transpose / binop)
        }
        targets.push_back(keel::move(expr));
        return true;
    };

    // Первый элемент
    if (!parseElem())
        return nullptr;

    // Остальные элементы через запятую или пробел ([a, b] = ... и [a b] = ...)
    while (!check(TokenType::RBRACKET) && !isAtEnd()) {
        match(TokenType::COMMA);
        if (check(TokenType::RBRACKET))
            break;
        if (!parseElem())
            return nullptr;
    }

    if (!check(TokenType::RBRACKET))
        return nullptr;
    advance();

    if (!check(TokenType::ASSIGN))
        return nullptr;

    // Точка невозврата: после '=' это точно multi-assign.
    // Ошибка в RHS — реальная синтаксическая ошибка.
    advance();

    auto rhs = parseExpression();
    if (names.size() > 1 && rhs && rhs->type == NodeType::IDENTIFIER) {
        auto callNode = makeNode(NodeType::CALL, rhs->line, rhs->col);
        callNode->children.push_back(keel::move(rhs));
        rhs = keel::move(callNode);
    }
    auto node = makeNode(NodeType::MULTI_ASSIGN, startLine, startCol);
    node->returnNames = keel::move(names);
    // Прикрепляем полноценные lvalue-цели только когда есть хотя бы одна
    // сложная — простой путь (все идентификаторы / ~) остаётся на
    // returnNames без изменений (нулевой риск регрессии).
    if (anyComplex)
        node->lhsTargets = keel::move(targets);
    node->children.push_back(keel::move(rhs));
    node->suppressOutput = consumeStmtTerminator(*node);
    skipNewlines();
    return node;
}

// ============================================================
// Control flow
// ============================================================

namespace {
void promoteCondShortCircuitAnd(ASTNode *node)
{
    if (!node || node->type != NodeType::BINARY_OP)
        return;
    if (node->strValue == "&" || node->strValue == "&&") {
        node->strValue.assign("&&");
        for (auto &ch : node->children)
            promoteCondShortCircuitAnd(ch.get());
    } else if (node->strValue == "|" || node->strValue == "||") {
        node->strValue.assign("||");
        for (auto &ch : node->children)
            promoteCondShortCircuitAnd(ch.get());
    }
}
} // namespace

ASTNodePtr Parser::parseIf()
{
    NestingGuard guard(*this);
    auto [ln, cl] = loc();
    auto node = makeNode(NodeType::IF_STMT, ln, cl);
    consume(TokenType::KW_IF, "if");

    auto cond = parseExpression();
    promoteCondShortCircuitAnd(cond.get());
    skipTerminators();
    auto body = parseBlock({TokenType::KW_ELSEIF, TokenType::KW_ELSE, TokenType::KW_END});
    node->branches.push_back(ASTBranch{keel::move(cond), keel::move(body)});

    while (check(TokenType::KW_ELSEIF)) {
        advance();
        auto c = parseExpression();
        promoteCondShortCircuitAnd(c.get());
        skipTerminators();
        auto b = parseBlock({TokenType::KW_ELSEIF, TokenType::KW_ELSE, TokenType::KW_END});
        node->branches.push_back(ASTBranch{keel::move(c), keel::move(b)});
    }

    if (match(TokenType::KW_ELSE)) {
        skipTerminators();
        node->elseBranch = parseBlock({TokenType::KW_END});
    }

    node->endLine = current().line;
    consume(TokenType::KW_END, "end");
    skipTerminators();
    return node;
}

ASTNodePtr Parser::parseFor()
{
    NestingGuard guard(*this);
    auto [ln, cl] = loc();
    auto node = makeNode(NodeType::FOR_STMT, ln, cl);
    consume(TokenType::KW_FOR, "for");

    const bool paren = check(TokenType::LPAREN);
    if (paren)
        advance();
    node->strValue = consume(TokenType::IDENTIFIER, "loop variable").value;
    consume(TokenType::ASSIGN, "=");
    node->children.push_back(parseExpression());
    if (paren) {
        if (check(TokenType::COMMA)) {
            advance();
            (void)parseExpression();
        }
        consume(TokenType::RPAREN, ")");
    }
    skipTerminators();

    node->children.push_back(parseBlock({TokenType::KW_END}));
    node->endLine = current().line;
    consume(TokenType::KW_END, "end");
    skipTerminators();
    return node;
}

ASTNodePtr Parser::parseWhile()
{
    NestingGuard guard(*this);
    auto [ln, cl] = loc();
    auto node = makeNode(NodeType::WHILE_STMT, ln, cl);
    consume(TokenType::KW_WHILE, "while");

    auto cond = parseExpression();
    promoteCondShortCircuitAnd(cond.get());
    node->children.push_back(keel::move(cond));
    skipTerminators();

    node->children.push_back(parseBlock({TokenType::KW_END}));
    node->endLine = current().line;
    consume(TokenType::KW_END, "end");
    skipTerminators();
    return node;
}

ASTNodePtr Parser::parseSwitch()
{
    NestingGuard guard(*this);
    auto [ln, cl] = loc();
    auto node = makeNode(NodeType::SWITCH_STMT, ln, cl);
    consume(TokenType::KW_SWITCH, "switch");

    node->children.push_back(parseExpression());
    skipTerminators();

    while (check(TokenType::KW_CASE)) {
        advance();
        auto ce = parseExpression();
        skipTerminators();
        auto b = parseBlock({TokenType::KW_CASE, TokenType::KW_OTHERWISE, TokenType::KW_END});
        node->branches.push_back(ASTBranch{keel::move(ce), keel::move(b)});
    }

    if (match(TokenType::KW_OTHERWISE)) {
        skipTerminators();
        node->elseBranch = parseBlock({TokenType::KW_END});
    }

    node->endLine = current().line;
    consume(TokenType::KW_END, "end");
    skipTerminators();
    return node;
}

ASTNodePtr Parser::parseTryCatch()
{
    NestingGuard guard(*this);
    auto [ln, cl] = loc();
    auto node = makeNode(NodeType::TRY_STMT, ln, cl);
    consume(TokenType::KW_TRY, "try");
    skipTerminators();

    node->children.push_back(parseBlock({TokenType::KW_CATCH, TokenType::KW_END}));

    if (match(TokenType::KW_CATCH)) {
        if (check(TokenType::IDENTIFIER)) {
            node->strValue = current().value.clone();
            advance();
        }
        skipTerminators();
        node->children.push_back(parseBlock({TokenType::KW_END}));
    }

    node->endLine = current().line;
    consume(TokenType::KW_END, "end");
    skipTerminators();
    return node;
}

ASTNodePtr Parser::parseGlobalPersistent()
{
    bool isGlobal = check(TokenType::KW_GLOBAL);
    auto [ln, cl] = loc();
    auto node = makeNode(isGlobal ? NodeType::GLOBAL_STMT : NodeType::PERSISTENT_STMT, ln, cl);
    advance();

    while (check(TokenType::IDENTIFIER)) {
        node->paramNames.push_back(current().value.clone());
        advance();
    }

    consumeStmtTerminator(*node);  // record `;` / `,` pos for graph view
    skipTerminators();
    return node;
}

// ============================================================
// Function definition
// ============================================================

bool Parser::probeHasOutputSignature() const
{
    if (current().type == TokenType::LBRACKET) {
        keel::usize probe = pos_ + 1;
        int depth = 1;
        while (probe < tokens_.size() && depth > 0) {
            if (tokens_[probe].type == TokenType::END_OF_INPUT)
                break;
            if (tokens_[probe].type == TokenType::LBRACKET)
                depth++;
            else if (tokens_[probe].type == TokenType::RBRACKET)
                depth--;
            probe++;
        }
        return (probe < tokens_.size() && tokens_[probe].type == TokenType::ASSIGN);
    }

    if (current().type == TokenType::IDENTIFIER && peekToken(1).type == TokenType::ASSIGN)
        return true;

    return false;
}

ASTNodePtr Parser::parseFunctionDef()
{
    auto [ln, cl] = loc();
    auto node = makeNode(NodeType::FUNCTION_DEF, ln, cl);
    consume(TokenType::KW_FUNCTION, "function");

    bool hasOutput = probeHasOutputSignature();

    if (hasOutput) {
        if (check(TokenType::LBRACKET)) {
            advance();
            while (!check(TokenType::RBRACKET) && !isAtEnd()) {
                match(TokenType::COMMA);
                if (check(TokenType::RBRACKET))
                    break;
                node->returnNames.push_back(consume(TokenType::IDENTIFIER, "return var").value);
            }
            consume(TokenType::RBRACKET, "]");
        } else {
            node->returnNames.push_back(consume(TokenType::IDENTIFIER, "return var").value);
        }
        consume(TokenType::ASSIGN, "=");
    }

    node->strValue = consume(TokenType::IDENTIFIER, "function name").value;
    // classdef property accessors carry a dotted name: `get.Prop` / `set.Prop`.
    if (check(TokenType::DOT)) {
        advance();
        Token propTok = consume(TokenType::IDENTIFIER, "accessor property");
        node->strValue.push_back('.');
        node->strValue.append(propTok.value);
    }

    if (match(TokenType::LPAREN)) {
        auto consumeParam = [&]() -> keel::String {
            if (match(TokenType::TILDE))
                return keel::format("__unused_{}", node->paramNames.size());
            return consume(TokenType::IDENTIFIER, "param").value;
        };
        if (!check(TokenType::RPAREN)) {
            node->paramNames.push_back(consumeParam());
            while (match(TokenType::COMMA))
                node->paramNames.push_back(consumeParam());
        }
        consume(TokenType::RPAREN, ")");
    }

    skipTerminators();
    node->children.push_back(parseBlock({TokenType::KW_END}));

    if (check(TokenType::KW_END)) {
        node->endLine = current().line;
        advance();
    } else if (!isAtEnd()) {
        keel::String err = keel::format("Expected 'end' for function '{}' defined at line {}",
                                        node->strValue, node->line);
        throw std::runtime_error(err.c_str());
    }

    skipTerminators();
    return node;
}

// ============================================================
// classdef
// ============================================================

ASTNodePtr Parser::parseClassDef()
{
    auto [ln, cl] = loc();
    auto node = makeNode(NodeType::CLASSDEF_DEF, ln, cl);
    consume(TokenType::KW_CLASSDEF, "classdef");

    // Parse an optional attribute block `(Attr, Attr=val, ...)`, returning
    // the identifier tokens flattened (e.g. (Access=private) → {"Access",
    // "private"}; (Static) → {"Static"}). Non-identifier values are skipped.
    auto parseAttrBlock = [&]() -> keel::Array<keel::String> {
        keel::Array<keel::String> attrs;
        if (!check(TokenType::LPAREN))
            return attrs;
        advance(); // (
        int depth = 1;
        while (depth > 0 && !isAtEnd()) {
            if (check(TokenType::LPAREN))
                ++depth;
            else if (check(TokenType::RPAREN))
                --depth;
            else if (check(TokenType::IDENTIFIER))
                attrs.push_back(current().value.clone());
            advance();
        }
        return attrs;
    };
    node->classAttrs = parseAttrBlock(); // class-level attributes (e.g. Sealed)

    node->strValue = consume(TokenType::IDENTIFIER, "class name").value;

    // Superclasses: `< Base1 & Base2 & ...` → paramNames. A superclass may be
    // a package-qualified name (`< pkg.sub.Base`), so read a dotted run, not a
    // single identifier — otherwise the trailing `.Base` falls through to the
    // body parser and errors as a missing block.
    auto parseSuperclassName = [&]() -> keel::String {
        keel::String name = consume(TokenType::IDENTIFIER, "superclass").value;
        while (check(TokenType::DOT)) {
            advance();
            Token part = consume(TokenType::IDENTIFIER, "superclass");
            name.push_back('.');
            name.append(part.value);
        }
        return name;
    };
    if (check(TokenType::LT)) {
        advance();
        node->paramNames.push_back(parseSuperclassName());
        while (check(TokenType::AND)) {
            advance();
            node->paramNames.push_back(parseSuperclassName());
        }
    }
    skipTerminators();

    // Body: a sequence of `properties ... end` / `methods ... end` blocks.
    // `properties` / `methods` are contextual identifiers, not keywords.
    while (!isAtEnd() && !check(TokenType::KW_END)) {
        const bool isProps = check(TokenType::IDENTIFIER) && current().value == "properties";
        const bool isMethods = check(TokenType::IDENTIFIER) && current().value == "methods";
        const bool isEnum = check(TokenType::IDENTIFIER) && current().value == "enumeration";
        if (!isProps && !isMethods && !isEnum) {
            keel::String err = keel::format(
                "classdef '{}': expected 'properties', 'methods' or 'enumeration' block at line {}",
                node->strValue, current().line);
            throw std::runtime_error(err.c_str());
        }
        advance();                              // consume the block keyword
        keel::Array<keel::String> blockAttrs = parseAttrBlock();
        skipTerminators();

        while (!isAtEnd() && !check(TokenType::KW_END)) {
            if (isProps) {
                auto [pln, pcl] = loc();
                auto prop = makeNode(NodeType::CLASSDEF_PROPERTY, pln, pcl);
                prop->strValue = consume(TokenType::IDENTIFIER, "property name").value;
                if (current().line == pln && match(TokenType::LPAREN)) {
                    int depth = 1;
                    while (!isAtEnd() && depth > 0) {
                        if (check(TokenType::LPAREN)) ++depth;
                        else if (check(TokenType::RPAREN)) --depth;
                        advance();
                    }
                }
                if (current().line == pln && check(TokenType::IDENTIFIER)) {
                    advance();
                    while (current().line == pln && match(TokenType::DOT) && check(TokenType::IDENTIFIER))
                        advance();
                }
                if (current().line == pln && match(TokenType::LBRACE)) {
                    int depth = 1;
                    while (!isAtEnd() && depth > 0) {
                        if (check(TokenType::LBRACE)) ++depth;
                        else if (check(TokenType::RBRACE)) --depth;
                        advance();
                    }
                }
                if (match(TokenType::ASSIGN))
                    prop->children.push_back(parseExpression());
                prop->classAttrs = blockAttrs.clone();
                node->children.push_back(keel::move(prop));
            } else if (isEnum) {
                // `Name` or `Name(arg, ...)` — a member, optionally with the
                // constructor arguments that define its underlying value.
                auto [eln, ecl] = loc();
                auto em = makeNode(NodeType::CLASSDEF_ENUM_MEMBER, eln, ecl);
                em->strValue = consume(TokenType::IDENTIFIER, "enumeration member").value;
                if (match(TokenType::LPAREN)) {
                    if (!check(TokenType::RPAREN)) {
                        em->children.push_back(parseExpression());
                        while (match(TokenType::COMMA))
                            em->children.push_back(parseExpression());
                    }
                    consume(TokenType::RPAREN, ")");
                }
                node->children.push_back(keel::move(em));
                match(TokenType::COMMA); // members may be comma-separated on a line
            } else if (!check(TokenType::KW_FUNCTION)) {
                // Abstract method: a signature with no `function`/body —
                // `[o1,o2] = name(p…)`, `o = name(p…)`, `name(p…)`, or `name`.
                auto [fln, fcl] = loc();
                auto fn = makeNode(NodeType::FUNCTION_DEF, fln, fcl);
                fn->classAttrs = blockAttrs.clone(); // carries the `Abstract` attribute
                if (check(TokenType::LBRACKET)) {
                    advance();
                    while (!check(TokenType::RBRACKET)) {
                        fn->returnNames.push_back(
                            consume(TokenType::IDENTIFIER, "output name").value);
                        if (!match(TokenType::COMMA))
                            break;
                    }
                    consume(TokenType::RBRACKET, "]");
                    consume(TokenType::ASSIGN, "=");
                    fn->strValue = consume(TokenType::IDENTIFIER, "method name").value;
                } else {
                    keel::String first = consume(TokenType::IDENTIFIER, "method name").value;
                    if (match(TokenType::ASSIGN)) {
                        fn->returnNames.push_back(keel::move(first));
                        fn->strValue = consume(TokenType::IDENTIFIER, "method name").value;
                    } else {
                        fn->strValue = keel::move(first);
                    }
                }
                if (match(TokenType::LPAREN)) {
                    while (!check(TokenType::RPAREN)) {
                        fn->paramNames.push_back(
                            consume(TokenType::IDENTIFIER, "parameter").value);
                        if (!match(TokenType::COMMA))
                            break;
                    }
                    consume(TokenType::RPAREN, ")");
                }
                node->children.push_back(keel::move(fn)); // empty body → abstract
            } else {
                auto fn = parseFunctionDef();
                fn->classAttrs = blockAttrs.clone();
                node->children.push_back(keel::move(fn));
            }
            skipTerminators();
        }
        consume(TokenType::KW_END, isProps      ? "end (properties)"
                                   : isEnum     ? "end (enumeration)"
                                                : "end (methods)");
        skipTerminators();
    }

    if (check(TokenType::KW_END)) {
        node->endLine = current().line;
        advance();
    } else if (!isAtEnd()) {
        keel::String err = keel::format("Expected 'end' for classdef '{}'", node->strValue);
        throw std::runtime_error(err.c_str());
    }
    skipTerminators();
    return node;
}

// ============================================================
// Block
// ============================================================

ASTNodePtr Parser::parseBlock(std::initializer_list<TokenType> terminators)
{
    NestingGuard guard(*this);
    auto [ln, cl] = loc();
    auto block = makeNode(NodeType::BLOCK, ln, cl);
    while (!isAtEnd()) {
        if (isTerminator(terminators))
            return block;
        keel::usize before = pos_;
        auto stmt = parseStatement();
        if (stmt)
            block->children.push_back(keel::move(stmt));
        skipNewlines();
        if (pos_ == before) {
            keel::String err = keel::format("Parse error: stuck in block at line {} col {}",
                                            current().line, current().col);
            throw std::runtime_error(err.c_str());
        }
    }
    return block;
}

// ============================================================
// Expressions
// FIX #1: Правильные приоритеты MATLAB (от низкого к высокому):
//   ||  →  &&  →  |  →  &  →  == ~= < > <= >=  →  :
//   →  + -  →  * / \ .* ./ .\  →  - ~ +  →  ^ .^  →  postfix
// ============================================================

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
        throw std::runtime_error(err.c_str());
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
            throw std::runtime_error(err.c_str());
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
