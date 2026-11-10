/* Native replacements for the Windows functions nhc.exe and smackw32.dll
 * import (KERNEL32, USER32, GDI32, WINMM; ADVAPI32 is registry.c, DDRAW
 * ddraw.c, DSOUND dsound.c, windows and messages window.c).
 *
 * Every native reads its arguments from the guest stack (ARG) and returns
 * like the stdcall original (RET pops them).  The game sees a Windows 95
 * machine: C:\NEVERHOOD is the installed game (rt_root\hd), D:\ the CD
 * (rt_root\cd). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rt.h"

void thunk_set(u32 a, NativeFn fn, const char *name);
static u32 last_error;
static u32 str_cmdline, str_env, str_envw;

const char *gstr(u32 a) { return a ? (const char *)MEM + a : NULL; }

u32 now_ms(void) { return timeGetTime(); }

/* ---------------- paths ---------------- */
void path_to_host(const char *g, char *out, size_t n)
{
    char tmp[600];
    const char *p = g;
    const char *base;
    if (isalpha((u8)p[0]) && p[1] == ':') {
        int d = toupper((u8)p[0]);
        p += 2;
        base = d == 'D' ? "cd" : "hd";
        if (d != 'D' && (*p == '\\' || *p == '/') && !_strnicmp(p + 1, "NEVERHOOD", 9) &&
            (p[10] == 0 || p[10] == '\\' || p[10] == '/'))
            p += 10;
    } else if (*p == '\\' || *p == '/') {
        base = "hd";
    } else {
        base = "hd";   /* relative: the current directory is C:\NEVERHOOD */
        snprintf(tmp, sizeof tmp, "\\%s", p);
        p = tmp;
    }
    while (*p == '\\' || *p == '/') p++;
    snprintf(out, n, "%s\\%s\\%s", rt_root, base, p);
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
    size_t l = strlen(out);
    while (l && out[l - 1] == '\\') out[--l] = 0;
}

/* ---------------- handles (files, find, mutex...) ---------------- */
#define MAXF 256
typedef struct { int used, kind; HANDLE h; } GH;   /* kind 1 file, 2 find, 3 other */
static GH ghandles[MAXF];

static u32 gh_new(int kind, HANDLE h)
{
    for (int i = 4; i < MAXF; i++)
        if (!ghandles[i].used) { ghandles[i].used = 1; ghandles[i].kind = kind; ghandles[i].h = h; return 0x100 + i * 4; }
    rt_fatal("out of handles");
    return 0;
}

static GH *gh_get(u32 h)
{
    u32 i = (h - 0x100) / 4;
    if (h < 0x100 || i >= MAXF || !ghandles[i].used) return NULL;
    return &ghandles[i];
}

/* ---------------- KERNEL32 ---------------- */
static void api_GetVersion(void) { RET(0xc0000004u, 0); }   /* Windows 95 */
static void api_GetCommandLineA(void) { RET(str_cmdline, 0); }
static void api_GetEnvironmentStrings(void) { RET(str_env, 0); }
static void api_GetEnvironmentStringsW(void) { RET(str_envw, 0); }
static void api_FreeEnvironmentStringsA(void) { RET(1, 1); }
static void api_FreeEnvironmentStringsW(void) { RET(1, 1); }
static void api_SetEnvironmentVariableA(void) { RET(1, 2); }

static void api_GetStartupInfoA(void)
{
    memset(MEM + ARG(0), 0, 68);
    WR32(ARG(0), 68);
    RET(0, 1);
}

static void api_GetModuleHandleA(void)
{
    const char *n = gstr(ARG(0));
    if (!n) RET(NHC_BASE, 1);
    if (strstr(n, "mack") || strstr(n, "MACK")) RET(SMK_BASE, 1);
    if (!_stricmp(n, "nhc.exe") || !_stricmp(n, "nhc")) RET(NHC_BASE, 1);
    RET(0x7e000000u, 1);
}

static void api_GetModuleFileNameA(void)
{
    const char *s = ARG(0) == SMK_BASE ? "C:\\NEVERHOOD\\SMACKW32.DLL" : "C:\\NEVERHOOD\\NHC.EXE";
    u32 n = ARG(2);
    if (n) {
        strncpy((char *)MEM + ARG(1), s, n);
        WR8(ARG(1) + n - 1, 0);
    }
    RET(strlen(s) < n ? strlen(s) : n, 3);
}

static void api_GetCurrentDirectoryA(void)
{
    const char *s = "C:\\NEVERHOOD";
    if (ARG(0) > strlen(s)) strcpy((char *)MEM + ARG(1), s);
    RET(strlen(s), 2);
}

