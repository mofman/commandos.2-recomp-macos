// HD menus / loading screens, ported from Commandos2-ModernPatch (PatchHDMenus, LoadScreenMode640/RestoreMode).
//
// The pause menu keeps the game resolution: two byte patches in recomp/overrides.txt (pause activate 0x464270).
//
// Loading screens draw a fixed 640x480 layout and present frames themselves. Originally the display was 640x480
// whenever they appeared from the pause menu; now that the pause menu stays in HD they would show up in a corner.
// Before a loading screen starts the display is switched to 640x480 with the video object's own mode change
// (the game's SetMode 0x402120 would also notify the half-loaded mission). After the logic update that ran the
// loading screen, if the game is still on the same screen (quick save / quick load) the previous mode is restored;
// otherwise the briefing / mission set their resolution themselves. Mission activate keeps 640x480 while a reload
// is pending and switches to the game resolution after it.
#include "hook.h"

// comm2.exe 1.2 addresses (Steam in brackets)
#define G_JUEGO        0xb58488u   // CJuego*            [0xBCBAD4]
#define G_OPTIONS      0xb58490u   // options / video settings [0xBCBADC]
#define G_VIDEO        0xb58654u   // video object*      [0xC15BDC]
#define J_STATE        0x2c        // current screen index

typedef struct { int32_t w, h, bpp; uint8_t flag, pad[3]; } VideoMode;

static uint32_t g_modebuf;          // guest scratch for a VideoMode
static VideoMode g_mode_before;
static int g_switched, g_state_before = -1;
static uint32_t g_mission_screen;

static uint32_t modebuf(void) { if (!g_modebuf) g_modebuf = galloc(32); return g_modebuf; }

static void get_mode(Cpu *cpu, uint32_t video, VideoMode *m) {
    uint32_t b = modebuf(), a = b;
    memset(gptr(b), 0, sizeof *m);
    guest_thiscall(cpu, f_0040ee50, video, 1, &a);
    memcpy(m, gptr(b), sizeof *m);
}
static int set_mode(Cpu *cpu, uint32_t video, const VideoMode *m) {
    uint32_t b = modebuf(), a = b;
    memcpy(gptr(b), m, sizeof *m);
    return (int)guest_thiscall(cpu, f_0040ee70, video, 1, &a);
}

static void loadscreen_mode640(Cpu *cpu) {
    if (!mod_cfg()->hd_menus) return;
    uint32_t j = g_rd32(G_JUEGO), video = g_rd32(G_VIDEO);
    if (!j || !video) return;
    uint32_t save[3] = { cpu->eax, cpu->ecx, cpu->edx };
    VideoMode m;
    get_mode(cpu, video, &m);
    if (m.w != 640 || m.h != 480) {
        g_mode_before = m;
        g_switched = 1;
        g_state_before = (int)g_rd32(j + J_STATE);
        m.w = 640; m.h = 480; m.bpp = 16;
        int r = set_mode(cpu, video, &m);
        LOG("modpatch: loading screen: display switched to 640x480 (%d)", r);
    }
    cpu->eax = save[0]; cpu->ecx = save[1]; cpu->edx = save[2];
}

// called after every logic update (main loop hook)
void hdmenus_after_update(Cpu *cpu) {
    if (!g_switched) return;
    g_switched = 0;
    uint32_t j = g_rd32(G_JUEGO), video = g_rd32(G_VIDEO);
    if (j && video && (int)g_rd32(j + J_STATE) == g_state_before) {
        int r = set_mode(cpu, video, &g_mode_before);
        LOG("modpatch: loading screen ended on the same screen: display restored to %dx%d (%d)", g_mode_before.w, g_mode_before.h, r);
    }
}

// site 0x517935: mission load -> loading screen begin 0x4e1490 (thiscall, 3 args)   [0x44c901 -> 0x4079d0]
void hook_loadscreen_begin(Cpu *restrict cpu) { loadscreen_mode640(cpu); f_004e1490(cpu); }
// site 0x518fbf: saved game load -> generic loading screen 0x4e0d40 (thiscall, 1 arg)   [0x44d4c5 -> 0x4075c0]
void hook_loadscreen_begin_generic(Cpu *restrict cpu) { loadscreen_mode640(cpu); f_004e0d40(cpu); }

// site 0x464e9d: mission activate 0x464e80 -> screen SetMode 0x474f60 (thiscall w, h)   [0x7abc2d -> 0x7add80]
void hook_mission_activate_mode(Cpu *restrict cpu) {
    g_mission_screen = cpu->ecx;
    if (mod_cfg()->hd_menus && g_mem[cpu->ecx + 9]) {    // reload pending: stay 640x480, switch after it
        g_wr32(cpu->esp + 4, 640);
        g_wr32(cpu->esp + 8, 480);
    }
    f_00474f60(cpu);
}

// site 0x464eb7: mission activate -> mission reload 0x518ea0 (thiscall, no args)   [0x7abc46 -> 0x447640]
void hook_mission_reload(Cpu *restrict cpu) {
    f_00518ea0(cpu);
    if (!mod_cfg()->hd_menus || !g_mission_screen) return;
    uint32_t save[3] = { cpu->eax, cpu->ecx, cpu->edx };
    uint32_t opt = g_rd32(G_OPTIONS);
    int w = (int)guest_thiscall(cpu, f_004ecfd0, opt, 0, NULL);
    int h = (int)guest_thiscall(cpu, f_004ed010, opt, 0, NULL);
    if (w > 0 && h > 0) {
        uint32_t a[2] = { (uint32_t)w, (uint32_t)h };
        guest_thiscall(cpu, f_00474f60, g_mission_screen, 2, a);
    }
    cpu->eax = save[0]; cpu->ecx = save[1]; cpu->edx = save[2];
}

// Popup dialogs (e.g. the 1.2 "exit game" confirm in the pause menu): the dialog setup 0x8e2de0 centres the box with
// the fixed menu size getters (0x4ed050 = 640, 0x4ed060 = 480), so on an HD screen it lands in the top-left 640x480
// corner while the pause menu is centred. Use the current display size instead (same result at 640x480).
static int display_size(Cpu *cpu, int height) {
    uint32_t video = g_rd32(G_VIDEO);
    if (!mod_cfg()->hd_menus || !video) return 0;
    VideoMode m;
    get_mode(cpu, video, &m);
    int v = height ? m.h : m.w;
    return v >= (height ? 480 : 640) ? v : 0;
}
// site 0x8e2ecf / 0x8e2eef: thiscall(options) menu width / height getter
void hook_dialog_width(Cpu *restrict cpu) {
    int v = display_size(cpu, 0);
    if (!v) { f_004ed050(cpu); return; }
    cpu->eax = (uint32_t)v; cpu->esp += 4;
}
void hook_dialog_height(Cpu *restrict cpu) {
    int v = display_size(cpu, 1);
    if (!v) { f_004ed060(cpu); return; }
    cpu->eax = (uint32_t)v; cpu->esp += 4;
}
