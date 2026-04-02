# internetometer-cpp

[English](./README.md) | **Русский**

[![Язык](https://img.shields.io/badge/language-C%2B%2B20-blue.svg)](#)
[![Сборка](https://img.shields.io/badge/build-CMake%203.20%2B-success.svg)](#)
[![Лицензия](https://img.shields.io/badge/license-MIT-yellow.svg)](../LICENSE)

Профессиональная C++-версия оригинального `internetometer-cli`.

`internetometer-cpp` — это консольная утилита для измерения параметров интернет-соединения через эндпоинты Яндекс Интернетометра (`yandex.ru/internet`). Проект сохраняет поведение исходного Go CLI и предоставляет чистую архитектуру на C++ с CMake-сборкой.

## Возможности

- Определение IPv4 и IPv6
- Определение региона (режимы RU/EN)
- Определение провайдера (ISP) и ASN
- Speed test:
  - задержка (latency)
  - скорость скачивания (download)
  - скорость отдачи (upload)
- Несколько форматов вывода:
  - человекочитаемый текст
  - JSON
  - метрики Prometheus
  - JSONL-лог (`--save`)
- Интерактивный TUI (`--tui`)
- Гибкие настройки timeout и concurrency

## Структура проекта

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

## Требования

- CMake `3.20+`
- Компилятор с поддержкой C++20
  - GCC 11+
  - Clang 13+
  - MSVC 2022+
- `libcurl`
- `nlohmann_json`

## Сборка

### Linux / macOS

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j
```

Путь к бинарнику:

- `cpp/build/internetometer`

### Windows (PowerShell)

```powershell
cmake -S cpp -B cpp/build
cmake --build cpp/build --config Release
```

Путь к бинарнику (multi-config генераторы):

- `cpp/build/Release/internetometer.exe`

## Быстрый старт

Запуск интерактивного режима (по умолчанию, если не переданы флаги вывода):

```bash
internetometer
```

Полная проверка в текстовом виде:

```bash
internetometer --all
```

Вывод в JSON:

```bash
internetometer --all --json
```

Метрики Prometheus:

```bash
internetometer --prometheus
```

## Флаги CLI

| Флаг | Описание | Значение по умолчанию |
|---|---|---|
| `--ip` | Показать IPv4 и IPv6 | `false` |
| `--speed` | Выполнить speed test (latency/download/upload) | `false` |
| `--all` | Запустить все проверки и вывести системную информацию | `false` |
| `--json` | Вывод результата в JSON | `false` |
| `--prometheus` | Вывод в формате метрик Prometheus | `false` |
| `--tui` | Принудительно включить интерактивный TUI | `false` |
| `--lang <ru\|en>` | Язык источника названия региона | `en` |
| `--concurrency <N>` | Количество параллельных воркеров в speed test | `4` |
| `--timeout <value>` | Глобальный timeout (`60`, `60s`, `500ms`, `2m`, `1h`) | `60s` |
| `--save <path>` | Добавить JSON-результат в JSONL-файл | пусто |

## Примеры

```bash
# Только IP
internetometer --ip

# Только speed test
internetometer --speed

# Полная диагностика + сохранение JSONL
internetometer --all --save internetometer.log.jsonl

# Английские названия регионов + больше потоков
internetometer --all --lang en --concurrency 8

# Короткий timeout
internetometer --speed --timeout 25s
```

## Формат Prometheus

Экспортируемые метрики:

- `internetometer_download_mbps`
- `internetometer_upload_mbps`
- `internetometer_latency_ms`

Если доступны данные, добавляются labels:

- `isp`
- `region`

## Технические детали

- HTTP-клиент: `libcurl`
- JSON-парсинг: `nlohmann_json`
- Параллелизм: `std::thread` + атомики
- Временная модель speed test соответствует логике оригинала (фиксированные окна для download/upload)
- TUI показывает текущую фазу и live throughput

## Диагностика проблем

### `cmake: command not found`

Установите CMake и проверьте, что он добавлен в `PATH`.

### Не найдены `libcurl` или `nlohmann_json`

Установите dev-пакеты через пакетный менеджер ОС или через vcpkg/conan, затем повторите CMake-конфигурацию.

### Низкие или нестабильные результаты скорости

- Увеличьте `--timeout`
- Подберите `--concurrency` (например: `4`, `6`, `8`)
- Выполните несколько прогонов и усредните результат

## Legal

Проект не является официальным продуктом Яндекса и не аффилирован с Яндексом.

## Лицензия

MIT. См. [LICENSE](../LICENSE).
