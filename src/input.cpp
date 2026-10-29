// Device input -> the logical flags the original's menu/game code reads (original FUN_004273d0).
//   app+0x39 up   +0x3a down   +0x3b left   +0x3c right   +0x3d confirm   +0x3f esc/back/quit   +0x40 tab/leaderboards
#include "rt.h"
#include "hand_decl.h"
#include <SDL.h>
#include <string.h>
#include "extras.h"
#include "cheats.h"

namespace {
bool g_key[SDL_NUM_SCANCODES];
bool g_btn[SDL_CONTROLLER_BUTTON_MAX];
float g_axis[SDL_CONTROLLER_AXIS_MAX];
bool g_touchDown = false;
float g_touchX = 0, g_touchY = 0;
bool g_touchTap = false;
bool g_pressedBack = false, g_pressedTab = false, g_pressedConfirm = false, g_pressedClear = false;
SDL_GameController* g_pad = nullptr;

// the level select screen (where the "HIGH SCORES" bar is)
bool onLevelSelect() { return extras_scores_bar_visible(); }
}

void input_press_confirm() { g_pressedConfirm = true; }

void input_clear_pressed()
{
    g_pressedBack = g_pressedTab = g_pressedConfirm = g_pressedClear = false;
    g_touchTap = false;
}

bool input_btn(int b) { return b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX && g_btn[b]; }
float input_axis(int a) { return a >= 0 && a < SDL_CONTROLLER_AXIS_MAX ? g_axis[a] : 0.f; }

void input_init()
{
    memset(g_key, 0, sizeof g_key);
    memset(g_btn, 0, sizeof g_btn);
    memset(g_axis, 0, sizeof g_axis);
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) {
                break;
            }
        }
    }
}

void input_event(const SDL_Event& e)
{
    switch (e.type) {
    case SDL_KEYDOWN:
        if (!e.key.repeat) {
            g_key[e.key.keysym.scancode] = true;
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) g_pressedBack = true;
            // TAB used to open the Steam leaderboards (not available): it opens the local high scores instead
            if (e.key.keysym.scancode == SDL_SCANCODE_TAB && onLevelSelect()) extras_open_scores();
            if (e.key.keysym.scancode == SDL_SCANCODE_C) g_pressedClear = true;
            if (e.key.keysym.scancode == SDL_SCANCODE_RETURN || e.key.keysym.scancode == SDL_SCANCODE_SPACE ||
                e.key.keysym.scancode == SDL_SCANCODE_KP_ENTER) g_pressedConfirm = true;
        }
        break;
    case SDL_KEYUP:
        g_key[e.key.keysym.scancode] = false;
        break;
    case SDL_CONTROLLERDEVICEADDED:
        if (!g_pad) {
            g_pad = SDL_GameControllerOpen(e.cdevice.which);
        }
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
            SDL_GameControllerClose(g_pad);
            g_pad = nullptr;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        g_btn[e.cbutton.button] = true;
#ifdef __SWITCH__
        // SDL reports the Switch pad positionally (xbox-style): Switch A is the east button = SDL "B"
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) g_pressedBack = true;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_Y) g_pressedClear = true;     // Switch X (north)
        // (Switch Y no longer opens the high scores: touch only)
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START && cheats_in_run()) g_pressedBack = true;   // "+" in a run: back to the menu
        else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B || e.cbutton.button == SDL_CONTROLLER_BUTTON_START)
            g_pressedConfirm = true;
#else
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) g_pressedBack = true;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_X) g_pressedClear = true;
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_Y && onLevelSelect()) extras_open_scores();
        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A || e.cbutton.button == SDL_CONTROLLER_BUTTON_START)
            g_pressedConfirm = true;
