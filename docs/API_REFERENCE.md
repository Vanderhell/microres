# API Reference

> **Header:** `#include "mres.h"`
>
> **Version:** 1.0.0

---

## Table of contents

1. [Platform callbacks](#platform-callbacks)
2. [Error codes](#error-codes)
3. [Retry](#retry)
4. [Circuit Breaker](#circuit-breaker)
5. [Rate Limiter](#rate-limiter)
6. [Thread safety](#thread-safety)

---

## Platform callbacks

microres is platform-agnostic. You provide two callbacks that bridge to
your hardware or OS.

### mres_clock_fn

```c
typedef uint32_t (*mres_clock_fn)(void);
```

Returns the current time in milliseconds. Must be monotonic. Wrapping at
`UINT32_MAX` is handled internally (unsigned subtraction). This means the
clock can wrap every ~49.7 days without issues.

**Platform examples:**

| Platform | Implementation |
|----------|---------------|
| STM32 HAL | `HAL_GetTick()` |
| FreeRTOS | `xTaskGetTickCount() * portTICK_PERIOD_MS` |
| ESP-IDF | `esp_timer_get_time() / 1000` |
| Zephyr | `k_uptime_get_32()` |
| Linux | `clock_gettime(CLOCK_MONOTONIC)` → ms |
| Arduino | `millis()` |

### mres_sleep_fn

```c
typedef void (*mres_sleep_fn)(uint32_t ms);
```

Blocks execution for the given number of milliseconds. Used by
`mres_retry_exec()` between attempts. May be NULL — in that case, retry
executes all attempts without delay (useful for non-blocking or
event-driven architectures).

| Platform | Implementation |
|----------|---------------|
| STM32 HAL | `HAL_Delay(ms)` |
| FreeRTOS | `vTaskDelay(pdMS_TO_TICKS(ms))` |
| Zephyr | `k_msleep(ms)` |
| Linux | `usleep(ms * 1000)` |
| Arduino | `delay(ms)` |

### mres_op_fn

```c
typedef int (*mres_op_fn)(void *ctx);
```

The operation being protected. Returns `0` on success, any negative value
on failure. The negative value is passed through as the error result,
allowing the caller to distinguish between different failure modes.

---

## Error codes

```c
typedef enum {
    MRES_OK              =  0,
    MRES_ERR_NULL        = -1,
    MRES_ERR_EXHAUSTED   = -2,
    MRES_ERR_OPEN        = -3,
    MRES_ERR_RATE_LIMITED = -4,
    MRES_ERR_OP_FAILED   = -5,
    MRES_ERR_INVALID     = -6,
} mres_err_t;
```

| Code | Meaning |
|------|---------|
| `MRES_OK` | Operation succeeded |
| `MRES_ERR_NULL` | A required pointer was NULL |
| `MRES_ERR_EXHAUSTED` | All retry attempts failed |
| `MRES_ERR_OPEN` | Circuit breaker is open, call blocked |
| `MRES_ERR_RATE_LIMITED` | Rate limit exceeded |
| `MRES_ERR_OP_FAILED` | The operation itself returned failure |
| `MRES_ERR_INVALID` | Invalid configuration (e.g., zero attempts) |

### mres_err_str

```c
const char *mres_err_str(mres_err_t err);
```

Returns a human-readable string for any error code.

---

## Retry

### Overview

Retry wraps a fallible operation and re-executes it with configurable
backoff delays when it fails.

```
    attempt 1    delay    attempt 2     delay      attempt 3
    ┌──┐        ┌────┐   ┌──┐        ┌────────┐   ┌──┐
    │op│──fail──▶│wait│──▶│op│──fail──▶│  wait  │──▶│op│──success
    └──┘        └────┘   └──┘        └────────┘   └──┘
                100ms                  200ms
```

### Backoff strategies

| Strategy | Formula | Example (base=100ms) |
|----------|---------|---------------------|
| `MRES_BACKOFF_FIXED` | `base` | 100, 100, 100, 100 |
| `MRES_BACKOFF_LINEAR` | `base × (attempt+1)` | 100, 200, 300, 400 |
| `MRES_BACKOFF_EXPONENTIAL` | `base × 2^attempt` | 100, 200, 400, 800 |

All strategies respect `max_delay_ms` as a ceiling.

**Jitter:** When enabled, adds ±25% randomisation to the delay. This
decorrelates retries from multiple devices hitting the same server
simultaneously (the "thundering herd" problem). The jitter uses a
lightweight xorshift32 PRNG seeded from the clock value — no external
entropy source needed.

### mres_retry_policy_t

```c
typedef struct {
    uint8_t        max_attempts;
    uint32_t       base_delay_ms;
    uint32_t       max_delay_ms;
    mres_backoff_t strategy;
    bool           jitter;
} mres_retry_policy_t;
```

| Field | Description |
|-------|-------------|
| `max_attempts` | Total attempts including first try. 1 = no retry. Must be > 0. |
| `base_delay_ms` | Base delay in milliseconds. |
| `max_delay_ms` | Maximum delay cap. 0 = no cap. |
| `strategy` | Backoff strategy (FIXED, LINEAR, EXPONENTIAL). |
| `jitter` | Add ±25% random jitter. |

### mres_retry_t

```c
typedef struct {
    const mres_retry_policy_t *policy;
    uint8_t                    attempts;
    int                        last_error;
} mres_retry_t;
```

Runtime state. After execution, `attempts` tells you how many tries were
made and `last_error` holds the operation's last return value.

### Functions

#### mres_retry_init

```c
mres_err_t mres_retry_init(mres_retry_t *retry, const mres_retry_policy_t *policy);
```

Initialise retry state. Must be called before `mres_retry_exec`.

#### mres_retry_exec

```c
int mres_retry_exec(mres_retry_t *retry, mres_op_fn op, void *ctx,
                    mres_clock_fn clock, mres_sleep_fn sleep);
```

Execute `op(ctx)` with retry. Returns `MRES_OK` if the operation
succeeded on any attempt, or `MRES_ERR_EXHAUSTED` if all attempts failed.

**Sleep behaviour:** If `sleep` is NULL, all attempts execute immediately
without delay. This is useful in event-driven systems where the caller
manages timing externally.

#### mres_retry_reset

```c
mres_err_t mres_retry_reset(mres_retry_t *retry);
```

Reset attempt counter and error for reuse.

#### mres_delay_calc

```c
uint32_t mres_delay_calc(const mres_retry_policy_t *policy, uint8_t attempt,
                         mres_clock_fn clock);
```

Calculate the delay for a specific attempt. Useful for logging or
implementing custom retry loops.

---

## Circuit Breaker

### Overview

A circuit breaker prevents repeated calls to a failing resource. It has
three states:

```
                    failure_count >= threshold
         ┌──────┐ ─────────────────────────▶ ┌──────┐
         │CLOSED│                             │ OPEN │
         │      │ ◀───── success ──────────── │      │
         └──────┘                             └──────┘
            ▲                                    │
            │           recovery_timeout         │
            │           elapsed                  ▼
            │         ┌───────────┐              │
            └─success─│ HALF_OPEN │◀─────────────┘
                      │           │
                      └───────────┘
                         │ failure
                         └──────▶ back to OPEN
```

- **CLOSED:** Normal operation. Calls pass through. Consecutive failures
  are counted.
- **OPEN:** Calls are blocked immediately (returns `MRES_ERR_OPEN`). The
  operation is not invoked. After `recovery_timeout_ms`, transitions to
  HALF_OPEN.
- **HALF_OPEN:** A limited number of probe calls are allowed. If any
  succeeds, transitions to CLOSED. If any fails, transitions back to OPEN.

### mres_breaker_policy_t

```c
typedef struct {
    uint8_t  failure_threshold;
    uint32_t recovery_timeout_ms;
    uint8_t  half_open_max_calls;
} mres_breaker_policy_t;
```

| Field | Description |
|-------|-------------|
| `failure_threshold` | Consecutive failures before tripping to OPEN. Must be > 0. |
| `recovery_timeout_ms` | Time in OPEN state before probing in HALF_OPEN. |
| `half_open_max_calls` | Maximum probe calls allowed in HALF_OPEN. |

### Functions

#### mres_breaker_init

```c
mres_err_t mres_breaker_init(mres_breaker_t *br, const mres_breaker_policy_t *policy);
```

Initialise breaker in CLOSED state.

#### mres_breaker_call

```c
int mres_breaker_call(mres_breaker_t *br, mres_op_fn op, void *ctx,
                      mres_clock_fn clock);
```

Execute `op(ctx)` through the breaker. Returns `MRES_OK` on success,
`MRES_ERR_OPEN` if blocked, or the operation's error code on failure.

#### mres_breaker_state / mres_breaker_state_name

```c
mres_breaker_state_t mres_breaker_state(const mres_breaker_t *br);
const char *mres_breaker_state_name(const mres_breaker_t *br);
```

Query the current state.

#### mres_breaker_remaining_ms

```c
uint32_t mres_breaker_remaining_ms(const mres_breaker_t *br, mres_clock_fn clock);
```

Returns milliseconds until recovery attempt. 0 if not in OPEN state.

#### mres_breaker_reset

```c
mres_err_t mres_breaker_reset(mres_breaker_t *br);
```

Force breaker to CLOSED. Useful for manual recovery.

#### mres_breaker_report_success / mres_breaker_report_failure

```c
mres_err_t mres_breaker_report_success(mres_breaker_t *br);
mres_err_t mres_breaker_report_failure(mres_breaker_t *br, mres_clock_fn clock);
```

Manually report outcomes without executing through the breaker. Useful for
async operations where the result arrives via callback.

---

## Rate Limiter

### Overview

Token bucket rate limiter. Tokens are consumed on each call and refilled
at a fixed rate.

```
    max_tokens = 5, refill = 2/sec

    ●●●●●  →  acquire(1)  →  ●●●●○
              acquire(1)  →  ●●●○○
              acquire(1)  →  ●●○○○
              ...1 second passes...
              refill(2)   →  ●●●●○
```

### mres_ratelimit_policy_t

```c
typedef struct {
    uint16_t max_tokens;
    uint32_t refill_ms;
    uint16_t refill_count;
} mres_ratelimit_policy_t;
```

| Field | Description |
|-------|-------------|
| `max_tokens` | Bucket capacity (ceiling). Must be > 0. |
| `refill_ms` | Interval between refills in ms. Must be > 0. |
| `refill_count` | Tokens added per interval. |

### Functions

#### mres_ratelimit_init

```c
mres_err_t mres_ratelimit_init(mres_ratelimit_t *rl,
                               const mres_ratelimit_policy_t *policy,
                               mres_clock_fn clock);
```

Initialise with full tokens.

#### mres_ratelimit_acquire

```c
bool mres_ratelimit_acquire(mres_ratelimit_t *rl, uint16_t count,
                            mres_clock_fn clock);
```

Try to consume `count` tokens. Returns `true` if allowed, `false` if
rate limited. Automatically refills based on elapsed time before checking.

#### mres_ratelimit_tokens

```c
uint16_t mres_ratelimit_tokens(mres_ratelimit_t *rl, mres_clock_fn clock);
```

Get current available tokens (after refill).

#### mres_ratelimit_reset

```c
mres_err_t mres_ratelimit_reset(mres_ratelimit_t *rl, mres_clock_fn clock);
```

Reset to full capacity.

---

## Thread safety

microres is **not thread-safe by default**. Each instance must be accessed
from one thread at a time. If you need to share a circuit breaker or rate
limiter across threads, protect it with your platform's mutex:

```c
/* FreeRTOS example */
xSemaphoreTake(breaker_mutex, portMAX_DELAY);
mres_breaker_call(&breaker, my_op, &ctx, my_clock);
xSemaphoreGive(breaker_mutex);
```

Different instances can be used from different threads without
synchronisation — they share no global state.
