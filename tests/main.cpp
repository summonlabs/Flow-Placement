// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "harness.hpp"

#include <algorithm>
#include <cstring>

namespace fptest {

int RunAll(const std::vector<std::string>& filters) {
  int executed = 0;
  int failed = 0;
  for (const TestCase& test : Registry()) {
    if (!filters.empty()) {
      const bool matches =
          std::find(filters.begin(), filters.end(), test.suite) != filters.end() ||
          std::find(filters.begin(), filters.end(), test.name) != filters.end();
      if (!matches) continue;
    }
    Context context;
    std::cout << "[ RUN  ] " << test.suite << "." << test.name << "\n";
    test.fn(context);
    ++executed;
    if (context.failures() != 0) {
      ++failed;
      std::cout << "[ FAIL ] " << test.suite << "." << test.name << " (" << context.failures()
                << " failures)\n";
    } else {
      std::cout << "[  OK  ] " << test.suite << "." << test.name << "\n";
    }
  }
  std::cout << (failed == 0 ? "PASSED " : "FAILED ") << (executed - failed) << "/" << executed
            << " tests\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace fptest

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--list") == 0) {
      for (const fptest::TestCase& test : fptest::Registry()) {
        std::cout << test.suite << "." << test.name << "\n";
      }
      return 0;
    }
    filters.emplace_back(argv[i]);
  }
  return fptest::RunAll(filters);
}
