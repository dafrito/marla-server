#include "marla.h"
#include <string.h>
#include <unistd.h>

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
    unsigned char c;
    if(marla_Ring_readc(ring, &c) != 1 || c != 'P') {
        fprintf(stderr, "P must be found once putback.\n");
        return 4;
    }

    marla_Ring_free(ring);
    return 0;
}
