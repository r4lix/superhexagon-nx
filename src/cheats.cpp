// Port-only cheats (secret menu). Everything works on the shared state blob around the converted game update:
//   cheats_before_update / cheats_after_update / cheats_before_draw are called from oneFrame() in main.cpp,
//   cheats_frame() from extras_dt(), extras_death_hook() from the patched collision code (patches.json).
// Every action can be bound to a pad button or a combo of buttons (bindings are saved in cheats.cfg).
#include "cheats.h"
#include "rt.h"
#include "hand_decl.h"
#include "third_party/imgui/imgui.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

double audio_music_pos_ms(int obj);

bool g_cheatGod = false;
bool extras_death_hook(int app);
void input_press_confirm();

namespace {
// ---- game state offsets (relative to the blob; APP = application object, same coordinates as the converted code) ----
const int O_DEATH = 0x2988, O_STAGE = 0x54b4, O_DEATH_ANIM = 0x188, O_TIMER = 0x2924, O_ROT = 0x1a0;
const int O_MUSIC = 0x403a4, O_MUSIC_FADE = 0x403ac, O_MUSIC_T0 = 0x403b8, O_MUSIC_OFF = 0x403c4;
const int O_ANGLE = 0x2980, O_ANGLE_PREV = 0x297c, O_PULSE = 0x29b4, O_WALLS = 0x210, O_WALL_N = 0x2920;
const int O_SIDE = 0x1cc, O_NSIDES = 0x198, O_LEFT = 0x3b, O_RIGHT = 0x3c;

// ---- actions -------------------------------------------------------------------------------------------
enum Act {
    A_GOD, A_NO_WALLS, A_BOT, A_SLOWMO, A_SPEED_UP, A_SPEED_DOWN, A_SPEED_RESET, A_PAUSE, A_STEP, A_REWIND, A_REWIND_DEATH,
    A_SAVE1, A_SAVE2, A_SAVE3, A_LOAD1, A_LOAD2, A_LOAD3, A_ROT_FREEZE, A_ROT_REVERSE, A_PLAYER_FAST, A_CLEAR_WALLS,
    A_NO_PULSE, A_SKIP_TIME, A_COUNT
};
struct ActInfo {
    const char* key;
    const char* label;
    bool hold;       // active while the binding is held (otherwise triggers on the press)
};
const ActInfo kActs[A_COUNT] = {
    {"god", "God mode (toggle)", false},
    {"nowalls", "No walls (toggle)", false},
    {"bot", "Autopilot (toggle)", false},
    {"slowmo", "Slow-mo (hold)", true},
    {"speedup", "Game speed +0.05", false},
    {"speeddown", "Game speed -0.05", false},
    {"speedreset", "Game speed 1x", false},
    {"pause", "Pause (toggle)", false},
    {"step", "Step one frame", false},
    {"rewind", "Rewind (hold)", true},
    {"rewinddeath", "Rewind on death (toggle)", false},
    {"save1", "Save state 1", false},
    {"save2", "Save state 2", false},
    {"save3", "Save state 3", false},
    {"load1", "Load state 1", false},
    {"load2", "Load state 2", false},
    {"load3", "Load state 3", false},
    {"rotfreeze", "Freeze rotation (toggle)", false},
    {"rotreverse", "Reverse rotation (toggle)", false},
    {"playerfast", "Fast player (toggle)", false},
    {"clearwalls", "Clear all walls now", false},
    {"nopulse", "No beat pulse (toggle)", false},
    {"skiptime", "Skip +10 seconds", false},
};

// pad bits: SDL controller buttons 0..20, plus the two triggers
const int BIT_ZL = 28, BIT_ZR = 29;
uint32_t g_bind[A_COUNT];
uint32_t g_raw = 0;           // buttons held right now (from the raw SDL events, also while the menu is open)
uint32_t g_prevHeld = 0;
bool g_active[A_COUNT];       // action currently triggered (for hold actions) / fired this frame (edge)
int g_capture = -1;           // action being rebound
uint32_t g_capMask = 0;
std::string g_base = ".";

// ---- cheat state -----------------------------------------------------------------------------------------
bool g_nowalls = false, g_bot = false, g_slowToggle = false, g_paused = false, g_rewindDeath = false;
bool g_forceHyper = false;     // test/training: every run starts in hyper mode
bool g_musicSlow = true, g_musicRewind = true;    // music follows slow-mo / speed, music plays backwards on rewind
bool g_rotFreeze = false, g_rotReverse = false, g_playerFast = false, g_nopulse = false;
float g_speed = 1.0f, g_slowFactor = 0.3f, g_rotMul = 1.0f, g_playerBoost = 2.0f;
float g_rewindSecs = 3.0f;
int g_stepFrames = 0;
bool g_slowHeld = false;
std::string g_toast;
double g_toastUntil = 0;
double g_clock = 0;

void toast(const char* fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_toast = b;
    g_toastUntil = g_clock + 2.5;
}

// ---- snapshots (rewind ring + save-state slots) -------------------------------------------------------------
// The playing state lives in the application object between SNAP_OFF and SNAP_OFF+SNAP_LEN (walls, player, rotation,
// timers, pattern state, run counters) plus a few fields further up written by the death sequence.
const int SNAP_OFF = 0x60, SNAP_LEN = 0x2d00 - 0x60;
const int kExtraOff[5] = {0x15c, 0x158, 0x5488, 0x54b0, 0x54fc};
struct Snap {
    uint8_t b[SNAP_LEN];
    int ex[5];
    int stage;
    int music;      // in-run music track (-1 none); the audio itself is not rewound
};
const int RING = 600;         // 10 s at 60 fps
std::vector<Snap> g_ring;
int g_ringHead = 0, g_ringCount = 0;
Snap g_slot[3];
bool g_slotValid[3];
int g_autoRewind = 0;         // frames left of an automatic rewind (rewind on death)
bool g_rewinding = false;

bool playing(int app)
{
    return A(int, app + O_STAGE) >= 0 && A(float, app + O_DEATH_ANIM) == 0.0f && A(float, app + O_DEATH) < 1.0f;
}

void capture(Snap& s, int app)
{
    memcpy(s.b, G + app + SNAP_OFF, SNAP_LEN);
    for (int i = 0; i < 5; ++i) {
        s.ex[i] = A(int, app + kExtraOff[i]);
    }
    s.stage = A(int, app + O_STAGE);
    s.music = A(int, app + O_MUSIC);
}

void restore(const Snap& s, int app)
{
    memcpy(G + app + SNAP_OFF, s.b, SNAP_LEN);
    for (int i = 0; i < 5; ++i) {
        A(int, app + kExtraOff[i]) = s.ex[i];
    }
    // Dying fades the music out and ends it (track = -1), which also stops the beat pulse (it is read from the music
    // clock). Rewinding past a death must bring the track back and cancel a fade in progress.
    if (s.music >= 0) {
        const int obj = app + (s.music * 9 + 0x10023) * 4;
        if (A(int, app + O_MUSIC) < 0) {
            A(int, app + O_MUSIC) = s.music;
            A(int, app + O_MUSIC_T0) = FUN_004387b0();
            A(int, app + O_MUSIC_T0 + 4) = 0;
            A(int, app + O_MUSIC_OFF) = 0;
            A(float, app + O_MUSIC_FADE) = 0.0f;
            FUN_0043a4c0(obj, 1.0f);
            FUN_0043a340(obj, 0);
        } else if (A(float, app + O_MUSIC_FADE) > 0.0f) {
            A(float, app + O_MUSIC_FADE) = 0.0f;
            FUN_0043a4c0(obj, 1.0f);
        }
    }
}

void ringPush(int app)
{
    if (g_ring.empty()) {
        g_ring.resize(RING);
    }
    capture(g_ring[(size_t)g_ringHead], app);
    g_ringHead = (g_ringHead + 1) % RING;
    if (g_ringCount < RING) {
        ++g_ringCount;
    }
}

bool ringPop(int app, int n)
{
    if (g_ringCount == 0) {
        return false;
    }
    if (n > g_ringCount) {
        n = g_ringCount;
    }
    g_ringHead = (g_ringHead - n + RING) % RING;
    g_ringCount -= n;
    int last = (g_ringHead - 1 + RING) % RING;       // newest remaining snapshot = the state to show
    if (g_ringCount == 0) {
        last = g_ringHead;                           // popped the oldest: show it (still valid memory)
        g_ringCount = 1;
        g_ringHead = (g_ringHead + 1) % RING;
    }
    restore(g_ring[(size_t)last], app);
    return true;
}

void saveSlot(int i)
{
    const int app = APP;
    if (!playing(app)) {
        toast("save state: start a run first");
        return;
    }
    capture(g_slot[i], app);
    g_slotValid[i] = true;
    toast("state %d saved", i + 1);
}

void loadSlot(int i)
{
    const int app = APP;
    if (!g_slotValid[i]) {
        toast("state %d is empty", i + 1);
        return;
    }
    if (A(int, app + O_STAGE) != g_slot[i].stage) {
        toast("state %d belongs to another level", i + 1);
        return;
    }
    restore(g_slot[i], app);
    g_ringCount = 0;
    g_autoRewind = 0;
    toast("state %d loaded", i + 1);
}

// ---- pad tracking ------------------------------------------------------------------------------------------
const char* bitName(int b)
{
#ifdef __SWITCH__
    static const char* n[21] = {"B", "A", "Y", "X", "-", "Home", "+", "L-Stick", "R-Stick", "L", "R", "Up", "Down", "Left", "Right",
                                "Cap", "P1", "P2", "P3", "P4", "Touch"};
#else
    static const char* n[21] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "L-Stick", "R-Stick", "LB", "RB", "Up", "Down", "Left",
                                "Right", "Misc", "P1", "P2", "P3", "P4", "Touch"};
#endif
    if (b == BIT_ZL) return "ZL";
    if (b == BIT_ZR) return "ZR";
    return b >= 0 && b < 21 ? n[b] : "?";
}

