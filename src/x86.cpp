// x86-32 interpreter: integer core + SSE scalar/packed subset + a little x87, as emitted by MSVC for the game.
#include "x86.h"
#include "rt.h"
#include <math.h>
#include <string.h>
#include <unordered_map>
#include <vector>

namespace {
uint8_t* g_img = nullptr;
uint32_t g_imgBase = 0, g_imgSize = 0;
std::unordered_map<uint32_t, X86::Hook> g_hooks;
const uint32_t RET_MAGIC = 0xFFFF0000u;

inline uint32_t mk(int bits) { return bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u); }
inline uint32_t sg(int bits) { return 1u << (bits - 1); }
inline bool parity(uint32_t v) { v &= 0xff; v ^= v >> 4; v &= 0xf; return !((0x6996 >> v) & 1); }
}

X86& x86()
{
    static X86 inst;
    return inst;
}

bool x86_load_image(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    uint32_t hdr[2];
    if (fread(hdr, 4, 2, f) != 2) {
        fclose(f);
        return false;
    }
    g_imgBase = hdr[0];
    g_imgSize = hdr[1];
    g_img = (uint8_t*)malloc(g_imgSize);
    size_t got = fread(g_img, 1, g_imgSize, f);
    fclose(f);
    return got == g_imgSize;
}

void x86_init(uint32_t stackBlobOffset, uint32_t stackSize, uint32_t tebBlobOffset)
{
    X86& c = x86();
    memset(c.reg, 0, sizeof c.reg);
    c.reg[4] = GBASE + stackBlobOffset + stackSize - 16;
    c.fsBase = GBASE + tebBlobOffset;
}

void X86::hook(uint32_t addr, int popBytes, std::function<void(X86&)> fn)
{
    g_hooks[addr] = Hook{std::move(fn), popBytes};
}

[[noreturn]] void X86::fault(const char* why)
{
    logf("x86 fault: %s at eip=%08x  (eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x)", why,
         eip, reg[0], reg[1], reg[2], reg[3], reg[4], reg[5], reg[6], reg[7]);
    for (int i = 0; i < 24; ++i) logf("  hist %08x", hist[(histPos + i + 8) & 31]);
    {   // frame-pointer chain (MSVC code keeps ebp frames)
        uint32_t bp = reg[5];
        for (int i = 0; i < 10 && bp >= GBASE && bp + 8 < BLOB_SIZE; ++i) {
            logf("  ret %08x", *(uint32_t*)(G + bp + 4));
            bp = *(uint32_t*)(G + bp);
        }
    }
    abort();
}

uint8_t* X86::ptr(uint32_t a, uint32_t n)
{
    if (a >= g_imgBase && a - g_imgBase + n <= g_imgSize) {
        return g_img + (a - g_imgBase);
    }
    if (a - GBASE + n <= BLOB_SIZE && a >= GBASE) {
        return G + (a - GBASE);
    }
    logf("x86 bad memory access %08x (%u bytes)", a, n);
    fault("memory");
}

uint32_t X86::rd32(uint32_t a) { uint32_t v; memcpy(&v, ptr(a, 4), 4); return v; }
uint16_t X86::rd16(uint32_t a) { uint16_t v; memcpy(&v, ptr(a, 2), 2); return v; }
uint8_t X86::rd8(uint32_t a) { return *ptr(a, 1); }
uint64_t X86::rd64(uint32_t a) { uint64_t v; memcpy(&v, ptr(a, 8), 8); return v; }
float X86::rdF(uint32_t a) { float v; memcpy(&v, ptr(a, 4), 4); return v; }
static uint32_t g_watch = 0;
static inline void codeGuard(uint32_t a, uint32_t eip, const char* w) { if (a >= 0x401000 && a < 0x4f0000) { logf("WRITE INTO CODE %s a=%08x eip=%08x", w, a, eip); X86& cc = x86(); logf("  regs eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x", cc.reg[0], cc.reg[1], cc.reg[2], cc.reg[3], cc.reg[6], cc.reg[7], cc.reg[5]); uint32_t bp = cc.reg[5]; for (int i = 0; i < 8 && bp < BLOB_SIZE; ++i) { logf("  ret %08x", *(uint32_t*)(G + bp + 4)); bp = *(uint32_t*)(G + bp); } } }
void X86::wr32(uint32_t a, uint32_t v) { codeGuard(a, eip, "wr32"); memcpy(ptr(a, 4), &v, 4); }
void X86::wr16(uint32_t a, uint16_t v) { codeGuard(a, eip, "wr16"); memcpy(ptr(a, 2), &v, 2); }
void X86::wr8(uint32_t a, uint8_t v) { codeGuard(a, eip, "wr8"); *ptr(a, 1) = v; }
void X86::wr64(uint32_t a, uint64_t v) { codeGuard(a, eip, "wr64"); memcpy(ptr(a, 8), &v, 8); }

// ---------------------------------------------------------------------------------------------------------------
namespace {
struct Ctx {
    X86* c;
    uint32_t start;
    bool opsz16 = false, rep = false, repne = false, fs = false;
};

struct ModRM {
    int mod, reg, rm;
    bool isReg;
    uint32_t addr;
};
}

#define R(i) reg[i]

static inline uint8_t get8(X86& c, int i) { return (i < 4) ? (uint8_t)c.reg[i] : (uint8_t)(c.reg[i - 4] >> 8); }
static inline void set8(X86& c, int i, uint8_t v)
{
    if (i < 4) {
        c.reg[i] = (c.reg[i] & ~0xffu) | v;
    } else {
        c.reg[i - 4] = (c.reg[i - 4] & ~0xff00u) | ((uint32_t)v << 8);
    }
}

