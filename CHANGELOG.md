# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] — 2026-03-20

### Added

- Retry engine with fixed, linear, and exponential backoff strategies.
- Optional ±25% jitter with xorshift32 PRNG.
- Delay cap (`max_delay_ms`) for all strategies.
- Non-blocking mode (NULL sleep function).
- `mres_delay_calc()` for custom retry loops.
- Circuit breaker with three-state model (CLOSED → OPEN → HALF_OPEN).
- Configurable failure threshold, recovery timeout, and probe call limit.
- Manual success/failure reporting for async patterns.
- Token bucket rate limiter with configurable refill rate and capacity.
- Full documentation: API reference, design rationale, porting guide, patterns.
- Test suite: 41 tests covering all primitives, edge cases, and error paths.
- Platform recipes for ESP32, STM32, Zephyr, Arduino, Linux, Windows.
