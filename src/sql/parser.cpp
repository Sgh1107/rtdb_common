#include "rtdb/sql.hpp"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace rtdb {
namespace sql {

namespace {

enum class Tok : uint8_t {
    kError = 0,
    kEof,
    kIdentifier,
    kInteger,
    kFloat,
    kString,
    kComma,
    kLParen,
    kRParen,
    kStar,
    kSemicolon,
    kDot,
    kMinus,
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
    kSelect,
    kInsert,
    kInto,
    kValues,
    kUpdate,
    kSet,
    kDelete,
    kFrom,
    kWhere,
    kOrder,
    kBy,
    kLimit,
    kOffset,
    kAnd,
    kOr,
    kNot,
    kIs,
    kNull,
    kTrue,
    kFalse,
    kAs,
    kAsc,
    kDesc,
    /// 子集外但可识别的 SQL 关键字（JOIN/GROUP BY/…）：解析为 kUnsupported。
    kUnsupportedKeyword,
};

struct Token {
    Tok type = Tok::kEof;
    std::string text;  ///< 标识符原文 / 错误消息 / 不支持关键字原文
    int64_t int_val = 0;
    double real_val = 0.0;
    size_t pos = 0;  ///< 起始字符偏移（诊断）
};

char LowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string Lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(LowerAscii(c));
    return out;
}

Tok KeywordType(const std::string& lower) noexcept {
    if (lower == "select") return Tok::kSelect;
    if (lower == "insert") return Tok::kInsert;
    if (lower == "into") return Tok::kInto;
    if (lower == "values") return Tok::kValues;
    if (lower == "update") return Tok::kUpdate;
    if (lower == "set") return Tok::kSet;
    if (lower == "delete") return Tok::kDelete;
    if (lower == "from") return Tok::kFrom;
    if (lower == "where") return Tok::kWhere;
    if (lower == "order") return Tok::kOrder;
    if (lower == "by") return Tok::kBy;
    if (lower == "limit") return Tok::kLimit;
    if (lower == "offset") return Tok::kOffset;
    if (lower == "and") return Tok::kAnd;
    if (lower == "or") return Tok::kOr;
    if (lower == "not") return Tok::kNot;
    if (lower == "is") return Tok::kIs;
    if (lower == "null") return Tok::kNull;
    if (lower == "true") return Tok::kTrue;
    if (lower == "false") return Tok::kFalse;
    if (lower == "as") return Tok::kAs;
    if (lower == "asc") return Tok::kAsc;
    if (lower == "desc") return Tok::kDesc;
    // 子集外但可识别的关键字：明确 kUnsupported，便于定位范围蔓延。
    if (lower == "join" || lower == "inner" || lower == "left" || lower == "right" ||
        lower == "outer" || lower == "full" || lower == "cross" || lower == "on" ||
        lower == "group" || lower == "having" || lower == "union" || lower == "distinct" ||
        lower == "in" || lower == "like" || lower == "between" || lower == "exists" ||
        lower == "create" || lower == "drop" || lower == "alter" || lower == "truncate" ||
        lower == "begin" || lower == "commit" || lower == "rollback" ||
        lower == "count" || lower == "sum" || lower == "avg" || lower == "min" ||
        lower == "max") {
        return Tok::kUnsupportedKeyword;
    }
    return Tok::kIdentifier;
}

bool IsIdentStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IsIdentCont(char c) noexcept { return IsIdentStart(c) || (c >= '0' && c <= '9'); }
bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

/// 词法分析器：一次性左到右；错误以 kError token 返回（不抛异常）。
class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    Token Next() {
        SkipBlankAndComments();
        Token t;
        t.pos = i_;
        if (i_ >= s_.size()) {
            t.type = Tok::kEof;
            return t;
        }
        const char c = s_[i_];
        if (IsIdentStart(c)) {
            const size_t b = i_;
            while (i_ < s_.size() && IsIdentCont(s_[i_])) ++i_;
            t.text.assign(s_.substr(b, i_ - b));
            t.type = KeywordType(Lower(t.text));
            return t;
        }
        if (IsDigit(c) || (c == '-' && i_ + 1 < s_.size() &&
                           (IsDigit(s_[i_ + 1]) || s_[i_ + 1] == '.'))) {
            return LexNumber();
        }
        if (c == '\'') return LexString();
        switch (c) {
            case ',':
                ++i_;
                t.type = Tok::kComma;
                return t;
            case '(':
                ++i_;
                t.type = Tok::kLParen;
                return t;
            case ')':
                ++i_;
                t.type = Tok::kRParen;
                return t;
            case '*':
                ++i_;
                t.type = Tok::kStar;
                return t;
            case ';':
                ++i_;
                t.type = Tok::kSemicolon;
                return t;
            case '.':
                ++i_;
                t.type = Tok::kDot;
                return t;
            case '-':
                ++i_;
                t.type = Tok::kMinus;
                return t;
            case '=':
                ++i_;
                t.type = Tok::kEq;
                return t;
            case '<':
                ++i_;
                if (i_ < s_.size() && s_[i_] == '=') {
                    ++i_;
                    t.type = Tok::kLe;
                } else if (i_ < s_.size() && s_[i_] == '>') {
                    ++i_;
                    t.type = Tok::kNe;
                } else {
                    t.type = Tok::kLt;
                }
                return t;
            case '>':
                ++i_;
                if (i_ < s_.size() && s_[i_] == '=') {
                    ++i_;
                    t.type = Tok::kGe;
                } else {
                    t.type = Tok::kGt;
                }
                return t;
            case '!':
                if (i_ + 1 < s_.size() && s_[i_ + 1] == '=') {
                    i_ += 2;
                    t.type = Tok::kNe;
                    return t;
                }
                return Error(t.pos, "unexpected character '!'（子集仅支持 !=）");
            default:
                break;
        }
        return Error(t.pos, std::string("unexpected character '") + c + "'");
    }

private:
    void SkipBlankAndComments() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i_;
                continue;
            }
            if (c == '-' && i_ + 1 < s_.size() && s_[i_ + 1] == '-') {
                i_ += 2;
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            break;
        }
    }

    Token Error(size_t pos, std::string msg) {
        ok_ = false;
        error_ = std::move(msg);
        Token t;
        t.type = Tok::kError;
        t.pos = pos;
        t.text = error_;
        return t;
    }

    Token LexNumber() {
        Token t;
        t.pos = i_;
        const size_t b = i_;
        if (s_[i_] == '-') ++i_;
        bool real = false;
        while (i_ < s_.size() && IsDigit(s_[i_])) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            real = true;
            ++i_;
            if (i_ >= s_.size() || !IsDigit(s_[i_])) {
                return Error(t.pos, "invalid numeric literal（小数点后缺数字）");
            }
            while (i_ < s_.size() && IsDigit(s_[i_])) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            real = true;
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ >= s_.size() || !IsDigit(s_[i_])) {
                return Error(t.pos, "invalid numeric literal（指数缺数字）");
            }
            while (i_ < s_.size() && IsDigit(s_[i_])) ++i_;
        }
        const std::string text(s_.substr(b, i_ - b));
        if (real) {
            errno = 0;
            char* end = nullptr;
            const double v = std::strtod(text.c_str(), &end);
            if (errno == ERANGE || end == nullptr || *end != '\0' || !std::isfinite(v)) {
                return Error(t.pos, "floating literal out of range");
            }
            t.type = Tok::kFloat;
            t.real_val = v;
        } else {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno == ERANGE || end == nullptr || *end != '\0') {
                return Error(t.pos, "integer literal out of range");
            }
            t.type = Tok::kInteger;
            t.int_val = static_cast<int64_t>(v);
        }
        return t;
    }

    Token LexString() {
        Token t;
        t.pos = i_;
        ++i_;  // 跳过起始单引号
        std::string out;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '\'') {
                if (i_ < s_.size() && s_[i_] == '\'') {  // '' 转义
                    out.push_back('\'');
                    ++i_;
                    continue;
                }
                t.type = Tok::kString;
                t.text = std::move(out);
                return t;
            }
            out.push_back(c);
        }
        return Error(t.pos, "unterminated string literal（缺结束单引号）");
    }

    std::string_view s_;
    size_t i_ = 0;
    bool ok_ = true;
    std::string error_;
};

