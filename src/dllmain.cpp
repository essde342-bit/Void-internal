// Void Internal — D3D11 Present hook + ESP + Aimbot
// Совместимо со старыми Windows SDK: IDXGISwapChain3 не используется.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdio.h>
#include <stdarg.h>
#include <vector>

#include "config.h"
#include "gfx.h"
#include "menu.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

Config g_cfg;

static bool      g_unloading = false;
static uintptr_t g_client    = 0;
static HMODULE   g_hmodule   = nullptr;

static void esp_render(float sw, float sh);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void vlog(const char* fmt, ...) {
    char buf[1024];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
static bool is_readable(const void* p, size_t size) {
    if (!p || size == 0) return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    uintptr_t start      = reinterpret_cast<uintptr_t>(p);
    uintptr_t end        = start + size;
    uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

    return end <= region_end;
}

static bool is_writable(const void* p, size_t size) {
    if (!p || size == 0) return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    uintptr_t start      = reinterpret_cast<uintptr_t>(p);
    uintptr_t end        = start + size;
    uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

    if (end > region_end) return false;

    return (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
}

template <typename T>
static bool read_val(uintptr_t addr, T& out) {
    if (!is_readable(reinterpret_cast<const void*>(addr), sizeof(T))) return false;
    memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
    return true;
}

// ---------------------------------------------------------------------------
// Offsets (CS2 / Source 2) — при обновлении игры сверять с дампом (a2x/cs2-dumper)
// ---------------------------------------------------------------------------
namespace off {
    inline constexpr uintptr_t dwEntityList          = 0x19C2E58;
    inline constexpr uintptr_t dwLocalPlayerPawn     = 0x182E768;
    inline constexpr uintptr_t dwViewMatrix          = 0x1A2A560;

    inline constexpr uintptr_t m_iTeamNum            = 0x3E3;
    inline constexpr uintptr_t m_iHealth             = 0x344;
    inline constexpr uintptr_t m_lifeState           = 0x348;
    inline constexpr uintptr_t m_pGameSceneNode      = 0x328;
    inline constexpr uintptr_t m_hPawn               = 0x5EC;
    inline constexpr uintptr_t m_hOriginalController = 0x7EC;

    inline constexpr uintptr_t m_modelState          = 0x160;
    inline constexpr uintptr_t m_vecAbsOrigin        = 0xC8;

    inline constexpr uintptr_t m_iszPlayerName       = 0x7F0;

    inline constexpr uintptr_t ENTITY_IDENTITIES       = 0x10;
    inline constexpr uintptr_t ENTITY_IDENTITY_STRIDE = 0x78;
    inline constexpr int       MAX_ENTITIES            = 512;   // entities per chunk
    inline constexpr int       MAX_ENTITY_INDEX        = 32768; // 64 chunks * 512
    inline constexpr int       MAX_CLIENTS             = 64;
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct ViewMatrix { float m[4][4]; };

static bool ptr_sane(uintptr_t p) {
    return p > 0x10000 && p < 0x7FFFFFFFFFFF;
}

static bool world_pos_plausible(const Vec3& v) {
    return v.x > -16384.f && v.x < 16384.f &&
           v.y > -16384.f && v.y < 16384.f &&
           v.z > -1024.f  && v.z < 16384.f;
}

static bool world_to_screen(const Vec3& w, Vec2& s, const ViewMatrix& vm, float sw, float sh) {
    if (!std::isfinite(sw) || !std::isfinite(sh) || sw <= 0.f || sh <= 0.f)
        return false;

    const float d = vm.m[3][0] * w.x + vm.m[3][1] * w.y +
                    vm.m[3][2] * w.z + vm.m[3][3];
    if (!std::isfinite(d) || d <= 0.001f)
        return false;

    float x = vm.m[0][0] * w.x + vm.m[0][1] * w.y +
              vm.m[0][2] * w.z + vm.m[0][3];
    float y = vm.m[1][0] * w.x + vm.m[1][1] * w.y +
              vm.m[1][2] * w.z + vm.m[1][3];

    const float inv = 1.f / d;
    x *= inv;
    y *= inv;
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;

    s.x = (sw * 0.5f) + (x * sw * 0.5f);
    s.y = (sh * 0.5f) - (y * sh * 0.5f);

    // Reject wildly invalid coordinates; they can create oversized primitives.
    return std::isfinite(s.x) && std::isfinite(s.y) &&
           s.x >= -sw && s.x <= sw * 2.f &&
           s.y >= -sh && s.y <= sh * 2.f;
}

// ---------------------------------------------------------------------------
// Entity helpers
// ---------------------------------------------------------------------------
static bool pawn_ok(uintptr_t p, uintptr_t local) {
    if (!ptr_sane(p) || p == local) return false;

    uint8_t   team = 0;
    int       hp   = 0;
    uint8_t   life = 1;
    uintptr_t node = 0;

    if (!read_val(p + off::m_iTeamNum, team)) return false;
    if (team != 2 && team != 3) return false;

    if (!read_val(p + off::m_iHealth, hp)) return false;
    if (hp < 1 || hp > 100) return false;

    if (!read_val(p + off::m_lifeState, life)) return false;
    if (life != 0) return false;

    if (!read_val(p + off::m_pGameSceneNode, node)) return false;
    return ptr_sane(node);
}

static uintptr_t resolve_instance(uintptr_t identity, uintptr_t local) {
    static const uintptr_t cands[4] = { 0x0, 0x70, 0x78, 0x80 };

    for (int i = 0; i < 4; i++) {
        uintptr_t p = 0;
        if (!read_val(identity + cands[i], p)) continue;
        if (pawn_ok(p, local)) return p;
    }
    return 0;
}

// Source 2 EntityList is a table of chunk pointers. Each chunk contains
// 512 CEntityIdentity records, each 0x78 bytes in CS2.
static bool entity_identity_at(uintptr_t entity_system, uint32_t index, uintptr_t& out_identity) {
    out_identity = 0;
    if (!ptr_sane(entity_system) || index >= (uint32_t)off::MAX_ENTITY_INDEX)
        return false;

    const uintptr_t chunk_ptr_addr =
        entity_system + off::ENTITY_IDENTITIES +
        (uintptr_t)(index / off::MAX_ENTITIES) * sizeof(uintptr_t);

    uintptr_t chunk = 0;
    if (!read_val(chunk_ptr_addr, chunk) || !ptr_sane(chunk))
        return false;

    const uintptr_t slot =
        chunk + (uintptr_t)(index % off::MAX_ENTITIES) * off::ENTITY_IDENTITY_STRIDE;

    if (!is_readable(reinterpret_cast<const void*>(slot), sizeof(uintptr_t)))
        return false;

    out_identity = slot;
    return true;
}

static uintptr_t entity_instance_at(uintptr_t entity_system, uint32_t index) {
    uintptr_t identity = 0;
    if (!entity_identity_at(entity_system, index, identity))
        return 0;

    uintptr_t instance = 0;
    if (!read_val(identity, instance) || !ptr_sane(instance))
        return 0;

    return instance;
}

static uintptr_t resolve_pawn_at(uintptr_t entity_system, uint32_t index, uintptr_t local) {
    const uintptr_t pawn = entity_instance_at(entity_system, index);
    return pawn_ok(pawn, local) ? pawn : 0;
}

static bool controller_ok(uintptr_t c, uint32_t& pawn_handle) {
    pawn_handle = 0xFFFFFFFFu;
    if (!ptr_sane(c)) return false;

    uint8_t team = 0;
    if (!read_val(c + off::m_iTeamNum, team) || (team != 2 && team != 3))
        return false;

    if (!read_val(c + off::m_hPawn, pawn_handle) || pawn_handle == 0xFFFFFFFFu)
        return false;

    return (pawn_handle & 0x7FFFu) < (uint32_t)off::MAX_ENTITY_INDEX;
}

static bool read_origin(uintptr_t node, Vec3& out) {
    if (!ptr_sane(node)) return false;
    if (!read_val(node + off::m_vecAbsOrigin, out)) return false;
    return world_pos_plausible(out);
}

static uintptr_t pick_bone_array(uintptr_t ms) {
    static int bone_off = -1;
    static const uintptr_t cands[8] = { 0x80, 0x88, 0x90, 0x78, 0xA0, 0xB0, 0xC0, 0x70 };

    if (bone_off >= 0) {
        uintptr_t ba = 0;
        if (read_val(ms + (uintptr_t)bone_off, ba) && ptr_sane(ba)) return ba;
        bone_off = -1; // Re-evaluate candidates after entity/model changes.
    }

    for (int i = 0; i < 8; i++) {
        uintptr_t ba = 0;
        if (!read_val(ms + cands[i], ba)) continue;
        if (!ptr_sane(ba)) continue;

        float tx = 0.f, ty = 0.f, tz = 0.f;
        if (!read_val(ba + 3  * sizeof(float), tx)) continue;
        if (!read_val(ba + 7  * sizeof(float), ty)) continue;
        if (!read_val(ba + 11 * sizeof(float), tz)) continue;

        Vec3 t = { tx, ty, tz };
        if (world_pos_plausible(t)) {
            bone_off = (int)cands[i];
            return ba;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GFX
// ---------------------------------------------------------------------------
struct Vertex {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
};

struct Batch {
    bool     textured;
    uint32_t start;
    uint32_t count;
};

static const char* VS_SRC = R"(
struct VS_IN { float4 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
PS_IN main(VS_IN i) {
    PS_IN o;
    o.pos = i.pos;
    o.col = i.col;
    o.uv  = i.uv;
    return o;
}
)";

static const char* PS_SOLID_SRC = R"(
struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
float4 main(PS_IN i) : SV_TARGET { return i.col; }
)";

static const char* PS_TEX_SRC = R"(
Texture2D    tex  : register(t0);
SamplerState samp : register(s0);
struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
float4 main(PS_IN i) : SV_TARGET {
    float a = tex.Sample(samp, i.uv).r;
    return float4(i.col.rgb, i.col.a * a);
}
)";

namespace gfx {

static ID3D11Device*             g_dev      = nullptr;
static ID3D11DeviceContext*      g_ctx      = nullptr;
static ID3D11VertexShader*       g_vs       = nullptr;
static ID3D11PixelShader*        g_ps_solid = nullptr;
static ID3D11PixelShader*        g_ps_tex   = nullptr;
static ID3D11InputLayout*        g_il       = nullptr;
static ID3D11Buffer*             g_vb       = nullptr;
static uint32_t                  g_vb_cap   = 0;
static ID3D11BlendState*         g_blend    = nullptr;
static ID3D11RasterizerState*    g_rs       = nullptr;
static ID3D11RenderTargetView*   g_rtv      = nullptr;
static ID3D11ShaderResourceView* g_font_srv = nullptr;
static ID3D11SamplerState*       g_samp     = nullptr;
static bool                      g_ready    = false;

static std::vector<Vertex> g_verts;
static std::vector<Batch>  g_batches;
static float g_screen_w = 1920.f;
static float g_screen_h = 1080.f;

static float g_cw[256];
static float g_ch;
static float g_u0[256], g_v0[256], g_u1[256], g_v1[256];

static bool build_font(ID3D11Device* dev, ID3D11ShaderResourceView** out_srv, ID3D11SamplerState** out_samp) {
    *out_srv  = nullptr;
    *out_samp = nullptr;

    const int atlas_w = 256;
    const int atlas_h = 256;
    const int cell_w  = 8;
    const int cell_h  = 16;
    const int cols    = atlas_w / cell_w;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = atlas_w;
    bmi.bmiHeader.biHeight      = -atlas_h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   bits = nullptr;
    HDC     hdc  = CreateCompatibleDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        DeleteDC(hdc);
        return false;
    }

    HGDIOBJ old = SelectObject(hdc, hbmp);
    memset(bits, 0, atlas_w * atlas_h * 4);

    HFONT font = CreateFontA(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas"
    );

    if (!font) {
        SelectObject(hdc, old);
        DeleteObject(hbmp);
        DeleteDC(hdc);
        return false;
    }

    HGDIOBJ old_font = SelectObject(hdc, font);
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);

    for (int c = 32; c < 256; c++) {
        int cx = (c - 32) % cols;
        int cy = (c - 32) / cols;

        char ch = (char)c;
        SIZE sz = {};
        GetTextExtentPoint32A(hdc, &ch, 1, &sz);

        float w = (float)sz.cx;
        if (w > (float)cell_w) w = (float)cell_w;   // FIX: не вылезать за ячейку
        if (w < 1.f) w = 1.f;
        g_cw[c] = w;

        TextOutA(hdc, cx * cell_w, cy * cell_h, &ch, 1);

        g_u0[c] = (float)(cx * cell_w)       / atlas_w;
        g_v0[c] = (float)(cy * cell_h)       / atlas_h;
        g_u1[c] = (float)(cx * cell_w) / atlas_w + w / atlas_w; // FIX: без захвата соседей
        g_v1[c] = (float)(cy * cell_h + cell_h) / atlas_h;
    }
    g_ch = (float)cell_h;

    SelectObject(hdc, old_font);
    DeleteObject(font);
    SelectObject(hdc, old);

    uint8_t*  alpha = new uint8_t[atlas_w * atlas_h];
    uint32_t* px    = (uint32_t*)bits;
    for (int i = 0; i < atlas_w * atlas_h; i++)
        alpha[i] = (uint8_t)((px[i] >> 16) & 0xFF);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = atlas_w;
    td.Height           = atlas_h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd = {};
    srd.pSysMem     = alpha;
    srd.SysMemPitch = atlas_w;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, &srd, &tex);
    delete[] alpha;

    bool result = false;

    if (SUCCEEDED(hr) && tex) {
        hr = dev->CreateShaderResourceView(tex, nullptr, out_srv);
        tex->Release();

        if (SUCCEEDED(hr) && *out_srv) {
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD   = D3D11_FLOAT32_MAX;

            hr = dev->CreateSamplerState(&sd, out_samp);
            if (SUCCEEDED(hr) && *out_samp) {
                result = true;
            } else if (*out_srv) {
                (*out_srv)->Release();
                *out_srv = nullptr;
            }
        }
    }

    DeleteObject(hbmp);
    DeleteDC(hdc);
    return result;
}

bool init(IDXGISwapChain* swap) {
    if (g_ready) return true;

    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev)
        return false;

    dev->GetImmediateContext(&ctx);
    if (!ctx) { dev->Release(); return false; }

    bool      ok   = false;
    ID3DBlob* blob = nullptr;

    ID3D11VertexShader*       vs   = nullptr;
    ID3D11PixelShader*        ps1  = nullptr;
    ID3D11PixelShader*        ps2  = nullptr;
    ID3D11InputLayout*        il   = nullptr;
    ID3D11Buffer*             vb   = nullptr;
    ID3D11BlendState*         bl   = nullptr;
    ID3D11RasterizerState*    rs   = nullptr;
    ID3D11ShaderResourceView* srv  = nullptr;
    ID3D11SamplerState*       samp = nullptr;

    do {
        if (FAILED(D3DCompile(VS_SRC, strlen(VS_SRC), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, nullptr)) || !blob)
            break;

        if (FAILED(dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs)) || !vs) {
            blob->Release(); blob = nullptr; break;
        }

        D3D11_INPUT_ELEMENT_DESC ied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };

        if (FAILED(dev->CreateInputLayout(ied, 3, blob->GetBufferPointer(), blob->GetBufferSize(), &il)) || !il) {
            blob->Release(); blob = nullptr; break;
        }
        blob->Release(); blob = nullptr;

        if (FAILED(D3DCompile(PS_SOLID_SRC, strlen(PS_SOLID_SRC), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, nullptr)) || !blob)
            break;
        if (FAILED(dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps1)) || !ps1) {
            blob->Release(); blob = nullptr; break;
        }
        blob->Release(); blob = nullptr;

        if (FAILED(D3DCompile(PS_TEX_SRC, strlen(PS_TEX_SRC), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, nullptr)) || !blob)
            break;
        if (FAILED(dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps2)) || !ps2) {
            blob->Release(); blob = nullptr; break;
        }
        blob->Release(); blob = nullptr;

        D3D11_BUFFER_DESC bd = {};
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth      = sizeof(Vertex) * 20000;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &vb)) || !vb) break;

        D3D11_BLEND_DESC bld = {};
        bld.RenderTarget[0].BlendEnable           = TRUE;
        bld.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        bld.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&bld, &bl)) || !bl) break;

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        if (FAILED(dev->CreateRasterizerState(&rd, &rs)) || !rs) break;

        if (!build_font(dev, &srv, &samp)) break;

        ok = true;
    } while (false);

    if (blob) blob->Release();

    if (!ok) {
        if (srv)  srv->Release();
        if (samp) samp->Release();
        if (rs)   rs->Release();
        if (bl)   bl->Release();
        if (vb)   vb->Release();
        if (il)   il->Release();
        if (ps2)  ps2->Release();
        if (ps1)  ps1->Release();
        if (vs)   vs->Release();
        if (ctx)  ctx->Release();
        if (dev)  dev->Release();
        vlog("gfx: init FAIL");
        return false;
    }

    g_dev = dev; g_ctx = ctx; g_vs = vs;
    g_ps_solid = ps1; g_ps_tex = ps2;
    g_il = il; g_vb = vb; g_vb_cap = 20000;
    g_blend = bl; g_rs = rs;
    g_font_srv = srv; g_samp = samp;
    g_ready = true;

    vlog("gfx: init OK");
    return true;
}

