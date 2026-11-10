/* Presentation: the game's 640x480 8-bit screen through Direct3D 9.
 *
 * Each frame: palette -> 32-bit texture, optional FXAA, upscale to the
 * window with the chosen filter (nearest, bilinear, Catmull-Rom bicubic,
 * sharp bilinear, xBR), optional contrast-adaptive sharpening - the same
 * post chain as our other ports (Dos tools kit).  Options are in the
 * window's menu bar (Video / View / Help) and neverhood.ini [video]. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "rt.h"

int opt_fullscreen = 1, opt_filter = 2, opt_sharpen = 1, opt_fxaa = 0, opt_aspect = 0, opt_showfps = 0;
/* Resolution: 0 = upscale straight to the window; 1-4 = upscale to N x
   640x480 first (the filter works at a whole multiple), then fit that
   picture to the window.  The game's art is 640x480: no extra detail. */
int opt_scale = 0;
static char ini_path[700];
static HWND hwnd;
static IDirect3D9 *d3d;
static IDirect3DDevice9 *dev;
static D3DPRESENT_PARAMETERS pp;
static IDirect3DTexture9 *src_tex, *aa_tex, *up_tex;
static IDirect3DSurface9 *aa_surf, *up_surf;
static UINT up_w, up_h;
static IDirect3DVertexShader9 *vs_blit;
static IDirect3DPixelShader9 *ps_copy, *ps_bicubic, *ps_sharpbil, *ps_fxaa, *ps_sharpen, *ps_over, *ps_xbr;
static IDirect3DVertexDeclaration9 *blit_decl;
static int d3d_ok;

typedef HRESULT (WINAPI *D3DCompile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, void **, void **);
typedef struct { void **vt; } Blob;

static void *compile(const char *src, const char *profile, SIZE_T *n)
{
    static D3DCompile_t comp;
    if (!comp) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (!m) m = LoadLibraryA("d3dcompiler_43.dll");
        if (m) comp = (D3DCompile_t)(void *)GetProcAddress(m, "D3DCompile");
        if (!comp) { rt_log("video: no HLSL compiler (d3dcompiler_47.dll)\n"); return NULL; }
    }
    Blob *code = NULL, *err = NULL;
    if (FAILED(comp(src, strlen(src), "post", NULL, NULL, "main", profile, 0, 0, (void **)&code, (void **)&err))) {
        if (err) { rt_log("video: shader error: %s\n", (const char *)((void *(WINAPI *)(void *))err->vt[3])(err)); ((ULONG(WINAPI *)(void *))err->vt[2])(err); }
        return NULL;
    }
    if (err) ((ULONG(WINAPI *)(void *))err->vt[2])(err);
    *n = ((SIZE_T(WINAPI *)(void *))code->vt[4])(code);
    void *p = malloc(*n);
    memcpy(p, ((void *(WINAPI *)(void *))code->vt[3])(code), *n);
    ((ULONG(WINAPI *)(void *))code->vt[2])(code);
    return p;
}

static const char *VS_BLIT =
    "float4 hp : register(c0);\n"
    "struct V { float4 p : POSITION; float2 t : TEXCOORD0; };\n"
    "V main(float4 p : POSITION, float2 t : TEXCOORD0) { V o; o.p = float4(p.xy + hp.xy, 0, 1); o.t = t; return o; }\n";
/* edge-directed pixel-art upscaler (own implementation of the xBR level-2
   rules: corner/edge detection on luma in a 5x5 neighbourhood, 30/45/60
   degree edges redrawn as smooth lines, blended over about one output
   pixel) - for 2D screens, fonts and the game's sprites */
