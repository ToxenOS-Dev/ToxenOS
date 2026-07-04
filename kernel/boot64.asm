bits 32

; ToxenOS x86_64 long-mode boot stub — Milestone 1.
;
; GRUB always drops the CPU into 32-bit protected mode regardless of the
; target kernel's bit width (multiboot2 has no "boot straight into long
; mode" option), so this file starts in bits 32 exactly like kernel/boot.asm
; and is responsible for the entire 32-bit -> long-mode transition itself.
;
; Memory layout (mirrors kernel/boot.asm's scheme, new high-half base):
;   0x100000 : .boot     (multiboot headers + GDT64 + entry code) [LOADED]
;   0x101000 : .bootdata (PML4/PDPT/PD page tables — zeroed at runtime) [NOLOAD]
;   0x200000 : .text     (kernel code, linked at 0xFFFFFFFF80200000) [LOADED]
;
; start (bits 32) builds 4-level page tables, enables PAE + long mode,
; loads a minimal 64-bit GDT, and far-jumps into high_entry64 (bits 64).
;
; IMPORTANT (Milestone 1 scope): the low identity map is INTENTIONALLY
; kept resident — it is not removed the way kernel/boot.asm removes its
; identity map once the high half is active. kernel64.c writes directly
; to VGA memory at 0xB8000 and reads the multiboot info block via a low
; physical pointer, and there is no IDT yet, so any stray access through
; an unmapped page would triple-fault with zero diagnostic output.
; Removing the identity map is deferred to a later milestone once early
; hardware/debug access has a proper virtual mapping.

; ── Multiboot headers ─────────────────────────────────────────────────────────
section .multiboot2
align 8
    dd 0xE85250D6
    dd 0
    dd (mb2_end - mb2_start)
    dd -(0xE85250D6 + 0 + (mb2_end - mb2_start))
mb2_start:
    ; Framebuffer request tag (type 5): ask GRUB for 1024x768x32 direct-color
    ; mode. GRUB will try to honor this and pass the actual framebuffer info
    ; back via a type-8 tag in the multiboot2 info struct. If GRUB cannot
    ; satisfy the request, it falls back to whatever mode it chose.
    align 8
    dw 5                    ; type: framebuffer
    dw 0                    ; flags: non-optional
    dd 20                   ; size (8-byte header + 12 bytes of data)
    dd 1024                 ; preferred width
    dd 768                  ; preferred height
    dd 32                   ; preferred depth (bits per pixel)
    ; End tag
    align 8
    dw 0                    ; end tag
    dw 0
    dd 8
mb2_end:

section .multiboot
align 4
    dd 0x1BADB002
    dd 0
    dd -(0x1BADB002)

; ── .boot: entry code + 64-bit GDT (LOADED by GRUB) ──────────────────────────
; VA == PA for this section (no KERNEL_VIRT_BASE64 offset) — identical
; convention to kernel/boot.asm's .boot section.
section .boot exec alloc

; Exported so kernel/tss64.c can write the TSS descriptor's two qwords
; (.tss_lo/.tss_hi below) at runtime, once the TSS struct's link-time
; address is known -- this file is assembled before that address exists,
; so the GDT slot is reserved here but filled in later, not statically.
global gdt64

align 8
gdt64:
    dq 0x0000000000000000      ; 0x00 null descriptor
.code: equ $ - gdt64
    dq 0x00209A0000000000      ; 0x08 kernel code64: P=1,S=1,Type=Exec/Read,DPL=0,L=1,D=0
.data: equ $ - gdt64
    dq 0x0000920000000000      ; 0x10 kernel data: P=1,S=1,Type=Read/Write,DPL=0
.ucode: equ $ - gdt64
    dq 0x0020FA0000000000      ; 0x18 user code64: same as kernel code, DPL=3 (access=0xFA)
.udata: equ $ - gdt64
    dq 0x0000F20000000000      ; 0x20 user data: same as kernel data, DPL=3 (access=0xF2)
.tss_lo: equ $ - gdt64
    dq 0x0000000000000000      ; 0x28 TSS descriptor, low qword (filled at runtime)
.tss_hi: equ $ - gdt64
    dq 0x0000000000000000      ; 0x30 TSS descriptor, high qword (base bits 63:32)
gdt64_end:

; Milestone 3B selectors. Must stay in sync with include/gdt64.h (NASM
; can't include that C header directly) and with the user-selector
; copies in kernel/ring3_test64.asm.
CODE64_SEL      equ 0x08
DATA64_SEL      equ 0x10
USER_CODE64_SEL equ 0x18
USER_DATA64_SEL equ 0x20
TSS64_SEL       equ 0x28

; GDT descriptor — physical address. gdt64 lives in .boot (identity-mapped
; low memory) so this stays valid even after CR0.PG is set, since the
; identity map is kept resident for this milestone (see header comment).
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

; ── Error path: CPU lacks long-mode support ──────────────────────────────────
; Runs before paging is enabled, so this writes straight to physical VGA
; memory — no virtual-memory machinery exists yet at this point.
msg_no_long_mode: db "ToxenOS64: CPU does not support long mode", 0

print_no_long_mode:
    mov edi, 0xB8000
    mov esi, msg_no_long_mode
.loop:
    mov al, [esi]
    test al, al
    jz .halt
    mov [edi], al
    mov byte [edi+1], 0x4F     ; white on red
    add edi, 2
    inc esi
    jmp .loop
.halt:
    cli
.hang:
    hlt
    jmp .hang

; ── .bootdata: PML4/PDPT/PD page tables (NOLOAD — zeroed by start) ───────────
; One PD page is shared by both the low identity map (via pdpt_low[0]) and
; the high-half map (via pdpt_high[510]) — both paths lead to the same
; physical frames, so there is no need for two separate PD tables.
section .bootdata
align 4096
; pdpt_high/pd exported for kernel/ring3_test64.c, which carves one PD
; entry's flat 2MB leaf into a 4KB page table for the hardcoded ring3
; stub and needs to flip the PAGE_USER bit on the high-half walk above
; it (pml4[511]/pdpt_high[510]) without disturbing the low identity alias.
global pml4
global pdpt_low
global pdpt_high
global pd
pml4:      times 512 dq 0
pdpt_low:  times 512 dq 0
pdpt_high: times 512 dq 0
pd:        times 512 dq 0

; ── Entry point (in .boot) ────────────────────────────────────────────────────
section .boot exec alloc
global start
extern stack_top64

PAGE_PRESENT  equ 0x001
PAGE_WRITABLE equ 0x002
PAGE_SIZE_2M  equ 0x080

; PML4[511] / PDPT_high[510] are the fixed tree indices for the canonical
; higher-half base 0xFFFFFFFF80000000 (top 2GB of the 64-bit address
; space — the region -mcmodel=kernel and linker64.ld both assume).
PML4_HIGH_IDX  equ 511
PDPT_HIGH_IDX  equ 510
; Number of 2MB PD entries to map (64MB window — same size as the 32-bit
; kernel's existing identity+high-half mapping in kernel/boot.asm).
MAP_PD_ENTRIES equ 32

start:
    cli
    mov esp, 0x90000        ; temp low stack (safe low memory)

    ; Stash multiboot magic (eax) / info pointer (ebx) on the stack —
    ; everything between here and the far jump (cpuid, the page-table
    ; zeroing loop's `rep stosd`, etc.) clobbers eax/ebx/ecx/edx/edi
    ; freely, so the only clobber-proof place to keep these is memory.
    ; They get popped back into edi/esi right before the far jump below,
    ; with nothing in between that touches either register, so the lower
    ; 32 bits of rdi/rsi will still hold them after the mode switch
    ; (32-bit writes zero-extend into the full 64-bit register).
    push eax                ; magic
    push ebx                ; mb_info_addr

    ; ── Confirm long-mode support before touching anything else ─────────────
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb print_no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29        ; EDX.LM
    jz print_no_long_mode

    ; ── Zero all four page-table pages (4 x 4KB = 16KB = 4096 dwords) ───────
    cld
    mov edi, pml4
    xor eax, eax
    mov ecx, 4096
    rep stosd

    ; ── Build the PD: 2MB pages covering the low MAP_PD_ENTRIES*2MB window ──
    xor ecx, ecx
.fill_pd:
    mov eax, ecx
    shl eax, 21                                       ; PA = ecx * 2MB
    or  eax, PAGE_PRESENT | PAGE_WRITABLE | PAGE_SIZE_2M
    mov [pd + ecx*8], eax
    mov dword [pd + ecx*8 + 4], 0
    inc ecx
    cmp ecx, MAP_PD_ENTRIES
    jl .fill_pd

    ; ── Point both pdpt_low[0] and pdpt_high[PDPT_HIGH_IDX] at the same PD ──
    mov eax, pd
    or  eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [pdpt_low], eax
    mov dword [pdpt_low + 4], 0
    mov [pdpt_high + PDPT_HIGH_IDX*8], eax
    mov dword [pdpt_high + PDPT_HIGH_IDX*8 + 4], 0

    ; ── PML4[0] -> pdpt_low (identity), PML4[511] -> pdpt_high (high-half) ──
    mov eax, pdpt_low
    or  eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [pml4], eax
    mov dword [pml4 + 4], 0

    mov eax, pdpt_high
    or  eax, PAGE_PRESENT | PAGE_WRITABLE
    mov [pml4 + PML4_HIGH_IDX*8], eax
    mov dword [pml4 + PML4_HIGH_IDX*8 + 4], 0

    ; ── Enable PAE (mandatory prerequisite for long mode) ────────────────────
    mov eax, cr4
    or  eax, 0x20            ; CR4.PAE = bit 5
    mov cr4, eax

    ; ── CR3 = PML4's PHYSICAL address. Paging is still off here, so this
    ; symbol's link-time address (in .bootdata, VA==PA, no high-half
    ; offset) IS its physical address — exactly like kernel/boot.asm's
    ; boot_pgdir. This must never be the high-half virtual alias; that
    ; mapping doesn't exist until after CR0.PG is set below. ───────────────
    mov eax, pml4
    mov cr3, eax

    ; ── Set EFER.LME — enables long mode (no 32-bit analog) ─────────────────
    mov ecx, 0xC0000080       ; IA32_EFER
    rdmsr
    or  eax, 1 << 8           ; EFER.LME
    wrmsr

    ; ── Enable paging. CPU is now in 32-bit compatibility submode of long
    ; mode: IA-32e paging is active, but CS still points at a 32-bit code
    ; segment, so 32-bit instructions keep executing until the far jump
    ; below loads a code segment with the L-bit set. ────────────────────────
    mov eax, cr0
    or  eax, 0x80000000       ; CR0.PG
    mov cr0, eax

    lgdt [gdt64_ptr]

    ; Restore magic/mb_info_addr into edi/esi right before the mode
    ; switch — popped in reverse push order. Nothing after this touches
    ; either register, so they survive into rdi/rsi across the jump.
    pop ebx                  ; mb_info_addr
    pop eax                  ; magic
    mov esi, ebx
    mov edi, eax

    ; Far jump: this is the actual bit-width switch (changes CS to the
    ; L-bit descriptor), as opposed to kernel/boot.asm's near `jmp eax`
    ; which only changes the virtual address while staying 32-bit.
    ;
    ; The target must be trampoline64 (below, still in .boot/low memory),
    ; NOT high_entry64 directly: a far jmp's offset operand in 32-bit
    ; code is only a 32-bit immediate, and high_entry64's real address
    ; (0xFFFFFFFF80200000+) doesn't fit in — and isn't reachable through
    ; our identity map even if truncated to 32 bits. trampoline64's
    ; low/identity-mapped address fits the 32-bit far-jump operand; once
    ; genuinely executing in 64-bit mode there, it finishes the trip with
    ; a register-indirect jump that supports a full 64-bit target.
    jmp CODE64_SEL:trampoline64

[bits 64]
trampoline64:
    mov rax, high_entry64
    jmp rax

; ── high_entry64 (in .text at virtual 0xFFFFFFFF80200000) ────────────────────
section .text
global high_entry64
extern kernel_main64

high_entry64:
    ; Now executing 64-bit code. Identity map is still active (by design,
    ; see the header comment), and the high-half map covers this address.
    mov ax, DATA64_SEL
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Set up a real, ABI-aligned 64-bit stack. The System V AMD64 ABI
    ; requires rsp % 16 == 0 at a `call` instruction; stack_top64 is
    ; already 16-byte aligned via linker64.ld, but `and rsp, -16` makes
    ; the alignment an explicit guarantee rather than an assumption.
    mov rsp, stack_top64
    and rsp, -16

    ; kernel_main64(magic, mb_info_addr) — System V AMD64 ABI passes the
    ; first two integer args in rdi/rsi, not on the stack (unlike the
    ; cdecl `push`-based convention kernel/boot.asm uses for kernel_main).
    call kernel_main64

.hang:
    hlt
    jmp .hang
