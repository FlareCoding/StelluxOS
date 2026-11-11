#ifndef STLXDM_SHELL_QUICK_CALC_HPP
#define STLXDM_SHELL_QUICK_CALC_HPP

#include <string>
#include <string_view>

/* The value of `text` as plain arithmetic: decimal numbers, + - * / % ^, parentheses and signs, spaces anywhere.
 * False unless it is a whole expression with an operator between two values, and its value is finite. */
bool calculate(std::string_view text, double* value);

/* `value` as a paste wants it, whole numbers without a fraction and the rest to ten significant digits */
std::string copied_number(double value);

/* `value` as the palette shows it, its copied form with commas between the thousands */
std::string shown_number(double value);

#endif
