// Core runtime: guest memory, PE mapping, dispatch, thunks, traps, string ops, x87 helpers.
#include "rt.h"
#include <sys/mman.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <limits.h>
#include <execinfo.h>

uint8_t *g_mem;
int g_trace_imports;
uint32_t g_hinstance = 0x400000;
static char g_game_dir[PATH_MAX];
static char g_exe_path[PATH_MAX];
static char g_user_dir[PATH_MAX];   // writable dir for OUTPUT\ (saves, profiles, settings) and the .ini files; "" = game dir

extern const struct { uint32_t addr; GuestFn fn; } g_dispatch_table[];
extern const uint32_t g_dispatch_count;
extern const struct ImportDef { uint32_t iat; const char *dll; const char *name; GuestFn fn; } g_imports[];
extern const uint32_t g_import_count;

// ------------------------------------------------------------------ thunks
#define THUNK_BASE 0xFFF00000u
#define THUNK_MAX  8192
static struct { GuestFn fn; const char *name; } g_thunks[THUNK_MAX];
static uint32_t g_nthunks;
static pthread_mutex_t g_thunk_lock = PTHREAD_MUTEX_INITIALIZER;

__thread const char *t_thunk_name;

uint32_t rt_thunk(GuestFn fn, const char *name) {
    pthread_mutex_lock(&g_thunk_lock);
    for (uint32_t i = 0; i < g_nthunks; i++)
        if (g_thunks[i].fn == fn && (g_thunks[i].name == name || (name && g_thunks[i].name && !strcmp(g_thunks[i].name, name)))) {
            pthread_mutex_unlock(&g_thunk_lock); return THUNK_BASE + 16 * i;
        }
    if (g_nthunks >= THUNK_MAX) { fprintf(stderr, "thunk table full\n"); abort(); }
    g_thunks[g_nthunks].fn = fn;
    g_thunks[g_nthunks].name = name;
    uint32_t a = THUNK_BASE + 16 * g_nthunks++;
    pthread_mutex_unlock(&g_thunk_lock);
    return a;
}

// ------------------------------------------------------------------ dispatch
static __thread uint32_t t_last_addr;
static __thread GuestFn t_last_fn;

static GuestFn lookup(uint32_t a) {
    uint32_t lo = 0, hi = g_dispatch_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_dispatch_table[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    if (lo < g_dispatch_count && g_dispatch_table[lo].addr == a) return g_dispatch_table[lo].fn;
    return NULL;
}

void dispatch(Cpu *cpu, uint32_t target) {
    if (target == t_last_addr && t_last_fn) { t_last_fn(cpu); return; }
    if (target >= THUNK_BASE) {
        uint32_t i = (target - THUNK_BASE) / 16;
        if (i < g_nthunks && (target & 15) == 0) { t_thunk_name = g_thunks[i].name; g_thunks[i].fn(cpu); return; }
        trap(cpu, target, "bad thunk address");
    }
    GuestFn f = lookup(target);
    if (!f) trap(cpu, target, "dispatch: no lifted function at target");
    t_last_addr = target; t_last_fn = f;
    f(cpu);
}

uint32_t call_guest(Cpu *cpu, uint32_t fn, int cdecl_, int nargs, ...) {
    va_list ap;
    uint32_t args[16];
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = cpu->esp;
    for (int i = nargs - 1; i >= 0; i--) { cpu->esp -= 4; g_wr32(cpu->esp, args[i]); }
    cpu->esp -= 4; g_wr32(cpu->esp, 0xFFFFFFF0u);   // fake return address
    dispatch(cpu, fn);
    if (cdecl_) cpu->esp += 4 * nargs;
    if (cpu->esp != esp0)
        LOG("call_guest(%08x): esp imbalance %d bytes", fn, (int)(cpu->esp - esp0));
    cpu->esp = esp0;
    return cpu->eax;
}

// ------------------------------------------------------------------ traps
static void host_bt(void) {
    void *f[64];
    int n = backtrace(f, 64);
    char **sy = backtrace_symbols(f, n);
    fprintf(stderr, "  host/guest call chain:");
    for (int i = 0; i < n; i++) {
        const char *q = strstr(sy[i], " f_");
        if (!q) q = strstr(sy[i], " imp_");
        if (!q) q = strstr(sy[i], " S_"); 
        if (q) { char b[64]; sscanf(q + 1, "%63s", b); fprintf(stderr, " %s", b); }
    }
    fprintf(stderr, "\n");
}

void rt_host_bt(void) { host_bt(); }
static void dump(Cpu *cpu) {
    host_bt();
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x\n  esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            cpu->eax, cpu->ecx, cpu->edx, cpu->ebx, cpu->esp, cpu->ebp, cpu->esi, cpu->edi);
    fprintf(stderr, "  stack:");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%s%08x", i % 8 ? " " : "\n    ", g_rd32(cpu->esp + 4 * i));
    fprintf(stderr, "\n  likely return addresses on stack:");
    int n = 0;
    for (uint32_t a = cpu->esp; a < cpu->esp + 0x2000 && n < 24; a += 4) {
        uint32_t v = g_rd32(a);
        if (v >= 0x401000 && v < 0xae5db1) { fprintf(stderr, " %08x", v); n++; }
    }
    fprintf(stderr, "\n");
}

