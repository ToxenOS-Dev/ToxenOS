global isr_timer
global isr_keyboard

extern timer_handler
extern keyboard_handler

section .text

isr_timer:
    pusha
    call timer_handler
    mov al, 0x20
    out 0x20, al
    popa
    iret

isr_keyboard:
    pusha
    call keyboard_handler
    mov al, 0x20
    out 0x20, al
    popa
    iret