class Parser {
public:
    explicit Parser(std::string_view sql) : lex_(sql) { Advance(); }

    Err Run(Statement* out) {
        if (out == nullptr) return Err::kInvalidArgument;
        if (cur_.type == Tok::kError) return FailSyntax(cur_.text);
        if (cur_.type == Tok::kEof) return FailSyntax("empty SQL statement");

        Err e = Err::kOk;
        switch (cur_.type) {
            case Tok::kSelect:
                out->type = StatementType::kSelect;
                e = ParseSelect(&out->select);
                break;
            case Tok::kInsert:
                out->type = StatementType::kInsert;
                e = ParseInsert(&out->insert);
                break;
            case Tok::kUpdate:
                out->type = StatementType::kUpdate;
                e = ParseUpdate(&out->update);
                break;
            case Tok::kDelete:
                out->type = StatementType::kDelete;
                e = ParseDelete(&out->del);
                break;
            case Tok::kUnsupportedKeyword:
                return FailUnsupported("unsupported SQL statement: " + cur_.text);
            default:
                return FailUnsupported("unsupported SQL statement: " + cur_.text);
        }
        if (e != Err::kOk) return e;

        if (cur_.type == Tok::kSemicolon) {
            Advance();
            if (cur_.type != Tok::kEof) return FailUnsupported("multiple statements not supported");
        }
        if (cur_.type == Tok::kUnsupportedKeyword) {
            return FailUnsupported("unsupported SQL clause: " + cur_.text);
        }
        if (cur_.type == Tok::kError) return FailSyntax(cur_.text);
        if (cur_.type != Tok::kEof) return FailSyntax("unexpected token after statement");
        return Err::kOk;
    }

