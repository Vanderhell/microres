# Contributing to microres

Thank you for considering a contribution.

## Scope

Contributions that align with the project philosophy:

- Bug fixes and edge case handling.
- Documentation and example improvements.
- Test coverage improvements.
- Platform porting notes.
- Performance improvements without added complexity.

Out of scope:

- Async/coroutine retry. Use `mres_delay_calc()` and your own scheduler.
- Thread-safe wrappers. These are platform-specific.
- Dynamic allocation. The library is zero-alloc by design.
- External dependencies. Zero dependencies by design.

## How to contribute

1. Open an issue first.
2. Fork and branch (`fix/description` or `feat/description`).
3. Write tests for your changes.
4. Follow the code style (C99, 4-space indent, `mres_` prefix, `const` correctness).
5. Submit a PR referencing the issue.

## Testing

```bash
cd tests
make
```

All tests must pass on GCC and Clang with `-Wall -Wextra -Wpedantic -Werror`.

## License

By contributing, you agree that your contributions will be licensed under
the MIT License.
