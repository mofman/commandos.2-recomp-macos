// Game bug fix ported from Commandos2-ModernPatch (PatchTileBlitOverflow / SplitTileBlit).
//
// The scaled map-background blitters (1.2 0x95ef80 and its blending twin 0x95f7f0; Steam 0x856880 /
// 0x856ea0) split the source into 64x64 tiles and keep one entry per tile column/row in fixed stack arrays.
// A large screen combined with zoom needs more tiles than that: the arrays overflow and the game crashes.
// Large blits are split into rectangles that each fit.
//
//   bool __thiscall Blit(uint16_t *dst, int pitchPx, float srcX, float srcY, int w, int h, uint32_t extra)
//   this+0x30 = source px per screen px, this+0x20 = source px per world unit (srcX/srcY are world units).
//
// The companion fix (tile cache column cap 24 -> 96, 1.2 0x9522ce) is a byte patch in recomp/overrides.txt.
#include "hook.h"

static void split_blit(Cpu *restrict cpu, GuestFn orig) {
    const uint32_t self = cpu->ecx;
    const uint32_t dst = ARG(0);
    const int pitch = (int)ARG(1);
    const float srcX = g_rdf(cpu->esp + 12), srcY = g_rdf(cpu->esp + 16);
    const int w = (int)ARG(4), h = (int)ARG(5);
    const uint32_t extra = ARG(6);

    const float step = g_rdf(self + 0x30);
    const float px_per_unit = g_rdf(self + 0x20);
    const float max_src = 64.0f * 16;     // keep each call well within the arrays
    int chunk = step > 0 ? (int)(max_src / step) : w;
    if (chunk < 16) chunk = 16;
    static int force = -1;
    if (force < 0) force = getenv("C2_SPLIT_TEST") ? atoi(getenv("C2_SPLIT_TEST")) : 0;
    if (force) chunk = force;             // testing: split every blit into force x force pieces
    if ((w <= chunk && h <= chunk) || px_per_unit <= 0 || w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        orig(cpu);                        // unchanged call, return address already on the stack
        return;
    }
    static int logged;
    if (!logged++) LOG("modpatch: splitting map blit %dx%d (chunk %d, step %.3f)", w, h, chunk, step);
    const float units_per_px = step / px_per_unit;
    uint32_t result = 0;
    for (int y = 0; y < h; y += chunk) {
        int ch = h - y < chunk ? h - y : chunk;
        for (int x = 0; x < w; x += chunk) {
            int cw = w - x < chunk ? w - x : chunk;
            uint32_t a[7] = { dst + (uint32_t)((y * pitch + x) * 2), (uint32_t)pitch,
                              f2u(srcX + x * units_per_px), f2u(srcY + y * units_per_px),
                              (uint32_t)cw, (uint32_t)ch, extra };
            result |= guest_thiscall(cpu, orig, self, 7, a) & 0xFF;
        }
    }
    cpu->ecx = self;
    RET(result, 7);
}

void f_0095ef80(Cpu *restrict cpu) { split_blit(cpu, orig_f_0095ef80); }
void f_0095f7f0(Cpu *restrict cpu) { split_blit(cpu, orig_f_0095f7f0); }