static const char *PS_XBR =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);   // w, h, 1/w, 1/h of the source\n"
    "float4 k : register(c2);    // x = output pixels per source pixel\n"
    "static const float3 Y = float3(0.2126, 0.7152, 0.0722);\n"
    "static const float4 Ao = float4(1.0, -1.0, -1.0, 1.0);\n"
    "static const float4 Bo = float4(1.0, 1.0, -1.0, -1.0);\n"
    "static const float4 Co = float4(1.5, 0.5, -0.5, 0.5);\n"
    "static const float4 Ax = float4(1.0, -1.0, -1.0, 1.0);\n"
    "static const float4 Bx = float4(0.5, 2.0, -0.5, -2.0);\n"
    "static const float4 Cx = float4(1.0, 1.0, -0.5, 0.0);\n"
    "static const float4 Ay = float4(1.0, -1.0, -1.0, 1.0);\n"
    "static const float4 By = float4(2.0, 0.5, -2.0, -0.5);\n"
    "static const float4 Cy = float4(2.0, 0.0, -1.0, 0.5);\n"
    "float4 df(float4 a, float4 b) { return abs(a - b); }\n"
    "float cdf(float3 a, float3 b) { float3 d = abs(a - b); return d.r + d.g + d.b; }\n"
    "float4 neq(float4 a, float4 b) { return step(0.002, abs(a - b)); }\n"
    "float4 eqv(float4 a, float4 b) { return 1.0 - step(0.06, abs(a - b)); }\n"
    "float4 wdist(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h)\n"
    "{\n"
    "    return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0 * df(g, h);\n"
    "}\n"
    "float3 T(float2 c, float x, float y) { return tex2D(s, c + float2(x, y) * ts.zw).rgb; }\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float2 p = uv * ts.xy;\n"
    "    float2 fp = frac(p);\n"
    "    float2 c = (floor(p) + 0.5) * ts.zw;\n"
    "    float3 A1 = T(c, -1, -2), B1 = T(c, 0, -2), C1 = T(c, 1, -2);\n"
    "    float3 A0 = T(c, -2, -1), A = T(c, -1, -1), B = T(c, 0, -1), C = T(c, 1, -1), C4 = T(c, 2, -1);\n"
    "    float3 D0 = T(c, -2, 0), D = T(c, -1, 0), E = T(c, 0, 0), F = T(c, 1, 0), F4 = T(c, 2, 0);\n"
    "    float3 G0 = T(c, -2, 1), G = T(c, -1, 1), H = T(c, 0, 1), I = T(c, 1, 1), I4 = T(c, 2, 1);\n"
    "    float3 G5 = T(c, -1, 2), H5 = T(c, 0, 2), I5 = T(c, 1, 2);\n"
    "    float4 b = float4(dot(B, Y), dot(D, Y), dot(H, Y), dot(F, Y));\n"
    "    float4 cc = float4(dot(C, Y), dot(A, Y), dot(G, Y), dot(I, Y));\n"
    "    float ee = dot(E, Y);\n"
    "    float4 e = float4(ee, ee, ee, ee);\n"
    "    float4 d = b.yzwx, f = b.wxyz, g = cc.zwxy, h = b.zwxy, i = cc.wxyz;\n"
    "    float4 i4 = float4(dot(I4, Y), dot(C1, Y), dot(A0, Y), dot(G5, Y));\n"
    "    float4 i5 = float4(dot(I5, Y), dot(C4, Y), dot(A1, Y), dot(G0, Y));\n"
    "    float4 h5 = float4(dot(H5, Y), dot(F4, Y), dot(B1, Y), dot(D0, Y));\n"
    "    float4 f4 = h5.yzwx;\n"
    "    float4 fx = Ao * fp.y + Bo * fp.x;\n"
    "    float4 fxl = Ax * fp.y + Bx * fp.x;\n"
    "    float4 fxu = Ay * fp.y + By * fp.x;\n"
    "    float4 lv0 = neq(e, f) * neq(e, h);\n"
    "    float4 lv1 = lv0 * saturate((1 - eqv(f, b)) * (1 - eqv(h, d)) + eqv(e, i) * (1 - eqv(f, i4)) * (1 - eqv(h, i5)) + eqv(e, g) + eqv(e, cc));\n"
    "    float4 lv2l = neq(e, g) * neq(d, g);\n"
    "    float4 lv2u = neq(e, cc) * neq(b, cc);\n"
    "    float dl = clamp(1.0 / k.x, 0.08, 0.5);   // blend width: about one output pixel\n"
    "    float4 delta = float4(dl, dl, dl, dl);\n"
    "    float4 fx45 = saturate((fx + delta - Co) / (2 * delta));\n"
    "    float4 fx30 = saturate((fxl + delta - Cx) / (2 * delta));\n"
    "    float4 fx60 = saturate((fxu + delta - Cy) / (2 * delta));\n"
    "    float4 wd1 = wdist(e, cc, g, i, h5, f4, h, f);\n"
    "    float4 wd2 = wdist(h, d, i5, f, i4, b, e, i);\n"
    "    float4 edr = step(wd1, wd2) * (1 - step(wd2, wd1)) * lv1;   // wd1 < wd2\n"
    "    float4 edrl = step(2.0 * df(f, g), df(h, cc)) * lv2l * edr;\n"
    "    float4 edru = step(2.0 * df(h, cc), df(f, g)) * lv2u * edr;\n"
    "    fx45 *= edr; fx30 *= edrl; fx60 *= edru;\n"
    "    float4 px = step(df(e, f), df(e, h));\n"
    "    float4 mx = max(max(fx30, fx60), fx45);\n"
    "    float3 r1 = lerp(E, lerp(H, F, px.x), mx.x);\n"
    "    r1 = lerp(r1, lerp(B, D, px.z), mx.z);\n"
    "    float3 r2 = lerp(E, lerp(F, B, px.y), mx.y);\n"
    "    r2 = lerp(r2, lerp(D, H, px.w), mx.w);\n"
    "    float3 r = lerp(r1, r2, step(cdf(E, r1), cdf(E, r2)));\n"
    "    return float4(r, 1);\n"
    "}\n";
