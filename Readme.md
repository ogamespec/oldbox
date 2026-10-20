# oldbox

`oldbox` — эмулятор IBM PC-совместимого компьютера 1998 года на C++20 с
SDL3-фронтендом. Проект написан с нуля по первичным спецификациям из `specs/`;
код других эмуляторов не использовался.

Целевая машина — **Reference Board** (EPoX/ProNiX EP-61BXA-M, i440BX) в
следующей конфигурации:

| Компонент | Реализация |
|---|---|
| CPU | Pentium II 233 MHz (Klamath) или 333 MHz (Deschutes), интерпретатор IA-32 |
| Chipset | Intel 82443BX + 82371AB PIIX4 (PCI, ISA, IDE, PIC, PIT, RTC, DMA, SMBus, 8042) |
| RAM | 32/64 MiB SDRAM (настраивается, 8–512 MiB), 3 slot |
| Flash | W29C020-12 (2MBit) |
| 2D video | S3 Trio64 (86C764), 4 MiB VRAM, VGA + S3 2D engine |
| 3D video | 3dfx Voodoo Graphics (SST-1, Diamond Monster 3D), 4 MiB, софтверный растеризатор |
| Audio | Creative Sound Blaster AWE64 Gold: SB16 DSP, CT1745 mixer, MPU-401, EMU8000, OPL3-совместимый fallback |
| Storage | PIIX4 IDE (PIO), ATA/ATAPI CD-ROM, 82077AA FDC 1.44 MiB |
| Input | PS/2 keyboard + PS/2 mouse через 8042 |
| Firmware | Award BIOS из `specs/`, встроенный диагностический POST ROM, S3 Trio64 option ROM |

## Быстрый старт

Требования: CMake ≥ 3.25, компилятор с C++20, Python 3, `as`/`ld` (binutils).
Windows-сборка (Visual Studio 2026, MSVC, без `as`/`ld`) описана в
[docs/windows.md](docs/windows.md):
`build-windows.bat` или `cmake --preset windows-vs2026` + `cmake --build --preset windows-vs2026-release`.
Готовый бинарник Windows — `build\vs2026\Release\oldbox.exe`; быстрый запуск
оригинального Award BIOS в окне — `run.bat` (меню: обычный запуск, автовход в
Setup по Del, FreeDOS-дискета, headless-проверка). В эмуляторе: `F11` — полный
экран, `F12` — скриншот в `oldbox-screenshot.bmp`. Горячей клавиши выхода нет:
окно закрывается только крестиком, а `Esc` и остальные клавиши уходят гостю.
Жёсткий диск: `assets\hdd\oldbox-hdd-500mb.img` — 500 МБ raw-образ
(таблица разделов MBR, раздел FAT16), подключается опцией `--hdd FILE` как
Primary Master (BIOS определяет его как `LBA, Mode 4, 524MB`).
SDL3 при отсутствии системного пакета скачивается CMake-ом (release 3.2.30).
На Linux без заголовков X11/Wayland SDL собирается в console-режиме; для окна
нужны `libx11-dev`/`libwayland-dev` (или Windows-сборка).

```bash
python3 tools/prepare_assets.py --floppy
# нужен FreeDOS-форматированный образ-основа (по умолчанию
# assets/floppy/awardflash1440.img; подойдёт любой FreeDOS 1.2 floppy)
python3 tools/make_freedos_floppy.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Запуск FreeDOS с настоящим Award-дампом (boot block загружает дискету и
передаёт управление FreeDOS; в конце виден промпт `A:\>`):

```bash
./build/oldbox --bios assets/bios/EP-ZXA.bin \
               --vga-bios assets/vga/S3Trio64_1_14-04.bin \
               --cpu 333 --memory 64 \
               --floppy assets/floppy/freedos1440.img
```

Для проверки без окна:

```bash
./build/oldbox --bios assets/bios/EP-ZXA.bin \
               --floppy assets/floppy/freedos1440.img \
               --headless --seconds 2 --dump-text
