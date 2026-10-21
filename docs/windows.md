# Сборка под Windows (Visual Studio 2026 + CMake)

`oldbox` собирается штатным MSVC-генератором CMake. SDL3 скачивается
CMake-ом (3.2.30) и линкуется статически, поэтому рядом с `oldbox.exe` не
нужен `SDL3.dll`. Встроенный fallback ROM берётся из заранее
сгенерированного `src/bios/fallback_bios.inc`, так что GNU `as`/`ld` для
сборки не требуются.

## Требования

- **Visual Studio 2026** (18.x) с рабочей нагрузкой
  «Разработка классических приложений на C++» (MSVC v145, Windows 10/11 SDK).
- **CMake из состава VS2026** — отдельная установка не нужна. Скрипты
  находят `cmake.exe` через `vswhere` в
  `<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
  Это тот же CMake, который использует «Open Folder» и Developer
  PowerShell для VS2026. Для генератора `Visual Studio 18 2026` нужен
  CMake ≥ 4.2; в VS2026 он уже такой. Если `cmake` в PATH — standalone
  версии ≥ 4.2, скрипт тоже подойдёт.
- **Python 3** — только для подготовки ассетов (`tools/*.py`); сама сборка
  без него работает, потому что firmware уже сгенерирован.
- JIT (`--jit`) поддерживается в x64-сборке: код размещается через
  `VirtualAlloc`/`VirtualProtect` (W^X), внешних зависимостей нет. На
  не-x86-64 хосте опция завершится понятной ошибкой.
- Для запуска тестов FreeDOS нужны `assets/bios/EP-ZXA.bin`,
  `assets/vga/S3Trio64_1_14-04.bin` и `assets/floppy/freedos1440.img`
  (они не входят в репозиторий).

## Быстрый старт

Из корня репозитория:

```bat
build-windows.bat
```

или PowerShell:

```powershell
.\build-windows.ps1 -Preset windows-vs2026 -Configuration Release
```

Скрипт сам выбирает **CMake из VS2026** (через `vswhere`), печатает его
версию и делает три шага: configure → build → ctest. Если VS2026 не найден,
используется `cmake` из PATH. Результат:

```text
build\vs2026\Release\oldbox.exe
build\vs2026\Release\oldbox_tests.exe
```

## Вручную через CMake presets

```bat
cmake --preset windows-vs2026
cmake --build --preset windows-vs2026-release
ctest --preset windows-vs2026-release --output-on-failure
```

Debug-сборка:

```bat
cmake --preset windows-vs2026
cmake --build --preset windows-vs2026-debug
ctest --preset windows-vs2026-debug --output-on-failure
```

Открыть в Visual Studio: **File → Open → Folder…** и выбрать корень
репозитория; VS подхватит `CMakePresets.json`. Либо
`cmake --preset windows-vs2026` и открыть `build\vs2026\oldbox.slnx`
(или сгенерированный `.sln`).

### Ручной запуск CMake из VS2026

Если нужен именно bundled CMake, запустите из обычного `cmd`:

```bat
for /f "usebackq delims=" %i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -version "[18.0,19.0)" -property installationPath`) do set VS=%i
"%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --version
"%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset windows-vs2026
"%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build --preset windows-vs2026-release
```

В «Developer PowerShell for VS 2026» достаточно `cmake --preset ...`:
bundled CMake уже первый в PATH.

### Если VS2026/CMake 4.2 ещё нет

Используйте запасной preset VS2022:

```bat
build-windows.bat windows-vs2022
```

Он собирает тот же код MSVC 2022 (v143) и кладёт результат в
`build\vs2022\Release`. Генератор `Visual Studio 18 2026` появился в CMake
4.2; на более старом CMake preset VS2026 не будет виден.

## Ассеты

```bat
py -3 tools\prepare_assets.py --floppy
py -3 tools\make_freedos_floppy.py
```

`prepare_assets.py` извлекает BIOS/VGA ROM из `specs\` и создаёт пустую
дискету. `make_freedos_floppy.py` по умолчанию берёт FreeDOS-образ
`assets\floppy\awardflash1440.img`; если его нет, укажите любой
FreeDOS 1.2 floppy через `--base`. Если `freedos1440.img` отсутствует
(или это установочная дискета FD-SETUP без оболочки), сквозной тест FreeDOS
просто пропускается, остальные 374 теста идут.

## Запуск

С окном:

```bat
build\vs2026\Release\oldbox.exe ^
  --bios assets\bios\EP-ZXA.bin ^
  --vga-bios assets\vga\S3Trio64_1_14-04.bin ^
  --floppy assets\floppy\freedos1440.img
```

Без окна (проверка текстового буфера):

```bat
build\vs2026\Release\oldbox.exe ^
  --bios assets\bios\EP-ZXA.bin ^
  --floppy assets\floppy\freedos1440.img ^
  --headless --seconds 2 --dump-text
```

`--headless` не создаёт окно и не открывает аудиоустройство. Для окна
используется SDL3 (Direct3D 11/Wasapi), отдельных DLL не требуется.

## Особенности Windows-сборки

- Компилятор: MSVC `/W4 /permissive- /utf-8 /Zc:__cplusplus`;
  предупреждения не переводятся в ошибки.
- `SDL_MAIN_HANDLED` определён для `oldbox`, поэтому используется обычный
  `main()` и не требуется `WinMain`.
- `OLDBOX_PREBUILT_FIRMWARE=ON` по умолчанию на Windows: берётся
  `src/bios/fallback_bios.inc`. Чтобы пересобрать ROM из
  `src/bios/fallback_bios.S`, нужны GNU `as`/`ld` (например, MSYS2) и
  `-DOLDBOX_PREBUILT_FIRMWARE=OFF`.
- SDL3 можно не скачивать, если он уже установлен:
  `-DOLDBOX_FETCH_SDL=OFF -DCMAKE_PREFIX_PATH=C:\SDL3`.
  Для offline-сборки с FetchContent задайте
  `-DFETCHCONTENT_SOURCE_DIR_SDL3=C:\src\SDL3-3.2.30`.

## Диагностика

| Симптом | Решение |
|---|---|
| `Could not find generator "Visual Studio 18 2026"` | Обновите CMake до 4.2+ или используйте `windows-vs2022`. |
| Ошибка скачивания SDL3 | Проверьте сеть/прокси; задайте `FETCHCONTENT_SOURCE_DIR_SDL3` или `OLDBOX_FETCH_SDL=OFF` с установленным SDL3. |
| `oldbox_tests` не находит `assets/...` | Запускайте `ctest --preset ...` из корня репозитория: тест-пресет выставляет рабочую директорию на корень. |
| FreeDOS-тест пропущен | Нет `assets/floppy/freedos1440.img`; выполните `py -3 tools\make_freedos_floppy.py`. |
| Долгий первый configure | CMake скачивает и собирает SDL3; последующие сборки используют кэш `build\vs2026\_deps`. |

Linux-сборка не изменилась: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
или `cmake --preset linux-release`.
