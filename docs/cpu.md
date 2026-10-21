# CPU: Pentium II

`oldbox` моделирует архитектурно видимое поведение Pentium II, а не
микроархитектуру (суперскалярность, предсказание переходов и т.п. не
воспроизводятся). Основная модель — интерпретатор с точным набором регистров,
флагов, сегментов, таблиц страниц и MSR. Дополнительно есть опциональный
JIT-компилятор `IA-32 -> x86-64` (`--jit`), который ускоряет горячий
целочисленный код, сохраняя интерпретатор как эталон поведения.

## Модели

| Модель | CPUID family/model | Особенности |
|---|---|---|
| Pentium II 233 (Klamath) | 6/3 | MMX, 512 KiB L2, без SSE |
| Pentium II 333 (Deschutes) | 6/5 | MMX, 512 KiB L2, без SSE |

CPUID leaf 1 выставляет FPU, VME, DE, PSE, TSC, MSR, PAE, MCE, CX8, SEP,
CMOV, PAT, PSE36, MMX; SSE/SSE2/FXSR не выставляются.

## Режимы

- **Real mode**: 16-битные умолчания, 0x66/0x67 префиксы, 20-битная
  адресация с A20, IVT в младшей памяти, 16-битный стек.
- **Protected mode**: GDT/LDT, проверки DPL/RPL/CPL, expand-down сегменты,
  conforming code, call gates 16/32-бит с копированием параметров и
  переключением стека, task gates, переключение задач через FAR JMP/CALL
  и IRET с NT, busy/link-поля TSS, 16/32-битные TSS, IDT interrupt/trap/task
  gates, переключение стека при смене кольца, IRET с возвратом в ring 3.
- **Virtual-8086**: 8086-сегментация (base=selector<<4, лимит 64 KiB),
  16-битные умолчания, CPL=3, user-доступ к страницам. Прерывания и
  исключения идут через protected-mode IDT с переключением на ring-0 стек
  из TSS и сохранением GS/FS/DS/ES/SS/ESP/EFLAGS/CS/EIP; IRET возвращает в
  V86 и восстанавливает сегменты. Реализованы IOPL-проверки INTn/PUSHF/
  POPF/IRET/CLI/STI, I/O permission bitmap и VME: CR4.VME, interrupt
  redirection bitmap, VIF/VIP, перенаправление software INT в IVT (method 5)
  и method-4 кадр для аппаратных прерываний. V86-задача может быть запущена
  через IRET или task gate.
- **Paging**: 4 KiB, PSE 4 MiB, PAE (3 уровня, 2 MiB/4 KiB), CR0.WP, U/S,
  page-fault error code (P/W/U), CR2, TLB с учётом CR3/CR4.
- **SMM**: вход по SMI, state-save в SMRAM (`SMBASE`, по умолчанию
  `0x30000`), handler на `SMBASE+8000`, `RSM` с восстановлением контекста,
  LDT base, auto-HALT restart, отложенным NMI и возможностью изменить
  SMBASE. Это отдельный режим CPU.
- **Debug/performance**: DR0–DR7, DR4/DR5 alias через CR4.DE, GD, data- и
  instruction-breakpoints, single-step TF, RF, #DB/DR6, task-switch BT;
  EVNTSEL0/1 и PerfCtr0/1 (INST_RETIRED/INST_DECODED/HW_INT_RX).
- **Machine Check**: MCG_CAP/STATUS, пять банков MCi_CTL/STATUS/ADDR/MISC,
  #MC (vector 18), MCIP/shutdown при повторном событии.
- **Local APIC**: MMIO `FEE00000`, ID/version/TPR/APR/PPR/EOI/LDR/DFR/SVR/
  ESR, ISR/TMR/IRR, ICR (fixed/lowest/NMI), LVT, timer one-shot/periodic,
  divide 2…128/1, приоритетная доставка и EOI.
  Модуль лежит в `src/cpu/apic.cpp`, а не в чипсете.

