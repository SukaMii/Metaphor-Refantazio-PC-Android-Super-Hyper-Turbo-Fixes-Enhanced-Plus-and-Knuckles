/* Initialisation: config, IAT hooks on the game module, signature patches, policies. */
#include "mcpu.h"
#include <stdio.h>
#include <string.h>
#include <intrin.h>

HMODULE g_self;
int g_is_wine;
Stats g_stats;

static uint8_t* g_game_base;
static size_t g_game_size;
static Targets g_targets;

uint8_t* game_base(void) { return g_game_base; }
size_t game_size(void) { return g_game_size; }

const char* fmt_addr(void* p, char* buf, size_t cap)
{
    HMODULE m = NULL;
    if (p && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)p, &m) && m) {
        char path[MAX_PATH];
        GetModuleFileNameA(m, path, sizeof(path));
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        snprintf(buf, cap, "%s+0x%llx", base, (unsigned long long)((uint8_t*)p - (uint8_t*)m));
    } else {
        snprintf(buf, cap, "%p", p);
    }
    return buf;
}

static void sibling_path(const char* ext, char* out, size_t cap)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, sizeof(path));
    char* dot = strrchr(path, '.');
    char* slash = strrchr(path, '\\');
    if (dot && (!slash || dot > slash))
        *dot = 0;
    snprintf(out, cap, "%s%s", path, ext);
}

#define HOOK(name, hook, orig)                                                                 \
    do {                                                                                       \
        int n_ = iat_hook(game, name, (void*)(hook), (void**)&(orig));                         \
        LOGI("IAT hook %-40s %s", name, n_ ? "installed" : "not imported");                    \
    } while (0)

