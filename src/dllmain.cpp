// Void Internal — CS2 internal ESP. Flip+blt vtable hook, self-validating reads.
// Offsets from sezzyaep/CS2-OFFSETS dump 18.11.2026.
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <vector>
#include "config.h"
#include "gfx.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static void vlog(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, "void_internal.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) { fprintf(f, "%s\n", buf); fclose(f); }
}

// ---------------------------------------------------------------------------
// Offsets (dump 18.11.2026 sezzyaep).
// [V] = verified из дампа, [?] = автоподбор рантаймом
// ---------------------------------------------------------------------------
namespace off {
    // Глобалы client.dll
    constexpr uintptr_t dwEntityList            = 0x270C680; // 40945280 [V]
    constexpr uintptr_t dwLocalPlayerController = 0x252ED78; // 38989176 [V]
    constexpr uintptr_t dwLocalPlayerPawn       = 0x2557538; // 39155000 [V]
    constexpr uintptr_t dwViewMatrix            = 0x255CE50; // 39177808 [V]

    // C_BaseEntity
    constexpr uintptr_t m_iHealth               = 0x34C;     // 844  [V]
    constexpr uintptr_t m_lifeState             = 0x354;     // 852  [V]
    constexpr uintptr_t m_pGameSceneNode        = 0x330;     // 816  [V]
    constexpr uintptr_t m_iTeamNum              = 0x3E7;     // 999  [V]

    // CGameSceneNode
    constexpr uintptr_t m_vecAbsOrigin          = 0xC8;      // 200  [V] plain Vector
    constexpr uintptr_t m_vecOrigin             = 0x80;      // 128  [V] quantized (fallback)

    // CSkeletonInstance (parent: CGameSceneNode)
    constexpr uintptr_t m_modelState            = 0x140;     // 320  [V]

    // CModelState — m_pBoneArray НЕТ в дампе, подбирается рантаймом из кандидатов.
    // Доступные поля: m_hModel=0xA0, m_ModelName=0xA8, m_pVPhysicsAggregate=0xE0,
    //                 m_flRootBoneOffset_xyz=0xE8/0xEC/0xF0, m_MeshGroupMask=0x208

    // CBasePlayerController
    constexpr uintptr_t m_hPawn                 = 0x6BC;     // 1724 [V]
    constexpr uintptr_t m_iszPlayerName         = 0x6FC;     // 1788 [V]

    // C_CSPlayerPawnBase
    constexpr uintptr_t m_hOriginalController   = 0x1560;    // 5472 [V]

    // CEntityIdentity — m_pInstance НЕТ в дампе, подбирается из кандидатов.
    // Доступные поля: m_name=0x18, m_designerName=0x20, m_flags=0x30,
    //                 m_worldGroupId=0x38, m_fDataObjectTypes=0x3C, m_PathIndex=0x40,
    //                 m_pAttributes=0x48, m_pPrev=0x50, m_pNext=0x58,
    //                 m_pPrevByClass=0x60, m_pNextByClass=0x68

    constexpr uintptr_t ENTITY_IDENTITIES       = 0x10;
    constexpr int       MAX_ENTITIES            = 4096;
}

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct ViewMatrix { float m[4][4]; };

static bool coord_sane(float v) { return v == v && v > -1e5f && v < 1e5f; }

static bool world_pos_plausible(const Vec3& v) {
    if (!coord_sane(v.x) || !coord_sane(v.y) || !coord_sane(v.z)) return false;
    if (v.x == 0.f && v.y == 0.f && v.z == 0.f) return false;
    return fabsf(v.x) < 20000.f && fabsf(v.y) < 20000.f && v.z > -3000.f && v.z < 3000.f;
}

static bool world_to_screen(const Vec3& w, Vec2& s, const ViewMatrix& vm, float sw, float sh) {
    float cw = vm.m[3][0]*w.x + vm.m[3][1]*w.y + vm.m[3][2]*w.z + vm.m[3][3];
    if (cw < 0.001f) return false;
    float cx = vm.m[0][0]*w.x + vm.m[0][1]*w.y + vm.m[0][2]*w.z + vm.m[0][3];
    float cy = vm.m[1][0]*w.x + vm.m[1][1]*w.y + vm.m[1][2]*w.z + vm.m[1][3];
    s.x = sw*0.5f + (cx/cw)*sw*0.5f;
    s.y = sh*0.5f - (cy/cw)*sh*0.5f;
    return true;
}

Config g_cfg = { true, true, true, true, true };
static bool g_unloading = false;
using gfx::rgba;

void esp_render(float sw, float sh);

namespace menu {
    void install();
    void set_hwnd(HWND h);
    void poll(float sw, float sh);
    void render();
    void toggle();
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
namespace gfx {
    struct Vertex { float x,y,z,w; float r,g,b,a; float u,v; };
    struct Batch  { bool textured; uint32_t start; uint32_t count; };

