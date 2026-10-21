# Аудит набора инструкций IA-32

Аудит выполнен по первичным документам из `specs/`:

- `specs/242690-001.pdf` — Pentium Pro Family Developer’s Manual, Volume 1
  (архитектура, форматы, режимы);
- `specs/242691-001.pdf` — Volume 2 (Programmer’s Reference Manual, описания
  инструкций, кодировки, флаги, исключения; MMX в это издание не входит);
- `specs/242692-001.pdf` — Volume 3 (системное программирование, сегментация,
  страницы, прерывания, TSS, SMM, приложение C с MSR);
- `specs/243502-001.pdf`, `specs/24333503.pdf` — Pentium II: аппаратная
  архитектура, CPUID, MSR, наличие MMX, отличия Klamath/Deschutes.
- MMX-семантика проверялась по архитектурному описанию набора MMX
  (в перечисленных томах Pentium Pro её нет); кодировки и эффекты
  сопоставлены с уже реализованными формами и тестами.

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
| 18 | `LAR`/`LSL` | результат `LAR` собирался не в тех битах (нужно: 16-битная форма — байт доступа в AH; 32-битная — байт 6 дескриптора в 23:16), `LSL` повторно масштабировал G-limit, не было проверок CPL/RPL и таблиц допустимых типов | DOS-extender Windows 95 standard mode неверно проверял селекторы и уходил в «Standard Mode: Bad Fault in MS-DOS Extender» | `system_lar_lsl_descriptor_access_rights`, `system_lar32_and_lsl32_use_descriptor_bytes` |
| 19 | `CMOVcc` | память читалась только при истинном условии | ложное условие скрывало возможный page fault | покрыто `cpu_condition_codes_setcc_cmovcc` |
| 20 | V86: сегментация, CPL, стек прерываний | V86-загрузки сегментов шли через GDT/LDT, `CPL` брался из 16-битного селектора, умолчания были 32-битными, прерывание сбрасывало CPU в real mode и не переключало стек | DOS-программы не могли работать в V86; неверные page-права и IOPL | `mode_v86_*` + `mode_task_switch_enters_virtual86` (16 тестов) |
| 21 | V86/VME: прерывания и IRET | не было IDT-доставки с ring-0 стеком, кадра GS/FS/DS/ES, очистки сегментов, IOPL-проверок PUSHF/POPF/IRET/INTn/CLI/STI; IRET не различал возврат из/в V86 | падение V86-задачи и порча стека | `mode_v86_interrupt_uses_idt_tss_stack_and_iret`, `mode_v86_iopl_sensitive_instructions_fault`, `mode_v86_pushf_popf_and_iret_preserve_v86_state` |
| 22 | VME | CR4.VME, redirection bitmap и VIF/VIP не реализованы, хотя CPUID выставлял VME | INTn в V86 всегда шёл в IDT; не работали virtual IF и redirection | `mode_v86_vme_*` (4 теста) |
| 23 | Task gate | селектор TSS читался из байтов 0–1 IDT-записи вместо 2–3 | любой task gate вёл на селектор 0 и shutdown | `mode_task_switch_enters_virtual86` |
| 24 | `BSWAP` | декодировался лишний ModRM-байт, выбирался не тот регистр и съедалась следующая инструкция | повреждение потока команд и неверный результат | `system_xchg_xadd_bswap` |
| 25 | Группа `0F 01` | `/6` и `/7` были перепутаны: LMSW не работал, INVLPG не проверял CPL | неверный CR0/TLB | `system_smsw_lmsw_sidt_and_cache_ops` |
| 26 | `VERR`/`VERW`, `LAR`/`LSL`, `SLDT`/`STR`, `LLDT`/`LTR`, `ARPL` | в real/V86 выполнялись вместо `#UD`; VERR/VERW всегда читали GDT и принимали code/system за data | неверные ZF и отсутствующие исключения | `system_arpl_verr_verw_lar_lsl_mode_rules` |
| 27 | FAR JMP/CALL/RETF, near-переходы, `BT` | FAR-переход разрешал эскалацию привилегий; RETF не восстанавливал SS:ESP; 16-битные near-ветви и LOOP не маскировали EIP; BT не поддерживал отрицательные смещения | межкольцевые переходы и битовые строки работали неверно | `mode_far_transfer_cannot_escalate_privilege`, `mode_retf_interprivilege_restores_stack`, `cpu_near_branch_16bit_operand_masks_eip`, `cpu_loop_uses_operand_size_for_eip_mask`, `cpu_bit_test_memory_signed_and_immediate_offsets` |
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
| 41 | `IRET`, возврат на внешний уровень | `IOPL`/`IF`/`VIF`/`VIP` обновлялись по **новому** CPL, а не по CPL на момент выполнения `IRET` | ring-0 обработчик не мог вернуть ring-3 код с `IOPL=3`; DOSX получал #GP на `CLI` и уходил в «Bad Fault in MS-DOS Extender» | `mode_tss_stack_switch_and_iret` |
| 42 | `MOV r32,Sreg` (8C), `MOV Sreg` (8E) | 32-битный регистр-приёмник не обнулял старшие 16 бит; `/6,/7` читали за границу `state_.seg[6]`; отсутствующий SS давал #GP вместо #SS, DS/ES/FS/GS — #GP вместо #NP | неверный selector, OOB-чтение, неверный вектор/error code | `audit_mov_from_segment_zero_extends_32bit_register`, `audit_mov_from_segment_rejects_register_fields_6_and_7`, `audit_mov_to_segment_not_present_raises_np_or_ss` |
| 43 | `LLDT`/`LTR` | селектор с TI=1 принимался и читался из GDT | неверный #GP/загрузка LDT/TSS | `audit_lldt_and_ltr_reject_ldt_selectors` |
| 44 | `STI` | не было одноинструкционного запрета прерываний | маскируемый IRQ мог быть доставлен сразу после STI, до следующей инструкции | `audit_sti_delays_maskable_interrupts_by_one_instruction` |
| 45 | `LOOP`/`JCXZ` | маска EIP бралась по address-size; по PRM Vol. 2 маска определяется operand-size | неверный target при 66/67-префиксах | `cpu_loop_uses_operand_size_for_eip_mask`, `audit_loop_and_jcxz_mask_eip_by_operand_size_not_address_size` |
| 46 | V86-доступ через 64K | проверялось только `offset > FFFFh`, а не последний байт | 2/4-байтовый доступ с `FFFFh` проходил без #GP | `audit_v86_access_crossing_64k_raises_gp` |
| 47 | `IRET` в V86 с CPL>0 | бит VM в образе EFLAGS молча игнорировался | вместо #GP(0) выполнялся возврат с потерей VM | `audit_iret_to_vm_from_cpl3_raises_gp` |
| 48 | Невалидные кодировки | группа 6 `/6,/7` давала #GP; SGDT/SIDT/LGDT/LIDT/INVLPG принимали mod=3, `0F 01 /5` был no-op; группа 8 `/0../3` не давала #UD; C6/C7 игнорировали reg-field; CMPXCHG8B m64 в регистре давал #GP; LEA mod=3 давала #GP | неверные исключения/молчаливое выполнение мусора | `audit_invalid_encodings_raise_ud`, `audit_lea_register_form_raises_ud` |
| 49 | `PACKSSDW` | второй операнд паковался со сдвигом 64/80 бит и терялся | старшие два слова результата всегда 0 | `audit_packssdw_packs_both_operands`, `audit_mmx_unpack_pack_paddq_and_byte_shift` |
| 50 | MMX-сдвиги | count маскировался `& 3Fh`; при count ≥ ширины lane сдвиг на величину ≥64 — UB | неверные результаты PSRL/PSRA/PSLL на больших count | `audit_mmx_shift_counts_at_or_above_lane_width` |
| 51 | `PMADDWD` | сумма двух 32-битных произведений переполняла `i32` (UB) | неверный результат на граничных словах `8000h` | `audit_pmaddwd_wraps_at_32_bits` |
| 52 | MSR | не было fixed-MTRR `250h–26Fh`, `BIOS_UPDT_TRIG=79h`; строгий #GP на весь незадействованный диапазон ломал загрузку FreeDOS (BIOS/ядро читают PII BBL/L2 MSR) | #GP вместо значения / зависание загрузки | `audit_unimplemented_msr_raises_gp`; сквозная загрузка FreeDOS |
| 53 | x87 env/save | environment всегда писался в 28-байтном protected-формате, FNSAVE — со сдвигом данных 28; tag word индексировался позицией стека, а не физическим регистром; FNSAVE/FINIT не сбрасывали SW/указатели | DOS-обработчики FPU и round-trip FRSTOR видели неверный образ | `audit_fpu_16bit_environment_and_save_layout`, `audit_fnsave_and_finit_restore_default_state` |
| 54 | SMM | при входе не очищались PE/PG | SMM-код видел protected-mode CR0 | `audit_smm_enters_with_pe_and_pg_clear_and_rsm_restores_cr0` |
| 55 | FPU-указатели | `fpu_last_ip/op` не обновлялись, не было CS | FSTENV/FSAVE сохраняли нулевые указатели | покрыто `audit_fpu_16bit_environment_and_save_layout` |
| 56 | Префиксы `66h`/`67h` | повторный префикс переключал размер обратно (toggle), хотя эффективный размер выбирается по наличию префикса | `66 66` давал 32-битную операцию, `67 67` — 32-битную адресацию | `audit_repeated_operand_and_address_prefixes_do_not_toggle` |
| 57 | `SGDT`/`SIDT` с operand-size 16 | сохранялся полный 32-битный base вместо 24-битного с нулевым шестым байтом | неверный образ псевдодескриптора для 16-битного кода | `audit_sgdt_sidt_16bit_operand_masks_base` |
| 58 | `MOV SS,r/m16` | code-сегмент с readable-битом принимался как стековый: `data` вычислялся только по S-биту | SS мог указывать на код | `audit_mov_ss_rejects_code_segment` |
| 59 | `LLDT`/`LTR` | отсутствующий LDT/TSS давал #GP вместо #NP | неверный вектор и error code | `audit_lldt_ltr_not_present_raise_np` |
| 60 | Загрузка CS / target IDT-гейта | отсутствующий code-сегмент давал #GP вместо #NP | RETF/IRET/прерывание на P=0 сегмент давали неверное исключение | `audit_far_return_and_gate_target_not_present_raise_np` |
| 61 | Call gate/task switch, SS | expand-down стековый сегмент отвергался проверкой `access & 0Ch` | корректный expand-down SS давал #TS | `audit_call_gate_accepts_expand_down_stack`, `audit_task_switch_accepts_expand_down_stack` |
| 62 | Accessed bit дескриптора | не выставлялся при загрузке code/data-сегмента | OS/отладчик видели A=0 после фактического использования сегмента | `audit_descriptor_accessed_bit_is_set_on_segment_load` |

