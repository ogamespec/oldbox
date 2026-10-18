# Матрица архитектурных возможностей CPU

Источники: `specs/242690-001.pdf` (Vol. 1), `specs/242691-001.pdf` (Vol. 2),
`specs/242692-001.pdf` (Vol. 3), `specs/243502-001.pdf` и
`specs/24333503.pdf` (Pentium II datasheet/electrical). Матрица фиксирует,
что именно моделируется архитектурно, что упрощено и что принципиально не
имеет наблюдаемого эффекта в эмуляторе без кэша и реального системного bus.

## Режимы и сегментация

| Возможность | Статус |
|---|---|
| Real-address mode, 16-битные умолчания, A20, wrap 64K | реализовано |
| Protected mode: GDT/LDT, DPL/RPL/CPL, expand-down, conforming | реализовано |
| Загрузка сегментов, null-селекторы, hidden-часть | реализовано |
| Virtual-8086: 8086-сегментация, CPL=3, IDT/TSS, IRET | реализовано |
| VME: redirection bitmap, VIF/VIP, method 4/5/6 | реализовано, method 6 для исключений упрощён |
| PVI: VIF/VIP для ring 3 protected mode | реализовано |
| Call gates 16/32-бит, parameter count, stack switch | реализовано |
| Task gates, FAR JMP/CALL на TSS, IRET с NT | реализовано |
| 16-битные TSS и 16-битные call gates | реализовано |
| Busy-биты TSS, link/NT, CR0.TS, очистка DR7.L при task switch | реализовано |
| IRET с NT: полная проверка busy/limit/типов | базово, без всех порядков ошибок |
| Сегментные и page-исключения, error code 16/32 | реализовано |

## Память и защита

| Возможность | Статус |
|---|---|
| 4K, PSE 4M, PAE (2M/4K), CR0.WP, U/S, PCD/PWT-биты | реализовано |
| Page-fault error code (P/W/U), CR2 | реализовано |
| PGE и global-бит TLB, INVLPG, CR3 flush | реализовано |
| Alignment check (CR0.AM + EFLAGS.AC + CPL3) | реализовано |
| PSE-36 (36-бит physical) | не наблюдаемо: Bus 32-битный, физической RAM >4G нет |
| Кэши L1/L2, write buffer, когерентность | не моделируются |
| MTRR fixed/variable, MTRRdefType, MTRRcap | регистры сохраняются; влияния на кэш нет |
| PAT | MSR сохраняется; влияния на кэш нет |

## Прерывания, задачи, системные инструкции

| Возможность | Статус |
|---|---|
| IDT interrupt/trap/task gates, DPL, #NP/#GP/#TS/#SS | реализовано |
| TSS esp0/ss0, I/O permission bitmap (32/16-бит TSS) | реализовано |
| SMM: state save map, SMBASE, RSM, auto HALT restart, NMI latch | реализовано, I/O restart field не используется |
| Machine Check: MCG_CAP/STATUS, 5 банков MCi_CTL/STATUS/ADDR/MISC, #MC | реализовано |
| Debug: DR0-7, DR4/5 alias/DE, GD, data/instruction breakpoints, TF, RF, BT | реализовано; exact/IO breakpoints не моделируются |
| Performance monitoring: EVNTSEL0/1, PerfCtr0/1, INST_RETIRED/DECODED, HW_INT_RX | базово; overflow/PMI и остальные события не считаются |
| CPUID leaf 0/1/2/0x8000000x, VME/MCE/MCA/PGE/CMOV/MMX | реализовано, cache-дескрипторы PII |
| MSR: TSC, APIC_BASE, SYSENTER, MTRR, PAT, MCA, perf | реализовано |
| CR0/CR2/CR3/CR4, CLTS, LMSW/SMSW, INVD/WBINVD/INVLPG | реализовано |
| FPU #NM (EM/TS/MP), WAIT | реализовано |

## APIC

| Возможность | Статус |
|---|---|
| ID/version/SVR/TPR/APR/PPR/LDR/DFR/ESR/EOI, ISR/TMR/IRR | реализовано |
| Timer one-shot/periodic, divide 2..128/1 | реализовано |
| ICR: fixed/lowest self-IPI, NMI, shorthand, delivery status | реализовано |
| LVT timer/perf/LINT0/LINT1/error | регистры и маски; delivery modes частично |
| APIC bus, arbitration, logical destination, MP protocol | не моделируются (один CPU) |

## x87/MMX

| Возможность | Статус |
|---|---|
| Загрузка/сохранение 32/64/80/BCD/целых, env/save | реализовано |
| Арифметика, сравнения, FCOMI/FCMOVcc (EFLAGS), константы | реализовано |
| FPREM/FPREM1, FSINCOS, FPTAN, FXTRACT, FYL2XP1 | реализовано |
| FPU status IE/ZE/ES, диапазоны FIST/FSQRT/FYL2X | частично (без точного #MF и полного набора OE/UE/PE) |
| Округление по CW для арифметики и store | частично (host long double) |
| MMX: packed arithmetic, saturation, compare, pack/unpack, shifts, PSHUFW, PMOVMSKB, MASKMOVQ | реализовано |
| SSE/SSE2/FXSR | отсутствуют у Klamath/Deschutes, CPUID не выставляет |

## Не моделируется сознательно

- Суперскалярный конвейер, предсказание переходов, штрафы, спекуляция,
  cycle-accurate тайминги.
- Реальные кэши, write buffer, bus locking/coherency, GTL+/APIC bus.
- Микроархитектурные счётчики производительности и PMI.
- 36-битная физическая память (PSE-36/PAE за пределами 4 GiB).
