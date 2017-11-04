#include "rainback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

int parsegraph_Connection_read(parsegraph_Connection* cxn, char* sink, size_t requested)
{
    parsegraph_Ring* const input = cxn->nature.client.input;
    SSL* ssl = cxn->nature.client.ssl;

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
        int nsslread = SSL_read(ssl, ringBuf, slotLen);
        if(nsslread <= 0) {
            switch(SSL_get_error(ssl, nsslread)) {
            case SSL_ERROR_NONE:
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                // Nothing.
                parsegraph_Ring_putbackWrite(input, slotLen);
                return sinkread;
            default:
                // Error; put everything back.
                parsegraph_Ring_putbackWrite(input, sinkread + slotLen);
                return nsslread;
            }
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
    return parsegraph_Ring_putback(cxn->nature.client.input, amount);
}

void parsegraph_Connection_putbackWrite(parsegraph_Connection* cxn, size_t amount)
{
    return parsegraph_Ring_putbackWrite(cxn->nature.client.output, amount);
}

int parsegraph_Connection_write(parsegraph_Connection* cxn, const char* source, size_t requested)
{
    return parsegraph_Ring_write(cxn->nature.client.output, source, requested);
}

int parsegraph_Connection_flush(parsegraph_Connection* cxn, int* outnflushed)
{
    void* buf;
    size_t len;

    int nflushed = 0;
    while(1) {
        parsegraph_Ring_readSlot(cxn->nature.client.output, &buf, &len);
        if(len == 0) {
            break;
        }
        int nsslwritten = SSL_write(cxn->nature.client.ssl, buf, len);
        if(nsslwritten <= 0) {
            if(outnflushed) {
                *outnflushed = nflushed;
            }
            return nsslwritten;
        }
        nflushed += nsslwritten;
        parsegraph_Ring_putback(cxn->nature.client.output, len - nsslwritten);
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

parsegraph_Connection* parsegraph_Connection_new()
{
    parsegraph_Connection* cxn = (parsegraph_Connection*)malloc(sizeof(*cxn));
    if(!cxn) {
        return 0;
    }

    // Set the type to unknown.
    cxn->type = parsegraph_ConnectionNature_UNKNOWN;

    // Initialize flags.
    cxn->shouldDestroy = 0;
    cxn->wantsWrite = 0;
    cxn->wantsRead = 0;

    return cxn;
}

extern void parsegraph_Client_handle(parsegraph_Connection* cxn, int event);
//extern void parsegraph_Backend_handle(parsegraph_Connection* cxn, int event);
void parsegraph_Connection_handle(parsegraph_Connection* cxn, int event)
{
    switch(cxn->type) {
    case parsegraph_ConnectionNature_CLIENT:
        parsegraph_Client_handle(cxn, event);
        break;
    case parsegraph_ConnectionNature_BACKEND:
        //parsegraph_Backend_handle(cxn, event);
        break;
    case parsegraph_ConnectionNature_UNKNOWN:
        return;
    }
}

void parsegraph_Connection_destroy(parsegraph_Connection* cxn)
{
    switch(cxn->type) {
    case parsegraph_ConnectionNature_CLIENT:
        parsegraph_Ring_free(cxn->nature.client.input);
        parsegraph_Ring_free(cxn->nature.client.output);
        SSL_free(cxn->nature.client.ssl);
        close(cxn->nature.client.fd);
        break;
    case parsegraph_ConnectionNature_BACKEND:
        break;
    case parsegraph_ConnectionNature_UNKNOWN:
        break;
    }

    /* Closing the descriptor will make epoll remove it
     from the set of descriptors which are monitored. */
    free(cxn);
}
