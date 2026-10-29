// Port-only extras: local high-score screen and the "super secret" Dear ImGui menu.
//
// Menu: Up Up Down Down Left Right Left Right B A on the D-pad (or hold L+R+ZL+ZR for a second;
// F10 on the keyboard build).  Everything in here only touches the state blob, the platform
// layer and the renderer - the converted game code is not modified except for the god-mode hook.
#include "extras.h"
#include "rt.h"
#include "gfx.h"
#include "hand_decl.h"
#include "cheats.h"
#include "third_party/imgui/imgui.h"
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>

bool input_btn(int b);
float input_axis(int a);
void rt_request_quit();
int text_px(int idx);
void gfx_set_alpha(int a);
int rt_log_copy(std::vector<std::string>& out);
void audio_shutdown();
void audio_set_halted(bool h);
void audio_set_rate(float r);
void input_clear_pressed();

namespace {
// ---- state ---------------------------------------------------------------------------------------
std::string g_base = ".";
bool g_menuOpen = false;
bool g_scoresOpen = false;
bool g_pauseOnOpen = true;
bool g_showFps = false;
bool g_swapLR = false;
bool g_mirror = false;
bool g_vsync = true;
int g_filter = 0;
float g_uiScale = 1.0f;
bool g_wantShot = false;
bool g_inited = false;
bool g_barSeen = false, g_barVisible = false;

// ---- own local high-score table (the original's table is only the Steam placeholder list) ----------
struct Score { int frames; char name[16]; };
Score g_hs[6][5];
int g_runBest = 0, g_runStall = 0, g_runMode = 0;
float g_runLast = 0;
bool g_runCommitted = true;
unsigned g_fontTex = 0;
std::vector<float> g_ft(120, 16.7f);
int g_ftPos = 0;
int g_forceTab = -1;
int g_memBase = 0x100;
int g_pokeOff = 0x2988, g_pokeVal = 0;
bool g_pokeFloat = false;
std::string g_status;
double g_statusUntil = 0;
double g_clock = 0;
float g_dt = 1.0f / 60.0f;

// konami: U U D D L R L R B A  (tokens: 1 up 2 down 3 left 4 right 5 b 6 a)
const int kKonami[10] = {1, 1, 2, 2, 3, 4, 3, 4, 5, 6};
int g_konamiPos = 0;
int g_chordFrames = 0;

const char* kModeNames[6] = {"HEXAGON", "HEXAGONER", "HEXAGONEST", "HYPER HEXAGON", "HYPER HEXAGONER", "HYPER HEXAGONEST"};
const char* kDiffNames[6] = {"HARD", "HARDER", "HARDEST", "HARDESTER", "HARDESTEST", "HARDESTESTER"};

// offsets relative to the application object
const int O_STATE = 0x4c, O_STAGE = 0x54b4, O_PAGE = 0x54f0, O_MENUSEL = 0x101a8, O_FRAMES = 0x2938;
const int O_PLAYER_ANGLE = 0x297c, O_MOVE_SPEED = 0x2984, O_WORLD_ROT = 0x1a0, O_DEATH = 0x2988;
const int O_BEST = GAME + 0x2988, O_TOP5 = GAME + 0x5aac, O_NAMES = GAME + 0x57dc;
const int O_UNLOCKED = GAME + 0x54b0, O_FINISHED = GAME + 0x54c8;
const int O_TUTORIAL_DONE = 0x5748;

void status(const char* fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_status = buf;
    g_statusUntil = g_clock + 4.0;
}

void saveCfg()
{
    FILE* f = fopen((g_base + "/extras.cfg").c_str(), "w");
    if (!f) {
        return;
    }
    fprintf(f, "filter=%d\nmirror=%d\nswaplr=%d\nvsync=%d\nfps=%d\nuiscale=%.2f\npauseonopen=%d\n", g_filter, g_mirror ? 1 : 0,
            g_swapLR ? 1 : 0, g_vsync ? 1 : 0, g_showFps ? 1 : 0, g_uiScale, g_pauseOnOpen ? 1 : 0);
    fclose(f);
}

void loadCfg()
{
    FILE* f = fopen((g_base + "/extras.cfg").c_str(), "r");
    if (!f) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        double v = atof(eq + 1);
        std::string k = line;
        if (k == "filter") g_filter = (int)v;
        else if (k == "mirror") g_mirror = v != 0;
        else if (k == "swaplr") g_swapLR = v != 0;
        else if (k == "vsync") g_vsync = v != 0;
        else if (k == "fps") g_showFps = v != 0;
        else if (k == "uiscale") g_uiScale = (float)v;
        else if (k == "pauseonopen") g_pauseOnOpen = v != 0;
    }
    fclose(f);
    if (g_uiScale < 0.5f || g_uiScale > 2.5f) {
        g_uiScale = 1.0f;
    }
}

