// Void Internal — CS2 internal ESP. Single TU, Xenos/manual-map compatible.
// No SEH, no FreeLibrary, no CRT static-init reliance.
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Offsets. Дамп 12.11.2026 (a2x/cs2-dumper, cs2-sdk.com).
// ---------------------------------------------------------------------------
namespace off {
    constexpr uintptr_t dwEntityList            = 0x2709010;
    constexpr uintptr_t dwLocalPlayerPawn       = 0x2553D38;
    constexpr uintptr_t dwViewMatrix            = 0x2559640;

    constexpr uintptr_t m_iHealth               = 0x34C;
    constexpr uintptr_t m_lifeState             = 0x354;
    constexpr uintptr_t m_pGameSceneNode        = 0x330;
    constexpr uintptr_t m_iTeamNum              = 0x3E7;

    constexpr uintptr_t m_vecOrigin             = 0x80;
    constexpr uintptr_t m_modelState            = 0x140;
    constexpr uintptr_t m_pBoneArray            = 0x80;

    constexpr uintptr_t m_hOriginalController   = 0x1560;
    constexpr uintptr_t m_hPawn                 = 0x6BC;
    constexpr uintptr_t m_iszPlayerName         = 0x6FC;

    constexpr uintptr_t ENTITY_IDENTITIES       = 0x10;
    constexpr int       MAX_ENTITIES            = 4096;
}

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct ViewMatrix { float m[4][4]; };

static bool world_to_screen(const Vec3& w, Vec2& s, const ViewMatrix& vm, float sw, float sh) {
    float cw = vm.m[3][0] * w.x + vm.m[3][1] * w.y + vm.m[3][2] * w.z + vm.m[3][3];
    if (cw < 0.001f) return false;
    float cx = vm.m[0][0] * w.x + vm.m[0][1] * w.y + vm.m[0][2] * w.z + vm.m[0][3];
    float cy = vm.m[1][0] * w.x + vm.m[1][1] * w.y + vm.m[1][2] * w.z + vm.m[1][3];
    s.x = sw * 0.5f + (cx / cw) * sw * 0.5f;
    s.y = sh * 0.5f - (cy / cw) * sh * 0.5f;
    return true;
}

static bool coord_sane(float v) {
    return v == v && v > -1e5f && v < 1e5f;
}

struct Config {
    bool box;
    bool skeleton;
    bool health;
    bool name;
    bool teamcheck;
} g_cfg;

static constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

void esp_render(float sw, float sh);

// Menu forward declarations
namespace menu {
    void install();
    void toggle();
    void render();
}

namespace gfx {
    struct Vertex { float x, y, z, w; float r, g, b, a; float u, v; };
    struct Batch  { bool textured; uint32_t start; uint32_t count; };

    static ID3D11Device*           g_dev = nullptr;
    static ID3D11DeviceContext*    g_ctx = nullptr;
    static ID3D11InputLayout*      g_il = nullptr;
    static ID3D11VertexShader*     g_vs = nullptr;
    static ID3D11PixelShader*      g_ps_solid = nullptr;
    static ID3D11PixelShader*      g_ps_tex = nullptr;
    static ID3D11Buffer*           g_vb = nullptr;
    static uint32_t                g_vb_cap = 0;
    static ID3D11BlendState*       g_blend = nullptr;
    static ID3D11RasterizerState*  g_rs = nullptr;
    static ID3D11ShaderResourceView* g_font_srv = nullptr;
    static ID3D11SamplerState*     g_samp = nullptr;
    static ID3D11RenderTargetView* g_rtv = nullptr;

    static std::vector<Vertex> g_verts;
    static std::vector<Batch>  g_batches;

    static float g_cw[256], g_ch = 16.f, g_u0[256], g_v0[256], g_u1[256], g_v1[256];
    static bool  g_ready = false;

    static const char* VS_SRC =
        "struct VI { float4 p : POSITION; float4 c : COLOR; float2 t : TEXCOORD; };\n"
        "struct VO { float4 p : SV_POSITION; float4 c : COLOR; float2 t : TEXCOORD; };\n"
        "VO main(VI i) { VO o; o.p = i.p; o.c = i.c; o.t = i.t; return o; }\n";

