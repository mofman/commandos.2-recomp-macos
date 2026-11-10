// Reads Commandos2-ModernPatch.ini (game dir), the same file and keys the Windows patch uses.
#include "hook.h"
#include <ctype.h>
#include <strings.h>

static ModCfg g_cfg = { .hd_menus = 1, .enabled = 1,
                        .camera = 1, .objects = 1, .mouse = 1, .menus = 1, .max_fps = 0, .debug = 0 };
static int g_loaded;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

const ModCfg *mod_cfg(void) {
    if (g_loaded) return &g_cfg;
    g_loaded = 1;
    char path[1024];
    rt_config_path("Commandos2-ModernPatch.ini", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) { LOG("modpatch: %s not found, using defaults", path); return &g_cfg; }
    char line[256];
    int in_section = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;   // BOM
        char *c = strchr(p, ';'); if (c) *c = 0;
        p = trim(p);
        if (*p == '[') { in_section = !strncasecmp(p, "[Commandos2-ModernPatch]", 24); continue; }
        char *eq = strchr(p, '=');
        if (!in_section || !eq) continue;
        *eq = 0;
        char *k = trim(p), *v = trim(eq + 1);
        if (!strcasecmp(k, "HDMenus")) g_cfg.hd_menus = atoi(v) != 0;
        else if (!strcasecmp(k, "Enabled")) g_cfg.enabled = atoi(v) != 0;
        else if (!strcasecmp(k, "Camera")) g_cfg.camera = atoi(v) != 0;
        else if (!strcasecmp(k, "Objects")) g_cfg.objects = atoi(v) != 0;
        else if (!strcasecmp(k, "Mouse")) g_cfg.mouse = atoi(v) != 0;
        else if (!strcasecmp(k, "Menus")) g_cfg.menus = atoi(v) != 0;
        else if (!strcasecmp(k, "MaxFps")) g_cfg.max_fps = atoi(v);
        else if (!strcasecmp(k, "Debug")) g_cfg.debug = atoi(v);
    }
    fclose(f);
    LOG("modpatch: HDMenus=%d Enabled=%d MaxFps=%d", g_cfg.hd_menus, g_cfg.enabled, g_cfg.max_fps);
    return &g_cfg;
}
