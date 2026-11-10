/* PC native runtime - entry point and core.
 *
 * The Neverhood's nhc.exe and the Smacker library smackw32.dll are
 * recompiled to C (src/gen/nhc, src/gen/smk).  They run on a 4 GB guest
 * address space: the original images are mapped at their addresses (the
 * DLL relocated to SMK_BASE) from the player's installed copy, and every
 * Windows / DirectX function they import is a native replacement here
 * (win32.c, ddraw.c, dsound.c, window.c, registry.c).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "rt.h"

CPU cpu;
const FuncEnt *rt_functab;
int rt_nfuncs;
u32 rt_tail_ret;
u8 *MEM;
u8 rt_parity_tab[256];
FILE *rt_logf;
char rt_root[512];
char rt_exe_dir[512];
volatile int rt_irq_pending;
u32 rt_hr_n = 1, rt_hr_k = 0, rt_hr_rowtab;
u8 vga_planes[4][65536];
int vga_rd_plane;
u8 vga_wr_mask;
u8 *vga_capture;

extern const FuncEnt nhc_functab[], smk_functab[];
extern const int nhc_nfuncs, smk_nfuncs;
extern const WrapEnt nhc_wraptab[], smk_wraptab[];

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (rt_logf) { vfprintf(rt_logf, fmt, ap); fflush(rt_logf); }
    va_end(ap);
}

void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s\n", buf);
    rt_log("regs: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
           cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7]);
    for (int i = 0; i < 16; i++) rt_log(" [esp+%02x]=%08x", i * 4, RD32(cpu.r[4] + i * 4));
    rt_log("\n");
    if (!getenv(ENV("HIDDEN"))) MessageBoxA(NULL, buf, GAME_TITLE, MB_ICONERROR);
    exit(1);
}

/* ---- things the recompiler can emit that a Windows game never reaches ---- */
void rt_bad_code(u32 at) { rt_fatal("execution reached undecodable code at %08x", at); }
void rt_fell_off(u32 at) { rt_fatal("fell off the end of a block at %08x", at); }
void rt_noreturn(u32 at) { rt_fatal("noreturn call returned at %08x", at); }
void rt_divide_error(u32 at) { rt_fatal("divide error at %08x", at); }
void rt_int(u32 n, u32 at) { rt_fatal("int %02x at %08x", n, at); }
u32 rt_in8(u32 port, u32 at) { rt_log("in port %x at %08x\n", port, at); return 0xff; }
u32 rt_in16(u32 port, u32 at) { return rt_in8(port, at) | 0xff00; }
u32 rt_in32(u32 port, u32 at) { return rt_in8(port, at) | 0xffffff00; }
void rt_out8(u32 port, u32 v, u32 at) { rt_log("out port %x,%x at %08x\n", port, v, at); }
void rt_out16(u32 port, u32 v, u32 at) { rt_out8(port, v, at); }
void rt_out32(u32 port, u32 v, u32 at) { rt_out8(port, v, at); }
void rt_load_seg(int idx, u16 sel) { cpu.sreg[idx] = sel; if (idx != S_FS) cpu.sbase[idx] = 0; }
u32 rt_sel_limit(u16 sel) { (void)sel; return 0xffffffffu; }
void rt_trace_enter(u32 f) { rt_log("> %08x esp=%08x\n", f, cpu.r[R_ESP]); }
void rt_trace_leave(u32 f) { rt_log("< %08x eax=%08x\n", f, cpu.r[R_EAX]); }
void rt_magic_chain(u32 ra) { rt_fatal("retf to %08x", ra); }
void irq_poll(void) { rt_irq_pending = 0; }

static int unimpl_count;
void rt_unimpl(u32 at, const char *what)
{
    if (unimpl_count++ < 200) rt_log("unimplemented instruction at %08x: %s\n", at, what);
}

void (*rt_wrap_orig(const char *native, u32 *addr))(void)
{
    for (const WrapEnt *w = nhc_wraptab; w->addr; w++)
        if (!strcmp(w->native, native)) { if (addr) *addr = w->addr; return w->orig; }
    for (const WrapEnt *w = smk_wraptab; w->addr; w++)
        if (!strcmp(w->native, native)) { if (addr) *addr = w->addr; return w->orig; }
    rt_fatal("no wrapped original for %s", native);
    return NULL;
}

/* ---- x87 helpers ---- */
long double rt_fround(long double v)
{
    switch ((cpu.fcw >> 10) & 3) {
    case 0: return nearbyintl(v);
    case 1: return floorl(v);
    case 2: return ceill(v);
    default: return truncl(v);
    }
}

s64 rt_fist(long double v, int bits)
{
    long double r = rt_fround(v);
    long double lim = bits == 16 ? 32768.0L : bits == 32 ? 2147483648.0L : 9223372036854775808.0L;
    if (!(r >= -lim && r < lim)) return bits == 16 ? (s64)(s16)0x8000 : bits == 32 ? (s64)(s32)0x80000000 : INT64_MIN;
    return (s64)r;
}

