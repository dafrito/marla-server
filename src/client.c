#include "rainback.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

static void parsegraph_clientRead(parsegraph_Connection* cxn);
static void parsegraph_clientWrite(parsegraph_Connection* cxn);

void parsegraph_Client_handle(parsegraph_Connection* cxn, int event)
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
            break;
        }
        parsegraph_Connection_putback(cxn, 1);

        parsegraph_clientRead(cxn);
    }

    // Write requests.
    parsegraph_clientWrite(cxn);

    if(cxn->stage == parsegraph_CLIENT_COMPLETE) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
        return;
    }
}

static void parsegraph_clientRead(parsegraph_Connection* cxn)
{
    parsegraph_ClientRequest* req = 0;
    if(!cxn->current_request) {
        // No request yet made.
        req = parsegraph_ClientRequest_new(cxn);
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
                printf("Request line contains control characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                printf("Request line contains delimiters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                printf("Request line contains unwise characters, so no valid request.\n");
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
                printf("Request method contains non-ASCII characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
        }
        if(nread == MAX_METHOD_LENGTH + 1 && !foundSpace) {
            printf("Request method is too long, so no valid request.\n");
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
            printf("Request method is unknown, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }

        printf("Found method: %s\n", req->method);
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
                printf("Request target contains control characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                printf("Request target contains delimiters, so no valid request.\n");
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
            printf("Request target is too long, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(!foundSpace) {
            // No space found in the fragment found, so incomplete read.
            return;
        }

        printf("Found URI: %s\n", req->uri);

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
                printf("Request version is unknown, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
        }

        if(givenVersion[len] == '\n') {
            parsegraph_Connection_putback(cxn, 1);
        }
        else if(givenVersion[len] != '\r' || givenVersion[len + 1] != '\n') {
            printf("Request version is unknown, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }

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
                printf("Header line contains control characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%' || c == '"')) {
                printf("Header name contains delimiters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                printf("Header name contains unwise characters, so no valid request.\n");
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
                printf("Header name contains non alphanumeric characters, so no valid request.\n");
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
            printf("Request version is too long, so no valid request.\n");
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
            fprintf(stderr, "HEADER: %s = %s\n", fieldName, fieldValue);

            if(!strcmp(fieldName, "Content-Length")) {
                if(req->contentLen != -2) {
                    printf("Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                char* endptr;
                long int x = strtol(fieldValue, &endptr, 10);
                if(*endptr != '\0' || x < 0) {
                    printf("Content-Length header value could not be read, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                req->contentLen = x;
            }
            else if(!strcmp(fieldName, "Host")) {
                memset(req->host, 0, sizeof(req->host));
                strncpy(req->host, fieldValue, sizeof(req->host) - 1);
            }
            else if(!strcmp(fieldName, "Transfer-Encoding")) {
                if(req->contentLen != -2) {
                    printf("Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }

                if(!strcmp(fieldValue, "chunked")) {
                    req->contentLen = parsegraph_MESSAGE_IS_CHUNKED;
                }
            }
            else if(!strcmp(fieldName, "Connection")) {
                if(!strcmp(fieldValue, "close")) {
                    req->contentLen = parsegraph_MESSAGE_USES_CLOSE;
                    req->close_after_done = 1;
                }
                else if(strcmp(fieldValue, "keep-alive")) {
                    printf("Connection is not understood, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
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

            }
            else if(!strcmp(fieldName, "Accept-Language")) {

            }
            else if(!strcmp(fieldName, "Accept-Encoding")) {

            }
            else if(!strcmp(fieldName, "Accept-Charset")) {

            }
            else if(!strcmp(fieldName, "Accept")) {

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
                            printf("Scheme invalid, so no valid request.\n");
                            cxn->stage = parsegraph_CLIENT_COMPLETE;
                            return;
                        }
                    }
                    *schemeSep = 0;
                    if(!strcmp("http", req->uri)) {
                        printf("HTTP scheme unsupported, so no valid request.\n");
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    else if(!strcmp("https", req->uri)) {
                        *schemeSep = ':';
                    }
                    else {
                        printf("Request scheme unrecognized.\n");
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                }
                char* hostPart = schemeSep + 3;
                char* hostSep = strstr(hostPart, "/");
                if(hostSep - hostPart >= MAX_FIELD_VALUE_LENGTH) {
                    printf("Host too long.\n");
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
                        printf("Host differs from absolute URI's host.\n");
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
                printf("Request target unrecognized.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }

            int accept = 1;
            req->handle(req, parsegraph_EVENT_ACCEPTING_REQUEST, &accept, 0);
            if(!accept) {
                printf("Request explicitly rejected.\n");
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

            break;
        }
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE) {
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
                    printf("Premature end of request body.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                break;
            }
            if(nread < 0) {
                // Error.
                printf("Error while receiving request body.\n");
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
        req->stage = parsegraph_CLIENT_REQUEST_RESPONDING;
    }

read_chunk_size:
    while(req->stage == parsegraph_CLIENT_REQUEST_READING_CHUNK_SIZE) {
        char buf[parsegraph_MAX_CHUNK_SIZE_LINE];
        memset(buf, 0, sizeof(buf));
        int nread = parsegraph_Connection_read(cxn, buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates premature end of stream.
            printf("Premature end of chunked request body.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(nread < 0) {
            // Error.
            printf("Error while receiving request body.\n");
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
                // It's a hex digit.
                foundHexDigit = 1;
                continue;
            }
            else if(foundHexDigit && c == '\n') {
                // It's the size-body separator.
                foundEnd = 1;
                buf[i] = 0;
                parsegraph_Connection_putback(cxn, nread - (i + 1));
                break;
            }
            else if(foundHexDigit && c == '\r' && i < nread - 1 && buf[i + 1] == '\n') {
                // It's the size-body separator.
                foundEnd = 1;
                buf[i] = 0;
                parsegraph_Connection_putback(cxn, nread - (i + 2));
                break;
            }
            else {
                // Garbage.
                printf("Error while receiving chunk size.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
        }

        if(!foundEnd) {
            if(nread >= parsegraph_MAX_CHUNK_SIZE_LINE) {
                printf("Chunk size line too long.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }

            // Incomplete read.
            parsegraph_Connection_putback(cxn, nread);
            return;
        }

        if(!foundHexDigit) {
            printf("Assertion failed while receiving chunk size.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }

        char* endptr = 0;
        long int chunkSize = strtol(buf, &endptr, 16);
        if(endptr != 0) {
            printf("Error while parsing chunk size.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(chunkSize < 0 || chunkSize > parsegraph_MAX_CHUNK_SIZE) {
            printf("Request chunk size is out of range.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        req->chunkSize = chunkSize;
        req->stage = parsegraph_CLIENT_REQUEST_READING_CHUNK_BODY;
        if(req->chunkSize == 0) {
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
                    printf("Premature end of request chunk body.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                break;
            }
            if(nread < 0) {
                // Error.
                printf("Error while receiving request chunk body.\n");
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

        // Consume trailiing EOL
        char buf[2];
        int nread = parsegraph_Connection_read(
            cxn,
            buf,
            sizeof(buf)
        );
        if(nread == 0) {
            // Zero-length read indicates end of stream.
            printf("Premature end of request chunk body.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(nread < 0) {
            // Error.
            printf("Error while receiving request chunk body.\n");
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
            printf("Error while receiving request chunk body.\n");
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
                printf("Header line contains control characters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%' || c == '"')) {
                printf("Header name contains delimiters, so no valid request.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                printf("Header name contains unwise characters, so no valid request.\n");
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
                printf("Header name contains non alphanumeric characters, so no valid request.\n");
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
            printf("Request version is too long, so no valid request.\n");
            cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(!foundNewline) {
            // Incomplete;
            return;
        }
        if(foundSeparator && fieldValue) {
            char* fieldName = fieldLine;
            printf("Header: %s = %s\n", fieldName, fieldValue);
            // Header found.
            req->handle(req, parsegraph_EVENT_HEADER, fieldName, fieldValue - fieldName);
            continue;
        }
        else if(fieldLine[0] == 0) {
            // Empty line. End of trailers.
            printf("End of headers\n");
            req->stage = parsegraph_CLIENT_REQUEST_RESPONDING;
            break;
        }
    }

    if(req->stage == parsegraph_CLIENT_REQUEST_DONE || req->stage == parsegraph_CLIENT_REQUEST_RESPONDING) {
        return;
    }
    else {
        printf("Unexpected request stage.\n");
        cxn->stage = parsegraph_CLIENT_COMPLETE;
        return;
    }
}

static void parsegraph_clientWrite(parsegraph_Connection* cxn)
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
                printf("Premature connection close.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            if(nwritten < 0) {
                printf("Error while writing connection.\n");
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
