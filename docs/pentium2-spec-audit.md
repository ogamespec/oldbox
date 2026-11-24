# Повторный аудит интерпретатора Pentium II

## Вывод о полноте

Интерпретатор покрывает основные группы IA-32/P6, режимы real/protected/V86,
сегментацию, переключение задач, paging, x87 и исходный MMX. **Полного
архитектурного соответствия Pentium II пока нет.** Наличие обработчика опкода
и успешная загрузка DOS не доказывают корректность всех форм, исключений и
пограничных условий. Процент «полноты» не вычисляется: исчерпывающего набора
архитектурных проверок в проекте нет.

Проверены `cpu.cpp`, `cpu_0f.cpp`, `cpu_system.cpp`, `cpu_x87.cpp`, быстрый TLB
в `cpu.hpp`, связанные тесты и прежние утверждения в документации.
APIC/SMM/MSR сопоставлены на уровне имеющихся возможностей и ограничений;
их полная верификация, электрические характеристики и cycle-accurate
микроархитектура не входят в результаты этого прохода.

## Источники

Основные локальные документы (использованы PDF и текстовые извлечения):

| Документ из `specs/` | Основание проверки |
|---|---|
| `242690-001.pdf` | архитектура Pentium Pro/P6 |
| `242691-001.pdf` | Vol. 2: LMSW (11-267), LOCK (11-269), RDPMC (11-330), x87, SHLD/SHRD, VERR/VERW |
| `242692-001.pdf` | Vol. 3: paging A/D и PGE (3-26–3-28), PAE, TSS (рис. 6-2, 6-5–6-6), предел длины инструкции 15 байт |
| `243502-001.pdf` | Pentium II, MMX (§2.3); отсылает к отдельному MMX Technology Developer’s Guide |
| `24333503.pdf` | целевая модель Pentium II и аппаратные возможности |

