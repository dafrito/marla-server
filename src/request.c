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

static void default_request_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    int* acceptor;
    char resp[parsegraph_BUFSIZE];
    char buf[parsegraph_BUFSIZE + 1];
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

        const char* message_body = "<!DOCTYPE html><html><body>Hello, <b>world.</b><br/></body></html>";
        const char* header = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        int nwritten = parsegraph_Connection_write(req->cxn, header, strlen(header));
        if(nwritten <= 0) {
            return;
        }

        buf[sizeof(buf) - 1] = 0;
        int cs = 0;
        int message_len = strlen(message_body);
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
            buf[i & (sizeof(buf) - 2)] = message_body[i];
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

parsegraph_ClientRequest* parsegraph_ClientRequest_new(parsegraph_Connection* cxn)
{
    parsegraph_ClientRequest* req = malloc(sizeof(parsegraph_ClientRequest));
    req->cxn = cxn;

    req->handle = default_request_handler;
    req->stage = parsegraph_CLIENT_REQUEST_FRESH;

    // Counters
    req->contentLen = parsegraph_MESSAGE_LENGTH_UNKNOWN;
    req->totalContentLen = 0;
    req->chunkSize = 0;

    // Content
    memset(req->host, 0, sizeof(req->host));
    memset(req->uri, 0, sizeof(req->uri));
    memset(req->method, 0, sizeof(req->method));

    // Flags
    req->expect_continue = 0;
    req->expect_trailer = 0;
    req->close_after_done = 0;

    req->next_request = 0;

    return req;
}

void parsegraph_ClientRequest_destroy(parsegraph_ClientRequest* req)
{
    if(req->handle) {
        req->handle(req, parsegraph_EVENT_DESTROYING, 0, 0);
    }
    free(req);
}