std::string maskName(uint32_t m)
{
    if (!m) {
        return "unbound";
    }
    std::string s;
    for (int b = 0; b < 32; ++b) {
        if (m & (1u << b)) {
            if (!s.empty()) s += " + ";
            s += bitName(b);
        }
    }
    return s;
}

int popcount(uint32_t v)
{
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

void defaults()
{
    memset(g_bind, 0, sizeof g_bind);
    // L / R / ZL / ZR / D-pad / sticks steer the player, so the defaults use X, Y, the stick clicks and -
    g_bind[A_REWIND] = 1u << SDL_CONTROLLER_BUTTON_X;     // Switch Y
    g_bind[A_SLOWMO] = 1u << SDL_CONTROLLER_BUTTON_Y;     // Switch X
    g_bind[A_PAUSE] = 1u << SDL_CONTROLLER_BUTTON_BACK;
    g_bind[A_SAVE1] = 1u << SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    g_bind[A_LOAD1] = 1u << SDL_CONTROLLER_BUTTON_LEFTSTICK;
}

// ---- performing actions --------------------------------------------------------------------------------------
void fire(int a)
{
    switch (a) {
    case A_GOD: g_cheatGod = !g_cheatGod; toast("god mode %s", g_cheatGod ? "ON" : "off"); break;
    case A_NO_WALLS: g_nowalls = !g_nowalls; toast("no walls %s", g_nowalls ? "ON" : "off"); break;
    case A_BOT: g_bot = !g_bot; toast("autopilot %s", g_bot ? "ON" : "off"); break;
    case A_SPEED_UP: g_speed = fminf(4.0f, roundf((g_speed + 0.05f) * 20.0f) / 20.0f); toast("speed %.2fx", g_speed); break;
    case A_SPEED_DOWN: g_speed = fmaxf(0.05f, roundf((g_speed - 0.05f) * 20.0f) / 20.0f); toast("speed %.2fx", g_speed); break;
    case A_SPEED_RESET: g_speed = 1.0f; toast("speed 1x"); break;
    case A_PAUSE: g_paused = !g_paused; toast(g_paused ? "paused" : "resumed"); break;
    case A_STEP: g_paused = true; g_stepFrames = 1; break;
    case A_REWIND_DEATH: g_rewindDeath = !g_rewindDeath; toast("rewind on death %s", g_rewindDeath ? "ON" : "off"); break;
    case A_SAVE1: case A_SAVE2: case A_SAVE3: saveSlot(a - A_SAVE1); break;
    case A_LOAD1: case A_LOAD2: case A_LOAD3: loadSlot(a - A_LOAD1); break;
    case A_ROT_FREEZE: g_rotFreeze = !g_rotFreeze; toast("rotation %s", g_rotFreeze ? "frozen" : "free"); break;
    case A_ROT_REVERSE: g_rotReverse = !g_rotReverse; toast("rotation %s", g_rotReverse ? "reversed" : "normal"); break;
    case A_PLAYER_FAST: g_playerFast = !g_playerFast; toast("fast player %s", g_playerFast ? "ON" : "off"); break;
    case A_CLEAR_WALLS: A(int, APP + O_WALL_N) = 0; toast("walls cleared"); break;
    case A_NO_PULSE: g_nopulse = !g_nopulse; toast("beat pulse %s", g_nopulse ? "off" : "on"); break;
    case A_SKIP_TIME:
        if (A(int, APP + O_STAGE) >= 0) {
            A(float, APP + O_TIMER) += 600.0f;
            toast("+10 s");
        }
        break;
    default: break;
    }
}

uint32_t held() { return g_raw; }

void pollBinds()
{
    const uint32_t h = held();
    // an action is "triggered" when all its buttons are held and no other bound action needs a strict superset of them
    bool trig[A_COUNT];
    for (int a = 0; a < A_COUNT; ++a) {
        trig[a] = g_bind[a] != 0 && (h & g_bind[a]) == g_bind[a];
    }
    for (int a = 0; a < A_COUNT; ++a) {
        for (int b = 0; trig[a] && b < A_COUNT; ++b) {
            if (b != a && g_bind[b] != g_bind[a] && trig[b] && (g_bind[b] & g_bind[a]) == g_bind[a]) {
                trig[a] = false;
            }
        }
    }
    for (int a = 0; a < A_COUNT; ++a) {
        bool was = g_active[a];
        g_active[a] = trig[a];
        if (!kActs[a].hold && trig[a] && !was) {
            fire(a);
        }
    }
    g_prevHeld = h;
}

// ---- autopilot ------------------------------------------------------------------------------------------------
// Plans a path over the hexagon's sides: a wall (200 thick) occupies the player ring while dist is in (-77, 145);
// the player needs ~8 frames per side. Picks the shortest path whose crossing windows never overlap a wall.
int g_prevDist[512];
float g_wallSpeed = 20.0f;
int g_botTarget = -1;       // committed target side (avoids dithering between two equally good sides)

// Autopilot. Every wall on a side keeps the player ring busy during a time window computed from its distance, its
// thickness and the wall speed. A backward search over (frame, side) finds which positions can still survive for the
// next HORIZON frames (moving to a neighbour side takes ~degPer/speed frames); the bot then walks the shortest
// collision-free path to a side that stays alive, staying put when that is already safe.
void botSteer(int app)
{
    if (!g_bot || A(int, app + O_STAGE) < 0 || A(float, app + O_DEATH_ANIM) != 0.0f) {
        return;
    }
    const int n = A(int, app + O_NSIDES);
    if (n < 3 || n > 12) {
        return;
    }
    const int degPer = 360 / n;
    const int cnt = A(int, app + O_WALL_N) < 500 ? A(int, app + O_WALL_N) : 500;
    float vsum = 0;
    int vn = 0;
    for (int i = 0; i < cnt; ++i) {
        int w = app + O_WALLS + i * 0x14;
        int d = A(int, w + 4);
        if ((char)A(int, w + 16) && g_prevDist[i] > d && g_prevDist[i] - d < 200) {
            vsum += (float)(g_prevDist[i] - d);
            ++vn;
        }
        g_prevDist[i] = d;
    }
    const double scale = A(double, app + 0x2950);
    const float sc = (float)(scale > 0.01 ? scale : 1.0);
    if (vn > 0) {
        g_wallSpeed = vsum / (float)vn / sc;
    }
    const float v = g_wallSpeed * sc;                                   // wall distance per frame
    const int pspd = A(int, app + 0x2984) > 0 ? A(int, app + 0x2984) : 7;
    const float degPerFrame = (float)pspd * sc;                        // player turn rate
    const int ang = A(int, app + O_ANGLE_PREV);
    const int cur = ((ang / degPer) % n + n) % n;

    struct Win { float t0, t1; };
    static Win wins[12][64];
    int wn[12] = {0};
    for (int i = 0; i < cnt; ++i) {
        int w = app + O_WALLS + i * 0x14;
        if (!(char)A(int, w + 16)) continue;
        int side = A(int, w);
        float d = (float)A(int, w + 4);
        if (side < 0 || side >= n || wn[side] >= 64) continue;
        float thick = (float)A(int, w + 8);
        float t0 = (d - 160.0f) / v - 1.0f, t1 = (d + thick - 110.0f) / v + 1.0f;
        if (t1 < 0) continue;
        wins[side][wn[side]++] = {t0, t1};
    }
    auto busy = [&](int s, float a, float b) {
        for (int k = 0; k < wn[s]; ++k) {
            if (a < wins[s][k].t1 && b > wins[s][k].t0) return true;
        }
        return false;
    };

    // backward table: life[t][s] = how many frames the player can still survive starting on side s at frame t
    const int H = 260;
    const int m = (int)ceilf((float)degPer / degPerFrame) + 1;        // frames to walk to a neighbour's centre
    const int mh = m / 2;
    static short life[H + 2][12];
    for (int s = 0; s < n; ++s) life[H][s] = 0;
    for (int t = H - 1; t >= 0; --t) {
        for (int s = 0; s < n; ++s) {
            if (busy(s, (float)t, (float)t + 1.0f)) {
                life[t][s] = 0;
                continue;
            }
            int best = 1 + life[t + 1][s];
            for (int dir = -1; dir <= 1; dir += 2) {
                int s2 = ((s + dir) % n + n) % n;
                if (busy(s, (float)t, (float)(t + mh)) || busy(s2, (float)(t + mh), (float)(t + m))) continue;
                int ta = t + m > H ? H : t + m;
                int l = m + life[ta][s2];
                if (l > best) best = l;
            }
            life[t][s] = (short)best;
        }
    }

    // candidate targets: shortest collision-free walks (both directions), then still alive on arrival
    int bestT = -1, bestDir = 0;
    float bestScore = -1e30f;
    for (int t = 0; t < n; ++t) {
        for (int dirSel = 0; dirSel < 2; ++dirSel) {
            const int dir = dirSel == 0 ? 1 : -1;
            const int steps = dir == 1 ? (t - cur + n) % n : (cur - t + n) % n;
            if (steps == 0 && dirSel == 1) continue;
            const float centre = (float)(t * degPer + degPer / 2);
            float angDist = dir == 1 ? fmodf(centre - (float)ang + 720.0f, 360.0f) : fmodf((float)ang - centre + 720.0f, 360.0f);
            if (steps == 0) angDist = 0.0f;
            else if (angDist < 1.0f) angDist = 360.0f;
            const float tArrive = angDist / degPerFrame;
            bool ok = true;
            // sides crossed on the way, with the time the player is inside each of them
            float prevT = 0.0f;
            for (int k = 0; k < steps && ok; ++k) {
                int s = ((cur + dir * k) % n + n) % n;
                float boundary = dir == 1 ? (float)((ang / degPer + k + 1) * degPer) - (float)ang
                                          : (float)ang - (float)((ang / degPer - k) * degPer);
                float tb = boundary / degPerFrame;
                if (busy(s, prevT, tb)) ok = false;
                prevT = tb;
            }
            if (ok && steps > 0 && busy(t, prevT, tArrive + 1.0f)) ok = false;
            int ta = (int)ceilf(tArrive) + 1;
            if (ta > H) ta = H;
            float score = (ok ? 100000.0f : 0.0f) + (float)life[ta][t] * 4.0f - 5.0f * (float)steps - tArrive * 0.5f;
            if (t == g_botTarget) score += 30.0f;                     // stick to the committed target
            if (score > bestScore) {
                bestScore = score;
                bestT = t;
                bestDir = dir;
            }
        }
    }
    g_botTarget = bestT;
    const int targetAngle = bestT * degPer + degPer / 2;
    int diff = (bestDir == 1) ? ((targetAngle - ang) % 360 + 360) % 360 : -(((ang - targetAngle) % 360 + 360) % 360);
    if (bestT == cur) {
        diff = ((targetAngle - ang) % 360 + 540) % 360 - 180;       // already on the side: just centre
    }
    A(uint8_t, app + O_LEFT) = 0;
    A(uint8_t, app + O_RIGHT) = 0;
    if (diff > 3) {
        A(uint8_t, app + O_LEFT) = 1;       // left raises the angle in this game
    } else if (diff < -3) {
        A(uint8_t, app + O_RIGHT) = 1;
    }
}

// ---- rotation / player multipliers (applied on the delta of the update) ----------------------------------------------
float g_prevRot = 0.0f;
int g_prevAngle = 0;

float wrap360f(float a)
{
    a = fmodf(a, 360.0f);
    return a < 0 ? a + 360.0f : a;
}
}   // namespace

