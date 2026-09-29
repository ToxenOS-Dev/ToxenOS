#ifndef VECTOR64_H
#define VECTOR64_H

// M+11A: CPU interrupt-vector allocator. Vector ownership is an explicit
// per-vector STATE table (never "a numeric range assumed free"): every
// one of the 256 vectors is exactly one of the classes below, built from
// an explicit reservation table at init, so a reserved vector (notably
// the 0x80 syscall gate sitting in the middle of the dynamic range) can
// never be returned by the allocator. Pure logic, no hardware access --
// instantiable (vector64_map_t) so the self-tests exercise fresh maps.
//
//   0x00-0x1F  CPU exceptions                    VEC_EXC
//   0x20-0x2F  fixed legacy ISA vectors          VEC_LEGACY_FREE / VEC_LEGACY_OWNED
//              (claimable ONLY via claim_legacy: vector = 0x20 + ISA IRQ)
//   0x30-0x7F  dynamically allocatable           VEC_FREE / VEC_ALLOCATED
//   0x80       syscall gate                      VEC_SYSCALL
//   0x81-0xEE  dynamically allocatable           VEC_FREE / VEC_ALLOCATED
//   0xEF-0xFE  future system/IPI vectors         VEC_SYSTEM
//   0xFF       LAPIC spurious                    VEC_SPURIOUS
//
// 0x30..0xEE is 191 vectors; minus 0x80 leaves 190 dynamic vectors.
#include <stdint.h>

#define VECTOR64_COUNT          256
#define VECTOR64_LEGACY_BASE    0x20
#define VECTOR64_LEGACY_COUNT   16
#define VECTOR64_SYSCALL        0x80
#define VECTOR64_DYN_FIRST      0x30
#define VECTOR64_DYN_LAST       0xEE
#define VECTOR64_SYSTEM_FIRST   0xEF
#define VECTOR64_SYSTEM_LAST    0xFE
#define VECTOR64_SPURIOUS       0xFF
#define VECTOR64_DYN_EXPECTED   190

typedef enum {
    VEC_FREE = 0,
    VEC_EXC,
    VEC_LEGACY_FREE,
    VEC_LEGACY_OWNED,
    VEC_SYSCALL,
    VEC_SYSTEM,
    VEC_SPURIOUS,
    VEC_ALLOCATED,
} vector64_state_t;

#define VECTOR64_NO_OWNER (-1)

typedef struct {
    uint8_t  state[VECTOR64_COUNT];
    int16_t  owner[VECTOR64_COUNT];
    uint32_t bad_free_count;   // free of a non-ALLOCATED vector (double free / reserved)
    uint32_t dup_claim_count;  // claim of an already-owned legacy vector / alloc collision
    uint32_t exhausted_count;  // alloc attempts that found nothing free
} vector64_map_t;

void irq_vector_map_init(vector64_map_t* m);

// M+11B: true exactly for the dynamic device-vector pool (the 190 vectors
// the allocator can hand out), derived from the SAME reservation table the
// allocator uses -- so it is false for exceptions, the fixed legacy
// vectors, 0x80 (syscall), the system/IPI range and 0xFF. MSI code uses
// this instead of duplicating any range.
int vector64_is_dynamic_device_vector(int vec);

// Lowest FREE vector, or -1 when none are left. Never returns a
// reserved or legacy vector. `owner` is the logical IRQ that owns it.
int  irq_vector_alloc(vector64_map_t* m, int owner);
// Frees an ALLOCATED vector. Returns 0, or -1 (and counts) for a
// double free, a reserved vector, or a legacy vector.
int  irq_vector_free(vector64_map_t* m, int vec);

// Fixed legacy vector for ISA IRQ 0-15 (vector = 0x20 + isa_irq).
// Returns the vector, or -1 if isa_irq is out of range or that legacy
// vector is already owned.
int  irq_vector_claim_legacy(vector64_map_t* m, int isa_irq, int owner);
int  irq_vector_release_legacy(vector64_map_t* m, int vec);

vector64_state_t irq_vector_state(const vector64_map_t* m, int vec);
// Owning logical IRQ, or VECTOR64_NO_OWNER.
int  irq_vector_owner(const vector64_map_t* m, int vec);
// Number of vectors currently FREE (dynamic pool only).
int  irq_vector_free_count(const vector64_map_t* m);

#endif // VECTOR64_H
