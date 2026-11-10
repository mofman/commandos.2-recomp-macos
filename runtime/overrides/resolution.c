// Widescreen support: replaces the game's video mode table and lets the options menus offer any number of modes.
//
// The 1.2 exe knows 4 modes (0..3 = 640x480, 800x600, 1024x768, 1280x1024): five small size helpers, a setter
// that clamps the index to 0..3 (0x4ecf00), and two options widgets:
//   - Options menu arrow selector (class at 0x476xxx, index at this+0xd4): left/right step through 0..2 only,
//     the label switch (0x476110) only knows R640/R800/R024, and the apply routines (0x476d20 / 0x476de0 / 0x476ec0)
//     notify the game with their own hardcoded 640x480 / 800x600 / 1024x756 sizes.
//   - Video options list (0x474f90): range 0..3 (0x8dfb90) and 4 fourcc labels R640/R800/R024/R280 (0x8dff50).
// All of these are replaced here, so the menus cycle through g_n modes and show each mode's real size.
//
// The mode list is fixed (no config): the MacBook screen shape (3024x1964) at three sizes, plus standard 16:10 and 16:9.
// Shift+F1/F2/F3 select the first three.
#include "hook.h"

#define G_JUEGO   0xb58488u   // CJuego*
#define G_OPTIONS 0xb58490u   // options / video settings (mode index at +0x54)
#define G_VIDEO   0xb58654u   // video object*

// The list size is also patched into the COMM2.CFG loader clamp (recomp/overrides.txt, 0x4ec4d2 / 0x4ec4d8).
// Appended (never reorder: COMM2.CFG stores the index): 16:9 modes for external displays.
static const int g_w[] = {984, 1232, 1280, 1512, 1680, 1920, 1600, 1920};
static const int g_h[] = {640, 800, 800, 982, 1050, 1200, 900, 1080};
#define NMODES ((int)(sizeof g_w / sizeof *g_w))
static const int g_n = NMODES;
static uint32_t g_label[NMODES];   // guest strings "WxH", made on first use (guest heap)

static void make_labels(void) {
    if (g_label[0]) return;
    for (int i = 0; i < g_n; i++) {
        g_label[i] = galloc(16);
        snprintf((char *)g_mem + g_label[i], 16, "%dx%d", g_w[i], g_h[i]);
    }
}

static int clamp_mode(uint32_t m) { return m >= (uint32_t)g_n ? 0 : (int)m; }

void f_004ecf30(Cpu *restrict cpu) { RET(g_w[clamp_mode(ARG(0))], 1); }
void f_004ecf80(Cpu *restrict cpu) { RET(g_h[clamp_mode(ARG(0))], 1); }
void f_004ecfd0(Cpu *restrict cpu) { cpu->eax = g_w[clamp_mode(g_rd32(cpu->ecx + 0x54))]; cpu->esp += 4; }
void f_004ed010(Cpu *restrict cpu) { cpu->eax = g_h[clamp_mode(g_rd32(cpu->ecx + 0x54))]; cpu->esp += 4; }
void f_004ed070(Cpu *restrict cpu) {
    int m = clamp_mode(g_rd32(cpu->ecx + 0x54));
    uint32_t out = ARG(0);
    g_wr32(out, g_w[m]); g_wr32(out + 4, g_h[m]);
    RET(out, 1);
}

int rt_video_mode_count(void) { return g_n; }
void rt_video_mode(int i, int *w, int *h) { i = clamp_mode((uint32_t)i); *w = g_w[i]; *h = g_h[i]; }

// 0x4ecf00: thiscall options->SetVideoMode(int) — clamp to the mode list (originally 0..3), then 0x4ec740.
void f_004ecf00(Cpu *restrict cpu) {
    int32_t m = (int32_t)ARG(0);
    if (m < 0) m = 0;
    if (m >= g_n) m = g_n - 1;
    uint32_t self = cpu->ecx;
    g_wr32(self + 0x54, (uint32_t)m);
    guest_thiscall(cpu, f_004ec740, self, 0, NULL);
    RET(cpu->eax, 1);
}

// ---- Options menu arrow selector (index at this+0xd4)

