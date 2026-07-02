#include "mres.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef bool (*test_fn)(void);

typedef struct {
    int run;
    int passed;
    int failed;
} harness_totals_t;

static harness_totals_t g_totals = { 0, 0, 0 };

#define CHECK_TRUE(expr)                                                           \
    do {                                                                           \
        if (!(expr)) {                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                 \
            return false;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_ERR(expected, actual)                                                \
    do {                                                                           \
        mres_err_t actual_once_ = (actual);                                        \
        if ((expected) != actual_once_) {                                          \
            printf("FAIL %s:%d: expected %ld got %ld\n",                           \
                   __FILE__,                                                       \
                   __LINE__,                                                       \
                   (long)(expected),                                               \
                   (long)actual_once_);                                            \
            return false;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_INT(expected, actual)                                                \
    do {                                                                           \
        int actual_once_ = (actual);                                               \
        if ((expected) != actual_once_) {                                          \
            printf("FAIL %s:%d: expected %d got %d\n",                             \
                   __FILE__, __LINE__, (expected), actual_once_);                  \
            return false;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_U32(expected, actual)                                                \
    do {                                                                           \
        uint32_t actual_once_ = (actual);                                          \
        if ((expected) != actual_once_) {                                          \
            printf("FAIL %s:%d: expected %lu got %lu\n",                           \
                   __FILE__,                                                       \
                   __LINE__,                                                       \
                   (unsigned long)(expected),                                      \
                   (unsigned long)actual_once_);                                   \
            return false;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_U16(expected, actual)                                                \
    do {                                                                           \
        uint16_t actual_once_ = (actual);                                          \
        if ((expected) != actual_once_) {                                          \
            printf("FAIL %s:%d: expected %u got %u\n",                             \
                   __FILE__, __LINE__, (unsigned)(expected), (unsigned)actual_once_); \
            return false;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_BOOL(expected, actual)                                               \
    do {                                                                           \
        bool actual_once_ = (actual);                                              \
        if ((expected) != actual_once_) {                                          \
            printf("FAIL %s:%d: expected %s got %s\n",                             \
                   __FILE__,                                                       \
                   __LINE__,                                                       \
                   (expected) ? "true" : "false",                                  \
                   actual_once_ ? "true" : "false");                               \
            return false;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_STR(expected, actual)                                                \
    do {                                                                           \
        const char *actual_once_ = (actual);                                       \
        if (strcmp((expected), actual_once_) != 0) {                               \
            printf("FAIL %s:%d: expected \"%s\" got \"%s\"\n",                     \
                   __FILE__, __LINE__, (expected), actual_once_);                  \
            return false;                                                          \
        }                                                                          \
    } while (0)

static bool run_test(const char *name, test_fn fn)
{
    bool passed = false;

    g_totals.run++;
    printf("  %-48s ", name);
    passed = fn();

    if (passed) {
        g_totals.passed++;
        printf("PASS\n");
    } else {
        g_totals.failed++;
    }

    return passed;
}

typedef struct {
    uint32_t now_ms;
    uint32_t total_wait_ms;
    int wait_status;
    int wait_calls;
    mres_retry_t *retry_for_reentry;
    mres_ratelimit_t *limiter_for_reentry;
    const mres_platform_t *platform_for_reentry;
} mock_clock_t;

typedef struct {
    int fail_count;
    int call_count;
    int fixed_result;
    mres_retry_t *retry_for_reentry;
    mres_breaker_t *breaker_for_reentry;
    const mres_platform_t *platform_for_reentry;
} mock_op_t;

static int op_succeeds(void *context);

static uint32_t mock_clock_now(void *context)
{
    mock_clock_t *clock = (mock_clock_t *)context;

    if (clock->limiter_for_reentry != NULL) {
        bool allowed = true;
        (void)mres_ratelimit_acquire(clock->limiter_for_reentry, 1u, clock->platform_for_reentry,
                                     &allowed);
    }

    return clock->now_ms;
}

static int mock_wait_fn(void *context, uint32_t delay_ms)
{
    mock_clock_t *clock = (mock_clock_t *)context;
    int result = 99;

    clock->wait_calls++;
    clock->total_wait_ms += delay_ms;
    clock->now_ms += delay_ms;

    if (clock->retry_for_reentry != NULL) {
        (void)mres_retry_reset(clock->retry_for_reentry);
        (void)mres_retry_exec(clock->retry_for_reentry, op_succeeds, NULL, NULL, &result);
    }

    return clock->wait_status;
}

static int op_succeeds(void *context)
{
    mock_op_t *op = (mock_op_t *)context;
    op->call_count++;
    return 0;
}

static int op_fails(void *context)
{
    mock_op_t *op = (mock_op_t *)context;
    op->call_count++;
    return op->fixed_result;
}

static int op_fail_then_succeed(void *context)
{
    mock_op_t *op = (mock_op_t *)context;
    op->call_count++;

    if (op->call_count <= op->fail_count) {
        return op->fixed_result;
    }

    return 0;
}

static int retry_reentrant_operation(void *context)
{
    mock_op_t *op = (mock_op_t *)context;
    int nested_result = 42;
    mres_err_t nested_status = MRES_OK;

    op->call_count++;
    nested_status =
        mres_retry_exec(op->retry_for_reentry, op_succeeds, context, NULL, &nested_result);
    if (nested_status != MRES_ERR_BUSY) {
        return -1000;
    }
    return op->fixed_result;
}

static int breaker_reentrant_operation(void *context)
{
    mock_op_t *op = (mock_op_t *)context;
    int nested_result = 42;
    mres_err_t nested_status = MRES_OK;

    op->call_count++;
    nested_status = mres_breaker_call(op->breaker_for_reentry, op_succeeds, context,
                                      op->platform_for_reentry, &nested_result);
    if (nested_status != MRES_ERR_BUSY) {
        return -1000;
    }
    return 0;
}

static mres_retry_policy_t retry_policy(uint8_t attempts, uint32_t base, uint32_t cap,
                                        uint8_t strategy, uint8_t jitter)
{
    mres_retry_policy_t policy;

    policy.max_attempts = attempts;
    policy.strategy = strategy;
    policy.jitter = jitter;
    policy.reserved0 = 0u;
    policy.base_delay_ms = base;
    policy.max_delay_ms = cap;

    return policy;
}

static mres_breaker_policy_t breaker_policy(uint8_t failures, uint32_t timeout_ms)
{
    mres_breaker_policy_t policy;

    policy.failure_threshold = failures;
    policy.half_open_max_calls = 1u;
    policy.reserved0 = 0u;
    policy.reserved1 = 0u;
    policy.recovery_timeout_ms = timeout_ms;

    return policy;
}

static mres_ratelimit_policy_t limiter_policy(uint16_t max_tokens, uint16_t refill_count,
                                              uint32_t refill_ms)
{
    mres_ratelimit_policy_t policy;

    policy.max_tokens = max_tokens;
    policy.refill_count = refill_count;
    policy.refill_ms = refill_ms;

    return policy;
}

static bool test_retry_policy_validation(void)
{
    mres_retry_t retry;
    mres_retry_policy_t invalid_strategy = retry_policy(3u, 10u, 0u, 9u, 0u);
    mres_retry_policy_t zero_attempts = retry_policy(0u, 10u, 0u, MRES_BACKOFF_FIXED, 0u);

    CHECK_ERR(MRES_ERR_INVALID, mres_retry_init(&retry, &invalid_strategy));
    CHECK_ERR(MRES_ERR_INVALID, mres_retry_init(&retry, &zero_attempts));
    return true;
}

static bool test_retry_policy_copy_and_seed(void)
{
    mres_retry_t left;
    mres_retry_t right;
    mres_retry_policy_t policy = retry_policy(3u, 1000u, 0u, MRES_BACKOFF_FIXED, 1u);
    uint32_t left_delay = 0u;
    uint32_t right_delay = 0u;

    CHECK_ERR(MRES_OK, mres_retry_init(&left, &policy));
    CHECK_ERR(MRES_OK, mres_retry_init(&right, &policy));
    CHECK_ERR(MRES_OK, mres_retry_seed(&left, 123u));
    CHECK_ERR(MRES_OK, mres_retry_seed(&right, 123u));

    policy.base_delay_ms = 10u;

    CHECK_ERR(MRES_OK, mres_delay_calc(&left, 0u, &left_delay));
    CHECK_ERR(MRES_OK, mres_delay_calc(&right, 0u, &right_delay));
    CHECK_U32(left_delay, right_delay);

    CHECK_ERR(MRES_OK, mres_retry_seed(&right, 456u));
    CHECK_ERR(MRES_OK, mres_delay_calc(&right, 0u, &right_delay));
    CHECK_TRUE(left_delay != right_delay);
    return true;
}

static bool test_retry_arithmetic_saturation(void)
{
    mres_retry_t retry;
    uint32_t delay_ms = 0u;
    mres_retry_policy_t linear = retry_policy(3u, UINT32_MAX, 100u, MRES_BACKOFF_LINEAR, 0u);
    mres_retry_policy_t exponential =
        retry_policy(3u, UINT32_MAX / 2u, 0u, MRES_BACKOFF_EXPONENTIAL, 0u);

    CHECK_ERR(MRES_OK, mres_retry_init(&retry, &linear));
    CHECK_ERR(MRES_OK, mres_delay_calc(&retry, 1u, &delay_ms));
    CHECK_U32(100u, delay_ms);

    CHECK_ERR(MRES_OK, mres_retry_init(&retry, &exponential));
    CHECK_ERR(MRES_OK, mres_delay_calc(&retry, 3u, &delay_ms));
    CHECK_U32(UINT32_MAX, delay_ms);
    return true;
}

static bool test_retry_exec_wait_contracts(void)
{
    mres_retry_t retry;
    mres_retry_policy_t policy = retry_policy(2u, 50u, 0u, MRES_BACKOFF_FIXED, 0u);
    mock_clock_t clock = { 10u, 0u, 0, 0, NULL, NULL, NULL };
    mock_op_t op = { 1, 0, 77, NULL, NULL, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    int operation_result = -1;

    CHECK_ERR(MRES_OK, mres_retry_init(&retry, &policy));
    CHECK_ERR(MRES_ERR_WAIT_REQUIRED,
              mres_retry_exec(&retry, op_fail_then_succeed, &op, NULL, &operation_result));
    CHECK_INT(77, operation_result);

    op.call_count = 0;
    clock.wait_status = -1;
    CHECK_ERR(MRES_ERR_WAIT_FAILED,
              mres_retry_exec(&retry, op_fail_then_succeed, &op, &platform, &operation_result));
    CHECK_INT(77, operation_result);
    return true;
}

static bool test_retry_exec_result_domain(void)
{
    mres_retry_t retry;
    mres_retry_policy_t policy = retry_policy(1u, 0u, 0u, MRES_BACKOFF_FIXED, 0u);
    mock_op_t op = { 5, 0, MRES_ERR_OPEN, NULL, NULL, NULL };
    int operation_result = 99;

    CHECK_ERR(MRES_OK, mres_retry_init(&retry, &policy));
    CHECK_ERR(MRES_ERR_EXHAUSTED,
              mres_retry_exec(&retry, op_fails, &op, NULL, &operation_result));
    CHECK_INT(MRES_ERR_OPEN, operation_result);
    return true;
}

static bool test_retry_same_instance_recursion_and_reset(void)
{
    mres_retry_t retry;
    mres_retry_policy_t policy = retry_policy(2u, 10u, 0u, MRES_BACKOFF_FIXED, 0u);
    mock_clock_t clock = { 0u, 0u, 0, 0, &retry, NULL, NULL };
    mock_op_t op = { 0, 0, 5, &retry, NULL, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    int operation_result = 0;

    CHECK_ERR(MRES_OK, mres_retry_init(&retry, &policy));
    CHECK_ERR(MRES_ERR_EXHAUSTED,
              mres_retry_exec(&retry, retry_reentrant_operation, &op, &platform, &operation_result));
    CHECK_ERR(MRES_OK, mres_retry_reset(&retry));
    return true;
}

static bool test_retry_nested_independent_instances(void)
{
    mres_retry_t outer_retry;
    mres_retry_t inner_retry;
    mres_retry_policy_t outer_policy = retry_policy(2u, 0u, 0u, MRES_BACKOFF_FIXED, 0u);
    mres_retry_policy_t inner_policy = retry_policy(1u, 0u, 0u, MRES_BACKOFF_FIXED, 0u);
    mock_op_t inner_op = { 0, 0, 0, NULL, NULL, NULL };
    int inner_result = -1;

    CHECK_ERR(MRES_OK, mres_retry_init(&outer_retry, &outer_policy));
    CHECK_ERR(MRES_OK, mres_retry_init(&inner_retry, &inner_policy));
    CHECK_ERR(MRES_OK, mres_retry_exec(&inner_retry, op_succeeds, &inner_op, NULL, &inner_result));
    CHECK_INT(0, inner_result);
    return true;
}

static bool test_breaker_policy_and_state_queries(void)
{
    mres_breaker_t breaker;
    mres_breaker_policy_t unsupported = breaker_policy(1u, 100u);
    mres_breaker_policy_t supported = breaker_policy(2u, 100u);
    mres_breaker_state_t state = 99u;
    const char *name = NULL;

    unsupported.half_open_max_calls = 2u;
    CHECK_ERR(MRES_ERR_UNSUPPORTED, mres_breaker_init(&breaker, &unsupported));
    CHECK_ERR(MRES_ERR_INVALID, mres_breaker_get_state(&breaker, &state));

    CHECK_ERR(MRES_OK, mres_breaker_init(&breaker, &supported));
    CHECK_ERR(MRES_OK, mres_breaker_get_state(&breaker, &state));
    CHECK_ERR(MRES_OK, mres_breaker_state_name(&breaker, &name));
    CHECK_U32(MRES_BREAKER_CLOSED, state);
    CHECK_STR("closed", name);
    return true;
}

static bool test_breaker_fail_closed_and_result_domain(void)
{
    mres_breaker_t breaker;
    mres_breaker_policy_t policy = breaker_policy(1u, 50u);
    mock_clock_t clock = { 100u, 0u, 0, 0, NULL, NULL, NULL };
    mock_op_t op = { 0, 0, MRES_ERR_OPEN, NULL, NULL, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    int result = 0;

    CHECK_ERR(MRES_OK, mres_breaker_init(&breaker, &policy));
    breaker.state = 9u;
    CHECK_ERR(MRES_ERR_INVALID, mres_breaker_call(&breaker, op_fails, &op, &platform, &result));
    CHECK_INT(0, op.call_count);

    CHECK_ERR(MRES_OK, mres_breaker_init(&breaker, &policy));
    CHECK_ERR(MRES_ERR_OP_FAILED,
              mres_breaker_call(&breaker, op_fails, &op, &platform, &result));
    CHECK_INT(MRES_ERR_OPEN, result);
    return true;
}

static bool test_breaker_open_timing_and_manual_report(void)
{
    mres_breaker_t breaker;
    mres_breaker_policy_t policy = breaker_policy(1u, 50u);
    mock_clock_t clock = { 100u, 0u, 0, 0, NULL, NULL, NULL };
    mock_op_t op = { 0, 0, 5, NULL, NULL, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    uint32_t remaining_ms = 0u;
    bool is_open = false;

    CHECK_ERR(MRES_OK, mres_breaker_init(&breaker, &policy));
    CHECK_ERR(MRES_ERR_OP_FAILED, mres_breaker_call(&breaker, op_fails, &op, &platform, NULL));
    CHECK_U32(1u, breaker.state);

    CHECK_ERR(MRES_OK, mres_breaker_report_success(&breaker));
    CHECK_U32(MRES_BREAKER_OPEN, breaker.state);

    CHECK_ERR(MRES_OK, mres_breaker_report_failure(&breaker, &platform));
    CHECK_U32(100u, breaker.opened_at_ms);

    CHECK_ERR(MRES_OK, mres_breaker_remaining_ms(&breaker, &platform, &remaining_ms, &is_open));
    CHECK_BOOL(true, is_open);
    CHECK_U32(50u, remaining_ms);

    clock.now_ms = 160u;
    CHECK_ERR(MRES_OK, mres_breaker_remaining_ms(&breaker, &platform, &remaining_ms, &is_open));
    CHECK_BOOL(true, is_open);
    CHECK_U32(0u, remaining_ms);
    return true;
}

static bool test_breaker_same_instance_recursion_and_policy_copy(void)
{
    mres_breaker_t breaker;
    mock_clock_t clock = { 100u, 0u, 0, 0, NULL, NULL, NULL };
    mock_op_t op = { 0, 0, 0, NULL, &breaker, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    mres_breaker_policy_t policy = breaker_policy(2u, 50u);

    op.platform_for_reentry = &platform;

    CHECK_ERR(MRES_OK, mres_breaker_init(&breaker, &policy));
    policy.failure_threshold = 9u;
    CHECK_ERR(MRES_OK, mres_breaker_call(&breaker, breaker_reentrant_operation, &op, &platform,
                                         NULL));
    CHECK_U32(2u, breaker.policy.failure_threshold);
    return true;
}

static bool test_rate_limiter_policy_and_range(void)
{
    mres_ratelimit_t limiter;
    mres_ratelimit_policy_t invalid = limiter_policy(1u, 0u, 1u);
    mres_ratelimit_policy_t valid = limiter_policy(2u, 1u, 10u);
    mock_clock_t clock = { 100u, 0u, 0, 0, NULL, NULL, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    bool allowed = true;

    CHECK_ERR(MRES_ERR_INVALID, mres_ratelimit_init(&limiter, &invalid, &platform));
    CHECK_ERR(MRES_OK, mres_ratelimit_init(&limiter, &valid, &platform));
    CHECK_ERR(MRES_ERR_INVALID, mres_ratelimit_acquire(&limiter, 0u, &platform, &allowed));
    CHECK_ERR(MRES_ERR_RANGE, mres_ratelimit_acquire(&limiter, 3u, &platform, &allowed));
    return true;
}

static bool test_rate_limiter_overflow_regression_and_remainder(void)
{
    mres_ratelimit_t limiter;
    mres_ratelimit_policy_t overflow_policy = limiter_policy(100u, 65535u, 1u);
    mres_ratelimit_policy_t remainder_policy = limiter_policy(5u, 2u, 10u);
    mock_clock_t clock = { 0u, 0u, 0, 0, NULL, NULL, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    bool allowed = false;
    uint16_t tokens = 0u;

    CHECK_ERR(MRES_OK, mres_ratelimit_init(&limiter, &overflow_policy, &platform));
    CHECK_ERR(MRES_OK, mres_ratelimit_acquire(&limiter, 100u, &platform, &allowed));
    CHECK_BOOL(true, allowed);
    clock.now_ms = 0xFFFEFFFFu;
    CHECK_ERR(MRES_OK, mres_ratelimit_tokens(&limiter, &platform, &tokens));
    CHECK_U16(100u, tokens);

    clock.now_ms = 0u;
    CHECK_ERR(MRES_OK, mres_ratelimit_init(&limiter, &remainder_policy, &platform));
    CHECK_ERR(MRES_OK, mres_ratelimit_acquire(&limiter, 5u, &platform, &allowed));
    clock.now_ms = 25u;
    CHECK_ERR(MRES_OK, mres_ratelimit_tokens(&limiter, &platform, &tokens));
    CHECK_U16(4u, tokens);
    CHECK_U32(20u, limiter.last_refill_ms);
    return true;
}

static bool test_rate_limiter_wrap_and_policy_copy(void)
{
    mres_ratelimit_t limiter;
    mock_clock_t clock = { UINT32_MAX - 5u, 0u, 0, 0, NULL, NULL, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    mres_ratelimit_policy_t policy = limiter_policy(4u, 1u, 10u);
    uint16_t tokens = 0u;
    bool allowed = false;

    CHECK_ERR(MRES_OK, mres_ratelimit_init(&limiter, &policy, &platform));
    CHECK_ERR(MRES_OK, mres_ratelimit_acquire(&limiter, 4u, &platform, &allowed));
    CHECK_BOOL(true, allowed);

    policy.max_tokens = 99u;
    clock.now_ms = 8u;
    CHECK_ERR(MRES_OK, mres_ratelimit_tokens(&limiter, &platform, &tokens));
    CHECK_U16(1u, tokens);
    CHECK_U16(4u, limiter.policy.max_tokens);
    return true;
}

static bool test_rate_limiter_same_instance_clock_recursion(void)
{
    mres_ratelimit_t limiter;
    mres_ratelimit_policy_t policy = limiter_policy(2u, 1u, 10u);
    mock_clock_t clock = { 100u, 0u, 0, 0, NULL, NULL, NULL };
    mres_platform_t platform = { &clock, mock_clock_now, mock_wait_fn };
    bool allowed = false;

    CHECK_ERR(MRES_OK, mres_ratelimit_init(&limiter, &policy, &platform));
    clock.limiter_for_reentry = &limiter;
    clock.platform_for_reentry = &platform;
    CHECK_ERR(MRES_OK, mres_ratelimit_acquire(&limiter, 1u, &platform, &allowed));
    CHECK_BOOL(true, allowed);
    return true;
}

static bool test_error_strings(void)
{
    CHECK_STR("ok", mres_err_str(MRES_OK));
    CHECK_STR("wait callback failed", mres_err_str(MRES_ERR_WAIT_FAILED));
    CHECK_STR("unknown error", mres_err_str((mres_err_t)1234));
    return true;
}

int main(void)
{
    printf("microres test suite\n");

    (void)run_test("retry policy validation", test_retry_policy_validation);
    (void)run_test("retry policy copy and seed", test_retry_policy_copy_and_seed);
    (void)run_test("retry arithmetic saturation", test_retry_arithmetic_saturation);
    (void)run_test("retry wait contracts", test_retry_exec_wait_contracts);
    (void)run_test("retry result domain", test_retry_exec_result_domain);
    (void)run_test("retry same-instance recursion", test_retry_same_instance_recursion_and_reset);
    (void)run_test("retry nested independent", test_retry_nested_independent_instances);
    (void)run_test("breaker policy and queries", test_breaker_policy_and_state_queries);
    (void)run_test("breaker fail closed and result", test_breaker_fail_closed_and_result_domain);
    (void)run_test("breaker timing and manual report", test_breaker_open_timing_and_manual_report);
    (void)run_test("breaker recursion and policy copy", test_breaker_same_instance_recursion_and_policy_copy);
    (void)run_test("rate limiter policy and range", test_rate_limiter_policy_and_range);
    (void)run_test("rate limiter overflow and remainder", test_rate_limiter_overflow_regression_and_remainder);
    (void)run_test("rate limiter wrap and policy copy", test_rate_limiter_wrap_and_policy_copy);
    (void)run_test("rate limiter clock recursion", test_rate_limiter_same_instance_clock_recursion);
    (void)run_test("error strings", test_error_strings);

    printf("results: %d run, %d passed, %d failed\n",
           g_totals.run, g_totals.passed, g_totals.failed);
    return (g_totals.failed == 0) ? 0 : 1;
}
