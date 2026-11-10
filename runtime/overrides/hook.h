// Helpers for overrides ported from Commandos2-ModernPatch (Steam build hooks, remapped to the retail exe).
// See runtime/overrides/ADDRESSES.md for the Steam -> retail address map.
#pragma once
#include "../rt.h"
#include "gen.h"
#include <string.h>

static inline float g_rdf(uint32_t a) { float f; memcpy(&f, g_mem + a, 4); return f; }
static inline void g_wrf(uint32_t a, float f) { memcpy(g_mem + a, &f, 4); }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// Call a lifted guest function directly (bypassing dispatch, so an override can reach its orig_f_ version).
// thiscall/stdcall: the callee pops its arguments. Returns eax.
static inline uint32_t guest_thiscall(Cpu *cpu, GuestFn fn, uint32_t self, int nargs, const uint32_t *args) {
    uint32_t esp0 = cpu->esp;
    for (int i = nargs - 1; i >= 0; i--) { cpu->esp -= 4; g_wr32(cpu->esp, args[i]); }
    cpu->esp -= 4; g_wr32(cpu->esp, 0xFFFFFFF0u);   // fake return address
    cpu->ecx = self;
    fn(cpu);
    if (cpu->esp != esp0) LOG("guest_thiscall: esp imbalance %d", (int)(cpu->esp - esp0));
    cpu->esp = esp0;
    return cpu->eax;
}

// Settings from Commandos2-ModernPatch.ini in the game dir (same keys as the Windows patch).
typedef struct {
    int hd_menus;       // HDMenus
    int enabled;        // Enabled (frame interpolation)
    int camera, objects, mouse, menus;
    int max_fps;        // MaxFps, 0 = display refresh
    int debug;
} ModCfg;
const ModCfg *mod_cfg(void);
