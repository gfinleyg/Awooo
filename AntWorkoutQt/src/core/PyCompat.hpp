#pragma once

// Python's own parsing, rounding and formatting, wherever the Python app's results depend on them: workout files,
// the settings file and the numbers shown read and come out the same in both apps (PLAN.md D10).

#include <QString>
#include <QStringView>

#include <optional>

namespace awo::py {

// str.isdigit() for the text these files hold: not empty, every character 0-9 (Python also takes other scripts'
// digits, which int() then reads; no workout file has them)
bool isDigits(QStringView text);

// int(text): surrounding whitespace, an optional sign, digits with single underscores between them. Empty when it
// isn't one, or doesn't fit in 64 bits
std::optional<long long> toInt(QStringView text);

// float(text): as int() plus a decimal point and exponent, and inf/infinity/nan in any case
std::optional<double> toFloat(QStringView text);

// round(value): the nearest whole number, halves to the even one
double round(double value);

// round(value, digits): the double nearest the correctly rounded decimal, as Python works it out
double round(double value, int digits);

// f"{value:.{digits}f}"
QString fixed(double value, int digits);

// f"{value:g}"
QString formatG(double value);

// a % b for floats: the result takes b's sign
double mod(double a, double b);

}  // namespace awo::py