    static ID3D11Device* g_dev=nullptr; static ID3D11DeviceContext* g_ctx=nullptr;
    static ID3D11InputLayout* g_il=nullptr;
    static ID3D11VertexShader* g_vs=nullptr;
    static ID3D11PixelShader *g_ps_solid=nullptr, *g_ps_tex=nullptr;
    static ID3D11Buffer* g_vb=nullptr; static uint32_t g_vb_cap=0;
    static ID3D11BlendState* g_blend=nullptr;
    static ID3D11RasterizerState* g_rs=nullptr;
    static ID3D11ShaderResourceView* g_font_srv=nullptr;
    static ID3D11SamplerState* g_samp=nullptr;
    static ID3D11RenderTargetView* g_rtv=nullptr;

    static std::vector<Vertex> g_verts;
    static std::vector<Batch>  g_batches;
    static float g_cw[256], g_ch=16.f, g_u0[256], g_v0[256], g_u1[256], g_v1[256];
    static bool g_ready=false;

    static const char* VS_SRC =
        "struct VI { float4 p:POSITION; float4 c:COLOR; float2 t:TEXCOORD; };\n"
        "struct VO { float4 p:SV_POSITION; float4 c:COLOR; float2 t:TEXCOORD; };\n"
        "VO main(VI i){ VO o; o.p=i.p; o.c=i.c; o.t=i.t; return o; }\n";
    static const char* PS_SOLID_SRC =
        "struct PI { float4 p:SV_POSITION; float4 c:COLOR; float2 t:TEXCOORD; };\n"
        "float4 main(PI i):SV_Target { return i.c; }\n";
    static const char* PS_TEX_SRC =
        "struct PI { float4 p:SV_POSITION; float4 c:COLOR; float2 t:TEXCOORD; };\n"
        "Texture2D tex:register(t0); SamplerState samp:register(s0);\n"
        "float4 main(PI i):SV_Target { float4 t=tex.Sample(samp,i.t); return float4(i.c.rgb, i.c.a*t.a); }\n";

    static bool build_font(ID3D11Device* dev, ID3D11ShaderResourceView** out_srv, ID3D11SamplerState** out_samp) {
        const int TW=512, TH=512;
        HDC hdc = CreateCompatibleDC(nullptr);
        if (!hdc) return false;
        HFONT hf = CreateFontA(16,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,
                               DEFAULT_PITCH|FF_DONTCARE,"Arial");
        HFONT oldf = (HFONT)SelectObject(hdc, hf);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth=TW; bmi.bmiHeader.biHeight=-TH;
        bmi.bmiHeader.biPlanes=1; bmi.bmiHeader.biBitCount=32; bmi.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr;
        HBITMAP hb = CreateDIBSection(hdc,&bmi,DIB_RGB_COLORS,&bits,nullptr,0);
        if (!hb || !bits) { SelectObject(hdc,oldf); DeleteObject(hf); DeleteDC(hdc); return false; }
        HBITMAP oldb = (HBITMAP)SelectObject(hdc, hb);
        SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,RGB(255,255,255));
        memset(bits,0,(size_t)TW*TH*4);

        int px=0,py=0,rowh=0;
        for (int i=32;i<256;i++) {
            SIZE sz; char ch=(char)i;
            GetTextExtentPoint32A(hdc,&ch,1,&sz);
            if (px+sz.cx+2>TW) { px=0; py+=rowh+2; rowh=0; }
            TextOutA(hdc,px,py,&ch,1);
            g_cw[i]=(float)sz.cx;
            g_u0[i]=(float)px/TW; g_v0[i]=(float)py/TH;
            g_u1[i]=(float)(px+sz.cx)/TW; g_v1[i]=(float)(py+sz.cy)/TH;
            if (sz.cy>rowh) rowh=sz.cy;
            px+=sz.cx+2;
        }
        uint32_t* pix=(uint32_t*)bits;
        for (int i=0;i<TW*TH;i++) {
            uint8_t r=(uint8_t)((pix[i]>>16)&0xFF), g=(uint8_t)((pix[i]>>8)&0xFF), b=(uint8_t)(pix[i]&0xFF);
            uint8_t a=(uint8_t)((r+g+b)/3);
            pix[i]=((uint32_t)a<<24)|0x00FFFFFFu;
        }
        D3D11_TEXTURE2D_DESC td={};
        td.Width=TW; td.Height=TH; td.MipLevels=1; td.ArraySize=1;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1;
        td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd={}; sd.pSysMem=bits; sd.SysMemPitch=TW*4;
        ID3D11Texture2D* tex=nullptr;
        HRESULT hr = dev->CreateTexture2D(&td,&sd,&tex);
        if (SUCCEEDED(hr)&&tex) { hr=dev->CreateShaderResourceView(tex,nullptr,out_srv); tex->Release(); }
        D3D11_SAMPLER_DESC sm={};
        sm.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU=sm.AddressV=sm.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        if (SUCCEEDED(hr)) hr=dev->CreateSamplerState(&sm,out_samp);