static const char *PS_OVER =
    "sampler2D s : register(s0);\n"
    "float4 main(float2 t : TEXCOORD0) : COLOR { return tex2D(s, t); }\n";
static const char *PS_COPY =
    "sampler2D s : register(s0);\n"
    "float4 main(float2 t : TEXCOORD0) : COLOR { return float4(tex2D(s, t).rgb, 1); }\n";
/* Catmull-Rom bicubic in 9 bilinear taps */
static const char *PS_BICUBIC =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 sp = uv * ts.xy, t1 = floor(sp - 0.5) + 0.5, f = sp - t1;\n"
    "  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
    "  float2 w12 = w1 + w2, o12 = w2 / w12;\n"
    "  float2 p0 = (t1 - 1) * ts.zw, p3 = (t1 + 2) * ts.zw, p12 = (t1 + o12) * ts.zw;\n"
    "  float3 r = 0;\n"
    "  r += tex2D(s, float2(p0.x, p0.y)).rgb * w0.x * w0.y;  r += tex2D(s, float2(p12.x, p0.y)).rgb * w12.x * w0.y;  r += tex2D(s, float2(p3.x, p0.y)).rgb * w3.x * w0.y;\n"
    "  r += tex2D(s, float2(p0.x, p12.y)).rgb * w0.x * w12.y; r += tex2D(s, float2(p12.x, p12.y)).rgb * w12.x * w12.y; r += tex2D(s, float2(p3.x, p12.y)).rgb * w3.x * w12.y;\n"
    "  r += tex2D(s, float2(p0.x, p3.y)).rgb * w0.x * w3.y;  r += tex2D(s, float2(p12.x, p3.y)).rgb * w12.x * w3.y;  r += tex2D(s, float2(p3.x, p3.y)).rgb * w3.x * w3.y;\n"
    "  return float4(saturate(r), 1);\n"
    "}\n";
/* sharp bilinear: nearest inside each source pixel, a one-output-pixel
   bilinear seam at its edges (crisp pixels without uneven sizes) */
static const char *PS_SHARPBIL =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);\n"
    "float4 k : register(c2);   // xy = output pixels per source pixel\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 t = uv * ts.xy, fl = floor(t), c = t - fl - 0.5;\n"
    "  float2 r = 0.5 - 0.5 / k.xy;\n"
    "  float2 f = (c - clamp(c, -r, r)) * k.xy + 0.5;\n"
    "  return float4(tex2D(s, (fl + f) * ts.zw).rgb, 1);\n"
    "}\n";
static const char *PS_FXAA =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 rcp = ts.zw;\n"
    "  float3 nw = tex2D(s, uv + float2(-1, -1) * rcp).rgb, ne = tex2D(s, uv + float2(1, -1) * rcp).rgb;\n"
    "  float3 sw = tex2D(s, uv + float2(-1, 1) * rcp).rgb, se = tex2D(s, uv + float2(1, 1) * rcp).rgb, m = tex2D(s, uv).rgb;\n"
    "  float3 L = float3(0.299, 0.587, 0.114);\n"
    "  float lnw = dot(nw, L), lne = dot(ne, L), lsw = dot(sw, L), lse = dot(se, L), lm = dot(m, L);\n"
    "  float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
    "  float2 dir = float2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
    "  float red = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "  float rmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);\n"
    "  dir = clamp(dir * rmin, -8.0, 8.0) * rcp;\n"
    "  float3 a = 0.5 * (tex2D(s, uv + dir * (1.0 / 3.0 - 0.5)).rgb + tex2D(s, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "  float3 b = a * 0.5 + 0.25 * (tex2D(s, uv - dir * 0.5).rgb + tex2D(s, uv + dir * 0.5).rgb);\n"
    "  float lb = dot(b, L);\n"
    "  return float4((lb < lmin || lb > lmax) ? a : b, 1);\n"
    "}\n";