static inline uint32_t fetch8(X86& c) { return c.rd8(c.eip++); }
static inline uint32_t fetch16(X86& c) { uint32_t v = c.rd16(c.eip); c.eip += 2; return v; }
static inline uint32_t fetch32(X86& c) { uint32_t v = c.rd32(c.eip); c.eip += 4; return v; }
static inline int32_t s8(uint32_t v) { return (int32_t)(int8_t)v; }

static ModRM decode(X86& c, bool fs)
{
    ModRM m;
    uint32_t b = fetch8(c);
    m.mod = b >> 6;
    m.reg = (b >> 3) & 7;
    m.rm = b & 7;
    m.isReg = (m.mod == 3);
    m.addr = 0;
    if (m.isReg) {
        return m;
    }
    uint32_t a = 0;
    if (m.rm == 4) {
        uint32_t sib = fetch8(c);
        int scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && m.mod == 0) {
            a = fetch32(c);
        } else {
            a = c.reg[base];
        }
        if (idx != 4) {
            a += c.reg[idx] << scale;
        }
    } else if (m.rm == 5 && m.mod == 0) {
        a = fetch32(c);
    } else {
        a = c.reg[m.rm];
    }
    if (m.mod == 1) {
        a += (uint32_t)s8(fetch8(c));
    } else if (m.mod == 2) {
        a += fetch32(c);
    }
    if (fs) {
        a += c.fsBase;
    }
    m.addr = a;
    return m;
}

static uint32_t rdOp(X86& c, const ModRM& m, int bits)
{
    if (m.isReg) {
        if (bits == 8) return get8(c, m.rm);
        if (bits == 16) return c.reg[m.rm] & 0xffff;
        return c.reg[m.rm];
    }
    if (bits == 8) return c.rd8(m.addr);
    if (bits == 16) return c.rd16(m.addr);
    return c.rd32(m.addr);
}

static void wrOp(X86& c, const ModRM& m, int bits, uint32_t v)
{
    if (m.isReg) {
        if (bits == 8) set8(c, m.rm, (uint8_t)v);
        else if (bits == 16) c.reg[m.rm] = (c.reg[m.rm] & 0xffff0000u) | (v & 0xffff);
        else c.reg[m.rm] = v;
        return;
    }
    if (bits == 8) c.wr8(m.addr, (uint8_t)v);
    else if (bits == 16) c.wr16(m.addr, (uint16_t)v);
    else c.wr32(m.addr, v);
}

static uint32_t rdReg(X86& c, int r, int bits)
{
    if (bits == 8) return get8(c, r);
    if (bits == 16) return c.reg[r] & 0xffff;
    return c.reg[r];
}

static void wrReg(X86& c, int r, int bits, uint32_t v)
{
    if (bits == 8) set8(c, r, (uint8_t)v);
    else if (bits == 16) c.reg[r] = (c.reg[r] & 0xffff0000u) | (v & 0xffff);
    else c.reg[r] = v;
}

static void setSZP(X86& c, uint32_t res, int bits)
{
    res &= mk(bits);
    c.ZF = res == 0;
    c.SF = (res & sg(bits)) != 0;
    c.PF = parity(res);
}

static uint32_t alu(X86& c, int op, uint32_t a, uint32_t b, int bits)
{
    uint32_t m = mk(bits), s = sg(bits);
    a &= m;
    b &= m;
    uint64_t r64;
    uint32_t res;
    switch (op) {
    case 0: case 2: {   // add / adc
        uint32_t cin = (op == 2 && c.CF) ? 1 : 0;
        r64 = (uint64_t)a + b + cin;
        res = (uint32_t)r64 & m;
        c.CF = (r64 >> bits) != 0;
        c.OF = ((a ^ res) & (b ^ res) & s) != 0;
        break;
    }
    case 3: case 5: case 7: {   // sbb / sub / cmp
        uint32_t cin = (op == 3 && c.CF) ? 1 : 0;
        r64 = (uint64_t)a - b - cin;
        res = (uint32_t)r64 & m;
        c.CF = (uint64_t)a < (uint64_t)b + cin;
        c.OF = ((a ^ b) & (a ^ res) & s) != 0;
        break;
    }
    case 1: res = (a | b) & m; c.CF = c.OF = false; break;
    case 4: res = (a & b) & m; c.CF = c.OF = false; break;
    default: res = (a ^ b) & m; c.CF = c.OF = false; break;   // 6 xor
    }
    setSZP(c, res, bits);
    return res;
}

static bool cond(X86& c, int cc)
{
    bool r;
    switch (cc >> 1) {
    case 0: r = c.OF; break;
    case 1: r = c.CF; break;
    case 2: r = c.ZF; break;
    case 3: r = c.CF || c.ZF; break;
    case 4: r = c.SF; break;
    case 5: r = c.PF; break;
    case 6: r = c.SF != c.OF; break;
    default: r = c.ZF || (c.SF != c.OF); break;
    }
    return (cc & 1) ? !r : r;
}

static void push32(X86& c, uint32_t v) { c.reg[4] -= 4; c.wr32(c.reg[4], v); }
static uint32_t pop32(X86& c) { uint32_t v = c.rd32(c.reg[4]); c.reg[4] += 4; return v; }

