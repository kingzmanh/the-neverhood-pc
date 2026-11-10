/* DirectSound replacement: IDirectSound and IDirectSoundBuffer on an SDL
 * audio mixer.  Secondary buffers are guest memory that the game (and the
 * recompiled Smacker library, for movie sound) fills through Lock/Unlock;
 * the mixer callback plays them like a sound card would, with volume, pan
 * and frequency, and reports play / write cursors from its own position.
 *
 * NH_MUTE=1 (and NH_HIDDEN test runs) keep the device running but silent. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define DS_OK 0
#define DSERR_INVALIDPARAM 0x80070057u
#define OUT_RATE 44100

typedef struct Buf {
    u32 obj;
    u32 mem, size;
    int ch, bits, rate;
    u32 freq;                  /* 0 = rate */
    s32 vol, pan;              /* hundredths of dB */
    float gl, gr;
    int primary, playing, looping, refs;
    double pos;                /* in frames */
    struct Buf *shared;        /* duplicate: memory owner */
} Buf;

#define MAXB 256
static Buf *bufs[MAXB];
static u32 vt_ds, vt_buf, obj_ds;
static SDL_AudioDeviceID dev;
static int muted;

static Buf *B(u32 obj) { u32 i = RD32(obj + 4); return (i < MAXB && bufs[i] && bufs[i]->obj == obj) ? bufs[i] : NULL; }

static void gains(Buf *b)
{
    float v = b->vol <= -10000 ? 0.f : powf(10.f, b->vol / 2000.f);
    float l = 1.f, r = 1.f;
    if (b->pan < 0) r = powf(10.f, (b->pan < -10000 ? -10000 : b->pan) / 2000.f);
    if (b->pan > 0) l = powf(10.f, -(b->pan > 10000 ? 10000 : b->pan) / 2000.f);
    b->gl = v * l;
    b->gr = v * r;
}

static int frame_bytes(Buf *b) { return b->ch * b->bits / 8; }

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    int n = len / 4;
    static float acc[8192 * 2];
    if (n > 8192) n = 8192;
    memset(acc, 0, n * 2 * sizeof(float));
    for (int i = 0; i < MAXB; i++) {
        Buf *b = bufs[i];
        if (!b || !b->playing || b->primary || !b->size) continue;
        int fb = frame_bytes(b);
        u32 frames = b->size / fb;
        if (!frames) continue;
        double step = (double)(b->freq ? b->freq : (u32)b->rate) / OUT_RATE;
        const u8 *m = MEM + b->mem;
        for (int k = 0; k < n; k++) {
            u32 f = (u32)b->pos;
            if (f >= frames) {
                if (b->looping) { b->pos -= frames; f = (u32)b->pos; if (f >= frames) { b->pos = 0; f = 0; } }
                else { b->playing = 0; b->pos = 0; break; }
            }
            float l, r;
            const u8 *p = m + f * fb;
            if (b->bits == 8) { l = (p[0] - 128) / 128.f; r = b->ch == 2 ? (p[1] - 128) / 128.f : l; }
            else { l = *(const s16 *)p / 32768.f; r = b->ch == 2 ? *(const s16 *)(p + 2) / 32768.f : l; }
            acc[2 * k] += l * b->gl;
            acc[2 * k + 1] += r * b->gr;
            b->pos += step;
        }
    }
    s16 *o = (s16 *)stream;
    for (int k = 0; k < n * 2; k++) {
        float v = muted ? 0.f : acc[k];
        if (v > 1.f) v = 1.f;
        if (v < -1.f) v = -1.f;
        o[k] = (s16)(v * 32767.f);
    }
    if (len > n * 4) memset(stream + n * 4, 0, len - n * 4);
}

void dsound_init(void)
{
    if (dev) return;
    muted = getenv(ENV("MUTE")) || getenv(ENV("HIDDEN"));
    SDL_AudioSpec want = { 0 }, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) rt_log("audio: %s\n", SDL_GetError());
    else { SDL_PauseAudioDevice(dev, 0); rt_log("audio: %d Hz%s\n", have.freq, muted ? " (muted)" : ""); }
}

