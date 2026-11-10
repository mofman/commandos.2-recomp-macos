// DirectDraw (v1 interfaces) over guest-memory 16-bit 565 surfaces; primary is presented via the platform layer.
#include "../rt.h"
#include "../platform.h"
#include <string.h>

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_NOTFOUND 0x887600FFu
#define DDSD_CAPS 1
#define DDSD_HEIGHT 2
#define DDSD_WIDTH 4
#define DDSD_PITCH 8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_CKSRCBLT 0x10000
#define DDSCAPS_BACKBUFFER 0x4
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_VIDEOMEMORY 0x4000

typedef struct Surf {
    uint32_t guest;            // COM object address
    int w, h, pitch, bpp;
    uint32_t mem;              // guest pixel memory
    uint32_t caps;
    struct Surf *back;         // attached back buffer (primary only)
    int primary;
    uint32_t ck_lo, ck_hi; int has_ck;
} Surf;

static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;
static Surf *g_primary;

// ------------------------------------------------------------------ desc helpers
static void write_pixfmt(uint32_t p, int bpp) {
    memset(gptr(p), 0, 32);
    g_wr32(p, 32);
    g_wr32(p + 4, 0x40);           // DDPF_RGB
    g_wr32(p + 12, bpp);
    if (bpp == 16) { g_wr32(p + 16, 0xF800); g_wr32(p + 20, 0x07E0); g_wr32(p + 24, 0x001F); }
    else if (bpp == 32) { g_wr32(p + 16, 0xFF0000); g_wr32(p + 20, 0xFF00); g_wr32(p + 24, 0xFF); }
}
static void write_desc(Surf *s, uint32_t d, int with_ptr) {
    uint32_t size = g_rd32(d);
    if (size < 108) size = 108;
    memset(gptr(d), 0, size);
    g_wr32(d, size);
    g_wr32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | (with_ptr ? DDSD_LPSURFACE : 0));
    g_wr32(d + 8, s->h);
    g_wr32(d + 12, s->w);
    g_wr32(d + 16, s->pitch);
    if (s->back) g_wr32(d + 20, 1);
    if (with_ptr) g_wr32(d + 36, s->mem);
    if (s->has_ck) { g_wr32(d + 64, s->ck_lo); g_wr32(d + 68, s->ck_hi); }
    write_pixfmt(d + 72, s->bpp);
    g_wr32(d + 104, s->caps);
}

static void present_primary(void) {
    Surf *p = g_primary;
    if (!p) return;
    plat_present(gptr(p->mem), p->pitch, p->w, p->h);
}

// ------------------------------------------------------------------ surface methods
static uint32_t new_surface(int w, int h, uint32_t caps);

static void S_Release(Cpu *cpu) {
    uint32_t o = THIS; uint32_t r = g_rd32(o + 4); if (r) r--; g_wr32(o + 4, r);
    // memory is not reclaimed (surfaces are few and long-lived)
    RET(r, 1);
}
static void S_GetSurfaceDesc(Cpu *cpu) { write_desc(HOST(Surf), ARG(1), 0); RET(DD_OK, 2); }
static void S_GetPixelFormat(Cpu *cpu) { write_pixfmt(ARG(1), HOST(Surf)->bpp); RET(DD_OK, 2); }
static void S_GetCaps(Cpu *cpu) { g_wr32(ARG(1), HOST(Surf)->caps); RET(DD_OK, 2); }
static void S_Lock(Cpu *cpu) {
    Surf *s = HOST(Surf);
    uint32_t rect = ARG(1), d = ARG(2);
    write_desc(s, d, 1);
    if (rect) {
        int l = (int)g_rd32(rect), t = (int)g_rd32(rect + 4);
        g_wr32(d + 36, s->mem + t * s->pitch + l * (s->bpp / 8));
    }
    RET(DD_OK, 5);
}
static void S_Unlock(Cpu *cpu) {
    Surf *s = HOST(Surf);
    if (s->primary && !s->back) present_primary();
    RET(DD_OK, 2);
}
static void S_IsLost(Cpu *cpu) { RET(DD_OK, 1); }
static void S_Restore(Cpu *cpu) { RET(DD_OK, 1); }
static void S_SetClipper(Cpu *cpu) { RET(DD_OK, 2); }
static void S_SetPalette(Cpu *cpu) { RET(DD_OK, 2); }
static void S_GetBltStatus(Cpu *cpu) { RET(DD_OK, 2); }
static void S_GetFlipStatus(Cpu *cpu) { RET(DD_OK, 2); }
static void S_SetColorKey(Cpu *cpu) {
    Surf *s = HOST(Surf);
    if (ARG(2)) { s->ck_lo = g_rd32(ARG(2)); s->ck_hi = g_rd32(ARG(2) + 4); s->has_ck = 1; }
    else s->has_ck = 0;
    RET(DD_OK, 3);
}
static void S_GetColorKey(Cpu *cpu) {
    Surf *s = HOST(Surf);
    if (!s->has_ck) RET(0x887600D2u, 3);   // DDERR_NOCOLORKEY
    g_wr32(ARG(2), s->ck_lo); g_wr32(ARG(2) + 4, s->ck_hi);
    RET(DD_OK, 3);
}
static void S_GetAttachedSurface(Cpu *cpu) {
    Surf *s = HOST(Surf);
    if (!s->back) RET(DDERR_NOTFOUND, 3);
    g_wr32(ARG(2), s->back->guest);
    RET(DD_OK, 3);
}
static void S_Flip(Cpu *cpu) {
    Surf *s = HOST(Surf);
    if (s->back) {
        uint32_t t = s->mem; s->mem = s->back->mem; s->back->mem = t;
        present_primary();
    }
    RET(DD_OK, 3);
}