static void api_GetFullPathNameA(void)
{
    char out[600];
    const char *p = gstr(ARG(0));
    if (isalpha((u8)p[0]) && p[1] == ':') snprintf(out, sizeof out, "%s", p);
    else if (p[0] == '\\') snprintf(out, sizeof out, "C:%s", p);
    else snprintf(out, sizeof out, "C:\\NEVERHOOD\\%s", p);
    u32 n = (u32)strlen(out);
    if (ARG(1) > n) {
        strcpy((char *)MEM + ARG(2), out);
        if (ARG(3)) { char *b = strrchr((char *)MEM + ARG(2), '\\'); WR32(ARG(3), b ? ARG(2) + (u32)(b + 1 - ((char *)MEM + ARG(2))) : 0); }
        RET(n, 4);
    }
    RET(n + 1, 4);
}

static void api_GetDriveTypeA(void)
{
    const char *p = gstr(ARG(0));
    int d = p ? toupper((u8)p[0]) : 'C';
    RET(d == 'C' ? DRIVE_FIXED : d == 'D' ? DRIVE_CDROM : DRIVE_NO_ROOT_DIR, 1);
}

static void api_HeapCreate(void) { RET(0x00ee0000u, 3); }
static void api_HeapAlloc(void) { RET(gmalloc(ARG(2)), 3); }
static void api_HeapFree(void) { gfree(ARG(2)); RET(1, 3); }
static void api_HeapSize(void) { RET(gsize(ARG(2)), 3); }
static void api_HeapReAlloc(void)
{
    u32 p = ARG(2), n = ARG(3);
    if (ARG(1) & 0x10) {   /* HEAP_REALLOC_IN_PLACE_ONLY */
        if (n + 16 <= (1u << RD32(p - 16 + 4))) { WR32(p - 16, n); RET(p, 4); }
        RET(0, 4);
    }
    RET(grealloc(p, n), 4);
}

static void api_LocalAlloc(void) { RET(gmalloc(ARG(1)), 2); }
static void api_LocalFree(void) { gfree(ARG(0)); RET(0, 1); }
static void api_LocalLock(void) { RET(ARG(0), 1); }
static void api_LocalUnlock(void) { RET(0, 1); }
static void api_LocalHandle(void) { RET(ARG(0), 1); }
static void api_LocalSize(void) { RET(gsize(ARG(0)), 1); }
static void api_LocalReAlloc(void) { RET(grealloc(ARG(0), ARG(1)), 3); }

static void api_VirtualAlloc(void)
{
    u32 a = ARG(0), n = ARG(1);
    if (a) RET(a, 4);      /* commit inside a block we handed out: already usable */
    RET(gmalloc(n), 4);
}
static void api_VirtualFree(void) { if (ARG(2) & 0x8000) gfree(ARG(0)); RET(1, 3); }

static void api_GlobalMemoryStatus(void)
{
    u32 m = ARG(0);
    WR32(m, 32);
    WR32(m + 4, 20);
    WR32(m + 8, 256u << 20); WR32(m + 12, 200u << 20);
    WR32(m + 16, 512u << 20); WR32(m + 20, 400u << 20);
    WR32(m + 24, 0x7ffe0000u); WR32(m + 28, 0x70000000u);
    RET(0, 1);
}

static void api_GetACP(void) { RET(1252, 0); }
static void api_GetOEMCP(void) { RET(437, 0); }
static void api_GetCPInfo(void)
{
    u32 p = ARG(1);
    memset(MEM + p, 0, 20);
    WR32(p, 1);
    WR8(p + 4, '?');
    RET(1, 2);
}

/* string conversions: the guest's buffers are host memory (MEM + addr) */
static void api_MultiByteToWideChar(void)
{
    int r = MultiByteToWideChar(ARG(0), ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3),
                                ARG(4) ? (LPWSTR)(MEM + ARG(4)) : NULL, (int)ARG(5));
    RET(r, 6);
}
static void api_WideCharToMultiByte(void)
{
    int r = WideCharToMultiByte(ARG(0), ARG(1), (LPCWSTR)(MEM + ARG(2)), (int)ARG(3),
                                ARG(4) ? (LPSTR)(MEM + ARG(4)) : NULL, (int)ARG(5), NULL, NULL);
    RET(r, 8);
}
static void api_LCMapStringA(void)
{
    int r = LCMapStringA(ARG(0), ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3), ARG(4) ? (LPSTR)(MEM + ARG(4)) : NULL, (int)ARG(5));
    RET(r, 6);
}
static void api_LCMapStringW(void)
{
    int r = LCMapStringW(ARG(0), ARG(1), (LPCWSTR)(MEM + ARG(2)), (int)ARG(3), ARG(4) ? (LPWSTR)(MEM + ARG(4)) : NULL, (int)ARG(5));
    RET(r, 6);
}
static void api_CompareStringA(void)
{
    RET(CompareStringA(ARG(0), ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3), (LPCSTR)(MEM + ARG(4)), (int)ARG(5)), 6);
}
static void api_CompareStringW(void)
{
    RET(CompareStringW(ARG(0), ARG(1), (LPCWSTR)(MEM + ARG(2)), (int)ARG(3), (LPCWSTR)(MEM + ARG(4)), (int)ARG(5)), 6);
}
static void api_GetStringTypeA(void)
{
    RET(GetStringTypeA(ARG(0), ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3), (LPWORD)(MEM + ARG(4))), 5);
}
static void api_GetStringTypeW(void)
{
    RET(GetStringTypeW(ARG(0), (LPCWSTR)(MEM + ARG(1)), (int)ARG(2), (LPWORD)(MEM + ARG(3))), 4);
}

