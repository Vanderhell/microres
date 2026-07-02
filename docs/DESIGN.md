# Design Notes

## Why the API changed

This repository originally mixed library status codes with callback return values, exposed ABI-sensitive public enums and `bool` fields, and documented behaviors the implementation did not actually provide. The current design tightens those contracts.

## Fixed public ABI

- public status and state scalar types are fixed-width integers
- public structs avoid `bool` storage
- generated configuration is treated as authoritative input to the public ABI

## Copied policies

Retry, breaker, and rate limiter copy validated policy values by value. Post-init mutation of the caller's policy object does not alter instance behavior.

## Explicit platform ownership

The platform bundle carries callback pointers plus caller-owned context:

- no globals are required
- multiple independent platform bundles can coexist
- callback ownership stays with the caller

## Synchronous-only retry

Retry is intentionally synchronous. The library does not attempt to provide an async scheduler, polling state machine, or event-loop abstraction. Caller-managed loops can use `mres_delay_calc`.

## Reentrancy model

- one execution context owns one instance at a time
- same-instance recursion is rejected with `MRES_ERR_BUSY`
- separate instances are independent
- shared instances need external serialization across the entire call, including callbacks

## Time model

Elapsed time uses unsigned subtraction on modulo `uint32_t` millisecond values. This supports natural wrap only when the caller does not exceed a full counter cycle between relevant observations. Backward jumps are unsupported and not generally detectable.

## No persistence guarantee

All instance state is RAM state only:

- no persistence
- no crash recovery
- no exactly-once guarantee
- abnormal termination stops execution immediately
