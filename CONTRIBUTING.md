# Contributing

## Baseline rules

- keep the library compatible with C99
- preserve caller-owned, fixed-memory design
- do not add heap allocation
- do not add hidden globals
- do not add async schedulers, thread pools, coroutine layers, or RTOS abstraction layers
- do not weaken tests to make a change pass
- do not tag or release unless explicitly requested

## Contract rules

- keep library status codes separate from callback result domains
- preserve fixed-width public ABI types
- preserve copied-policy semantics
- keep same-instance busy guards intact
- document any supported behavior before claiming it in README or docs

## Build and test expectations

- CMake packaging and consumer fixtures should remain functional
- compile-fail tests should use repository-defined diagnostics where possible
- shared-instance threading claims require explicit external-locking documentation and tests
