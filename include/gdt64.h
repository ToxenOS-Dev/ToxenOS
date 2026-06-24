#ifndef GDT64_H
#define GDT64_H

// 64-bit GDT selector constants. Must stay in sync with the `equ`
// constants of the same name in kernel/boot64.asm (NASM can't include
// this C header directly, so the values are duplicated there with a
// cross-reference comment) and with kernel/ring3_test64.asm's own
// copies of the user selectors.
#define CODE64_SEL      0x08
#define DATA64_SEL      0x10
#define USER_CODE64_SEL 0x18   // RPL=3 when placed in an iretq frame: |3
#define USER_DATA64_SEL 0x20   // RPL=3 when placed in an iretq frame: |3
#define TSS64_SEL       0x28

#endif // GDT64_H