void applyGfx()
{
    gfx_set_filter(g_filter);
    gfx_set_mirror(g_mirror);
    gfx_set_vsync(g_vsync && !getenv("FASTRUN"));
}

std::string fmtTime(int frames)
{
    // the game shows best times as SS:FF (seconds : frames within the second, 60 fps)
    if (frames < 0) {
        frames = 0;
    }
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d", frames / 60, frames % 60);
    return b;
}

void hsLoad()
{
    memset(g_hs, 0, sizeof g_hs);
    FILE* f = fopen((g_base + "/highscores.dat").c_str(), "rb");
    if (!f) {
        return;
    }
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == 0x31534853u) {
        if (fread(g_hs, sizeof g_hs, 1, f) != 1) {
            memset(g_hs, 0, sizeof g_hs);
        }
    }
    fclose(f);
}

void hsSave()
{
    FILE* f = fopen((g_base + "/highscores.dat").c_str(), "wb");
    if (!f) {
        return;
    }
    uint32_t magic = 0x31534853u;
    fwrite(&magic, 4, 1, f);
    fwrite(g_hs, sizeof g_hs, 1, f);
    fclose(f);
}

void hsCommit(int mode, int frames)
{
    if (mode < 0 || mode > 5 || frames < 30) {
        return;
    }
    Score* t = g_hs[mode];
    int pos = 5;
    for (int i = 0; i < 5; ++i) {
        if (frames > t[i].frames) {
            pos = i;
            break;
        }
    }
    if (pos >= 5) {
        return;
    }
    for (int i = 4; i > pos; --i) {
        t[i] = t[i - 1];
    }
    t[pos].frames = frames;
    std::string nm = bstr(GAME + 0xd8b0);
    if (nm.empty()) {
        nm = "PLAYER";
    }
    memset(t[pos].name, 0, sizeof t[pos].name);
    strncpy(t[pos].name, nm.c_str(), 15);
    hsSave();
}

// follow the run timer (float frame counter at +0x2924): when it stops advancing the run is over
void trackRun()
{
    const bool inGame = A(int, APP + 0x54b4) >= 0;
    float t = A(float, APP + 0x2924);
    if (!inGame) {
        g_runBest = 0;
        g_runStall = 0;
        g_runLast = 0;
        g_runCommitted = true;
        return;
    }
    if (t < g_runLast - 5.0f || g_runLast == 0.0f) {
        // new run started
        g_runCommitted = false;
        g_runBest = 0;
        g_runStall = 0;
    }
    if (t > g_runLast + 0.01f) {
        g_runStall = 0;
        g_runBest = (int)t;
        g_runMode = A(int, APP + 0x54a4) + (A(uint8_t, APP + 0x54f8) ? 3 : 0);
    } else if (!g_runCommitted && g_runBest > 0 && ++g_runStall >= 20) {
        g_runCommitted = true;
        hsCommit(g_runMode, g_runBest);
    }
    g_runLast = t;
}

