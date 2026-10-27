#include "core/expression.h"

#include <cmath>

namespace fhmi {

namespace {

class ExpressionParser
{
  public:
    ExpressionParser(const QString &text, double value) : m_text(text), m_value(value) {}

    std::optional<double> run()
    {
        const auto result = sum();
        skipSpace();
        if (!result || m_pos != m_text.size() || !std::isfinite(*result)) {
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

    std::optional<double> sum()
    {
        auto left = product();
        while (left) {
            if (accept("+")) {
                const auto right = product();
                if (!right) {
                    return std::nullopt;
                }
                *left += *right;
            } else if (accept("-")) {
                const auto right = product();
                if (!right) {
                    return std::nullopt;
                }
                *left -= *right;
            } else {
                break;
            }
        }
        return left;
    }

    std::optional<double> product()
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
                *left *= *right;
            } else if (accept("//")) {
                const auto right = unary();
                if (!right || *right == 0) {
                    return std::nullopt;
                }
                *left = std::floor(*left / *right);
            } else if (accept("/")) {
                const auto right = unary();
                if (!right || *right == 0) {
                    return std::nullopt;
                }
                *left /= *right;
            } else if (accept("%")) {
                const auto right = unary();
                if (!right || *right == 0) {
                    return std::nullopt;
                }
                // Python's modulo takes the sign of the divisor.
                *left = *left - *right * std::floor(*left / *right);
            } else {
                break;
            }
        }
        return left;
    }

    std::optional<double> unary()
    {
        if (accept("-")) {
            const auto operand = unary();
            return operand ? std::optional<double>(-*operand) : std::nullopt;
        }
        if (accept("+")) {
            return unary();
        }
        return power();
    }

    std::optional<double> power()
    {
        const auto base = atom();
        if (base && accept("**")) {
            const auto exponent = unary(); // right associative, binds tighter than unary on the left
            if (!exponent) {
                return std::nullopt;
            }
            return std::pow(*base, *exponent);
        }
        return base;
    }

    std::optional<double> atom()
    {
        skipSpace();
        if (accept("(")) {
            const auto inner = sum();
            if (!inner || !accept(")")) {
                return std::nullopt;
            }
            return inner;
        }
        if (m_text.mid(m_pos, 5) == QLatin1String("value")) {
            const int after = m_pos + 5;
            if (after >= m_text.size() || !(m_text.at(after).isLetterOrNumber() || m_text.at(after) == '_')) {
                m_pos = after;
                return m_value;
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
        return ok ? std::optional<double>(number) : std::nullopt;
    }

    const QString &m_text;
    double m_value;
    int m_pos = 0;
};

} // namespace

std::optional<double> Expression::evaluate(const QString &expression, double value)
{
    return ExpressionParser(expression, value).run();
}

} // namespace fhmi
