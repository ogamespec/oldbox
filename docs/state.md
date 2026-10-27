# Save states

OldBox умеет сохранять и восстанавливать состояние машины в один файл.  Это
нужно для отладки: вместо повторного прохождения POST, загрузки FreeDOS и
запуска программы можно один раз сохранить точку и загружать её заново.

## Команды консоли

```text
save [FILE]       записать состояние (по умолчанию oldbox-quicksave.state)
load [FILE]       восстановить состояние (по умолчанию oldbox-quicksave.state)
quicksave         то же, что save без аргумента
quickload         то же, что load без аргумента
```

Перед `save` недисковые изменения ATA и floppy сбрасываются на host, чтобы
файл образа и снимок не расходились.  `load` можно делать в любой момент, но
удобнее всего на паузе (`pause`), когда гость не выполняет инструкцию.

## Командная строка

```bat
oldbox.exe ... --load-state point.obx
oldbox.exe ... --seconds 45 --save-state point.obx
```

`--load-state` загружает снимок после начального сброса машины и до запуска
эмуляции.  `--save-state` пишет снимок после завершения прогона (в headless —
после `--seconds`, в окне — при выходе).

Пример отладки Windows 95 без 80-секундного ожидания:

```bat
oldbox.exe --bios assets\bios\EP-ZXA.bin ^
  --vga-bios assets\vga\S3Trio64_1_14-04.bin ^
  --floppy temp\freedos-win95-setup.img ^
  --cdrom C:\Isos\Windows95OSR2.iso --hdd assets\hdd\win95-hdd.img ^
  --headless --seconds 45 --save-state temp\setup-prompt.state

oldbox.exe ... --load-state temp\setup-prompt.state ^
  --seconds 20 --enter-every 1 --enter-start 1 --exc-trace --dump-text
```

## Что входит в снимок

- CPU: полный `CpuState`, LDT/TR, TSS, MSR/APIC-очередь, FPU/MMX, счётчики
  инструкций и циклов;
- RAM, A20, flash и счётчики POST;
- PIIX4: PIC, PIT, RTC/CMOS, DMA, 8042/клавиатура, SMBus, PM;
- PCI-конфигурация и I440BX/PAM;
- ATA/ATAPI: регистры каналов и приводов, буферы команд, CD-таблица;
- floppy: оба диска и состояние 82077AA;
- S3 Trio64: VRAM, VGA/S3-регистры, 2D-движок;
- Voodoo, если он включён;
- Super I/O W83877TF, LM78, Local APIC, PC speaker;
- счётчики `Machine` (frames, cycles, очередь ввода).

Формат: заголовок `OLDBOXST`, версия, хеш конфигурации, CRC32 и размер
payload.  Файл платформонезависим по раскладке (little-endian), но привязан к
версии emulator'а и к конфигурации машины: при другом объёме RAM, CPU, видео
или путях к образам загрузка отклоняется.

## Ограничения

- **Аудиокарта не входит в снимок.**  При загрузке AWE64 переинициализируется;
  цифровой звук и MIDI после `load` нужно запускать заново.  Для DOSX/Setup это
  не важно.
- **Содержимое HDD не откатывается.**  В снимок попадает состояние
  контроллера, но не 500-МБ образ.  Если после `save` гость записал на диск,
  `load` не отменит эти записи.  Для воспроизводимых проверок используйте
  отдельную копию HDD или `flush` + резервную копию.
- Снимок большой: около 70–80 МБ при 64 МБ RAM, 4 МБ VRAM и включённом
  Voodoo.  Это ожидаемо: RAM и VRAM хранятся целиком, без сжатия.
- Снимки, сделанные до изменения формата, несовместимы: номер версии в
  заголовке проверяется.