// ---- hooks called by the game code ---------------------------------------------------------------------------------
bool extras_death_hook(int app)
{
    if (g_cheatGod) {
        return true;
    }
    if (g_rewindDeath && !g_rewinding && g_autoRewind == 0 && A(float, app + O_DEATH) >= 1.0f && g_ringCount > 45 + 2) {
        g_autoRewind = (int)(g_rewindSecs * 60.0f);
        if (g_autoRewind > g_ringCount) {
            g_autoRewind = g_ringCount;
        }
        return true;        // the death is cancelled; the next frame starts rewinding
    }
    return false;
}

// ---- public ---------------------------------------------------------------------------------------------------------
float cheats_music_rate()
{
    if (g_rewinding && g_musicRewind) {
        return -2.0f;
    }
    return g_musicSlow ? (float)cheats_time_scale() : 1.0f;
}

bool cheats_rewinding() { return g_rewinding; }

bool cheats_in_run() { return playing(APP); }

bool cheats_paused() { return g_paused; }
bool cheats_capturing() { return g_capture >= 0; }

void cheats_init(const std::string& baseDir)
{
    g_base = baseDir;
    defaults();
    FILE* f = fopen((g_base + "/cheats.cfg").c_str(), "r");
    if (!f) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        unsigned v = 0;
        if (sscanf(line, "bind.%63[^=]=%u", key, &v) == 2) {
            for (int a = 0; a < A_COUNT; ++a) {
                if (!strcmp(key, kActs[a].key)) {
                    g_bind[a] = v;
                }
            }
        } else if (sscanf(line, "rewindsecs=%u", &v) == 1 && v >= 1 && v <= 10) {
            g_rewindSecs = (float)v;
        } else if (sscanf(line, "musicslow=%u", &v) == 1) {
            g_musicSlow = v != 0;
        } else if (sscanf(line, "musicrewind=%u", &v) == 1) {
            g_musicRewind = v != 0;
        }
    }
    fclose(f);
}

