// Core runtime: blob, allocator, strings, randomness, small leaf functions used by the converted code.
#include "rt.h"
#include "hand_decl.h"
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <SDL.h>
#include <ctime>

uint8_t* G = nullptr;
long double g_st0 = 0;

// ---- logging -------------------------------------------------------------------------------
std::string g_logPath;   // set by main on Switch (next to the data folder)
static std::vector<std::string> g_logRing;
static size_t g_logRingPos = 0;

int rt_log_copy(std::vector<std::string>& out)
{
    out.clear();
    size_t n = g_logRing.size();
    for (size_t k = 0; k < n; ++k) {
        out.push_back(g_logRing[(g_logRingPos + k) % n]);
    }
    return (int)n;
}

void logf(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stderr);
    fputc('\n', stderr);
    if (g_logRing.size() < 200) {
        g_logRing.push_back(line);
    } else {
        g_logRing[g_logRingPos] = line;
        g_logRingPos = (g_logRingPos + 1) % g_logRing.size();
    }
#ifdef __SWITCH__
    static FILE* lf = fopen((g_logPath.empty() ? std::string("sdmc:/switch/superhexagon/log.txt") : g_logPath).c_str(), "w");
    if (lf) {
        fputs(line, lf);
        fputc('\n', lf);
        fflush(lf);
    }
#endif
}

// ---- allocator (size-class free lists inside the blob) ---------------------------------------
namespace {
uint32_t g_arenaTop = ARENA_BASE;
std::vector<int> g_free[32];

int classOf(uint32_t n)
{
    int c = 4;               // 16 bytes
    while ((1u << c) < n + 16) {
        ++c;
    }
    return c;
}
}

int arena_alloc(uint32_t n)
{
    if (n == 0) {
        n = 1;
    }
    int c = classOf(n);
    int off;
    if (!g_free[c].empty()) {
        off = g_free[c].back();
        g_free[c].pop_back();
    } else {
        uint32_t sz = 1u << c;
        if (g_arenaTop + sz > BLOB_SIZE) {
            logf("arena exhausted");
            abort();
        }
        off = (int)g_arenaTop + 16;
        g_arenaTop += sz;
        A(uint32_t, off - 16) = (uint32_t)c;
    }
    memset(G + off, 0, 1u << c);   // generous: the original's allocations are uninitialised anyway
    return off;
}

void arena_free(int off)
{
    if (off < (int)ARENA_BASE + 16) {
        return;
    }
    int c = (int)A(uint32_t, off - 16);
    if (c >= 4 && c < 32) {
        g_free[c].push_back(off);
    }
}

int FUN_004eaf92(uint32_t size) { return arena_alloc(size); }
void FUN_004eafc2(int ptr) { arena_free(ptr); }

void* bmemcpy(int d, int s, uint32_t n)
{
    if ((uint32_t)d >= BLOB_SIZE || (uint32_t)s >= BLOB_SIZE || n > 0x1000000) { logf("bad memcpy d=%x s=%x n=%u", d, s, n); *(volatile int*)0 = 0; }
    return memcpy(G + d, G + s, n);
}
void* bmemmove(int d, int s, uint32_t n) { return memmove(G + d, G + s, n); }
void* bmemset(int d, int v, uint32_t n) { return memset(G + d, v, n); }

[[noreturn]] void FUN_00401240(void) { logf("std::length_error"); abort(); }
[[noreturn]] void FUN_00404ad0(void) { logf("std::length_error"); abort(); }
[[noreturn]] void FUN_00404ae0(void) { logf("std::bad_alloc"); abort(); }
uint32_t sh_invalid_parameter() { logf("invalid parameter"); abort(); return 0; }
float g_dtScale = 1.0f;
int DAT_0055e8fc = 0;

int _eh_vector_constructor_iterator_(int base, uint32_t size, int count, int, int)
{
    if (size == 0x18) {                       // std::string elements: empty, SSO capacity 15
        for (int i = 0; i < count; ++i) {
            int o = base + i * 0x18;
            A(uint8_t, o) = 0;
            A(uint32_t, o + 0x10) = 0;
            A(uint32_t, o + 0x14) = 0xf;
        }
    }
    return base;
}
int _eh_vector_destructor_iterator_(int base, uint32_t, int, int) { return base; }
int64_t sh_alldiv(uint32_t alo, int32_t ahi, uint32_t blo, int32_t bhi)
{
    int64_t a = ((int64_t)ahi << 32) | alo, b = ((int64_t)bhi << 32) | blo;
    return b ? a / b : 0;
}
int64_t sh_allrem(uint32_t alo, int32_t ahi, uint32_t blo, int32_t bhi)
{
    int64_t a = ((int64_t)ahi << 32) | alo, b = ((int64_t)bhi << 32) | blo;
    return b ? a % b : 0;
}

// ---- MSVC std::string in the blob --------------------------------------------------------
std::string bstr(int off)
{
    uint32_t len = A(uint32_t, off + 0x10);
    uint32_t cap = A(uint32_t, off + 0x14);
    const char* p = cap > 0xf ? (const char*)(G + A(uint32_t, off)) : (const char*)(G + off);
    return std::string(p, len);
}

