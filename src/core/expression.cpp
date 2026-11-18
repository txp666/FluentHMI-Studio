#include "core/expression.h"

#include <QtCore/QtNumeric>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>

namespace fhmi {

namespace {

// Python's kinds of number. Error is an exception raised while evaluating: it ends the
// evaluation unless `and`, `or` or a comparison chain is decided without that operand, which
// Python then never evaluates.
enum class Kind
{
    Int,
    Float,
    Bool,
    Unknown,
    Error
};

// An int or a bool (0 / 1) in `integer`, a float in `real`.
struct Value
{
    Kind kind = Kind::Error;
    qint64 integer = 0;
    double real = 0.0;

    bool isError() const { return kind == Kind::Error || kind == Kind::Unknown; }
    bool isInteger() const { return kind == Kind::Int || kind == Kind::Bool; } // an int to Python's operators
    double toReal() const { return kind == Kind::Float ? real : double(integer); }
    bool truthy() const { return kind == Kind::Float ? real != 0.0 : integer != 0; }
};

Value intValue(qint64 n) { return {Kind::Int, n, 0.0}; }

Value boolValue(bool b) { return {Kind::Bool, b ? 1 : 0, 0.0}; }

Value floatValue(double d) { return {Kind::Float, 0, d}; }

// Project conditions aggregate truth, including disconnected inputs. A known fault is
// sufficient for OR; a known false input is sufficient for AND. Calculation errors stay errors.
Value projectLogical(const Value &a, const Value &b, bool isOr)
{
    if (a.kind == Kind::Error)
        return a;
    if (!a.isError() && a.truthy() == isOr)
        return boolValue(isOr);
    if (b.kind == Kind::Error)
        return b;
    if (!b.isError() && b.truthy() == isOr)
        return boolValue(isOr);
    if (a.isError() || b.isError())
        return {Kind::Unknown};
    return boolValue(!isOr);
}

enum class Op
{
    Add,
    Subtract,
    Multiply,
    Divide,
    FloorDivide,
    Modulo,
    Power,
    BitAnd,
    BitOr,
    BitXor,
    ShiftLeft,
    ShiftRight
};

// Python's divmod of two floats (CPython's _float_div_mod): the remainder takes the divisor's
// sign, and the quotient is snapped to the whole number it approximates.
void floatDivMod(double a, double b, double *quotient, double *remainder)
{
    double mod = std::fmod(a, b);
    double div = (a - mod) / b;
    if (mod != 0.0) {
        if ((b < 0) != (mod < 0)) {
            mod += b;
            div -= 1.0;
        }
    } else {
        mod = std::copysign(0.0, b);
    }
    double floorDiv = std::copysign(0.0, a / b);
    if (div != 0.0) {
        floorDiv = std::floor(div);
        if (div - floorDiv > 0.5) {
            floorDiv += 1.0;
        }
    }
    *quotient = floorDiv;
    *remainder = mod;
}

// A float to a float power as Python's float.__pow__ has it: zero to a negative power raises,
// a negative number to a fractional one is complex (which no binding shows), and a finite
// result too large for a float raises OverflowError.
Value floatPower(double base, double exponent)
{
    if (base == 0.0 && exponent < 0.0 && std::isfinite(exponent)) {
        return {};
    }
    if (base < 0.0 && std::isfinite(base) && std::isfinite(exponent) && exponent != std::floor(exponent)) {
        return {};
    }
    const double result = std::pow(base, exponent);
    if (std::isinf(result) && std::isfinite(base) && std::isfinite(exponent)) {
        return {};
    }
    return floatValue(result);
}

// An int to a non-negative int power; nullopt when it outgrows 64 bits.
std::optional<qint64> integerPower(qint64 base, qint64 exponent)
{
    qint64 result = 1;
    while (exponent > 0) {
        if ((exponent & 1) && qMulOverflow(result, base, &result)) {
            return std::nullopt;
        }
        exponent >>= 1;
        // Squaring overflows only when a further factor would make the result overflow too.
        if (exponent > 0 && qMulOverflow(base, base, &base)) {
            return std::nullopt;
        }
    }
    return result;
}

// A binary operator as Python applies it to ints, floats and bools.
Value apply(Op op, const Value &a, const Value &b)
{
    if (a.isError() || b.isError()) {
        return a.kind == Kind::Error || b.kind == Kind::Error ? Value{} : Value{Kind::Unknown};
    }
    const bool integers = a.isInteger() && b.isInteger();
    const qint64 i = a.integer, j = b.integer;
    const double x = a.toReal(), y = b.toReal();
    qint64 n = 0;
    double quotient = 0.0, remainder = 0.0;
    switch (op) {
    case Op::Add:
        if (integers) {
            return qAddOverflow(i, j, &n) ? Value{} : intValue(n);
        }
        return floatValue(x + y);
    case Op::Subtract:
        if (integers) {
            return qSubOverflow(i, j, &n) ? Value{} : intValue(n);
        }
        return floatValue(x - y);
    case Op::Multiply:
        if (integers) {
            return qMulOverflow(i, j, &n) ? Value{} : intValue(n);
        }
        return floatValue(x * y);
    case Op::Divide: // always a float, as in Python 3
        return y == 0.0 ? Value{} : floatValue(x / y);
    case Op::FloorDivide:
        if (integers) {
            if (j == 0 || (i == std::numeric_limits<qint64>::min() && j == -1)) {
                return {};
            }
            const qint64 q = i / j;
            return intValue(i % j != 0 && (i < 0) != (j < 0) ? q - 1 : q); // rounded down, not towards 0
        }
        if (y == 0.0) {
            return {};
        }
        floatDivMod(x, y, &quotient, &remainder);
        return floatValue(quotient);
    case Op::Modulo:
        if (integers) {
            if (j == 0) {
                return {};
            }
            if (j == -1) {
                return intValue(0); // also where i % j itself would overflow
            }
            const qint64 r = i % j;
            return intValue(r != 0 && (r < 0) != (j < 0) ? r + j : r); // the divisor's sign
        }
        if (y == 0.0) {
            return {};
        }
        floatDivMod(x, y, &quotient, &remainder);
        return floatValue(remainder);
    case Op::Power:
        if (integers && j >= 0) {
            const auto power = integerPower(i, j);
            return power ? intValue(*power) : Value{};
        }
        return floatPower(x, y); // an int to a negative power is a float
    case Op::BitAnd:
    case Op::BitOr:
    case Op::BitXor: {
        if (!integers) {
            return {}; // Python's TypeError: bit operations take ints and bools only
        }
        const qint64 bits = op == Op::BitAnd ? i & j : op == Op::BitOr ? i | j : i ^ j;
        return a.kind == Kind::Bool && b.kind == Kind::Bool ? boolValue(bits != 0) : intValue(bits);
    }
    case Op::ShiftLeft:
        if (!integers || j < 0) {
            return {}; // a float, or a negative shift count
        }
        if (i == 0) {
            return intValue(0);
        }
        if (j >= 64) {
            return {};
        }
        n = i;
        for (qint64 step = 0; step < j; ++step) {
            if (qMulOverflow(n, qint64(2), &n)) {
                return {};
            }
        }
        return intValue(n);
    case Op::ShiftRight:
        if (!integers || j < 0) {
            return {};
        }
        return intValue(j >= 63 ? (i < 0 ? -1 : 0) : i >> j); // rounds down, as Python's does
    }
    return {};
}

// -1, 0 or 1 as an int compares with a float, exactly as Python compares them (not after
// rounding the int to a float); 2 when the float is NaN.
int compareIntReal(qint64 i, double d)
{
    if (std::isnan(d)) {
        return 2;
    }
    const double rounded = double(i);
    if (rounded != d) { // rounding keeps the order of distinct numbers
        return rounded < d ? -1 : 1;
    }
    if (d >= 9223372036854775808.0) { // 2^63: i rounded up to a float above every qint64
        return -1;
    }
    const qint64 whole = qint64(d); // d is a whole number within range, the rounding of i
    return (i > whole) - (i < whole);
}

// -1, 0 or 1 by Python's ordering of numbers; 2 when they are unordered (a NaN).
int compare(const Value &a, const Value &b)
{
    if (a.isInteger() && b.isInteger()) {
        return (a.integer > b.integer) - (a.integer < b.integer);
    }
    if (a.isInteger()) {
        return compareIntReal(a.integer, b.real);
    }
    if (b.isInteger()) {
        const int order = compareIntReal(b.integer, a.real);
        return order == 2 ? 2 : -order;
    }
    if (std::isnan(a.real) || std::isnan(b.real)) {
        return 2;
    }
    return (a.real > b.real) - (a.real < b.real);
}

bool holds(QLatin1String op, int order)
{
    if (op == QLatin1String("!=")) {
        return order != 0; // NaN is unequal to everything
    }
    if (op == QLatin1String("==")) {
        return order == 0;
    }
    if (op == QLatin1String("<")) {
        return order == -1;
    }
    if (op == QLatin1String("<=")) {
        return order == -1 || order == 0;
    }
    if (op == QLatin1String(">")) {
        return order == 1;
    }
    return order == 1 || order == 0; // >=
}

bool isNameChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

bool isDecimal(QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('9'); }

bool isBinary(QChar c) { return c == QLatin1Char('0') || c == QLatin1Char('1'); }

bool isOctal(QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('7'); }

bool isHex(QChar c)
{
    return isDecimal(c) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
           (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
}

Value fromVariant(const QVariant &value)
{
    Value bound;
    switch (value.typeId()) {
    case QMetaType::Bool:
        bound = boolValue(value.toBool());
        break;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
        bound = intValue(value.toLongLong());
        break;
    case QMetaType::ULongLong:
        if (value.toULongLong() > quint64(std::numeric_limits<qint64>::max())) {
            return {};
        }
        bound = intValue(qint64(value.toULongLong()));
        break;
    case QMetaType::Double:
    case QMetaType::Float:
        bound = floatValue(value.toDouble());
        break;
    default:
        return {};
    }
    return bound;
}

struct ReferenceSpan
{
    qsizetype start;
    qsizetype length;
    QString name;
    QChar quote;
};

// Python's grammar, lowest precedence first: or, and, not, comparison, |, ^, &, shifts,
// + -, * / // %, unary + - ~, **, atom. Parsing evaluates as it goes; a syntax error is
// nullopt, an error at run time a Value of kind Error.
class ExpressionParser
{
  public:
    ExpressionParser(const QString &text, Value value, const Expression::Resolver &resolve = {},
                     QStringList *refs = nullptr, bool *usesValue = nullptr,
                     QList<ReferenceSpan> *spans = nullptr)
        : m_text(text), m_value(value), m_resolve(resolve), m_refs(refs), m_usesValue(usesValue), m_spans(spans)
    {
    }

    std::optional<Value> run()
    {
        const auto result = orExpr();
        skipSpace();
        if (!result || m_pos != m_text.size()) {
            return std::nullopt;
        }
        return result;
    }

  private:
    struct Token
    {
        const char *text;
        Op op;
    };

    using Level = std::optional<Value> (ExpressionParser::*)();

    QChar at(qsizetype pos) const { return pos < m_text.size() ? m_text.at(pos) : QChar(); }

    void skipSpace()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace()) {
            ++m_pos;
        }
    }