bool ready() { return g_ready; }

void shutdown() {
    if (g_rtv)      { g_rtv->Release();      g_rtv = nullptr; }
    if (g_vb)       { g_vb->Release();       g_vb = nullptr; }
    if (g_blend)    { g_blend->Release();    g_blend = nullptr; }
    if (g_rs)       { g_rs->Release();       g_rs = nullptr; }
    if (g_font_srv) { g_font_srv->Release(); g_font_srv = nullptr; }
    if (g_samp)     { g_samp->Release();     g_samp = nullptr; }
    if (g_il)       { g_il->Release();       g_il = nullptr; }
    if (g_vs)       { g_vs->Release();       g_vs = nullptr; }
    if (g_ps_solid) { g_ps_solid->Release(); g_ps_solid = nullptr; }
    if (g_ps_tex)   { g_ps_tex->Release();   g_ps_tex = nullptr; }
    if (g_ctx)      { g_ctx->Release();      g_ctx = nullptr; }
    if (g_dev)      { g_dev->Release();      g_dev = nullptr; }

    g_verts.clear();
    g_batches.clear();
    g_vb_cap = 0;
    g_ready = false;

    vlog("gfx: shutdown");
}

static void begin_frame(IDXGISwapChain* swap, float& sw, float& sh) {
    g_verts.clear();
    g_batches.clear();

    sw = 0.f;
    sh = 0.f;

    if (!g_ready) return;

    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }

    // Без IDXGISwapChain3 — совместимо со всеми SDK.
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        D3D11_TEXTURE2D_DESC dd;
        bb->GetDesc(&dd);

        sw = (float)dd.Width;
        sh = (float)dd.Height;
        g_screen_w = sw;
        g_screen_h = sh;

        if (FAILED(g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv)))
            g_rtv = nullptr;

        bb->Release();
    }
}

