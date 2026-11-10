// High frame rate with interpolation, ported from Commandos2-ModernPatch (Hook_Update / Hook_Frame / Hook_EndIter /
// Hook_VisorDraw). The game's logic runs at 25 ticks per second; between two ticks extra frames are drawn with the
// camera interpolated between the last two ticks' positions, on one steady clock of 1000 / MaxFps ms.
//
// 1.2 WinMain 0x402200, loop 0x4031ae..0x40347d (WinMain's esp in the loop = "wm"):
//   0x40323a  call CJuego::Update 0x4523d0           [Steam 0x405239 -> 0x7956f0]   site hook_update
//   0x4032a9  call CJuego::Frame 0x4526f0 (canvas)   [Steam 0x40529b -> 0x792e10]   site hook_frame
//   0x403465  call canvas reset 0x420f60             [Steam 0x40535c -> 0x7cacd0]   site hook_enditer
// World viewer draw 0x520930 (vtable slot, CVentanaVisorMundo)  [Steam vtbl 0xAF37C4[2]]   override f_00520930
//
// Differences from the Windows patch: extra frames only in missions / inventory (no menu smoothing: the retail arrow
// is moved by DirectInput deltas once per tick, so menus would gain nothing); no object interpolation yet.
#include "hook.h"
#include "../platform.h"
#include <math.h>
#include <time.h>

// ---- comm2.exe 1.2 addresses
#define G_JUEGO        0xb58488u
#define G_MISION       0xb5a650u
#define G_FRAMEADAPT   0xb0925cu   // byte: adaptive frame skipping       [0xBB432C]
#define G_DREWFRAME    0xb09268u   // byte: this iteration was drawn      [0xBB432D]
#define G_SKIPPRESENT  0xb5a648u   // byte                                [0xBD57E4]
#define J_STATE        0x2c
#define J_FPSLIMITED   0xc4
#define J_TRANSITION   0xec
#define M_TARGETFPS    0x5b0
#define WM_LOCKINFO    0x18
#define WM_NODRAW      0x17
#define WM_CANVAS      0x44
#define WM_VIDEO       0x90
#define WM_HAVEVIDEO   0x9c
#define WM_SURFINFO    0x11c
#define VM_CAMERA      0xdc
#define CAM_X          0x68
#define CAM_Y          0x6c
#define CAM_ZOOM       0x74

void hdmenus_after_update(Cpu *cpu);

// ---- state
static uint32_t g_tick;
static double g_tick_time, g_tick_interval = 40.0;
static int g_in_game;
static float g_alpha = 1.0f;
static int g_interpolating;
static int g_in_extra_frame;
static uint32_t g_last_mission;
static double g_next_frame_slot;
static float g_tick_frame_alpha;
static int g_max_fps;
static int g_stat_frames, g_stat_ticks;
static double g_stat_start;

static double now_ms(void) { return plat_time_ms_f(); }
static void wait_until(double t) {
    for (;;) {
        double d = t - now_ms();
        if (d <= 0) return;
        if (d > 1.5) { struct timespec ts = { 0, (long)((d - 1.0) * 1e6) }; nanosleep(&ts, NULL); }
        // last ~1 ms: spin
    }
}

static int active(void) { return mod_cfg()->enabled; }
static int max_fps(void) {
    if (!g_max_fps) {
        int v = mod_cfg()->max_fps;
        if (v <= 0) v = plat_refresh_hz();
        g_max_fps = v < 25 ? 25 : v > 1000 ? 1000 : v;
        LOG("modpatch: frame rate %d (MaxFps=%d)", g_max_fps, mod_cfg()->max_fps);
    }
    return g_max_fps;
}

static int is_in_game(void) {
    uint32_t j = g_rd32(G_JUEGO), mis = g_rd32(G_MISION);
    if (!j || !mis) return 0;
    int state = (int)g_rd32(j + J_STATE);
    if (state != 1 && state != 7) return 0;     // 1 = mission, 7 = inventory
    int tr = (int)g_rd32(j + J_TRANSITION);
    return tr != 1 && tr != 2;
}

