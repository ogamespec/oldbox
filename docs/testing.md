# Тестирование

```
ctest --test-dir build --output-on-failure
# или
./build/oldbox_tests
```

219 тестов, сгруппированных по файлам:

| Файл | Что проверяет |
|---|---|
| `test_cpu.cpp` | базовые ALU/флаги, строки, FPU, protected/paging/PAE, MSR |
| `test_cpu_instructions.cpp` | ALU всех размеров, INC/DEC, NEG/NOT, MUL/DIV, сдвиги/вращения, BT/BTS/BTR/BTC со знаковыми смещениями, BSF/BSR, SETcc/CMOVcc, MOVZX/MOVSX, стек/call/popad, строки + REPZ/REPNE, BCD, ENTER/LEAVE/XLAT, IN/OUT, 16-битные near-переходы и LOOP |
| `test_cpu_modes.cpp` | real 16/32, segment wrap, A20, protected limits, LDT, IDT error code 16/32, TSS ring3→ring0→ring3, call gates 16/32, task switching/IRET NT/TSS T-bit, 4K/PSE/PAE, WP, V86-сегментация/прерывания/IRET/VME, SGDT/LGDT layout |
| `test_cpu_system.cpp` | CPUID 233/333/VME/кэш-дескрипторы, MSR PAT/MTRR/MCA/perf, unknown MSR #GP, SYSENTER/SYSEXIT, RDTSC/RDPMC, CMPXCHG/CMPXCHG8B, BOUND, LDS/LSS/LFS/LGS, far call, #NM, CR/DR/CLTS, debug-регистры и breakpoints, CLI privilege, ARPL/VERR/VERW/LAR/LSL, PUSHF/POPF/LAHF/SAHF/CMC, INTO, SMSW/LMSW/SIDT, XCHG/XADD/BSWAP, SMM |
| `test_cpu_fpu.cpp` | x87 load/store/arith/compare/control/env, MMX mov/padd/pand/compare/shift/unpack/movd/emms |
| `test_io.cpp` | Bus I/O dispatch 8/16/32, boundaries, port 0x80, A20, CPU in/out, IOPL/CPL, TSS bitmap, 0x92/8042 A20 |
| `test_voodoo.cpp` | 28 тестов Voodoo: PCI/BAR, регистры, треугольники, клиппинг, Z/alpha/chroma/fog/stipple, текстуры/mip/bilinear, LFB, swap |
| `test_s3.cpp` | PCI/ROM/BAR, VGA text/mode13/12, raster ops, S3 engine, linear modes, timing |
| `test_audio.cpp` | DSP reset/version, mixer, EMU8000 note/ROM/register, OPL3, MPU-401, DAC/DMA IRQ |
| `test_storage.cpp` | ATA IDENTIFY/read-write, LBA48, missing drive, FDC read/write, ATAPI ISO, native BAR |
| `test_devices.cpp` | PIC/PIT/RTC/SMBus |
| `test_machine.cpp` | сквозная загрузка FreeDOS с настоящим `EP-ZXA.bin` до промпта `A:\>` |

## Принципы

- Все тесты используют настоящий `Bus`, `Cpu`, `PciBus` и устройства, а не
  заглушки; тестовый harness только задаёт начальное состояние регистров.
- Тесты инструкций сверяют не только результат, но и CF/PF/AF/ZF/SF/OF,
  причём для ALU есть независимая эталонная модель (`ref_add`/`ref_sub`/
  `ref_logic`), а не повторение формулы из эмулятора.
- Граничные случаи перебираются явно: полный перебор 8-битных DAA/DAS
  (256 значений × AF × CF), ADC/SBB с `src=0xFF/0xFFFF/0xFFFFFFFF` и CF=1,
  сдвиги с count=1 и CF=0/1, `INT_MIN / -1` для IDIV.
- Тесты режимов проверяют наблюдаемые архитектурные эффекты: CR2,
  error code, селекторы, TSS-стек, переходы колец.
- Любая найденная ошибка сначала воспроизводится тестом, затем правится
  ядро. Так были найдены и исправлены: порядок limit/base в LGDT/SGDT,
  селектор/offset в IDT-гейте, DPL в битах 6:5, D-бит сегмента, REPZ/REPNE
  termination, 16-битные умолчания real mode, LDS/LSS far pointer, LEAVE,
  I/O 16/32 на границе 64K и A20. Итерация V86 добавила тесты на IDT/TSS-стек,
  IOPL-проверки, VME redirection/VIF, task gate, 16-битный error code,
  знаковые смещения BT, BSWAP и запрет эскалации привилегий.
- Аудит 2024/2025: ADC/SBB с переполнением `src+carry` (из-за этого не
  загружался FreeDOS), флаги IMUL/MUL, AAM/AAA/AAS, ENTER, OF у вращений,
  `MOV CS`/BOUND (#UD), wrap IP в real mode, XLAT, SHLD/SHRD, IDIV overflow,
  адрес возобновления после исключения, V86/VME, BSWAP, группа 0F 01,
  VERR/VERW, task gate, FAR-переходы. Подробности — `docs/cpu-audit.md`.

Сквозной тест FreeDOS пропускается, если в `assets/` нет `EP-ZXA.bin` или
`freedos1440.img` (они не входят в репозиторий). Он запускает машину на
несколько эмуляционных секунд и ищет `A:\>` в VGA text buffer.
