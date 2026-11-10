// DirectSound: secondary buffers live in guest memory, mixed in software to the SDL audio stream.
#include "../rt.h"
#include "../platform.h"
#include <string.h>
#include <math.h>

#define DS_OK 0
#define MIX_RATE 44100
#define MAXBUF 512

typedef struct Notify { uint32_t ofs, event; } Notify;
typedef struct Buf {
    uint32_t guest;
    int primary;
    uint32_t mem, size;           // guest data
    int channels, bits, rate, block;
    uint32_t freq;                // current playback frequency
    int32_t volume, pan;          // hundredths of dB
    int playing, looping;
    uint64_t pos;                 // byte position << 16 (fixed point in bytes)
    Notify notify[64]; int nnotify;
    float gl, gr;                 // cached gains
    int active;
} Buf;

static Buf *g_bufs[MAXBUF];
static int g_nbufs;

void rt_event_set(uint32_t h);

static void update_gain(Buf *b) {
    float v = b->volume <= -10000 ? 0.f : powf(10.f, b->volume / 2000.f);
    float l = 1.f, r = 1.f;
    if (b->pan < 0) r = powf(10.f, b->pan / 2000.f);
    else if (b->pan > 0) l = powf(10.f, -b->pan / 2000.f);
    b->gl = v * l; b->gr = v * r;
}

static void fire_crossed(Buf *b, uint32_t from, uint32_t to, int wrapped) {
    for (int i = 0; i < b->nnotify; i++) {
        uint32_t o = b->notify[i].ofs;
        if (o == 0xFFFFFFFFu) continue;
        if ((!wrapped && o >= from && o < to) || (wrapped && (o >= from || o < to))) {
            if (getenv("C2_TRACE")) fprintf(stderr, "[mix] notify ev %x (buf %u, ofs %u, %u->%u wrapped=%d)\n", b->notify[i].event, b->size, o, from, to, wrapped);
            rt_event_set(b->notify[i].event);
        }
    }
}
static void fire_stop(Buf *b) {
    for (int i = 0; i < b->nnotify; i++) if (b->notify[i].ofs == 0xFFFFFFFFu) rt_event_set(b->notify[i].event);
}

