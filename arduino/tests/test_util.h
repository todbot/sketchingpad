// SPDX-FileCopyrightText: 2026 Tod Kurt
// SPDX-License-Identifier: GPL-3.0-or-later
//
// test_util.h -- the whole test framework, deliberately
#pragma once

#include <stdio.h>

static int test_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      printf("  ok   %s\n", msg);                                              \
    } else {                                                                   \
      printf("  FAIL %s   (%s:%d)\n", msg, __FILE__, __LINE__);                \
      test_failures++;                                                         \
    }                                                                          \
  } while (0)

static inline int test_report(const char *name) {
  printf("\n%s: %s (%d failures)\n", name, test_failures ? "FAILED" : "ALL PASS",
         test_failures);
  return test_failures != 0;
}
