/*
 * TFU-FPS-Fix 1.0 - high frame rate fix for Star Wars: The Force Unleashed (PC, v1.2.94564)
 *
 * Loaded by the game as dinput8.dll (all DirectInput calls are forwarded to the system DLL).
 * SWTFU.exe is never modified on disk; patches are applied in memory after the Steam DRM
 * has decrypted the code, and every patch verifies the original bytes first.
 *
 * Features
 *   - Frame cap patch (vanilla 30).
 *   - Fixed 30 Hz simulation: App::GameUpdate (0x41BB90) runs on a real-time 30 Hz clock while
 *     the game renders at the cap. All timer outputs are overridden during a tick so Havok gets
 *     exactly the vanilla 30 FPS step pattern (2 x 1/60 s substeps). Game time stopped
 *     (hit-stop/pause) passes through every frame like vanilla.
 *   - Render interpolation at the Direct3D 9 level: per-draw snapshots of vertex shader
 *     constants on tick frames, blended on the frames in between (skeletal animation).
 *   - Borderless windowed (no topmost, covers the window's monitor).
 *
 * Freestanding C (no CRT). Built with clang --target=i686-pc-windows-msvc + lld-link.
 * Addresses: image base 0x400000; Steam and retail builds share the same code layout.
 * See docs/REVERSE_ENGINEERING.md for how every address and value below was found.
 *
 * Copyright (c) 2026 Eric (Goat). MIT License, see LICENSE.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            BOOL;
typedef void*          HANDLE;
typedef long long      i64;

#define WINAPI __stdcall
#define IMP __declspec(dllimport)

/* ---------------- game addresses (SWTFU.exe v1.2.94564, image base 0x400000) ---------------- */
#define ADDR_TIMING_SIG       0x415F92u  /* bytes verified before patching (frame timer)        */
#define ADDR_CAP_INSN         0x426E07u  /* mov dword [esi+0x68], 30   (App ctor: frame cap)     */
#define ADDR_CAP_IMM          0x426E0Au  /*   ...its immediate operand                          */
#define ADDR_CALL_GAMEUPDATE  0x426AC0u  /* call App::GameUpdate inside App::Tick (0x426930)    */
#define ADDR_GAMEUPDATE       0x41BB90u  /* void __thiscall App::GameUpdate(float dt)           */
#define ADDR_DT               0x10D4008u /* float: scaled frame time (game time)                */
#define ADDR_DT_UNSCALED      0x10D400Cu /* float: unscaled frame time                          */
#define ADDR_DT_VEC           0x1240290u /* float[4]: frame time broadcast (SSE)                */
#define ADDR_INVDT_VEC        0x1240280u /* float[4]: 1 / frame time (1.0 when time is stopped) */
#define ADDR_GAME_STATE       0x10D4000u /* int: game state (telemetry only)                    */
#define ADDR_TIMER_MODE       0x124046Cu /* int: frame timer mode (telemetry only)              */
#define ADDR_LIMITER          0x1240458u /* float: frame limiter value (telemetry only)         */
#define APP_FRAME_CAP         0x68       /* App+0x68 int:   frame cap in FPS                     */
#define APP_TIMER_DT          0x6C       /* App+0x6C float: timer's dt output for GameUpdate     */
#define APP_PHYSICS_DT        0x70       /* App+0x70 float: smoothed physics step (game-owned)   */

/* kernel32 */
IMP u32    WINAPI GetModuleFileNameA(HANDLE, char*, u32);
IMP HANDLE WINAPI GetModuleHandleA(const char*);
IMP HANDLE WINAPI LoadLibraryA(const char*);
IMP void*  WINAPI GetProcAddress(HANDLE, const char*);
IMP u32    WINAPI GetSystemDirectoryA(char*, u32);
IMP HANDLE WINAPI CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
IMP BOOL   WINAPI WriteFile(HANDLE, const void*, u32, u32*, void*);
IMP BOOL   WINAPI FlushFileBuffers(HANDLE);
IMP BOOL   WINAPI VirtualProtect(void*, u32, u32, u32*);
IMP u32    WINAPI GetPrivateProfileIntA(const char*, const char*, int, const char*);
IMP BOOL   WINAPI QueryPerformanceCounter(i64*);
IMP BOOL   WINAPI QueryPerformanceFrequency(i64*);
IMP u32    WINAPI GetTickCount(void);
IMP BOOL   WINAPI DisableThreadLibraryCalls(HANDLE);
IMP HANDLE WINAPI GetCurrentProcess(void);
IMP BOOL   WINAPI FlushInstructionCache(HANDLE, const void*, u32);
/* user32 */
IMP int __cdecl wsprintfA(char*, const char*, ...);

int _fltused = 0;
volatile int g_tick_this_frame;   /* set by hook_GameUpdate when a sim tick ran this frame */
volatile float g_alpha = 1.0f;    /* interpolation factor for rendering, set by hook_GameUpdate */
static int memcmp(const void* a, const void* b, u32 n) { const u8* x = a; const u8* y = b; for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1; return 0; }
void* memset(void* d, int c, u32 n) { u8* p = d; while (n--) *p++ = (u8)c; return d; }
void* memcpy(void* d, const void* s, u32 n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }

static u32 slen(const char* s) { u32 n = 0; while (s[n]) n++; return n; }
static void scpy(char* d, const char* s) { while ((*d++ = *s++)); }
static void scat(char* d, const char* s) { scpy(d + slen(d), s); }
static int  scmpi(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 1;
        if (!x) return 0;
    }
}

/* ---------------- logging ---------------- */
static HANDLE g_log = (HANDLE)-1;
static char   g_dir[520];