    std::string LastError() const { return err_; }

private:
    // ---- token 流 ----
    void Advance() {
        cur_ = lex_.Next();
        if (cur_.type == Tok::kError) err_ = cur_.text;
    }

    Err FailSyntax(std::string msg) {
        err_ = std::move(msg);
        return Err::kSqlSyntax;
    }
    Err FailUnsupported(std::string msg) {
        err_ = std::move(msg);
        return Err::kUnsupported;
    }
    Err FailHere(const char* what) {
        if (cur_.type == Tok::kError) return FailSyntax(cur_.text);
        return FailSyntax(std::string("expected ") + what + " (at offset " +
                          std::to_string(cur_.pos) + ")");
    }

    bool Expect(Tok t, const char* what, Err* err) {
        if (cur_.type != t) {
            *err = FailHere(what);
            return false;
        }
        Advance();
        return true;
    }

    Err TakeIdentifier(std::string* out) {
        if (cur_.type == Tok::kUnsupportedKeyword) {
            return FailUnsupported("unsupported SQL keyword: " + cur_.text);
        }
        if (cur_.type != Tok::kIdentifier) return FailHere("identifier");
        *out = cur_.text;
        if (out->size() > 64) return FailSyntax("identifier too long (max 64)");
        Advance();
        if (cur_.type == Tok::kDot) return FailUnsupported("qualified name / subquery not supported");
        return Err::kOk;
    }

    Err TakeLiteral(ColumnValue* out) {
        switch (cur_.type) {
            case Tok::kInteger:
                *out = ColumnValue::Int(cur_.int_val);
                break;
            case Tok::kFloat:
                *out = ColumnValue::Real(cur_.real_val);
                break;
            case Tok::kString:
                *out = ColumnValue::Text(cur_.text);
                break;
            case Tok::kTrue:
                *out = ColumnValue::Bool(true);
                break;
            case Tok::kFalse:
                *out = ColumnValue::Bool(false);
                break;
            case Tok::kNull:
                *out = ColumnValue::Null();
                break;
            case Tok::kUnsupportedKeyword:
                return FailUnsupported("unsupported literal: " + cur_.text);
            default:
                return FailHere("literal");
        }
        Advance();
        return Err::kOk;
    }

    // ---- 语句 ----
    Err ParseSelect(SelectStatement* s) {
        Advance();  // SELECT
        if (cur_.type == Tok::kStar) {
            s->select_all = true;
            Advance();
        } else {
            for (;;) {
                std::string col;
                Err e = TakeIdentifier(&col);
                if (e != Err::kOk) return e;
                if (cur_.type == Tok::kLParen) {
                    return FailUnsupported("functions/aggregates not supported");
                }
                s->columns.push_back(std::move(col));
                // 可选别名：接受但 AST 不保留（docs/16 D4 子集内）。
                if (cur_.type == Tok::kAs) {
                    Advance();
                    std::string alias;
                    e = TakeIdentifier(&alias);
                    if (e != Err::kOk) return e;
                }
                if (cur_.type != Tok::kComma) break;
                Advance();
            }
            if (s->columns.empty()) return FailHere("select column");
        }

        Err e = Err::kOk;
        if (!Expect(Tok::kFrom, "FROM", &e)) return e;
        e = TakeIdentifier(&s->table);
        if (e != Err::kOk) return e;

        if (cur_.type == Tok::kWhere) {
            Advance();
            Condition cond;
            e = ParseExpr(&cond);
            if (e != Err::kOk) return e;
            s->where = std::move(cond);
        }
        if (cur_.type == Tok::kOrder) {
            Advance();
            if (!Expect(Tok::kBy, "BY", &e)) return e;
            for (;;) {
                OrderBy ob;
                e = TakeIdentifier(&ob.column);
                if (e != Err::kOk) return e;
                if (cur_.type == Tok::kAsc) {
                    Advance();
                } else if (cur_.type == Tok::kDesc) {
                    ob.descending = true;
                    Advance();
                }
                s->order_by.push_back(std::move(ob));
                if (cur_.type != Tok::kComma) break;
                Advance();
            }
        }
        if (cur_.type == Tok::kLimit) {
            Advance();
            e = TakeNonNegativeInt(&s->limit);
            if (e != Err::kOk) return e;
            if (cur_.type == Tok::kOffset) {
                Advance();
                e = TakeNonNegativeInt(&s->offset);
                if (e != Err::kOk) return e;
            }
        }
        return Err::kOk;
    }

