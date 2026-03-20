# microres

**Fault-tolerance primitives for embedded systems.**

[![C99](https://img.shields.io/badge/C-99-00599C.svg)](https://en.wikipedia.org/wiki/C99)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Zero Dependencies](https://img.shields.io/badge/dependencies-0-success.svg)](#features)
[![Zero Allocation](https://img.shields.io/badge/allocation-zero-success.svg)](#features)
[![Platform Agnostic](https://img.shields.io/badge/platform-agnostic-informational.svg)](docs/PORTING_GUIDE.md)

C99 · Zero dependencies · Zero allocations · Platform-agnostic · Portable

---

## Why microres?

Every IoT device eventually faces the same problems: the WiFi drops, the
MQTT broker doesn't respond, the sensor returns garbage, the flash write
fails. Every developer writes the same retry loops, the same backoff
timers, the same "if it failed 5 times, stop trying" logic — over and over,
scattered across the codebase, each slightly different, each with its own
bugs.

**microres** extracts these patterns into tested, reusable primitives:

```
┌─────────────────────────────────────────────────────────┐
│                    Your application                     │
├─────────────┬──────────────────┬────────────────────────┤
│   Retry     │  Circuit Breaker │     Rate Limiter       │
│             │                  │                        │
│ exponential │  CLOSED ──▶ OPEN │  token bucket          │
│ linear      │    ▲         │   │  fixed window          │
│ fixed       │    └─ HALF ◀─┘   │                        │
│ + jitter    │    OPEN          │                        │
├─────────────┴──────────────────┴────────────────────────┤
│              microres engine (~400 lines)                │
├─────────────────────────────────────────────────────────┤
│     Platform: clock_fn + sleep_fn (you provide)         │
└─────────────────────────────────────────────────────────┘
```

## Features

- **Retry with backoff** — fixed, linear, or exponential delay with
  optional jitter. Configurable max attempts and max delay cap.
- **Circuit breaker** — prevents repeated calls to a failing resource.
  Three states: closed (normal) → open (blocked) → half-open (probing).
  Automatic recovery after a timeout.
- **Rate limiter** — token bucket algorithm to throttle calls. Useful for
  API rate limits, sensor polling caps, or burst control.
- **Composable** — combine retry inside a circuit breaker, or rate-limit
  before retrying. Each primitive is independent.
- **Platform-agnostic** — you provide two callbacks: `clock_fn` (get
  current time in ms) and `sleep_fn` (delay for N ms). Works on any
  platform: bare metal, FreeRTOS, Zephyr, Linux, Windows.
- **Zero dynamic allocation** — all state is caller-provided. Policies
  are `const` and ROM-safe.
- **Deterministic** — no hidden state, no globals, no surprises. Every
  call's behaviour is fully determined by its inputs.
- **Small footprint** — ~400 lines of C. Typical compiled size: < 1.5 KB
  text, < 20 B RAM per instance.

## Quick start

### 1. Add to your project

```
your_project/
├── lib/
│   └── microres/
│       ├── include/
│       │   └── mres.h
│       └── src/
│           └── mres.c
```

### 2. Provide platform callbacks

```c
#include "mres.h"

/* ESP-IDF / FreeRTOS example */
static uint32_t my_clock(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void my_sleep(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* STM32 HAL example */
static uint32_t my_clock(void) { return HAL_GetTick(); }
static void my_sleep(uint32_t ms) { HAL_Delay(ms); }

/* Linux example */
#include <time.h>
#include <unistd.h>
static uint32_t my_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static void my_sleep(uint32_t ms) { usleep(ms * 1000); }
```

### 3. Retry an operation

```c
/* The operation to retry — returns 0 on success, negative on failure */
static int connect_mqtt(void *ctx) {
    mqtt_client_t *c = (mqtt_client_t *)ctx;
    int err = mqtt_connect(c->host, c->port);
    return err;  /* 0 = success, -1 = failed */
}

/* Configure retry policy */
static const mres_retry_policy_t mqtt_retry = {
    .max_attempts  = 5,
    .base_delay_ms = 1000,
    .max_delay_ms  = 30000,
    .strategy      = MRES_BACKOFF_EXPONENTIAL,
    .jitter        = true,
};

/* Execute with retry */
mres_retry_t retry;
mres_retry_init(&retry, &mqtt_retry);

int result = mres_retry_exec(&retry, connect_mqtt, &client, my_clock, my_sleep);
if (result == MRES_OK) {
    printf("Connected after %d attempts\n", retry.attempts);
} else {
    printf("Failed after %d attempts\n", retry.attempts);
}
```

### 4. Protect with a circuit breaker

```c
static const mres_breaker_policy_t broker_policy = {
    .failure_threshold    = 3,       /* open after 3 consecutive failures */
    .recovery_timeout_ms  = 60000,   /* try again after 60 seconds */
    .half_open_max_calls  = 1,       /* allow 1 probe call in half-open */
};

mres_breaker_t breaker;
mres_breaker_init(&breaker, &broker_policy);

/* Each call goes through the breaker */
int result = mres_breaker_call(&breaker, connect_mqtt, &client, my_clock);

switch (result) {
case MRES_OK:
    /* Success — breaker stays closed / transitions to closed */
    break;
case MRES_ERR_OPEN:
    /* Breaker is open — call was NOT made, resource is down */
    printf("Broker unreachable, retry in %lu ms\n",
           mres_breaker_remaining_ms(&breaker, my_clock));
    break;
default:
    /* Operation failed — breaker tracks the failure */
    break;
}
```

### 5. Throttle with a rate limiter

```c
static const mres_ratelimit_policy_t api_limit = {
    .max_tokens  = 10,       /* 10 calls allowed */
    .refill_ms   = 1000,     /* per second */
    .refill_count = 10,      /* refill all 10 tokens each interval */
};

mres_ratelimit_t limiter;
mres_ratelimit_init(&limiter, &api_limit, my_clock);

if (mres_ratelimit_acquire(&limiter, 1, my_clock)) {
    /* Allowed — make the API call */
    send_telemetry(&data);
} else {
    /* Throttled — skip or queue */
    printf("Rate limited, %d tokens left\n", mres_ratelimit_tokens(&limiter));
}
```

### 6. Compose patterns

```c
/* Retry inside a circuit breaker */
int resilient_publish(void *ctx) {
    mqtt_ctx_t *m = (mqtt_ctx_t *)ctx;

    /* Rate limit first */
    if (!mres_ratelimit_acquire(&m->limiter, 1, my_clock)) {
        return MRES_ERR_RATE_LIMITED;
    }

    /* Circuit breaker wraps the retry */
    mres_retry_t retry;
    mres_retry_init(&retry, &publish_retry_policy);

    return mres_breaker_call(&m->breaker, publish_with_retry, m, my_clock);
}
```

## Configuration

All options are compile-time `#define`s.

| Macro                 | Default | Description                                  |
|-----------------------|---------|----------------------------------------------|
| `MRES_ENABLE_JITTER`  | 1       | Enable jitter support (needs simple PRNG)    |
| `MRES_ASSERT(expr)`   | (none)  | Custom assert macro                          |
| `MRES_MAX_ATTEMPTS`   | 255     | Maximum retry attempts (uint8_t range)       |

## API at a glance

### Retry

| Function            | Description                              |
|---------------------|------------------------------------------|
| `mres_retry_init`   | Initialise retry state                   |
| `mres_retry_exec`   | Execute operation with retry + backoff   |
| `mres_retry_reset`  | Reset attempt counter                    |
| `mres_delay_calc`   | Calculate delay for a given attempt      |

### Circuit Breaker

| Function                   | Description                           |
|----------------------------|---------------------------------------|
| `mres_breaker_init`        | Initialise circuit breaker            |
| `mres_breaker_call`        | Execute operation through breaker     |
| `mres_breaker_state`       | Get current state (closed/open/half)  |
| `mres_breaker_state_name`  | State as string                       |
| `mres_breaker_remaining_ms`| Time until recovery attempt           |
| `mres_breaker_reset`       | Force breaker to closed               |

### Rate Limiter

| Function                | Description                            |
|-------------------------|----------------------------------------|
| `mres_ratelimit_init`   | Initialise rate limiter                |
| `mres_ratelimit_acquire`| Try to consume tokens                  |
| `mres_ratelimit_tokens` | Get current token count                |
| `mres_ratelimit_reset`  | Reset to full tokens                   |

Full reference: **[docs/API_REFERENCE.md](docs/API_REFERENCE.md)**

## Documentation

| Document                                  | Content                                |
|-------------------------------------------|----------------------------------------|
| [API Reference](docs/API_REFERENCE.md)    | Every function, type, and macro        |
| [Design Rationale](docs/DESIGN.md)        | Architecture decisions and tradeoffs   |
| [Porting Guide](docs/PORTING_GUIDE.md)    | Platform integration recipes           |
| [Patterns](docs/PATTERNS.md)              | Real-world composition patterns        |

## Building the tests

```bash
cd tests
make            # builds and runs all tests
```

Requires only a C99 compiler (gcc or clang) and make.

## Project structure

```
microres/
├── include/
│   └── mres.h               # public header
├── src/
│   └── mres.c               # implementation (~400 lines)
├── tests/
│   ├── test_all.c            # complete test suite
│   └── Makefile
├── docs/
│   ├── API_REFERENCE.md
│   ├── DESIGN.md
│   ├── PORTING_GUIDE.md
│   └── PATTERNS.md
├── README.md
├── CHANGELOG.md
├── CONTRIBUTING.md
└── LICENSE
```

## Ecosystem

microres pairs well with other embedded libraries:

| Library | Role | Combination |
|---------|------|-------------|
| [microfsm](https://github.com/Vanderhell/microfsm) | State machine | Guard conditions use breaker state |
| [iotspool](https://github.com/Vanderhell/iotspool) | MQTT queue | Retry failed publishes from spool |

## License

MIT — see [LICENSE](LICENSE).