static float target_fps(void) {
    uint32_t mis = g_rd32(G_MISION);
    if (mis) { int f = (int)g_rd32(mis + M_TARGETFPS); if (f > 0 && f <= 1000) return (float)f; }
    return 25.0f;
}

// ---- camera interpolation state (per camera object)
typedef struct {
    uint32_t bam;
    float m[9];          // +0x38 +0x3C +0x44 +0x48 +0x50 +0x54 +0x40 +0x4C +0x58
    float pos[2], scale, t[2];
} CamRot;
static const int kRotOffs[9] = { 0x38, 0x3c, 0x44, 0x48, 0x50, 0x54, 0x40, 0x4c, 0x58 };
typedef struct { uint32_t cam, tick; float prev[3], cur[3]; CamRot rprev, rcur; } CamState;
static CamState g_cams[16];
static int g_ncams;
static void reset_cams(void) { g_ncams = 0; }
static CamState *cam_state(uint32_t cam) {
    for (int i = 0; i < g_ncams; i++) if (g_cams[i].cam == cam) return &g_cams[i];
    if (g_ncams == 16) g_ncams = 0;
    CamState *s = &g_cams[g_ncams++];
    s->cam = cam; s->tick = 0xFFFFFFFFu;
    return s;
}

// ---- hooks

// site 0x40323a: CJuego::Update (thiscall, no args)
void hook_update(Cpu *restrict cpu) {
    f_004523d0(cpu);
    uint32_t eax = cpu->eax;
    hdmenus_after_update(cpu);

    uint32_t mis = g_rd32(G_MISION);
    if (mis != g_last_mission) { reset_cams(); g_last_mission = mis; }
    int in_game = active() && is_in_game();
    if (in_game != g_in_game) {
        reset_cams(); g_in_game = in_game;
        uint32_t j = g_rd32(G_JUEGO);
        LOG("modpatch: in game: %d (state %d)", in_game, j ? (int)g_rd32(j + J_STATE) : -1);
    }
    if (in_game) {
        g_mem[G_FRAMEADAPT] = 0;     // the game's adaptive frame skipping fights with our pacing
        g_tick_interval = 1000.0 / target_fps();
        double now = now_ms();
        double next = g_tick_time + g_tick_interval;
        g_tick_time = (now - next > g_tick_interval || next > now + g_tick_interval) ? now : next;
        g_tick++;
        g_stat_ticks++;
        // the tick's own frame waits for its slot on the steady frame clock
        const double frame_ms = 1000.0 / max_fps();
        now = now_ms();
        if (g_next_frame_slot < now - frame_ms || g_next_frame_slot > now + frame_ms) g_next_frame_slot = now;
        if (g_next_frame_slot > now) { wait_until(g_next_frame_slot); now = g_next_frame_slot; }
        double a = (now - g_tick_time) / g_tick_interval;
        g_tick_frame_alpha = (float)(a < 0 ? 0 : a > 1 ? 1 : a);
        g_next_frame_slot = now + frame_ms;
    }
    cpu->eax = eax;
}

// site 0x4032a9: CJuego::Frame (thiscall, canvas)
void hook_frame(Cpu *restrict cpu) {
    if (!g_in_game) { f_004526f0(cpu); return; }
    uint32_t j = cpu->ecx;
    uint8_t limited = g_mem[j + J_FPSLIMITED];
    g_mem[j + J_FPSLIMITED] = 0;           // we do the pacing
    g_alpha = g_tick_frame_alpha;
    g_interpolating = 1;
    f_004526f0(cpu);
    g_interpolating = 0;
    g_mem[j + J_FPSLIMITED] = limited;
    g_stat_frames++;
}