        SelectObject(hdc,oldb); DeleteObject(hb);
        SelectObject(hdc,oldf); DeleteObject(hf); DeleteDC(hdc);
        return SUCCEEDED(hr) && *out_srv && *out_samp;
    }

    static bool init(IDXGISwapChain* swap) {
        if (g_ready) return true;
        if (!swap) return false;
        ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr;
        ID3DBlob *vsb=nullptr,*ps1=nullptr,*ps2=nullptr;
        ID3D11VertexShader* vs=nullptr; ID3D11PixelShader *pss=nullptr,*pst=nullptr;
        ID3D11InputLayout* il=nullptr; ID3D11Buffer* vb=nullptr;
        ID3D11BlendState* bl=nullptr; ID3D11RasterizerState* rs=nullptr;
        ID3D11ShaderResourceView* srv=nullptr; ID3D11SamplerState* samp=nullptr;
        bool ok=false;
        do {
            if (FAILED(swap->GetDevice(__uuidof(ID3D11Device),(void**)&dev))||!dev) break;
            dev->GetImmediateContext(&ctx); if(!ctx) break;
            if (FAILED(D3DCompile(VS_SRC,strlen(VS_SRC),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsb,nullptr))||!vsb) break;
            if (FAILED(D3DCompile(PS_SOLID_SRC,strlen(PS_SOLID_SRC),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&ps1,nullptr))||!ps1) break;
            if (FAILED(D3DCompile(PS_TEX_SRC,strlen(PS_TEX_SRC),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&ps2,nullptr))||!ps2) break;
            if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(),vsb->GetBufferSize(),nullptr,&vs))||!vs) break;
            if (FAILED(dev->CreatePixelShader(ps1->GetBufferPointer(),ps1->GetBufferSize(),nullptr,&pss))||!pss) break;
            if (FAILED(dev->CreatePixelShader(ps2->GetBufferPointer(),ps2->GetBufferSize(),nullptr,&pst))||!pst) break;
            D3D11_INPUT_ELEMENT_DESC ild[]={
                {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
                {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
                {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
            };
            if (FAILED(dev->CreateInputLayout(ild,3,vsb->GetBufferPointer(),vsb->GetBufferSize(),&il))||!il) break;
            D3D11_BUFFER_DESC bd={};
            bd.Usage=D3D11_USAGE_DYNAMIC; bd.ByteWidth=sizeof(Vertex)*20000;
            bd.BindFlags=D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&bd,nullptr,&vb))||!vb) break;
            D3D11_BLEND_DESC bld={};
            bld.RenderTarget[0].BlendEnable=TRUE;
            bld.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;
            bld.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
            bld.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
            bld.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
            bld.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
            bld.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
            bld.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(dev->CreateBlendState(&bld,&bl))||!bl) break;
            D3D11_RASTERIZER_DESC rd={};
            rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE;
            if (FAILED(dev->CreateRasterizerState(&rd,&rs))||!rs) break;
            if (!build_font(dev,&srv,&samp)) break;
            ok=true;
        } while(false);
        if (vsb) vsb->Release();
        if (ps1) ps1->Release();
        if (ps2) ps2->Release();
        if (!ok) {
            if(srv)srv->Release(); if(samp)samp->Release(); if(rs)rs->Release();
            if(bl)bl->Release(); if(vb)vb->Release(); if(il)il->Release();
            if(pst)pst->Release(); if(pss)pss->Release(); if(vs)vs->Release();
            if(ctx)ctx->Release(); if(dev)dev->Release();
            vlog("gfx: init FAIL");
            return false;
        }
        g_dev=dev; g_ctx=ctx; g_vs=vs; g_ps_solid=pss; g_ps_tex=pst;
        g_il=il; g_vb=vb; g_vb_cap=20000; g_blend=bl; g_rs=rs;
        g_font_srv=srv; g_samp=samp; g_ready=true;
        vlog("gfx: init OK");
        return true;
    }

    bool ready() { return g_ready; }

    void shutdown() {
        if(g_rtv){g_rtv->Release();g_rtv=nullptr;}
        if(g_vb){g_vb->Release();g_vb=nullptr;}
        if(g_blend){g_blend->Release();g_blend=nullptr;}
        if(g_rs){g_rs->Release();g_rs=nullptr;}
        if(g_font_srv){g_font_srv->Release();g_font_srv=nullptr;}
        if(g_samp){g_samp->Release();g_samp=nullptr;}
        if(g_il){g_il->Release();g_il=nullptr;}
        if(g_vs){g_vs->Release();g_vs=nullptr;}
        if(g_ps_solid){g_ps_solid->Release();g_ps_solid=nullptr;}
        if(g_ps_tex){g_ps_tex->Release();g_ps_tex=nullptr;}
        if(g_ctx){g_ctx->Release();g_ctx=nullptr;}
        if(g_dev){g_dev->Release();g_dev=nullptr;}
        g_verts.clear(); g_batches.clear(); g_ready=false;
        vlog("gfx: shutdown");
    }

    static void begin_frame(IDXGISwapChain* swap, float& sw, float& sh) {
        g_verts.clear(); g_batches.clear();
        sw=0.f; sh=0.f;
        if (!g_ready) return;
        if (g_rtv) { g_rtv->Release(); g_rtv=nullptr; }

        UINT idx = 0;
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain3),(void**)&sc3)) && sc3) {
            idx = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
        ID3D11Texture2D* bb=nullptr;
        if (SUCCEEDED(swap->GetBuffer(idx,__uuidof(ID3D11Texture2D),(void**)&bb)) && bb) {
            D3D11_TEXTURE2D_DESC dd; bb->GetDesc(&dd);
            sw=(float)dd.Width; sh=(float)dd.Height;
            g_dev->CreateRenderTargetView(bb,nullptr,&g_rtv);
            bb->Release();
        }
    }

    static uint32_t reserve(bool textured, uint32_t n) {
        if (g_batches.empty() || g_batches.back().textured!=textured) {
            Batch b; b.textured=textured; b.start=(uint32_t)g_verts.size(); b.count=0;
            g_batches.push_back(b);
        }
        uint32_t start=(uint32_t)g_verts.size();
        g_batches.back().count+=n;
        g_verts.resize(g_verts.size()+n);
        return start;
    }
    static void fill(Vertex& v,float x,float y,uint32_t col,float u,float vv) {
        v.x=x; v.y=y; v.z=0.f; v.w=1.f;
        v.r=((col>>24)&0xFF)/255.f; v.g=((col>>16)&0xFF)/255.f;
        v.b=((col>>8)&0xFF)/255.f;  v.a=(col&0xFF)/255.f;
        v.u=u; v.v=vv;
    }
    static void add_quad(float x0,float y0,float x1,float y1,uint32_t col,
                         float u0,float v0,float u1,float v1) {
        uint32_t s=reserve(true,6);
        fill(g_verts[s+0],x0,y0,col,u0,v0); fill(g_verts[s+1],x1,y0,col,u1,v0);
        fill(g_verts[s+2],x0,y1,col,u0,v1); fill(g_verts[s+3],x1,y0,col,u1,v0);
        fill(g_verts[s+4],x1,y1,col,u1,v1); fill(g_verts[s+5],x0,y1,col,u0,v1);
    }
    void draw_line(float x0,float y0,float x1,float y1,uint32_t col) {
        float dx=x1-x0, dy=y1-y0, len=sqrtf(dx*dx+dy*dy);
        if (len<0.0001f) return;
        float nx=(-dy/len)*0.5f, ny=(dx/len)*0.5f;
        uint32_t s=reserve(false,6);
        fill(g_verts[s+0],x0+nx,y0+ny,col,0,0); fill(g_verts[s+1],x1+nx,y1+ny,col,0,0);
        fill(g_verts[s+2],x0-nx,y0-ny,col,0,0); fill(g_verts[s+3],x1+nx,y1+ny,col,0,0);
        fill(g_verts[s+4],x1-nx,y1-ny,col,0,0); fill(g_verts[s+5],x0-nx,y0-ny,col,0,0);
    }
    void draw_rect(float x,float y,float w,float h,uint32_t col) {
        draw_line(x,y,x+w,y,col); draw_line(x+w,y,x+w,y+h,col);
        draw_line(x+w,y+h,x,y+h,col); draw_line(x,y+h,x,y,col);
    }
    void draw_rect_filled(float x,float y,float w,float h,uint32_t col) {
        uint32_t s=reserve(false,6);
        fill(g_verts[s+0],x,y,col,0,0); fill(g_verts[s+1],x+w,y,col,0,0);
        fill(g_verts[s+2],x,y+h,col,0,0); fill(g_verts[s+3],x+w,y,col,0,0);
        fill(g_verts[s+4],x+w,y+h,col,0,0); fill(g_verts[s+5],x,y+h,col,0,0);
    }
    float text_width(const char* s) {
        float w=0.f;
        for(;*s;s++){ unsigned char c=(unsigned char)*s; w += (c>=32)?g_cw[c]:4.f; }
        return w;
    }
    void draw_text(float x,float y,uint32_t col,bool centered,const char* fmt,...) {
        char buf[512]; va_list ap; va_start(ap,fmt);
        vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
        if (centered) x -= text_width(buf)*0.5f;
        float cx=x;
        for(char* p=buf;*p;p++){
            unsigned char c=(unsigned char)*p;
            if(c<32){cx+=4.f;continue;}
            add_quad(cx,y,cx+g_cw[c],y+g_ch,col,g_u0[c],g_v0[c],g_u1[c],g_v1[c]);
            cx+=g_cw[c];
        }
    }

    static void end_frame() {
        if (!g_ready || !g_rtv || g_verts.empty()) {
            if(g_rtv){g_rtv->Release();g_rtv=nullptr;}
            return;
        }
        if (g_verts.size()>g_vb_cap) {
            if(g_vb) g_vb->Release();
            g_vb_cap=(uint32_t)g_verts.size()*2;
            D3D11_BUFFER_DESC bd={};
            bd.Usage=D3D11_USAGE_DYNAMIC; bd.ByteWidth=sizeof(Vertex)*g_vb_cap;
            bd.BindFlags=D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            g_dev->CreateBuffer(&bd,nullptr,&g_vb);
        }
        D3D11_MAPPED_SUBRESOURCE mp;
        if (FAILED(g_ctx->Map(g_vb,0,D3D11_MAP_WRITE_DISCARD,0,&mp))) {
            g_rtv->Release(); g_rtv=nullptr; return;
        }
        memcpy(mp.pData,g_verts.data(),g_verts.size()*sizeof(Vertex));
        g_ctx->Unmap(g_vb,0);

        ID3D11RenderTargetView* oRTV=nullptr; ID3D11DepthStencilView* oDSV=nullptr;
        g_ctx->OMGetRenderTargets(1,&oRTV,&oDSV);
        UINT nvp=1; D3D11_VIEWPORT oVP[1]={}; g_ctx->RSGetViewports(&nvp,oVP);
        ID3D11BlendState* oBlend=nullptr; FLOAT oBF[4]={}; UINT oMask=0;
        g_ctx->OMGetBlendState(&oBlend,oBF,&oMask);
        ID3D11RasterizerState* oRS=nullptr; g_ctx->RSGetState(&oRS);
        ID3D11InputLayout* oIL=nullptr; g_ctx->IAGetInputLayout(&oIL);
        D3D11_PRIMITIVE_TOPOLOGY oTopo=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        g_ctx->IAGetPrimitiveTopology(&oTopo);
        ID3D11Buffer* oVB=nullptr; UINT oStride=0,oOff=0;
        g_ctx->IAGetVertexBuffers(0,1,&oVB,&oStride,&oOff);
        ID3D11VertexShader* oVS=nullptr; g_ctx->VSGetShader(&oVS,nullptr,nullptr);
        ID3D11PixelShader* oPS=nullptr; g_ctx->PSGetShader(&oPS,nullptr,nullptr);
        ID3D11ShaderResourceView* oSRV=nullptr; g_ctx->PSGetShaderResources(0,1,&oSRV);
        ID3D11SamplerState* oSamp=nullptr; g_ctx->PSGetSamplers(0,1,&oSamp);

        D3D11_VIEWPORT vp={};
        vp.MinDepth=0.f; vp.MaxDepth=1.f; vp.Width=1920.f; vp.Height=1080.f;
        ID3D11Resource* res=nullptr; g_rtv->GetResource(&res);
        if(res){
            ID3D11Texture2D* t2=nullptr;
            if(SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&t2))){
                D3D11_TEXTURE2D_DESC dd; t2->GetDesc(&dd);
                vp.Width=(float)dd.Width; vp.Height=(float)dd.Height; t2->Release();
            }
            res->Release();
        }
        g_ctx->OMSetRenderTargets(1,&g_rtv,nullptr);
        g_ctx->RSSetViewports(1,&vp);
        g_ctx->IASetInputLayout(g_il);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride=sizeof(Vertex), off=0;
        g_ctx->IASetVertexBuffers(0,1,&g_vb,&stride,&off);
        g_ctx->RSSetState(g_rs);
        float bf[4]={0,0,0,0};
        g_ctx->OMSetBlendState(g_blend,bf,0xFFFFFFFF);
        g_ctx->VSSetShader(g_vs,nullptr,0);
        for(size_t i=0;i<g_batches.size();i++){
            const Batch& b=g_batches[i];
            g_ctx->PSSetShader(b.textured?g_ps_tex:g_ps_solid,nullptr,0);
            if(b.textured){
                g_ctx->PSSetShaderResources(0,1,&g_font_srv);
                g_ctx->PSSetSamplers(0,1,&g_samp);
            }
            g_ctx->Draw(b.count,b.start);
        }

        g_ctx->OMSetRenderTargets(1,&oRTV,oDSV);
        if(nvp) g_ctx->RSSetViewports(nvp,oVP);
        g_ctx->OMSetBlendState(oBlend,oBF,oMask);
        g_ctx->RSSetState(oRS);
        g_ctx->IASetInputLayout(oIL);
        g_ctx->IASetPrimitiveTopology(oTopo);
        g_ctx->IASetVertexBuffers(0,1,&oVB,&oStride,&oOff);
        g_ctx->VSSetShader(oVS,nullptr,0);
        g_ctx->PSSetShader(oPS,nullptr,0);
        g_ctx->PSSetShaderResources(0,1,&oSRV);
        g_ctx->PSSetSamplers(0,1,&oSamp);
        if(oRTV)oRTV->Release(); if(oDSV)oDSV->Release();
        if(oBlend)oBlend->Release(); if(oRS)oRS->Release();
        if(oIL)oIL->Release(); if(oVB)oVB->Release();
        if(oVS)oVS->Release(); if(oPS)oPS->Release();
        if(oSRV)oSRV->Release(); if(oSamp)oSamp->Release();

        g_rtv->Release(); g_rtv=nullptr;
    }
}