static volatile long g_writelock;
static void logs(const char* s) {
    if (g_log == (HANDLE)-1) return;
    while (__sync_lock_test_and_set(&g_writelock, 1)) { }
    u32 w; WriteFile(g_log, s, slen(s), &w, 0); WriteFile(g_log, "\r\n", 2, &w, 0);
    FlushFileBuffers(g_log);
    __sync_lock_release(&g_writelock);
}
#define LOGF(...) do { char _b[512]; wsprintfA(_b, __VA_ARGS__); logs(_b); } while (0)

/* ---------------- config ---------------- */
static char g_ini[600];
static int  cfg_cap;        /* frame cap in FPS; 0 = vanilla (30) */
static int  cfg_log;        /* 1 = write tfufix.log */
static int  cfg_telemetry;  /* 1 = periodic statistics in the log (troubleshooting) */

/* ---------------- real dinput8 ---------------- */
typedef long (WINAPI *DI8Create_t)(HANDLE, u32, const void*, void**, void*);
static DI8Create_t real_DI8Create;

static void load_real_dinput8(void) {
    char p[520];
    if (real_DI8Create) return;
    GetSystemDirectoryA(p, 260);
    scat(p, "\\dinput8.dll");
    HANDLE h = LoadLibraryA(p);
    if (h) real_DI8Create = (DI8Create_t)GetProcAddress(h, "DirectInput8Create");
    LOGF("real dinput8: %s -> %s", p, real_DI8Create ? "ok" : "FAILED");
}

long WINAPI DirectInput8Create(HANDLE inst, u32 ver, const void* riid, void** out, void* outer) {
    load_real_dinput8();
    if (!real_DI8Create) return 0x80004005; /* E_FAIL */
    return real_DI8Create(inst, ver, riid, out, outer);
}

/* ---------------- patching ---------------- */
static const u8 SIG_TIMING[] = { 0x0F, 0x2F, 0xC1, 0x0F, 0x86, 0xBB, 0x00, 0x00, 0x00,
                                 0xF3, 0x0F, 0x10, 0x1D, 0x08, 0x40, 0x0D, 0x01 }; /* @ADDR_TIMING_SIG */
static const u8 SIG_CAP[]    = { 0xC7, 0x46, 0x68 };                           /* @ADDR_CAP_INSN */

static int mem_eq(const void* a, const void* b, u32 n) {
    const u8* x = a; const u8* y = b; while (n--) if (*x++ != *y++) return 0; return 1;
}

