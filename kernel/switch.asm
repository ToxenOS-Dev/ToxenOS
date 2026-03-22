global context_switch

section .text

context_switch:
    push ebx
    push esi
    push edi
    push ebp

    mov eax, [esp + 20]
    mov [eax], esp

    mov eax, [esp + 24]
    mov esp, [eax]

    pop ebp
    pop edi
    pop esi
    pop ebx

    ret