static uint32_t reserve(bool textured, uint32_t n) {
    if (g_batches.empty() || g_batches.back().textured != textured) {
        Batch b;
        b.textured = textured;
        b.start    = (uint32_t)g_verts.size();
        b.count    = 0;
        g_batches.push_back(b);
    }

    uint32_t start = (uint32_t)g_verts.size();
    g_batches.back().count += n;
    g_verts.resize(g_verts.size() + n);
    return start;
}

static void fill(Vertex& v, float x, float y, uint32_t col, float u, float vv) {
    // UI primitives are authored in pixels; D3D clip-space is [-1, +1].
    const float sw = (g_screen_w > 1.f) ? g_screen_w : 1920.f;
    const float sh = (g_screen_h > 1.f) ? g_screen_h : 1080.f;
    v.x = (x / sw) * 2.f - 1.f;
    v.y = 1.f - (y / sh) * 2.f;
    v.z = 0.f; v.w = 1.f;
    v.r = ((col >> 24) & 0xFF) / 255.f;
    v.g = ((col >> 16) & 0xFF) / 255.f;
    v.b = ((col >> 8)  & 0xFF) / 255.f;
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

    float nx = (-dy / len) * 0.5f;
    float ny = ( dx / len) * 0.5f;

    uint32_t s = reserve(false, 6);
    fill(g_verts[s + 0], x0 + nx, y0 + ny, col, 0, 0);
    fill(g_verts[s + 1], x1 + nx, y1 + ny, col, 0, 0);
    fill(g_verts[s + 2], x0 - nx, y0 - ny, col, 0, 0);
    fill(g_verts[s + 3], x1 + nx, y1 + ny, col, 0, 0);
    fill(g_verts[s + 4], x1 - nx, y1 - ny, col, 0, 0);
    fill(g_verts[s + 5], x0 - nx, y0 - ny, col, 0, 0);
}