void trap(Cpu *cpu, uint32_t addr, const char *why) {
    fprintf(stderr, "\n*** TRAP at %08x: %s\n", addr, why);
    dump(cpu);
    fflush(stderr);
    abort();
}

void rt_unimplemented(Cpu *cpu, const char *name) {
    fprintf(stderr, "\n*** UNIMPLEMENTED IMPORT: %s (called from %08x)\n", name, g_rd32(cpu->esp));
    dump(cpu);
    fflush(stderr);
    exit(3);
}

// ------------------------------------------------------------------ guest heap
// Regions: heap 0x10000000-0x70000000, valloc 0x70000000-0xE0000000.
#define HEAP_LO 0x10000000u
#define HEAP_HI 0x70000000u
#define VA_LO   0x70000000u
#define VA_HI   0xE0000000u
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_top = HEAP_LO, g_va_top = VA_LO;
// 32 small buckets, then 1 KiB through 2 GiB. Previously this stopped at
// 128 KiB, so every larger allocation leaked when the guest freed it.
#define NBUCKET 54
static uint32_t g_free[NBUCKET];   // singly linked via first dword

static int bucket_of(uint32_t sz, uint32_t *rounded) {
    // 16-byte granularity buckets up to 512, then powers of two
    if (sz <= 512) { uint32_t r = (sz + 15) & ~15u; if (!r) r = 16; *rounded = r; return r / 16 - 1; }
    int b = 32 - __builtin_clz(sz - 1);      // ceil log2
    *rounded = 1u << b;
    return 32 + (b - 10);                      // 1K -> 32 ...
}

uint32_t rt_heap_top(void) { return g_heap_top; }
uint32_t rt_va_top(void) { return g_va_top; }
uint32_t galloc(uint32_t size) {
    // Reject impossible sizes before rounding (which can otherwise wrap).
    if (size > HEAP_HI - HEAP_LO - 16) {
        LOG("guest heap: invalid allocation size %u", size);
        return 0;
    }
    uint32_t r;
    int b = bucket_of(size, &r);
    if (b >= NBUCKET) b = -1;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t p = 0;
    if (b >= 0 && g_free[b]) {
        p = g_free[b];
        g_free[b] = g_rd32(p);
    } else {
        if (b < 0) r = (size + 4095) & ~4095u;
        uint32_t hdr = g_heap_top;
        if (hdr > HEAP_HI - 16 || r > HEAP_HI - hdr - 16) {
            pthread_mutex_unlock(&g_heap_lock);
            LOG("guest heap exhausted: requested=%u capacity=%u top=%08x limit=%08x", size, r, hdr, HEAP_HI);
            host_bt();
            return 0;
        }
        g_heap_top += 16 + r;
        g_wr32(hdr, r);                // [p-16] capacity, [p-12] requested size, [p-8] magic
        g_wr32(hdr + 8, 0x48454150);   // 'HEAP'
        p = hdr + 16;
    }
    pthread_mutex_unlock(&g_heap_lock);
    g_wr32(p - 12, size);
    memset(gptr(p), 0, size);
    return p;
}

