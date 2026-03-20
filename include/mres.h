/*
 * microres — Fault-tolerance primitives for embedded systems.
 *
 * C99 · Zero dependencies · Zero allocations · Platform-agnostic · Portable
 *
 * SPDX-License-Identifier: MIT
 * https://github.com/Vanderhell/microres
 */

#ifndef MRES_H
#define MRES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Configuration ─────────────────────────────────────────────────────── */

#ifndef MRES_ENABLE_JITTER
#define MRES_ENABLE_JITTER 1
#endif

#ifndef MRES_MAX_ATTEMPTS
#define MRES_MAX_ATTEMPTS 255
#endif

/* ── Assert ────────────────────────────────────────────────────────────── */

#ifdef MRES_ASSERT
#define MRES_CHECK_NULL(ptr) do { MRES_ASSERT((ptr) != NULL); } while (0)
#else
#define MRES_CHECK_NULL(ptr) do { if ((ptr) == NULL) return MRES_ERR_NULL; } while (0)
#endif

/* ── Platform callbacks ────────────────────────────────────────────────── */

/**
 * Clock function — returns current time in milliseconds.
 *
 * Must be monotonic (non-decreasing). Wrapping at UINT32_MAX is handled
 * internally. Examples: HAL_GetTick, xTaskGetTickCount * portTICK_PERIOD_MS,
 * clock_gettime(CLOCK_MONOTONIC).
 */
typedef uint32_t (*mres_clock_fn)(void);

/**
 * Sleep function — blocks for the given number of milliseconds.
 *
 * Examples: HAL_Delay, vTaskDelay(pdMS_TO_TICKS(ms)), usleep(ms * 1000).
 * May be NULL for non-blocking usage (retry returns between attempts).
 */
typedef void (*mres_sleep_fn)(uint32_t ms);

/**
 * Operation function — the call being protected.
 *
 * @param ctx  Application context (opaque pointer).
 * @return 0 on success, negative on failure. The exact negative value is
 *         passed through as the error result.
 */
typedef int (*mres_op_fn)(void *ctx);

/* ── Error codes ───────────────────────────────────────────────────────── */

typedef enum {
    MRES_OK             =  0,   /**< Success.                                */
    MRES_ERR_NULL       = -1,   /**< NULL pointer argument.                  */
    MRES_ERR_EXHAUSTED  = -2,   /**< All retry attempts failed.              */
    MRES_ERR_OPEN       = -3,   /**< Circuit breaker is open (call blocked). */
    MRES_ERR_RATE_LIMITED = -4, /**< Rate limit exceeded.                    */
    MRES_ERR_OP_FAILED  = -5,   /**< Operation returned failure.             */
    MRES_ERR_INVALID    = -6,   /**< Invalid configuration.                  */
} mres_err_t;

/** Convert error code to human-readable string. */
const char *mres_err_str(mres_err_t err);

/* ═══════════════════════════════════════════════════════════════════════════
 * Retry
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Backoff strategy for retry delays. */
typedef enum {
    MRES_BACKOFF_FIXED       = 0,   /**< Constant delay.                    */
    MRES_BACKOFF_LINEAR      = 1,   /**< base * attempt.                    */
    MRES_BACKOFF_EXPONENTIAL = 2,   /**< base * 2^(attempt-1).              */
} mres_backoff_t;

/**
 * Retry policy — immutable configuration. Can be const / ROM.
 */
typedef struct {
    uint8_t        max_attempts;    /**< Maximum attempts (1 = no retry).    */
    uint32_t       base_delay_ms;   /**< Base delay between attempts.        */
    uint32_t       max_delay_ms;    /**< Delay cap (0 = no cap).             */
    mres_backoff_t strategy;        /**< Backoff strategy.                   */
    bool           jitter;          /**< Add ±25% random jitter to delay.    */
} mres_retry_policy_t;

/**
 * Retry state — runtime instance. One per operation in flight.
 */
typedef struct {
    const mres_retry_policy_t *policy;     /**< Pointer to policy.           */
    uint8_t                    attempts;   /**< Attempts made so far.        */
    int                        last_error; /**< Last operation return value.  */
} mres_retry_t;

/**
 * Initialise retry state.
 *
 * @param retry   Instance (caller-allocated).
 * @param policy  Retry policy (const, ROM-safe).
 * @return MRES_OK on success.
 */
mres_err_t mres_retry_init(mres_retry_t *retry, const mres_retry_policy_t *policy);

/**
 * Execute an operation with retry and backoff.
 *
 * Calls op(ctx) up to policy->max_attempts times. Between attempts, sleeps
 * for the calculated backoff delay. If sleep is NULL, returns MRES_ERR_OP_FAILED
 * after each failed attempt (caller controls timing).
 *
 * @param retry  Initialised retry instance.
 * @param op     Operation to retry.
 * @param ctx    Context passed to op.
 * @param clock  Clock function (used for jitter seed). May be NULL if jitter disabled.
 * @param sleep  Sleep function. May be NULL for manual stepping.
 * @return MRES_OK if op succeeded, MRES_ERR_EXHAUSTED if all attempts failed.
 */
int mres_retry_exec(mres_retry_t *retry, mres_op_fn op, void *ctx,
                    mres_clock_fn clock, mres_sleep_fn sleep);

/**
 * Reset retry state for reuse.
 */
mres_err_t mres_retry_reset(mres_retry_t *retry);

