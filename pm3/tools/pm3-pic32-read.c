/* pm3-pic32-read: read PIC32 board-controller registers (ambient light, input voltage)
 * through the stock `NxExe prox2 read` utility and print one JSON line per reading.
 * Read-only.  See docs/software/pm3-pic32.md.
 *
 *   pm3-pic32-read ambient
 *   pm3-pic32-read --count 0 --interval 5 ambient input-voltage
 *
 * Exit: 0 every reading succeeded, 1 at least one failed, 2 usage. */
#define _GNU_SOURCE
#include "pm3_pic32.h"
#include "pm3_serial.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TOOL_VERSION "0.1.0"

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static int usage(FILE *o)
{
    fprintf(o, "usage: pm3-pic32-read [options] REGISTER...\n"
               "  REGISTER          ");
    for (size_t i = 0; i < PM3_PIC32_NREGS; i++) fprintf(o, "%s%s", i ? " | " : "", PM3_PIC32_REGS[i].name);
    fprintf(o, "\n"
               "  --count N         rounds to read (default 1, 0 = until SIGINT/SIGTERM)\n"
               "  --interval S      seconds between rounds (default 5, minimum 1)\n"
               "  --timeout S       per-read deadline (default 3)\n"
               "  --nxexe PATH      stock utility (default /usr/bin/NxExe)\n"
               "  --workdir DIR     its working directory (default /run)\n"
               "  --list            print the known registers as JSON and exit\n"
               "  --version\n");
    return o == stdout ? 0 : 2;
}

int main(int argc, char **argv)
{
    const char *nxexe = "/usr/bin/NxExe", *workdir = "/run";
    long count = 1;
    double interval = 5, timeout = 3;
    const pm3_pic32_reg *regs[8];
    int nregs = 0;
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(o, "--count") && more) { count = atol(argv[++i]); if (count < 0) return usage(stderr); }
        else if (!strcmp(o, "--interval") && more) { interval = atof(argv[++i]); if (interval < 1) return usage(stderr); }
        else if (!strcmp(o, "--timeout") && more) { timeout = atof(argv[++i]); if (timeout <= 0 || timeout > 30) return usage(stderr); }
        else if (!strcmp(o, "--nxexe") && more) nxexe = argv[++i];
        else if (!strcmp(o, "--workdir") && more) workdir = argv[++i];
        else if (!strcmp(o, "--list")) {
            for (size_t k = 0; k < PM3_PIC32_NREGS; k++)
                printf("{\"register\":\"%s\",\"addr\":\"0x%04x\",\"units\":\"%s\"}\n",
                       PM3_PIC32_REGS[k].name, PM3_PIC32_REGS[k].addr, PM3_PIC32_REGS[k].units);
            return 0;
        }
        else if (!strcmp(o, "--version")) { puts("pm3-pic32-read " TOOL_VERSION); return 0; }
        else if (!strcmp(o, "--help") || !strcmp(o, "-h")) return usage(stdout);
        else if (o[0] == '-') return usage(stderr);
        else {
            const pm3_pic32_reg *r = pm3_pic32_find(o);
            if (!r || nregs >= (int)(sizeof regs / sizeof regs[0])) { fprintf(stderr, "unknown register: %s\n", o); return usage(stderr); }
            regs[nregs++] = r;
        }
    }
    if (!nregs) return usage(stderr);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    double t0 = pm3_now();
    int failed = 0;
    for (long round = 0; !g_stop && (count == 0 || round < count); round++) {
        if (round) {
            double until = pm3_now() + interval;
            while (!g_stop && pm3_now() < until) usleep(50000);
            if (g_stop) break;
        }
        for (int k = 0; k < nregs && !g_stop; k++) {
            int v = 0;
            char why[32] = "";
            double t = pm3_now();
            if (pm3_pic32_read(nxexe, workdir, regs[k]->addr, timeout, &v, why, sizeof why) == 0)
                printf("{\"t\":%.3f,\"event\":\"pic32_read\",\"register\":\"%s\",\"addr\":\"0x%04x\",\"value\":%d,\"elapsed\":%.3f}\n",
                       t - t0, regs[k]->name, regs[k]->addr, v, pm3_now() - t);
            else {
                failed = 1;
                printf("{\"t\":%.3f,\"event\":\"pic32_error\",\"register\":\"%s\",\"addr\":\"0x%04x\",\"reason\":\"%s\",\"elapsed\":%.3f}\n",
                       t - t0, regs[k]->name, regs[k]->addr, why, pm3_now() - t);
            }
            fflush(stdout);
        }
    }
    return failed;
}
