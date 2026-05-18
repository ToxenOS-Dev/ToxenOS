bits 32

; Higher-half kernel boot stub
;
; Memory layout after GRUB loads us:
;   0x100000: .boot section  (multiboot headers + GDT + entry code) [LOADED]
;   0x101000: .bootdata      (page tables — zeroed by start at runtime) [NOLOAD]
;   0x200000: .text          (kernel code, linked at 0xC0200000) [LOADED]
;
; GRUB jumps to start (physical 0x100xxx).
; start fills the page tables, enables paging, jumps to high_entry.
; high_entry (in .text at 0xC0200000) runs in the high half.

KERNEL_VIRT_BASE equ 0xC0000000
PAGE_PRESENT     equ 0x001
PAGE_WRITABLE    equ 0x002

; ── Multiboot headers ─────────────────────────────────────────────────────────
section .multiboot2
align 8
    dd 0xE85250D6
    dd 0
    dd (mb2_end - mb2_start)
    dd -(0xE85250D6 + 0 + (mb2_end - mb2_start))
mb2_start:
    align 8
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32
    align 8
    dw 0
    dw 0
    dd 8
mb2_end:

section .multiboot
align 4
    dd 0x1BADB002
    dd 0
    dd -(0x1BADB002)

; ── .boot: entry code + GDT (LOADED by GRUB) ─────────────────────────────────
; Everything here is at a physical address that GRUB writes into RAM.
; VA == PA for this section (no KERNEL_VIRT_BASE offset).
section .boot exec alloc

; GDT — must be here (not in .bootdata) so GRUB loads the actual bytes
align 8
gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF   ; 0x08 kernel code
    dq 0x00CF92000000FFFF   ; 0x10 kernel data
    dq 0x00CFFA000000FFFF   ; 0x18 user code
    dq 0x00CFF2000000FFFF   ; 0x20 user data
    dq 0x0000000000000000   ; 0x28 TSS placeholder
    dq 0x0000000000000000   ; 0x30 TSS high
gdt_end:

; GDT descriptor — physical address, used from start() before paging
gdt_ptr_phys:
    dw gdt_end - gdt - 1
    dd gdt                  ; physical address of GDT

; GDT descriptor — for use in high_entry after identity map removed
; The GDT is in .boot (first 4MB), still accessible via boot_pgtab_hi
; at virtual address = physical + KERNEL_VIRT_BASE
gdt_ptr_virt:
    dw gdt_end - gdt - 1
    dd gdt + KERNEL_VIRT_BASE

global tss_entry
tss_entry:
    dd gdt + 40 + KERNEL_VIRT_BASE

global gdt_base
gdt_base:
    dd gdt + KERNEL_VIRT_BASE

; ── .bootdata: page tables (NOLOAD — zeroed by start at runtime) ─────────────
section .bootdata
align 4096
global boot_pgdir
boot_pgdir:     times 1024 dd 0

align 4096
global boot_pgtab_lo
boot_pgtab_lo:  times 1024 dd 0

align 4096
global boot_pgtab_hi
boot_pgtab_hi:  times 1024 dd 0

align 4096
global boot_pgtab_hi2
boot_pgtab_hi2: times 1024 dd 0

; ── Entry point (in .boot) ────────────────────────────────────────────────────
section .boot exec alloc
global start
extern stack_top

start:
    ; eax = multiboot magic, ebx = multiboot info (physical)
    mov esp, 0x7C000        ; temp stack (safe low memory)
    push eax                ; save magic
    push ebx                ; save mb_info_addr

    ; Zero the page tables (NOLOAD section — may contain garbage)
    cld
    mov edi, boot_pgdir
    xor eax, eax
    mov ecx, (4 * 1024)     ; 4 pages × 1024 dwords (pgdir + lo + hi + hi2)
    rep stosd

    pop esi                 ; mb_info_addr
    pop edi                 ; magic

    ; Fill boot_pgtab_lo: identity map VA 0x000xxxxx -> PA 0x000xxxxx
    mov edx, boot_pgtab_lo
    xor ecx, ecx
.fill_lo:
    mov eax, ecx
    shl eax, 12
    or  eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [edx + ecx*4], eax
    inc ecx
    cmp ecx, 1024
    jl  .fill_lo

    ; Fill boot_pgtab_hi: map PA 0x000xxxxx -> VA 0xC00xxxxx (first 4MB)
    mov edx, boot_pgtab_hi
    xor ecx, ecx
.fill_hi:
    mov eax, ecx
    shl eax, 12
    or  eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [edx + ecx*4], eax
    inc ecx
    cmp ecx, 1024
    jl  .fill_hi

    ; Fill boot_pgtab_hi2: map PA 0x400xxxxx -> VA 0xC04xxxxx (next 4MB)
    mov edx, boot_pgtab_hi2
    xor ecx, ecx
.fill_hi2:
    mov eax, ecx
    add eax, 1024               ; start at physical page 1024 = PA 0x400000
    shl eax, 12
    or  eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [edx + ecx*4], eax
    inc ecx
    cmp ecx, 1024
    jl  .fill_hi2

    ; Install tables into directory
    mov eax, boot_pgtab_lo
    or  eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pgdir + 0*4], eax         ; entry 0: identity (PA 0–4MB)

    mov eax, boot_pgtab_hi
    or  eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pgdir + 768*4], eax       ; entry 768: VA 0xC0000000 (PA 0–4MB)

    mov eax, boot_pgtab_hi2
    or  eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pgdir + 769*4], eax       ; entry 769: VA 0xC0400000 (PA 4–8MB)

    ; Enable paging
    mov eax, boot_pgdir
    mov cr3, eax
    mov eax, cr0
    or  eax, 0x80000000
    mov cr0, eax

    ; Jump to high_entry at its HIGH HALF virtual address.
    ; The identity map keeps us running until the far jump lands there.
    mov eax, high_entry
    jmp eax

; ── high_entry (in .text at virtual 0xC0200000) ───────────────────────────────
section .text
global high_entry
extern kernel_main

high_entry:
    ; Now executing at virtual 0xC0200000+.
    ; Identity map is still active.

    ; Set up real kernel stack
    mov esp, stack_top

    ; Load GDT — gdt_ptr_virt is in .boot (low section, still identity-mapped)
    ; gdt_ptr_virt.base = gdt + KERNEL_VIRT_BASE so the GDT base is correct
    lgdt [gdt_ptr_virt]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.cs_flush
.cs_flush:

    ; Remove identity map now that we are fully in the high half
    ; Access boot_pgdir via its high-half virtual address
    mov dword [boot_pgdir + KERNEL_VIRT_BASE], 0
    mov eax, cr3
    mov cr3, eax            ; TLB flush

    ; kernel_main(magic, mb_info_addr)
    push esi
    push edi
    call kernel_main
    jmp $
