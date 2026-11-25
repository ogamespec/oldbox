# S3 Trio64: спецификация против реализации

Документ сопоставляет Data Book S3 Trio32/Trio64 (март 1995,
[`s3-trio.md`](s3-trio.md)) с эмулятором в `src/s3_trio64.cpp` / `src/s3_trio64.hpp`
и фиксирует, что реализовано полностью, что реализовано с приближениями и что
принципиально не наблюдаемо в функциональном эмуляторе.

Обозначения:

- **Полностью** — регистр/режим/команда имеют документированную семантику и
  проверены тестом.
- **Частично** — регистр хранится и влияет на то, что можно наблюдать в
  эмуляторе, но аналоговая/внешняя часть не моделируется либо поведение
  упрощено (причина указана).
- **Только хранение** — значение читается/пишется, но не влияет на вывод:
  относится к физическому уровню (тайминги DRAM, вывод пинов, внешние
  устройства), которого у эмулятора нет.

## 1. Сводная таблица по разделам Data Book

| Раздел Data Book | Состояние | Комментарий |
|---|---|---|
| 1. Introduction, 2. Mechanical, 3. Pins, 4. Electrical | ➖ | Пины, корпус, аналоговые параметры и AC-тайминги не моделируются; PCI-идентификация и интерфейсы — см. ниже. |
| 5. Reset and Initialization | **Полностью** (функционально) | Сброс регистров, страппинг-значения PCI/FPM/размер памяти (CR36/37/68), `reset()`, `set_text_mode()`, `set_mode13h()`. |
| 6. System Bus Interfaces (PCI) | **Полностью** | PCI config space, BAR0/BAR1, expansion ROM BAR, DEVSEL/класс, Interrupt Line/Pin. VL-Bus не моделируется (в эмуляторе только PCI-конфигурация). |
| 7. Display Memory | **Частично** | Размеры 1/2/4 МБ, FPM/EDO-выбор (CR36), refresh count (CR11.6/CR3A), M/N (CR54/CR60) хранятся. Тайминги RAS/CAS/OE/WE (CR68), shared frame buffer BREQ/BGNT и внешний мастер — только регистры/состояние. |
| 8. RAMDAC | **Полностью** | Режимы 0, 8, 9, 10, 12, 13; LUT 6 бит; DAC mask; read/write index; DAC status; snoop-бит PCI; SENSE. |
| 9. Clock Synthesis | **Полностью** (расчёт), **частично** (загрузка) | Формула PLL `((M+2)·fREF)/((N+2)·2^R)`, SR10–SR15, выбор DCLK по MISC[3:2], DCLK/2, MCLK. Стробы загрузки SR15 обрабатываются как регистры, значение PLL применяется сразу. |
| 10. Miscellaneous Functions | **Частично** | BIOS ROM интерфейс, DPMS (SRD), GIP/GOP (CR55/CR5C), прерывания — полностью. Feature connector, genlock и shared frame buffer — регистровая семантика без внешних устройств. |
| 11. Software Setup | **Полностью** | Все ключи разблокировки CR38/CR39/SR8/CR40, тест присутствия (CR30/2D/2E/2F), режимы VGA/Enhanced. |
| 12. VGA Compatibility | **Полностью** (кроме CGA/HGC-режимов) | Planar 4bpp, chain-4, Mode X, odd/even, raster ops, read mode 1, split screen, panning, текстовый курсор, 9-точечный шрифт, blink. |
| 13. Enhanced Mode Programming | **Полностью** | Линейная адресация, packed MMIO, все примеры Section 13.3.3.1–13.3.3.18. |
| 14. VGA Standard Registers | **Полностью** | MISC/FCR/STATUS0/STATUS1, SR0–SR18, CR0–CR18/CR22/CR24, GR0–GR8, AR00–AR14, setup 46E8, RAMDAC 3C6–3C9. |
| 15. S3 VGA Registers | **Полностью** | CR2D–CR3C, включая ID/rev, MEM_CNFG, BKWD_1/2/3, lock-регистры, MISC_1, CR3B/CR3C. |
| 16. System Control Registers | **Полностью** | CR40–CR4F, включая режим курсора, позицию, цвета, адрес и смещения. |
| 17. System Extension Registers | **Полностью** (регистры) | CR50–CR6C; LAW size/page, CR59/5A ↔ BAR0, GOUT, overflow-биты, M/N, sync, misc. |
| 18. Enhanced Commands | **Полностью** | Все типы команд: line, polyline/2-point, rectangle, image transfer (through/across), BitBLT (through/across), PatBLT, polygon solid/pattern, 4-point trapezoid solid/pattern, Bresenham trapezoid solid/pattern, short stroke; все регистры команд и статуса. |
| 19. PCI Registers | **Полностью** | Vendor/Device ID `5333:8811`, Command (включая DAC SNP), Status, Class Code, BAR0, ROM BAR, Interrupt Line/Pin. |