// ---------------------------------------------------------------------------
// Hook: патчим ОБЕ vtable (flip-model и blt-model), оригинал свой для каждой.
// ---------------------------------------------------------------------------
namespace hk {
    typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*,UINT,UINT);
    struct Entry { void** vtable; PresentFn orig; };
    static Entry g_entries[2];
    static int   g_count = 0;
    static bool  g_hooked = false;
    static bool  g_first_fire = false;

    static PresentFn orig_for(IDXGISwapChain* sc) {
        void** vt = *(void***)sc;
        for (int i=0;i<g_count;i++) if (g_entries[i].vtable==vt) return g_entries[i].orig;
        return g_count ? g_entries[0].orig : nullptr;
    }

    HRESULT __stdcall hooked_present(IDXGISwapChain* swap, UINT sync, UINT flags) {
        PresentFn orig = orig_for(swap);
        if (!orig) return S_OK;
        if (g_unloading) return orig(swap,sync,flags);

        if (!g_first_fire) {
            g_first_fire = true;
            DXGI_SWAP_CHAIN_DESC d={};
            swap->GetDesc(&d);
            vlog("hk: PRESENT FIRED  %ux%u buffers=%u effect=%d hwnd=%p",
                 d.BufferDesc.Width, d.BufferDesc.Height, d.BufferCount, (int)d.SwapEffect, d.OutputWindow);
            if (d.OutputWindow) menu::set_hwnd(d.OutputWindow);
        }
        if (!gfx::init(swap)) return orig(swap,sync,flags);

        float sw=0.f, sh=0.f;
        gfx::begin_frame(swap,sw,sh);
        if (sw>0.f && sh>0.f) {
            menu::poll(sw,sh);
            esp_render(sw,sh);
            menu::render();
        }
        gfx::end_frame();
        return orig(swap,sync,flags);
    }

    static bool patch_vtable(void** vt) {
        if (!vt) return false;
        for (int i=0;i<g_count;i++) if (g_entries[i].vtable==vt) return true;
        if (g_count>=2) return false;
        PresentFn orig = (PresentFn)vt[8];
        DWORD old;
        if (!VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
        vt[8] = (void*)&hooked_present;
        VirtualProtect(&vt[8], sizeof(void*), old, &old);
        g_entries[g_count].vtable = vt;
        g_entries[g_count].orig = orig;
        g_count++;
        vlog("hk: patched vtable %p (orig %p)", (void*)vt, (void*)orig);
        return true;
    }

    static bool make_dummy(bool flip) {
        DXGI_SWAP_CHAIN_DESC sd={};
        sd.BufferCount = flip ? 2 : 1;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = GetDesktopWindow();
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* sc=nullptr; ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
                        nullptr,0,D3D11_SDK_VERSION,&sd,&sc,&dev,nullptr,&ctx);
        if (FAILED(hr)||!sc) { vlog("hk: dummy(%s) fail hr=0x%08X", flip?"flip":"blt", (unsigned)hr); return false; }
        bool ok = patch_vtable(*(void***)sc);
        sc->Release(); if(dev)dev->Release(); if(ctx)ctx->Release();
        return ok;
    }

    static bool install() {
        if (g_hooked) return true;
        bool a = make_dummy(true);   // flip-model (CS2 использует)
        bool b = make_dummy(false);  // blt-model (fallback)
        g_hooked = (a||b);
        if (g_hooked) vlog("hk: install ok, vtables=%d", g_count);
        return g_hooked;
    }

    static void remove() {
        for (int i=0;i<g_count;i++) {
            void** vt=g_entries[i].vtable;
            DWORD old;
            if (VirtualProtect(&vt[8],sizeof(void*),PAGE_EXECUTE_READWRITE,&old)) {
                vt[8]=(void*)g_entries[i].orig;
                VirtualProtect(&vt[8],sizeof(void*),old,&old);
            }
        }
        g_count=0; g_hooked=false; g_first_fire=false;
        vlog("hk: restored");
    }
}

