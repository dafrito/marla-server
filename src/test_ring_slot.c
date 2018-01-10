#include "marla.h"
#include <string.h>

int main()
{
    int CAP = 8;
    marla_Ring* ring = marla_Ring_create(CAP);

    const char* line = "0123456789";
    int linelen = strlen(line) + 1;
    int nwritten = marla_Ring_write(ring, line, 4);
    if(nwritten != 4) {
        fprintf(stderr, "Write underflowed(%d).\n", nwritten);
        return 1;
    }

    void* buf;
    size_t len;
    marla_Ring_writeSlot(ring, &buf, &len);

    if(len != 4) {
        fprintf(stderr, "Write slot underflowed(%d).\n", nwritten);
        return 1;
    }

    if(marla_Ring_size(ring) != 8) {
        fprintf(stderr, "Ring size(%d) is unexpected.\n", marla_Ring_size(ring));
        return 1;
    }

    marla_Ring_readSlot(ring, &buf, &len);
    if(len != 8) {
        fprintf(stderr, "Read slot underflowed(%d).\n", nwritten);
        return 1;
    }

    if(marla_Ring_size(ring) != 0) {
        fprintf(stderr, "Ring size(%d) is unexpected.\n", marla_Ring_size(ring));
        return 1;
    }

    marla_Ring_free(ring);
    return 0;
}
