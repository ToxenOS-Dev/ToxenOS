#ifndef RING3_TEST64_H
#define RING3_TEST64_H

// Milestone 3B: hardcoded ring3 smoke test. Maps one shared read-only
// executable page (a 4-byte "ud2; jmp $" stub) and one user stack page,
// both carved out of pd[]'s otherwise-unused tail (see ring3_test64.c),
// then iretq's into ring3. The stub immediately raises #UD, caught by
// the existing ISR64/IDT64 path -- the only way control ever returns
// to the kernel. Not real userland: no syscalls, no NEX64/ELF64, no
// per-process address space.
void ring3_test64_start(void);

#endif // RING3_TEST64_H