static void write_code(u32 addr, const void* bytes, u32 n) {
    u32 old;
    VirtualProtect((void*)addr, n, 0x40 /*PAGE_EXECUTE_READWRITE*/, &old);
    memcpy((void*)addr, bytes, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
}

static volatile long g_state; /* 0 = waiting for decrypt, 1 = patching, 2 = done, 3 = gave up */

static int code_ready(void) {
    return mem_eq((void*)ADDR_TIMING_SIG, SIG_TIMING, sizeof SIG_TIMING) &&
           mem_eq((void*)ADDR_CAP_INSN, SIG_CAP, sizeof SIG_CAP);
}

/* ---------------- fixed 30 Hz simulation ----------------
 * App::Tick (0x426930) calls App::GameUpdate(float dt) (0x41BB90, __thiscall, ret 4) once per
 * frame at 0x426AC0; that call is redirected here. With FixedTimestep=1 GameUpdate only runs on
 * 30 Hz ticks of a real-time clock, with all timer outputs set to the tick's values for its
 * duration. Rendering and the rest of App::Tick still run every frame. */
typedef void (__thiscall *GameUpdate_t)(void*, float);
static int   cfg_fixed_update;
static u32   g_last_tick, g_start_tick;
static float g_acc;
static u32   g_upd_frames, g_upd_ticks;

static i64   g_qpf, g_qpc_last;
static float g_real_acc, g_scaled_acc;

/* Real (wall-clock) time since last call, in seconds. */
static float real_frame_time(void) {
    i64 now; QueryPerformanceCounter(&now);
    if (!g_qpf) { QueryPerformanceFrequency(&g_qpf); g_qpc_last = now; return 1.0f / 30.0f; }
    float r = (float)(now - g_qpc_last) / (float)g_qpf;
    g_qpc_last = now;
    return r;
}

static void __thiscall hook_GameUpdate(void* self, float dt) {
    const float step = 1.0f / 30.0f;
    float real = real_frame_time();
    g_upd_frames++;
    if (cfg_telemetry) {
        u32 t = GetTickCount();
        if (t - g_last_tick >= 1000) {
            float lim = *(volatile float*)ADDR_LIMITER;
            LOGF("[t=%us] frames/s=%u sim-ticks/s=%u game dt=%d us real dt=%d us | state=%d timermode=%d lim=%d us cap=%d physdt=%d us",
                 (t - g_start_tick) / 1000, g_upd_frames, g_upd_ticks,
                 (int)(dt * 1000000.0f), (int)(real * 1000000.0f),
                 *(volatile int*)ADDR_GAME_STATE, *(volatile int*)ADDR_TIMER_MODE,
                 (int)(lim * 1000000.0f), *(volatile int*)((u8*)self + APP_FRAME_CAP),
                 (int)(*(volatile float*)((u8*)self + APP_PHYSICS_DT) * 1000000.0f));
            g_last_tick = t; g_upd_frames = 0; g_upd_ticks = 0;
        }
    }
    if (!cfg_fixed_update) {               /* passthrough */
        g_alpha = 1.0f;
        g_upd_ticks++;
        g_tick_this_frame = 1;
        ((GameUpdate_t)ADDR_GAMEUPDATE)(self, dt);
        return;
    }
    /* Game time stopped (hit-stop/pause): vanilla runs GameUpdate(0) at the uncapped frame rate
       and the hit-stop length is counted in update CALLS. Pass every frame through like vanilla
       (nothing integrates at dt = 0); at 30 Hz, hit-stop would last several times longer. */
    if (dt <= 0.0f) {
        g_alpha = 1.0f;
        g_upd_ticks++;
        g_tick_this_frame = 1;
        ((GameUpdate_t)ADDR_GAMEUPDATE)(self, 0.0f);
        return;
    }
    /* Tick timing is driven by REAL time. The game's own dt (which can be scaled or 0 during
       hit-stop, slow-mo and pause) is accumulated separately and handed to the tick, so the
       game's time-scaling behaves exactly as at vanilla 30 FPS. (Driving ticks by game dt
       would deadlock: the code that ends hit-stop lives inside GameUpdate.) */
    if (real > 0.25f) real = 0.25f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    g_acc        += real;
    g_real_acc   += real;
    g_scaled_acc += dt;
    if (g_acc + 0.0005f < step) {           /* not time for a sim tick yet */
        g_alpha = g_acc / step;
        return;
    }
    g_acc -= step;
    if (g_acc > step) g_acc = step;         /* at most one tick per frame (no catch-up bursts) */
    if (g_acc < 0.0f) g_acc = 0.0f;
    g_alpha = g_acc / step;

    /* game time for this tick = step * (game time / real time) over the frames it covers */
    float tick_dt = step;
    if (g_real_acc > 0.0001f) tick_dt = step * (g_scaled_acc / g_real_acc);
    if (tick_dt > 0.25f) tick_dt = 0.25f;
    g_real_acc = 0.0f; g_scaled_acc = 0.0f;

    g_upd_ticks++;
    /* Override ALL timer outputs for the duration of the tick; different subsystems read
       different ones (scaled dt, unscaled dt, the SSE dt / 1/dt vectors, App+0x6C). */
    volatile float* gdt  = (volatile float*)ADDR_DT;
    volatile float* gdtu = (volatile float*)ADDR_DT_UNSCALED;
    volatile float* vdt  = (volatile float*)ADDR_DT_VEC;
    volatile float* vinv = (volatile float*)ADDR_INVDT_VEC;
    float saved = *gdt, saved_u = *gdtu, saved_v[4], saved_i[4];
    for (int k = 0; k < 4; k++) { saved_v[k] = vdt[k]; saved_i[k] = vinv[k]; }
    float inv = tick_dt > 0.0f ? 1.0f / tick_dt : 1.0f;   /* timer stores 1.0 when dt is 0 */
    /* [App+0x6C] = timer's dt output. GameUpdate (0x41BDA8) derives the physics step
       [App+0x70] from it: target = [App+0x6C] * timescale, approached with a per-update
       rate limit of [App+0x74]. A step > 1/45 s is split into two Havok substeps, so a 1/30 s
       tick gives the vanilla 2 x 1/60 s pattern. [App+0x70] is persistent smoothing state and
       must not be touched (resetting it each tick keeps it rate-limited below 1/30 s). */
    volatile float* adt = (volatile float*)((u8*)self + APP_TIMER_DT);
    float saved_adt = *adt;
    *adt = tick_dt;
    *gdtu = step;
    for (int k = 0; k < 4; k++) { vdt[k] = tick_dt; vinv[k] = inv; }
    *gdt = tick_dt;
    g_tick_this_frame = 1;
    ((GameUpdate_t)ADDR_GAMEUPDATE)(self, tick_dt);
    *gdt = saved; *gdtu = saved_u; *adt = saved_adt;
    for (int k = 0; k < 4; k++) { vdt[k] = saved_v[k]; vinv[k] = saved_i[k]; }
}

static void apply_patches(void) {
    u32 vanilla = *(u32*)ADDR_CAP_IMM;
    LOGF("game code verified (vanilla cap %u)", vanilla);
    if (cfg_cap > 0 && cfg_cap <= 1000) {
        u32 v = (u32)cfg_cap;
        write_code(ADDR_CAP_IMM, &v, 4);
        LOGF("frame cap: %u FPS", v);
    } else {
        logs("frame cap: vanilla");
    }
    {
        if (*(u8*)ADDR_CALL_GAMEUPDATE == 0xE8 &&
            ADDR_CALL_GAMEUPDATE + 5 + *(u32*)(ADDR_CALL_GAMEUPDATE + 1) == ADDR_GAMEUPDATE) {
            u8 c[5]; u32 rel = (u32)hook_GameUpdate - (ADDR_CALL_GAMEUPDATE + 5);
            c[0] = 0xE8; memcpy(c + 1, &rel, 4);
            write_code(ADDR_CALL_GAMEUPDATE, c, 5);
            logs(cfg_fixed_update ? "fixed 30 Hz simulation: on" : "fixed 30 Hz simulation: off (raw unlock, physics bugs expected above 30 FPS)");
        } else {
            logs("ERROR: GameUpdate call site not as expected; hook not applied");
        }
    }
}

/* Replace an import of the game exe (by name, or by resolved address) with a hook. Must run after
   the Steam DRM has rebuilt the import table, i.e. from the startup hook, not from DllMain. */
static int hook_iat(const char* dll, const char* fn, void* hook, void** orig) {
    u8* base = (u8*)GetModuleHandleA(0);
    u32 pe = *(u32*)(base + 0x3C);
    u32 imp_rva = *(u32*)(base + pe + 0x80);
    if (!imp_rva) return 0;
    void* target = GetProcAddress(GetModuleHandleA(dll), fn);
    for (u32* d = (u32*)(base + imp_rva); d[3]; d += 5) {
        if (scmpi((char*)(base + d[3]), dll)) continue;
        u32* iat = (u32*)(base + d[4]);
        u32* ilt = d[0] ? (u32*)(base + d[0]) : iat;
        for (u32 i = 0; iat[i]; i++) {
            int match = 0;
            if (!(ilt[i] & 0x80000000u) && ilt != iat) {
                const char* nm = (const char*)(base + ilt[i] + 2);
                match = !scmpi(nm, fn);
            }
            if (!match && (void*)iat[i] == target) match = 1;
            if (match) {
                u32 old;
                *orig = (void*)iat[i];
                VirtualProtect(&iat[i], 4, 0x04, &old);
                iat[i] = (u32)hook;
                VirtualProtect(&iat[i], 4, old, &old);
                return 1;
            }
        }
    }
    return 0;
}

/* Sanity check: main module must be a 0x400000-based image large enough to contain
   every address we touch (SWTFU.exe SizeOfImage = 0x36A0000 Steam / 0x3687000 retail). */
static int is_game_image(void) {
    u8* base = (u8*)GetModuleHandleA(0);
    if (base != (u8*)0x400000) return 0;
    u32 pe = *(u32*)(base + 0x3C);
    u32 size_of_image = *(u32*)(base + pe + 0x50);
    return size_of_image >= 0x1300000;
}

/* ---------------- Direct3D 9 hooks ----------------
 * Direct3DCreate9 (IAT, hooked after decryption) -> IDirect3D9::CreateDevice (vtable 16) ->
 * device vtable hooks used by the interpolation and borderless features. */
typedef long HRESULT;
typedef void* (WINAPI *D3DCreate9_t)(u32);
typedef HRESULT (WINAPI *CreateDevice_t)(void*, u32, u32, void*, u32, void*, void**);
typedef HRESULT (WINAPI *Present_t)(void*, const void*, const void*, void*, const void*);
typedef HRESULT (WINAPI *DIP_t)(void*, u32, int, u32, u32, u32, u32);
typedef HRESULT (WINAPI *DP_t)(void*, u32, u32, u32);
typedef HRESULT (WINAPI *VSCF_t)(void*, u32, const float*, u32);

static D3DCreate9_t   real_D3DCreate9;
static CreateDevice_t real_CreateDevice;
static Present_t      real_Present;
static DIP_t          real_DIP;
static DP_t           real_DP;
static VSCF_t         real_VSCF;

static void patch_vtbl(void** vt, int idx, void* hook, void** orig) {
    u32 old;
    *orig = vt[idx];
    VirtualProtect(&vt[idx], 4, 0x04, &old);
    vt[idx] = hook;
    VirtualProtect(&vt[idx], 4, old, &old);
}

static void interp_before_draw(void* d, u32 a, u32 b, u32 c);
static void interp_restore(void* d);
static float g_shadow[256][4], g_dev[256][4];
static u16   g_bones;
static HRESULT WINAPI hk_DIP(void* d, u32 type, int base, u32 minv, u32 nv, u32 start, u32 prims) {
    interp_before_draw(d, (u32)base, start, prims);
    return real_DIP(d, type, base, minv, nv, start, prims);
}
static HRESULT WINAPI hk_DP(void* d, u32 type, u32 start, u32 prims) {
    interp_before_draw(d, 0xFFFFFFFFu, start, prims);
    return real_DP(d, type, start, prims);
}
static HRESULT WINAPI hk_VSCF(void* d, u32 reg, const float* data, u32 count) {
    if (data && reg < 256) {
        u32 n = count; if (reg + n > 256) n = 256 - reg;
        memcpy(g_shadow[reg], data, n * 16);
        memcpy(g_dev[reg], data, n * 16);
        if (reg == 0) g_bones = (u16)((count >= 12 && count <= 189 && count % 3 == 0) ? count : 0);
    }
    return real_VSCF(d, reg, data, count);
}
/* ---------------- render interpolation ----------------
 * The renderer redraws the last simulated state every frame. We identify each draw call by
 * (vertex shader, stream-0 VB, IB, texture 0, base vertex, start index, prim count) plus its
 * occurrence number within the frame. On frames that follow a sim tick we snapshot vertex
 * shader constants c0..c203 per draw into the "cur" table (previous "cur" becomes "prev").
 * On every frame, interpolated registers are uploaded as lerp(prev, cur, alpha) where
 * alpha = time since last tick / tick length. Interpolated registers: c192..c203 (per-draw
 * matrices) and the bone palette (c0.. when uploaded with >= 12 registers).
 * Safety: unmatched draws use the game's values; large jumps snap; registers the game itself
 * changes on non-tick frames are marked "live" and never interpolated.
 * In practice c192..c203 contain the camera (which the game updates every frame) and become
 * live within the first frames, so the effective result is smooth skeletal animation.
 * (Interpolating them per draw instead made everything jitter with camera motion.) */
typedef HRESULT (WINAPI *SetTex_t)(void*, u32, void*);
typedef HRESULT (WINAPI *SetVS_t)(void*, void*);
typedef HRESULT (WINAPI *SetSS_t)(void*, u32, void*, u32, u32);
typedef HRESULT (WINAPI *SetIdx_t)(void*, void*);
typedef HRESULT (WINAPI *DPUP_t)(void*, u32, u32, const void*, u32);
typedef HRESULT (WINAPI *DIPUP_t)(void*, u32, u32, u32, u32, const void*, u32, const void*, u32);
static SetTex_t real_SetTex; static SetVS_t real_SetVS; static SetSS_t real_SetSS;
static SetIdx_t real_SetIdx; static DPUP_t real_DPUP; static DIPUP_t real_DIPUP;

#define NREG 204
#define MAXDRAWS 1024
#define HSLOTS 4096
typedef struct { u32 key; float c[NREG][4]; u16 bones; } snap_t;
typedef struct { snap_t d[MAXDRAWS]; u32 n; u16 slot[HSLOTS]; } table_t;   /* slot: index+1 */
static table_t g_tab[2];
static int     g_cur;                          /* g_tab[g_cur] = cur, other = prev */
static void   *s_vs, *s_vb, *s_ib, *s_tex0;
static u32     s_vb_off;
static u32     g_occ_key[HSLOTS]; static u16 g_occ_cnt[HSLOTS]; static u32 g_occ_frame[HSLOTS];
static u32     g_frame_no;
static u8      g_live[NREG];
static int     cfg_interp;
static float   cfg_snap = 8.0f;
static int     g_frame_is_tick;                /* this frame's state was just produced by a tick */
static u32     st_draws, st_matched, st_snapped, st_new, st_full;
static int     interp_new_frame_pending;
static volatile int g_pending_tick;
static u32     st_rotations, g_interp_log_last;

static u32 mix(u32 h, u32 v) { h ^= v; h *= 0x01000193u; return h; }

static snap_t* tab_find(table_t* t, u32 key) {
    u32 i = (key * 2654435761u) & (HSLOTS - 1);
    for (u32 k = 0; k < HSLOTS; k++, i = (i + 1) & (HSLOTS - 1)) {
        u16 s = t->slot[i];
        if (!s) return 0;
        if (t->d[s - 1].key == key) return &t->d[s - 1];
    }
    return 0;
}
static snap_t* tab_add(table_t* t, u32 key) {
    if (t->n >= MAXDRAWS) { st_full++; return 0; }
    u32 i = (key * 2654435761u) & (HSLOTS - 1);
    for (u32 k = 0; k < HSLOTS; k++, i = (i + 1) & (HSLOTS - 1)) {
        if (!t->slot[i]) { t->slot[i] = (u16)(t->n + 1); snap_t* e = &t->d[t->n++]; e->key = key; return e; }
        if (t->d[t->slot[i] - 1].key == key) return &t->d[t->slot[i] - 1];
    }
    return 0;
}
static void tab_clear(table_t* t) { t->n = 0; memset(t->slot, 0, sizeof t->slot); }

/* called at the first draw after each Present */
static u32 g_frames_since_tick;
static void interp_new_frame(void) {
    g_frame_no++;
    st_rotations += g_pending_tick ? 1 : 0;
    g_frames_since_tick = g_pending_tick ? 0 : g_frames_since_tick + 1;
    if (g_pending_tick) {                /* new sim state will be rendered: rotate tables */
        g_cur ^= 1;
        tab_clear(&g_tab[g_cur]);
        g_frame_is_tick = 1;
    } else g_frame_is_tick = 0;
}

static u32 draw_key(u32 a, u32 b, u32 c) {
    u32 h = 0x811C9DC5u;
    h = mix(h, (u32)s_vs); h = mix(h, (u32)s_vb); h = mix(h, s_vb_off); h = mix(h, (u32)s_ib);
    h = mix(h, (u32)s_tex0); h = mix(h, a); h = mix(h, b); h = mix(h, c);
    u32 i = (h * 2654435761u) & (HSLOTS - 1);
    for (u32 k = 0; k < 64; k++, i = (i + 1) & (HSLOTS - 1)) {
        if (g_occ_frame[i] != g_frame_no) { g_occ_frame[i] = g_frame_no; g_occ_key[i] = h; g_occ_cnt[i] = 0; break; }
        if (g_occ_key[i] == h) break;
    }
    u32 occ = g_occ_cnt[i]++;
    return mix(h, occ * 0x9E3779B1u);
}

static void upload(void* d, int r, const float* v) {
    if (g_dev[r][0] == v[0] && g_dev[r][1] == v[1] && g_dev[r][2] == v[2] && g_dev[r][3] == v[3]) return;
    memcpy(g_dev[r], v, 16);
    real_VSCF(d, (u32)r, v, 1);
}

static void interp_before_draw(void* d, u32 a, u32 b, u32 c) {
    if (!cfg_interp) return;
    if (interp_new_frame_pending) { interp_new_frame_pending = 0; interp_new_frame(); }
    st_draws++;
    u32 key = draw_key(a, b, c);
    table_t* cur = &g_tab[g_cur];
    table_t* prv = &g_tab[g_cur ^ 1];
    snap_t* ec;
    if (g_frame_is_tick) {
        ec = tab_add(cur, key);
        if (ec) { memcpy(ec->c, g_shadow, sizeof ec->c); ec->bones = g_bones; }
    } else {
        ec = tab_find(cur, key);
        if (ec) {   /* learn registers the game changes on its own between ticks */
            for (int r = 192; r < NREG; r++)
                if (!g_live[r] && memcmp(ec->c[r], g_shadow[r], 16)) { g_live[r] = 1; LOGF("interp: c%d changes between ticks -> live (not interpolated)", r); }
        }
    }
    snap_t* ep = tab_find(prv, key);
    float al = g_alpha;
    if (g_frames_since_tick > 8) al = 1.0f;          /* menus/loading: no interpolation */
    if (!ec || !ep || al >= 0.999f) { if (!ep) st_new++;
        /* make sure the device has the game's values for the interpolated ranges */
        for (int r = 192; r < NREG; r++) upload(d, r, g_shadow[r]);
        for (int r = 0; r < g_bones && r < 192; r++) upload(d, r, g_shadow[r]);
        return;
    }
    st_matched++;
    /* snap test on the main per-draw matrix: compare translation-ish column (row 3 / col 3) */
    float md = 0.0f;
    for (int r = 192; r < 196; r++) { float x = ec->c[r][3] - ep->c[r][3]; if (x < 0) x = -x; if (x > md) md = x; }
    int snap = md > cfg_snap;
    if (snap) st_snapped++;
    float v[4];
    for (int r = 192; r < NREG; r++) {
        if (snap || g_live[r]) { upload(d, r, g_shadow[r]); continue; }
        for (int k = 0; k < 4; k++) v[k] = ep->c[r][k] + (ec->c[r][k] - ep->c[r][k]) * al;
        upload(d, r, v);
    }
    int nb = g_bones < 192 ? g_bones : 191;
    int bones_ok = !snap && ec->bones == g_bones && ep->bones == g_bones && g_bones >= 12;
    for (int r = 0; r < nb; r++) {
        if (!bones_ok) { upload(d, r, g_shadow[r]); continue; }
        for (int k = 0; k < 4; k++) v[k] = ep->c[r][k] + (ec->c[r][k] - ep->c[r][k]) * al;
        upload(d, r, v);
    }
}
static void interp_restore(void* d) {   /* before draws we don't interpolate */
    if (!cfg_interp) return;
    if (interp_new_frame_pending) { interp_new_frame_pending = 0; interp_new_frame(); }
    for (int r = 192; r < NREG; r++) upload(d, r, g_shadow[r]);
    for (int r = 0; r < g_bones && r < 192; r++) upload(d, r, g_shadow[r]);
}

static HRESULT WINAPI hk_SetTex(void* d, u32 st, void* t) { if (st == 0) s_tex0 = t; return real_SetTex(d, st, t); }
static HRESULT WINAPI hk_SetVS(void* d, void* vs) { s_vs = vs; return real_SetVS(d, vs); }
static HRESULT WINAPI hk_SetSS(void* d, u32 n, void* vb, u32 off, u32 stride) { if (n == 0) { s_vb = vb; s_vb_off = off; } return real_SetSS(d, n, vb, off, stride); }
static HRESULT WINAPI hk_SetIdx(void* d, void* ib) { s_ib = ib; return real_SetIdx(d, ib); }
static HRESULT WINAPI hk_DPUP(void* d, u32 t, u32 n, const void* v, u32 s) { interp_restore(d); return real_DPUP(d, t, n, v, s); }
static HRESULT WINAPI hk_DIPUP(void* d, u32 t, u32 mi, u32 nv, u32 pc, const void* ix, u32 f, const void* v, u32 s) { interp_restore(d); return real_DIPUP(d, t, mi, nv, pc, ix, f, v, s); }

static void interp_stats(void) {
    if (!cfg_interp) return;
    LOGF("   INTERP: draws=%u matched=%u snapped=%u unmatched=%u tablefull=%u rotations=%u cur=%u prev=%u alpha(now)=%d%%",
         st_draws, st_matched, st_snapped, st_new, st_full, st_rotations,
         g_tab[g_cur].n, g_tab[g_cur ^ 1].n, (int)(g_alpha * 100.0f));
    st_draws = st_matched = st_snapped = st_new = st_full = st_rotations = 0;
}

static HRESULT WINAPI hk_Present(void* d, const void* a, const void* b, void* w, const void* c) {
    HRESULT hr = real_Present(d, a, b, w, c);
    /* Pipelined renderer: order is GameUpdate -> Present -> draws for the new state.
       Latch "a tick happened since the last Present" for the draws that follow. */
    g_pending_tick = g_tick_this_frame;
    interp_new_frame_pending = 1;
    g_tick_this_frame = 0;
    if (cfg_telemetry) {
        u32 t = GetTickCount();
        if (t - g_interp_log_last >= 5000) { g_interp_log_last = t; interp_stats(); }
    }
    return hr;
}

/* ---------------- borderless windowed ----------------
 * At CreateDevice/Reset: Windowed=TRUE, refresh 0 (Windows decides), resolution unchanged
 * (D3D scales it to the window if it differs from the monitor). The game window is restyled
 * to a borderless popup covering the monitor it is on. Fallbacks if the driver refuses:
 * back buffer format X8R8G8B8, then the game's original fullscreen parameters. */
IMP int  WINAPI GetWindowLongA(void*, int);
IMP int  WINAPI SetWindowLongA(void*, int, int);
IMP BOOL WINAPI SetWindowPos(void*, void*, int, int, int, int, u32);
IMP void* WINAPI MonitorFromWindow(void*, u32);
IMP BOOL WINAPI GetMonitorInfoA(void*, void*);
typedef HRESULT (WINAPI *Reset_t)(void*, void*);
static Reset_t real_Reset;
static int     cfg_borderless;
static void*   g_game_hwnd;

static void borderless_params(u32* p) {
    p[8]  = 1;      /* Windowed */
    p[12] = 0;      /* FullScreen_RefreshRateInHz must be 0 when windowed */
}
static void borderless_window(void* hwnd) {
    if (!hwnd) return;
    int st = GetWindowLongA(hwnd, -16), ex = GetWindowLongA(hwnd, -20);
    struct { u32 cb; int mon[4]; int work[4]; u32 flags; } mi;
    mi.cb = sizeof mi;
    void* mon = MonitorFromWindow(hwnd, 2 /*MONITOR_DEFAULTTONEAREST*/);
    if (!mon || !GetMonitorInfoA(mon, &mi)) { logs("borderless: GetMonitorInfo failed"); return; }
    int nst = (int)((st & ~(0x00C00000 | 0x00040000 | 0x00020000 | 0x00010000 | 0x00080000)) | 0x80000000 | 0x10000000);
    /* remove WS_CAPTION, WS_THICKFRAME, WS_MINIMIZEBOX, WS_MAXIMIZEBOX, WS_SYSMENU; add WS_POPUP, WS_VISIBLE */
    int nex = ex & ~(0x00000001 | 0x00000200 | 0x00020000 | 0x00000100 | 0x00000008);
    /* remove WS_EX_DLGMODALFRAME, WS_EX_CLIENTEDGE, WS_EX_STATICEDGE, WS_EX_WINDOWEDGE, WS_EX_TOPMOST */
    SetWindowLongA(hwnd, -16, nst);
    SetWindowLongA(hwnd, -20, nex);
    /* HWND_NOTOPMOST: clearing WS_EX_TOPMOST via SetWindowLong does not change the z-order
       state; only SetWindowPos does (otherwise no other window can be brought over the game) */
    SetWindowPos(hwnd, (void*)-2 /*HWND_NOTOPMOST*/, mi.mon[0], mi.mon[1], mi.mon[2] - mi.mon[0], mi.mon[3] - mi.mon[1],
                 0x0020 /*SWP_FRAMECHANGED*/ | 0x0040 /*SWP_SHOWWINDOW*/);
    LOGF("borderless: window %08X style %08X->%08X ex %08X->%08X rect %d,%d %dx%d",
         (u32)hwnd, st, nst, ex, nex, mi.mon[0], mi.mon[1], mi.mon[2] - mi.mon[0], mi.mon[3] - mi.mon[1]);
}

/* The game creates its window with WS_EX_TOPMOST (0x8); strip it at creation when borderless. */
typedef void* (WINAPI *CWEA_t)(u32, const char*, const char*, u32, int, int, int, int, void*, void*, void*, void*);
static CWEA_t real_CWEA;
static void* WINAPI hk_CWEA(u32 ex, const char* cls, const char* name, u32 st, int x, int y, int w, int h,
                            void* parent, void* menu, void* inst, void* param) {
    u32 nex = cfg_borderless ? (ex & ~0x00000008u) : ex;
    void* hw = real_CWEA(nex, cls, name, st, x, y, w, h, parent, menu, inst, param);
    LOGF("CreateWindowExA: ex %08X->%08X style %08X %dx%d -> %08X", ex, nex, st, w, h, (u32)hw);
    return hw;
}

static HRESULT WINAPI hk_Reset(void* d, void* pp) {
    u32* p = (u32*)pp;
    LOGF("Reset: %ux%u windowed=%u refresh=%u", p[0], p[1], p[8], p[12]);
    if (!cfg_borderless) return real_Reset(d, pp);
    u32 orig[14]; memcpy(orig, p, sizeof orig);
    borderless_params(p);
    HRESULT hr = real_Reset(d, pp);
    if (hr < 0) { p[2] = 22; hr = real_Reset(d, pp); LOGF("  Reset borderless retry X8R8G8B8 -> %08X", (u32)hr); }
    if (hr < 0) { memcpy(p, orig, sizeof orig); hr = real_Reset(d, pp); LOGF("  Reset fallback to original -> %08X", (u32)hr); }
    else borderless_window(g_game_hwnd);
    LOGF("  Reset -> %08X", (u32)hr);
    return hr;
}

static HRESULT WINAPI hk_CreateDevice(void* d3d, u32 adapter, u32 type, void* hwnd, u32 flags, void* pp, void** out) {
    u32* p = (u32*)pp;
    LOGF("CreateDevice: adapter=%u type=%u flags=%08X hwnd=%08X", adapter, type, flags, (u32)hwnd);
    LOGF("  present params: %ux%u fmt=%u count=%u msaa=%u swap=%u window=%08X windowed=%u autodepth=%u depthfmt=%u flags=%08X refresh=%u interval=%08X",
         p[0], p[1], p[2], p[3], p[4], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13]);
    g_game_hwnd = p[7] ? (void*)p[7] : hwnd;
    HRESULT hr;
    if (cfg_borderless) {
        u32 orig[14]; memcpy(orig, p, sizeof orig);
        borderless_params(p);
        hr = real_CreateDevice(d3d, adapter, type, hwnd, flags, pp, out);
        LOGF("  CreateDevice borderless -> %08X", (u32)hr);
        if (hr < 0) { p[2] = 22; hr = real_CreateDevice(d3d, adapter, type, hwnd, flags, pp, out); LOGF("  retry X8R8G8B8 -> %08X", (u32)hr); }
        if (hr < 0) { memcpy(p, orig, sizeof orig); hr = real_CreateDevice(d3d, adapter, type, hwnd, flags, pp, out); LOGF("  fallback to original fullscreen -> %08X", (u32)hr); }
        else borderless_window(g_game_hwnd);
    } else {
        hr = real_CreateDevice(d3d, adapter, type, hwnd, flags, pp, out);
    }
    LOGF("  CreateDevice -> %08X", (u32)hr);
    if (hr >= 0 && out && *out && !real_Present) {
        void** vt = *(void***)*out;
        patch_vtbl(vt, 17, (void*)hk_Present,        (void**)&real_Present);
        patch_vtbl(vt, 16, (void*)hk_Reset,          (void**)&real_Reset);
        patch_vtbl(vt, 81, (void*)hk_DP,             (void**)&real_DP);
        patch_vtbl(vt, 82, (void*)hk_DIP,            (void**)&real_DIP);
        patch_vtbl(vt, 94, (void*)hk_VSCF,           (void**)&real_VSCF);
        patch_vtbl(vt, 65, (void*)hk_SetTex,         (void**)&real_SetTex);
        patch_vtbl(vt, 92, (void*)hk_SetVS,          (void**)&real_SetVS);
        patch_vtbl(vt, 100,(void*)hk_SetSS,          (void**)&real_SetSS);
        patch_vtbl(vt, 104,(void*)hk_SetIdx,         (void**)&real_SetIdx);
        patch_vtbl(vt, 83, (void*)hk_DPUP,           (void**)&real_DPUP);
        patch_vtbl(vt, 84, (void*)hk_DIPUP,          (void**)&real_DIPUP);
        logs("  device hooks installed");
    }
    return hr;
}

