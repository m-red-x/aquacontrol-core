/* Minimal C test harness.
 *
 * Deliberately not GoogleTest: that would pull a C++ toolchain and a fetched
 * dependency into what is otherwise a zero-dependency pure-C library. CTest
 * already gives per-test reporting from plain executables, and a beginner can
 * read all of this in one sitting.
 */
#ifndef AQUA_TEST_H
#define AQUA_TEST_H

#include <stdio.h>

static int aqua_checks = 0;
static int aqua_failures = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    aqua_checks++;                                                          \
    if (!(cond)) {                                                          \
      aqua_failures++;                                                      \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                       \
  } while (0)

#define CHECK_EQ(actual, expected)                                          \
  do {                                                                      \
    long long a_ = (long long)(actual);                                     \
    long long e_ = (long long)(expected);                                   \
    aqua_checks++;                                                          \
    if (a_ != e_) {                                                         \
      aqua_failures++;                                                      \
      printf("FAIL %s:%d  %s  (got %lld, want %lld)\n", __FILE__, __LINE__, \
             #actual, a_, e_);                                             \
    }                                                                       \
  } while (0)

#define CHECK_NEAR(actual, expected, tol)                                   \
  do {                                                                      \
    long long a_ = (long long)(actual);                                     \
    long long e_ = (long long)(expected);                                   \
    long long d_ = (a_ > e_) ? (a_ - e_) : (e_ - a_);                       \
    aqua_checks++;                                                          \
    if (d_ > (long long)(tol)) {                                            \
      aqua_failures++;                                                      \
      printf("FAIL %s:%d  %s  (got %lld, want %lld +/- %lld)\n", __FILE__,  \
             __LINE__, #actual, a_, e_, (long long)(tol));                  \
    }                                                                       \
  } while (0)

#define AQUA_TEST_REPORT()                                                  \
  do {                                                                      \
    printf("%d checks, %d failures\n", aqua_checks, aqua_failures);         \
    return aqua_failures == 0 ? 0 : 1;                                      \
  } while (0)

#endif /* AQUA_TEST_H */
