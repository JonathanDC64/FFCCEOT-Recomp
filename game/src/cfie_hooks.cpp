// Game-specific glue for hooks listed in game/config/cfie/hooks.txt.
#include "ntr/nitro.h"

namespace {
constexpr uint32_t kOsIrqTable = 0x027E0000;            // OS_IRQTable (DTCM)
constexpr uint32_t kOsIrqHandlerThreadSwitch = 0x01FF8190;  // OS_IrqHandler_ThreadSwitch (ITCM)
}  // namespace

extern "C" void cfie_os_irq_handler(NtrCpu* c) { ntr::nitro::irq_handler(c, kOsIrqTable, kOsIrqHandlerThreadSwitch); }
