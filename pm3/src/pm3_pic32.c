/* PIC32 register reads through the stock utility; see pm3_pic32.h. */
#define _GNU_SOURCE
#include "pm3_pic32.h"
#include "pm3_serial.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const pm3_pic32_reg PM3_PIC32_REGS[] = {
    { "ambient", 0x500b, "raw sensor counts, not lux (one bench unit: covered 0, room ~48, flashlight ~3273)" },
    { "input-voltage", 0x5040, "millivolts at the supply input (not a battery state of charge)" },
};
const size_t PM3_PIC32_NREGS = sizeof PM3_PIC32_REGS / sizeof PM3_PIC32_REGS[0];

const pm3_pic32_reg *pm3_pic32_find(const char *name)
{
    for (size_t i = 0; i < PM3_PIC32_NREGS; i++)
        if (!strcmp(PM3_PIC32_REGS[i].name, name)) return &PM3_PIC32_REGS[i];
    return NULL;
}

int pm3_pic32_parse(const char *s, unsigned addr, int *value)
{
    char key[32];
    snprintf(key, sizeof key, "Addr %u:", addr);
    const char *p = strstr(s, key);
    if (!p) return 0;
    if (p != s && p[-1] != '\n') return 0;            /* must start a line */
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return 0;
    char *end;
    errno = 0;
    long v = strtol(p, &end, 10);
    if (errno || v < 0 || v > 65535) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r') end++;
    if (*end != '\n' && *end != 0) return 0;
    *value = (int)v;
    return 1;
}

int pm3_pic32_read(const char *nxexe, const char *workdir, unsigned addr, double timeout_s,
                   int *value, char *reason, size_t reasoncap)
{
    char arg[16], buf[1024];
    size_t used = 0;
    int fds[2];
    snprintf(arg, sizeof arg, "0x%04x", addr);
    if (pipe2(fds, O_CLOEXEC) != 0) { snprintf(reason, reasoncap, "spawn"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); snprintf(reason, reasoncap, "spawn"); return -1; }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }
        if (chdir(workdir) != 0) _exit(126);
        execl(nxexe, "NxExe", "prox2", "read", arg, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    double end = pm3_now() + timeout_s;
    int st = 0, done = 0, timed_out = 0;
    for (;;) {
        double left = end - pm3_now();
        if (left <= 0) { timed_out = 1; break; }
        struct pollfd p = { fds[0], POLLIN, 0 };
        int r = poll(&p, 1, (int)(left * 1000) + 1);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            ssize_t n = read(fds[0], buf + used, sizeof buf - 1 - used);
            if (n > 0) { used += (size_t)n; if (used >= sizeof buf - 1) break; continue; }
            if (n == 0) break;                         /* all writers closed: the child is exiting */
        }
    }
    close(fds[0]);
    buf[used] = 0;
    if (timed_out) kill(pid, SIGKILL);
    /* Bounded wait for the exit status; a child that hangs after closing its output is killed. */
    for (int i = 0; i < 200 && !done; i++) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) done = 1;
        else if (r < 0 && errno != EINTR) break;
        else if (i == 100) kill(pid, SIGKILL);
        else usleep(10000);
    }
    if (!done) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    if (timed_out) { snprintf(reason, reasoncap, "timeout"); return -1; }
    if (WIFSIGNALED(st)) { snprintf(reason, reasoncap, "signal=%d", WTERMSIG(st)); return -1; }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { snprintf(reason, reasoncap, "exit=%d", WIFEXITED(st) ? WEXITSTATUS(st) : -1); return -1; }
    if (!pm3_pic32_parse(buf, addr, value)) { snprintf(reason, reasoncap, "parse"); return -1; }
    return 0;
}