static void api_GetTimeZoneInformation(void) { RET(GetTimeZoneInformation((LPTIME_ZONE_INFORMATION)(MEM + ARG(0))), 1); }
static void api_GetSystemTime(void) { GetSystemTime((LPSYSTEMTIME)(MEM + ARG(0))); RET(0, 1); }
static void api_SystemTimeToFileTime(void) { RET(SystemTimeToFileTime((SYSTEMTIME *)(MEM + ARG(0)), (FILETIME *)(MEM + ARG(1))), 2); }
static void api_FileTimeToSystemTime(void) { RET(FileTimeToSystemTime((FILETIME *)(MEM + ARG(0)), (SYSTEMTIME *)(MEM + ARG(1))), 2); }
static void api_FileTimeToLocalFileTime(void) { RET(FileTimeToLocalFileTime((FILETIME *)(MEM + ARG(0)), (FILETIME *)(MEM + ARG(1))), 2); }

static void api_IsBadPtr(void) { RET(0, 2); }
static void api_IsBadCodePtr(void) { RET(0, 1); }
static void api_SetUnhandledExceptionFilter(void) { RET(0, 1); }
static void api_UnhandledExceptionFilter(void) { RET(0, 1); }
static void api_RtlUnwind(void)
{
    /* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): no
       exceptions are ever raised here, so only drop the SEH frames above
       the target */
    u32 target = ARG(0);
    rt_log("RtlUnwind to frame %08x\n", target);
    if (target) WR32(TEB_ADDR, target);
    RET(0, 4);
}

static void api_OutputDebugStringA(void) { rt_log("debug: %s\n", gstr(ARG(0))); RET(0, 1); }
static void api_GetLastError(void) { RET(last_error, 0); }
static void api_Sleep(void) { u32 ms = ARG(0); if (ms) Sleep(ms > 100 ? 100 : ms); timers_poll(); RET(0, 1); }

static void api_ExitProcess(void)
{
    rt_log("ExitProcess(%d)\n", ARG(0));
    reg_save();
    exit((int)ARG(0));
}
static void api_TerminateProcess(void) { rt_log("TerminateProcess\n"); reg_save(); exit((int)ARG(1)); }
static void api_GetCurrentProcess(void) { RET(0xffffffffu, 0); }
static void api_GetCurrentProcessId(void) { RET(0x40, 0); }
static void api_GetCurrentThread(void) { RET(0xfffffffeu, 0); }
static void api_GetCurrentThreadId(void) { RET(0x44, 0); }
static void api_SetThreadPriority(void) { RET(1, 2); }

/* the only thread nhc.exe starts is its resource prefetcher, whose result
   the main loop polls: run it to completion right here */
static void api_CreateThread(void)
{
    u32 fn = ARG(2), param = ARG(3), idp = ARG(5);
    u32 h = gh_new(3, NULL);
    if (idp) WR32(idp, 0x48);
    guest_call(fn, 1, param);     /* runs below the API's own arguments on the stack */
    RET(h, 6);
}
static void api_TerminateThread(void) { RET(1, 2); }
static void api_ExitThread(void) { rt_fatal("ExitThread"); }

static void api_CreateMutexA(void) { RET(gh_new(3, NULL), 3); }
static void api_CreateEventA(void) { RET(gh_new(3, NULL), 4); }
static void api_SetEvent(void) { RET(1, 1); }
static void api_ReleaseMutex(void) { RET(1, 1); }
static void api_WaitForSingleObjectEx(void) { RET(0, 3); }
static void api_WaitForSingleObject(void) { RET(0, 2); }

static void api_CloseHandle(void)
{
    GH *g = gh_get(ARG(0));
    if (g) {
        if (g->kind == 1 && g->h) CloseHandle(g->h);
        if (g->kind == 2 && g->h) FindClose(g->h);
        g->used = 0;
    }
    RET(1, 1);
}

static void api_GetStdHandle(void) { RET(0x10 + (ARG(0) & 3), 1); }
static void api_SetStdHandle(void) { RET(1, 2); }
static void api_SetHandleCount(void) { RET(ARG(0), 1); }
static void api_GetFileType(void)
{
    GH *g = gh_get(ARG(0));
    RET(g && g->kind == 1 ? FILE_TYPE_DISK : ARG(0) >= 0x10 && ARG(0) < 0x14 ? FILE_TYPE_CHAR : FILE_TYPE_UNKNOWN, 1);
}

/* CreateFileA(name, access, share, sec, disp, flags, template) */
static void api_CreateFileA(void)
{
    char p[700];
    path_to_host(gstr(ARG(0)), p, sizeof p);
    HANDLE h = CreateFileA(p, ARG(1), ARG(2) | FILE_SHARE_READ, NULL, ARG(4), FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        last_error = GetLastError();
        rt_log("CreateFileA %s -> %s failed (%u)\n", gstr(ARG(0)), p, last_error);
        RET(0xffffffffu, 7);
    }
    rt_log("CreateFileA %s\n", gstr(ARG(0)));
    RET(gh_new(1, h), 7);
}