    bool accept(const char *token)
    {
        skipSpace();
        const QLatin1String t(token);
        if (QStringView(m_text).mid(m_pos).startsWith(t)) {
            m_pos += t.size();
            return true;
        }
        return false;
    }

    // A keyword or name such as `and`, which must not be the start of a longer name.
    bool acceptWord(const char *word)
    {
        skipSpace();
        const QLatin1String w(word);
        if (!QStringView(m_text).mid(m_pos).startsWith(w) || isNameChar(at(m_pos + w.size()))) {
            return false;
        }
        m_pos += w.size();
        return true;
    }

    std::optional<Value> orExpr()
    {
        auto left = andExpr();
        while (left && acceptWord("or")) {
            const auto right = andExpr();
            if (!right) {
                return std::nullopt;
            }
            if (m_resolve) {
                left = projectLogical(*left, *right, true);
            } else if (!left->isError() && !left->truthy()) { // the first true operand, else the last one
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
            if (m_resolve) {
                left = projectLogical(*left, *right, false);
            } else if (!left->isError() && left->truthy()) { // the first false operand, else the last one
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
            return operand->isError() ? *operand : boolValue(!operand->truthy());
        }
        return comparison();
    }

    // `0 < value < 40` chains as Python does: every neighbouring pair must hold, and once one
    // does not (or raises) the operands after it are not evaluated.
    std::optional<Value> comparison()
    {
        const auto left = bitOr();
        if (!left) {
            return std::nullopt;
        }
        Value previous = *left;
        std::optional<Value> decided; // False, or an error
        bool compared = false;
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
            compared = true;
            if (decided) {
                continue;
            }
            if (previous.isError() || right->isError()) {
                decided = previous.kind == Kind::Error || right->kind == Kind::Error ? Value{}
                                                                                    : Value{Kind::Unknown};
            } else if (!holds(QLatin1String(op), compare(previous, *right))) {
                decided = boolValue(false);
            }
            previous = *right;
        }
        if (!compared) {
            return left;
        }
        return decided ? *decided : boolValue(true);
    }

    // One level of left-associative binary operators.
    std::optional<Value> binaryLevel(Level operand, std::initializer_list<Token> tokens)
    {
        auto left = (this->*operand)();
        while (left) {
            const Token *match = nullptr;
            for (const Token &token : tokens) {
                if (accept(token.text)) {
                    match = &token;
                    break;
                }
            }
            if (!match) {
                break;
            }
            const auto right = (this->*operand)();
            if (!right) {
                return std::nullopt;
            }
            left = apply(match->op, *left, *right);
        }
        return left;
    }

    std::optional<Value> bitOr() { return binaryLevel(&ExpressionParser::bitXor, {{"|", Op::BitOr}}); }
    std::optional<Value> bitXor() { return binaryLevel(&ExpressionParser::bitAnd, {{"^", Op::BitXor}}); }
    std::optional<Value> bitAnd() { return binaryLevel(&ExpressionParser::shift, {{"&", Op::BitAnd}}); }
    std::optional<Value> shift()
    {
        return binaryLevel(&ExpressionParser::sum, {{"<<", Op::ShiftLeft}, {">>", Op::ShiftRight}});
    }
    std::optional<Value> sum()
    {
        return binaryLevel(&ExpressionParser::product, {{"+", Op::Add}, {"-", Op::Subtract}});
    }
    std::optional<Value> product()
    {
        // "**" never reaches this level: power() takes it right after its operand.
        return binaryLevel(&ExpressionParser::unary,
                           {{"*", Op::Multiply}, {"//", Op::FloorDivide}, {"/", Op::Divide}, {"%", Op::Modulo}});
    }

    std::optional<Value> unary()
    {
        for (const char *sign : {"-", "+", "~"}) {
            if (!accept(sign)) {
                continue;
            }
            const auto operand = unary();
            if (!operand) {
                return std::nullopt;
            }
            const Value &v = *operand;
            if (v.isError()) {
                return v;
            }
            if (*sign == '~') { // ints and bools only; ~x is -x - 1
                return v.isInteger() ? intValue(~v.integer) : Value{};
            }
            if (v.kind == Kind::Float) {
                return floatValue(*sign == '-' ? -v.real : v.real);
            }
            if (*sign == '+') {
                return intValue(v.integer); // a bool becomes an int
            }
            return v.integer == std::numeric_limits<qint64>::min() ? Value{} : intValue(-v.integer);
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
            return apply(Op::Power, *base, *exponent);
        }
        return base;
    }

    std::optional<Value> atom()
    {
        if (accept("(")) {
            const auto inner = orExpr();
            if (!inner || !accept(")")) {
                return std::nullopt;
            }
            return inner;
        }
        if (acceptWord("T")) {
            if (!accept("["))
                return std::nullopt;
            skipSpace();
            const QChar quote = at(m_pos++);
            if (quote != QLatin1Char('"') && quote != QLatin1Char('\''))
                return std::nullopt;
            const qsizetype start = m_pos;
            QString name;
            while (!at(m_pos).isNull() && at(m_pos) != quote) {
                QChar ch = at(m_pos++);
                if (ch == QLatin1Char('\\')) {
                    ch = at(m_pos++);
                    if (ch != quote && ch != QLatin1Char('\\'))
                        return std::nullopt;
                }
                name.append(ch);
            }
            const qsizetype length = m_pos - start;
            if (at(m_pos++) != quote || name.isEmpty() || !accept("]"))
                return std::nullopt;
            if (m_spans)
                m_spans->append({start, length, name, quote});
            if (m_refs && !m_refs->contains(name))
                m_refs->append(name);
            if (!m_resolve)
                return Value{};
            const QVariant value = m_resolve(name);
            return value.isValid() && !value.isNull() ? fromVariant(value) : Value{Kind::Unknown};
        }
        if (acceptWord("value")) {
            if (m_usesValue)
                *m_usesValue = true;
            return m_value;
        }
        if (acceptWord("True")) {
            return boolValue(true);
        }
        if (acceptWord("False")) {
            return boolValue(false);
        }
        return number();
    }

    // Digits `isDigit` accepts, with single underscores between them as Python allows; the
    // digits go to `out` without the underscores. False when there is not one digit.
    bool digits(bool (*isDigit)(QChar), QString *out)
    {
        const qsizetype start = m_pos;
        while (isDigit(at(m_pos))) {
            out->append(at(m_pos++));
            if (at(m_pos) == QLatin1Char('_') && isDigit(at(m_pos + 1))) {
                ++m_pos;
            }
        }
        return m_pos > start;
    }

    // A literal as Python's tokenizer reads it: 0x / 0o / 0b ints, decimal ints (no leading
    // zeros), and floats with a '.' or an exponent. An int too large for 64 bits is valid
    // Python but cannot be evaluated here.
    std::optional<Value> number()
    {
        skipSpace();
        QString text;
        const QChar prefix = at(m_pos + 1).toLower();
        if (at(m_pos) == QLatin1Char('0') &&
            (prefix == QLatin1Char('x') || prefix == QLatin1Char('o') || prefix == QLatin1Char('b'))) {
            m_pos += 2;
            const int base = prefix == QLatin1Char('x') ? 16 : prefix == QLatin1Char('o') ? 8 : 2;
            const auto isDigit = base == 16 ? &isHex : base == 8 ? &isOctal : &isBinary;
            if (at(m_pos) == QLatin1Char('_') && isDigit(at(m_pos + 1))) {
                ++m_pos; // 0x_FF
            }
            if (!digits(isDigit, &text) || isNameChar(at(m_pos))) {
                return std::nullopt; // no digits, or one the base does not have
            }
            bool ok = false;
            const qint64 n = text.toLongLong(&ok, base);
            return ok ? intValue(n) : Value{};
        }
        const bool whole = digits(&isDecimal, &text);
        bool real = false;
        if (at(m_pos) == QLatin1Char('.')) {
            ++m_pos;
            text.append(QLatin1Char('.'));
            const bool fraction = digits(&isDecimal, &text);
            if (!whole && !fraction) {
                return std::nullopt;
            }
            real = true;
        } else if (!whole) {
            return std::nullopt;
        }
        if (at(m_pos) == QLatin1Char('e') || at(m_pos) == QLatin1Char('E')) {
            qsizetype probe = m_pos + 1;
            const bool signedExponent = at(probe) == QLatin1Char('+') || at(probe) == QLatin1Char('-');
            if (signedExponent) {
                ++probe;
            }
            if (isDecimal(at(probe))) {
                text.append(QLatin1Char('e'));
                if (signedExponent) {
                    text.append(at(probe - 1));
                }
                m_pos = probe;
                digits(&isDecimal, &text);
                real = true;
            }
        }
        if (at(m_pos) == QLatin1Char('j') || at(m_pos) == QLatin1Char('J')) {
            return std::nullopt; // an imaginary literal: complex numbers are out of scope
        }
        if (real) {
            // The text is a valid float, so the only failures are overflow (inf, as Python's
            // float literal gives) and underflow (0.0, likewise).
            return floatValue(text.toDouble());
        }
        if (text.size() > 1 && text.startsWith(QLatin1Char('0')) && text.count(QLatin1Char('0')) != text.size()) {
            return std::nullopt; // Python allows no leading zeros in a decimal int: 07 is an error
        }
        bool ok = false;
        const qint64 n = text.toLongLong(&ok);
        return ok ? intValue(n) : Value{};
    }

    const QString &m_text;
    Value m_value;
    Expression::Resolver m_resolve;
    QStringList *m_refs;
    bool *m_usesValue;
    QList<ReferenceSpan> *m_spans;
    qsizetype m_pos = 0;
};

} // namespace

QStringList Expression::references(const QString &expression, bool *valid, bool *usesValue)
{
    QStringList refs;
    if (usesValue)
        *usesValue = false;
    const auto parsed = ExpressionParser(expression, {}, {}, &refs, usesValue).run();
    if (valid)
        *valid = parsed.has_value();
    return refs;
}

QString Expression::renameReference(const QString &expression, const QString &previous, const QString &current)
{
    QList<ReferenceSpan> spans;
    if (!ExpressionParser(expression, {}, {}, nullptr, nullptr, &spans).run())
        return expression;
    QString renamed = expression;
    for (auto it = spans.crbegin(); it != spans.crend(); ++it) {
        if (it->name != previous)
            continue;
        QString escaped = current;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(it->quote, QString(QLatin1Char('\\')) + it->quote);
        renamed.replace(it->start, it->length, escaped);
    }
    return renamed;
}

QVariant Expression::evaluate(const QString &expression, const QVariant &value, const Resolver &resolve)
{
    if (!resolve && fromVariant(value).isError())
        return {};
    const auto result = ExpressionParser(expression, fromVariant(value), resolve).run();
    if (!result) {
        return {};
    }
    switch (result->kind) {
    case Kind::Bool:
        return result->integer != 0;
    case Kind::Int:
        return qlonglong(result->integer);
    case Kind::Float:
        return std::isfinite(result->real) ? QVariant(result->real) : QVariant();
    case Kind::Error:
    case Kind::Unknown:
        break;
    }
    return {};
}

} // namespace fhmi
