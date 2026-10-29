// Window + immediate-mode 2D renderer (coloured triangles, rectangles, alpha-textured glyph quads).
#pragma once
#include <stdint.h>
#include <string>

bool gfx_init(int w, int h, bool fullscreen);
void gfx_shutdown();
void gfx_begin_frame();            // clears with the clear colour
void gfx_end_frame();              // present
void gfx_size(int* w, int* h);     // drawable size in pixels
void gfx_set_clear(int r, int g, int b);
void gfx_set_color(int r, int g, int b);               // current rect/text colour (0..255)
void gfx_rect(float x, float y, float w, float h);     // filled with the current colour
void gfx_tri(const float* xy3, const uint8_t* rgba4);  // 3 vertices, one colour
void gfx_flush();
void gfx_hud_scope(bool on);   // HUD drawing in progress (widescreen edge placement)

// text: fonts are loaded per size index (1..5) at the pixel sizes the layout asks for
void text_load(int sizeIdx, int sizePx, const char* ttfPath);
float text_width(int sizeIdx, const std::string& s);
float text_height(int sizeIdx, const std::string& s);   // height of the string's bounding box
void text_draw(int sizeIdx, float x, float baselineY, const std::string& s);

void gfx_screenshot(const char* path);   // PNG of the current back buffer

// extras (secret menu): colour filters / mirror / vsync and the Dear ImGui renderer
void gfx_set_filter(int mode);     // 0 none, 1 invert, 2 grayscale, 3 sepia, 4 red/blue swap, 5 high contrast
void gfx_set_mirror(bool on);
void gfx_set_vsync(bool on);
unsigned gfx_make_alpha_texture(const unsigned char* px, int w, int h);
struct ImDrawData;
void gfx_render_imgui(ImDrawData* dd);
