/*
 * microres — Implementation.
 *
 * SPDX-License-Identifier: MIT
 * https://github.com/Vanderhell/microres
 */

#include "mres.h"

/* ── Internal helpers ──────────────────────────────────────────────────── */

/** Elapsed time handling uint32_t wrap-around. */
static inline uint32_t elapsed_ms(uint32_t start, uint32_t now)
{
    return now - start;  /* works correctly with unsigned wrap */
}

/** Clamp a value to a maximum. */
static inline uint32_t clamp_u32(uint32_t value, uint32_t max)
{
    return (max > 0 && value > max) ? max : value;
}

/** Minimum of two uint16_t values. */
static inline uint16_t min_u16(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}

#if MRES_ENABLE_JITTER
/**
 * Simple deterministic jitter: ±25% based on a seed.
 *
 * We use a lightweight xorshift to generate pseudo-random jitter from
 * the clock value + attempt number. This avoids needing rand() or a
 * seed state. The jitter is "good enough" to decorrelate retries from
 * multiple devices — it does not need to be cryptographic.
 */
static uint32_t apply_jitter(uint32_t delay, uint32_t seed)
{
    /* xorshift32 */
    uint32_t x = seed;
    if (x == 0) x = 0xDEADBEEF;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    /* ±25%: jitter range is delay/2, centered on delay */
    uint32_t quarter = delay / 4;
    if (quarter == 0) return delay;

    uint32_t jitter = x % (quarter * 2);  /* 0 .. delay/2 */
    return delay - quarter + jitter;       /* delay ± 25% */
}
#endif

/* ── Error strings ─────────────────────────────────────────────────────── */