    Err TakeNonNegativeInt(int64_t* out) {
        if (cur_.type != Tok::kInteger || cur_.int_val < 0) {
            return FailHere("non-negative integer");
        }
        *out = cur_.int_val;
        Advance();
        return Err::kOk;
    }

    Err ParseInsert(InsertStatement* s) {
        Advance();  // INSERT
        Err e = Err::kOk;
        if (!Expect(Tok::kInto, "INTO", &e)) return e;
        e = TakeIdentifier(&s->table);
        if (e != Err::kOk) return e;
        if (!Expect(Tok::kLParen, "'('", &e)) return e;
        for (;;) {
            std::string col;
            e = TakeIdentifier(&col);
            if (e != Err::kOk) return e;
            for (const std::string& prev : s->columns) {
                if (prev == col) return FailSyntax("duplicate column: " + col);
            }
            s->columns.push_back(std::move(col));
            if (cur_.type != Tok::kComma) break;
            Advance();
        }
        if (!Expect(Tok::kRParen, "')'", &e)) return e;
        if (!Expect(Tok::kValues, "VALUES", &e)) return e;

        for (;;) {
            if (!Expect(Tok::kLParen, "'('", &e)) return e;
            std::vector<ColumnValue> row;
            for (;;) {
                ColumnValue v;
                e = TakeLiteral(&v);
                if (e != Err::kOk) return e;
                row.push_back(std::move(v));
                if (cur_.type == Tok::kComma) {
                    Advance();
                    continue;
                }
                break;
            }
            if (row.size() != s->columns.size()) {
                return FailSyntax("VALUES count mismatch for column list");
            }
            s->rows.push_back(std::move(row));
            if (!Expect(Tok::kRParen, "')'", &e)) return e;
            if (cur_.type != Tok::kComma) break;
            Advance();
        }
        return Err::kOk;
    }

    Err ParseUpdate(UpdateStatement* s) {
        Advance();  // UPDATE
        Err e = TakeIdentifier(&s->table);
        if (e != Err::kOk) return e;
        if (!Expect(Tok::kSet, "SET", &e)) return e;
        for (;;) {
            Assignment a;
            e = TakeIdentifier(&a.column);
            if (e != Err::kOk) return e;
            if (!Expect(Tok::kEq, "'='", &e)) return e;
            e = TakeLiteral(&a.value);
            if (e != Err::kOk) return e;
            for (const Assignment& prev : s->assignments) {
                if (prev.column == a.column) return FailSyntax("duplicate assignment: " + a.column);
            }
            s->assignments.push_back(std::move(a));
            if (cur_.type != Tok::kComma) break;
            Advance();
        }
        if (cur_.type == Tok::kWhere) {
            Advance();
            Condition cond;
            e = ParseExpr(&cond);
            if (e != Err::kOk) return e;
            s->where = std::move(cond);
        }
        return Err::kOk;
    }

    Err ParseDelete(DeleteStatement* s) {
        Advance();  // DELETE
        Err e = Err::kOk;
        if (!Expect(Tok::kFrom, "FROM", &e)) return e;
        e = TakeIdentifier(&s->table);
        if (e != Err::kOk) return e;
        if (cur_.type == Tok::kWhere) {
            Advance();
            Condition cond;
            e = ParseExpr(&cond);
            if (e != Err::kOk) return e;
            s->where = std::move(cond);
        }
        return Err::kOk;
    }

    // ---- WHERE 表达式（AND 优先级低于 OR；NOT 高于两者）----
    Err ParseExpr(Condition* out) { return ParseOr(out); }