static uint32_t shiftOp(X86& c, int op, uint32_t v, uint32_t n, int bits)
{
    n &= 31;
    if (n == 0) {
        return v;
    }
    uint32_t m = mk(bits), s = sg(bits);
    v &= m;
    uint32_t r = v;
    switch (op) {
    case 0: {   // rol
        n %= bits;
        r = ((v << n) | (v >> (bits - n))) & m;
        c.CF = r & 1;
        return r;
    }
    case 1: {   // ror
        n %= bits;
        r = ((v >> n) | (v << (bits - n))) & m;
        c.CF = (r & s) != 0;
        return r;
    }
    case 4: case 6: // shl
        c.CF = n <= (uint32_t)bits ? ((v >> (bits - n)) & 1) : 0;
        r = (n >= 32 ? 0 : (v << n)) & m;
        c.OF = ((r & s) != 0) != c.CF;
        break;
    case 5:         // shr
        c.CF = (v >> (n - 1)) & 1;
        r = n >= 32 ? 0 : v >> n;
        c.OF = (v & s) != 0;
        break;
    case 7: {       // sar
        int32_t sv = bits == 32 ? (int32_t)v : (bits == 16 ? (int32_t)(int16_t)v : (int32_t)(int8_t)v);
        c.CF = (sv >> (n - 1 > 31 ? 31 : n - 1)) & 1;
        r = (uint32_t)(sv >> (n > 31 ? 31 : n)) & m;
        c.OF = false;
        break;
    }
    default:
        r = v;
        break;
    }
    setSZP(c, r, bits);
    return r;
}

// ---- x87 helpers -----------------------------------------------------------------------------------------------
static void fpush(X86& c, long double v)
{
    for (int i = 7; i > 0; --i) c.st[i] = c.st[i - 1];
    c.st[0] = v;
    if (c.ftop < 8) ++c.ftop;
}
static long double fpop(X86& c)
{
    long double v = c.st[0];
    for (int i = 0; i < 7; ++i) c.st[i] = c.st[i + 1];
    if (c.ftop > 0) --c.ftop;
    return v;
}

// ---- main step ---------------------------------------------------------------------------------------------------
static float xf(const X86::Xmm& x, int i) { return x.f[i]; }