    static const char* PS_SOLID_SRC =
        "struct PI { float4 p : SV_POSITION; float4 c : COLOR; float2 t : TEXCOORD; };\n"
        "float4 main(PI i) : SV_Target { return i.c; }\n";

    static const char* PS_TEX_SRC =
        "struct PI { float4 p : SV_POSITION; float4 c : COLOR; float2 t : TEXCOORD; };\n"
        "Texture2D tex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "float4 main(PI i) : SV_Target { float4 t = tex.Sample(samp, i.t); return float4(i.c.rgb, i.c.a * t.a); }\n";

    static void build_font() {
        const int TW = 512, TH = 512;
        HDC hdc = CreateCompatibleDC(nullptr);
        HFONT hf = CreateFontA(16, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, "Arial");
        HFONT old = (HFONT)SelectObject(hdc, hf);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = TW;
        bmi.bmiHeader.biHeight = -TH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP hb = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(hdc, hb);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        memset(bits, 0, (size_t)TW * TH * 4);

        int px = 0, py = 0, rowh = 0;
        for (int i = 32; i < 256; i++) {
            SIZE sz;
            char ch = (char)i;
            GetTextExtentPoint32A(hdc, &ch, 1, &sz);
            if (px + sz.cx + 2 > TW) { px = 0; py += rowh + 2; rowh = 0; }
            TextOutA(hdc, px, py, &ch, 1);
            g_cw[i] = (float)sz.cx;
            g_u0[i] = (float)px / TW;
            g_v0[i] = (float)py / TH;
            g_u1[i] = (float)(px + sz.cx) / TW;
            g_v1[i] = (float)(py + sz.cy) / TH;
            if (sz.cy > rowh) rowh = sz.cy;
            px += sz.cx + 2;
        }
        SelectObject(hdc, old);

        uint32_t* pix = (uint32_t*)bits;
        for (int i = 0; i < TW * TH; i++) {
            uint8_t r = (uint8_t)((pix[i] >> 16) & 0xFF);
            uint8_t g = (uint8_t)((pix[i] >> 8) & 0xFF);
            uint8_t b = (uint8_t)(pix[i] & 0xFF);
            uint8_t a = (uint8_t)((r + g + b) / 3);
            pix[i] = ((uint32_t)a << 24) | 0x00FFFFFFu;
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = TW; td.Height = TH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = bits; sd.SysMemPitch = TW * 4;
        ID3D11Texture2D* tex = nullptr;
        g_dev->CreateTexture2D(&td, &sd, &tex);
        g_dev->CreateShaderResourceView(tex, nullptr, &g_font_srv);
        if (tex) tex->Release();

        D3D11_SAMPLER_DESC sm = {};
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        g_dev->CreateSamplerState(&sm, &g_samp);

        DeleteObject(hb);
        DeleteObject(hf);
        DeleteDC(hdc);
    }

    static bool init(IDXGISwapChain* swap) {
        if (g_ready) return true;
        if (!swap) return false;
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev))) return false;
        g_dev->GetImmediateContext(&g_ctx);

        ID3DBlob *vsb = nullptr, *ps1 = nullptr, *ps2 = nullptr;
        if (FAILED(D3DCompile(VS_SRC, strlen(VS_SRC), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, nullptr))) return false;
        if (FAILED(D3DCompile(PS_SOLID_SRC, strlen(PS_SOLID_SRC), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps1, nullptr))) return false;
        if (FAILED(D3DCompile(PS_TEX_SRC, strlen(PS_TEX_SRC), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps2, nullptr))) return false;

        g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
        g_dev->CreatePixelShader(ps1->GetBufferPointer(), ps1->GetBufferSize(), nullptr, &g_ps_solid);
        g_dev->CreatePixelShader(ps2->GetBufferPointer(), ps2->GetBufferSize(), nullptr, &g_ps_tex);

        D3D11_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        g_dev->CreateInputLayout(il, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_il);
        vsb->Release(); ps1->Release(); ps2->Release();

        g_vb_cap = 20000;
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(Vertex) * g_vb_cap;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&bd, nullptr, &g_vb);

        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bl, &g_blend);

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        g_dev->CreateRasterizerState(&rd, &g_rs);

        build_font();
        g_ready = true;
        return true;
    }

    static void begin_frame(IDXGISwapChain* swap, float& sw, float& sh) {
        g_verts.clear();
        g_batches.clear();
        sw = 0.f; sh = 0.f;
        if (!g_ready) return;

        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
            D3D11_TEXTURE2D_DESC dd;
            bb->GetDesc(&dd);
            sw = (float)dd.Width; sh = (float)dd.Height;
            g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
            bb->Release();
        }
    }

    static uint32_t reserve(bool textured, uint32_t n) {
        if (g_batches.empty() || g_batches.back().textured != textured) {
            Batch b; b.textured = textured; b.start = (uint32_t)g_verts.size(); b.count = 0;
            g_batches.push_back(b);
        }
        uint32_t start = (uint32_t)g_verts.size();
        g_batches.back().count += n;
        g_verts.resize(g_verts.size() + n);
        return start;
    }

    static void fill(Vertex& v, float x, float y, uint32_t col, float u, float vv) {
        v.x = x; v.y = y; v.z = 0.f; v.w = 1.f;
        v.r = ((col >> 24) & 0xFF) / 255.f;
        v.g = ((col >> 16) & 0xFF) / 255.f;
        v.b = ((col >> 8) & 0xFF) / 255.f;
        v.a = (col & 0xFF) / 255.f;
        v.u = u; v.v = vv;
    }

    static void add_quad(float x0, float y0, float x1, float y1, uint32_t col,
                         float u0, float v0, float u1, float v1) {
        uint32_t s = reserve(true, 6);
        fill(g_verts[s + 0], x0, y0, col, u0, v0);
        fill(g_verts[s + 1], x1, y0, col, u1, v0);
        fill(g_verts[s + 2], x0, y1, col, u0, v1);
        fill(g_verts[s + 3], x1, y0, col, u1, v0);
        fill(g_verts[s + 4], x1, y1, col, u1, v1);
        fill(g_verts[s + 5], x0, y1, col, u0, v1);
    }

    void draw_line(float x0, float y0, float x1, float y1, uint32_t col) {
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.0001f) return;
        float nx = (-dy / len) * 0.5f, ny = (dx / len) * 0.5f;
        uint32_t s = reserve(false, 6);
        fill(g_verts[s + 0], x0 + nx, y0 + ny, col, 0, 0);
        fill(g_verts[s + 1], x1 + nx, y1 + ny, col, 0, 0);
        fill(g_verts[s + 2], x0 - nx, y0 - ny, col, 0, 0);
        fill(g_verts[s + 3], x1 + nx, y1 + ny, col, 0, 0);
        fill(g_verts[s + 4], x1 - nx, y1 - ny, col, 0, 0);
        fill(g_verts[s + 5], x0 - nx, y0 - ny, col, 0, 0);
    }

    static void draw_rect(float x, float y, float w, float h, uint32_t col) {
        draw_line(x, y, x + w, y, col);
        draw_line(x + w, y, x + w, y + h, col);
        draw_line(x + w, y + h, x, y + h, col);
        draw_line(x, y + h, x, y, col);
    }

    void draw_rect_filled(float x, float y, float w, float h, uint32_t col) {
        uint32_t s = reserve(false, 6);
        fill(g_verts[s + 0], x, y, col, 0, 0);
        fill(g_verts[s + 1], x + w, y, col, 0, 0);
        fill(g_verts[s + 2], x, y + h, col, 0, 0);
        fill(g_verts[s + 3], x + w, y, col, 0, 0);
        fill(g_verts[s + 4], x + w, y + h, col, 0, 0);
        fill(g_verts[s + 5], x, y + h, col, 0, 0);
    }

    static float text_width(const char* s) {
        float w = 0.f;
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            w += (c >= 32) ? g_cw[c] : 4.f;
        }
        return w;
    }

    void draw_text(float x, float y, uint32_t col, bool centered, const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (centered) x -= text_width(buf) * 0.5f;
        float cx = x;
        for (char* p = buf; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c < 32) { cx += 4.f; continue; }
            add_quad(cx, y, cx + g_cw[c], y + g_ch, col, g_u0[c], g_v0[c], g_u1[c], g_v1[c]);
            cx += g_cw[c];
        }
    }

    static void end_frame() {
        if (!g_ready || !g_rtv || g_verts.empty()) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            return;
        }
        if (g_verts.size() > g_vb_cap) {
            if (g_vb) g_vb->Release();
            g_vb_cap = (uint32_t)g_verts.size() * 2;
            D3D11_BUFFER_DESC bd = {};
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.ByteWidth = sizeof(Vertex) * g_vb_cap;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            g_dev->CreateBuffer(&bd, nullptr, &g_vb);
        }
        D3D11_MAPPED_SUBRESOURCE mp;
        if (SUCCEEDED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
            memcpy(mp.pData, g_verts.data(), g_verts.size() * sizeof(Vertex));
            g_ctx->Unmap(g_vb, 0);
        }

        D3D11_VIEWPORT vp = {};
        vp.MinDepth = 0.f; vp.MaxDepth = 1.f;
        vp.Width = 1920.f; vp.Height = 1080.f;
        ID3D11Resource* res = nullptr;
        g_rtv->GetResource(&res);
        if (res) {
            ID3D11Texture2D* t2 = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2))) {
                D3D11_TEXTURE2D_DESC dd;
                t2->GetDesc(&dd);
                vp.Width = (float)dd.Width; vp.Height = (float)dd.Height;
                t2->Release();
            }
            res->Release();
        }

        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->IASetInputLayout(g_il);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = sizeof(Vertex), off = 0;
        g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
        g_ctx->RSSetState(g_rs);
        float bf[4] = { 0, 0, 0, 0 };
        g_ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
        g_ctx->VSSetShader(g_vs, nullptr, 0);

        for (size_t i = 0; i < g_batches.size(); i++) {
            const Batch& b = g_batches[i];
            g_ctx->PSSetShader(b.textured ? g_ps_tex : g_ps_solid, nullptr, 0);
            if (b.textured) {
                g_ctx->PSSetShaderResources(0, 1, &g_font_srv);
                g_ctx->PSSetSamplers(0, 1, &g_samp);
            }
            g_ctx->Draw(b.count, b.start);
        }

        g_rtv->Release();
        g_rtv = nullptr;
    }
}

