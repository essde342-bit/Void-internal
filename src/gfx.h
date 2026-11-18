#pragma once
#include <cstdint>

namespace gfx {

inline constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

bool  ready();
void  shutdown();
void  draw_line(float x0, float y0, float x1, float y1, uint32_t col);
void  draw_rect(float x, float y, float w, float h, uint32_t col);
void  draw_rect_filled(float x, float y, float w, float h, uint32_t col);
void  draw_text(float x, float y, uint32_t col, bool centered, const char* fmt, ...);
float text_width(const char* s);

} // namespace gfx
