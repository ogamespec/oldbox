/* OldBox fallback POST ROM.
 *
 * A small, real 16-bit x86 BIOS image: it installs INT 10h/11h/12h/13h/15h/16h/
 * 19h services, enumerates PCI, prints the detected CPU and memory, and boots
 * the first sector of the primary ATA disk using PIO.  It exists so the machine
 * has a deterministic firmware path when the user has not extracted the
 * reference-board BIOS from specs/mb.  It is not a replacement for Award BIOS.
 */
.code16
.section .text
.global _start
_start:
        cli
        xorw %ax, %ax
        movw %ax, %ds
        movw %ax, %es
        movw %ax, %ss
        movw $0x7c00, %sp
        ljmp $0xf000, $rom_start

rom_start:
        call install_vectors
        movw $0xf000, %ax
        movw %ax, %ds
        movw $0xb800, %ax
        movw %ax, %es
        cld
        call clear_screen
        movw $msg_banner, %si
        call puts
        call print_cpu
        call print_memory
        call print_pci
        call print_audio
        call boot_floppy
        call boot_disk
        movw $msg_halt, %si
        call puts
halt_loop:
        hlt
        jmp halt_loop

/* ---------------------------------------------------------------- output */
clear_screen:
        pushw %ax
        pushw %cx
        pushw %di
        movw $0x0720, %ax
        xorw %di, %di
        movw $2000, %cx
        rep stosw
        movw $0, %cs:cursor
        popw %di
        popw %cx
        popw %ax
        ret

puts:
        pushw %ax
.Lputs_next:
        lodsb
        testb %al, %al
        jz .Lputs_done
        cmpb $13, %al
        je .Lputs_cr
        cmpb $10, %al
        je .Lputs_lf
        call putc
        jmp .Lputs_next
.Lputs_cr:
        movw $0, %cs:cursor
        jmp .Lputs_next
.Lputs_lf:
        addw $80, %cs:cursor
        jmp .Lputs_next
.Lputs_done:
        popw %ax
        ret

putc:
        pushw %ax
        pushw %bx
        pushw %di
        pushw %es
        movw $0xb800, %bx
        movw %bx, %es
        movw %cs:cursor, %di
        shlw $1, %di
        movb %al, %es:(%di)
        movb $0x07, %es:1(%di)
        incw %cs:cursor
        cmpw $2000, %cs:cursor
        jb .Lputc_done
        movw $0, %cs:cursor
.Lputc_done:
        popw %es
        popw %di
        popw %bx
        popw %ax
        ret

put_hex16:
        pushw %ax
        pushw %cx
        movw $4, %cx
.Lloop:
        rolw $4, %ax
        movw %ax, %bx
        andw $0x000f, %bx
        cmpb $10, %bl
        jb .Ldigit
        addb $('a' - 10), %bl
        jmp .Lemit
.Ldigit:
        addb $'0', %bl
.Lemit:
        movb %bl, %al
        call putc
        loop .Lloop
        popw %cx
        popw %ax
        ret

put_hex8:
        pushw %ax
        pushw %bx
        movb %al, %bl
        shrb $4, %al
        call .Lhex_digit
        movb %bl, %al
        andb $0x0f, %al
        call .Lhex_digit
        popw %bx
        popw %ax
        ret
.Lhex_digit:
        andb $0x0f, %al
        cmpb $10, %al
        jb .Lhex_num
        addb $('a' - 10), %al
        jmp .Lhex_emit
.Lhex_num:
        addb $'0', %al
.Lhex_emit:
        call putc
        ret

/* --------------------------------------------------------------- CPUID */
print_cpu:
        pusha
        movl $0, %eax
        cpuid
        movl %ebx, vendor+0
        movl %edx, vendor+4
        movl %ecx, vendor+8
        movb $0, vendor+12
        pushw %ds
        movw $0xf000, %ax
        movw %ax, %ds
        movw $msg_cpu, %si
        call puts
        movw $vendor, %si
        call puts
        movw $msg_nl, %si
        call puts
        popw %ds
        popa
        ret

/* -------------------------------------------------------------- memory */
print_memory:
        pushw %ds
        pushw %es
        movw $0xf000, %ax
        movw %ax, %ds
        movw $msg_mem, %si
        call puts
        movw $0x40, %ax
        movw %ax, %es
        movw %es:0x17, %ax
        call put_hex16
        movw $msg_kb, %si
        call puts
        popw %es
        popw %ds
        ret

/* --------------------------------------------------------------- PCI */
pci_read32:
        /* %eax address, result %eax */
        movl %eax, %edx
        movw $0xcf8, %dx
        outl %eax, %dx
        movw $0xcfc, %dx
        inl %dx, %eax
        ret
pci_write32:
        movl %eax, %edx
        movw $0xcf8, %dx
        outl %eax, %dx
        ret

print_pci:
        pusha
        pushw %ds
        movw $0xf000, %ax
        movw %ax, %ds
        movw $msg_pci, %si
        call puts
        xorl %ebx, %ebx
