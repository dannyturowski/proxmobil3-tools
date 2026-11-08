/* Read-only access to PIC32 board-controller registers through the stock utility
 * (`NxExe prox2 read <addr>`), which owns the controller link.  Nothing here
 * writes to the PIC32 or changes its watchdog, power or backlight. */
#ifndef PM3_PIC32_H
#define PM3_PIC32_H

#include <stddef.h>

typedef struct {
    const char *name;     /* CLI name */
    unsigned addr;        /* register address */
    const char *units;    /* what the raw value means, as far as verified */
} pm3_pic32_reg;

/* The registers this toolkit reads.  Only read-only, documented ones are listed. */
extern const pm3_pic32_reg PM3_PIC32_REGS[];
extern const size_t PM3_PIC32_NREGS;
const pm3_pic32_reg *pm3_pic32_find(const char *name);

/* Parse the utility's output: a line "Addr <decimal addr>: <decimal value>".
 * Accepts surrounding lines, tabs/spaces and CRLF; rejects signs, junk and values
 * above 65535.  Returns 1 and the value, or 0. */
int pm3_pic32_parse(const char *out, unsigned addr, int *value);

/* Run `<nxexe> prox2 read 0x<addr>` with stdin from /dev/null, cwd `workdir`,
 * stdout+stderr captured (bounded) and a hard deadline; the child is killed at the
 * deadline.  Returns 0 and the value, or -1 with a short reason
 * ("spawn", "timeout", "exit=N", "signal=N", "parse"). */
int pm3_pic32_read(const char *nxexe, const char *workdir, unsigned addr, double timeout_s,
                   int *value, char *reason, size_t reasoncap);

#endif
