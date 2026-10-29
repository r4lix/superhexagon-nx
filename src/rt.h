// Runtime support for the converted game code: the flat state blob, its allocator, MSVC-layout strings.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string>

extern uint8_t* G;
#define A(T, off) (*(T*)(G + (uint32_t)(off)))

// ---- layout of the blob -------------------------------------------------------------------
// The original's application object is APP_SIZE bytes; everything inside it is addressed with
// offsets relative to it (the game object lives at +0x10, UI at +0x101b8, audio at +0x22bb8).
constexpr int APP = 0x100;
constexpr int APP_SIZE = 0x40d00;
constexpr int GAME = APP + 0x10;
constexpr int UI = APP + 0x101b8;
constexpr int AUDIO = APP + 0x22bb8;
constexpr uint32_t ROD_BASE = APP + APP_SIZE + 0x100;
constexpr uint32_t ROD_SIZE = 0x10000;
constexpr uint32_t ARENA_BASE = 0x600000;   // above the guest image window (0x400000..0x570000)
constexpr uint32_t BLOB_SIZE = 64u * 1024 * 1024;

inline int Ai(int off) { return A(int32_t, APP + off); }

// ---- strings with the original's (MSVC x86) std::string layout ----------------------------
std::string bstr(int off);
void bstr_set(int off, const std::string& s);
int bstr_len(int off);

// ---- allocator ---------------------------------------------------------------------------
int arena_alloc(uint32_t n);
void arena_free(int off);

// ---- lifecycle ---------------------------------------------------------------------------
void rt_init();
void rt_set_double(int off, double v);

// ---- logging -----------------------------------------------------------------------------
void logf(const char* fmt, ...);

// time since start in milliseconds
int rt_ms();
