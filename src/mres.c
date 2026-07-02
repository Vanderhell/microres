/*
 * microres - Implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mres.h"

#define MRES_RETRY_MAGIC 0x4D525259u
#define MRES_BREAKER_MAGIC 0x4D524252u
#define MRES_RATELIMIT_MAGIC 0x4D52524Cu
#define MRES_DEFAULT_JITTER_SEED 0x9E3779B9u

static mres_err_t mres_validate_retry_policy(const mres_retry_policy_t *policy);
static mres_err_t mres_validate_breaker_policy(const mres_breaker_policy_t *policy);
static mres_err_t mres_validate_ratelimit_policy(const mres_ratelimit_policy_t *policy);
static bool mres_breaker_state_valid(uint8_t state);

#ifdef MRES_ASSERT
#define MRES_DIAG_ASSERT(expr)                                                     \
    do {                                                                           \
        int mres_assert_once_ = ((expr) ? 1 : 0);                                  \
        if (mres_assert_once_ == 0) {                                              \
            MRES_ASSERT(mres_assert_once_ != 0);                                   \
        }                                                                          \
    } while (0)
#else
#define MRES_DIAG_ASSERT(expr) do { (void)(expr); } while (0)
#endif

static uint32_t mres_elapsed_ms(uint32_t start, uint32_t now)
{
    return now - start;
}

static uint32_t mres_clamp_delay(uint32_t value, uint32_t cap)
{
    if ((cap != 0u) && (value > cap)) {
        return cap;
    }

    return value;
}

static uint32_t mres_saturating_mul_u32(uint32_t left, uint32_t right)
{
    if ((left == 0u) || (right == 0u)) {
        return 0u;
    }

    if (left > (UINT32_MAX / right)) {
        return UINT32_MAX;
    }

    return left * right;
}

static uint32_t mres_next_jitter(uint32_t *state)
{
    uint32_t value = *state;

    if (value == 0u) {
        value = MRES_DEFAULT_JITTER_SEED;
    }

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;

    return value;
}

static uint32_t mres_apply_jitter(uint32_t delay, uint32_t random_value)
{
#if MRES_ENABLE_JITTER
    uint32_t quarter;
    uint32_t range;
    uint32_t offset;

    if (delay == 0u) {
        return 0u;
    }

    quarter = delay / 4u;
    if (quarter == 0u) {
        return delay;
    }

    range = quarter * 2u;
    offset = random_value % (range + 1u);
    return (delay - quarter) + offset;
#else
    (void)random_value;
    return delay;
#endif
}

static bool mres_retry_is_ready(const mres_retry_t *retry)
{
    return (retry != NULL) && (retry->magic == MRES_RETRY_MAGIC) && (retry->initialized != 0u);
}

static bool mres_breaker_is_ready(const mres_breaker_t *breaker)
{
    return (breaker != NULL) && (breaker->magic == MRES_BREAKER_MAGIC) &&
           (breaker->initialized != 0u);
}

static bool mres_ratelimit_is_ready(const mres_ratelimit_t *limiter)
{
    return (limiter != NULL) && (limiter->magic == MRES_RATELIMIT_MAGIC) &&
           (limiter->initialized != 0u);
}

static bool mres_retry_is_busy_instance(const mres_retry_t *retry)
{
    return mres_retry_is_ready(retry) && (retry->active != 0u) &&
           (mres_validate_retry_policy(&retry->policy) == MRES_OK);
}

static bool mres_breaker_is_busy_instance(const mres_breaker_t *breaker)
{
    return mres_breaker_is_ready(breaker) && (breaker->active != 0u) &&
           mres_breaker_state_valid(breaker->state) &&
           (mres_validate_breaker_policy(&breaker->policy) == MRES_OK);
}

static bool mres_ratelimit_is_busy_instance(const mres_ratelimit_t *limiter)
{
    return mres_ratelimit_is_ready(limiter) && (limiter->active != 0u) &&
           (mres_validate_ratelimit_policy(&limiter->policy) == MRES_OK);
}

static mres_err_t mres_validate_retry_policy(const mres_retry_policy_t *policy)
{
    if (policy == NULL) {
        MRES_DIAG_ASSERT(policy != NULL);
        return MRES_ERR_NULL;
    }

    if (policy->max_attempts == 0u) {
        return MRES_ERR_INVALID;
    }

#if (MRES_MAX_ATTEMPTS < UINT8_MAX)
    if (policy->max_attempts > (uint8_t)MRES_MAX_ATTEMPTS) {
        return MRES_ERR_RANGE;
    }
#endif

    if ((policy->strategy != MRES_BACKOFF_FIXED) &&
        (policy->strategy != MRES_BACKOFF_LINEAR) &&
        (policy->strategy != MRES_BACKOFF_EXPONENTIAL)) {
        return MRES_ERR_INVALID;
    }

    if ((policy->jitter != 0u) && (policy->jitter != 1u)) {
        return MRES_ERR_INVALID;
    }

#if !MRES_ENABLE_JITTER
    if (policy->jitter != 0u) {
        return MRES_ERR_UNSUPPORTED;
    }
#endif

    return MRES_OK;
}

static mres_err_t mres_validate_breaker_policy(const mres_breaker_policy_t *policy)
{
    if (policy == NULL) {
        MRES_DIAG_ASSERT(policy != NULL);
        return MRES_ERR_NULL;
    }

    if (policy->failure_threshold == 0u) {
        return MRES_ERR_INVALID;
    }

    if (policy->recovery_timeout_ms == 0u) {
        return MRES_ERR_INVALID;
    }

    if (policy->half_open_max_calls != 1u) {
        return MRES_ERR_UNSUPPORTED;
    }

    return MRES_OK;
}

static mres_err_t mres_validate_ratelimit_policy(const mres_ratelimit_policy_t *policy)
{
    if (policy == NULL) {
        MRES_DIAG_ASSERT(policy != NULL);
        return MRES_ERR_NULL;
    }

    if (policy->max_tokens == 0u) {
        return MRES_ERR_INVALID;
    }

    if (policy->refill_ms == 0u) {
        return MRES_ERR_INVALID;
    }

    if (policy->refill_count == 0u) {
        return MRES_ERR_INVALID;
    }

    return MRES_OK;
}

static mres_err_t mres_retry_delay_value(mres_retry_t *retry, uint8_t attempt, uint32_t *delay_ms)
{
    uint32_t delay = 0u;

    if (retry == NULL) {
        MRES_DIAG_ASSERT(retry != NULL);
        return MRES_ERR_NULL;
    }

    if (delay_ms == NULL) {
        MRES_DIAG_ASSERT(delay_ms != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_retry_is_ready(retry)) {
        return MRES_ERR_INVALID;
    }

    switch (retry->policy.strategy) {
    case MRES_BACKOFF_FIXED:
        delay = retry->policy.base_delay_ms;
        break;

    case MRES_BACKOFF_LINEAR:
        delay = mres_saturating_mul_u32(retry->policy.base_delay_ms, (uint32_t)attempt + 1u);
        break;

    case MRES_BACKOFF_EXPONENTIAL: {
        uint32_t multiplier = 1u;
        uint8_t shift_count = attempt;

        while (shift_count > 0u) {
            if (multiplier > (UINT32_MAX / 2u)) {
                multiplier = UINT32_MAX;
                break;
            }

            multiplier *= 2u;
            shift_count--;
        }

        delay = mres_saturating_mul_u32(retry->policy.base_delay_ms, multiplier);
        break;
    }

    default:
        return MRES_ERR_INVALID;
    }

    delay = mres_clamp_delay(delay, retry->policy.max_delay_ms);

#if MRES_ENABLE_JITTER
    if (retry->policy.jitter != 0u) {
        delay = mres_apply_jitter(delay, mres_next_jitter(&retry->jitter_state));
        delay = mres_clamp_delay(delay, retry->policy.max_delay_ms);
    }
#endif

    *delay_ms = delay;
    return MRES_OK;
}

static bool mres_breaker_state_valid(uint8_t state)
{
    return (state == MRES_BREAKER_CLOSED) || (state == MRES_BREAKER_OPEN) ||
           (state == MRES_BREAKER_HALF_OPEN);
}

static mres_err_t mres_platform_clock_now(const mres_platform_t *platform, uint32_t *now_ms)
{
    if (platform == NULL) {
        MRES_DIAG_ASSERT(platform != NULL);
        return MRES_ERR_NULL;
    }

    if (platform->clock == NULL) {
        MRES_DIAG_ASSERT(platform->clock != NULL);
        return MRES_ERR_NULL;
    }

    if (now_ms == NULL) {
        MRES_DIAG_ASSERT(now_ms != NULL);
        return MRES_ERR_NULL;
    }

    *now_ms = platform->clock(platform->context);
    return MRES_OK;
}

static mres_err_t mres_breaker_trip_now(mres_breaker_t *breaker, const mres_platform_t *platform)
{
    uint32_t now_ms = 0u;
    mres_err_t err = mres_platform_clock_now(platform, &now_ms);

    if (err != MRES_OK) {
        return err;
    }

    breaker->state = MRES_BREAKER_OPEN;
    breaker->opened_at_ms = now_ms;
    return MRES_OK;
}

static mres_err_t mres_breaker_close(mres_breaker_t *breaker)
{
    breaker->state = MRES_BREAKER_CLOSED;
    breaker->failure_count = 0u;
    breaker->opened_at_ms = 0u;
    return MRES_OK;
}

static mres_err_t mres_ratelimit_refill(mres_ratelimit_t *limiter, const mres_platform_t *platform)
{
    uint32_t now_ms = 0u;
    uint32_t elapsed = 0u;
    uint32_t intervals = 0u;
    uint32_t missing = 0u;
    uint32_t intervals_needed = 0u;
    uint32_t advanced = 0u;
    mres_err_t err = MRES_OK;

    err = mres_platform_clock_now(platform, &now_ms);
    if (err != MRES_OK) {
        return err;
    }

    elapsed = mres_elapsed_ms(limiter->last_refill_ms, now_ms);
    if (elapsed < limiter->policy.refill_ms) {
        return MRES_OK;
    }

    intervals = elapsed / limiter->policy.refill_ms;
    advanced = elapsed - (elapsed % limiter->policy.refill_ms);

    if (limiter->tokens >= limiter->policy.max_tokens) {
        limiter->tokens = limiter->policy.max_tokens;
        limiter->last_refill_ms += advanced;
        return MRES_OK;
    }

    missing = (uint32_t)limiter->policy.max_tokens - (uint32_t)limiter->tokens;
    intervals_needed = (missing + (uint32_t)limiter->policy.refill_count - 1u) /
                       (uint32_t)limiter->policy.refill_count;

    if (intervals >= intervals_needed) {
        limiter->tokens = limiter->policy.max_tokens;
    } else {
        limiter->tokens = (uint16_t)((uint32_t)limiter->tokens +
                                     (intervals * (uint32_t)limiter->policy.refill_count));
    }

    limiter->last_refill_ms += advanced;
    return MRES_OK;
}

const char *mres_err_str(mres_err_t err)
{
    switch (err) {
    case MRES_OK:
        return "ok";
    case MRES_ERR_NULL:
        return "null argument";
    case MRES_ERR_INVALID:
        return "invalid state or configuration";
    case MRES_ERR_RANGE:
        return "value out of range";
    case MRES_ERR_BUSY:
        return "instance busy";
    case MRES_ERR_EXHAUSTED:
        return "attempts exhausted";
    case MRES_ERR_OPEN:
        return "breaker open";
    case MRES_ERR_OP_FAILED:
        return "operation failed";
    case MRES_ERR_WAIT_REQUIRED:
        return "wait callback required";
    case MRES_ERR_WAIT_FAILED:
        return "wait callback failed";
    case MRES_ERR_UNSUPPORTED:
        return "unsupported configuration";
    default:
        return "unknown error";
    }
}

mres_err_t mres_retry_init(mres_retry_t *retry, const mres_retry_policy_t *policy)
{
    mres_retry_t next_value;
    mres_err_t err = MRES_OK;

    if (retry == NULL) {
        MRES_DIAG_ASSERT(retry != NULL);
        return MRES_ERR_NULL;
    }

    if (mres_retry_is_busy_instance(retry)) {
        return MRES_ERR_BUSY;
    }

    err = mres_validate_retry_policy(policy);
    if (err != MRES_OK) {
        return err;
    }

    next_value.magic = MRES_RETRY_MAGIC;
    next_value.jitter_state = MRES_DEFAULT_JITTER_SEED;
    next_value.policy = *policy;
    next_value.last_operation_result = 0;
    next_value.attempts = 0u;
    next_value.initialized = 1u;
    next_value.active = 0u;
    next_value.reserved0 = 0u;
    *retry = next_value;

    return MRES_OK;
}

mres_err_t mres_retry_seed(mres_retry_t *retry, uint32_t seed)
{
    if (retry == NULL) {
        MRES_DIAG_ASSERT(retry != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_retry_is_ready(retry)) {
        return MRES_ERR_INVALID;
    }

    if (retry->active != 0u) {
        return MRES_ERR_BUSY;
    }

    retry->jitter_state = (seed == 0u) ? MRES_DEFAULT_JITTER_SEED : seed;
    return MRES_OK;
}

mres_err_t mres_retry_exec(
    mres_retry_t *retry,
    mres_op_fn operation,
    void *operation_context,
    const mres_platform_t *platform,
    int *operation_result)
{
    uint8_t attempt = 0u;

    if (retry == NULL) {
        MRES_DIAG_ASSERT(retry != NULL);
        return MRES_ERR_NULL;
    }

    if (operation == NULL) {
        MRES_DIAG_ASSERT(operation != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_retry_is_ready(retry)) {
        return MRES_ERR_INVALID;
    }

    if (retry->active != 0u) {
        return MRES_ERR_BUSY;
    }

    retry->active = 1u;
    retry->attempts = 0u;
    retry->last_operation_result = 0;

    for (attempt = 0u; attempt < retry->policy.max_attempts; ++attempt) {
        uint32_t delay_ms = 0u;
        int result = operation(operation_context);

        retry->attempts = (uint8_t)(attempt + 1u);
        retry->last_operation_result = result;

        if (operation_result != NULL) {
            *operation_result = result;
        }

        if (result == 0) {
            retry->active = 0u;
            return MRES_OK;
        }

        if ((uint8_t)(attempt + 1u) >= retry->policy.max_attempts) {
            break;
        }

        (void)mres_retry_delay_value(retry, attempt, &delay_ms);

        if (delay_ms == 0u) {
            continue;
        }

        if ((platform == NULL) || (platform->wait == NULL)) {
            retry->active = 0u;
            return MRES_ERR_WAIT_REQUIRED;
        }

        if (platform->wait(platform->context, delay_ms) != 0) {
            retry->active = 0u;
            return MRES_ERR_WAIT_FAILED;
        }
    }

    retry->active = 0u;
    return MRES_ERR_EXHAUSTED;
}

mres_err_t mres_retry_reset(mres_retry_t *retry)
{
    if (retry == NULL) {
        MRES_DIAG_ASSERT(retry != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_retry_is_ready(retry)) {
        return MRES_ERR_INVALID;
    }

    if (retry->active != 0u) {
        return MRES_ERR_BUSY;
    }

    retry->attempts = 0u;
    retry->last_operation_result = 0;
    retry->jitter_state = MRES_DEFAULT_JITTER_SEED;
    return MRES_OK;
}

mres_err_t mres_delay_calc(mres_retry_t *retry, uint8_t attempt, uint32_t *delay_ms)
{
    if (retry == NULL) {
        MRES_DIAG_ASSERT(retry != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_retry_is_ready(retry)) {
        return MRES_ERR_INVALID;
    }

    if (retry->active != 0u) {
        return MRES_ERR_BUSY;
    }

    return mres_retry_delay_value(retry, attempt, delay_ms);
}

mres_err_t mres_breaker_init(mres_breaker_t *breaker, const mres_breaker_policy_t *policy)
{
    mres_breaker_t next_value;
    mres_err_t err = MRES_OK;

    if (breaker == NULL) {
        MRES_DIAG_ASSERT(breaker != NULL);
        return MRES_ERR_NULL;
    }

    if (mres_breaker_is_busy_instance(breaker)) {
        return MRES_ERR_BUSY;
    }

    err = mres_validate_breaker_policy(policy);
    if (err != MRES_OK) {
        return err;
    }

    next_value.magic = MRES_BREAKER_MAGIC;
    next_value.policy = *policy;
    next_value.opened_at_ms = 0u;
    next_value.state = MRES_BREAKER_CLOSED;
    next_value.failure_count = 0u;
    next_value.initialized = 1u;
    next_value.active = 0u;
    *breaker = next_value;

    return MRES_OK;
}

mres_err_t mres_breaker_call(
    mres_breaker_t *breaker,
    mres_op_fn operation,
    void *operation_context,
    const mres_platform_t *platform,
    int *operation_result)
{
    int result = 0;
    uint32_t now_ms = 0u;
    mres_err_t err;

    if (breaker == NULL) {
        MRES_DIAG_ASSERT(breaker != NULL);
        return MRES_ERR_NULL;
    }

    if (operation == NULL) {
        MRES_DIAG_ASSERT(operation != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_breaker_is_ready(breaker)) {
        return MRES_ERR_INVALID;
    }

    if (breaker->active != 0u) {
        return MRES_ERR_BUSY;
    }

    if (!mres_breaker_state_valid(breaker->state)) {
        return MRES_ERR_INVALID;
    }

    breaker->active = 1u;

    if (breaker->state == MRES_BREAKER_OPEN) {
        err = mres_platform_clock_now(platform, &now_ms);
        if (err != MRES_OK) {
            breaker->active = 0u;
            return err;
        }

        if (mres_elapsed_ms(breaker->opened_at_ms, now_ms) < breaker->policy.recovery_timeout_ms) {
            breaker->active = 0u;
            return MRES_ERR_OPEN;
        }

        breaker->state = MRES_BREAKER_HALF_OPEN;
    } else if (breaker->state == MRES_BREAKER_HALF_OPEN) {
        breaker->active = 0u;
        return MRES_ERR_INVALID;
    }

    result = operation(operation_context);
    if (operation_result != NULL) {
        *operation_result = result;
    }

    if (result == 0) {
        (void)mres_breaker_close(breaker);
        breaker->active = 0u;
        return MRES_OK;
    }

    if (breaker->state == MRES_BREAKER_HALF_OPEN) {
        err = mres_breaker_trip_now(breaker, platform);
        breaker->active = 0u;
        return (err == MRES_OK) ? MRES_ERR_OP_FAILED : err;
    }

    if (breaker->failure_count < UINT8_MAX) {
        breaker->failure_count++;
    }

    if (breaker->failure_count >= breaker->policy.failure_threshold) {
        err = mres_breaker_trip_now(breaker, platform);
        breaker->active = 0u;
        return (err == MRES_OK) ? MRES_ERR_OP_FAILED : err;
    }

    breaker->active = 0u;
    return MRES_ERR_OP_FAILED;
}

mres_err_t mres_breaker_get_state(const mres_breaker_t *breaker, mres_breaker_state_t *state)
{
    if (breaker == NULL) {
        MRES_DIAG_ASSERT(breaker != NULL);
        return MRES_ERR_NULL;
    }

    if (state == NULL) {
        MRES_DIAG_ASSERT(state != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_breaker_is_ready(breaker)) {
        return MRES_ERR_INVALID;
    }

    if (!mres_breaker_state_valid(breaker->state)) {
        return MRES_ERR_INVALID;
    }

    *state = breaker->state;
    return MRES_OK;
}

mres_err_t mres_breaker_state_name(const mres_breaker_t *breaker, const char **name)
{
    if (breaker == NULL) {
        MRES_DIAG_ASSERT(breaker != NULL);
        return MRES_ERR_NULL;
    }

    if (name == NULL) {
        MRES_DIAG_ASSERT(name != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_breaker_is_ready(breaker)) {
        return MRES_ERR_INVALID;
    }

    switch (breaker->state) {
    case MRES_BREAKER_CLOSED:
        *name = "closed";
        return MRES_OK;

    case MRES_BREAKER_OPEN:
        *name = "open";
        return MRES_OK;

    case MRES_BREAKER_HALF_OPEN:
        *name = "half_open";
        return MRES_OK;

    default:
        return MRES_ERR_INVALID;
    }
}

mres_err_t mres_breaker_remaining_ms(
    const mres_breaker_t *breaker,
    const mres_platform_t *platform,
    uint32_t *remaining_ms,
    bool *is_open)
{
    uint32_t now_ms = 0u;
    uint32_t elapsed = 0u;
    mres_err_t err = MRES_OK;

    if (breaker == NULL) {
        MRES_DIAG_ASSERT(breaker != NULL);
        return MRES_ERR_NULL;
    }

    if ((remaining_ms == NULL) || (is_open == NULL)) {
        MRES_DIAG_ASSERT((remaining_ms != NULL) && (is_open != NULL));
        return MRES_ERR_NULL;
    }

    if (!mres_breaker_is_ready(breaker)) {
        return MRES_ERR_INVALID;
    }

    if (!mres_breaker_state_valid(breaker->state)) {
        return MRES_ERR_INVALID;
    }

    if (breaker->state != MRES_BREAKER_OPEN) {
        *remaining_ms = 0u;
        *is_open = false;
        return MRES_OK;
    }

    err = mres_platform_clock_now(platform, &now_ms);
    if (err != MRES_OK) {
        return err;
    }

    elapsed = mres_elapsed_ms(breaker->opened_at_ms, now_ms);
    *is_open = true;

    if (elapsed >= breaker->policy.recovery_timeout_ms) {
        *remaining_ms = 0u;
        return MRES_OK;
    }

    *remaining_ms = breaker->policy.recovery_timeout_ms - elapsed;
    return MRES_OK;
}

mres_err_t mres_breaker_reset(mres_breaker_t *breaker)
{
    if (breaker == NULL) {
        MRES_DIAG_ASSERT(breaker != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_breaker_is_ready(breaker)) {
        return MRES_ERR_INVALID;
    }

    if (breaker->active != 0u) {
        return MRES_ERR_BUSY;
    }

    if (!mres_breaker_state_valid(breaker->state)) {
        return MRES_ERR_INVALID;
    }

    return mres_breaker_close(breaker);
}

mres_err_t mres_breaker_report_success(mres_breaker_t *breaker)
{
    if (breaker == NULL) {
        MRES_DIAG_ASSERT(breaker != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_breaker_is_ready(breaker)) {
        return MRES_ERR_INVALID;
    }

    if (breaker->active != 0u) {
        return MRES_ERR_BUSY;
    }

    switch (breaker->state) {
    case MRES_BREAKER_CLOSED:
        breaker->failure_count = 0u;
        return MRES_OK;

    case MRES_BREAKER_HALF_OPEN:
        return mres_breaker_close(breaker);

    case MRES_BREAKER_OPEN:
        return MRES_OK;

    default:
        return MRES_ERR_INVALID;
    }
}

mres_err_t mres_breaker_report_failure(mres_breaker_t *breaker, const mres_platform_t *platform)
{
    mres_err_t err = MRES_OK;

    if (breaker == NULL) {
        MRES_DIAG_ASSERT(breaker != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_breaker_is_ready(breaker)) {
        return MRES_ERR_INVALID;
    }

    if (breaker->active != 0u) {
        return MRES_ERR_BUSY;
    }

    switch (breaker->state) {
    case MRES_BREAKER_CLOSED:
        if (breaker->failure_count < UINT8_MAX) {
            breaker->failure_count++;
        }

        if (breaker->failure_count >= breaker->policy.failure_threshold) {
            return mres_breaker_trip_now(breaker, platform);
        }

        return MRES_OK;

    case MRES_BREAKER_HALF_OPEN:
        err = mres_breaker_trip_now(breaker, platform);
        return err;

    case MRES_BREAKER_OPEN:
        return MRES_OK;

    default:
        return MRES_ERR_INVALID;
    }
}

mres_err_t mres_ratelimit_init(
    mres_ratelimit_t *limiter,
    const mres_ratelimit_policy_t *policy,
    const mres_platform_t *platform)
{
    uint32_t now_ms = 0u;
    mres_ratelimit_t next_value;
    mres_err_t err = MRES_OK;

    if (limiter == NULL) {
        MRES_DIAG_ASSERT(limiter != NULL);
        return MRES_ERR_NULL;
    }

    if (mres_ratelimit_is_busy_instance(limiter)) {
        return MRES_ERR_BUSY;
    }

    err = mres_validate_ratelimit_policy(policy);
    if (err != MRES_OK) {
        return err;
    }

    err = mres_platform_clock_now(platform, &now_ms);
    if (err != MRES_OK) {
        return err;
    }

    next_value.magic = MRES_RATELIMIT_MAGIC;
    next_value.policy = *policy;
    next_value.last_refill_ms = now_ms;
    next_value.tokens = policy->max_tokens;
    next_value.initialized = 1u;
    next_value.active = 0u;
    *limiter = next_value;

    return MRES_OK;
}

mres_err_t mres_ratelimit_acquire(
    mres_ratelimit_t *limiter,
    uint16_t count,
    const mres_platform_t *platform,
    bool *allowed)
{
    mres_err_t err = MRES_OK;

    if (limiter == NULL) {
        MRES_DIAG_ASSERT(limiter != NULL);
        return MRES_ERR_NULL;
    }

    if (allowed == NULL) {
        MRES_DIAG_ASSERT(allowed != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_ratelimit_is_ready(limiter)) {
        return MRES_ERR_INVALID;
    }

    if (limiter->active != 0u) {
        return MRES_ERR_BUSY;
    }

    if (count == 0u) {
        return MRES_ERR_INVALID;
    }

    if (count > limiter->policy.max_tokens) {
        return MRES_ERR_RANGE;
    }

    limiter->active = 1u;
    err = mres_ratelimit_refill(limiter, platform);
    if (err != MRES_OK) {
        limiter->active = 0u;
        return err;
    }

    if (limiter->tokens >= count) {
        limiter->tokens = (uint16_t)(limiter->tokens - count);
        *allowed = true;
    } else {
        *allowed = false;
    }

    limiter->active = 0u;
    return MRES_OK;
}

mres_err_t mres_ratelimit_tokens(
    mres_ratelimit_t *limiter,
    const mres_platform_t *platform,
    uint16_t *tokens)
{
    mres_err_t err = MRES_OK;

    if (limiter == NULL) {
        MRES_DIAG_ASSERT(limiter != NULL);
        return MRES_ERR_NULL;
    }

    if (tokens == NULL) {
        MRES_DIAG_ASSERT(tokens != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_ratelimit_is_ready(limiter)) {
        return MRES_ERR_INVALID;
    }

    if (limiter->active != 0u) {
        return MRES_ERR_BUSY;
    }

    limiter->active = 1u;
    err = mres_ratelimit_refill(limiter, platform);
    if (err != MRES_OK) {
        limiter->active = 0u;
        return err;
    }

    *tokens = limiter->tokens;
    limiter->active = 0u;
    return MRES_OK;
}

mres_err_t mres_ratelimit_reset(mres_ratelimit_t *limiter, const mres_platform_t *platform)
{
    uint32_t now_ms = 0u;
    mres_err_t err = MRES_OK;

    if (limiter == NULL) {
        MRES_DIAG_ASSERT(limiter != NULL);
        return MRES_ERR_NULL;
    }

    if (!mres_ratelimit_is_ready(limiter)) {
        return MRES_ERR_INVALID;
    }

    if (limiter->active != 0u) {
        return MRES_ERR_BUSY;
    }

    limiter->active = 1u;
    err = mres_platform_clock_now(platform, &now_ms);
    if (err != MRES_OK) {
        limiter->active = 0u;
        return err;
    }

    limiter->tokens = limiter->policy.max_tokens;
    limiter->last_refill_ms = now_ms;
    limiter->active = 0u;
    return MRES_OK;
}
