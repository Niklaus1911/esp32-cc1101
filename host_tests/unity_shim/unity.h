#pragma once
// Minimal host stand-in for the ESP-IDF Unity API used by component tests, so pure tests (such as
// the RF decoder's) also run on the host. A failed assertion aborts the current test case.

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace unity_shim {

struct TestCase {
    const char *name;
    void (*function)();
};

inline std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char *name, void (*function)()) { registry().push_back({name, function}); }
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void fail(const char *file, int line, const std::string &message)
{
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

inline void check_equal(long long expected, long long actual, const char *file, int line)
{
    if (expected != actual) {
        fail(file, line, "expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

inline void check_equal_unsigned(unsigned long long expected, unsigned long long actual, const char *file,
                                 int line)
{
    if (expected != actual) {
        fail(file, line, "expected " + std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

}  // namespace unity_shim

#define UNITY_SHIM_CONCAT_(left, right) left##right
#define UNITY_SHIM_CONCAT(left, right) UNITY_SHIM_CONCAT_(left, right)
#define TEST_CASE(name, tags)                                                                  \
    static void UNITY_SHIM_CONCAT(unity_shim_test_, __LINE__)();                               \
    static const unity_shim::Registrar UNITY_SHIM_CONCAT(unity_shim_registrar_, __LINE__)(     \
        name, &UNITY_SHIM_CONCAT(unity_shim_test_, __LINE__));                                 \
    static void UNITY_SHIM_CONCAT(unity_shim_test_, __LINE__)()

#define TEST_ASSERT_TRUE(condition)                                                            \
    do {                                                                                       \
        if (!(condition)) unity_shim::fail(__FILE__, __LINE__, "expected true: " #condition);  \
    } while (0)
#define TEST_ASSERT_FALSE(condition)                                                           \
    do {                                                                                       \
        if (condition) unity_shim::fail(__FILE__, __LINE__, "expected false: " #condition);    \
    } while (0)
#define TEST_ASSERT_EQUAL(expected, actual)                                                    \
    unity_shim::check_equal(static_cast<long long>(expected), static_cast<long long>(actual), __FILE__, __LINE__)
#define TEST_ASSERT_EQUAL_UINT8(expected, actual)                                              \
    unity_shim::check_equal_unsigned(static_cast<uint8_t>(expected), static_cast<uint8_t>(actual), __FILE__, __LINE__)
#define TEST_ASSERT_EQUAL_UINT16(expected, actual)                                             \
    unity_shim::check_equal_unsigned(static_cast<uint16_t>(expected), static_cast<uint16_t>(actual), __FILE__, __LINE__)
#define TEST_ASSERT_EQUAL_UINT32(expected, actual)                                             \
    unity_shim::check_equal_unsigned(static_cast<uint32_t>(expected), static_cast<uint32_t>(actual), __FILE__, __LINE__)
#define TEST_ASSERT_EQUAL_UINT64(expected, actual)                                             \
    unity_shim::check_equal_unsigned(static_cast<uint64_t>(expected), static_cast<uint64_t>(actual), __FILE__, __LINE__)
#define TEST_ASSERT_GREATER_OR_EQUAL_UINT16(threshold, actual)                                 \
    do {                                                                                       \
        if (static_cast<uint16_t>(actual) < static_cast<uint16_t>(threshold))                  \
            unity_shim::fail(__FILE__, __LINE__, "expected >= " #threshold ": " #actual);      \
    } while (0)
#define TEST_ASSERT_LESS_OR_EQUAL_UINT16(threshold, actual)                                    \
    do {                                                                                       \
        if (static_cast<uint16_t>(actual) > static_cast<uint16_t>(threshold))                  \
            unity_shim::fail(__FILE__, __LINE__, "expected <= " #threshold ": " #actual);      \
    } while (0)
