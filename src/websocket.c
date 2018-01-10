#include "marla.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

marla_Connection* marla_WebSocket_connect(int fd)
{
    marla_Connection* cxn = marla_Connection_new(fd);
    cxn->type = marla_ConnectionNature_WEBSOCKET;
    return cxn;
}

void marla_WebSocket_handle(marla_Connection* backend, int event)
{
    if(backend->type != marla_ConnectionNature_WEBSOCKET) {
        return;
    }

    // BACKEND EPOLLOUT: Async write request line, headers, and body to backend.
    // BACKEND EPOLLIN: Async read response line, headers, and body from backend.
}
