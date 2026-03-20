# Design Rationale

Why microres is built the way it is.

---

## 1. Callback-based platform abstraction

**Decision:** The user provides `clock_fn` and `sleep_fn` callbacks rather
than microres calling platform APIs directly.

**Why:**

- Bare-metal STM32 uses `HAL_GetTick()`. FreeRTOS uses `xTaskGetTickCount()`.
  Zephyr uses `k_uptime_get_32()`. Linux uses `clock_gettime()`. There is
  no universal time API in C.
- A `#ifdef` approach (compile-time platform selection) forces the library
  to know about every platform. Adding a new platform means modifying the
  library. Callbacks invert this dependency.
- Callbacks make testing trivial — inject a mock clock that you control
  exactly. No need for real timers in unit tests.

**Tradeoff accepted:** Two extra function pointer parameters on some calls.
In practice, most users wrap these in a thin project-specific layer that
hides the callbacks.

---

## 2. Synchronous retry execution

**Decision:** `mres_retry_exec()` blocks (via `sleep_fn`) between attempts
and returns the final result.

**Alternative considered:** Async / state-machine retry where the caller
polls `mres_retry_step()` in a loop. This would support cooperative
multitasking better.

**Why synchronous:**

- It's the simplest correct API. One call, one result.
- On FreeRTOS, each task already has its own stack — blocking is fine.
- On bare-metal super-loops, `sleep_fn` can be NULL and all attempts
  execute immediately. The caller manages timing externally.
- Users who need async retry can build it trivially using `mres_delay_calc()`
  and their own timer.

**Tradeoff accepted:** The calling thread blocks during retry. Not ideal
for single-threaded event loops, but mitigated by the NULL sleep option.

---

## 3. Circuit breaker: consecutive vs sliding window

**Decision:** The breaker counts **consecutive** failures, not failures
within a time window.

**Why:**

- Consecutive counting requires one `uint8_t` counter. Sliding window
  requires a timestamp ring buffer — significantly more RAM and complexity.
- For IoT devices, consecutive failures are the natural failure mode. If
  the WiFi is down, every attempt fails in sequence. Sliding windows
  matter more for high-throughput servers with mixed success/failure.
- A single success resets the counter to zero. This is the right behaviour:
  if the resource is working, even intermittently, the breaker should stay
  closed.

**Tradeoff accepted:** A device that experiences 50% random failures
(alternating success/failure) will never trip the breaker. In practice,
this pattern is rare in IoT — failures tend to be correlated.

---

## 4. Token bucket rate limiter

**Decision:** Use token bucket rather than sliding window, leaky bucket,
or fixed window.

**Why:**

- Token bucket allows bursts up to the bucket capacity while enforcing
  a long-term average rate. This matches IoT patterns well: a sensor
  might batch-send 5 readings, then go quiet, then batch again.
- Fixed window has the boundary problem (double the rate at window edges).
- Sliding window requires per-call timestamps — too much memory for MCUs.
- Leaky bucket enforces perfectly smooth output — too restrictive for
  bursty IoT workloads.
- Token bucket state is three fields: `tokens`, `last_refill`, and a
  pointer to the policy. Minimal RAM.

**Tradeoff accepted:** Token bucket doesn't guarantee perfectly uniform
spacing between calls. If exact spacing is needed, use a timer directly.

---

## 5. uint32_t time with wrap handling

**Decision:** All timestamps are `uint32_t` milliseconds, and elapsed
time is computed with unsigned subtraction (`now - start`).

**Why:**

- `uint32_t` wraps at ~49.7 days. Unsigned subtraction naturally handles
  this wrap: if `start = 0xFFFFFF00` and `now = 0x00000100`, then
  `now - start = 0x200` (512 ms) — correct.
- `uint64_t` would avoid wrap but doubles the size of every timestamp
  field and isn't natively supported on some 8/16-bit MCUs.
- Most IoT timeouts are seconds to minutes. 49.7 days of continuous
  uptime without a single successful operation is beyond any realistic
  recovery scenario.

**Tradeoff accepted:** If a breaker stays OPEN for > 49.7 days (without
the device rebooting), the timeout check may wrap. This is not a
realistic scenario.

---

## 6. No dynamic allocation

Same rationale as microfsm: many bare-metal projects disable the heap.
All state is caller-provided. Policies are `const` and ROM-safe.

---

## 7. Jitter with xorshift

**Decision:** Jitter uses a xorshift32 PRNG seeded from the clock value
rather than calling `rand()`.

**Why:**

- `rand()` is not reentrant on many embedded platforms. Calling it from
  ISR context or multiple tasks without a mutex is undefined behaviour.
- `rand()` requires `srand()` initialisation. If the user forgets, the
  jitter is deterministic (always the same seed).
- xorshift32 is 4 lines of code, has good distribution for our purpose,
  and is seeded differently on each call (clock + attempt number).
- The jitter doesn't need to be cryptographic — it just needs to
  decorrelate retries from multiple devices. Even mediocre randomness
  achieves this.

**Tradeoff accepted:** The PRNG is not cryptographically secure. This is
fine — jitter is a performance optimisation, not a security feature.

---

## 8. Composability over integration

**Decision:** Retry, circuit breaker, and rate limiter are independent
primitives that can be combined by the user, rather than a single
integrated "resilience pipeline".

**Why:**

- Not every use case needs all three. A sensor polling loop needs retry
  but not rate limiting. An API client needs rate limiting but maybe not
  a circuit breaker.
- A pipeline model would need to define a fixed execution order. But the
  right order depends on the use case: sometimes you rate-limit first,
  sometimes you check the breaker first.
- Independent primitives are easier to test, easier to understand, and
  produce smaller binaries (linker can strip unused code).

**Tradeoff accepted:** The user writes the composition code. This is
typically 5–10 lines — see PATTERNS.md for examples.

---

## 9. Naming and prefix

**Why "microres"?**

- **micro** — microcontroller-friendly, matching the microfsm naming.
- **res** — resilience.
- The `mres_` prefix is short, unique, and consistent across all symbols.

---

## Summary of tradeoffs

| Decision | Gains | Costs |
|----------|-------|-------|
| Callback abstraction | Any platform, easy testing | Extra fn params |
| Synchronous retry | Simple API, one call | Blocks the thread |
| Consecutive failures | 1 byte counter, simple | Won't trip on mixed results |
| Token bucket | Allows bursts, tiny state | No exact spacing |
| uint32_t time | Compact, wrap-safe | 49.7 day wrap edge case |
| No malloc | Deterministic, safe | Caller manages memory |
| xorshift jitter | No rand(), reentrant | Not cryptographic |
| Composable primitives | Flexible, testable, small | User writes glue code |