// ---- diagnostics (C2_DIAG=N): diff guest memory across the Nth extra frame to find state that advances per draw
uint32_t rt_heap_top(void);
uint32_t rt_va_top(void);
static int g_extra_count;
static uint8_t *g_snap;
static void diag_ranges(uint32_t *lo, uint32_t *hi, int *n) {   // 3 ranges
    lo[0] = 0xb02000; hi[0] = 0xba94cc;            // .data
    lo[1] = 0x10000000; hi[1] = rt_heap_top();      // guest heap
    lo[2] = 0x70000000; hi[2] = rt_va_top();        // VirtualAlloc area (the game's own pools)
    *n = 3;
}
static void diag_before(void) {
    uint32_t lo[3], hi[3]; int n; diag_ranges(lo, hi, &n);
    size_t tot = 0; for (int i = 0; i < n; i++) tot += hi[i] - lo[i];
    g_snap = malloc(tot);
    LOG("diag: snapshot %zu MB (heap top %08x, va top %08x)", tot >> 20, hi[1], hi[2]);
    if (!g_snap) { LOG("diag: out of memory"); return; }
    size_t o = 0; for (int i = 0; i < n; i++) { memcpy(g_snap + o, g_mem + lo[i], hi[i] - lo[i]); o += hi[i] - lo[i]; }
}
static uint32_t owner_vtable(uint32_t a, uint32_t *obj) {
    for (uint32_t p = a & ~3u, k = 0; k < 0x800 && p >= 0x10000000; p -= 4, k += 4) {
        uint32_t v = g_rd32(p);
        if (v >= 0xae6000 && v < 0xb02000 && g_rd32(v) >= 0x401000 && g_rd32(v) < 0xae5db1) { *obj = p; return v; }
    }
    *obj = 0; return 0;
}
static void diag_after(void) {
    if (!g_snap) return;
    uint32_t lo[3], hi[3]; int n; diag_ranges(lo, hi, &n);
    // pass 1: count changed dwords per 16 KB page (pixel buffers change in many places)
    static uint16_t cnt[0x40000];   // 16 KB pages of the 4 GB guest space
    memset(cnt, 0, sizeof cnt);
    size_t o = 0;
    for (int r = 0; r < n; r++) {
        for (uint32_t a = lo[r]; a < hi[r]; a += 4) {
            uint32_t before; memcpy(&before, g_snap + o + (a - lo[r]), 4);
            if (before != g_rd32(a) && cnt[a >> 14] < 65535) cnt[a >> 14]++;
        }
        o += hi[r] - lo[r];
    }
    for (uint32_t pg = 0; pg < 0x40000; pg++) {
        if (cnt[pg] <= 24) continue;
        LOG("diag: busy page %08x: %d dwords changed", pg << 14, cnt[pg]);
        if (getenv("C2_DIAG_N")) continue;
        // first few changes of the page (object state may sit just before an image buffer)
        int k = 0;
        for (int r = 0; r < n && k < 4; r++) {
            size_t ro = 0; for (int q = 0; q < r; q++) ro += hi[q] - lo[q];
            for (uint32_t a = pg << 14; a < ((pg + 1) << 14) && k < 4; a += 4) {
                if (a < lo[r] || a >= hi[r]) continue;
                uint32_t b; memcpy(&b, g_snap + ro + (a - lo[r]), 4);
                if (b != g_rd32(a)) { LOG("diag:     %08x %08x -> %08x", a, b, g_rd32(a)); k++; }
            }
        }
    }
    o = 0; int shown = 0;
    for (int r = 0; r < n; r++) {
        for (uint32_t a = lo[r]; a < hi[r]; a += 4) {
            uint32_t before; memcpy(&before, g_snap + o + (a - lo[r]), 4);
            uint32_t now = g_rd32(a);
            int ptrish = (before >> 20) == (a >> 20) && (now >> 20) == (a >> 20);   // list links within the same heap area
            if (before == now || (cnt[a >> 14] > 24 && (a < 0x1b1bc000 || a >= 0x1b1c0000)) || ptrish) continue;
            if (shown < 300) {
                uint32_t obj, vt = owner_vtable(a, &obj);
                LOG("diag: %08x %08x -> %08x  (page changes %d)  owner vt %06x obj %08x +0x%x", a, before, now, cnt[a >> 14], vt, obj, obj ? a - obj : 0);
                shown++;
            }
        }
        o += hi[r] - lo[r];
    }
    // who points into the changed image buffers (C2_DIAG_PTR=hex address)
    if (getenv("C2_DIAG_PTR")) {
        uint32_t t = (uint32_t)strtoul(getenv("C2_DIAG_PTR"), NULL, 16), best = 0, where = 0;
        for (int r = 0; r < n; r++)
            for (uint32_t a = lo[r]; a < hi[r]; a += 4) { uint32_t v = g_rd32(a); if (v <= t && v > t - 0x80000 && v > best) { best = v; where = a; } }
        uint32_t obj, vt = owner_vtable(where, &obj);
        LOG("diag: nearest pointer below %08x is %08x stored at %08x (owner vt %06x obj %08x +0x%x)", t, best, where, vt, obj, obj ? where - obj : 0);
        for (uint32_t k = where - 0x60; k < where + 0x40; k += 16)
            LOG("diag:   %08x: %08x %08x %08x %08x", k, g_rd32(k), g_rd32(k + 4), g_rd32(k + 8), g_rd32(k + 12));
    }
    LOG("diag: done (%d shown)", shown);
    free(g_snap); g_snap = NULL;
}