static void get_rect(uint32_t r, Surf *s, int *l, int *t, int *rr, int *b) {
    if (r) { *l = (int)g_rd32(r); *t = (int)g_rd32(r + 4); *rr = (int)g_rd32(r + 8); *b = (int)g_rd32(r + 12); }
    else { *l = 0; *t = 0; *rr = s->w; *b = s->h; }
}

static void blit(Surf *d, int dl, int dt, int dr, int db, Surf *s, int sl, int st, int sr, int sb, int use_ck) {
    int dw = dr - dl, dh = db - dt, sw = sr - sl, sh = sb - st;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    uint8_t *dp = gptr(d->mem), *sp = gptr(s->mem);
    uint16_t ck = (uint16_t)s->ck_lo;
    for (int y = 0; y < dh; y++) {
        int dy = dt + y; if (dy < 0 || dy >= d->h) continue;
        int sy = st + (dw == sw && dh == sh ? y : y * sh / dh); if (sy < 0 || sy >= s->h) continue;
        uint16_t *drow = (uint16_t *)(dp + dy * d->pitch), *srow = (uint16_t *)(sp + sy * s->pitch);
        if (dw == sw && !use_ck && dl >= 0 && dr <= d->w && sl >= 0 && sr <= s->w) {
            memmove(drow + dl, srow + sl, dw * 2);
            continue;
        }
        for (int x = 0; x < dw; x++) {
            int dx = dl + x; if (dx < 0 || dx >= d->w) continue;
            int sx = sl + (dw == sw ? x : x * sw / dw); if (sx < 0 || sx >= s->w) continue;
            uint16_t v = srow[sx];
            if (use_ck && v == ck) continue;
            drow[dx] = v;
        }
    }
}

static void S_Blt(Cpu *cpu) {
    // (this, lpDestRect, lpSrcSurface, lpSrcRect, dwFlags, lpDDBltFx)
    Surf *d = HOST(Surf);
    uint32_t flags = ARG(4), fx = ARG(5);
    int dl, dt, dr, db;
    get_rect(ARG(1), d, &dl, &dt, &dr, &db);
    if (flags & 0x400) {   // DDBLT_COLORFILL
        uint16_t c = (uint16_t)g_rd32(fx + 80);
        for (int y = dt; y < db && y < d->h; y++) {
            if (y < 0) continue;
            uint16_t *row = (uint16_t *)gptr(d->mem + y * d->pitch);
            for (int x = dl < 0 ? 0 : dl; x < dr && x < d->w; x++) row[x] = c;
        }
    } else if (ARG(2)) {
        Surf *s = com_host(ARG(2));
        int sl, st, sr, sb;
        get_rect(ARG(3), s, &sl, &st, &sr, &sb);
        blit(d, dl, dt, dr, db, s, sl, st, sr, sb, (flags & 0x8000) && s->has_ck);   // DDBLT_KEYSRC
    }
    if (d->primary && !d->back) present_primary();
    RET(DD_OK, 6);
}
static void S_BltFast(Cpu *cpu) {
    // (this, x, y, src, srcRect, trans)
    Surf *d = HOST(Surf), *s = com_host(ARG(3));
    int x = (int)ARG(1), y = (int)ARG(2);
    int sl, st, sr, sb;
    get_rect(ARG(4), s, &sl, &st, &sr, &sb);
    blit(d, x, y, x + sr - sl, y + sb - st, s, sl, st, sr, sb, (ARG(5) & 1) && s->has_ck);   // DDBLTFAST_SRCCOLORKEY
    if (d->primary && !d->back) present_primary();
    RET(DD_OK, 6);
}
static void S_GetDC(Cpu *cpu) { g_wr32(ARG(1), 0x20002); RET(DD_OK, 2); }
static void S_ReleaseDC(Cpu *cpu) { RET(DD_OK, 2); }
static void S_AddAttachedSurface(Cpu *cpu) { RET(DD_OK, 2); }