// ---------------------------------------------------------------------------
// ESP: авто-подбор origin/bonearray с валидацией + диагностика
// ---------------------------------------------------------------------------
static uintptr_t g_client = 0;
static int  g_origin_off = -1;
static int  g_bone_off   = -1;
static int  g_diag       = 0;

static bool ptr_sane(uintptr_t p) { return p>0x10000 && p<0x7FFFFFFFFFFF; }

static bool pawn_ok(uintptr_t p, uintptr_t local) {
    if (!ptr_sane(p)||p==local) return false;
    uint8_t team=*(uint8_t*)(p+off::m_iTeamNum);
    if (team!=2&&team!=3) return false;
    int hp=*(int*)(p+off::m_iHealth);
    if (hp<1||hp>100) return false;
    if (*(uint8_t*)(p+off::m_lifeState)!=0) return false;
    return ptr_sane(*(uintptr_t*)(p+off::m_pGameSceneNode));
}

// CEntityIdentity::m_pInstance нет в дампе — подбираем из кандидатов
static uintptr_t resolve_instance(uintptr_t identity, uintptr_t local) {
    static const uintptr_t cands[4]={0x0,0x70,0x78,0x80};
    for(int i=0;i<4;i++){
        uintptr_t p=*(uintptr_t*)(identity+cands[i]);
        if (pawn_ok(p,local)) return p;
    }
    return 0;
}

