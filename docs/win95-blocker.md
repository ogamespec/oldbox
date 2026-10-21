# Windows 95 OSR2 setup: current blocker (no workarounds)

This note records the state of the Windows 95 installation investigation.  All
guest-state workarounds (LDT identity remapping, shadow seeding, raw-selector
fallback, XMS fragmenter, pre-populated C:) have been removed; only immutable
CPU/emulator fixes and read-only diagnostics remain.

## Symptom

DOSX (the Windows 95 standard-mode DOS extender) builds an LDT alias for a
real-mode code segment and the monitor jumps into it:

```
lcall 0x0387:0x0B7E          ; alias CS base 0x0040DE20, limit 0x2ABF
```

The alias block is zero.  Execution walks through zero bytes until the limit
and faults:

```
EXC 13  csip=0387:2AC0  op=00...   Standard Mode: Fault outside of MS-DOS Extender
```

## What has been verified (emulator side)

* XMS `MoveExtendedMemoryBlock` (HIMEM) copies data correctly (dedicated COM
  test moves a pattern to an XMS block and back, buffers compare equal).
* Protected-mode `rep movsd` above 1 MB copies correctly
  (`mode_protected_rep_movsd_above_1mb`).
* `repe cmpsb` and `rep movsb` behave per specification.
* `LAR`/`LSL` and outer-ring `IRET` (IOPL/IF) were fixed; `FE /2../7` now
  raises `#UD`.
* A20-off alias (`0x00DE20`) does not contain the copy either.
* DOSX does **not** install an INT 21 hook in the IVT before the EXEC; the
  only IVT[0x21] write from its stub (`4DFA:0924`) restores the DOS vector
  during the fatal-error cleanup.
* The loader EXECs `C:\WININST0.400\krnl386.exe` (`INT 21 AX=4B00` from
  `4DFA:00EF`).  DOS loads it low (segment 0x387); the high block stays empty.
* The `INT 21 AX=3F` from `5EDB:1AD4` is a SYSTEM.INI read into the low
  bounce buffer `0x666F0`, not the kernel image.

## Where the missing copy must be

The block base `0x0040DE20` is installed by DOSX's internal DPMI Set Descriptor
(`0x37F:0x2B77`); it is not derived through the public INT 31 set-base path
(only selectors 0x277/0x33F get low bases through `INT 31 AX=0007/0008`, and
`INT 31 AX=000A` is used once for 0x307).  Therefore the transfer of the
real-mode segment into `0x0040DE20` is internal DOSX/KRNL386 loader logic.

The next step is to trace that internal loader (protected CS 0x53/0x307), find
the transfer and the condition that skips it.  No guest-state substitution is
acceptable as a fix.