// ---- local high scores --------------------------------------------------------------------------------
void drawScores()
{
    int w, h;
    gfx_size(&w, &h);
    const float s = (float)h / 640.0f;
    gfx_set_color(0, 0, 0);
    gfx_set_alpha(236);
    gfx_rect(0, 0, (float)w, (float)h);
    gfx_set_alpha(255);

    const int titleIdx = 2;
    const int rowIdx = 1;
    std::string title = "LOCAL HIGH SCORES";
    gfx_set_color(255, 255, 255);
    float tw = text_width(titleIdx, title);
    text_draw(titleIdx, ((float)w - tw) / 2, 58 * s, title);

    const int cols = 3;
    const float cellW = (float)w / cols;
    const float top = 100 * s;
    const float cellH = 240 * s;
    for (int m = 0; m < 6; ++m) {
        float cx = (float)(m % cols) * cellW + 28 * s;
        float cy = top + (float)(m / cols) * cellH;
        // best time of the mode (shown on the level select screen)
        int best = A(int, O_BEST + m * 4);
        if (g_hs[m][0].frames > best) {
            best = g_hs[m][0].frames;
        }
        gfx_set_color(255, 205, 70);
        text_draw(rowIdx, cx, cy + 20 * s, kModeNames[m]);
        gfx_set_color(170, 170, 170);
        std::string bt = std::string("BEST ") + (best > 0 ? fmtTime(best) : std::string("--:--"));
        text_draw(rowIdx, cx, cy + 46 * s, bt);
        for (int r = 0; r < 5; ++r) {
            int idx = m * 5 + r;
            (void)idx;
            int t = g_hs[m][r].frames;
            std::string name = g_hs[m][r].frames > 0 ? std::string(g_hs[m][r].name) : std::string("---");
            float y = cy + (80 + r * 30) * s;
            gfx_set_color(255, 255, 255);
            char rk[8];
            snprintf(rk, sizeof rk, "%d.", r + 1);
            text_draw(rowIdx, cx, y, rk);
            text_draw(rowIdx, cx + 36 * s, y, t > 0 ? fmtTime(t) : std::string("--:--"));
            const float nameX = cx + 36 * s + text_width(rowIdx, "00:00") + 14 * s;
            while (name.size() > 1 && nameX + text_width(rowIdx, name) > (float)(m % cols + 1) * cellW - 10 * s) {
                name.resize(name.size() - 1);
            }
            gfx_set_color(200, 200, 200);
            text_draw(rowIdx, nameX, y, name);
        }
    }
    gfx_set_color(200, 200, 200);
    std::string foot = "B - BACK";
    text_draw(rowIdx, ((float)w - text_width(rowIdx, foot)) / 2, (float)h - 22 * s, foot);
}

// ---- menu ------------------------------------------------------------------------------------------
void resetTop5()
{
    static const int defs[5] = {3600, 2700, 180, 120, 60};
    for (int m = 0; m < 6; ++m) {
        for (int r = 0; r < 5; ++r) {
            A(int, O_TOP5 + (m * 5 + r) * 4) = defs[r];
            bstr_set(O_NAMES + (m * 5 + r) * 0x18, "");
        }
    }
}