static void api_ReadFile(void)
{
    GH *g = gh_get(ARG(0));
    DWORD got = 0;
    if (!g || g->kind != 1) { last_error = ERROR_INVALID_HANDLE; RET(0, 5); }
    BOOL ok = ReadFile(g->h, MEM + ARG(1), ARG(2), &got, NULL);
    if (ARG(3)) WR32(ARG(3), got);
    if (!ok) last_error = GetLastError();
    RET(ok, 5);
}

static void api_WriteFile(void)
{
    DWORD put = 0;
    u32 h = ARG(0);
    if (h >= 0x10 && h < 0x14) {   /* std handles: to the log */
        rt_log("%.*s", (int)ARG(2), (char *)MEM + ARG(1));
        if (ARG(3)) WR32(ARG(3), ARG(2));
        RET(1, 5);
    }
    GH *g = gh_get(h);
    if (!g || g->kind != 1) { last_error = ERROR_INVALID_HANDLE; RET(0, 5); }
    BOOL ok = WriteFile(g->h, MEM + ARG(1), ARG(2), &put, NULL);
    if (ARG(3)) WR32(ARG(3), put);
    RET(ok, 5);
}

static void api_SetFilePointer(void)
{
    GH *g = gh_get(ARG(0));
    if (!g || g->kind != 1) { last_error = ERROR_INVALID_HANDLE; RET(0xffffffffu, 4); }
    LONG hi = ARG(2) ? (LONG)RD32(ARG(2)) : 0;
    DWORD r = SetFilePointer(g->h, (LONG)ARG(1), ARG(2) ? &hi : NULL, ARG(3));
    if (ARG(2)) WR32(ARG(2), hi);
    if (r == INVALID_SET_FILE_POINTER) last_error = GetLastError();
    RET(r, 4);
}

static void api_FlushFileBuffers(void) { RET(1, 1); }

static void find_to_guest(WIN32_FIND_DATAA *fd, u32 g)
{
    memcpy(MEM + g, fd, sizeof *fd);   /* same layout (320 bytes) */
}

static void api_FindFirstFileA(void)
{
    char p[700];
    path_to_host(gstr(ARG(0)), p, sizeof p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        last_error = GetLastError();
        rt_log("FindFirstFileA %s -> %s: none\n", gstr(ARG(0)), p);
        RET(0xffffffffu, 2);
    }
    find_to_guest(&fd, ARG(1));
    RET(gh_new(2, h), 2);
}

static void api_FindNextFileA(void)
{
    GH *g = gh_get(ARG(0));
    WIN32_FIND_DATAA fd;
    if (!g || g->kind != 2 || !FindNextFileA(g->h, &fd)) { last_error = ERROR_NO_MORE_FILES; RET(0, 2); }
    find_to_guest(&fd, ARG(1));
    RET(1, 2);
}

static void api_FindClose(void)
{
    GH *g = gh_get(ARG(0));
    if (g && g->kind == 2) { FindClose(g->h); g->used = 0; }
    RET(1, 1);
}

static void api_LoadLibraryA(void)
{
    const char *n = gstr(ARG(0));
    rt_log("LoadLibraryA %s\n", n);
    if (n && (strstr(n, "mack") || strstr(n, "MACK"))) RET(SMK_BASE, 1);
    /* a pseudo module handle per DLL name, resolved by GetProcAddress */
    u32 h = gstrdup(n ? n : "");
    RET(h | 0x80000000u, 1);
}
static void api_LoadModule(void) { RET(2, 2); }
static void api_FreeLibrary(void) { RET(1, 1); }

static void api_GetProcAddress(void)
{
    u32 h = ARG(0);
    const char *nm = gstr(ARG(1));
    if (ARG(1) < 0x10000) RET(0, 2);
    if (h == SMK_BASE) RET(pe_export(&img_smk, nm), 2);
    const char *dll = (h & 0x80000000u) ? (const char *)MEM + (h & 0x7fffffffu) : "KERNEL32.DLL";
    u32 a = api_resolve(dll, nm);
    rt_log("GetProcAddress %s!%s -> %08x\n", dll, nm, a);
    RET(a, 2);
}

static void api_SetErrorMode(void) { RET(0, 1); }
static void api_SetConsoleCtrlHandler(void) { RET(1, 2); }

/* TLS: one thread */
static u32 tls[64];
static int tls_used[64];
static void api_TlsAlloc(void) { for (int i = 0; i < 64; i++) if (!tls_used[i]) { tls_used[i] = 1; RET(i, 0); } RET(0xffffffffu, 0); }
static void api_TlsFree(void) { if (ARG(0) < 64) tls_used[ARG(0)] = 0; RET(1, 1); }
static void api_TlsGetValue(void) { RET(ARG(0) < 64 ? tls[ARG(0)] : 0, 1); }
static void api_TlsSetValue(void) { if (ARG(0) < 64) tls[ARG(0)] = ARG(1); RET(1, 2); }

