#include "gfx.h"
#include "rt.h"
#include <SDL.h>
#include <string.h>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#include "third_party/imgui/imgui.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

// ---- minimal GL 2 / GLES 2 loader --------------------------------------------------------------
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef int GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef unsigned char GLboolean;
typedef ptrdiff_t GLsizeiptr;

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_LINEAR 0x2601
#define GL_ALPHA 0x1906
#define GL_RGBA 0x1908
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_SCISSOR_TEST 0x0C11

#define GLFUNCS(X) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLuint)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetAttribLocation, (GLuint, const GLchar*)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glGenTextures, (GLsizei, GLuint*)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))

#define DECL(ret, name, args) static ret(*name) args;
GLFUNCS(DECL)
#undef DECL

static bool loadGL()
{
#define LOAD(ret, name, args) \
    name = (ret(*) args)SDL_GL_GetProcAddress(#name); \
    if (!name) { logf("missing GL function %s", #name); return false; }
    GLFUNCS(LOAD)
#undef LOAD
    return true;
}

// ---- state ----------------------------------------------------------------------------------
namespace {
SDL_Window* g_win = nullptr;
SDL_GLContext g_ctx = nullptr;
GLuint g_prog = 0, g_whiteTex = 0;
GLint g_aPos = -1, g_aCol = -1, g_aUV = -1, g_uView = -1, g_uTex = -1, g_uUseTex = -1, g_uFlip = -1;
int g_filter = 0;
float g_flip = 1.f;
int g_w = 1280, g_h = 720;
float g_clear[3] = {0, 0, 0};
uint8_t g_col[4] = {255, 255, 255, 255};

struct Vtx { float x, y, u, v; uint8_t r, g, b, a; };
std::vector<Vtx> g_verts;
GLuint g_curTex = 0;

struct Font {
    bool ok = false;
    int px = 0;
    stbtt_bakedchar cdata[96];
    GLuint tex = 0;
    float lineAscent = 0;
    std::vector<uint8_t> ttf;
};
Font g_fonts[6];
const int ATLAS = 1024;

GLuint compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        logf("shader error: %s", log);
    }
    return s;
}
}

bool gfx_init(int w, int h, bool fullscreen)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        logf("SDL video init failed: %s", SDL_GetError());
        return false;
    }
#ifdef __SWITCH__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    g_win = SDL_CreateWindow("Super Hexagon", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!g_win) {
        logf("window: %s", SDL_GetError());
        return false;
    }
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) {
        logf("GL context: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetSwapInterval(getenv("FASTRUN") ? 0 : 1);
    if (!loadGL()) {
        return false;
    }
    const char* vs =
        "attribute vec2 aPos; attribute vec2 aUV; attribute vec4 aCol;\n"
        "uniform vec2 uView; uniform float uFlip; varying vec2 vUV; varying vec4 vCol;\n"
        "void main(){ vec2 p = aPos / uView * 2.0 - 1.0; gl_Position = vec4(p.x * uFlip, -p.y, 0.0, 1.0); vUV = aUV; vCol = aCol; }\n";
    const char* fs =
        "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
        "varying vec2 vUV; varying vec4 vCol; uniform sampler2D uTex; uniform float uUseTex;\n"
        "void main(){ vec4 c = vCol; if (uUseTex > 0.5) { c.a *= texture2D(uTex, vUV).a; } gl_FragColor = c; }\n";
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, v);
    glAttachShader(g_prog, f);
    glLinkProgram(g_prog);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        logf("program link failed");
        return false;
    }
    g_aPos = glGetAttribLocation(g_prog, "aPos");
    g_aUV = glGetAttribLocation(g_prog, "aUV");
    g_aCol = glGetAttribLocation(g_prog, "aCol");
    g_uView = glGetUniformLocation(g_prog, "uView");
    g_uTex = glGetUniformLocation(g_prog, "uTex");
    g_uUseTex = glGetUniformLocation(g_prog, "uUseTex");
    g_uFlip = glGetUniformLocation(g_prog, "uFlip");
    uint8_t white = 255;
    glGenTextures(1, &g_whiteTex);
    glBindTexture(GL_TEXTURE_2D, g_whiteTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 1, 1, 0, GL_ALPHA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    SDL_GL_GetDrawableSize(g_win, &g_w, &g_h);
    return true;
}