static void mix(int16_t *out, int frames) {
    static int32_t acc[2 * 8192];
    memset(acc, 0, frames * 2 * sizeof(int32_t));
    for (int k = 0; k < g_nbufs; k++) {
        Buf *b = g_bufs[k];
        if (!b || !b->active || b->primary || !b->playing || !b->size) continue;
        const uint8_t *d = gptr(b->mem);
        uint64_t step = ((uint64_t)b->freq << 16) / MIX_RATE;   // frames << 16 per output frame
        uint32_t frames_total = b->size / b->block;
        uint64_t fpos = b->pos;                                   // in frames << 16
        uint32_t start_byte = (uint32_t)(fpos >> 16) * b->block;
        int wrapped = 0;
        int32_t gl = (int32_t)(b->gl * 256), gr = (int32_t)(b->gr * 256);
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)(fpos >> 16);
            if (f >= frames_total) {
                if (b->looping) { fpos -= (uint64_t)frames_total << 16; f -= frames_total; wrapped = 1; }
                else { b->playing = 0; fire_stop(b); break; }
            }
            int32_t l, r;
            const uint8_t *s = d + f * b->block;
            if (b->bits == 16) {
                l = (int16_t)(s[0] | s[1] << 8);
                r = b->channels == 2 ? (int16_t)(s[2] | s[3] << 8) : l;
            } else {
                l = ((int)s[0] - 128) << 8;
                r = b->channels == 2 ? ((int)s[1] - 128) << 8 : l;
            }
            acc[2 * i] += (l * gl) >> 8;
            acc[2 * i + 1] += (r * gr) >> 8;
            fpos += step;
        }
        b->pos = fpos;
        uint32_t end_byte = ((uint32_t)(fpos >> 16) % (frames_total ? frames_total : 1)) * b->block;
        if (b->nnotify) fire_crossed(b, start_byte, end_byte, wrapped || end_byte < start_byte);
    }
    for (int i = 0; i < frames * 2; i++) {
        int32_t v = acc[i];
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

// ------------------------------------------------------------------ buffer methods
static void B_GetCaps(Cpu *cpu) {
    Buf *b = HOST(Buf);
    uint32_t c = ARG(1);
    g_wr32(c + 4, b->primary ? 1 : 0); g_wr32(c + 8, b->size); g_wr32(c + 12, 0); g_wr32(c + 16, 0);
    RET(DS_OK, 2);
}
static uint32_t cur_byte(Buf *b) {
    uint32_t frames_total = b->size / (b->block ? b->block : 1);
    if (!frames_total) return 0;
    return ((uint32_t)(b->pos >> 16) % frames_total) * b->block;
}
static void B_GetCurrentPosition(Cpu *cpu) {
    Buf *b = HOST(Buf);
    static int n; if (b->size > 100000 && n++ < 20) TRACE("DS GetCurrentPosition big buf -> %u (caller %08x)", cur_byte(b), g_rd32(cpu->esp));
    plat_audio_lock();
    uint32_t p = cur_byte(b);
    plat_audio_unlock();
    if (ARG(1)) g_wr32(ARG(1), p);
    if (ARG(2)) g_wr32(ARG(2), b->size ? (p + b->block * 1024) % b->size : 0);   // write cursor slightly ahead
    RET(DS_OK, 3);
}
static void B_GetFormat(Cpu *cpu) {
    Buf *b = HOST(Buf);
    uint32_t w = ARG(1);
    if (w) {
        g_wr16(w, 1); g_wr16(w + 2, b->channels); g_wr32(w + 4, b->rate); g_wr32(w + 8, b->rate * b->block);
        g_wr16(w + 12, b->block); g_wr16(w + 14, b->bits);
        if (ARG(2) >= 18) g_wr16(w + 16, 0);
    }
    if (ARG(3)) g_wr32(ARG(3), 18);
    RET(DS_OK, 4);
}
static void B_GetVolume(Cpu *cpu) { g_wr32(ARG(1), HOST(Buf)->volume); RET(DS_OK, 2); }
static void B_GetPan(Cpu *cpu) { g_wr32(ARG(1), HOST(Buf)->pan); RET(DS_OK, 2); }
static void B_GetFrequency(Cpu *cpu) { g_wr32(ARG(1), HOST(Buf)->freq); RET(DS_OK, 2); }
static void B_GetStatus(Cpu *cpu) {
    Buf *b = HOST(Buf);
    g_wr32(ARG(1), b->playing ? (1 | (b->looping ? 4 : 0)) : 0);
    RET(DS_OK, 2);
}
static void B_Lock(Cpu *cpu) {
    // (this, offset, bytes, ptr1*, len1*, ptr2*, len2*, flags)
    Buf *b = HOST(Buf);
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 1) off = cur_byte(b);          // DSBLOCK_FROMWRITECURSOR
    if (flags & 2) n = b->size;                // DSBLOCK_ENTIREBUFFER
    if (!b->size) { g_wr32(ARG(3), 0); g_wr32(ARG(4), 0); RET(DS_OK, 8); }
    off %= b->size;
    if (n > b->size) n = b->size;
    uint32_t l1 = off + n > b->size ? b->size - off : n;
    if (b->size > 60000) TRACE("DS Lock buf%u off=%u n=%u flags=%x play=%u", b->size, off, n, flags, cur_byte(b));
    g_wr32(ARG(3), b->mem + off); g_wr32(ARG(4), l1);
    if (ARG(5)) g_wr32(ARG(5), l1 < n ? b->mem : 0);
    if (ARG(6)) g_wr32(ARG(6), n - l1);
    RET(DS_OK, 8);
}
static void B_Unlock(Cpu *cpu) { RET(DS_OK, 5); }
static void B_Play(Cpu *cpu) {
    Buf *b = HOST(Buf);
    TRACE("DS Play size=%u flags=%x caller %08x", b->size, ARG(3), g_rd32(cpu->esp));
    plat_audio_lock();
    b->playing = 1; b->looping = ARG(3) & 1;
    plat_audio_unlock();
    RET(DS_OK, 4);
}
static void B_Stop(Cpu *cpu) {
    Buf *b = HOST(Buf);
    TRACE("DS Stop size=%u pos=%u", b->size, cur_byte(b));
    plat_audio_lock();
    int was = b->playing;
    b->playing = 0;
    if (was) fire_stop(b);
    plat_audio_unlock();
    RET(DS_OK, 1);
}
static void B_SetCurrentPosition(Cpu *cpu) {
    Buf *b = HOST(Buf);
    plat_audio_lock();
    b->pos = (uint64_t)(ARG(1) / (b->block ? b->block : 1)) << 16;
    plat_audio_unlock();
    RET(DS_OK, 2);
}
static void B_SetFormat(Cpu *cpu) {
    Buf *b = HOST(Buf);
    uint32_t w = ARG(1);
    LOG("DS primary SetFormat %u Hz %u ch %u bits", g_rd32(w + 4), g_rd16(w + 2), g_rd16(w + 14));
    (void)b;
    RET(DS_OK, 2);
}
static void B_SetVolume(Cpu *cpu) { Buf *b = HOST(Buf); TRACE("DS SetVolume size=%u %d", b->size, (int32_t)ARG(1)); b->volume = (int32_t)ARG(1); update_gain(b); RET(DS_OK, 2); }
static void B_SetPan(Cpu *cpu) { Buf *b = HOST(Buf); TRACE("DS SetPan size=%u %d", b->size, (int32_t)ARG(1)); b->pan = (int32_t)ARG(1); update_gain(b); RET(DS_OK, 2); }
static void B_SetFrequency(Cpu *cpu) { Buf *b = HOST(Buf); TRACE("DS SetFrequency size=%u %u", b->size, ARG(1)); b->freq = ARG(1) ? ARG(1) : (uint32_t)b->rate; RET(DS_OK, 2); }
static void B_Restore(Cpu *cpu) { RET(DS_OK, 1); }
static void B_Release(Cpu *cpu) {
    uint32_t o = THIS; uint32_t r = g_rd32(o + 4); if (r) r--; g_wr32(o + 4, r);
    if (!r) {
        Buf *b = com_host(o);
        plat_audio_lock();
        b->active = 0; b->playing = 0;
        plat_audio_unlock();
    }
    RET(r, 1);
}

