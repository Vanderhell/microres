/*
 * microres test suite.
 *
 * Build: gcc -std=c99 -Wall -Wextra -I../include ../src/mres.c test_all.c -o test_all
 * Run:   ./test_all
 */

#include "mres.h"
#include <stdio.h>
#include <string.h>

/* ── Minimal test framework ────────────────────────────────────────────── */

static int tests_run = 0, tests_passed = 0, tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do {                                     \
    tests_run++;                                                \
    printf("  %-55s ", #name);                                  \
    name();                                                     \
    printf("PASS\n");                                           \
    tests_passed++;                                             \
} while (0)

#define ASSERT_EQ(expected, actual) do {                        \
    if ((expected) != (actual)) {                               \
        printf("FAIL\n    %s:%d: expected %d, got %d\n",       \
               __FILE__, __LINE__, (int)(expected), (int)(actual)); \
        tests_failed++; return;                                 \
    }                                                           \
} while (0)

#define ASSERT_TRUE(expr) do {                                  \
    if (!(expr)) {                                              \
        printf("FAIL\n    %s:%d: expected true\n",              \
               __FILE__, __LINE__);                             \
        tests_failed++; return;                                 \
    }                                                           \
} while (0)

#define ASSERT_FALSE(expr) do {                                 \
    if ((expr)) {                                               \
        printf("FAIL\n    %s:%d: expected false\n",             \
               __FILE__, __LINE__);                             \
        tests_failed++; return;                                 \
    }                                                           \
} while (0)

#define ASSERT_STR_EQ(expected, actual) do {                    \
    if (strcmp((expected), (actual)) != 0) {                     \
        printf("FAIL\n    %s:%d: expected \"%s\", got \"%s\"\n",\
               __FILE__, __LINE__, (expected), (actual));       \
        tests_failed++; return;                                 \
    }                                                           \
} while (0)

#define ASSERT_GE(val, minimum) do {                            \
    if ((int)(val) < (int)(minimum)) {                          \
        printf("FAIL\n    %s:%d: %d < %d\n",                   \
               __FILE__, __LINE__, (int)(val), (int)(minimum)); \
        tests_failed++; return;                                 \
    }                                                           \
} while (0)

#define ASSERT_LE(val, maximum) do {                            \
    if ((int)(val) > (int)(maximum)) {                          \
        printf("FAIL\n    %s:%d: %d > %d\n",                   \
               __FILE__, __LINE__, (int)(val), (int)(maximum)); \
        tests_failed++; return;                                 \
    }                                                           \
} while (0)

/* ── Mock clock and sleep ──────────────────────────────────────────────── */

static uint32_t mock_time_ms = 0;
static uint32_t total_sleep_ms = 0;

static uint32_t mock_clock(void)          { return mock_time_ms; }
static void mock_sleep(uint32_t ms)       { mock_time_ms += ms; total_sleep_ms += ms; }

static void reset_mocks(void) {
    mock_time_ms   = 1000;  /* start at 1s to avoid zero-edge cases */
    total_sleep_ms = 0;
}

/* ── Mock operations ───────────────────────────────────────────────────── */

static int op_call_count = 0;
static int op_fail_until = 0;   /* fail this many times, then succeed */

static int mock_op_succeed(void *ctx) {
    (void)ctx;
    op_call_count++;
    return 0;
}

static int mock_op_fail(void *ctx) {
    (void)ctx;
    op_call_count++;
    return -1;
}

static int mock_op_fail_then_succeed(void *ctx) {
    (void)ctx;
    op_call_count++;
    if (op_call_count <= op_fail_until) {
        return -1;
    }
    return 0;
}