## 2. Что было добавлено/исправлено в этой итерации

До этой итерации эмулятор покрывал VGA-ядро, линейный буфер и базовые
команды движка (line, rectangle, BitBLT, PatBLT, image transfer, short
stroke). Ниже — закрытые пробелы.

### 2.1 Регистры и доступ

- **CR2D = 88h** (старший байт Device ID) вместо ошибочного 53h; CR2E = 11h,
  CR2F = 00h, CR30 = E1h.
- **CR22** (CPU latch), **CR24/CR26** (attribute index + ENB PLT + инверсный
  AFF) читаются по документу.
- **CR33.4** блокирует запись RAMDAC (3C6–3C9), **CR33.6** — палитру/overscan
  (AR00–AR0F/AR11).
- **CR35.4/CR35.5** блокируют вертикальные/горизонтальные timing-регистры с
  сохранением документированных исключений (CR7.4 и т. д.).
- **CR11.4/CR11.5** — arm/disable вертикального прерывания; запись
  пересчитывает линию INTA.
- **CR40.0 (EN ENH)** действительно включает enhanced-регистры: при 0 порты
  `x2E8`/packed MMIO читаются как `FFh`, записи игнорируются.
- **SR9.7** (`MMIO only`) запрещает PIO-доступ к enhanced-регистрам при
  включённом MMIO.
- **CR53.6** (nibble swap) применяется к линейному окну.
- **CR58[1:0]** задаёт размер линейного окна (64 КиБ/1/2/4 МБ);
  **CR31.0 + CR6A[5:0]** — 64-КиБ страница внутри окна.
- **CR59/CR5A и PCI BAR0** отражают одни и те же биты 31–23 в обе стороны.
- **CR55.2** перенаправляет чтение 3C8h на General Input Port.
- **CR65.2** разрешает порт 3C3h.
- **STATUS0** (3C2h): бит 4 — SENSE, бит 7 — pending CRT interrupt.
- **STATUS1** (3?Ah): биты 0/3/2 считаются из реального положения луча
  (CRTC H/V счётчики), а не из грубого таймера; биты 5–4 — видеотест
  (нули без активного видеоконвейера).
- **DAC status** (3C7h) различает чтение и запись.
- **AR14** (pixel padding) формирует V7–V6 и, при AR10.7, V5–V4 индекса DAC;
  **AR12[3:0]** маскирует плоскости в 4bpp.
- **AR10.5** (TOP PAN) сбрасывает pixel pan ниже line compare.

### 2.2 Прерывания и DPMS

- Полная модель **VGA vertical retrace interrupt**: CR32.4, CR11.4/CR11.5,
  STATUS0.7, защёлка в конце кадра.
- **Enhanced interrupt subsystem 42E8**: статусы VSY/GE BSY/FIFO OVF/FIFO EMP,
  разрешения (биты 8–11), очистка (биты 0–3), soft reset GE (bits 15–14),
  объединение с INTA через PCI Interrupt Line.
- **INTA#** маршрутизируется в PIC через callback; `Machine` передаёт его в
  `piix4_.pic()`.
- **DPMS** (SRD[7:4]): при HSYNC=0 и VSYNC=0 монитор считается выключенным и
  кадр отдаётся чёрным. Флаги `dpms_hsync_off()/dpms_vsync_off()/dpms_off()`
  доступны для фронтенда.

### 2.3 Тактирование

- Формула PLL из Section 9, регистры SR10–SR13 (N/R/M), SR15 (DCLK/2, инверсия),
  MISC[3:2] (25.175/28.322/внешний/PLL).
- `dclk_hz()`, `mclk_hz()`, `refresh_hz()` — наблюдаемые значения.
- Период кадра и положение луча для STATUS1 считаются из CRTC (H/V total,
  interlace, VT X2, horizontal counter double).
- По умолчанию частота обновления поверхности — 60 Гц (как у остальных
  видеоустройств эмулятора); переменная окружения
  `OLDBOX_S3_CRTC_PACING=1` включает пейсинг на реальной частоте CRTC.

### 2.4 RAMDAC/вывод

- **Mode 8** (clock doubled): удвоение ширины (2 пикселя/VCLK) при выполнении
  условий SR18.7 и SR15.4/6.