    Err ParseOr(Condition* out) {
        Condition left;
        Err e = ParseAnd(&left);
        if (e != Err::kOk) return e;
        if (cur_.type != Tok::kOr) {
            *out = std::move(left);
            return Err::kOk;
        }
        Condition node;
        node.kind = Condition::Kind::kOr;
        node.children.push_back(std::move(left));
        while (cur_.type == Tok::kOr) {
            Advance();
            Condition right;
            e = ParseAnd(&right);
            if (e != Err::kOk) return e;
            node.children.push_back(std::move(right));
        }
        *out = std::move(node);
        return Err::kOk;
    }

    Err ParseAnd(Condition* out) {
        Condition left;
        Err e = ParseNot(&left);
        if (e != Err::kOk) return e;
        if (cur_.type != Tok::kAnd) {
            *out = std::move(left);
            return Err::kOk;
        }
        Condition node;
        node.kind = Condition::Kind::kAnd;
        node.children.push_back(std::move(left));
        while (cur_.type == Tok::kAnd) {
            Advance();
            Condition right;
            e = ParseNot(&right);
            if (e != Err::kOk) return e;
            node.children.push_back(std::move(right));
        }
        *out = std::move(node);
        return Err::kOk;
    }

    Err ParseNot(Condition* out) {
        if (cur_.type == Tok::kNot) {
            Advance();
            Condition child;
            const Err e = ParseNot(&child);
            if (e != Err::kOk) return e;
            Condition node;
            node.kind = Condition::Kind::kNot;
            node.children.push_back(std::move(child));
            *out = std::move(node);
            return Err::kOk;
        }
        return ParsePrimary(out);
    }

    Err ParsePrimary(Condition* out) {
        if (cur_.type == Tok::kLParen) {
            Advance();
            Condition inner;
            Err e = ParseExpr(&inner);
            if (e != Err::kOk) return e;
            if (cur_.type != Tok::kRParen) return FailHere("')'");
            Advance();
            *out = std::move(inner);
            return Err::kOk;
        }
        return ParseOperand(out);
    }

    Err ParseOperand(Condition* out) {
        Condition node;
        Err e = TakeIdentifier(&node.column);
        if (e != Err::kOk) return e;
        if (cur_.type == Tok::kLParen) {
            return FailUnsupported("functions/aggregates not supported");
        }
        if (cur_.type == Tok::kIs) {
            Advance();
            bool negate = false;
            if (cur_.type == Tok::kNot) {
                negate = true;
                Advance();
            }
            if (cur_.type != Tok::kNull) return FailHere("NULL");
            Advance();
            node.kind = negate ? Condition::Kind::kIsNotNull : Condition::Kind::kIsNull;
            *out = std::move(node);
            return Err::kOk;
        }
        switch (cur_.type) {
            case Tok::kEq:
                node.op = CompareOp::kEq;
                break;
            case Tok::kNe:
                node.op = CompareOp::kNe;
                break;
            case Tok::kLt:
                node.op = CompareOp::kLt;
                break;
            case Tok::kLe:
                node.op = CompareOp::kLe;
                break;
            case Tok::kGt:
                node.op = CompareOp::kGt;
                break;
            case Tok::kGe:
                node.op = CompareOp::kGe;
                break;
            case Tok::kUnsupportedKeyword:
                return FailUnsupported("unsupported WHERE construct: " + cur_.text);
            default:
                return FailHere("comparison operator");
        }
        Advance();
        node.kind = Condition::Kind::kCompare;
        e = TakeLiteral(&node.value);
        if (e != Err::kOk) return e;
        *out = std::move(node);
        return Err::kOk;
    }

    Lexer lex_;
    Token cur_;
    std::string err_;
};

}  // namespace

Err Parse(std::string_view sql, Statement* out, std::string* error) {
    if (out == nullptr) return Err::kInvalidArgument;
    Parser p(sql);
    const Err e = p.Run(out);
    if (error != nullptr) *error = e == Err::kOk ? std::string() : p.LastError();
    return e;
}

const char* StatementTypeName(StatementType type) noexcept {
    switch (type) {
        case StatementType::kSelect:
            return "SELECT";
        case StatementType::kInsert:
            return "INSERT";
        case StatementType::kUpdate:
            return "UPDATE";
        case StatementType::kDelete:
            return "DELETE";
    }
    return "UNKNOWN";
}

const char* CompareOpName(CompareOp op) noexcept {
    switch (op) {
        case CompareOp::kEq:
            return "=";
        case CompareOp::kNe:
            return "<>";
        case CompareOp::kLt:
            return "<";
        case CompareOp::kLe:
            return "<=";
        case CompareOp::kGt:
            return ">";
        case CompareOp::kGe:
            return ">=";
    }
    return "?";
}

}  // namespace sql
}  // namespace rtdb
