/* Stand-ins for Watt-32's idna.c, which is left out of the build: its iconv
 * code and charset tables take about 200 KB that DOSAGENT has no use for.
 * They are only reached with DOMAIN.IDNA set in WATTCP.CFG (off by
 * default); then ASCII host names still resolve and others fail. */
#include <string.h>

#include "wattcp.h"
#include "idna.h"

BOOL iconv_init(WORD code_page)
{
    (void)code_page;
    return TRUE;
}

const char *iconv_strerror(int rc)
{
    (void)rc;
    return "IDNA support not built in";
}

BOOL IDNA_convert_to_ACE(char *name, size_t *size)
{
    const char *p;
    for (p = name; *p; p++)
        if (*p & 0x80)
            return FALSE;
    *size = strlen(name);
    return TRUE;
}

BOOL IDNA_convert_from_ACE(char *name, size_t *size)
{
    *size = strlen(name);
    return TRUE;
}
