/* Guest heap for HeapAlloc / LocalAlloc / VirtualAlloc / COM objects.
 *
 * Power-of-two size classes with free lists and a bump pointer: simple,
 * fast, never moves a block.  Every block has a 16-byte header
 * (requested size, class, magic) right before the address handed out. */
#include <string.h>
#include "rt.h"

#define HDR 16u
#define MAGIC 0x4e48484du
static u32 bump = HEAP_BASE;
static u32 freelist[32];

static int class_of(u32 n)
{
    int c = 5;                       /* 32 bytes minimum, header included */
    while ((1u << c) < n + HDR) c++;
    return c;
}

u32 gmalloc(u32 size)
{
    int c = class_of(size);
    u32 b = freelist[c];
    if (b) freelist[c] = RD32(b + HDR);
    else {
        u32 bs = 1u << c;
        u32 al = bs >= 0x1000 ? 0x1000 : 16;
        /* big blocks page aligned (header in the page before) */
        u32 start = ((bump + HDR + al - 1) & ~(al - 1)) - HDR;
        if (start + bs > HEAP_END) rt_fatal("guest heap exhausted (%u bytes asked)", size);
        b = start;
        bump = start + bs;
    }
    WR32(b, size);
    WR32(b + 4, c);
    WR32(b + 8, MAGIC);
    memset(MEM + b + HDR, 0, size);
    return b + HDR;
}

u32 gcalloc(u32 size) { return gmalloc(size); }

void gfree(u32 p)
{
    if (!p) return;
    u32 b = p - HDR;
    if (RD32(b + 8) != MAGIC) { rt_log("gfree: not a heap block %08x\n", p); return; }
    int c = RD32(b + 4);
    WR32(b + 8, 0);
    WR32(b + HDR, freelist[c]);
    freelist[c] = b;
}

u32 gsize(u32 p)
{
    if (!p || RD32(p - HDR + 8) != MAGIC) return 0;
    return RD32(p - HDR);
}

u32 grealloc(u32 p, u32 size)
{
    if (!p) return gmalloc(size);
    u32 b = p - HDR;
    if (RD32(b + 8) != MAGIC) { rt_log("grealloc: not a heap block %08x\n", p); return 0; }
    u32 old = RD32(b);
    if (size + HDR <= (1u << RD32(b + 4))) {
        if (size > old) memset(MEM + p + old, 0, size - old);
        WR32(b, size);
        return p;
    }
    u32 n = gmalloc(size);
    memcpy(MEM + n, MEM + p, old < size ? old : size);
    gfree(p);
    return n;
}

u32 gstrdup(const char *s)
{
    u32 n = (u32)strlen(s) + 1;
    u32 p = gmalloc(n);
    memcpy(MEM + p, s, n);
    return p;
}
