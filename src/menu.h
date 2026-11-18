#pragma once

#include <Windows.h>

namespace menu {

void set_hwnd(HWND h);
void install();
void toggle();
void poll(float sw, float sh);
void render();

} // namespace menu