static void draw_frame_extra(Cpu *cpu, uint32_t juego, uint32_t wm, float alpha) {
    static int diag_at = -1;
    if (diag_at < 0) diag_at = getenv("C2_DIAG") ? atoi(getenv("C2_DIAG")) : 0;
    int diag_now = diag_at && ++g_extra_count == diag_at;
    static double diag_ms = -1;   // C2_DIAG_MS=t: first extra frame after t ms of run time
    if (diag_ms < 0) diag_ms = getenv("C2_DIAG_MS") ? atof(getenv("C2_DIAG_MS")) : 0;
    static int diag_left = -1;
    if (diag_left < 0) diag_left = getenv("C2_DIAG_N") ? atoi(getenv("C2_DIAG_N")) : 0;
    if (diag_ms > 0 && plat_time_ms() >= diag_ms) { diag_now = 1; if (--diag_left <= 0) diag_ms = 1e18; }
    if (diag_now) diag_before();
    uint32_t canvas = wm + WM_CANVAS, video = wm + WM_VIDEO, lock = wm + WM_LOCKINFO, surf = wm + WM_SURFINFO;
    g_wr32(canvas, 0);
    if (guest_thiscall(cpu, f_00420dc0, canvas, 1, &surf) != 0) return;   // canvas init
    guest_thiscall(cpu, f_00421510, video, 1, &lock);                       // lock
    uint8_t limited = g_mem[juego + J_FPSLIMITED];
    g_mem[juego + J_FPSLIMITED] = 0;
    g_alpha = alpha;
    g_interpolating = 1;
    g_in_extra_frame = 1;
    guest_thiscall(cpu, f_004526f0, juego, 1, &canvas);                     // CJuego::Frame
    g_in_extra_frame = 0;
    g_interpolating = 0;
    g_mem[juego + J_FPSLIMITED] = limited;
    guest_thiscall(cpu, f_00421590, video, 1, &lock);                       // unlock
    if (g_mem[G_SKIPPRESENT] == 0 && g_mem[G_DREWFRAME] != 0) {
        uint32_t zero = 0;
        guest_thiscall(cpu, f_00422220, video, 1, &zero);                   // present
    }
    guest_thiscall(cpu, f_00420f60, canvas, 0, NULL);                       // canvas reset
    g_stat_frames++;
    if (diag_now) diag_after();
}