void cheats_save()
{
    FILE* f = fopen((g_base + "/cheats.cfg").c_str(), "w");
    if (!f) {
        return;
    }
    for (int a = 0; a < A_COUNT; ++a) {
        fprintf(f, "bind.%s=%u\n", kActs[a].key, g_bind[a]);
    }
    fprintf(f, "rewindsecs=%u\n", (unsigned)g_rewindSecs);
    fprintf(f, "musicslow=%u\nmusicrewind=%u\n", g_musicSlow ? 1u : 0u, g_musicRewind ? 1u : 0u);
    fclose(f);
}

void cheats_event(const SDL_Event& e)
{
    switch (e.type) {
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        const int b = e.cbutton.button;
        if (b >= 0 && b < 21) {
            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                g_raw |= 1u << b;
                if (g_capture >= 0) g_capMask |= 1u << b;
            } else {
                g_raw &= ~(1u << b);
            }
        }
        break;
    }
    case SDL_CONTROLLERAXISMOTION:
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            const int bit = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? BIT_ZL : BIT_ZR;
            if (e.caxis.value > 16000) {
                g_raw |= 1u << bit;
                if (g_capture >= 0) g_capMask |= 1u << bit;
            } else if (e.caxis.value < 8000) {
                g_raw &= ~(1u << bit);
            }
        }
        break;
    default:
        break;
    }
    // finish a rebinding when everything was released again
    if (g_capture >= 0 && g_capMask != 0 && g_raw == 0) {
        if (popcount(g_capMask) > 3) {
            toast("too many buttons");
        } else {
            g_bind[g_capture] = g_capMask;
            toast("%s -> %s", kActs[g_capture].label, maskName(g_capMask).c_str());
            cheats_save();
        }
        g_capture = -1;
        g_capMask = 0;
    }
}