namespace hk {
    typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
    static PresentFn o_present = nullptr;
    static void** g_vtable = nullptr;
    static bool g_hooked = false;

    static bool install();
    static void remove();

    HRESULT __stdcall hooked_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
        gfx::init(swap);
        float sw = 0.f, sh = 0.f;
        gfx::begin_frame(swap, sw, sh);
        if (sw > 0.f && sh > 0.f) {
            esp_render(sw, sh);
            menu::render();
        }
        gfx::end_frame();
        return o_present(swap, sync, flags);
    }

    static bool install() {
        if (g_hooked) return true;
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = GetForegroundWindow();
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        IDXGISwapChain* sc = nullptr;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx)))
            return false;

        g_vtable = *(void***)sc;
        o_present = (PresentFn)g_vtable[8];

        DWORD old;
        VirtualProtect(&g_vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        g_vtable[8] = (void*)&hooked_present;
        VirtualProtect(&g_vtable[8], sizeof(void*), old, &old);

        sc->Release(); dev->Release(); ctx->Release();
        g_hooked = true;
        return true;
    }

    static void remove() {
        if (!g_hooked) return;
        DWORD old;
        VirtualProtect(&g_vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        g_vtable[8] = (void*)o_present;
        VirtualProtect(&g_vtable[8], sizeof(void*), old, &old);
        g_hooked = false;
    }
}