static const ComMethod SURF_VT[] = {
    {"QueryInterface", 0}, {"AddRef", 0}, {"Release", S_Release}, {"AddAttachedSurface", S_AddAttachedSurface},
    {"AddOverlayDirtyRect", 0}, {"Blt", S_Blt}, {"BltBatch", 0}, {"BltFast", S_BltFast},
    {"DeleteAttachedSurface", 0}, {"EnumAttachedSurfaces", 0}, {"EnumOverlayZOrders", 0}, {"Flip", S_Flip},
    {"GetAttachedSurface", S_GetAttachedSurface}, {"GetBltStatus", S_GetBltStatus}, {"GetCaps", S_GetCaps},
    {"GetClipper", 0}, {"GetColorKey", S_GetColorKey}, {"GetDC", S_GetDC}, {"GetFlipStatus", S_GetFlipStatus},
    {"GetOverlayPosition", 0}, {"GetPalette", 0}, {"GetPixelFormat", S_GetPixelFormat},
    {"GetSurfaceDesc", S_GetSurfaceDesc}, {"Initialize", 0}, {"IsLost", S_IsLost}, {"Lock", S_Lock},
    {"ReleaseDC", S_ReleaseDC}, {"Restore", S_Restore}, {"SetClipper", S_SetClipper}, {"SetColorKey", S_SetColorKey},
    {"SetOverlayPosition", 0}, {"SetPalette", S_SetPalette}, {"Unlock", S_Unlock}, {"UpdateOverlay", 0},
    {"UpdateOverlayDisplay", 0}, {"UpdateOverlayZOrder", 0},
};

static uint32_t new_surface(int w, int h, uint32_t caps) {
    Surf *s = calloc(1, sizeof *s);
    s->w = w; s->h = h; s->bpp = 16;
    s->pitch = (w * 2 + 7) & ~7;
    s->caps = caps;
    s->mem = galloc(s->pitch * h + 64);
    s->guest = com_new("IDirectDrawSurface", SURF_VT, sizeof SURF_VT / sizeof *SURF_VT, s);
    return s->guest;
}

// ------------------------------------------------------------------ IDirectDraw
static void DD_SetCooperativeLevel(Cpu *cpu) { TRACE("DD SetCooperativeLevel(%x)", ARG(2)); RET(DD_OK, 3); }
static void DD_SetDisplayMode(Cpu *cpu) {
    g_mode_w = (int)ARG(1); g_mode_h = (int)ARG(2); g_mode_bpp = (int)ARG(3);
    LOG("DD SetDisplayMode %dx%dx%d", g_mode_w, g_mode_h, g_mode_bpp);
    if (getenv("C2_MODETRACE")) { void rt_host_bt(void); rt_host_bt(); }
    plat_set_mode(g_mode_w, g_mode_h);
    RET(DD_OK, 4);
}
static void DD_RestoreDisplayMode(Cpu *cpu) { RET(DD_OK, 1); }
static void DD_GetDisplayMode(Cpu *cpu) {
    Surf tmp = {0}; tmp.w = g_mode_w; tmp.h = g_mode_h; tmp.bpp = g_mode_bpp; tmp.pitch = g_mode_w * 2;
    write_desc(&tmp, ARG(1), 0);
    RET(DD_OK, 2);
}
static void DD_EnumDisplayModes(Cpu *cpu) {
    // (this, flags, lpDDSurfaceDesc, lpContext, lpEnumModesCallback)
    int modes[5 + 16][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}};
    int rt_video_mode_count(void); void rt_video_mode(int, int *, int *);
    int nmodes = 5 + rt_video_mode_count();
    for (int k = 5; k < nmodes; k++) rt_video_mode(k - 5, &modes[k][0], &modes[k][1]);
    uint32_t ctx = ARG(3), cb = ARG(4);
    uint32_t d = galloc(108);
    uint32_t esp = cpu->esp;
    for (int i = 0; i < nmodes; i++) {
        for (int bpp = 16; bpp <= 32; bpp += 16) {
            Surf tmp = {0}; tmp.w = modes[i][0]; tmp.h = modes[i][1]; tmp.bpp = bpp; tmp.pitch = tmp.w * bpp / 8;
            g_wr32(d, 108);
            write_desc(&tmp, d, 0);
            uint32_t r = call_guest(cpu, cb, 0, 2, d, ctx);
            if (!r) goto done;
        }
    }