void draw_rect(float x, float y, float w, float h, uint32_t col) {
    draw_line(x, y, x + w, y, col);
    draw_line(x + w, y, x + w, y + h, col);
    draw_line(x + w, y + h, x, y + h, col);
    draw_line(x, y + h, x, y, col);
}

void draw_rect_filled(float x, float y, float w, float h, uint32_t col) {
    uint32_t s = reserve(false, 6);
    fill(g_verts[s + 0], x,     y,     col, 0, 0);
    fill(g_verts[s + 1], x + w, y,     col, 0, 0);
    fill(g_verts[s + 2], x,     y + h, col, 0, 0);
    fill(g_verts[s + 3], x + w, y,     col, 0, 0);
    fill(g_verts[s + 4], x + w, y + h, col, 0, 0);
    fill(g_verts[s + 5], x,     y + h, col, 0, 0);
}

float text_width(const char* s) {
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
        if (g_vb) { g_vb->Release(); g_vb = nullptr; }

        uint32_t newcap = (uint32_t)g_verts.size() * 2;

        D3D11_BUFFER_DESC bd = {};
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth      = sizeof(Vertex) * newcap;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        // FIX: проверяем результат CreateBuffer — раньше при фейле был Map(nullptr)
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_vb)) || !g_vb) {
            g_vb = nullptr;
            g_vb_cap = 0;
            g_rtv->Release();
            g_rtv = nullptr;
            return;
        }
        g_vb_cap = newcap;
    }

    if (!g_vb) {
        g_rtv->Release();
        g_rtv = nullptr;
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
        g_rtv->Release();
        g_rtv = nullptr;
        return;
    }
    memcpy(mp.pData, g_verts.data(), g_verts.size() * sizeof(Vertex));
    g_ctx->Unmap(g_vb, 0);

    // Save state
    ID3D11RenderTargetView* oRTV = nullptr;
    ID3D11DepthStencilView* oDSV = nullptr;
    g_ctx->OMGetRenderTargets(1, &oRTV, &oDSV);

    UINT nvp = 1;
    D3D11_VIEWPORT oVP[1] = {};
    g_ctx->RSGetViewports(&nvp, oVP);

    ID3D11BlendState* oBlend = nullptr;
    FLOAT oBF[4] = {};
    UINT  oMask  = 0;
    g_ctx->OMGetBlendState(&oBlend, oBF, &oMask);

    ID3D11RasterizerState* oRS = nullptr;
    g_ctx->RSGetState(&oRS);

    ID3D11InputLayout* oIL = nullptr;
    g_ctx->IAGetInputLayout(&oIL);

    D3D11_PRIMITIVE_TOPOLOGY oTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    g_ctx->IAGetPrimitiveTopology(&oTopo);

    ID3D11Buffer* oVB = nullptr;
    UINT oStride = 0, oOff = 0;
    g_ctx->IAGetVertexBuffers(0, 1, &oVB, &oStride, &oOff);

    ID3D11VertexShader* oVS = nullptr;
    g_ctx->VSGetShader(&oVS, nullptr, nullptr);

    ID3D11PixelShader* oPS = nullptr;
    g_ctx->PSGetShader(&oPS, nullptr, nullptr);

    ID3D11ShaderResourceView* oSRV = nullptr;
    g_ctx->PSGetShaderResources(0, 1, &oSRV);

    ID3D11SamplerState* oSamp = nullptr;
    g_ctx->PSGetSamplers(0, 1, &oSamp);

    D3D11_VIEWPORT vp = {};
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;
    vp.Width  = 1920.f;
    vp.Height = 1080.f;

    ID3D11Resource* res = nullptr;
    g_rtv->GetResource(&res);
    if (res) {
        ID3D11Texture2D* t2 = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2))) {
            D3D11_TEXTURE2D_DESC dd;
            t2->GetDesc(&dd);
            vp.Width  = (float)dd.Width;
            vp.Height = (float)dd.Height;
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

    // Restore state
    g_ctx->OMSetRenderTargets(1, &oRTV, oDSV);
    if (nvp) g_ctx->RSSetViewports(nvp, oVP);
    g_ctx->OMSetBlendState(oBlend, oBF, oMask);
    g_ctx->RSSetState(oRS);
    g_ctx->IASetInputLayout(oIL);
    if (oTopo != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) // FIX: не выставлять UNDEFINED
        g_ctx->IASetPrimitiveTopology(oTopo);
    g_ctx->IASetVertexBuffers(0, 1, &oVB, &oStride, &oOff);
    g_ctx->VSSetShader(oVS, nullptr, 0);
    g_ctx->PSSetShader(oPS, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &oSRV);
    g_ctx->PSSetSamplers(0, 1, &oSamp);

    if (oRTV)   oRTV->Release();
    if (oDSV)   oDSV->Release();
    if (oBlend) oBlend->Release();
    if (oRS)    oRS->Release();
    if (oIL)    oIL->Release();
    if (oVB)    oVB->Release();
    if (oVS)    oVS->Release();
    if (oPS)    oPS->Release();
    if (oSRV)   oSRV->Release();
    if (oSamp)  oSamp->Release();

    g_rtv->Release();
    g_rtv = nullptr;
}

} // namespace gfx