- **Mode 12** (Trio32, packed 24 bpp): 3 байта на пиксель, DCLK/3, ширина
  `HDE·8/3`.
- **Mode 9/10/13**: 15/16/24 bpp, 32bpp с неиспользуемым старшим байтом.
- **Hardware cursor**: 64×64 AND/XOR, стеки цветов CR4A/CR4B, offset CR4E/4F,
  режимы MS-Windows и X11 (CR55.4), сброс стека чтением CR45.
- **Graphics blink** (AR10.3) инвертирует V3; **текстовый blink** — как раньше.
- **CR8** (preset row scan) сдвигает текстовый кадр по вертикали.
- **Interlace** (CR42.5) удваивает вертикальный период (два поля на кадр);
  построение кадра остаётся прогрессивным.

### 2.5 Графический движок

- Команды **3 / 0Bh** (polygon fill solid/pattern), **4 / 0Ch**
  (4-point trapezoid solid/pattern), **5 / 0Dh** (Bresenham trapezoid
  solid/pattern) с заливкой span'ов и обновлением обеих кромок.
- **Pattern-заливки** используют CR PAT_X/PAT_Y (EAE8/EAEA), PatBLT — CUR_X/CUR_Y.
- **Image transfer**: передача по строкам с выравниванием на ширину шины
  (8/16/32 бит), отбрасыванием padding-битов, byte swap (CMD.12) и особым
  режимом CMD[10:9]=11 (выравнивание только по байту); позиция курсора после
  команды — по документу (под нижним левым / справа от верхнего правого угла).
- **Textured line**: биты маски из CPU, ожидание данных, padding по строке,
  byte swap.
- **RD_MASK** применяется к источнику цвета (bitplane read mask), а не только
  к выбору mix-регистра.
- **Byte swap для short stroke**: порядок байтов 9EE8h по CMD.12.
- **RSF** (MULT_MISC.4) переключает старшее/младшее слово 32-битных цветовых
  регистров при 16-битном PIO-доступе в 32bpp.
- **Read Register Select** (BEE8h, index F) — селектор 0–Ah с автоинкрементом.
- **GP_STAT** (9AE8h): корректные биты FIFO empty/GE busy.
- Проверка границ VRAM и клиппинга для всех команд.

### 2.6 Отображение

- **packed 4bpp** enhanced-режим (два пикселя в байте) отделён от planar 4bpp;
- linear 4bpp (редкий случай) читается как nibble-поток;
- **plane mask AR12** для planar 4bpp;
- **AR14** в индексе DAC;
- **CR8** в тексте;
- **mode12**;
- **clock doubled mode 8**.

## 3. Ограничения и приближения

Ниже перечислено то, что сознательно не моделируется или моделируется
упрощённо. Это не «забытые» функции, а части, для которых в функциональном
эмуляторе нет наблюдаемого результата либо нет внешнего устройства.

1. **Пины, корпус, аналоговые параметры, AC-тайминги** (разделы 2–4):
   эмулятор не выдаёт AR/AG/AB, HSYNC/VSYNC, VCLK, PD/MA/RAS/CAS/WE/OE.
2. **Тайминги DRAM** (CR68, refresh, RAS/CAS stretch, EDO/FPM): регистры
   хранятся, но доступ к VRAM мгновенный. M/N-параметры FIFO (CR54/CR60)
   также только хранятся.
3. **Shared Frame Buffer** (CR50.2, SRA P-value, BREQ/BGNT): регистры и
   аксессоры есть, второго мастера памяти нет.
4. **Feature connector** (SRD.0, SRB, ESYNC/EVCLK/EVIDEO, PA[15:0], BLANK):
   регистры и `feature_connector_enabled()` есть; внешнего источника пикселей
   нет, поэтому данные PA в RAMDAC не подмешиваются.
5. **Genlocking** (CR56/CR57/CR63): регистры хранятся, внешний VSYNC-вход не
   подключён; `genlock_enabled()` отражает режим.
6. **Interlace**: вертикальный период удваивается, но кадр строится
   прогрессивно (эмулятор отдаёт полный кадр, а не два поля).
7. **Загрузка PLL**: значения SR10–SR13 применяются сразу; биты загрузки
   SR15.0/1/5 принимаются, но не стробируют переключение частоты.
8. **Частота обновления**: по умолчанию 60 Гц; `refresh_hz()` возвращает
   реальную частоту CRTC, а `OLDBOX_S3_CRTC_PACING=1` включает её для
   пейсинга кадров.
