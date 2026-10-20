#include "lemma/parser.h"

#include "lemma/error.h"

#include <fast_float/fast_float.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <utility>

namespace lemma {

namespace {

struct Token {
    enum class Kind { Number, Identifier, Op, LParen, RParen, Comma, Semicolon, Assign, Equals, Equation, Arrow, End };

    Token(Kind k, std::size_t b, std::size_t e) : kind(k), begin(b), end(e) {}

    Kind kind;
    std::size_t begin;
    std::size_t end;
    double number = 0.0;
    std::string text; // Identifier name, or the ASCII form of an Op
};

// Unicode spellings of operators, mapped to their ASCII equivalents.
struct Alias {
    std::string_view utf8;
    Token::Kind kind;
    char op;
};

constexpr Alias kAliases[] = {
    {"·", Token::Kind::Op, '*'},
    {"×", Token::Kind::Op, '*'},
    {"÷", Token::Kind::Op, '/'},
    {"−", Token::Kind::Op, '-'},
    {"≔", Token::Kind::Assign, 0},
    {"→", Token::Kind::Arrow, 0},
    {"⩵", Token::Kind::Equation, 0},
};

const Alias* aliasAt(std::string_view src, std::size_t i)
{
    for (const Alias& alias : kAliases) {
        if (src.substr(i).starts_with(alias.utf8)) {
            return &alias;
        }
    }
    return nullptr;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

std::vector<Token> tokenize(std::string_view src)
{
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < src.size()) {
        const auto c = static_cast<unsigned char>(src[i]);
        if (std::isspace(c)) {
            ++i;
            continue;
        }

        if (const Alias* alias = aliasAt(src, i)) {
            Token t{alias->kind, i, i + alias->utf8.size()};
            if (alias->op) {
                t.text = std::string(1, alias->op);
            }
            tokens.push_back(std::move(t));
            i += alias->utf8.size();
            continue;
        }

        if (isDigit(src[i]) || (src[i] == '.' && i + 1 < src.size() && isDigit(src[i + 1]))) {
            const std::size_t start = i;
            while (i < src.size() && isDigit(src[i])) ++i;
            if (i < src.size() && src[i] == '.') {
                ++i;
                while (i < src.size() && isDigit(src[i])) ++i;
            }
            // Only an exponent if digits follow, so "2 e" and "2em" stay multiplication.
            if (i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
                std::size_t j = i + 1;
                if (j < src.size() && (src[j] == '+' || src[j] == '-')) ++j;
                if (j < src.size() && isDigit(src[j])) {
                    i = j;
                    while (i < src.size() && isDigit(src[i])) ++i;
                }
            }
            Token t{Token::Kind::Number, start, i};
            const auto result = fast_float::from_chars(src.data() + start, src.data() + i, t.number);
            if (result.ec == std::errc::result_out_of_range) {
                throw Error("Number is out of range", start, i);
            }
            tokens.push_back(std::move(t));
            continue;
        }

        if (isIdentStart(c)) {
            const std::size_t start = i;
            // Stop before a Unicode operator glued to a name, e.g. "a·b".
            while (i < src.size() && isIdentChar(static_cast<unsigned char>(src[i])) && !aliasAt(src, i)) {
                ++i;
            }
            Token t{Token::Kind::Identifier, start, i};
            t.text = std::string(src.substr(start, i - start));
            tokens.push_back(std::move(t));
            continue;
        }

        bool matched = false;
        for (const auto& [text, kind] : {std::pair{":=", Token::Kind::Assign}, std::pair{"->", Token::Kind::Arrow},
                                         std::pair{"==", Token::Kind::Equation}}) {
            if (src.substr(i).starts_with(text)) {
                tokens.push_back({kind, i, i + 2});
                i += 2;
                matched = true;
                break;
            }
        }
        if (matched) {
            continue;
        }

        Token t{Token::Kind::End, i, i + 1};
        switch (src[i]) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '^':
            t.kind = Token::Kind::Op;
            t.text = std::string(1, src[i]);
            break;
        case '(': t.kind = Token::Kind::LParen; break;
        case ')': t.kind = Token::Kind::RParen; break;
        case ',': t.kind = Token::Kind::Comma; break;
        case ';': t.kind = Token::Kind::Semicolon; break;
        case '=': t.kind = Token::Kind::Equals; break;
        default:
            throw Error("Unexpected character '" + std::string(1, src[i]) + "'", i, i + 1);
        }
        tokens.push_back(std::move(t));
        ++i;
    }
    tokens.push_back({Token::Kind::End, src.size(), src.size()});
    return tokens;
}

class Parser {
public:
    explicit Parser(std::string_view src) : src_(src), tokens_(tokenize(src)) {}