// ---------------------------------------------------------------------------
// Aimbot
// ---------------------------------------------------------------------------
namespace aim {

constexpr float PI = 3.14159265358979323846f;

struct Vec2Ang { float pitch, yaw; };

static int g_view_offset_off = -1;
static int g_view_angles_off = -1;
static int g_va_stable       = 0;   // FIX: подтверждение кандидата перед записью

static Vec3 get_eye_pos(uintptr_t pawn) {
    static const int cands[4] = { 0xC8, 0xD0, 0xC0, 0xB8 };

    uintptr_t node = 0;
    if (!read_val(pawn + off::m_pGameSceneNode, node) || !ptr_sane(node))
        return { 0, 0, 0 };

    Vec3 origin;
    if (!read_origin(node, origin))
        return { 0, 0, 0 };

    if (g_view_offset_off >= 0) {
        Vec3 vo;
        if (read_val(pawn + (uintptr_t)g_view_offset_off, vo))
            return { origin.x + vo.x, origin.y + vo.y, origin.z + vo.z };
    }

    for (int i = 0; i < 4; i++) {
        Vec3 vo;
        if (!read_val(pawn + (uintptr_t)cands[i], vo)) continue;

        if (vo.x > -1.f && vo.x < 1.f &&
            vo.y > -1.f && vo.y < 1.f &&
            vo.z > 20.f && vo.z < 80.f) {
            g_view_offset_off = cands[i];
            vlog("aim: view_offset picked = 0x%X", cands[i]);
            return { origin.x + vo.x, origin.y + vo.y, origin.z + vo.z };
        }
    }

    return { origin.x, origin.y, origin.z + 64.f };
}

static Vec2Ang calc_angles(const Vec3& from, const Vec3& to) {
    Vec3 d = { to.x - from.x, to.y - from.y, to.z - from.z };
    float hyp = sqrtf(d.x * d.x + d.y * d.y);

    Vec2Ang a;
    a.pitch = -atan2f(d.z, hyp) * (180.f / PI);
    a.yaw   =  atan2f(d.y, d.x) * (180.f / PI);
    return a;
}

static void normalize(Vec2Ang& a) {
    if (a.pitch >  89.f) a.pitch =  89.f;
    if (a.pitch < -89.f) a.pitch = -89.f;
    while (a.yaw >  180.f)  a.yaw -= 360.f;
    while (a.yaw <= -180.f) a.yaw += 360.f;
}

static float ang_diff(const Vec2Ang& a, const Vec2Ang& b) {
    float dp = a.pitch - b.pitch;
    float dy = a.yaw   - b.yaw;
    while (dy >  180.f)  dy -= 360.f;
    while (dy <= -180.f) dy += 360.f;
    return sqrtf(dp * dp + dy * dy);
}

// FIX: сглаживание теперь зависит от времени кадра, а не от FPS
static Vec2Ang lerp_angles(const Vec2Ang& cur, const Vec2Ang& tgt, float step) {
    if (step >= 1.f) return tgt;
    Vec2Ang d = { tgt.pitch - cur.pitch, tgt.yaw - cur.yaw };
    normalize(d);
    Vec2Ang out = { cur.pitch + d.pitch * step, cur.yaw + d.yaw * step };
    normalize(out);
    return out;
}

static uintptr_t get_view_angles_addr() {
    if (!g_client) return 0;

    if (g_view_angles_off >= 0)
        return g_client + (uintptr_t)g_view_angles_off;

    // Кандидаты. Если игра обновилась — брать актуальный dwViewAngles из дампа/сигнатуры.
    static const int cands[] = {
        0x1A2D500, 0x1A2D4E0, 0x1A2D4C0, 0x1A2D4A0, 0x1A2D480,
        0x1A2A600, 0x1A2A620, 0x1A2A640, 0x1A2A660, 0x1A2A680
    };

    for (int offv : cands) {
        uintptr_t a = g_client + (uintptr_t)offv;
        if ((a & 3) != 0) continue;
        if (!is_readable((void*)a, sizeof(Vec2Ang))) continue;

        Vec2Ang v;
        if (!read_val(a, v)) continue;

        if (v.pitch >= -89.f && v.pitch <= 89.f &&
            v.yaw   >= -180.f && v.yaw <= 180.f) {
            g_view_angles_off = offv;
            g_va_stable = 0;
            vlog("aim: view_angles candidate = 0x%X (awaiting confirm)", offv);
            return a;
        }
    }
    return 0;
}

static Vec2Ang read_view_angles() {
    uintptr_t a = get_view_angles_addr();
    if (!a) { g_va_stable = 0; return { 0, 0 }; }

    Vec2Ang out;
    if (!read_val(a, out)) { g_va_stable = 0; return { 0, 0 }; }

    if (out.pitch >= -89.f && out.pitch <= 89.f &&
        out.yaw   >= -180.f && out.yaw <= 180.f) {
        if (g_va_stable < 1000) g_va_stable++;
    } else {
        // Кандидат оказался мусором — сбрасываем и ищем заново
        g_va_stable = 0;
        g_view_angles_off = -1;
        return { 0, 0 };
    }

    return out;
}

static void write_view_angles(Vec2Ang a) {
    // FIX: пишем только после 30 стабильных кадров — защита от записи в случайную память
    if (g_va_stable < 30) return;

    uintptr_t addr = get_view_angles_addr();
    if (!addr) return;
    if (!is_writable((void*)addr, sizeof(Vec2Ang))) return;

    normalize(a);
    memcpy(reinterpret_cast<void*>(addr), &a, sizeof(Vec2Ang));
}

static Vec3 get_bone_pos(uintptr_t pawn, int bone_id) {
    if (bone_id < 0 || bone_id >= 64) return { 0, 0, 0 };

    uintptr_t node = 0;
    if (!read_val(pawn + off::m_pGameSceneNode, node) || !ptr_sane(node))
        return { 0, 0, 0 };

    uintptr_t ms = 0;
    if (!read_val(node + off::m_modelState, ms) || !ptr_sane(ms))
        return { 0, 0, 0 };

    uintptr_t ba = pick_bone_array(ms);
    if (!ba) return { 0, 0, 0 };

    float x = 0.f, y = 0.f, z = 0.f;
    if (!read_val(ba + (bone_id * 12 + 3)  * sizeof(float), x)) return { 0, 0, 0 };
    if (!read_val(ba + (bone_id * 12 + 7)  * sizeof(float), y)) return { 0, 0, 0 };
    if (!read_val(ba + (bone_id * 12 + 11) * sizeof(float), z)) return { 0, 0, 0 };

    Vec3 p = { x, y, z };
    if (!world_pos_plausible(p)) return { 0, 0, 0 };
    return p;
}

static Vec3 get_target_pos(uintptr_t pawn) {
    static const int bone_map[3] = { 6, 5, 4 }; // head, neck, chest
    int bone = bone_map[g_cfg.aimbot_bone % 3];

    Vec3 bp = get_bone_pos(pawn, bone);
    if (bp.x != 0.f || bp.y != 0.f || bp.z != 0.f) return bp;

    uintptr_t node = 0;
    if (!read_val(pawn + off::m_pGameSceneNode, node) || !ptr_sane(node))
        return { 0, 0, 0 };

    Vec3 origin;
    if (!read_origin(node, origin)) return { 0, 0, 0 };
    return { origin.x, origin.y, origin.z + 72.f };
}

static bool is_visible(uintptr_t pawn) {
    // Заглушка: полный TraceRay требует хука engine trace.
    Vec3 tp = get_target_pos(pawn);
    return world_pos_plausible(tp);
}

static uintptr_t find_best_target(float& out_fov_deg) {
    out_fov_deg = g_cfg.aimbot_fov;
    if (!g_client) return 0;

    uintptr_t ent_sys = 0, local = 0;
    if (!read_val(g_client + off::dwEntityList, ent_sys) || !ptr_sane(ent_sys)) return 0;
    if (!read_val(g_client + off::dwLocalPlayerPawn, local) || !ptr_sane(local)) return 0;

    uint8_t local_team = 0;
    if (!read_val(local + off::m_iTeamNum, local_team)) return 0;
    if (local_team != 2 && local_team != 3) return 0;

    Vec3 eye = get_eye_pos(local);
    if (!world_pos_plausible(eye)) return 0;

    Vec2Ang cur = read_view_angles();
    uintptr_t ident_base = ent_sys + off::ENTITY_IDENTITIES;

    float best_fov = g_cfg.aimbot_fov;
    uintptr_t best = 0;
    int null_run = 0;

    for (int i = 0; i < off::MAX_ENTITIES; i++) {
        uintptr_t identity = 0;
        if (!read_val(ident_base + i * sizeof(uintptr_t), identity) || !ptr_sane(identity)) {
            if (++null_run > 64) break;
            continue;
        }
        null_run = 0;

        uintptr_t pawn = resolve_instance(identity, local);
        if (!pawn) continue;

        int hp = 0;
        uint8_t team = 0;
        if (!read_val(pawn + off::m_iHealth, hp)) continue;
        if (hp < 1 || hp > 100) continue;
        if (!read_val(pawn + off::m_iTeamNum, team)) continue;
        if (team != 2 && team != 3) continue;
        if (g_cfg.teamcheck && team == local_team) continue;

        if (g_cfg.aimbot_vis_check && !is_visible(pawn)) continue;

        Vec3 tp = get_target_pos(pawn);
        if (!world_pos_plausible(tp)) continue;

        Vec2Ang want = calc_angles(eye, tp);
        float f = ang_diff(cur, want);
        if (f < best_fov) {
            best_fov = f;
            best = pawn;
        }
    }

    out_fov_deg = best_fov;
    return best;
}

void render_fov_circle(float sw, float sh) {
    if (!g_cfg.aimbot || !g_cfg.aimbot_show_fov) return;

    float cx = sw * 0.5f;
    float cy = sh * 0.5f;
    float t  = tanf(g_cfg.aimbot_fov * PI / 180.f);

    // FIX: точная проекция через viewmatrix (эллипс), а не грубое fov*(sw/90)
    float rx = t * (sw / 90.f);
    float ry = rx;

    ViewMatrix vm;
    if (g_client && read_val(g_client + off::dwViewMatrix, vm) &&
        vm.m[0][0] > 0.01f && vm.m[1][1] > 0.01f) {
        rx = t * vm.m[0][0] * sw * 0.5f;
        ry = t * vm.m[1][1] * sh * 0.5f;
    }

    if (rx < 1.f) rx = 1.f;
    if (ry < 1.f) ry = 1.f;
    if (rx > sw)  rx = sw;
    if (ry > sh)  ry = sh;

    const int segs = 48;
    uint32_t col = gfx::rgba(255, 80, 80, 200);

    for (int i = 0; i < segs; i++) {
        float a0 = (float)i       / segs * 2.f * PI;
        float a1 = (float)(i + 1) / segs * 2.f * PI;
        gfx::draw_line(
            cx + cosf(a0) * rx, cy + sinf(a0) * ry,
            cx + cosf(a1) * rx, cy + sinf(a1) * ry,
            col
        );
    }
}

void run() {
    if (!g_cfg.aimbot) return;
    if (!(GetAsyncKeyState(g_cfg.aimbot_key) & 0x8000)) return;

    float f = 0.f;
    uintptr_t target = find_best_target(f);
    if (!target) return;

    uintptr_t local = 0;
    if (!read_val(g_client + off::dwLocalPlayerPawn, local) || !ptr_sane(local)) return;

    Vec3 eye = get_eye_pos(local);
    Vec3 tp  = get_target_pos(target);
    if (!world_pos_plausible(eye) || !world_pos_plausible(tp)) return;

    // FIX: frame-time normalized smoothing
    static ULONGLONG s_last = 0;
    ULONGLONG now = GetTickCount64();
    float dt = s_last ? (float)(now - s_last) / 1000.f : (1.f / 60.f);
    s_last = now;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.1f)   dt = 0.1f;