uint32_t gsize(uint32_t p) { return p ? g_rd32(p - 12) : 0; }

void gfree(uint32_t p) {
    if (!p) return;
    if (g_rd32(p - 8) != 0x48454150) { LOG("gfree: bad pointer %08x", p); return; }
    uint32_t r = g_rd32(p - 16), rr;
    int b = bucket_of(r, &rr);
    if (b < 0 || b >= NBUCKET || rr != r) return;
    pthread_mutex_lock(&g_heap_lock);
    g_wr32(p, g_free[b]);
    g_free[b] = p;
    pthread_mutex_unlock(&g_heap_lock);
}

uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return galloc(size);
    uint32_t cap = g_rd32(p - 16), old = gsize(p);
    if (size <= cap) { if (size > old) memset(gptr(p + old), 0, size - old); g_wr32(p - 12, size); return p; }
    uint32_t q = galloc(size);
    if (!q) return 0;
    memcpy(gptr(q), gptr(p), old < size ? old : size);
    gfree(p);
    return q;
}

uint32_t gvalloc(uint32_t size) {
    size = (size + 0xffff) & ~0xffffu;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t p = g_va_top;
    if (p + size > VA_HI) { pthread_mutex_unlock(&g_heap_lock); return 0; }
    g_va_top += size;
    pthread_mutex_unlock(&g_heap_lock);
    return p;
}

uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = galloc(n);
    memcpy(gptr(p), s, n);
    return p;
}

// ------------------------------------------------------------------ handles
#define MAX_HANDLES 4096
static Handle g_handles[MAX_HANDLES];
static pthread_mutex_t g_hlock = PTHREAD_MUTEX_INITIALIZER;

uint32_t h_new(HKind kind, void *p) {
    pthread_mutex_lock(&g_hlock);
    for (int i = 16; i < MAX_HANDLES; i++) {
        if (g_handles[i].kind == H_FREE) {
            g_handles[i] = (Handle){kind, p, -1, 0};
            pthread_mutex_unlock(&g_hlock);
            return 0x1000 + 4 * i;
        }
    }
    pthread_mutex_unlock(&g_hlock);
    LOG("out of handles");
    return 0;
}
Handle *h_get(uint32_t h) {
    if (h < 0x1000 || (h & 3)) return NULL;
    uint32_t i = (h - 0x1000) / 4;
    if (i >= MAX_HANDLES || g_handles[i].kind == H_FREE) return NULL;
    return &g_handles[i];
}
void h_free(uint32_t h) {
    Handle *x = h_get(h);
    if (x) x->kind = H_FREE;
}

// ------------------------------------------------------------------ threads / TIB
static __thread Cpu *t_cpu;
static __thread uint32_t t_last_error;
Cpu *rt_cpu(void) { return t_cpu; }
static Cpu *g_main_cpu;
int rt_is_main_thread(void) { return t_cpu == g_main_cpu; }
void rt_set_last_error(uint32_t e) { t_last_error = e; }
uint32_t rt_last_error(void) { return t_last_error; }

static uint32_t g_next_tid = 0x100;