static void reset_op(void) {
    op_call_count  = 0;
    op_fail_until  = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tests: Retry
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(test_retry_init) {
    mres_retry_policy_t policy = { .max_attempts = 3, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_FIXED };
    mres_retry_t retry;
    ASSERT_EQ(MRES_OK, mres_retry_init(&retry, &policy));
    ASSERT_EQ(0, retry.attempts);
}

TEST(test_retry_init_null) {
    mres_retry_policy_t policy = { .max_attempts = 3 };
    mres_retry_t retry;
    ASSERT_EQ(MRES_ERR_NULL, mres_retry_init(NULL, &policy));
    ASSERT_EQ(MRES_ERR_NULL, mres_retry_init(&retry, NULL));
}

TEST(test_retry_init_zero_attempts) {
    mres_retry_policy_t policy = { .max_attempts = 0 };
    mres_retry_t retry;
    ASSERT_EQ(MRES_ERR_INVALID, mres_retry_init(&retry, &policy));
}

TEST(test_retry_success_first_attempt) {
    reset_mocks(); reset_op();
    mres_retry_policy_t policy = { .max_attempts = 3, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_FIXED };
    mres_retry_t retry;
    mres_retry_init(&retry, &policy);
    ASSERT_EQ(MRES_OK, mres_retry_exec(&retry, mock_op_succeed, NULL,
                                         mock_clock, mock_sleep));
    ASSERT_EQ(1, retry.attempts);
    ASSERT_EQ(1, op_call_count);
    ASSERT_EQ(0, (int)total_sleep_ms);  /* no sleep needed */
}

TEST(test_retry_success_after_failures) {
    reset_mocks(); reset_op();
    op_fail_until = 2;
    mres_retry_policy_t policy = { .max_attempts = 5, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_FIXED };
    mres_retry_t retry;
    mres_retry_init(&retry, &policy);
    ASSERT_EQ(MRES_OK, mres_retry_exec(&retry, mock_op_fail_then_succeed, NULL,
                                         mock_clock, mock_sleep));
    ASSERT_EQ(3, retry.attempts);
    ASSERT_EQ(3, op_call_count);
}

TEST(test_retry_exhausted) {
    reset_mocks(); reset_op();
    mres_retry_policy_t policy = { .max_attempts = 3, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_FIXED };
    mres_retry_t retry;
    mres_retry_init(&retry, &policy);
    ASSERT_EQ(MRES_ERR_EXHAUSTED, mres_retry_exec(&retry, mock_op_fail, NULL,
                                                    mock_clock, mock_sleep));
    ASSERT_EQ(3, retry.attempts);
    ASSERT_EQ(3, op_call_count);
}

TEST(test_retry_single_attempt) {
    reset_mocks(); reset_op();
    mres_retry_policy_t policy = { .max_attempts = 1, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_FIXED };
    mres_retry_t retry;
    mres_retry_init(&retry, &policy);
    ASSERT_EQ(MRES_ERR_EXHAUSTED, mres_retry_exec(&retry, mock_op_fail, NULL,
                                                    mock_clock, mock_sleep));
    ASSERT_EQ(1, op_call_count);
}

TEST(test_retry_reset) {
    mres_retry_policy_t policy = { .max_attempts = 3 };
    mres_retry_t retry;
    mres_retry_init(&retry, &policy);
    retry.attempts = 3;
    retry.last_error = -1;
    ASSERT_EQ(MRES_OK, mres_retry_reset(&retry));
    ASSERT_EQ(0, retry.attempts);
    ASSERT_EQ(0, retry.last_error);
}

/* ── Delay calculation tests ──────────────────────────────────────────── */

TEST(test_delay_fixed) {
    mres_retry_policy_t policy = { .max_attempts = 5, .base_delay_ms = 200,
                                   .strategy = MRES_BACKOFF_FIXED, .jitter = false };
    ASSERT_EQ(200, (int)mres_delay_calc(&policy, 0, NULL));
    ASSERT_EQ(200, (int)mres_delay_calc(&policy, 1, NULL));
    ASSERT_EQ(200, (int)mres_delay_calc(&policy, 4, NULL));
}

TEST(test_delay_linear) {
    mres_retry_policy_t policy = { .max_attempts = 5, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_LINEAR, .jitter = false };
    ASSERT_EQ(100, (int)mres_delay_calc(&policy, 0, NULL));   /* 100 * 1 */
    ASSERT_EQ(200, (int)mres_delay_calc(&policy, 1, NULL));   /* 100 * 2 */
    ASSERT_EQ(300, (int)mres_delay_calc(&policy, 2, NULL));   /* 100 * 3 */
}

TEST(test_delay_exponential) {
    mres_retry_policy_t policy = { .max_attempts = 5, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_EXPONENTIAL, .jitter = false };
    ASSERT_EQ(100,  (int)mres_delay_calc(&policy, 0, NULL));  /* 100 * 1 */
    ASSERT_EQ(200,  (int)mres_delay_calc(&policy, 1, NULL));  /* 100 * 2 */
    ASSERT_EQ(400,  (int)mres_delay_calc(&policy, 2, NULL));  /* 100 * 4 */
    ASSERT_EQ(800,  (int)mres_delay_calc(&policy, 3, NULL));  /* 100 * 8 */
    ASSERT_EQ(1600, (int)mres_delay_calc(&policy, 4, NULL));  /* 100 * 16 */
}

TEST(test_delay_capped) {
    mres_retry_policy_t policy = { .max_attempts = 10, .base_delay_ms = 1000,
                                   .max_delay_ms = 5000,
                                   .strategy = MRES_BACKOFF_EXPONENTIAL, .jitter = false };
    /* 1000, 2000, 4000, 5000(capped), 5000, ... */
    ASSERT_EQ(1000, (int)mres_delay_calc(&policy, 0, NULL));
    ASSERT_EQ(2000, (int)mres_delay_calc(&policy, 1, NULL));
    ASSERT_EQ(4000, (int)mres_delay_calc(&policy, 2, NULL));
    ASSERT_EQ(5000, (int)mres_delay_calc(&policy, 3, NULL));
    ASSERT_EQ(5000, (int)mres_delay_calc(&policy, 4, NULL));
}

TEST(test_delay_jitter_in_range) {
    reset_mocks();
    mres_retry_policy_t policy = { .max_attempts = 5, .base_delay_ms = 1000,
                                   .max_delay_ms = 30000,
                                   .strategy = MRES_BACKOFF_FIXED, .jitter = true };
    /* ±25% of 1000 = 750..1250 */
    for (int i = 0; i < 20; i++) {
        mock_time_ms = (uint32_t)(1000 + i * 37);  /* vary seed */
        uint32_t d = mres_delay_calc(&policy, 0, mock_clock);
        ASSERT_GE(d, 750);
        ASSERT_LE(d, 1250);
    }
}

TEST(test_delay_null_policy) {
    ASSERT_EQ(0, (int)mres_delay_calc(NULL, 0, NULL));
}

/* ── Retry with sleep tracking ────────────────────────────────────────── */

TEST(test_retry_fixed_sleep_total) {
    reset_mocks(); reset_op();
    mres_retry_policy_t policy = { .max_attempts = 4, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_FIXED, .jitter = false };
    mres_retry_t retry;
    mres_retry_init(&retry, &policy);
    mres_retry_exec(&retry, mock_op_fail, NULL, mock_clock, mock_sleep);
    /* 3 sleeps (not after last attempt), each 100ms */
    ASSERT_EQ(300, (int)total_sleep_ms);
}

TEST(test_retry_no_sleep_fn) {
    reset_mocks(); reset_op();
    op_fail_until = 2;
    mres_retry_policy_t policy = { .max_attempts = 5, .base_delay_ms = 100,
                                   .strategy = MRES_BACKOFF_FIXED };
    mres_retry_t retry;
    mres_retry_init(&retry, &policy);
    /* sleep = NULL → no blocking, but still retries */
    ASSERT_EQ(MRES_OK, mres_retry_exec(&retry, mock_op_fail_then_succeed, NULL,
                                         mock_clock, NULL));
    ASSERT_EQ(3, op_call_count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tests: Circuit Breaker
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(test_breaker_init) {
    mres_breaker_policy_t policy = { .failure_threshold = 3,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    ASSERT_EQ(MRES_OK, mres_breaker_init(&br, &policy));
    ASSERT_EQ(MRES_BREAKER_CLOSED, mres_breaker_state(&br));
}

TEST(test_breaker_init_null) {
    mres_breaker_policy_t policy = { .failure_threshold = 3 };
    mres_breaker_t br;
    ASSERT_EQ(MRES_ERR_NULL, mres_breaker_init(NULL, &policy));
    ASSERT_EQ(MRES_ERR_NULL, mres_breaker_init(&br, NULL));
}

TEST(test_breaker_init_zero_threshold) {
    mres_breaker_policy_t policy = { .failure_threshold = 0 };
    mres_breaker_t br;
    ASSERT_EQ(MRES_ERR_INVALID, mres_breaker_init(&br, &policy));
}

TEST(test_breaker_passes_on_success) {
    reset_mocks(); reset_op();
    mres_breaker_policy_t policy = { .failure_threshold = 3,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);
    ASSERT_EQ(MRES_OK, mres_breaker_call(&br, mock_op_succeed, NULL, mock_clock));
    ASSERT_EQ(MRES_BREAKER_CLOSED, br.state);
}

TEST(test_breaker_trips_after_threshold) {
    reset_mocks(); reset_op();
    mres_breaker_policy_t policy = { .failure_threshold = 3,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);

    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* failure 1 */
    ASSERT_EQ(MRES_BREAKER_CLOSED, br.state);
    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* failure 2 */
    ASSERT_EQ(MRES_BREAKER_CLOSED, br.state);
    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* failure 3 → OPEN */
    ASSERT_EQ(MRES_BREAKER_OPEN, br.state);
}

TEST(test_breaker_blocks_when_open) {
    reset_mocks(); reset_op();
    mres_breaker_policy_t policy = { .failure_threshold = 1,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);

    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* → OPEN */
    ASSERT_EQ(MRES_BREAKER_OPEN, br.state);

    reset_op();
    int result = mres_breaker_call(&br, mock_op_succeed, NULL, mock_clock);
    ASSERT_EQ(MRES_ERR_OPEN, result);
    ASSERT_EQ(0, op_call_count);  /* op was NOT called */
}

TEST(test_breaker_success_resets_count) {
    reset_mocks(); reset_op();
    mres_breaker_policy_t policy = { .failure_threshold = 3,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);

    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* failure 1 */
    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* failure 2 */
    mres_breaker_call(&br, mock_op_succeed, NULL, mock_clock); /* success resets */
    ASSERT_EQ(0, br.failure_count);
    ASSERT_EQ(MRES_BREAKER_CLOSED, br.state);
}

TEST(test_breaker_recovery_to_half_open) {
    reset_mocks(); reset_op();
    mres_breaker_policy_t policy = { .failure_threshold = 1,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);

    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* → OPEN */
    ASSERT_EQ(MRES_BREAKER_OPEN, br.state);

    /* Advance time past recovery timeout */
    mock_time_ms += 6000;

    reset_op();
    /* Next call should transition to HALF_OPEN and execute */
    mres_breaker_call(&br, mock_op_succeed, NULL, mock_clock);
    ASSERT_EQ(MRES_BREAKER_CLOSED, br.state);  /* success → CLOSED */
    ASSERT_EQ(1, op_call_count);
}

TEST(test_breaker_half_open_failure_reopens) {
    reset_mocks(); reset_op();
    mres_breaker_policy_t policy = { .failure_threshold = 1,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);

    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* → OPEN */
    mock_time_ms += 6000;

    /* Probe call fails → back to OPEN */
    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);
    ASSERT_EQ(MRES_BREAKER_OPEN, br.state);
}

TEST(test_breaker_remaining_ms) {
    reset_mocks(); reset_op();
    mres_breaker_policy_t policy = { .failure_threshold = 1,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);

    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);  /* → OPEN */
    mock_time_ms += 2000;
    ASSERT_EQ(3000, (int)mres_breaker_remaining_ms(&br, mock_clock));

    mock_time_ms += 3000;
    ASSERT_EQ(0, (int)mres_breaker_remaining_ms(&br, mock_clock));
}

TEST(test_breaker_remaining_ms_not_open) {
    mres_breaker_policy_t policy = { .failure_threshold = 3,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);
    ASSERT_EQ(0, (int)mres_breaker_remaining_ms(&br, mock_clock));
}

TEST(test_breaker_reset) {
    reset_mocks(); reset_op();
    mres_breaker_policy_t policy = { .failure_threshold = 1,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);
    mres_breaker_call(&br, mock_op_fail, NULL, mock_clock);
    ASSERT_EQ(MRES_BREAKER_OPEN, br.state);

    mres_breaker_reset(&br);
    ASSERT_EQ(MRES_BREAKER_CLOSED, br.state);
    ASSERT_EQ(0, br.failure_count);
}

TEST(test_breaker_state_name) {
    mres_breaker_policy_t policy = { .failure_threshold = 1,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);
    ASSERT_STR_EQ("CLOSED", mres_breaker_state_name(&br));

    br.state = MRES_BREAKER_OPEN;
    ASSERT_STR_EQ("OPEN", mres_breaker_state_name(&br));

    br.state = MRES_BREAKER_HALF_OPEN;
    ASSERT_STR_EQ("HALF_OPEN", mres_breaker_state_name(&br));
}

TEST(test_breaker_report_success) {
    reset_mocks();
    mres_breaker_policy_t policy = { .failure_threshold = 3,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);
    br.failure_count = 2;
    mres_breaker_report_success(&br);
    ASSERT_EQ(0, br.failure_count);
}

TEST(test_breaker_report_failure) {
    reset_mocks();
    mres_breaker_policy_t policy = { .failure_threshold = 2,
                                     .recovery_timeout_ms = 5000,
                                     .half_open_max_calls = 1 };
    mres_breaker_t br;
    mres_breaker_init(&br, &policy);

    mres_breaker_report_failure(&br, mock_clock);
    ASSERT_EQ(1, br.failure_count);
    ASSERT_EQ(MRES_BREAKER_CLOSED, br.state);

    mres_breaker_report_failure(&br, mock_clock);
    ASSERT_EQ(MRES_BREAKER_OPEN, br.state);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tests: Rate Limiter
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(test_ratelimit_init) {
    reset_mocks();
    mres_ratelimit_policy_t policy = { .max_tokens = 10, .refill_ms = 1000,
                                       .refill_count = 10 };
    mres_ratelimit_t rl;
    ASSERT_EQ(MRES_OK, mres_ratelimit_init(&rl, &policy, mock_clock));
    ASSERT_EQ(10, rl.tokens);
}

TEST(test_ratelimit_init_null) {
    reset_mocks();
    mres_ratelimit_policy_t policy = { .max_tokens = 10, .refill_ms = 1000,
                                       .refill_count = 10 };
    mres_ratelimit_t rl;
    ASSERT_EQ(MRES_ERR_NULL, mres_ratelimit_init(NULL, &policy, mock_clock));
    ASSERT_EQ(MRES_ERR_NULL, mres_ratelimit_init(&rl, NULL, mock_clock));
    ASSERT_EQ(MRES_ERR_NULL, mres_ratelimit_init(&rl, &policy, NULL));
}

TEST(test_ratelimit_init_invalid) {
    reset_mocks();
    mres_ratelimit_policy_t p1 = { .max_tokens = 0, .refill_ms = 1000, .refill_count = 1 };
    mres_ratelimit_policy_t p2 = { .max_tokens = 10, .refill_ms = 0, .refill_count = 1 };
    mres_ratelimit_t rl;
    ASSERT_EQ(MRES_ERR_INVALID, mres_ratelimit_init(&rl, &p1, mock_clock));
    ASSERT_EQ(MRES_ERR_INVALID, mres_ratelimit_init(&rl, &p2, mock_clock));
}

TEST(test_ratelimit_acquire_basic) {
    reset_mocks();
    mres_ratelimit_policy_t policy = { .max_tokens = 3, .refill_ms = 1000,
                                       .refill_count = 3 };
    mres_ratelimit_t rl;
    mres_ratelimit_init(&rl, &policy, mock_clock);

    ASSERT_TRUE(mres_ratelimit_acquire(&rl, 1, mock_clock));   /* 3→2 */
    ASSERT_TRUE(mres_ratelimit_acquire(&rl, 1, mock_clock));   /* 2→1 */
    ASSERT_TRUE(mres_ratelimit_acquire(&rl, 1, mock_clock));   /* 1→0 */
    ASSERT_FALSE(mres_ratelimit_acquire(&rl, 1, mock_clock));  /* 0, blocked */
}

TEST(test_ratelimit_refill) {
    reset_mocks();
    mres_ratelimit_policy_t policy = { .max_tokens = 5, .refill_ms = 1000,
                                       .refill_count = 2 };
    mres_ratelimit_t rl;
    mres_ratelimit_init(&rl, &policy, mock_clock);

    /* Consume all */
    for (int i = 0; i < 5; i++) {
        mres_ratelimit_acquire(&rl, 1, mock_clock);
    }
    ASSERT_EQ(0, rl.tokens);

    /* Advance 1 second → refill 2 tokens */
    mock_time_ms += 1000;
    ASSERT_TRUE(mres_ratelimit_acquire(&rl, 1, mock_clock));
    ASSERT_EQ(1, rl.tokens);  /* 2 refilled, 1 consumed = 1 */
}

TEST(test_ratelimit_refill_capped) {
    reset_mocks();
    mres_ratelimit_policy_t policy = { .max_tokens = 5, .refill_ms = 1000,
                                       .refill_count = 100 };
    mres_ratelimit_t rl;
    mres_ratelimit_init(&rl, &policy, mock_clock);

    /* Consume all, then wait */
    for (int i = 0; i < 5; i++) {
        mres_ratelimit_acquire(&rl, 1, mock_clock);
    }
    mock_time_ms += 5000;

    /* Should be capped at max_tokens */
    uint16_t tokens = mres_ratelimit_tokens(&rl, mock_clock);
    ASSERT_EQ(5, tokens);
}

TEST(test_ratelimit_multi_acquire) {
    reset_mocks();
    mres_ratelimit_policy_t policy = { .max_tokens = 10, .refill_ms = 1000,
                                       .refill_count = 10 };
    mres_ratelimit_t rl;
    mres_ratelimit_init(&rl, &policy, mock_clock);

    ASSERT_TRUE(mres_ratelimit_acquire(&rl, 5, mock_clock));   /* 10→5 */
    ASSERT_TRUE(mres_ratelimit_acquire(&rl, 5, mock_clock));   /* 5→0 */
    ASSERT_FALSE(mres_ratelimit_acquire(&rl, 1, mock_clock));  /* 0, blocked */
}

TEST(test_ratelimit_reset) {
    reset_mocks();
    mres_ratelimit_policy_t policy = { .max_tokens = 5, .refill_ms = 1000,
                                       .refill_count = 5 };
    mres_ratelimit_t rl;
    mres_ratelimit_init(&rl, &policy, mock_clock);
    mres_ratelimit_acquire(&rl, 5, mock_clock);
    ASSERT_EQ(0, rl.tokens);

    mres_ratelimit_reset(&rl, mock_clock);
    ASSERT_EQ(5, rl.tokens);
}

TEST(test_ratelimit_tokens_query) {
    reset_mocks();
    mres_ratelimit_policy_t policy = { .max_tokens = 10, .refill_ms = 1000,
                                       .refill_count = 5 };
    mres_ratelimit_t rl;
    mres_ratelimit_init(&rl, &policy, mock_clock);
    mres_ratelimit_acquire(&rl, 3, mock_clock);
    ASSERT_EQ(7, mres_ratelimit_tokens(&rl, mock_clock));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tests: Error strings
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST(test_err_str) {
    ASSERT_STR_EQ("ok",                   mres_err_str(MRES_OK));
    ASSERT_STR_EQ("null pointer",         mres_err_str(MRES_ERR_NULL));
    ASSERT_STR_EQ("retries exhausted",    mres_err_str(MRES_ERR_EXHAUSTED));
    ASSERT_STR_EQ("circuit breaker open", mres_err_str(MRES_ERR_OPEN));
    ASSERT_STR_EQ("rate limited",         mres_err_str(MRES_ERR_RATE_LIMITED));
    ASSERT_STR_EQ("operation failed",     mres_err_str(MRES_ERR_OP_FAILED));
    ASSERT_STR_EQ("invalid configuration", mres_err_str(MRES_ERR_INVALID));
    ASSERT_STR_EQ("unknown error",        mres_err_str((mres_err_t)99));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n=== microres test suite ===\n\n");

    printf("[Retry - Init]\n");
    RUN_TEST(test_retry_init);
    RUN_TEST(test_retry_init_null);
    RUN_TEST(test_retry_init_zero_attempts);

    printf("\n[Retry - Execution]\n");
    RUN_TEST(test_retry_success_first_attempt);
    RUN_TEST(test_retry_success_after_failures);
    RUN_TEST(test_retry_exhausted);
    RUN_TEST(test_retry_single_attempt);
    RUN_TEST(test_retry_reset);
    RUN_TEST(test_retry_fixed_sleep_total);
    RUN_TEST(test_retry_no_sleep_fn);

    printf("\n[Retry - Delay Calculation]\n");
    RUN_TEST(test_delay_fixed);
    RUN_TEST(test_delay_linear);
    RUN_TEST(test_delay_exponential);
    RUN_TEST(test_delay_capped);
    RUN_TEST(test_delay_jitter_in_range);
    RUN_TEST(test_delay_null_policy);

    printf("\n[Circuit Breaker - Init]\n");
    RUN_TEST(test_breaker_init);
    RUN_TEST(test_breaker_init_null);
    RUN_TEST(test_breaker_init_zero_threshold);

    printf("\n[Circuit Breaker - Behaviour]\n");
    RUN_TEST(test_breaker_passes_on_success);
    RUN_TEST(test_breaker_trips_after_threshold);
    RUN_TEST(test_breaker_blocks_when_open);
    RUN_TEST(test_breaker_success_resets_count);
    RUN_TEST(test_breaker_recovery_to_half_open);
    RUN_TEST(test_breaker_half_open_failure_reopens);
    RUN_TEST(test_breaker_remaining_ms);
    RUN_TEST(test_breaker_remaining_ms_not_open);
    RUN_TEST(test_breaker_reset);
    RUN_TEST(test_breaker_state_name);
    RUN_TEST(test_breaker_report_success);
    RUN_TEST(test_breaker_report_failure);

    printf("\n[Rate Limiter - Init]\n");
    RUN_TEST(test_ratelimit_init);
    RUN_TEST(test_ratelimit_init_null);
    RUN_TEST(test_ratelimit_init_invalid);

    printf("\n[Rate Limiter - Behaviour]\n");
    RUN_TEST(test_ratelimit_acquire_basic);
    RUN_TEST(test_ratelimit_refill);
    RUN_TEST(test_ratelimit_refill_capped);
    RUN_TEST(test_ratelimit_multi_acquire);
    RUN_TEST(test_ratelimit_reset);
    RUN_TEST(test_ratelimit_tokens_query);

    printf("\n[Error Strings]\n");
    RUN_TEST(test_err_str);

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n\n");

    return tests_failed > 0 ? 1 : 0;
}