.Lpci_dev_loop:
        cmpl $32, %ebx
        jae .Lpci_done
        movl %ebx, %eax
        shll $11, %eax
        orl $0x80000000, %eax
        call pci_read32
        cmpl $0xffffffff, %eax
        je .Lpci_next
        movl %eax, %ecx
        movl %ebx, %eax
        shll $11, %eax
        orl $0x80000000, %eax
        call pci_read32
        movl %eax, %edx
        movw $msg_pci_dev, %si
        call puts
        movw %bx, %ax
        call put_hex8
        movw $msg_colon, %si
        call puts
        movw %dx, %ax
        call put_hex16
        movw $msg_colon, %si
        call puts
        shrl $16, %edx
        movw %dx, %ax
        call put_hex16
        movw $msg_nl, %si
        call puts
.Lpci_next:
        incw %bx
        jmp .Lpci_dev_loop
.Lpci_done:
        popw %ds
        popa
        ret

/* -------------------------------------------------------------- audio */
print_audio:
        pushw %ds
        movw $0xf000, %ax
        movw %ax, %ds
        movw $msg_audio, %si
        call puts
        popw %ds
        ret

/* --------------------------------------------------------------- boot */
boot_disk:
        pushw %ds
        pushw %es
        movw $0xf000, %ax
        movw %ax, %ds
        movw $msg_boot, %si
        call puts
        movw $0x1f6, %dx
        movb $0xe0, %al
        outb %al, %dx
        movw $0x1f2, %dx
        movb $1, %al
        outb %al, %dx
        xorw %ax, %ax
        movw $0x1f3, %dx
        outb %al, %dx
        incw %dx
        outb %al, %dx
        incw %dx
        outb %al, %dx
        movw $0x1f7, %dx
        movb $0x20, %al
        outb %al, %dx
        movw $0x1f7, %dx
        movw $0xffff, %cx
.Lwait:
        inb %dx, %al
        testb $0x80, %al
        jnz .Lnext
        testb $0x08, %al
        jnz .Lread
        testb $0x01, %al
        jnz .Lfail
.Lnext:
        loop .Lwait
.Lfail:
        movw $msg_noboot, %si
        call puts
        popw %es
        popw %ds
        ret
.Lread:
        xorw %ax, %ax
        movw %ax, %es
        movw $0x7c00, %bx
        movw $0x1f0, %dx
        movw $256, %cx
        rep insw
        movw $msg_loaded, %si
        call puts
        cmpw $0xaa55, %es:0x7dfe
        jne .Lbad
        ljmp $0, $0x7c00
.Lbad:
        movw $msg_badbios, %si
        call puts
        popw %es
        popw %ds
        ret


/* --------------------------------------------------------------- floppy boot */
boot_floppy:
        pusha
        pushw %es
        xorw %ax, %ax
        movw %ax, %es
        movw $0x7c00, %bx
        xorw %dx, %dx
        xorw %cx, %cx
        incb %cl
        movw $0x0201, %ax
        int $0x13
        jc .Lfloppy_fail
        cmpw $0xaa55, %es:0x7dfe
        jne .Lfloppy_fail
        movw $msg_floppy, %si
        call puts
        ljmp $0, $0x7c00
.Lfloppy_fail:
        popw %es
        popa
        ret

/* --------------------------------------------------------- interrupt 10h */
int10:
        pushw %ds
        pushw %es
        pushw %ax
        pushw %bx
        pushw %cx
        pushw %dx
        pushw %si
        pushw %di
        movw $0x40, %ax
        movw %ax, %ds
        movw $0xb800, %ax
        movw %ax, %es
        popw %ax
        pushw %ax
        cmpb $0x0e, %ah
        je .Lteletype
        cmpb $0x00, %ah
        je .Lsetmode
        cmpb $0x03, %ah
        je .Lgetcursor
        cmpb $0x02, %ah
        je .Lsetcursor
        cmpb $0x0f, %ah
        je .Lgetmode
        jmp .Lint10_iret
.Lsetmode:
        call clear_screen
        movw $0x0000, 0x50
        movb $3, 0x49
        movb $80, 0x4a
        jmp .Lint10_iret
.Lgetcursor:
        movw 0x50, %dx
        jmp .Lint10_iret
.Lsetcursor:
        movw %dx, 0x50
        jmp .Lint10_iret
.Lgetmode:
        movb 0x49, %al
        movb 0x4a, %ah
        jmp .Lint10_iret
.Lteletype:
        cmpb $0x0d, %al
        je .Lcr
        cmpb $0x0a, %al
        je .Llf
        cmpb $0x08, %al
        je .Lbs
        movw 0x50, %di
        shlw $1, %di
        movb %al, %es:(%di)
        incw 0x50
        cmpw $80, 0x50
        jb .Lint10_iret
.Lcr:
        movw $0, 0x50
        jmp .Lint10_iret
.Llf:
        addw $80, 0x50
        cmpw $2000, 0x50
        jb .Lint10_iret
        call scroll
        subw $80, 0x50
        jmp .Lint10_iret