/* contrast-adaptive sharpening (own implementation of the CAS idea) */
static const char *PS_SHARPEN =
    "sampler2D s : register(s0);\n"
    "float4 ts : register(c1);\n"
    "float4 k : register(c2);   // x = sharpness 0..1\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float2 d = ts.zw;\n"
    "  float3 c = tex2D(s, uv).rgb, n = tex2D(s, uv + float2(0, -d.y)).rgb, so = tex2D(s, uv + float2(0, d.y)).rgb;\n"
    "  float3 e = tex2D(s, uv + float2(d.x, 0)).rgb, w = tex2D(s, uv + float2(-d.x, 0)).rgb;\n"
    "  float3 mn = min(c, min(min(n, so), min(e, w))), mx = max(c, max(max(n, so), max(e, w)));\n"
    "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));\n"
    "  float3 wt = -amp / lerp(8.0, 5.0, k.x);\n"
    "  return float4(saturate((c + (n + so + e + w) * wt) / (1.0 + 4.0 * wt)), 1);\n"
    "}\n";

static IDirect3DPixelShader9 *mk_ps(const char *src)
{
    SIZE_T n;
    void *bc = compile(src, "ps_3_0", &n);
    IDirect3DPixelShader9 *ps = NULL;
    if (bc) { IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)bc, &ps); free(bc); }
    return ps;
}

static void release_targets(void)
{
    if (src_tex) { IDirect3DTexture9_Release(src_tex); src_tex = NULL; }
    if (aa_surf) { IDirect3DSurface9_Release(aa_surf); aa_surf = NULL; }
    if (aa_tex) { IDirect3DTexture9_Release(aa_tex); aa_tex = NULL; }
    if (up_surf) { IDirect3DSurface9_Release(up_surf); up_surf = NULL; }
    if (up_tex) { IDirect3DTexture9_Release(up_tex); up_tex = NULL; }
    up_w = up_h = 0;
}

static void desktop_size(UINT *w, UINT *h)
{
    *w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (*w < 640) *w = 640;
    if (*h < 480) *h = 480;
}

static int d3d_init(void)
{
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return 0;
    memset(&pp, 0, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    desktop_size(&pp.BackBufferWidth, &pp.BackBufferHeight);
    pp.hDeviceWindow = hwnd;
    /* the game paces itself (Flip waits like the 60 Hz vertical blank): no blocking Present */
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT h1 = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev), h2 = h1;
    if (FAILED(h1)) h2 = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev);
    if (FAILED(h2)) {
        static int nlog;
        if (nlog++ < 3) rt_log("video: CreateDevice failed %08lx / %08lx (back buffer %ux%u)\n", h1, h2, pp.BackBufferWidth, pp.BackBufferHeight);
        IDirect3D9_Release(d3d);
        d3d = NULL;
        return 0;
    }
    SIZE_T n;
    void *bc = compile(VS_BLIT, "vs_3_0", &n);
    if (bc) { IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)bc, &vs_blit); free(bc); }
    ps_copy = mk_ps(PS_COPY);
    ps_bicubic = mk_ps(PS_BICUBIC);
    ps_sharpbil = mk_ps(PS_SHARPBIL);
    ps_fxaa = mk_ps(PS_FXAA);
    ps_sharpen = mk_ps(PS_SHARPEN);
    ps_over = mk_ps(PS_OVER);
    ps_xbr = mk_ps(PS_XBR);
    (void)ps_over;
    D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_POSITION, 0},
                              {0, 8, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
    IDirect3DDevice9_CreateVertexDeclaration(dev, el, &blit_decl);
    if (!vs_blit || !ps_copy) rt_log("video: post shaders unavailable, plain scaling only\n");
    rt_log("video: Direct3D 9, back buffer %ux%u\n", pp.BackBufferWidth, pp.BackBufferHeight);
    return 1;
}

static int ensure_targets(UINT ow, UINT oh)
{
    if (!src_tex) {
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, 640, 480, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &src_tex, NULL))) return 0;
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 640, 480, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &aa_tex, NULL)))
            IDirect3DTexture9_GetSurfaceLevel(aa_tex, 0, &aa_surf);
    }
    if (ow != up_w || oh != up_h) {
        if (up_surf) { IDirect3DSurface9_Release(up_surf); up_surf = NULL; }
        if (up_tex) { IDirect3DTexture9_Release(up_tex); up_tex = NULL; }
        if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, ow, oh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &up_tex, NULL)))
            IDirect3DTexture9_GetSurfaceLevel(up_tex, 0, &up_surf);
        up_w = ow; up_h = oh;
    }
    return 1;
}

