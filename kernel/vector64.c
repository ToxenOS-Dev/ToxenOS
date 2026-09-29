// kernel/vector64.c -- M+11A CPU vector allocator (see include/vector64.h).
#include "../include/vector64.h"

typedef struct { uint8_t first, last; vector64_state_t state; } vec_reservation_t;

// The explicit reservation table. Everything not listed here starts
// VEC_FREE; the dynamic pool is therefore exactly "not reserved", with
// 0x80 reserved by name rather than by omission from a range.
static const vec_reservation_t g_reservations[] = {
    { 0x00, 0x1F, VEC_EXC },
    { 0x20, 0x2F, VEC_LEGACY_FREE },
    { VECTOR64_SYSCALL, VECTOR64_SYSCALL, VEC_SYSCALL },
    { VECTOR64_SYSTEM_FIRST, VECTOR64_SYSTEM_LAST, VEC_SYSTEM },
    { VECTOR64_SPURIOUS, VECTOR64_SPURIOUS, VEC_SPURIOUS },
};

// The single source of truth for "is this vector reserved, and as what":
// VEC_FREE means "not covered by any reservation", i.e. part of the dynamic
// device-vector pool.
static vector64_state_t reservation_class(int v) {
    vector64_state_t st = VEC_FREE;
    for (unsigned i = 0; i < sizeof(g_reservations) / sizeof(g_reservations[0]); i++)
        if (v >= g_reservations[i].first && v <= g_reservations[i].last) st = g_reservations[i].state;
    return st;
}

int vector64_is_dynamic_device_vector(int v) {
    if (v < 0 || v >= VECTOR64_COUNT) return 0;
    return reservation_class(v) == VEC_FREE;
}

void irq_vector_map_init(vector64_map_t* m) {
    for (int v = 0; v < VECTOR64_COUNT; v++) { m->state[v] = (uint8_t)reservation_class(v); m->owner[v] = VECTOR64_NO_OWNER; }
    m->bad_free_count = m->dup_claim_count = m->exhausted_count = 0;
}

int irq_vector_alloc(vector64_map_t* m, int owner) {
    for (int v = 0; v < VECTOR64_COUNT; v++) {
        if (m->state[v] == VEC_FREE) {
            m->state[v] = VEC_ALLOCATED;
            m->owner[v] = (int16_t)owner;
            return v;
        }
    }
    m->exhausted_count++;
    return -1;
}

int irq_vector_free(vector64_map_t* m, int vec) {
    if (vec < 0 || vec >= VECTOR64_COUNT || m->state[vec] != VEC_ALLOCATED) { m->bad_free_count++; return -1; }
    m->state[vec] = VEC_FREE;
    m->owner[vec] = VECTOR64_NO_OWNER;
    return 0;
}

int irq_vector_claim_legacy(vector64_map_t* m, int isa_irq, int owner) {
    if (isa_irq < 0 || isa_irq >= VECTOR64_LEGACY_COUNT) return -1;
    int v = VECTOR64_LEGACY_BASE + isa_irq;
    if (m->state[v] != VEC_LEGACY_FREE) { m->dup_claim_count++; return -1; }
    m->state[v] = VEC_LEGACY_OWNED;
    m->owner[v] = (int16_t)owner;
    return v;
}

int irq_vector_release_legacy(vector64_map_t* m, int vec) {
    if (vec < VECTOR64_LEGACY_BASE || vec >= VECTOR64_LEGACY_BASE + VECTOR64_LEGACY_COUNT ||
        m->state[vec] != VEC_LEGACY_OWNED) { m->bad_free_count++; return -1; }
    m->state[vec] = VEC_LEGACY_FREE;
    m->owner[vec] = VECTOR64_NO_OWNER;
    return 0;
}

vector64_state_t irq_vector_state(const vector64_map_t* m, int vec) {
    if (vec < 0 || vec >= VECTOR64_COUNT) return VEC_EXC; // out of range: treat as unusable
    return (vector64_state_t)m->state[vec];
}

int irq_vector_owner(const vector64_map_t* m, int vec) {
    if (vec < 0 || vec >= VECTOR64_COUNT) return VECTOR64_NO_OWNER;
    return m->owner[vec];
}

int irq_vector_free_count(const vector64_map_t* m) {
    int n = 0;
    for (int v = 0; v < VECTOR64_COUNT; v++) if (m->state[v] == VEC_FREE) n++;
    return n;
}
