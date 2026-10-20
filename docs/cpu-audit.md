# Аудит набора инструкций IA-32

Аудит выполнен по первичным документам из `specs/`:

- `specs/242690-001.pdf` — Pentium Pro Family Developer’s Manual, Volume 1
  (архитектура, форматы, режимы);
- `specs/242691-001.pdf` — Volume 2 (Programmer’s Reference Manual, описания
  инструкций, кодировки, флаги, исключения);
- `specs/242692-001.pdf` — Volume 3 (системное программирование, сегментация,
  страницы, прерывания, TSS);
- `specs/243502-001.pdf`, `specs/24333503.pdf` — Pentium II: CPUID, MSR, MMX,
  отличия Klamath/Deschutes.

Код других эмуляторов не использовался. Проверка велась по наблюдаемым
архитектурным эффектам: результат, CF/PF/AF/ZF/SF/OF, размер и порядок
записи в стек, селекторы/базы/лимиты, номер и error code исключения,
содержимое памяти и портов.

## Методика

1. Для каждой группы опкодов выписаны кодировки и семантика из тома 2:
   размер операнда по умолчанию/префиксам, направление, флаги (какие
   определены, какие undefined), поведение при 0/переполнении, особые случаи.
2. В `tests/` добавлены детерминированные проверки. Для ALU используется
   независимая эталонная модель (`tests/cpu_harness.hpp`: `ref_add`, `ref_sub`,
   `ref_logic`) и сверяются все шесть флагов, а не только результат.
3. Граничные значения перебираются явно: нули, ±1, знаковый бит, `0x7f/0x80`,
   `0xffff`, `0xffffffff`, максимумы, а для 8-битных BCD/сдвигов — полный
   перебор 256 значений.
4. Сквозная проверка: FreeDOS 1.2 с настоящим `EP-ZXA.bin`. UPX-распаковщик
   `KERNEL.SYS` — большой стресс-тест целочисленных инструкций, строковых
   операций, сдвигов и битовой арифметики.

## Найденные и исправленные ошибки

