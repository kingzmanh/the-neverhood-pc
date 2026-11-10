/* DirectDraw replacement: IDirectDraw, IDirectDrawSurface, IDirectDrawPalette.
 *
 * The game asks for 640x480x8 exclusive mode, a primary surface with one
 * back buffer, a palette, and offscreen surfaces it BltFasts (colour keyed)
 * onto the back buffer, then Flips.  Here every surface is plain guest
 * memory; Flip / drawing into the primary hands the front buffer and the
 * palette to the presenter (window.c).  COM objects live in guest memory:
 * [vtable][object id]; the vtables hold native thunks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_NOTFOUND 0x887600ffu

typedef struct Surf {
    u32 obj;               /* guest COM object */
    int w, h, pitch;
    u32 mem;               /* guest pixels */
    u32 caps;
    int ck_on;
    u32 ck_lo, ck_hi;
    struct Surf *back;     /* primary: attached back buffer */
    struct Pal *pal;
    int primary, refs;
} Surf;

typedef struct Pal { u32 obj; u32 rgb[256]; int refs; } Pal;

#define MAXS 512
static Surf *surfs[MAXS];
static Pal *pals[64];
static u32 vt_dd, vt_surf, vt_pal;
static Surf *primary;
static u32 obj_dd;

static u32 cur_pal[256];

static Surf *S(u32 obj) { u32 i = RD32(obj + 4); return (i < MAXS && surfs[i] && surfs[i]->obj == obj) ? surfs[i] : NULL; }
static Pal *P(u32 obj) { u32 i = RD32(obj + 4); return (i < 64 && pals[i] && pals[i]->obj == obj) ? pals[i] : NULL; }

static void present(void)
{
    if (!primary) return;
    if (primary->pal) memcpy(cur_pal, primary->pal->rgb, sizeof cur_pal);
    present_frame(MEM + primary->mem, primary->pitch, cur_pal);
}

/* frame pacing: a Flip with DDFLIP_WAIT waited for the 60 Hz vertical blank */
static void vsync_wait(void)
{
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    s64 period = freq.QuadPart / 60;
    if (last.QuadPart && now.QuadPart - last.QuadPart < period) {
        s64 left = period - (now.QuadPart - last.QuadPart);
        DWORD ms = (DWORD)(left * 1000 / freq.QuadPart);
        if (ms > 1) Sleep(ms - 1);
        do QueryPerformanceCounter(&now); while (now.QuadPart - last.QuadPart < period);
        last.QuadPart += period;
        if (now.QuadPart - last.QuadPart > period) last = now;
    } else last = now;
}

static Surf *surf_new(int w, int h, u32 caps)
{
    int i = 1;
    while (i < MAXS && surfs[i]) i++;
    if (i == MAXS) rt_fatal("out of DirectDraw surfaces");
    Surf *s = calloc(1, sizeof *s);
    s->w = w;
    s->h = h;
    s->pitch = (w + 3) & ~3;
    s->mem = gmalloc(s->pitch * h);
    s->caps = caps;
    s->refs = 1;
    s->obj = gmalloc(8);
    WR32(s->obj, vt_surf);
    WR32(s->obj + 4, i);
    surfs[i] = s;
    return s;
}

/* ---------------- IDirectDraw ---------------- */
static void dd_QueryInterface(void) { WR32(ARG(2), ARG(0)); RET(DD_OK, 3); }
static void dd_AddRef(void) { RET(2, 1); }
static void dd_Release(void) { RET(0, 1); }
static void dd_SetCooperativeLevel(void) { rt_log("ddraw: SetCooperativeLevel %x\n", ARG(2)); RET(DD_OK, 3); }
static void dd_SetDisplayMode(void) { rt_log("ddraw: SetDisplayMode %ux%ux%u\n", ARG(1), ARG(2), ARG(3)); RET(DD_OK, 4); }
static void dd_RestoreDisplayMode(void) { RET(DD_OK, 1); }
static void dd_WaitForVerticalBlank(void) { vsync_wait(); RET(DD_OK, 3); }
static void dd_FlipToGDISurface(void) { RET(DD_OK, 1); }
static void dd_GetAvailableVidMem(void) { if (ARG(2)) WR32(ARG(2), 8 << 20); if (ARG(3)) WR32(ARG(3), 8 << 20); RET(DD_OK, 4); }
static void dd_GetVerticalBlankStatus(void) { WR32(ARG(1), 0); RET(DD_OK, 2); }