static bool controller_ok(uintptr_t c) {
    if(!ptr_sane(c)) return false;
    uint8_t team=*(uint8_t*)(c+off::m_iTeamNum);
    if(team!=2&&team!=3) return false;
    return (*(uint32_t*)(c+off::m_hPawn)&0x7FFF) < (uint32_t)off::MAX_ENTITIES;
}

// m_vecAbsOrigin (0xC8) — primary, m_vecOrigin (0x80, квантованный) — fallback
static bool read_origin(uintptr_t node, Vec3& out) {
    static const uintptr_t cands[4]={0xC8,0xD0,0xC0,0x80};
    if (g_origin_off>=0) {
        out=*(Vec3*)(node+(uintptr_t)g_origin_off);
        return world_pos_plausible(out);
    }
    for(int i=0;i<4;i++){
        Vec3 v=*(Vec3*)(node+cands[i]);
        if (world_pos_plausible(v)) {
            g_origin_off=(int)cands[i];
            vlog("esp: origin offset picked = 0x%X  (%.1f %.1f %.1f)",
                 g_origin_off, v.x, v.y, v.z);
            out=v; return true;
        }
    }
    return false;
}

// CModelState::m_pBoneArray нет в дампе — расширенный автоподбор
static uintptr_t pick_bone_array(uintptr_t ms) {
    static const uintptr_t cands[8]={0x80,0x88,0x90,0x78,0xA0,0xB0,0xC0,0x70};
    if (g_bone_off>=0) {
        uintptr_t ba=*(uintptr_t*)(ms+(uintptr_t)g_bone_off);
        return ptr_sane(ba)?ba:0;
    }
    for(int i=0;i<8;i++){
        uintptr_t ba=*(uintptr_t*)(ms+cands[i]);
        if(!ptr_sane(ba)) continue;
        const float* b=(const float*)ba;
        Vec3 t={b[3],b[7],b[11]};
        if (world_pos_plausible(t)) {
            g_bone_off=(int)cands[i];
            vlog("esp: bonearray offset picked = 0x%X", g_bone_off);
            return ba;
        }
    }
    return 0;
}