| # | Инструкция/группа | Ошибка | Последствие | Регрессия |
|---|---|---|---|---|
| 1 | `ADC`/`SBB` (r/m,r; r/m,imm; AL/AX,imm; группа 1) | перенос/заём не участвовал в расчёте CF/OF/AF, если `src+carry` переполнял разрядность | **FreeDOS не загружался**: UPX-распаковщик использует `adc bh,bh` и `adc cx,cx` для сборки битового потока | `cpu_alu_all_sizes_and_operations`, `cpu_adc_sbb_carry_boundary` |
| 2 | `IMUL` r,r/m и r,r/m,imm | результат дополнительно записывал SF/ZF/PF (undefined) | неверные условные переходы после умножения | `cpu_mul_imul_flags` |
| 3 | `MUL r/m8` | CF/OF не выставлялись при `AH != 0` | потеря переполнения 8-битного умножения | `cpu_mul_imul_flags` |
| 4 | `AAM` | SF/ZF/PF считались по частному (AH), а не по остатку (AL) | неверные флаги после `AAM` | `cpu_aam_aad_aaa_aas` |
| 5 | `AAA`/`AAS` | перенос из AL складывался с AH (AAA), AH не всегда уменьшался (AAS) | неверный BCD-результат | `cpu_aam_aad_aaa_aas` |
| 6 | `ENTER` | цикл копирования кадра шёл `level` раз вместо `level-1`, всегда читал 4 байта, лишний `push frame` для level=0 | повреждение стека | `cpu_enter_levels` |
| 7 | `ROL/ROR/RCL/RCR` | OF не обновлялся для count=1 | неверный `JO/JNO` после вращения | `cpu_shift_rotate_flags` |
| 8 | `MOV Sreg,r/m` | `MOV CS` и reg=6/7 выполнялись вместо `#UD` | порча CS/выход за границы массива сегментов | `cpu_invalid_segment_and_bound_raise_ud` |
| 9 | `BOUND r,m` | форма mod=3 не давала `#UD` | неверное исключение | `cpu_invalid_segment_and_bound_raise_ud` |
| 10 | near `JMP/Jcc/LOOP/JCXZ` в real mode | 16-битное смещение складывалось с 32-битным EIP без маски | «висящие» старшие биты EIP, неверный трассировщик | `cpu_real_mode_near_branch_wraps` |
| 11 | `XLAT` | обходил проверку лимита сегмента | доступ за пределы сегмента в protected mode | покрыто `cpu_enter_leave_and_xlat` |
| 12 | `SHLD/SHRD` | при count > разрядности сдвиг на отрицательную величину (UB); CF при count=разрядности брался не из того бита | неопределённое поведение и неверный CF | `cpu_shld_shrd` |
| 13 | `IDIV` | `INT_MIN / -1` вызывал UB в C++ до проверки переполнения | падение эмулятора вместо `#DE` | `cpu_idiv_overflow_raises_divide_error` |
| 14 | Возобновление после исключения | `opcode_ip_` не записывался | `#GP/#PF` возвращались на адрес 0 | проверяется тестами исключений/режимов |
| 15 | `FILD m32int` | 32-битное значение читалось как беззнаковое | отрицательные целые превращались в большие положительные | `fpu_fild_negative_and_fist_rounding` |
| 16 | `FIST/FISTP` | округление делалось через `trunc`, игнорируя CW[11:10] | неверные целые при round/floor/ceil; отсутствовал `FISTTP` | `fpu_fild_negative_and_fist_rounding` |
| 17 | `FNSTENV`/`FSTENV`, `FCOMI` | сохранялся немаскированный CW и менялся внутренний CW; FCOMI не очищал OF/SF/AF | неверный env-образ и флаги после сравнения | `fpu_fstenv_masks_stored_control_word`, `fpu_fcomi_clears_of_sf_af` |
| 18 | `LAR` | access rights сдвигались на 8 бит (`access<<8`, flags<<16) | неверный формат результата LAR | `system_lar_lsl_descriptor_access_rights` |
| 19 | `CMOVcc` | память читалась только при истинном условии | ложное условие скрывало возможный page fault | покрыто `cpu_condition_codes_setcc_cmovcc` |
| 20 | V86: сегментация, CPL, стек прерываний | V86-загрузки сегментов шли через GDT/LDT, `CPL` брался из 16-битного селектора, умолчания были 32-битными, прерывание сбрасывало CPU в real mode и не переключало стек | DOS-программы не могли работать в V86; неверные page-права и IOPL | `mode_v86_*` + `mode_task_switch_enters_virtual86` (16 тестов) |
| 21 | V86/VME: прерывания и IRET | не было IDT-доставки с ring-0 стеком, кадра GS/FS/DS/ES, очистки сегментов, IOPL-проверок PUSHF/POPF/IRET/INTn/CLI/STI; IRET не различал возврат из/в V86 | падение V86-задачи и порча стека | `mode_v86_interrupt_uses_idt_tss_stack_and_iret`, `mode_v86_iopl_sensitive_instructions_fault`, `mode_v86_pushf_popf_and_iret_preserve_v86_state` |
| 22 | VME | CR4.VME, redirection bitmap и VIF/VIP не реализованы, хотя CPUID выставлял VME | INTn в V86 всегда шёл в IDT; не работали virtual IF и redirection | `mode_v86_vme_*` (4 теста) |
| 23 | Task gate | селектор TSS читался из байтов 0–1 IDT-записи вместо 2–3 | любой task gate вёл на селектор 0 и shutdown | `mode_task_switch_enters_virtual86` |
| 24 | `BSWAP` | декодировался лишний ModRM-байт, выбирался не тот регистр и съедалась следующая инструкция | повреждение потока команд и неверный результат | `system_xchg_xadd_bswap` |
| 25 | Группа `0F 01` | `/6` и `/7` были перепутаны: LMSW не работал, INVLPG не проверял CPL | неверный CR0/TLB | `system_smsw_lmsw_sidt_and_cache_ops` |
| 26 | `VERR`/`VERW`, `LAR`/`LSL`, `SLDT`/`STR`, `LLDT`/`LTR`, `ARPL` | в real/V86 выполнялись вместо `#UD`; VERR/VERW всегда читали GDT и принимали code/system за data | неверные ZF и отсутствующие исключения | `system_arpl_verr_verw_lar_lsl_mode_rules` |
| 27 | FAR JMP/CALL/RETF, near-переходы, `BT` | FAR-переход разрешал эскалацию привилегий; RETF не восстанавливал SS:ESP; 16-битные near-ветви и LOOP не маскировали EIP; BT не поддерживал отрицательные смещения | межкольцевые переходы и битовые строки работали неверно | `mode_far_transfer_cannot_escalate_privilege`, `mode_retf_interprivilege_restores_stack`, `cpu_near_branch_16bit_operand_masks_eip`, `cpu_loop_uses_address_size_for_eip_mask`, `cpu_bit_test_memory_signed_and_immediate_offsets` |
| 28 | Call gates | FAR CALL/JMP не распознавали call gate/task gate/TSS; не было копирования параметров и переключения стека | межкольцевые вызовы и 16/32-битные интерфейсы не работали | `mode_call_gate_*`, `mode_16bit_call_gate_pushes_word_frame` |
| 29 | Task switching | `switch_to_task` не вёл busy/NT/link, не сохранял CR3/LDT, не поддерживал 16-битный TSS и возврат по IRET NT | вложенные задачи и возврат в приостановленную задачу ломались | `mode_far_call_task_switch_links_and_iret_returns`, `mode_16bit_tss_task_switch`, `mode_task_switch_tss_debug_trap_sets_bt` |
| 30 | TSS-офсеты | CS/SS/DS/FS/GS в 32-битном TSS читались со сдвигом на 2 байта; I/O map base читался с 0x66 вместо 0x64 | неверные сегменты после task switch и bitmap I/O | `mode_far_call_task_switch_links_and_iret_returns`, `mode_v86_io_permission_bitmap_is_checked` |
| 31 | Debug | DR6/DR7 не имели reset-значений; не было data/instruction breakpoints, TF, RF, GD, DR4/5 alias, BT | отладчики и single-step не работали | `system_debug_*`, `system_icebp_and_debug_register_access` |
| 32 | PGE/INVLPG | не было global-бита TLB и выборочного INVLPG | CR3 не сохранял global-страницы, INVLPG сбрасывал весь TLB | `system_pge_keeps_global_tlb_entries` |
| 33 | Alignment check | CR0.AM/EFLAGS.AC/CPL3 не давали #AC | отсутствовала диагностика невыровненных обращений | `system_alignment_check_raises_ac` |
| 34 | Machine check | MCG_CAP/STATUS, банки MCi_*, #MC не реализованы | MCE-обработчики не работали | `system_machine_check_*` |
| 35 | MTRR/Perf MSR | PhysBase/PhysMask были алиасированы; PerfCtr читались по неверным адресам (0x187/0x188 вместо 0xC1/0xC2) | неверные MSR-значения | `system_mtrr_base_mask_registers_are_independent`, `system_performance_counters_*` |
| 36 | SMM | не сохранялся LDT base; авто-HALT restart и latched NMI терялись | RSM возвращал неполное состояние | `system_smm_saves_ldt_base_and_auto_halt`, `system_smm_blocks_nmi_until_rsm` |
| 37 | APIC | TPR/ISR/IRR не управляли доставкой, timer divide 8..11 были неверны, ICR trigger неверен | маскировка прерываний и таймер работали неверно | `local_apic_tpr_masks_irr_and_eoi_services_pending`, `local_apic_timer_divide_and_version` |
| 38 | x87 | FCMOVcc использовал x87 C0/C2/C3 вместо EFLAGS; FPREM/FPREM1/FSINCOS/FPTAN/FXTRACT/FYL2XP1 были заглушками; FIST не проверял диапазон | неверные результаты и UB при переполнении | `fpu_fprem_*`, `fpu_fsincos_fptan_and_fxtract`, `fpu_fcmov*`, `fpu_fist_out_of_range_stores_indefinite` |
| 39 | FWAIT | TS вызывал #NM даже при MP=0 | неверная семантика WAIT | `system_fwait_respects_ts_and_mp` |
| 40 | CPUID leaf 2 | EDX не содержал cache/TLB дескрипторы | неверная информация о кэшах | `system_cpuid_identity_and_features` |

