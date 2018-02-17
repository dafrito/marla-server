#include "marla.h"
#include <httpd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
