#include "rainback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

parsegraph_Connection* parsegraph_Backend_connect(int fd)
{
    parsegraph_Connection* cxn = parsegraph_Connection_new(fd);
    cxn->type = parsegraph_ConnectionNature_BACKEND;
    return cxn;
}

void parsegraph_Backend_handle(parsegraph_Connection* backend, int event)
{
    if(backend->type != parsegraph_ConnectionNature_BACKEND) {
        return;
    }

    // BACKEND EPOLLOUT: Async write request line, headers, and body to backend.
    // BACKEND EPOLLIN: Async read response line, headers, and body from backend.
}