static void draw_skeleton(uintptr_t pawn,const ViewMatrix& vm,float sw,float sh,uint32_t col) {
    uintptr_t node=*(uintptr_t*)(pawn+off::m_pGameSceneNode);
    if(!ptr_sane(node)) return;
    uintptr_t ms=*(uintptr_t*)(node+off::m_modelState);
    if(!ptr_sane(ms)) return;
    uintptr_t ba=pick_bone_array(ms);
    if(!ba) return;
    static const int conn[14][2]={
        {0,5},{5,7},{7,8},{8,9},{9,10},{7,11},{11,12},{12,13},
        {5,14},{14,15},{15,16},{5,17},{17,18},{18,19}
    };
    const float* bones=(const float*)ba;
    for(int i=0;i<14;i++){
        int a=conn[i][0], b=conn[i][1];
        if(a>=64||b>=64) continue;
        Vec3 pa={bones[a*12+3],bones[a*12+7],bones[a*12+11]};
        Vec3 pb={bones[b*12+3],bones[b*12+7],bones[b*12+11]};
        if(!world_pos_plausible(pa)||!world_pos_plausible(pb)) continue;
        Vec2 sa,sb;
        if(world_to_screen(pa,sa,vm,sw,sh)&&world_to_screen(pb,sb,vm,sw,sh))
            gfx::draw_line(sa.x,sa.y,sb.x,sb.y,col);
    }
}

