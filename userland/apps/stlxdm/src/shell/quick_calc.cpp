#include "quick_calc.hpp"

#include <cmath>
#include <cstdio>

/* Whole numbers past this lose their last digits in a double, so they show in exponent form */
constexpr double WHOLE_LIMIT = 1e15;

namespace {

/* Recursive descent, loosest first: sums of products of signed powers of numbers or parenthesized sums, a power's
 * exponent signed in turn, so -2^2 is -4 and 2^3^2 is 512, as written math reads them */
class arithmetic {
public:
    explicit arithmetic(std::string_view text) : m_text(text) {}

    bool parse(double* value) {
        bool parsed = sum(value);
        skip_spaces();

        return parsed && m_at == m_text.size() && m_operators > 0 && std::isfinite(*value);
    }

private:
    void skip_spaces() {
        while (m_at < m_text.size() && m_text[m_at] == ' ') {
            m_at++;
        }
    }

    bool take(char c) {
        skip_spaces();
        if (m_at < m_text.size() && m_text[m_at] == c) {
            m_at++;
            return true;
        }

        return false;
    }

    bool sum(double* value) {
        if (!product(value)) {
            return false;
        }

        for (;;) {
            bool is_add = take('+');
            if (!is_add && !take('-')) {
                return true;
            }

            double right = 0;
            if (!product(&right)) {
                return false;
            }

            *value = is_add ? *value + right : *value - right;
            m_operators++;
        }
    }

    bool product(double* value) {
        if (!signed_value(value)) {
            return false;
        }

        for (;;) {
            char op = take('*') ? '*' : take('/') ? '/' : take('%') ? '%' : 0;
            if (op == 0) {
                return true;
            }

            double right = 0;
            if (!signed_value(&right)) {
                return false;
            }

            *value = op == '*' ? *value * right : op == '/' ? *value / right : std::fmod(*value, right);
            m_operators++;
        }
    }

    bool signed_value(double* value) {
        if (take('-')) {
            bool parsed = signed_value(value);
            *value = -*value;
            return parsed;
        }

        if (take('+')) {
            return signed_value(value);
        }

        return power(value);
    }

    bool power(double* value) {
        if (!primary(value)) {
            return false;
        }

        if (!take('^')) {
            return true;
        }

        double exponent = 0;
        if (!signed_value(&exponent)) {
            return false;
        }

        *value = std::pow(*value, exponent);
        m_operators++;

        return true;
    }

    bool primary(double* value) {
        if (take('(')) {
            return sum(value) && take(')');
        }

        skip_spaces();
        return number(value);
    }

    /* Digits with an optional fraction, either side of the point allowed to be empty but not both */
    bool number(double* value) {
        int digits = 0;
        double whole = 0;
        while (m_at < m_text.size() && is_digit(m_text[m_at])) {
            whole = whole * 10 + (m_text[m_at] - '0');
            m_at++;
            digits++;
        }

        double fraction = 0;
        double scale = 1;
        if (m_at < m_text.size() && m_text[m_at] == '.') {
            m_at++;
            while (m_at < m_text.size() && is_digit(m_text[m_at])) {
                fraction = fraction * 10 + (m_text[m_at] - '0');
                scale *= 10;
                m_at++;
                digits++;
            }
        }

        *value = whole + fraction / scale;
        return digits > 0;
    }

    static bool is_digit(char c) {
        return c >= '0' && c <= '9';
    }

    std::string_view m_text;
    size_t           m_at = 0;
    int              m_operators = 0;
};

} // namespace

bool calculate(std::string_view text, double* value) {
    *value = 0;
    return arithmetic(text).parse(value);
}

std::string copied_number(double value) {
    double shown = value == 0 ? 0 : value;
    bool is_whole = shown == std::floor(shown) && std::fabs(shown) < WHOLE_LIMIT;

    char text[64];
    snprintf(text, sizeof(text), is_whole ? "%.0f" : "%.10g", shown);

    return text;
}

std::string shown_number(double value) {
    std::string text = copied_number(value);
    if (text.find('e') != std::string::npos) {
        return text;
    }

    size_t digits_start = text[0] == '-' ? 1 : 0;
    size_t digits_end = text.find('.');
    if (digits_end == std::string::npos) {
        digits_end = text.size();
    }

    for (size_t at = digits_end; at > digits_start + 3; at -= 3) {
        text.insert(at - 3, ",");
    }

    return text;
}
