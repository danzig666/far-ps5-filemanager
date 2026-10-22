#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// Tiny test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }

namespace test {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& Failures() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

inline int RunAll() {
  int failedCases = 0;
  for (const auto& c : Registry()) {
    int before = Failures();
    try {
      c.fn();
    } catch (const std::exception& e) {
      std::printf("  exception: %s\n", e.what());
      ++Failures();
    } catch (...) {
      std::printf("  unknown exception\n");
      ++Failures();
    }
    bool ok = Failures() == before;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    if (!ok) ++failedCases;
  }
  std::printf("%zu tests, %d failed\n", Registry().size(), failedCases);
  return failedCases ? 1 : 0;
}

}  // namespace test

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                         \
  static void TEST_CAT(test_fn_, __LINE__)();                              \
  static test::Registrar TEST_CAT(test_reg_, __LINE__)(name, TEST_CAT(test_fn_, __LINE__)); \
  static void TEST_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
      ++test::Failures();                                                    \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    if (!((a) == (b))) {                                                                 \
      std::printf("  %s:%d: CHECK_EQ(%s, %s) failed\n", __FILE__, __LINE__, #a, #b);   \
      ++test::Failures();                                                                \
    }                                                                                    \
  } while (0)

#define CHECK_THROWS(expr, type)                                                         \
  do {                                                                                   \
    bool thrown = false;                                                                 \
    try {                                                                                \
      (void)(expr);                                                                      \
    } catch (const type&) {                                                              \
      thrown = true;                                                                     \
    }                                                                                    \
    if (!thrown) {                                                                       \
      std::printf("  %s:%d: %s did not throw %s\n", __FILE__, __LINE__, #expr, #type);  \
      ++test::Failures();                                                                \
    }                                                                                    \
  } while (0)
