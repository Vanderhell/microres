#include "mres.h"

int main(void)
{
    mres_retry_t retry;
    mres_retry_policy_t policy = { 1u, MRES_BACKOFF_FIXED, 0u, 0u, 0u, 0u };

    return (mres_retry_init(&retry, &policy) == MRES_OK) ? 0 : 1;
}
