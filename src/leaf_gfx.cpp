// Window / drawing leaves of the original (FNA3D/openFrameworks wrappers) on top of gfx.cpp.
#include "rt.h"
#include "hand_decl.h"
#include "gfx.h"
#include <string.h>

static uint8_t g_batchCol[4] = {255, 255, 255, 255};
static float g_musicVol = 1.0f, g_soundVol = 1.0f;
static bool g_quit = false;

float rt_music_volume() { return g_musicVol; }
float rt_sound_volume() { return g_soundVol; }
bool rt_quit_requested() { return g_quit; }
void rt_request_quit() { g_quit = true; }

// ---- shape batch -------------------------------------------------------------------------------
int FUN_0042f880(int, float, int, int) { return 1; }   // handle of the single batch

void FUN_0042fe20(int, UD4 r, UD4 g, UD4 b, UD4 a)
{
    g_batchCol[0] = (uint8_t)r;
    g_batchCol[1] = (uint8_t)g;
    g_batchCol[2] = (uint8_t)b;
    g_batchCol[3] = (uint8_t)a;
}

static float lo(UD8 w) { uint32_t u = (uint32_t)w.v; float f; memcpy(&f, &u, 4); return f; }
static float hi(UD8 w) { uint32_t u = (uint32_t)(w.v >> 32); float f; memcpy(&f, &u, 4); return f; }

void FUN_0042fae0(int, UD8 p1, UD4, UD8 p2, UD4, UD8 p3, UD4, int)
{
    float xy[6] = {lo(p1), hi(p1), lo(p2), hi(p2), lo(p3), hi(p3)};
    gfx_tri(xy, g_batchCol);
}

void FUN_0042fd90(int) {}             // begin layers
void FUN_0042fdd0(int) { gfx_flush(); }  // draw whatever was queued

// ---- window state ---------------------------------------------------------------------------------
void FUN_004386c0(int r, int g, int b) { gfx_set_clear(r, g, b); }
void FUN_004389c0(uint8_t r, uint8_t g, uint8_t b) { gfx_set_color(r, g, b); }
void FUN_00438760(float x, float y, float w, float h)
{
    // The original pillarboxes 16:9 with two full-height black bars drawn last; skip them so the stage
    // background fills the whole screen (the 16:10 HUD stays centred).
    int sw, sh;
    gfx_size(&sw, &sh);
    if (y <= 0.5f && h >= (float)sh - 1.f && w < (float)sw * 0.5f) {
        return;
    }
    gfx_rect(x, y, w, h);
}
bool FUN_00438710(void) { return true; }          // "fullscreen"
void FUN_004389f0(uint8_t) {}                      // vsync switch: always on
void FUN_004387a0(void) { g_quit = true; }
int FUN_004387c0(void) { int w, h; gfx_size(&w, &h); return h; }   // height (see FUN_00424610)
int FUN_004388a0(void) { int w, h; gfx_size(&w, &h); return w; }   // width
void FUN_004388d0(void) {}
void FUN_00438af0(void) {}
void FUN_00439170(void) {}
void FUN_00438b20(float v) { g_musicVol = v; }
void FUN_00438b40(float v) { g_soundVol = v; }
