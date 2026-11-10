/* The game's window, its message queue, input and presentation.
 *
 * The game registers a window class and runs a classic PeekMessage /
 * GetMessage loop; its window procedure (guest code) gets the messages a
 * Windows 95 window would: WM_ACTIVATEAPP, keys, WM_CHAR, mouse in 640x480
 * coordinates.  The real window here is ours: resizable, the 640x480
 * picture scaled into it at 4:3.
 *
 * Test switches: NH_HIDDEN=1 (no window shown, no dialogs), NH_KEYS=
 * "ms:vk[:hold];..." (virtual-key presses), NH_CLICK="x,y,ms;..." (left
 * clicks at game coordinates), NH_SHOT="ms;ms..." (frames -> shots\),
 * NH_QUIT=ms (close after that time). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

u32 g_hwnd = 0x00010010u;
u32 g_wndproc;
int quit_requested;
static HWND hwnd;
static int hidden, cursor_hidden, game_cursor_hidden, in_menu;
static u32 wstyle = 0x90080000u;
static int guest_cursor_count = 0;
static u8 keys[256];
static u32 t0;

/* ---------------- message queue ---------------- */
typedef struct { u32 hwnd, msg, wp, lp, time, x, y; } GMsg;
#define QN 512
static GMsg q[QN];
static int qh, qt;
static int quit_posted, quit_code;

void msgq_post(u32 h, u32 msg, u32 wp, u32 lp)
{
    if ((qt + 1) % QN == qh) return;
    q[qt] = (GMsg){ h, msg, wp, lp, now_ms(), 0, 0 };
    qt = (qt + 1) % QN;
}

void msgq_quit(int code) { quit_posted = 1; quit_code = code; }

/* ---------------- scripted input / screenshots (tests) ---------------- */
typedef struct { u32 at, kind, a, b, hold; int done; } Ev;
static Ev evs[256];
static int nev;
static u32 shots[64];
static int nshots, shot_next;
static u32 quit_at;

static void script_init(void)
{
    const char *k = getenv(ENV("KEYS"));
    while (k && *k && nev < 256) {
        unsigned ms, vk, hold = 100;
        int used = 0;
        if (sscanf(k, "%u:%x%n", &ms, &vk, &used) < 2) break;
        k += used;
        if (*k == ':') { sscanf(k + 1, "%u%n", &hold, &used); k += 1 + used; }
        evs[nev++] = (Ev){ ms, 0, vk, 0, hold, 0 };
        if (*k == ';') k++;
    }
    const char *c = getenv(ENV("CLICK"));
    while (c && *c && nev < 256) {
        unsigned x, y, ms;
        int used = 0;
        if (sscanf(c, "%u,%u,%u%n", &x, &y, &ms, &used) < 3) break;
        c += used;
        evs[nev++] = (Ev){ ms, 1, x, y, 80, 0 };
        if (*c == ';') c++;
    }
    const char *s = getenv(ENV("SHOT"));
    while (s && *s && nshots < 64) {
        shots[nshots++] = (u32)strtoul(s, (char **)&s, 10);
        if (*s == ';' || *s == ',') s++; else break;
    }
    if (getenv(ENV("QUIT"))) quit_at = (u32)atoi(getenv(ENV("QUIT")));
}

static void key_event(u32 vk, int down)
{
    keys[vk & 0xff] = down ? 0x80 : 0;
    msgq_post(g_hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, down ? 1 : 0xc0000001u);
    if (down) {
        u32 ch = 0;
        if (vk >= 'A' && vk <= 'Z') ch = vk + 32;
        else if ((vk >= '0' && vk <= '9') || vk == ' ' || vk == 13 || vk == 8 || vk == 27) ch = vk;
        if (ch) msgq_post(g_hwnd, WM_CHAR, ch, 1);
    }
}

