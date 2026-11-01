#pragma once

// A test harness small enough to not be a dependency.
//
// The project builds with -fno-exceptions, which rules out most of the usual
// frameworks, and the whole point of the host target is that it stays trivial
// to build. Tests self-register through static constructors and report failures
// by counter, never by throwing.

#include <cstdio>
#include <string>
#include <vector>

namespace mc::test {

using TestFn = void (*)();

struct Case {
    const char* name;
    TestFn fn;
};

std::vector<Case>& registry();
int failures();
void reportFailure(const char* file, int line, const std::string& message);

// How many times slower than a native host this run is, from MC_TEST_SLOW
// (default 1). Tests that give a threaded worker a fixed number of frames to
// finish multiply that cap by it; tools/test_big_endian.sh sets it for qemu,
// where the worker runs ~20x slower and a native cap reads as a hang.
int slowScale();

struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};

std::string describe(bool value);
std::string describe(const std::string& value);
std::string describe(const char* value);
std::string describe(long long value);
std::string describe(unsigned long long value);
std::string describe(double value);

template <typename T>
std::string describe(const T& value)
{
    return describe(static_cast<long long>(value));
}

}  // namespace mc::test

#define TEST(name)                                                       \
    static void name();                                                  \
    static ::mc::test::Registrar registrar_##name(#name, name);          \
    static void name()

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            ::mc::test::reportFailure(__FILE__, __LINE__, #condition);         \
            return;                                                            \
        }                                                                      \
    } while (false)

// Reports both sides on failure, which is the whole reason to prefer it over
// CHECK for comparisons.
#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        const auto actual_ = (actual);                                         \
        const auto expected_ = (expected);                                     \
        if (!(actual_ == expected_)) {                                         \
            ::mc::test::reportFailure(__FILE__, __LINE__,                      \
                                      std::string(#actual) + " == " +          \
                                          ::mc::test::describe(actual_) +      \
                                          ", expected " +                      \
                                          ::mc::test::describe(expected_));    \
            return;                                                            \
        }                                                                      \
    } while (false)
