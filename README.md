# internetometer-cpp

[**English**](./README.md) | [Русский](./README.ru.md)

[![Language](https://img.shields.io/badge/language-C%2B%2B20-blue.svg)](#)
[![Build](https://img.shields.io/badge/build-CMake%203.20%2B-success.svg)](#)
[![License](https://img.shields.io/badge/license-MIT-yellow.svg)](../LICENSE)

A production-ready C++ rewrite of the original `internetometer-cli`.

`internetometer-cpp` is a command-line utility for measuring internet connection characteristics using Yandex Internetometer endpoints (`yandex.ru/internet`). It preserves the original Go CLI behavior while providing a clean C++ codebase and CMake build.

## Highlights

- IPv4 and IPv6 detection
- Region detection (RU/EN output mode)
- ISP and ASN extraction
- Network speed test:
  - Latency
  - Download throughput
  - Upload throughput
- Multiple output modes:
  - Human-readable text
  - JSON
  - Prometheus metrics
  - JSONL append logging (`--save`)
- Interactive terminal UI (`--tui`)
- Adjustable timeout and parallelism

## Project Structure

```text
cpp/
├─ CMakeLists.txt
├─ README.md
├─ README.ru.md
└─ src/
   ├─ main.cpp
   ├─ yandex_client.hpp
   ├─ yandex_client.cpp
   ├─ tui.hpp
   └─ tui.cpp
```

## Requirements

- CMake `3.20+`
- C++20 compatible compiler
  - GCC 11+
  - Clang 13+
  - MSVC 2022+
- `libcurl`
- `nlohmann_json`

## Build

### Linux / macOS

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j
```

Binary path:

- `cpp/build/internetometer`

### Windows (PowerShell)

```powershell
cmake -S cpp -B cpp/build
cmake --build cpp/build --config Release
```

Binary path (multi-config generators):

- `cpp/build/Release/internetometer.exe`

## Quick Start

Start interactive mode (default when no output flags are provided):

```bash
internetometer
```

Run full test in text format:

```bash
internetometer --all
```

JSON output:

```bash
internetometer --all --json
```

Prometheus metrics:

```bash
internetometer --prometheus
```

## CLI Flags

| Flag | Description | Default |
|---|---|---|
| `--ip` | Show IPv4 and IPv6 | `false` |
| `--speed` | Run speed test (latency/download/upload) | `false` |
| `--all` | Run all tests and include system info | `false` |
| `--json` | Output result as JSON | `false` |
| `--prometheus` | Output Prometheus metrics | `false` |
| `--tui` | Force interactive TUI mode | `false` |
| `--lang <ru\|en>` | Language for region source | `en` |
| `--concurrency <N>` | Parallel workers for speed test | `4` |
| `--timeout <value>` | Global timeout (`60`, `60s`, `500ms`, `2m`, `1h`) | `60s` |
| `--save <path>` | Append JSON result to JSONL file | empty |

## Examples

```bash
# IP only
internetometer --ip

# Speed only (plain text)
internetometer --speed

# Full diagnostics + save JSONL
internetometer --all --save internetometer.log.jsonl

# English region labels, higher parallelism
internetometer --all --lang en --concurrency 8

# Short timeout
internetometer --speed --timeout 25s
```

## Prometheus Output

Exports:

- `internetometer_download_mbps`
- `internetometer_upload_mbps`
- `internetometer_latency_ms`

When available, labels include:

- `isp`
- `region`

## Implementation Notes

- HTTP transport: `libcurl`
- JSON parsing: `nlohmann_json`
- Concurrency: `std::thread` + atomics
- Timing model matches original behavior (fixed test windows for download/upload)
- TUI provides live phase progress and current throughput

## Troubleshooting

### `cmake: command not found`

Install CMake and ensure it is available in `PATH`.

### Missing `libcurl` or `nlohmann_json`

Install development packages via your system package manager or vcpkg/conan, then re-run CMake.

### Very low/unstable speed results

- Increase `--timeout`
- Tune `--concurrency` (e.g. `4`, `6`, `8`)
- Re-run multiple times to average transient network effects

## Legal

This is an unofficial CLI client and is not affiliated with Yandex.

## License

MIT. See [LICENSE](../LICENSE).