static void script_poll(void)
{
    u32 t = now_ms() - t0;
    for (int i = 0; i < nev; i++) {
        Ev *e = &evs[i];
        if (e->done == 2) continue;
        if (!e->done && t >= e->at) {
            e->done = 1;
            if (e->kind == 0) key_event(e->a, 1);
            else {
                u32 lp = e->a | (e->b << 16);
                msgq_post(g_hwnd, WM_MOUSEMOVE, 0, lp);
                msgq_post(g_hwnd, WM_LBUTTONDOWN, 1, lp);
            }
        } else if (e->done == 1 && t >= e->at + e->hold) {
            e->done = 2;
            if (e->kind == 0) key_event(e->a, 0);
            else msgq_post(g_hwnd, WM_LBUTTONUP, 0, e->a | (e->b << 16));
        }
    }
    if (quit_at && t >= quit_at) { rt_log("test quit at %u ms\n", t); reg_save(); exit(0); }
}

/* ---------------- presentation (present.c: Direct3D 9; GDI fallback here) ---------------- */
int present_init(HWND h);
int present_rgb(const u8 *pix, const u32 *pal);
void present_dest_rect(int cw, int ch, int *dx, int *dy, int *dw, int *dh);
int present_menu_command(int id);
void present_set_fullscreen(int on);
void present_fps(double fps);
void settings_load(void);
extern int opt_fullscreen;
static int d3d_ok;
static u8 frame[640 * 480];
static u32 frame_pal[256];
static struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bmi;
static int have_frame;

static void dest_rect(int cw, int ch, int *x, int *y, int *w, int *h) { present_dest_rect(cw, ch, x, y, w, h); }

static void paint(HDC dc)
{
    if (!have_frame) return;
    if (d3d_ok && present_rgb(frame, frame_pal)) return;
    RECT r;
    GetClientRect(hwnd, &r);
    int x, y, w, h;
    dest_rect(r.right, r.bottom, &x, &y, &w, &h);
    bmi.h.biSize = sizeof bmi.h;
    bmi.h.biWidth = 640;
    bmi.h.biHeight = -480;
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = 8;
    bmi.h.biCompression = BI_RGB;
    bmi.h.biClrUsed = 256;
    for (int i = 0; i < 256; i++) {
        u32 c = frame_pal[i];
        bmi.c[i].rgbRed = c & 0xff; bmi.c[i].rgbGreen = (c >> 8) & 0xff; bmi.c[i].rgbBlue = (c >> 16) & 0xff;
    }
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, w, h, 0, 0, 640, 480, frame, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
    HBRUSH b = GetStockObject(BLACK_BRUSH);
    RECT bars[4] = { {0, 0, x, r.bottom}, {x + w, 0, r.right, r.bottom}, {x, 0, x + w, y}, {x, y + h, x + w, r.bottom} };
    for (int i = 0; i < 4; i++) if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top) FillRect(dc, &bars[i], b);
}

