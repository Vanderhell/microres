# Porting Guide

## Minimum environment

- C99 compiler for the library
- a monotonic millisecond clock callback
- an optional wait callback for positive-delay retry execution

## Platform bundle recipe

```c
static uint32_t platform_clock(void *context) {
    (void)context;
    return 0u;
}

static int platform_wait(void *context, uint32_t delay_ms) {
    (void)context;
    (void)delay_ms;
    return 0;
}

static mres_platform_t platform = {
    NULL,
    platform_clock,
    platform_wait,
};
```

## Main-loop and task use

- Retry and breaker callbacks may block and may execute arbitrary application code.
- Do not call retry or breaker APIs from ISR context.
- Rate-limiter ISR use is not claimed by this repository.

## Shared instances

If multiple threads or tasks need the same instance, lock around the whole operation:

```c
lock();
status = mres_breaker_call(&breaker, operation, &ctx, &platform, &operation_result);
unlock();
```

Locking only around part of the operation is insufficient because callbacks run while the instance is active.

## POSIX note

Convert milliseconds carefully in host glue code. Avoid examples that multiply into narrower platform types without bounds checking.

## Windows note

`GetTickCount()`-style clocks fit the modulo `uint32_t` model, but the same wrap limits and serialization limits still apply.
