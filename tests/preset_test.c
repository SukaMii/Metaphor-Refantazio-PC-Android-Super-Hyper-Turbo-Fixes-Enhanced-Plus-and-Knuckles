/* Parses every preset the way the DLL does and prints the settings that matter, so
 * a typo in a preset cannot silently fall back to a default. Usage: preset_test <dir> */
#include "../src/mcpu.h"
#include <stdio.h>

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "presets";
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.ini", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 1;
    printf("%-26s %3s %5s %5s %5s %5s %4s %-8s %5s %5s %4s %3s %5s %4s %3s\n", "preset", "on", "ySpin", "ySlp", "spinI",
           "CSspn", "aff", "mainCore", "pinDx", "prio", "jobs", "fps", "sampl", "fsem", "ab");
    do {
        char path[MAX_PATH];
        char full[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        GetFullPathNameA(path, MAX_PATH, full, NULL);
        Config c;
        config_defaults(&c);
        config_load(&c, full);
        if (!c.enabled && c.profile_when_disabled)
            config_neutral(&c);
        printf("%-26s %3d %5d %5d %5d %5d %4d %-8s %5s %5d %2d/%d %3d %5d %4d %3d\n", fd.cFileName, c.enabled,
               c.yield_spin_us, c.yield_sleep_us, c.spin_iters, c.cs_spin_count, c.affinity_mode,
               c.main_cores_auto ? "auto" : (c.main_cores ? "mask" : "OS"), c.fast_thread_names[0] ? "yes" : "no",
               c.main_priority, c.job_pool0, c.job_pool1, c.limiter_fps, c.sampler_hz, c.fast_sem, c.ab_enabled);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return 0;
}