static void dd_GetCaps(void)
{
    /* DDCAPS: dwSize, dwCaps (blt, colour key, palette) ... */
    for (int k = 1; k <= 2; k++) {
        u32 c = ARG(k);
        if (!c) continue;
        u32 n = RD32(c);
        if (n > 4) memset(MEM + c + 4, 0, n - 4);
        WR32(c + 4, 0x00000040u | 0x00400000u | 0x00000400u);  /* DDCAPS_BLT | DDCAPS_COLORKEY | DDCAPS_PALETTE? */
        WR32(c + 0x3c, 8 << 20);   /* dwVidMemTotal */
        WR32(c + 0x40, 8 << 20);   /* dwVidMemFree */
    }
    RET(DD_OK, 3);
}

static void dd_GetDisplayMode(void)
{
    u32 d = ARG(1);
    memset(MEM + d + 4, 0, 0x68);
    WR32(d + 4, 0x100f);
    WR32(d + 8, 480); WR32(d + 12, 640); WR32(d + 16, 640);
    WR32(d + 0x48, 32); WR32(d + 0x4c, 0x60); WR32(d + 0x54, 8);
    RET(DD_OK, 2);
}

/* CreateSurface(this, desc, &surface, outer) */
static void dd_CreateSurface(void)
{
    u32 d = ARG(1);
    u32 flags = RD32(d + 4), caps = RD32(d + 0x68);
    Surf *s;
    if (caps & 0x200) {   /* DDSCAPS_PRIMARYSURFACE */
        s = surf_new(640, 480, caps | 0x2000 | 0x4000 | 0x8000);
        s->primary = 1;
        primary = s;
        if ((flags & 0x20) && RD32(d + 0x14)) {   /* back buffer */
            s->back = surf_new(640, 480, 0x4 | 0x4000);
        }
        rt_log("ddraw: primary surface%s\n", s->back ? " + back buffer" : "");
    } else {
        int w = (flags & 4) ? (int)RD32(d + 12) : 640, h = (flags & 2) ? (int)RD32(d + 8) : 480;
        if (!(caps & 0x800)) caps |= 0x4000;   /* in "video memory" unless asked for system memory */
        s = surf_new(w, h, caps);
    }
    WR32(ARG(2), s->obj);
    RET(DD_OK, 4);
}

/* CreatePalette(this, flags, entries, &pal, outer) */
static void dd_CreatePalette(void)
{
    int i = 1;
    while (i < 64 && pals[i]) i++;
    Pal *p = calloc(1, sizeof *p);
    p->refs = 1;
    p->obj = gmalloc(8);
    WR32(p->obj, vt_pal);
    WR32(p->obj + 4, i);
    pals[i] = p;
    u32 e = ARG(2);
    if (e) for (int k = 0; k < 256; k++) p->rgb[k] = RD32(e + 4 * k) & 0xffffff;
    WR32(ARG(3), p->obj);
    RET(DD_OK, 5);
}

static void dd_unsup1(void) { RET(DDERR_UNSUPPORTED, 1); }
static void dd_unsup2(void) { RET(DDERR_UNSUPPORTED, 2); }
static void dd_unsup3(void) { RET(DDERR_UNSUPPORTED, 3); }
static void dd_unsup4(void) { RET(DDERR_UNSUPPORTED, 4); }
static void dd_unsup5(void) { RET(DDERR_UNSUPPORTED, 5); }
static void dd_unsup6(void) { RET(DDERR_UNSUPPORTED, 6); }

