#include "core/util/coord_text.hpp"

namespace mc {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

// Whitespace, and at most one comma. Two commas in a row means a field was left
// out -- "1,,2" is three numbers with the middle one missing, not two numbers --
// and letting it through would silently shift the values.
void skipSeparator(const char** p, bool* sawComma)
{
    *sawComma = false;
    while (isSpace(**p)) {
        ++*p;
    }
    if (**p == ',') {
        *sawComma = true;
        ++*p;
        while (isSpace(**p)) {
            ++*p;
        }
    }
}

// One signed decimal number, by hand. Returns false if there is not one here.
//
// The digits accumulate in double as they are read. That loses precision past
// 2^53, which is far beyond the +-32,000,000 the caller will accept, so the
// range check afterwards catches anything the accumulation could have blurred.
bool parseNumber(const char** p, double* out)
{
    const char* c = *p;

    bool negative = false;
    if (*c == '+' || *c == '-') {
        negative = *c == '-';
        ++c;
    }

    // At least one digit before the point. ".5" is rejected on purpose: it is
    // far more often a typo than an intention, and "0.5" is unambiguous.
    if (!isDigit(*c)) {
        return false;
    }

    double value = 0.0;
    int digits = 0;
    while (isDigit(*c)) {
        value = value * 10.0 + double(*c - '0');
        ++c;
        // A number this long is not a coordinate. Stopping here keeps the
        // accumulation from running away before the range check sees it.
        if (++digits > 18) {
            return false;
        }
    }

    if (*c == '.') {
        ++c;
        // A trailing point with no digits after it -- "64." -- is incomplete
        // input rather than the integer 64.
        if (!isDigit(*c)) {
            return false;
        }

        // **The fraction accumulates as an integer and is divided once.** The
        // obvious loop -- `value += digit * scale; scale *= 0.1` -- multiplies
        // by an inexact 0.1 repeatedly and compounds the error: it turns
        // "-0.75" into -0.75000000000000011. Building 75/100 instead is a
        // single correctly-rounded division, and both 10^15 and any integer
        // below it are exact in a double, so nothing is lost on the way in.
        u64 fraction = 0;
        double divisor = 1.0;
        int fractionDigits = 0;
        while (isDigit(*c)) {
            // Past fifteen digits a decimal cannot move a double that is
            // already holding a coordinate, so the rest are read and dropped
            // rather than overflowing the accumulator.
            if (fractionDigits < 15) {
                fraction = fraction * 10u + u64(*c - '0');
                divisor *= 10.0;
                ++fractionDigits;
            }
            ++c;
        }
        value += double(fraction) / divisor;
    }

    *out = negative ? -value : value;
    *p = c;
    return true;
}

}  // namespace

const char* parseCoordinateTriple(const char* text, CoordTriple* out)
{
    if (text == nullptr) {
        return "empty";
    }

    const char* p = text;
    while (isSpace(*p)) {
        ++p;
    }
    if (*p == '\0') {
        return "type three numbers: x y z";
    }

    double values[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            // A separator is required between numbers. Without this, "1-2 3"
            // would read as 1, -2, 3, which is not what anyone typing it meant.
            const char* before = p;
            bool sawComma = false;
            skipSeparator(&p, &sawComma);
            if (p == before && !sawComma) {
                return "put a space or comma between the numbers";
            }
        }

        if (!parseNumber(&p, &values[i])) {
            return i == 0 ? "type three numbers: x y z" : "need three numbers: x y z";
        }
    }

    bool trailingComma = false;
    skipSeparator(&p, &trailingComma);
    if (*p != '\0' || trailingComma) {
        return "three numbers only: x y z";
    }

    // Range last, so a typo in the third number is reported as a typo rather
    // than as whatever the first two happened to be.
    if (values[0] < -kWorldHorizontalLimit || values[0] > kWorldHorizontalLimit ||
        values[2] < -kWorldHorizontalLimit || values[2] > kWorldHorizontalLimit) {
        return "x and z must be within +-32000000";
    }
    if (values[1] < kCameraMinY || values[1] > kCameraMaxY) {
        return "y must be between 1 and 254";
    }

    out->x = values[0];
    out->y = values[1];
    out->z = values[2];
    return nullptr;
}

}  // namespace mc