void present_shot(const char *path);
static void save_shot(u32 t)
{
    char dir[700], p[760];
    snprintf(dir, sizeof dir, "%s\\shots", rt_exe_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(p, sizeof p, "%s\\shot_%06u.bmp", dir, t);
    if (getenv(ENV("SHOTOUT")) && d3d_ok) {
        present_rgb(frame, frame_pal);
        present_shot(p);
        rt_log("shot %s (presented)\n", p);
        return;
    }
    FILE *f = fopen(p, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = { 0x4d42, sizeof fh + 40 + 1024 + 640 * 480, 0, 0, sizeof fh + 40 + 1024 };
    BITMAPINFOHEADER ih = { 40, 640, 480, 1, 8, 0, 640 * 480, 0, 0, 256, 0 };
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, 40, 1, f);
    for (int i = 0; i < 256; i++) { u8 q4[4] = { (u8)(frame_pal[i] >> 16), (u8)(frame_pal[i] >> 8), (u8)frame_pal[i], 0 }; fwrite(q4, 4, 1, f); }
    for (int y = 479; y >= 0; y--) fwrite(frame + y * 640, 640, 1, f);
    fclose(f);
    rt_log("shot %s\n", p);
}

static int frames;
void present_frame(const u8 *pix, int pitch, const u32 *pal)
{
    for (int y = 0; y < 480; y++) memcpy(frame + y * 640, pix + y * pitch, 640);
    memcpy(frame_pal, pal, sizeof frame_pal);
    have_frame = 1;
    frames++;
    u32 t = now_ms() - t0;
    while (shot_next < nshots && t >= shots[shot_next]) { save_shot(shots[shot_next]); shot_next++; }
    if (hwnd && !hidden && !IsIconic(hwnd)) {
        if (!(d3d_ok && present_rgb(frame, frame_pal))) {
            HDC dc = GetDC(hwnd);
            paint(dc);
            ReleaseDC(hwnd, dc);
        }
    }
    static u32 last_fps;
    static int last_frames;
    if (t - last_fps >= 5000) {
        rt_log("fps %.1f\n", (frames - last_frames) * 1000.0 / (t - last_fps));
        present_fps((frames - last_frames) * 1000.0 / (t - last_fps));
        last_fps = t;
        last_frames = frames;
    }
}

/* ---------------- host window ---------------- */
static int map_mouse(LPARAM lp, u32 *out)
{
    RECT r;
    GetClientRect(hwnd, &r);
    int x, y, w, h;
    dest_rect(r.right, r.bottom, &x, &y, &w, &h);
    int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
    int gx = w ? (mx - x) * 640 / w : 0, gy = h ? (my - y) * 480 / h : 0;
    if (gx < 0) gx = 0;
    if (gx > 639) gx = 639;
    if (gy < 0) gy = 0;
    if (gy > 479) gy = 479;
    *out = (u32)gx | ((u32)gy << 16);
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    u32 glp;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: msgq_post(g_hwnd, WM_CLOSE, 0, 0); return 0;
    case WM_ACTIVATEAPP:   /* the game pauses when it loses focus (hidden test runs: never) */
        if (!hidden) msgq_post(g_hwnd, WM_ACTIVATEAPP, wp ? 1 : 0, 0);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { present_set_fullscreen(!opt_fullscreen); return 0; }   /* Alt+Enter */
        break;
    case WM_SYSCHAR: if (wp == VK_RETURN) return 0; break;
    case WM_COMMAND: if (present_menu_command(LOWORD(wp))) { InvalidateRect(h, NULL, FALSE); return 0; } break;
    case WM_KEYDOWN: keys[wp & 0xff] = 0x80; msgq_post(g_hwnd, WM_KEYDOWN, (u32)wp, (u32)lp); return 0;
    case WM_KEYUP: keys[wp & 0xff] = 0; msgq_post(g_hwnd, WM_KEYUP, (u32)wp, (u32)lp); return 0;
    case WM_CHAR: msgq_post(g_hwnd, WM_CHAR, (u32)wp, (u32)lp); return 0;
    case WM_MOUSEMOVE: map_mouse(lp, &glp); msgq_post(g_hwnd, WM_MOUSEMOVE, (u32)wp, glp); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        map_mouse(lp, &glp);
        if (m == WM_LBUTTONDOWN) SetCapture(h);
        if (m == WM_LBUTTONUP) ReleaseCapture();
        msgq_post(g_hwnd, m, (u32)wp, glp);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && cursor_hidden) { SetCursor(NULL); return TRUE; }
        break;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_ENTERMENULOOP: in_menu = 1; win_cursor(1); break;
    case WM_EXITMENULOOP: in_menu = 0; win_cursor(!game_cursor_hidden); break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

void win_create(u32 wndproc, const char *title)
{
    g_wndproc = wndproc;
    hidden = getenv(ENV("HIDDEN")) != NULL;
    t0 = now_ms();
    script_init();
    settings_load();
    WNDCLASSA wc = { 0 };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = host_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "NeverhoodPC";
    RegisterClassA(&wc);
    /* default size: the largest whole multiple of 640x480 that fits the work area */
    RECT wa;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    int k = 1;
    while (640 * (k + 1) <= wa.right - wa.left - 40 && 480 * (k + 1) <= wa.bottom - wa.top - 80) k++;
    RECT r = { 0, 0, 640 * k, 480 * k };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    hwnd = CreateWindowExA(0, "NeverhoodPC", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    rt_log("window %dx%d (x%d)%s\n", 640 * k, 480 * k, k, hidden ? " hidden" : "");
    d3d_ok = present_init(hwnd);
    int ww, wh;   /* NH_WINSIZE=WxH: client size for test runs / screenshots */
    const char *ws = getenv(ENV("WINSIZE"));
    if (ws && sscanf(ws, "%dx%d", &ww, &wh) == 2) {
        RECT c = { 0, 0, ww, wh };
        AdjustWindowRect(&c, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), GetMenu(hwnd) != NULL);
        SetWindowPos(hwnd, NULL, 0, 0, c.right - c.left, c.bottom - c.top, SWP_NOMOVE | SWP_NOZORDER);
    }
}

int win_pump(int wait)
{
    MSG m;
    if (wait && qh == qt && !quit_posted) MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT);
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    script_poll();
    timers_poll();
    return !quit_posted;
}

