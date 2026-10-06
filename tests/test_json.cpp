// The JSON helpers are shared by metrics.json and the --progress-json stream, both of which are
// parsed by the platform worker.
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include "metrics/Metrics.hpp"

using aquasph::jsonEscape;
using aquasph::jsonNumber;

TEST(JsonHelpers, EscapesQuotesBackslashesAndCommonControls) {
    EXPECT_EQ(jsonEscape("a\"b\\c\nd\te\r"), "a\\\"b\\\\c\\nd\\te\\r");
}

TEST(JsonHelpers, EscapesEveryOtherControlCharacter) {
    // Before the fix these bytes passed through raw, which RFC 8259
    // forbids inside a string.
    EXPECT_EQ(jsonEscape(std::string("x\x01y\x1fz", 5)), "x\\u0001y\\u001fz");
    EXPECT_EQ(jsonEscape(std::string("\b", 1)), "\\u0008");
}

TEST(JsonHelpers, LeavesUtf8Untouched) {
    EXPECT_EQ(jsonEscape("\xC3\xA9t\xC3\xA9"), "\xC3\xA9t\xC3\xA9");
}

TEST(JsonHelpers, NonFiniteNumbersBecomeNull) {
    EXPECT_EQ(jsonNumber(std::numeric_limits<double>::quiet_NaN()), "null");
    EXPECT_EQ(jsonNumber(std::numeric_limits<double>::infinity()), "null");
    EXPECT_EQ(jsonNumber(-std::numeric_limits<double>::infinity()), "null");
    EXPECT_EQ(jsonNumber(0.5), "0.5");
}