const char *mres_err_str(mres_err_t err)
{
    switch (err) {
    case MRES_OK:              return "ok";
    case MRES_ERR_NULL:        return "null pointer";
    case MRES_ERR_EXHAUSTED:   return "retries exhausted";
    case MRES_ERR_OPEN:        return "circuit breaker open";
    case MRES_ERR_RATE_LIMITED: return "rate limited";
    case MRES_ERR_OP_FAILED:   return "operation failed";
    case MRES_ERR_INVALID:     return "invalid configuration";
    default:                   return "unknown error";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Retry
 * ═══════════════════════════════════════════════════════════════════════════ */

mres_err_t mres_retry_init(mres_retry_t *retry, const mres_retry_policy_t *policy)
{
    MRES_CHECK_NULL(retry);
    MRES_CHECK_NULL(policy);

    if (policy->max_attempts == 0) {
        return MRES_ERR_INVALID;
    }

    retry->policy     = policy;
    retry->attempts   = 0;
    retry->last_error = 0;

    return MRES_OK;
}

uint32_t mres_delay_calc(const mres_retry_policy_t *policy, uint8_t attempt,
                         mres_clock_fn clock)
{
    if (policy == NULL) return 0;

    uint32_t delay;

    switch (policy->strategy) {
    case MRES_BACKOFF_LINEAR:
        delay = policy->base_delay_ms * ((uint32_t)attempt + 1);
        break;

    case MRES_BACKOFF_EXPONENTIAL: {
        delay = policy->base_delay_ms;
        for (uint8_t i = 0; i < attempt && delay < UINT32_MAX / 2; i++) {
            delay *= 2;
        }
        break;
    }

    case MRES_BACKOFF_FIXED:
    default:
        delay = policy->base_delay_ms;
        break;
    }

    /* Apply cap */
    delay = clamp_u32(delay, policy->max_delay_ms);

#if MRES_ENABLE_JITTER
    if (policy->jitter) {
        uint32_t seed = (clock != NULL) ? clock() : 0;
        seed ^= (uint32_t)attempt * 2654435761U;  /* Knuth multiplicative hash */
        delay = apply_jitter(delay, seed);
        /* Re-apply cap after jitter (jitter can push above) */
        delay = clamp_u32(delay, policy->max_delay_ms);
    }
#else
    (void)clock;
#endif

    return delay;
}

int mres_retry_exec(mres_retry_t *retry, mres_op_fn op, void *ctx,
                    mres_clock_fn clock, mres_sleep_fn sleep)
{
    if (retry == NULL || op == NULL || retry->policy == NULL) {
        return MRES_ERR_NULL;
    }

    const mres_retry_policy_t *policy = retry->policy;
    retry->attempts = 0;

    for (uint8_t i = 0; i < policy->max_attempts; i++) {
        retry->attempts = i + 1;

        int result = op(ctx);
        if (result == 0) {
            retry->last_error = 0;
            return MRES_OK;
        }

        retry->last_error = result;

        /* Don't sleep after the last attempt */
        if (i + 1 < policy->max_attempts) {
            uint32_t delay = mres_delay_calc(policy, i, clock);

            if (sleep != NULL && delay > 0) {
                sleep(delay);
            } else if (sleep == NULL) {
                /* No sleep function — return control to caller.
                 * In non-blocking mode, this lets the caller manage timing.
                 * We continue to the next attempt immediately. */
            }
        }
    }

    return MRES_ERR_EXHAUSTED;
}

mres_err_t mres_retry_reset(mres_retry_t *retry)
{
    MRES_CHECK_NULL(retry);
    retry->attempts   = 0;
    retry->last_error = 0;
    return MRES_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Circuit Breaker
 * ═══════════════════════════════════════════════════════════════════════════ */

mres_err_t mres_breaker_init(mres_breaker_t *br, const mres_breaker_policy_t *policy)
{
    MRES_CHECK_NULL(br);
    MRES_CHECK_NULL(policy);

    if (policy->failure_threshold == 0) {
        return MRES_ERR_INVALID;
    }

    br->policy          = policy;
    br->state           = MRES_BREAKER_CLOSED;
    br->failure_count   = 0;
    br->half_open_calls = 0;
    br->opened_at       = 0;

    return MRES_OK;
}

/** Internal: transition to OPEN state. */
static void breaker_trip(mres_breaker_t *br, uint32_t now)
{
    br->state     = MRES_BREAKER_OPEN;
    br->opened_at = now;
}

/** Internal: transition to CLOSED state. */
static void breaker_close(mres_breaker_t *br)
{
    br->state           = MRES_BREAKER_CLOSED;
    br->failure_count   = 0;
    br->half_open_calls = 0;
}

/** Internal: check if recovery timeout has elapsed. */
static bool breaker_timeout_elapsed(const mres_breaker_t *br, uint32_t now)
{
    return elapsed_ms(br->opened_at, now) >= br->policy->recovery_timeout_ms;
}

int mres_breaker_call(mres_breaker_t *br, mres_op_fn op, void *ctx,
                      mres_clock_fn clock)
{
    if (br == NULL || op == NULL || clock == NULL || br->policy == NULL) {
        return MRES_ERR_NULL;
    }

    uint32_t now = clock();

    /* State evaluation */
    switch (br->state) {
    case MRES_BREAKER_OPEN:
        if (breaker_timeout_elapsed(br, now)) {
            /* Transition to HALF_OPEN */
            br->state           = MRES_BREAKER_HALF_OPEN;
            br->half_open_calls = 0;
        } else {
            return MRES_ERR_OPEN;
        }
        /* fall through to HALF_OPEN */
        /* fall through */

    case MRES_BREAKER_HALF_OPEN:
        if (br->state == MRES_BREAKER_HALF_OPEN &&
            br->half_open_calls >= br->policy->half_open_max_calls) {
            /* Already made max probe calls, still waiting */
            return MRES_ERR_OPEN;
        }
        br->half_open_calls++;
        break;

    case MRES_BREAKER_CLOSED:
    default:
        break;
    }

    /* Execute the operation */
    int result = op(ctx);

    if (result == 0) {
        /* Success */
        if (br->state == MRES_BREAKER_HALF_OPEN) {
            breaker_close(br);
        } else {
            br->failure_count = 0;
        }
        return MRES_OK;
    }

    /* Failure */
    if (br->state == MRES_BREAKER_HALF_OPEN) {
        /* Probe failed — go back to OPEN */
        breaker_trip(br, now);
    } else {
        /* CLOSED: increment failure count */
        br->failure_count++;
        if (br->failure_count >= br->policy->failure_threshold) {
            breaker_trip(br, now);
        }
    }

    return result;  /* pass through the operation's error code */
}

mres_breaker_state_t mres_breaker_state(const mres_breaker_t *br)
{
    if (br == NULL) return MRES_BREAKER_CLOSED;
    return br->state;
}

const char *mres_breaker_state_name(const mres_breaker_t *br)
{
    if (br == NULL) return "?";
    switch (br->state) {
    case MRES_BREAKER_CLOSED:    return "CLOSED";
    case MRES_BREAKER_OPEN:      return "OPEN";
    case MRES_BREAKER_HALF_OPEN: return "HALF_OPEN";
    default:                     return "?";
    }
}

uint32_t mres_breaker_remaining_ms(const mres_breaker_t *br, mres_clock_fn clock)
{
    if (br == NULL || clock == NULL || br->state != MRES_BREAKER_OPEN) {
        return 0;
    }

    uint32_t elapsed = elapsed_ms(br->opened_at, clock());
    if (elapsed >= br->policy->recovery_timeout_ms) {
        return 0;
    }

    return br->policy->recovery_timeout_ms - elapsed;
}

mres_err_t mres_breaker_reset(mres_breaker_t *br)
{
    MRES_CHECK_NULL(br);
    breaker_close(br);
    return MRES_OK;
}

mres_err_t mres_breaker_report_success(mres_breaker_t *br)
{
    MRES_CHECK_NULL(br);
    if (br->state == MRES_BREAKER_HALF_OPEN) {
        breaker_close(br);
    } else {
        br->failure_count = 0;
    }
    return MRES_OK;
}

mres_err_t mres_breaker_report_failure(mres_breaker_t *br, mres_clock_fn clock)
{
    MRES_CHECK_NULL(br);
    if (clock == NULL) return MRES_ERR_NULL;

    uint32_t now = clock();

    if (br->state == MRES_BREAKER_HALF_OPEN) {
        breaker_trip(br, now);
    } else {
        br->failure_count++;
        if (br->failure_count >= br->policy->failure_threshold) {
            breaker_trip(br, now);
        }
    }

    return MRES_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Rate Limiter
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Internal: refill tokens based on elapsed time. */
static void ratelimit_refill(mres_ratelimit_t *rl, uint32_t now)
{
    uint32_t elapsed = elapsed_ms(rl->last_refill, now);

    if (elapsed >= rl->policy->refill_ms) {
        uint32_t intervals = elapsed / rl->policy->refill_ms;
        uint32_t add = intervals * rl->policy->refill_count;

        uint32_t new_tokens = (uint32_t)rl->tokens + add;
        rl->tokens = min_u16((uint16_t)(new_tokens > UINT16_MAX ? UINT16_MAX : new_tokens),
                             rl->policy->max_tokens);

        /* Advance last_refill by whole intervals only (preserve remainder) */
        rl->last_refill += intervals * rl->policy->refill_ms;
    }
}

mres_err_t mres_ratelimit_init(mres_ratelimit_t *rl,
                               const mres_ratelimit_policy_t *policy,
                               mres_clock_fn clock)
{
    MRES_CHECK_NULL(rl);
    MRES_CHECK_NULL(policy);
    MRES_CHECK_NULL(clock);

    if (policy->max_tokens == 0 || policy->refill_ms == 0) {
        return MRES_ERR_INVALID;
    }

    rl->policy      = policy;
    rl->tokens      = policy->max_tokens;
    rl->last_refill = clock();

    return MRES_OK;
}

bool mres_ratelimit_acquire(mres_ratelimit_t *rl, uint16_t count,
                            mres_clock_fn clock)
{
    if (rl == NULL || clock == NULL || rl->policy == NULL || count == 0) {
        return false;
    }

    ratelimit_refill(rl, clock());

    if (rl->tokens >= count) {
        rl->tokens -= count;
        return true;
    }

    return false;
}

uint16_t mres_ratelimit_tokens(mres_ratelimit_t *rl, mres_clock_fn clock)
{
    if (rl == NULL || clock == NULL || rl->policy == NULL) {
        return 0;
    }

    ratelimit_refill(rl, clock());
    return rl->tokens;
}

mres_err_t mres_ratelimit_reset(mres_ratelimit_t *rl, mres_clock_fn clock)
{
    MRES_CHECK_NULL(rl);
    MRES_CHECK_NULL(clock);

    rl->tokens      = rl->policy->max_tokens;
    rl->last_refill = clock();

    return MRES_OK;
}