void win_cursor(int show)
{
    if (!in_menu) game_cursor_hidden = !show;
    cursor_hidden = in_menu ? 0 : !show;
    if (hwnd) {
        POINT p;
        GetCursorPos(&p);
        SetCursorPos(p.x, p.y);   /* re-evaluates WM_SETCURSOR */
    }
}

u32 win_key_state(int vk) { return keys[vk & 0xff]; }

/* ---------------- USER32 natives ---------------- */
static void write_msg(u32 a, GMsg *m)
{
    WR32(a, m->hwnd); WR32(a + 4, m->msg); WR32(a + 8, m->wp); WR32(a + 12, m->lp);
    WR32(a + 16, m->time); WR32(a + 20, m->x); WR32(a + 24, m->y);
}

int msgq_get(u32 msg_addr, u32 h, u32 lo, u32 hi, int remove)
{
    (void)h; (void)lo; (void)hi;
    if (qh != qt) {
        write_msg(msg_addr, &q[qh]);
        if (remove) qh = (qh + 1) % QN;
        return 1;
    }
    if (quit_posted) {
        GMsg m = { 0, WM_QUIT, (u32)quit_code, 0, now_ms(), 0, 0 };
        write_msg(msg_addr, &m);
        return 1;
    }
    return 0;
}

/* GetMessageA(msg, hwnd, min, max) */
void api_GetMessageA(void)
{
    for (;;) {
        win_pump(0);
        if (msgq_get(ARG(0), ARG(1), ARG(2), ARG(3), 1)) RET(RD32(ARG(0) + 4) != WM_QUIT, 4);
        win_pump(1);
    }
}

/* PeekMessageA(msg, hwnd, min, max, remove) */
void api_PeekMessageA(void)
{
    win_pump(0);
    RET(msgq_get(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4) & 1), 5);
}

void api_TranslateMessage(void) { RET(0, 1); }   /* WM_CHAR comes from the host window */

void api_DispatchMessageA(void)
{
    u32 m = ARG(0);
    u32 r = 0;
    if (g_wndproc && RD32(m + 4) != WM_QUIT)
        r = guest_call(g_wndproc, 4, RD32(m), RD32(m + 4), RD32(m + 8), RD32(m + 12));
    RET(r, 1);
}

void api_SendMessageA(void)
{
    u32 r = g_wndproc ? guest_call(g_wndproc, 4, ARG(0), ARG(1), ARG(2), ARG(3)) : 0;
    RET(r, 4);
}

void api_CallWindowProcA(void)
{
    u32 r = guest_call(ARG(0), 4, ARG(1), ARG(2), ARG(3), ARG(4));
    RET(r, 5);
}

void api_DefWindowProcA(void)
{
    u32 msg = ARG(1);
    if (msg == WM_CLOSE) guest_call(g_wndproc, 4, g_hwnd, WM_DESTROY, 0, 0);
    RET(0, 4);
}

void api_PostQuitMessage(void) { rt_log("PostQuitMessage(%d)\n", ARG(0)); msgq_quit((int)ARG(0)); RET(0, 1); }