#endif
        break;
    case SDL_CONTROLLERBUTTONUP:
        g_btn[e.cbutton.button] = false;
        break;
    case SDL_JOYBUTTONDOWN:
        logf("joy button %d", (int)e.jbutton.button);
        // raw Switch button order: 10 is "+" (not always reported through the game controller mapping)
        if (e.jbutton.button == 10) g_pressedConfirm = true;
        break;
    case SDL_CONTROLLERAXISMOTION:
        g_axis[e.caxis.axis] = e.caxis.value / 32767.f;
        break;
    case SDL_FINGERDOWN:
        if (onLevelSelect() && e.tfinger.x < 0.34f && e.tfinger.y < 0.09f) {
            extras_open_scores();      // the "HIGH SCORES" bar in the top left corner
            break;
        }
        g_touchDown = true;
        g_touchX = e.tfinger.x;
        g_touchY = e.tfinger.y;
        g_touchTap = true;
        break;
    case SDL_FINGERMOTION:
        g_touchX = e.tfinger.x;
        g_touchY = e.tfinger.y;
        break;
    case SDL_FINGERUP:
        g_touchDown = false;
        break;
    default:
        break;
    }
}

// scripted input for automated runs: "frame:action,..." with action = left right up down confirm back tab
// (a "!" prefix releases the hold-type actions left/right/up/down)
#include <vector>
#include <string>
#include <stdlib.h>
struct ScriptEv { int frame; std::string act; };
static std::vector<ScriptEv> g_script;
static bool g_sHeld[4] = {false, false, false, false};
static int g_frameNo = 0;

void input_set_script(const std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        size_t c = s.find(',', i);
        if (c == std::string::npos) c = s.size();
        std::string tok = s.substr(i, c - i);
        size_t colon = tok.find(':');
        if (colon != std::string::npos) {
            g_script.push_back({atoi(tok.substr(0, colon).c_str()), tok.substr(colon + 1)});
        }
        i = c + 1;
    }
}

static void runScript()
{
    for (const ScriptEv& e : g_script) {
        if (e.frame != g_frameNo) continue;
        std::string a = e.act;
        bool rel = !a.empty() && a[0] == '!';
        if (rel) a = a.substr(1);
        if (a == "left") g_sHeld[0] = !rel;
        else if (a == "right") g_sHeld[1] = !rel;
        else if (a == "up") g_sHeld[2] = !rel;
        else if (a == "down") g_sHeld[3] = !rel;
        else if (a == "confirm") g_pressedConfirm = true;
        else if (a == "back") g_pressedBack = true;
        else if (a == "tab") g_pressedTab = true;
        else if (a == "menu") extras_toggle_menu();
        else if (a == "scores") extras_open_scores();
        else if (a.compare(0, 4, "mtab") == 0) extras_select_tab(atoi(a.c_str() + 4));
        else if (a == "y") { if (onLevelSelect()) extras_open_scores(); }
        else if (a == "god") g_cheatGod = true;
        else if (cheats_run(a)) {}
        else if (a == "konami") {
            // test hook: inject the unlock sequence as controller events
            static const int seq[10] = {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                        SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                                        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
#ifdef __SWITCH__
                                        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B};
#else
                                        SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A};
#endif
            for (int k = 0; k < 10; ++k) {
                SDL_Event ev;
                SDL_zero(ev);
                ev.type = SDL_CONTROLLERBUTTONDOWN;
                ev.cbutton.button = (Uint8)seq[k];
                SDL_PushEvent(&ev);
                ev.type = SDL_CONTROLLERBUTTONUP;
                SDL_PushEvent(&ev);
            }
        }
    }
    ++g_frameNo;
}