static void* WINAPI hk_D3DCreate9(u32 sdk) {
    void* d3d = real_D3DCreate9(sdk);
    LOGF("Direct3DCreate9(%u) -> %08X", sdk, (u32)d3d);
    if (d3d && !real_CreateDevice)
        patch_vtbl(*(void***)d3d, 16, (void*)hk_CreateDevice, (void**)&real_CreateDevice);
    return d3d;
}

static void install_d3d_hooks(void) {
    if (cfg_borderless) {
        if (hook_iat("USER32.dll", "CreateWindowExA", (void*)hk_CWEA, (void**)&real_CWEA)) logs("borderless: CreateWindowExA hooked");
        else logs("borderless: CreateWindowExA hook FAILED");
    }
    if (hook_iat("d3d9.dll", "Direct3DCreate9", (void*)hk_D3DCreate9, (void**)&real_D3DCreate9))
        logs("Direct3D hooks ready");
    else logs("ERROR: Direct3DCreate9 hook failed (interpolation/borderless unavailable)");
}

/* ---------------- startup hook ----------------
 * SteamStub rebuilds the exe's IAT after our DllMain, so IAT hooks installed there are lost.
 * Instead: one-shot inline jmp at the start of kernel32!GetStartupInfoA itself. The CRT
 * entry (0x8ECEB8) calls it before _initterm and WinMain (which constructs the app object
 * containing the frame cap, 0x401210 -> 0x426CF0). At that point the code is decrypted
 * and the final IAT is in place. If something calls it earlier (e.g. the stub), we re-arm. */
