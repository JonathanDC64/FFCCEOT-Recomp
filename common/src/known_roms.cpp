#include "ntr/rom.hpp"

namespace ntr {

static const KnownRom kKnownRoms[] = {
    {"1df23d5a3a655bdd587c360b581202931d0f66e1", "CFIE", "USA",
     "Final Fantasy Crystal Chronicles - Echoes of Time (USA) (En,Fr,Es)", true},
};

std::span<const KnownRom> known_roms() { return kKnownRoms; }

const KnownRom* find_known_rom(const Sha1Digest& digest) {
    std::string hex = to_hex(digest);
    for (const KnownRom& r : kKnownRoms)
        if (hex == r.sha1_hex) return &r;
    return nullptr;
}

}  // namespace ntr
