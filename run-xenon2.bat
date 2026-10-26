set "EXE=build\vs2026\Release\oldbox.exe"
set "BIOS=assets\bios\EP-ZXA.bin"
set "VGABIOS=assets\vga\S3Trio64_1_14-04.bin"
set "FLOPPY=assets\floppy\freedos1440.img"
set "GAMEFLOPPY=assets\floppy\xenon2.ima"

"%EXE%" --bios "%BIOS%" --vga-bios "%VGABIOS%" --floppy-b "%GAMEFLOPPY%" --floppy "%FLOPPY%"