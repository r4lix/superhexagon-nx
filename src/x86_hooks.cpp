// Native implementations of everything the interpreted original code calls outside its own logic
// (text/shape drawing prims, allocator, timing, randomness), plus the entry points used by the port.
#include "x86.h"
#include "rt.h"
#include "gfx.h"
#include "hand_decl.h"
#include "extras.h"
#include <math.h>
#include <string.h>

namespace {
constexpr uint32_t STACK_SIZE = 0x40000;
bool g_ready = false;

// the original ships keyboard prompt strings; show Joy-Con wording instead (HUD copies them out of the exe's
// string literals, so it is simplest to translate when they are drawn / measured)
const char* joyconPrompt(const std::string& s)
{
    static const char* const map[][2] = {
        {"ESC - RETURN TO MENU", "B - RETURN TO MENU"}, {"PRESS SPACE TO START", "PRESS A TO START"},
        {"ESC - STAGE SELECT", "B - STAGE SELECT"}, {"PRESS LEFT AND RIGHT KEYS", "PRESS LEFT AND RIGHT"},
        {"PRESS SPACE TO CONTINUE", "PRESS A TO CONTINUE"}, {"PRESS SPACE TO RETRY", "PRESS A TO RETRY"},
        {"ESC - QUIT", "B - QUIT"}, {"ESC - CANCEL", "B - CANCEL"}, {"PRESS SPACE TO CONFIRM", "PRESS A TO CONFIRM"},
        {"C - CLEAR RECORDS", "X - CLEAR RECORDS"}, {"TAB - HIGH SCORES", "TAP - HIGH SCORES"},   // opens the local high scores
        {"PRESS SPACE TO SELECT", "PRESS A TO SELECT"}};
    for (auto& m : map) {
        if (s == m[0]) return m[1];
    }
    return nullptr;
}
std::string gstr(X86& c, uint32_t a)
{
    uint32_t size = c.rd32(a + 16), cap = c.rd32(a + 20);
    uint32_t data = cap > 0xf ? c.rd32(a) : a;
    std::string s((const char*)c.ptr(data, size ? size : 1), size);
    if (const char* r = joyconPrompt(s)) {
        return r;
    }
    return s;
}
void gstrFree(X86& c, uint32_t a)
{
    if (c.rd32(a + 20) > 0xf) {
        arena_free((int)(c.rd32(a) - GBASE));
    }
}
inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// layout metrics in the ui object (blob offset `ui`)
inline double scX(uint32_t ui) { return A(double, ui + 0x20); }
inline double scY(uint32_t ui) { return A(double, ui + 0x28); }
inline double offX(uint32_t ui) { return A(double, ui + 0x30); }
inline double offY(uint32_t ui) { return A(double, ui + 0x38); }

int widthUnits(uint32_t ui, const std::string& s, int size)
{
    if (size < 1 || size > 5) size = 1;
    return (int)(text_width(size, s) / scX(ui));
}

// y offset the original adds before scaling, per font size
int yAdj(int size) { return size == 2 ? 5 : 4; }

void drawText(uint32_t ui, int x, int y, const std::string& s, int r, int g, int b, bool center, int size)
{
    if (s.find("STEAM UNAVAILABLE") != std::string::npos) {
        static int last = -10000;
        if (rt_ms() - last > 500) logf("LEADERBOARD SCREEN at t=%d ms", rt_ms());
        last = rt_ms();
    }
    if (s == "Y - HIGH SCORES") {
        extras_note_scores_bar();
    }
    gfx_set_color(clamp255(r), clamp255(g), clamp255(b));
    if (size < 1 || size > 5) size = 1;
    if (center) {
        x += A(int, ui + 8) - widthUnits(ui, s, size) / 2;
    }
    int px = (int)((double)(int)((double)x * scX(ui)) + offX(ui));
    int py = (int)((double)(int)((double)(y + yAdj(size)) * scY(ui)) + offY(ui));
    float h = text_height(size, s);
    text_draw(size, (float)px, (float)(py + (int)h), s);
}
}