void tabCheats()
{
    cheats_tab();
    ImGui::Separator();
    ImGui::TextUnformatted("Progress");
    if (ImGui::Button("Unlock all levels")) {
        for (int m = 0; m < 3; ++m) {
            if (A(int, O_BEST + m * 4) < 3660) {
                A(int, O_BEST + m * 4) = 3660;     // 61 s on each of the first three levels
            }
        }
        for (int m = 0; m < 3; ++m) {
            A(int, O_UNLOCKED + m * 4) = 1;
        }
        status("levels unlocked (best times raised to 61s)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset best times")) {
        for (int m = 0; m < 14; ++m) {
            A(int, O_BEST + m * 4) = 0;
        }
        for (int m = 0; m < 3; ++m) {
            A(int, O_UNLOCKED + m * 4) = 0;
            A(int, O_FINISHED + m * 4) = 0;
        }
        status("best times reset");
    }
    if (ImGui::Button("Clear local high scores")) {
        memset(g_hs, 0, sizeof g_hs);
        hsSave();
        status("local high scores cleared");
    }
    ImGui::SameLine();
    bool tut = A(int, APP + O_TUTORIAL_DONE) != 0;
    if (ImGui::Checkbox("Tutorial done", &tut)) {
        A(int, APP + O_TUTORIAL_DONE) = tut ? 1 : 0;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Best times (seconds)");
    for (int m = 0; m < 6; ++m) {
        int frames = A(int, O_BEST + m * 4);
        float sec = (float)frames / 60.0f;
        char lbl[64];
        snprintf(lbl, sizeof lbl, "%s##best%d", kModeNames[m], m);
        if (ImGui::SliderFloat(lbl, &sec, 0.0f, 120.0f, "%.2f s")) {
            A(int, O_BEST + m * 4) = (int)(sec * 60.0f);
        }
    }
}

void tabSettings()
{
    int mus = A(int, APP + 0x20), snd = A(int, APP + 0x24);
    if (ImGui::SliderInt("Music volume", &mus, 0, 10)) {
        A(int, APP + 0x20) = mus;
        FUN_00438b20((float)mus / 10.0f);
    }
    if (ImGui::SliderInt("Sound volume", &snd, 0, 10)) {
        A(int, APP + 0x24) = snd;
        FUN_00438b40((float)snd / 10.0f);
    }
    ImGui::Separator();
    bool dirty = false;
    static const char* const filters[] = {"None", "Invert", "Grayscale", "Sepia", "Red/Blue swap", "High contrast"};
    dirty |= ImGui::Combo("Colour filter", &g_filter, filters, 6);
    dirty |= ImGui::Checkbox("Mirror the screen", &g_mirror);
    ImGui::Checkbox("Swap left / right controls", &g_swapLR);
    dirty |= ImGui::Checkbox("VSync", &g_vsync);
    ImGui::Checkbox("Show FPS overlay", &g_showFps);
    ImGui::Checkbox("Pause the game while this menu is open", &g_pauseOnOpen);
    ImGui::SliderFloat("Menu scale", &g_uiScale, 0.6f, 2.0f, "%.2f");
    if (dirty) {
        applyGfx();
    }
    if (ImGui::Button("Save extras settings")) {
        saveCfg();
        status("saved %s/extras.cfg", g_base.c_str());
    }
}

struct VarRow {
    const char* name;
    int off;
    char type;   // i int, f float, d double
};

void tabDebug()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("%.1f FPS   %.2f ms/frame   blob %u MB", io.Framerate, 1000.0f / (io.Framerate > 0 ? io.Framerate : 1), BLOB_SIZE >> 20);
    ImGui::PlotLines("##ft", g_ft.data(), (int)g_ft.size(), g_ftPos, "frame time (ms)", 0.0f, 40.0f, ImVec2(0, 60 * g_uiScale));
    ImGui::Separator();
    static const VarRow rows[] = {
        {"UI state (0x4c)", O_STATE, 'i'},        {"Stage (0x54b4, -2 menu / 0.. in game)", O_STAGE, 'i'},
        {"Menu page (0x54f0)", O_PAGE, 'i'},      {"Menu selection (0x101a8)", O_MENUSEL, 'i'},
        {"Frame counter (0x2938)", O_FRAMES, 'i'},    {"Run timer (0x2924)", 0x2924, 'f'}, {"Player angle (0x297c)", O_PLAYER_ANGLE, 'i'},
        {"Move speed (0x2984)", O_MOVE_SPEED, 'i'}, {"World rotation (0x1a0)", O_WORLD_ROT, 'f'},
        {"Death flag (0x2988)", O_DEATH, 'f'},    {"Time scale (0x2950)", 0x2950, 'd'},
    };
    if (ImGui::CollapsingHeader("Game variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const VarRow& r : rows) {
            ImGui::PushID(r.off);
            if (r.type == 'i') {
                int v = A(int, APP + r.off);
                if (ImGui::DragInt(r.name, &v)) {
                    A(int, APP + r.off) = v;
                }
            } else if (r.type == 'f') {
                float v = A(float, APP + r.off);
                if (ImGui::DragFloat(r.name, &v, 0.1f)) {
                    A(float, APP + r.off) = v;
                }
            } else {
                double v = A(double, APP + r.off);
                ImGui::Text("%s = %.4f", r.name, v);
            }
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Memory (offset from the application object)")) {
        ImGui::DragInt("base##mem", &g_memBase, 16.0f, 0, 0x40000, "0x%X");
        if (g_memBase < 0) g_memBase = 0;
        g_memBase &= ~3;
        for (int row = 0; row < 12; ++row) {
            int o = g_memBase + row * 16;
            char line[160];
            int n = snprintf(line, sizeof line, "%05X:", o);
            for (int i = 0; i < 16; i += 4) {
                n += snprintf(line + n, sizeof line - n, " %08X", A(uint32_t, APP + o + i));
            }
            ImGui::TextUnformatted(line);
        }
        ImGui::DragInt("poke offset", &g_pokeOff, 4.0f, 0, 0x40000, "0x%X");
        ImGui::Checkbox("as float", &g_pokeFloat);
        if (g_pokeFloat) {
            static float fv = 0;
            ImGui::DragFloat("value##pf", &fv, 0.1f);
            if (ImGui::Button("Poke float")) {
                A(float, APP + (g_pokeOff & ~3)) = fv;
            }
        } else {
            ImGui::DragInt("value##pi", &g_pokeVal);
            if (ImGui::Button("Poke int")) {
                A(int, APP + (g_pokeOff & ~3)) = g_pokeVal;
            }
        }
    }
    if (ImGui::CollapsingHeader("Log")) {
        std::vector<std::string> lines;
        rt_log_copy(lines);
        int from = (int)lines.size() > 40 ? (int)lines.size() - 40 : 0;
        for (int i = from; i < (int)lines.size(); ++i) {
            ImGui::TextUnformatted(lines[(size_t)i].c_str());
        }
    }
    ImGui::Separator();
    if (ImGui::Button("Screenshot")) {
        g_wantShot = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump state blob")) {
        std::string p = g_base + "/blob.bin";
        FILE* f = fopen(p.c_str(), "wb");
        if (f) {
            fwrite(G, 1, 0x60000, f);
            fclose(f);
            status("wrote %s", p.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) {
        rt_request_quit();
    }
}

void tabAbout()
{
    ImGui::TextUnformatted("Super Hexagon - Nintendo Switch port");
    ImGui::TextUnformatted("Game by Terry Cavanagh, music by Chipzel.");
    ImGui::TextUnformatted("The original game logic runs unmodified (recompiled from your own copy),");
    ImGui::TextUnformatted("on top of a small SDL2 / OpenGL ES platform layer.");
    ImGui::Separator();
    ImGui::TextUnformatted("Secret menu: D-pad Up Up Down Down Left Right Left Right B A,");
    ImGui::TextUnformatted("or hold L + R + ZL + ZR for one second.");
    ImGui::TextUnformatted("Menu navigation: D-pad / left stick, A = select, B = back, or touch.");
    ImGui::TextUnformatted("Dear ImGui (MIT) by Omar Cornut.");
}

void drawMenu()
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ds.x * 0.86f, ds.y * 0.88f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    if (ImGui::Begin("Super Secret Menu", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTabBar("tabs")) {
            auto fl = [](int i) { return g_forceTab == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
            if (ImGui::BeginTabItem("Cheats", nullptr, fl(0))) {
                tabCheats();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings", nullptr, fl(1))) {
                tabSettings();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Debug", nullptr, fl(2))) {
                tabDebug();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("About", nullptr, fl(3))) {
                tabAbout();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            g_forceTab = -1;
        }
        if (g_clock < g_statusUntil) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "%s", g_status.c_str());
        }
    }
    ImGui::End();
    if (!open) {
        g_menuOpen = false;
        saveCfg();
    }
}

void drawOsd(const std::string& txt)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 8), ImGuiCond_Always, ImVec2(0.5f, 1));
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##osd", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::TextColored(ImVec4(1, 0.9f, 0.3f, 1), "%s", txt.c_str());
    }
    ImGui::End();
}