Пункты 1–62 подтверждены тестами; пункт 1 — ещё и сквозной загрузкой
FreeDOS, пункты 20–22 — тестами `mode_v86_*`, пункты 28–37 — тестами
`mode_*`, `system_*`, `local_apic_*`, пункты 42–62 — `test_cpu_audit.cpp`
и обновлёнными тестами `test_cpu_instructions.cpp`.

## Покрытие по группам

| Группа | Что проверено |
|---|---|
| Пересылки | MOV всех размеров, MOV Sreg (zero-extension, #UD/#NP/#SS, запрет code→SS), MOVZX/MOVSX, LEA (включая #UD для mod=3), LDS/LES/LSS/LFS/LGS, XCHG, BSWAP, MOV CR/DR |
| Префиксы | 66/67 в real/protected/V86, повторные префиксы, operand/address size для строк, LEA, moffs, стека (SS.B), LOOP/JCXZ, SGDT/SIDT |
| Арифметика | ADD/ADC/SUB/SBB/CMP/INC/DEC/NEG, все размеры и граничные значения, CF/OF/AF/PF/ZF/SF |
| Умножение/деление | MUL/IMUL (1/2/3 операнда), DIV/IDIV, переполнение и `#DE`, знаковые случаи |
| Логика | AND/OR/XOR/TEST/NOT, обнуление CF/OF/AF, SZP |
| Сдвиги/вращения | SHL/SHR/SAR/ROL/ROR/RCL/RCR, count=0/1/>разрядности, CF/OF, SHLD/SHRD |
| Биты | BT/BTS/BTR/BTC (регистр/память/imm, отрицательные смещения), BSF/BSR, ZF при нуле |
| Переходы | Jcc short/near, CALL/RET, CALL FAR/RETF, JMP/JMP FAR, LOOP/LOOPZ/LOOPNZ/JCXZ (маска EIP по operand-size), обёртка IP в real/protected/V86, межкольцевой RETF, запрет эскалации FAR JMP/CALL |
| Стек | PUSH/POP, PUSHA/POPA, push imm, ENTER/LEAVE, порядок кадра, SP в PUSHA |
| Строки | MOVS/CMPS/STOS/LODS/SCAS, DF=0/1, REP/REPZ/REPNE, адресация 16/32, сегментные overrides |
| BCD | DAA/DAS — полный перебор 256×AF×CF, AAA/AAS, AAM/AAD |
| I/O | IN/OUT 8/16/32, границы портов, INS/OUTS, IOPL/CPL, TSS I/O bitmap |
| Прерывания | INT n/INT3/INTO/IRET, real/protected/V86, error code 16/32, IDT interrupt/trap/task gates, TSS-стек, V86-кадр с сегментами, VME-перенаправление |
| Сегментация | real/protected, загрузка сегментов, DPL/RPL/CPL, лимиты, expand-down/conforming, LDT, accessed-bit, expand-down SS в call gate/task switch, #NP для LDT/TSS/CS |
| Страницы | 4 KiB, PSE, PAE, WP, U/S, page fault error code, CR2, TLB |
| Системные | CPUID, MSR (fixed-MTRR, PII BBL, #GP вне диапазона), RDTSC/RDPMC, SYSENTER/SYSEXIT, CR/DR, CLTS, INVLPG, WBINVD, SMM (PE/PG, LDT, auto-HALT/NMI), Local APIC |
| x87 | загрузка/сохранение 32/64/80/BCD/целых, арифметика, сравнения, FCOMI, FCMOVcc, управление, env/save в 14/28-байтных real/protected форматах, tag word по физическим регистрам, FNSAVE/FINIT reset |
| MMX | арифметика с насыщением (PACKSSDW/PACKSSWB/PACKUSWB), сравнения, распаковка/упаковка, сдвиги с count ≥ ширины lane, умножение/PMADDWD wrap, MOVD/MOVQ, EMMS |

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
- IDT-вектор за пределом таблицы или отсутствующий (P=0) гейт приводят к
  shutdown, а не к полноценной цепочке #DF/#NP; вложенная доставка
  исключения внутри обработчика поддерживается только на один уровень;
- `SYSENTER`/`SYSEXIT` загружают CS/SS из GDT упрощённо: тип/present/DPL
  целевых дескрипторов проверяются не полностью; page-table A/D-биты
  (в отличие от accessed-bit сегментных дескрипторов) не выставляются;
- FPU: #MF не доставляется, статусные флаги OE/UE/PE и округление
  арифметики по CW моделируются частично;
- APIC: один CPU, без APIC bus/arbitration и полного MP protocol;
  MTRR/PAT не влияют на кэш, потому что кэша в модели нет;
- performance counters считают только INST_RETIRED, INST_DECODED и
  HW_INT_RX; overflow/PMI и остальные события не моделируются;
- SMM и Local APIC — упрощённые модели, достаточные для тестов, но не
  для произвольного системного ПО;
- часть редких MMX-форм и недокументированных опкодов не реализована;
  неизвестный опкод логируется, а не исполняется;
- MSR-пространство ниже `0x200` (PII BBL/L2, fixed-MTRR, last-branch/ROB)
  хранится как обычные регистры, а не проверяется по битовым полям; `#GP`
  для неизвестных MSR выдаётся выше этого диапазона. Это компромисс:
  BIOS/ядро FreeDOS читают PII-специфичные BBL MSR, отсутствующие в
  приложении C Pentium Pro;
- `FISTTP` (`DB/DD/DF /1`) оставлен как расширение, хотя на Klamath/
  Deschutes эта форма не определена;
- FPU data pointer не отслеживается (в env-образе пишется 0), а указатели
  обновляются и для control-инструкций x87; для FSTENV/FSAVE это влияет
  только на диагностические поля.

## Как воспроизвести

```bash
cmake --build build -j
./build/oldbox_tests

./build/oldbox --bios assets/bios/EP-ZXA.bin \
  --floppy assets/floppy/freedos1440.img \
  --headless --seconds 2 --dump-text
```

Во втором случае в text buffer должны быть строки `FreeDOS kernel ...`,
`FreeCom version ...` и `A:\>` — если `freedos1440.img` является
загрузочной дискетой оболочки. Для установочной FD-SETUP-дискеты виден
экран `FreeDOS 1.2 - Install`; сквозной тест в этом случае пропускается.
