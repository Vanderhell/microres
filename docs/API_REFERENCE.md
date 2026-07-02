# API Reference

## Configuration

`mres.h` includes the generated public config header `mres_config.h`.

Public config rules:

- `MRES_ENABLE_JITTER` must be exactly `0` or `1`
- `MRES_MAX_ATTEMPTS` must be in `1..255`
- consumer-side overrides that conflict with the generated header fail compilation

## Core types

```c
typedef int32_t mres_err_t;
typedef uint8_t mres_backoff_t;
typedef uint8_t mres_breaker_state_t;
```

Status codes:

- `MRES_OK`
- `MRES_ERR_NULL`
- `MRES_ERR_INVALID`
- `MRES_ERR_RANGE`
- `MRES_ERR_BUSY`
- `MRES_ERR_EXHAUSTED`
- `MRES_ERR_OPEN`
- `MRES_ERR_OP_FAILED`
- `MRES_ERR_WAIT_REQUIRED`
- `MRES_ERR_WAIT_FAILED`
- `MRES_ERR_UNSUPPORTED`

## Platform bundle

```c
typedef uint32_t (*mres_clock_fn)(void *context);
typedef int (*mres_wait_fn)(void *context, uint32_t delay_ms);

typedef struct {
    void *context;
    mres_clock_fn clock;
    mres_wait_fn wait;
} mres_platform_t;
```

Clock contract:

- units are milliseconds
- values are treated as a modulo `uint32_t` counter
- wrap is supported only within one complete counter cycle
- arbitrary backward jumps are unsupported
- the callback must be valid in the calling context and must return promptly

Wait contract:

- returns `0` only when the wait was accepted or completed under the host platform contract
- any non-zero wait result stops retry execution and returns `MRES_ERR_WAIT_FAILED`

## Retry

Policy:

```c
typedef struct {
    uint8_t max_attempts;
    uint8_t strategy;
    uint8_t jitter;
    uint8_t reserved0;
    uint32_t base_delay_ms;
    uint32_t max_delay_ms;
} mres_retry_policy_t;
```

Instance:

```c
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
```

Functions:

```c
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
```

Retry semantics:

- `mres_retry_exec` is synchronous
- `MRES_OK` means the operation ran and returned `0`
- `MRES_ERR_EXHAUSTED` means all permitted attempts were used and each operation call returned non-zero
- `MRES_ERR_WAIT_REQUIRED` means a positive delay was computed but no wait callback was available
- `operation_result` is written only after an operation callback actually runs
- same-instance recursive execution or reset during execution returns `MRES_ERR_BUSY`

Backoff formulas:

- fixed: `base`
- linear: `base * (attempt + 1)`
- exponential: `base * 2^attempt`

Arithmetic rules:

- intermediate overflow saturates at `UINT32_MAX`
- `max_delay_ms` is applied after mathematical saturation
- jitter never wraps arithmetic and is capped again after jitter when `max_delay_ms != 0`

## Circuit breaker

Policy:

```c
typedef struct {
    uint8_t failure_threshold;
    uint8_t half_open_max_calls;
    uint8_t reserved0;
    uint8_t reserved1;
    uint32_t recovery_timeout_ms;
} mres_breaker_policy_t;
```

Only a single synchronous half-open probe is supported, so `half_open_max_calls` must be `1`.

Key functions:

```c
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
```

Breaker semantics:

- invalid or corrupted states fail closed with `MRES_ERR_INVALID`
- `MRES_ERR_OPEN` means the operation was not called
- `MRES_OK` means the operation was called and returned `0`
- `MRES_ERR_OP_FAILED` means the operation was called and returned non-zero
- open timestamps are captured after the failed operation returns, using a fresh clock sample
- manual success in `OPEN` leaves the breaker open
- manual failure in `OPEN` leaves the existing timeout in place

## Rate limiter

Policy:

```c
typedef struct {
    uint16_t max_tokens;
    uint16_t refill_count;
    uint32_t refill_ms;
} mres_ratelimit_policy_t;
```

Functions:

```c
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
```

Rate-limiter semantics:

- zero capacity, zero refill interval, and zero refill count are invalid
- `count == 0` is invalid
- `count > max_tokens` returns `MRES_ERR_RANGE`
- temporary rate limiting returns `MRES_OK` with `allowed == false`
- policy is copied into the instance during successful initialization
- same-instance recursion during acquire, query, or reset returns `MRES_ERR_BUSY`
