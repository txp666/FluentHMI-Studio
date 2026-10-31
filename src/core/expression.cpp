#include "core/expression.h"

#include <cmath>
#include <cstdint>

namespace fhmi {

namespace {

// A parsed value. `boolean` marks the result of a comparison or `not`, which Python reports as
// True / False rather than 1 / 0; arithmetic on it gives a plain number again, as in Python.
struct Value
{
    double number = 0.0;
    bool boolean = false;
};

// Python's grammar, lowest precedence first: or, and, not, comparison, |, ^, &, shifts,
// + -, * / // %, unary + - ~, **, atom. Bit operations need whole numbers, like Python's ints.
class ExpressionParser
{
  public:
    ExpressionParser(const QString &text, double value) : m_text(text), m_value(value) {}

    std::optional<Value> run()
    {
        const auto result = orExpr();
        skipSpace();
        if (!result || m_pos != m_text.size() || !std::isfinite(result->number)) {
            return std::nullopt;
        }
        return result;
    }

  private:
    void skipSpace()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace()) {
            ++m_pos;
        }
    }

    bool accept(const char *token)
    {
        skipSpace();
        const QString t = QString::fromLatin1(token);
        if (m_text.mid(m_pos, t.size()) == t) {
            m_pos += t.size();
            return true;
        }
        return false;
    }

    // A keyword such as `and`, which must not be the start of a longer name.
    bool acceptWord(const char *word)
    {
        skipSpace();
        const QString w = QString::fromLatin1(word);
        const int after = m_pos + w.size();
        if (m_text.mid(m_pos, w.size()) != w ||
            (after < m_text.size() && (m_text.at(after).isLetterOrNumber() || m_text.at(after) == '_'))) {
            return false;
        }
        m_pos = after;
        return true;
    }

    static bool truthy(const Value &v) { return v.number != 0.0; }

    // Python's ints: a whole number that a double holds exactly.
    static std::optional<int64_t> integer(const Value &v)
    {
        if (v.number != std::floor(v.number) || std::fabs(v.number) >= 9007199254740992.0) {
            return std::nullopt;
        }
        return int64_t(v.number);
    }

    std::optional<Value> orExpr()
    {
        auto left = andExpr();
        while (left && acceptWord("or")) {
            const auto right = andExpr();
            if (!right) {
                return std::nullopt;
            }
            if (!truthy(*left)) { // the first true operand, else the last one
                left = right;
            }
        }
        return left;
    }

    std::optional<Value> andExpr()
    {
        auto left = notExpr();
        while (left && acceptWord("and")) {
            const auto right = notExpr();
            if (!right) {
                return std::nullopt;
            }
            if (truthy(*left)) { // the first false operand, else the last one
                left = right;
            }
        }
        return left;
    }

    std::optional<Value> notExpr()
    {
        if (acceptWord("not")) {
            const auto operand = notExpr();
            if (!operand) {
                return std::nullopt;
            }
            return Value{truthy(*operand) ? 0.0 : 1.0, true};
        }
        return comparison();
    }

    // `0 < value < 40` chains as Python does: every neighbouring pair must hold.
    std::optional<Value> comparison()
    {
        auto left = bitOr();
        if (!left) {
            return std::nullopt;
        }
        bool compared = false;
        bool holds = true;
        Value previous = *left;
        while (true) {
            const char *op = nullptr;
            for (const char *candidate : {"<=", ">=", "==", "!=", "<", ">"}) {
                if (accept(candidate)) {
                    op = candidate;
                    break;
                }
            }
            if (!op) {
                break;
            }
            const auto right = bitOr();
            if (!right) {
                return std::nullopt;
            }
            const double a = previous.number, b = right->number;
            const QString name = QString::fromLatin1(op);
            holds = holds && (name == "<=" ? a <= b : name == ">=" ? a >= b : name == "==" ? a == b
                            : name == "!=" ? a != b : name == "<" ? a < b : a > b);
            previous = *right;
            compared = true;
        }
        if (compared) {
            return Value{holds ? 1.0 : 0.0, true};
        }
        return left;
    }

    using IntegerOp = int64_t (*)(int64_t, int64_t);

    // One level of left-associative binary operators that work on whole numbers.
    std::optional<Value> integerLevel(std::optional<Value> (ExpressionParser::*operand)(), const char *token,
                                      IntegerOp apply, const char *otherToken = nullptr, IntegerOp otherApply = nullptr)
    {
        auto left = (this->*operand)();
        while (left) {
            IntegerOp op = nullptr;
            if (accept(token)) {
                op = apply;
            } else if (otherToken && accept(otherToken)) {
                op = otherApply;
            } else {
                break;
            }
            const auto right = (this->*operand)();
            const auto a = integer(*left);
            const auto b = right ? integer(*right) : std::nullopt;
            if (!a || !b) {
                return std::nullopt; // Python's bit operations take ints only
            }
            if ((op == shiftLeft || op == shiftRight) && (*b < 0 || *b > 62)) {
                return std::nullopt; // a negative shift is an error; a huge one overflows
            }
            left = Value{double(op(*a, *b)), false};
        }
        return left;
    }

