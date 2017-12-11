#include "rainback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

static int readSource(parsegraph_Connection* cxn, void* sink, size_t len)
{
    parsegraph_BackendSource* cxnSource = cxn->source;
    return read(cxnSource->fd, sink, len);
}

static int writeSource(parsegraph_Connection* cxn, void* source, size_t len)
{
    parsegraph_BackendSource* cxnSource = cxn->source;
    return write(cxnSource->fd, source, len);
}

static void acceptSource(parsegraph_Connection* cxn)
{
    // Accepted and secured.
    cxn->stage = parsegraph_BACKEND_READY;
}

static int shutdownSource(parsegraph_Connection* cxn)
{
    return 1;
}

static void destroySource(parsegraph_Connection* cxn)
{
    parsegraph_BackendSource* source = cxn->source;
    close(source->fd);
    free(source);

    cxn->source = 0;
    cxn->readSource = 0;
    cxn->writeSource = 0;
    cxn->acceptSource = 0;
    cxn->shutdownSource = 0;
    cxn->destroySource = 0;
}

void parsegraph_Backend_init(parsegraph_Connection* cxn, int fd)
{
    parsegraph_BackendSource* source = malloc(sizeof *source);
    cxn->source = source;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->acceptSource = acceptSource;
    cxn->shutdownSource = shutdownSource;
    cxn->destroySource = destroySource;
    source->fd = fd;
}

void parsegraph_Backend_enqueue(parsegraph_Connection* cxn, parsegraph_ClientRequest* req)
{
    req->cxn = cxn;
    req->stage = parsegraph_BACKEND_REQUEST_FRESH;

    if(!cxn->current_request) {
        cxn->current_request = req;
        cxn->latest_request = req;
    }
    else {
        cxn->latest_request->next_request = req;
        cxn->latest_request = req;
    }
}

void parsegraph_backendWrite(parsegraph_Connection* cxn)
{
    // BACKEND EPOLLOUT: Write request line, headers, and body to backend.
}

void parsegraph_backendRead(parsegraph_Connection* cxn)
{
    // BACKEND EPOLLIN: Async read response line, headers, and body from backend.
}
