// Port-only cheats of the secret menu: god mode, time control (speed / slow-mo / pause / rewind / save states),
// rotation + player speed, wall tricks, autopilot - each one bindable to a controller button / combo.
#pragma once
#include <SDL.h>
#include <string>

void cheats_init(const std::string& baseDir);
void cheats_save();
void cheats_event(const SDL_Event& e);     // every SDL event (before the menu consumes it): tracks the pad for binds
bool cheats_capturing();                    // waiting for the user to press the new binding

// once per render frame. Returns true when the game update must not run this frame (paused / rewinding).
bool cheats_frame(double dt, bool bindsActive, bool overlayHalt);
double cheats_time_scale();                 // game time factor (speed * slow-mo)

// around the game update (see oneFrame in main.cpp)
void cheats_override_input(int app);   // called by the input code after the device flags were set (autopilot)
void cheats_before_update(int app);
void cheats_after_update(int app);
void cheats_before_draw(int app);

void cheats_tab();                          // the "Cheats" tab content (below the progress section)
bool cheats_osd(std::string& out);          // active cheats summary; false when none
bool cheats_run(const std::string& action); // scripted test hook ("rewind", "save1", "load1", "bot", ...)
bool cheats_paused();
bool cheats_rewinding();
float cheats_music_rate();                  // playback rate for the music (slow-mo / speed / reverse on rewind)
bool cheats_in_run();                       // a run is in progress and the player is alive