uint32_t rt_new_thread_cpu(Cpu *c) {
    memset(c, 0, sizeof *c);
    uint32_t stack = gvalloc(GUEST_STACK_SIZE);
    uint32_t tib = galloc(0x1000);
    g_wr32(tib + 0x00, 0xFFFFFFFFu);                // SEH chain end
    g_wr32(tib + 0x04, stack + GUEST_STACK_SIZE);   // stack base (top)
    g_wr32(tib + 0x08, stack);                      // stack limit
    g_wr32(tib + 0x18, tib);                        // self
    g_wr32(tib + 0x20, 0x42);                       // pid
    g_wr32(tib + 0x24, __atomic_fetch_add(&g_next_tid, 4, __ATOMIC_SEQ_CST));
    c->fs_base = tib;
    c->esp = stack + GUEST_STACK_SIZE - 64;
    c->fcw = 0x27f;
    t_cpu = c;
    return c->esp;
}

// ------------------------------------------------------------------ string ops
void rt_string(Cpu *cpu, int op, int size, int rep) {
    uint8_t *m = g_mem;
    int32_t step = cpu->df ? -size : size;
    uint32_t mask = sz_mask(size);
    if (rep != REP_NONE && cpu->ecx == 0) return;
    // fast paths
    if (rep != REP_NONE && !cpu->df && op == STR_MOVS) {
        uint32_t n = cpu->ecx * size;
        memmove(PTR(cpu->edi), PTR(cpu->esi), n);
        cpu->esi += n; cpu->edi += n; cpu->ecx = 0;
        return;
    }
    if (rep != REP_NONE && !cpu->df && op == STR_STOS) {
        uint32_t n = cpu->ecx;
        if (size == 1) memset(PTR(cpu->edi), (uint8_t)cpu->eax, n);
        else for (uint32_t k = 0; k < n; k++) {
            if (size == 2) WR16(cpu->edi + 2 * k, cpu->eax); else WR32(cpu->edi + 4 * k, cpu->eax);
        }
        cpu->edi += n * size; cpu->ecx = 0;
        return;
    }
    for (;;) {
        uint32_t a = 0, b = 0;
        switch (op) {
        case STR_MOVS:
            if (size == 1) WR8(cpu->edi, RD8(cpu->esi)); else if (size == 2) WR16(cpu->edi, RD16(cpu->esi));
            else WR32(cpu->edi, RD32(cpu->esi));
            cpu->esi += step; cpu->edi += step; break;
        case STR_STOS:
            if (size == 1) WR8(cpu->edi, cpu->eax); else if (size == 2) WR16(cpu->edi, cpu->eax); else WR32(cpu->edi, cpu->eax);
            cpu->edi += step; break;
        case STR_LODS:
            if (size == 1) W8L(cpu->eax, RD8(cpu->esi)); else if (size == 2) W16(cpu->eax, RD16(cpu->esi));
            else cpu->eax = RD32(cpu->esi);
            cpu->esi += step; break;
        case STR_SCAS:
            a = cpu->eax & mask;
            b = size == 1 ? RD8(cpu->edi) : size == 2 ? RD16(cpu->edi) : RD32(cpu->edi);
            SETF(CC_SUB, size, a, b, (a - b) & mask);
            cpu->edi += step; break;
        case STR_CMPS:
            a = size == 1 ? RD8(cpu->esi) : size == 2 ? RD16(cpu->esi) : RD32(cpu->esi);
            b = size == 1 ? RD8(cpu->edi) : size == 2 ? RD16(cpu->edi) : RD32(cpu->edi);
            SETF(CC_SUB, size, a, b, (a - b) & mask);
            cpu->esi += step; cpu->edi += step; break;
        }
        if (rep == REP_NONE) return;
        if (--cpu->ecx == 0) return;
        if (op == STR_SCAS || op == STR_CMPS) {
            int z = a == b;
            if (rep == REP_E && !z) return;
            if (rep == REP_NE && z) return;
        }
    }
}

void rt_cpuid(Cpu *cpu) {
    switch (cpu->eax) {
    case 0: cpu->eax = 1; cpu->ebx = 0x756e6547; cpu->edx = 0x49656e69; cpu->ecx = 0x6c65746e; break; // GenuineIntel
    case 1: cpu->eax = 0x683; cpu->ebx = 0; cpu->ecx = 0; cpu->edx = 0x00000011; break;           // FPU + TSC
    default: cpu->eax = cpu->ebx = cpu->ecx = cpu->edx = 0;
    }
}

