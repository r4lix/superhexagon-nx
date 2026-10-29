// Small x86-32 interpreter (the subset MSVC emits for Super Hexagon's game code) used to run the original
// machine code of the menu/HUD functions directly against the shared state blob.
//   guest address space:   [IMG_BASE ..)   the exe image (.text/.rdata/.data) loaded from data/image.bin
//                          [GBASE ..)      the state blob G (guest address = GBASE + blob offset)
#pragma once
#include <stdint.h>
#include <functional>
#include <initializer_list>
#include <string>

constexpr uint32_t GBASE = 0;   // guest address == blob offset (the exe image occupies 0x401000..0x570000, never used by the blob)

class X86 {
public:
    uint32_t reg[8];          // eax ecx edx ebx esp ebp esi edi
    uint32_t eip = 0;
    bool CF = false, ZF = false, SF = false, OF = false, PF = false, DF = false;
    union Xmm { float f[4]; double d[2]; uint32_t u[4]; uint64_t q[2]; };
    Xmm xmm[8];
    long double st[8];
    int ftop = 0;             // number of values on the x87 stack
    uint32_t fsBase = 0;
    uint64_t instrCount = 0;
    uint32_t hist[32];
    int histPos = 0;

    // intercepted guest addresses -> native handlers.  Handler reads its args with arg(i) (i-th stack dword
    // after the return address) and returns through eax / st0 / xmm0.  `pop` = bytes the callee removes.
    struct Hook { std::function<void(X86&)> fn; int pop; };
    void hook(uint32_t addr, int popBytes, std::function<void(X86&)> fn);

    uint32_t arg(int i) { return rd32(reg[4] + 4 + 4 * (uint32_t)i); }
    uint32_t rd32(uint32_t a);
    uint16_t rd16(uint32_t a);
    uint8_t rd8(uint32_t a);
    uint64_t rd64(uint32_t a);
    void wr32(uint32_t a, uint32_t v);
    void wr16(uint32_t a, uint16_t v);
    void wr8(uint32_t a, uint8_t v);
    void wr64(uint32_t a, uint64_t v);
    float rdF(uint32_t a);
    uint8_t* ptr(uint32_t a, uint32_t n = 1);

    // call a guest function (cdecl-style stack args pushed right to left; ecx = `thisPtr` for thiscall)
    uint32_t call(uint32_t fn, uint32_t thisPtr, std::initializer_list<uint32_t> args);
    void run(uint32_t stopEip);
    [[noreturn]] void fault(const char* why);
    bool trace = false;

private:
    std::string describe();
    void step();
};

X86& x86();
bool x86_load_image(const std::string& path);       // data/image.bin
void x86_init(uint32_t stackBlobOffset, uint32_t stackSize, uint32_t tebBlobOffset);
