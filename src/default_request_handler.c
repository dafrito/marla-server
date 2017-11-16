#include "rainback.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

static void default_request_handler_func(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    int* acceptor;
    char resp[parsegraph_BUFSIZE];
    char buf[parsegraph_BUFSIZE + 1];
    memset(buf, 0, sizeof buf);
    switch(ev) {
    case parsegraph_EVENT_HEADER:
        break;
    case parsegraph_EVENT_ACCEPTING_REQUEST:
        acceptor = data;
        *acceptor = 1;
        break;
    case parsegraph_EVENT_REQUEST_BODY:
        break;
    case parsegraph_EVENT_RESPOND:
        fprintf(stderr, "Responding...\n");
        memset(resp, 0, sizeof(resp));

        const char* header = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        int nwritten = parsegraph_Connection_write(req->cxn, header, strlen(header));
        if(nwritten <= 0) {
            return;
        }
        int cs = 0;
        int message_len = snprintf(buf, sizeof buf, "<!DOCTYPE html><html><head><script>function run() { WS=new WebSocket(\"wss://localhost:4434/\"); WS.onopen = function() { alert('notime'); }; }</script></head><body onload='run()'>Hello, <b>world.</b><p>This is request %d</body></html>", req->id);
        for(int i = 0; i <= message_len; ++i) {
            if((i == message_len) || (i && !(i & (sizeof(buf) - 2)))) {
                if(i & (sizeof(buf) - 2)) {
                    buf[i & (sizeof(buf) - 2)] = 0;
                }
                int rv = snprintf(resp, 1023, "%x\r\n", cs);
                if(rv < 0) {
                    dprintf(3, "Failed to generate response.");
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                memcpy(resp + rv, buf, cs);
                resp[rv + cs] = '\r';
                resp[rv + cs + 1] = '\n';

                int nwritten = parsegraph_Connection_write(req->cxn, resp, rv + cs + 2);
                if(nwritten <= 0) {
                    return;
                }
                cs = 0;
            }
            if(i == message_len) {
                break;
            }
            buf[i & (sizeof(buf) - 2)] = buf[i];
            ++cs;
        }

        nwritten = parsegraph_Connection_write(req->cxn, "0\r\n\r\n", 5);
        if(nwritten <= 0) {
            return;
        }

        // Mark connection as complete.
        req->stage = parsegraph_CLIENT_REQUEST_DONE;
        break;
    case parsegraph_EVENT_DESTROYING:
        break;
    }
}

void (*default_request_handler)(struct parsegraph_ClientRequest*, enum parsegraph_ClientEvent, void*, int) = default_request_handler_func;
