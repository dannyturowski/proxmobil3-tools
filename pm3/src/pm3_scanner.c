/* Scanner illumination commands; see pm3_scanner.h. */
#include "pm3_scanner.h"

#include <stdio.h>

int pm3_scanner_level_cmd(char *out, size_t cap, int level)
{
    if (level < 0 || level > PM3_SCANNER_MAX_LEVEL) return -1;
    int n = snprintf(out, cap, "RDILPMB%d\r\n", level);
    return n > 0 && (size_t)n < cap ? n : -1;
}

static int is_word(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

/* Accepts the XML attribute form the engine answers with, MB="25" (as in
 * <RD><IL LO="0" MB="25" AC="0" /></RD>), and a bare token MB25. */
int pm3_scanner_parse_level(const char *s, size_t n, int *level)
{
    for (size_t i = 0; i + 2 < n; i++) {
        if (s[i] != 'M' || s[i + 1] != 'B' || (i > 0 && is_word(s[i - 1]))) continue;
        size_t j = i + 2;
        int quoted = 0;
        if (j + 1 < n && s[j] == '=' && s[j + 1] == '"') { quoted = 1; j += 2; }
        long v = 0;
        int digits = 0;
        while (j < n && s[j] >= '0' && s[j] <= '9' && digits < 4) { v = v * 10 + (s[j] - '0'); j++; digits++; }
        if (!digits) continue;
        if (quoted ? (j >= n || s[j] != '"') : (j < n && is_word(s[j]))) continue;
        if (v > PM3_SCANNER_MAX_LEVEL) continue;
        *level = (int)v;
        return 1;
    }
    return 0;
}
