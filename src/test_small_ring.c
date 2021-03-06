#include "marla.h"
#include <string.h>
#include <httpd.h>
#include <http_config.h>
#include <http_protocol.h>
#include <ap_config.h>
#include <apr_dbd.h>
#include <mod_dbd.h>
#include <unistd.h>

AP_DECLARE(void) ap_log_perror_(const char *file, int line, int module_index,
                                int level, apr_status_t status, apr_pool_t *p,
                                const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char exp[512];
    memset(exp, 0, sizeof(exp));
    int len = vsprintf(exp, fmt, args);
    write(3, exp, len);
    va_end(args);
}

int main()
{
    int CAP = 8;
    marla_Ring* ring = marla_Ring_new(CAP);

    const char* line = "0123456789";
    int linelen = strlen(line) + 1;
    int nwritten = marla_Ring_write(ring, line, linelen);
    if(nwritten > CAP) {
        fprintf(stderr, "nwritten > capacity\n");
        return 1;
    }

    unsigned char out[marla_BUFSIZE];
    int nread = marla_Ring_read(ring, out, CAP);
    if(nread != CAP) {
        fprintf(stderr, "nread(%d) must be equal to the capacity(%d)\n", nread, CAP);
        return 2;
    }
    out[CAP] = 0;
    if(memcmp("01234567", out, 8)) {
        fprintf(stderr, "Strings are not equal: %s\n", (char*)out);
        return 3;
    }

    marla_Ring_free(ring);
    return 0;
}