/* ---------------- WINMM ---------------- */
static void api_timeGetTime(void) { timers_poll(); RET(now_ms(), 0); }
static void api_timeBeginPeriod(void) { RET(0, 1); }
static void api_timeEndPeriod(void) { RET(0, 1); }

/* timeSetEvent(delay, res, callback, user, flags): callbacks run at API calls */
typedef struct { int used; u32 delay, cb, user, flags, next; } Timer;
static Timer timers[8];
static int in_timer;
static void api_timeSetEvent(void)
{
    for (int i = 0; i < 8; i++)
        if (!timers[i].used) {
            timers[i] = (Timer){ 1, ARG(0) ? ARG(0) : 1, ARG(2), ARG(3), ARG(4), now_ms() + ARG(0) };
            RET(i + 1, 5);
        }
    RET(0, 5);
}
static void api_timeKillEvent(void) { if (ARG(0) - 1 < 8) timers[ARG(0) - 1].used = 0; RET(0, 1); }

void timers_poll(void)
{
    if (in_timer) return;
    in_timer = 1;
    u32 t = now_ms();
    for (int i = 0; i < 8; i++) {
        Timer *tm = &timers[i];
        if (!tm->used || (s32)(t - tm->next) < 0) continue;
        tm->next = t + tm->delay;
        if (!(tm->flags & 1)) tm->used = 0;   /* TIME_ONESHOT */
        guest_call(tm->cb, 5, i + 1, 0, tm->user, 0, 0);
    }
    in_timer = 0;
}

/* waveOut: no device, Smacker uses DirectSound */
static void api_waveOutOpen(void) { RET(MMSYSERR_NODRIVER, 6); }
static void api_waveOut2(void) { RET(MMSYSERR_INVALHANDLE, 2); }
static void api_waveOut1(void) { RET(MMSYSERR_INVALHANDLE, 1); }
static void api_waveOut3(void) { RET(MMSYSERR_INVALHANDLE, 3); }

/* mmio on host files */
#define MAXMM 16
static HMMIO mm[MAXMM];
static void api_mmioOpenA(void)
{
    char p[700];
    path_to_host(gstr(ARG(0)), p, sizeof p);
    HMMIO h = mmioOpenA(p, NULL, ARG(2));
    if (!h) { rt_log("mmioOpenA %s failed\n", p); RET(0, 3); }
    for (int i = 0; i < MAXMM; i++) if (!mm[i]) { mm[i] = h; RET(0x200 + i, 3); }
    RET(0, 3);
}
static HMMIO mmh(u32 h) { return h >= 0x200 && h < 0x200 + MAXMM ? mm[h - 0x200] : NULL; }
static void api_mmioClose(void) { HMMIO h = mmh(ARG(0)); if (h) { mmioClose(h, 0); mm[ARG(0) - 0x200] = NULL; } RET(0, 2); }
static void api_mmioRead(void) { HMMIO h = mmh(ARG(0)); RET(h ? mmioRead(h, (HPSTR)(MEM + ARG(1)), (LONG)ARG(2)) : -1, 3); }
static void api_mmioWrite(void) { HMMIO h = mmh(ARG(0)); RET(h ? mmioWrite(h, (const char *)(MEM + ARG(1)), (LONG)ARG(2)) : -1, 3); }
static void api_mmioSeek(void) { HMMIO h = mmh(ARG(0)); RET(h ? mmioSeek(h, (LONG)ARG(1), (int)ARG(2)) : -1, 3); }

/* ---------------- USER32 (window side in window.c) ---------------- */
void api_window_table(void);
static void api_MessageBoxA(void)
{
    rt_log("MessageBoxA: [%s] %s\n", gstr(ARG(2)) ? gstr(ARG(2)) : "", gstr(ARG(1)) ? gstr(ARG(1)) : "");
    if (getenv(ENV("HIDDEN"))) RET(IDOK, 4);
    win_cursor(1);
    RET(MessageBoxA(NULL, gstr(ARG(1)), gstr(ARG(2)), ARG(3) & ~0x3000u), 4);
}

/* wsprintfA(buf, fmt, ...): cdecl */
static void api_wsprintfA(void)
{
    char out[1100];
    const char *f = gstr(ARG(1));
    int ai = 2, o = 0;
    while (*f && o < 1024) {
        if (*f != '%') { out[o++] = *f++; continue; }
        char spec[32];
        int s = 0;
        spec[s++] = *f++;
        while (*f && strchr("-+ #0123456789.l", *f) && s < 28) spec[s++] = *f++;
        char c = *f ? *f++ : 0;
        spec[s++] = c;
        spec[s] = 0;
        if (c == 's') {
            spec[s - 1] = 's';
            o += snprintf(out + o, sizeof out - o, spec, gstr(ARG(ai)) ? gstr(ARG(ai)) : "(null)");
            ai++;
        } else if (c == '%') out[o++] = '%';
        else if (c) {
            /* drop 'l' modifiers, ints are 32 bits */
            char sp2[32]; int k = 0;
            for (int i = 0; spec[i]; i++) if (spec[i] != 'l') sp2[k++] = spec[i];
            sp2[k] = 0;
            o += snprintf(out + o, sizeof out - o, sp2, ARG(ai));
            ai++;
        }
    }
    if (o > 1024) o = 1024;
    out[o] = 0;
    memcpy(MEM + ARG(0), out, o + 1);
    RETC(o);
}