void esp_render(float sw,float sh) {
    if(!g_client) return;
    uintptr_t ent_sys=*(uintptr_t*)(g_client+off::dwEntityList);
    uintptr_t local  =*(uintptr_t*)(g_client+off::dwLocalPlayerPawn);
    if(!ptr_sane(ent_sys)||!ptr_sane(local)) {
        if(g_diag<3){ g_diag++; vlog("esp: no ent_sys/local (%p / %p)",(void*)ent_sys,(void*)local); }
        return;
    }
    ViewMatrix vm=*(ViewMatrix*)(g_client+off::dwViewMatrix);
    uintptr_t* ident=(uintptr_t*)(ent_sys+off::ENTITY_IDENTITIES);
    uint8_t local_team=*(uint8_t*)(local+off::m_iTeamNum);
    if(local_team!=2&&local_team!=3) return;

    int found=0, drawn=0, null_run=0;
    for(int i=0;i<off::MAX_ENTITIES;i++){
        uintptr_t identity=ident[i];
        if(!ptr_sane(identity)){ if(++null_run>512) break; continue; }
        null_run=0;
        uintptr_t pawn=resolve_instance(identity,local);
        if(!pawn) continue;
        found++;
        int hp=*(int*)(pawn+off::m_iHealth);
        uint8_t team=*(uint8_t*)(pawn+off::m_iTeamNum);
        if(g_cfg.teamcheck&&team==local_team) continue;
        uintptr_t node=*(uintptr_t*)(pawn+off::m_pGameSceneNode);
        if(!ptr_sane(node)) continue;
        Vec3 origin;
        if(!read_origin(node,origin)) continue;

        Vec3 head_w={origin.x,origin.y,origin.z+72.f};
        Vec2 head_s,feet_s;
        if(!world_to_screen(head_w,head_s,vm,sw,sh)) continue;
        if(!world_to_screen(origin,feet_s,vm,sw,sh)) continue;
        float h=feet_s.y-head_s.y;
        if(h<4.f||h>sh*2.f) continue;
        float w=h*0.4f, bx=head_s.x-w*0.5f;
        drawn++;
        uint32_t col=(team==local_team)?rgba(60,200,60,255):rgba(255,60,60,255);
        if(g_cfg.box){
            gfx::draw_rect(bx-1,head_s.y-1,w+2,h+2,rgba(0,0,0,200));
            gfx::draw_rect(bx,head_s.y,w,h,col);
        }
        if(g_cfg.health){
            float pct=(hp>100)?1.f:hp/100.f;
            gfx::draw_rect_filled(bx-6.f,head_s.y,4.f,h,rgba(0,0,0,180));
            gfx::draw_rect_filled(bx-6.f,head_s.y+h-h*pct,4.f,h*pct,
                rgba((uint8_t)(255-255*pct),(uint8_t)(255*pct),0,255));
        }
        if(g_cfg.name){
            char name[32]; memset(name,0,sizeof(name));
            uint32_t hctl=*(uint32_t*)(pawn+off::m_hOriginalController);
            uint32_t cidx=hctl&0x7FFF;
            if(cidx<(uint32_t)off::MAX_ENTITIES){
                uintptr_t cident=ident[cidx];
                if(ptr_sane(cident)){
                    static const uintptr_t cc[4]={0x0,0x70,0x78,0x80};
                    for(int k=0;k<4&&name[0]==0;k++){
                        uintptr_t ctl=*(uintptr_t*)(cident+cc[k]);
                        if(!controller_ok(ctl)) continue;
                        const char* nm=(const char*)(ctl+off::m_iszPlayerName);
                        if(nm[0]>=0x20&&nm[0]<=0x7E){
                            int n=0;
                            while(n<31&&nm[n]>=0x20&&nm[n]<=0x7E){name[n]=nm[n];n++;}
                        }
                    }
                }
            }
            if(name[0]) gfx::draw_text(head_s.x,head_s.y-18.f,rgba(255,255,255,255),true,"%s [%d]",name,hp);
            else        gfx::draw_text(head_s.x,head_s.y-18.f,rgba(255,255,255,255),true,"HP: %d",hp);
        }
        if(g_cfg.skeleton) draw_skeleton(pawn,vm,sw,sh,rgba(255,255,255,220));
    }
    if(g_diag<5){ g_diag++; vlog("esp: pawns=%d drawn=%d team=%u",found,drawn,local_team); }
}

// ---------------------------------------------------------------------------
static DWORD WINAPI init_thread(LPVOID) {
    vlog("=== void: thread start ===");
    while(!g_client){
        g_client=(uintptr_t)GetModuleHandleA("client.dll");
        if(!g_client) Sleep(200);
    }
    vlog("void: client.dll=%p",(void*)g_client);
    Sleep(3000);
    menu::install();
    hk::install();
    for(;;){
        if(GetAsyncKeyState(VK_END)&1) break;
        if(!hk::g_hooked){ hk::install(); Sleep(1000); }
        else Sleep(10);
    }
    g_unloading=true;
    hk::remove();
    gfx::shutdown();
    Sleep(300);
    vlog("void: thread exit");
    ExitThread(0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) CreateThread(nullptr,0,init_thread,nullptr,0,nullptr);
    else if(reason==DLL_PROCESS_DETACH){ g_unloading=true; hk::remove(); gfx::shutdown(); }
    return TRUE;
}
