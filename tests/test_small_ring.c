#include "rainback.h"
#include <string.h>

int main()
{
    int CAP = 8;
    parsegraph_Ring* ring = parsegraph_Ring_new(CAP);

    const char* line = "0123456789";
    int linelen = strlen(line) + 1;
    int nwritten = parsegraph_Ring_write(ring, line, linelen);
    if(nwritten > CAP) {
        fprintf(stderr, "nwritten > capacity\n");
        return 1;
    }

    char out[parsegraph_BUFSIZE];
    int nread = parsegraph_Ring_read(ring, out, CAP);
    if(nread != CAP) {
        fprintf(stderr, "nread(%d) must be equal to the capacity(%d)\n", nread, CAP);
        return 2;
    }
    out[CAP] = 0;
    if(strcmp("01234567", out)) {
        fprintf(stderr, "Strings are not equal: %s\n", out);
        return 3;
    }

    parsegraph_Ring_free(ring);
    return 0;
}
