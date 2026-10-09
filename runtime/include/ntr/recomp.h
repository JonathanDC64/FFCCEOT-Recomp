/*
 * Interface between recompiled guest code and the ntr runtime.
 *
 * Every guest function becomes `void fn_<module>_<addr>(NtrCpu* c)`. Guest registers and flags live in
 * NtrCpu; memory goes through the page-table fast path below or the runtime's slow-path handlers.
 * This header must stay plain C11 so the generated code also builds with TinyCC.
 */
#ifndef NTR_RECOMP_H
#define NTR_RECOMP_H

#include <stddef.h>
#include <stdint.h>
#ifdef NTR_GAME_MODULE
/* The generated game module is built freestanding (no C library, nothing to link against but itself). */
#define NTR_MEMCPY __builtin_memcpy
#else
#include <string.h>
#define NTR_MEMCPY memcpy
#endif

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

/* One entry per ARM9 overlay or code region. It is resident when every probe word matches guest memory. */
typedef struct NtrOverlayInfo {
    uint32_t id, base, size, flags, nprobes;
    uint32_t probe_addr[4], probe_val[4];
} NtrOverlayInfo;

/* Not recompiled: while resident, the runtime runs code at these addresses on its interpreter. */
#define NTR_OVERLAY_INTERPRET 1u

#define NTR_MODULE_MAIN 0
#define NTR_MODULE_ITCM 1
#define NTR_MODULE_DTCM 2
#define NTR_MODULE_ARM7 3
#define NTR_MODULE_OVERLAY(n) (100 + (n))

/* ---- the game module ----
 * The recompiled game is a shared library of its own (never shipped: setup builds it from the player's ROM). It
 * links against nothing. Its one exported function, ntr_game_load, is given the runtime's services and the game's
 * hooks by the host, and returns the tables of recompiled functions. Bump NTR_ABI_VERSION whenever this interface or
 * the generated code's meaning changes: a module built for another version is refused (and rebuilt by setup). */
#define NTR_ABI_VERSION 1u

#define NTR_RUNTIME_SERVICES(X) \
    X(uint32_t, read32_slow, (NtrCpu* c, uint32_t addr)) \
    X(uint32_t, read16_slow, (NtrCpu* c, uint32_t addr)) \
    X(uint32_t, read8_slow, (NtrCpu* c, uint32_t addr)) \
    X(void, write32_slow, (NtrCpu* c, uint32_t addr, uint32_t v)) \
    X(void, write16_slow, (NtrCpu* c, uint32_t addr, uint32_t v)) \
    X(void, write8_slow, (NtrCpu* c, uint32_t addr, uint32_t v)) \
    /* call/jump to guest code; bit 0 = Thumb */ \
    X(void, call, (NtrCpu* c, uint32_t target)) \
    /* Computed jump that is not a return (a tail call): the target returns to `entry_lr`, the jumping function's own LR. */ \
    X(void, jump, (NtrCpu* c, uint32_t target, uint32_t from, uint32_t entry_lr)) \
    X(void, swi, (NtrCpu* c, uint32_t number, uint32_t pc)) \
    /* scheduler events, IRQ delivery */ \
    X(void, poll, (NtrCpu* c)) \
    X(uint32_t, get_spsr, (NtrCpu* c)) \
    X(void, set_cpsr, (NtrCpu* c, uint32_t value, uint32_t mask)) \
    X(void, set_spsr, (NtrCpu* c, uint32_t value, uint32_t mask)) \
    /* MOVS pc / LDM {..pc}^ */ \
    X(void, exception_return, (NtrCpu* c, uint32_t target)) \
    X(uint32_t, get_user_reg, (NtrCpu* c, uint32_t r)) \
    X(void, set_user_reg, (NtrCpu* c, uint32_t r, uint32_t v)) \
    X(uint32_t, mrc, (NtrCpu* c, uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2)) \
    X(void, mcr, (NtrCpu* c, uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v)) \
    X(void, undefined, (NtrCpu* c, uint32_t pc, uint32_t opcode)) \
    X(void, bad_jump, (NtrCpu* c, uint32_t pc, uint32_t target)) \
    X(void, unimplemented, (NtrCpu* c, uint32_t addr)) \
    /* debug builds of the generated code (NTR_TRACE_ENTRY) record every function entry for the fatal error report */ \
    X(void, trace_enter, (NtrCpu* c, uint32_t addr))

