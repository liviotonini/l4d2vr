#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>

inline int g_TestFailures = 0;
inline int g_TestChecks = 0;

#define CHECK(condition) do { ++g_TestChecks; if (!(condition)) { ++g_TestFailures; printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)
#define CHECK_NEAR(value, expected, tolerance) do { ++g_TestChecks; float v_ = (float)(value), e_ = (float)(expected); if (fabsf(v_ - e_) > (float)(tolerance)) { ++g_TestFailures; printf("  FAILED %s:%d: %s = %f, expected %f\n", __FILE__, __LINE__, #value, v_, e_); } } while (0)
