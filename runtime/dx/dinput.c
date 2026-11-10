// DirectInput 7 (mouse + keyboard devices) on top of platform input state.
#include "../rt.h"
#include "../platform.h"
#include <string.h>

#define DI_OK 0
typedef struct { int is_mouse; uint32_t data_size; } Dev;

static void D_GetCapabilities(Cpu *cpu) {
    uint32_t c = ARG(1), sz = g_rd32(c);
    memset(gptr(c + 4), 0, sz - 4);
    Dev *d = HOST(Dev);
    g_wr32(c + 4, 5);                                  // DIDC_ATTACHED|DIDC_POLLEDDEVICE
    g_wr32(c + 8, d->is_mouse ? 0x12 : 0x13);         // DIDEVTYPE_MOUSE / KEYBOARD
    g_wr32(c + 12, d->is_mouse ? 3 : 0);              // axes
    g_wr32(c + 16, d->is_mouse ? 3 : 128);            // buttons
    RET(DI_OK, 2);
}
static void D_SetProperty(Cpu *cpu) { RET(DI_OK, 3); }
static void D_GetProperty(Cpu *cpu) { RET(DI_OK, 3); }
static void D_Acquire(Cpu *cpu) { RET(DI_OK, 1); }
static void D_Unacquire(Cpu *cpu) { RET(DI_OK, 1); }
static void D_SetDataFormat(Cpu *cpu) {
    Dev *d = HOST(Dev);
    d->data_size = g_rd32(ARG(1) + 12);
    TRACE("DI SetDataFormat size=%u", d->data_size);
    RET(DI_OK, 2);
}
static void D_SetEventNotification(Cpu *cpu) { RET(DI_OK, 2); }
static void D_SetCooperativeLevel(Cpu *cpu) { RET(DI_OK, 3); }
static void D_Poll(Cpu *cpu) { RET(DI_OK, 1); }
static void D_GetDeviceState(Cpu *cpu) {
    Dev *d = HOST(Dev);
    uint32_t n = ARG(1), p = ARG(2);
    plat_pump();
    memset(gptr(p), 0, n);
    if (d->is_mouse) {
        g_wr32(p, (uint32_t)g_mouse_dx); g_wr32(p + 4, (uint32_t)g_mouse_dy);
        if (n >= 12) g_wr32(p + 8, (uint32_t)g_mouse_dz);
        g_mouse_dx = g_mouse_dy = g_mouse_dz = 0;
        for (uint32_t i = 0; i < 4 && 12 + i < n; i++) gptr(p + 12)[i] = g_mouse_btn[i];
    } else {
        memcpy(gptr(p), g_dik_down, n < 256 ? n : 256);
    }
    RET(DI_OK, 3);
}
static void D_GetDeviceData(Cpu *cpu) {
    // (this, cbObjectData, rgdod, pdwInOut, dwFlags)
    Dev *d = HOST(Dev);
    uint32_t osz = ARG(1), arr = ARG(2), inout = ARG(3), flags = ARG(4);
    plat_pump();
    uint32_t want = g_rd32(inout), got = 0;
    if (!d->is_mouse) { g_wr32(inout, 0); RET(DI_OK, 5); }
    if (!arr) {   // flush
        DIEvt e;
        while (plat_mouse_event_pop(&e)) got++;
        g_wr32(inout, got);
        RET(DI_OK, 5);
    }
    DIEvt e;
    while (got < want && !(flags & 1) && plat_mouse_event_pop(&e)) {
        uint32_t o = arr + got * osz;
        g_wr32(o, e.ofs); g_wr32(o + 4, e.data); g_wr32(o + 8, e.ts); g_wr32(o + 12, e.seq);
        got++;
    }
    g_wr32(inout, got);
    RET(DI_OK, 5);
}

static const ComMethod DEV_VT[] = {
    {"QueryInterface", 0}, {"AddRef", 0}, {"Release", 0}, {"GetCapabilities", D_GetCapabilities},
    {"EnumObjects", 0}, {"GetProperty", D_GetProperty}, {"SetProperty", D_SetProperty}, {"Acquire", D_Acquire},
    {"Unacquire", D_Unacquire}, {"GetDeviceState", D_GetDeviceState}, {"GetDeviceData", D_GetDeviceData},
    {"SetDataFormat", D_SetDataFormat}, {"SetEventNotification", D_SetEventNotification},
    {"SetCooperativeLevel", D_SetCooperativeLevel}, {"GetObjectInfo", 0}, {"GetDeviceInfo", 0},
    {"RunControlPanel", 0}, {"Initialize", 0}, {"CreateEffect", 0}, {"EnumEffects", 0}, {"GetEffectInfo", 0},
    {"GetForceFeedbackState", 0}, {"SendForceFeedbackCommand", 0}, {"EnumCreatedEffectObjects", 0},
    {"Escape", 0}, {"Poll", D_Poll}, {"SendDeviceData", 0}, {"EnumEffectsInFile", 0}, {"WriteEffectToFile", 0},
};

static uint32_t make_dev(uint32_t guid) {
    Dev *d = calloc(1, sizeof *d);
    d->is_mouse = g_rd32(guid) == 0x6F1D2B60u;
    LOG("DirectInput device: %s", d->is_mouse ? "mouse" : "keyboard");
    return com_new(d->is_mouse ? "IDirectInputDevice7(mouse)" : "IDirectInputDevice7(kbd)", DEV_VT, sizeof DEV_VT / sizeof *DEV_VT, d);
}
static void DI_CreateDevice(Cpu *cpu) { g_wr32(ARG(2), make_dev(ARG(1))); RET(DI_OK, 4); }
static void DI_CreateDeviceEx(Cpu *cpu) { g_wr32(ARG(3), make_dev(ARG(1))); RET(DI_OK, 5); }
static void DI_EnumDevices(Cpu *cpu) { RET(DI_OK, 5); }
static void DI_GetDeviceStatus(Cpu *cpu) { RET(DI_OK, 2); }

static const ComMethod DI_VT[] = {
    {"QueryInterface", 0}, {"AddRef", 0}, {"Release", 0}, {"CreateDevice", DI_CreateDevice},
    {"EnumDevices", DI_EnumDevices}, {"GetDeviceStatus", DI_GetDeviceStatus}, {"RunControlPanel", 0},
    {"Initialize", 0}, {"FindDevice", 0}, {"CreateDeviceEx", DI_CreateDeviceEx},
};

SHIM(DINPUT, DirectInputCreateEx) {
    LOG("DirectInputCreateEx(version %x)", ARG(1));
    g_wr32(ARG(3), com_new("IDirectInput7", DI_VT, sizeof DI_VT / sizeof *DI_VT, NULL));
    RET(DI_OK, 5);
}
