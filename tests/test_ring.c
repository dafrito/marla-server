#include "prepare.h"
#include "rainback.h"
#include <string.h>

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
    parsegraph_Ring* ring = parsegraph_Ring_new(parsegraph_BUFSIZE);

    const char* line = "Hello, world!";
    int linelen = strlen(line) + 1;
    parsegraph_Ring_write(ring, line, linelen);

    char out[parsegraph_BUFSIZE];
    int nread = parsegraph_Ring_read(ring, out, parsegraph_BUFSIZE);
    if(nread != linelen) {
        fprintf(stderr, "nread(%d) must be equal to the linelen(%d)\n", nread, linelen);
        return 1;
    }
    if(strcmp(line, out)) {
        fprintf(stderr, "Strings are not equal");
        return 1;
    }

    parsegraph_Ring_free(ring);
    return 0;
}
