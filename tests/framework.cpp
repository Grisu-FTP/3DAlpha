#include "framework.hpp"

#include <cstdio>
#include <cstring>

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

// An optional substring filter, so one test can be run on its own.
//
// The suite takes minutes -- worldgen fixtures and the streaming tests are the
// bulk of it -- and iterating on a single case by running all of them is the
// difference between a ten-second loop and a five-minute one. Substring rather
// than a pattern language: the names are long and distinctive, and `revisit` is
// all anyone is going to type.
//
// **It reports what it ran.** A filter that matched nothing and exited 0 would
// look exactly like a passing run, which is the one way a convenience like this
// can cost more than it saves.
int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;

    int failed = 0;
    int ran = 0;
    for (const auto& testCase : mc::test::registry()) {
        if (filter != nullptr && std::strstr(testCase.name, filter) == nullptr) {
            continue;
        }
        ++ran;
        mc::test::g_caseFailures = 0;
        std::printf("  %s\n", testCase.name);
        testCase.fn();
        if (mc::test::g_caseFailures != 0) {
            failed++;
        }
    }

    if (filter != nullptr && ran == 0) {
        std::printf("\nno test matches \"%s\"\n", filter);
        return 1;
    }

    if (failed == 0) {
        std::printf("\n%d/%d passed\n", ran, ran);
        return 0;
    }
    std::printf("\n%d/%d FAILED (%d checks)\n", failed, ran, mc::test::failures());
    return 1;
}
