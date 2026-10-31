/* Unit test for the /proc parsers used by the Android telemetry (which itself only
 * runs under Wine). Includes telemetry.c to reach its static helpers. */
#include "../src/telemetry.c"

static Cand cand(int pid, const char* comm, int threads, long long ticks, int name_match)
{
    Cand c;
    memset(&c, 0, sizeof(c));
    c.pid = pid, c.threads = threads, c.ticks = ticks, c.name_match = name_match;
    snprintf(c.comm, sizeof(c.comm), "%s", comm);
    return c;
}

int main(void)
{
    int fail = 0;
    /* comm with spaces and parentheses; utime(14)=523 stime(15)=210 num_threads(20)=61 processor(39)=6 */
    char stat[512];
    int n = snprintf(stat, sizeof(stat), "4321 (a (b) c) R");
    for (int field = 4; field <= 45; field++) {
        long v = field == 14 ? 523 : field == 15 ? 210 : field == 20 ? 61 : field == 39 ? 6 : field;
        n += snprintf(stat + n, sizeof(stat) - n, " %ld", v);
    }
    char comm[20];
    long long ticks = 0;
    int cpu = -1, threads = 0;
    int ok = parse_stat(stat, comm, sizeof(comm), &ticks, &cpu, &threads);
    printf("  stat: ok=%d comm='%s' ticks=%lld cpu=%d threads=%d\n", ok, comm, ticks, cpu, threads);
    if (!ok || strcmp(comm, "a (b) c") || ticks != 733 || cpu != 6 || threads != 61)
        fail++;

    const char* status = "Name:\twine_threadpool\nState:\tS (sleeping)\nCpus_allowed:\tc0\nCpus_allowed_list:\t6-7\n"
                         "voluntary_ctxt_switches:\t100\nnonvoluntary_ctxt_switches:\t23\n";
    long long ctx = parse_ctx(status);
    uint64_t al = parse_allowed(status);
    printf("  status: context switches=%lld, allowed mask 0x%llx\n", ctx, (unsigned long long)al);
    if (ctx != 123 || al != 0xC0)
        fail++;

    /* CPU lists both ways */
    static const struct {
        const char* text;
        uint64_t mask;
    } lists[] = {{"0-7\n", 0xFF}, {"6-7", 0xC0}, {"0-5,7", 0xBF}, {"0,2,4-5", 0x35}, {"3", 0x8}, {"", 0}};
    for (unsigned i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        uint64_t m = parse_cpu_list(lists[i].text);
        char back[64];
        format_cpu_list(m, back, sizeof(back));
        char want[16];
        snprintf(want, sizeof(want), "%s", lists[i].text);
        want[strcspn(want, "\n")] = 0;
        if (m != lists[i].mask || strcmp(back, want)) {
            printf("  cpu list '%s': mask 0x%llx, back '%s' FAIL\n", want, (unsigned long long)m, back);
            fail++;
        }
    }
    if (popcount64(0xBF) != 7)
        fail++;

    /* which process is the game (Windows: 12.40 s CPU, 59 threads) */
    Cand c[5];
    /* the Winlator case: explorer.exe has the game's path on its command line but no CPU time;
     * the game's own process does not carry the EXE name */
    c[0] = cand(8645, "explorer.exe", 13, 50, 0);
    c[1] = cand(8600, "wineserver", 3, 900, 0);
    c[2] = cand(8700, "wine64-preload", 63, 1238, 0);
    int pick = pick_game(c, 3, 1240, 2, 59);
    printf("  game process: picked %d (want 8700)\n", pick >= 0 ? c[pick].pid : -1);
    if (pick != 2)
        fail++;
    /* a name match breaks a tie between two processes with the right CPU time */
    c[3] = cand(8800, "METAPHOR.exe", 61, 1300, 1);
    pick = pick_game(c, 4, 1240, 2, 59);
    if (pick != 3)
        fail++;
    /* too few threads, or CPU time too far off: none */
    c[0] = cand(1, "METAPHOR.exe", 20, 1240, 1);
    c[1] = cand(2, "METAPHOR.exe", 60, 2000, 1);
    pick = pick_game(c, 2, 1240, 2, 59);
    if (pick != -1)
        fail++;

    /* /proc/stat: user nice system idle iowait irq softirq steal; busy = all but idle and iowait */
    long long busy = 0, total = 0;
    ok = parse_proc_stat("cpu  100 5 50 800 20 3 2 0 0 0\ncpu0 1 2 3", &busy, &total);
    printf("  /proc/stat: ok=%d busy=%lld total=%lld\n", ok, busy, total);
    if (!ok || busy != 160 || total != 980 || parse_proc_stat("intr 1 2", &busy, &total))
        fail++;

    /* privacy: only the emulator's own processes are named in the log */
    if (strcmp(log_name("wineserver"), "wineserver") || strcmp(log_name("METAPHOR.exe"), "METAPHOR.exe") ||
        strcmp(log_name("FEXInterpreter"), "FEXInterpreter") || strcmp(log_name("firefox"), "(another program)") ||
        strcmp(log_name("discord"), "(another program)"))
        fail++;
    printf("  names in the log: wineserver -> %s, firefox -> %s\n", log_name("wineserver"), log_name("firefox"));

    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}
