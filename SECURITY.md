# Security Policy

## Reporting

Report suspected security issues privately to the repository maintainer instead of opening a public issue with exploit details.

## Scope notes

This repository is a small C utility library with caller-owned state. It does not claim:

- cryptographic randomness
- persistence or tamper resistance
- fault recovery after abnormal termination
- thread-safe shared-instance behavior without caller locking
