/*
 * Generic replacements for NitroSDK routines, used as recompiler hooks (game/config/<code>/hooks.txt).
 * Hooks that need game-specific addresses are wrapped by the game's own glue code.
 */
#ifndef NTR_NITRO_H
#define NTR_NITRO_H

#include "ntr/recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

void ntr_nitro_save_context(NtrCpu* c); /* OS_SaveContext */
void ntr_nitro_load_context(NtrCpu* c); /* OS_LoadContext */
void ntr_nitro_init_context(NtrCpu* c); /* OS_InitContext */

/* DS Protect detection checks ("flashcart", "emulator", ...): report nothing detected, as on a genuine cartridge. */
void ntr_dsprot_not_detected(NtrCpu* c);

#ifdef __cplusplus
}

namespace ntr::nitro {
/* OS_IrqHandler, given the addresses of OS_IRQTable and OS_IrqHandler_ThreadSwitch. */
void irq_handler(NtrCpu* c, uint32_t irq_table, uint32_t thread_switch);
}  // namespace ntr::nitro
#endif

#endif
