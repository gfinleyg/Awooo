#include "PyCompat.hpp"

#include <charconv>
#include <cmath>
#include <string>

namespace awo::py {

namespace {

bool isAsciiDigit(QChar c) {
    return c >= u'0' && c <= u'9';
}

// The number's characters with Python's underscores taken out (each has to sit between two digits), as ASCII;
// empty when they aren't where Python allows them or there's anything but ASCII
std::optional<std::string> withoutUnderscores(QStringView text) {
    std::string out;
    out.reserve(static_cast<std::size_t>(text.size()));
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c.unicode() > 0x7F) {
            return std::nullopt;
        }
        if (c == u'_') {
            if (i == 0 || i + 1 >= text.size() || !isAsciiDigit(text[i - 1]) || !isAsciiDigit(text[i + 1])) {
                return std::nullopt;
            }
            continue;
        }
        out.push_back(static_cast<char>(c.unicode()));
    }
    return out;
}

}  // namespace

bool isDigits(QStringView text) {
    if (text.isEmpty()) {
        return false;
    }
    for (QChar c : text) {
        if (!isAsciiDigit(c)) {
            return false;
        }
    }
    return true;
}

std::optional<long long> toInt(QStringView text) {
    text = text.trimmed();
    bool negative = false;
    if (!text.isEmpty() && (text[0] == u'+' || text[0] == u'-')) {
        negative = text[0] == u'-';
        text = text.mid(1);
    }
    if (text.isEmpty() || !isAsciiDigit(text[0])) {
        return std::nullopt;
    }
    const std::optional<std::string> digits = withoutUnderscores(text);
    if (!digits) {
        return std::nullopt;
    }
    for (char c : *digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    unsigned long long magnitude = 0;
    const auto [end, error] = std::from_chars(digits->data(), digits->data() + digits->size(), magnitude);
    if (error != std::errc() || end != digits->data() + digits->size()) {
        return std::nullopt;
    }
    if (negative) {
        if (magnitude > 9223372036854775808ull) {
            return std::nullopt;
        }
        return magnitude == 9223372036854775808ull ? (-9223372036854775807ll - 1) : -static_cast<long long>(magnitude);
    }
    if (magnitude > 9223372036854775807ull) {
        return std::nullopt;
    }
    return static_cast<long long>(magnitude);
}

std::optional<double> toFloat(QStringView text) {
    text = text.trimmed();
    bool negative = false;
    if (!text.isEmpty() && (text[0] == u'+' || text[0] == u'-')) {
        negative = text[0] == u'-';
        text = text.mid(1);
    }
    if (text.isEmpty() || text[0] == u'+' || text[0] == u'-') {
        return std::nullopt;
    }
    const QString lower = text.toString().toLower();
    if (lower == u"inf" || lower == u"infinity") {
        return negative ? -HUGE_VAL : HUGE_VAL;
    }
    if (lower == u"nan") {
        return std::nan("");
    }
    // only digits, a point and an exponent from here (from_chars would also read inf/nan spellings Python doesn't)
    for (QChar c : text) {
        if (!isAsciiDigit(c) && c != u'.' && c != u'e' && c != u'E' && c != u'+' && c != u'-' && c != u'_') {
            return std::nullopt;
        }
    }
    const std::optional<std::string> number = withoutUnderscores(text);
    if (!number || number->empty()) {
        return std::nullopt;
    }
    double value = 0;
    const auto [end, error] = std::from_chars(number->data(), number->data() + number->size(), value);
    // (out of range: Python gives inf for a huge one and 0 for a tiny one, as from_chars' value does)
    if ((error != std::errc() && error != std::errc::result_out_of_range) || end != number->data() + number->size()) {
        return std::nullopt;
    }
    if (error == std::errc::result_out_of_range) {
        value = std::fabs(value) < 1 ? 0.0 : HUGE_VAL;
    }
    return negative ? -value : value;
}

double round(double value) {
    // nearbyint rounds in the current rounding mode, which is to-nearest-even unless something changed it
    return std::nearbyint(value);
}

double round(double value, int digits) {
    if (!std::isfinite(value)) {
        return value;
    }
    return fixed(value, digits).toDouble();
}

QString fixed(double value, int digits) {
    // to_chars works from the exact binary value and breaks exact ties to even, as Python's formatting does
    char buffer[400];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, digits);
    if (error != std::errc()) {
        return QString::number(value, 'f', digits);
    }
    return QString::fromLatin1(buffer, end - buffer);
}

QString formatG(double value) {
    char buffer[64];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::general, 6);
    if (error != std::errc()) {
        return QString::number(value, 'g', 6);
    }
    return QString::fromLatin1(buffer, end - buffer);
}

double mod(double a, double b) {
    double result = std::fmod(a, b);
    if (result != 0) {
        if ((b < 0) != (result < 0)) {
            result += b;
        }
    } else {
        result = std::copysign(0.0, b);
    }
    return result;
}

}  // namespace awo::py
