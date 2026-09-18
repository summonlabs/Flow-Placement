// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal, dependency-free test harness. Tests run plainly with no timeouts:
// a test that hangs is a defect to diagnose, not a test to abandon.

#ifndef FLOWPLACE_TESTS_HARNESS_HPP
#define FLOWPLACE_TESTS_HARNESS_HPP

#include <cstdint>
#include <iostream>
#include <string>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "flowplace/explain.hpp"
#include "flowplace/ids.hpp"
#include "flowplace/model.hpp"
#include "flowplace/runtime.hpp"
#include "flowplace/status.hpp"
#include "flowplace/wire.hpp"

namespace fptest {

// ---- value rendering -----------------------------------------------------
inline std::string Show(bool v) { return v ? "true" : "false"; }
inline std::string Show(const std::string& v) { return v; }
inline std::string Show(std::string_view v) { return std::string(v); }
inline std::string Show(const char* v) { return std::string(v); }
template <class T>
std::string Show(const flowplace::Id<T>& v) { return v.ToString(); }
template <class T>
std::string Show(const flowplace::Generation<T>& v) { return v.ToString(); }
inline std::string Show(const flowplace::Digest& v) { return v.ToHex(); }
inline std::string Show(const flowplace::FabricEpoch& v) { return std::to_string(v.value()); }
inline std::string Show(const flowplace::Status& v) { return v.ToString(); }
inline std::string Show(flowplace::StatusCode v) { return std::string(flowplace::StatusCodeName(v)); }
inline std::string Show(flowplace::Outcome v) { return std::string(flowplace::OutcomeName(v)); }
inline std::string Show(flowplace::AttemptState v) {
  return std::string(flowplace::AttemptStateName(v));
}
inline std::string Show(flowplace::ExclusionReason v) {
  return std::string(flowplace::ExclusionReasonName(v));
}
inline std::string Show(flowplace::AttemptPhase v) {
  return std::string(flowplace::AttemptPhaseName(v));
}
inline std::string Show(flowplace::IncumbentDelta v) {
  return std::string(flowplace::IncumbentDeltaName(v));
}
inline std::string Show(flowplace::RevalidationVerdict v) {
  return std::string(flowplace::RevalidationVerdictName(v));
}
inline std::string Show(flowplace::PathTier v) { return std::string(flowplace::PathTierName(v)); }
inline std::string Show(flowplace::ServiceClass v) {
  return std::string(flowplace::ServiceClassName(v));
}
inline std::string Show(flowplace::LocalityScope v) {
  return std::string(flowplace::LocalityScopeName(v));
}
inline std::string Show(flowplace::ObjectiveKind v) {
  return std::string(flowplace::ObjectiveKindName(v));
}
inline std::string Show(flowplace::Direction v) { return std::string(flowplace::DirectionName(v)); }
inline std::string Show(flowplace::PriorityClass v) {
  return std::string(flowplace::PriorityClassName(v));
}
inline std::string Show(flowplace::GateAction v) { return std::string(flowplace::GateActionName(v)); }
inline std::string Show(flowplace::ChurnAction v) {
  return std::string(flowplace::ChurnActionName(v));
}
inline std::string Show(flowplace::FrameKind v) {
  return std::string(flowplace::FrameKindName(v));
}
inline std::string Show(flowplace::EvidencePolicyMode v) {
  return std::string(flowplace::EvidencePolicyModeName(v));
}
template <class T>
std::string Show(const T& v) {
  std::ostringstream out;
  out << v;
  return out.str();
}

class Context {
 public:
  void Fail(const char* file, int line, const std::string& message) {
    ++failures_;
    std::cout << "  FAIL " << file << ":" << line << ": " << message << "\n";
  }
  [[nodiscard]] int failures() const { return failures_; }

 private:
  int failures_ = 0;
};

using TestFn = void (*)(Context&);

struct TestCase {
  std::string suite;
  std::string name;
  TestFn fn;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

struct Registrar {
  Registrar(const char* suite, const char* name, TestFn fn) {
    Registry().push_back(TestCase{suite, name, fn});
  }
};

int RunAll(const std::vector<std::string>& filters);

}  // namespace fptest

#define FP_TEST(suite_name, test_name)                                                     \
  static void suite_name##_##test_name##_body([[maybe_unused]] fptest::Context& fp_ctx);   \
  static const fptest::Registrar suite_name##_##test_name##_registrar(                     \
      #suite_name, #test_name, &suite_name##_##test_name##_body);                          \
  static void suite_name##_##test_name##_body([[maybe_unused]] fptest::Context& fp_ctx)

// The condition is bound to a bool first so that a constant expression inside
// a check does not trip the "conditional expression is constant" warning.
#define FP_CHECK(expr)                                                       \
  do {                                                                       \
    const bool fp_condition = static_cast<bool>(expr);                       \
    if (!fp_condition) {                                                     \
      fp_ctx.Fail(__FILE__, __LINE__, "check failed: " #expr);                \
    }                                                                        \
  } while (false)

#define FP_REQUIRE(expr)                                                                 \
  do {                                                                                   \
    const bool fp_condition = static_cast<bool>(expr);                                   \
    if (!fp_condition) {                                                                 \
      fp_ctx.Fail(__FILE__, __LINE__, "requirement failed: " #expr);                      \
      return;                                                                            \
    }                                                                                    \
  } while (false)

#define FP_CHECK_EQ(a, b)                                                                \
  do {                                                                                   \
    const auto fp_lhs = (a);                                                             \
    const auto fp_rhs = (b);                                                             \
    const bool fp_condition = (fp_lhs == fp_rhs);                                        \
    if (!fp_condition) {                                                                 \
      fp_ctx.Fail(__FILE__, __LINE__, std::string("expected " #a " == " #b " but got ") + \
                                          fptest::Show(fp_lhs) + " vs " +                  \
                                          fptest::Show(fp_rhs));                         \
    }                                                                                    \
  } while (false)

#define FP_CHECK_NE(a, b)                                                                \
  do {                                                                                   \
    const auto fp_lhs = (a);                                                             \
    const auto fp_rhs = (b);                                                             \
    const bool fp_condition = (fp_lhs == fp_rhs);                                        \
    if (fp_condition) {                                                                  \
      fp_ctx.Fail(__FILE__, __LINE__, std::string("expected " #a " != " #b " but both ") + \
                                          fptest::Show(fp_lhs));                         \
    }                                                                                    \
  } while (false)

#endif  // FLOWPLACE_TESTS_HARNESS_HPP
