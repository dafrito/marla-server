#include "rainback.h"
#include <string.h>
#include <unistd.h>

struct FixedSource {
const char* content;
int nread;
};

static int readSource(struct parsegraph_Connection* cxn, void* sink, size_t len)
{
    struct FixedSource* src = cxn->source;
    int slen = strlen(src->content);
    int nwritten = len;
    if(nwritten > slen - src->nread) {
        nwritten = slen - src->nread;
    }
    if(nwritten == 0) {
        return 0;
    }
    memcpy(sink, src->content + src->nread, nwritten);
    src->nread += nwritten;
    printf("READING %d\n", nwritten);
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
    snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_simple_lf(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\nHost: localhost:%s\n\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_simple_mixed_lf(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\nHost: localhost:%s\r\n\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_simple_leading_crlf(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "\r\nGET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_simple_leading_lf(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "\nGET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_bad_method(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_no_host(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\n\r\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_no_version(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "GET /\r\nHost: localhost: %s\r\n\r\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_no_target(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\n\r\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

    parsegraph_Connection_destroy(cxn);
}

static int test_nothing(const char* port)
{
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    snprintf(source_str, sizeof(source_str) - 1, "\r\n\r\n", port);
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    struct FixedSource src = {
        source_str,
        0
    };
    cxn->source = &src;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;

    // Read from the source.
    parsegraph_Connection_handle(cxn, 0);

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
    printf("test_simple_lf:");
    test_simple_lf(port);
    test_simple_lf(port);
    printf("test_simple_mixed_lf:");
    test_simple_mixed_lf(port);
    printf("test_simple_leading_lf:");
    test_simple_leading_lf(port);
    printf("test_simple_leading_crlf:");
    test_simple_leading_crlf(port);
    printf("test_bad_method:");
    test_bad_method(port);
    printf("test_no_host:");
    test_no_host(port);
    printf("test_no_version:");
    test_no_version(port);
    printf("test_no_target:");
    test_no_target(port);
    printf("test_nothing:");
    test_nothing(port);
    printf("Tests finished.\n");
    return 0;
}