void X86::step()
{
    Ctx cx;
    cx.c = this;
    cx.start = eip;
    hist[histPos++ & 31] = eip;
    bool opsz16 = false, rep = false, repne = false, fs = false;
    uint32_t op;
    for (;;) {
        op = fetch8(*this);
        if (op == 0x66) opsz16 = true;
        else if (op == 0xF3) rep = true;
        else if (op == 0xF2) repne = true;
        else if (op == 0x64) fs = true;
        else if (op == 0x2E || op == 0x3E || op == 0xF0 || op == 0x26 || op == 0x36 || op == 0x65) {}
        else break;
    }
    int vb = opsz16 ? 16 : 32;
    ++instrCount;

    if (op <= 0x3F && (op & 7) < 6) {
        int aop = (op >> 3) & 7;
        int form = op & 7;
        if (form == 4) {
            uint32_t r = alu(*this, aop, get8(*this, 0), fetch8(*this), 8);
            if (aop != 7) set8(*this, 0, (uint8_t)r);
            return;
        }
        if (form == 5) {
            uint32_t imm = opsz16 ? fetch16(*this) : fetch32(*this);
            uint32_t r = alu(*this, aop, rdReg(*this, 0, vb), imm, vb);
            if (aop != 7) wrReg(*this, 0, vb, r);
            return;
        }
        ModRM m = decode(*this, fs);
        int bits = (form & 1) ? vb : 8;
        if (form < 2) {     // E, G
            uint32_t r = alu(*this, aop, rdOp(*this, m, bits), rdReg(*this, m.reg, bits), bits);
            if (aop != 7) wrOp(*this, m, bits, r);
        } else {            // G, E
            uint32_t r = alu(*this, aop, rdReg(*this, m.reg, bits), rdOp(*this, m, bits), bits);
            if (aop != 7) wrReg(*this, m.reg, bits, r);
        }
        return;
    }

    switch (op) {
    case 0x0F:
        goto twobyte;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
        int r = op & 7;
        bool cf = CF;
        uint32_t v = alu(*this, 0, rdReg(*this, r, vb), 1, vb);
        CF = cf;
        wrReg(*this, r, vb, v);
        return;
    }
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        int r = op & 7;
        bool cf = CF;
        uint32_t v = alu(*this, 5, rdReg(*this, r, vb), 1, vb);
        CF = cf;
        wrReg(*this, r, vb, v);
        return;
    }
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push32(*this, reg[op & 7]);
        return;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        reg[op & 7] = pop32(*this);
        return;
    case 0x68: push32(*this, fetch32(*this)); return;
    case 0x6A: push32(*this, (uint32_t)s8(fetch8(*this))); return;
    case 0x69: case 0x6B: {
        ModRM m = decode(*this, fs);
        int32_t a = (int32_t)rdOp(*this, m, 32);
        int32_t b = (op == 0x69) ? (int32_t)fetch32(*this) : s8(fetch8(*this));
        int64_t r = (int64_t)a * b;
        reg[m.reg] = (uint32_t)r;
        CF = OF = (r != (int32_t)r);
        setSZP(*this, (uint32_t)r, 32);
        return;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int32_t d = s8(fetch8(*this));
        if (cond(*this, op & 15)) eip += (uint32_t)d;
        return;
    }
    case 0x80: case 0x81: case 0x83: {
        ModRM m = decode(*this, fs);
        int bits = (op == 0x80) ? 8 : vb;
        uint32_t imm = (op == 0x81) ? (opsz16 ? fetch16(*this) : fetch32(*this)) : (uint32_t)s8(fetch8(*this));
        uint32_t r = alu(*this, m.reg, rdOp(*this, m, bits), imm, bits);
        if (m.reg != 7) wrOp(*this, m, bits, r);
        return;
    }
    case 0x84: case 0x85: {
        ModRM m = decode(*this, fs);
        int bits = (op == 0x84) ? 8 : vb;
        alu(*this, 4, rdOp(*this, m, bits), rdReg(*this, m.reg, bits), bits);
        return;
    }
    case 0x86: case 0x87: {
        ModRM m = decode(*this, fs);
        int bits = (op == 0x86) ? 8 : vb;
        uint32_t a = rdOp(*this, m, bits), b = rdReg(*this, m.reg, bits);
        wrOp(*this, m, bits, b);
        wrReg(*this, m.reg, bits, a);
        return;
    }
    case 0x88: case 0x89: {
        ModRM m = decode(*this, fs);
        int bits = (op == 0x88) ? 8 : vb;
        wrOp(*this, m, bits, rdReg(*this, m.reg, bits));
        return;
    }
    case 0x8A: case 0x8B: {
        ModRM m = decode(*this, fs);
        int bits = (op == 0x8A) ? 8 : vb;
        wrReg(*this, m.reg, bits, rdOp(*this, m, bits));
        return;
    }
    case 0x8D: {
        ModRM m = decode(*this, false);
        wrReg(*this, m.reg, vb, m.addr);
        return;
    }
    case 0x8F: {
        ModRM m = decode(*this, fs);
        uint32_t v = pop32(*this);
        wrOp(*this, m, 32, v);
        return;
    }
    case 0x90: return;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        std::swap(reg[0], reg[op & 7]);
        return;
    }
    case 0x98: reg[0] = opsz16 ? ((reg[0] & 0xffff0000u) | (uint16_t)(int16_t)(int8_t)reg[0]) : (uint32_t)(int32_t)(int16_t)reg[0]; return;
    case 0x99: reg[2] = (reg[0] & 0x80000000u) ? 0xffffffffu : 0; return;
    case 0x9E: {    // sahf
        uint8_t ah = (reg[0] >> 8) & 0xff;
        CF = ah & 1; PF = ah & 4; ZF = ah & 0x40; SF = ah & 0x80;
        return;
    }
    case 0x9F: {    // lahf
        uint8_t ah = 2 | (CF ? 1 : 0) | (PF ? 4 : 0) | (ZF ? 0x40 : 0) | (SF ? 0x80 : 0);
        reg[0] = (reg[0] & ~0xff00u) | ((uint32_t)ah << 8);
        return;
    }
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {   // mov al/eax <-> [moffs32]
        uint32_t a = fetch32(*this) + (fs ? fsBase : 0);
        int bits = (op & 1) ? vb : 8;
        if (op < 0xA2) {
            wrReg(*this, 0, bits, bits == 8 ? rd8(a) : (bits == 16 ? rd16(a) : rd32(a)));
        } else {
            uint32_t v = rdReg(*this, 0, bits);
            if (bits == 8) wr8(a, (uint8_t)v); else if (bits == 16) wr16(a, (uint16_t)v); else wr32(a, v);
        }
        return;
    }
    case 0xA4: case 0xA5: case 0xAA: case 0xAB: {   // movs / stos
        int bits = (op & 1) ? vb : 8;
        int step = bits / 8 * (DF ? -1 : 1);
        uint32_t cnt = (rep || repne) ? reg[1] : 1;
        for (uint32_t i = 0; i < cnt; ++i) {
            if (op <= 0xA5) {
                uint32_t v = bits == 8 ? rd8(reg[6]) : (bits == 16 ? rd16(reg[6]) : rd32(reg[6]));
                if (bits == 8) wr8(reg[7], (uint8_t)v); else if (bits == 16) wr16(reg[7], (uint16_t)v); else wr32(reg[7], v);
                reg[6] += step;
            } else {
                uint32_t v = reg[0];
                if (bits == 8) wr8(reg[7], (uint8_t)v); else if (bits == 16) wr16(reg[7], (uint16_t)v); else wr32(reg[7], v);
            }
            reg[7] += step;
        }
        if (rep || repne) reg[1] = 0;
        return;
    }
    case 0xA8: alu(*this, 4, get8(*this, 0), fetch8(*this), 8); return;
    case 0xA9: alu(*this, 4, rdReg(*this, 0, vb), opsz16 ? fetch16(*this) : fetch32(*this), vb); return;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set8(*this, op & 7, (uint8_t)fetch8(*this));
        return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        wrReg(*this, op & 7, vb, opsz16 ? fetch16(*this) : fetch32(*this));
        return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        ModRM m = decode(*this, fs);
        int bits = (op & 1) ? vb : 8;
        uint32_t n = (op <= 0xC1) ? fetch8(*this) : ((op <= 0xD1) ? 1 : (reg[1] & 0xff));
        uint32_t v = shiftOp(*this, m.reg, rdOp(*this, m, bits), n, bits);
        wrOp(*this, m, bits, v);
        return;
    }
    case 0xC2: {
        uint32_t n = fetch16(*this);
        eip = pop32(*this);
        reg[4] += n;
        return;
    }
    case 0xC3: eip = pop32(*this); return;
    case 0xC6: case 0xC7: {
        ModRM m = decode(*this, fs);
        int bits = (op == 0xC6) ? 8 : vb;
        uint32_t imm = bits == 8 ? fetch8(*this) : (bits == 16 ? fetch16(*this) : fetch32(*this));
        wrOp(*this, m, bits, imm);
        return;
    }
    case 0xC9: reg[4] = reg[5]; reg[5] = pop32(*this); return;
    case 0xCC: fault("int3");
    case 0xFC: DF = false; return;
    case 0xFD: DF = true; return;
    case 0xE8: case 0xE9: case 0xEB: {
        int32_t d = (op == 0xEB) ? s8(fetch8(*this)) : (int32_t)fetch32(*this);
        uint32_t target = eip + (uint32_t)d;
        if (op == 0xE8) {
            push32(*this, eip);
        }
        auto it = g_hooks.find(target);
        if (it != g_hooks.end()) {
            // for a call the return address was pushed above; for a tail jump it is already on the stack
            it->second.fn(*this);
            eip = pop32(*this);
            reg[4] += (uint32_t)it->second.pop;
            return;
        }
        eip = target;
        return;
    }
    case 0xF6: case 0xF7: {
        ModRM m = decode(*this, fs);
        int bits = (op == 0xF6) ? 8 : vb;
        uint32_t v = rdOp(*this, m, bits);
        switch (m.reg) {
        case 0: case 1: alu(*this, 4, v, bits == 8 ? fetch8(*this) : (bits == 16 ? fetch16(*this) : fetch32(*this)), bits); return;
        case 2: wrOp(*this, m, bits, ~v); return;
        case 3: {
            uint32_t r = alu(*this, 5, 0, v, bits);
            CF = (v & mk(bits)) != 0;
            wrOp(*this, m, bits, r);
            return;
        }
        case 4: {   // mul
            if (bits == 32) {
                uint64_t r = (uint64_t)reg[0] * v;
                reg[0] = (uint32_t)r; reg[2] = (uint32_t)(r >> 32);
                CF = OF = reg[2] != 0;
            } else fault("mul8/16");
            return;
        }
        case 5: {   // imul
            if (bits == 32) {
                int64_t r = (int64_t)(int32_t)reg[0] * (int32_t)v;
                reg[0] = (uint32_t)r; reg[2] = (uint32_t)((uint64_t)r >> 32);
                CF = OF = (r != (int32_t)r);
            } else fault("imul8/16");
            return;
        }
        case 6: {   // div
            if (bits != 32) fault("div8/16");
            if (v == 0) fault("divide by zero");
            uint64_t n = ((uint64_t)reg[2] << 32) | reg[0];
            reg[0] = (uint32_t)(n / v);
            reg[2] = (uint32_t)(n % v);
            return;
        }
        default: {  // idiv
            if (bits != 32) fault("idiv8/16");
            int32_t d = (int32_t)v;
            if (d == 0) fault("divide by zero");
            int64_t n = (int64_t)(((uint64_t)reg[2] << 32) | reg[0]);
            reg[0] = (uint32_t)(int32_t)(n / d);
            reg[2] = (uint32_t)(int32_t)(n % d);
            return;
        }
        }
    }
    case 0xFE: case 0xFF: {
        ModRM m = decode(*this, fs);
        int bits = (op == 0xFE) ? 8 : vb;
        switch (m.reg) {
        case 0: case 1: {
            bool cf = CF;
            uint32_t v = alu(*this, m.reg == 0 ? 0 : 5, rdOp(*this, m, bits), 1, bits);
            CF = cf;
            wrOp(*this, m, bits, v);
            return;
        }
        case 2: case 4: {   // call / jmp r/m32
            uint32_t target = rdOp(*this, m, 32);
            if (m.reg == 2) {
                push32(*this, eip);
            }
            auto it = g_hooks.find(target);
            if (it != g_hooks.end()) {
                it->second.fn(*this);
                eip = pop32(*this);
                reg[4] += (uint32_t)it->second.pop;
                return;
            }
            eip = target;
            return;
        }
        case 6: push32(*this, rdOp(*this, m, 32)); return;
        default: fault("FF /7");
        }
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        ModRM m = decode(*this, fs);
        if (!m.isReg) {
            switch (op) {
            case 0xD8: case 0xDC: {
                long double v = (op == 0xD8) ? (long double)rdF(m.addr) : (long double)*(double*)ptr(m.addr, 8);
                long double a = st[0];
                switch (m.reg) {
                case 0: st[0] = a + v; break;
                case 1: st[0] = a * v; break;
                case 2: case 3: CF = a < v; ZF = a == v; PF = false; if (m.reg == 3) fpop(*this); break;
                case 4: st[0] = a - v; break;
                case 5: st[0] = v - a; break;
                case 6: st[0] = a / v; break;
                default: st[0] = v / a; break;
                }
                return;
            }
            case 0xD9:
                switch (m.reg) {
                case 0: fpush(*this, rdF(m.addr)); return;
                case 2: { float f = (float)st[0]; memcpy(ptr(m.addr, 4), &f, 4); return; }
                case 3: { float f = (float)st[0]; memcpy(ptr(m.addr, 4), &f, 4); fpop(*this); return; }
                case 5: return;     // fldcw
                case 7: wr16(m.addr, 0x037f); return;   // fnstcw
                default: fault("D9 mem");
                }
            case 0xDB:
                switch (m.reg) {
                case 0: fpush(*this, (long double)(int32_t)rd32(m.addr)); return;
                case 2: case 3: wr32(m.addr, (uint32_t)(int32_t)llrintl(st[0])); if (m.reg == 3) fpop(*this); return;
                case 5: {   // fld m80
                    uint8_t b[10]; memcpy(b, ptr(m.addr, 10), 10);
                    int64_t mant; memcpy(&mant, b, 8);
                    int ex = (b[8] | (b[9] << 8)) & 0x7fff;
                    bool sgn = b[9] & 0x80;
                    long double v = ldexpl((long double)(uint64_t)mant, ex - 16383 - 63);
                    fpush(*this, sgn ? -v : v);
                    return;
                }
                case 7: {   // fstp m80
                    long double v = fpop(*this);
                    int ex; long double fr = frexpl(v < 0 ? -v : v, &ex);
                    uint64_t mant = v == 0 ? 0 : (uint64_t)ldexpl(fr, 64);
                    uint8_t b[10]; memcpy(b, &mant, 8);
                    int e = v == 0 ? 0 : ex - 1 + 16383;
                    b[8] = e & 0xff; b[9] = ((e >> 8) & 0x7f) | (v < 0 ? 0x80 : 0);
                    memcpy(ptr(m.addr, 10), b, 10);
                    return;
                }
                default: fault("DB mem");
                }
            case 0xDD:
                switch (m.reg) {
                case 0: { double d; memcpy(&d, ptr(m.addr, 8), 8); fpush(*this, d); return; }
                case 2: case 3: { double d = (double)st[0]; memcpy(ptr(m.addr, 8), &d, 8); if (m.reg == 3) fpop(*this); return; }
                default: fault("DD mem");
                }
            case 0xDF:
                switch (m.reg) {
                case 0: fpush(*this, (long double)(int16_t)rd16(m.addr)); return;
                case 5: { int64_t i; memcpy(&i, ptr(m.addr, 8), 8); fpush(*this, (long double)i); return; }
                case 7: { int64_t i = llrintl(st[0]); memcpy(ptr(m.addr, 8), &i, 8); fpop(*this); return; }
                default: fault("DF mem");
                }
            default: fault("x87 mem");
            }
        }
        // register forms
        int i = m.rm;
        switch (op) {
        case 0xD8: case 0xDC: case 0xDE: {
            long double &d = (op == 0xD8) ? st[0] : st[i];
            long double s = (op == 0xD8) ? st[i] : st[0];
            long double a = d;
            int sel = m.reg;
            if (op != 0xD8) { // DC/DE swap the sub/div direction
                if (sel == 4) sel = 5; else if (sel == 5) sel = 4; else if (sel == 6) sel = 7; else if (sel == 7) sel = 6;
            }
            switch (sel) {
            case 0: d = a + s; break;
            case 1: d = a * s; break;
            case 4: d = a - s; break;
            case 5: d = s - a; break;
            case 6: d = a / s; break;
            case 7: d = s / a; break;
            default: fault("x87 cmp reg");
            }
            if (op == 0xDE) fpop(*this);
            return;
        }
        case 0xD9: {
            uint32_t modrm = 0xC0 | (m.reg << 3) | m.rm;
            if (m.reg == 0) { fpush(*this, st[i]); return; }
            if (m.reg == 1) { std::swap(st[0], st[i]); return; }
            switch (modrm) {
            case 0xE0: st[0] = -st[0]; return;
            case 0xE1: st[0] = fabsl(st[0]); return;
            case 0xE8: fpush(*this, 1.0L); return;
            case 0xEE: fpush(*this, 0.0L); return;
            case 0xFA: st[0] = sqrtl(st[0]); return;
            case 0xFC: st[0] = nearbyintl(st[0]); return;
            case 0xD0: return;
            default: break;
            }
            break;
        }
        case 0xDD:
            if (m.reg == 3) { st[i] = st[0]; fpop(*this); return; }
            if (m.reg == 0) { return; }     // ffree
            if (m.reg == 4 || m.reg == 5) {  // fucom / fucomp
                long double a = st[0], b = st[i];
                CF = a < b; ZF = a == b; PF = false;
                if (m.reg == 5) fpop(*this);
                return;
            }
            break;
        case 0xDF:
            if (m.reg == 4 && m.rm == 0) {   // fnstsw ax
                long double a = st[0], b = st[1];
                (void)b;
                reg[0] = (reg[0] & 0xffff0000u) | 0x0000;
                return;
            }
            if (m.reg == 5 || m.reg == 6) {  // fucomip / fcomip
                long double a = st[0], b = st[i];
                CF = a < b; ZF = a == b; PF = (a != a || b != b);
                fpop(*this);
                return;
            }
            break;
        case 0xDB:
            if (m.reg == 5 || m.reg == 6) {  // fucomi / fcomi
                long double a = st[0], b = st[i];
                CF = a < b; ZF = a == b; PF = (a != a || b != b);
                return;
            }
            break;
        default: break;
        }
        fault("x87 reg form");
    }
    default:
        fault("unimplemented opcode");
    }

