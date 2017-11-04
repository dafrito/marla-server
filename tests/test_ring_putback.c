#include "rainback.h"
#include <string.h>

int main()
{
    int CAP = 1024;
    parsegraph_Ring* ring = parsegraph_Ring_new(CAP);

    const char* line = "0123456789";
    int linelen = strlen(line) + 1;
    int nwritten = parsegraph_Ring_write(ring, line, 4);
    if(nwritten > CAP) {
        fprintf(stderr, "nwritten > capacity\n");
        return 1;
    }

    parsegraph_Ring_putbackWrite(ring, 1);
    parsegraph_Ring_writec(ring, 'P');

    char out[parsegraph_BUFSIZE];
    int nread = parsegraph_Ring_read(ring, out, CAP);
    if(nread != 4) {
        fprintf(stderr, "nread(%d) must be equal to 4, the expected number of written characters.", nread);
        return 2;
    }
    out[nread] = 0;
    if(strcmp("012P", out)) {
        fprintf(stderr, "Strings are not equal: %s\n", out);
        return 3;
    }

    parsegraph_Ring_putback(ring, 1);
    char c = parsegraph_Ring_readc(ring);
    if(c != 'P') {
        fprintf(stderr, "P must be found once putback.\n");
        return 4;
    }

    parsegraph_Ring_free(ring);
    return 0;
}