    float sm = g_cfg.aimbot_smooth;
    if (sm < 1.f) sm = 1.f;

    float step = (dt * 60.f) / sm;
    if (step > 1.f) step = 1.f;

    Vec2Ang want = calc_angles(eye, tp);
    Vec2Ang cur  = read_view_angles();
    Vec2Ang out  = lerp_angles(cur, want, step);

    write_view_angles(out);
}

} // namespace aim

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------
namespace hk {

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);

struct Entry {
    void**    vtable;
    PresentFn orig;
};

static Entry g_entries[2];
static int   g_count      = 0;
static bool  g_hooked     = false;
static bool  g_first_fire = false;

static PresentFn orig_for(IDXGISwapChain* sc) {
    void** vt = *(void***)sc;
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].vtable == vt) return g_entries[i].orig;
    return g_count ? g_entries[0].orig : nullptr;
}

static HRESULT __stdcall hooked_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    PresentFn orig = orig_for(swap);
    if (!orig) return S_OK;

    if (g_unloading)
        return orig(swap, sync, flags);

    if (!g_first_fire) {
        g_first_fire = true;
        DXGI_SWAP_CHAIN_DESC d = {};
        swap->GetDesc(&d);
        vlog("hk: PRESENT FIRED %ux%u hwnd=%p",
             d.BufferDesc.Width, d.BufferDesc.Height, d.OutputWindow);
        if (d.OutputWindow) menu::set_hwnd(d.OutputWindow);
    }

    // FIX: при фейле init не молотим D3DCompile каждый кадр — retry раз в секунду
    if (!gfx::ready()) {
        static ULONGLONG s_next_try = 0;
        ULONGLONG now = GetTickCount64();
        if (now < s_next_try) return orig(swap, sync, flags);
        if (!gfx::init(swap)) {
            s_next_try = now + 1000;
            return orig(swap, sync, flags);
        }
    }

    float sw = 0.f, sh = 0.f;
    gfx::begin_frame(swap, sw, sh);

    if (sw > 0.f && sh > 0.f) {
        menu::poll(sw, sh);
        aim::run();
        esp_render(sw, sh);
        aim::render_fov_circle(sw, sh);
        menu::render();
    }

    gfx::end_frame();
    return orig(swap, sync, flags);
}