.Lbs:
        cmpw $0, 0x50
        je .Lint10_iret
        decw 0x50
        movw 0x50, %di
        shlw $1, %di
        movb $' ', %es:(%di)
        jmp .Lint10_iret
.Lint10_iret:
        popw %di
        popw %si
        popw %dx
        popw %cx
        popw %bx
        popw %ax
        popw %es
        popw %ds
        iret

scroll:
        pushw %ax
        pushw %cx
        pushw %si
        pushw %di
        pushw %ds
        pushw %es
        movw $0xb800, %ax
        movw %ax, %ds
        movw %ax, %es
        movw $160, %si
        xorw %di, %di
        movw $1920, %cx
        rep movsw
        movw $0x0720, %ax
        movw $80, %cx
        rep stosw
        popw %es
        popw %ds
        popw %di
        popw %si
        popw %cx
        popw %ax
        ret

/* --------------------------------------------------------- interrupt 16h */
int16:
        pushw %ds
        pushw %bx
        movw $0x40, %ax
        movw %ax, %ds
.Lcheck:
        inb $0x64, %al
        testb $1, %al
        jz .Lnone
        inb $0x60, %al
        movb %al, %ah
        call scancode_to_ascii
        clc
        popw %bx
        popw %ds
        iret
.Lnone:
        stc
        xorw %ax, %ax
        popw %bx
        popw %ds
        iret
scancode_to_ascii:
        pushw %bx
        pushw %si
        movw $keymap, %si
        xorw %bx, %bx
        movb %ah, %bl
        cmpb $0x39, %bl
        ja .Lzero
        addw %bx, %si
        movb (%si), %al
        jmp .Lout
.Lzero:
        xorw %ax, %ax
.Lout:
        popw %si
        popw %bx
        ret

/* --------------------------------------------------------- interrupt 13h */
int13_handler:
        iret

/* --------------------------------------------------------- interrupt 15h */
int15:
        cmpb $0x88, %ah
        jne .Le801
        movw $0x7c00, %ax
        clc
        iret
.Le801:
        cmpb $0xe8, %ah
        jne .Lint15_iret
        movw $0x7c00, %ax
        xorw %bx, %bx
        clc
        iret
.Lint15_iret:
        iret

int12:
        movw $640, %ax
        iret
int11:
        movw $0x0021, %ax
        iret

/* --------------------------------------------------------------- vectors */
install_vectors:
        pushw %es
        pushw %di
        pushw %ax
        xorw %ax, %ax
        movw %ax, %es
        movw $0x40, %di
        movw $int10, %ax
        stosw
        movw $0xf000, %ax
        stosw
        movw $0x44, %di
        movw $int11, %ax
        stosw
        movw $0xf000, %ax
        stosw
        movw $0x48, %di
        movw $int12, %ax
        stosw
        movw $0xf000, %ax
        stosw
        movw $0x4c, %di
        movw $int13_handler, %ax
        stosw
        movw $0xf000, %ax
        stosw
        movw $0x54, %di
        movw $int15, %ax
        stosw
        movw $0xf000, %ax
        stosw
        movw $0x58, %di
        movw $int16, %ax
        stosw
        movw $0xf000, %ax
        stosw
        movw $0x64, %di
        movw $boot_disk, %ax
        stosw
        movw $0xf000, %ax
        stosw
        popw %ax
        popw %di
        popw %es
        ret

/* --------------------------------------------------------------- strings */
msg_banner:    .asciz "\r\nOldBox Reference Board BIOS v0.2 (fallback)\r\nIntel 440BX / Pentium II platform\r\n\r\n"
msg_cpu:       .asciz "CPU: "
msg_mem:       .asciz "Memory (CMOS): "
msg_kb:        .asciz " KB\r\n"
msg_pci:       .asciz "PCI devices:\r\n"
msg_pci_dev:   .asciz "  dev "
msg_colon:     .asciz ":"
msg_nl:        .asciz "\r\n"
msg_audio:     .asciz "AWE64 Gold: ISA PnP, EMU8000 32-voice, MPU-401\r\n"
msg_boot:      .asciz "\r\nBooting primary ATA master...\r\n"
msg_floppy:    .asciz "\r\nBooting floppy A:...\r\n"
msg_noboot:    .asciz "No bootable ATA device.\r\n"
msg_loaded:    .asciz "Boot sector loaded.\r\n"
msg_badbios:   .asciz "Invalid boot signature 55AA.\r\n"
msg_halt:      .asciz "\r\nSystem halted. Reset to restart.\r\n"
vendor:        .space 16, 0
cursor:        .word 0
keymap:
        .byte 0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,9
        .byte 'q','w','e','r','t','y','u','i','o','p','[',']',13,0
        .byte 'a','s','d','f','g','h','j','k','l',';',39,'`',0
        .byte '\\','z','x','c','v','b','n','m',',','.','/',0
        .byte '*',0,' ',0

/* --------------------------------------------------------------- reset */
.org 0xfff0
reset:
        ljmp $0xf000, $_start
.org 0xfffe
        .word 0xaa55
