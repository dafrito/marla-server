#include "rainback.h"
#include <string.h>
#include <unistd.h>
#include <openssl/rand.h>

struct FixedSource {
unsigned char* content;
int nread;
int len;
};

static int readSource(struct parsegraph_Connection* cxn, void* sink, size_t len)
{
    struct FixedSource* src = cxn->source;
    int slen = src->len;
    int nwritten = len;
    if(nwritten > slen - src->nread) {
        nwritten = slen - src->nread;
    }
    if(nwritten == 0) {
        return 0;
    }
    memcpy(sink, src->content + src->nread, nwritten);
    src->nread += nwritten;
    return nwritten;
}

static int writeSource(struct parsegraph_Connection* cxn, void* source, size_t len)
{
    //write(1, source, len);
    return len;
}

static int test_simple(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));

    unsigned char handkey[16];
    RAND_bytes(handkey, sizeof handkey);

    BIO* mem = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, mem);
    BIO_write(b64, handkey, sizeof handkey);
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);

    unsigned char encodedhandkey[32];
    memcpy(encodedhandkey, bptr->data, bptr->length);
    encodedhandkey[bptr->length] = 0;
    BIO_free_all(b64);

    snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\nHost: localhost:%s\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", port, encodedhandkey);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0,
        strlen(source_str)
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    for(int i = 0; i < 10; ++i) {
        source_str[0] = 0x81;
        source_str[1] = 0x85;
        source_str[2] = 0x37;
        source_str[3] = 0xfa;
        source_str[4] = 0x21;
        source_str[5] = 0x3d;
        source_str[6] = 0x7f;
        source_str[7] = 0x9f;
        source_str[8] = 0x4d;
        source_str[9] = 0x51;
        source_str[10] = 0x58;
        src.nread = 0;
        src.len = 11;
        parsegraph_Connection_handle(cxn, 0);
    }

    parsegraph_Connection_destroy(cxn);
}

int main(int argc, char* argv[])
{
    if(argc < 1) {
        printf("Usage: %s <port>\n", argv[0]);
        return -1;
    }

    const char* port = argv[1];
    printf("test_simple:");
    test_simple(port);
    return 0;
}
