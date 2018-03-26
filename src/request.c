#include "marla.h"
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

int marla_Request_numKilled = 0;

void marla_killRequest(struct marla_Request* req, const char* reason, ...)
{
    va_list ap;
    va_start(ap, reason);
    vsnprintf(req->error, sizeof req->error, reason, ap);
    if(req->writeStage > marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT || req->readStage < marla_CLIENT_REQUEST_DONE_READING) {
        marla_logMessagef(req->cxn->server, "Killing request and ending connection: %s.", req->error);
        req->cxn->stage = marla_CLIENT_COMPLETE;
    }
    else {
        marla_logMessagef(req->cxn->server, "Killing request: %s.", req->error);
        if(req->cxn->server->undertaker) {
            req->cxn->server->undertaker(req);
        }
    }
    //fprintf(stderr, "Killing request: %s\n", req->error);
    va_end(ap);
    ++marla_Request_numKilled;
}

void marla_dumpRequest(marla_Request* req)
{
    fprintf(stderr, "Request %d (%s, %s)\n", req->id, marla_nameRequestReadStage(req->readStage), marla_nameRequestWriteStage(req->writeStage));
}

const char* marla_nameRequestReadStage(enum marla_RequestReadStage stage)
{
    switch(stage) {
    case marla_CLIENT_REQUEST_READ_FRESH:
        return "CLIENT_REQUEST_READ_FRESH";
    case marla_BACKEND_REQUEST_READING_RESPONSE_LINE:
        return "BACKEND_REQUEST_READING_RESPONSE_LINE";
    case marla_BACKEND_REQUEST_FRESH:
        return "BACKEND_REQUEST_FRESH";
    case marla_CLIENT_REQUEST_READING_METHOD:
        return "CLIENT_REQUEST_READING_METHOD";
    case marla_CLIENT_REQUEST_PAST_METHOD:
        return "CLIENT_REQUEST_PAST_METHOD";
    case marla_CLIENT_REQUEST_READING_REQUEST_TARGET:
        return "CLIENT_REQUEST_READING_REQUEST_TARGET";
    case marla_CLIENT_REQUEST_PAST_REQUEST_TARGET:
        return "CLIENT_REQUEST_PAST_REQUEST_TARGET";
    case marla_CLIENT_REQUEST_READING_VERSION:
        return "CLIENT_REQUEST_READING_VERSION";
    case marla_BACKEND_REQUEST_READING_HEADERS:
        return "BACKEND_REQUEST_READING_HEADERS";
    case marla_CLIENT_REQUEST_READING_FIELD:
        return "CLIENT_REQUEST_READING_FIELD";
    case marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE:
        return "CLIENT_REQUEST_AWAITING_CONTINUE_WRITE";
    case marla_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE:
        return "CLIENT_REQUEST_AWAITING_UPGRADE_WRITE";
    case marla_CLIENT_REQUEST_READING_REQUEST_BODY:
        return "CLIENT_REQUEST_READING_REQUEST_BODY";
    case marla_BACKEND_REQUEST_READING_RESPONSE_BODY:
        return "BACKEND_REQUEST_READING_RESPONSE_BODY";
    case marla_CLIENT_REQUEST_READING_CHUNK_SIZE:
        return "CLIENT_REQUEST_READING_CHUNK_SIZE";
    case marla_CLIENT_REQUEST_READING_CHUNK_BODY:
        return "CLIENT_REQUEST_READING_CHUNK_BODY";
    case marla_BACKEND_REQUEST_READING_CHUNK_SIZE:
        return "BACKEND_REQUEST_READING_CHUNK_SIZE";
    case marla_BACKEND_REQUEST_READING_CHUNK_BODY:
        return "BACKEND_REQUEST_READING_CHUNK_BODY";
    case marla_CLIENT_REQUEST_WEBSOCKET:
        return "CLIENT_REQUEST_WEBSOCKET";
    case marla_BACKEND_REQUEST_AFTER_RESPONSE:
        return "BACKEND_REQUEST_AFTER_RESPONSE";
    case marla_BACKEND_REQUEST_READING_RESPONSE_TRAILERS:
        return "BACKEND_REQUEST_READING_RESPONSE_TRAILERS";
    case marla_BACKEND_REQUEST_DONE_READING:
        return "BACKEND_REQUEST_DONE_READING";
    case marla_CLIENT_REQUEST_DONE_READING:
        return "CLIENT_REQUEST_DONE_READING";
    }
    return "?";
}