// Store the selection in the options and tell a running game the new size (originally a hardcoded table).
static void selector_apply(Cpu *cpu, uint32_t self) {
    uint32_t idx = g_rd32(self + 0xd4), opts = g_rd32(G_OPTIONS);
    guest_thiscall(cpu, f_004ecf00, opts, 1, &idx);
    uint32_t esp0 = cpu->esp;
    cpu->esp -= 32;
    uint32_t buf = cpu->esp + 8;   // VideoMode {w, h, bpp, flag}
    guest_thiscall(cpu, f_0040ee50, g_rd32(G_VIDEO), 1, &buf);
    int m = clamp_mode(idx);
    g_wr32(buf, g_w[m]); g_wr32(buf + 4, g_h[m]); g_wr32(buf + 8, 16);
    uint32_t j = g_rd32(G_JUEGO);
    if (j) guest_thiscall(cpu, f_004546e0, j, 1, &buf);
    cpu->esp = esp0;
}
static void selector_step(Cpu *cpu, uint32_t self, int d) {
    int32_t cur = (int32_t)g_rd32(self + 0xd4), nxt = cur + d;
    if (nxt < 0) nxt = 0;
    if (nxt >= g_n) nxt = g_n - 1;
    if (nxt == cur) return;
    g_wr32(self + 0xd4, (uint32_t)nxt);
    selector_apply(cpu, self);
}
void f_00476d20(Cpu *restrict cpu) { selector_step(cpu, cpu->ecx, -1); cpu->esp += 4; }   // previous
void f_00476de0(Cpu *restrict cpu) { selector_step(cpu, cpu->ecx, +1); cpu->esp += 4; }   // next
void f_00476ec0(Cpu *restrict cpu) { selector_apply(cpu, cpu->ecx); RET(0, 1); }          // apply current

// 0x4762b0: selector mouse handler. Its left arrow is inline code that only steps 2 -> 1 -> 0 (anything else
// becomes 0), so clicks on either arrow are handled here; everything else goes to the original.
void f_004762b0(Cpu *restrict cpu) {
    uint32_t self = cpu->ecx, msg = ARG(1), mtype = g_rd32(msg);   // (?, message*)
    if (mtype != 0x202 && mtype != 0x208) { orig_f_004762b0(cpu); return; }
    int32_t x = (int32_t)g_rd32(msg + 4), y = (int32_t)g_rd32(msg + 8);
    int32_t bx = (int32_t)g_rd32(self + 0xb0), by = (int32_t)g_rd32(self + 0xb4) + 0x81;
    if (y >= by && y < by + 0x12) {
        if (x >= bx + 0xc0 && x < bx + 0xd4) selector_step(cpu, self, -1);
        else if (x >= bx + 0x1ad && x < bx + 0x1c1) selector_step(cpu, self, +1);
    }
    uint32_t args[2] = { ARG(0), ARG(1) };
    uint32_t r = guest_thiscall(cpu, f_008d78b0, self, 2, args);   // base widget handler
    RET(r, 2);
}

// 0x476110: selector label draw. Its switch only draws indices 0..2; draw with index 2 (key R024) and let
// hook_video_label substitute the real mode's text.
static int g_label_idx;
void f_00476110(Cpu *restrict cpu) {
    uint32_t self = cpu->ecx, idx = g_rd32(self + 0xd4);
    g_label_idx = clamp_mode(idx);
    if (idx > 2) g_wr32(self + 0xd4, 2);
    orig_f_00476110(cpu);
    g_wr32(self + 0xd4, idx);
}

// site 0x47614a: label draw -> string table lookup 0x414ef0, thiscall(key, 0); keys R640/R800/R024.
void hook_video_label(Cpu *restrict cpu) {
    uint32_t key = ARG(0);
    if (key != 0xb0bf44u && key != 0xb0bf4cu && key != 0xb0bf54u) { f_00414ef0(cpu); return; }
    make_labels();
    RET(g_label[g_label_idx], 2);
}

// ---- Video options list (0x474f90)

// site 0x475028: list->SetRange(min 0, max 3, cur) 0x8dfb90 -> max = last mode.
void hook_video_list_range(Cpu *restrict cpu) {
    g_wr32(cpu->esp + 8, (uint32_t)(g_n - 1));
    f_008dfb90(cpu);
}
// site 0x475066: list->SetLabels(fourccs, 4, colour, key colour) 0x8dff50 -> one label per mode.
void hook_video_list_labels(Cpu *restrict cpu) {
    make_labels();
    uint32_t self = cpu->ecx, size = (uint32_t)g_n * 4;
    uint32_t tab = guest_thiscall(cpu, f_004113f0, 0, 1, &size);   // game allocator (cdecl), freed with the list
    for (int i = 0; i < g_n; i++) g_wr32(tab + 4 * i, g_label[i]);
    g_wr32(self + 0xe0, (uint32_t)g_n);
    g_wr32(self + 0xdc, tab);
    g_wr32(self + 0xe8, ARG(2));
    g_wr32(self + 0xe4, ARG(3));
    RET(self, 4);
}