## Инструкции

Покрыты:

- вся базовая целочисленная группа 1-байтовых опкодов: ALU, INC/DEC, PUSH/POP,
  PUSHA/POPA, BOUND, ARPL, IMUL, INS/OUTS, Jcc, группы 1–5, MOV/MOVSX/MOVZX,
  LEA, LDS/LES/LSS/LFS/LGS, строковые MOVS/CMPS/STOS/LODS/SCAS с REP/REPZ/REPNE,
  CALL/JMP/RET/RETF/INT/IRET, ENTER/LEAVE, XLAT, BCD (DAA/DAS/AAA/AAS),
  сдвиги/вращения, MUL/IMUL/DIV/IDIV, BT/BTS/BTR/BTC, BSF/BSR, SHLD/SHRD;
- двухбайтовые (0F): системные группы 6/7, LAR/LSL, MOV CR/DR, CLTS, INVD,
  WBINVD, INVLPG, WRMSR/RDMSR/RDPMC, RDTSC, SYSENTER/SYSEXIT, CMOVcc, SETcc,
  Jcc rel32, CMPXCHG/CMPXCHG8B, XADD, BSWAP, MOVZX/MOVSX, MMX (частично,
  основные packed-операции);
- x87: загрузка/сохранение 32/64/80-bit и целых форматов, арифметика,
  сравнения, FCOMI/FCOMIP/FUCOMI/FUCOMIP, FCMOVcc (по x87 C0/C2/C3, а не
  по EFLAGS), константы, FABS/FCHS/FSQRT/FSIN/FCOS/FSCALE,
  FNSTSW/FNSTCW/FLDCW, FSTENV/FLDENV/FNSAVE/FRSTOR, FBLD/FBSTP;
- MMX: PADDB/W/D/Q, PSUBB/W/D/Q, PADDSB/W, PSUBSB/W, PAVGB/W, PMINUB,
  PMAXUB, PMINSW, PMAXSW, PMULHUW, PMULHW, PMULLW, PMULUDQ, PMADDWD,
  PSADBW, PAND/POR/PXOR/PANDN, PUNPCK*, PACKSS/PACKUS, PCMPEQ/GT,
  PSLL/PSRL/PSRA, PSLLDQ/PSRLDQ, PSHUFW, PMOVMSKB, MOVD/MOVQ, MOVNTQ,
  MASKMOVQ, EMMS.

Известные пробелы CPU (не маскируются): FXSAVE/FXRSTOR (у PII нет SSE,
но FXSR-check отдельный), часть редких MMX-форм, а также трансцендентные x87
считаются через host long double, а не по таблицам Intel; FPREM/FPREM1
выполняют полное уменьшение за один шаг, а не пошагово; #MF не доставляется,
флаги OE/UE/PE и округление арифметики по CW моделируются частично.
`FISTTP` (`DB/DD/DF /1`) реализован как удобное расширение, хотя на
Pentium II эта форма не определена. MSR-пространство ниже `0x200` (включая
PII BBL/L2 MSR) хранится как обычные регистры, поскольку у модели нет
кэша; `#GP` для неизвестных MSR выдаётся выше этого диапазона.
`FF /7` даёт #UD. VME method-6 для исключений реализован; порядок редких
ошибок повреждённого TSS упрощён. Эти пункты отмечены как TODO, а не выданы за
готовые.

