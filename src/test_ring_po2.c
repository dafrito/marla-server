#include "marla.h"
#include <httpd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char** argv)
{
    if(argc < 2) {
        fprintf(stderr, "Usage: test_ring_po2 <CAP>");
    }

    char* endptr;
    long cap = strtol(argv[1], &endptr, 10);
    marla_Ring* ring = marla_Ring_new(cap);
    marla_Ring_free(ring);
    return 0;
}