static void blit(IDirect3DTexture9 *src, UINT sw, UINT sh, IDirect3DPixelShader9 *ps, D3DVIEWPORT9 vp, int linear)
{
    IDirect3DDevice9_SetViewport(dev, &vp);
    float hp[4] = {-1.0f / vp.Width, 1.0f / vp.Height, 0, 0};
    float ts[4] = {(float)sw, (float)sh, 1.0f / sw, 1.0f / sh};
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, hp, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, ts, 1);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)src);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    float q[4][4] = {{-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, -1, 1, 1}};
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, 16);
}

/* where the 640x480 picture goes in a cw x ch client area */
void present_dest_rect(int cw, int ch, int *dx, int *dy, int *dw, int *dh)
{
    *dx = 0; *dy = 0; *dw = cw; *dh = ch;
    if (opt_aspect == 0) {
        if (cw * 3 > ch * 4) { *dw = ch * 4 / 3; *dx = (cw - *dw) / 2; }
        else { *dh = cw * 3 / 4; *dy = (ch - *dh) / 2; }
    } else if (opt_aspect == 2) {
        int k = cw / 640 < ch / 480 ? cw / 640 : ch / 480;
        if (k < 1) k = 1;
        *dw = 640 * k; *dh = 480 * k;
        if (*dw > cw) *dw = cw;
        if (*dh > ch) *dh = ch;
        *dx = (cw - *dw) / 2; *dy = (ch - *dh) / 2;
    }
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

static u32 rgb[640 * 480];

/* returns 0 when Direct3D is not available (window.c falls back to GDI) */
int present_rgb(const u8 *pix, const u32 *pal)
{
    if (!d3d_ok) {
        /* the device can be unavailable for a while (another program in
           exclusive fullscreen): try again every 2 seconds, GDI meanwhile */
        static u32 last_try;
        u32 t = GetTickCount();
        if (t - last_try < 2000) return 0;
        last_try = t;
        if (!(d3d_ok = d3d_init())) return 0;
    }
    for (int i = 0; i < 640 * 480; i++) {
        u32 c = pal[pix[i]];
        rgb[i] = ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
    }
    HRESULT hr = IDirect3DDevice9_TestCooperativeLevel(dev);
    if (hr == D3DERR_DEVICELOST) return 1;
    if (hr == D3DERR_DEVICENOTRESET) {
        release_targets();
        desktop_size(&pp.BackBufferWidth, &pp.BackBufferHeight);
        if (FAILED(IDirect3DDevice9_Reset(dev, &pp))) return 1;
    }
    RECT cr;
    GetClientRect(hwnd, &cr);
    UINT cw = cr.right > 0 ? (UINT)cr.right : 1, ch = cr.bottom > 0 ? (UINT)cr.bottom : 1;
    if (cw > pp.BackBufferWidth || ch > pp.BackBufferHeight) {
        release_targets();
        desktop_size(&pp.BackBufferWidth, &pp.BackBufferHeight);
        if (cw > pp.BackBufferWidth) pp.BackBufferWidth = cw;
        if (ch > pp.BackBufferHeight) pp.BackBufferHeight = ch;
        if (FAILED(IDirect3DDevice9_Reset(dev, &pp))) return 1;
    }
    int dx, dy, dw, dh;
    present_dest_rect(cw, ch, &dx, &dy, &dw, &dh);
    int uw = opt_scale ? 640 * opt_scale : dw, uh = opt_scale ? 480 * opt_scale : dh;
    if (!ensure_targets(uw, uh)) return 1;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(src_tex, 0, &lr, NULL, D3DLOCK_DISCARD))) {
        for (int y = 0; y < 480; y++) memcpy((u8 *)lr.pBits + y * lr.Pitch, rgb + y * 640, 640 * 4);
        IDirect3DTexture9_UnlockRect(src_tex, 0);
    }
    IDirect3DSurface9 *bb = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return 1;
    IDirect3DDevice9_BeginScene(dev);
    D3DRECT all = {0, 0, (LONG)cw, (LONG)ch};
    if (vs_blit && ps_copy && up_surf) {
        IDirect3DDevice9_SetVertexShader(dev, vs_blit);
        IDirect3DDevice9_SetVertexDeclaration(dev, blit_decl);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DTexture9 *cur = src_tex;
        D3DVIEWPORT9 vs = {0, 0, 640, 480, 0, 1};
        if (opt_fxaa && ps_fxaa && aa_surf) {
            IDirect3DDevice9_SetRenderTarget(dev, 0, aa_surf);
            blit(cur, 640, 480, ps_fxaa, vs, 1);
            cur = aa_tex;
        }
        IDirect3DDevice9_SetRenderTarget(dev, 0, up_surf);
        D3DVIEWPORT9 vu = {0, 0, (DWORD)uw, (DWORD)uh, 0, 1};
        float k[4] = {(float)uw / 640, (float)uh / 480, 0, 0};
        int fit = uw != dw || uh != dh;   /* the upscaled picture is fitted to the window (linear) */
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 2, k, 1);
        if (opt_filter == 4 && ps_xbr) blit(cur, 640, 480, ps_xbr, vu, 0);
        else if (opt_filter == 2 && ps_bicubic) blit(cur, 640, 480, ps_bicubic, vu, 1);
        else if (opt_filter == 3 && ps_sharpbil) blit(cur, 640, 480, ps_sharpbil, vu, 1);
        else blit(cur, 640, 480, ps_copy, vu, opt_filter != 0);
        IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
        IDirect3DDevice9_Clear(dev, 1, &all, D3DCLEAR_TARGET, 0, 1, 0);
        D3DVIEWPORT9 vd = {(DWORD)dx, (DWORD)dy, (DWORD)dw, (DWORD)dh, 0, 1};
        if (opt_sharpen && ps_sharpen) {
            float s[4] = {opt_sharpen == 1 ? 0.3f : opt_sharpen == 2 ? 0.6f : 1.0f, 0, 0, 0};
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 2, s, 1);
            blit(up_tex, uw, uh, ps_sharpen, vd, fit);
        } else blit(up_tex, uw, uh, ps_copy, vd, fit);
    } else {
        IDirect3DDevice9_Clear(dev, 1, &all, D3DCLEAR_TARGET, 0, 1, 0);
        IDirect3DSurface9 *s = NULL;
        IDirect3DTexture9_GetSurfaceLevel(src_tex, 0, &s);
        RECT r = {dx, dy, dx + dw, dy + dh};
        IDirect3DDevice9_StretchRect(dev, s, NULL, bb, &r, opt_filter ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        IDirect3DSurface9_Release(s);
    }
    IDirect3DDevice9_EndScene(dev);
    IDirect3DSurface9_Release(bb);
    RECT sr = {0, 0, (LONG)cw, (LONG)ch};
    IDirect3DDevice9_Present(dev, &sr, NULL, NULL, NULL);
    return 1;
}

