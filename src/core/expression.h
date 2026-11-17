#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <functional>

namespace fhmi {

// A small evaluator for binding transforms such as "value * 0.1", "(value - 32) / 1.8" or
// "(value >> 1) & 1", following Python's eval: the operators + - * / // % **, the bit
// operations & | ^ ~ << >>, comparisons < <= > >= == != (chained as in Python), and / or /
// not, unary signs and parentheses, with Python's precedence. Values are ints, floats and
// bools as in Python: a literal is an int unless it has a '.' or an exponent (0x, 0o and 0b
// literals are ints), True and False are bools, and each operator gives the kind Python's
// gives, so 7 // 2 is 3, 7 / 2 is 3.5 and 3.0 & 1 is an error. Anything else (names, calls)
// is rejected rather than guessed, so the binding falls back to the raw value just as the
// Python designer does when eval raises.
class Expression
{
  public:
    // `expression` with `value` (a bool, an integer or a double) as the variable `value`: a
    // bool, a qlonglong or a double, as eval gives a bool, an int or a float. Invalid where
    // eval raises, where an int outgrows 64 bits and where a float is infinite or NaN.
    // With a project Resolver, and/or aggregate booleans: true OR unknown is true,
    // false AND unknown is false; an undecidable result stays invalid. Plain value
    // transforms retain Python's operand-returning and/or behavior.
    using Resolver = std::function<QVariant(const QString &)>;
    static QVariant evaluate(const QString &expression, const QVariant &value, const Resolver &resolve = {});
    // T["name"] refers to a project data point; parsing never executes arbitrary code.
    static QStringList references(const QString &expression, bool *valid = nullptr, bool *usesValue = nullptr);
    // Rewrites only parsed T["name"] references, preserving whitespace and quote style.
    static QString renameReference(const QString &expression, const QString &previous, const QString &current);
};

} // namespace fhmi