void drawFps()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(6, io.DisplaySize.y - 6), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::Text("%.0f FPS  %.1f ms", io.Framerate, 1000.0f / (io.Framerate > 0 ? io.Framerate : 1));
    }
    ImGui::End();
}

int konamiToken(const SDL_Event& e)
{
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return 1;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 2;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 3;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 4;
#ifdef __SWITCH__
        // positional: Switch B is SDL "A", Switch A is SDL "B"
        case SDL_CONTROLLER_BUTTON_A: return 5;
        case SDL_CONTROLLER_BUTTON_B: return 6;
#else
        case SDL_CONTROLLER_BUTTON_B: return 5;
        case SDL_CONTROLLER_BUTTON_A: return 6;
#endif
        default: return 0;
        }
    }
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_UP: return 1;
        case SDL_SCANCODE_DOWN: return 2;
        case SDL_SCANCODE_LEFT: return 3;
        case SDL_SCANCODE_RIGHT: return 4;
        case SDL_SCANCODE_B: return 5;
        case SDL_SCANCODE_A: return 6;
        default: return 0;
        }
    }
    return 0;
}

ImGuiKey padKey(Uint8 b)
{
    switch (b) {
#ifdef __SWITCH__
    case SDL_CONTROLLER_BUTTON_B: return ImGuiKey_GamepadFaceDown;     // Switch A = activate
    case SDL_CONTROLLER_BUTTON_A: return ImGuiKey_GamepadFaceRight;    // Switch B = cancel
    case SDL_CONTROLLER_BUTTON_Y: return ImGuiKey_GamepadFaceUp;       // Switch X
    case SDL_CONTROLLER_BUTTON_X: return ImGuiKey_GamepadFaceLeft;     // Switch Y
#else
    case SDL_CONTROLLER_BUTTON_A: return ImGuiKey_GamepadFaceDown;
    case SDL_CONTROLLER_BUTTON_B: return ImGuiKey_GamepadFaceRight;
    case SDL_CONTROLLER_BUTTON_Y: return ImGuiKey_GamepadFaceUp;
    case SDL_CONTROLLER_BUTTON_X: return ImGuiKey_GamepadFaceLeft;
#endif
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ImGuiKey_GamepadL1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ImGuiKey_GamepadR1;
    case SDL_CONTROLLER_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_CONTROLLER_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}
}   // namespace