static void lock(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

static Buf *buf_new(void)
{
    int i = 1;
    while (i < MAXB && bufs[i]) i++;
    if (i == MAXB) return NULL;
    Buf *b = calloc(1, sizeof *b);
    b->obj = gmalloc(8);
    WR32(b->obj, vt_buf);
    WR32(b->obj + 4, i);
    b->refs = 1;
    b->ch = 1; b->bits = 8; b->rate = 22050;
    gains(b);
    lock();
    bufs[i] = b;
    unlock();
    return b;
}

/* ---------------- IDirectSound ---------------- */
static void ds_QueryInterface(void) { WR32(ARG(2), ARG(0)); RET(DS_OK, 3); }
static void ds_AddRef(void) { RET(2, 1); }
static void ds_Release(void) { RET(0, 1); }
static void ds_SetCooperativeLevel(void) { RET(DS_OK, 3); }
static void ds_Compact(void) { RET(DS_OK, 1); }
static void ds_GetSpeakerConfig(void) { WR32(ARG(1), 4); RET(DS_OK, 2); }
static void ds_ok2(void) { RET(DS_OK, 2); }

static void ds_GetCaps(void)
{
    u32 c = ARG(1);
    u32 n = RD32(c);
    if (n > 4) memset(MEM + c + 4, 0, n - 4);
    WR32(c + 4, 0x00000001u | 0x00000004u | 0x00000400u | 0x00000800u | 0x0f00u);   /* PRIMARY/SECONDARY mono/stereo 8/16 */
    WR32(c + 8, 100); WR32(c + 12, 100000);
    RET(DS_OK, 2);
}

/* CreateSoundBuffer(this, desc, &buf, outer) */
static void ds_CreateSoundBuffer(void)
{
    u32 d = ARG(1);
    u32 flags = RD32(d + 4), bytes = RD32(d + 8), wf = RD32(d + 16);
    Buf *b = buf_new();
    if (!b) RET(0x8878000au, 4);
    if (flags & 1) { b->primary = 1; rt_log("dsound: primary buffer\n"); }
    else {
        if (wf) { b->ch = RD16(wf + 2); b->rate = RD32(wf + 4); b->bits = RD16(wf + 14); }
        if (b->ch < 1 || b->ch > 2) b->ch = 1;
        if (b->bits != 8 && b->bits != 16) b->bits = 8;
        b->size = bytes;
        b->mem = gmalloc(bytes ? bytes : 4);
        static int nlog;
        if (nlog++ < 40) rt_log("dsound: buffer %u bytes %d Hz %d-bit %s\n", bytes, b->rate, b->bits, b->ch == 2 ? "stereo" : "mono");
        if (b->bits == 8) memset(MEM + b->mem, 0x80, bytes);
    }
    WR32(ARG(2), b->obj);
    RET(DS_OK, 4);
}

/* DuplicateSoundBuffer(this, src, &dup) */
static void ds_DuplicateSoundBuffer(void)
{
    Buf *s = B(ARG(1));
    if (!s) RET(DSERR_INVALIDPARAM, 3);
    Buf *b = buf_new();
    if (!b) RET(0x8878000au, 3);
    b->mem = s->mem; b->size = s->size; b->ch = s->ch; b->bits = s->bits; b->rate = s->rate;
    b->shared = s->shared ? s->shared : s;
    WR32(ARG(2), b->obj);
    RET(DS_OK, 3);
}

/* ---------------- IDirectSoundBuffer ---------------- */
static void b_QueryInterface(void) { Buf *b = B(ARG(0)); if (b) b->refs++; WR32(ARG(2), ARG(0)); RET(DS_OK, 3); }
static void b_AddRef(void) { Buf *b = B(ARG(0)); if (b) b->refs++; RET(b ? b->refs : 0, 1); }
static void b_Release(void)
{
    Buf *b = B(ARG(0));
    if (!b) RET(0, 1);
    if (--b->refs > 0) RET(b->refs, 1);
    lock();
    bufs[RD32(b->obj + 4)] = NULL;
    unlock();
    int used = 0;
    if (!b->shared) for (int i = 0; i < MAXB; i++) if (bufs[i] && bufs[i]->shared == b) used = 1;
    if (!b->shared && !used && b->mem) gfree(b->mem);
    gfree(b->obj);
    free(b);
    RET(0, 1);
}

static void b_GetCaps(void)
{
    Buf *b = B(ARG(0));
    u32 c = ARG(1);
    WR32(c + 4, b && b->primary ? 1 : 0x8 | 0x80 | 0x40 | 0x20);
    WR32(c + 8, b ? b->size : 0);
    WR32(c + 12, 0); WR32(c + 16, 0);
    RET(DS_OK, 2);
}

/* GetCurrentPosition(this, &play, &write) */
static void b_GetCurrentPosition(void)
{
    Buf *b = B(ARG(0));
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    lock();
    u32 fb = frame_bytes(b);
    u32 play = (u32)b->pos * fb;
    unlock();
    if (b->size) play %= b->size;
    u32 ahead = ((b->freq ? b->freq : (u32)b->rate) / 66) * fb;   /* ~15 ms */
    u32 write = b->size ? (play + (b->playing ? ahead : 0)) % b->size : 0;
    if (ARG(1)) WR32(ARG(1), play);
    if (ARG(2)) WR32(ARG(2), write);
    RET(DS_OK, 3);
}

static void b_GetFormat(void)
{
    Buf *b = B(ARG(0));
    u32 w = ARG(1);
    if (w && b) {
        WR16(w, 1); WR16(w + 2, b->ch); WR32(w + 4, b->rate);
        WR32(w + 8, b->rate * frame_bytes(b)); WR16(w + 12, frame_bytes(b)); WR16(w + 14, b->bits);
        if (ARG(2) >= 18) WR16(w + 16, 0);
    }
    if (ARG(3)) WR32(ARG(3), 18);
    RET(DS_OK, 4);
}

static void b_GetVolume(void) { Buf *b = B(ARG(0)); WR32(ARG(1), b ? (u32)b->vol : 0); RET(DS_OK, 2); }
static void b_GetPan(void) { Buf *b = B(ARG(0)); WR32(ARG(1), b ? (u32)b->pan : 0); RET(DS_OK, 2); }
static void b_GetFrequency(void) { Buf *b = B(ARG(0)); WR32(ARG(1), b ? (b->freq ? b->freq : (u32)b->rate) : 0); RET(DS_OK, 2); }
static void b_GetStatus(void)
{
    Buf *b = B(ARG(0));
    lock();
    u32 s = b && b->playing ? (1 | (b->looping ? 4 : 0)) : 0;
    unlock();
    WR32(ARG(1), s);
    RET(DS_OK, 2);
}

/* Lock(this, offset, bytes, &p1, &n1, &p2, &n2, flags) */
static void b_Lock(void)
{
    Buf *b = B(ARG(0));
    if (!b || !b->size) RET(DSERR_INVALIDPARAM, 8);
    u32 off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 1) {   /* DSBLOCK_FROMWRITECURSOR */
        u32 fb = frame_bytes(b);
        off = ((u32)b->pos * fb + ((b->freq ? b->freq : (u32)b->rate) / 66) * fb) % b->size;
    }
    if (flags & 2) { off = 0; n = b->size; }   /* DSBLOCK_ENTIREBUFFER */
    off %= b->size;
    if (n > b->size) n = b->size;
    u32 n1 = n, n2 = 0;
    if (off + n > b->size) { n1 = b->size - off; n2 = n - n1; }
    WR32(ARG(3), b->mem + off);
    WR32(ARG(4), n1);
    if (ARG(5)) WR32(ARG(5), n2 ? b->mem : 0);
    if (ARG(6)) WR32(ARG(6), n2);
    RET(DS_OK, 8);
}