static bool patch_vtable(void** vt) {
    if (!vt) return false;

    for (int i = 0; i < g_count; i++)
        if (g_entries[i].vtable == vt) return true;

    if (g_count >= 2) return false;

    PresentFn orig = (PresentFn)vt[8];

    DWORD old;
    if (!VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return false;

    vt[8] = (void*)&hooked_present;
    VirtualProtect(&vt[8], sizeof(void*), old, &old);

    g_entries[g_count].vtable = vt;
    g_entries[g_count].orig   = orig;
    g_count++;

    vlog("hk: patched vtable %p", (void*)vt);
    return true;
}

static bool make_dummy(bool flip) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount        = flip ? 2 : 1;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = GetDesktopWindow();
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain*      sc  = nullptr;
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &sc, &dev, nullptr, &ctx
    );

    if (FAILED(hr) || !sc) {
        vlog("hk: dummy(%s) fail hr=0x%08X", flip ? "flip" : "blt", (unsigned)hr);
        return false;
    }

    bool ok = patch_vtable(*(void***)sc);

    sc->Release();
    if (dev) dev->Release();
    if (ctx) ctx->Release();
    return ok;
}

static bool install() {
    if (g_hooked) return true;
    bool a = make_dummy(true);
    bool b = make_dummy(false);
    g_hooked = (a || b);
    if (g_hooked) vlog("hk: install ok, vtables=%d", g_count);
    return g_hooked;
}

static void remove() {
    for (int i = 0; i < g_count; i++) {
        void** vt = g_entries[i].vtable;
        DWORD old;
        if (VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            vt[8] = (void*)g_entries[i].orig;
            VirtualProtect(&vt[8], sizeof(void*), old, &old);
        }
    }
    g_count = 0;
    g_hooked = false;
    g_first_fire = false;
    vlog("hk: restored");
}

bool installed() { return g_hooked; }

} // namespace hk

// ---------------------------------------------------------------------------
// ESP
// ---------------------------------------------------------------------------
static void draw_skeleton(uintptr_t pawn, const ViewMatrix& vm, float sw, float sh, uint32_t col) {
    uintptr_t node = 0;
    if (!read_val(pawn + off::m_pGameSceneNode, node) || !ptr_sane(node)) return;

    uintptr_t ms = 0;
    if (!read_val(node + off::m_modelState, ms) || !ptr_sane(ms)) return;

    uintptr_t ba = pick_bone_array(ms);
    if (!ba) return;

    // FIX: один блоковый рид вместо 84 VirtualQuery на скелет
    float bones[20 * 12];
    if (!is_readable(reinterpret_cast<const void*>(ba), sizeof(bones))) return;
    memcpy(bones, reinterpret_cast<const void*>(ba), sizeof(bones));

    static const int conn[14][2] = {
        {0, 5}, {5, 7}, {7, 8}, {8, 9}, {9, 10},
        {7, 11}, {11, 12}, {12, 13},
        {5, 14}, {14, 15}, {15, 16},
        {5, 17}, {17, 18}, {18, 19}
    };

    for (int i = 0; i < 14; i++) {
        int a = conn[i][0];
        int b = conn[i][1];
        if (a >= 20 || b >= 20) continue;

        Vec3 pa = { bones[a * 12 + 3], bones[a * 12 + 7], bones[a * 12 + 11] };
        Vec3 pb = { bones[b * 12 + 3], bones[b * 12 + 7], bones[b * 12 + 11] };

        if (!world_pos_plausible(pa) || !world_pos_plausible(pb)) continue;

        Vec2 sa, sb;
        if (!world_to_screen(pa, sa, vm, sw, sh)) continue;
        if (!world_to_screen(pb, sb, vm, sw, sh)) continue;

        gfx::draw_line(sa.x, sa.y, sb.x, sb.y, col);
    }
}

