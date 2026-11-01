#include <cstdio>

#include "unity.h"

int main()
{
    int failures = 0;
    for (const unity_shim::TestCase &test : unity_shim::registry()) {
        try {
            test.function();
        } catch (const unity_shim::Failure &failure) {
            ++failures;
            std::printf("FAIL: %s\n  %s\n", test.name, failure.what());
        }
    }
    std::printf("%zu tests, %d failures\n", unity_shim::registry().size(), failures);
    return failures == 0 ? 0 : 1;
}
