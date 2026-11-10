/* Registry (ADVAPI32 Reg*) replacement: the game keeps its settings and its
 * saved games in the registry.  Here the registry is a small tree kept in
 * a text file next to the exe (neverhood_registry.txt), so saves travel
 * with the game and nothing is written to the Windows registry.
 *
 * File format: "[path]" lines (root\sub\key, roots HKLM / HKCU), then
 * "name=type:hexbytes" lines for the values of that key. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

typedef struct Val { char name[128]; u32 type, len; u8 *data; struct Val *next; } Val;
typedef struct Key { char path[512]; Val *vals; struct Key *next; } Key;
static Key *keys;
static int dirty;
static char reg_file[700];

static Key *key_find(const char *path, int create)
{
    for (Key *k = keys; k; k = k->next)
        if (!_stricmp(k->path, path)) return k;
    if (!create) return NULL;
    Key *k = calloc(1, sizeof *k);
    snprintf(k->path, sizeof k->path, "%s", path);
    k->next = keys;
    keys = k;
    dirty = 1;
    return k;
}

static Val *val_find(Key *k, const char *name, int create)
{
    for (Val *v = k->vals; v; v = v->next)
        if (!_stricmp(v->name, name)) return v;
    if (!create) return NULL;
    Val *v = calloc(1, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->next = k->vals;
    k->vals = v;
    return v;
}

static void val_set(Key *k, const char *name, u32 type, const void *data, u32 len)
{
    Val *v = val_find(k, name, 1);
    free(v->data);
    v->data = malloc(len ? len : 1);
    memcpy(v->data, data, len);
    v->len = len;
    v->type = type;
    dirty = 1;
}

void reg_save(void)
{
    if (!dirty) return;
    FILE *f = fopen(reg_file, "w");
    if (!f) return;
    for (Key *k = keys; k; k = k->next) {
        fprintf(f, "[%s]\n", k->path);
        for (Val *v = k->vals; v; v = v->next) {
            fprintf(f, "%s=%u:", v->name, v->type);
            for (u32 i = 0; i < v->len; i++) fprintf(f, "%02x", v->data[i]);
            fprintf(f, "\n");
        }
    }
    fclose(f);
    dirty = 0;
}

void reg_init(void)
{
    snprintf(reg_file, sizeof reg_file, "%s\\%s", rt_exe_dir, getenv(ENV("REGFILE")) ? getenv(ENV("REGFILE")) : GAME_SHORT "_registry.txt");
    FILE *f = fopen(reg_file, "r");
    Key *cur = NULL;
    if (f) {
        static char line[1 << 16];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '[') {
                char *e = strrchr(line, ']');
                if (e) *e = 0;
                cur = key_find(line + 1, 1);
            } else if (cur && strchr(line, '=')) {
                char *eq = strrchr(line, '=');
                *eq = 0;
                u32 type = (u32)strtoul(eq + 1, NULL, 10);
                char *hex = strchr(eq + 1, ':');
                u32 len = hex ? (u32)strlen(hex + 1) / 2 : 0;
                u8 *b = malloc(len + 1);
                for (u32 i = 0; i < len; i++) { unsigned x; sscanf(hex + 1 + 2 * i, "%2x", &x); b[i] = (u8)x; }
                val_set(cur, line, type, b, len);
                free(b);
            }
        }
        fclose(f);
    }
    /* what the game's installer wrote: the data "drive" is the CD copy (D:\) */
    Key *k = key_find("HKLM\\SOFTWARE\\DreamWorks Interactive\\Neverhood", 1);
    val_set(k, "Data Drive", REG_SZ, "D:\\", 4);
    val_set(k, "Installed Directory", REG_SZ, "C:\\NEVERHOOD", 13);
    val_set(k, "Run File", REG_SZ, "C:\\NEVERHOOD\\NHC", 17);
    u32 one = 1, all = 0xffffffffu;
    val_set(k, "Installed", REG_DWORD, &one, 4);
    val_set(k, "KBPS", REG_DWORD, &all, 4);
    val_set(k, "KBPS_CD", REG_DWORD, &all, 4);
    reg_save();
}

/* handles: 0x80000000.. predefined roots; ours are 0x5e000000 + index */
#define MAXH 256
static char *hpath[MAXH];

static const char *root_name(u32 h)
{
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    default: return NULL;
    }
}

static int path_of(u32 h, const char *sub, char *out, size_t n)
{
    const char *base = root_name(h);
    if (!base) {
        u32 i = h - 0x5e000000u;
        if (i >= MAXH || !hpath[i]) return 0;
        base = hpath[i];
    }
    if (sub && *sub) snprintf(out, n, "%s\\%s", base, sub);
    else snprintf(out, n, "%s", base);
    size_t l = strlen(out);
    while (l && out[l - 1] == '\\') out[--l] = 0;
    return 1;
}

static u32 new_handle(const char *path)
{
    for (int i = 0; i < MAXH; i++)
        if (!hpath[i]) { hpath[i] = _strdup(path); return 0x5e000000u + i; }
    rt_fatal("out of registry handles");
    return 0;
}