void FUN_004273d0(int app)
{
    runScript();
    // clear the pulse flags (original: two 32-bit stores)
    A(uint32_t, app + 0x39) = 0;
    A(uint32_t, app + 0x3d) = 0;

    bool left = g_sHeld[0] || g_key[SDL_SCANCODE_LEFT] || g_key[SDL_SCANCODE_A] || g_key[SDL_SCANCODE_KP_4] ||
                g_btn[SDL_CONTROLLER_BUTTON_DPAD_LEFT] || g_btn[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] ||
                g_axis[SDL_CONTROLLER_AXIS_LEFTX] < -0.5f || g_axis[SDL_CONTROLLER_AXIS_RIGHTX] < -0.5f ||
                g_axis[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > 0.5f;
    bool right = g_sHeld[1] || g_key[SDL_SCANCODE_RIGHT] || g_key[SDL_SCANCODE_D] || g_key[SDL_SCANCODE_KP_6] ||
                 g_btn[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] || g_btn[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] ||
                 g_axis[SDL_CONTROLLER_AXIS_LEFTX] > 0.5f || g_axis[SDL_CONTROLLER_AXIS_RIGHTX] > 0.5f ||
                 g_axis[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] > 0.5f;
    if (g_touchDown) {
        if (g_touchX < 0.5f) {
            left = true;
        } else {
            right = true;
        }
    }
    if (extras_swap_lr()) {
        bool t = left;
        left = right;
        right = t;
    }
    bool up = g_sHeld[2] || g_key[SDL_SCANCODE_UP] || g_key[SDL_SCANCODE_W] || g_btn[SDL_CONTROLLER_BUTTON_DPAD_UP] ||
              g_axis[SDL_CONTROLLER_AXIS_LEFTY] < -0.5f;
    bool down = g_sHeld[3] || g_key[SDL_SCANCODE_DOWN] || g_key[SDL_SCANCODE_S] || g_btn[SDL_CONTROLLER_BUTTON_DPAD_DOWN] ||
                g_axis[SDL_CONTROLLER_AXIS_LEFTY] > 0.5f;

    // held-frame counters decide which side wins when both are down: the newer press
    int& lc = A(int, app + 0x4062c);
    int& rc = A(int, app + 0x40630);
    if (left) {
        A(uint8_t, app + 0x3b) = 1;
        lc = (lc == -1) ? 0 : lc + 1;
    } else {
        lc = -1;
    }
    if (right) {
        A(uint8_t, app + 0x3c) = 1;
        rc = (rc == -1) ? 0 : rc + 1;
    } else {
        rc = -1;
    }
    if (left && right) {
        if (lc < rc) {
            A(uint8_t, app + 0x3c) = 0;
        } else {
            A(uint8_t, app + 0x3b) = 0;
        }
    }
    cheats_override_input(app);      // autopilot
    if (up) A(uint8_t, app + 0x39) = 1;
    if (down) A(uint8_t, app + 0x3a) = 1;
    if (g_pressedConfirm) A(uint8_t, app + 0x3d) = 1;
    if (g_pressedTab) A(uint8_t, app + 0x40) = 1;      // TAB: Steam leaderboards (not reachable from the controller)
    if (g_pressedBack) A(uint8_t, app + 0x3f) = 1;      // ESC: back / quit
    if (g_pressedClear) A(uint8_t, app + 0x41) = 1;
    if (g_touchTap) {
        A(uint8_t, app + 0x3d) = 1;
    }
    // original tail (0x42854c): while the input lock (+0x48) is set, swallow direction/confirm/esc until all are released
    if (A(uint8_t, app + 0x48)) {
        if (A(uint8_t, app + 0x3d) || A(uint8_t, app + 0x3c) || A(uint8_t, app + 0x3b) || A(uint8_t, app + 0x3e) ||
            A(uint8_t, app + 0x3a) || A(uint8_t, app + 0x39) || A(uint8_t, app + 0x3f)) {
            A(uint32_t, app + 0x3b) = 0;
            A(uint16_t, app + 0x39) = 0;
            A(uint8_t, app + 0x3f) = 0;
        } else {
            A(uint8_t, app + 0x48) = 0;
        }
    }
    g_pressedConfirm = g_pressedTab = g_pressedBack = g_pressedClear = false;
    g_touchTap = false;
}