    static int64_t bitOrOp(int64_t a, int64_t b) { return a | b; }
    static int64_t bitXorOp(int64_t a, int64_t b) { return a ^ b; }
    static int64_t bitAndOp(int64_t a, int64_t b) { return a & b; }
    static int64_t shiftLeft(int64_t a, int64_t b) { return int64_t(uint64_t(a) << b); }
    static int64_t shiftRight(int64_t a, int64_t b) { return a >> b; }

    std::optional<Value> bitOr() { return integerLevel(&ExpressionParser::bitXor, "|", bitOrOp); }
    std::optional<Value> bitXor() { return integerLevel(&ExpressionParser::bitAnd, "^", bitXorOp); }
    std::optional<Value> bitAnd() { return integerLevel(&ExpressionParser::shift, "&", bitAndOp); }
    std::optional<Value> shift()
    {
        return integerLevel(&ExpressionParser::sum, "<<", shiftLeft, ">>", shiftRight);
    }

    std::optional<Value> sum()
    {
        auto left = product();
        while (left) {
            if (accept("+")) {
                const auto right = product();
                if (!right) {
                    return std::nullopt;
                }
                left = Value{left->number + right->number, false};
            } else if (accept("-")) {
                const auto right = product();
                if (!right) {
                    return std::nullopt;
                }
                left = Value{left->number - right->number, false};
            } else {
                break;
            }
        }
        return left;
    }

    std::optional<Value> product()
    {
        auto left = unary();
        while (left) {
            skipSpace();
            // "**" is a power, not two multiplications.
            if (m_text.mid(m_pos, 2) == QLatin1String("**")) {
                break;
            }
            if (accept("*")) {
                const auto right = unary();
                if (!right) {
                    return std::nullopt;
                }
                left = Value{left->number * right->number, false};
            } else if (accept("//")) {
                const auto right = unary();
                if (!right || right->number == 0) {
                    return std::nullopt;
                }
                left = Value{std::floor(left->number / right->number), false};
            } else if (accept("/")) {
                const auto right = unary();
                if (!right || right->number == 0) {
                    return std::nullopt;
                }
                left = Value{left->number / right->number, false};
            } else if (accept("%")) {
                const auto right = unary();
                if (!right || right->number == 0) {
                    return std::nullopt;
                }
                // Python's modulo takes the sign of the divisor.
                const double a = left->number, b = right->number;
                left = Value{a - b * std::floor(a / b), false};
            } else {
                break;
            }
        }
        return left;
    }

    std::optional<Value> unary()
    {
        if (accept("-")) {
            const auto operand = unary();
            return operand ? std::optional<Value>(Value{-operand->number, false}) : std::nullopt;
        }
        if (accept("+")) {
            const auto operand = unary();
            return operand ? std::optional<Value>(Value{operand->number, false}) : std::nullopt;
        }
        if (accept("~")) {
            const auto operand = unary();
            const auto whole = operand ? integer(*operand) : std::nullopt;
            return whole ? std::optional<Value>(Value{double(~*whole), false}) : std::nullopt;
        }
        return power();
    }

    std::optional<Value> power()
    {
        const auto base = atom();
        if (base && accept("**")) {
            const auto exponent = unary(); // right associative, binds tighter than unary on the left
            if (!exponent) {
                return std::nullopt;
            }
            return Value{std::pow(base->number, exponent->number), false};
        }
        return base;
    }

    std::optional<Value> atom()
    {
        skipSpace();
        if (accept("(")) {
            const auto inner = orExpr();
            if (!inner || !accept(")")) {
                return std::nullopt;
            }
            return inner;
        }
        if (m_text.mid(m_pos, 5) == QLatin1String("value")) {
            const int after = m_pos + 5;
            if (after >= m_text.size() || !(m_text.at(after).isLetterOrNumber() || m_text.at(after) == '_')) {
                m_pos = after;
                return Value{m_value, false};
            }
            return std::nullopt;
        }
        const int start = m_pos;
        while (m_pos < m_text.size() && (m_text.at(m_pos).isDigit() || m_text.at(m_pos) == '.')) {
            ++m_pos;
        }
        if (m_pos < m_text.size() && (m_text.at(m_pos) == 'e' || m_text.at(m_pos) == 'E')) {
            int probe = m_pos + 1;
            if (probe < m_text.size() && (m_text.at(probe) == '+' || m_text.at(probe) == '-')) {
                ++probe;
            }
            if (probe < m_text.size() && m_text.at(probe).isDigit()) {
                m_pos = probe;
                while (m_pos < m_text.size() && m_text.at(m_pos).isDigit()) {
                    ++m_pos;
                }
            }
        }
        if (m_pos == start) {
            return std::nullopt;
        }
        bool ok = false;
        const double number = m_text.mid(start, m_pos - start).toDouble(&ok);
        return ok ? std::optional<Value>(Value{number, false}) : std::nullopt;
    }

    const QString &m_text;
    double m_value;
    int m_pos = 0;
};

} // namespace

std::optional<double> Expression::evaluate(const QString &expression, double value, bool *boolean)
{
    const auto result = ExpressionParser(expression, value).run();
    if (boolean) {
        *boolean = result && result->boolean;
    }
    return result ? std::optional<double>(result->number) : std::nullopt;
}

} // namespace fhmi
