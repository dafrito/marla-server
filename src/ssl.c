#include "rainback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

static void common_SSL_return(parsegraph_Connection* cxn, int rv)
{
    parsegraph_SSLSource* cxnSource = cxn->source;
    switch(SSL_get_error(cxnSource->ssl, rv)) {
    case SSL_ERROR_WANT_WRITE:
        cxn->wantsWrite = 1;
        break;
    case SSL_ERROR_WANT_READ:
        cxn->wantsRead = 1;
        break;
    default:
        cxn->shouldDestroy = 1;
        break;
    }
}

static int describeSSLSource(parsegraph_Connection* cxn, char* sink, size_t len)
{
    parsegraph_SSLSource* cxnSource = cxn->source;
    memset(sink, 0, len);
    snprintf(sink, len, "FD %d SSL", cxnSource->fd);
    return 0;
}

static int readSSLSource(parsegraph_Connection* cxn, void* sink, size_t len)
{
    parsegraph_SSLSource* cxnSource = cxn->source;
    int nsslread = SSL_read(cxnSource->ssl, sink, len);
    if(nsslread <= 0) {
        common_SSL_return(cxn, nsslread);
        return -1;
    }
    return nsslread;
}

static int writeSSLSource(parsegraph_Connection* cxn, void* source, size_t len)
{
    parsegraph_SSLSource* cxnSource = cxn->source;
    int nsslwritten = SSL_write(cxnSource->ssl, source, len);
    if(nsslwritten <= 0) {
        common_SSL_return(cxn, nsslwritten);
        return -1;
    }
    return nsslwritten;
}

static void acceptSSLSource(parsegraph_Connection* cxn)
{
    parsegraph_SSLSource* cxnSource = cxn->source;
    int rv = SSL_accept(cxnSource->ssl);
    if(rv == 0) {
        // Shutdown controlled
        cxn->shouldDestroy = 1;
        return;
    }
    else if(rv != 1) {
        common_SSL_return(cxn, rv);
        return;
    }

    // Accepted and secured.
    cxn->stage = parsegraph_CLIENT_SECURED;
}

static int shutdownSSLSource(parsegraph_Connection* cxn)
{
    parsegraph_SSLSource* cxnSource = cxn->source;
    int rv = SSL_shutdown(cxnSource->ssl);
    if(rv == 1) {
        return rv;
    }
    if(rv == 0) {
        rv = SSL_shutdown(cxnSource->ssl);
        common_SSL_return(cxn, rv);
        return rv;
    }
    if(rv < 0) {
        common_SSL_return(cxn, rv);
    }
    return rv;
}

static void destroySSLSource(parsegraph_Connection* cxn)
{
    parsegraph_SSLSource* cxnSource = cxn->source;
    SSL_free(cxnSource->ssl);
    close(cxnSource->fd);
    free(cxn->source);
}

int parsegraph_SSL_init(parsegraph_Connection* cxn, SSL_CTX* ctx, int fd)
{
    parsegraph_SSLSource* source = malloc(sizeof *source);
    cxn->source = source;
    cxn->readSource = readSSLSource;
    cxn->writeSource = writeSSLSource;
    cxn->acceptSource = acceptSSLSource;
    cxn->shutdownSource = shutdownSSLSource;
    cxn->destroySource = destroySSLSource;
    cxn->describeSource = describeSSLSource;
    source->ctx = ctx;
    source->fd = fd;
    source->ssl = SSL_new(ctx);
    return SSL_set_fd(source->ssl, fd);
}
