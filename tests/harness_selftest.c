#include <stdbool.h>
#include <stdio.h>

typedef bool (*selftest_fn)(int *side_effects);

static bool pass_case(int *side_effects)
{
    *side_effects += 1;
    return true;
}

static bool fail_case(int *side_effects)
{
    *side_effects += 1;
    return false;
}

static int run_case(selftest_fn fn, int *passed, int *failed, int *side_effects)
{
    const bool ok = fn(side_effects);

    if (ok) {
        *passed += 1;
        return 0;
    }

    *failed += 1;
    return 1;
}

int main(void)
{
    int passed = 0;
    int failed = 0;
    int side_effects = 0;
    int exit_code = 0;

    exit_code |= run_case(pass_case, &passed, &failed, &side_effects);
    exit_code |= run_case(fail_case, &passed, &failed, &side_effects);

    if ((passed != 1) || (failed != 1) || (side_effects != 2) || (exit_code == 0)) {
        printf("harness selftest failed\n");
        return 2;
    }

    printf("harness selftest ok\n");
    return 1;
}
