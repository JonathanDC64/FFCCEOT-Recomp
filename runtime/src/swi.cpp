// High-level emulation of the ARM9 BIOS software interrupts (SWI/SVC) used by NitroSDK games.
#include <cmath>

#include "runtime_internal.hpp"

namespace ntr {

namespace {

u32 rd32(NtrCpu* c, u32 a) { return ntr_rd32(c, a); }
u32 rd16(NtrCpu* c, u32 a) { return ntr_rd16(c, a); }
u32 rd8(NtrCpu* c, u32 a) { return ntr_rd8(c, a); }

u16 crc16(u16 crc, NtrCpu* c, u32 addr, u32 len) {
    static const u16 table[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
    for (u32 i = 0; i < len; i++) {
        crc ^= u16(rd8(c, addr + i));
        for (int j = 0; j < 8; j++) {
            bool carry = crc & 1;
            crc >>= 1;
            if (carry) crc ^= u16(table[j] << (7 - j));
        }
    }
    return crc;
}

// Byte sink that supports the 16-bit-write variants (VRAM cannot take 8-bit writes).
struct Writer {
    NtrCpu* c;
    u32 dst;
    bool wide;
    u32 pending = 0;
    bool half = false;
    void put(u8 b) {
        if (!wide) {
            ntr_wr8(c, dst++, b);
            return;
        }
        if (!half) {
            pending = b;
            half = true;
        } else {
            ntr_wr16(c, dst & ~1u, pending | (u32(b) << 8));
            dst += 2;
            half = false;
        }
    }
    u8 peek_back(u32 dist) {  // byte written `dist` bytes ago (LZ77 back-reference)
        u32 pos = dst + (half ? 1 : 0) - dist;
        if (half && pos == dst) return u8(pending);
        return u8(rd8(c, pos));
    }
};

// Source of compressed bytes: either plain memory or the game's read callbacks.
struct Reader {
    RuntimeImpl* r;
    NtrCpu* c;
    u32 src;
    u32 callbacks = 0;  // 0 = direct memory reads
    u8 byte() {
        if (!callbacks) return u8(rd8(c, src++));
        c->r[0] = src++;
        r->call_guest(rd32(c, callbacks + 8));  // readByte
        return u8(c->r[0]);
    }
    u32 word() {
        if (!callbacks) {
            u32 v = rd32(c, src);
            src += 4;
            return v;
        }
        u32 v = byte();
        v |= u32(byte()) << 8;
        v |= u32(byte()) << 16;
        v |= u32(byte()) << 24;
        return v;
    }
};

u32 open_stream(RuntimeImpl& r, NtrCpu* c, Reader& in, u32 dst, u32 param) {
    if (!in.callbacks) return in.word();
    c->r[0] = in.src;
    c->r[1] = dst;
    c->r[2] = param;
    r.call_guest(rd32(c, in.callbacks + 0));  // initStream: returns the header word
    in.src += 4;
    return c->r[0];
}

void close_stream(RuntimeImpl& r, NtrCpu* c, Reader& in) {
    if (!in.callbacks) return;
    u32 fn = rd32(c, in.callbacks + 4);
    if (!fn) return;
    c->r[0] = in.src;
    r.call_guest(fn);
}

void lz77(RuntimeImpl& r, NtrCpu* c, u32 src, u32 dst, bool wide, u32 param, u32 callbacks) {
    Reader in{&r, c, src, callbacks};
    u32 header = open_stream(r, c, in, dst, param);
    u32 size = header >> 8;
    Writer out{c, dst, wide};
    u32 done = 0;
    while (done < size) {
        u8 flags = in.byte();
        for (int i = 0; i < 8 && done < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u8 b1 = in.byte(), b2 = in.byte();
                u32 len = (b1 >> 4) + 3, disp = (u32(b1 & 0xF) << 8 | b2) + 1;
                for (u32 k = 0; k < len && done < size; k++, done++) out.put(out.peek_back(disp));
            } else {
                out.put(in.byte());
                done++;
            }
        }
    }
    close_stream(r, c, in);
}

void rle(RuntimeImpl& r, NtrCpu* c, u32 src, u32 dst, bool wide, u32 param, u32 callbacks) {
    Reader in{&r, c, src, callbacks};
    u32 size = open_stream(r, c, in, dst, param) >> 8;
    Writer out{c, dst, wide};
    u32 done = 0;
    while (done < size) {
        u8 flag = in.byte();
        if (flag & 0x80) {
            u32 len = (flag & 0x7F) + 3;
            u8 b = in.byte();
            for (u32 k = 0; k < len && done < size; k++, done++) out.put(b);
        } else {
            u32 len = (flag & 0x7F) + 1;
            for (u32 k = 0; k < len && done < size; k++, done++) out.put(in.byte());
        }
    }
    close_stream(r, c, in);
}

void huffman(RuntimeImpl& r, NtrCpu* c, u32 src, u32 dst, u32 param, u32 callbacks) {
    Reader in{&r, c, src, callbacks};
    u32 header = open_stream(r, c, in, dst, param);
    const u32 bits = header & 0xF, size = header >> 8;
    // The tree is read into a host buffer: size byte, then (size+1)*2 bytes of nodes.
    u8 tree_size = in.byte();
    std::vector<u8> tree((tree_size + 1) * 2);
    tree[0] = tree_size;
    for (size_t i = 1; i < tree.size(); i++) tree[i] = in.byte();
    u32 written = 0, out_word = 0, out_bits = 0;
    u32 node = 1;  // offset of the root node in `tree`
    while (written < size) {
        u32 data = in.word();
        for (int i = 31; i >= 0 && written < size; i--) {
            u8 n = tree[node];
            bool bit = (data >> i) & 1;
            u32 child = (node & ~1u) + (n & 0x3F) * 2 + 2 + (bit ? 1 : 0);
            bool leaf = bit ? (n & 0x40) : (n & 0x80);
            if (leaf) {
                out_word |= u32(tree[child]) << out_bits;
                out_bits += bits;
                node = 1;
                if (out_bits == 32) {
                    ntr_wr32(c, dst + written, out_word);
                    written += 4;
                    out_word = out_bits = 0;
                }
            } else {
                node = child;
            }
        }
    }
    close_stream(r, c, in);
}

void bit_unpack(NtrCpu* c, u32 src, u32 dst, u32 info) {
    u32 len = rd16(c, info), sw = rd8(c, info + 2), dw = rd8(c, info + 3), off = rd32(c, info + 4);
    bool zero = off >> 31;
    off &= 0x7FFFFFFF;
    u32 out = 0, out_bits = 0;
    for (u32 i = 0; i < len; i++) {
        u32 b = rd8(c, src + i);
        for (u32 j = 0; j < 8; j += sw) {
            u32 v = (b >> j) & ((1u << sw) - 1);
            if (v || zero) v += off;
            out |= v << out_bits;
            out_bits += dw;
            if (out_bits >= 32) {
                ntr_wr32(c, dst, out);
                dst += 4;
                out = out_bits = 0;
            }
        }
    }
}

}  // namespace

void RuntimeImpl::swi(u32 number, u32 pc) {
    NtrCpu* c = &cpu;
    switch (number) {
    case 0x03:  // WaitByLoop
        cpu.cycles += int64_t(cpu.r[0]) * 4;
        break;
    case 0x06:  // Halt
        halt();
        break;
    case 0x09: {  // Div
        int32_t num = int32_t(cpu.r[0]), den = int32_t(cpu.r[1]);
        if (den == 0) {
            cpu.r[0] = num < 0 ? u32(-1) : 1;
            cpu.r[1] = u32(num);
        } else if (num == INT32_MIN && den == -1) {
            cpu.r[0] = u32(INT32_MIN);
            cpu.r[1] = 0;
        } else {
            cpu.r[0] = u32(num / den);
            cpu.r[1] = u32(num % den);
        }
        int32_t q = int32_t(cpu.r[0]);
        cpu.r[3] = u32(q < 0 ? -q : q);
        break;
    }
    case 0x0B: {  // CpuSet
        u32 src = cpu.r[0], dst = cpu.r[1], ctl = cpu.r[2], n = ctl & 0x1FFFFF;
        bool fill = ctl & (1u << 24), word = ctl & (1u << 26);
        if (word) {
            src &= ~3u;
            dst &= ~3u;
            u32 v = rd32(c, src);
            for (u32 i = 0; i < n; i++) ntr_wr32(c, dst + i * 4, fill ? v : rd32(c, src + i * 4));
        } else {
            src &= ~1u;
            dst &= ~1u;
            u32 v = rd16(c, src);
            for (u32 i = 0; i < n; i++) ntr_wr16(c, dst + i * 2, fill ? v : rd16(c, src + i * 2));
        }
        break;
    }
    case 0x0C: {  // CpuFastSet
        u32 src = cpu.r[0] & ~3u, dst = cpu.r[1] & ~3u, ctl = cpu.r[2], n = ((ctl & 0x1FFFFF) + 7) & ~7u;
        bool fill = ctl & (1u << 24);
        u32 v = rd32(c, src);
        for (u32 i = 0; i < n; i++) ntr_wr32(c, dst + i * 4, fill ? v : rd32(c, src + i * 4));
        break;
    }
    case 0x0D:  // Sqrt
        cpu.r[0] = u32(std::sqrt(double(cpu.r[0])));
        break;
    case 0x0E:  // GetCRC16
        cpu.r[0] = crc16(u16(cpu.r[0]), c, cpu.r[1], cpu.r[2]);
        break;
    case 0x0F:  // IsDebugger
        cpu.r[0] = 0;
        break;
    case 0x10:  // BitUnPack
        bit_unpack(c, cpu.r[0], cpu.r[1], cpu.r[2]);
        break;
    case 0x11:  // LZ77UnCompReadNormalWrite8bit
        lz77(*this, c, cpu.r[0], cpu.r[1], false, 0, 0);
        break;
    case 0x12:  // LZ77UnCompReadByCallbackWrite16bit
        lz77(*this, c, cpu.r[0], cpu.r[1], true, cpu.r[2], cpu.r[3]);
        break;
    case 0x13:  // HuffUnCompReadByCallback
        huffman(*this, c, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3]);
        break;
    case 0x14:  // RLUnCompReadNormalWrite8bit
        rle(*this, c, cpu.r[0], cpu.r[1], false, 0, 0);
        break;
    case 0x15:  // RLUnCompReadByCallbackWrite16bit
        rle(*this, c, cpu.r[0], cpu.r[1], true, cpu.r[2], cpu.r[3]);
        break;
    default:
        fatal("unimplemented BIOS SWI %02x at %08x", number, pc);
    }
}

}  // namespace ntr