/* ---------------- IDirectDrawSurface ---------------- */
static void s_QueryInterface(void) { Surf *s = S(ARG(0)); if (s) s->refs++; WR32(ARG(2), ARG(0)); RET(DD_OK, 3); }
static void s_AddRef(void) { Surf *s = S(ARG(0)); if (s) s->refs++; RET(s ? s->refs : 0, 1); }
static void s_Release(void)
{
    Surf *s = S(ARG(0));
    if (!s) RET(0, 1);
    if (--s->refs > 0) RET(s->refs, 1);
    u32 i = RD32(s->obj + 4);
    if (s == primary) primary = NULL;
    if (s->back) { surfs[RD32(s->back->obj + 4)] = NULL; gfree(s->back->mem); gfree(s->back->obj); free(s->back); }
    surfs[i] = NULL;
    gfree(s->mem);
    gfree(s->obj);
    free(s);
    RET(0, 1);
}

static int key_match(Surf *s, u8 c) { return c >= s->ck_lo && c <= s->ck_hi; }

static void blit(Surf *d, int dx, int dy, Surf *s, int sx, int sy, int w, int h, int keyed)
{
    /* clip to both surfaces */
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (sx + w > s->w) w = s->w - sx;
    if (sy + h > s->h) h = s->h - sy;
    if (dx + w > d->w) w = d->w - dx;
    if (dy + h > d->h) h = d->h - dy;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        u8 *dp = MEM + d->mem + (dy + y) * d->pitch + dx;
        const u8 *sp = MEM + s->mem + (sy + y) * s->pitch + sx;
        if (!keyed) memmove(dp, sp, w);
        else for (int x = 0; x < w; x++) if (!key_match(s, sp[x])) dp[x] = sp[x];
    }
}

static void read_rect(u32 r, int *x, int *y, int *w, int *h, Surf *s)
{
    if (r) { *x = (s32)RD32(r); *y = (s32)RD32(r + 4); *w = (s32)RD32(r + 8) - *x; *h = (s32)RD32(r + 12) - *y; }
    else { *x = 0; *y = 0; *w = s->w; *h = s->h; }
}

/* BltFast(this, x, y, src, srcrect, trans) */
static void s_BltFast(void)
{
    Surf *d = S(ARG(0)), *s = S(ARG(3));
    if (!d || !s) RET(DDERR_INVALIDPARAMS, 6);
    int sx, sy, w, h;
    read_rect(ARG(4), &sx, &sy, &w, &h, s);
    blit(d, (s32)ARG(1), (s32)ARG(2), s, sx, sy, w, h, (ARG(5) & 1) && s->ck_on);
    if (d == primary) present();
    RET(DD_OK, 6);
}

/* Blt(this, dstrect, src, srcrect, flags, fx) */
static void s_Blt(void)
{
    Surf *d = S(ARG(0));
    if (!d) RET(DDERR_INVALIDPARAMS, 6);
    u32 flags = ARG(4);
    int dx, dy, dw, dh;
    read_rect(ARG(1), &dx, &dy, &dw, &dh, d);
    if (flags & 0x400) {   /* DDBLT_COLORFILL */
        u8 c = (u8)RD32(ARG(5) + 0x50);
        if (dx < 0) { dw += dx; dx = 0; }
        if (dy < 0) { dh += dy; dy = 0; }
        if (dx + dw > d->w) dw = d->w - dx;
        if (dy + dh > d->h) dh = d->h - dy;
        for (int y = 0; y < dh; y++) memset(MEM + d->mem + (dy + y) * d->pitch + dx, c, dw > 0 ? dw : 0);
    } else {
        Surf *s = S(ARG(2));
        if (!s) RET(DDERR_INVALIDPARAMS, 6);
        int sx, sy, sw, sh;
        read_rect(ARG(3), &sx, &sy, &sw, &sh, s);
        int keyed = (flags & 0x8000) && s->ck_on;
        if (sw == dw && sh == dh) blit(d, dx, dy, s, sx, sy, sw, sh, keyed);
        else {   /* stretch, nearest */
            for (int y = 0; y < dh; y++) {
                int ty = dy + y, fy = sy + y * sh / dh;
                if (ty < 0 || ty >= d->h || fy < 0 || fy >= s->h) continue;
                for (int x = 0; x < dw; x++) {
                    int tx = dx + x, fx = sx + x * sw / dw;
                    if (tx < 0 || tx >= d->w || fx < 0 || fx >= s->w) continue;
                    u8 c = RD8(s->mem + fy * s->pitch + fx);
                    if (!keyed || !key_match(s, c)) WR8(d->mem + ty * d->pitch + tx, c);
                }
            }
        }
    }
    if (d == primary) present();
    RET(DD_OK, 6);
}

