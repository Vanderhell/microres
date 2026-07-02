#include "mres.h"

int main()
{
    mres_breaker_state_t state = MRES_BREAKER_CLOSED;
    return state == MRES_BREAKER_CLOSED ? 0 : 1;
}
