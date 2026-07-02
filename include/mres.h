/*
 * microres - Fault-tolerance primitives for caller-owned execution contexts.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MRES_H
#define MRES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(MRES_ENABLE_JITTER) || defined(MRES_MAX_ATTEMPTS)
#error "MRES_DIAG_CONFLICTING_CONFIG_OVERRIDE"
#endif

#ifdef MRES_USE_BOOL_FIELDS
#error "MRES_DIAG_UNSUPPORTED_ABI_CHANGE"
#endif

#ifndef MRES_CONFIG_HEADER
#define MRES_CONFIG_HEADER "mres_config.h"
#endif

#include MRES_CONFIG_HEADER

#if !defined(MRES_CONFIG_ABI_REVISION) || (MRES_CONFIG_ABI_REVISION != 1)
#error "MRES_DIAG_CONFLICTING_GENERATED_CONFIG"
#endif

#if ((MRES_ENABLE_JITTER != 0) && (MRES_ENABLE_JITTER != 1))
#error "MRES_DIAG_INVALID_ENABLE_JITTER"
#endif

#if ((MRES_MAX_ATTEMPTS < 1) || (MRES_MAX_ATTEMPTS > 255))
#error "MRES_DIAG_INVALID_MAX_ATTEMPTS"
#endif

typedef int32_t mres_err_t;
typedef uint8_t mres_backoff_t;
typedef uint8_t mres_breaker_state_t;

#define MRES_OK ((mres_err_t)0)
#define MRES_ERR_NULL ((mres_err_t)-1)
#define MRES_ERR_INVALID ((mres_err_t)-2)
#define MRES_ERR_RANGE ((mres_err_t)-3)
#define MRES_ERR_BUSY ((mres_err_t)-4)
#define MRES_ERR_EXHAUSTED ((mres_err_t)-5)
#define MRES_ERR_OPEN ((mres_err_t)-6)
#define MRES_ERR_OP_FAILED ((mres_err_t)-7)
#define MRES_ERR_WAIT_REQUIRED ((mres_err_t)-8)
#define MRES_ERR_WAIT_FAILED ((mres_err_t)-9)
#define MRES_ERR_UNSUPPORTED ((mres_err_t)-10)

#define MRES_BACKOFF_FIXED ((mres_backoff_t)0u)
#define MRES_BACKOFF_LINEAR ((mres_backoff_t)1u)
#define MRES_BACKOFF_EXPONENTIAL ((mres_backoff_t)2u)

#define MRES_BREAKER_CLOSED ((mres_breaker_state_t)0u)
#define MRES_BREAKER_OPEN ((mres_breaker_state_t)1u)
#define MRES_BREAKER_HALF_OPEN ((mres_breaker_state_t)2u)

typedef uint32_t (*mres_clock_fn)(void *context);
typedef int (*mres_wait_fn)(void *context, uint32_t delay_ms);
typedef int (*mres_op_fn)(void *context);

typedef struct {
    void *context;
    mres_clock_fn clock;
    mres_wait_fn wait;
} mres_platform_t;

typedef struct {
    uint8_t max_attempts;
    uint8_t strategy;
    uint8_t jitter;
    uint8_t reserved0;
    uint32_t base_delay_ms;
    uint32_t max_delay_ms;
} mres_retry_policy_t;

typedef struct {
    uint8_t failure_threshold;
    uint8_t half_open_max_calls;
    uint8_t reserved0;
    uint8_t reserved1;
    uint32_t recovery_timeout_ms;
} mres_breaker_policy_t;

typedef struct {
    uint16_t max_tokens;
    uint16_t refill_count;
    uint32_t refill_ms;
} mres_ratelimit_policy_t;

typedef struct {
    uint32_t magic;
    uint32_t jitter_state;
    mres_retry_policy_t policy;
    int32_t last_operation_result;
    uint8_t attempts;
    uint8_t initialized;
    uint8_t active;
    uint8_t reserved0;
} mres_retry_t;

typedef struct {
    uint32_t magic;
    mres_breaker_policy_t policy;
    uint32_t opened_at_ms;
    uint8_t state;
    uint8_t failure_count;
    uint8_t initialized;
    uint8_t active;
} mres_breaker_t;

typedef struct {
    uint32_t magic;
    mres_ratelimit_policy_t policy;
    uint32_t last_refill_ms;
    uint16_t tokens;
    uint8_t initialized;
    uint8_t active;
} mres_ratelimit_t;

const char *mres_err_str(mres_err_t err);

mres_err_t mres_retry_init(mres_retry_t *retry, const mres_retry_policy_t *policy);
mres_err_t mres_retry_seed(mres_retry_t *retry, uint32_t seed);
mres_err_t mres_retry_exec(
    mres_retry_t *retry,
    mres_op_fn operation,
    void *operation_context,
    const mres_platform_t *platform,
    int *operation_result);
mres_err_t mres_retry_reset(mres_retry_t *retry);
mres_err_t mres_delay_calc(mres_retry_t *retry, uint8_t attempt, uint32_t *delay_ms);

mres_err_t mres_breaker_init(mres_breaker_t *breaker, const mres_breaker_policy_t *policy);
mres_err_t mres_breaker_call(
    mres_breaker_t *breaker,
    mres_op_fn operation,
    void *operation_context,
    const mres_platform_t *platform,
    int *operation_result);
mres_err_t mres_breaker_get_state(const mres_breaker_t *breaker, mres_breaker_state_t *state);
mres_err_t mres_breaker_state_name(const mres_breaker_t *breaker, const char **name);
mres_err_t mres_breaker_remaining_ms(
    const mres_breaker_t *breaker,
    const mres_platform_t *platform,
    uint32_t *remaining_ms,
    bool *is_open);
mres_err_t mres_breaker_reset(mres_breaker_t *breaker);
mres_err_t mres_breaker_report_success(mres_breaker_t *breaker);
mres_err_t mres_breaker_report_failure(mres_breaker_t *breaker, const mres_platform_t *platform);

mres_err_t mres_ratelimit_init(
    mres_ratelimit_t *limiter,
    const mres_ratelimit_policy_t *policy,
    const mres_platform_t *platform);
mres_err_t mres_ratelimit_acquire(
    mres_ratelimit_t *limiter,
    uint16_t count,
    const mres_platform_t *platform,
    bool *allowed);
mres_err_t mres_ratelimit_tokens(
    mres_ratelimit_t *limiter,
    const mres_platform_t *platform,
    uint16_t *tokens);
mres_err_t mres_ratelimit_reset(mres_ratelimit_t *limiter, const mres_platform_t *platform);

#ifdef __cplusplus
}
#endif

#endif /* MRES_H */
