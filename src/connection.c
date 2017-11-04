#include "rainback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

// SSL source
    // int parsegraph_Source_New(SSL_CTX* ctx, int fd)
    // void parsegraph_Source_Destroy()
    // int parsegraph_Source_Init(SSL*)
    // int parsegraph_Source_Read(void* sink, size_t len)
    // int parsegraph_Source_Write(void* source, size_t len)

// Loopback source
    // int parsegraph_Source_New(int bufsize)
    // void parsegraph_Source_Destroy()
    // int parsegraph_Source_Init()
    // int parsegraph_Source_Read(void* sink, size_t len)
    // int parsegraph_Source_Write(void* source, size_t len)
    // int parsegraph_Source_Feed(void* source, size_t len) <-- loopback
    // int parsegraph_Source_Consume(void* sink, size_t len) <-- loopback

// parsegraph_Connection {
//     void* source;

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
    if(rv <= 0) {
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
    source->ctx = ctx;
    source->fd = fd;
    source->ssl = SSL_new(ctx);
    return SSL_set_fd(source->ssl, fd);
}

parsegraph_Connection* parsegraph_Connection_new()
{
    parsegraph_Connection* cxn = (parsegraph_Connection*)malloc(sizeof(*cxn));
    if(!cxn) {
        return 0;
    }

    // Initialize flags.
    cxn->shouldDestroy = 0;
    cxn->wantsWrite = 0;
    cxn->wantsRead = 0;

    cxn->source = 0;
    cxn->readSource = 0;
    cxn->writeSource = 0;
    cxn->acceptSource = 0;
    cxn->shutdownSource = 0;
    cxn->destroySource = 0;

    // Initialize the buffer.
    cxn->input = parsegraph_Ring_new(parsegraph_BUFSIZE);
    cxn->output = parsegraph_Ring_new(parsegraph_BUFSIZE);

    cxn->stage = parsegraph_CLIENT_ACCEPTED;
    cxn->requests_in_process = 0;
    cxn->latest_request = 0;
    cxn->current_request = 0;

    return cxn;
}

int parsegraph_Connection_read(parsegraph_Connection* cxn, char* sink, size_t requested)
{
    parsegraph_Ring* const input = cxn->input;

    int sinkread = 0;
    int partialRead = 0;
    while(sinkread < requested) {
        // Read input from buffer.
        int nbufread = parsegraph_Ring_read(input, sink + sinkread, requested - sinkread);
        if(nbufread < 0) {
            // Pass error return value through to caller.
            parsegraph_Ring_putback(input, sinkread);
            return nbufread;
        }
        sinkread += nbufread;

        if(partialRead) {
            // Nothing more to read.
            break;
        }

        // Refill buffer from SSL.
        void* ringBuf;
        size_t slotLen;
        parsegraph_Ring_writeSlot(input, &ringBuf, &slotLen);
        if(slotLen == 0) {
            break;
        }
        int nsslread = cxn->readSource(cxn, ringBuf, slotLen);
        if(nsslread <= 0) {
            if(cxn->shouldDestroy) {
                // Error; put everything back.
                parsegraph_Ring_putbackWrite(input, sinkread + slotLen);
                return nsslread;
            }
            parsegraph_Ring_putbackWrite(input, slotLen);
            return sinkread;
        }
        if(nsslread < slotLen) {
            parsegraph_Ring_putbackWrite(input, slotLen - nsslread);
            partialRead = 1;
        }
    }
    return sinkread;
}

void parsegraph_Connection_putback(parsegraph_Connection* cxn, size_t amount)
{
    return parsegraph_Ring_putback(cxn->input, amount);
}

void parsegraph_Connection_putbackWrite(parsegraph_Connection* cxn, size_t amount)
{
    return parsegraph_Ring_putbackWrite(cxn->output, amount);
}

int parsegraph_Connection_write(parsegraph_Connection* cxn, const char* source, size_t requested)
{
    return parsegraph_Ring_write(cxn->output, source, requested);
}

int parsegraph_Connection_flush(parsegraph_Connection* cxn, int* outnflushed)
{
    void* buf;
    size_t len;

    int nflushed = 0;
    while(1) {
        parsegraph_Ring_readSlot(cxn->output, &buf, &len);
        if(len == 0) {
            break;
        }
        int nsslwritten = cxn->writeSource(cxn, buf, len);
        if(nsslwritten <= 0) {
            if(outnflushed) {
                *outnflushed = nflushed;
            }
            return nsslwritten;
        }
        nflushed += nsslwritten;
        parsegraph_Ring_putback(cxn->output, len - nsslwritten);
        if(nsslwritten < len) {
            // Partial write.
            break;
        }
    }

    if(outnflushed) {
        *outnflushed = nflushed;
    }
    return nflushed;
}

extern void parsegraph_Client_handle(parsegraph_Connection* cxn, int event);
void parsegraph_Connection_handle(parsegraph_Connection* cxn, int event)
{
    parsegraph_Client_handle(cxn, event);
}

void parsegraph_Connection_destroy(parsegraph_Connection* cxn)
{
    if(cxn->destroySource) {
        cxn->destroySource(cxn);
        cxn->destroySource = 0;
    }
    parsegraph_Ring_free(cxn->input);
    parsegraph_Ring_free(cxn->output);
    /* Closing the descriptor will make epoll remove it
     from the set of descriptors which are monitored. */
    free(cxn);
}