void api_RegisterClassA(void)
{
    u32 wc = ARG(0);
    g_wndproc = RD32(wc + 4);
    rt_log("RegisterClassA %s wndproc %08x\n", gstr(RD32(wc + 36)), g_wndproc);
    RET(0xc001, 1);
}

/* CreateWindowExA(ex, class, title, style, x, y, w, h, parent, menu, inst, param) */
void api_CreateWindowExA(void)
{
    wstyle = ARG(3);
    if (!hwnd) win_create(g_wndproc, GAME_TITLE);
    guest_call(g_wndproc, 4, g_hwnd, WM_CREATE, 0, 0);
    RET(g_hwnd, 12);
}

void api_ShowWindow(void)
{
    /* only the first show counts: later the game minimizes its exclusive-mode
       window when it loses focus (SW_SHOWMINNOACTIVE), which our window
       must not follow */
    int cmd = (int)ARG(1);
    static int shown;
    if (cmd && !shown) {
        shown = 1;
        if (hwnd && !hidden) { ShowWindow(hwnd, SW_SHOWNORMAL); SetForegroundWindow(hwnd); }
        msgq_post(g_hwnd, WM_ACTIVATEAPP, 1, 0);
    }
    RET(1, 2);
}

void api_DestroyWindow(void)
{
    rt_log("DestroyWindow\n");
    guest_call(g_wndproc, 4, g_hwnd, WM_DESTROY, 0, 0);
    RET(1, 1);
}

void api_SetActiveWindow(void) { RET(g_hwnd, 1); }
void api_InvalidateRect(void) { RET(1, 3); }
void api_FindWindowA(void) { RET(0, 2); }
void api_RegisterWindowMessageA(void) { RET(0xc100, 1); }
void api_SetFocus(void) { RET(g_hwnd, 1); }
void api_UpdateWindow(void) { RET(1, 1); }
void api_LoadIconA(void) { RET(0x1001, 2); }
void api_LoadCursorA(void) { RET(0x1002, 2); }
void api_SetCursor(void) { win_cursor(ARG(0) != 0 && guest_cursor_count >= 0); RET(0x1002, 1); }
void api_ShowCursor(void) { guest_cursor_count += ARG(0) ? 1 : -1; win_cursor(guest_cursor_count >= 0); RET(guest_cursor_count, 1); }
void api_IsIconic(void) { RET(hwnd && IsIconic(hwnd), 1); }
void api_GetKeyState(void) { u32 s = keys[ARG(0) & 0xff]; RET(s ? 0xff80u : 0, 1); }
static u32 wlong_proc_sub;
void api_GetWindowLongA(void)
{
    s32 i = (s32)ARG(1);
    RET(i == -16 ? wstyle : i == -4 ? g_wndproc : i == -20 ? 8 : 0, 2);
}
void api_SetWindowLongA(void)
{
    s32 i = (s32)ARG(1);
    u32 old = 0;
    if (i == -16) { old = wstyle; wstyle = ARG(2); }
    else if (i == -4) { old = g_wndproc; g_wndproc = ARG(2); wlong_proc_sub = 1; rt_log("window procedure now %08x\n", g_wndproc); }
    RET(old, 3);
}
void api_ClientToScreen(void) { RET(1, 2); }
void api_GetActiveWindow(void) { RET(g_hwnd, 0); }
void api_GetLastActivePopup(void) { RET(ARG(0), 1); }
void api_GetClassInfoA(void) { RET(0, 3); }
void api_GetDC(void) { RET(0x2001, 1); }
void api_ReleaseDC(void) { RET(1, 2); }
void api_GetFocus(void) { RET(g_hwnd, 0); }
void api_GetTopWindow(void) { RET(g_hwnd, 1); }
void api_GetWindowThreadProcessId(void) { if (ARG(1)) WR32(ARG(1), 0x40); RET(0x44, 2); }
void api_GetWindow(void) { RET(0, 2); }
void api_IsWindow(void) { RET(ARG(0) == g_hwnd, 1); }
