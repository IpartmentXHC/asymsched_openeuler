#pragma once

#include <cmath>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {

class Failure : public std::runtime_error {
public:
    explicit Failure(const std::string& message)
        : std::runtime_error(message) {}
};

using Function = void (*)();

struct Case {
    const char* name;
    Function function;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> registered;
    return registered;
}

struct Registrar {
    Registrar(const char* name, Function function) {
        cases().push_back(Case{name, function});
    }
};

[[noreturn]] inline void fail(const char* file, int line,
                              const std::string& message) {
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": " +
                  message);
}

inline int run_all(int argc, char** argv) {
    std::string filter;
    if (argc > 1) {
        filter = argv[1];
    }
    int failed = 0;
    int selected = 0;
    for (const auto& test_case : cases()) {
        if (!filter.empty() &&
            std::string(test_case.name).find(filter) == std::string::npos) {
            continue;
        }
        ++selected;
        try {
            test_case.function();
            std::cout << "[PASS] " << test_case.name << "\n";
        } catch (const Failure& error) {
            ++failed;
            std::cout << "[FAIL] " << test_case.name << "\n  " << error.what()
                      << "\n";
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[FAIL] " << test_case.name
                      << "\n  unexpected exception: " << error.what() << "\n";
        } catch (...) {
            ++failed;
            std::cout << "[FAIL] " << test_case.name
                      << "\n  unexpected non-standard exception\n";
        }
    }
    std::cout << "\n" << (selected - failed) << "/" << selected
              << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace test

#define TEST(name)                                                     \
    static void name();                                                \
    static ::test::Registrar registrar_##name(#name, name);            \
    static void name()

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            ::test::fail(__FILE__, __LINE__,                           \
                         "CHECK failed: " #condition);                 \
        }                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                     \
    do {                                                               \
        if (!((actual) == (expected))) {                               \
            ::test::fail(__FILE__, __LINE__,                           \
                         "CHECK_EQ failed: " #actual " == " #expected); \
        }                                                              \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                        \
    do {                                                               \
        const double value_ = (actual);                                \
        const double expected_ = (expected);                           \
        const double tolerance_ = (tolerance);                         \
        if (std::abs(value_ - expected_) > tolerance_) {              \
            std::ostringstream message_;                               \
            message_ << "CHECK_NEAR failed: " << value_ << " vs "      \
                     << expected_;                                     \
            ::test::fail(__FILE__, __LINE__, message_.str());          \
        }                                                              \
    } while (0)

#define CHECK_THROWS(...)                                              \
    do {                                                               \
        bool thrown_ = false;                                          \
        try {                                                          \
            __VA_ARGS__;                                               \
        } catch (...) {                                                \
            thrown_ = true;                                            \
        }                                                              \
        if (!thrown_) {                                                \
            ::test::fail(__FILE__, __LINE__,                           \
                         "CHECK_THROWS failed: " #__VA_ARGS__);        \
        }                                                              \
    } while (0)

#define TEST_MAIN                                                      \
    int main(int argc, char** argv) {                                  \
        return ::test::run_all(argc, argv);                            \
    }
