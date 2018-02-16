#include "marla.h"
#include <string.h>
#include <unistd.h>
#include <openssl/rand.h>
#include <httpd.h>

AP_DECLARE(void) ap_log_perror_(const char *file, int line, int module_index,
                                int level, apr_status_t status, apr_pool_t *p,
                                const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char exp[512];
    memset(exp, 0, sizeof(exp));
    int len = vsprintf(exp, fmt, args);
    write(3, exp, len);
    va_end(args);
}

struct FixedSource {
unsigned char* content;
int nread;
int len;
};

static int readSource(struct marla_Connection* cxn, void* sink, size_t len)
{
    struct FixedSource* src = cxn->source;
    int slen = src->len;
    int nwritten = len;
    if(nwritten > slen - src->nread) {
        nwritten = slen - src->nread;
    }
    if(nwritten == 0) {
        return -1;
    }
    memcpy(sink, src->content + src->nread, nwritten);
    src->nread += nwritten;
    return nwritten;
}

static int writeSource(struct marla_Connection* cxn, void* source, size_t len)
{
    //write(1, source, len);
    return len;
}

static int describeSource(marla_Connection* cxn, char* sink, size_t len)
{
    struct FixedSource* src = cxn->source;
    memset(sink, 0, len);
    snprintf(sink, len, "Fixed(%d, %d)", src->nread, src->len);
    return 0;
}

static void acceptSource(marla_Connection* cxn)
{
    // Accepted and secured.
    cxn->stage = marla_CLIENT_SECURED;
}

static int shutdownSource(marla_Connection* cxn)
{
    return 0;
}

static void destroySource(marla_Connection* cxn)
{
    // Noop.
}

static int test_simple(struct marla_Server* server, const char* port)
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
    marla_Connection* cxn = marla_Connection_new(server);
    struct FixedSource src = {
        (unsigned char*)source_str,
        0,
        strlen(source_str)
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->acceptSource = acceptSource;
    cxn->shutdownSource = shutdownSource;
    cxn->destroySource = destroySource;
    cxn->describeSource = describeSource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);

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
        marla_clientRead(cxn);
        marla_clientWrite(cxn);
    }

    if(cxn->stage == marla_CLIENT_COMPLETE) {
        fprintf(stderr, "Connection must still be open\n");
        return 1;
    }

    source_str[0] = 0x88;
    source_str[1] = 0x82;
    source_str[2] = 0x38;
    source_str[3] = 0xfb;
    source_str[4] = 0x22;
    source_str[5] = 0x3e;
    uint16_t closeCode = 1000;
    source_str[6] = ((char)(closeCode >> 8)) ^ source_str[2];
    source_str[7] = ((char)closeCode) ^ source_str[3];
    src.nread = 0;
    src.len = 8;
    marla_clientRead(cxn);
    marla_clientWrite(cxn);

    if(cxn->stage != marla_CLIENT_COMPLETE) {
        fprintf(stderr, "Connection must no longer be open\n");
        return 1;
    }

    marla_Connection_destroy(cxn);
    return 0;
}

int main(int argc, char* argv[])
{
    fprintf(stderr, "Testing WebSocket\r\n");

    if(argc < 1) {
        printf("Usage: %s <port>\n", argv[0]);
        return -1;
    }

    struct marla_Server server;
    marla_Server_init(&server);

    int failed = 0;
    const char* port = argv[1];
    printf("test_simple:");
    if(0 == test_simple(&server, port)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    marla_Server_free(&server);
    return failed;
}
