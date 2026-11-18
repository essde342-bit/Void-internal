// Void Internal — меню

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifndef WINVER
#define WINVER 0x0A00
#endif

#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <stdio.h>
#include <stdarg.h>

#include "config.h"
#include "gfx.h"
#include "menu.h"

namespace menu {

static bool  g_visible = true;
static float g_x = 60.f, g_y = 60.f, g_w = 280.f, g_h = 360.f;
static bool  g_dragging = false;
static float g_drag_ox = 0.f, g_drag_oy = 0.f;
static HWND  g_hwnd = nullptr;
static bool  g_hwnd_from_swap = false;
static bool  g_last_rshift = false, g_last_lmb = false;

static bool in_rect(float px, float py, float rx, float ry, float rw, float rh) {
    return px >= rx && px <= rx + rw && py >= ry && py <= ry + rh;
}

void set_hwnd(HWND h) {
    if (h && !g_hwnd_from_swap) {
        g_hwnd = h;
        g_hwnd_from_swap = true;
    }
}

void install() {
    if (g_hwnd_from_swap) return;
    g_hwnd = FindWindowA("SDL_app", nullptr);
    if (!g_hwnd) g_hwnd = GetForegroundWindow();
}

void toggle() {
    g_visible = !g_visible;
}

void poll(float sw, float sh) {
    bool rs = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
    if (rs && !g_last_rshift) toggle();
    g_last_rshift = rs;

    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool click = lmb && !g_last_lmb;
    g_last_lmb = lmb;

    if (!g_visible || !g_hwnd) return;

    POINT p;
    GetCursorPos(&p);
    ScreenToClient(g_hwnd, &p);

    RECT cr;
    GetClientRect(g_hwnd, &cr);

    float cw = (float)(cr.right - cr.left);
    float ch = (float)(cr.bottom - cr.top);
    if (cw < 1.f) cw = sw;
    if (ch < 1.f) ch = sh;

    float mx = (float)p.x * (sw / cw);
    float my = (float)p.y * (sh / ch);

    if (click && in_rect(mx, my, g_x, g_y, g_w, 24.f)) {
        g_dragging = true;
        g_drag_ox = mx - g_x;
        g_drag_oy = my - g_y;
    }

    if (!lmb) g_dragging = false;
    if (g_dragging && lmb) {
        g_x = mx - g_drag_ox;
        g_y = my - g_drag_oy;
    }

    if (click && !g_dragging) {
        float cy = g_y + 40.f;

        // ESP
        if (in_rect(mx, my, g_x + 10.f, cy,          220.f, 20.f)) g_cfg.box       = !g_cfg.box;
        if (in_rect(mx, my, g_x + 10.f, cy + 26.f,   220.f, 20.f)) g_cfg.skeleton  = !g_cfg.skeleton;
        if (in_rect(mx, my, g_x + 10.f, cy + 52.f,   220.f, 20.f)) g_cfg.health    = !g_cfg.health;
        if (in_rect(mx, my, g_x + 10.f, cy + 78.f,   220.f, 20.f)) g_cfg.name      = !g_cfg.name;
        if (in_rect(mx, my, g_x + 10.f, cy + 104.f,  220.f, 20.f)) g_cfg.teamcheck = !g_cfg.teamcheck;

        // Aimbot
        if (in_rect(mx, my, g_x + 10.f, cy + 156.f,  220.f, 20.f)) g_cfg.aimbot           = !g_cfg.aimbot;
        if (in_rect(mx, my, g_x + 10.f, cy + 182.f,  220.f, 20.f)) g_cfg.aimbot_show_fov  = !g_cfg.aimbot_show_fov;
        if (in_rect(mx, my, g_x + 10.f, cy + 208.f,  220.f, 20.f)) g_cfg.aimbot_vis_check = !g_cfg.aimbot_vis_check;
        if (in_rect(mx, my, g_x + 10.f, cy + 234.f,  220.f, 20.f)) g_cfg.aimbot_bone = (g_cfg.aimbot_bone + 1) % 3;
    }
}

static void checkbox(float x, float y, bool state, const char* label) {
    uint32_t bg = state ? gfx::rgba(180, 120, 255, 255) : gfx::rgba(60, 60, 60, 220);

    gfx::draw_rect_filled(x, y, 16.f, 16.f, bg);
    gfx::draw_rect(x, y, 16.f, 16.f, gfx::rgba(255, 255, 255, 255));

    if (state) {
        gfx::draw_line(x + 3.f, y + 8.f,  x + 7.f,  y + 12.f, gfx::rgba(255, 255, 255, 255));
        gfx::draw_line(x + 7.f, y + 12.f, x + 13.f, y + 4.f,  gfx::rgba(255, 255, 255, 255));
    }

    gfx::draw_text(x + 24.f, y + 1.f, gfx::rgba(255, 255, 255, 255), false, "%s", label);
}

static void label(float x, float y, uint32_t col, const char* fmt, ...) {
    char buf[128];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    gfx::draw_text(x, y, col, false, "%s", buf);
}

void render() {
    if (!g_visible) return;

    gfx::draw_rect_filled(g_x, g_y, g_w, g_h, gfx::rgba(24, 24, 28, 235));
    gfx::draw_rect(g_x, g_y, g_w, g_h, gfx::rgba(180, 120, 255, 255));
    gfx::draw_rect_filled(g_x, g_y, g_w, 24.f, gfx::rgba(60, 40, 90, 255));

    gfx::draw_text(g_x + 10.f, g_y + 4.f, gfx::rgba(255, 255, 255, 255), false, "VOID INTERNAL");
    gfx::draw_text(g_x + g_w - 90.f, g_y + 4.f, gfx::rgba(200, 200, 200, 255), false, "RSHIFT hide");

    float cy = g_y + 40.f;

    // ESP
    checkbox(g_x + 10.f, cy,          g_cfg.box,       "Box ESP");
    checkbox(g_x + 10.f, cy + 26.f,   g_cfg.skeleton,  "Skeleton");
    checkbox(g_x + 10.f, cy + 52.f,   g_cfg.health,    "Health Bar");
    checkbox(g_x + 10.f, cy + 78.f,   g_cfg.name,      "Player Names");
    checkbox(g_x + 10.f, cy + 104.f,  g_cfg.teamcheck, "Team Check");

    // Aimbot separator
    gfx::draw_rect_filled(g_x + 6.f, cy + 134.f, g_w - 12.f, 1.f, gfx::rgba(180, 120, 255, 180));
    label(g_x + 10.f, cy + 138.f, gfx::rgba(180, 120, 255, 255), "AIMBOT");

    checkbox(g_x + 10.f, cy + 156.f, g_cfg.aimbot,           "Enable (Mouse5)");
    checkbox(g_x + 10.f, cy + 182.f, g_cfg.aimbot_show_fov,  "Show FOV circle");
    checkbox(g_x + 10.f, cy + 208.f, g_cfg.aimbot_vis_check, "Visibility check");

    static const char* bone_names[] = { "Head", "Neck", "Chest" };
    label(g_x + 10.f, cy + 234.f, gfx::rgba(255, 255, 255, 255),
          "[ ] Bone: %s  (click to cycle)", bone_names[g_cfg.aimbot_bone % 3]);

    gfx::draw_text(g_x + 10.f, g_y + g_h - 20.f, gfx::rgba(150, 150, 150, 255), false, "END = unload");
}

} // namespace menu
