// Platform layer on SDL3: window + Metal-backed renderer, events -> Win32 messages, audio device.
#include "rt.h"
#include "platform.h"
#include <SDL3/SDL.h>
#include <pthread.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static int g_w = 640, g_h = 480;
static SDL_AudioStream *g_audio;
static void (*g_mix)(int16_t *, int);
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;

uint8_t g_vk_down[256], g_vk_toggle[256], g_vk_pressed_since[256];
int g_mouse_x, g_mouse_y;
int32_t g_mouse_dx, g_mouse_dy, g_mouse_dz;
uint8_t g_mouse_btn[4];
uint8_t g_dik_down[256];
uint32_t g_main_hwnd;
int g_quit_requested;

#define QCAP 1024
static Msg g_q[QCAP];
static int g_qh, g_qt;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;

#define DICAP 256
static DIEvt g_die[DICAP];
static int g_dih, g_dit;
static uint32_t g_diseq;

void rt_platform_init(int argc, char **argv) {
    (void)argc; (void)argv;
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    // full screen without a separate Space: the menu bar and Dock stay hidden instead of appearing when the
    // mouse touches the top / bottom edge (which the game uses for edge scrolling)
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
    // relative mouse mode (full screen) keeps the system pointer speed / acceleration
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        LOG("SDL_Init failed: %s", SDL_GetError());
        exit(1);
    }
}

uint32_t plat_time_ms(void) { return (uint32_t)SDL_GetTicks(); }
double plat_time_ms_f(void) { return (double)SDL_GetTicksNS() / 1e6; }
// refresh rate of the display the game window is on (or the primary display), 60 if unknown
int plat_refresh_hz(void) {
    SDL_DisplayID d = g_win ? SDL_GetDisplayForWindow(g_win) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
    return m && m->refresh_rate > 1 ? (int)(m->refresh_rate + 0.5f) : 60;
}
int plat_mode_w(void) { return g_w; }
int plat_mode_h(void) { return g_h; }

// Full screen uses relative mouse mode: the macOS pointer is hidden and held in place (it can never show up, reach
// the notch strip, the menu bar or the Dock); the game's cursor position is tracked here from the movement.
static int g_relmouse;
static float g_rel_x, g_rel_y;   // cursor position in game pixels while relative
static void set_fullscreen(int on) {
    SDL_SetWindowFullscreen(g_win, on);
    SDL_SetWindowMouseGrab(g_win, on);
    g_rel_x = (float)g_mouse_x; g_rel_y = (float)g_mouse_y;
    g_relmouse = on;
    SDL_SetWindowRelativeMouseMode(g_win, on);
    if (!on && g_ren) {   // back to a window: put the system pointer where the game's cursor is
        float wx, wy;
        SDL_RenderCoordinatesToWindow(g_ren, (float)g_mouse_x, (float)g_mouse_y, &wx, &wy);
        SDL_WarpMouseInWindow(g_win, wx, wy);
    }
    SDL_HideCursor();
}