bool cheats_frame(double dt, bool bindsActive, bool overlayHalt)
{
    g_clock += dt;
    if (g_capture < 0 && bindsActive) {
        pollBinds();
        g_slowHeld = g_active[A_SLOWMO];
    } else {
        for (int a = 0; a < A_COUNT; ++a) g_active[a] = false;
        g_slowHeld = false;
    }
    const int app = APP;
    const bool inRun = A(int, app + O_STAGE) >= 0;
    // rewind: held button or automatic rewind after a death
    const bool wantRewind = (g_active[A_REWIND] || g_autoRewind > 0) && inRun && !overlayHalt;
    g_rewinding = false;
    if (wantRewind) {
        // the first moments of a run (intro zoom / spawn) are not safe to restore: keep a floor of ~0.75 s
        const int kFloor = 45;
        const int steps = g_ringCount - kFloor >= 2 ? 2 : g_ringCount - kFloor;
        if (steps <= 0) {
            g_autoRewind = 0;
        } else {
            if (ringPop(app, steps)) {
                g_rewinding = true;
                if (g_autoRewind > 0) {
                    g_autoRewind -= steps;
                    if (g_autoRewind < 0) g_autoRewind = 0;
                }
            } else {
                g_autoRewind = 0;
            }
        }
        if (g_rewinding) {
            return true;
        }
    }
    if (overlayHalt) {
        if (g_paused && g_stepFrames > 0) {
            --g_stepFrames;
            return false;
        }
        return true;
    }
    if (g_paused) {
        if (g_stepFrames > 0) {
            --g_stepFrames;
            return false;
        }
        return true;
    }
    return false;
}