int mcpu_init(HMODULE self, const char* ini_override)
{
    static LONG once;
    if (InterlockedExchange(&once, 1))
        return 1;
    g_self = self;

    /* Never let the loader unload us: our hooks would point into freed memory. */
    HMODULE pinned;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)&mcpu_init,
                       &pinned);

    /* One active copy per process (e.g. MetaphorCPU.asi in the game folder plus the
     * launcher injecting MetaphorCPU.dll). The first one to load wins. */
    char mutex_name[64];
    snprintf(mutex_name, sizeof(mutex_name), "Local\\MetaphorCPU-%lu", GetCurrentProcessId());
    CreateMutexA(NULL, FALSE, mutex_name);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        OutputDebugStringA("MetaphorCPU: another copy is already active in this process; this one stays idle\n");
        return 1;
    }

    char ini[MAX_PATH], logp[MAX_PATH];
    char env[MAX_PATH];
    if (ini_override)
        snprintf(ini, sizeof(ini), "%s", ini_override);
    else if (GetEnvironmentVariableA("METAPHORCPU_INI", env, sizeof(env)))
        snprintf(ini, sizeof(ini), "%s", env);
    else
        sibling_path(".ini", ini, sizeof(ini));
    sibling_path(".log", logp, sizeof(logp));

    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    typedef const char* (*wine_get_version_t)(void);
    wine_get_version_t wgv = (wine_get_version_t)GetProcAddress(ntdll, "wine_get_version");
    g_is_wine = wgv != NULL; /* before the config: it picks the profile's defaults */

    config_defaults(&g_cfg);
    config_load(&g_cfg, ini);

    /* keep the previous run (e.g. the other half of an A/B test) as MetaphorCPU.prev.log */
    char prevp[MAX_PATH];
    sibling_path(".prev.log", prevp, sizeof(prevp));
    MoveFileExA(logp, prevp, MOVEFILE_REPLACE_EXISTING);
    log_open(logp);
    if (g_cfg.log_copy[0] && !log_copy_open(g_cfg.log_copy))
        LOGI("Log copy to %s: could not be opened", g_cfg.log_copy);

    HMODULE game = GetModuleHandleA(NULL);
    g_game_base = (uint8_t*)game;
    {
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_game_base + ((IMAGE_DOS_HEADER*)game)->e_lfanew);
        g_game_size = nt->OptionalHeader.SizeOfImage;
        char exe[MAX_PATH], brand[64];
        GetModuleFileNameA(game, exe, sizeof(exe));
        cpu_brand(brand, sizeof(brand));
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        LOGI("MetaphorCPU " MCPU_VERSION " loaded into %s (timestamp %lu)", exe,
             (unsigned long)nt->FileHeader.TimeDateStamp);
        LOGI("Config: %s%s", ini, GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES ? " (missing, defaults)" : "");
        LOGI("Host: %s | CPU: %s | logical processors: %lu | process mask 0x%llx", wgv ? wgv() : "Windows",
             brand, si.dwNumberOfProcessors, (unsigned long long)threads_process_mask());
        if (wgv)
            LOGI("Wine %s detected: timer-period calls are cheap there; Sleep(0)/spin fixes matter most", wgv());
        char pcfix[96] = "";
        int pc_side = g_cfg.profile == PROFILE_PC || g_cfg.profile == PROFILE_DECK;
        char why[128];
        snprintf(why, sizeof(why), "%s", !g_cfg.profile_auto ? "set in the INI"
                                         : g_cfg.profile == PROFILE_DECK ? g_cfg.profile_why
                                         : wgv ? "auto: Wine" : "auto: native Windows");
        if (g_cfg.profile_auto && g_cfg.profile == PROFILE_DECK) {
            memmove(why + 6, why, strlen(why) + 1 < sizeof(why) - 6 ? strlen(why) + 1 : sizeof(why) - 6);
            memcpy(why, "auto: ", 6);
        }
        if (pc_side && (g_cfg.fix_timer_churn || g_cfg.replace_spinlock || g_cfg.fast_sem))
            snprintf(pcfix, sizeof(pcfix), "; [PC] fixes on:%s%s%s", g_cfg.fix_timer_churn ? " timer" : "",
                     g_cfg.replace_spinlock ? " spinlock" : "", g_cfg.fast_sem ? " job-semaphore" : "");
        LOGI("Profile: %s (%s)%s%s",
             g_cfg.profile == PROFILE_PC ? "PC" : g_cfg.profile == PROFILE_DECK ? "Steam Deck" : "Android", why,
             g_cfg.profile == PROFILE_DECK
                 ? " - the picture changes as on Android; CPU-side fixes off unless set in [PC] (their INI keys are "
                   "ignored), shadow culling as configured"
             : pc_side ? " - CPU-side fixes off unless set in [PC] (the others target Wine; "
                         "their INI keys are ignored), shadow culling as configured"
                       : " - CPU-side fixes and shadow culling as configured",
             pcfix);
    }
    if (g_cfg.enabled &&
        (g_cfg.q_char_lod || g_cfg.q_far_anim > 1 || g_cfg.q_far_mip || g_cfg.q_shadow_dist || g_cfg.q_shadow_refresh > 1)) {
        char lod[96] = "the game's distance";
        if (g_cfg.q_char_lod && g_cfg.q_alod)
            snprintf(lod, sizeof(lod), "%d m from the hero (in to %d m when below %.0f FPS)", g_cfg.q_char_lod,
                     g_cfg.q_alod_min, g_cfg.q_alod_down);
        else if (g_cfg.q_char_lod)
            snprintf(lod, sizeof(lod), "%d m from the hero", g_cfg.q_char_lod);
        LOGI("Quality bubble: the game's simple NPC models beyond %s | NPCs beyond %d m from the camera animated "
             "every %d frame(s) | far textures mip +%d | shadows up to %d m%s, far shadows every %d frame(s)",
             lod, g_cfg.q_far_dist, g_cfg.q_far_anim, g_cfg.q_far_mip, g_cfg.q_shadow_dist ? g_cfg.q_shadow_dist : 200,
             g_cfg.q_shadow_dist ? "" : " (the game's)", g_cfg.q_shadow_refresh);
    }
    if (g_cfg.enabled) {
        static const char* const smaa[] = {"off", "Medium", "High", "Ultra (the game's)"};
        LOGI("Post-processing: SMAA %s | volumetric fog %s | glare %s | steadier image (SteadyImage) %s",
             smaa[g_cfg.q_smaa & 3], g_cfg.q_fog ? "on" : "off", g_cfg.q_glare ? "on" : "off", g_cfg.q_steady ? "on" : "off");
        char ol[40], rim[40], inner[40], od[40];
        snprintf(ol, sizeof(ol), g_cfg.q_outline_dist <= 0 ? "all drawn (the game's)" : g_cfg.q_outline_dist < 1 ? "none"
                                                                                           : "within %.0f m", g_cfg.q_outline_dist);
        snprintf(rim, sizeof(rim), g_cfg.q_rim_dist <= 0 ? "on (the game's)" : g_cfg.q_rim_dist < 1 ? "off" : "within %.0f m",
                 g_cfg.q_rim_dist);
        snprintf(inner, sizeof(inner), g_cfg.q_inner_dist <= 0 ? "on (the game's)" : g_cfg.q_inner_dist < 1 ? "off"
                                                                                       : "within %.0f m", g_cfg.q_inner_dist);
        snprintf(od, sizeof(od), g_cfg.q_ol_depth > 0 ? "%.2f%% of the distance" : "off", 100.0 * g_cfg.q_ol_depth);
        char limbs[80];
        snprintf(limbs, sizeof(limbs), g_cfg.q_mv_skin_dist > 0 ? "within %.0f m with more than %d characters around"
                                                                  : "every character's",
                 g_cfg.q_mv_skin_dist, g_cfg.q_mv_crowd);
        LOGI("Ink look: character outlines %s | rim light %s | inner outline lines %s | outline depth offset %s | "
             "painterly %s | character textures %+.2f mip | world textures %+.2f mip | temporal stabilisation %s "
             "(history: characters %.0f%%, the rest %.0f%% clamped to %d neighbours; F7 switches it) | characters' "
             "motion vectors %s (limbs: %s)",
             ol, rim, inner, od, g_cfg.q_painterly ? "on" : "off", 0.25 * g_cfg.q_char_tex_q, 0.25 * g_cfg.q_world_tex_q,
             g_cfg.q_stab > 0 || g_cfg.q_stab_world > 0 ? "on" : "off", 100.0 * g_cfg.q_stab, 100.0 * g_cfg.q_stab_world,
             g_cfg.q_stab_world_taps == 4 ? 4 : 8, g_cfg.q_mv_hooks ? (g_cfg.q_mv ? "on" : "off (cycle)") : "off", limbs);
    }
    if (!g_cfg.enabled) {
        if (!g_cfg.profile_when_disabled) {
            LOGI("Disabled in config; nothing patched.");
            return 1;
        }
        config_neutral(&g_cfg);
        LOGI("Mode: BASELINE - Enabled=0: game behaviour unchanged, pass-through hooks for measuring only");
    } else {
        LOGI("Mode: ACTIVE - fixes enabled");
    }

    /* ---- 1. preparation: nothing below calls into the game or its proxies, and
     *         nothing is hooked yet, so a failure here leaves the game untouched */
    timing_init();
    spinlock_init();
    threads_init(game);
    frames_init();
    LOGI("Settings: yield %s (spin %d us, sleep %d us) | spinlock %s | CS spin %d | affinity mode %d, main cores %s, "
         "fast-name pinning '%s' | main prio %d | job pools %d/%d | fast semaphore %d | precise usleep %d | "
         "limiter %d fps | telemetry %d | sampler %d Hz",
         g_cfg.adaptive_yield ? "adaptive" : "off", g_cfg.yield_spin_us, g_cfg.yield_sleep_us,
         g_cfg.replace_spinlock ? "replaced" : "original", g_cfg.cs_spin_count, g_cfg.affinity_mode,
         g_cfg.main_cores_auto ? "auto" : (g_cfg.main_cores ? "mask" : "OS"), g_cfg.fast_thread_names,
         g_cfg.main_priority, g_cfg.job_pool0, g_cfg.job_pool1, g_cfg.fast_sem, g_cfg.precise_usleep,
         g_cfg.limiter_fps, g_cfg.telemetry, g_cfg.sampler_hz);

    LOGI("Signature scan:");
    int found = sig_resolve(g_game_base, &g_targets, 1);
    LOGI("Signature scan: %d/8 targets found", found);

    if (g_cfg.detect_fast_cores && (g_cfg.main_cores_auto || g_cfg.fast_thread_names[0]))
        g_fast_mask = detect_fast_cores(threads_process_mask() &
                                        (g_cfg.allowed_cores ? g_cfg.allowed_cores : ~0ull));
    DWORD main_tid = threads_find_main();

    /* ---- 2. IAT hooks on the game module only */
    HOOK("Sleep", hk_Sleep, o_Sleep);
    HOOK("timeBeginPeriod", hk_timeBeginPeriod, o_timeBeginPeriod); /* pass-through when FixTimerChurn=0 */
    HOOK("timeEndPeriod", hk_timeEndPeriod, o_timeEndPeriod);
    if (g_cfg.frame_stats) {
        HOOK("D3D11CreateDeviceAndSwapChain", hk_D3D11CreateDeviceAndSwapChain, o_D3D11CreateDeviceAndSwapChain);
        HOOK("CreateDXGIFactory1", hk_CreateDXGIFactory1, o_CreateDXGIFactory1);
    }
    HOOK("CreateThread", hk_CreateThread, o_CreateThread);
    HOOK("_beginthreadex", hk_beginthreadex, o_beginthreadex);
    HOOK("SetThreadAffinityMask", hk_SetThreadAffinityMask, o_SetThreadAffinityMask);
    HOOK("SetThreadPriority", hk_SetThreadPriority, o_SetThreadPriority);
    HOOK("InitializeCriticalSectionAndSpinCount", hk_InitializeCriticalSectionAndSpinCount,
         o_InitializeCriticalSectionAndSpinCount);
    if (g_cfg.wait_profile) {
        waitprof_init();
        HOOK("WaitForSingleObject", hk_WaitForSingleObject, o_WaitForSingleObject);
        HOOK("WaitForSingleObjectEx", hk_WaitForSingleObjectEx, o_WaitForSingleObjectEx);
        HOOK("SleepEx", hk_SleepEx, o_SleepEx);
        HOOK("EnterCriticalSection", hk_EnterCriticalSection, o_EnterCriticalSection);
        if (!o_WaitForSingleObject || !o_WaitForSingleObjectEx || !o_SleepEx || !o_EnterCriticalSection) {
            /* never call through a NULL original: fall back to the system functions */
            if (!o_WaitForSingleObject) o_WaitForSingleObject = WaitForSingleObject;
            if (!o_WaitForSingleObjectEx) o_WaitForSingleObjectEx = WaitForSingleObjectEx;
            if (!o_SleepEx) o_SleepEx = SleepEx;
            if (!o_EnterCriticalSection) o_EnterCriticalSection = EnterCriticalSection;
        }
        LOGI("Wait profile: main-thread waits measured per frame (sleeps, event waits, critical sections, yields)");
    }