done:
    cpu->esp = esp;
    RET(DD_OK, 5);
}
static void DD_CreateSurface(Cpu *cpu) {
    // (this, lpDDSurfaceDesc, lplpDDSurface, pUnkOuter)
    uint32_t d = ARG(1);
    uint32_t flags = g_rd32(d + 4), caps = (flags & DDSD_CAPS) ? g_rd32(d + 104) : 0;
    int w = (flags & DDSD_WIDTH) ? (int)g_rd32(d + 12) : g_mode_w;
    int h = (flags & DDSD_HEIGHT) ? (int)g_rd32(d + 8) : g_mode_h;
    uint32_t o;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        w = g_mode_w; h = g_mode_h;
        o = new_surface(w, h, caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY);
        Surf *s = com_host(o);
        s->primary = 1;
        g_primary = s;
        if ((flags & DDSD_BACKBUFFERCOUNT) && g_rd32(d + 20) >= 1) {
            uint32_t b = new_surface(w, h, DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY);
            s->back = com_host(b);
        }
        plat_set_mode(w, h);
        LOG("DD CreateSurface primary %dx%d backbuffer=%d", w, h, s->back != NULL);
    } else {
        o = new_surface(w, h, caps ? caps : DDSCAPS_OFFSCREENPLAIN);
        if (flags & DDSD_CKSRCBLT) { Surf *s = com_host(o); s->has_ck = 1; s->ck_lo = g_rd32(d + 64); s->ck_hi = g_rd32(d + 68); }
        TRACE("DD CreateSurface %dx%d caps=%x", w, h, caps);
    }
    g_wr32(ARG(2), o);
    RET(DD_OK, 4);
}
static void DD_GetCaps(Cpu *cpu) {
    // (this, lpDDDriverCaps, lpDDHELCaps)
    for (int k = 1; k <= 2; k++) {
        uint32_t c = ARG(k);
        if (!c) continue;
        uint32_t size = g_rd32(c);
        memset(gptr(c + 4), 0, size - 4);
        g_wr32(c + 4, 0x40 | 0x40000 | 0x200000);   // DDCAPS_BLT | DDCAPS_COLORKEY | DDCAPS_BLTCOLORFILL... (approximate)
        g_wr32(c + 0x3C, 256u << 20);              // dwVidMemTotal
        g_wr32(c + 0x40, 256u << 20);              // dwVidMemFree
    }
    RET(DD_OK, 3);
}
static void DD_WaitForVerticalBlank(Cpu *cpu) { RET(DD_OK, 3); }
static void DD_FlipToGDISurface(Cpu *cpu) { RET(DD_OK, 1); }

static void C_SetHWnd(Cpu *cpu) { RET(DD_OK, 3); }
static const ComMethod CLIP_VT[] = {
    {"QueryInterface", 0}, {"AddRef", 0}, {"Release", 0}, {"GetClipList", 0}, {"GetHWnd", 0},
    {"Initialize", 0}, {"IsClipListChanged", 0}, {"SetClipList", 0}, {"SetHWnd", C_SetHWnd},
};
static void DD_CreateClipper(Cpu *cpu) {
    g_wr32(ARG(2), com_new("IDirectDrawClipper", CLIP_VT, sizeof CLIP_VT / sizeof *CLIP_VT, NULL));
    RET(DD_OK, 4);
}

static const ComMethod DD_VT[] = {
    {"QueryInterface", 0}, {"AddRef", 0}, {"Release", 0}, {"Compact", 0}, {"CreateClipper", DD_CreateClipper},
    {"CreatePalette", 0}, {"CreateSurface", DD_CreateSurface}, {"DuplicateSurface", 0},
    {"EnumDisplayModes", DD_EnumDisplayModes}, {"EnumSurfaces", 0}, {"FlipToGDISurface", DD_FlipToGDISurface},
    {"GetCaps", DD_GetCaps}, {"GetDisplayMode", DD_GetDisplayMode}, {"GetFourCCCodes", 0}, {"GetGDISurface", 0},
    {"GetMonitorFrequency", 0}, {"GetScanLine", 0}, {"GetVerticalBlankStatus", 0}, {"Initialize", 0},
    {"RestoreDisplayMode", DD_RestoreDisplayMode}, {"SetCooperativeLevel", DD_SetCooperativeLevel},
    {"SetDisplayMode", DD_SetDisplayMode}, {"WaitForVerticalBlank", DD_WaitForVerticalBlank},
};

SHIM(DDRAW, DirectDrawCreate) {
    LOG("DirectDrawCreate");
    g_wr32(ARG(1), com_new("IDirectDraw", DD_VT, sizeof DD_VT / sizeof *DD_VT, NULL));
    RET(DD_OK, 3);
}
