#pragma once
// The shared test harness (sub-project 4, step 2). Every C++ suite ends with the same
// machine-readable line, which tools/check.py reads from the test's output:
//
//   {"suite":"<name>","passed":<n>,"failed":<n>,"failures":["<what>", ...]}
//
// Two ways to use it:
//   - CHECK / RUN below, and `return ohao::test::finish("suite");` at the end of main;
//   - a suite with its own macros keeps them and ends with
//     `return ohao::test::finish("suite", passed, failed, failures);`.
// Header-only, standard library only, so any suite can include it.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ohao::test {

// This process's id (scratch files a test writes are named by it: CTest runs copies side by side).
inline unsigned long processId() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Sets an environment variable for this process; an empty value removes it (as _putenv_s does on Windows).
inline void setEnvVar(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (value[0] != '\0') setenv(name, value, 1);
    else unsetenv(name);
#endif
}

struct State {
    int passed{0};
    int failed{0};
    std::vector<std::string> failures;
    const char* current{""};   // the test RUN is running
};

inline State& state() {
    static State s;
    return s;
}

// `s` as a JSON string literal (quotes included).
inline std::string jsonString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

// Prints "Results: p/t passed" and the JSON line; returns the exit code (0 = all passed).
inline int finish(const char* suite, int passed, int failed, const std::vector<std::string>& failures) {
    std::string line = "{\"suite\":" + jsonString(suite) + ",\"passed\":" + std::to_string(passed) +
                       ",\"failed\":" + std::to_string(failed) + ",\"failures\":[";
    for (std::size_t i = 0; i < failures.size(); ++i) line += (i ? "," : "") + jsonString(failures[i]);
    line += "]}";
    std::printf("Results: %d/%d passed\n%s\n", passed, passed + failed, line.c_str());
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}

inline int finish(const char* suite) {
    const State& s = state();
    return finish(suite, s.passed, s.failed, s.failures);
}

} // namespace ohao::test

// Fails the current test (returns from it) when `expr` is false.
#define CHECK(expr, msg)                                                                         \
    do {                                                                                         \
        if (!(expr)) {                                                                           \
            std::printf("    FAIL: %s  [%s]\n", (msg), #expr);                                   \
            ++::ohao::test::state().failed;                                                      \
            ::ohao::test::state().failures.push_back(std::string(::ohao::test::state().current) + \
                                                     ": " + (msg));                              \
            return;                                                                              \
        }                                                                                        \
    } while (0)

// Runs one test function; it passes if no CHECK in it failed.
#define RUN(fn)                                                                    \
    do {                                                                           \
        ::ohao::test::state().current = #fn;                                       \
        std::printf("  %-60s", #fn);                                               \
        const int failedBefore = ::ohao::test::state().failed;                     \
        fn();                                                                      \
        if (::ohao::test::state().failed == failedBefore) {                        \
            ++::ohao::test::state().passed;                                        \
            std::printf("PASS\n");                                                 \
        }                                                                          \
    } while (0)