/**
 * Calculate the delay for a given attempt number (0-based).
 * Useful for logging or custom retry loops.
 *
 * @param policy   Retry policy.
 * @param attempt  Attempt number (0 = first retry delay).
 * @param clock    Clock function (for jitter seed). May be NULL.
 * @return Delay in milliseconds.
 */
uint32_t mres_delay_calc(const mres_retry_policy_t *policy, uint8_t attempt,
                         mres_clock_fn clock);

/* ═══════════════════════════════════════════════════════════════════════════
 * Circuit Breaker
 * ═══════════════════════════════════════════════════════════════════════════ */

/** Circuit breaker states. */
typedef enum {
    MRES_BREAKER_CLOSED    = 0,   /**< Normal — calls pass through.         */
    MRES_BREAKER_OPEN      = 1,   /**< Tripped — calls are blocked.         */
    MRES_BREAKER_HALF_OPEN = 2,   /**< Probing — limited calls allowed.     */
} mres_breaker_state_t;

/**
 * Circuit breaker policy — immutable configuration.
 */
typedef struct {
    uint8_t  failure_threshold;    /**< Consecutive failures to trip.        */
    uint32_t recovery_timeout_ms;  /**< Time in OPEN before trying HALF_OPEN.*/
    uint8_t  half_open_max_calls;  /**< Max probe calls in HALF_OPEN.        */
} mres_breaker_policy_t;

/**
 * Circuit breaker instance — runtime state.
 */
typedef struct {
    const mres_breaker_policy_t *policy;          /**< Pointer to policy.    */
    mres_breaker_state_t         state;           /**< Current state.        */
    uint8_t                      failure_count;   /**< Consecutive failures. */
    uint8_t                      half_open_calls; /**< Calls made in HALF_OPEN.*/
    uint32_t                     opened_at;       /**< Timestamp when opened.*/
} mres_breaker_t;

/**
 * Initialise circuit breaker (starts in CLOSED state).
 */
mres_err_t mres_breaker_init(mres_breaker_t *br, const mres_breaker_policy_t *policy);

/**
 * Execute an operation through the circuit breaker.
 *
 * - CLOSED: calls op(ctx). On failure, increments failure count. If threshold
 *   reached, transitions to OPEN.
 * - OPEN: returns MRES_ERR_OPEN immediately without calling op. If recovery
 *   timeout has elapsed, transitions to HALF_OPEN and falls through.
 * - HALF_OPEN: calls op(ctx) up to half_open_max_calls. On success,
 *   transitions to CLOSED. On failure, transitions back to OPEN.
 *
 * @param br     Initialised breaker instance.
 * @param op     Operation to execute.
 * @param ctx    Context passed to op.
 * @param clock  Clock function (required).
 * @return MRES_OK on success, MRES_ERR_OPEN if blocked, or the op's error.
 */
int mres_breaker_call(mres_breaker_t *br, mres_op_fn op, void *ctx,
                      mres_clock_fn clock);

/** Get current breaker state. */
mres_breaker_state_t mres_breaker_state(const mres_breaker_t *br);

/** Get state as human-readable string. */
const char *mres_breaker_state_name(const mres_breaker_t *br);

/**
 * Get remaining milliseconds until recovery attempt.
 * Returns 0 if breaker is not in OPEN state.
 */
uint32_t mres_breaker_remaining_ms(const mres_breaker_t *br, mres_clock_fn clock);

/** Force breaker back to CLOSED state. */
mres_err_t mres_breaker_reset(mres_breaker_t *br);

/**
 * Manually report a success or failure without executing an operation.
 * Useful when the result comes from outside the breaker (e.g., async callback).
 */
mres_err_t mres_breaker_report_success(mres_breaker_t *br);
mres_err_t mres_breaker_report_failure(mres_breaker_t *br, mres_clock_fn clock);

/* ═══════════════════════════════════════════════════════════════════════════
 * Rate Limiter (Token Bucket)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Rate limiter policy — immutable configuration.
 */
typedef struct {
    uint16_t max_tokens;    /**< Bucket capacity.                           */
    uint32_t refill_ms;     /**< Refill interval in milliseconds.           */
    uint16_t refill_count;  /**< Tokens added each interval.                */
} mres_ratelimit_policy_t;

/**
 * Rate limiter instance — runtime state.
 */
typedef struct {
    const mres_ratelimit_policy_t *policy;       /**< Pointer to policy.    */
    uint16_t                       tokens;       /**< Current token count.  */
    uint32_t                       last_refill;  /**< Last refill timestamp.*/
} mres_ratelimit_t;

/**
 * Initialise rate limiter (starts with full tokens).
 */
mres_err_t mres_ratelimit_init(mres_ratelimit_t *rl,
                               const mres_ratelimit_policy_t *policy,
                               mres_clock_fn clock);

/**
 * Try to consume tokens.
 *
 * Refills tokens based on elapsed time, then attempts to consume the
 * requested count. Returns true if tokens were available, false if not.
 *
 * @param rl     Initialised rate limiter.
 * @param count  Number of tokens to consume (usually 1).
 * @param clock  Clock function (required).
 * @return true if tokens consumed, false if rate limited.
 */
bool mres_ratelimit_acquire(mres_ratelimit_t *rl, uint16_t count,
                            mres_clock_fn clock);

/** Get current token count (after refill). */
uint16_t mres_ratelimit_tokens(mres_ratelimit_t *rl, mres_clock_fn clock);

/** Reset to full tokens. */
mres_err_t mres_ratelimit_reset(mres_ratelimit_t *rl, mres_clock_fn clock);

#ifdef __cplusplus
}
#endif

#endif /* MRES_H */
