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
#include <openssl/rand.h>

void parsegraph_killClientRequest(struct parsegraph_ClientRequest* req, const char* reason, ...)
{
    va_list ap;
    va_start(ap, reason);
    vsnprintf(req->error, sizeof req->error, reason, ap);
    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
    va_end(ap);
}

const char* parsegraph_nameRequestReadStage(enum parsegraph_RequestReadStage stage)
{
    switch(stage) {
    case parsegraph_BACKEND_REQUEST_AWAITING_RESPONSE:
        return "BACKEND_REQUEST_AWAITING_RESPONSE";
    case parsegraph_CLIENT_REQUEST_READ_FRESH:
        return "CLIENT_REQUEST_READ_FRESH";
    case parsegraph_BACKEND_REQUEST_FRESH:
        return "BACKEND_REQUEST_FRESH";
    case parsegraph_CLIENT_REQUEST_READING_METHOD:
        return "CLIENT_REQUEST_READING_METHOD";
    case parsegraph_CLIENT_REQUEST_PAST_METHOD:
        return "CLIENT_REQUEST_PAST_METHOD";
    case parsegraph_CLIENT_REQUEST_READING_REQUEST_TARGET:
        return "CLIENT_REQUEST_READING_REQUEST_TARGET";
    case parsegraph_CLIENT_REQUEST_PAST_REQUEST_TARGET:
        return "CLIENT_REQUEST_PAST_REQUEST_TARGET";
    case parsegraph_CLIENT_REQUEST_READING_VERSION:
        return "CLIENT_REQUEST_READING_VERSION";
    case parsegraph_BACKEND_REQUEST_WRITTEN:
        return "BACKEND_REQUEST_WRITTEN";
    case parsegraph_BACKEND_REQUEST_READING_HEADERS:
        return "BACKEND_REQUEST_READING_HEADERS";
    case parsegraph_CLIENT_REQUEST_READING_FIELD:
        return "CLIENT_REQUEST_READING_FIELD";
    case parsegraph_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE:
        return "CLIENT_REQUEST_AWAITING_CONTINUE_WRITE";
    case parsegraph_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE:
        return "CLIENT_REQUEST_AWAITING_UPGRADE_WRITE";
    case parsegraph_CLIENT_REQUEST_READING_REQUEST_BODY:
        return "CLIENT_REQUEST_READING_REQUEST_BODY";
    case parsegraph_BACKEND_REQUEST_READING_RESPONSE_BODY:
        return "BACKEND_REQUEST_READING_RESPONSE_BODY";
    case parsegraph_CLIENT_REQUEST_READING_CHUNK_SIZE:
        return "CLIENT_REQUEST_READING_CHUNK_SIZE";
    case parsegraph_CLIENT_REQUEST_READING_CHUNK_BODY:
        return "CLIENT_REQUEST_READING_CHUNK_BODY";
    case parsegraph_CLIENT_REQUEST_READING_TRAILER:
        return "CLIENT_REQUEST_READING_TRAILER";
    case parsegraph_BACKEND_REQUEST_READING_RESPONSE_TRAILER:
        return "BACKEND_REQUEST_READING_RESPONSE_TRAILER";
    case parsegraph_BACKEND_REQUEST_RESPONDING:
        return "BACKEND_REQUEST_RESPONDING";
    case parsegraph_CLIENT_REQUEST_WEBSOCKET:
        return "CLIENT_REQUEST_WEBSOCKET";
    case parsegraph_BACKEND_REQUEST_DONE_READING:
        return "BACKEND_REQUEST_DONE_READING";
    case parsegraph_CLIENT_REQUEST_DONE_READING:
        return "CLIENT_REQUEST_DONE_READING";
    }
    return "?";
}

const char* parsegraph_nameRequestWriteStage(enum parsegraph_RequestWriteStage stage)
{
    switch(stage) {

    case parsegraph_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT:
        return "CLIENT_REQUEST_WRITE_AWAITING_ACCEPT";
    case parsegraph_CLIENT_REQUEST_WRITING_CONTINUE:
        return "CLIENT_REQUEST_WRITING_CONTINUE";
    case parsegraph_CLIENT_REQUEST_WRITING_UPGRADE:
        return "CLIENT_REQUEST_WRITING_UPGRADE";
    case parsegraph_CLIENT_REQUEST_DONE_WRITING:
        return "CLIENT_REQUEST_DONE_WRITING";
    case parsegraph_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE:
        return "CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE";
    case parsegraph_CLIENT_REQUEST_WRITING_RESPONSE:
        return "CLIENT_REQUEST_WRITING_RESPONSE";
    }
    return "?";
}

int parsegraph_ClientRequest_NEXT_ID = 1;

parsegraph_ClientRequest* parsegraph_ClientRequest_new(parsegraph_Connection* cxn)
{
    parsegraph_ClientRequest* req = malloc(sizeof(parsegraph_ClientRequest));
    req->cxn = cxn;
    req->statusCode = 0;
    memset(req->statusLine, 0, sizeof req->statusLine);

    req->id = parsegraph_ClientRequest_NEXT_ID++;

    req->handle = 0;
    req->handleData = 0;
    req->readStage = parsegraph_CLIENT_REQUEST_READ_FRESH;
    req->writeStage = parsegraph_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT;

    // Counters
    req->contentLen = parsegraph_MESSAGE_LENGTH_UNKNOWN;
    req->totalContentLen = 0;
    req->chunkSize = 0;

    // Content
    memset(req->error, 0, sizeof req->error);
    memset(req->host, 0, sizeof(req->host));
    memset(req->uri, 0, sizeof(req->uri));
    memset(req->method, 0, sizeof(req->method));
    memset(req->contentType, 0, sizeof(req->contentType));
    memset(req->websocket_nonce, 0, sizeof(req->websocket_nonce));
    memset(req->websocket_accept, 0, sizeof(req->websocket_accept));
    memset(req->websocket_frame, 0, sizeof(req->websocket_frame));
    memset(req->websocket_ping, 0, sizeof req->websocket_ping);
    req->websocket_pongLen = 0;
    memset(req->websocket_pong, 0, sizeof req->websocket_pong);
    memset(req->websocket_closeReason, 0, sizeof req->websocket_closeReason);
    req->websocket_closeReasonLen = 0;

    req->websocket_type = -1;
    req->websocket_fin = 0;
    req->needWebSocketClose = 0;
    req->doingPong = 0;
    req->doingWebSocketClose = 0;
    req->websocketFrameWritten = 0;
    req->websocketFrameLen = 0;
    req->websocketFrameRead = 0;
    req->websocketFrameOutLen = 0;
    memset(req->websocketOutMask, 0, sizeof req->websocketOutMask);
    memset(req->websocketMask, 0, sizeof req->websocketMask);
    req->expect_continue = 0;

    req->websocket_version = 13;

    // Flags
    req->expect_continue = 0;
    req->expect_trailer = 0;
    req->expect_upgrade = 0;
    req->expect_websocket = 0;
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
