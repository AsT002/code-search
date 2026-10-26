# code-search

A local code search engine written in C++. It indexes Git repositories and finds code in them fast.

Work in progress. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md) for what's next.

## Requirements

- CMake 3.25 or newer
- Ninja
- A C++20 compiler (Clang or GCC)
- Optional: `clang-format` and `clang-tidy` (on macOS, `brew install llvm`)

Tested on macOS with Apple Clang. Linux should work but hasn't been tested yet.

## Build and test

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets:

| Preset    | What it does |
|-----------|--------------|
| `debug`   | Debug build |
| `release` | Optimized build, used for benchmarks |
| `asan`    | Debug build with AddressSanitizer and UndefinedBehaviorSanitizer |
| `tidy`    | Debug build that runs clang-tidy on every file |

All presets treat compiler warnings as errors.

## Formatting

```sh
cmake --build build/debug --target format        # fix formatting
cmake --build build/debug --target format-check  # check only
```

## Benchmarks

```sh
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/codesearch_benchmarks
```
