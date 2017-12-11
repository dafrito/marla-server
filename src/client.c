#include "rainback.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/sha.h>

int parsegraph_ClientRequest_NEXT_ID = 1;

parsegraph_ClientRequest* parsegraph_ClientRequest_new(parsegraph_Connection* cxn, struct parsegraph_Server* server)
{
    parsegraph_ClientRequest* req = malloc(sizeof(parsegraph_ClientRequest));
    req->cxn = cxn;
    req->server = server;

    req->id = parsegraph_ClientRequest_NEXT_ID++;

    req->handle = default_request_handler;
    req->stage = parsegraph_CLIENT_REQUEST_FRESH;

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

static void parsegraph_clientRead(parsegraph_Connection* cxn, struct parsegraph_Server* server);
static void parsegraph_clientWrite(parsegraph_Connection* cxn, struct parsegraph_Server* server);

void parsegraph_Client_handle(parsegraph_Connection* cxn, struct parsegraph_Server* server, int event)
{
    if(cxn->stage == parsegraph_CLIENT_ACCEPTED) {
        // Client has just connected.
        if(cxn->acceptSource) {
            cxn->acceptSource(cxn);
            if(cxn->stage != parsegraph_CLIENT_SECURED) {
                return;
            }
        }
        else {
            cxn->stage = parsegraph_CLIENT_SECURED;
        }
    }

    // Read in requests.
    while(cxn->stage == parsegraph_CLIENT_SECURED) {
        char c;
        int nread = parsegraph_Connection_read(cxn, &c, 1);
        if(nread <= 0) {
            parsegraph_clientWrite(cxn, server);
            break;
        }
        else if(nread > 0) {
            parsegraph_Connection_putback(cxn, 1);
        }

        parsegraph_clientRead(cxn, server);
        parsegraph_clientWrite(cxn, server);
    }

    // Read in backend requests.
    while(cxn->stage == parsegraph_BACKEND_READY) {
        if(!cxn->current_request) {
            break;
        }

        parsegraph_backendWrite(cxn);
        parsegraph_backendRead(cxn);
    }

    if(cxn->stage == parsegraph_CLIENT_COMPLETE) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
        return;
    }
}

