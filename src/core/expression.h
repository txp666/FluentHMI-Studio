#pragma once

#include <QtCore/QString>

#include <optional>

namespace fhmi {

// A small arithmetic evaluator for binding transforms such as "value * 0.1" or
// "(value - 32) / 1.8". Supports + - * / % **, unary signs, parentheses, numbers and
// the variable `value`. Anything else (names, calls) is rejected rather than guessed,
// so the binding falls back to the raw value just as the Python designer does on error.
class Expression
{
  public:
    static std::optional<double> evaluate(const QString &expression, double value);
};

} // namespace fhmi