Пункты 1–40 подтверждены тестами; пункт 1 — ещё и сквозной загрузкой
FreeDOS, пункты 20–22 — тестами `mode_v86_*`, пункты 28–37 — тестами
`mode_*`, `system_*`, `local_apic_*`.

## Покрытие по группам

| Группа | Что проверено |
|---|---|
| Пересылки | MOV всех размеров, MOVZX/MOVSX, LEA, LDS/LES/LSS/LFS/LGS, XCHG, BSWAP, MOV CR/DR |
| Арифметика | ADD/ADC/SUB/SBB/CMP/INC/DEC/NEG, все размеры и граничные значения, CF/OF/AF/PF/ZF/SF |
| Умножение/деление | MUL/IMUL (1/2/3 операнда), DIV/IDIV, переполнение и `#DE`, знаковые случаи |
| Логика | AND/OR/XOR/TEST/NOT, обнуление CF/OF/AF, SZP |
| Сдвиги/вращения | SHL/SHR/SAR/ROL/ROR/RCL/RCR, count=0/1/>разрядности, CF/OF, SHLD/SHRD |
| Биты | BT/BTS/BTR/BTC (регистр/память/imm, отрицательные смещения), BSF/BSR, ZF при нуле |
| Переходы | Jcc short/near, CALL/RET, CALL FAR/RETF, JMP/JMP FAR, LOOP/LOOPZ/LOOPNZ/JCXZ, обёртка IP в real/protected/V86, межкольцевой RETF, запрет эскалации FAR JMP/CALL |
| Стек | PUSH/POP, PUSHA/POPA, push imm, ENTER/LEAVE, порядок кадра, SP в PUSHA |
| Строки | MOVS/CMPS/STOS/LODS/SCAS, DF=0/1, REP/REPZ/REPNE, адресация 16/32, сегментные overrides |
| BCD | DAA/DAS — полный перебор 256×AF×CF, AAA/AAS, AAM/AAD |
| I/O | IN/OUT 8/16/32, границы портов, INS/OUTS, IOPL/CPL, TSS I/O bitmap |
| Прерывания | INT n/INT3/INTO/IRET, real/protected/V86, error code 16/32, IDT interrupt/trap/task gates, TSS-стек, V86-кадр с сегментами, VME-перенаправление |
| Сегментация | real/protected, загрузка сегментов, DPL/RPL/CPL, лимиты, expand-down/conforming, LDT |
| Страницы | 4 KiB, PSE, PAE, WP, U/S, page fault error code, CR2, TLB |
| Системные | CPUID, MSR, RDTSC/RDPMC, SYSENTER/SYSEXIT, CR/DR, CLTS, INVLPG, WBINVD, SMM, Local APIC |
| x87 | загрузка/сохранение 32/64/80/BCD/целых, арифметика, сравнения, FCOMI, FCMOVcc, управление, env/save |
| MMX | арифметика с насыщением, сравнения, распаковка/упаковка, сдвиги, умножение, MOVD/MOVQ, EMMS |

