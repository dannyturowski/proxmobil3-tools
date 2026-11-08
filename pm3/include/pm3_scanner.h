/* Barcode-scanner illumination control for the Code CR8200-family engine fitted to
 * some PROXmobil3 units (FTDI USB serial, 115200 8N1).
 *
 * Commands are Code's text configuration commands, each ended by CR LF:
 *   RDILPMB<n>   runtime ("P", not saved) illumination brightness, n = 0..100; 0 = LEDs off
 *   RDILG        query the illumination settings; one unit answered
 *                <RD><IL LO="0" MB="25" AC="0" /></RD>
 * Runtime settings return to the saved configuration on a scanner power cycle, so
 * "off" has to be re-applied at every boot (see docs/software/pm3-scanner-light.md).
 * Units with a Honeywell engine use a different command set and are not supported. */
#ifndef PM3_SCANNER_H
#define PM3_SCANNER_H

#include <stddef.h>

#define PM3_SCANNER_MAX_LEVEL 100

/* "RDILPMB<n>\r\n" into out.  Returns its length, or -1 if level is out of range. */
int pm3_scanner_level_cmd(char *out, size_t cap, int level);

/* Find the illumination brightness in a reply: the first MB="<digits>" attribute or
 * whole token MB<digits> (not part of a longer word).  Other bytes are ignored.
 * Returns 1 and the level (0..100), or 0. */
int pm3_scanner_parse_level(const char *reply, size_t n, int *level);

/* FTDI FT231X, the scanner's USB-serial bridge on the units this was written for. */
#define PM3_SCANNER_VID 0x0403
#define PM3_SCANNER_PID 0x6015

#endif