int bstr_len(int off) { return (int)A(uint32_t, off + 0x10); }

void bstr_set(int off, const std::string& s)
{
    uint32_t cap = A(uint32_t, off + 0x14);
    if (cap > 0xf) {
        int heap = (int)A(uint32_t, off);
        if (s.size() <= cap) {
            memcpy(G + heap, s.c_str(), s.size() + 1);
            A(uint32_t, off + 0x10) = (uint32_t)s.size();
            return;
        }
        arena_free(heap);
    } else if (s.size() <= 0xf) {
        memcpy(G + off, s.c_str(), s.size() + 1);
        A(uint32_t, off + 0x10) = (uint32_t)s.size();
        A(uint32_t, off + 0x14) = 0xf;
        return;
    }
    int heap = arena_alloc((uint32_t)s.size() + 1);
    memcpy(G + heap, s.c_str(), s.size() + 1);
    A(uint32_t, off) = (uint32_t)heap;
    A(uint32_t, off + 0x10) = (uint32_t)s.size();
    A(uint32_t, off + 0x14) = (uint32_t)s.size();
}

void FUN_004045e0(int str)
{
    uint32_t cap = A(uint32_t, str + 0x14);
    if (cap > 0xf) {
        arena_free((int)A(uint32_t, str));
    }
    A(uint32_t, str + 0x10) = 0;
    A(uint32_t, str + 0x14) = 0xf;
    A(uint8_t, str) = 0;
}

void NameBackspace(int str, int placeholder)
{
    std::string s = bstr(str);
    std::string ph = placeholder ? bstr(placeholder) : std::string("PLAYER");
    if (s == ph || s.empty()) {
        s.clear();
    } else {
        s.pop_back();
    }
    bstr_set(str, s);
}

// ---- read-only data ----------------------------------------------------------------------
struct RodEntry { uint32_t va; uint32_t len; const uint8_t* data; };
extern const RodEntry g_rodTable[];

static int g_rodOff[2048];
static uint32_t g_rodVa[2048];
static int g_rodCount = 0;

int rod(uint32_t va)
{
    for (int i = 0; i < g_rodCount; ++i) {
        if (g_rodVa[i] == va) {
            return g_rodOff[i];
        }
    }
    logf("rod: unknown data address 0x%x", va);
    return (int)ROD_BASE;
}

// ---- randomness / x87 --------------------------------------------------------------------
static uint32_t g_seed = 1;
static int msvc_rand()
{
    g_seed = g_seed * 214013u + 2531011u;
    return (int)((g_seed >> 16) & 0x7fff);
}

long double FUN_00438900(float range)
{
    int r = msvc_rand();
    float v = (float)r * range * 3.0517578e-05f;
    g_st0 = v;
    return v;
}

// ---- time ----------------------------------------------------------------------------------
static uint32_t g_t0 = 0;
int rt_ms() { return (int)(SDL_GetTicks() - g_t0); }
int FUN_004387b0(void) { return rt_ms(); }
long double FUN_00431120(void) { return 1.0L; }
static double g_measured = 0;
long double FUN_00431130(void) { return g_measured; }
void FUN_00431180(...) {}
void FUN_004311a0(...) {}       // frame function stores g_measured itself (see main.cpp)
void rt_set_measured(double v) { g_measured = v; }

// fmod helpers: the original passes the double in two ints followed by the modulus
static double fromParts(uint32_t lo, uint32_t hi)
{
    uint64_t u = ((uint64_t)hi << 32) | lo;
    double d;
    memcpy(&d, &u, 8);
    return d;
}
int FUN_004311c0(uint32_t lo, uint32_t hi, int n)
{
    return (int)floor(fmod(fromParts(lo, hi), (double)n));
}
long double FUN_00431200(uint32_t lo, uint32_t hi, int n)
{
    float r = (float)fmod(fromParts(lo, hi), (double)n);
    g_st0 = r;
    return r;
}

// ---- trivial leaves ------------------------------------------------------------------------
uint32_t FUN_004326a0(uint32_t, uint32_t, uint32_t) { return 0; }
void FUN_00432900(void) {}
int FUN_004326f0(int p) { return p; }
void FUN_004329b0(int) {}
int FUN_00432730(int p) { return p; }
int FUN_00432880(int p) { return p; }
int FUN_0043ef00(int p) { return p; }
int FUN_00430130(int) { return 0; }
int FUN_004288d0(int) { return 0; }

void FUN_0043a2e0(int vec, UD4 x, UD4 y, UD4 z)
{
    A(uint32_t, vec) = x.v;
    A(uint32_t, vec + 4) = y.v;
    A(uint32_t, vec + 8) = z.v;
}