// site 0x403465: canvas reset 0x420f60 (thiscall canvas) at the end of a loop iteration: draw extra frames until the
// next tick is due.
void hook_enditer(Cpu *restrict cpu) {
    uint32_t canvas = cpu->ecx;
    f_00420f60(cpu);
    if (!(g_in_game && is_in_game())) return;
    Cpu save = *cpu;
    uint32_t wm = canvas - WM_CANVAS;
    uint32_t juego = g_rd32(G_JUEGO);
    double deadline = g_tick_time + g_tick_interval;
    double frame_ms = 1000.0 / max_fps();
    int can_draw = g_rd32(wm + WM_HAVEVIDEO) != 0 && g_mem[wm + WM_NODRAW] == 0;
    for (;;) {
        double next = g_next_frame_slot;
        if (!can_draw || next + 0.5 > deadline) break;
        wait_until(next);
        double now = now_ms();
        g_next_frame_slot = next + frame_ms;
        if (g_next_frame_slot < now) g_next_frame_slot = now;     // fell behind: do not catch up
        float alpha = (float)((now - g_tick_time) / g_tick_interval);
        if (alpha < 0.f) alpha = 0.f;
        if (alpha > 1.f) alpha = 1.f;
        draw_frame_extra(cpu, juego, wm, alpha);
    }
    wait_until(deadline);
    *cpu = save;

    if (mod_cfg()->debug) {
        double now = now_ms();
        if (g_stat_start == 0) g_stat_start = now;
        if (now - g_stat_start >= 5000.0) {
            double s = (now - g_stat_start) / 1000.0;
            LOG("modpatch: ticks/s %.2f  frames/s %.1f", g_stat_ticks / s, g_stat_frames / s);
            g_stat_start = now; g_stat_ticks = g_stat_frames = 0;
        }
    }
}