```

Без BIOS-файла используется встроенный 64 KiB POST ROM, который показывает
конфигурацию, перечисляет PCI-устройства и пытается загрузиться с ATA-диска.

Полезные режимы:

```bash
./build/oldbox --headless --seconds 5 --dump-text
./build/oldbox --selftest
./build/oldbox --list-devices
./build/oldbox --headless --seconds 10 --screenshot screen.ppm
./build/oldbox --demo voodoo --headless --screenshot voodoo.ppm
```

`--headless` не создаёт окно; `--dump-text` печатает VGA text buffer,
`--dump-ram` сохраняет первые 1 MiB гостевой RAM, `--screenshot` пишет PPM.
F1 захватывает/отпускает мышь, F11 — fullscreen, F12 — BMP-скриншот. Выхода по
горячей клавише нет: окно закрывается крестиком, все клавиши (включая Esc)
передаются гостю.

## Что уже работает

- **FreeDOS 1.2 загружается с дискеты до промпта `A:\>`** на реальном
  `EP-ZXA.bin` (Award BootBlock проверяет контрольную сумму, читает дискету,
  FreeDOS распаковывает `KERNEL.SYS` UPX-декомпрессором и запускает FreeCom).
- Интерпретатор IA-32: real mode, protected mode, полноценный V86-monitor
  (IDT/TSS, стек ring 0, IOPL-проверки, возврат через IRET), VME
  (CR4.VME, interrupt redirection bitmap, VIF/VIP), сегментация, GDT/LDT,
  TSS и переключение стека, 4K/PSE/PAE paging, IDT/исключения, MSR, CPUID,
  SYSENTER/SYSEXIT, call/task gates, 16/32-битные TSS, debug-регистры,
  machine check, performance counters, APIC, FPU (x87), MMX, строковые
  операции, REP/REPZ/REPNE.
- i440BX/PIIX4: PCI config mechanism #1, BAR-ы, PIC, PIT, RTC/CMOS, DMA,
  8042, SMBus + SPD EEPROM, PM-блок, порт 0x92 (A20/reset), ELCR.
- S3 Trio64: VGA-плоскости, text 720×400, mode 13h/12h, linear modes,
  palette/DAC, S3 unlock, packed MMIO 2D engine (fill/blit/line).
- Voodoo SST-1: PCI 121a:0001, 16 MiB BAR, регистровый файл, triangle setup,
  edge walking, Z/alpha/chroma/fog/stipple, перспективно-корректные текстуры,
  mip/bilinear, LFB и swapbuffer. Это именно растровый софтверный конвейер,
  без Glide/OpenGL и без перехвата вызовов.
- AWE64: reset/version DSP, mixer, single/auto-init DMA, MPU-401 UART,
  EMU8000 (32 голоса), OPL3 fallback с таймерами/статус-регистром OPL2
  (YM3812: период (256−N)×80.8 мкс, RST/ST, флаги FT1/FT2/IRQ), SDL audio.
- PC Speaker: канал 2 PIT (1193182/reload Гц) через порт 61h (gate/data),
  квадратная волна микшируется в тот же поток, что и звуковая карта, —
  бипер не блокирует и не заменяет звук AWE64.
- 8042: PS/2-мышь по IBM PS/2 HITR (статус 64h, командный байт, команды
  контроллера и устройства, 3-байтовые пакеты, IRQ12); в гостевой FreeDOS
  на `freedos-games.img` грузится штатный Microsoft `MMOUSE.COM`
  (INT 33h), собирается `tools/make_games_floppy.py`.
- ATA/ATAPI и FDC: PIO-команды, IDENTIFY, ISO9660 CD, 1.44M floppy + DMA2.
- I/O: полная 64K-карта портов, 8/16/32-битные доступы, разбиение при
  пересечении границ устройств, IOPL/CPL и I/O permission bitmap в TSS.
- A20: mask в Bus, port 0x92 и 8042 output port, тесты на aliasing.

## Статус BIOS

`EP-ZXA.bin` — настоящий 256 KiB образ Award BIOS для EP-61BXA-M. Он
запускается в Award CMOS Setup.

**Жёсткие правила прошивки (обязательны):**

- Используется **только дамп настоящего Award BIOS** (`--bios`, по
  умолчанию ищется `assets/bios/EP-ZXA.bin`). Встроенной/запасной
  прошивки не существует: `src/bios` удалён, а без реального дампа
  эмулятор отказывается стартовать.
- **Никаких хуков BIOS-сервисов.** `oldbox` — low-level эмулятор: гость
  выполняет настоящий код Award BIOS, и каждое прерывание процессора
  обязано идти в этот код и в реальные модели устройств
  (8259/82077AA/IDE/RTC/SMBus, ...). Перехват INT 10h/13h/15h/1Ah (или
  любого другого вектора) для подмены сервисов запрещён. Если гость
  зависает — чинить модель устройства или CPU, а не прошивку. См. маркер
  `NO BIOS-SERVICE HOOKS` в [src/machine.cpp](src/machine.cpp).

## Тесты

```bash
./build/oldbox_tests
```

Покрытие: 279 тестов — ALU/флаги всех размеров с эталонной моделью,
ADC/SBB с граничным переносом, MUL/IMUL/DIV/IDIV, BCD (DAA/DAS полным
перебором, AAA/AAS/AAM/AAD), сдвиги/вращения и их флаги, bit scan и
BT/BTS/BTR/BTC со знаковыми смещениями, SETcc/CMOVcc, строковые операции
и REPZ/REPNE, stack/ENTER/far call, FPU и MMX, real/protected/V86,
V86-monitor и VME (IDT/TSS-стек, IOPL, redirection bitmap, VIF/VIP),
call/task gates, 16/32-битные TSS, IRET NT, debug-регистры и breakpoints,
PGE/PVI/AC, machine check, MTRR/perf MSR, APIC priority/timer, SMM
(LDT/auto-HALT/NMI), x87 FPREM/FSINCOS/FPTAN/FXTRACT, paging 4K/PSE/PAE,
page-fault error code, MSR/CPUID/SYSENTER, I/O-привилегии и bitmap,
A20/port 0x92/8042, PCI/BAR, Voodoo-растеризация, S3/VGA, ATA/ATAPI/FDC,
SB16/EMU8000/OPL3 и сквозная загрузка FreeDOS до `A:\>`
(`freedos_boots_to_command_prompt_with_reference_bios`).

Аудит инструкций и найденные ошибки: [docs/cpu-audit.md](docs/cpu-audit.md).

## Документация

- [docs/architecture.md](docs/architecture.md) — структура проекта и поток данных.
- [docs/cpu.md](docs/cpu.md) — модель Pentium II, режимы, MSR, покрытие и ограничения.
- [docs/cpu-audit.md](docs/cpu-audit.md) — методика аудита инструкций, найденные
  ошибки, покрытие по группам и остаточные пробелы.
- [docs/firmware.md](docs/firmware.md) — BIOS 256 KiB, shadow RAM, banking, POST.
- [assets/floppy/MEMTEST_WRAPPER.IMG](assets/floppy/MEMTEST_WRAPPER.IMG) —
  FAT12-обёртка для `MEMTEST.IMG` (собирается
  `tools/make_memtest_wrapper.py`).
- `assets/floppy/dd2.img` — загрузочная FreeDOS-дискета с Dangerous Dave 2
  (HIMEMX + DOS=HIGH, AUTOEXEC запускает DAVE2.EXE), собирается
  `tools/make_dd2_floppy.py` из `C:\Games\DD2`; в `run.bat` — пункт 9.
- [docs/cpu-features.md](docs/cpu-features.md) — матрица архитектурных
  возможностей Pentium Pro/Pentium II: реализовано/упрощено/не моделируется.
- [docs/io-a20.md](docs/io-a20.md) — I/O-карта, привилегии, A20.
- [docs/testing.md](docs/testing.md) — как устроены тесты и что они проверяют.
- [docs/windows.md](docs/windows.md) — сборка под Windows (Visual Studio 2026,
  CMake presets, статический SDL3, prebuilt fallback ROM).

## Ограничения

- CPU — функциональный интерпретатор, не cycle-accurate и без спекулятивного
  исполнения; SMM и локальный APIC реализованы упрощённо, часть редких MSR
  не реализована. Call-гейты и переходы на TSS через FAR JMP/CALL не
  моделируются; VME-перенаправление исключений (method 6) упрощено.
- SSE/FXSR не эмулируются (у выбранных Klamath/Deschutes их нет).
- AWE64 Gold ROM (4 MiB) отсутствует в `specs/`; по умолчанию генерируется
  1 MiB банк, `load_rom()` принимает внешний дамп.
- Voodoo: tDetail, часть texture-combine, NCC I/Q, gamma CLUT и triple
  buffering не реализованы; см. docs/voodoo.md.
- ATA bus-master/UDMA и CD-DA не реализованы; PIO/ATAPI достаточно для
  загрузки и чтения ISO9660.

## Лицензия и firmware

Код проекта — оригинальный. Дампы BIOS/ROM и образы из `specs/` не входят в
репозиторий; `tools/prepare_assets.py` только копирует их из локального
`specs/`. Перед распространением firmware ознакомьтесь с условиями
правообладателей.