// vector<int>::reserve(n)
void FUN_0042dbe0(int vec, uint32_t n)
{
    int oldBegin = A(int, vec);
    int oldEnd = A(int, vec + 4);
    int nb = arena_alloc(n * 4);
    uint32_t used = oldBegin ? (uint32_t)(oldEnd - oldBegin) : 0;
    if (used) {
        memcpy(G + nb, G + oldBegin, used);
    }
    if (oldBegin) {
        arena_free(oldBegin);
    }
    A(int, vec) = nb;
    A(int, vec + 4) = nb + (int)used;
    A(int, vec + 8) = nb + (int)(n * 4);
}

// ---- trig tables stored as vector<double> in the game object ------------------------------
static void fillVec(int vec, int count, double step, bool useCos)
{
    int mem = arena_alloc((uint32_t)count * 8);
    for (int k = 0; k < count; ++k) {
        double a = ((double)(k * 2) * 3.141592653589793) / step;
        A(double, mem + k * 8) = useCos ? cos(a) : sin(a);
    }
    A(int, vec) = mem;
    A(int, vec + 4) = mem + count * 8;
    A(int, vec + 8) = mem + count * 8;
}

void FillGameTrig(int game)
{
    fillVec(game + 0x3633 * 4, 360, 360.0, false);   // sin, 1 degree steps
    fillVec(game + 0x3636 * 4, 360, 360.0, true);    // cos
}

// the UI/render object keeps 1-degree and 0.1-degree tables as well (original FUN_00424840)
void FUN_00424840(int obj)
{
    fillVec(obj + 0x00, 360, 360.0, false);
    fillVec(obj + 0x18, 360, 360.0, true);
    fillVec(obj + 0x0c, 3600, 3600.0, false);
    fillVec(obj + 0x24, 3600, 3600.0, true);
    A(uint32_t, obj + 0x30) = 0;
    A(uint32_t, obj + 0x38) = 0;
    A(uint32_t, obj + 0x34) = 0;
}

// the credits page lists the original's play-testers (FUN_004150b0 pushed them into a vector<string>)
void CreditNames(int game)
{
    static const char* const names[] = {
        "ADAM SALTSMAN", "ALAN HAZELDEN", "ANDI MCCLURE", "ANDY MOORE", "BENNETT FODDY", "BRANDON BOYER",
        "BRANDON ROTHWEILER", "BRANDON WILLIAMSON", "CHRISTIAN DE CHENU", "COLIN NORTHWAY", "DAVID DING", "IAN SNYDER",
        "JASPER BYRNE", "JEREMY PENNER", "JOHN POLSON", "KEVIN MESSMAN", "NAOMI CLARK", "HAYDEN SCOTT-BARON",
        "NICOLAI TROSHINSKY", "RUARI O'SULLIVAN", "SOPHIE HOULDEN", "STEPHEN LAVELLE"};
    const int n = (int)(sizeof names / sizeof names[0]);
    int mem = arena_alloc((uint32_t)n * 0x18);
    memset(G + mem, 0, (size_t)n * 0x18);
    for (int i = 0; i < n; ++i) {
        A(uint32_t, mem + i * 0x18 + 0x14) = 15;      // SSO capacity before bstr_set grows it
        bstr_set(mem + i * 0x18, names[i]);
    }
    A(int, game + 0x576c) = mem;
    A(int, game + 0x5770) = mem + n * 0x18;
    A(int, game + 0x5774) = mem + n * 0x18;
}

// ---- lifecycle -----------------------------------------------------------------------------
void rt_init()
{
    G = (uint8_t*)calloc(BLOB_SIZE, 1);
    if (!G) {
        logf("out of memory allocating %u MB state blob", BLOB_SIZE >> 20);
        exit(2);
    }
    g_t0 = SDL_GetTicks();
    g_seed = (uint32_t)time(nullptr);
    uint32_t cur = ROD_BASE;
    for (const RodEntry* e = g_rodTable; e->va; ++e) {
        memcpy(G + cur, e->data, e->len);
        g_rodVa[g_rodCount] = e->va;
        g_rodOff[g_rodCount] = (int)cur;
        ++g_rodCount;
        cur += (e->len + 15) & ~15u;
    }
}

void rt_set_double(int off, double v) { A(double, off) = v; }

// ---- std::string pieces the generated code calls -----------------------------------------------
int FUN_00404ba0(int str, uint32_t src, uint32_t n)
{
    std::string s = bstr(str);
    s.append((const char*)(G + src), n);
    bstr_set(str, s);
    return str;
}

int FUN_00415c00(int str, uint32_t pos, uint32_t src, uint32_t n)
{
    std::string s = bstr(str);
    if (pos > s.size()) {
        pos = (uint32_t)s.size();
    }
    s.insert(pos, (const char*)(G + src), n);
    bstr_set(str, s);
    return str;
}

void FUN_00417ae0(int str, uint8_t c)
{
    std::string s = bstr(str);
    s.push_back((char)c);
    bstr_set(str, s);
}

void FUN_004145d0(int) {}

#include <map>
int lit(const char* s)
{
    static std::map<std::string, int> cache;
    auto it = cache.find(s);
    if (it != cache.end()) {
        return it->second;
    }
    size_t n = strlen(s) + 1;
    int off = arena_alloc((uint32_t)n);
    memcpy(G + off, s, n);
    cache[s] = off;
    return off;
}