static void N_SetNotificationPositions(Cpu *cpu) {
    Buf *b = HOST(Buf);
    uint32_t n = ARG(1), arr = ARG(2);
    TRACE("DS SetNotificationPositions n=%u first ofs=%u ev=%x", n, n ? g_rd32(arr) : 0, n ? g_rd32(arr + 4) : 0);
    plat_audio_lock();
    b->nnotify = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        TRACE("   notify[%u] ofs=%u ev=%x", i, g_rd32(arr + 8 * i), g_rd32(arr + 8 * i + 4));
        b->notify[i].ofs = g_rd32(arr + 8 * i);
        b->notify[i].event = g_rd32(arr + 8 * i + 4);
        b->nnotify++;
    }
    plat_audio_unlock();
    RET(DS_OK, 3);
}
static const ComMethod NOTIFY_VT[] = {
    {"QueryInterface", 0}, {"AddRef", 0}, {"Release", 0}, {"SetNotificationPositions", N_SetNotificationPositions},
};

static void B_QueryInterface(Cpu *cpu) {
    uint32_t iid = ARG(1);
    if (g_rd32(iid) == 0xb0210783u) {   // IID_IDirectSoundNotify
        g_wr32(ARG(2), com_new("IDirectSoundNotify", NOTIFY_VT, 4, HOST(Buf)));
        RET(DS_OK, 3);
    }
    LOG("DSBuffer QI %08x unsupported", g_rd32(iid));
    g_wr32(ARG(2), 0);
    RET(E_NOINTERFACE, 3);
}

static const ComMethod BUF_VT[] = {
    {"QueryInterface", B_QueryInterface}, {"AddRef", 0}, {"Release", B_Release}, {"GetCaps", B_GetCaps},
    {"GetCurrentPosition", B_GetCurrentPosition}, {"GetFormat", B_GetFormat}, {"GetVolume", B_GetVolume},
    {"GetPan", B_GetPan}, {"GetFrequency", B_GetFrequency}, {"GetStatus", B_GetStatus}, {"Initialize", 0},
    {"Lock", B_Lock}, {"Play", B_Play}, {"SetCurrentPosition", B_SetCurrentPosition}, {"SetFormat", B_SetFormat},
    {"SetVolume", B_SetVolume}, {"SetPan", B_SetPan}, {"SetFrequency", B_SetFrequency}, {"Stop", B_Stop},
    {"Unlock", B_Unlock}, {"Restore", B_Restore},
};

