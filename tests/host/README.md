# Factory reset host regression test

Compiles the production settings and startup reset modules with in-memory NVS,
virtual time, and GPIO stubs. No ESP32 is required; this does not simulate ROM
boot strapping, flash hardware, or Wi-Fi startup.

With a C11 host compiler (GCC or Clang), from the repository root:

```sh
mkdir -p build_host_tests
cc -std=c11 -Itests/host/stubs -Imain main/repeater_settings.c main/startup_factory_reset.c tests/host/factory_reset_test.c -o build_host_tests/factory_reset_test
./build_host_tests/factory_reset_test
```

Do not compile with NDEBUG: the test uses assert for checks.

On Windows, in a Visual Studio developer command prompt (VS 2022 with C11 atomics):

```bat
if not exist build_host_tests mkdir build_host_tests
cl /nologo /std:c11 /experimental:c11atomics /D_CRT_SECURE_NO_WARNINGS /Itests/host/stubs /Imain main/repeater_settings.c main/startup_factory_reset.c tests/host/factory_reset_test.c /Fobuild_host_tests/ /Febuild_host_tests/factory_reset_test.exe
build_host_tests\factory_reset_test.exe
```