twobyte: {
    uint32_t op2 = fetch8(*this);
    if (op2 >= 0x80 && op2 <= 0x8F) {
        int32_t d = (int32_t)fetch32(*this);
        if (cond(*this, op2 & 15)) eip += (uint32_t)d;
        return;
    }
    if (op2 >= 0x90 && op2 <= 0x9F) {
        ModRM m = decode(*this, fs);
        wrOp(*this, m, 8, cond(*this, op2 & 15) ? 1 : 0);
        return;
    }
    if (op2 >= 0x40 && op2 <= 0x4F) {
        ModRM m = decode(*this, fs);
        uint32_t v = rdOp(*this, m, vb);
        if (cond(*this, op2 & 15)) wrReg(*this, m.reg, vb, v);
        return;
    }
    ModRM m;
    switch (op2) {
    case 0xAF: {
        m = decode(*this, fs);
        int64_t r = (int64_t)(int32_t)reg[m.reg] * (int32_t)rdOp(*this, m, 32);
        reg[m.reg] = (uint32_t)r;
        CF = OF = (r != (int32_t)r);
        setSZP(*this, (uint32_t)r, 32);
        return;
    }
    case 0xB6: case 0xB7: {
        m = decode(*this, fs);
        wrReg(*this, m.reg, 32, rdOp(*this, m, op2 == 0xB6 ? 8 : 16));
        return;
    }
    case 0xBE: case 0xBF: {
        m = decode(*this, fs);
        uint32_t v = rdOp(*this, m, op2 == 0xBE ? 8 : 16);
        wrReg(*this, m.reg, 32, op2 == 0xBE ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v);
        return;
    }
    case 0x0B: fault("ud2");
    case 0xA2: reg[0] = reg[1] = reg[2] = reg[3] = 0; return;   // cpuid
    case 0x1F: decode(*this, fs); return;                        // nop r/m
    case 0xBA: {    // bt/bts.. imm8 (only bt)
        m = decode(*this, fs);
        uint32_t v = rdOp(*this, m, 32);
        uint32_t bit = fetch8(*this) & 31;
        CF = (v >> bit) & 1;
        return;
    }
    default:
        break;
    }

    // SSE
    m = decode(*this, fs);
    auto rdX = [&](int bytes) -> Xmm {
        Xmm x;
        memset(&x, 0, sizeof x);
        if (m.isReg) x = xmm[m.rm];
        else memcpy(&x, ptr(m.addr, (uint32_t)bytes), (size_t)bytes);
        return x;
    };
    int pre = rep ? 1 : (repne ? 2 : (opsz16 ? 3 : 0));   // 0 ps, 1 ss, 2 sd, 3 pd
    Xmm& d = xmm[m.reg];
    switch (op2) {
    case 0x10: {
        if (pre == 1) { if (m.isReg) d.f[0] = xmm[m.rm].f[0]; else { memset(&d, 0, 16); d.f[0] = rdF(m.addr); } }
        else if (pre == 2) { if (m.isReg) d.d[0] = xmm[m.rm].d[0]; else { memset(&d, 0, 16); d.q[0] = rd64(m.addr); } }
        else d = rdX(16);
        return;
    }
    case 0x11: {
        if (pre == 1) { if (m.isReg) xmm[m.rm].f[0] = d.f[0]; else memcpy(ptr(m.addr, 4), &d.f[0], 4); }
        else if (pre == 2) { if (m.isReg) xmm[m.rm].d[0] = d.d[0]; else memcpy(ptr(m.addr, 8), &d.d[0], 8); }
        else if (m.isReg) xmm[m.rm] = d; else memcpy(ptr(m.addr, 16), &d, 16);
        return;
    }
    case 0x12: if (m.isReg) d.q[0] = xmm[m.rm].q[1]; else d.q[0] = rd64(m.addr); return;   // movlps/movlpd (movhlps)
    case 0x13: memcpy(ptr(m.addr, 8), &d.q[0], 8); return;
    case 0x16: if (m.isReg) d.q[1] = xmm[m.rm].q[0]; else d.q[1] = rd64(m.addr); return;   // movhps/movhpd
    case 0x17: memcpy(ptr(m.addr, 8), &d.q[1], 8); return;
    case 0x14: {    // unpcklps / unpcklpd
        Xmm s = rdX(16);
        if (pre == 3) { d.q[1] = s.q[0]; }
        else { float a0 = d.f[0], a1 = d.f[1], b0 = s.f[0], b1 = s.f[1]; d.f[0] = a0; d.f[1] = b0; d.f[2] = a1; d.f[3] = b1; }
        return;
    }
    case 0x28: d = rdX(16); return;
    case 0x29: if (m.isReg) xmm[m.rm] = d; else memcpy(ptr(m.addr, 16), &d, 16); return;
    case 0x2A: {
        int32_t v = (int32_t)rdOp(*this, m, 32);
        if (pre == 1) d.f[0] = (float)v; else d.d[0] = (double)v;
        return;
    }
    case 0x2C: case 0x2D: {
        double v = 0;
        if (pre == 1) v = m.isReg ? xmm[m.rm].f[0] : rdF(m.addr);
        else v = m.isReg ? xmm[m.rm].d[0] : *(double*)ptr(m.addr, 8);
        int32_t r;
        if (v != v || v >= 2147483648.0 || v < -2147483648.0) r = (int32_t)0x80000000;
        else r = (op2 == 0x2C) ? (int32_t)v : (int32_t)llrint(v);
        reg[m.reg] = (uint32_t)r;
        return;
    }
    case 0x2E: case 0x2F: {
        double a, b;
        if (pre == 3) { a = d.d[0]; b = m.isReg ? xmm[m.rm].d[0] : *(double*)ptr(m.addr, 8); }
        else { a = d.f[0]; b = m.isReg ? xmm[m.rm].f[0] : rdF(m.addr); }
        OF = SF = false;
        if (a != a || b != b) { ZF = PF = CF = true; }
        else { PF = false; ZF = a == b; CF = a < b; }
        return;
    }
    case 0x51: {
        if (pre == 1) d.f[0] = sqrtf(m.isReg ? xmm[m.rm].f[0] : rdF(m.addr));
        else if (pre == 2) d.d[0] = sqrt(m.isReg ? xmm[m.rm].d[0] : *(double*)ptr(m.addr, 8));
        else fault("sqrtps");
        return;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: {
        Xmm s = rdX(16);
        for (int i = 0; i < 2; ++i) {
            uint64_t a = d.q[i], b = s.q[i];
            d.q[i] = op2 == 0x54 ? (a & b) : op2 == 0x55 ? (~a & b) : op2 == 0x56 ? (a | b) : (a ^ b);
        }
        return;
    }
    case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        auto f = [&](double a, double b) -> double {
            switch (op2) {
            case 0x58: return a + b;
            case 0x59: return a * b;
            case 0x5C: return a - b;
            case 0x5D: return a < b ? a : b;
            case 0x5E: return a / b;
            default: return a > b ? a : b;
            }
        };
        auto ff = [&](float a, float b) -> float {
            switch (op2) {
            case 0x58: return a + b;
            case 0x59: return a * b;
            case 0x5C: return a - b;
            case 0x5D: return a < b ? a : b;
            case 0x5E: return a / b;
            default: return a > b ? a : b;
            }
        };
        if (pre == 1) { float b = m.isReg ? xmm[m.rm].f[0] : rdF(m.addr); d.f[0] = ff(d.f[0], b); }
        else if (pre == 2) { double b = m.isReg ? xmm[m.rm].d[0] : *(double*)ptr(m.addr, 8); d.d[0] = f(d.d[0], b); }
        else if (pre == 0) { Xmm s = rdX(16); for (int i = 0; i < 4; ++i) d.f[i] = ff(d.f[i], s.f[i]); }
        else { Xmm s = rdX(16); for (int i = 0; i < 2; ++i) d.d[i] = f(d.d[i], s.d[i]); }
        return;
    }
    case 0x5A: {
        if (pre == 0) { Xmm s = rdX(8); double a = s.f[0], b = s.f[1]; d.d[0] = a; d.d[1] = b; }
        else if (pre == 3) { Xmm s = rdX(16); float a = (float)s.d[0], b = (float)s.d[1]; d.f[0] = a; d.f[1] = b; d.q[1] = 0; }
        else if (pre == 1) { d.d[0] = m.isReg ? xmm[m.rm].f[0] : rdF(m.addr); }
        else { d.f[0] = (float)(m.isReg ? xmm[m.rm].d[0] : *(double*)ptr(m.addr, 8)); }
        return;
    }
    case 0x5B: {
        Xmm s = rdX(16);
        if (pre == 0) { for (int i = 0; i < 4; ++i) d.f[i] = (float)(int32_t)s.u[i]; }
        else if (pre == 3) { for (int i = 0; i < 4; ++i) d.u[i] = (uint32_t)(int32_t)lrintf(s.f[i]); }
        else { for (int i = 0; i < 4; ++i) d.u[i] = (uint32_t)(int32_t)s.f[i]; }
        return;
    }
    case 0xE6: {
        if (pre == 1) { Xmm s = rdX(8); double a = (int32_t)s.u[0], b = (int32_t)s.u[1]; d.d[0] = a; d.d[1] = b; }
        else if (pre == 3) { Xmm s = rdX(16); int32_t a = (int32_t)s.d[0], b = (int32_t)s.d[1]; d.u[0] = (uint32_t)a; d.u[1] = (uint32_t)b; d.q[1] = 0; }
        else fault("E6");
        return;
    }
    case 0x6E: { uint32_t v = rdOp(*this, m, 32); memset(&d, 0, 16); d.u[0] = v; return; }
    case 0x7E: {
        if (pre == 1) { Xmm s = rdX(8); d.q[0] = s.q[0]; d.q[1] = 0; }
        else { wrOp(*this, m, 32, d.u[0]); }
        return;
    }
    case 0xD6: if (m.isReg) { xmm[m.rm].q[0] = d.q[0]; xmm[m.rm].q[1] = 0; } else memcpy(ptr(m.addr, 8), &d.q[0], 8); return;
    case 0x6F: d = rdX(16); return;
    case 0x7F: if (m.isReg) xmm[m.rm] = d; else memcpy(ptr(m.addr, 16), &d, 16); return;
    case 0xEF: { Xmm s = rdX(16); d.q[0] ^= s.q[0]; d.q[1] ^= s.q[1]; return; }
    case 0x73: { uint32_t n = fetch8(*this); (void)n; fault("psrlq"); }
    case 0xC6: {    // shufps / shufpd
        Xmm s = rdX(16);
        uint32_t imm = fetch8(*this);
        if (pre == 0) {
            Xmm r;
            r.f[0] = d.f[imm & 3]; r.f[1] = d.f[(imm >> 2) & 3]; r.f[2] = s.f[(imm >> 4) & 3]; r.f[3] = s.f[(imm >> 6) & 3];
            d = r;
        } else {
            double a = d.d[imm & 1], b = s.d[(imm >> 1) & 1];
            d.d[0] = a; d.d[1] = b;
        }
        return;
    }
    case 0x70: {    // pshufd
        Xmm s = rdX(16);
        uint32_t imm = fetch8(*this);
        Xmm r;
        for (int i = 0; i < 4; ++i) r.u[i] = s.u[(imm >> (i * 2)) & 3];
        d = r;
        return;
    }
    default: {
        logf("x86: unimplemented 0F %02X at %08x", op2, cx.start);
        fault("unimplemented 0F opcode");
    }
    }
}
}

void X86::run(uint32_t stopEip)
{
    while (eip != stopEip) {
        step();
    }
}

uint32_t X86::call(uint32_t fn, uint32_t thisPtr, std::initializer_list<uint32_t> args)
{
    uint32_t savedEsp = reg[4];
    uint32_t savedEip = eip;
    std::vector<uint32_t> a(args);
    for (size_t i = a.size(); i-- > 0;) {
        push32(*this, a[i]);
    }
    push32(*this, RET_MAGIC);
    reg[1] = thisPtr;
    eip = fn;
    run(RET_MAGIC);
    uint32_t r = reg[0];
    reg[4] = savedEsp;
    eip = savedEip;
    return r;
}