// ---- public ------------------------------------------------------------------------------------------
bool extras_swap_lr() { return g_swapLR; }
void extras_note_scores_bar() { g_barSeen = true; }
bool extras_scores_bar_visible() { return g_barVisible; }
bool extras_overlay_open() { return g_menuOpen || g_scoresOpen; }

void extras_toggle_menu()
{
    g_menuOpen = !g_menuOpen;
    g_scoresOpen = false;
    if (g_menuOpen) {
        status("secret menu unlocked");
    } else {
        saveCfg();
        cheats_save();
    }
}

void extras_select_tab(int t) { g_forceTab = t; }
void extras_open_scores() { g_scoresOpen = true; g_menuOpen = false; }

void extras_init(const std::string& baseDir)
{
    g_base = baseDir;
    loadCfg();
    cheats_init(baseDir);
    hsLoad();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    int w, h;
    gfx_size(&w, &h);
    ImFontConfig fc;
    fc.SizePixels = (float)h / 33.0f;
    if (fc.SizePixels < 15.0f) fc.SizePixels = 15.0f;
    fc.OversampleH = 2;
    fc.OversampleV = 2;
    io.Fonts->AddFontDefault(&fc);
    unsigned char* px = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsAlpha8(&px, &tw, &th);
    g_fontTex = gfx_make_alpha_texture(px, tw, th);
    io.Fonts->SetTexID((ImTextureID)(uintptr_t)g_fontTex);
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.ScrollbarSize = 18.0f;
    st.FramePadding = ImVec2(8, 6);
    st.ItemSpacing = ImVec2(10, 8);
    st.GrabMinSize = 22.0f;
    applyGfx();
    g_inited = true;
}