/* ---------------- settings ---------------- */
static int ini_int(const char *sec, const char *key, int def, int lo, int hi)
{
    int v = GetPrivateProfileIntA(sec, key, def, ini_path);
    return v < lo || v > hi ? def : v;
}

static void put(const char *key, int v)
{
    char s[16];
    snprintf(s, sizeof s, "%d", v);
    WritePrivateProfileStringA("video", key, s, ini_path);
}

void settings_load(void)
{
    snprintf(ini_path, sizeof ini_path, "%s\\" GAME_SHORT ".ini", rt_exe_dir);
    opt_fullscreen = ini_int("video", "Fullscreen", 1, 0, 1);
    opt_filter = ini_int("video", "Filter", 2, 0, 4);
    opt_sharpen = ini_int("video", "Sharpen", 1, 0, 3);
    opt_fxaa = ini_int("video", "FXAA", 0, 0, 1);
    opt_aspect = ini_int("video", "Aspect", 0, 0, 2);
    opt_showfps = ini_int("video", "ShowFPS", 0, 0, 1);
    opt_scale = ini_int("video", "Resolution", 0, 0, 4);
    if (getenv(ENV("SCALE"))) opt_scale = atoi(getenv(ENV("SCALE"))) & 7;
    if (opt_scale > 4) opt_scale = 4;
    if (getenv(ENV("HIDDEN"))) opt_fullscreen = 0;
}

static void settings_save(void)
{
    if (getenv(ENV("HIDDEN"))) return;
    put("Fullscreen", opt_fullscreen);
    put("Filter", opt_filter);
    put("Sharpen", opt_sharpen);
    put("FXAA", opt_fxaa);
    put("Aspect", opt_aspect);
    put("ShowFPS", opt_showfps);
    put("Resolution", opt_scale);
}

/* ---------------- menu bar ---------------- */
enum { ID_FILTER = 1000, ID_SHARP = 1010, ID_FXAA = 1020, ID_ASPECT = 1030, ID_WIN = 1040, ID_FULLSCREEN = 1050,
       ID_SHOWFPS = 1051, ID_CONTROLS = 1060, ID_ABOUT = 1061, ID_SCALE = 1070 };
static HMENU g_menu;
static WINDOWPLACEMENT wpl = { sizeof wpl };