double cheats_time_scale()
{
    double f = (double)g_speed;
    if (g_slowToggle || g_slowHeld) {
        f *= (double)g_slowFactor;
    }
    return f;
}

void cheats_override_input(int app)
{
    botSteer(app);
}

void cheats_before_update(int app)
{
    if (g_forceHyper) {
        A(uint8_t, app + 0x5508) = 1;
    }
    // the beat pulse reads the music position from the game clock: keep it on the real (slowed / reversed / resumed) audio
    if (A(int, app + O_MUSIC) >= 0) {
        const double p = audio_music_pos_ms(app + (A(int, app + O_MUSIC) * 9 + 0x10023) * 4);
        if (p >= 0) {
            const int now = FUN_004387b0(), off = A(int, app + O_MUSIC_OFF);
            const double cur = (double)(now - A(int, app + O_MUSIC_T0) + off);
            if (fabs(cur - p) > 40.0 || fabs(cheats_music_rate() - 1.0) > 1e-3) {
                A(int, app + O_MUSIC_T0) = now + off - (int)p;
            }
        }
    }
    g_prevRot = A(float, app + O_ROT);
    g_prevAngle = A(int, app + O_ANGLE_PREV);
}

void cheats_after_update(int app)
{
    if (A(int, app + O_STAGE) >= 0) {
        // rotation of the stage: scale the per-frame change
        float mulRot = g_rotFreeze ? 0.0f : g_rotMul;
        if (g_rotReverse) {
            mulRot = -mulRot;
        }
        if (mulRot != 1.0f) {
            float cur = A(float, app + O_ROT);
            float d = fmodf(cur - g_prevRot + 540.0f, 360.0f) - 180.0f;
            A(float, app + O_ROT) = wrap360f(g_prevRot + d * mulRot);
        }
        // player speed
        if (g_playerFast && g_playerBoost != 1.0f && A(float, app + O_DEATH) < 1.0f) {
            int cur = A(int, app + O_ANGLE_PREV);
            int d = ((cur - g_prevAngle) % 360 + 540) % 360 - 180;
            if (d != 0) {
                int n = ((g_prevAngle + (int)lroundf((float)d * g_playerBoost)) % 360 + 360) % 360;
                A(int, app + O_ANGLE_PREV) = n;
                A(int, app + O_ANGLE) = n;
            }
        }
        if (g_nowalls) {
            A(int, app + O_WALL_N) = 0;
        }
        if (g_nopulse) {
            A(float, app + O_PULSE) = 0.0f;
        }
        if (playing(app) && !g_rewinding) {
            ringPush(app);
        }
    } else {
        g_ringCount = 0;
        g_autoRewind = 0;
    }
}