static void ensure_window(void) {
    if (g_win) return;
    g_win = SDL_CreateWindow("Commandos 2", g_w * 2 > 1600 ? g_w : g_w * 2, g_w * 2 > 1600 ? g_h : g_h * 2,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    g_ren = SDL_CreateRenderer(g_win, NULL);
    LOG("renderer: %s", SDL_GetRendererName(g_ren));
    SDL_SetRenderVSync(g_ren, 1);
    SDL_HideCursor();
    SDL_StartTextInput(g_win);
    // start full screen (C2_WINDOWED=1, or a scripted test run, keeps a window); Cmd+Enter toggles
    if (!getenv("C2_WINDOWED") && !getenv("C2_INPUT")) set_fullscreen(1);
}

void plat_set_mode(int w, int h) {
    g_w = w; g_h = h;
    ensure_window();
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_LINEAR);
    SDL_SetRenderLogicalPresentation(g_ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    LOG("display mode %dx%d", w, h);
}

void plat_set_title(const char *s) { ensure_window(); SDL_SetWindowTitle(g_win, s); }
void plat_show_cursor(int show) { (void)show; /* the game draws its own cursor */ }
void plat_warp_mouse(int x, int y) {
    if (!g_win) return;
    if (g_relmouse) { g_rel_x = (float)x; g_rel_y = (float)y; g_mouse_x = x; g_mouse_y = y; return; }
    float wx, wy;
    SDL_RenderCoordinatesToWindow(g_ren, (float)x, (float)y, &wx, &wy);
    SDL_WarpMouseInWindow(g_win, wx, wy);
    g_mouse_x = x; g_mouse_y = y;
}

static void dump_frame(const uint8_t *pix, int pitch, int w, int h, unsigned n) {
    const char *dir = getenv("C2_DUMP");
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%05u.bmp", dir, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t rowsz = (uint32_t)w * 3, pad = (4 - rowsz % 4) % 4, img = (rowsz + pad) * h;
    uint8_t hdr[54] = {'B', 'M'};
    *(uint32_t *)(hdr + 2) = 54 + img; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = w; *(int32_t *)(hdr + 22) = h; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = img;
    fwrite(hdr, 1, 54, f);
    uint8_t row[4096 * 3 + 4] = {0};
    for (int y = h - 1; y >= 0; y--) {
        const uint16_t *src = (const uint16_t *)(pix + y * pitch);
        for (int x = 0; x < w; x++) {
            uint16_t v = src[x];
            row[3 * x] = (uint8_t)((v & 0x1f) << 3); row[3 * x + 1] = (uint8_t)(((v >> 5) & 0x3f) << 2); row[3 * x + 2] = (uint8_t)((v >> 11) << 3);
        }
        fwrite(row, 1, rowsz + pad, f);
    }
    fclose(f);
}

// ---- scripted input for automated testing: C2_INPUT=file with lines "<ms> <action> [args]"
//   click X Y | rclick X Y | move X Y | key VK(hex or dec) | dump
typedef struct { uint32_t t; char act[16]; int a, b; } ScriptEv;
static ScriptEv g_script[512];
static int g_nscript, g_script_pos, g_dump_req;
static void load_script(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *fn = getenv("C2_INPUT");
    if (!fn) return;
    FILE *f = fopen(fn, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f) && g_nscript < 512) {
        ScriptEv *e = &g_script[g_nscript];
        e->a = e->b = 0;
        if (line[0] == '#' || sscanf(line, "%u %15s %i %i", &e->t, e->act, &e->a, &e->b) < 2) continue;
        g_nscript++;
    }
    fclose(f);
    LOG("input script: %d events", g_nscript);
}
static void mouse_button(int b, int down);
static void di_push(uint32_t ofs, uint32_t data);
static int g_zoom_taps;   // mouse wheel zoom: pending keypad +/- taps (+ = in)
static void run_script(void) {
    load_script();
    uint32_t now = plat_time_ms();
    while (g_script_pos < g_nscript && g_script[g_script_pos].t <= now) {
        ScriptEv *e = &g_script[g_script_pos++];
        LOG("script @%u: %s %d %d", now, e->act, e->a, e->b);
        if (!strcmp(e->act, "dump")) { g_dump_req = 1; continue; }
        if (!strcmp(e->act, "find")) {   // find A B: log guest addresses holding dwords A, B in a row (debug)
            uint32_t rt_heap_top(void), rt_va_top(void);
            uint32_t lo[3] = {0x400000, 0x10000000, 0x70000000}, hi[3] = {0xbb0000, rt_heap_top(), rt_va_top()};
            int n = 0;
            for (int r = 0; r < 3; r++)
                for (uint32_t a = lo[r]; a + 8 <= hi[r] && n < 64; a += 4)
                    if (g_rd32(a) == (uint32_t)e->a && g_rd32(a + 4) == (uint32_t)e->b) {
                        LOG("find %d %d @%08x  [-0x40..]:", e->a, e->b, a);
                        for (int k = -16; k < 8; k++) fprintf(stderr, "%s%08x", k % 8 ? " " : "\n    ", g_rd32(a + 4 * k));
                        fprintf(stderr, "\n");
                        n++;
                    }
            continue;
        }
        if (!strcmp(e->act, "dumpn")) { g_dump_req = e->a > 0 ? e->a : 1; continue; }   // N consecutive frames
        if (!strcmp(e->act, "key")) {
            uint32_t vk = (uint32_t)e->a;
            g_vk_down[vk] = 1; g_vk_pressed_since[vk] = 1;
            plat_post_msg(0, 0x100, vk, 1);
            if (vk == 0x0D || vk == 0x1B || vk == 0x08 || (vk >= 0x20 && vk < 0x7f)) plat_post_msg(0, 0x102, vk >= 'A' && vk <= 'Z' ? vk + 32 : vk, 1);
            plat_post_msg(0, 0x101, vk, 0xC0000001u);
            g_vk_down[vk] = 0;
            continue;
        }
        if (!strcmp(e->act, "kdown") || !strcmp(e->act, "kup")) {   // kdown VK DIK: held key (e.g. 0x27 0xcd = right)
            int down = e->act[1] == 'd';
            uint32_t vk = (uint32_t)e->a & 0xff;
            if (e->b) g_dik_down[e->b & 0xff] = down ? 0x80 : 0;
            g_vk_down[vk] = down;
            if (down) g_vk_pressed_since[vk] = 1;
            plat_post_msg(0, down ? 0x100 : 0x101, vk, down ? 1 : 0xC0000001u);
            continue;
        }
        if (!strcmp(e->act, "wheel")) { g_zoom_taps += e->a; continue; }   // wheel N: N zoom taps (+ = in)
        int dx = e->a - g_mouse_x, dy = e->b - g_mouse_y;
        g_mouse_dx += dx; g_mouse_dy += dy;
        g_mouse_x = e->a; g_mouse_y = e->b;
        plat_post_msg(0, 0x200, 0, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
        if (!strcmp(e->act, "click")) { mouse_button(SDL_BUTTON_LEFT, 1); mouse_button(SDL_BUTTON_LEFT, 0); }
        else if (!strcmp(e->act, "down")) mouse_button(SDL_BUTTON_LEFT, 1);
        else if (!strcmp(e->act, "up")) mouse_button(SDL_BUTTON_LEFT, 0);
        else if (!strcmp(e->act, "rclick")) { mouse_button(SDL_BUTTON_RIGHT, 1); mouse_button(SDL_BUTTON_RIGHT, 0); }
    }
}

void plat_present(const uint8_t *pix, int pitch, int w, int h) {
    if (getenv("C2_PRESENTTRACE")) { void rt_host_bt(void); rt_host_bt(); }
    static unsigned frames;
    frames++;
    if (frames % 100 == 1) LOG("present #%u (%dx%d)", frames, w, h);
    if ((getenv("C2_DUMP_ALL") && frames % 100 == 1) || g_dump_req) { if (g_dump_req > 0) g_dump_req--; dump_frame(pix, pitch, w, h, frames); }
    ensure_window();
    if (!g_tex || w != g_w || h != g_h) plat_set_mode(w, h);
    SDL_UpdateTexture(g_tex, NULL, pix, pitch);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
    plat_pump();
}

// ------------------------------------------------------------------ messages
void plat_post_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&g_qlock);
    int n = (g_qt + 1) % QCAP;
    if (n != g_qh) {
        g_q[g_qt] = (Msg){hwnd ? hwnd : g_main_hwnd, msg, wp, lp, plat_time_ms(), (uint32_t)g_mouse_x, (uint32_t)g_mouse_y};
        g_qt = n;
    }
    pthread_mutex_unlock(&g_qlock);
}

