// GDI32, ADVAPI32, WINMM, ole32, COMCTL32, comdlg32, IMM32, DPLAYX shims.
#include "../rt.h"
#include "../platform.h"
#include <string.h>
#include <strings.h>
#include <time.h>

// ------------------------------------------------------------------ GDI32 (only used for desktop queries / editor)
SHIM(GDI32, GetDeviceCaps) {
    switch (ARG(1)) {
    case 8: RET(1920, 2);    // HORZRES
    case 10: RET(1080, 2);   // VERTRES
    case 12: RET(32, 2);     // BITSPIXEL
    case 14: RET(1, 2);      // PLANES
    case 88: case 90: RET(96, 2);   // LOGPIXELSX/Y
    case 116: RET(60, 2);    // VREFRESH
    }
    RET(0, 2);
}
SHIM(GDI32, GetStockObject) { RET(0x40000 + ARG(0), 1); }
SHIM(GDI32, CreateFontIndirectA) { RET(0x40100, 1); }
SHIM(GDI32, CreateSolidBrush) { RET(0x40200, 1); }
SHIM(GDI32, SelectObject) { RET(0x40000, 2); }
SHIM(GDI32, DeleteObject) { RET(1, 1); }
SHIM(GDI32, GetObjectA) { memset(gptr(ARG(2)), 0, ARG(1)); RET(ARG(1), 3); }
SHIM(GDI32, SetStretchBltMode) { RET(1, 2); }
SHIM(GDI32, BitBlt) { RET(1, 9); }
SHIM(GDI32, StretchBlt) { RET(1, 11); }

// ------------------------------------------------------------------ ADVAPI32: registry as a small in-memory table
// HKLM\SOFTWARE\Pyro\Comm2\1.0 values the game may enumerate.
static const struct { const char *name; uint32_t type; const char *sval; uint32_t dval; } g_reg[] = {
    {"Path", 1, "C:\\Commandos2", 0},
    {"InstallPath", 1, "C:\\Commandos2", 0},
    {"Language", 1, "English", 0},
};
#define NREG (sizeof g_reg / sizeof *g_reg)
SHIM(ADVAPI32, RegOpenKeyExA) {
    const char *k = ARG(1) ? ARGP(1) : "";
    TRACE("RegOpenKeyExA(%08x, %s)", ARG(0), k);
    if (strcasestr(k, "Pyro\\Comm2")) { g_wr32(ARG(4), 0x80001234u); RET(0, 5); }
    RET(2, 5);   // ERROR_FILE_NOT_FOUND
}
SHIM(ADVAPI32, RegCloseKey) { RET(0, 1); }
SHIM(ADVAPI32, RegQueryInfoKeyA) {
    // (hKey, lpClass, lpcClass, lpReserved, lpcSubKeys, lpcMaxSubKeyLen, lpcMaxClassLen, lpcValues, lpcMaxValueNameLen, lpcMaxValueLen, lpcbSecurityDescriptor, lpftLastWriteTime)
    if (ARG(4)) g_wr32(ARG(4), 0);
    if (ARG(5)) g_wr32(ARG(5), 0);
    if (ARG(6)) g_wr32(ARG(6), 0);
    if (ARG(7)) g_wr32(ARG(7), NREG);
    if (ARG(8)) g_wr32(ARG(8), 32);
    if (ARG(9)) g_wr32(ARG(9), 260);
    RET(0, 12);
}
SHIM(ADVAPI32, RegEnumValueA) {
    // (hKey, index, name, *nameLen, reserved, *type, data, *dataLen)
    uint32_t i = ARG(1);
    if (i >= NREG) RET(259, 8);   // ERROR_NO_MORE_ITEMS
    strcpy(ARGP(2), g_reg[i].name);
    g_wr32(ARG(3), (uint32_t)strlen(g_reg[i].name));
    if (ARG(5)) g_wr32(ARG(5), g_reg[i].type);
    if (g_reg[i].type == 1) {
        uint32_t n = (uint32_t)strlen(g_reg[i].sval) + 1;
        if (ARG(6)) strcpy(ARGP(6), g_reg[i].sval);
        if (ARG(7)) g_wr32(ARG(7), n);
    } else {
        if (ARG(6)) g_wr32(ARG(6), g_reg[i].dval);
        if (ARG(7)) g_wr32(ARG(7), 4);
    }
    RET(0, 8);
}

// ------------------------------------------------------------------ WINMM
SHIM(WINMM, timeGetTime) { RET(plat_time_ms(), 0); }

// ------------------------------------------------------------------ ole32
SHIM(OLE32, CoInitialize) { RET(0, 1); }
SHIM(OLE32, CoUninitialize) { RET(0, 0); }
SHIM(OLE32, CoCreateInstance) {
    uint32_t clsid = ARG(0);
    LOG("CoCreateInstance(%08x-...) -> REGDB_E_CLASSNOTREG", g_rd32(clsid));
    if (ARG(4)) g_wr32(ARG(4), 0);
    RET(0x80040154u, 5);
}

// ------------------------------------------------------------------ misc
SHIM(COMCTL32, ord17) { RET(0, 0); }          // InitCommonControls
SHIM(COMDLG32, ChooseFontA) { RET(0, 1); }
SHIM(COMDLG32, GetSaveFileNameA) { RET(0, 1); }
SHIM(IMM32, ImmGetContext) { RET(0, 1); }
SHIM(IMM32, ImmSetOpenStatus) { RET(1, 2); }
SHIM(DPLAYX, ord4) {                          // DirectPlayLobbyCreateA(guid, lplpDPL, outer, data, size)
    LOG("DirectPlayLobbyCreateA -> unavailable (multiplayer disabled)");
    if (ARG(1)) g_wr32(ARG(1), 0);
    RET(0x80004005u, 5);
}