void cheats_before_draw(int app)
{
    (void)app;
}

bool cheats_osd(std::string& out)
{
    out.clear();
    auto add = [&](const char* s) { out += out.empty() ? s : std::string("  ") + s; };
    if (g_cheatGod) add("GOD");
    if (g_nowalls) add("NO WALLS");
    if (g_bot) add("AUTOPILOT");
    if (g_paused) add("PAUSED");
    if (g_speed != 1.0f) {
        char b[24];
        snprintf(b, sizeof b, "%.2fx", g_speed);
        add(b);
    }
    if (g_slowToggle || g_slowHeld) add("SLOW-MO");
    if (g_rewinding) add("REWIND");
    if (g_rewindDeath) add("REWIND-ON-DEATH");
    if (g_rotFreeze) add("ROT FROZEN");
    if (g_rotReverse) add("ROT REVERSED");
    if (g_playerFast) add("FAST PLAYER");
    if (g_nopulse) add("NO PULSE");
    if (g_clock < g_toastUntil) add(g_toast.c_str());
    return !out.empty();
}

bool cheats_run(const std::string& a)
{
    for (int i = 0; i < A_COUNT; ++i) {
        if (a == kActs[i].key) {
            if (kActs[i].hold) {
                g_active[i] = !g_active[i];
                if (i == A_SLOWMO) g_slowHeld = g_active[i];
            } else {
                fire(i);
            }
            return true;
        }
    }
    if (a == "hyper") {
        g_forceHyper = !g_forceHyper;
        return true;
    }
    if (a == "slowtoggle") {
        g_slowToggle = !g_slowToggle;
        return true;
    }
    if (a == "rewind5") {                    // test: start an automatic rewind of 2 s
        g_autoRewind = g_ringCount < 120 ? g_ringCount : 120;
        return true;
    }
    return false;
}