int plat_peek_msg(Msg *m, uint32_t fmin, uint32_t fmax, int remove) {
    pthread_mutex_lock(&g_qlock);
    for (int i = g_qh; i != g_qt; i = (i + 1) % QCAP) {
        Msg *x = &g_q[i];
        if (fmin || fmax) { if (x->msg < fmin || x->msg > fmax) continue; }
        *m = *x;
        if (remove) {
            // remove i by shifting
            for (int j = i; j != g_qh; ) { int p = (j - 1 + QCAP) % QCAP; g_q[j] = g_q[p]; j = p; }
            g_qh = (g_qh + 1) % QCAP;
        }
        pthread_mutex_unlock(&g_qlock);
        return 1;
    }
    pthread_mutex_unlock(&g_qlock);
    return 0;
}

int plat_mouse_event_pop(DIEvt *e) {
    if (g_dih == g_dit) return 0;
    *e = g_die[g_dih];
    g_dih = (g_dih + 1) % DICAP;
    return 1;
}
static void di_push(uint32_t ofs, uint32_t data) {
    int n = (g_dit + 1) % DICAP;
    if (n == g_dih) g_dih = (g_dih + 1) % DICAP;
    g_die[g_dit] = (DIEvt){ofs, data, plat_time_ms(), ++g_diseq};
    g_dit = n;
}

