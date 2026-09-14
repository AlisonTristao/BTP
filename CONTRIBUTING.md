# Contributing to BTP

Thank you for contributing to the Binary Telemetry Protocol.

## Before submitting a change

- Read the project documentation in `docs/` and the relevant sections of `README.md`.
- Keep protocol and wire-format changes backward-compatible whenever possible.
- Update the documentation, examples, and test vectors when behavior or the protocol specification changes.
- Do not commit build output, generated files, credentials, or device-specific configuration.

## Building and testing

Configure the project with CMake, then build and run the test suite:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For documentation changes, build the MkDocs site using the dependencies listed in `requirements-docs.txt`.

## Pull requests

Please explain the motivation for the change, describe any protocol or API impact, and include focused tests for new behavior or regressions. Keep unrelated formatting and refactoring out of the same pull request.

By submitting a contribution, you agree that it may be distributed under the project's license.
