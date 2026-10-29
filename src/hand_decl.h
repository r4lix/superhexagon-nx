#pragma once
// Prototypes of the hand-written leaf functions the generated game code calls (implemented in src/*.cpp).
// Names are the original's addresses (see NOTES.md for what each one is).
#include <stdint.h>
#include "ud.h"

// --- C runtime / allocator ---
[[noreturn]] void FUN_00401240(void);               // std::_Xlength_error
[[noreturn]] void FUN_00404ad0(void);               // std::_Xlength_error
[[noreturn]] void FUN_00404ae0(void);               // std::_Xbad_alloc
int FUN_004eaf92(uint32_t size);                    // operator new
void FUN_004eafc2(int ptr);                         // operator delete
void FUN_004045e0(int str);                         // std::string::_Tidy
void FUN_0042dbe0(int vec, uint32_t n);             // vector<int>::reserve
int FUN_0043ef00(int p);                            // xml settings ctor
int FUN_004326f0(int p);                            // sound object ctor
void FUN_004329b0(int p);                           // sound object dtor
int FUN_00432730(int p);                            // font object ctor
int FUN_00432880(int p);                            // vec3 ctor
uint32_t FUN_004326a0(uint32_t a, uint32_t b, uint32_t c);  // log line (no-op)
void FUN_00432900(void);                            // log flush (no-op)
int FUN_00430130(int p);                            // steam leaderboard helper

// --- time / randomness ---
int FUN_004311c0(uint32_t lo, uint32_t hi, int n);   // floor(fmod(double, n)) (double passed as two words)
long double FUN_00431200(uint32_t lo, uint32_t hi, int n);  // fmod, leaves result in st0
void FUN_00424840(int obj);                          // trig tables of the ui object
long double FUN_00438900(float range);              // rand() * range / 32768
int FUN_004387b0(void);                             // milliseconds since start
long double FUN_00431120(void);                     // 1.0
long double FUN_00431130(void);                     // measured frame time
void FUN_00431180(...);
void FUN_004311a0(...);                           // store measured frame time (double*)

// --- window / drawing state ---
void FUN_004386c0(int r, int g, int b);             // clear colour
void FUN_004389c0(uint8_t r, uint8_t g, uint8_t b); // draw colour
void FUN_00438760(float x, float y, float w, float h);  // filled rectangle
bool FUN_00438710(void);                            // fullscreen?
void FUN_004389f0(uint8_t fullscreen);
void FUN_004387a0(void);                            // quit
int FUN_004387c0(void);                             // drawable width
int FUN_004388a0(void);                             // drawable height
void FUN_004388d0(void);                            // hide cursor
void FUN_00438af0(void);                            // show cursor
void FUN_00439170(void);                            // toggle fullscreen
void FUN_00438b20(float v);                         // music volume
void FUN_00438b40(float v);                         // sound volume
void FUN_0043a2e0(int vec, UD4 x, UD4 y, UD4 z);

// --- shape batch (triangle renderer) ---
int FUN_0042f880(int this_, float vertsPerShape, int maxPerLayer, int layers);
void FUN_0042fe20(int batch, UD4 r, UD4 g, UD4 b, UD4 a);
void FUN_0042fae0(int batch, UD8 p1, UD4 z1, UD8 p2, UD4 z2, UD8 p3, UD4 z3, int layer);
void FUN_0042fd90(int batch);                       // begin: reset all layers
void FUN_0042fdd0(int batch);                       // end: draw everything queued

// --- sound ---
void FUN_00439190(int snd);                         // play
void FUN_0043a340(int snd, int offsetMs);           // (re)start music at offset
void FUN_0043a4c0(int snd, float vol);              // set volume
void FUN_0043cbd0(int snd);                         // stop

// --- text / fonts ---
void FUN_0041fa30(int ui);                          // (re)load fonts at the sizes in the layout table

// --- persistence ---
void FUN_00415e80(int game);
void FUN_00416e50(int game);
char FUN_00417ef0(int game);
void FUN_00418640(int game);
void FUN_00419290(int game);
void FUN_00419930(int game);
void FUN_004050d0(int game);                        // local high score insertion
int FUN_004288d0(int p);                            // steam
void GameLoadPersisted(int game);                   // replaces the original constructor's file loading
bool extras_death_hook(int app);
extern bool g_cheatGod;                           // extras: ignore wall collisions (secret menu)
void FillGameTrig(int game);                        // sin/cos tables (vector<double> in the game object)
void CreditNames(int game);                        // tester list shown on the credits pages (vector<string> at game+0x576c)
void NameBackspace(int str, int placeholder);       // text-entry delete

// --- input / HUD ---
void FUN_004273d0(int app);                         // poll devices -> logical flags
void FUN_00405dd0(int game, int ui, int p);         // HUD
void FUN_00407b50(int game, int ui, int p);         // HUD
long long FUN_004023c0(...);

// --- std::string internals used by the name entry ---
int FUN_00404ba0(int str, uint32_t src, uint32_t n);        // append(src, n)
int FUN_00415c00(int str, uint32_t pos, uint32_t src, uint32_t n);  // insert(pos, src, n)
void FUN_00417ae0(int str, uint8_t c);                      // push_back
void FUN_004145d0(int p);                                   // resolve data path (unused)