// ---- the menu tab -----------------------------------------------------------------------------------------------------
namespace {
void bindRow(int a)
{
    ImGui::PushID(a);
    ImGui::TextDisabled("%s", maskName(g_bind[a]).c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.0f + ImGui::GetWindowWidth() * 0.62f);
    if (g_capture == a) {
        if (ImGui::Button("press buttons...")) {
            g_capture = -1;
            g_capMask = 0;
        }
    } else if (ImGui::Button("Bind")) {
        g_capture = a;
        g_capMask = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("X")) {
        g_bind[a] = 0;
        cheats_save();
    }
    ImGui::PopID();
}

// a toggle row: checkbox + binding
void toggleRow(int a, const char* label, bool* v)
{
    ImGui::PushID(100 + a);
    ImGui::Checkbox(label, v);
    ImGui::PopID();
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.40f);
    bindRow(a);
}

void buttonRow(int a, const char* label)
{
    ImGui::PushID(200 + a);
    if (ImGui::Button(label)) {
        fire(a);
    }
    ImGui::PopID();
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.40f);
    bindRow(a);
}
}   // namespace

void cheats_tab()
{
    if (g_capture >= 0) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "Press the button (or combo) for: %s - release everything to confirm",
                           kActs[g_capture].label);
    } else {
        ImGui::TextDisabled("Bindings work in game with this menu closed. Combos are fine (e.g. L + X). Defaults: Y rewind, X slow-mo, - pause, stick clicks = save / load 1.");
    }
    ImGui::SeparatorText("Survival");
    bool god = g_cheatGod;
    if (ImGui::Checkbox("God mode / no collision", &god)) g_cheatGod = god;
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.40f);
    bindRow(A_GOD);
    toggleRow(A_NO_WALLS, "No walls at all", &g_nowalls);
    toggleRow(A_BOT, "Autopilot (plays for you)", &g_bot);
    buttonRow(A_CLEAR_WALLS, "Clear all walls now");

    ImGui::SeparatorText("Time");
    ImGui::SliderFloat("Game speed", &g_speed, 0.05f, 4.0f, "%.2fx");
    ImGui::SameLine();
    if (ImGui::Button("1x##spd")) g_speed = 1.0f;
    buttonRow(A_SPEED_UP, "Speed +0.05");
    buttonRow(A_SPEED_DOWN, "Speed -0.05");
    buttonRow(A_SPEED_RESET, "Speed 1x");
    ImGui::SliderFloat("Slow-mo factor", &g_slowFactor, 0.05f, 0.9f, "%.2fx");
    ImGui::Checkbox("Slow-mo on (toggle)", &g_slowToggle);
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.40f);
    ImGui::PushID("slowhold");
    bindRow(A_SLOWMO);
    ImGui::PopID();
    toggleRow(A_PAUSE, "Pause", &g_paused);
    buttonRow(A_STEP, "Step one frame");
    ImGui::SliderFloat("Rewind buffer (s)", &g_rewindSecs, 1.0f, 10.0f, "%.0f s");
    ImGui::Text("Rewind (hold):  %d frames buffered", g_ringCount);
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.40f);
    bindRow(A_REWIND);
    toggleRow(A_REWIND_DEATH, "Rewind on death (instead of dying)", &g_rewindDeath);
    ImGui::Checkbox("Music follows slow-mo / speed (off = bypass)", &g_musicSlow);
    ImGui::Checkbox("Music rewinds too (off = bypass)", &g_musicRewind);
    for (int i = 0; i < 3; ++i) {
        char l1[32], l2[32];
        snprintf(l1, sizeof l1, "Save state %d%s", i + 1, g_slotValid[i] ? " *" : "");
        snprintf(l2, sizeof l2, "Load state %d", i + 1);
        buttonRow(A_SAVE1 + i, l1);
        buttonRow(A_LOAD1 + i, l2);
    }
    buttonRow(A_SKIP_TIME, "Skip +10 seconds");

    ImGui::SeparatorText("Rotation, player, visuals");
    ImGui::SliderFloat("Rotation speed", &g_rotMul, -3.0f, 3.0f, "%.2fx");
    toggleRow(A_ROT_FREEZE, "Freeze rotation", &g_rotFreeze);
    toggleRow(A_ROT_REVERSE, "Reverse rotation", &g_rotReverse);
    ImGui::SliderFloat("Fast player boost", &g_playerBoost, 1.0f, 4.0f, "%.2fx");
    toggleRow(A_PLAYER_FAST, "Fast player", &g_playerFast);
    toggleRow(A_NO_PULSE, "No beat pulse", &g_nopulse);
    if (ImGui::Button("Reset all bindings")) {
        defaults();
        cheats_save();
    }
}
