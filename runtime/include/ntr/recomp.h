/*
 * Interface between recompiled guest code and the ntr runtime.
 *
 * Every guest function becomes `void fn_<module>_<addr>(NtrCpu* c)`. Guest registers and flags live in
 * NtrCpu; memory goes through the page-table fast path below or the runtime's slow-path handlers.
 * This header must stay plain C11 so the generated code also builds with TinyCC.
 */
#ifndef NTR_RECOMP_H
#define NTR_RECOMP_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NTR_PAGE_SHIFT 12
#define NTR_PAGE_MASK 0xFFFu

typedef struct NtrCpu NtrCpu;
typedef void (*NtrFunc)(NtrCpu* c);

struct NtrCpu {
    uint32_t r[16];
    uint32_t n, z, c, v, q; /* condition flags, each 0 or 1 */
    uint32_t cpsr_ctrl;     /* CPSR bits 0-7: mode, T, F, I */
    int64_t cycles;         /* elapsed guest cycles (approximate) */
    int64_t next_event;     /* the runtime is polled once cycles reach this */
    uint8_t* const* read_pages;  /* host pointer per 4 KiB guest page, NULL = slow path */
    uint8_t* const* write_pages;
    void* runtime;
};

/* One entry per recompiled function. `addr` has bit 0 set for Thumb functions. */
typedef struct NtrFuncEntry {
    uint32_t addr;
    uint16_t module; /* see NTR_MODULE_* */
    NtrFunc fn;
} NtrFuncEntry;

#define NTR_MODULE_MAIN 0
#define NTR_MODULE_ITCM 1
#define NTR_MODULE_DTCM 2
#define NTR_MODULE_ARM7 3
#define NTR_MODULE_OVERLAY(n) (100 + (n))

/* ---- runtime services (implemented by the runtime) ---- */
uint32_t ntr_read32_slow(NtrCpu* c, uint32_t addr);
uint32_t ntr_read16_slow(NtrCpu* c, uint32_t addr);
uint32_t ntr_read8_slow(NtrCpu* c, uint32_t addr);
void ntr_write32_slow(NtrCpu* c, uint32_t addr, uint32_t v);
void ntr_write16_slow(NtrCpu* c, uint32_t addr, uint32_t v);
void ntr_write8_slow(NtrCpu* c, uint32_t addr, uint32_t v);

void ntr_call(NtrCpu* c, uint32_t target);           /* call/jump to guest code; bit 0 = Thumb */
void ntr_swi(NtrCpu* c, uint32_t number, uint32_t pc);
void ntr_poll(NtrCpu* c);                            /* scheduler events, IRQ delivery */
uint32_t ntr_get_spsr(NtrCpu* c);
void ntr_set_cpsr(NtrCpu* c, uint32_t value, uint32_t mask);
void ntr_set_spsr(NtrCpu* c, uint32_t value, uint32_t mask);
void ntr_exception_return(NtrCpu* c, uint32_t target); /* MOVS pc / LDM {..pc}^ */
uint32_t ntr_get_user_reg(NtrCpu* c, uint32_t r);
void ntr_set_user_reg(NtrCpu* c, uint32_t r, uint32_t v);
uint32_t ntr_mrc(NtrCpu* c, uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2);
void ntr_mcr(NtrCpu* c, uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v);
void ntr_undefined(NtrCpu* c, uint32_t pc, uint32_t opcode);
void ntr_bad_jump(NtrCpu* c, uint32_t pc, uint32_t target);
void ntr_unimplemented(NtrCpu* c, uint32_t addr);

/* ---- register / flag helpers ---- */
#define R(n) (c->r[n])
#define POLL() do { if (c->cycles >= c->next_event) ntr_poll(c); } while (0)

static inline uint32_t ntr_get_cpsr(NtrCpu* c) {
    return (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) | (c->q << 27) | c->cpsr_ctrl;
}

static inline uint32_t ntr_ror(uint32_t v, uint32_t s) {
    s &= 31;
    return s ? (v >> s) | (v << (32 - s)) : v;
}

static inline uint32_t ntr_clz(uint32_t v) {
    uint32_t n = 0;
    if (!v) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

static inline uint32_t ntr_add_flags(NtrCpu* c, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    c->n = r >> 31; c->z = r == 0; c->c = r < a; c->v = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}

static inline uint32_t ntr_adc_flags(NtrCpu* c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t w = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)w;
    c->n = r >> 31; c->z = r == 0; c->c = (uint32_t)(w >> 32); c->v = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}

static inline uint32_t ntr_sub_flags(NtrCpu* c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->n = r >> 31; c->z = r == 0; c->c = a >= b; c->v = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}

static inline uint32_t ntr_sbc_flags(NtrCpu* c, uint32_t a, uint32_t b, uint32_t cin) {
    uint32_t borrow = cin ^ 1u;
    uint32_t r = a - b - borrow;
    c->n = r >> 31; c->z = r == 0; c->c = (uint64_t)a >= (uint64_t)b + borrow; c->v = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}