typedef void (WINAPI *GSI_t)(void*);
static GSI_t g_gsi;
static u8    g_gsi_saved[5];
static u32   g_gsi_calls;

static void arm_gsi(void* hook) {
    u8 j[5]; u32 rel = (u32)hook - ((u32)g_gsi + 5);
    j[0] = 0xE9; memcpy(j + 1, &rel, 4);
    write_code((u32)g_gsi, j, 5);
}

static void WINAPI hook_GSI(void* si) {
    write_code((u32)g_gsi, g_gsi_saved, 5);   /* restore original */
    g_gsi_calls++;
    g_gsi(si);
    if (g_state == 0 && code_ready()) {
        LOGF("startup hook: code decrypted (GetStartupInfoA call #%u)", g_gsi_calls);
        g_state = 1; apply_patches(); g_state = 2;
        if (cfg_interp || cfg_borderless) install_d3d_hooks();
    } else if (g_state == 0 && g_gsi_calls < 50) {
        arm_gsi((void*)hook_GSI);              /* too early: re-arm */
    } else if (g_state == 0) {
        g_state = 3;
        logs("ERROR: code never matched expected bytes - different game version? No patches applied.");
    }
}

/* ---------------- entry ---------------- */
BOOL WINAPI DllMain(HANDLE inst, u32 reason, void* reserved) {
    (void)reserved;
    if (reason != 1) return 1;
    DisableThreadLibraryCalls(inst);

    GetModuleFileNameA(0, g_dir, 260);
    const char* exe = g_dir;
    for (int i = (int)slen(g_dir) - 1; i >= 0; i--) if (g_dir[i] == '\\') { exe = g_dir + i + 1; break; }

    /* The DLL is also loaded by "SWTFU Launcher.exe" (and possibly other tools) from the
       same folder. Only act inside the game itself; everywhere else be a pure proxy. */
    if (scmpi(exe, "SWTFU.exe") || !is_game_image()) return 1;

    for (int i = (int)slen(g_dir) - 1; i >= 0; i--) if (g_dir[i] == '\\') { g_dir[i + 1] = 0; break; }

    scpy(g_ini, g_dir); scat(g_ini, "tfufix.ini");
    cfg_log       = (int)GetPrivateProfileIntA("Debug", "Log", 1, g_ini);
    cfg_telemetry = (int)GetPrivateProfileIntA("Debug", "Telemetry", 0, g_ini);
    cfg_cap       = (int)GetPrivateProfileIntA("Framerate", "FPSCap", 60, g_ini);
    /* [Physics] FixedTimestep; falls back to the pre-release key [Experimental] FixedUpdate */
    cfg_fixed_update = (int)GetPrivateProfileIntA("Physics", "FixedTimestep",
                       (int)GetPrivateProfileIntA("Experimental", "FixedUpdate", 1, g_ini), g_ini);
    cfg_interp       = (int)GetPrivateProfileIntA("Interpolation", "Enabled", 1, g_ini);
    cfg_snap         = (float)GetPrivateProfileIntA("Interpolation", "SnapDistance", 8, g_ini);
    cfg_borderless   = (int)GetPrivateProfileIntA("Display", "Borderless", 1, g_ini);
    if (cfg_interp && !cfg_fixed_update) cfg_interp = 0;   /* interpolation needs the fixed timestep */
    if (cfg_telemetry) cfg_log = 1;

    if (cfg_log) {
        char lp[600]; scpy(lp, g_dir); scat(lp, "tfufix.log");
        g_log = CreateFileA(lp, 0x40000000 /*GENERIC_WRITE*/, 1 /*SHARE_READ*/, 0, 2 /*CREATE_ALWAYS*/, 0x80, 0);
    }

    g_start_tick = g_last_tick = GetTickCount();
    logs("TFU-FPS-Fix 1.0 loaded");
    LOGF("settings: FPSCap=%d FixedTimestep=%d Interpolation=%d Borderless=%d Telemetry=%d",
         cfg_cap, cfg_fixed_update, cfg_interp, cfg_borderless, cfg_telemetry);

    if (code_ready()) {           /* e.g. an unpacked exe: patch immediately */
        logs("code already decrypted at load");
        g_state = 1; apply_patches(); g_state = 2;
        if (cfg_interp || cfg_borderless) install_d3d_hooks();   /* IAT is final here too */
    }
    if (g_state == 0) {
        g_gsi = (GSI_t)GetProcAddress(GetModuleHandleA("KERNEL32.dll"), "GetStartupInfoA");
        if (g_gsi) {
            memcpy(g_gsi_saved, (void*)g_gsi, 5);
            arm_gsi((void*)hook_GSI);
            logs("startup hook armed (GetStartupInfoA)");
        } else {
            logs("ERROR: GetStartupInfoA not found");
        }
    }
    return 1;
}
