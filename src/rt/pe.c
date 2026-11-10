/* PE image loader: maps nhc.exe / smackw32.dll from the player's copy at
 * their guest addresses (the DLL relocated), binds imports.
 *
 * Imports from Windows DLLs get thunks to the native replacements (win32.c);
 * the exe's imports from SMACKW32.DLL bind to the recompiled DLL's exports. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

void thunk_set(u32 a, NativeFn fn, const char *name);
NativeFn api_lookup(const char *dll, const char *name);

static u32 rva_ptr(Image *im, u32 rva) { return im->base + rva; }

int pe_load(Image *im)
{
    char path[700];
    snprintf(path, sizeof path, "%s\\hd\\%s", rt_root, im->file);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    u32 pe = *(u32 *)(d + 0x3c);
    if (memcmp(d + pe, "PE\0\0", 4)) { free(d); return 0; }
    u16 nsec = *(u16 *)(d + pe + 6);
    u32 opt = pe + 24;
    u16 optsz = *(u16 *)(d + pe + 20);
    u32 image_base = *(u32 *)(d + opt + 28);
    u32 ep = *(u32 *)(d + opt + 16);
    im->size = *(u32 *)(d + opt + 56);
    u32 ndirs = *(u32 *)(d + opt + 92);
    u32 dirs[32] = { 0 };
    if (ndirs > 16) ndirs = 16;
    memcpy(dirs, d + opt + 96, ndirs * 8);
    s32 delta = (s32)(im->base - image_base);
    VirtualAlloc(MEM + im->base, im->size, MEM_COMMIT, PAGE_READWRITE);
    memcpy(MEM + im->base, d, 0x400);
    u32 sec = opt + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        u32 vsize = *(u32 *)(d + sec + 8), va = *(u32 *)(d + sec + 12);
        u32 rawsz = *(u32 *)(d + sec + 16), rawptr = *(u32 *)(d + sec + 20);
        if (vsize < rawsz) vsize = rawsz;
        u32 c = rawsz < vsize ? rawsz : vsize;
        if (rawptr + c > (u32)n) c = rawptr < (u32)n ? n - rawptr : 0;
        memcpy(MEM + im->base + va, d + rawptr, c);
    }
    free(d);
    /* relocations */
    if (ndirs > 5 && dirs[11] && delta) {
        u32 pos = dirs[10], end = dirs[10] + dirs[11];
        while (pos < end) {
            u32 page = RD32(rva_ptr(im, pos)), bsize = RD32(rva_ptr(im, pos + 4));
            if (!bsize) break;
            for (u32 k = 0; k < (bsize - 8) / 2; k++) {
                u16 e = RD16(rva_ptr(im, pos + 8 + 2 * k));
                if ((e >> 12) == 3) {
                    u32 a = rva_ptr(im, page + (e & 0xfff));
                    WR32(a, RD32(a) + delta);
                }
            }
            pos += bsize;
        }
    }
    /* imports */
    int nimp = 0, nmiss = 0;
    if (ndirs > 1 && dirs[3]) {
        for (u32 pos = dirs[2];; pos += 20) {
            u32 ilt = RD32(rva_ptr(im, pos)), name = RD32(rva_ptr(im, pos + 12)), iat = RD32(rva_ptr(im, pos + 16));
            if (!name) break;
            const char *dll = (const char *)MEM + rva_ptr(im, name);
            u32 src = ilt ? ilt : iat;
            for (u32 k = 0;; k++) {
                u32 v = RD32(rva_ptr(im, src + 4 * k));
                if (!v) break;
                char nm[128];
                if (v & 0x80000000u) snprintf(nm, sizeof nm, "#%u", v & 0xffff);
                else snprintf(nm, sizeof nm, "%s", (const char *)MEM + rva_ptr(im, v + 2));
                u32 slot = rva_ptr(im, iat + 4 * k);
                if (!_stricmp(dll, "SMACKW32.DLL")) {
                    u32 a = pe_export(&img_smk, nm);
                    if (!a) rt_fatal("smackw32.dll has no export %s", nm);
                    WR32(slot, a);
                } else {
                    u32 th = im->import_base + 16 * nimp;
                    NativeFn fn = api_lookup(dll, nm);
                    char *full = malloc(strlen(dll) + strlen(nm) + 2);
                    sprintf(full, "%s!%s", dll, nm);
                    thunk_set(th, fn, full);
                    if (!fn) { nmiss++; rt_log("  no native for %s (calls will stop the game)\n", full); }
                    WR32(slot, th);
                }
                nimp++;
            }
        }
    }
    im->entry = im->base + ep;
    rt_log("loaded %s at %08x (%u bytes), %d imports, %d without native\n", im->file, im->base, im->size, nimp, nmiss);
    return 1;
}

u32 pe_export(Image *im, const char *name)
{
    u32 pe = RD32(im->base + 0x3c);
    u32 dir = RD32(im->base + pe + 24 + 96);
    if (!dir) return 0;
    u32 e = im->base + dir;
    u32 obase = RD32(e + 16), nfun = RD32(e + 20), nnames = RD32(e + 24);
    u32 afun = im->base + RD32(e + 28), anames = im->base + RD32(e + 32), aord = im->base + RD32(e + 36);
    if (name[0] == '#') {
        u32 o = (u32)atoi(name + 1) - obase;
        return o < nfun && RD32(afun + 4 * o) ? im->base + RD32(afun + 4 * o) : 0;
    }
    for (u32 i = 0; i < nnames; i++)
        if (!strcmp((const char *)MEM + im->base + RD32(anames + 4 * i), name))
            return im->base + RD32(afun + 4 * RD16(aord + 2 * i));
    return 0;
}
