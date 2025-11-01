# EconoPET

## Firmware Unit Tests

Firmware unit tests run on the Linux host using mock implementations of the EconoPET hardware.

```sh
cmake --build build --target test_project
ctest --test-dir build --output-on-failure --verbose -L fw
```