static Buf *register_buf(Buf *b) {
    plat_audio_lock();
    int slot = -1;
    for (int i = 0; i < g_nbufs; i++) if (!g_bufs[i]->active) { slot = i; break; }
    if (slot < 0 && g_nbufs < MAXBUF) slot = g_nbufs++;
    if (slot >= 0) { if (g_bufs[slot] && g_bufs[slot] != b) free(g_bufs[slot]); g_bufs[slot] = b; }
    b->active = 1;
    plat_audio_unlock();
    b->guest = com_new("IDirectSoundBuffer", BUF_VT, sizeof BUF_VT / sizeof *BUF_VT, b);
    return b;
}

static void DS_CreateSoundBuffer(Cpu *cpu) {
    uint32_t desc = ARG(1);
    uint32_t flags = g_rd32(desc + 4), bytes = g_rd32(desc + 8), wfx = g_rd32(desc + 16);
    Buf *b = calloc(1, sizeof *b);
    b->primary = flags & 1;
    b->channels = 2; b->bits = 16; b->rate = MIX_RATE;
    if (wfx) { b->channels = g_rd16(wfx + 2); b->rate = (int)g_rd32(wfx + 4); b->bits = g_rd16(wfx + 14); }
    b->block = b->channels * b->bits / 8;
    if (!b->block) b->block = 1;
    b->freq = (uint32_t)b->rate;
    b->size = b->primary ? 0 : bytes;
    if (b->size) b->mem = galloc(b->size);
    update_gain(b);
    register_buf(b);
    TRACE("DS CreateSoundBuffer primary=%d %u bytes %d Hz %d ch %d bit", b->primary, b->size, b->rate, b->channels, b->bits);
    g_wr32(ARG(2), b->guest);
    RET(DS_OK, 4);
}
static void DS_DuplicateSoundBuffer(Cpu *cpu) {
    Buf *src = com_host(ARG(1));
    Buf *b = calloc(1, sizeof *b);
    *b = *src;
    b->playing = 0; b->pos = 0; b->nnotify = 0; b->volume = 0; b->pan = 0;
    update_gain(b);
    register_buf(b);
    g_wr32(ARG(2), b->guest);
    RET(DS_OK, 3);
}
static void DS_GetCaps(Cpu *cpu) {
    uint32_t c = ARG(1), sz = g_rd32(c);
    memset(gptr(c + 4), 0, sz - 4);
    g_wr32(c + 4, 0xF0F);   // continuous rate, mono/stereo, 8/16 bit, primary+secondary
    g_wr32(c + 8, 100); g_wr32(c + 12, 100000);
    RET(DS_OK, 2);
}
static void DS_SetCooperativeLevel(Cpu *cpu) { RET(DS_OK, 3); }
static void DS_GetSpeakerConfig(Cpu *cpu) { g_wr32(ARG(1), 4); RET(DS_OK, 2); }   // DSSPEAKER_STEREO
static void DS_SetSpeakerConfig(Cpu *cpu) { RET(DS_OK, 2); }
static void DS_Compact(Cpu *cpu) { RET(DS_OK, 1); }

static const ComMethod DS_VT[] = {
    {"QueryInterface", 0}, {"AddRef", 0}, {"Release", 0}, {"CreateSoundBuffer", DS_CreateSoundBuffer},
    {"GetCaps", DS_GetCaps}, {"DuplicateSoundBuffer", DS_DuplicateSoundBuffer},
    {"SetCooperativeLevel", DS_SetCooperativeLevel}, {"Compact", DS_Compact},
    {"GetSpeakerConfig", DS_GetSpeakerConfig}, {"SetSpeakerConfig", DS_SetSpeakerConfig}, {"Initialize", 0},
};

SHIM(DSOUND, ord1) {   // DirectSoundCreate(guid, lplpDS, outer)
    LOG("DirectSoundCreate");
    plat_audio_start(mix, MIX_RATE);
    g_wr32(ARG(1), com_new("IDirectSound", DS_VT, sizeof DS_VT / sizeof *DS_VT, NULL));
    RET(DS_OK, 3);
}