void x86_install_hooks()
{
    X86& c = x86();
    // ---- allocator / CRT -------------------------------------------------------------------------------
    c.hook(0x4eaf92, 0, [](X86& m) { m.reg[0] = GBASE + (uint32_t)arena_alloc(m.arg(0)); });
    c.hook(0x4eafc2, 0, [](X86& m) { uint32_t p = m.arg(0); if (p >= GBASE) arena_free((int)(p - GBASE)); });

    // ---- text ---------------------------------------------------------------------------------------------
    c.hook(0x422c90, 0x30, [](X86& m) {          // draw string, font 1
        uint32_t ui = m.reg[1] - GBASE, sa = m.reg[4] + 4 + 8;
        std::string s = gstr(m, sa);
        drawText(ui, (int)m.arg(0), (int)m.arg(1), s, (int)m.arg(8), (int)m.arg(9), (int)m.arg(10), (m.arg(11) & 0xff) != 0, 1);
        gstrFree(m, sa);
    });
    c.hook(0x41c400, 0x34, [](X86& m) {          // draw string, font 2..5
        uint32_t ui = m.reg[1] - GBASE, sa = m.reg[4] + 4 + 8;
        std::string s = gstr(m, sa);
        drawText(ui, (int)m.arg(0), (int)m.arg(1), s, (int)m.arg(8), (int)m.arg(9), (int)m.arg(10), (m.arg(11) & 0xff) != 0, (int)m.arg(12));
        gstrFree(m, sa);
    });
    c.hook(0x422e20, 0x34, [](X86& m) {          // right-aligned variant of the above
        uint32_t ui = m.reg[1] - GBASE, sa = m.reg[4] + 4 + 8;
        std::string s = gstr(m, sa);
        int size = (int)m.arg(12);
        drawText(ui, (int)m.arg(0) - widthUnits(ui, s, size), (int)m.arg(1), s, (int)m.arg(8), (int)m.arg(9), (int)m.arg(10),
                 (m.arg(11) & 0xff) != 0, size);
        gstrFree(m, sa);
    });
    c.hook(0x422ee0, 0x2c, [](X86& m) {          // right-aligned, font 1
        uint32_t ui = m.reg[1] - GBASE, sa = m.reg[4] + 4 + 8;
        std::string s = gstr(m, sa);
        drawText(ui, (int)m.arg(0) - widthUnits(ui, s, 1), (int)m.arg(1), s, (int)m.arg(8), (int)m.arg(9), (int)m.arg(10), false, 1);
        gstrFree(m, sa);
    });
    c.hook(0x422930, 0x1c, [](X86& m) {          // string width in layout units
        uint32_t ui = m.reg[1] - GBASE, sa = m.reg[4] + 4;
        std::string s = gstr(m, sa);
        m.reg[0] = (uint32_t)widthUnits(ui, s, (int)m.arg(6));
        gstrFree(m, sa);
    });
    c.hook(0x41f8c0, 0x1c, [](X86& m) {          // string height in pixels
        uint32_t sa = m.reg[4] + 4;
        std::string s = gstr(m, sa);
        int size = (int)m.arg(6);
        if (size < 1 || size > 5) size = 1;
        m.reg[0] = (uint32_t)(int)text_height(size, s);
        gstrFree(m, sa);
    });

    // ---- draw state / shapes ------------------------------------------------------------------------------
    c.hook(0x4389c0, 0, [](X86& m) { gfx_set_color((int)(m.arg(0) & 0xff), (int)(m.arg(1) & 0xff), (int)(m.arg(2) & 0xff)); });
    c.hook(0x438760, 0, [](X86& m) { gfx_rect(fbits(m.arg(0)), fbits(m.arg(1)), fbits(m.arg(2)), fbits(m.arg(3))); });
    c.hook(0x42fe20, 0x10, [](X86& m) {
        FUN_0042fe20(1, m.arg(0), m.arg(1), m.arg(2), m.arg(3));
    });
    c.hook(0x42fae0, 0x28, [](X86& m) {
        UD8 p1((uint64_t)m.arg(0) | ((uint64_t)m.arg(1) << 32));
        UD8 p2((uint64_t)m.arg(3) | ((uint64_t)m.arg(4) << 32));
        UD8 p3((uint64_t)m.arg(6) | ((uint64_t)m.arg(7) << 32));
        FUN_0042fae0(1, p1, m.arg(2), p2, m.arg(5), p3, m.arg(8), (int)m.arg(9));
    });
    c.hook(0x438710, 0, [](X86& m) { m.reg[0] = 1; });
    // std::string copy through an ostringstream in the original: out = arg0, const string* = arg1 (cdecl)
    c.hook(0x402500, 0, [](X86& m) {
        uint32_t out = m.arg(0);
        std::string s = gstr(m, m.arg(1));
        A(uint32_t, out + 0x10) = 0;
        A(uint32_t, out + 0x14) = 0xf;
        A(uint8_t, out) = 0;
        bstr_set((int)out, s);
        m.reg[0] = out;
    });
    // std::string from int (ostringstream in the original): out = arg0, int* = arg1 (cdecl)
    c.hook(0x4023c0, 0, [](X86& m) {
        uint32_t out = m.arg(0);
        int32_t v = (int32_t)m.rd32(m.arg(1));
        A(uint32_t, out + 0x10) = 0;
        A(uint32_t, out + 0x14) = 0xf;
        A(uint8_t, out) = 0;
        bstr_set((int)out, std::to_string(v));
        m.reg[0] = out;
    });

    // ---- time / random ------------------------------------------------------------------------------------
    c.hook(0x4387b0, 0, [](X86& m) { m.reg[0] = (uint32_t)rt_ms(); });
    c.hook(0x438900, 0, [](X86& m) {            // float rand(float range) -> st0
        long double v = FUN_00438900(fbits(m.arg(0)));
        m.st[0] = 0;
        for (int i = 7; i > 0; --i) m.st[i] = m.st[i - 1];
        m.st[0] = v;
        if (m.ftop < 8) ++m.ftop;
    });
}

