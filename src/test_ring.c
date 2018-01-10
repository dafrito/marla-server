#include "marla.h"
#include <string.h>
#include <httpd.h>

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

int test_ring_read()
{
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);

    const char* line = "Hello, world!";
    int linelen = strlen(line) + 1;
    marla_Ring_write(ring, line, linelen);

    unsigned char out[marla_BUFSIZE];
    size_t nread = marla_Ring_read(ring, out, marla_BUFSIZE);
    if(nread != linelen) {
        fprintf(stderr, "nread(%ld) must be equal to the linelen(%d)\n", nread, linelen);
        return 1;
    }
    if(memcmp(line, out, nread)) {
        fprintf(stderr, "Strings are not equal");
        return 1;
    }

    marla_Ring_free(ring);
    return 0;
}

int test_ring_write()
{
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);

    void* slotData;
    size_t slotLen;
    marla_Ring_writeSlot(ring, &slotData, &slotLen);

    if(slotLen != marla_BUFSIZE) {
        fprintf(stderr, "slotLen must be equal to the BUFSIZE for an empty ring.\n");
        return 1;
    }
    marla_Ring_writeSlot(ring, &slotData, &slotLen);
    if(slotLen != 0) {
        fprintf(stderr, "writeSlot must not treat full rings as empty. Size reported was %ld\n", marla_Ring_size(ring));
        return 1;
    }

    marla_Ring_free(ring);
    return 0;
}

int test_ring_readSlot()
{
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    for(int i = 0; i < marla_BUFSIZE; ++i) {
        marla_Ring_write(ring, "A", 1);
    }

    void* slotData;
    size_t slotLen;
    marla_Ring_readSlot(ring, &slotData, &slotLen);
    if(slotLen != marla_BUFSIZE) {
        fprintf(stderr, "slotLen must be equal to the BUFSIZE for a full ring.\n");
        return 1;
    }
    marla_Ring_readSlot(ring, &slotData, &slotLen);
    if(slotLen != 0) {
        fprintf(stderr, "readSlot must not treat empty rings as full.\n");
        return 1;
    }

    marla_Ring_free(ring);
    return 0;
}

int test_ring_nearFullReads()
{
    const int CAP = 16;
    marla_Ring* ring = marla_Ring_new(CAP);
    for(int i = 0; i < CAP-1; ++i) {
        marla_Ring_writec(ring, 1+i);
    }
    if(marla_Ring_size(ring) != CAP-1) {
        fprintf(stderr, "Ring's size must match the number of bytes written.\n");
        return 1;
    }
    void* bytes;
    size_t slotLen;
    marla_Ring_readSlot(ring, &bytes, &slotLen);
    if(slotLen != CAP-1) {
        fprintf(stderr, "Ring must return the same number of bytes as written.\n");
        return 1;
    }
    for(int i = 0; i < CAP-1; ++i) {
        char c = ((char*)bytes)[i];
        if(c != 1+i) {
            fprintf(stderr, "Ring must return bytes in the order written. (%d != %d\n", c, 1+i);
            return 1;
        }
    }
    marla_Ring_readSlot(ring, &bytes, &slotLen);
    if(slotLen != 0) {
        fprintf(stderr, "Ring must, once all bytes are written, return empty read slots.\n");
        return 1;
    }

    marla_Ring_free(ring);
    return 0;
}

int test_ring_emptyWrite()
{
    const int CAP = 16;
    char buf[CAP];
    const char* given = "abcd" "efgh" "ijkl" "mnop";
    memcpy(buf, given, 16);
    marla_Ring* ring = marla_Ring_new(CAP);
    marla_Ring_write(ring,buf, 16);
    marla_Ring_write(ring,buf, 0);

    char in[CAP + 1];
    if(16 != marla_Ring_read(ring, (unsigned char*)in, 16)) {
        fprintf(stderr, "Ring must read the full ring.\n");
        return 1;
    }
    in[CAP] = 0;
    if(strcmp(given, in)) {
        fprintf(stderr, "Empty write must not affect ring state.\n");
        return 1;
    }

    marla_Ring_free(ring);
    return 0;
}

int test_ring_simplify()
{
    const int CAP = 1024;
    unsigned char buf[CAP];
    const char* given = "abcd" "efgh" "ijkl" "mnop";
    memcpy(buf, given, 16);
    marla_Ring* ring = marla_Ring_new(CAP);
    marla_Ring_write(ring,buf, 16);
    marla_Ring_read(ring,buf,4);
    marla_Ring_simplify(ring);

    if(ring->read_index != 0) {
        return 1;
    }
    marla_Ring_read(ring,buf,12);
    buf[12] = 0;
    if(strcmp("efghijklmnop", (char*)buf)) {
        return 1;
    }
    marla_Ring_simplify(ring);
    return 0;
}

int main()
{
    return test_ring_read() ||
        test_ring_write() ||
        test_ring_readSlot() ||
        test_ring_nearFullReads() ||
        test_ring_emptyWrite() ||
        test_ring_simplify();
}
