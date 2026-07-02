# microres

`microres` is a small C library for three caller-owned resilience primitives:

- synchronous retry with fixed, linear, or exponential backoff
- a one-probe circuit breaker
- a discrete token-bucket rate limiter

The current repository state is an audited contract rewrite. It is not declared release-ready or production-ready in this repo.

## Scope

- C99 library API
- C11 and C++ consumer builds via CMake
- caller-owned state only
- no heap allocation
- no hidden globals
- no internal locking

## Non-goals

- async schedulers or coroutine APIs
- ISR-safe retry or breaker execution
- persistence or crash recovery
- implicit thread safety

## Quick start

```c
#include "mres.h"

static uint32_t app_clock(void *context) {
    (void)context;
    return 0u;
}

static int app_wait(void *context, uint32_t delay_ms) {
    (void)context;
    (void)delay_ms;
    return 0;
}

static int operation(void *context) {
    (void)context;
    return 0;
}

int main(void) {
    mres_platform_t platform = { NULL, app_clock, app_wait };
    mres_retry_policy_t retry_policy = { 3u, MRES_BACKOFF_LINEAR, 0u, 0u, 100u, 500u };
    mres_retry_t retry;
    int operation_result = 0;

    if (mres_retry_init(&retry, &retry_policy) != MRES_OK) {
        return 1;
    }

    if (mres_retry_exec(&retry, operation, NULL, &platform, &operation_result) != MRES_OK) {
        return 1;
    }

    return 0;
}
```

## Contracts

- Retry is synchronous. A positive computed delay without a wait callback returns `MRES_ERR_WAIT_REQUIRED`.
- Retry, breaker, and rate limiter each copy policy input during successful initialization.
- Library status and operation return codes are separated. Callback return values are reported through output parameters.
- Jitter uses per-instance xorshift state. It is deterministic for equal seeds and is not cryptographic.
- `uint32_t` clocks are treated as modulo millisecond counters. One full wrap cycle is the supported limit for elapsed-time reasoning.
- Separate instances are independent. Shared instances require external serialization around the full call, including callbacks.
- `longjmp` or other nonlocal exits from callbacks are unsupported and may leave an instance busy.

## Build and package support

- CMake target: `microres::microres`
- Generated public config header: `mres_config.h`
- Consumer fixtures:
  - `tests/consumers/add_subdirectory`
  - `tests/consumers/find_package`
  - `tests/consumers/cpp11`
  - `tests/consumers/cpp17`
  - `tests/consumers/cpp20`

## Documentation

- [API reference](docs/API_REFERENCE.md)
- [Cookbook](docs/COOKBOOK.md)
- [Design notes](docs/DESIGN.md)
- [Porting guide](docs/PORTING_GUIDE.md)
- [Issues and troubleshooting](docs/ISSUES.md)
- [Verification status](docs/VERIFICATION.md)
- [Contributing](CONTRIBUTING.md)
- [Security](SECURITY.md)
- [Changelog](CHANGELOG.md)

## Release workflow

Releases are tag-triggered only. The repository workflow is defined in [.github/workflows/release.yml](.github/workflows/release.yml) and operates on pushed `v*` tags.