## Найдено вне CPU при сквозной загрузке

- INT 13h: драйвер A: через `FloppyController::read_chs`, геометрия 1.44M,
  корректные ошибки для отсутствующих дисков и отказ от EDD (`AH=41h`
  возвращает CF=1), чтобы boot sector не уходил в неподдерживаемый `AH=42h`.
- INT 1Ah: AH=00 (тики BDA), AH=02 (время RTC), AH=04 (дата RTC), AH=01/03
  (установка времени/даты); без этого FreeDOS/FreeCom получали мусор.
- PIT: `reload=0` = 65536 и дельта-накопление тиков (см.
  `docs/firmware.md`); иначе BDA-таймер и время DOS шли в десятки тысяч раз быстрее
  реального (после первой ошибки — на тысячи процентов).

## Остаточные ограничения

Они не скрыты и перечислены также в `docs/cpu.md`:

- интерпретатор не cycle-accurate; штрафы за промахи, спекуляция,
  конвейер и порядок выполнения не моделируются;
- SSE/SSE2/FXSR отсутствуют — у Klamath/Deschutes их нет, CPUID их не
  выставляет; `FXSAVE/FXRSTOR` не реализованы;
- x87-трансцендентные функции (`FSIN`, `FCOS`, `F2XM1`, `FYL2X`, `FYL2XP1`,
  `FSINCOS`, `FPATAN`) считаются через `long double` хоста, а не по таблицам
  Intel; `FPREM/FPREM1` не выполняют пошаговое уменьшение; маски/исключения
  FPU моделируются частично (IE/UD флаги, без полного набора);
- V86-monitor и VME работают для DOS-сценария: 8086-сегментация, IDT/TSS,
  IOPL-проверки, redirection bitmap, VIF/VIP и method 6 для исключений и
  аппаратных прерываний. Остаются упрощения: method 3 для аппаратных
  прерываний и часть PVI/VME-нюансов моделируются приближённо;
- call-гейты, task gates и FAR JMP/CALL на TSS реализованы; IRET с NT
  выполняет переход по link-полю TSS, но порядок редких ошибок #TS/#GP
  при повреждённых TSS упрощён;
- FPU: #MF не доставляется, статусные флаги OE/UE/PE и округление
  арифметики по CW моделируются частично;
- APIC: один CPU, без APIC bus/arbitration и полного MP protocol;
  MTRR/PAT не влияют на кэш, потому что кэша в модели нет;
- performance counters считают только INST_RETIRED, INST_DECODED и
  HW_INT_RX; overflow/PMI и остальные события не моделируются;
- SMM и Local APIC — упрощённые модели, достаточные для тестов, но не
  для произвольного системного ПО;
- часть редких MMX-форм и недокументированных опкодов не реализована;
  неизвестный опкод логируется, а не исполняется.

## Как воспроизвести

```bash
cmake --build build -j
./build/oldbox_tests

./build/oldbox --bios assets/bios/EP-ZXA.bin \
  --floppy assets/floppy/freedos1440.img \
  --headless --seconds 2 --dump-text
```

Во втором случае в text buffer должны быть строки `FreeDOS kernel ...`,
`FreeCom version ...` и `A:\>`.