static uintptr_t g_client = 0;

static bool ptr_sane(uintptr_t p) {
    return p > 0x10000 && p < 0x7FFFFFFFFFFF;
}

static bool pawn_ok(uintptr_t p, uintptr_t local) {
    if (!ptr_sane(p) || p == local) return false;
    uint8_t team = *(uint8_t*)(p + off::m_iTeamNum);
    if (team != 2 && team != 3) return false;
    int hp = *(int*)(p + off::m_iHealth);
    if (hp < 1 || hp > 100) return false;
    uint8_t ls = *(uint8_t*)(p + off::m_lifeState);
    if (ls != 0) return false;
    return ptr_sane(*(uintptr_t*)(p + off::m_pGameSceneNode));
}

static uintptr_t resolve_instance(uintptr_t identity, uintptr_t local) {
    static const uintptr_t cands[3] = { 0x0, 0x70, 0x78 };
    for (int i = 0; i < 3; i++) {
        uintptr_t p = *(uintptr_t*)(identity + cands[i]);
        if (pawn_ok(p, local)) return p;
    }
    return 0;
}

static bool controller_ok(uintptr_t c) {
    if (!ptr_sane(c)) return false;
    uint8_t team = *(uint8_t*)(c + off::m_iTeamNum);
    if (team != 2 && team != 3) return false;
    uint32_t hp_handle = *(uint32_t*)(c + off::m_hPawn);
    return (hp_handle & 0x7FFF) < (uint32_t)off::MAX_ENTITIES;
}