void gfx_shutdown()
{
    if (g_ctx) {
        SDL_GL_DeleteContext(g_ctx);
    }
    if (g_win) {
        SDL_DestroyWindow(g_win);
    }
}

void gfx_set_alpha(int a) { g_col[3] = (uint8_t)a; }
int text_px(int idx) { return idx >= 1 && idx <= 5 ? g_fonts[idx].px : 0; }
void gfx_set_filter(int mode) { g_filter = mode; }
void gfx_set_mirror(bool on) { g_flip = on ? -1.f : 1.f; }
void gfx_set_vsync(bool on) { SDL_GL_SetSwapInterval(on ? 1 : 0); }

unsigned gfx_make_alpha_texture(const unsigned char* px, int w, int h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void gfx_render_imgui(ImDrawData* dd)
{
    gfx_flush();
    const int savedFilter = g_filter;
    const float savedFlip = g_flip;
    g_filter = 0;
    g_flip = 1.f;
    glEnable(GL_SCISSOR_TEST);
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (int i = 0; i < cl->CmdBuffer.Size; ++i) {
            const ImDrawCmd& cmd = cl->CmdBuffer[i];
            if (cmd.UserCallback || cmd.ElemCount == 0) {
                continue;
            }
            float x0 = cmd.ClipRect.x, y0 = cmd.ClipRect.y, x1 = cmd.ClipRect.z, y1 = cmd.ClipRect.w;
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }
            glScissor((GLint)x0, (GLint)((float)g_h - y1), (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
            g_curTex = (GLuint)(uintptr_t)cmd.GetTexID();
            for (unsigned k = 0; k < cmd.ElemCount; ++k) {
                const ImDrawVert& v = cl->VtxBuffer[cl->IdxBuffer[cmd.IdxOffset + k] + cmd.VtxOffset];
                Vtx o;
                o.x = v.pos.x;
                o.y = v.pos.y;
                o.u = v.uv.x;
                o.v = v.uv.y;
                o.r = (uint8_t)(v.col & 0xff);
                o.g = (uint8_t)((v.col >> 8) & 0xff);
                o.b = (uint8_t)((v.col >> 16) & 0xff);
                o.a = (uint8_t)(v.col >> 24);
                g_verts.push_back(o);
            }
            gfx_flush();
        }
    }
    glDisable(GL_SCISSOR_TEST);
    g_curTex = 0;
    g_filter = savedFilter;
    g_flip = savedFlip;
}

void gfx_size(int* w, int* h)
{
    SDL_GL_GetDrawableSize(g_win, &g_w, &g_h);
    *w = g_w;
    *h = g_h;
}

void gfx_set_clear(int r, int g, int b)
{
    g_clear[0] = r / 255.f;
    g_clear[1] = g / 255.f;
    g_clear[2] = b / 255.f;
}

void gfx_set_color(int r, int g, int b)
{
    g_col[0] = (uint8_t)r;
    g_col[1] = (uint8_t)g;
    g_col[2] = (uint8_t)b;
    g_col[3] = 255;
}

extern bool g_edgeHudReset;
void gfx_begin_frame()
{
    g_edgeHudReset = true;
    SDL_GL_GetDrawableSize(g_win, &g_w, &g_h);
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(g_clear[0], g_clear[1], g_clear[2], 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gfx_flush()
{
    if (g_verts.empty()) {
        return;
    }
    glUseProgram(g_prog);
    glUniform2f(g_uView, (float)g_w, (float)g_h);
    glUniform1i(g_uTex, 0);
    glUniform1f(g_uUseTex, g_curTex ? 1.f : 0.f);
    glUniform1f(g_uFlip, g_flip);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_curTex ? g_curTex : g_whiteTex);
    const Vtx* base = g_verts.data();
    glEnableVertexAttribArray((GLuint)g_aPos);
    glEnableVertexAttribArray((GLuint)g_aUV);
    glEnableVertexAttribArray((GLuint)g_aCol);
    glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, 0, sizeof(Vtx), &base->x);
    glVertexAttribPointer((GLuint)g_aUV, 2, GL_FLOAT, 0, sizeof(Vtx), &base->u);
    glVertexAttribPointer((GLuint)g_aCol, 4, GL_UNSIGNED_BYTE, 1, sizeof(Vtx), &base->r);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_verts.size());
    g_verts.clear();
}

