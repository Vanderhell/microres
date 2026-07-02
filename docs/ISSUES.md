# Issues And Troubleshooting

## NULL assert hook no longer replaces validation

`MRES_ASSERT` is diagnostic only. Runtime validation still runs and returns a library status.

## Policy mutation after init

Policies are copied during successful initialization. Mutating the caller's policy object does not update an existing instance.

## Invalid jitter config

Generated configuration values outside the supported range fail compilation with an `MRES_DIAG_*` diagnostic.

## Wait callback failure

A non-zero wait callback return becomes `MRES_ERR_WAIT_FAILED`.

## Missing wait with positive delay

If retry computes a positive delay and `platform->wait` is absent, the call returns `MRES_ERR_WAIT_REQUIRED`.

## Operation error collision

Operation callback results are reported through output parameters so they cannot collide with library status values.

## Retry busy due to recursion

Same-instance recursive execution or reset returns `MRES_ERR_BUSY`.

## Breaker invalid state

Invalid breaker states fail closed with `MRES_ERR_INVALID`.

## Breaker open timing

Open timestamps are captured after the failing callback returns.

## Manual report semantics

Manual success in `OPEN` does not close the breaker. Manual failure in `OPEN` does not reset the existing timeout.

## Half-open probe count

This repository supports one synchronous half-open probe only.

## Rate limiter overflow/refill

Refill logic bounds multiplication through missing-token calculations to avoid overflow-driven underfill.

## Count zero behavior

`mres_ratelimit_acquire(..., 0, ...)` is invalid.

## Clock rollback unsupported

The library assumes a monotonic modulo counter within one wrap cycle. Arbitrary backward jumps are unsupported.

## Thread and ISR claims

Shared-instance thread safety and ISR safety are not claimed. External serialization is required for shared instances.

## CMake find_package

Use the installed package config and link `microres::microres` only.

## Makefile flags

The test Makefile now honors caller-provided `CPPFLAGS`, `CFLAGS`, and `LDFLAGS`.

## Compile-fail diagnostic differences

Compile-fail tests check repository-defined `MRES_DIAG_*` tokens rather than compiler-specific wording.

## Sanitizer or static-tool availability

Some CI jobs depend on toolchain availability in the runner image and should be treated as environment-sensitive until verified.