static void draw_skeleton(uintptr_t pawn, const ViewMatrix& vm, float sw, float sh, uint32_t col) {
    uintptr_t node = *(uintptr_t*)(pawn + off::m_pGameSceneNode);
    if (!ptr_sane(node)) return;
    uintptr_t ms = *(uintptr_t*)(node + off::m_modelState);
    if (!ptr_sane(ms)) return;
    uintptr_t ba = *(uintptr_t*)(ms + off::m_pBoneArray);
    if (!ptr_sane(ba)) return;

    static const int conn[14][2] = {
        {0, 5}, {5, 7}, {7, 8}, {8, 9}, {9, 10}, {7, 11}, {11, 12}, {12, 13},
        {5, 14}, {14, 15}, {15, 16}, {5, 17}, {17, 18}, {18, 19}
    };
    const float* bones = (const float*)ba;
    for (int i = 0; i < 14; i++) {
        int a = conn[i][0], b = conn[i][1];
        Vec3 pa = { bones[a * 12 + 3], bones[a * 12 + 7], bones[a * 12 + 11] };
        Vec3 pb = { bones[b * 12 + 3], bones[b * 12 + 7], bones[b * 12 + 11] };
        if (!coord_sane(pa.x) || !coord_sane(pa.y) || !coord_sane(pa.z)) continue;
        if (!coord_sane(pb.x) || !coord_sane(pb.y) || !coord_sane(pb.z)) continue;
        Vec2 sa, sb;
        if (world_to_screen(pa, sa, vm, sw, sh) && world_to_screen(pb, sb, vm, sw, sh))
            gfx::draw_line(sa.x, sa.y, sb.x, sb.y, col);
    }
}