// World viewer draw (thiscall canvas, ret 4): render with an interpolated camera, then restore it.
void f_00520930(Cpu *restrict cpu) {
    uint32_t viewer = cpu->ecx;
    uint32_t cam = g_rd32(viewer + VM_CAMERA);
    if (!g_interpolating || !mod_cfg()->camera || !cam) { orig_f_00520930(cpu); return; }

    static int camlog = -1;
    if (camlog < 0) camlog = getenv("C2_CAMLOG") != NULL;
    if (camlog) {
        static int n;
        if (n++ % 50 == 0)
            LOG("cam %08x pos %.2f %.2f zoom %.4f  70=%.4f (1/zoom %.4f)  80,84=%d,%d  94,98=%.1f,%.1f (want %.1f,%.1f)  bam %08x  108=%d",
                cam, g_rdf(cam + CAM_X), g_rdf(cam + CAM_Y), g_rdf(cam + CAM_ZOOM), g_rdf(cam + 0x70), 1.0f / g_rdf(cam + CAM_ZOOM),
                (int)g_rd32(cam + 0x80), (int)g_rd32(cam + 0x84), g_rdf(cam + 0x94), g_rdf(cam + 0x98),
                (int)g_rd32(cam + 0x80) * g_rdf(cam + CAM_ZOOM), (int)g_rd32(cam + 0x84) * g_rdf(cam + CAM_ZOOM),
                g_rd32(cam + 0x28), g_mem[cam + 0x108]);
    }

    float actual[3] = { g_rdf(cam + CAM_X), g_rdf(cam + CAM_Y), g_rdf(cam + CAM_ZOOM) };
    CamRot rnow;
    rnow.bam = g_rd32(cam + 0x28);
    for (int i = 0; i < 9; i++) rnow.m[i] = g_rdf(cam + kRotOffs[i]);
    rnow.pos[0] = actual[0]; rnow.pos[1] = actual[1];
    rnow.scale = g_rdf(cam + 0x70);
    rnow.t[0] = g_rdf(cam + 0x5c); rnow.t[1] = g_rdf(cam + 0x60);

    CamState *s = cam_state(cam);
    if (s->tick != g_tick) {
        if (s->tick == 0xFFFFFFFFu || s->tick + 1 != g_tick) { memcpy(s->prev, actual, sizeof actual); s->rprev = rnow; }
        else { memcpy(s->prev, s->cur, sizeof actual); s->rprev = s->rcur; }
        memcpy(s->cur, actual, sizeof actual);
        s->rcur = rnow;
        s->tick = g_tick;
        // large jumps (centering on a unit, switching views): snap
        float dx = s->cur[0] - s->prev[0], dy = s->cur[1] - s->prev[1];
        float zr = s->cur[2] / (s->prev[2] != 0 ? s->prev[2] : 1.0f);
        int32_t turn = (int32_t)(s->rcur.bam - s->rprev.bam);
        int view_switch = turn <= -0x20000000 || turn >= 0x20000000;
        if (dx * dx + dy * dy > 300.0f * 300.0f || zr > 1.5f || zr < 0.66f || view_switch) {
            memcpy(s->prev, s->cur, sizeof actual);
            s->rprev = s->rcur;
        }
    }

    float a = g_alpha;
    float want_zoom = s->prev[2] + (s->cur[2] - s->prev[2]) * a;
    g_wrf(cam + CAM_X, s->prev[0] + (s->cur[0] - s->prev[0]) * a);
    g_wrf(cam + CAM_Y, s->prev[1] + (s->cur[1] - s->prev[1]) * a);
    // zoom: scaling around one fixed screen point, zoom interpolated geometrically
    {
        const float z0 = s->prev[2], z1 = s->cur[2];
        if (z0 > 0 && z1 > 0 && fabsf(z1 - z0) > 1e-4f * z0) {
            const double z = z0 * pow((double)z1 / z0, (double)a);
            for (int i = 0; i < 2; i++) {
                const double S = ((double)s->cur[i] - s->prev[i]) / ((double)z0 - z1);
                const double A = s->prev[i] + S * z0;
                g_wrf(cam + (i ? CAM_Y : CAM_X), (float)(A - S * z));
            }
            want_zoom = (float)z;
        }
    }
    g_wrf(cam + CAM_ZOOM, want_zoom);
    if (camlog && (s->prev[0] != s->cur[0] || s->prev[1] != s->cur[1])) {
        static int n;
        if (n++ < 40) LOG("interp tick %u alpha %.3f  prev %.1f,%.1f cur %.1f,%.1f -> drawn %.2f,%.2f", g_tick, a,
                          s->prev[0], s->prev[1], s->cur[0], s->cur[1], g_rdf(cam + CAM_X), g_rdf(cam + CAM_Y));
    }
    // derived per-tick values: +0x70 = 1/zoom, view size +0x94/+0x98 = window size (+0x80/+0x84) * zoom;
    // map background (cam+0x10C if byte cam+0x108) scale +0x28, drawing scale +0x2C = +0x28 * +0x24, +0x30 = 1/+0x2C
    const float s70 = g_rdf(cam + 0x70), s94 = g_rdf(cam + 0x94), s98 = g_rdf(cam + 0x98);
    float w70 = s70, w94 = s94, w98 = s98;
    int zoom_derived = 0;
    uint32_t bg = 0;
    float bg_saved[3] = {0}, bg_want[3] = {0};
    if (want_zoom > 0 && want_zoom != actual[2]) {
        w70 = 1.0f / want_zoom;
        w94 = (float)(int32_t)g_rd32(cam + 0x80) * want_zoom;
        w98 = (float)(int32_t)g_rd32(cam + 0x84) * want_zoom;
        g_wrf(cam + 0x70, w70); g_wrf(cam + 0x94, w94); g_wrf(cam + 0x98, w98);
        zoom_derived = 1;
        bg = g_mem[cam + 0x108] ? g_rd32(cam + 0x10c) : 0;
        if (bg && g_mem[bg] && g_rdf(bg + 0x24) > 0) {
            for (int i = 0; i < 3; i++) bg_saved[i] = g_rdf(bg + 0x28 + 4 * i);
            const float f2c = w70 * g_rdf(bg + 0x24);
            bg_want[0] = w70; bg_want[1] = f2c; bg_want[2] = f2c > 0 ? 1.0f / f2c : 0.0f;
            for (int i = 0; i < 3; i++) g_wrf(bg + 0x28 + 4 * i, bg_want[i]);
        } else bg = 0;
    }

    // rotation / tilt (interiors): interpolate the angle and keep the ground point the game turns around in place
    float want_x = g_rdf(cam + CAM_X), want_y = g_rdf(cam + CAM_Y);
    int rotated = 0;
    float wrote_m0 = 0, wrote_m2 = 0;
    const int32_t dbam = (int32_t)(s->rcur.bam - s->rprev.bam);
    const int matrix_changed = dbam != 0 || memcmp(s->rprev.m, s->rcur.m, sizeof s->rcur.m) != 0;
    if (matrix_changed && dbam > -0x20000000 && dbam < 0x20000000 && s->rprev.scale == s->rcur.scale &&
        s->rcur.scale > 0 && s->prev[2] == s->cur[2]) {
        const CamRot *p0 = &s->rprev, *p1 = &s->rcur;
#define KOF(r, i) ((r)->m[i] * (r)->m[1] - (r)->m[(i) + 1] * (r)->m[0])
        const float kA0 = KOF(p0, 2), kB0 = KOF(p0, 4), kA1 = KOF(p1, 2), kB1 = KOF(p1, 4);
#undef KOF
        uint32_t bam = p0->bam + (uint32_t)(int32_t)((double)dbam * a);
        double th = bam * (6.283185307179586 / 4294967296.0);
        float c = (float)cos(th), sn = (float)sin(th);
        if (dbam == 0) { c = p1->m[0]; sn = p1->m[1]; }
        const float kA = kA0 + (kA1 - kA0) * a, kB = kB0 + (kB1 - kB0) * a;
        float m[9] = { c, sn, sn * kA, -c * kA, sn * kB, -c * kB,
                       p0->m[6] + (p1->m[6] - p0->m[6]) * a, p0->m[7] + (p1->m[7] - p0->m[7]) * a,
                       p0->m[8] + (p1->m[8] - p0->m[8]) * a };
        g_wr32(cam + 0x28, bam);
        for (int i = 0; i < 9; i++) g_wrf(cam + kRotOffs[i], m[i]);
        wrote_m0 = m[0]; wrote_m2 = m[2];
        const double d00 = (double)p0->m[0] - p1->m[0], d01 = (double)p0->m[1] - p1->m[1];
        const double d10 = (double)p0->m[4] - p1->m[4], d11 = (double)p0->m[5] - p1->m[5];
        const double det = d00 * d11 - d01 * d10;
        const double bx = (double)p0->pos[0] - p1->pos[0] + p1->t[0] - p0->t[0];
        const double by = (double)p0->pos[1] - p1->pos[1] + p1->t[1] - p0->t[1];
        if (fabs(det) > 1e-7) {
            const double wx = (bx * d11 - d01 * by) / det, wy = (d00 * by - d10 * bx) / det;
            if (fabs(wx) < 1e7 && fabs(wy) < 1e7) {
                const double sx = p0->m[0] * wx + p0->m[1] * wy + p0->t[0] - p0->pos[0];
                const double sy = p0->m[4] * wx + p0->m[5] * wy + p0->t[1] - p0->pos[1];
                const double tx = p0->t[0] + (p1->t[0] - p0->t[0]) * a, ty = p0->t[1] + (p1->t[1] - p0->t[1]) * a;
                want_x = (float)(m[0] * wx + m[1] * wy + tx - sx);
                want_y = (float)(m[4] * wx + m[5] * wy + ty - sy);
                g_wrf(cam + CAM_X, want_x);
                g_wrf(cam + CAM_Y, want_y);
            }
        }
        rotated = 1;
        g_mem[cam + 0x124] = 1;      // interiors: re-render the 3D image with the interpolated view
    }

    orig_f_00520930(cpu);

    if (rotated && g_rdf(cam + 0x38) == wrote_m0 && g_rdf(cam + 0x44) == wrote_m2) {
        g_mem[cam + 0x124] = 1;
        g_wr32(cam + 0x28, rnow.bam);
        for (int i = 0; i < 9; i++) g_wrf(cam + kRotOffs[i], rnow.m[i]);
    }
    // restore unless the draw itself changed the camera
    if (g_rdf(cam + CAM_X) == want_x) g_wrf(cam + CAM_X, actual[0]);
    if (g_rdf(cam + CAM_Y) == want_y) g_wrf(cam + CAM_Y, actual[1]);
    if (g_rdf(cam + CAM_ZOOM) == want_zoom) g_wrf(cam + CAM_ZOOM, actual[2]);
    if (zoom_derived) {
        if (g_rdf(cam + 0x70) == w70) g_wrf(cam + 0x70, s70);
        if (g_rdf(cam + 0x94) == w94) g_wrf(cam + 0x94, s94);
        if (g_rdf(cam + 0x98) == w98) g_wrf(cam + 0x98, s98);
    }
    if (bg) for (int i = 0; i < 3; i++) if (g_rdf(bg + 0x28 + 4 * i) == bg_want[i]) g_wrf(bg + 0x28 + 4 * i, bg_saved[i]);
}