/* ---------------- GDI32 (only debug drawing in nhc, palette helpers in smk) ---------------- */
static void api_gdi1(void) { RET(1, 1); }
static void api_gdi2(void) { RET(1, 2); }
static void api_gdi3(void) { RET(1, 3); }
static void api_gdi4(void) { RET(1, 4); }
static void api_gdi5(void) { RET(1, 5); }
static void api_gdi9(void) { RET(1, 9); }
static void api_TextOutA(void) { RET(1, 5); }
static void api_GetDeviceCaps(void)
{
    u32 i = ARG(1);
    RET(i == 12 ? 8 : i == 14 ? 1 : i == 104 ? 256 : i == 106 ? 20 : i == 38 ? 0x100 : 0, 2);  /* BITSPIXEL, PLANES, SIZEPALETTE, NUMRESERVED, RASTERCAPS=RC_PALETTE */
}
static void api_GetSystemPaletteEntries(void) { memset(MEM + ARG(3), 0, ARG(2) * 4); RET(ARG(2), 4); }
static void api_GetSystemPaletteUse(void) { RET(1, 1); }
static void api_GetNearestPaletteIndex(void) { RET(0, 2); }
static void api_GetDIBits(void) { RET(0, 7); }
static void api_StretchDIBits(void) { RET(0, 13); }
static void api_BitBlt(void) { RET(1, 9); }

/* ---------------- table ---------------- */
void api_RegOpenKeyExA(void); void api_RegCreateKeyExA(void); void api_RegCloseKey(void);
void api_RegQueryValueExA(void); void api_RegSetValueExA(void); void api_RegDeleteKeyA(void); void api_RegEnumKeyA(void);
/* window.c */
void api_PostQuitMessage(void); void api_ShowWindow(void); void api_SetActiveWindow(void); void api_InvalidateRect(void);
void api_TranslateMessage(void); void api_DestroyWindow(void); void api_DispatchMessageA(void); void api_FindWindowA(void);
void api_GetMessageA(void); void api_PeekMessageA(void); void api_RegisterWindowMessageA(void); void api_SetFocus(void);
void api_UpdateWindow(void); void api_LoadIconA(void); void api_CreateWindowExA(void); void api_RegisterClassA(void);
void api_SetCursor(void); void api_IsIconic(void); void api_DefWindowProcA(void); void api_SendMessageA(void);
void api_GetKeyState(void); void api_GetWindowLongA(void); void api_SetWindowLongA(void); void api_LoadCursorA(void);
void api_ShowCursor(void); void api_CallWindowProcA(void); void api_ClientToScreen(void); void api_GetActiveWindow(void);
void api_GetClassInfoA(void); void api_GetDC(void); void api_GetFocus(void); void api_GetTopWindow(void);
void api_GetWindowThreadProcessId(void); void api_GetWindow(void); void api_IsWindow(void); void api_ReleaseDC(void);
void api_GetLastActivePopup(void);