static void s_Flip(void)
{
    Surf *s = S(ARG(0));
    if (s && s->back) {
        u32 m = s->mem;
        s->mem = s->back->mem;
        s->back->mem = m;
        if (!(ARG(2) & 0x8)) vsync_wait();   /* not DDFLIP_NOVSYNC */
        present();
    }
    RET(DD_OK, 3);
}

static void s_GetAttachedSurface(void)
{
    Surf *s = S(ARG(0));
    if (!s || !s->back) RET(DDERR_NOTFOUND, 3);
    s->back->refs++;
    WR32(ARG(2), s->back->obj);
    RET(DD_OK, 3);
}

static void s_GetCaps(void) { Surf *s = S(ARG(0)); WR32(ARG(1), s ? s->caps : 0); RET(DD_OK, 2); }

static void fill_desc(Surf *s, u32 d, u32 ptr)
{
    memset(MEM + d + 4, 0, 0x68);
    WR32(d, 0x6c);
    WR32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (ptr ? 0x800 : 0));
    WR32(d + 8, s->h);
    WR32(d + 12, s->w);
    WR32(d + 16, s->pitch);
    WR32(d + 0x24, ptr);
    if (s->ck_on) { WR32(d + 0x40, s->ck_lo); WR32(d + 0x44, s->ck_hi); WR32(d + 4, RD32(d + 4) | 0x10000); }
    WR32(d + 0x48, 32);
    WR32(d + 0x4c, 0x60);   /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
    WR32(d + 0x54, 8);
    WR32(d + 0x68, s->caps);
}

static void s_GetSurfaceDesc(void) { Surf *s = S(ARG(0)); if (!s) RET(DDERR_INVALIDPARAMS, 2); fill_desc(s, ARG(1), 0); RET(DD_OK, 2); }

static void s_GetPixelFormat(void)
{
    u32 p = ARG(1);
    memset(MEM + p, 0, 32);
    WR32(p, 32); WR32(p + 4, 0x60); WR32(p + 12, 8);
    RET(DD_OK, 2);
}

/* Lock(this, rect, desc, flags, event) */
static void s_Lock(void)
{
    Surf *s = S(ARG(0));
    if (!s) RET(DDERR_INVALIDPARAMS, 5);
    u32 p = s->mem;
    if (ARG(1)) p += (s32)RD32(ARG(1) + 4) * s->pitch + (s32)RD32(ARG(1));
    fill_desc(s, ARG(2), p);
    RET(DD_OK, 5);
}

static void s_Unlock(void)
{
    Surf *s = S(ARG(0));
    if (s == primary) present();
    RET(DD_OK, 2);
}

/* SetColorKey(this, flags, key) */
static void s_SetColorKey(void)
{
    Surf *s = S(ARG(0));
    if (s && (ARG(1) & 8)) {   /* DDCKEY_SRCBLT */
        if (ARG(2)) { s->ck_on = 1; s->ck_lo = RD32(ARG(2)) & 0xff; s->ck_hi = RD32(ARG(2) + 4) & 0xff; if (s->ck_hi < s->ck_lo) s->ck_hi = s->ck_lo; }
        else s->ck_on = 0;
    }
    RET(DD_OK, 3);
}

static void s_GetColorKey(void)
{
    Surf *s = S(ARG(0));
    if (!s || !s->ck_on) RET(0x887600e2u, 3);   /* DDERR_NOCOLORKEY */
    WR32(ARG(2), s->ck_lo);
    WR32(ARG(2) + 4, s->ck_hi);
    RET(DD_OK, 3);
}

