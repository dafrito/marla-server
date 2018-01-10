#include "marla.h"
#include <ctype.h>

static int marla_processClientFields(marla_ClientRequest* req)
{
    marla_Connection* cxn = req->cxn;
    while(req->readStage == marla_CLIENT_REQUEST_READING_FIELD) {
        char fieldLine[MAX_FIELD_NAME_LENGTH + 2 + MAX_FIELD_VALUE_LENGTH + 2];
        memset(fieldLine, 0, sizeof(fieldLine));
        int nread = marla_Connection_read(cxn, (unsigned char*)fieldLine, sizeof(fieldLine));
        if(nread <= 0) {
            // Error.
            return -1;
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
            if(i < nread - 1 && fieldLine[i] == '\r' && fieldLine[i + 1] == '\n') {
                fieldLine[i] = 0;
                foundNewline = 1;
                marla_Connection_putbackRead(cxn, nread - i - 2);
                break;
            }
            char c = fieldLine[i];
            if(c <= 0x1f || c == 0x7f) {
                marla_killClientRequest(req, "Header line contains control characters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%' || c == '"')) {
                marla_killClientRequest(req, "Header name contains delimiters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                marla_killClientRequest(req, "Header name contains unwise characters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && !isalnum(c) && c != '-') {
                if(c == ':' && i > 0 && i < nread - 1) {
                    foundSeparator = i;
                    fieldLine[i] = 0;
                    toleratingSpaces = 1;
                    continue;
                }
                marla_killClientRequest(req, "Header name contains non alphanumeric characters, so no valid request.\n");
                return -1;
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
            marla_killClientRequest(req, "Request version is too long, so no valid request.\n");
            return -1;
        }
        if(!foundNewline) {
            // Incomplete;
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }
        if(foundSeparator && fieldValue) {
            // Header found.
            char* fieldName = fieldLine;
            //fprintf(stderr, "HEADER: %s = %s\n", fieldName, fieldValue);
            marla_logMessagecf(req->cxn->server, "HTTP Headers", "%s = %s", fieldName, fieldValue);

            if(!strcasecmp(fieldName, "Content-Length")) {
                if(req->contentLen != -2) {
                    marla_killClientRequest(req, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    return -1;
                }
                char* endptr;
                long int x = strtol(fieldValue, &endptr, 10);
                if(*endptr != '\0' || x < 0) {
                    marla_killClientRequest(req, "Content-Length header value could not be read, so no valid request.\n");
                    return -1;
                }
                req->contentLen = x;
            }
            else if(!strcasecmp(fieldName, "Host")) {
                memset(req->host, 0, sizeof(req->host));
                strncpy(req->host, fieldValue, sizeof(req->host) - 1);
            }
            else if(!strcasecmp(fieldName, "Transfer-Encoding")) {
                if(req->contentLen != -2) {
                    marla_killClientRequest(req, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    return -1;
                }

                if(!strcasecmp(fieldValue, "chunked")) {
                    req->contentLen = marla_MESSAGE_IS_CHUNKED;
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
                        req->contentLen = marla_MESSAGE_USES_CLOSE;
                        req->close_after_done = 1;
                    }
                    else if(!strcasecmp(fieldToken, "Upgrade")) {
                        req->expect_upgrade = 1;
                    }
                    else if(strcasecmp(fieldToken, "keep-alive")) {
                        marla_killClientRequest(req, "Connection is not understood, so no valid request.\n");
                        return -1;
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
            else if(req->handle) {
                req->handle(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
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
                    marla_killClientRequest(req, "No Host provided.");
                    return -1;
                }
            }
            else if(isascii(req->uri[0])) {
                char* schemeSep = strstr(req->uri, "://");
                if(schemeSep != 0) {
                    for(char* c = req->uri; c != schemeSep; ++c) {
                        if(!isascii(*c)) {
                            marla_killClientRequest(req, "Scheme invalid, so no valid request.\n");
                            return -1;
                        }
                    }
                    *schemeSep = 0;
                    if(!strcmp("http", req->uri)) {
                        marla_killClientRequest(req, "HTTP scheme unsupported, so no valid request.\n");
                        return -1;
                    }
                    else if(!strcmp("https", req->uri)) {
                        *schemeSep = ':';
                    }
                    else {
                        marla_killClientRequest(req, "Request scheme unrecognized.\n");
                        return -1;
                    }
                }
                char* hostPart = schemeSep + 3;
                char* hostSep = strstr(hostPart, "/");
                if(hostSep - hostPart >= MAX_FIELD_VALUE_LENGTH) {
                    marla_killClientRequest(req, "Host too long.\n");
                    return -1;
                }

                if(hostSep == 0) {
                    // GET https://localhost
                    strncpy(req->host, hostPart, MAX_FIELD_VALUE_LENGTH);
                }
                else {
                    // GET https://localhost/absolute/path?query
                    *hostSep = 0;
                    if(req->host[0] != 0 && strcmp(req->host, hostPart)) {
                        marla_killClientRequest(req, "Host differs from absolute URI's host.\n");
                        return -1;
                    }
                    strncpy(req->host, hostPart, MAX_FIELD_VALUE_LENGTH);
                    *hostSep = '/';

                    // Transform an absolute URI into a origin form
                    memmove(req->uri, hostSep, strlen(hostSep));
                }
            }
            else {
                marla_killClientRequest(req, "Request target unrecognized.\n");
                return -1;
            }

            if(req->expect_upgrade && req->expect_websocket && req->websocket_nonce[0] != 0 && req->websocket_version == 13) {
                marla_logMessagef(req->cxn->server, "Doing WebSocket connection handshake");
                // Test Websocket nonce.
                char buf[MAX_FIELD_VALUE_LENGTH + 32 + 1];
                memset(buf, 0, sizeof(buf));
                strcpy(buf, req->websocket_nonce);
                if(strlen(req->websocket_nonce) != 24) {
                    marla_logMessagef(req->cxn->server, "WebSocket key is of an inappropriate length of %d bytes", strlen(req->websocket_nonce));
                    marla_killClientRequest(req, "WebSocket key must be exactly 24 bytes.");
                    return -1;
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
            if(req->handle) {
                req->handle(req, marla_EVENT_ACCEPTING_REQUEST, &accept, 0);
            }
            if(!accept) {
                marla_killClientRequest(req, "Request explicitly rejected.\n");
                return -1;
            }

            marla_logMessagecf(req->cxn->server, "URL Requests", "Handling request for %s", req->uri);

            if(req->writeStage != marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT) {
                marla_killClientRequest(req, "Request explicitly rejected.\n");
                return -1;
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
                switch(req->contentLen) {
                case marla_MESSAGE_IS_CHUNKED:
                    req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
                    break;
                case 0:
                case marla_MESSAGE_LENGTH_UNKNOWN:
                    req->readStage = marla_CLIENT_REQUEST_DONE_READING;
                    break;
                default:
                case marla_MESSAGE_USES_CLOSE:
                    req->close_after_done = 1;
                    req->readStage = marla_CLIENT_REQUEST_READING_REQUEST_BODY;
                    break;
                }
                req->writeStage = marla_CLIENT_REQUEST_WRITING_RESPONSE;
            }

            break;
        }
    }
    return 0;
}

static int marla_readRequestChunks(marla_ClientRequest* req)
{
    marla_Connection* cxn = req->cxn;
read_chunk_size:
    while(req->readStage == marla_CLIENT_REQUEST_READING_CHUNK_SIZE) {
        char buf[marla_MAX_CHUNK_SIZE_LINE];
        memset(buf, 0, sizeof(buf));
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates premature end of stream.
            marla_killClientRequest(req, "Premature end of chunked request body.\n");
            return -1;
        }
        if(nread < 0) {
            // Error.
            marla_killClientRequest(req, "Error %d while receiving request body.\n", nread);
            return -1;
        }
        if(nread < 3) {
            // A read too small.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
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
            else if(foundHexDigit && c == '\r' && i < nread - 1 && buf[i + 1] == '\n') {
                // Size-body separator.
                foundEnd = 1;
                buf[i] = 0;
                marla_Connection_putbackRead(cxn, nread - (i + 2));
                break;
            }
            else {
                // Garbage.
                marla_killClientRequest(req, "Error while receiving chunk size.\n");
                return -1;
            }
        }

        if(!foundEnd) {
            if(nread >= marla_MAX_CHUNK_SIZE_LINE) {
                marla_killClientRequest(req, "Chunk size line too long.\n");
                return -1;
            }

            // Incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }

        if(!foundHexDigit) {
            marla_killClientRequest(req, "Failed to find any hex digits in chunk size.\n");
            return -1;
        }

        char* endptr = 0;
        long int chunkSize = strtol(buf, &endptr, 16);
        if(*endptr != 0) {
            marla_killClientRequest(req, "Error while parsing chunk size.\n");
            return -1;
        }
        if(chunkSize < 0 || chunkSize > marla_MAX_CHUNK_SIZE) {
            marla_killClientRequest(req, "Request chunk size is out of range.\n");
            return -1;
        }
        req->chunkSize = chunkSize;
        req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_BODY;
        if(req->chunkSize == 0) {
            if(req->handle) {
                req->handle(req, marla_EVENT_REQUEST_BODY, 0, 0);
            }
            if(req->expect_trailer) {
                req->readStage = marla_CLIENT_REQUEST_READING_TRAILER;
            }
            else {
                req->readStage = marla_CLIENT_REQUEST_DONE_READING;
            }
        }
    }

    if(req->readStage == marla_CLIENT_REQUEST_READING_CHUNK_BODY) {
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
                    marla_killClientRequest(req, "Premature end of request chunk body.\n");
                    return -1;
                }
                break;
            }
            if(nread < 0) {
                // Error.
                marla_killClientRequest(req, "Error while receiving request chunk body.\n");
                return -1;
            }
            if(nread < 4 && req->chunkSize > 4) {
                // A read too small.
                marla_Connection_putbackRead(cxn, nread);
                return -1;
            }

            // Handle input.
            req->totalContentLen += nread;
            req->chunkSize -= nread;
            if(req->handle) {
                req->handle(req, marla_EVENT_REQUEST_BODY, buf, nread);
            }
        }

        // Consume trailing EOL
        char buf[2];
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates end of stream.
            marla_killClientRequest(req, "Premature end of request chunk body.\n");
            return -1;
        }
        if(nread < 0) {
            // Error.
            marla_killClientRequest(req, "Error while receiving request chunk body.\n");
            return -1;
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
            return -1;
        }
        else {
            marla_killClientRequest(req, "Error while receiving request chunk body.\n");
            return -1;
        }
        req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
        goto read_chunk_size;
    }

    return 0;
}

int marla_processStatusLine(marla_ClientRequest* req)
{
    marla_Connection* cxn = req->cxn;
    if(req->readStage == marla_CLIENT_REQUEST_READ_FRESH) {
        while(1) {
            unsigned char c;
            int nread = marla_Connection_read(cxn, &c, 1);
            if(nread <= 0) {
                return -1;
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
        size_t nread = marla_Connection_read(cxn, (unsigned char*)req->method, MAX_METHOD_LENGTH + 1);
        if(nread <= 0) {
            // Error.
            return -1;
        }
        if(nread < MIN_METHOD_LENGTH + 1) {
            // Incomplete.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }

        // Validate the given method.
        int foundSpace = 0;
        for(int i = 0; i < nread; ++i) {
            char c = req->method[i];
            if(c <= 0x1f || c == 0x7f) {
                marla_killClientRequest(req, "Request line contains control characters, so no valid request.\n");
                return -1;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                marla_killClientRequest(req, "Request line contains delimiters, so no valid request.\n");
                return -1;
            }
            if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                marla_killClientRequest(req, "Request line contains unwise characters, so no valid request.\n");
                return -1;
            }
            if(c == ' ') {
                marla_Connection_putbackRead(cxn, nread - i);
                req->method[i] = 0;
                foundSpace = 1;
                break;
            }
            if(!isascii(c)) {
                marla_killClientRequest(req, "Request method contains non-ASCII characters, so no valid request.\n");
                return -1;
            }
        }
        if(nread == MAX_METHOD_LENGTH + 1 && !foundSpace) {
            marla_killClientRequest(req, "Request method is too long, so no valid request.\n");
            return -1;
        }
        if(!foundSpace) {
            // No space found in the fragment found, so incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
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
            marla_killClientRequest(req, "Request method '%s' is unknown, so no valid request.\n", req->method);
            return -1;
        }

        //printf("Found method: %s\n", req->method);
    }

    if(req->readStage == marla_CLIENT_REQUEST_PAST_METHOD) {
        while(1) {
            unsigned char c;
            int nread = marla_Connection_read(cxn, &c, 1);
            if(nread <= 0) {
                return -1;
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
        if(nread <= 0) {
            // Error.
            return -1;
        }
        if(nread < 2) {
            // Incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }

        // Validate the given method.
        int foundSpace = 0;
        for(int i = 0; i < nread; ++i) {
            char c = req->uri[i];
            if(c <= 0x1f || c == 0x7f) {
                marla_killClientRequest(req, "Request target contains control characters, so no valid request.\n");
                return -1;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                marla_killClientRequest(req, "Request target contains delimiters, so no valid request.\n");
                return -1;
            }
            if(c == ' ') {
                marla_Connection_putbackRead(cxn, nread - i);
                req->uri[i] = 0;
                foundSpace = 1;
                break;
            }
        }
        if(nread == MAX_URI_LENGTH + 1 && !foundSpace) {
            marla_killClientRequest(req, "Request target is too long, so no valid request.\n");
            return -1;
        }
        if(!foundSpace) {
            // No space found in the fragment found, so incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }
        memset(req->uri + strlen(req->uri), 0, sizeof(req->uri) - strlen(req->uri));

        //printf("Found URI: %s\n", req->uri);

        req->readStage = marla_CLIENT_REQUEST_PAST_REQUEST_TARGET;
    }

    if(req->readStage == marla_CLIENT_REQUEST_PAST_REQUEST_TARGET) {
        while(1) {
            unsigned char c;
            int nread = marla_Connection_read(cxn, &c, 1);
            if(nread <= 0) {
                return -1;
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
        const char* expected = "HTTP/1.1";
        size_t versionLen = strlen(expected);
        unsigned char givenVersion[8];
        memset(givenVersion, 0, sizeof(givenVersion));
        size_t nread = marla_Connection_read(cxn, givenVersion, versionLen + 1);
        if(nread <= 0) {
            // Error.
            return -1;
        }
        if(nread < versionLen + 1) {
            // Incomplete.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }

        // Validate.
        for(int i = 0; i < versionLen; ++i) {
            if(givenVersion[i] != expected[i]) {
                marla_killClientRequest(req, "Request version is unknown, so no valid request.\n");
                return -1;
            }
        }

        if(givenVersion[versionLen] == '\r') {
            unsigned char c;
            size_t nwritten = marla_Connection_read(cxn, &c, 1);
            if(nwritten < 1) {
                marla_Connection_putbackRead(cxn, nread);
                return -1;
            }
            if(c != '\n') {
                marla_killClientRequest(req, "Unterminated request line.\n");
                return -1;
            }
        }
        else if(givenVersion[versionLen] != '\n') {
            marla_killClientRequest(req, "Unterminated request line.\n");
            return -1;
        }

        marla_Server_invokeHook(cxn->server, marla_SERVER_HOOK_ROUTE, req);

        req->readStage = marla_CLIENT_REQUEST_READING_FIELD;
    }

    return 0;
}

static int marla_processTrailer(marla_ClientRequest* req)
{
    marla_Connection* cxn = req->cxn;
    while(req->readStage == marla_CLIENT_REQUEST_READING_TRAILER) {
        char fieldLine[MAX_FIELD_NAME_LENGTH + 2 + MAX_FIELD_VALUE_LENGTH + 2];
        memset(fieldLine, 0, sizeof(fieldLine));
        int nread = marla_Connection_read(cxn, (unsigned char*)fieldLine, sizeof(fieldLine));
        if(nread <= 0) {
            // Error.
            return -1;
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
            if(i < nread - 1 && fieldLine[i] == '\r' && fieldLine[i + 1] == '\n') {
                fieldLine[i] = 0;
                foundNewline = 1;
                marla_Connection_putbackRead(cxn, nread - i - 2);
                break;
            }
            char c = fieldLine[i];
            if(c <= 0x1f || c == 0x7f) {
                marla_killClientRequest(req, "Header line contains control characters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%' || c == '"')) {
                marla_killClientRequest(req, "Header name contains delimiters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                marla_killClientRequest(req, "Header name contains unwise characters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && !isalnum(c) && c != '-') {
                if(c == ':' && i > 0 && i < nread - 1) {
                    foundSeparator = i;
                    fieldLine[i] = 0;
                    toleratingSpaces = 1;
                    continue;
                }
                marla_killClientRequest(req, "Header name contains non alphanumeric characters, so no valid request.\n");
                return -1;
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
            marla_killClientRequest(req, "Request version is too long, so no valid request.\n");
            return -1;
        }
        if(!foundNewline) {
            // Incomplete;
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }
        if(foundSeparator && fieldValue) {
            char* fieldName = fieldLine;
            // Header found.
            if(req->handle) {
                req->handle(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
            }
            continue;
        }
        else if(fieldLine[0] == 0) {
            // Empty line. End of trailers.
            req->readStage = marla_CLIENT_REQUEST_DONE_READING;
            break;
        }
    }
    return 0;
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

int marla_clientRead(marla_Connection* cxn)
{
    //fprintf(stderr, "marla_clientRead\n");
    marla_clientAccept(cxn);

    // Read in backend requests.
    if(cxn->stage == marla_BACKEND_READY) {
        return marla_backendRead(cxn);
    }

    marla_ClientRequest* req = 0;
    if(!cxn->current_request) {
        unsigned char c;
        int nread = marla_Connection_read(cxn, &c, 1);
        if(nread <= 0) {
            return -1;
        }
        marla_Connection_putbackRead(cxn, 1);

        // No request yet made.
        req = marla_ClientRequest_new(cxn);
        cxn->current_request = req;
        cxn->latest_request = req;
        ++cxn->requests_in_process;
    }
    else {
        req = cxn->latest_request;
    }

    if(0 != marla_processStatusLine(req)) {
        return -1;
    }

    if(0 != marla_processClientFields(req)) {
        return -1;
    }

    if(req->readStage == marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE || req->readStage == marla_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE) {
        return 0;
    }

    if(req->readStage == marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
        while(req->contentLen != 0) {
            // Read request body.
            char buf[marla_BUFSIZE];
            memset(buf, 0, sizeof(buf));
            int requestedLen = req->contentLen;
            if(requestedLen > sizeof(buf)) {
                requestedLen = sizeof(buf);
            }
            int nread = marla_Connection_read(cxn, (unsigned char*)buf, requestedLen);
            if(nread == 0) {
                // Zero-length read indicates end of stream.
                if(req->contentLen > 0) {
                    marla_killClientRequest(req, "Premature end of request body.\n");
                    return -1;
                }
                break;
            }
            if(nread < 0) {
                // Error.
                marla_killClientRequest(req, "Error while receiving request body.\n");
                return -1;
            }
            if(nread < 4 && req->contentLen > 4) {
                // A read too small.
                marla_Connection_putbackRead(cxn, nread);
                return -1;
            }

            // Handle input.
            req->totalContentLen += nread;
            req->contentLen -= nread;
            if(req->handle) {
                req->handle(req, marla_EVENT_REQUEST_BODY, buf, nread);
            }
        }
        if(req->handle) {
            req->handle(req, marla_EVENT_REQUEST_BODY, 0, 0);
        }
        req->readStage = marla_CLIENT_REQUEST_DONE_READING;
    }

    if(0 != marla_readRequestChunks(req)) {
        return -1;
    }

    if(0 != marla_processTrailer(req)) {
        return -1;
    }

    if(req->readStage == marla_CLIENT_REQUEST_WEBSOCKET) {
        int continueCalling = 0;
        if(req->handle) {
            req->handle(req, marla_EVENT_READ, &continueCalling, 0);
        }
        return continueCalling;
    }
    if(req->readStage == marla_CLIENT_REQUEST_DONE_READING) {
        return 0;
    }
    else {
        marla_killClientRequest(req, "Unexpected request stage.\n");
        return -1;
    }
    if(cxn->stage == marla_CLIENT_COMPLETE && !cxn->shouldDestroy) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
        return -1;
    }
}

int marla_clientWrite(marla_Connection* cxn)
{
    //fprintf(stderr, "marla_clientWrite\n");

    // Read in backend requests.
    if(cxn->stage == marla_BACKEND_READY) {
        return marla_backendWrite(cxn);
    }

    marla_Ring* output = cxn->output;

    // Write current output.
    if(marla_Ring_size(output) > 0) {
        int nflushed;
        int rv = marla_Connection_flush(cxn, &nflushed);
        if(rv <= 0) {
            return rv;
        }
    }

    marla_ClientRequest* req = cxn->current_request;
    if(req == 0) {
        return -1;
    }

    if(req->writeStage == marla_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE) {
        // Check if the handler can respond.
        if(req->handle) {
            req->handle(req, marla_EVENT_WEBSOCKET_RESPOND, 0, 0);
        }
        return 0;
    }

    if(req->writeStage < marla_CLIENT_REQUEST_WRITING_RESPONSE) {
        // Request premature; couldn't write.
        return -1;
    }

    if(marla_Ring_size(output) > marla_Ring_capacity(output) - 4) {
        // Buffer too full.
        return -1;
    }

    if(req->writeStage == marla_CLIENT_REQUEST_WRITING_CONTINUE) {
        if(req->readStage != marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE) {
            marla_killClientRequest(req, "Unexpected read stage %s.\n", marla_nameRequestReadStage(req->readStage));
            return -1;
        }
        const char* statusLine = "HTTP/1.1 100 Continue\r\n";
        size_t len = strlen(statusLine);
        int nwritten = marla_Connection_write(cxn, statusLine, len);
        if(nwritten == 0) {
            marla_killClientRequest(req, "Premature connection close.\n");
            return 0;
        }
        if(nwritten < 0) {
            marla_killClientRequest(req, "Error while writing connection.\n");
            return nwritten;

        }
        if(nwritten <= len) {
            // Only allow writes of the whole thing.
            marla_Connection_putbackWrite(cxn, nwritten);
            return -1;
        }

        if(req->contentLen == -1) {
            req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
        }
        else if(req->contentLen == 0) {
            req->readStage = marla_CLIENT_REQUEST_DONE_READING;
        }
        else if(req->contentLen > 0) {
            req->readStage = marla_CLIENT_REQUEST_READING_REQUEST_BODY;
        }

        req->writeStage = marla_CLIENT_REQUEST_WRITING_RESPONSE;
    }

    if(req->writeStage == marla_CLIENT_REQUEST_WRITING_UPGRADE) {
        char out[1024];
        int nwrit = snprintf(out, sizeof(out), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", req->websocket_accept);
        int nwritten = marla_Connection_write(cxn, out, nwrit);
        if(nwritten == 0) {
            marla_killClientRequest(req, "Premature connection close.\n");
            return -1;
        }
        if(nwritten < 0) {
            marla_killClientRequest(req, "Error while writing connection.\n");
            return -1;

        }
        if(nwritten < nwrit) {
            // Only allow writes of the whole thing.
            marla_Connection_putbackWrite(cxn, nwritten);
            return -1;
        }

        // Allow the handler to change when WebSocket is going to be used.
        marla_Server_invokeHook(cxn->server, marla_SERVER_HOOK_WEBSOCKET, req);

        req->readStage = marla_CLIENT_REQUEST_WEBSOCKET;
        req->writeStage = marla_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE;
        fprintf(stderr, "Going websocket\n");
        return 0;
    }

    while(req->writeStage == marla_CLIENT_REQUEST_WRITING_RESPONSE) {
        int choked = 0;
        if(req->handle) {
            req->handle(req, marla_EVENT_RESPOND, &choked, 0);
        }

        // Write current output.
        if(marla_Ring_size(output) > 0) {
            int nflushed;
            int rv = marla_Connection_flush(cxn, &nflushed);
            if(rv <= 0) {
                //fprintf(stderr, "Responder choked.\n");
                return rv;
            }
        }
        else if(!req->handle) {
            req->writeStage = marla_CLIENT_REQUEST_DONE_WRITING;
        }
        else if(choked) {
            return -1;
        }
    }

    if(req->writeStage == marla_CLIENT_REQUEST_DONE_WRITING) {
        //fprintf(stderr, "Done writing!\n");
        // Write current output.
        while(marla_Ring_size(cxn->output) > 0) {
            int nflushed;
            int rv = marla_Connection_flush(cxn, &nflushed);
            if(rv <= 0) {
                return rv;
            }
        }

        if(cxn->current_request == cxn->latest_request) {
            cxn->current_request = 0;
            cxn->latest_request = 0;
        }
        else {
            cxn->current_request = req->next_request;
        }
        if(req->close_after_done) {
            //fprintf(stderr, "CLOSING AFTER DONE.\n");
            cxn->stage = marla_CLIENT_COMPLETE;
        }
        marla_ClientRequest_destroy(req);
        --cxn->requests_in_process;
        if(cxn->stage != marla_CLIENT_COMPLETE) {
            return 0;
        }
    }

    if(cxn->stage == marla_CLIENT_COMPLETE && !cxn->shouldDestroy) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
        return -1;
    }
    return 0;
}
