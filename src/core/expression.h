#pragma once

#include <QtCore/QString>

#include <optional>

namespace fhmi {

// A small evaluator for binding transforms such as "value * 0.1", "(value - 32) / 1.8" or
// "(value >> 1) & 1", following Python's operators and precedence: + - * / // % **, the
// bit operations & | ^ ~ << >> (whole numbers only), comparisons < <= > >= == != (chained
// as in Python), and / or / not, unary signs, parentheses, numbers and the variable `value`.
// Anything else (names, calls) is rejected rather than guessed, so the binding falls back
// to the raw value just as the Python designer does on error.
class Expression
{
  public:
    // `boolean` is set when the result is a comparison's or `not`'s True / False.
    static std::optional<double> evaluate(const QString &expression, double value, bool *boolean = nullptr);
};

} // namespace fhmi