void rt_fpu_save(u32 a)
{
    memset(MEM + a, 0, 108);
    WR16(a, cpu.fcw);
    WR16(a + 4, FPU_SW());
    for (int i = 0; i < 8; i++) WRF80(a + 28 + i * 10, ST(i));
    FPU_INIT();
}

void rt_fpu_restore(u32 a)
{
    cpu.fcw = RD16(a);
    u16 sw = RD16(a + 4);
    cpu.fsw = sw & ~0x3800;
    cpu.ftop = (sw >> 11) & 7;
    for (int i = 0; i < 8; i++) ST(i) = RDF80(a + 28 + i * 10);
}

/* ---- code lookup and dispatch ---- */
static void (*lookup_in(const FuncEnt *t, int n, u32 addr))(void)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t[mid].addr == addr) return t[mid].fn;
        if (t[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* thunks (imports, COM methods, GetProcAddress results) */
#define NTHUNK ((THUNK_END - THUNK_BASE) >> 4)
static NativeFn *thunk_fn;
static const char **thunk_name;
static u32 thunk_next = 0x7f200000u;

u32 thunk_new(NativeFn fn, const char *name)
{
    u32 a = thunk_next;
    thunk_next += 16;
    thunk_fn[(a - THUNK_BASE) >> 4] = fn;
    thunk_name[(a - THUNK_BASE) >> 4] = name;
    return a;
}

void thunk_set(u32 a, NativeFn fn, const char *name)
{
    thunk_fn[(a - THUNK_BASE) >> 4] = fn;
    thunk_name[(a - THUNK_BASE) >> 4] = name;
}

const char *thunk_name_of(u32 a)
{
    if (a < THUNK_BASE || a >= THUNK_END) return NULL;
    return thunk_name[(a - THUNK_BASE) >> 4];
}

static int trace_api = -1;

static void call_thunk(u32 target, u32 at)
{
    u32 i = (target - THUNK_BASE) >> 4;
    NativeFn fn = thunk_fn[i];
    rt_tail_ret = 0;
    if (!fn) rt_fatal("call to unknown native %08x (%s) from %08x", target, thunk_name[i] ? thunk_name[i] : "?", at);
    if (trace_api) {
        u32 sp = cpu.r[R_ESP];
        rt_log("api %s from %08x (%08x %08x %08x %08x)", thunk_name[i], RD32(sp), RD32(sp + 4), RD32(sp + 8), RD32(sp + 12), RD32(sp + 16));
        fn();
        rt_log(" -> %08x\n", cpu.r[R_EAX]);
        return;
    }
    fn();
}

void (*rt_lookup(u32 addr))(void)
{
    void (*f)(void) = lookup_in(nhc_functab, nhc_nfuncs, addr);
    if (!f) f = lookup_in(smk_functab, smk_nfuncs, addr);
    return f;
}

void rt_call(u32 target, u32 at)
{
    if (target >= THUNK_BASE && target < THUNK_END) { call_thunk(target, at); return; }
    void (*fn)(void) = rt_lookup(target);
    if (!fn) rt_fatal("call to unknown code %08x from %08x", target, at);
    fn();
}

void rt_jump(u32 target, u32 at)
{
    if (target >= THUNK_BASE && target < THUNK_END) { call_thunk(target, at); return; }
    void (*fn)(void) = rt_lookup(target);
    if (!fn) rt_fatal("jump to unknown code %08x from %08x", target, at);
    fn();
}

/* call guest code from a native: arguments pushed right to left, return
   address MAGIC_RET; works for stdcall and cdecl (esp restored here) */
u32 guest_call(u32 fn, int nargs, ...)
{
    u32 args[16];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, u32);
    va_end(ap);
    u32 sp = cpu.r[R_ESP];
    u32 saved_tail = rt_tail_ret;
    u32 ebx = cpu.r[R_EBX], esi = cpu.r[R_ESI], edi = cpu.r[R_EDI], ebp = cpu.r[R_EBP];
    for (int i = nargs - 1; i >= 0; i--) { cpu.r[R_ESP] -= 4; WR32(cpu.r[R_ESP], args[i]); }
    cpu.r[R_ESP] -= 4;
    WR32(cpu.r[R_ESP], MAGIC_RET);
    rt_tail_ret = 0;
    if (fn >= THUNK_BASE && fn < THUNK_END) call_thunk(fn, 0);
    else {
        void (*f)(void) = rt_lookup(fn);
        if (!f) rt_fatal("guest_call: no function at %08x", fn);
        f();
    }
    u32 r = cpu.r[R_EAX];
    cpu.r[R_ESP] = sp;
    cpu.r[R_EBX] = ebx; cpu.r[R_ESI] = esi; cpu.r[R_EDI] = edi; cpu.r[R_EBP] = ebp;
    rt_tail_ret = saved_tail;
    return r;
}

/* heuristic backtrace: return addresses into the game code found on the guest stack */
void rt_backtrace(const char *tag)
{
    u32 sp = cpu.r[R_ESP];
    int n = 0;
    rt_log("  %s backtrace:", tag);
    for (u32 i = 0; i < 512 && n < 16; i++) {
        u32 v = RD32(sp + i * 4);
        if (((v > 0x401000 && v < 0x49e000) || (v > 0x10001000 && v < 0x1000e000)) && RD8(v - 5) == 0xe8) { rt_log(" %08x", v - 5); n++; }
    }
    rt_log("\n");
}

/* ---- guest memory ---- */
static LONG WINAPI guest_fault(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
        if (addr >= (ULONG_PTR)MEM && addr < (ULONG_PTR)MEM + GUEST_RESERVE) {
            u32 ga = (u32)(addr - (ULONG_PTR)MEM);
            /* commit on demand: guest code may touch memory it never asked for */
            if (VirtualAlloc((void *)(addr & ~(ULONG_PTR)0xffff), 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
                static int warned;
                if (warned++ < 20) rt_log("note: committed guest page at %08x on demand host %p base %p\n", ga,
                                          (void *)ep->ContextRecord->Rip, (void *)GetModuleHandleA(NULL));
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            rt_log("guest access fault at %08x\n", ga);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void mem_init(void)
{
    MEM = VirtualAlloc(NULL, GUEST_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
    if (!MEM) rt_fatal("cannot reserve guest address space");
    if (!VirtualAlloc(MEM, GUEST_COMMIT, MEM_COMMIT, PAGE_READWRITE)) rt_fatal("cannot commit guest memory");
    VirtualAlloc(MEM + THUNK_BASE, THUNK_END - THUNK_BASE, MEM_COMMIT, PAGE_READWRITE);
    AddVectoredExceptionHandler(1, guest_fault);
    for (int i = 0; i < 256; i++) {
        int b = i, p = 1;
        while (b) { p ^= b & 1; b >>= 1; }
        rt_parity_tab[i] = (u8)p;
    }
    thunk_fn = calloc(NTHUNK, sizeof *thunk_fn);
    thunk_name = calloc(NTHUNK, sizeof *thunk_name);
    /* the code at a thunk address is "ret": harmless if anything disassembles it */
    memset(MEM + THUNK_BASE, 0xc3, THUNK_END - THUNK_BASE);
}

Image img_nhc = { "nhc.exe", NHC_BASE, 0x7f000000u };
Image img_smk = { "smackw32.dll", SMK_BASE, 0x7f100000u };

int main(int argc, char **argv)
{
    GetModuleFileNameA(NULL, rt_exe_dir, sizeof rt_exe_dir);
    char *s = strrchr(rt_exe_dir, '\\');
    if (s) *s = 0;
    snprintf(rt_root, sizeof rt_root, "%s\\game", rt_exe_dir);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(rt_root, sizeof rt_root, "%s", argv[++i]);
    if (getenv(ENV("ROOT"))) snprintf(rt_root, sizeof rt_root, "%s", getenv(ENV("ROOT")));
    char logp[700];
    snprintf(logp, sizeof logp, "%s\\" GAME_SHORT ".log", rt_exe_dir);
    rt_logf = fopen(logp, "w");
    rt_log(GAME_TITLE " - PC native runtime\nroot: %s\n", rt_root);
    trace_api = getenv(ENV("TRACEAPI")) != NULL;
    if (getenv(ENV("HIDDEN"))) SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) rt_fatal("SDL: %s", SDL_GetError());
    mem_init();
    api_init();
    reg_init();
    if (!pe_load(&img_smk)) rt_fatal("cannot load %s from %s\\hd", img_smk.file, rt_root);
    if (!pe_load(&img_nhc)) rt_fatal("cannot load %s from %s\\hd", img_nhc.file, rt_root);

    /* thread block: fs:[0] = end of the SEH chain, fs:[18h] = self */
    WR32(TEB_ADDR, 0xffffffffu);
    WR32(TEB_ADDR + 4, STACK_TOP);
    WR32(TEB_ADDR + 8, STACK_LO);
    WR32(TEB_ADDR + 0x18, TEB_ADDR);
    cpu.sbase[S_FS] = TEB_ADDR;
    cpu.sreg[S_FS] = 0x3b;
    cpu.sreg[S_CS] = 0x1b; cpu.sreg[S_DS] = cpu.sreg[S_ES] = cpu.sreg[S_SS] = 0x23;
    cpu.r[R_ESP] = STACK_TOP;
    FPU_INIT();
    cpu.fcw = 0x27f;   /* Win32 default: 53-bit precision */

    rt_log("smackw32.dll DllMain\n");
    guest_call(img_smk.entry, 3, SMK_BASE, 1, 0);
    rt_log("nhc.exe entry %08x\n", img_nhc.entry);
    guest_call(img_nhc.entry, 0);
    rt_log("game returned from its entry point\n");
    reg_save();
    return 0;
}
