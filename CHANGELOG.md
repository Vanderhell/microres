# Changelog

All notable changes to this project are documented here.

## [1.0.0] - 2026-07-02

### Changed

- replaced ABI-sensitive public enums and `bool` fields with fixed-width public scalar contracts
- added authoritative generated config handling for `MRES_ENABLE_JITTER` and `MRES_MAX_ATTEMPTS`
- separated library status from callback result domains for retry and breaker operations
- rewrote retry, breaker, and rate-limiter contracts around copied policies and same-instance busy guards
- added CMake package metadata, compile-fail fixtures, consumer fixtures, CI workflow definitions, and tag-based release workflow
- rewrote repository documentation, cookbook, issues guide, verification notes, and contribution guidance

### Release notes

- first tag-backed release for the audited contract rewrite