const char* marla_nameRequestWriteStage(enum marla_RequestWriteStage stage)
{
    switch(stage) {
    case marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT:
        return "CLIENT_REQUEST_WRITE_AWAITING_ACCEPT";
    case marla_CLIENT_REQUEST_WRITING_CONTINUE:
        return "CLIENT_REQUEST_WRITING_CONTINUE";
    case marla_CLIENT_REQUEST_WRITING_UPGRADE:
        return "CLIENT_REQUEST_WRITING_UPGRADE";
    case marla_CLIENT_REQUEST_DONE_WRITING:
        return "CLIENT_REQUEST_DONE_WRITING";
    case marla_CLIENT_REQUEST_AFTER_RESPONSE:
        return "CLIENT_REQUEST_AFTER_RESPONSE";
    case marla_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE:
        return "CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE";
    case marla_CLIENT_REQUEST_WRITING_RESPONSE:
        return "CLIENT_REQUEST_WRITING_RESPONSE";
    case marla_BACKEND_REQUEST_WRITING_HEADERS:
        return "BACKEND_REQUEST_WRITING_HEADERS";
    case marla_BACKEND_REQUEST_WRITING_REQUEST_LINE:
        return "BACKEND_REQUEST_WRITING_REQUEST_LINE";
    case marla_BACKEND_REQUEST_WRITING_REQUEST_BODY:
        return "BACKEND_REQUEST_WRITING_REQUEST_BODY";
    case marla_CLIENT_REQUEST_WRITING_TRAILERS:
        return "CLIENT_REQUEST_WRITING_TRAILERS";
    case marla_BACKEND_REQUEST_DONE_WRITING:
        return "BACKEND_REQUEST_DONE_WRITING";
    }
    return "?";
}

int marla_Request_NEXT_ID = 1;

marla_Request* marla_Request_new(marla_Connection* cxn)
{
    marla_Request* req = malloc(sizeof(marla_Request));
    req->cxn = cxn;
    req->statusCode = 0;
    memset(req->statusLine, 0, sizeof req->statusLine);

    req->refs = 1;

    req->id = marla_Request_NEXT_ID++;

    req->handler = 0;
    req->handlerData = 0;
    req->readStage = marla_CLIENT_REQUEST_READ_FRESH;
    req->writeStage = marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT;

    // Counters
    req->requestLen = marla_MESSAGE_LENGTH_UNKNOWN;
    req->responseLen = marla_MESSAGE_LENGTH_UNKNOWN;
    req->remainingResponseLen = 0;
    req->remainingContentLen = 0;
    req->totalContentLen = 0;
    req->chunkSize = 0;
    req->lastReadIndex = 0;

    // Content
    memset(req->error, 0, sizeof req->error);
    memset(req->host, 0, sizeof(req->host));
    memset(req->uri, 0, sizeof(req->uri));
    memset(req->method, 0, sizeof(req->method));
    memset(req->contentType, 0, sizeof(req->contentType));
    memset(req->redirectLocation, 0, sizeof req->redirectLocation);
    memset(req->acceptHeader, 0, sizeof req->acceptHeader);
    memset(req->cookieHeader, 0, sizeof req->cookieHeader);
    memset(req->setCookieHeader, 0, sizeof req->setCookieHeader);
    memset(req->websocket_nonce, 0, sizeof(req->websocket_nonce));
    memset(req->websocket_accept, 0, sizeof(req->websocket_accept));
    memset(req->websocket_frame, 0, sizeof(req->websocket_frame));
    memset(req->websocket_ping, 0, sizeof req->websocket_ping);
    req->websocket_pingLen = 0;
    req->websocket_pongLen = 0;
    memset(req->websocket_pong, 0, sizeof req->websocket_pong);
    memset(req->websocket_closeReason, 0, sizeof req->websocket_closeReason);
    req->websocket_closeCode = 0;
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
    req->connection_indicates_trailer = 0;
    req->is_backend = 0;
    req->backendPeer = 0;
    req->expect_continue = 0;
    req->expect_trailer = 0;
    req->expect_upgrade = 0;
    req->expect_websocket = 0;
    req->close_after_done = 0;

    req->next_request = 0;

    return req;
}

void marla_Request_ref(marla_Request* req)
{
    ++req->refs;
}

void marla_Request_unref(marla_Request* req)
{
    --req->refs;
    if(req->refs > 0) {
        return;
    }
    if(req->refs < 0) {
        fprintf(stderr, "Already destroyed\n");
        abort();
    }

    // Destroy.
    if(req->handler) {
        req->handler(req, marla_EVENT_DESTROYING, 0, 0);
    }
    if(req->error[0] != 0) {
        marla_logMessagef(req->cxn->server, "Destroying request %d with error %s", req->id, req->error);
    }
    else {
        marla_logMessagef(req->cxn->server, "Destroying request %d", req->id);
    }
    if(req->backendPeer) {
        req->backendPeer->backendPeer = 0;
        if(!req->cxn->is_backend && req->backendPeer) {
            marla_Request_unref(req->backendPeer);
        }
    }
    free(req);
}
