// Nintendo backwards-LZ decompression (MI_UncompressBackward / "BLZ").
//
// Footer (last 8 bytes of the compressed region):
//   u32 bits 0-23: length of the compressed part (counted back from the end)
//   u32 bits 24-31: footer length (bytes to skip before the compressed stream)
//   u32: number of extra bytes the decompressed data grows by
#include "ntr/rom.hpp"

#include <cstring>

namespace ntr {

std::vector<uint8_t> blz_decompress(std::span<const uint8_t> in) {
    if (in.size() < 8) throw RomError("BLZ: input too small");
    uint32_t enc_info, extra;
    std::memcpy(&enc_info, in.data() + in.size() - 8, 4);
    std::memcpy(&extra, in.data() + in.size() - 4, 4);
    uint32_t footer_len = enc_info >> 24;
    uint32_t enc_len = enc_info & 0xFFFFFF;
    if (enc_len > in.size() || footer_len > enc_len) throw RomError("BLZ: bad footer");

    std::vector<uint8_t> out(in.size() + extra);
    std::memcpy(out.data(), in.data(), in.size());

    size_t src = in.size() - footer_len;
    size_t dst = out.size();
    const size_t end = in.size() - enc_len;

    while (src > end) {
        uint8_t flags = out[--src];
        for (int bit = 0; bit < 8 && src > end; bit++, flags <<= 1) {
            if (flags & 0x80) {
                if (src < end + 2) throw RomError("BLZ: truncated reference");
                src -= 2;
                uint16_t v = uint16_t(out[src] | (out[src + 1] << 8));
                size_t disp = (v & 0x0FFF) + 3;
                size_t len = (v >> 12) + 3;
                if (len > dst) throw RomError("BLZ: output underflow");
                for (size_t i = 0; i < len; i++) {
                    --dst;
                    if (dst + disp >= out.size()) throw RomError("BLZ: bad displacement");
                    out[dst] = out[dst + disp];
                }
            } else {
                if (dst == 0) throw RomError("BLZ: output underflow");
                out[--dst] = out[--src];
            }
        }
    }
    return out;
}

}  // namespace ntr
