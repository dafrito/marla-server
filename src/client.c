#include "marla.h"
#include <ctype.h>
#include <endian.h>
#include <string.h>

marla_WriteResult marla_processClientFields(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;
    while(req->readStage == marla_CLIENT_REQUEST_READING_FIELD) {
        char fieldLine[MAX_FIELD_NAME_LENGTH + 2 + MAX_FIELD_VALUE_LENGTH + 2];
        memset(fieldLine, 0, sizeof(fieldLine));
        int nread = marla_Connection_read(cxn, (unsigned char*)fieldLine, sizeof(fieldLine));
        if(nread <= 0) {
            // Error.
            return marla_WriteResult_UPSTREAM_CHOKED;
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
                marla_Connection_putbackRead(cxn, nread - i - 1);
                break;
            }
            if(fieldLine[i] == '\r') {
                if(i >= nread - 1) {
                    marla_Connection_putbackRead(cxn, nread);
                    return marla_WriteResult_UPSTREAM_CHOKED;
                }
                if(fieldLine[i + 1] != '\n') {
                    marla_killRequest(req, "Header line is not terminated properly, so no valid request.");
                    return marla_WriteResult_KILLED;
                }
                fieldLine[i] = 0;
                foundNewline = 1;
                marla_Connection_putbackRead(cxn, nread - i - 2);
                break;
            }
            char c = fieldLine[i];
            if((c <= 0x1f && c != '\t') || c == 0x7f) {
                marla_killRequest(req, "Client request header contains control characters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%' || c == '"')) {
                marla_killRequest(req, "Client request header contains delimiters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                marla_killRequest(req, "Client request header contains unwise characters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(!foundSeparator && !isalnum(c) && c != '-' && c != '\t') {
                if(c == ':' && i > 0 && i < nread - 1) {
                    foundSeparator = i;
                    fieldLine[i] = 0;
                    toleratingSpaces = 1;
                    continue;
                }
                marla_killRequest(req, "Client request header contains non alphanumeric characters, so no valid request.");
                return marla_WriteResult_KILLED;
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
            marla_killRequest(req, "Request version is too long, so no valid request.");
            return marla_WriteResult_KILLED;
        }
        if(!foundNewline) {
            // Incomplete;
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        if(foundSeparator && fieldValue) {
            // Header found.
            char* fieldName = fieldLine;
            //fprintf(stderr, "HEADER: %s = %s\n", fieldName, fieldValue);
            marla_logMessagecf(req->cxn->server, "HTTP Headers", "%s = %s", fieldName, fieldValue);

            if(!strcasecmp(fieldName, "Content-Length")) {
                if(req->requestLen != marla_MESSAGE_LENGTH_UNKNOWN) {
                    marla_killRequest(req, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.");
                    return marla_WriteResult_KILLED;
                }
                char* endptr;
                long int parsedLen = strtol(fieldValue, &endptr, 10);
                if(*endptr != '\0' || parsedLen < 0) {
                    marla_killRequest(req, "Content-Length header value could not be read, so no valid request.");
                    return marla_WriteResult_KILLED;
                }
                req->requestLen = parsedLen;
                if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(!strcasecmp(fieldName, "Host")) {
                memset(req->host, 0, sizeof(req->host));
                strncpy(req->host, fieldValue, sizeof(req->host) - 1);
                if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(!strcasecmp(fieldName, "Transfer-Encoding")) {
                if(req->requestLen != marla_MESSAGE_LENGTH_UNKNOWN) {
                    marla_killRequest(req, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.");
                    return marla_WriteResult_KILLED;
                }

                if(!strcasecmp(fieldValue, "chunked")) {
                    req->requestLen = marla_MESSAGE_IS_CHUNKED;
                }
            }
            else if(!strcasecmp(fieldName, "Connection")) {
                char* sp;
                char* fieldToken = strtok_r(fieldValue, ", ", &sp);
                int hasMultiple = 1;
                if(!fieldToken) {
                    fieldToken = fieldValue;
                    hasMultiple = 0;
                }
                while(fieldToken) {
                    if(!strcasecmp(fieldToken, "close")) {
                        if(req->requestLen == marla_MESSAGE_LENGTH_UNKNOWN) {
                            req->requestLen = marla_MESSAGE_USES_CLOSE;
                        }
                        req->close_after_done = 1;
                        marla_logMessage(req->cxn->server, "Request will close once done.");
                    }
                    else if(!strcasecmp(fieldToken, "Upgrade")) {
                        req->expect_upgrade = 1;
                    }
                    else if(!strcasecmp(fieldToken, "TE")) {
                        req->connection_indicates_trailer = 1;
                    }
                    else if(strcasecmp(fieldToken, "keep-alive")) {
                        marla_killRequest(req, "Connection is not understood, so no valid request.");
                        return marla_WriteResult_KILLED;
                    }
                    else if(req->handler) {
                        req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                    }
                    if(hasMultiple) {
                        fieldToken = strtok_r(0, ", ", &sp);
                    }
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
                else if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(!strcmp(fieldName, "Cookie")) {
                strncpy(req->cookieHeader, fieldValue, MAX_FIELD_VALUE_LENGTH);
                if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(!strcmp(fieldName, "Content-Type")) {
                strncpy(req->contentType, fieldValue, MAX_FIELD_VALUE_LENGTH);
                if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(!strcmp(fieldName, "Accept-Language")) {
                if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(!strcmp(fieldName, "Accept-Encoding")) {
                if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }

            }
            else if(!strcmp(fieldName, "Accept-Charset")) {
                if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(!strcmp(fieldName, "Sec-WebSocket-Key")) {
                strncpy(req->websocket_nonce, fieldValue, MAX_WEBSOCKET_NONCE_LENGTH);
            }
            else if(!strcmp(fieldName, "Sec-WebSocket-Version")) {
                if(!strcmp(fieldValue, "13")) {
                    req->websocket_version = 13;
                }
                else {
                    marla_killRequest(req, "Unexpected WebSocket version");
                }
            }
            else if(!strcmp(fieldName, "Accept")) {
                strncpy(req->acceptHeader, fieldValue, MAX_FIELD_VALUE_LENGTH);
                if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(!strcmp(fieldName, "Upgrade")) {
                if(!strcmp(fieldValue, "websocket")) {
                    req->expect_websocket = 1;
                }
                else if(req->handler) {
                    req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
                }
            }
            else if(req->handler) {
                req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
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
                    marla_killRequest(req, "No Host provided.");
                    return marla_WriteResult_KILLED;
                }
            }
            else if(isascii(req->uri[0])) {
                char* schemeSep = strstr(req->uri, "://");
                if(schemeSep != 0) {
                    for(char* c = req->uri; c != schemeSep; ++c) {
                        if(!isascii(*c)) {
                            marla_killRequest(req, "Scheme invalid, so no valid request.");
                            return marla_WriteResult_KILLED;
                        }
                    }
                    *schemeSep = 0;
                    if(!strcmp("http", req->uri)) {
                        *schemeSep = ':';
                    }
                    else if(!strcmp("https", req->uri)) {
                        *schemeSep = ':';
                    }
                    else {
                        marla_killRequest(req, "Request scheme unrecognized.");
                        return marla_WriteResult_KILLED;
                    }
                }
                char* hostPart = schemeSep + 3;
                char* hostSep = strstr(hostPart, "/");
                if(hostSep - hostPart >= MAX_FIELD_VALUE_LENGTH) {
                    marla_killRequest(req, "Host too long.");
                    return marla_WriteResult_KILLED;
                }
                if(hostSep == 0) {
                    // GET https://localhost
                    strncpy(req->host, hostPart, MAX_FIELD_VALUE_LENGTH);
                }
                else {
                    // GET https://localhost/absolute/path?query
                    *hostSep = 0;
                    if(req->host[0] != 0 && strcmp(req->host, hostPart)) {
                        marla_killRequest(req, "Host differs from absolute URI's host.");
                        return marla_WriteResult_KILLED;
                    }
                    strncpy(req->host, hostPart, MAX_FIELD_VALUE_LENGTH);
                    *hostSep = '/';

                    // Transform an absolute URI into a origin form
                    memmove(req->uri, hostSep, strlen(hostSep));
                }

                if(index(req->host, '@')) {
                    marla_killRequest(req, "Request must not provide userinfo.");
                    return marla_WriteResult_KILLED;
                }
            }
            else {
                marla_killRequest(req, "Request target unrecognized.");
                return marla_WriteResult_KILLED;
            }

            if(req->expect_upgrade && req->expect_websocket && req->websocket_nonce[0] != 0 && req->websocket_version == 13) {
                marla_logMessagef(req->cxn->server, "Doing WebSocket connection handshake");
                // Test Websocket nonce.
                char buf[MAX_FIELD_VALUE_LENGTH + 32 + 1];
                memset(buf, 0, sizeof(buf));
                strcpy(buf, req->websocket_nonce);
                if(strlen(req->websocket_nonce) != 24) {
                    marla_logMessagef(req->cxn->server, "WebSocket key is of an inappropriate length of %d bytes", strlen(req->websocket_nonce));
                    marla_killRequest(req, "WebSocket key must be exactly 24 bytes.");
                    return marla_WriteResult_KILLED;
                }
                strcat(buf, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
                unsigned char digest[SHA_DIGEST_LENGTH];
                memset(digest, 0, sizeof(digest));
                SHA1((unsigned char*)buf, strlen(buf), digest);

                BIO* mem = BIO_new(BIO_s_mem());
                BIO* b64 = BIO_new(BIO_f_base64());
                BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
                BIO_push(b64, mem);
                BIO_write(b64, digest, SHA_DIGEST_LENGTH);
                BIO_flush(b64);
                BUF_MEM* bptr;
                BIO_get_mem_ptr(b64, &bptr);

                memcpy(req->websocket_accept, bptr->data, bptr->length);
                req->websocket_accept[bptr->length] = 0;
                BIO_free_all(b64);

                req->statusCode = 101;
                strcpy(req->statusLine, "Switching Protocols");

                marla_logMessagef(req->cxn->server, "WebSocket opening handshake passed.");
            }
            else {
                req->expect_websocket = 0;
            }

            int accept = 1;
            if(req->handler) {
                req->handler(req, marla_EVENT_ACCEPTING_REQUEST, &accept, 0);
            }
            if(!accept) {
                marla_killRequest(req, "Request explicitly rejected.");
                return marla_WriteResult_KILLED;
            }

            marla_logMessagecf(req->cxn->server, "URL Requests", "Handling request for %s", req->uri);

            if(req->writeStage != marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT) {
                marla_killRequest(req, "Request explicitly rejected.");
                return marla_WriteResult_KILLED;
            }
            if(req->expect_upgrade) {
                req->readStage = marla_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE;
                req->writeStage = marla_CLIENT_REQUEST_WRITING_UPGRADE;
            }
            else if(req->expect_continue) {
                // Request not rejected. Issue 100-continue if needed.
                req->readStage = marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE;
                req->writeStage = marla_CLIENT_REQUEST_WRITING_CONTINUE;
            }
            else {
                switch(req->requestLen) {
                case marla_MESSAGE_IS_CHUNKED:
                    req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
                    marla_logMessage(server, "Reading request chunks");
                    break;
                case 0:
                case marla_MESSAGE_LENGTH_UNKNOWN:
                    marla_logMessage(server, "Done reading");
                    req->remainingContentLen = 0;
                    req->readStage = marla_CLIENT_REQUEST_READING_REQUEST_BODY;
                    break;
                case marla_MESSAGE_USES_CLOSE:
                    req->close_after_done = 1;
                    // Fall through.
                default:
                    req->readStage = marla_CLIENT_REQUEST_READING_REQUEST_BODY;
                    req->remainingContentLen = req->requestLen;
                    marla_logMessagef(server, "Reading request body of length %d", req->requestLen);
                    break;
                }
                if(req->requestLen > 0) {
                    req->totalContentLen = req->requestLen;
                    req->remainingContentLen = req->requestLen;
                }
                req->writeStage = marla_CLIENT_REQUEST_WRITING_RESPONSE;
            }

            break;
        }
    }

    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult marla_readRequestChunks(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;
read_chunk_size:
    while(req->readStage == marla_CLIENT_REQUEST_READING_CHUNK_SIZE) {
        char buf[marla_MAX_CHUNK_SIZE_LINE];
        memset(buf, 0, sizeof(buf));
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates premature end of stream.
            marla_killRequest(req, "Premature end of chunked request body.");
            return marla_WriteResult_KILLED;
        }
        if(nread < 0) {
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        if(nread < 3) {
            // A read too small.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
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
                marla_Connection_putbackRead(cxn, nread - (i + 1));
                break;
            }
            else if(foundHexDigit && c == '\r') {
                if(i >= nread - 1) {
                    marla_Connection_putbackRead(cxn, nread);
                    return marla_WriteResult_UPSTREAM_CHOKED;
                }
                if(buf[i + 1] != '\n') {
                    marla_killRequest(req, "Chunk is not terminated properly.");
                    return marla_WriteResult_KILLED;
                }
                // Size-body separator.
                foundEnd = 1;
                buf[i] = 0;
                marla_Connection_putbackRead(cxn, nread - (i + 2));
                break;
            }
            else {
                // Garbage.
                marla_killRequest(req, "Error while receiving chunk size.");
                return marla_WriteResult_KILLED;
            }
        }

        if(!foundEnd) {
            if(nread >= marla_MAX_CHUNK_SIZE_LINE) {
                marla_killRequest(req, "Chunk size line too long.");
                return marla_WriteResult_KILLED;
            }

            // Incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }

        if(!foundHexDigit) {
            marla_killRequest(req, "Failed to find any hex digits in chunk size.");
            return marla_WriteResult_KILLED;
        }

        char* endptr = 0;
        long int chunkSize = strtol(buf, &endptr, 16);
        if(*endptr != 0) {
            marla_killRequest(req, "Error while parsing chunk size.");
            return marla_WriteResult_KILLED;
        }
        if(chunkSize < 0 || chunkSize > marla_MAX_CHUNK_SIZE) {
            marla_killRequest(req, "Request chunk size is out of range.");
            return marla_WriteResult_KILLED;
        }
        req->chunkSize = chunkSize;
        req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_BODY;
        if(req->chunkSize == 0) {
            if(req->handler) {
                marla_WriteEvent result;
                marla_WriteEvent_init(&result, marla_WriteResult_CONTINUE);
                for(;;) {
                    req->handler(req, marla_EVENT_REQUEST_BODY, &result, -1);
                    switch(result.status) {
                    case marla_WriteResult_CONTINUE:
                        continue;
                    case marla_WriteResult_UPSTREAM_CHOKED:
                        // Some non-us upstream choked; treat it as downstream.
                        return marla_WriteResult_DOWNSTREAM_CHOKED;
                    case marla_WriteResult_DOWNSTREAM_CHOKED:
                    case marla_WriteResult_LOCKED:
                    case marla_WriteResult_KILLED:
                    case marla_WriteResult_CLOSED:
                    case marla_WriteResult_TIMEOUT:
                        return result.status;
                    }
                }
            }
        }
    }

    if(req->readStage == marla_CLIENT_REQUEST_READING_CHUNK_BODY) {
        marla_WriteEvent result;
        marla_WriteEvent_init(&result, marla_WriteResult_CONTINUE);
        result.index = req->lastReadIndex;

        while(req->chunkSize > 0) {
            char buf[marla_BUFSIZE];
            memset(buf, 0, sizeof(buf));
            int requestedLen = req->chunkSize;
            if(requestedLen > sizeof(buf)) {
                requestedLen = sizeof(buf);
            }

            int nread = marla_Connection_read(cxn, (unsigned char*)buf, requestedLen);
            if(nread == 0) {
                // Zero-length read indicates end of stream.
                if(req->chunkSize > 0) {
                    marla_killRequest(req, "Premature end of request chunk body.");
                    return marla_WriteResult_KILLED;
                }
                break;
            }
            if(nread < 0) {
                return marla_WriteResult_UPSTREAM_CHOKED;
            }
            if(nread < 4 && req->chunkSize > 4) {
                // A read too small.
                marla_Connection_putbackRead(cxn, nread);
                return marla_WriteResult_UPSTREAM_CHOKED;
            }

            if(!req->handler) {
                req->totalContentLen += nread;
                req->chunkSize -= nread;
                continue;
            }

            // Handle input.
            req->totalContentLen += nread;
            req->chunkSize -= nread;

            result.buf = &buf;
            result.length = nread;
            req->handler(req, marla_EVENT_REQUEST_BODY, &result, -1);
            result.buf = 0;

            switch(result.status) {
            case marla_WriteResult_CONTINUE:
                if(result.index < result.length) {
                    // Spurious continue.
                    marla_killRequest(req, "Client request handler indicated continue, but chunk not completely read.");
                    return marla_WriteResult_KILLED;
                }
                // Continue to read.
                result.index = 0;
                req->lastReadIndex = 0;
                marla_Connection_refill(req->cxn, 0);
                break;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                // This request body was being streamed to some other downstream source, which could not process
                // all of this chunk's input.
                marla_Connection_putbackRead(cxn, nread);
                req->totalContentLen -= nread;
                req->chunkSize += nread;
                return marla_WriteResult_DOWNSTREAM_CHOKED;
            case marla_WriteResult_UPSTREAM_CHOKED:
                if(result.length > 0 && result.index < result.length) {
                    marla_killRequest(req, "Client request handler indicated upstream choked despite partial read.");
                    return marla_WriteResult_KILLED;
                }
                // Continue to read the next chunk.
                req->lastReadIndex = 0;
                result.index = 0;
                req->lastReadIndex = result.index;
                marla_Connection_refill(req->cxn, 0);
                break;
            case marla_WriteResult_LOCKED:
                marla_logMessage(server, "Client request handler locked.");
                marla_Connection_putbackRead(cxn, nread);
                req->totalContentLen -= nread;
                req->chunkSize += nread;
                return result.status;
            case marla_WriteResult_TIMEOUT:
                marla_logMessage(server, "Client request handler timed out.");
                req->totalContentLen -= nread;
                req->chunkSize += nread;
                marla_Connection_putbackRead(cxn, nread);
                return result.status;
            case marla_WriteResult_KILLED:
                marla_logMessage(server, "Client request handler indicated killed.");
                return result.status;
            case marla_WriteResult_CLOSED:
                marla_logMessage(server, "Client request handler indicated closed.");
                return result.status;
            }
        }

        // Consume trailing EOL
        char buf[2];
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates end of stream.
            marla_killRequest(req, "Premature end of request chunk body.");
            return marla_WriteResult_KILLED;
        }
        if(nread < 0) {
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        if(nread >= 1 && buf[0] == '\n') {
            // Non-standard LF separator
            if(nread > 1) {
                marla_Connection_putbackRead(cxn, nread - 1);
            }
        }
        else if(nread >= 2 && buf[0] == '\r' && buf[1] == '\n') {
            // CRLF separator
            if(nread > 2) {
                marla_Connection_putbackRead(cxn, nread - 2);
            }
        }
        else if(nread == 1 && buf[0] == '\r') {
            // Partial read.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        else {
            marla_killRequest(req, "Error while receiving request chunk body.");
            return marla_WriteResult_KILLED;
        }
        req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
        goto read_chunk_size;
    }

    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult marla_readRequestBody(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;
    if(req->readStage < marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    if(req->readStage > marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
        return marla_WriteResult_CONTINUE;
    }

    marla_WriteEvent result;
    marla_WriteEvent_init(&result, marla_WriteResult_CONTINUE);
    result.index = req->lastReadIndex;

    while(req->remainingContentLen != 0) {
        // Read request body.
        char buf[marla_BUFSIZE];
        memset(buf, 0, sizeof(buf));
        int requestedLen = req->remainingContentLen;
        if(requestedLen > sizeof(buf)) {
            requestedLen = sizeof(buf);
        }
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, requestedLen);
        if(nread == 0) {
            // Zero-length read indicates end of stream.
            if(req->remainingContentLen > 0) {
                marla_killRequest(req, "Premature end of request body.");
                return marla_WriteResult_KILLED;
            }
            break;
        }
        if(nread < 0) {
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        if(nread < 4 && req->remainingContentLen > 4) {
            // A read too small.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        //fprintf(stderr, "Read %d bytes from connection.\n", nread);

        if(!req->handler) {
            req->totalContentLen += nread;
            req->remainingContentLen -= nread;
            continue;
        }

        // Handle input.
        req->totalContentLen += nread;
        req->remainingContentLen -= nread;

        result.buf = &buf;
        result.length = nread;
        req->handler(req, marla_EVENT_REQUEST_BODY, &result, -1);
        result.buf = 0;

        //fprintf(stderr, "REQUEST_BODY handler returned %s.\n", marla_nameWriteResult(result.status));

        switch(result.status) {
        case marla_WriteResult_CONTINUE:
            if(result.index < result.length) {
                // Spurious continue.
                marla_killRequest(req, "Client request handler indicated continue, but chunk not completely read.");
                return marla_WriteResult_KILLED;
            }
            result.index = 0;
            req->lastReadIndex = 0;
            marla_Connection_refill(req->cxn, 0);
            break;
        case marla_WriteResult_DOWNSTREAM_CHOKED:
            // This request body was being streamed to some other downstream source, which could not process
            // all of this chunk's input.
            marla_Connection_putbackRead(cxn, nread);
            req->totalContentLen -= nread;
            req->remainingContentLen += nread;
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        case marla_WriteResult_UPSTREAM_CHOKED:
            if(result.length > 0 && result.index < result.length) {
                marla_killRequest(req, "Client request handler indicated upstream choked despite partial read.");
                return marla_WriteResult_KILLED;
            }
            // Continue to read the next chunk.
            req->lastReadIndex = 0;
            result.index = 0;
            marla_Connection_refill(req->cxn, 0);
            break;
        case marla_WriteResult_LOCKED:
        case marla_WriteResult_TIMEOUT:
            marla_logMessage(server, "Client request handler locked or timed out.");
            marla_Connection_putbackRead(cxn, nread);
            req->totalContentLen -= nread;
            req->remainingContentLen += nread;
            return marla_WriteResult_TIMEOUT;
        case marla_WriteResult_CLOSED:
        case marla_WriteResult_KILLED:
            return result.status;
        }
    }

    if(req->handler) {
        result.buf = 0;
        result.index = 0;
        result.length = 0;
        result.status = marla_WriteResult_CONTINUE;
        for(; req->cxn->stage != marla_CLIENT_COMPLETE && req->readStage != marla_CLIENT_REQUEST_DONE_READING;) {
            req->handler(req, marla_EVENT_REQUEST_BODY, &result, -1);
            switch(result.status) {
            case marla_WriteResult_CONTINUE:
                continue;
            case marla_WriteResult_UPSTREAM_CHOKED:
                // Some non-us upstream choked; treat it as downstream.
                return marla_WriteResult_DOWNSTREAM_CHOKED;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
            case marla_WriteResult_LOCKED:
            case marla_WriteResult_KILLED:
            case marla_WriteResult_CLOSED:
            case marla_WriteResult_TIMEOUT:
                return result.status;
            }
        }
    }
    req->readStage = marla_CLIENT_REQUEST_DONE_READING;

    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult marla_processStatusLine(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    if(req->readStage == marla_CLIENT_REQUEST_READ_FRESH) {
        for(unsigned char c;;) {
            int nread = marla_Connection_read(cxn, &c, 1);
            if(nread < 0) {
                return marla_WriteResult_UPSTREAM_CHOKED;
            }
            if(nread == 0) {
                return marla_WriteResult_CLOSED;
            }
            if(c != '\r' && c != '\n') {
                marla_Connection_putbackRead(cxn, 1);
                req->readStage = marla_CLIENT_REQUEST_READING_METHOD;
                break;
            }
        }
    }

    if(req->readStage == marla_CLIENT_REQUEST_READING_METHOD) {
        memset(req->method, 0, sizeof(req->method));
        int nread = marla_Connection_read(cxn, (unsigned char*)req->method, MAX_METHOD_LENGTH + 1);
        if(nread < 0) {
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        if(nread == 0) {
            return marla_WriteResult_CLOSED;
        }
        if(nread < MIN_METHOD_LENGTH + 1) {
            // Incomplete.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }

        // Validate the given method.
        int foundSpace = 0;
        for(int i = 0; i < nread; ++i) {
            char c = req->method[i];
            if(c <= 0x1f || c == 0x7f) {
                marla_killRequest(req, "Request line contains control characters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                marla_killRequest(req, "Request line contains delimiters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                marla_killRequest(req, "Request line contains unwise characters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(c == ' ') {
                marla_Connection_putbackRead(cxn, nread - i);
                req->method[i] = 0;
                foundSpace = 1;
                break;
            }
            if(!isascii(c)) {
                marla_killRequest(req, "Request method contains non-ASCII characters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
        }
        if(nread == MAX_METHOD_LENGTH + 1 && !foundSpace) {
            marla_killRequest(req, "Request method is too long, so no valid request.");
            return marla_WriteResult_KILLED;
        }
        if(!foundSpace) {
            // No space found in the fragment found, so incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }

        memset(req->method + strlen(req->method), 0, sizeof(req->method) - strlen(req->method));
        req->readStage = marla_CLIENT_REQUEST_PAST_METHOD;

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
            marla_killRequest(req, "Request method '%s' is unknown, so no valid request.", req->method);
            return marla_WriteResult_KILLED;
        }

        marla_logMessagef(req->cxn->server, "Found method: %s", req->method);
    }

    if(req->readStage == marla_CLIENT_REQUEST_PAST_METHOD) {
        while(1) {
            unsigned char c;
            int nread = marla_Connection_read(cxn, &c, 1);
            if(nread < 0) {
                return marla_WriteResult_UPSTREAM_CHOKED;
            }
            if(nread == 0) {
                return marla_WriteResult_CLOSED;
            }
            if(c != ' ') {
                marla_Connection_putbackRead(cxn, 1);
                req->readStage = marla_CLIENT_REQUEST_READING_REQUEST_TARGET;
                break;
            }
        }
    }

    if(req->readStage == marla_CLIENT_REQUEST_READING_REQUEST_TARGET) {
        memset(req->uri, 0, sizeof(req->uri));
        int nread = marla_Connection_read(cxn, (unsigned char*)req->uri, MAX_URI_LENGTH + 1);
        if(nread < 0) {
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        if(nread == 0) {
            return marla_WriteResult_CLOSED;
        }
        if(nread < 2) {
            // Incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }

        // Validate the given method.
        int foundSpace = 0;
        for(int i = 0; i < nread; ++i) {
            char c = req->uri[i];
            if(c <= 0x1f || c == 0x7f) {
                marla_killRequest(req, "Request target contains control characters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                marla_killRequest(req, "Request target contains delimiters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(c == ' ') {
                marla_Connection_putbackRead(cxn, nread - i);
                req->uri[i] = 0;
                foundSpace = 1;
                break;
            }
        }
        if(nread == MAX_URI_LENGTH + 1 && !foundSpace) {
            marla_killRequest(req, "Request target is too long, so no valid request.");
            return marla_WriteResult_KILLED;
        }
        if(!foundSpace) {
            // No space found in the fragment found, so incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        memset(req->uri + strlen(req->uri), 0, sizeof(req->uri) - strlen(req->uri));

        marla_logMessagef(req->cxn->server, "Found URI: %s", req->uri);

        req->readStage = marla_CLIENT_REQUEST_PAST_REQUEST_TARGET;
    }

    if(req->readStage == marla_CLIENT_REQUEST_PAST_REQUEST_TARGET) {
        while(1) {
            unsigned char c;
            int nread = marla_Connection_read(cxn, &c, 1);
            if(nread < 0) {
                return marla_WriteResult_UPSTREAM_CHOKED;
            }
            if(nread == 0) {
                return marla_WriteResult_CLOSED;
            }
            if(c != ' ') {
                marla_Connection_putbackRead(cxn, 1);
                req->readStage = marla_CLIENT_REQUEST_READING_VERSION;
                break;
            }
        }
    }

    if(req->readStage == marla_CLIENT_REQUEST_READING_VERSION) {
        //                     "01234567"
        //fprintf(stderr, "PROCESS STATUS LINe\n");
        //marla_dumpRequest(req);
        const char* expected = "HTTP/1.1";
        size_t versionLen = strlen(expected);
        unsigned char givenVersion[10];
        memset(givenVersion, 0, sizeof(givenVersion));
        int nread = marla_Connection_read(cxn, givenVersion, versionLen + 1);
        if(nread < 0) {
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        if(nread == 0) {
            return marla_WriteResult_CLOSED;
        }
        if(nread < versionLen + 1) {
            // Incomplete.
            marla_Connection_putbackRead(cxn, nread);
            //fprintf(stderr, "nread is incomplete: %ld\n", nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }

        // Validate.
        for(int i = 0; i < versionLen; ++i) {
            if(givenVersion[i] != expected[i]) {
                marla_killRequest(req, "Request version is unknown, so no valid request.");
                return marla_WriteResult_KILLED;
            }
        }

        if(givenVersion[versionLen] == '\r') {
            unsigned char c;
            int nwritten = marla_Connection_read(cxn, &c, 1);
            //fprintf(stderr, "%d %d %ld %d\n", (int)'\r', (int)givenVersion[versionLen], nread, nwritten);
            if(nwritten < 1) {
                //fprintf(stderr, "No last character!\n");
                marla_Connection_putbackRead(cxn, nread);
                return marla_WriteResult_UPSTREAM_CHOKED;
            }
            if(nwritten != 1) {
                marla_die(req->cxn->server, "Unexpected number of characters read: %d", nwritten);
            }
            if(c != '\n') {
                marla_killRequest(req, "Unterminated request line.");
                return marla_WriteResult_KILLED;
            }
        }
        else if(givenVersion[versionLen] != '\n') {
            marla_killRequest(req, "Unterminated request line.");
            return marla_WriteResult_KILLED;
        }

        marla_Server_invokeHook(cxn->server, marla_ServerHook_ROUTE, req);

        req->readStage = marla_CLIENT_REQUEST_READING_FIELD;
    }

    return marla_WriteResult_CONTINUE;
}

int marla_clientAccept(marla_Connection* cxn)
{
    if(cxn->stage == marla_CLIENT_ACCEPTED) {
        // Client has just connected.
        if(cxn->acceptSource) {
            cxn->acceptSource(cxn);
            if(cxn->stage != marla_CLIENT_SECURED) {
                return -1;
            }
        }
        else {
            cxn->stage = marla_CLIENT_SECURED;
        }
    }
    return 0;
}

marla_WriteResult marla_inputWebSocket(marla_Request* req);
marla_WriteResult marla_outputWebSocket(marla_Request* req);

marla_WriteResult marla_clientRead(marla_Connection* cxn)
{
    marla_clientAccept(cxn);

    // Read in backend requests.
    if(cxn->is_backend) {
        return marla_backendRead(cxn);
    }

    if(cxn->in_read) {
        marla_logMessagecf(cxn->server, "Processing", "Client connection asked to read, but already reading.");
        return marla_WriteResult_LOCKED;
    }
    cxn->in_read = 1;

    marla_logEntercf(cxn->server, "Processing", "Reading from client connection");
    marla_Connection_refill(cxn, 0);
    marla_Request* req = 0;
    marla_Server* server = cxn->server;
    if(!cxn->current_request) {
        unsigned char c;
        int nread = marla_Connection_read(cxn, &c, 1);
        if(nread <= 0) {
            cxn->in_read = 0;
            marla_logLeave(server, 0);
            if(nread == 0) {
                goto exit_closed;
            }
            goto exit_upstream_choked;
        }
        marla_Connection_putbackRead(cxn, 1);

        // No request yet made.
        req = marla_Request_new(cxn);
        cxn->current_request = req;
        cxn->latest_request = req;
        ++cxn->requests_in_process;
    }
    else {
        req = cxn->latest_request;
    }

    marla_Request_ref(req);

    if(req->is_backend) {
        marla_die(cxn->server, "Backend request found its way in client connection processing.");
    }

    marla_logMessagecf(cxn->server, "Client processing", "clientRead: %s", marla_nameRequestReadStage(req->readStage));

    marla_WriteResult wr;
    wr = marla_processStatusLine(req);
    if(wr != marla_WriteResult_CONTINUE) {
        marla_Request_unref(req);
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return wr;
    }

    wr = marla_processClientFields(req);
    if(wr != marla_WriteResult_CONTINUE) {
        marla_Request_unref(req);
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return wr;
    }

    while(req->readStage == marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE || req->readStage == marla_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE) {
        // Attempt to write.
        wr = marla_clientWrite(req->cxn);
        switch(wr) {
        case marla_WriteResult_LOCKED:
            marla_Request_unref(req);
            goto exit_downstream_choked;
        case marla_WriteResult_UPSTREAM_CHOKED:
            marla_killRequest(req, "Failed to write HTTP continue or upgrade response because upstream choked, but there is nothing to choke on.");
            marla_Request_unref(req);
            goto exit_killed;
        case marla_WriteResult_CONTINUE:
            continue;
        case marla_WriteResult_CLOSED:
        case marla_WriteResult_KILLED:
        case marla_WriteResult_DOWNSTREAM_CHOKED:
        case marla_WriteResult_TIMEOUT:
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }
    }

    while(req->readStage < marla_CLIENT_REQUEST_DONE_READING) {
        wr = marla_inputWebSocket(req);
        if(wr != marla_WriteResult_CONTINUE) {
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }

        wr = marla_readRequestBody(req);
        if(wr != marla_WriteResult_CONTINUE) {
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }

        wr = marla_readRequestChunks(req);
        if(wr != marla_WriteResult_CONTINUE) {
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }

        if(req->backendPeer) {
            for(int loop = 1; loop && req->backendPeer && req->backendPeer->cxn->stage != marla_CLIENT_COMPLETE && req->backendPeer->writeStage != marla_BACKEND_REQUEST_DONE_WRITING; ) {
                wr = marla_backendWrite(req->backendPeer->cxn);
                switch(wr) {
                case marla_WriteResult_CONTINUE:
                    continue;
                case marla_WriteResult_KILLED:
                case marla_WriteResult_CLOSED:
                    loop = 0;
                    break;
                case marla_WriteResult_LOCKED:
                case marla_WriteResult_DOWNSTREAM_CHOKED:
                    marla_Request_unref(req);
                    goto exit_downstream_choked;
                case marla_WriteResult_UPSTREAM_CHOKED:
                    break;
                case marla_WriteResult_TIMEOUT:
                    marla_Request_unref(req);
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return wr;
                }
            }
        }
        else {
            for(; cxn->requests_in_process > 0 && req->cxn->stage != marla_CLIENT_COMPLETE && req->writeStage != marla_CLIENT_REQUEST_DONE_WRITING;) {
                wr = marla_clientWrite(req->cxn);
                switch(wr) {
                case marla_WriteResult_CONTINUE:
                    continue;
                case marla_WriteResult_UPSTREAM_CHOKED:
                    break;
                case marla_WriteResult_DOWNSTREAM_CHOKED:
                case marla_WriteResult_TIMEOUT:
                case marla_WriteResult_KILLED:
                case marla_WriteResult_CLOSED:
                case marla_WriteResult_LOCKED:
                    marla_Request_unref(req);
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return wr;
                }
            }
        }
    }

    if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        marla_killRequest(req, "Unexpected request stage: %d", req->readStage);
        marla_Request_unref(req);
        goto exit_killed;
    }

    for(; cxn->requests_in_process > 0;) {
        wr = marla_clientWrite(req->cxn);
        switch(wr) {
        case marla_WriteResult_CONTINUE:
            continue;
        case marla_WriteResult_UPSTREAM_CHOKED:
            // It's possible a different upstream choked, but this one has no more data and so cannot be the source of the choke.
            marla_logMessage(server, "Different upstream must have choked.");
            marla_Request_unref(req);
            goto exit_downstream_choked;
        case marla_WriteResult_KILLED:
        case marla_WriteResult_CLOSED:
            marla_Request_unref(req);
            goto exit_continue;
        case marla_WriteResult_DOWNSTREAM_CHOKED:
        case marla_WriteResult_TIMEOUT:
        case marla_WriteResult_LOCKED:
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }
    }

    marla_Request_unref(req);

    if(cxn->requests_in_process > 0) {
        for(marla_Request* req = cxn->current_request; req; req = req->next_request) {
            if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
                goto exit_continue;
            }
        }
    }
    goto exit_upstream_choked;
exit_upstream_choked:
    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return marla_WriteResult_DOWNSTREAM_CHOKED;
exit_downstream_choked:
    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return marla_WriteResult_DOWNSTREAM_CHOKED;
exit_killed:
    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return marla_WriteResult_KILLED;
exit_closed:
    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return marla_WriteResult_CLOSED;
exit_continue:
    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return marla_WriteResult_CONTINUE;
}

marla_WriteResult marla_clientWrite(marla_Connection* cxn)
{
    // Read in backend requests.
    if(cxn->is_backend) {
        return marla_backendWrite(cxn);
    }

    marla_Ring* output = cxn->output;

    // Write current output.
    if(marla_Ring_size(output) > 0) {
        int nflushed;
        int rv = marla_Connection_flush(cxn, &nflushed);
        if(rv < 0) {
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        if(rv == 0) {
            return marla_WriteResult_CLOSED;
        }
    }

    if(cxn->in_write) {
        marla_logMessagecf(cxn->server, "Processing", "Called to write to client, but already writing to client.");
        return marla_WriteResult_LOCKED;
    }
    cxn->in_write = 1;
    marla_Server* server = cxn->server;

    if(marla_Ring_size(output) > marla_Ring_capacity(output) - 4) {
        // Buffer too full.
        cxn->in_write = 0;
        return marla_WriteResult_DOWNSTREAM_CHOKED;
    }

    marla_Request* req = cxn->current_request;
    if(!req) {
        cxn->in_write = 0;
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    marla_Request_ref(req);

    marla_logEntercf(cxn->server, "Processing", "Writing to client with current request's write state: %s", marla_nameRequestWriteStage(req->writeStage));
    if(req->writeStage == marla_CLIENT_REQUEST_WRITING_CONTINUE) {
        if(req->readStage != marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE) {
            marla_killRequest(req, "Unexpected read stage %s.", marla_nameRequestReadStage(req->readStage));
            marla_Request_unref(req);
            goto exit_killed;
        }
        const char* statusLine = "HTTP/1.1 100 Continue\r\n\r\n";
        size_t len = strlen(statusLine);
        int nwritten = marla_Connection_write(cxn, statusLine, len);
        if(nwritten == 0) {
            marla_killRequest(req, "Premature connection close.");
            marla_Request_unref(req);
            goto exit_killed;
        }
        if(nwritten < 0) {
            marla_killRequest(req, "Error %d while writing connection.", nwritten);
            marla_Request_unref(req);
            goto exit_killed;
        }
        if(nwritten <= len) {
            // Only allow writes of the whole thing.
            marla_Connection_putbackWrite(cxn, nwritten);
            marla_Request_unref(req);
            goto exit_downstream_choked;
        }

        if(req->requestLen == marla_MESSAGE_IS_CHUNKED) {
            req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
        }
        else if(req->requestLen == 0) {
            req->readStage = marla_CLIENT_REQUEST_DONE_READING;
        }
        else if(req->requestLen > 0) {
            req->readStage = marla_CLIENT_REQUEST_READING_REQUEST_BODY;
            req->remainingContentLen = req->requestLen;
        }

        req->writeStage = marla_CLIENT_REQUEST_WRITING_RESPONSE;
    }

    if(req->writeStage == marla_CLIENT_REQUEST_WRITING_UPGRADE) {
        char out[1024];
        int nwrit = snprintf(out, sizeof(out), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", req->websocket_accept);
        int nwritten = marla_Connection_write(cxn, out, nwrit);
        if(nwritten == 0) {
            marla_killRequest(req, "Premature connection close.");
            marla_Request_unref(req);
            goto exit_killed;
        }
        if(nwritten < 0) {
            marla_killRequest(req, "Error while writing connection.");
            marla_Request_unref(req);
            goto exit_killed;
        }
        if(nwritten < nwrit) {
            // Only allow writes of the whole thing.
            marla_Connection_putbackWrite(cxn, nwritten);
            marla_Request_unref(req);
            goto exit_downstream_choked;
        }

        // Allow the handler to change when WebSocket is going to be used.
        marla_Server_invokeHook(cxn->server, marla_ServerHook_WEBSOCKET, req);

        req->readStage = marla_CLIENT_REQUEST_WEBSOCKET;
        req->writeStage = marla_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE;

        marla_logMessage(server, "Going websocket");
        for(int loop = 1; loop;) {
            switch(marla_clientRead(cxn)) {
            case marla_WriteResult_CONTINUE:
                continue;
            case marla_WriteResult_UPSTREAM_CHOKED:
            case marla_WriteResult_DOWNSTREAM_CHOKED:
            case marla_WriteResult_TIMEOUT:
            case marla_WriteResult_LOCKED:
                loop = 0;
                continue;
            case marla_WriteResult_CLOSED:
                marla_Request_unref(req);
                goto exit_closed;
            case marla_WriteResult_KILLED:
                marla_Request_unref(req);
                goto exit_killed;
            }
        }
    }

    marla_WriteResult wr = marla_outputWebSocket(req);
    if(wr != marla_WriteResult_CONTINUE) {
        marla_Request_unref(req);
        marla_logLeave(server, 0);
        cxn->in_write = 0;
        return wr;
    }

    marla_WriteEvent result;
    marla_WriteEvent_init(&result, marla_WriteResult_CONTINUE);

    for(; req->cxn->stage != marla_CLIENT_COMPLETE && req->writeStage == marla_CLIENT_REQUEST_WRITING_RESPONSE; ) {
        switch(result.status) {
        case marla_WriteResult_UPSTREAM_CHOKED:
            marla_Request_unref(req);
            goto exit_upstream_choked;
        case marla_WriteResult_DOWNSTREAM_CHOKED:
            // Write current output.
            if(marla_Ring_size(output) > 0) {
                int nflushed;
                int rv = marla_Connection_flush(cxn, &nflushed);
                if(rv < 0) {
                    marla_logMessage(server, "Downstream truly choked.");
                    marla_Request_unref(req);
                    goto exit_downstream_choked;
                }
                if(rv == 0) {
                    marla_Request_unref(req);
                    goto exit_closed;
                }
            }
            else {
                marla_Request_unref(req);
                goto exit_downstream_choked;
            }
            // Fall through regardless.
        case marla_WriteResult_CONTINUE:
            if(req->handler) {
                req->handler(req, marla_EVENT_MUST_WRITE, &result, 0);
                marla_logMessagef(req->cxn->server, "Client response handler indicated %s", marla_nameWriteResult(result.status));
                continue;
            }
            req->writeStage = marla_CLIENT_REQUEST_AFTER_RESPONSE;
            continue;
        case marla_WriteResult_TIMEOUT:
            marla_Request_unref(req);
            goto exit_timeout;
        case marla_WriteResult_LOCKED:
            marla_Request_unref(req);
            goto exit_locked;
        case marla_WriteResult_KILLED:
            marla_Request_unref(req);
            goto exit_killed;
        case marla_WriteResult_CLOSED:
            marla_Request_unref(req);
            goto exit_closed;
        }
    }

    if(req->writeStage == marla_CLIENT_REQUEST_AFTER_RESPONSE) {
        if(req->expect_trailer) {
            req->writeStage = marla_CLIENT_REQUEST_WRITING_TRAILERS;

        }
        else {
            req->writeStage = marla_CLIENT_REQUEST_DONE_WRITING;
        }
    }

    if(req->writeStage == marla_CLIENT_REQUEST_WRITING_TRAILERS) {
        req->writeStage = marla_CLIENT_REQUEST_DONE_WRITING;
    }

    if(req->writeStage == marla_CLIENT_REQUEST_DONE_WRITING) {
        //fprintf(stderr, "Done writing!\n");
        // Write current output.
        while(marla_Ring_size(cxn->output) > 0) {
            int nflushed;
            int rv = marla_Connection_flush(cxn, &nflushed);
            if(rv < 0) {
                marla_Request_unref(req);
                goto exit_downstream_choked;
            }
            if(rv == 0) {
                marla_Request_unref(req);
                goto exit_closed;
            }
        }

        marla_Connection* backend = 0;
        if(req->backendPeer) {
            backend = req->backendPeer->cxn;
        }
        if(cxn->current_request == cxn->latest_request) {
            cxn->current_request = 0;
            cxn->latest_request = 0;
        }
        else {
            cxn->current_request = req->next_request;
        }
        marla_Request_unref(req);
        --cxn->requests_in_process;
        if(req->close_after_done) {
            marla_logMessagef(server, "Closing connection %d now that client request %d is done writing.", cxn->id, req->id);
            marla_Request_unref(req);
            goto shutdown;
        }
        if(backend) {
            for(int loop = 1; loop;) {
                switch(marla_backendRead(backend)) {
                case marla_WriteResult_CONTINUE:
                    continue;
                case marla_WriteResult_LOCKED:
                case marla_WriteResult_TIMEOUT:
                case marla_WriteResult_DOWNSTREAM_CHOKED:
                case marla_WriteResult_UPSTREAM_CHOKED:
                case marla_WriteResult_KILLED:
                case marla_WriteResult_CLOSED:
                    loop = 0;
                    continue;
                }
            }
        }
        if(cxn->stage == marla_CLIENT_COMPLETE) {
            goto exit_closed;
        }
        cxn->in_write = 0;
        for(int loop = 1; loop;) {
            switch(marla_clientRead(cxn)) {
            case marla_WriteResult_CONTINUE:
                continue;
            case marla_WriteResult_LOCKED:
            case marla_WriteResult_TIMEOUT:
            case marla_WriteResult_DOWNSTREAM_CHOKED:
            case marla_WriteResult_UPSTREAM_CHOKED:
            case marla_WriteResult_KILLED:
            case marla_WriteResult_CLOSED:
                loop = 0;
                continue;
            }
        }
    }
    goto exit_upstream_choked;

exit_continue:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_CONTINUE;
exit_timeout:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_TIMEOUT;
exit_locked:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_LOCKED;
exit_upstream_choked:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_UPSTREAM_CHOKED;
exit_downstream_choked:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_DOWNSTREAM_CHOKED;
exit_killed:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_KILLED;
exit_closed:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_CLOSED;
shutdown:
    cxn->stage = marla_CLIENT_COMPLETE;
    if(!cxn->shouldDestroy) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
        marla_logLeave(server, 0);
        cxn->in_write = 0;
        return marla_WriteResult_DOWNSTREAM_CHOKED;
    }
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_CLOSED;
}

const char* marla_nameClientEvent(enum marla_ClientEvent ev)
{
    switch(ev) {
    case marla_BACKEND_EVENT_NEED_HEADERS:
        return "BACKEND_EVENT_NEED_HEADERS";
    case marla_BACKEND_EVENT_TRAILER:
        return "BACKEND_EVENT_TRAILER";
    case marla_BACKEND_EVENT_RESPONSE_BODY:
        return "BACKEND_EVENT_RESPONSE_BODY";
    case marla_BACKEND_EVENT_HEADER:
        return "BACKEND_EVENT_HEADER";
    case marla_BACKEND_EVENT_MUST_READ:
        return "BACKEND_EVENT_MUST_READ";
    case marla_BACKEND_EVENT_MUST_WRITE:
        return "BACKEND_EVENT_MUST_WRITE";
    case marla_BACKEND_EVENT_CLIENT_PEER_CLOSED:
        return "BACKEND_EVENT_CLIENT_PEER_CLOSED";
    case marla_BACKEND_EVENT_CLOSING:
        return "BACKEND_EVENT_CLOSING";
    case marla_EVENT_BACKEND_PEER_CLOSED:
        return "EVENT_BACKEND_PEER_CLOSED";
    case marla_EVENT_HEADER:
        return "EVENT_HEADER";
    case marla_EVENT_ACCEPTING_REQUEST:
        return "EVENT_ACCEPTING_REQUEST";
    case marla_BACKEND_EVENT_ACCEPTING_RESPONSE:
        return "BACKEND_EVENT_ACCEPTING_RESPONSE";
    case marla_EVENT_REQUEST_BODY:
        return "EVENT_REQUEST_BODY";
    case marla_EVENT_FORM_FIELD:
        return "EVENT_FORM_FIELD";
    case marla_EVENT_MUST_WRITE:
        return "EVENT_MUST_WRITE";
    case marla_EVENT_WEBSOCKET_MUST_READ:
        return "EVENT_WEBSOCKET_MUST_READ";
    case marla_EVENT_WEBSOCKET_MUST_WRITE:
        return "EVENT_WEBSOCKET_MUST_WRITE";
    case marla_EVENT_WEBSOCKET_CLOSING:
        return "EVENT_WEBSOCKET_CLOSING";
    case marla_EVENT_WEBSOCKET_CLOSE_REASON:
        return "EVENT_WEBSOCKET_CLOSE_REASON";
    case marla_EVENT_DESTROYING:
        return "EVENT_DESTROYING";
    case marla_BACKEND_EVENT_DESTROYING:
        return "BACKEND_EVENT_DESTROYING";
    }
    return "?";
}
