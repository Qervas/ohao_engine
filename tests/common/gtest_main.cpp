// main() for the GoogleTest suites (env_cdf_test, sobol_test, denoise_parse_test): runs
// every test as gtest_main does, then prints the shared harness's JSON summary line
// (tests/common/harness.hpp), one entry per test case.
#include "harness.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

std::string suiteName(const char* argv0) {
    std::string s(argv0 ? argv0 : "gtest");
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s.erase(0, slash + 1);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s.resize(s.size() - 4);
    return s;
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    const testing::UnitTest& unit = *testing::UnitTest::GetInstance();
    int passed = 0, failed = 0;
    std::vector<std::string> failures;
    for (int i = 0; i < unit.total_test_suite_count(); ++i) {
        const testing::TestSuite& suite = *unit.GetTestSuite(i);
        for (int j = 0; j < suite.total_test_count(); ++j) {
            const testing::TestInfo& t = *suite.GetTestInfo(j);
            if (!t.should_run()) continue;
            if (t.result()->Passed()) {
                ++passed;
            } else {
                ++failed;
                failures.push_back(std::string(suite.name()) + "." + t.name());
            }
        }
    }
    const int summary = ohao::test::finish(suiteName(argc > 0 ? argv[0] : nullptr).c_str(), passed, failed, failures);
    return rc != 0 ? rc : summary;
}
