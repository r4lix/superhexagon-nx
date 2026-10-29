// HUD / menu text. The original draws it with ofTrueTypeFont inside two ~6000-line functions
// (FUN_00405dd0 / FUN_00407b50); this is the hand port on top of text_* from gfx.cpp.
#include "rt.h"
#include "hand_decl.h"
#include "gfx.h"
#include <string>

const std::string& rt_data_dir();

// original FUN_0041fa30: (re)load the five font sizes the layout asks for (ui+0x44..0x54, in points)
void FUN_0041fa30(int ui)
{
    std::string ttf = rt_data_dir() + "/fonts/bumpitup.ttf";
    for (int i = 0; i < 5; ++i) {
        int sz = A(int, ui + 0x44 + i * 4);
        if (sz > 0) {
            text_load(i + 1, sz, ttf.c_str());
        }
    }
}