void gfx_end_frame()
{
    gfx_flush();
    SDL_GL_SwapWindow(g_win);
}

static void applyFilter(uint8_t* c)
{
    int r = c[0], g = c[1], b = c[2];
    switch (g_filter) {
    case 1:   // invert
        c[0] = (uint8_t)(255 - r);
        c[1] = (uint8_t)(255 - g);
        c[2] = (uint8_t)(255 - b);
        break;
    case 2: { // grayscale
        int y = (r * 77 + g * 150 + b * 29) >> 8;
        c[0] = c[1] = c[2] = (uint8_t)y;
        break;
    }
    case 3: { // sepia
        int y = (r * 77 + g * 150 + b * 29) >> 8;
        int rr = y + 40, gg = y + 12, bb = y - 30;
        c[0] = (uint8_t)(rr > 255 ? 255 : rr);
        c[1] = (uint8_t)(gg > 255 ? 255 : gg);
        c[2] = (uint8_t)(bb < 0 ? 0 : bb);
        break;
    }
    case 4:   // red/blue swap
        c[0] = (uint8_t)b;
        c[2] = (uint8_t)r;
        break;
    case 5: { // high contrast
        for (int i = 0; i < 3; ++i) {
            int v = (c[i] - 128) * 2 + 128;
            c[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        break;
    }
    }
}

static void pushTri(float x0, float y0, float u0, float v0, float x1, float y1, float u1, float v1,
                    float x2, float y2, float u2, float v2, const uint8_t* c, GLuint tex)
{
    if (tex != g_curTex) {
        gfx_flush();
        g_curTex = tex;
    }
    uint8_t f[4] = {c[0], c[1], c[2], c[3]};
    if (g_filter) {
        applyFilter(f);
    }
    Vtx a = {x0, y0, u0, v0, f[0], f[1], f[2], f[3]};
    Vtx b = {x1, y1, u1, v1, f[0], f[1], f[2], f[3]};
    Vtx d = {x2, y2, u2, v2, f[0], f[1], f[2], f[3]};
    g_verts.push_back(a);
    g_verts.push_back(b);
    g_verts.push_back(d);
}

// Widescreen: the original keeps its HUD inside the centred 16:10 area. While the in-run HUD is drawn (a black
// polygon in the top strip marks it) the top-left / top-right boxes are pushed out to the screen edges.
bool g_edgeHudReset = false;
static bool g_hudScope = false, g_edgeHud = false;

void gfx_hud_scope(bool on)
{
    g_hudScope = on;
}

static float hudDx(float x, float ymax)
{
    if (g_edgeHudReset) {
        g_edgeHudReset = false;
        g_edgeHud = false;
    }
    if (!g_edgeHud || ymax > g_h * 0.15f) {
        return 0.f;
    }
    float pad = ((float)g_w - (float)g_h * 1.6f) * 0.5f;
    if (pad < 1.f) {
        return 0.f;
    }
    return x < (float)g_w * 0.5f ? -pad : pad;
}

void gfx_tri(const float* p, const uint8_t* rgba)
{
    float ymax = p[1] > p[3] ? (p[1] > p[5] ? p[1] : p[5]) : (p[3] > p[5] ? p[3] : p[5]);
    if (g_edgeHudReset) {
        g_edgeHudReset = false;
        g_edgeHud = false;
    }
    if (g_hudScope && !g_edgeHud && rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] > 0 && ymax <= g_h * 0.15f) {
        g_edgeHud = true;
    }
    float dx = hudDx((p[0] + p[2] + p[4]) / 3.f, ymax);
    pushTri(p[0] + dx, p[1], 0, 0, p[2] + dx, p[3], 0, 0, p[4] + dx, p[5], 0, 0, rgba, 0);
}

void gfx_rect(float x, float y, float w, float h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    x += hudDx(x + w * 0.5f, y + h);
    pushTri(x, y, 0, 0, x + w, y, 0, 0, x, y + h, 0, 0, g_col, 0);
    pushTri(x + w, y, 0, 0, x + w, y + h, 0, 0, x, y + h, 0, 0, g_col, 0);
}

// ---- text ------------------------------------------------------------------------------------
void text_load(int idx, int sizePx, const char* path)
{
    if (idx < 1 || idx > 5) {
        return;
    }
    Font& f = g_fonts[idx];
    if (f.ok && f.px == sizePx) {
        return;
    }
    if (f.ttf.empty()) {
        FILE* fp = fopen(path, "rb");
        if (!fp) {
            logf("cannot open font %s", path);
            return;
        }
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        f.ttf.resize((size_t)n);
        if (fread(f.ttf.data(), 1, (size_t)n, fp) != (size_t)n) {
            logf("short read on font");
        }
        fclose(fp);
    }
    // ofTrueTypeFont asks FreeType for `size` points at 96 dpi
    float pxf = (float)sizePx * 96.0f / 72.0f;
    std::vector<uint8_t> bmp((size_t)ATLAS * ATLAS);
    int r = stbtt_BakeFontBitmap(f.ttf.data(), 0, pxf, bmp.data(), ATLAS, ATLAS, 32, 96, f.cdata);
    if (r <= 0) {
        logf("font atlas too small for size %d", sizePx);
    }
    if (!f.tex) {
        glGenTextures(1, &f.tex);
    }
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS, ATLAS, 0, GL_ALPHA, GL_UNSIGNED_BYTE, bmp.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f.px = sizePx;
    f.ok = true;
    stbtt_fontinfo fi;
    stbtt_InitFont(&fi, f.ttf.data(), 0);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    f.lineAscent = asc * stbtt_ScaleForPixelHeight(&fi, pxf);
}

static const float kLetterSpacing = 0.98f;   // ofTrueTypeFont::letterSpacing set by the original
static const float kSpaceSize = 0.4f;

float text_width(int idx, const std::string& s)
{
    if (idx < 1 || idx > 5 || !g_fonts[idx].ok) {
        return 0;
    }
    Font& f = g_fonts[idx];
    float w = 0;
    for (unsigned char c : s) {
        if (c == ' ') {
            w += f.cdata['p' - 32].xadvance * kLetterSpacing * kSpaceSize;
        } else if (c >= 33 && c < 128) {
            w += f.cdata[c - 32].xadvance * kLetterSpacing;
        }
    }
    return w;
}

float text_height(int idx, const std::string& s)
{
    if (idx < 1 || idx > 5 || !g_fonts[idx].ok) {
        return 0;
    }
    Font& f = g_fonts[idx];
    float h = 0;
    for (unsigned char c : s) {
        if (c >= 33 && c < 128) {
            const stbtt_bakedchar& b = f.cdata[c - 32];
            float gh = (float)(b.y1 - b.y0);
            if (gh > h) {
                h = gh;
            }
        }
    }
    return h;
}

void text_draw(int idx, float x, float y, const std::string& s)
{
    if (idx < 1 || idx > 5 || !g_fonts[idx].ok) {
        return;
    }
    Font& f = g_fonts[idx];
    x += hudDx(x, y);
    float cx = x;
    for (unsigned char c : s) {
        if (c == ' ') {
            cx += f.cdata['p' - 32].xadvance * kLetterSpacing * kSpaceSize;
            continue;
        }
        if (c < 33 || c >= 128) {
            continue;
        }
        stbtt_aligned_quad q;
        float px = cx, py = y;
        stbtt_GetBakedQuad(f.cdata, ATLAS, ATLAS, c - 32, &px, &py, &q, 1);
        pushTri(q.x0, q.y0, q.s0, q.t0, q.x1, q.y0, q.s1, q.t0, q.x0, q.y1, q.s0, q.t1, g_col, f.tex);
        pushTri(q.x1, q.y0, q.s1, q.t0, q.x1, q.y1, q.s1, q.t1, q.x0, q.y1, q.s0, q.t1, g_col, f.tex);
        cx += f.cdata[c - 32].xadvance * kLetterSpacing;
    }
}

void gfx_screenshot(const char* path)
{
    gfx_flush();
    int w = g_w, h = g_h;
    std::vector<uint8_t> px((size_t)w * h * 4), flipped((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; ++y) {
        memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    }
    stbi_write_png(path, w, h, 4, flipped.data(), w * 4);
}