static void installThunks(const std::string& dataDir)
{
    X86& c = x86();
    FILE* f = fopen((dataDir + "/thunks.txt").c_str(), "r");
    if (!f) {
        return;
    }
    char name[512];
    unsigned va;
    while (fscanf(f, "%x %511s", &va, name) == 2) {
        std::string n = name;
        auto F = [&](X86& m, int idx) { return fbits(m.arg(idx)); };
        (void)F;
        if (n == "memcpy" || n == "memmove") {
            c.hook(va, 0, [](X86& m) {
                uint32_t d = m.arg(0), s = m.arg(1), n = m.arg(2);
                if (d >= 0x401000 && d < 0x4f0000) logf("MEMMOVE INTO CODE d=%08x s=%08x n=%u at eip=%08x ret=%08x", d, s, n, m.eip, m.rd32(m.reg[4]));
                if (n) memmove(m.ptr(d, n), m.ptr(s, n), n);
                m.reg[0] = d;
            });
        } else if (n == "memset") {
            c.hook(va, 0, [](X86& m) {
                uint32_t d = m.arg(0), n = m.arg(2);
                if (n) memset(m.ptr(d, n), (int)m.arg(1), n);
                m.reg[0] = d;
            });
        } else if (n == "memchr") {
            c.hook(va, 0, [](X86& m) {
                uint32_t s = m.arg(0), n = m.arg(2);
                m.reg[0] = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    if (m.rd8(s + i) == (uint8_t)m.arg(1)) { m.reg[0] = s + i; break; }
                }
            });
        } else if (n == "malloc") {
            c.hook(va, 0, [](X86& m) { m.reg[0] = GBASE + (uint32_t)arena_alloc(m.arg(0)); });
        } else if (n == "free") {
            c.hook(va, 0, [](X86& m) { uint32_t p = m.arg(0); if (p >= GBASE) arena_free((int)(p - GBASE)); });
        } else if (n == "floor") {
            c.hook(va, 0, [](X86& m) { double d = floor(*(double*)m.ptr(m.reg[4] + 4, 8)); m.xmm[0].d[0] = d; m.st[0] = d; });
        } else if (n == "_libm_sse2_sin_precise") {
            c.hook(va, 0, [](X86& m) { m.xmm[0].d[0] = sin(m.xmm[0].d[0]); });
        } else if (n == "_libm_sse2_cos_precise") {
            c.hook(va, 0, [](X86& m) { m.xmm[0].d[0] = cos(m.xmm[0].d[0]); });
        } else if (n == "_libm_sse2_tan_precise") {
            c.hook(va, 0, [](X86& m) { m.xmm[0].d[0] = tan(m.xmm[0].d[0]); });
        } else if (n == "_CIfmod") {
            c.hook(va, 0, [](X86& m) {      // st1 = x, st0 = y  ->  fmod(x, y)
                long double y = m.st[0], x = m.st[1];
                m.st[1] = fmodl(x, y);
                for (int i = 0; i < 7; ++i) m.st[i] = m.st[i + 1];
                if (m.ftop > 0) --m.ftop;
            });
        } else if (n == "_invalid_parameter_noinfo_noreturn" || n == "_Xlength_error" || n.find("_Xbad_alloc") != std::string::npos ||
                   n == "terminate" || n == "exit" || n.find("_Xlength_error") != std::string::npos) {
            c.hook(va, 0, [n](X86& m) { logf("guest called %s", n.c_str()); m.fault("fatal import"); });
        } else if (n == "SDL_GetTicks") {
            c.hook(va, 0, [](X86& m) { m.reg[0] = (uint32_t)rt_ms(); });
        } else {
            // everything else the original imports (atexit tables, SDL/FNA/FAudio, iostream bits): do nothing, return 0
            c.hook(va, 0, [n](X86& m) { static int cnt = 0; if (cnt++ < 40) logf("guest import ignored: %s", n.c_str()); m.reg[0] = 0; });
        }
    }
    fclose(f);
}

bool x86_boot(const std::string& dataDir)
{
    if (!x86_load_image(dataDir + "/image.bin")) {
        logf("missing %s/image.bin", dataDir.c_str());
        return false;
    }
    installThunks(dataDir);
    int stack = arena_alloc(STACK_SIZE);
    int teb = arena_alloc(0x1000);
    x86_init((uint32_t)stack, STACK_SIZE, (uint32_t)teb);
    x86_install_hooks();
    g_ready = true;
    return true;
}

// ---- entry points for the converted code -----------------------------------------------------------------------
void FUN_00405dd0(int game, int ui, int p)
{
    gfx_hud_scope(true);
    if (g_ready) x86().call(0x405dd0, GBASE + (uint32_t)game, {GBASE + (uint32_t)ui, GBASE + (uint32_t)p});
    gfx_hud_scope(false);
}
void FUN_00407b50(int game, int ui, int p)
{
    gfx_hud_scope(true);
    if (g_ready) x86().call(0x407b50, GBASE + (uint32_t)game, {GBASE + (uint32_t)ui, GBASE + (uint32_t)p});
    gfx_hud_scope(false);
}
