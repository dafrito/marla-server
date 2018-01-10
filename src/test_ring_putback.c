#include "marla.h"
#include <string.h>
#include <httpd.h>
#include <http_config.h>
#include <http_protocol.h>
#include <ap_config.h>
#include <apr_dbd.h>
#include <mod_dbd.h>

AP_DECLARE(void) ap_log_perror_(const char *file, int line, int module_index,
                                int level, apr_status_t status, apr_pool_t *p,
                                const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char exp[512];
    memset(exp, 0, sizeof(exp));
    vsprintf(exp, fmt, args);
    dprintf(3, exp);
    va_end(args);
}

int main()
{
    int CAP = 1024;
    marla_Ring* ring = marla_Ring_new(CAP);

    const char* line = "0123456789";
    int nwritten = marla_Ring_write(ring, line, 4);
    if(nwritten > CAP) {
        fprintf(stderr, "nwritten > capacity\n");
        return 1;
    }

    marla_Ring_putbackWrite(ring, 1);
    marla_Ring_writec(ring, 'P');

    unsigned char out[marla_BUFSIZE];
    int nread = marla_Ring_read(ring, out, CAP);
    if(nread != 4) {
        fprintf(stderr, "nread(%d) must be equal to 4, the expected number of written characters.", nread);
        return 2;
    }
    out[nread] = 0;
    if(memcmp("012P", out, 4)) {
        fprintf(stderr, "Strings are not equal: %s\n", out);
        return 3;
    }

    marla_Ring_putbackRead(ring, 1);
    char c = marla_Ring_readc(ring);
    if(c != 'P') {
        fprintf(stderr, "P must be found once putback.\n");
        return 4;
    }

    marla_Ring_free(ring);
    return 0;
}
