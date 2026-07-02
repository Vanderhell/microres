#include "mres.h"

int main()
{
    mres_backoff_t strategy = MRES_BACKOFF_FIXED;
    return strategy == MRES_BACKOFF_FIXED ? 0 : 1;
}
