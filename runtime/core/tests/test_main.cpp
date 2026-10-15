#include <cstdio>
#include <cstdlib>

#include "test_common.hpp"

int g_failures = 0;
int g_checks = 0;

int main() {
    int fuzzIterations = 20000;
    if (const char* env = std::getenv("SG_FUZZ_ITERS")) {
        fuzzIterations = std::atoi(env);
    }

    RunGlobalPositionTests();
    RunFloatingOriginTests();
    RunChunkTests();
    RunChunkFuzzTests(fuzzIterations);

    std::printf("checks: %d, failures: %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