typedef struct { const char *name; NativeFn fn; } ApiEnt;
static const ApiEnt api_tab[] = {
    {"GetVersion", api_GetVersion}, {"GetCommandLineA", api_GetCommandLineA},
    {"GetEnvironmentStrings", api_GetEnvironmentStrings}, {"GetEnvironmentStringsA", api_GetEnvironmentStrings},
    {"GetEnvironmentStringsW", api_GetEnvironmentStringsW}, {"FreeEnvironmentStringsA", api_FreeEnvironmentStringsA},
    {"FreeEnvironmentStringsW", api_FreeEnvironmentStringsW}, {"SetEnvironmentVariableA", api_SetEnvironmentVariableA},
    {"GetStartupInfoA", api_GetStartupInfoA}, {"GetModuleHandleA", api_GetModuleHandleA},
    {"GetModuleFileNameA", api_GetModuleFileNameA}, {"GetCurrentDirectoryA", api_GetCurrentDirectoryA},
    {"GetFullPathNameA", api_GetFullPathNameA}, {"GetDriveTypeA", api_GetDriveTypeA},
    {"HeapCreate", api_HeapCreate}, {"HeapAlloc", api_HeapAlloc}, {"HeapFree", api_HeapFree},
    {"HeapSize", api_HeapSize}, {"HeapReAlloc", api_HeapReAlloc},
    {"LocalAlloc", api_LocalAlloc}, {"LocalFree", api_LocalFree}, {"LocalLock", api_LocalLock},
    {"LocalUnlock", api_LocalUnlock}, {"LocalHandle", api_LocalHandle}, {"LocalSize", api_LocalSize},
    {"LocalReAlloc", api_LocalReAlloc}, {"VirtualAlloc", api_VirtualAlloc}, {"VirtualFree", api_VirtualFree},
    {"GlobalMemoryStatus", api_GlobalMemoryStatus}, {"GetACP", api_GetACP}, {"GetOEMCP", api_GetOEMCP},
    {"GetCPInfo", api_GetCPInfo}, {"MultiByteToWideChar", api_MultiByteToWideChar},
    {"WideCharToMultiByte", api_WideCharToMultiByte}, {"LCMapStringA", api_LCMapStringA},
    {"LCMapStringW", api_LCMapStringW}, {"CompareStringA", api_CompareStringA}, {"CompareStringW", api_CompareStringW},
    {"GetStringTypeA", api_GetStringTypeA}, {"GetStringTypeW", api_GetStringTypeW},
    {"GetTimeZoneInformation", api_GetTimeZoneInformation}, {"GetSystemTime", api_GetSystemTime},
    {"SystemTimeToFileTime", api_SystemTimeToFileTime}, {"FileTimeToSystemTime", api_FileTimeToSystemTime},
    {"FileTimeToLocalFileTime", api_FileTimeToLocalFileTime},
    {"IsBadReadPtr", api_IsBadPtr}, {"IsBadWritePtr", api_IsBadPtr}, {"IsBadCodePtr", api_IsBadCodePtr},
    {"SetUnhandledExceptionFilter", api_SetUnhandledExceptionFilter}, {"UnhandledExceptionFilter", api_UnhandledExceptionFilter},
    {"RtlUnwind", api_RtlUnwind}, {"OutputDebugStringA", api_OutputDebugStringA}, {"GetLastError", api_GetLastError},
    {"Sleep", api_Sleep}, {"ExitProcess", api_ExitProcess}, {"TerminateProcess", api_TerminateProcess},
    {"GetCurrentProcess", api_GetCurrentProcess}, {"GetCurrentProcessId", api_GetCurrentProcessId},
    {"GetCurrentThread", api_GetCurrentThread}, {"GetCurrentThreadId", api_GetCurrentThreadId},
    {"SetThreadPriority", api_SetThreadPriority}, {"CreateThread", api_CreateThread},
    {"TerminateThread", api_TerminateThread}, {"ExitThread", api_ExitThread},
    {"CreateMutexA", api_CreateMutexA}, {"CreateEventA", api_CreateEventA}, {"SetEvent", api_SetEvent},
    {"ReleaseMutex", api_ReleaseMutex}, {"WaitForSingleObjectEx", api_WaitForSingleObjectEx},
    {"WaitForSingleObject", api_WaitForSingleObject}, {"CloseHandle", api_CloseHandle},
    {"GetStdHandle", api_GetStdHandle}, {"SetStdHandle", api_SetStdHandle}, {"SetHandleCount", api_SetHandleCount},
    {"GetFileType", api_GetFileType}, {"CreateFileA", api_CreateFileA}, {"ReadFile", api_ReadFile},
    {"WriteFile", api_WriteFile}, {"SetFilePointer", api_SetFilePointer}, {"FlushFileBuffers", api_FlushFileBuffers},
    {"FindFirstFileA", api_FindFirstFileA}, {"FindNextFileA", api_FindNextFileA}, {"FindClose", api_FindClose},
    {"LoadLibraryA", api_LoadLibraryA}, {"LoadModule", api_LoadModule}, {"FreeLibrary", api_FreeLibrary},
    {"GetProcAddress", api_GetProcAddress}, {"SetErrorMode", api_SetErrorMode},
    {"SetConsoleCtrlHandler", api_SetConsoleCtrlHandler},
    {"TlsAlloc", api_TlsAlloc}, {"TlsFree", api_TlsFree}, {"TlsGetValue", api_TlsGetValue}, {"TlsSetValue", api_TlsSetValue},
    {"timeGetTime", api_timeGetTime}, {"timeBeginPeriod", api_timeBeginPeriod}, {"timeEndPeriod", api_timeEndPeriod},
    {"timeSetEvent", api_timeSetEvent}, {"timeKillEvent", api_timeKillEvent},
    {"waveOutOpen", api_waveOutOpen}, {"waveOutClose", api_waveOut1}, {"waveOutGetID", api_waveOut2},
    {"waveOutPrepareHeader", api_waveOut3}, {"waveOutUnprepareHeader", api_waveOut3}, {"waveOutReset", api_waveOut1},
    {"waveOutSetVolume", api_waveOut2}, {"waveOutWrite", api_waveOut3},
    {"mmioOpenA", api_mmioOpenA}, {"mmioClose", api_mmioClose}, {"mmioRead", api_mmioRead},
    {"mmioWrite", api_mmioWrite}, {"mmioSeek", api_mmioSeek},
    {"MessageBoxA", api_MessageBoxA}, {"wsprintfA", api_wsprintfA},
    {"PostQuitMessage", api_PostQuitMessage}, {"ShowWindow", api_ShowWindow}, {"SetActiveWindow", api_SetActiveWindow},
    {"InvalidateRect", api_InvalidateRect}, {"TranslateMessage", api_TranslateMessage},
    {"DestroyWindow", api_DestroyWindow}, {"DispatchMessageA", api_DispatchMessageA}, {"FindWindowA", api_FindWindowA},
    {"GetMessageA", api_GetMessageA}, {"PeekMessageA", api_PeekMessageA},
    {"RegisterWindowMessageA", api_RegisterWindowMessageA}, {"SetFocus", api_SetFocus}, {"UpdateWindow", api_UpdateWindow},
    {"LoadIconA", api_LoadIconA}, {"CreateWindowExA", api_CreateWindowExA}, {"RegisterClassA", api_RegisterClassA},
    {"SetCursor", api_SetCursor}, {"IsIconic", api_IsIconic}, {"DefWindowProcA", api_DefWindowProcA},
    {"SendMessageA", api_SendMessageA}, {"GetKeyState", api_GetKeyState}, {"GetWindowLongA", api_GetWindowLongA},
    {"SetWindowLongA", api_SetWindowLongA}, {"LoadCursorA", api_LoadCursorA}, {"ShowCursor", api_ShowCursor},
    {"CallWindowProcA", api_CallWindowProcA}, {"ClientToScreen", api_ClientToScreen},
    {"GetActiveWindow", api_GetActiveWindow}, {"GetClassInfoA", api_GetClassInfoA}, {"GetDC", api_GetDC},
    {"GetFocus", api_GetFocus}, {"GetTopWindow", api_GetTopWindow},
    {"GetWindowThreadProcessId", api_GetWindowThreadProcessId}, {"GetWindow", api_GetWindow},
    {"IsWindow", api_IsWindow}, {"ReleaseDC", api_ReleaseDC}, {"GetLastActivePopup", api_GetLastActivePopup},
    {"Rectangle", api_gdi5}, {"GetStockObject", api_gdi1}, {"LineTo", api_gdi3}, {"TextOutA", api_TextOutA},
    {"SelectObject", api_gdi2}, {"MoveToEx", api_gdi4}, {"CreateCompatibleBitmap", api_gdi3},
    {"CreateCompatibleDC", api_gdi1}, {"CreatePalette", api_gdi1}, {"DeleteDC", api_gdi1}, {"DeleteObject", api_gdi1},
    {"GetDIBits", api_GetDIBits}, {"GetDeviceCaps", api_GetDeviceCaps}, {"GetNearestPaletteIndex", api_GetNearestPaletteIndex},
    {"GetSystemPaletteEntries", api_GetSystemPaletteEntries}, {"GetSystemPaletteUse", api_GetSystemPaletteUse},
    {"RealizePalette", api_gdi1}, {"SelectPalette", api_gdi3}, {"SetStretchBltMode", api_gdi2},
    {"StretchDIBits", api_StretchDIBits}, {"BitBlt", api_BitBlt},
    {"RegOpenKeyExA", api_RegOpenKeyExA}, {"RegCreateKeyExA", api_RegCreateKeyExA}, {"RegCloseKey", api_RegCloseKey},
    {"RegQueryValueExA", api_RegQueryValueExA}, {"RegSetValueExA", api_RegSetValueExA},
    {"RegDeleteKeyA", api_RegDeleteKeyA}, {"RegEnumKeyA", api_RegEnumKeyA},
    {"DirectDrawCreate", api_DirectDrawCreate}, {"DirectSoundCreate", api_DirectSoundCreate},
    {NULL, NULL}
};