    Statement parse()
    {
        Statement s;
        if (peek().kind == Token::Kind::End) {
            return s;
        }

        if (peek().kind == Token::Kind::Identifier && peek(1).kind == Token::Kind::Assign) {
            s.kind = Statement::Kind::Definition;
            s.name = next().text;
            s.assignPos = next().begin;
            s.expr = expression();
        } else if (isFunctionDefinition()) {
            s.kind = Statement::Kind::FunctionDefinition;
            s.name = next().text;
            next(); // (
            if (peek().kind != Token::Kind::RParen) {
                do {
                    s.params.push_back(expect(Token::Kind::Identifier, "Expected a parameter name").text);
                } while (accept(Token::Kind::Comma));
            }
            expect(Token::Kind::RParen, "Expected ')'");
            s.assignPos = expect(Token::Kind::Assign, "Expected ':='").begin;
            s.expr = expression();
        } else {
            s.kind = Statement::Kind::Expression;
            s.expr = relation();
            if (accept(Token::Kind::Arrow)) {
                s.kind = Statement::Kind::Symbolic;
                keywords(s);
            } else if (peek().kind == Token::Kind::Equals) {
                s.kind = Statement::Kind::Evaluation;
                s.equalsPos = next().begin;
                if (peek().kind != Token::Kind::End) {
                    const std::size_t start = peek().begin;
                    s.unit = expression();
                    s.unitText = std::string(src_.substr(start, s.unit->end - start));
                }
            } else if (peek().kind == Token::Kind::Assign) {
                throw Error("Only a name or a function can be defined with :=", peek().begin, peek().end);
            }
        }

        if (peek().kind != Token::Kind::End) {
            throw unexpected(peek());
        }
        return s;
    }

private:
    const Token& peek(std::size_t ahead = 0) const
    {
        return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)];
    }

    const Token& next() { return tokens_[pos_ < tokens_.size() - 1 ? pos_++ : pos_]; }

    bool accept(Token::Kind kind)
    {
        if (peek().kind != kind) return false;
        next();
        return true;
    }

    const Token& expect(Token::Kind kind, const char* message)
    {
        if (peek().kind != kind) {
            throw Error(message, peek().begin, peek().end);
        }
        return next();
    }

    Error unexpected(const Token& t) const
    {
        if (t.kind == Token::Kind::End) {
            return Error("Incomplete expression", t.begin, t.end);
        }
        return Error("Unexpected '" + std::string(src_.substr(t.begin, t.end - t.begin)) + "'", t.begin, t.end);
    }

    bool isOp(char op) const { return peek().kind == Token::Kind::Op && peek().text[0] == op; }

    // name ( [name {, name}] ) :=
    bool isFunctionDefinition() const
    {
        if (peek().kind != Token::Kind::Identifier || peek(1).kind != Token::Kind::LParen) {
            return false;
        }
        std::size_t i = 2;
        if (peek(i).kind == Token::Kind::Identifier) {
            ++i;
            while (peek(i).kind == Token::Kind::Comma && peek(i + 1).kind == Token::Kind::Identifier) {
                i += 2;
            }
        }
        return peek(i).kind == Token::Kind::RParen && peek(i + 1).kind == Token::Kind::Assign;
    }

    static NodePtr makeBinary(char op, NodePtr lhs, NodePtr rhs)
    {
        auto n = std::make_shared<Node>();
        n->kind = Node::Kind::Binary;
        n->op = op;
        n->begin = lhs->begin;
        n->end = rhs->end;
        n->args = {std::move(lhs), std::move(rhs)};
        return n;
    }

    // keywords := [keyword {';' keyword}]      keyword := name {',' relation}
    void keywords(Statement& s)
    {
        if (peek().kind == Token::Kind::End) {
            return;
        }
        do {
            Keyword k{expect(Token::Kind::Identifier, "Expected a keyword").text, {}};
            while (accept(Token::Kind::Comma)) {
                k.args.push_back(relation());
            }
            s.keywords.push_back(std::move(k));
        } while (accept(Token::Kind::Semicolon));
    }

    // relation := expression ['==' expression]      an equation, for symbolic evaluation
    NodePtr relation()
    {
        NodePtr lhs = expression();
        if (accept(Token::Kind::Equation)) {
            return makeBinary('=', lhs, expression());
        }
        return lhs;
    }

    // expression := term (('+' | '-') term)*
    NodePtr expression()
    {
        NodePtr lhs = term();
        while (isOp('+') || isOp('-')) {
            const char op = next().text[0];
            lhs = makeBinary(op, lhs, term());
        }
        return lhs;
    }

    // term := unary (('*' | '/') unary | unary)*      juxtaposition multiplies: "10 N"
    NodePtr term()
    {
        NodePtr lhs = unary();
        while (true) {
            if (isOp('*') || isOp('/')) {
                const char op = next().text[0];
                lhs = makeBinary(op, lhs, unary());
            } else if (peek().kind == Token::Kind::Identifier || peek().kind == Token::Kind::LParen) {
                auto product = std::make_shared<Node>(*makeBinary('*', lhs, unary()));
                product->juxtaposed = true;
                lhs = std::move(product);
            } else {
                return lhs;
            }
        }
    }

    // unary := ('-' | '+') unary | power
    NodePtr unary()
    {
        if (isOp('-') || isOp('+')) {
            const Token& t = next();
            const char op = t.text[0];
            const std::size_t begin = t.begin;
            NodePtr operand = unary();
            if (op == '+') {
                return operand;
            }
            auto n = std::make_shared<Node>();
            n->kind = Node::Kind::Unary;
            n->op = '-';
            n->begin = begin;
            n->end = operand->end;
            n->args = {std::move(operand)};
            return n;
        }
        return power();
    }

    // power := primary ('^' unary)?      right associative, and -2^2 is -(2^2)
    NodePtr power()
    {
        NodePtr base = primary();
        if (isOp('^')) {
            next();
            return makeBinary('^', base, unary());
        }
        return base;
    }

    // primary := number | name | name '(' args ')' | '(' expression ')'
    NodePtr primary()
    {
        const Token& t = peek();
        auto n = std::make_shared<Node>();
        n->begin = t.begin;
        n->end = t.end;
        switch (t.kind) {
        case Token::Kind::Number:
            n->kind = Node::Kind::Number;
            n->number = next().number;
            return n;
        case Token::Kind::Identifier:
            n->kind = Node::Kind::Identifier;
            n->name = next().text;
            if (accept(Token::Kind::LParen)) {
                n->kind = Node::Kind::Call;
                if (peek().kind != Token::Kind::RParen) {
                    do {
                        n->args.push_back(expression());
                    } while (accept(Token::Kind::Comma));
                }
                n->end = expect(Token::Kind::RParen, "Expected ')'").end;
            }
            return n;
        case Token::Kind::LParen: {
            next();
            NodePtr inner = expression();
            expect(Token::Kind::RParen, "Expected ')'");
            return inner;
        }
        default:
            throw unexpected(t);
        }
    }

    std::string_view src_;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

} // namespace

Statement parseStatement(std::string_view source)
{
    return Parser(source).parse();
}

} // namespace lemma
