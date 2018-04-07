#include "marla.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

static int describeSource(marla_Connection* cxn, char* sink, size_t len)
{
    marla_ClearTextSource* cxnSource = cxn->source;
    memset(sink, 0, len);
    snprintf(sink, len, "FD %d", cxnSource->fd);
    return 0;
}

static int readSource(marla_Connection* cxn, void* sink, size_t len)
{
    marla_ClearTextSource* cxnSource = cxn->source;
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

static int writeSource(marla_Connection* cxn, void* source, size_t len)
{
    marla_ClearTextSource* cxnSource = cxn->source;

    //char logbuf[1024];
    //memcpy(logbuf, source, len > 1024 ? 1024 : len);
    //marla_Server_log(cxn->server, logbuf, len > 1024 ? 1024 : len);
    int nwritten = write(cxnSource->fd, source, len);
    if(nwritten <= 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            cxn->wantsWrite = 1;
        }
        else {
            cxn->shouldDestroy = 1;
        }
        return -1;
    }
    return nwritten;
}

static void acceptSource(marla_Connection* cxn)
{
    // Accepted and secured.
    cxn->stage = marla_CLIENT_SECURED;
}

static int shutdownSource(marla_Connection* cxn)
{
    marla_logMessage(cxn->server, "Shutting down cleartext source.");
    marla_ClearTextSource* cxnSource = cxn->source;

    while(!marla_Ring_isEmpty(cxn->output)) {
        int nflushed;
        switch(marla_Connection_flush(cxn, &nflushed)) {
        case marla_WriteResult_CLOSED:
            marla_Ring_clear(cxn->output);
            continue;
        case marla_WriteResult_DOWNSTREAM_CHOKED:
            return -1;
        case marla_WriteResult_UPSTREAM_CHOKED:
            continue;
        }
    }
    int rv = shutdown(cxnSource->fd, SHUT_RDWR);
    marla_logMessagef(cxn->server, "shutdown() returned %d", rv);
    return 1;
}

static void destroySource(marla_Connection* cxn)
{
    marla_logMessage(cxn->server, "Destroying cleartext source.");
    marla_ClearTextSource* cxnSource = cxn->source;
    close(cxnSource->fd);
    free(cxn->source);
}

int marla_cleartext_init(marla_Connection* cxn, int fd)
{
    marla_ClearTextSource* source = malloc(sizeof *source);
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
