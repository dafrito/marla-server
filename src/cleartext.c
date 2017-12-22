#include "rainback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

static int describeSource(parsegraph_Connection* cxn, char* sink, size_t len)
{
    parsegraph_ClearTextSource* cxnSource = cxn->source;
    memset(sink, 0, len);
    snprintf(sink, len, "FD %d", cxnSource->fd);
    return 0;
}

static int readSource(parsegraph_Connection* cxn, void* sink, size_t len)
{
    parsegraph_ClearTextSource* cxnSource = cxn->source;
    int nsslread = read(cxnSource->fd, sink, len);
    if(nsslread <= 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            cxn->wantsRead = 1;
        }
        else {
            cxn->shouldDestroy = 1;
        }
        return -1;
    }
    return nsslread;
}

static int writeSource(parsegraph_Connection* cxn, void* source, size_t len)
{
    parsegraph_ClearTextSource* cxnSource = cxn->source;
    int nwritten = write(cxnSource->fd, source, len);
    if(nwritten <= 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            cxn->wantsRead = 1;
        }
        else {
            cxn->shouldDestroy = 1;
        }
        return -1;
    }
    return nwritten;
}

static void acceptSource(parsegraph_Connection* cxn)
{
    // Accepted and secured.
    cxn->stage = parsegraph_CLIENT_SECURED;
}

static int shutdownSource(parsegraph_Connection* cxn)
{
    return 0;
}

static void destroySource(parsegraph_Connection* cxn)
{
    parsegraph_ClearTextSource* cxnSource = cxn->source;
    close(cxnSource->fd);
    free(cxn->source);
}

int parsegraph_cleartext_init(parsegraph_Connection* cxn, int fd)
{
    parsegraph_ClearTextSource* source = malloc(sizeof *source);
    cxn->source = source;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->acceptSource = acceptSource;
    cxn->shutdownSource = shutdownSource;
    cxn->destroySource = destroySource;
    cxn->describeSource = describeSource;
    source->fd = fd;
    return 0;
}