// site 0x51c839: mission draw 0x51c810 -> [mission+0x1c]->0x516080 (-> 0x64a9f0): 15-frame arrow cursor animation,
// advanced on every draw. Skipped in extra frames so the cursor animates at the original rate.
//   [Steam 0x44bd10 -> 0x437d30]
void hook_cursor_anim(Cpu *restrict cpu) {
    if (g_in_extra_frame) { cpu->esp += 4; return; }     // thiscall, no args: just drop the return address
    f_00516080(cpu);
}

// 3D face sequences (CVisorSecuencias3D draw 0x6ced50, thiscall(canvas), ret 4) advance one keyframe per draw call:
// 0x59a6f0 renders the next keyframe into [this+0xc] (and 0x6ce8b0 picks the next sequence at the end). In extra
// frames skip that and only run the draw tail (0x6cedd2): blit the current keyframe image at [this+0x34]/[this+0x38].
//   [Steam CVisorSecuencias3D::Draw 0x92a5f0, draw-only tail 0x92a673]
void f_006ced50(Cpu *restrict cpu) {
    if (!g_in_extra_frame) { orig_f_006ced50(cpu); return; }
    uint32_t self = cpu->ecx, canvas = ARG(0);
    uint32_t save_ecx = cpu->ecx;
    if (g_mem[self + 0x44] && g_rd32(self + 0xc)) {
        uint32_t dx = 0, dy = 0, seq = g_rd32(self + 4);
        if (seq) {
            uint32_t id = g_rd32(seq);
            dx = guest_thiscall(cpu, f_0059b170, g_rd32(self + 0x14), 1, &id);
            id = g_rd32(g_rd32(self + 4));
            dy = guest_thiscall(cpu, f_0059b220, g_rd32(self + 0x14), 1, &id);
        }
        uint32_t a[10] = { g_rd32(self + 0x34) + dx, g_rd32(self + 0x38) + dy, g_rd32(self + 0xc),
                           0, 0, 1, 0x20, 0, 0, 0x3f800000u };
        guest_thiscall(cpu, f_004325c0, canvas, 10, a);
    }
    cpu->ecx = save_ecx;
    cpu->esp += 8;    // ret 4
}