// ------------------------------------------------------------------ x87 80-bit
double f80_load(const uint8_t *p) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
    int sign = se >> 15, exp = se & 0x7fff;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
void f80_store(uint8_t *p, double v) {
    uint64_t mant = 0; uint16_t se = 0;
    if (v != 0) {
        int sign = signbit(v);
        if (isinf(v)) { se = 0x7fff; mant = 0x8000000000000000ull; }
        else if (isnan(v)) { se = 0x7fff; mant = 0xC000000000000000ull; }
        else {
            int e; double f = frexp(fabs(v), &e);  // f in [0.5,1)
            mant = (uint64_t)ldexp(f, 64);
            se = (uint16_t)(e - 1 + 16383);
        }
        if (sign) se |= 0x8000;
    } else if (signbit(v)) se = 0x8000;
    memcpy(p, &mant, 8); memcpy(p + 8, &se, 2);
}
uint32_t fxam(Cpu *cpu) {
    double v = ST(0);
    uint32_t c = 0;
    if (isnan(v)) c = 0x100;
    else if (isinf(v)) c = 0x500;
    else if (v == 0) c = 0x4000;
    else if (fpclassify(v) == FP_SUBNORMAL) c = 0x4400;
    else c = 0x400;
    if (signbit(v)) c |= 0x200;
    cpu->fsw = (cpu->fsw & ~0x4700u) | c;
    return c;
}