static void esp_render(float sw, float sh) {
    if (!g_client || sw <= 0.f || sh <= 0.f) return;

    uintptr_t entity_system = 0, local = 0;
    if (!read_val(g_client + off::dwEntityList, entity_system) || !ptr_sane(entity_system)) return;
    if (!read_val(g_client + off::dwLocalPlayerPawn, local) || !ptr_sane(local)) return;

    ViewMatrix vm;
    if (!read_val(g_client + off::dwViewMatrix, vm)) return;

    uint8_t local_team = 0;
    if (!read_val(local + off::m_iTeamNum, local_team)) return;
    if (local_team != 2 && local_team != 3) return;

    // Player controllers normally occupy the first entity chunk.
    // Resolve each controller's pawn through its handle, which may point
    // into a later chunk.
    for (uint32_t i = 1; i <= (uint32_t)off::MAX_CLIENTS; ++i) {
        const uintptr_t controller = entity_instance_at(entity_system, i);
        if (!ptr_sane(controller)) continue;

        uint32_t pawn_handle = 0xFFFFFFFFu;
        if (!controller_ok(controller, pawn_handle)) continue;

        const uint32_t pawn_index = pawn_handle & 0x7FFFu;
        const uintptr_t pawn = resolve_pawn_at(entity_system, pawn_index, local);
        if (!pawn || pawn == local) continue;

        int hp = 0;
        uint8_t team = 0;
        if (!read_val(pawn + off::m_iHealth, hp) || hp < 1 || hp > 100) continue;
        if (!read_val(pawn + off::m_iTeamNum, team) || (team != 2 && team != 3)) continue;
        if (g_cfg.teamcheck && team == local_team) continue;

        uintptr_t node = 0;
        if (!read_val(pawn + off::m_pGameSceneNode, node) || !ptr_sane(node)) continue;

        Vec3 origin;
        if (!read_origin(node, origin)) continue;

        Vec3 head_w = { origin.x, origin.y, origin.z + 72.f };
        Vec2 head_s, feet_s;
        if (!world_to_screen(head_w, head_s, vm, sw, sh)) continue;
        if (!world_to_screen(origin, feet_s, vm, sw, sh)) continue;

        const float h = feet_s.y - head_s.y;
        if (!std::isfinite(h) || h < 4.f || h > sh * 2.f) continue;

        const float w = h * 0.4f;
        if (head_s.x + w < 0.f || head_s.x - w > sw ||
            feet_s.y < 0.f || head_s.y > sh)
            continue;

        const float bx = head_s.x - w * 0.5f;
        const uint32_t col = (team == local_team)
            ? gfx::rgba(60, 200, 60, 255)
            : gfx::rgba(255, 60, 60, 255);

        if (g_cfg.box) {
            gfx::draw_rect(bx - 1.f, head_s.y - 1.f, w + 2.f, h + 2.f, gfx::rgba(0, 0, 0, 200));
            gfx::draw_rect(bx, head_s.y, w, h, col);
        }

        if (g_cfg.health) {
            const float pct = (float)hp / 100.f;
            gfx::draw_rect_filled(bx - 6.f, head_s.y, 4.f, h, gfx::rgba(0, 0, 0, 180));
            gfx::draw_rect_filled(
                bx - 6.f, head_s.y + h - h * pct, 4.f, h * pct,
                gfx::rgba((uint8_t)(255.f - 255.f * pct), (uint8_t)(255.f * pct), 0, 255)
            );
        }

        if (g_cfg.name) {
            char name[32] = {};
            const uintptr_t name_addr = controller + off::m_iszPlayerName;
            if (is_readable(reinterpret_cast<const void*>(name_addr), sizeof(name))) {
                const char* src = reinterpret_cast<const char*>(name_addr);
                int n = 0;
                while (n < (int)sizeof(name) - 1 && src[n] >= 0x20 && src[n] <= 0x7E) {
                    name[n] = src[n];
                    ++n;
                }
                name[n] = 0;
            }

            if (name[0])
                gfx::draw_text(head_s.x, head_s.y - 18.f, gfx::rgba(255, 255, 255, 255), true, "%s [%d]", name, hp);
            else
                gfx::draw_text(head_s.x, head_s.y - 18.f, gfx::rgba(255, 255, 255, 255), true, "HP: %d", hp);
        }

        if (g_cfg.skeleton)
            draw_skeleton(pawn, vm, sw, sh, gfx::rgba(255, 255, 255, 220));
    }
}

// ---------------------------------------------------------------------------
// Main thread
// ---------------------------------------------------------------------------
static DWORD WINAPI init_thread(LPVOID) {
    vlog("=== void: thread start ===");

    while (!g_client && !g_unloading) {
        g_client = (uintptr_t)GetModuleHandleA("client.dll");
        if (!g_client) Sleep(200);
    }
    if (g_unloading) return 0;

    vlog("void: client.dll=%p", (void*)g_client);

    Sleep(3000);
    menu::install();
    hk::install();

    for (;;) {
        if (GetAsyncKeyState(VK_END) & 1) break;
        if (!hk::installed()) { hk::install(); Sleep(1000); }
        else Sleep(10);
    }

    // FIX: корректная выгрузка без гонки с render-потоком:
    // 1) флаг -> render-поток выходит из хука
    // 2) ждём -> снимаем хук -> ждём -> освобождаем gfx
    // 3) FreeLibraryAndExitThread вместо голого ExitThread (DLL реально выгружается)
    g_unloading = true;
    Sleep(400);
    hk::remove();
    Sleep(200);
    gfx::shutdown();

    vlog("void: bye");
    FreeLibraryAndExitThread(g_hmodule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hmodule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_unloading = true;
        hk::remove();      // идемпотентно
        gfx::shutdown();   // идемпотентно
    }
    return TRUE;
}
