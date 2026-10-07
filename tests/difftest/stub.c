/*
 * Minimal runtime for differential tests of recompiled instructions.
 * Memory is sparse; unwritten bytes have a deterministic pseudo-random value (same function as difftest.py),
 * except the code blob, which reads back as itself. All accesses use the slow path so writes can be logged.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ntr/recomp.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

extern const NtrFuncEntry ntr_recomp_functions[];
extern const size_t ntr_recomp_function_count;

#define MAX_DIRTY 4096
static struct { uint32_t addr; uint8_t val; } g_dirty[MAX_DIRTY];
static int g_ndirty;
static struct { uint32_t addr, size, value; } g_writes[256];
static int g_nwrites;
static uint32_t g_calls, g_last_call, g_errors;
static const uint8_t* g_code;
static uint32_t g_code_base, g_code_size;
static uint8_t* g_null_pages[1u << 20];

static uint32_t hash_word(uint32_t a) {
    uint32_t h = a * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    return h;
}

static uint8_t mem_byte(uint32_t a) {
    for (int i = g_ndirty - 1; i >= 0; i--)
        if (g_dirty[i].addr == a) return g_dirty[i].val;
    if (a - g_code_base < g_code_size) return g_code[a - g_code_base];
    return (uint8_t)(hash_word(a & ~3u) >> ((a & 3) * 8));
}

static void set_byte(uint32_t a, uint8_t v) {
    if (g_ndirty < MAX_DIRTY) {
        g_dirty[g_ndirty].addr = a;
        g_dirty[g_ndirty].val = v;
        g_ndirty++;
    } else {
        g_errors++;
    }
}

static void log_write(uint32_t a, uint32_t size, uint32_t v) {
    if (g_nwrites < 256) {
        g_writes[g_nwrites].addr = a;
        g_writes[g_nwrites].size = size;
        g_writes[g_nwrites].value = v;
        g_nwrites++;
    }
}

uint32_t ntr_read32_slow(NtrCpu* c, uint32_t a) {
    return mem_byte(a) | (mem_byte(a + 1) << 8) | (mem_byte(a + 2) << 16) | ((uint32_t)mem_byte(a + 3) << 24);
}
uint32_t ntr_read16_slow(NtrCpu* c, uint32_t a) { return mem_byte(a) | (mem_byte(a + 1) << 8); }
uint32_t ntr_read8_slow(NtrCpu* c, uint32_t a) { return mem_byte(a); }
void ntr_write32_slow(NtrCpu* c, uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; i++) set_byte(a + i, (uint8_t)(v >> (i * 8)));
    log_write(a, 4, v);
}
void ntr_write16_slow(NtrCpu* c, uint32_t a, uint32_t v) {
    set_byte(a, (uint8_t)v);
    set_byte(a + 1, (uint8_t)(v >> 8));
    log_write(a, 2, v);
}
void ntr_write8_slow(NtrCpu* c, uint32_t a, uint32_t v) {
    set_byte(a, (uint8_t)v);
    log_write(a, 1, v);
}

void ntr_call(NtrCpu* c, uint32_t target) { g_calls++; g_last_call = target; }
void ntr_jump(NtrCpu* c, uint32_t target, uint32_t from, uint32_t entry_lr) { g_calls++; g_last_call = target; }
void ntr_swi(NtrCpu* c, uint32_t number, uint32_t pc) { g_errors++; }
void ntr_poll(NtrCpu* c) {}
uint32_t ntr_get_spsr(NtrCpu* c) { g_errors++; return 0; }
void ntr_set_cpsr(NtrCpu* c, uint32_t value, uint32_t mask) { g_errors++; }
void ntr_set_spsr(NtrCpu* c, uint32_t value, uint32_t mask) { g_errors++; }
void ntr_exception_return(NtrCpu* c, uint32_t target) { g_errors++; }
uint32_t ntr_get_user_reg(NtrCpu* c, uint32_t r) { return c->r[r]; }
void ntr_set_user_reg(NtrCpu* c, uint32_t r, uint32_t v) { c->r[r] = v; }
uint32_t ntr_mrc(NtrCpu* c, uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) { g_errors++; return 0; }
void ntr_mcr(NtrCpu* c, uint32_t cp, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v) { g_errors++; }
void ntr_undefined(NtrCpu* c, uint32_t pc, uint32_t opcode) { g_errors++; }
void ntr_bad_jump(NtrCpu* c, uint32_t pc, uint32_t target) { g_errors++; }
void ntr_unimplemented(NtrCpu* c, uint32_t addr) { g_errors++; }

EXPORT void dt_set_code(const uint8_t* code, uint32_t base, uint32_t size) {
    g_code = code;
    g_code_base = base;
    g_code_size = size;
}

/* regs[16] in/out; flags[5] = n z c v q in/out. Returns 0 on success, -1 if the function is missing. */
EXPORT int dt_run(uint32_t addr, uint32_t* regs, uint32_t* flags) {
    const NtrFuncEntry* e = NULL;
    for (size_t i = 0; i < ntr_recomp_function_count; i++)
        if ((ntr_recomp_functions[i].addr & ~1u) == addr) { e = &ntr_recomp_functions[i]; break; }
    if (!e) return -1;

    NtrCpu c;
    memset(&c, 0, sizeof c);
    memcpy(c.r, regs, 64);
    c.n = flags[0]; c.z = flags[1]; c.c = flags[2]; c.v = flags[3]; c.q = flags[4];
    c.cpsr_ctrl = 0x1F;
    c.next_event = INT64_MAX;
    c.read_pages = g_null_pages;
    c.write_pages = g_null_pages;
    g_ndirty = g_nwrites = 0;
    g_calls = g_errors = 0;
    e->fn(&c);
    memcpy(regs, c.r, 64);
    flags[0] = c.n; flags[1] = c.z; flags[2] = c.c; flags[3] = c.v; flags[4] = c.q;
    return 0;
}

EXPORT int dt_writes(uint32_t* out, int max) {
    int n = g_nwrites < max ? g_nwrites : max;
    for (int i = 0; i < n; i++) {
        out[i * 3] = g_writes[i].addr;
        out[i * 3 + 1] = g_writes[i].size;
        out[i * 3 + 2] = g_writes[i].value;
    }
    return n;
}

EXPORT uint32_t dt_errors(void) { return g_errors; }
EXPORT uint32_t dt_calls(void) { return g_calls; }
void ntr_trace_enter(NtrCpu* c, uint32_t addr) {}