9. **CGA/Hercules-режимы** (CR17.0/1, address wrap, count-by-2/4,
   doubleword/quadword-адресация как отдельные режимы): эмулятор выводит
   стандартные VGA/Enhanced/VESA-режимы; экзотические CGA/HGC-карты не
   поддерживаются. Кромки polygon fill и 4-point trapezoid интерполируются
   линейно (DDA), а не Bresenham-движком кромок, поэтому возможны отличия в
   1 пиксель на границе заливки; Bresenham-трапеция использует честный
   Bresenham-обход.
10. **CR31.1** (two-page screen), **CR31.2** (VGA 16-bit bus),
    **CR3A.3** (top-of-memory), **CR3A.5/CR31.6** (high-speed text) —
    регистры хранятся, но не меняют организацию памяти/вывод.
11. **CR45.4** (cursor right-side storage), **CR4C.11-8** — обычный курсор
    реализован; режим «справа от битмапа» не моделируется.
12. **AR10.1** (monochrome attributes), **AR11/CR33.5** (border/overscan):
    кадр содержит только активную область, рамка не рисуется.
13. **CR65[4:3]** (задержка BLANK для Mode 12), **CR67.0** (фаза VCLK),
    **SR14/SR16/SR17** (тестовые биты) — только хранение.
14. **VL-Bus** (раздел 6.2): эмулятор реализует PCI-конфигурацию; VL-Bus
    регистры-страппы читаются, но VL-циклы не эмулируются.
15. **RAMDAC snooping** (PCI Command.5, CR34[2:0]): бит хранится, второго
    RAMDAC на ISA нет.
16. **BIOS ROM интерфейс**: PCI expansion ROM BAR и чтение ROM реализованы;
    VL-Bus ROMCS и 16/32-битные ROM-циклы — нет.

## 4. Тесты

Новые/расширенные тесты в `tests/test_s3.cpp`:

- `s3_device_id_and_readonly_registers` — CR2D/2E/30, read-only.
- `s3_cr22_latch_and_cr24_attribute_status` — CPU latch, attribute status.
- `s3_clock_synthesis_pll_and_refresh` — PLL M/N/R, MCLK, refresh 70 Гц.
- `s3_input_status_registers` — SENSE, display active, VSYNC, AFF.
- `s3_general_input_port_and_general_output_port` — CR55.2/CR5C.
- `s3_ramdac_and_palette_write_locks` — CR33.4/CR33.6.
- `s3_crt35_timing_register_locks` — CR35.4/5.
- `s3_ar14_pixel_padding_and_ar12_plane_mask` — AR14/AR12.
- `s3_linear_window_law_size_and_64k_page` — CR58/CR6A/CR59-5A/BAR0.
- `s3_pci_interrupt_generation_vga_and_enhanced` — VGA и enhanced IRQ.
- `s3_dpms_off_blanks_output` — DPMS.
- `s3_engine_register_access_requires_cr40` — CR40.0.
- `s3_engine_polygon_fill_solid` — тип 3.
- `s3_engine_trapezoid_4point_solid` — тип 4.
- `s3_engine_brezenham_trapezoid_solid` — тип 5.
- `s3_engine_polygon_fill_pattern` — тип 0Bh + PAT_X/PAT_Y.
- `s3_engine_image_transfer_bus_padding` — padding строк, BUS SIZE.
- `s3_engine_rsf_word_access_to_32bit_colors` — RSF.
- `s3_hardware_cursor_windows_and_x11_modes` — MS/X11 decode.
- `s3_graphics_blink_inverts_v3` — AR10.3 в графике.
- `s3_text_preset_row_scan_scrolls_vertically` — CR8.
- `s3_ramdac_mode8_clock_doubling` — CR67 mode 8.
- `s3_ramdac_mode12_packed_24bpp` — Mode 12.
- `s3_engine_read_register_select_and_gp_stat` — BEE8h selector, GP_STAT.
- `s3_cr53_nibble_swap_on_linear_access` — CR53.6.
- `s3_display_start_address_up_to_4mb` — 20-битный счётчик display start.

Полный прогон: `build/vs2026/Release/oldbox_tests.exe` — 523 теста, включая
сквозную загрузку FreeDOS и Dangerous Dave 2.

## 5. Как запускать

```powershell
# сборка и все тесты
.\build-windows.bat

# только S3
.\build\vs2026\Release\oldbox_tests.exe s3_

# реальная частота CRTC вместо 60 Гц
$env:OLDBOX_S3_CRTC_PACING = "1"
.\build\vs2026\Release\oldbox.exe --bios assets\bios\EP-ZXA.bin --vga-bios assets\vga\S3Trio64_1_14-04.bin
```