static void s_SetPalette(void)
{
    Surf *s = S(ARG(0));
    Pal *p = ARG(1) ? P(ARG(1)) : NULL;
    if (s) {
        s->pal = p;
        if (p) p->refs++;
        if (s == primary && p) memcpy(cur_pal, p->rgb, sizeof cur_pal);
    }
    RET(DD_OK, 2);
}

static void s_GetPalette(void)
{
    Surf *s = S(ARG(0));
    if (!s || !s->pal) RET(0x887600e9u, 2);   /* DDERR_NOPALETTEATTACHED */
    s->pal->refs++;
    WR32(ARG(1), s->pal->obj);
    RET(DD_OK, 2);
}

static void s_IsLost(void) { RET(DD_OK, 1); }
static void s_Restore(void) { RET(DD_OK, 1); }
static void s_GetBltStatus(void) { RET(DD_OK, 2); }
static void s_GetFlipStatus(void) { RET(DD_OK, 2); }
static void s_GetDC(void) { RET(DDERR_UNSUPPORTED, 2); }
static void s_ReleaseDC(void) { RET(DD_OK, 2); }
static void s_SetClipper(void) { RET(DD_OK, 2); }
static void s_PageLock(void) { RET(DD_OK, 2); }
static void s_GetDDInterface(void) { WR32(ARG(1), obj_dd); RET(DD_OK, 2); }

/* ---------------- IDirectDrawPalette ---------------- */
static void p_AddRef(void) { Pal *p = P(ARG(0)); if (p) p->refs++; RET(p ? p->refs : 0, 1); }
static void p_Release(void) { Pal *p = P(ARG(0)); if (p && p->refs > 0) p->refs--; RET(p ? p->refs : 0, 1); }
static void p_GetCaps(void) { WR32(ARG(1), 0x4 | 0x40); RET(DD_OK, 2); }   /* 8BIT | ALLOW256 */

/* SetEntries(this, flags, start, count, entries) */
static void p_SetEntries(void)
{
    Pal *p = P(ARG(0));
    if (!p) RET(DDERR_INVALIDPARAMS, 5);
    u32 st = ARG(2), n = ARG(3), e = ARG(4);
    for (u32 k = 0; k < n && st + k < 256; k++) p->rgb[st + k] = RD32(e + 4 * k) & 0xffffff;
    if (primary && primary->pal == p) present();   /* palette fades show without a flip */
    RET(DD_OK, 5);
}

static void p_GetEntries(void)
{
    Pal *p = P(ARG(0));
    if (!p) RET(DDERR_INVALIDPARAMS, 5);
    u32 st = ARG(2), n = ARG(3), e = ARG(4);
    for (u32 k = 0; k < n && st + k < 256; k++) WR32(e + 4 * k, p->rgb[st + k]);
    RET(DD_OK, 5);
}

/* ---------------- vtables ---------------- */
static u32 make_vt(const NativeFn *fns, const char **names, int n)
{
    u32 vt = gmalloc(4 * n);
    for (int i = 0; i < n; i++) WR32(vt + 4 * i, thunk_new(fns[i], names[i]));
    return vt;
}