void extras_shutdown()
{
    if (g_inited) {
        ImGui::DestroyContext();
        g_inited = false;
    }
}

bool extras_event(const SDL_Event& e)
{
    if (!g_inited) {
        return false;
    }
    cheats_event(e);
    if (cheats_capturing() && (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERAXISMOTION)) {
        return true;       // rebinding: the press belongs to the binder
    }
    // --- unlock gestures
    int tok = konamiToken(e);
    if (tok) {
        if (tok == kKonami[g_konamiPos]) {
            if (++g_konamiPos == 10) {
                g_konamiPos = 0;
                extras_toggle_menu();
                return false;
            }
        } else {
            g_konamiPos = (tok == kKonami[0]) ? 1 : 0;
        }
    }
    if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F10) {
        extras_toggle_menu();
        return true;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (g_scoresOpen) {
        // any of back / confirm / Y closes the list
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A || e.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
                e.cbutton.button == SDL_CONTROLLER_BUTTON_X || e.cbutton.button == SDL_CONTROLLER_BUTTON_Y ||
                e.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                g_scoresOpen = false;
            }
            return true;
        }
        if (e.type == SDL_KEYDOWN) {
            g_scoresOpen = false;
            return true;
        }
        if (e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN) {
            g_scoresOpen = false;
            return true;
        }
        if (e.type == SDL_JOYBUTTONDOWN) {
            return true;
        }
        return false;   // releases etc. fall through
    }
    if (!g_menuOpen) {
        return false;
    }
    // --- menu open: events go to ImGui
    switch (e.type) {
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            extras_toggle_menu();       // "+" closes the menu
            return true;
        }
        ImGuiKey k = padKey(e.cbutton.button);
        if (k != ImGuiKey_None) {
            io.AddKeyEvent(k, e.type == SDL_CONTROLLERBUTTONDOWN);
        }
        return e.type == SDL_CONTROLLERBUTTONDOWN;
    }
    case SDL_CONTROLLERAXISMOTION: {
        float v = e.caxis.value / 32767.f;
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, v < -0.2f, v < 0 ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, v > 0.2f, v > 0 ? v : 0);
        } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, v < -0.2f, v < 0 ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, v > 0.2f, v > 0 ? v : 0);
        }
        return false;
    }
    case SDL_MOUSEMOTION:
        if (e.motion.which == SDL_TOUCH_MOUSEID) return true;     // synthesized from touch: handled via finger events
        io.AddMousePosEvent((float)e.motion.x, (float)e.motion.y);
        return true;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e.button.which == SDL_TOUCH_MOUSEID) return e.type == SDL_MOUSEBUTTONDOWN;
        io.AddMousePosEvent((float)e.button.x, (float)e.button.y);
        io.AddMouseButtonEvent(e.button.button == SDL_BUTTON_RIGHT ? 1 : 0, e.type == SDL_MOUSEBUTTONDOWN);
        return e.type == SDL_MOUSEBUTTONDOWN;
    case SDL_MOUSEWHEEL:
        io.AddMouseWheelEvent((float)e.wheel.x, (float)e.wheel.y);
        return true;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        ImGuiKey k = ImGuiKey_None;
        switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_UP: k = ImGuiKey_UpArrow; break;
        case SDL_SCANCODE_DOWN: k = ImGuiKey_DownArrow; break;
        case SDL_SCANCODE_LEFT: k = ImGuiKey_LeftArrow; break;
        case SDL_SCANCODE_RIGHT: k = ImGuiKey_RightArrow; break;
        case SDL_SCANCODE_RETURN: k = ImGuiKey_Enter; break;
        case SDL_SCANCODE_ESCAPE: k = ImGuiKey_Escape; break;
        case SDL_SCANCODE_TAB: k = ImGuiKey_Tab; break;
        default: break;
        }
        if (k != ImGuiKey_None) {
            io.AddKeyEvent(k, e.type == SDL_KEYDOWN);
        }
        return e.type == SDL_KEYDOWN;
    }
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP: {
        // touch drives the ImGui mouse; a drag that starts on empty space scrolls the window
        static float lastY = 0;
        const float px = e.tfinger.x * io.DisplaySize.x, py = e.tfinger.y * io.DisplaySize.y;
        io.AddMousePosEvent(px, py);
        if (e.type == SDL_FINGERDOWN) {
            io.AddMouseButtonEvent(0, true);
            lastY = py;
        } else if (e.type == SDL_FINGERUP) {
            io.AddMouseButtonEvent(0, false);
        } else {
            if (!ImGui::IsAnyItemActive()) {
                io.AddMouseWheelEvent(0, (py - lastY) / ImGui::GetTextLineHeightWithSpacing());
            }
            lastY = py;
        }
        return e.type != SDL_FINGERUP;
    }
    case SDL_JOYBUTTONDOWN:
        return true;
    default:
        return false;
    }
}

