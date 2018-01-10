#include "marla.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

static void common_SSL_return(marla_Connection* cxn, int rv)
{
    marla_SSLSource* cxnSource = cxn->source;
    switch(SSL_get_error(cxnSource->ssl, rv)) {
    case SSL_ERROR_WANT_WRITE:
        cxn->wantsWrite = 1;
        break;
    case SSL_ERROR_WANT_READ:
        cxn->wantsRead = 1;
        break;
    case SSL_ERROR_SYSCALL:
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            cxn->wantsWrite = 1;
            cxn->wantsRead = 1;
            //fprintf(stderr, "SSL connection is asynchronously awating input because of syscall: %d %d %d\n", rv, SSL_get_error(cxnSource->ssl, rv), errno);
        }
        else if(errno != 0) {
            //fprintf(stderr, "SSL connection needs destruction because of syscall: %d %d %d\n", rv, SSL_get_error(cxnSource->ssl, rv), errno);
            cxn->shouldDestroy = 1;
        }
        else {
            return;
        }
        break;
    default:
        //fprintf(stderr, "SSL connection needs destruction: %d %d\n", rv, SSL_get_error(cxnSource->ssl, rv));
        cxn->shouldDestroy = 1;
        break;
    }
}

static int describeSSLSource(marla_Connection* cxn, char* sink, size_t len)
{
    marla_SSLSource* cxnSource = cxn->source;
    memset(sink, 0, len);
    snprintf(sink, len, "FD %d SSL", cxnSource->fd);
    return 0;
}

static int readSSLSource(marla_Connection* cxn, void* sink, size_t len)
{
    marla_SSLSource* cxnSource = cxn->source;
    int nsslread = SSL_read(cxnSource->ssl, sink, len);
    if(nsslread <= 0) {
        common_SSL_return(cxn, nsslread);
        return -1;
    }
    if(nsslread > 0) {
        marla_logMessagecf(cxn->server, "I/O", "Read %d bytes over SSL.", nsslread);
    }
    return nsslread;
}

static int writeSSLSource(marla_Connection* cxn, void* source, size_t len)
{
    marla_SSLSource* cxnSource = cxn->source;
    int nsslwritten = SSL_write(cxnSource->ssl, source, len);
    if(nsslwritten <= 0) {
        common_SSL_return(cxn, nsslwritten);
        return -1;
    }
    if(nsslwritten > 0) {
        marla_logMessagecf(cxn->server, "I/O", "Wrote %d bytes over SSL.", nsslwritten);
    }
    return nsslwritten;
}

static void acceptSSLSource(marla_Connection* cxn)
{
    marla_SSLSource* cxnSource = cxn->source;
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
    cxn->stage = marla_CLIENT_SECURED;
}

static int shutdownSSLSource(marla_Connection* cxn)
{
    marla_SSLSource* cxnSource = cxn->source;
    int rv = SSL_shutdown(cxnSource->ssl);
    if(rv == 1) {
        //fprintf(stderr, "SSL Shutdown completed\n");
        return rv;
    }
    if(rv == 0) {
        rv = SSL_shutdown(cxnSource->ssl);
        common_SSL_return(cxn, rv);
        //fprintf(stderr, "SSL Shutdown not yet finished. %d\n", rv);
        return rv;
    }
    if(rv < 0) {
        common_SSL_return(cxn, rv);
        if(SSL_get_error(cxnSource->ssl, rv) == SSL_ERROR_SYSCALL && errno == 0) {
            //fprintf(stderr, "SSL Shutdown not yet finished\n");
            return 0;
        }
        //fprintf(stderr, "SSL Shutdown failed\n");
    }
    return rv;
}

static void destroySSLSource(marla_Connection* cxn)
{
    marla_SSLSource* cxnSource = cxn->source;
    SSL_free(cxnSource->ssl);
    close(cxnSource->fd);
    free(cxn->source);
}

int marla_SSL_init(marla_Connection* cxn, SSL_CTX* ctx, int fd)
{
    marla_SSLSource* source = malloc(sizeof *source);
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
