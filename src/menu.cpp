// Void Internal — Custom D3D11 GUI menu (no ImGui dependency)
#include <Windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstring>
#include "config.h"

namespace gfx {
    void draw_rect_filled(float x, float y, float w, float h, uint32_t col);
    void draw_text(float x, float y, uint32_t col, bool centered, const char* fmt, ...);
    void draw_line(float x0, float y0, float x1, float y1, uint32_t col);
    static constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
    }
}

namespace menu {
    static bool g_visible = true;
    static float g_x = 50.f, g_y = 50.f;
    static float g_w = 250.f, g_h = 200.f;
    static bool g_dragging = false;
    static float g_drag_off_x = 0.f, g_drag_off_y = 0.f;
    static WNDPROC g_old_wndproc = nullptr;
    static HWND g_hwnd = nullptr;

    static bool point_in_rect(float px, float py, float rx, float ry, float rw, float rh) {
        return px >= rx && px <= rx + rw && py >= ry && py <= ry + rh;
    }

    static void draw_checkbox(float x, float y, bool* state, const char* label) {
        uint32_t bg = *state ? gfx::rgba(180, 120, 255, 255) : gfx::rgba(60, 60, 60, 200);
        gfx::draw_rect_filled(x, y, 16.f, 16.f, bg);
        gfx::draw_line(x, y, x + 16.f, y, gfx::rgba(255, 255, 255, 255));
        gfx::draw_line(x + 16.f, y, x + 16.f, y + 16.f, gfx::rgba(255, 255, 255, 255));
        gfx::draw_line(x + 16.f, y + 16.f, x, y + 16.f, gfx::rgba(255, 255, 255, 255));
        gfx::draw_line(x, y + 16.f, x, y, gfx::rgba(255, 255, 255, 255));
        
        if (*state) {
            gfx::draw_line(x + 3.f, y + 8.f, x + 7.f, y + 12.f, gfx::rgba(255, 255, 255, 255));
            gfx::draw_line(x + 7.f, y + 12.f, x + 13.f, y + 4.f, gfx::rgba(255, 255, 255, 255));
        }
        
        gfx::draw_text(x + 22.f, y + 2.f, gfx::rgba(255, 255, 255, 255), false, "%s", label);
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (g_visible) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            float mx = (float)p.x, my = (float)p.y;

            if (msg == WM_LBUTTONDOWN) {
                if (point_in_rect(mx, my, g_x, g_y, g_w, 24.f)) {
                    g_dragging = true;
                    g_drag_off_x = mx - g_x;
                    g_drag_off_y = my - g_y;
                    SetCapture(hwnd);
                    return 0;
                }
                // checkboxes
                float cy = g_y + 40.f;
                if (point_in_rect(mx, my, g_x + 10.f, cy, 16.f, 16.f)) { g_cfg.box = !g_cfg.box; return 0; }
                if (point_in_rect(mx, my, g_x + 10.f, cy + 24.f, 16.f, 16.f)) { g_cfg.skeleton = !g_cfg.skeleton; return 0; }
                if (point_in_rect(mx, my, g_x + 10.f, cy + 48.f, 16.f, 16.f)) { g_cfg.health = !g_cfg.health; return 0; }
                if (point_in_rect(mx, my, g_x + 10.f, cy + 72.f, 16.f, 16.f)) { g_cfg.name = !g_cfg.name; return 0; }
                if (point_in_rect(mx, my, g_x + 10.f, cy + 96.f, 16.f, 16.f)) { g_cfg.teamcheck = !g_cfg.teamcheck; return 0; }
            } else if (msg == WM_MOUSEMOVE && g_dragging) {
                g_x = mx - g_drag_off_x;
                g_y = my - g_drag_off_y;
                return 0;
            } else if (msg == WM_LBUTTONUP && g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                return 0;
            }

            if (point_in_rect(mx, my, g_x, g_y, g_w, g_h)) return 0;
        }
        return CallWindowProcA(g_old_wndproc, hwnd, msg, wp, lp);
    }

    void install() {
        g_hwnd = FindWindowA("SDL_app", nullptr);
        if (!g_hwnd) g_hwnd = GetForegroundWindow();
        if (g_hwnd && !g_old_wndproc) {
            g_old_wndproc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wnd_proc);
        }
    }

    void toggle() {
        g_visible = !g_visible;
    }

    void render() {
        if (!g_visible) return;

        // background
        gfx::draw_rect_filled(g_x, g_y, g_w, g_h, gfx::rgba(30, 30, 30, 220));
        // title bar
        gfx::draw_rect_filled(g_x, g_y, g_w, 24.f, gfx::rgba(60, 40, 90, 255));
        gfx::draw_text(g_x + 10.f, g_y + 4.f, gfx::rgba(255, 255, 255, 255), false, "VOID INTERNAL");
        gfx::draw_text(g_x + g_w - 70.f, g_y + 4.f, gfx::rgba(255, 255, 255, 255), false, "RSHIFT hide");

        float cy = g_y + 40.f;
        draw_checkbox(g_x + 10.f, cy, &g_cfg.box, "Box ESP");
        draw_checkbox(g_x + 10.f, cy + 24.f, &g_cfg.skeleton, "Skeleton");
        draw_checkbox(g_x + 10.f, cy + 48.f, &g_cfg.health, "Health Bar");
        draw_checkbox(g_x + 10.f, cy + 72.f, &g_cfg.name, "Player Names");
        draw_checkbox(g_x + 10.f, cy + 96.f, &g_cfg.teamcheck, "Team Check");
    }
}
