#include "framework.hpp"

#include <cstdio>

namespace mc::test {

namespace {
int g_failures = 0;
int g_caseFailures = 0;
}  // namespace

std::vector<Case>& registry()
{
    // Function-local so registration order across translation units cannot
    // depend on static initialisation order.
    static std::vector<Case> cases;
    return cases;
}

int failures()
{
    return g_failures;
}

void reportFailure(const char* file, int line, const std::string& message)
{
    g_failures++;
    g_caseFailures++;
    std::printf("      %s:%d: %s\n", file, line, message.c_str());
}

std::string describe(bool value)
{
    return value ? "true" : "false";
}

std::string describe(const std::string& value)
{
    return "\"" + value + "\"";
}

std::string describe(const char* value)
{
    return std::string("\"") + (value != nullptr ? value : "(null)") + "\"";
}

std::string describe(long long value)
{
    return std::to_string(value);
}

std::string describe(unsigned long long value)
{
    return std::to_string(value);
}

std::string describe(double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

}  // namespace mc::test

int main()
{
    int failed = 0;
    for (const auto& testCase : mc::test::registry()) {
        mc::test::g_caseFailures = 0;
        std::printf("  %s\n", testCase.name);
        testCase.fn();
        if (mc::test::g_caseFailures != 0) {
            failed++;
        }
    }

    const int total = static_cast<int>(mc::test::registry().size());
    if (failed == 0) {
        std::printf("\n%d/%d passed\n", total, total);
        return 0;
    }
    std::printf("\n%d/%d FAILED (%d checks)\n", failed, total, mc::test::failures());
    return 1;
}