static void update_checks(void)
{
    CheckMenuRadioItem(g_menu, ID_FILTER, ID_FILTER + 4, ID_FILTER + opt_filter, MF_BYCOMMAND);
    CheckMenuRadioItem(g_menu, ID_SHARP, ID_SHARP + 3, ID_SHARP + opt_sharpen, MF_BYCOMMAND);
    CheckMenuRadioItem(g_menu, ID_ASPECT, ID_ASPECT + 2, ID_ASPECT + opt_aspect, MF_BYCOMMAND);
    CheckMenuRadioItem(g_menu, ID_SCALE, ID_SCALE + 4, ID_SCALE + opt_scale, MF_BYCOMMAND);
    CheckMenuItem(g_menu, ID_FXAA, MF_BYCOMMAND | (opt_fxaa ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, ID_FULLSCREEN, MF_BYCOMMAND | (opt_fullscreen ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, ID_SHOWFPS, MF_BYCOMMAND | (opt_showfps ? MF_CHECKED : MF_UNCHECKED));
}

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu(), video = CreatePopupMenu(), asp = CreatePopupMenu(), filt = CreatePopupMenu(),
          sharp = CreatePopupMenu(), view = CreatePopupMenu(), winm = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(filt, MF_STRING, ID_FILTER + 2, L"Bicubic  (smooth and sharp - best for the clay pictures)");
    AppendMenuW(filt, MF_SEPARATOR, 0, NULL);
    AppendMenuW(filt, MF_STRING, ID_FILTER + 0, L"Nearest  (sharp pixels)");
    AppendMenuW(filt, MF_STRING, ID_FILTER + 1, L"Bilinear  (smooth)");
    AppendMenuW(filt, MF_STRING, ID_FILTER + 3, L"Sharp bilinear  (crisp pixels, even sizes)");
    AppendMenuW(filt, MF_STRING, ID_FILTER + 4, L"xBR  (smooth edges, made for pixel art)");
    AppendMenuW(sharp, MF_STRING, ID_SHARP + 0, L"Off");
    AppendMenuW(sharp, MF_STRING, ID_SHARP + 1, L"Low");
    AppendMenuW(sharp, MF_STRING, ID_SHARP + 2, L"Medium");
    AppendMenuW(sharp, MF_STRING, ID_SHARP + 3, L"High");
    AppendMenuW(asp, MF_STRING, ID_ASPECT + 0, L"Keep aspect 4:3  (black bars if the window doesn't match)");
    AppendMenuW(asp, MF_STRING, ID_ASPECT + 1, L"Stretch to window");
    AppendMenuW(asp, MF_STRING, ID_ASPECT + 2, L"Integer scaling  (pixel-exact)");
    HMENU res = CreatePopupMenu();
    AppendMenuW(res, MF_STRING, ID_SCALE + 0, L"Match window  (default)");
    AppendMenuW(res, MF_SEPARATOR, 0, NULL);
    for (int s = 1; s <= 4; s++) {
        wchar_t t[64];
        swprintf(t, 64, L"%dx  (%d x %d)%ls", s, 640 * s, 480 * s, s == 1 ? L"  original" : L"");
        AppendMenuW(res, MF_STRING, ID_SCALE + s, t);
    }
    AppendMenuW(res, MF_SEPARATOR, 0, NULL);
    AppendMenuW(res, MF_STRING | MF_GRAYED, 0, L"The game's pictures and movies are 640x480: this is the size");
    AppendMenuW(res, MF_STRING | MF_GRAYED, 0, L"the Upscale Filter makes them before they fit the screen");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)res, L"&Resolution");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)asp, L"&Aspect Ratio");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)filt, L"Upscale &Filter");
    AppendMenuW(video, MF_STRING, ID_FXAA, L"Anti-aliasing (&FXAA)");
    AppendMenuW(video, MF_POPUP, (UINT_PTR)sharp, L"&Sharpening (contrast adaptive)");
    for (int k = 1; k <= 3; k++) { wchar_t t[32]; swprintf(t, 32, L"%d x %d", 640 * k, 480 * k); AppendMenuW(winm, MF_STRING, ID_WIN + k, t); }
    AppendMenuW(view, MF_POPUP, (UINT_PTR)winm, L"&Window Size");
    AppendMenuW(view, MF_STRING, ID_FULLSCREEN, L"&Fullscreen\tAlt+Enter");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, ID_SHOWFPS, L"Show FPS in title");
    AppendMenuW(help, MF_STRING, ID_CONTROLS, L"&Controls...");
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)video, L"&Video");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"Vie&w");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