static inline void ntr_nz(NtrCpu* c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }

/* Shifts by register: amount is the bottom byte; *carry is updated only when the shift is non-zero. */
static inline uint32_t ntr_lsl_reg(uint32_t v, uint32_t s, uint32_t* carry) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *carry = (v >> (32 - s)) & 1; return v << s; }
    *carry = s == 32 ? (v & 1) : 0;
    return 0;
}
static inline uint32_t ntr_lsr_reg(uint32_t v, uint32_t s, uint32_t* carry) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *carry = (v >> (s - 1)) & 1; return v >> s; }
    *carry = s == 32 ? (v >> 31) : 0;
    return 0;
}
static inline uint32_t ntr_asr_reg(uint32_t v, uint32_t s, uint32_t* carry) {
    s &= 0xFF;
    if (s == 0) return v;
    if (s < 32) { *carry = (v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    *carry = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ntr_ror_reg(uint32_t v, uint32_t s, uint32_t* carry) {
    s &= 0xFF;
    if (s == 0) return v;
    s &= 31;
    if (s == 0) { *carry = v >> 31; return v; }
    *carry = (v >> (s - 1)) & 1;
    return (v >> s) | (v << (32 - s));
}

/* Saturating arithmetic for QADD/QSUB/QDADD/QDSUB and SMLAxy accumulation. */
static inline uint32_t ntr_qadd(NtrCpu* c, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a + (int32_t)b;
    if (r > 0x7FFFFFFF) { c->q = 1; return 0x7FFFFFFFu; }
    if (r < -0x7FFFFFFF - 1) { c->q = 1; return 0x80000000u; }
    return (uint32_t)r;
}
static inline uint32_t ntr_qsub(NtrCpu* c, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a - (int32_t)b;
    if (r > 0x7FFFFFFF) { c->q = 1; return 0x7FFFFFFFu; }
    if (r < -0x7FFFFFFF - 1) { c->q = 1; return 0x80000000u; }
    return (uint32_t)r;
}
static inline uint32_t ntr_add_q(NtrCpu* c, uint32_t a, uint32_t b) { /* add, set Q on signed overflow */
    uint32_t r = a + b;
    if (((~(a ^ b)) & (a ^ r)) >> 31) c->q = 1;
    return r;
}

/* ---- memory ---- */
static inline uint32_t ntr_rd32(NtrCpu* c, uint32_t a) {
    uint8_t* p = c->read_pages[a >> NTR_PAGE_SHIFT];
    uint32_t v;
    if (p) { memcpy(&v, p + (a & (NTR_PAGE_MASK & ~3u)), 4); return v; }
    return ntr_read32_slow(c, a & ~3u);
}
/* LDR semantics: misaligned addresses rotate the aligned word. */
static inline uint32_t ntr_ldr(NtrCpu* c, uint32_t a) {
    uint32_t v = ntr_rd32(c, a);
    uint32_t s = (a & 3) * 8;
    return s ? (v >> s) | (v << (32 - s)) : v;
}
static inline uint32_t ntr_rd16(NtrCpu* c, uint32_t a) {
    uint8_t* p = c->read_pages[a >> NTR_PAGE_SHIFT];
    uint16_t v;
    if (p) { memcpy(&v, p + (a & (NTR_PAGE_MASK & ~1u)), 2); return v; }
    return ntr_read16_slow(c, a & ~1u);
}
static inline uint32_t ntr_rd8(NtrCpu* c, uint32_t a) {
    uint8_t* p = c->read_pages[a >> NTR_PAGE_SHIFT];
    if (p) return p[a & NTR_PAGE_MASK];
    return ntr_read8_slow(c, a);
}
static inline void ntr_wr32(NtrCpu* c, uint32_t a, uint32_t v) {
    uint8_t* p = c->write_pages[a >> NTR_PAGE_SHIFT];
    if (p) { memcpy(p + (a & (NTR_PAGE_MASK & ~3u)), &v, 4); return; }
    ntr_write32_slow(c, a & ~3u, v);
}
static inline void ntr_wr16(NtrCpu* c, uint32_t a, uint32_t v) {
    uint8_t* p = c->write_pages[a >> NTR_PAGE_SHIFT];
    uint16_t h = (uint16_t)v;
    if (p) { memcpy(p + (a & (NTR_PAGE_MASK & ~1u)), &h, 2); return; }
    ntr_write16_slow(c, a & ~1u, v & 0xFFFF);
}
static inline void ntr_wr8(NtrCpu* c, uint32_t a, uint32_t v) {
    uint8_t* p = c->write_pages[a >> NTR_PAGE_SHIFT];
    if (p) { p[a & NTR_PAGE_MASK] = (uint8_t)v; return; }
    ntr_write8_slow(c, a, v & 0xFF);
}

#ifdef __cplusplus
}
#endif

#endif /* NTR_RECOMP_H */