static void b_Unlock(void) { RET(DS_OK, 5); }

/* Play(this, res1, priority, flags) */
static void b_Play(void)
{
    Buf *b = B(ARG(0));
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    lock();
    b->playing = 1;
    b->looping = ARG(3) & 1;
    unlock();
    RET(DS_OK, 4);
}

static void b_Stop(void) { Buf *b = B(ARG(0)); if (b) { lock(); b->playing = 0; unlock(); } RET(DS_OK, 1); }

static void b_SetCurrentPosition(void)
{
    Buf *b = B(ARG(0));
    if (b && !b->primary) { lock(); b->pos = (double)(ARG(1) / frame_bytes(b)); unlock(); }
    RET(DS_OK, 2);
}

static void b_SetFormat(void) { RET(DS_OK, 2); }
static void b_SetVolume(void) { Buf *b = B(ARG(0)); if (b) { lock(); b->vol = (s32)ARG(1); gains(b); unlock(); } RET(DS_OK, 2); }
static void b_SetPan(void) { Buf *b = B(ARG(0)); if (b) { lock(); b->pan = (s32)ARG(1); gains(b); unlock(); } RET(DS_OK, 2); }
static void b_SetFrequency(void) { Buf *b = B(ARG(0)); if (b) { lock(); b->freq = ARG(1); unlock(); } RET(DS_OK, 2); }
static void b_Restore(void) { RET(DS_OK, 1); }
static void b_Initialize(void) { RET(DS_OK, 3); }

