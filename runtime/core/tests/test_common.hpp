// Minimal dependency-free test harness.
#pragma once

#include <cmath>
#include <cstdio>

extern int g_failures;
extern int g_checks;

#define SG_CHECK(cond)                                                                 \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

#define SG_CHECK_NEAR(a, b, tol)                                                          \
    do {                                                                                  \
        ++g_checks;                                                                       \
        const double sg_a_ = static_cast<double>(a);                                      \
        const double sg_b_ = static_cast<double>(b);                                      \
        if (!(std::fabs(sg_a_ - sg_b_) <= (tol))) {                                       \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s=%.9g %s=%.9g tol=%.3g\n",  \
                         __FILE__, __LINE__, #a, sg_a_, #b, sg_b_, static_cast<double>(tol)); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

void RunGlobalPositionTests();
void RunFloatingOriginTests();
void RunChunkTests();
void RunChunkFuzzTests(int iterations);