void present_set_fullscreen(int on)
{
    opt_fullscreen = on;
    DWORD st = GetWindowLongA(hwnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi = { sizeof mi };
        if (st & WS_CAPTION) GetWindowPlacement(hwnd, &wpl);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetMenu(hwnd, NULL);
        SetWindowLongA(hwnd, GWL_STYLE, (st & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetMenu(hwnd, g_menu);
        if (wpl.rcNormalPosition.right > wpl.rcNormalPosition.left) SetWindowPlacement(hwnd, &wpl);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    settings_save();
    update_checks();
}

static void set_client_size(int w, int h)
{
    if (opt_fullscreen) present_set_fullscreen(0);
    ShowWindow(hwnd, SW_RESTORE);
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), TRUE);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static const char *controls_text =
    "Mouse: point and click - walk, pick up, use.\n"
    "Esc: skip a movie / open the game's Options menu (save, load, quit).\n\n"
    "Alt+Enter: fullscreen / window.\n"
    "The menu bar (Video, View, Help) is shown in window mode.";
static const char *about_text =
    "The Neverhood - PC Native\n\n"
    "The original 1996 game's own code (nhc.exe and the Smacker library)\n"
    "recompiled to run natively on modern Windows: no compatibility mode,\n"
    "no DirectDraw emulation, no registry entries, no disc to mount.\n\n"
    "The Neverhood (c) 1996 The Neverhood, Inc. / DreamWorks Interactive.";

/* returns 1 if the command was ours */
int present_menu_command(int id)
{
    if (id >= ID_FILTER && id <= ID_FILTER + 4) opt_filter = id - ID_FILTER;
    else if (id >= ID_SHARP && id <= ID_SHARP + 3) opt_sharpen = id - ID_SHARP;
    else if (id >= ID_ASPECT && id <= ID_ASPECT + 2) opt_aspect = id - ID_ASPECT;
    else if (id == ID_FXAA) opt_fxaa = !opt_fxaa;
    else if (id >= ID_SCALE && id <= ID_SCALE + 4) opt_scale = id - ID_SCALE;
    else if (id == ID_SHOWFPS) { opt_showfps = !opt_showfps; if (!opt_showfps) SetWindowTextA(hwnd, GAME_TITLE); }
    else if (id > ID_WIN && id <= ID_WIN + 3) { set_client_size(640 * (id - ID_WIN), 480 * (id - ID_WIN)); return 1; }
    else if (id == ID_FULLSCREEN) { present_set_fullscreen(!opt_fullscreen); return 1; }
    else if (id == ID_CONTROLS) { MessageBoxA(hwnd, controls_text, GAME_TITLE " - Controls", MB_ICONINFORMATION); return 1; }
    else if (id == ID_ABOUT) { MessageBoxA(hwnd, about_text, "About", MB_ICONINFORMATION); return 1; }
    else return 0;
    settings_save();
    update_checks();
    return 1;
}

void present_fps(double fps)
{
    if (!opt_showfps || !hwnd) return;
    char t[96];
    snprintf(t, sizeof t, GAME_TITLE " - %.0f fps", fps);
    SetWindowTextA(hwnd, t);
}

int present_init(HWND h)
{
    hwnd = h;
    d3d_ok = d3d_init();
    if (!d3d_ok) rt_log("video: Direct3D 9 unavailable, GDI output\n");
    g_menu = build_menu();
    update_checks();
    if (!getenv(ENV("HIDDEN"))) {
        if (opt_fullscreen) present_set_fullscreen(1);
        else SetMenu(hwnd, g_menu);
    }
    return d3d_ok;
}

/* NH_SHOTOUT test switch: the presented picture (client area) as a 24-bit BMP */
void present_shot(const char *path)
{
    if (!d3d_ok) return;
    RECT cr;
    GetClientRect(hwnd, &cr);
    int w = cr.right, h = cr.bottom;
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, pp.BackBufferWidth, pp.BackBufferHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
            FILE *f = fopen(path, "wb");
            if (f) {
                int stride = (w * 3 + 3) & ~3;
                BITMAPFILEHEADER fh = { 0x4d42, (DWORD)(sizeof fh + 40 + stride * h), 0, 0, sizeof fh + 40 };
                BITMAPINFOHEADER ih = { 40, w, h, 1, 24, 0, (DWORD)(stride * h), 0, 0, 0, 0 };
                fwrite(&fh, sizeof fh, 1, f);
                fwrite(&ih, 40, 1, f);
                u8 *row = calloc(1, stride);
                for (int y = h - 1; y >= 0; y--) {
                    const u8 *s = (const u8 *)lr.pBits + y * lr.Pitch;
                    for (int x = 0; x < w; x++) { row[3 * x] = s[4 * x]; row[3 * x + 1] = s[4 * x + 1]; row[3 * x + 2] = s[4 * x + 2]; }
                    fwrite(row, stride, 1, f);
                }
                free(row);
                fclose(f);
            }
            IDirect3DSurface9_UnlockRect(sys);
        }
    }
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}