static u32 make_vt(const NativeFn *fns, const char **names, int n)
{
    u32 vt = gmalloc(4 * n);
    for (int i = 0; i < n; i++) WR32(vt + 4 * i, thunk_new(fns[i], names[i]));
    return vt;
}

/* DirectSoundCreate(guid, lplpDS, outer) */
void api_DirectSoundCreate(void)
{
    if (!vt_ds) {
        static const NativeFn ds[] = { ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer, ds_GetCaps,
            ds_DuplicateSoundBuffer, ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig, ds_ok2, ds_ok2 };
        static const char *dsn[] = { "IDirectSound::QueryInterface", "IDirectSound::AddRef", "IDirectSound::Release",
            "IDirectSound::CreateSoundBuffer", "IDirectSound::GetCaps", "IDirectSound::DuplicateSoundBuffer",
            "IDirectSound::SetCooperativeLevel", "IDirectSound::Compact", "IDirectSound::GetSpeakerConfig",
            "IDirectSound::SetSpeakerConfig", "IDirectSound::Initialize" };
        vt_ds = make_vt(ds, dsn, 11);
        static const NativeFn bf[] = { b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition,
            b_GetFormat, b_GetVolume, b_GetPan, b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play,
            b_SetCurrentPosition, b_SetFormat, b_SetVolume, b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore };
        static const char *bfn[] = { "Buffer::QueryInterface", "Buffer::AddRef", "Buffer::Release", "Buffer::GetCaps",
            "Buffer::GetCurrentPosition", "Buffer::GetFormat", "Buffer::GetVolume", "Buffer::GetPan",
            "Buffer::GetFrequency", "Buffer::GetStatus", "Buffer::Initialize", "Buffer::Lock", "Buffer::Play",
            "Buffer::SetCurrentPosition", "Buffer::SetFormat", "Buffer::SetVolume", "Buffer::SetPan",
            "Buffer::SetFrequency", "Buffer::Stop", "Buffer::Unlock", "Buffer::Restore" };
        vt_buf = make_vt(bf, bfn, 21);
        obj_ds = gmalloc(8);
        WR32(obj_ds, vt_ds);
        dsound_init();
    }
    WR32(ARG(1), obj_ds);
    rt_log("DirectSoundCreate\n");
    RET(DS_OK, 3);
}