Локальный Pentium Pro PRM не содержит полного описания MMX и
SYSENTER/SYSEXIT. Для этих дополнений использована также опубликованная
выдержка Intel SDM: [SYSENTER](https://www.felixcloutier.com/x86/sysenter),
[SYSEXIT](https://www.felixcloutier.com/x86/sysexit),
[EMMS](https://www.felixcloutier.com/x86/emms),
[PADDUS](https://www.felixcloutier.com/x86/paddusb:paddusw),
[PANDN](https://www.felixcloutier.com/x86/pandn).
Это вторичная HTML-публикация; современные расширения из этих описаний
не переносились в модель Pentium II. Код других эмуляторов не использовался.

## Исправления

| Подсистема | Найденное расхождение и исправление | Регрессии (`audit_pii_*`) |
|---|---|---|
| Декодер | Не ограничивалась длина инструкции. Теперь 16-й байт, включая immediate, вызывает #GP(0) | `instruction_length_includes_immediates` |
| LOCK | Префикс игнорировался. Проверяются инструкция, направление, память и reg-поле группы до изменения операндов | `lock_rejects_invalid_forms_before_side_effects` |
| LMSW | PE мог сбрасываться. Сделан sticky при LMSW | `lmsw_cannot_clear_pe` |
| RDPMC | Проверялся TSD вместо CR4.PCE; ECX маскировался до 2 бит. Теперь PCE=бит 8, разрешены только индексы 0/1 | `rdpmc_checks_pce_and_full_counter_index` |
| SYSENTER/SYSEXIT | Читались GDT-дескрипторы, неверно изменялся IF. Теперь фиксированные flat CS/SS, проверки PE/CS/CPL, SYSENTER очищает IF, SYSEXIT его сохраняет | `sysenter_sysexit_*` |
| VERR/VERW | P=0 ошибочно означал отказ. Проверка доступности не зависит от Present | `verr_verw_ignore_present_bit` |
| SHLD/SHRD | Не вычислялся определённый OF при count=1 | `shld_shrd_define_overflow_for_count_one` |
| 32-битный TSS | ES/CS/SS/DS/FS/GS читались и сохранялись с шагом 2 вместо 4. Правильные смещения: 48/4C/50/54/58/5C hex | `tss_selector_slots_and_io_map_are_independent`, существующие task-switch/V86-тесты |
| I/O bitmap | Offset читался из 64h (поле T), с обнулением младшего бита. Теперь 66h, нечётные адреса допустимы; у 16-битного TSS bitmap отсутствует | тот же тест, `mode_v86_*`, `cpu_io_bitmap_*` |
| Paging | Не выставлялись A/D. Добавлены A для PDE/PTE и D для листа, в том числе при первой записи после TLB-read-hit; запрещённая запись не выставляет D | `paging_sets_accessed_and_dirty_on_tlb_hit`, `paging_denied_write_does_not_set_dirty` |
| PAE | PDPTE ошибочно проверялся по U/S. У legacy PAE права задаются PDE/PTE | проверка A/D с CPL3, PDPTE=base\|1, страницы 4K/2M |
| TLB/PGE | `CR3 OR page` давал коллизии и делал global-запись недоступной после смены CR3. Теперь tag — линейная страница; CR3 сбрасывает non-global, D обновляет исходный лист | `tlb_tags_do_not_alias_linear_pages`, `global_translation_and_dirty_survive_cr3_reload` |
| SMM/TLB | Кэш трансляций мог пережить возврат с изменённой таблицей. Сброс на входе/выходе SMM | `smm_return_invalidates_cached_translation` |
| MMX arithmetic | PADDUSB/W и PSUBUSB/W выполняли wrap вместо unsigned saturation; PANDN инвертировал источник вместо назначения; PMULLW допускал signed overflow хоста | `mmx_unsigned_saturation_and_pandn`, `pmullw_high_unsigned_products_wrap_without_signed_overflow` |
| MMX state | Не проверялись EM/TS; EMMS был no-op. Теперь #UD/#NM, MMX помечает теги valid и TOP=0, EMMS освобождает теги | `mmx_availability_and_emms_tags` |
| Граница ISA | Выполнялись SSE/SSE2 packed-расширения, несуществующие MMX PSRAQ/PSLLDQ/PSRLDQ и SSE3 FISTTP. Теперь #UD | `rejects_post_pii_integer_extensions`, обновлённые MMX/FIST-тесты |
| x87 арифметика | FSCALE снимал ST(0) и путал аргументы; FRNDINT игнорировал RC; DC-регистровые FSUB[R]/FDIV[R] были переставлены | `x87_scale_round_and_register_arithmetic` |
| FPREM/FPREM1 | Выбор операции сравнивал D9 с F5; биты частного были переставлены. Исправлены округление, C0=Q2/C3=Q1/C1=Q0, добавлено частичное уменьшение с C2 | `x87_remainders_*`, `x87_partial_remainder_requires_multiple_steps` |
| x87 environment | FNSTENV маскировал сохранённый CW вместо внутреннего. Теперь сначала сохранение исходного CW, затем маскирование. Control-инструкции сохраняют предшествующий FIP/FOP; FOP включает ModRM; FLDENV восстанавливает empty-теги | `x87_environment_*`, `fpu_fstenv_saves_control_word_then_masks_exceptions` |
| x87 misc | FBLD был закодирован DB /4 вместо DF /4; FFREE игнорировал TOP; FNCLEX очищал только ES; FUCOMI NaN не выставлял CF/ZF и портил C0/C2/C3 | BCD roundtrip, `x87_extended_special_values_and_free_register`, `fucomi_nan_*` |
| Extended format | Infinity/NaN записывались неверно; NaN читался как infinity; исправлены классификация и кодировка специальных значений | `x87_extended_special_values_and_free_register` |

Часть прежних тестов повторяла ошибки реализации. Их ожидания исправлены
по документам; в частности, прежние пункты 17 и 30 в `cpu-audit.md` были
неверны. Добавлены 24 отдельных регрессионных теста с переборами размеров,
режимов, индексов, насыщения и битов частного.

## Что ещё требуется для полного соответствия

- **x87:** нужен независимый extended-precision backend. MSVC `long double`
  равен double; диапазон и точность Intel 80-bit здесь не обеспечиваются.
  Не полны PC/RC для арифметики/store, NaN payload/SNaN, stack faults,
  OE/UE/PE/DE, подавление результата при немаскированном исключении и #MF.
  FPU data pointer ещё не отслеживается; все tag-классы не моделируются.
- **MMX/x87 aliasing:** массивы данных по-прежнему раздельные. Теги/EMMS
  исправлены, но FSAVE/FRSTOR не обеспечивают точный round-trip MMX-контекста.
  #MF перед MMX и все варианты приоритетов ошибок требуют доработки.
- **Paging/исключения:** нет полного контроля reserved bits и #PF.RSVD,
  36-битных физических адресов, PDPTE caching; A при неуспешном обходе
  моделируется неполно. Требуется точная атомарность записи через границу
  страницы и восстановления состояния после fault.
- **Доставка исключений:** отсутствующие/повреждённые IDT-гейты и вложенные
  ошибки не образуют полную архитектурную цепочку #NP/#GP/#DF/triple fault;
  порядок ошибок task switch и interrupt shadow требует отдельной проверки.
- **Системные регистры:** не все reserved-биты CR0/CR4 и MSR проверяются;
  MSR ниже 200h сохраняются слишком широко. CPUID/cache descriptors и
  модельно-зависимые PAT/PSE36/прочие признаки требуют уточнения по stepping.
- **SMM/APIC/debug/perf:** частичные модели, без полного MP/APIC bus,
  IO breakpoints, PMI и всех performance events; SMM save map неполна.
- **Микроархитектура:** нет точных кэшей, bus locking/coherency, конвейера,
  спекуляции и cycle-accurate таймингов.

Это остаточные задачи, а не подтверждённые возможности. Новые регрессии
проверяют конкретные исправления и не заменяют полную CPU conformance suite.

## Проверка

Сборка Windows/MSVC Release, цель `oldbox_tests`; полный CTest включает
CPU, устройства, сохранение состояния и загрузку FreeDOS с настоящим BIOS.
До изменений: 471 тест. После изменений: 495 тестов.

```powershell
cmake --build build/vs2026 --config Release --target oldbox_tests --parallel
ctest --test-dir build/vs2026 -C Release --output-on-failure
```