#ifdef MCPU_REPRO_V01
    /* regression repro only: v0.1 called into winmm here, during DllMain */
    if (o_timeBeginPeriod)
        o_timeBeginPeriod(1);
#endif

    /* ---- 3. engine patches located by signature */
    if (g_targets.spin_lock && g_cfg.replace_spinlock) {
        int ok = patch_abs_jmp(g_targets.spin_lock, (void*)mcpu_spin_lock, 14) &&
                 patch_abs_jmp(g_targets.spin_unlock, (void*)mcpu_spin_unlock, 16);
        LOGI("Patch spinlock (acquire/release) -> %s", ok ? "replaced" : "FAILED");
    }
    if (g_targets.usleep && (g_cfg.fix_timer_churn || g_cfg.precise_usleep)) {
        int ok = patch_abs_jmp(g_targets.usleep, (void*)mcpu_usleep, 14);
        LOGI("Patch engine usleep -> %s (%s)", ok ? "replaced" : "FAILED",
             g_cfg.precise_usleep ? "microsecond precise" : "ms rounding kept, no timer churn");
    }
    int cores = 0;
    for (uint64_t m = threads_process_mask(); m; m &= m - 1)
        cores++;
    if (g_cfg.fast_sem && cores < 4) {
        /* tests\sem_test.c: with 1-2 cores the engine's batching handshake is faster */
        LOGI("Patch job semaphore -> skipped: only %d cores available (needs 4+ to help)", cores);
    } else if (g_cfg.fast_sem) {
        if (g_targets.sem_create && g_targets.sem_wait && g_targets.sem_post && jobsync_init()) {
            int ok = patch_abs_jmp(g_targets.sem_wait, (void*)mcpu_sem_wait, 14) &&
                     patch_abs_jmp(g_targets.sem_post, (void*)mcpu_sem_post, 14);
            LOGI("Patch job semaphore (wake-one, non-blocking post, %d polls before sleeping) -> %s%s",
                 g_cfg.sem_spin, ok ? "replaced" : "FAILED", g_cfg.ab_enabled ? " [fixed for both A/B halves]" : "");
        } else {
            LOGI("Patch job semaphore -> skipped (signatures or WaitOnAddress missing)");
        }
    }
    if (g_targets.yield) {
        g_engine_main_tid = g_targets.main_tid;
        LOGI("Engine main-thread id variable at %p", (void*)g_targets.main_tid);
    }
    if (g_targets.thread_create) {
        static const uint8_t prologue[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                             0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
        if (memcmp(g_targets.thread_create, prologue, sizeof(prologue)) == 0) {
            o_gfd_thread_create = make_trampoline(g_targets.thread_create, sizeof(prologue));
            int ok = o_gfd_thread_create && patch_abs_jmp(g_targets.thread_create, (void*)hk_gfd_thread_create, 15);
            LOGI("Hook engine thread-create (names threads) -> %s", ok ? "installed" : "FAILED");
        }
    }
    if (!g_targets.thread_create && g_cfg.worker_priority != 99)
        LOGI("Job worker priority: not available in this game build (its thread creation is inside the protected code)");
    if (!g_targets.job_init && (g_cfg.job_pool0 || g_cfg.job_pool1))
        LOGI("Job pools: not available in this game build (its job setup is inside the protected code)");
    if (g_targets.job_init && (g_cfg.job_pool0 || g_cfg.job_pool1)) {
        if (g_cfg.job_pool0) {
            uint8_t v = (uint8_t)g_cfg.job_pool0;
            patch_bytes(g_targets.job_init + 7, &v, 1);
        }
        if (g_cfg.job_pool1) {
            uint8_t v = (uint8_t)g_cfg.job_pool1;
            patch_bytes(g_targets.job_init + 22, &v, 1);
        }
        LOGI("Job pools: worker counts pool0=%d pool1=%d (game default 5/5) [EXPERIMENTAL]",
             g_targets.job_init[7], g_targets.job_init[22]);
    }
    shadowcull_install();

    /* ---- 4. core placement */
    LOGI("Main thread: tid %lu (earliest created)%s", main_tid,
         main_tid == GetCurrentThreadId() ? ", current thread" : "");
    threads_apply_main_policy(main_tid);

    telemetry_start();
    sampler_start();
    ab_start();
    if (g_cfg.prof_enabled || g_cfg.fast_thread_names[0] || g_cfg.sampler_hz > 0)
        profiler_start();
    LOGI("Initialisation complete.");
    return 1;
}

/* Diagnostics for tests and external tools. */
__declspec(dllexport) void MetaphorCPU_GetStats(Stats* out)
{
    if (out)
        *out = g_stats;
}
