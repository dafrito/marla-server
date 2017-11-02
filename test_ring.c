#include "rainback.h"
#include <string.h>

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