Исправлено при аудите: ADC/SBB (перенос при переполнении src+carry — именно
это блокировало загрузку FreeDOS), флаги IMUL/MUL, AAM/AAA/AAS, ENTER,
OF у ROL/ROR/RCL/RCR, `MOV CS` и BOUND (#UD), wrap IP в real mode, XLAT,
SHLD/SHRD, IDIV overflow, адрес возобновления после исключения
(`opcode_ip_`). В очередной итерации исправлены V86-сегментация/CPL/стек
прерываний, VME (redirection bitmap, VIF/VIP), BSWAP (лишний ModRM),
группа 0F 01 (/6 LMSW, /7 INVLPG), VERR/VERW и LAR/LSL в real/V86,
знаковые смещения BT, 16-битные near-переходы/LOOP, межкольцевой RETF,
task gate и запрет эскалации привилегий через FAR JMP/CALL. Далее добавлены
call gates, task switching через TSS/IRET NT, 16-битные TSS/gates,
debug-регистры и breakpoints, PGE/PVI/AC, machine check, MTRR/perf MSR,
APIC priority/timer, SMM LDT/auto-HALT/NMI и исправления x87. Аудит 2026
добавил zero-extension `MOV r32,Sreg`, #NP/#SS для отсутствующих сегментов,
STI shadow, маску EIP LOOP/JCXZ по operand-size, проверку границы 64K в V86,
#GP для IRET в V86 с CPL>0, #UD для невалидных форм групп 6/7/8, C6/C7,
0F C7 и LEA, исправление PACKSSDW и насыщение MMX-сдвигов, wrap PMADDWD,
строгий #GP для незадействованных MSR (с хранением PII BBL/fixed-MTRR),
PE/PG в SMM, 14/28-байтовые x87 env-форматы, сброс FNSAVE/FINIT и
физическую индексацию FPU tag word. Последующая проверка 66/67-префиксов
и дескрипторов добавила идемпотентность повторных префиксов, 24-битный
base у 16-битных SGDT/SIDT, запрет code-сегмента в SS, #NP для
отсутствующих LDT/TSS/CS/IDT-target, поддержку expand-down SS в
call gate/task switch и выставление accessed-bit. Аудит JIT 2026 исправил
инвертированный OF у INC/DEC (эталонная формула и тест), знаковое расширение
dword в PACKSSDW и UB сдвига в PSRAQ при count >= 64. Полная матрица —
[docs/cpu-features.md](cpu-features.md), детали — [docs/cpu-audit.md](cpu-audit.md).

## MSR

Реализованы: TSC (0x10), APIC_BASE (0x1B, только хранение), EBL_CR_POWERON
(0x2A), BIOS_UPDT_TRIG/BIOS_SIGN (0x79/0x8B), MTRRCAP (0xFE),
SYSENTER_CS/ESP/EIP (0x174–0x176), MCG_CAP/STATUS (0x179/0x17A),
EVNTSEL/PERFCTR (0x186–0x18A), DEBUGCTLMSR (0x1D9), LASTBRANCH (0x1DB),
PAT (0x277), MTRRdef (0x2FF), MTRRphysBase/Mask (0x200–0x20F),
fixed-MTRR (0x250–0x26F). Пространство ниже 0x200 (включая PII BBL/L2 MSR)
хранится как регистры; неизвестный MSR вне его даёт #GP(0).

## FPU/MMX

x87-стек хранится как `long double` + теги; 80-битные форматы
преобразуются в/из x87 extended. MMX-регистры — отдельный массив `u64`.
MMX не разделяет физические регистры с x87 (упрощение); EMMS — no-op.

## JIT

`--jit` (алиас `--jitc`) включает baseline-JIT, транслирующий базовые блоки
гостевого IA-32 в машинный код x86-64. Интерпретатор остаётся эталоном: JIT
не является отдельной моделью CPU и для любой неподдержанной инструкции
возвращается в интерпретатор на один шаг.

Реализация (`src/cpu/jit.cpp`):

- кэш блоков по ключу `CS.base:EIP`, режима, CPL, `CS`-дескриптора,
  `CR0/CR3/CR4` и A20; блок заканчивается на переходе, вызове/возврате,
  инструкции с видимым изменением `CS/SS`, I/O, прерывании или после
  32 инструкций;
- трансляция целочисленного ядра: MOV/LEA, ALU, INC/DEC, PUSH/POP, TEST,
  XCHG, MUL/IMUL, сдвиги/вращения с фиксированным количеством, Jcc, LOOP,
  CALL/RET, CBW/CWDE/CWD/CDQ, MOVZX/MOVSX, CMOVcc, SETcc, BSWAP,
  BT/BTS/BTR/BTC (регистровые формы), а также `IN/OUT` и загрузки сегментов
  (`MOV Sreg`, PUSH/POP Sreg, LDS/LES/LSS/LFS/LGS — экспериментально);
- регистры гостя хранятся в `CpuState`, арифметические флаги — в `RFLAGS`
  хоста; перед выходом из блока/helper-вызовом они сливаются в
  `CpuState.eflags` с учётом особенностей эталонного интерпретатора
  (в частности, инвертированного PF и исправленного OF у INC/DEC);
- память/стек/сегменты/I/O идут через helper-функции, которые вызывают те же
  `Cpu::read_mem/write_mem/push_value/pop_value/...`, поэтому сегментация,
  paging, page fault, I/O-привилегии, счётчики и побочные эффекты шины
  совпадают с интерпретатором;
- исключение helper-а не разворачивает стек через сгенерированный код:
  оно записывается в `JitContext`, блок возвращается в C++, и #GP/#PF/#UD
  доставляется обычным путём;
- перед повторным использованием блока проверяется актуальность физических
  страниц кода (на случай изменения page tables), а запись в гостевую память
  инвалидирует только блоки, чьи **точные байтовые диапазоны** пересекаются
  с записью; это важно для real-mode, где код и данные делят страницу;
- `Machine` ограничивает блок ближайшей границей тика устройств (512 циклов),
  чтобы PIT/RTC/клавиатура наблюдали время на тех же инструкциях, что и в
  интерпретаторе. Это уменьшает «дрожание» таймеров, но не делает JIT
  cycle-accurate.

Консервативное подмножество выбрано не случайно: расширение набора
инструкций меняет границы блоков и может увести таймерозависимую инициализацию
Award BIOS/S3 на другой путь. Поэтому дополнительные формы (переменные сдвиги,
INT/IRET, RETF, дальние указатели, CLI/STI, CLD/STD) реализованы, но по
умолчанию выключены; разработчик может включить их через
`OLDBOX_JIT_DISABLE` (битовая маска, 0 — включить всё). Конфигурация по
умолчанию проверена загрузкой FreeDOS до `A:\>`.

Производительность измеряется не временем загрузки (её задают I/O и
устройства), а числом гостевых инструкций в секунду на синтетических циклах
без I/O. На эталонном хосте (Release, x86-64, 2 млн итераций):

| Цикл | Интерпретатор | JIT |
|---|---|---|
| ALU/регистры (6.0 млн инструкций) | 36.5 MIPS | 91.9 MIPS |
| CALL/RET (12.0 млн инструкций) | 42.3 MIPS | 50.6 MIPS |
| LOAD/STORE (4 тыс. инструкций) | 30.0 MIPS | 35.5 MIPS |

Это baseline-JIT, а не оптимизирующий компилятор: он не делает регистрового
распределения между инструкциями, не инлайнит память и не строит цепочки
блоков. Основной выигрыш — устранение декодера/диспетчеризации на горячих
блоках; на I/O- и FPU-тяжёлом коде выигрыша нет, а на коротких блоках с
частым fallback-ом возможен проигрыш интерпретатору.

## Тесты

374 теста: `tests/test_cpu*.cpp` (CPU-инструкции, режимы, система, FPU/MMX),
`tests/test_cpu_audit.cpp` (регрессии аудита 2026), `tests/test_io.cpp`
(I/O-привилегии и A20), `tests/test_jit.cpp` (дифференциальные тесты JIT и
сквозная загрузка FreeDOS с `--jit`) и `tests/test_machine.cpp` (сквозная
загрузка FreeDOS интерпретатором). Полный список — `./build/oldbox_tests`.

Аудит инструкций, найденные ошибки и остаточные пробелы описаны в
[docs/cpu-audit.md](cpu-audit.md).
