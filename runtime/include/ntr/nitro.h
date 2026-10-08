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

/* CARDi_RequestStreamCommand, given the address of the card library's common state (cardi_common): a synchronous
   backup read, write or verify, done on the save memory at once. Returns false (nothing done) for any other request,
   which then has to run as usual. */
bool card_backup_request(NtrCpu* c, uint32_t common);
}  // namespace ntr::nitro
#endif

#endif