static void parsegraph_clientRead(parsegraph_Connection* cxn, struct parsegraph_Server* server)
{
    parsegraph_ClientRequest* req = 0;
    if(!cxn->current_request) {
        // No request yet made.
        req = parsegraph_ClientRequest_new(cxn, server);
        cxn->current_request = req;
        cxn->latest_request = req;
        ++cxn->requests_in_process;
    }
    else {
        req = cxn->latest_request;
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_FRESH) {
        while(1) {
            char c;
            int nread = parsegraph_Connection_read(cxn, &c, 1);
            if(nread <= 0) {
                return;
            }
            if(c != '\r' && c != '\n') {
                parsegraph_Connection_putback(cxn, 1);
                req->stage = parsegraph_CLIENT_REQUEST_READING_METHOD;
                break;
            }
        }
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_READING_METHOD) {
        memset(req->method, 0, sizeof(req->method));
        int nread = parsegraph_Connection_read(cxn, req->method, MAX_METHOD_LENGTH + 1);
        if(nread <= 0) {
            // Error.
            return;
        }
        if(nread < MIN_METHOD_LENGTH + 1) {
            // Incomplete.
            parsegraph_Connection_putback(cxn, nread);
            return;
        }

        // Validate the given method.
        int foundSpace = 0;
        for(int i = 0; i < nread; ++i) {
            char c = req->method[i];
            if(c <= 0x1f || c == 0x7f) {
                snprintf(req->error, sizeof req->error, "Request line contains control characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                snprintf(req->error, sizeof req->error, "Request line contains delimiters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                snprintf(req->error, sizeof req->error, "Request line contains unwise characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(c == ' ') {
                parsegraph_Connection_putback(cxn, nread - i);
                req->method[i] = 0;
                foundSpace = 1;
                break;
            }
            if(!isascii(c)) {
                snprintf(req->error, sizeof req->error, "Request method contains non-ASCII characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
        }
        if(nread == MAX_METHOD_LENGTH + 1 && !foundSpace) {
            snprintf(req->error, sizeof req->error, "Request method is too long, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(!foundSpace) {
            // No space found in the fragment found, so incomplete read.
            return;
        }

        req->stage = parsegraph_CLIENT_REQUEST_PAST_METHOD;

        if(!strcmp(req->method, "GET")) {

        }
        else if(!strcmp(req->method, "HEAD")) {

        }
        else if(!strcmp(req->method, "POST")) {

        }
        else if(!strcmp(req->method, "PUT")) {

        }
        else if(!strcmp(req->method, "DELETE")) {

        }
        else if(!strcmp(req->method, "CONNECT")) {

        }
        else if(!strcmp(req->method, "OPTIONS")) {

        }
        else if(!strcmp(req->method, "TRACE")) {
            // A client MUST NOT send a message body in a TRACE request.
        }
        else {
            snprintf(req->error, sizeof req->error, "Request method '%s' is unknown, so no valid request.\n", req->method);
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }

        //printf("Found method: %s\n", req->method);
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_PAST_METHOD) {
        while(1) {
            char c;
            int nread = parsegraph_Connection_read(cxn, &c, 1);
            if(nread <= 0) {
                return;
            }
            if(c != ' ') {
                parsegraph_Connection_putback(cxn, 1);
                req->stage = parsegraph_CLIENT_REQUEST_READING_REQUEST_TARGET;
                break;
            }
        }
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_READING_REQUEST_TARGET) {
        memset(req->uri, 0, sizeof(req->uri));
        int nread = parsegraph_Connection_read(cxn, req->uri, MAX_URI_LENGTH + 1);
        if(nread <= 0) {
            // Error.
            return;
        }
        if(nread < MIN_METHOD_LENGTH + 1) {
            // Incomplete.
            parsegraph_Connection_putback(cxn, nread);
            return;
        }

        // Validate the given method.
        int foundSpace = 0;
        for(int i = 0; i < nread; ++i) {
            char c = req->uri[i];
            if(c <= 0x1f || c == 0x7f) {
                snprintf(req->error, sizeof req->error, "Request target contains control characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                snprintf(req->error, sizeof req->error, "Request target contains delimiters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(c == ' ') {
                parsegraph_Connection_putback(cxn, nread - i);
                req->uri[i] = 0;
                foundSpace = 1;
                break;
            }
        }
        if(nread == MAX_URI_LENGTH + 1 && !foundSpace) {
            snprintf(req->error, sizeof req->error, "Request target is too long, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(!foundSpace) {
            // No space found in the fragment found, so incomplete read.
            return;
        }

        //printf("Found URI: %s\n", req->uri);

        req->stage = parsegraph_CLIENT_REQUEST_PAST_REQUEST_TARGET;
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_PAST_REQUEST_TARGET) {
        while(1) {
            char c;
            int nread = parsegraph_Connection_read(cxn, &c, 1);
            if(nread <= 0) {
                return;
            }
            if(c != ' ') {
                parsegraph_Connection_putback(cxn, 1);
                req->stage = parsegraph_CLIENT_REQUEST_READING_VERSION;
                break;
            }
        }
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_READING_VERSION) {
        //                     "01234567"
        const char* expected = "HTTP/1.1";
        char givenVersion[10];
        memset(givenVersion, 0, sizeof(givenVersion));
        int nread = parsegraph_Connection_read(cxn, givenVersion, sizeof(givenVersion));
        if(nread <= 0) {
            // Error.
            return;
        }
        if(nread < 10) {
            // Incomplete.
            parsegraph_Connection_putback(cxn, nread);
            return;
        }

        // Validate.
        int len = strlen(expected);
        for(int i = 0; i < len; ++i) {
            if(givenVersion[i] != expected[i]) {
                snprintf(req->error, sizeof req->error, "Request version is unknown, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
        }

        if(givenVersion[len] == '\n') {
            parsegraph_Connection_putback(cxn, 1);
        }
        else if(givenVersion[len] != '\r' || givenVersion[len + 1] != '\n') {
            snprintf(req->error, sizeof req->error, "Request version is unknown, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }

        parsegraph_Server_invokeHook(req->server, parsegraph_SERVER_HOOK_ROUTE, req);

        req->stage = parsegraph_CLIENT_REQUEST_READING_FIELD;
    }

    while(req->stage == parsegraph_CLIENT_REQUEST_READING_FIELD) {
        char fieldLine[MAX_FIELD_NAME_LENGTH + 2 + MAX_FIELD_VALUE_LENGTH + 2];
        memset(fieldLine, 0, sizeof(fieldLine));
        int nread = parsegraph_Connection_read(cxn, fieldLine, sizeof(fieldLine));
        if(nread <= 0) {
            // Error.
            return;
        }

        // Validate.
        int foundNewline = 0;
        int foundSeparator = 0;
        int toleratingSpaces = 0;
        char* fieldValue = 0;
        for(int i = 0; i < nread; ++i) {
            if(fieldLine[i] == '\n') {
                fieldLine[i] = 0;
                foundNewline = 1;
                parsegraph_Connection_putback(cxn, nread - i - 1);
                break;
            }
            if(i < nread - 1 && fieldLine[i] == '\r' && fieldLine[i + 1] == '\n') {
                fieldLine[i] = 0;
                foundNewline = 1;
                parsegraph_Connection_putback(cxn, nread - i - 2);
                break;
            }
            char c = fieldLine[i];
            if(c <= 0x1f || c == 0x7f) {
                snprintf(req->error, sizeof req->error, "Header line contains control characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%' || c == '"')) {
                snprintf(req->error, sizeof req->error, "Header name contains delimiters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                snprintf(req->error, sizeof req->error, "Header name contains unwise characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && !isalnum(c) && c != '-') {
                if(c == ':' && i > 0 && i < nread - 1) {
                    foundSeparator = i;
                    fieldLine[i] = 0;
                    toleratingSpaces = 1;
                    continue;
                }
                snprintf(req->error, sizeof req->error, "Header name contains non alphanumeric characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(foundSeparator && toleratingSpaces) {
                if(c == ' ') {
                    continue;
                }
                toleratingSpaces = 0;
                fieldValue = fieldLine + i;
            }
        }

        // Validate.
        if(!foundNewline && nread == sizeof(fieldLine)) {
            snprintf(req->error, sizeof req->error, "Request version is too long, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(!foundNewline) {
            // Incomplete;
            return;
        }
        if(foundSeparator && fieldValue) {
            // Header found.
            char* fieldName = fieldLine;
            //fprintf(stderr, "HEADER: %s = %s\n", fieldName, fieldValue);

            if(!strcasecmp(fieldName, "Content-Length")) {
                if(req->contentLen != -2) {
                    snprintf(req->error, sizeof req->error, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                char* endptr;
                long int x = strtol(fieldValue, &endptr, 10);
                if(*endptr != '\0' || x < 0) {
                    snprintf(req->error, sizeof req->error, "Content-Length header value could not be read, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                req->contentLen = x;
            }
            else if(!strcasecmp(fieldName, "Host")) {
                memset(req->host, 0, sizeof(req->host));
                strncpy(req->host, fieldValue, sizeof(req->host) - 1);
            }
            else if(!strcasecmp(fieldName, "Transfer-Encoding")) {
                if(req->contentLen != -2) {
                    snprintf(req->error, sizeof req->error, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }

                if(!strcasecmp(fieldValue, "chunked")) {
                    req->contentLen = parsegraph_MESSAGE_IS_CHUNKED;
                }
            }
            else if(!strcasecmp(fieldName, "Connection")) {
                char* sp;
                char* fieldToken = strtok_r(fieldValue, ", ", &sp);
                if(!fieldToken) {
                    fieldToken = fieldValue;
                }
                else while(fieldToken) {
                    if(!strcasecmp(fieldToken, "close")) {
                        req->contentLen = parsegraph_MESSAGE_USES_CLOSE;
                        req->close_after_done = 1;
                    }
                    else if(!strcasecmp(fieldToken, "Upgrade")) {
                        req->expect_upgrade = 1;
                    }
                    else if(strcasecmp(fieldToken, "keep-alive")) {
                        snprintf(req->error, sizeof req->error, "Connection is not understood, so no valid request.\n");
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    fieldToken = strtok_r(0, ", ", &sp);
                }
            }
            else if(!strcmp(fieldName, "Trailer")) {

            }
            else if(!strcmp(fieldName, "TE")) {

            }
            else if(!strcmp(fieldName, "Range")) {

            }
            else if(!strcmp(fieldName, "If-Unmodified-Since")) {

            }
            else if(!strcmp(fieldName, "If-Range")) {

            }
            else if(!strcmp(fieldName, "If-None-Match")) {

            }
            else if(!strcmp(fieldName, "If-Modified-Since")) {

            }
            else if(!strcmp(fieldName, "If-Match")) {

            }
            else if(!strcmp(fieldName, "Expect")) {
                if(!strcmp(fieldValue, "100-continue")) {
                    req->expect_continue = 1;
                }
            }
            else if(!strcmp(fieldName, "Content-Type")) {
                strncpy(req->contentType, fieldValue, MAX_FIELD_VALUE_LENGTH);
            }
            else if(!strcmp(fieldName, "Accept-Language")) {

            }
            else if(!strcmp(fieldName, "Accept-Encoding")) {

            }
            else if(!strcmp(fieldName, "Accept-Charset")) {

            }
            else if(!strcmp(fieldName, "Sec-WebSocket-Key")) {
                strncpy(req->websocket_nonce, fieldValue, MAX_WEBSOCKET_NONCE_LENGTH);
            }
            else if(!strcmp(fieldName, "Sec-WebSocket-Version")) {
                if(!strcmp(fieldValue, "13")) {
                    req->websocket_version = 13;
                }
            }
            else if(!strcmp(fieldName, "Accept")) {

            }
            else if(!strcmp(fieldName, "Upgrade")) {
                if(!strcmp(fieldValue, "websocket")) {
                    req->expect_websocket = 1;
                }
            }
            else {
                req->handle(req, parsegraph_EVENT_HEADER, fieldName, fieldValue - fieldName);
            }

            continue;
        }
        else if(fieldLine[0] == 0) {
            // Empty line. End of request headers.

            // Determine target URI.
            if(req->uri[0] == '/') {
                // Origin form.

                if(req->host[0] == 0) {
                    // No Host sent.
                    printf("No Host provided.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
            }
            else if(isascii(req->uri[0])) {
                char* schemeSep = strstr(req->uri, "://");
                if(schemeSep != 0) {
                    for(char* c = req->uri; c != schemeSep; ++c) {
                        if(!isascii(*c)) {
                            snprintf(req->error, sizeof req->error, "Scheme invalid, so no valid request.\n");
                            cxn->stage = parsegraph_CLIENT_COMPLETE;
                            return;
                        }
                    }
                    *schemeSep = 0;
                    if(!strcmp("http", req->uri)) {
                        snprintf(req->error, sizeof req->error, "HTTP scheme unsupported, so no valid request.\n");
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    else if(!strcmp("https", req->uri)) {
                        *schemeSep = ':';
                    }
                    else {
                        snprintf(req->error, sizeof req->error, "Request scheme unrecognized.\n");
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                }
                char* hostPart = schemeSep + 3;
                char* hostSep = strstr(hostPart, "/");
                if(hostSep - hostPart >= MAX_FIELD_VALUE_LENGTH) {
                    snprintf(req->error, sizeof req->error, "Host too long.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }

                if(hostSep == 0) {
                    // GET https://localhost
                    strncpy(req->host, hostPart, MAX_FIELD_VALUE_LENGTH);
                }
                else {
                    // GET https://localhost/absolute/path?query
                    *hostSep = 0;
                    if(req->host[0] != 0 && strcmp(req->host, hostPart)) {
                        snprintf(req->error, sizeof req->error, "Host differs from absolute URI's host.\n");
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    strncpy(req->host, hostPart, MAX_FIELD_VALUE_LENGTH);
                    *hostSep = '/';

                    // Transform an absolute URI into a origin form
                    memmove(req->uri, hostSep, strlen(hostSep));
                }
            }
            else {
                snprintf(req->error, sizeof req->error, "Request target unrecognized.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }

            if(req->expect_upgrade && req->expect_websocket && req->websocket_nonce[0] != 0 && req->websocket_version == 13)
            {
                // Test Websocket nonce.
                unsigned char buf[MAX_FIELD_VALUE_LENGTH + 32 + 1];
                memset(buf, 0, sizeof(buf));
                strcpy(buf, req->websocket_nonce);
                strcat(buf, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
                unsigned char digest[SHA_DIGEST_LENGTH];
                memset(digest, 0, sizeof(digest));
                SHA1(buf, strlen(buf), digest);

                BIO* mem = BIO_new(BIO_s_mem());
                BIO* b64 = BIO_new(BIO_f_base64());
                BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
                BIO_push(b64, mem);
                BIO_write(b64, digest, SHA_DIGEST_LENGTH);
                BIO_flush(b64);
                BUF_MEM* bptr;
                BIO_get_mem_ptr(b64, &bptr);

                char* buff = (char*)malloc(bptr->length);
                memcpy(req->websocket_accept, bptr->data, bptr->length);
                req->websocket_accept[bptr->length] = 0;
                BIO_free_all(b64);

                req->stage = parsegraph_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE;
            }
            else {
                req->expect_upgrade = 0;
                req->expect_websocket = 0;

                int accept = 1;
                req->handle(req, parsegraph_EVENT_ACCEPTING_REQUEST, &accept, 0);
                if(!accept) {
                    snprintf(req->error, sizeof req->error, "Request explicitly rejected.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }

                // Request not rejected. Issue 100-continue if needed.
                if(req->expect_continue) {
                    req->stage = parsegraph_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE;
                }
                else {
                    switch(req->contentLen) {
                    case parsegraph_MESSAGE_IS_CHUNKED:
                        req->stage = parsegraph_CLIENT_REQUEST_READING_CHUNK_SIZE;
                        break;
                    case 0:
                    case parsegraph_MESSAGE_LENGTH_UNKNOWN:
                        req->stage = parsegraph_CLIENT_REQUEST_RESPONDING;
                        break;
                    default:
                    case parsegraph_MESSAGE_USES_CLOSE:
                        req->close_after_done = 1;
                        req->stage = parsegraph_CLIENT_REQUEST_READING_REQUEST_BODY;
                        break;
                    }
                }
            }

            break;
        }
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE || req->stage == parsegraph_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE) {
        return;
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_READING_REQUEST_BODY) {
        while(req->contentLen != 0) {
            // Read request body.
            char buf[parsegraph_BUFSIZE];
            memset(buf, 0, sizeof(buf));
            int requestedLen = req->contentLen;
            if(requestedLen > sizeof(buf)) {
                requestedLen = sizeof(buf);
            }
            int nread = parsegraph_Connection_read(cxn, buf, requestedLen);
            if(nread == 0) {
                // Zero-length read indicates end of stream.
                if(req->contentLen > 0) {
                    snprintf(req->error, sizeof req->error, "Premature end of request body.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                break;
            }
            if(nread < 0) {
                // Error.
                snprintf(req->error, sizeof req->error, "Error while receiving request body.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(nread < 4 && req->contentLen > 4) {
                // A read too small.
                parsegraph_Connection_putback(cxn, nread);
                return;
            }

            // Handle input.
            req->totalContentLen += nread;
            req->contentLen -= nread;
            req->handle(req, parsegraph_EVENT_REQUEST_BODY, buf, nread);
        }
        req->handle(req, parsegraph_EVENT_REQUEST_BODY, 0, 0);
        req->stage = parsegraph_CLIENT_REQUEST_RESPONDING;
    }

read_chunk_size:
    while(req->stage == parsegraph_CLIENT_REQUEST_READING_CHUNK_SIZE) {
        char buf[parsegraph_MAX_CHUNK_SIZE_LINE];
        memset(buf, 0, sizeof(buf));
        int nread = parsegraph_Connection_read(cxn, buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates premature end of stream.
            snprintf(req->error, sizeof req->error, "Premature end of chunked request body.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(nread < 0) {
            // Error.
            snprintf(req->error, sizeof req->error, "Error while receiving request body.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(nread < 3) {
            // A read too small.
            parsegraph_Connection_putback(cxn, nread);
            return;
        }

        int foundHexDigit = 0;
        int foundEnd = 0;
        for(int i = 0; i < nread; ++i) {
            char c = buf[i];
            if((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9')) {
                // Hex digit.
                foundHexDigit = 1;
                continue;
            }
            else if(foundHexDigit && c == '\n') {
                // Size-body separator.
                foundEnd = 1;
                buf[i] = 0;
                parsegraph_Connection_putback(cxn, nread - (i + 1));
                break;
            }
            else if(foundHexDigit && c == '\r' && i < nread - 1 && buf[i + 1] == '\n') {
                // Size-body separator.
                foundEnd = 1;
                buf[i] = 0;
                parsegraph_Connection_putback(cxn, nread - (i + 2));
                break;
            }
            else {
                // Garbage.
                snprintf(req->error, sizeof req->error, "Error while receiving chunk size.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
        }

        if(!foundEnd) {
            if(nread >= parsegraph_MAX_CHUNK_SIZE_LINE) {
                snprintf(req->error, sizeof req->error, "Chunk size line too long.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }

            // Incomplete read.
            parsegraph_Connection_putback(cxn, nread);
            return;
        }

        if(!foundHexDigit) {
            snprintf(req->error, sizeof req->error, "Assertion failed while receiving chunk size.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }

        char* endptr = 0;
        long int chunkSize = strtol(buf, &endptr, 16);
        if(endptr != 0) {
            snprintf(req->error, sizeof req->error, "Error while parsing chunk size.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(chunkSize < 0 || chunkSize > parsegraph_MAX_CHUNK_SIZE) {
            snprintf(req->error, sizeof req->error, "Request chunk size is out of range.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        req->chunkSize = chunkSize;
        req->stage = parsegraph_CLIENT_REQUEST_READING_CHUNK_BODY;
        if(req->chunkSize == 0) {
            req->handle(req, parsegraph_EVENT_REQUEST_BODY, 0, 0);
            if(req->expect_trailer) {
                req->stage = parsegraph_CLIENT_REQUEST_READING_TRAILER;
            }
            else {
                req->stage = parsegraph_CLIENT_REQUEST_RESPONDING;
            }
        }
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_READING_CHUNK_BODY) {
        while(req->chunkSize > 0) {
            char buf[parsegraph_BUFSIZE];
            memset(buf, 0, sizeof(buf));
            int requestedLen = req->chunkSize;
            if(requestedLen > sizeof(buf)) {
                requestedLen = sizeof(buf);
            }

            int nread = parsegraph_Connection_read(
                cxn,
                buf,
                requestedLen
            );
            if(nread == 0) {
                // Zero-length read indicates end of stream.
                if(req->chunkSize > 0) {
                    snprintf(req->error, sizeof req->error, "Premature end of request chunk body.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                break;
            }
            if(nread < 0) {
                // Error.
                snprintf(req->error, sizeof req->error, "Error while receiving request chunk body.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(nread < 4 && req->chunkSize > 4) {
                // A read too small.
                parsegraph_Connection_putback(cxn, nread);
                return;
            }

            // Handle input.
            req->totalContentLen += nread;
            req->chunkSize -= nread;
            req->handle(req, parsegraph_EVENT_REQUEST_BODY, buf, nread);
        }

        // Consume trailing EOL
        char buf[2];
        int nread = parsegraph_Connection_read(
            cxn,
            buf,
            sizeof(buf)
        );
        if(nread == 0) {
            // Zero-length read indicates end of stream.
            snprintf(req->error, sizeof req->error, "Premature end of request chunk body.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(nread < 0) {
            // Error.
            snprintf(req->error, sizeof req->error, "Error while receiving request chunk body.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(nread >= 1 && buf[0] == '\n') {
            // Non-standard LF separator
            if(nread > 1) {
                parsegraph_Connection_putback(cxn, nread - 1);
            }
        }
        else if(nread >= 2 && buf[0] == '\r' && buf[1] == '\n') {
            // CRLF separator
            if(nread > 2) {
                parsegraph_Connection_putback(cxn, nread - 2);
            }
        }
        else if(nread == 1 && buf[0] == '\r') {
            // Partial read.
            parsegraph_Connection_putback(cxn, nread);
            return;
        }
        else {
            snprintf(req->error, sizeof req->error, "Error while receiving request chunk body.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        req->stage = parsegraph_CLIENT_REQUEST_READING_CHUNK_SIZE;
        goto read_chunk_size;
    }

    while(req->stage == parsegraph_CLIENT_REQUEST_READING_TRAILER) {
        char fieldLine[MAX_FIELD_NAME_LENGTH + 2 + MAX_FIELD_VALUE_LENGTH + 2];
        memset(fieldLine, 0, sizeof(fieldLine));
        int nread = parsegraph_Connection_read(cxn, fieldLine, sizeof(fieldLine));
        if(nread <= 0) {
            // Error.
            return;
        }

        // Validate.
        int foundNewline = 0;
        int foundSeparator = 0;
        int toleratingSpaces = 0;
        char* fieldValue = 0;
        for(int i = 0; i < nread; ++i) {
            if(fieldLine[i] == '\n') {
                fieldLine[i] = 0;
                foundNewline = 1;
                parsegraph_Connection_putback(cxn, nread - i - 1);
                break;
            }
            if(i < nread - 1 && fieldLine[i] == '\r' && fieldLine[i + 1] == '\n') {
                fieldLine[i] = 0;
                foundNewline = 1;
                parsegraph_Connection_putback(cxn, nread - i - 2);
                break;
            }
            char c = fieldLine[i];
            if(c <= 0x1f || c == 0x7f) {
                snprintf(req->error, sizeof req->error, "Header line contains control characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%' || c == '"')) {
                snprintf(req->error, sizeof req->error, "Header name contains delimiters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                snprintf(req->error, sizeof req->error, "Header name contains unwise characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && !isalnum(c) && c != '-') {
                if(c == ':' && i > 0 && i < nread - 1) {
                    foundSeparator = i;
                    fieldLine[i] = 0;
                    toleratingSpaces = 1;
                    continue;
                }
                snprintf(req->error, sizeof req->error, "Header name contains non alphanumeric characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(foundSeparator && toleratingSpaces) {
                if(c == ' ') {
                    continue;
                }
                toleratingSpaces = 0;
                fieldValue = fieldLine + i;
            }
        }

        // Validate.
        if(!foundNewline && nread == sizeof(fieldLine)) {
            snprintf(req->error, sizeof req->error, "Request version is too long, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(!foundNewline) {
            // Incomplete;
            return;
        }
        if(foundSeparator && fieldValue) {
            char* fieldName = fieldLine;
            // Header found.
            req->handle(req, parsegraph_EVENT_HEADER, fieldName, fieldValue - fieldName);
            continue;
        }
        else if(fieldLine[0] == 0) {
            // Empty line. End of trailers.
            req->stage = parsegraph_CLIENT_REQUEST_RESPONDING;
            break;
        }
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_WEBSOCKET) {
        req->handle(req, parsegraph_EVENT_READ, 0, 0);
        return;
    }
    if(req->stage == parsegraph_CLIENT_REQUEST_DONE || req->stage == parsegraph_CLIENT_REQUEST_RESPONDING) {
        return;
    }
    else {
        snprintf(req->error, sizeof req->error, "Unexpected request stage.\n");
        cxn->stage = parsegraph_CLIENT_COMPLETE;
        return;
    }
}

static void parsegraph_clientWrite(parsegraph_Connection* cxn, struct parsegraph_Server* server)
{
    if(cxn->stage != parsegraph_CLIENT_SECURED) {
        return;
    }

    parsegraph_Ring* output = cxn->output;

    // Write current output.
    if(parsegraph_Ring_size(output) > 0) {
        int nflushed;
        int rv = parsegraph_Connection_flush(cxn, &nflushed);
        if(rv <= 0) {
            return;
        }
    }

    parsegraph_ClientRequest* req;
    while((req = cxn->current_request) != 0) {
        if(req->stage == parsegraph_CLIENT_REQUEST_WEBSOCKET) {
            // Check if the handler can respond.
            req->handle(req, parsegraph_EVENT_WEBSOCKET_RESPOND, 0, 0);
            return;
        }

        if(req->stage <= parsegraph_CLIENT_REQUEST_READING_FIELD) {
            // Request premature; couldn't write.
            return;
        }

        if(req->stage == parsegraph_CLIENT_REQUEST_DONE) {
            cxn->current_request = req->next_request;
            if(req->close_after_done) {
                cxn->stage = parsegraph_CLIENT_COMPLETE;
            }
            parsegraph_ClientRequest_destroy(req);
            continue;
        }

        if(parsegraph_Ring_size(output) > parsegraph_Ring_capacity(output) - 4) {
            // Buffer too full.
            return;
        }

        if(req->stage == parsegraph_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE) {
            const char* statusLine = "HTTP/1.1 100 Continue\r\n";
            size_t len = strlen(statusLine);
            int nwritten = parsegraph_Connection_write(cxn, statusLine, len);
            if(nwritten == 0) {
                snprintf(req->error, sizeof req->error, "Premature connection close.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(nwritten < 0) {
                snprintf(req->error, sizeof req->error, "Error while writing connection.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;

            }
            if(nwritten <= len) {
                // Only allow writes of the whole thing.
                parsegraph_Connection_putbackWrite(cxn, nwritten);
                return;
            }

            if(req->contentLen == -1) {
                req->stage = parsegraph_CLIENT_REQUEST_READING_CHUNK_SIZE;
            }
            else if(req->contentLen == 0) {
                req->stage = parsegraph_CLIENT_REQUEST_DONE;
            }
            else if(req->contentLen > 0) {
                req->stage = parsegraph_CLIENT_REQUEST_READING_REQUEST_BODY;
            }
            return;
        }

        if(req->stage == parsegraph_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE) {
            char out[1024];
            int nwrit = snprintf(out, sizeof(out), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", req->websocket_accept);
            int nwritten = parsegraph_Connection_write(cxn, out, nwrit);
            if(nwritten == 0) {
                snprintf(req->error, sizeof req->error, "Premature connection close.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(nwritten < 0) {
                snprintf(req->error, sizeof req->error, "Error while writing connection.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;

            }
            if(nwritten < nwrit) {
                // Only allow writes of the whole thing.
                parsegraph_Connection_putbackWrite(cxn, nwritten);
                return;
            }

            req->stage = parsegraph_CLIENT_REQUEST_WEBSOCKET;
            return;
        }

        if(req->stage > parsegraph_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE) {
            req->handle(req, parsegraph_EVENT_RESPOND, 0, 0);

            // Write current output.
            if(parsegraph_Ring_size(output) > 0) {
                int nflushed;
                int rv = parsegraph_Connection_flush(cxn, &nflushed);
                if(rv <= 0) {
                    return;
                }
            }
        }
    }
}
