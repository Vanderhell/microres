# Cookbook

## 1. Minimal platform bundle

```c
mres_platform_t platform = { .context = NULL, .clock = app_clock, .wait = app_wait };
```

## 2. Fixed-delay retry

```c
mres_retry_policy_t policy = { 3u, MRES_BACKOFF_FIXED, 0u, 0u, 100u, 0u };
```

## 3. Linear backoff retry

```c
mres_retry_policy_t policy = { 4u, MRES_BACKOFF_LINEAR, 0u, 0u, 50u, 250u };
```

## 4. Exponential backoff retry

```c
mres_retry_policy_t policy = { 5u, MRES_BACKOFF_EXPONENTIAL, 0u, 0u, 25u, 500u };
```

## 5. Seeded jitter retry

```c
mres_retry_init(&retry, &policy);
mres_retry_seed(&retry, 1234u);
```

## 6. Caller-managed retry loop

```c
uint32_t delay_ms = 0u;
mres_delay_calc(&retry, attempt, &delay_ms);
```

## 7. Wait-required handling

Check for `MRES_ERR_WAIT_REQUIRED` when a policy can compute positive delays and no wait callback is available.

## 8. Breaker CLOSED/OPEN/HALF_OPEN flow

Use `mres_breaker_get_state` and `mres_breaker_remaining_ms` to inspect state explicitly.

## 9. Manual breaker reporting

Use manual success and failure reports only when no breaker call is active and the caller can associate the result with the current breaker generation.

## 10. Rate limiter acquire

`mres_ratelimit_acquire` returns `MRES_OK` and `allowed == false` when the limiter is temporarily empty.

## 11. Rate limiter query

Use `mres_ratelimit_tokens` to query the post-refill token count.

## 12. Wraparound clock mock

Simulate wrap with a test clock that advances from values near `UINT32_MAX` to small values after wrap.

## 13. Two independent instances

Use two retry or breaker instances rather than sharing one instance between unrelated operations.

## 14. External locking pattern

Lock around the entire call and its callbacks when multiple tasks share one instance.

## 15. CMake install/find_package consumer

See `tests/consumers/find_package`.

## 16. add_subdirectory consumer

See `tests/consumers/add_subdirectory`.

## 17. C++ consumer note

The repository includes C++11, C++17, and C++20 consumer fixtures.

## 18. Embedded main-loop note

Retry remains synchronous. A main loop that cannot block should compute delays and schedule its own later retry.

## 19. No-ISR retry/breaker warning

Retry and breaker callbacks may block and may execute arbitrary application code, so this repo does not claim ISR safety for them.

## 20. Abnormal termination note

If the process aborts, resets, faults, or loses power, in-memory retry, breaker, and limiter state is lost immediately.