typedef struct NtrHostApi {
    uint32_t abi;  /* NTR_ABI_VERSION */
    uint32_t size; /* sizeof(NtrHostApi) */
#define NTR_DECLARE_SERVICE(ret, name, args) ret (*name) args;
    NTR_RUNTIME_SERVICES(NTR_DECLARE_SERVICE)
#undef NTR_DECLARE_SERVICE
    /* The game's own hooks (hooks.txt), by name; NULL if the host has none of that name. */
    void* (*find_hook)(const char* name);
} NtrHostApi;

/* What a loaded module provides: one entry per recompiled function and per overlay. */
typedef struct NtrGameInfo {
    uint32_t abi;
    const NtrFuncEntry* functions;
    size_t function_count;
    const NtrOverlayInfo* overlays;
    size_t overlay_count;
} NtrGameInfo;

/* The module's entry point; NULL if the module cannot work with this host. */
typedef const NtrGameInfo* (*NtrGameLoadFn)(const NtrHostApi* api);
#define NTR_GAME_LOAD_SYMBOL "ntr_game_load"

#ifdef NTR_GAME_MODULE
/* Inside the module the runtime's services are the host's, through a table filled when the module is loaded. */
extern NtrHostApi ntr_host;
#define ntr_read32_slow (ntr_host.read32_slow)
#define ntr_read16_slow (ntr_host.read16_slow)
#define ntr_read8_slow (ntr_host.read8_slow)
#define ntr_write32_slow (ntr_host.write32_slow)
#define ntr_write16_slow (ntr_host.write16_slow)
#define ntr_write8_slow (ntr_host.write8_slow)
#define ntr_call (ntr_host.call)
#define ntr_jump (ntr_host.jump)
#define ntr_swi (ntr_host.swi)
#define ntr_poll (ntr_host.poll)
#define ntr_get_spsr (ntr_host.get_spsr)
#define ntr_set_cpsr (ntr_host.set_cpsr)
#define ntr_set_spsr (ntr_host.set_spsr)
#define ntr_exception_return (ntr_host.exception_return)
#define ntr_get_user_reg (ntr_host.get_user_reg)
#define ntr_set_user_reg (ntr_host.set_user_reg)
#define ntr_mrc (ntr_host.mrc)
#define ntr_mcr (ntr_host.mcr)
#define ntr_undefined (ntr_host.undefined)
#define ntr_bad_jump (ntr_host.bad_jump)
#define ntr_unimplemented (ntr_host.unimplemented)
#define ntr_trace_enter (ntr_host.trace_enter)
#else
/* ---- runtime services (implemented by the runtime) ---- */
#define NTR_PROTOTYPE_SERVICE(ret, name, args) ret ntr_##name args;
NTR_RUNTIME_SERVICES(NTR_PROTOTYPE_SERVICE)
#undef NTR_PROTOTYPE_SERVICE
/* Fills in `api` with the runtime's services (find_hook is left for the host to set). */
void ntr_fill_host_api(NtrHostApi* api);
#endif

/* ---- register / flag helpers ---- */
#define R(n) (c->r[n])
/* Debug builds of the generated code (NTR_TRACE_ENTRY) record every function entry in a ring buffer
 * that the runtime prints on a fatal error. */
#ifdef NTR_TRACE_ENTRY
#define NTR_ENTER(a) ntr_trace_enter(c, a)
#else
#define NTR_ENTER(a) ((void)0)
#endif
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
    if (p) { NTR_MEMCPY(&v, p + (a & (NTR_PAGE_MASK & ~3u)), 4); return v; }
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
    if (p) { NTR_MEMCPY(&v, p + (a & (NTR_PAGE_MASK & ~1u)), 2); return v; }
    return ntr_read16_slow(c, a & ~1u);
}
static inline uint32_t ntr_rd8(NtrCpu* c, uint32_t a) {
    uint8_t* p = c->read_pages[a >> NTR_PAGE_SHIFT];
    if (p) return p[a & NTR_PAGE_MASK];
    return ntr_read8_slow(c, a);
}
static inline void ntr_wr32(NtrCpu* c, uint32_t a, uint32_t v) {
    uint8_t* p = c->write_pages[a >> NTR_PAGE_SHIFT];
    if (p) { NTR_MEMCPY(p + (a & (NTR_PAGE_MASK & ~3u)), &v, 4); return; }
    ntr_write32_slow(c, a & ~3u, v);
}
static inline void ntr_wr16(NtrCpu* c, uint32_t a, uint32_t v) {
    uint8_t* p = c->write_pages[a >> NTR_PAGE_SHIFT];
    uint16_t h = (uint16_t)v;
    if (p) { NTR_MEMCPY(p + (a & (NTR_PAGE_MASK & ~1u)), &h, 2); return; }
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