// SDL scancode -> (VK, PC set-1 scancode, extended)
static void map_key(SDL_Scancode sc, SDL_Keycode kc, uint32_t *vk, uint32_t *pcsc, int *ext) {
    *ext = 0; *pcsc = 0; *vk = 0;
    if (kc >= 'a' && kc <= 'z') *vk = (uint32_t)(kc - 32);
    else if (kc >= '0' && kc <= '9') *vk = (uint32_t)kc;
    static const struct { SDL_Scancode s; uint8_t vk, pc, ext; } T[] = {
        {SDL_SCANCODE_ESCAPE, 0x1B, 0x01, 0}, {SDL_SCANCODE_RETURN, 0x0D, 0x1C, 0}, {SDL_SCANCODE_SPACE, 0x20, 0x39, 0},
        {SDL_SCANCODE_TAB, 0x09, 0x0F, 0}, {SDL_SCANCODE_BACKSPACE, 0x08, 0x0E, 0},
        {SDL_SCANCODE_LSHIFT, 0x10, 0x2A, 0}, {SDL_SCANCODE_RSHIFT, 0x10, 0x36, 0},
        {SDL_SCANCODE_LCTRL, 0x11, 0x1D, 0}, {SDL_SCANCODE_RCTRL, 0x11, 0x1D, 1},
        {SDL_SCANCODE_LALT, 0x12, 0x38, 0}, {SDL_SCANCODE_RALT, 0x12, 0x38, 1},
        {SDL_SCANCODE_LGUI, 0x11, 0x1D, 0}, {SDL_SCANCODE_RGUI, 0x11, 0x1D, 1},   // Cmd acts as Ctrl
        {SDL_SCANCODE_LEFT, 0x25, 0x4B, 1}, {SDL_SCANCODE_UP, 0x26, 0x48, 1}, {SDL_SCANCODE_RIGHT, 0x27, 0x4D, 1},
        {SDL_SCANCODE_DOWN, 0x28, 0x50, 1}, {SDL_SCANCODE_INSERT, 0x2D, 0x52, 1}, {SDL_SCANCODE_DELETE, 0x2E, 0x53, 1},
        {SDL_SCANCODE_HOME, 0x24, 0x47, 1}, {SDL_SCANCODE_END, 0x23, 0x4F, 1}, {SDL_SCANCODE_PAGEUP, 0x21, 0x49, 1},
        {SDL_SCANCODE_PAGEDOWN, 0x22, 0x51, 1}, {SDL_SCANCODE_PAUSE, 0x13, 0x45, 0}, {SDL_SCANCODE_CAPSLOCK, 0x14, 0x3A, 0},
        {SDL_SCANCODE_F1, 0x70, 0x3B, 0}, {SDL_SCANCODE_F2, 0x71, 0x3C, 0}, {SDL_SCANCODE_F3, 0x72, 0x3D, 0},
        {SDL_SCANCODE_F4, 0x73, 0x3E, 0}, {SDL_SCANCODE_F5, 0x74, 0x3F, 0}, {SDL_SCANCODE_F6, 0x75, 0x40, 0},
        {SDL_SCANCODE_F7, 0x76, 0x41, 0}, {SDL_SCANCODE_F8, 0x77, 0x42, 0}, {SDL_SCANCODE_F9, 0x78, 0x43, 0},
        {SDL_SCANCODE_F10, 0x79, 0x44, 0}, {SDL_SCANCODE_F11, 0x7A, 0x57, 0}, {SDL_SCANCODE_F12, 0x7B, 0x58, 0},
        {SDL_SCANCODE_KP_0, 0x60, 0x52, 0}, {SDL_SCANCODE_KP_1, 0x61, 0x4F, 0}, {SDL_SCANCODE_KP_2, 0x62, 0x50, 0},
        {SDL_SCANCODE_KP_3, 0x63, 0x51, 0}, {SDL_SCANCODE_KP_4, 0x64, 0x4B, 0}, {SDL_SCANCODE_KP_5, 0x65, 0x4C, 0},
        {SDL_SCANCODE_KP_6, 0x66, 0x4D, 0}, {SDL_SCANCODE_KP_7, 0x67, 0x47, 0}, {SDL_SCANCODE_KP_8, 0x68, 0x48, 0},
        {SDL_SCANCODE_KP_9, 0x69, 0x49, 0}, {SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0x37, 0}, {SDL_SCANCODE_KP_PLUS, 0x6B, 0x4E, 0},
        {SDL_SCANCODE_KP_MINUS, 0x6D, 0x4A, 0}, {SDL_SCANCODE_KP_PERIOD, 0x6E, 0x53, 0}, {SDL_SCANCODE_KP_DIVIDE, 0x6F, 0x35, 1},
        {SDL_SCANCODE_KP_ENTER, 0x0D, 0x1C, 1},
        // Mac keyboards have no keypad: the main-row -/= keys act as keypad -/+ (the game's zoom out/in keys).
        // Typed characters come from SDL text input, so typing '-' / '=' in text fields is unaffected.
        {SDL_SCANCODE_SEMICOLON, 0xBA, 0x27, 0}, {SDL_SCANCODE_EQUALS, 0x6B, 0x4E, 0}, {SDL_SCANCODE_COMMA, 0xBC, 0x33, 0},
        {SDL_SCANCODE_MINUS, 0x6D, 0x4A, 0}, {SDL_SCANCODE_PERIOD, 0xBE, 0x34, 0}, {SDL_SCANCODE_SLASH, 0xBF, 0x35, 0},
        {SDL_SCANCODE_GRAVE, 0xC0, 0x29, 0}, {SDL_SCANCODE_LEFTBRACKET, 0xDB, 0x1A, 0}, {SDL_SCANCODE_BACKSLASH, 0xDC, 0x2B, 0},
        {SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0x1B, 0}, {SDL_SCANCODE_APOSTROPHE, 0xDE, 0x28, 0},
    };
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (T[i].s == sc) { *vk = T[i].vk; *pcsc = T[i].pc; *ext = T[i].ext; return; }
    static const uint8_t letters[26] = {0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { *vk = 'A' + (sc - SDL_SCANCODE_A); *pcsc = letters[sc - SDL_SCANCODE_A]; }
    else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { *vk = '1' + (sc - SDL_SCANCODE_1); *pcsc = 0x02 + (sc - SDL_SCANCODE_1); }
    else if (sc == SDL_SCANCODE_0) { *vk = '0'; *pcsc = 0x0B; }
}

static void mouse_button(int b, int down) {
    static const uint32_t dn[3] = {0x201, 0x204, 0x207}, up[3] = {0x202, 0x205, 0x208};
    int i = b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_RIGHT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : -1;
    if (i < 0) return;
    g_mouse_btn[i] = down ? 0x80 : 0;
    static const uint8_t vk[3] = {0x01, 0x02, 0x04};
    g_vk_down[vk[i]] = down;
    if (down) g_vk_pressed_since[vk[i]] = 1;
    uint32_t mk = (g_mouse_btn[0] ? 1 : 0) | (g_mouse_btn[1] ? 2 : 0) | (g_mouse_btn[2] ? 0x10 : 0) |
                  (g_vk_down[0x10] ? 4 : 0) | (g_vk_down[0x11] ? 8 : 0);
    plat_post_msg(0, down ? dn[i] : up[i], mk, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
    di_push(12 + i, down ? 0x80 : 0);   // DIMOFS_BUTTON0..
}

// synthetic key press/release for the game's keyboard-only actions; both Win32 messages and the DirectInput key
// state see it
static void synth_key(uint32_t vk, uint32_t pc, int down) {
    g_dik_down[pc] = down ? 0x80 : 0;
    g_vk_down[vk] = down;
    if (down) { g_vk_pressed_since[vk] = 1; g_vk_toggle[vk] ^= 1; }
    plat_post_msg(0, down ? 0x100 : 0x101, vk, 1 | (pc << 16) | (down ? 0 : 3u << 30));
}

// mouse wheel zoom: pending taps (+ = zoom in) played back as keypad +/- presses held long enough (80 ms) for the
// game's input polling to see them, 30 ms apart
static void zoom_taps(void) {
    static uint32_t vk, until;   // vk != 0 while a key is held
    uint32_t now = plat_time_ms();
    if (vk) {
        if ((int32_t)(now - until) < 0) return;
        synth_key(vk, vk == 0x6B ? 0x4E : 0x4A, 0);
        vk = 0;
        until = now + 30;
        return;
    }
    if (!g_zoom_taps || (int32_t)(now - until) < 0) return;
    vk = g_zoom_taps > 0 ? 0x6B : 0x6D;   // VK_ADD / VK_SUBTRACT
    g_zoom_taps += g_zoom_taps > 0 ? -1 : 1;
    synth_key(vk, vk == 0x6B ? 0x4E : 0x4A, 1);
    until = now + 80;
}

void plat_pump(void) {
    SDL_Event e;
    zoom_taps();
    run_script();
    while (SDL_PollEvent(&e)) {
        if (g_ren) SDL_ConvertEventToRenderCoordinates(g_ren, &e);
        switch (e.type) {
        case SDL_EVENT_QUIT:
            g_quit_requested = 1;
            plat_post_msg(0, 0x0010, 0, 0);   // WM_CLOSE
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            uint32_t vk, pc; int ext;
            map_key(e.key.scancode, e.key.key, &vk, &pc, &ext);
            if (!vk) break;
            int down = e.type == SDL_EVENT_KEY_DOWN;
            // Cmd+Q / Cmd+Enter: host shortcuts
            if (down && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_Q) { g_quit_requested = 1; exit(0); }
            if (down && (e.key.mod & SDL_KMOD_GUI) && e.key.scancode == SDL_SCANCODE_RETURN) {
                set_fullscreen(!(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (pc) g_dik_down[(pc | (ext ? 0x80 : 0)) & 0xff] = down ? 0x80 : 0;
            uint32_t prev = g_vk_down[vk];
            g_vk_down[vk] = down;
            if (down) { g_vk_pressed_since[vk] = 1; if (!e.key.repeat) g_vk_toggle[vk] ^= 1; }
            int alt = g_vk_down[0x12];
            uint32_t lp = 1 | (pc << 16) | ((uint32_t)ext << 24) | (alt ? 1u << 29 : 0) | (prev ? 1u << 30 : 0) | (down ? 0 : 3u << 30);
            uint32_t msg = down ? (alt && vk != 0x12 ? 0x104 : 0x100) : (alt && vk != 0x12 ? 0x105 : 0x101);
            plat_post_msg(0, msg, vk, lp);
            // WM_CHAR for control keys that SDL text input does not deliver
            if (down && (vk == 0x08 || vk == 0x0D || vk == 0x1B || vk == 0x09)) plat_post_msg(0, 0x102, vk, lp);
            break;
        }
        case SDL_EVENT_TEXT_INPUT:
            for (const unsigned char *s = (const unsigned char *)e.text.text; *s; s++) {
                uint32_t c = *s;
                if (c >= 0xC0 && (s[1] & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (s[1] & 0x3F); s++; }   // 2-byte UTF-8 -> Latin-1
                else if (c >= 0x80) continue;
                plat_post_msg(0, 0x102, c, 1);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            int nx = (int)e.motion.x, ny = (int)e.motion.y;
            if (g_relmouse) {   // position from the movement (already scaled to game pixels)
                g_rel_x += e.motion.xrel; g_rel_y += e.motion.yrel;
                if (g_rel_x < 0) g_rel_x = 0; if (g_rel_y < 0) g_rel_y = 0;
                if (g_rel_x > g_w - 1) g_rel_x = (float)(g_w - 1); if (g_rel_y > g_h - 1) g_rel_y = (float)(g_h - 1);
                nx = (int)g_rel_x; ny = (int)g_rel_y;
            }
            if (nx < 0) nx = 0; if (ny < 0) ny = 0; if (nx >= g_w) nx = g_w - 1; if (ny >= g_h) ny = g_h - 1;
            g_mouse_dx += (int)(e.motion.xrel + (e.motion.xrel > 0 ? 0.5f : -0.5f));
            g_mouse_dy += (int)(e.motion.yrel + (e.motion.yrel > 0 ? 0.5f : -0.5f));
            if ((int)e.motion.xrel) di_push(0, (uint32_t)(int)e.motion.xrel);
            if ((int)e.motion.yrel) di_push(4, (uint32_t)(int)e.motion.yrel);
            g_mouse_x = nx; g_mouse_y = ny;
            uint32_t mk = (g_mouse_btn[0] ? 1 : 0) | (g_mouse_btn[1] ? 2 : 0) | (g_mouse_btn[2] ? 0x10 : 0);
            plat_post_msg(0, 0x200, mk, (uint32_t)(nx & 0xffff) | ((uint32_t)ny << 16));
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (!g_relmouse) { g_mouse_x = (int)e.button.x; g_mouse_y = (int)e.button.y; }
            mouse_button(e.button.button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.clicks == 2) {
                int i = e.button.button == SDL_BUTTON_LEFT ? 0x203 : e.button.button == SDL_BUTTON_RIGHT ? 0x206 : 0;
                if (i) plat_post_msg(0, (uint32_t)i, 1, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            int d = (int)(e.wheel.y * 120);
            g_mouse_dz += d;
            di_push(8, (uint32_t)d);
            plat_post_msg(0, 0x20A, (uint32_t)(d & 0xffff) << 16, (uint32_t)(g_mouse_x & 0xffff) | ((uint32_t)g_mouse_y << 16));
            // the game has no wheel zoom: each notch becomes a tap of its zoom keys (keypad +/-). Physical direction
            // (macOS "natural scrolling" undone): wheel forward / two fingers up = zoom in.
            float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            static float acc;
            acc += y;
            while (acc >= 1.0f) { acc -= 1.0f; if (g_zoom_taps < 4) g_zoom_taps++; }
            while (acc <= -1.0f) { acc += 1.0f; if (g_zoom_taps > -4) g_zoom_taps--; }
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            SDL_HideCursor();                     // the game draws its own arrow
            plat_post_msg(0, 0x001C, 1, 0);       // WM_ACTIVATEAPP
            plat_post_msg(0, 0x0006, 1, 0);       // WM_ACTIVATE
            break;
        // macOS can show the system pointer again after these; keep it hidden over the game window
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        case SDL_EVENT_WINDOW_SHOWN:
            SDL_HideCursor();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            memset(g_vk_down, 0, sizeof g_vk_down);
            break;
        }
    }
}

// ------------------------------------------------------------------ audio
static int g_rate;
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)total;
    int frames = additional / 4;
    if (frames <= 0) return;
    static int16_t buf[16384];
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        pthread_mutex_lock(&g_audio_lock);
        g_mix(buf, n);
        pthread_mutex_unlock(&g_audio_lock);
        SDL_PutAudioStreamData(s, buf, n * 4);
        frames -= n;
    }
}
void plat_audio_start(void (*mix)(int16_t *, int), int rate) {
    if (g_audio) return;
    g_mix = mix; g_rate = rate;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, rate};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!g_audio) { LOG("audio open failed: %s", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(g_audio);
    LOG("audio started %d Hz", rate);
}
void plat_audio_lock(void) { pthread_mutex_lock(&g_audio_lock); }
void plat_audio_unlock(void) { pthread_mutex_unlock(&g_audio_lock); }