// ------------------------------------------------------------------ paths
const char *rt_game_dir(void) { return g_game_dir; }
const char *rt_user_dir(void) { return g_user_dir[0] ? g_user_dir : g_game_dir; }
// Host path of a user-editable config file (Commandos2-ModernPatch.ini).
char *rt_config_path(const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/%s", rt_user_dir(), name); return out; }

static char g_cwd[PATH_MAX] = "";   // guest cwd relative to game dir, using '/'

// Resolve one component case-insensitively inside dir.
static int ci_component(char *dir, const char *comp, size_t cap) {
    size_t dl = strlen(dir);
    snprintf(dir + dl, cap - dl, "/%s", comp);
    struct stat st;
    if (stat(dir, &st) == 0) return 1;
    dir[dl] = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcasecmp(e->d_name, comp) == 0) {
                snprintf(dir + dl, cap - dl, "/%s", e->d_name);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    snprintf(dir + dl, cap - dl, "/%s", comp);
    return 0;
}

char *rt_map_path(const char *wp, char *out, size_t cap) {
    char tmp[PATH_MAX];
    size_t n = 0;
    const char *s = wp;
    int absolute = 0;
    if (((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':') { s += 2; absolute = 1; }
    if (*s == '\\' || *s == '/') absolute = 1;
    if (!absolute && g_cwd[0]) n = snprintf(tmp, sizeof tmp, "%s/", g_cwd);
    for (; *s && n < sizeof tmp - 1; s++) tmp[n++] = (*s == '\\') ? '/' : *s;
    tmp[n] = 0;
    // normalise components
    char *comps[256]; int nc = 0;
    char *save = NULL;
    for (char *c = strtok_r(tmp, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) { if (nc) nc--; continue; }
        if (nc < 256) comps[nc++] = c;
    }
    // the virtual install dir C:\Commandos2 is the game dir itself
    int first = 0;
    if (absolute && nc && !strcasecmp(comps[0], "Commandos2")) first = 1;
    // OUTPUT\ (the only directory the game writes) lives in the user dir when one is set (packaged .app)
    snprintf(out, cap, "%s", g_user_dir[0] && nc > first && !strcasecmp(comps[first], "OUTPUT") ? g_user_dir : g_game_dir);
    for (int i = first; i < nc; i++) ci_component(out, comps[i], cap);
    return out;
}

void rt_set_cwd(const char *wp) {
    char host[PATH_MAX];
    rt_map_path(wp, host, sizeof host);
    size_t gl = strlen(g_game_dir), ul = strlen(g_user_dir);
    if (strncmp(host, g_game_dir, gl) == 0) snprintf(g_cwd, sizeof g_cwd, "%s", host + gl + (host[gl] == '/'));
    else if (ul && strncmp(host, g_user_dir, ul) == 0) snprintf(g_cwd, sizeof g_cwd, "%s", host + ul + (host[ul] == '/'));
}
const char *rt_get_cwd(void) { return g_cwd; }
const char *rt_exe_path(void) { return g_exe_path; }

// ------------------------------------------------------------------ PE loading
#include <fcntl.h>
static void load_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(len);
    fread(d, 1, len, f); fclose(f);
    uint32_t pe = *(uint32_t *)(d + 0x3c);
    uint16_t nsec = *(uint16_t *)(d + pe + 6), optsz = *(uint16_t *)(d + pe + 20);
    uint32_t opt = pe + 24, base = *(uint32_t *)(d + opt + 28), hdrsz = *(uint32_t *)(d + opt + 60);
    memcpy(gptr(base), d, hdrsz);
    for (int i = 0; i < nsec; i++) {
        uint8_t *s = d + opt + optsz + 40 * i;
        uint32_t vs = *(uint32_t *)(s + 8), va = *(uint32_t *)(s + 12), rs = *(uint32_t *)(s + 16), rp = *(uint32_t *)(s + 20);
        (void)vs;
        if (rp + rs > (uint32_t)len) rs = (uint32_t)len - rp;
        memcpy(gptr(base + va), d + rp, rs);
    }
    free(d);
    // byte patches from recomp/overrides.txt (already baked into the lifted code; data reads need them here too)
    extern const struct BytePatch { uint32_t addr; uint32_t len; const char *bytes; } g_patches[];
    extern const uint32_t g_patch_count;
    for (uint32_t i = 0; i < g_patch_count; i++) memcpy(gptr(g_patches[i].addr), g_patches[i].bytes, g_patches[i].len);
    for (uint32_t i = 0; i < g_import_count; i++)
        g_wr32(g_imports[i].iat, rt_thunk(g_imports[i].fn, g_imports[i].name));
}

uint32_t rt_import_thunk(const char *dll, const char *name) {
    for (uint32_t i = 0; i < g_import_count; i++)
        if (!strcasecmp(g_imports[i].name, name)) return g_rd32(g_imports[i].iat);
    (void)dll;
    return 0;
}

static void segv(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    uintptr_t a = (uintptr_t)si->si_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x100000000ull)
        fprintf(stderr, "\n*** guest memory fault at guest address %08x (sig %d)\n", (uint32_t)(a - (uintptr_t)g_mem), sig);
    else
        fprintf(stderr, "\n*** host fault at %p (sig %d)\n", si->si_addr, sig);
    Cpu *c = t_cpu;
    if (c) dump(c);
    signal(sig, SIG_DFL);
    abort();
}

void rt_platform_init(int argc, char **argv);   // platform.c (SDL)

// ---- packaged .app: <bundle>/Contents/Resources/game holds the (read-only) game, comm2.exe included; saves,
// profiles, settings and the .ini files go to ~/Library/Application Support/Commandos 2 Recomp (seeded on first run
// from the bundle's game/OUTPUT and game/*.ini). Outside a bundle nothing changes unless C2_USER_DIR is set.
#include <mach-o/dyld.h>
#include <copyfile.h>
static int bundle_resources(char *out, size_t cap) {
    char p[PATH_MAX], r[PATH_MAX];
    uint32_t n = sizeof p;
    if (_NSGetExecutablePath(p, &n) != 0 || !realpath(p, r)) return 0;
    char *m = strstr(r, ".app/Contents/MacOS/");
    if (!m) return 0;
    m[strlen(".app/Contents")] = 0;
    snprintf(out, cap, "%s/Resources/game", r);
    struct stat st;
    return stat(out, &st) == 0;
}
static void mkdirs(const char *path) {
    char t[PATH_MAX];
    snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}
static void seed_user_dir(void) {
    char src[PATH_MAX], dst[PATH_MAX];
    struct stat st;
    mkdirs(g_user_dir);
    snprintf(dst, sizeof dst, "%s/OUTPUT", g_user_dir);
    if (stat(dst, &st) != 0) {
        snprintf(src, sizeof src, "%s/OUTPUT", g_game_dir);
        if (stat(src, &st) == 0) copyfile(src, dst, NULL, COPYFILE_ALL | COPYFILE_RECURSIVE);
        else mkdir(dst, 0755);
    }
    static const char *const inis[] = { "Commandos2-ModernPatch.ini" };
    for (size_t i = 0; i < sizeof inis / sizeof *inis; i++) {
        snprintf(src, sizeof src, "%s/%s", g_game_dir, inis[i]);
        snprintf(dst, sizeof dst, "%s/%s", g_user_dir, inis[i]);
        if (stat(dst, &st) != 0 && stat(src, &st) == 0) copyfile(src, dst, NULL, COPYFILE_ALL);
    }
}

int main(int argc, char **argv) {
    char res[PATH_MAX], exe_def[PATH_MAX];
    const char *gd = getenv("C2_GAME_DIR"), *exe = getenv("C2_EXE"), *ud = getenv("C2_USER_DIR");
    int bundled = !gd && bundle_resources(res, sizeof res);
    if (bundled) {
        gd = res;
        snprintf(exe_def, sizeof exe_def, "%s/comm2.exe", res);
        if (!exe) exe = exe_def;
        static char def_ud[PATH_MAX];
        const char *home = getenv("HOME");
        snprintf(def_ud, sizeof def_ud, "%s/Library/Application Support/Commandos 2 Recomp", home ? home : "/tmp");
        if (!ud) ud = def_ud;
    }
    if (!gd) gd = "gamedata";
    realpath(gd, g_game_dir);
    if (!exe) {
        snprintf(exe_def, sizeof exe_def, "%s/comm2.exe", gd);
        exe = exe_def;
    }
    realpath(exe, g_exe_path);
    if (ud && *ud) {
        snprintf(g_user_dir, sizeof g_user_dir, "%s", ud);
        seed_user_dir();
        char r[PATH_MAX];
        if (realpath(g_user_dir, r)) snprintf(g_user_dir, sizeof g_user_dir, "%s", r);
    }
    if (bundled && !getenv("C2_LOG_STDERR")) {   // Finder launches have no terminal: keep a log next to the saves
        char lp[PATH_MAX];
        snprintf(lp, sizeof lp, "%s/c2native.log", g_user_dir);
        if (freopen(lp, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    }
    g_trace_imports = getenv("C2_TRACE") != NULL;

    g_mem = mmap(NULL, 0x100000000ull + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) { perror("mmap"); return 1; }
    mprotect(g_mem, 0x10000, PROT_NONE);   // null page guard
    struct sigaction sa = {0};
    sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);

    load_image(g_exe_path);
    rt_platform_init(argc, argv);

    static Cpu main_cpu;
    rt_new_thread_cpu(&main_cpu);
    g_main_cpu = &main_cpu;
    uint32_t pe = g_rd32(0x400000 + 0x3c);
    uint32_t entry = 0x400000 + g_rd32(0x400000 + pe + 24 + 16);
    LOG("game dir %s, entry %08x", g_game_dir, entry);
    if (g_user_dir[0]) LOG("user dir %s (OUTPUT, .ini files)", g_user_dir);
    Cpu *cpu = &main_cpu;
    cpu->esp -= 4; g_wr32(cpu->esp, 0xFFFFFFF0u);
    dispatch(cpu, entry);
    LOG("entry returned eax=%08x", cpu->eax);
    return (int)cpu->eax;
}
