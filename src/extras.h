// Port-only extras: local high-score screen and the "super secret" Dear ImGui menu (cheats, settings, debug).
#pragma once
#include <SDL.h>
#include <string>

void extras_init(const std::string& baseDir);
void extras_shutdown();

// Feed every SDL event here first; true = the extras consumed it (don't pass it to the game).
// Button/key releases are never consumed so the game's held-state tables can't get stuck.
bool extras_event(const SDL_Event& e);

// Called once per frame before the game update. Returns the dt to give the game; *skip is set when the
// game update should not run this frame (paused / menu open with pause-on-open).
double extras_dt(double dt, bool* skip);

// Called after the game was drawn, before presenting.
void extras_draw();

// triggers (also used by the scripted-input test hook)
void extras_toggle_menu();
void extras_open_scores();
void extras_select_tab(int t);   // test hook: 0 cheats, 1 settings, 2 debug, 3 about
bool extras_overlay_open();
void extras_note_scores_bar();      // the HUD drew its "HIGH SCORES" bar this frame
bool extras_scores_bar_visible();

// state read by the rest of the port
extern bool g_cheatGod;        // ignore wall collisions
bool extras_swap_lr();         // left/right controls swapped