void esp_render(float sw, float sh) {
    if (!g_client) return;
    uintptr_t ent_sys = *(uintptr_t*)(g_client + off::dwEntityList);
    uintptr_t local   = *(uintptr_t*)(g_client + off::dwLocalPlayerPawn);
    if (!ptr_sane(ent_sys) || !ptr_sane(local)) return;

    ViewMatrix vm = *(ViewMatrix*)(g_client + off::dwViewMatrix);
    uintptr_t* ident = (uintptr_t*)(ent_sys + off::ENTITY_IDENTITIES);

    uint8_t local_team = *(uint8_t*)(local + off::m_iTeamNum);
    if (local_team != 2 && local_team != 3) return;

    int null_run = 0;
    for (int i = 0; i < off::MAX_ENTITIES; i++) {
        uintptr_t identity = ident[i];
        if (!ptr_sane(identity)) {
            if (++null_run > 512) break;
            continue;
        }
        null_run = 0;

        uintptr_t pawn = resolve_instance(identity, local);
        if (!pawn) continue;

        int hp = *(int*)(pawn + off::m_iHealth);
        uint8_t team = *(uint8_t*)(pawn + off::m_iTeamNum);
        if (g_cfg.teamcheck && team == local_team) continue;

        uintptr_t node = *(uintptr_t*)(pawn + off::m_pGameSceneNode);
        if (!ptr_sane(node)) continue;
        Vec3 origin = *(Vec3*)(node + off::m_vecOrigin);
        if (!coord_sane(origin.x) || !coord_sane(origin.y) || !coord_sane(origin.z)) continue;
        if (origin.x == 0.f && origin.y == 0.f && origin.z == 0.f) continue;

        Vec3 head_w = { origin.x, origin.y, origin.z + 72.f };
        Vec2 head_s, feet_s;
        if (!world_to_screen(head_w, head_s, vm, sw, sh)) continue;
        if (!world_to_screen(origin, feet_s, vm, sw, sh)) continue;

        float h = feet_s.y - head_s.y;
        if (h < 4.f || h > sh * 2.f) continue;
        float w = h * 0.4f;
        float bx = head_s.x - w * 0.5f;

        uint32_t col = (team == local_team) ? rgba(60, 200, 60, 255) : rgba(255, 60, 60, 255);

        if (g_cfg.box) {
            gfx::draw_rect(bx - 1, head_s.y - 1, w + 2, h + 2, rgba(0, 0, 0, 200));
            gfx::draw_rect(bx, head_s.y, w, h, col);
        }
        if (g_cfg.health) {
            float pct = (hp > 100) ? 1.f : hp / 100.f;
            gfx::draw_rect_filled(bx - 6.f, head_s.y, 4.f, h, rgba(0, 0, 0, 180));
            gfx::draw_rect_filled(bx - 6.f, head_s.y + h - h * pct, 4.f, h * pct,
                                  rgba((uint8_t)(255 - 255 * pct), (uint8_t)(255 * pct), 0, 255));
        }
        if (g_cfg.name) {
            char name[32];
            memset(name, 0, sizeof(name));
            uint32_t hctl = *(uint32_t*)(pawn + off::m_hOriginalController);
            uint32_t cidx = hctl & 0x7FFF;
            if (cidx < (uint32_t)off::MAX_ENTITIES) {
                uintptr_t cident = ident[cidx];
                if (ptr_sane(cident)) {
                    static const uintptr_t cc[3] = { 0x0, 0x70, 0x78 };
                    for (int k = 0; k < 3 && name[0] == 0; k++) {
                        uintptr_t ctl = *(uintptr_t*)(cident + cc[k]);
                        if (!controller_ok(ctl)) continue;
                        const char* nm = (const char*)(ctl + off::m_iszPlayerName);
                        if (nm[0] >= 0x20 && nm[0] <= 0x7E) {
                            int n = 0;
                            while (n < 31 && nm[n] >= 0x20 && nm[n] <= 0x7E) { name[n] = nm[n]; n++; }
                        }
                    }
                }
            }
            if (name[0])
                gfx::draw_text(head_s.x, head_s.y - 18.f, rgba(255, 255, 255, 255), true, "%s [%d]", name, hp);
            else
                gfx::draw_text(head_s.x, head_s.y - 18.f, rgba(255, 255, 255, 255), true, "HP: %d", hp);
        }
        if (g_cfg.skeleton) {
            draw_skeleton(pawn, vm, sw, sh, rgba(255, 255, 255, 220));
        }
    }
}

static DWORD WINAPI init_thread(LPVOID) {
    g_cfg.box = true;
    g_cfg.skeleton = true;
    g_cfg.health = true;
    g_cfg.name = true;
    g_cfg.teamcheck = true;

    while (!g_client) {
        g_client = (uintptr_t)GetModuleHandleA("client.dll");
        if (!g_client) Sleep(200);
    }
    Sleep(3000);

    menu::install();
    hk::install();

    while (hk::g_hooked) {
        if (GetAsyncKeyState(VK_END) & 1) break;
        if (GetAsyncKeyState(VK_RSHIFT) & 1) menu::toggle();
        Sleep(10);
    }

    hk::remove();
    Sleep(300);
    ExitThread(0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        hk::remove();
    }
    return TRUE;
}