static int has_prefix_key(const char *path)
{
    size_t l = strlen(path);
    for (Key *k = keys; k; k = k->next)
        if (!_strnicmp(k->path, path, l) && (k->path[l] == 0 || k->path[l] == '\\')) return 1;
    return 0;
}

/* RegOpenKeyExA(hKey, sub, opts, sam, phk) */
void api_RegOpenKeyExA(void)
{
    char p[600];
    if (!path_of(ARG(0), gstr(ARG(1)), p, sizeof p)) RET(6, 5);
    if (!has_prefix_key(p) && !root_name(ARG(0)) ) RET(2, 5);
    if (!has_prefix_key(p)) RET(2, 5);
    WR32(ARG(4), new_handle(p));
    RET(0, 5);
}

/* RegCreateKeyExA(hKey, sub, res, class, opts, sam, sec, phk, disp) */
void api_RegCreateKeyExA(void)
{
    char p[600];
    if (!path_of(ARG(0), gstr(ARG(1)), p, sizeof p)) RET(6, 9);
    int existed = has_prefix_key(p);
    key_find(p, 1);
    WR32(ARG(7), new_handle(p));
    if (ARG(8)) WR32(ARG(8), existed ? 2 : 1);
    reg_save();
    RET(0, 9);
}

void api_RegCloseKey(void)
{
    u32 i = ARG(0) - 0x5e000000u;
    if (i < MAXH && hpath[i]) { free(hpath[i]); hpath[i] = NULL; }
    RET(0, 1);
}

/* RegQueryValueExA(hKey, name, res, type, data, cbData) */
void api_RegQueryValueExA(void)
{
    char p[600];
    if (!path_of(ARG(0), NULL, p, sizeof p)) RET(6, 6);
    Key *k = key_find(p, 0);
    const char *nm = gstr(ARG(1));
    Val *v = k ? val_find(k, nm ? nm : "", 0) : NULL;
    if (!v) RET(2, 6);
    if (ARG(3)) WR32(ARG(3), v->type);
    u32 cb = ARG(5) ? RD32(ARG(5)) : 0;
    if (ARG(4)) {
        if (cb < v->len) { WR32(ARG(5), v->len); RET(234, 6); }   /* ERROR_MORE_DATA */
        memcpy(MEM + ARG(4), v->data, v->len);
    }
    if (ARG(5)) WR32(ARG(5), v->len);
    RET(0, 6);
}

/* RegSetValueExA(hKey, name, res, type, data, cb) */
void api_RegSetValueExA(void)
{
    char p[600];
    if (!path_of(ARG(0), NULL, p, sizeof p)) RET(6, 6);
    Key *k = key_find(p, 1);
    const char *nm = gstr(ARG(1));
    val_set(k, nm ? nm : "", ARG(3), MEM + ARG(4), ARG(5));
    reg_save();
    RET(0, 6);
}

/* RegDeleteKeyA(hKey, sub) */
void api_RegDeleteKeyA(void)
{
    char p[600];
    if (!path_of(ARG(0), gstr(ARG(1)), p, sizeof p)) RET(6, 2);
    for (Key **pk = &keys; *pk; pk = &(*pk)->next)
        if (!_stricmp((*pk)->path, p)) {
            Key *k = *pk;
            *pk = k->next;
            for (Val *v = k->vals, *n; v; v = n) { n = v->next; free(v->data); free(v); }
            free(k);
            dirty = 1;
            reg_save();
            RET(0, 2);
        }
    RET(2, 2);
}

/* RegEnumKeyA(hKey, index, name, cch): direct sub keys */
void api_RegEnumKeyA(void)
{
    char p[600];
    if (!path_of(ARG(0), NULL, p, sizeof p)) RET(6, 4);
    size_t l = strlen(p);
    char seen[64][128];
    int ns = 0;
    u32 want = ARG(1);
    /* oldest keys first, like creation order */
    Key *order[1024];
    int n = 0;
    for (Key *k = keys; k && n < 1024; k = k->next) order[n++] = k;
    for (int j = n - 1; j >= 0; j--) {
        Key *k = order[j];
        if (_strnicmp(k->path, p, l) || k->path[l] != '\\') continue;
        char sub[128];
        snprintf(sub, sizeof sub, "%s", k->path + l + 1);
        char *bs = strchr(sub, '\\');
        if (bs) *bs = 0;
        int dup = 0;
        for (int i = 0; i < ns; i++) if (!_stricmp(seen[i], sub)) dup = 1;
        if (dup || ns >= 64) continue;
        snprintf(seen[ns++], 128, "%s", sub);
        if ((u32)(ns - 1) == want) {
            if (strlen(sub) + 1 > ARG(3)) RET(234, 4);
            strcpy((char *)MEM + ARG(2), sub);
            RET(0, 4);
        }
    }
    RET(259, 4);   /* ERROR_NO_MORE_ITEMS */
}
