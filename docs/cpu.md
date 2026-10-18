# CPU: Pentium II

`oldbox` моделирует архитектурно видимое поведение Pentium II, а не
микроархитектуру (суперскалярность, предсказание переходов и т.п. не
воспроизводятся). Модель — интерпретатор с точным набором регистров,
флагов, сегментов, таблиц страниц и MSR.

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
`FF /7` даёт #UD. VME method-6 для исключений и порядок редких ошибок
повреждённого TSS упрощены. Эти пункты отмечены как TODO, а не выданы за
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
APIC priority/timer, SMM LDT/auto-HALT/NMI и исправления x87. Полная
матрица — [docs/cpu-features.md](cpu-features.md), детали — [docs/cpu-audit.md](cpu-audit.md).

## MSR

Реализованы: TSC (0x10), APIC_BASE (0x1B, только хранение), EBL_CR_POWERON
(0x2A), MTRRCAP (0xFE), SYSENTER_CS/ESP/EIP (0x174–0x176), MCG_CAP/STATUS
(0x179/0x17A), EVNTSEL/PERFCTR (0x186–0x18A), DEBUGCTLMSR (0x1D9),
LASTBRANCH (0x1DB), PAT (0x277), MTRRdef (0x2FF), MTRRphysBase/Mask
(0x200–0x20F). Неизвестный MSR даёт #GP(0).

## FPU/MMX

x87-стек хранится как `long double` + теги; 80-битные форматы
преобразуются в/из x87 extended. MMX-регистры — отдельный массив `u64`.
MMX не разделяет физические регистры с x87 (упрощение); EMMS — no-op.

## Тесты

219 тестов: `tests/test_cpu*.cpp` (CPU-инструкции, режимы, система, FPU/MMX),
`tests/test_io.cpp` (I/O-привилегии и A20) и `tests/test_machine.cpp`
(сквозная загрузка FreeDOS до `A:\>`). Полный список — `./build/oldbox_tests`.

Аудит инструкций, найденные ошибки и остаточные пробелы описаны в
[docs/cpu-audit.md](cpu-audit.md).