double extras_dt(double dt, bool* skip)
{
    g_clock += dt;
    g_dt = (float)dt;
    static bool wasOpen = false;
    const bool isOpen = g_menuOpen || g_scoresOpen;
    if (wasOpen && !isOpen) {
        input_clear_pressed();     // presses made while an overlay was up must not leak into the game
    }
    wasOpen = isOpen;
    if (!g_inited) {
        *skip = false;
        return dt;
    }
    // L + R + ZL + ZR held for a second opens the menu
    bool chord = input_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) && input_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
                 input_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f && input_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f;
    if (chord) {
        if (++g_chordFrames == 60) {
            extras_toggle_menu();
        }
    } else {
        g_chordFrames = 0;
    }
    g_ft[(size_t)g_ftPos] = (float)(dt * 1000.0);
    g_ftPos = (g_ftPos + 1) % (int)g_ft.size();

    const bool overlayHalt = g_scoresOpen || (g_menuOpen && g_pauseOnOpen);
    *skip = cheats_frame(dt, !g_menuOpen && !g_scoresOpen, overlayHalt);
    audio_set_halted(overlayHalt || cheats_paused());
    audio_set_rate(cheats_music_rate());
    if (!*skip) {
        trackRun();
    }
    return dt;
}

void extras_draw()
{
    g_barVisible = g_barSeen;
    g_barSeen = false;
    if (!g_inited) {
        return;
    }
    if (g_scoresOpen) {
        drawScores();
    }
    if (g_wantShot) {
        g_wantShot = false;
        static int n = 0;
        char p[300];
        snprintf(p, sizeof p, "%s/shot_%03d.png", g_base.c_str(), n++);
        gfx_flush();
        gfx_screenshot(p);
        status("saved %s", p);
    }
    std::string osd;
    const bool showOsd = !g_menuOpen && cheats_osd(osd);
    if (!g_menuOpen && !g_showFps && !showOsd) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    int w, h;
    gfx_size(&w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = g_dt > 0.0001f ? g_dt : 1.0f / 60.0f;
    io.FontGlobalScale = g_uiScale;
    ImGui::NewFrame();
    if (g_menuOpen) {
        drawMenu();
    }
    if (g_showFps && !g_menuOpen) {
        drawFps();
    }
    if (showOsd) {
        drawOsd(osd);
    }
    ImGui::Render();
    gfx_render_imgui(ImGui::GetDrawData());
}