static void init_vtables(void)
{
    static const NativeFn dd[] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_unsup1, dd_unsup4, dd_CreatePalette,
        dd_CreateSurface, dd_unsup3, dd_unsup5, dd_unsup5, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode,
        dd_unsup3, dd_unsup2, dd_unsup2, dd_unsup2, dd_GetVerticalBlankStatus, dd_unsup2, dd_RestoreDisplayMode,
        dd_SetCooperativeLevel, dd_SetDisplayMode, dd_WaitForVerticalBlank, dd_GetAvailableVidMem };
    static const char *ddn[] = { "IDirectDraw::QueryInterface", "IDirectDraw::AddRef", "IDirectDraw::Release",
        "IDirectDraw::Compact", "IDirectDraw::CreateClipper", "IDirectDraw::CreatePalette", "IDirectDraw::CreateSurface",
        "IDirectDraw::DuplicateSurface", "IDirectDraw::EnumDisplayModes", "IDirectDraw::EnumSurfaces",
        "IDirectDraw::FlipToGDISurface", "IDirectDraw::GetCaps", "IDirectDraw::GetDisplayMode",
        "IDirectDraw::GetFourCCCodes", "IDirectDraw::GetGDISurface", "IDirectDraw::GetMonitorFrequency",
        "IDirectDraw::GetScanLine", "IDirectDraw::GetVerticalBlankStatus", "IDirectDraw::Initialize",
        "IDirectDraw::RestoreDisplayMode", "IDirectDraw::SetCooperativeLevel", "IDirectDraw::SetDisplayMode",
        "IDirectDraw::WaitForVerticalBlank", "IDirectDraw2::GetAvailableVidMem" };
    vt_dd = make_vt(dd, ddn, 24);
    static const NativeFn sf[] = { s_QueryInterface, s_AddRef, s_Release, dd_unsup2, dd_unsup2, s_Blt, dd_unsup4,
        s_BltFast, dd_unsup3, dd_unsup3, dd_unsup4, s_Flip, s_GetAttachedSurface, s_GetBltStatus, s_GetCaps,
        dd_unsup2, s_GetColorKey, s_GetDC, s_GetFlipStatus, dd_unsup3, s_GetPalette, s_GetPixelFormat,
        s_GetSurfaceDesc, dd_unsup3, s_IsLost, s_Lock, s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey,
        dd_unsup3, s_SetPalette, s_Unlock, dd_unsup6, dd_unsup2, dd_unsup3,
        s_GetDDInterface, s_PageLock, s_PageLock, dd_unsup3 };
    static const char *sfn[] = { "Surface::QueryInterface", "Surface::AddRef", "Surface::Release",
        "Surface::AddAttachedSurface", "Surface::AddOverlayDirtyRect", "Surface::Blt", "Surface::BltBatch",
        "Surface::BltFast", "Surface::DeleteAttachedSurface", "Surface::EnumAttachedSurfaces",
        "Surface::EnumOverlayZOrders", "Surface::Flip", "Surface::GetAttachedSurface", "Surface::GetBltStatus",
        "Surface::GetCaps", "Surface::GetClipper", "Surface::GetColorKey", "Surface::GetDC", "Surface::GetFlipStatus",
        "Surface::GetOverlayPosition", "Surface::GetPalette", "Surface::GetPixelFormat", "Surface::GetSurfaceDesc",
        "Surface::Initialize", "Surface::IsLost", "Surface::Lock", "Surface::ReleaseDC", "Surface::Restore",
        "Surface::SetClipper", "Surface::SetColorKey", "Surface::SetOverlayPosition", "Surface::SetPalette",
        "Surface::Unlock", "Surface::UpdateOverlay", "Surface::UpdateOverlayDisplay", "Surface::UpdateOverlayZOrder",
        "Surface2::GetDDInterface", "Surface2::PageLock", "Surface2::PageUnlock", "Surface3::SetSurfaceDesc" };
    vt_surf = make_vt(sf, sfn, 40);
    static const NativeFn pf[] = { dd_QueryInterface, p_AddRef, p_Release, p_GetCaps, p_GetEntries, dd_unsup4, p_SetEntries };
    static const char *pfn[] = { "Palette::QueryInterface", "Palette::AddRef", "Palette::Release", "Palette::GetCaps",
        "Palette::GetEntries", "Palette::Initialize", "Palette::SetEntries" };
    vt_pal = make_vt(pf, pfn, 7);
}

/* DirectDrawCreate(guid, lplpDD, outer) */
void api_DirectDrawCreate(void)
{
    if (!vt_dd) init_vtables();
    if (!obj_dd) {
        obj_dd = gmalloc(8);
        WR32(obj_dd, vt_dd);
    }
    WR32(ARG(1), obj_dd);
    rt_log("DirectDrawCreate\n");
    RET(DD_OK, 3);
}