NativeFn api_lookup(const char *dll, const char *name)
{
    (void)dll;
    for (const ApiEnt *e = api_tab; e->name; e++)
        if (!strcmp(e->name, name)) return e->fn;
    return NULL;
}

u32 api_resolve(const char *dll, const char *name)
{
    NativeFn fn = api_lookup(dll, name);
    if (!fn) return 0;
    static struct { NativeFn fn; u32 a; } cache[64];
    static int nc;
    for (int i = 0; i < nc; i++) if (cache[i].fn == fn) return cache[i].a;
    u32 a = thunk_new(fn, _strdup(name));
    if (nc < 64) { cache[nc].fn = fn; cache[nc].a = a; nc++; }
    return a;
}

void api_init(void)
{
    const char *cl = "C:\\NEVERHOOD\\NHC.EXE";
    str_cmdline = STRINGS_ADDR;
    strcpy((char *)MEM + str_cmdline, cl);
    str_env = STRINGS_ADDR + 0x400;
    const char *env = "PATH=C:\\NEVERHOOD";
    strcpy((char *)MEM + str_env, env);
    WR16(str_env + strlen(env) + 1, 0);
    str_envw = STRINGS_ADDR + 0x800;
    for (size_t i = 0; env[i]; i++) WR16(str_envw + 2 * i, (u8)env[i]);
    WR32(str_envw + 2 * strlen(env), 0);
    WR32(str_envw + 2 * strlen(env) + 4, 0);
}
