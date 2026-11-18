// Void Internal — GUI menu. Input via render-thread polling (no WndProc, no SDL conflict).
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include "config.h"
#include "gfx.h"

namespace menu {
    static bool  g_visible = true;
    static float g_x = 60.f, g_y = 60.f;
    static float g_w = 260.f, g_h = 210.f;
    static bool  g_dragging = false;
    static float g_drag_ox = 0.f, g_drag_oy = 0.f;
    static HWND  g_hwnd = nullptr;
    static bool  g_last_rshift = false;
    static bool  g_last_lmb = false;

    static bool in_rect(float px, float py, float rx, float ry, float rw, float rh) {
        return px >= rx && px <= rx + rw && py >= ry && py <= ry + rh;
    }

    void install() {
        g_hwnd = FindWindowA("SDL_app", nullptr);
        if (!g_hwnd) g_hwnd = GetForegroundWindow();
        if (!g_hwnd) g_hwnd = GetDesktopWindow();
    }

    void toggle() { g_visible = !g_visible; }

    void poll(float sw, float sh) {
        bool rs = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
        if (rs && !g_last_rshift) toggle();
        g_last_rshift = rs;

        if (!g_visible || !g_hwnd) { g_last_lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; return; }

        POINT p;
        GetCursorPos(&p);
        ScreenToClient(g_hwnd, &p);
        RECT cr;
        GetClientRect(g_hwnd, &cr);
        float cw = (float)(cr.right - cr.left);
        float chh = (float)(cr.bottom - cr.top);
        if (cw < 1.f) cw = sw;
        if (chh < 1.f) chh = sh;
        float mx = (float)p.x * (sw / cw);
        float my = (float)p.y * (sh / chh);

        bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool click = lmb && !g_last_lmb;
        g_last_lmb = lmb;

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
            if (in_rect(mx, my, g_x + 10.f, cy,         200.f, 20.f)) g_cfg.box       = !g_cfg.box;
            if (in_rect(mx, my, g_x + 10.f, cy + 26.f,  200.f, 20.f)) g_cfg.skeleton  = !g_cfg.skeleton;
            if (in_rect(mx, my, g_x + 10.f, cy + 52.f,  200.f, 20.f)) g_cfg.health    = !g_cfg.health;
            if (in_rect(mx, my, g_x + 10.f, cy + 78.f,  200.f, 20.f)) g_cfg.name      = !g_cfg.name;
            if (in_rect(mx, my, g_x + 10.f, cy + 104.f, 200.f, 20.f)) g_cfg.teamcheck = !g_cfg.teamcheck;
        }
    }

    static void checkbox(float x, float y, bool state, const char* label) {
        uint32_t bg = state ? gfx::rgba(180, 120, 255, 255) : gfx::rgba(60, 60, 60, 220);
        gfx::draw_rect_filled(x, y, 16.f, 16.f, bg);
        gfx::draw_rect(x, y, 16.f, 16.f, gfx::rgba(255, 255, 255, 255));
        if (state) {
            gfx::draw_line(x + 3.f, y + 8.f, x + 7.f, y + 12.f, gfx::rgba(255, 255, 255, 255));
            gfx::draw_line(x + 7.f, y + 12.f, x + 13.f, y + 4.f, gfx::rgba(255, 255, 255, 255));
        }
        gfx::draw_text(x + 24.f, y + 1.f, gfx::rgba(255, 255, 255, 255), false, "%s", label);
    }

    void render() {
        if (!g_visible) return;
        gfx::draw_rect_filled(g_x, g_y, g_w, g_h, gfx::rgba(24, 24, 28, 235));
        gfx::draw_rect(g_x, g_y, g_w, g_h, gfx::rgba(180, 120, 255, 255));
        gfx::draw_rect_filled(g_x, g_y, g_w, 24.f, gfx::rgba(60, 40, 90, 255));
        gfx::draw_text(g_x + 10.f, g_y + 4.f, gfx::rgba(255, 255, 255, 255), false, "VOID INTERNAL");
        gfx::draw_text(g_x + g_w - 80.f, g_y + 4.f, gfx::rgba(200, 200, 200, 255), false, "RSHIFT hide");

        float cy = g_y + 40.f;
        checkbox(g_x + 10.f, cy,         g_cfg.box,       "Box ESP");
        checkbox(g_x + 10.f, cy + 26.f,  g_cfg.skeleton,  "Skeleton");
        checkbox(g_x + 10.f, cy + 52.f,  g_cfg.health,    "Health Bar");
        checkbox(g_x + 10.f, cy + 78.f,  g_cfg.name,      "Player Names");
        checkbox(g_x + 10.f, cy + 104.f, g_cfg.teamcheck, "Team Check");
        gfx::draw_text(g_x + 10.f, g_y + g_h - 20.f, gfx::rgba(150, 150, 150, 255), false, "END = unload");
    }
}
