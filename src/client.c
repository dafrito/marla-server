#include "marla.h"
#include <ctype.h>
#include <endian.h>

void marla_closeWebSocketRequest(marla_Request* req, uint16_t closeCode, const char* reason, size_t reasonLen)
{
    if(req->needWebSocketClose || req->doingWebSocketClose) {
        return;
    }
    memcpy(req->websocket_closeReason, reason, reasonLen);
    req->websocket_closeReasonLen = reasonLen;
    req->websocket_closeCode = closeCode;
    req->needWebSocketClose = 1;
    if(req->handler) {
        req->handler(req, marla_EVENT_WEBSOCKET_CLOSING, &closeCode, 2);
        req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, (void*)reason, reasonLen);
        req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, 0, 0);
    }
    marla_clientWrite(req->cxn);
}

const char* marla_nameClientEvent(enum marla_ClientEvent ev)
{
    switch(ev) {
    case marla_BACKEND_EVENT_NEED_HEADERS:
        return "BACKEND_EVENT_NEED_HEADERS";
    case marla_BACKEND_EVENT_RESPONSE_BODY:
        return "BACKEND_EVENT_RESPONSE_BODY";
    case marla_BACKEND_EVENT_HEADER:
        return "BACKEND_EVENT_HEADER";
    case marla_BACKEND_EVENT_MUST_READ:
        return "BACKEND_EVENT_MUST_READ";
    case marla_BACKEND_EVENT_MUST_WRITE:
        return "BACKEND_EVENT_MUST_WRITE";
    case marla_BACKEND_EVENT_NEED_TRAILERS:
        return "BACKEND_EVENT_NEED_TRAILERS";
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
    return "";
}

/*
 *  %x0 denotes a continuation frame
 *  %x1 denotes a text frame
 *  %x2 denotes a binary frame
 *  %x3-7 are reserved for further non-control frames
 *  %x8 denotes a connection close
 *  %x9 denotes a ping
 *  %xA denotes a pong
 *  %xB-F are reserved for further control frames
 */
int marla_writeWebSocketHeader(struct marla_Request* req, unsigned char opcode, uint64_t frameLen)
{
    unsigned char out[7];
    int outlen = 2;

    // Generate the header.
    if(frameLen > 65535) {
        memcpy(out + outlen, &frameLen, sizeof frameLen);
        outlen += sizeof frameLen;
    }
    else if(frameLen > 125) {
        uint16_t fl = (uint16_t)frameLen;
        memcpy(out + outlen, &fl, sizeof fl);
        outlen += sizeof fl;
    }
    else {
        out[0] = (1 << 7) | (opcode % 16);
        out[1] = (unsigned char)frameLen;
    }

    // Write the header.
    int nread = marla_Connection_write(req->cxn, out, outlen);
    if(nread <= 0) {
        return nread;
    }
    if(nread < outlen) {
        marla_Connection_putbackWrite(req->cxn, nread);
        return -1;
    }
    return outlen;
}

int marla_writeWebSocket(struct marla_Request* req, unsigned char* data, int dataLen)
{
    int nwritten = marla_Connection_write(req->cxn, data, dataLen);
    if(nwritten > 0) {
        req->websocketFrameWritten += nwritten;
    }
    return nwritten;
}

int marla_readWebSocket(struct marla_Request* req, unsigned char* data, int dataLen)
{
    if(dataLen > req->websocketFrameLen) {
        dataLen = req->websocketFrameLen;
    }
    int nread = marla_Connection_read(req->cxn, data, dataLen);
    if(nread <= 0) {
        return nread;
    }
    if(req->websocketMask) {
        // Unmask the data.
        for(int i = 0; i < nread; ++i) {
            data[i] = data[i] ^ req->websocketMask[(req->websocketFrameRead + i) % 4];
        }
    }
    req->websocketFrameRead += nread;
    return nread;
}

int marla_WebSocketRemaining(struct marla_Request* req)
{
    return req->websocketFrameLen - req->websocketFrameRead;
}

void marla_putbackWebSocketRead(struct marla_Request* req, int dataLen)
{
    marla_Connection_putbackRead(req->cxn, dataLen);
    req->websocketFrameRead -= dataLen;
}

void marla_putbackWebSocketWrite(struct marla_Request* req, int dataLen)
{
    marla_Connection_putbackWrite(req->cxn, dataLen);
    req->websocketFrameWritten -= dataLen;
}

static int marla_processClientFields(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;
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
            if(fieldLine[i] == '\r') {
                if(i >= nread - 1) {
                    marla_Connection_putbackRead(cxn, nread);
                    return -1;
                }
                if(fieldLine[i + 1] != '\n') {
                    marla_killRequest(req, "Header line is not terminated properly, so no valid request.");
                    return 1;
                }
                fieldLine[i] = 0;
                foundNewline = 1;
                marla_Connection_putbackRead(cxn, nread - i - 2);
                break;
            }
            char c = fieldLine[i];
            if(c <= 0x1f || c == 0x7f) {
                marla_killRequest(req, "Header line contains control characters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%' || c == '"')) {
                marla_killRequest(req, "Header name contains delimiters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                marla_killRequest(req, "Header name contains unwise characters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && !isalnum(c) && c != '-') {
                if(c == ':' && i > 0 && i < nread - 1) {
                    foundSeparator = i;
                    fieldLine[i] = 0;
                    toleratingSpaces = 1;
                    continue;
                }
                marla_killRequest(req, "Header name contains non alphanumeric characters, so no valid request.\n");
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
            marla_killRequest(req, "Request version is too long, so no valid request.\n");
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
                if(req->givenContentLen != marla_MESSAGE_LENGTH_UNKNOWN) {
                    marla_killRequest(req, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    return -1;
                }
                char* endptr;
                long int parsedLen = strtol(fieldValue, &endptr, 10);
                if(*endptr != '\0' || parsedLen < 0) {
                    marla_killRequest(req, "Content-Length header value could not be read, so no valid request.\n");
                    return -1;
                }
                req->givenContentLen = parsedLen;
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
                if(req->givenContentLen != marla_MESSAGE_LENGTH_UNKNOWN) {
                    marla_killRequest(req, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    return -1;
                }

                if(!strcasecmp(fieldValue, "chunked")) {
                    req->givenContentLen = marla_MESSAGE_IS_CHUNKED;
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
                        req->givenContentLen = marla_MESSAGE_USES_CLOSE;
                        req->close_after_done = 1;
                        marla_logMessage(req->cxn->server, "Request will close once done.");
                    }
                    else if(!strcasecmp(fieldToken, "Upgrade")) {
                        req->expect_upgrade = 1;
                    }
                    else if(strcasecmp(fieldToken, "keep-alive")) {
                        marla_killRequest(req, "Connection is not understood, so no valid request.\n");
                        return -1;
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
                else {
                    marla_killRequest(req, "Unexpected WebSocket version");
                }
            }
            else if(!strcmp(fieldName, "Accept")) {
                strncpy(req->acceptHeader, fieldValue, MAX_FIELD_VALUE_LENGTH);
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
                    return -1;
                }
            }
            else if(isascii(req->uri[0])) {
                char* schemeSep = strstr(req->uri, "://");
                if(schemeSep != 0) {
                    for(char* c = req->uri; c != schemeSep; ++c) {
                        if(!isascii(*c)) {
                            marla_killRequest(req, "Scheme invalid, so no valid request.\n");
                            return -1;
                        }
                    }
                    *schemeSep = 0;
                    if(!strcmp("http", req->uri)) {
                        marla_killRequest(req, "HTTP scheme unsupported, so no valid request.\n");
                        return -1;
                    }
                    else if(!strcmp("https", req->uri)) {
                        *schemeSep = ':';
                    }
                    else {
                        marla_killRequest(req, "Request scheme unrecognized.\n");
                        return -1;
                    }
                }
                char* hostPart = schemeSep + 3;
                char* hostSep = strstr(hostPart, "/");
                if(hostSep - hostPart >= MAX_FIELD_VALUE_LENGTH) {
                    marla_killRequest(req, "Host too long.\n");
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
                        marla_killRequest(req, "Host differs from absolute URI's host.\n");
                        return -1;
                    }
                    strncpy(req->host, hostPart, MAX_FIELD_VALUE_LENGTH);
                    *hostSep = '/';

                    // Transform an absolute URI into a origin form
                    memmove(req->uri, hostSep, strlen(hostSep));
                }
            }
            else {
                marla_killRequest(req, "Request target unrecognized.\n");
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
                    marla_killRequest(req, "WebSocket key must be exactly 24 bytes.");
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
            if(req->handler) {
                req->handler(req, marla_EVENT_ACCEPTING_REQUEST, &accept, 0);
            }
            if(!accept) {
                marla_killRequest(req, "Request explicitly rejected.\n");
                return -1;
            }

            marla_logMessagecf(req->cxn->server, "URL Requests", "Handling request for %s", req->uri);

            if(req->writeStage != marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT) {
                marla_killRequest(req, "Request explicitly rejected.\n");
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
                switch(req->givenContentLen) {
                case marla_MESSAGE_IS_CHUNKED:
                    req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
                    marla_logMessage(server, "Reading request chunks");
                    break;
                case 0:
                case marla_MESSAGE_LENGTH_UNKNOWN:
                    if(req->expect_trailer) {
                        req->readStage = marla_CLIENT_REQUEST_READING_TRAILER;
                        marla_logMessage(server, "Reading trailer");
                    }
                    else {
                        req->readStage = marla_CLIENT_REQUEST_DONE_READING;
                        marla_logMessage(server, "Done reading");
                    }
                    break;
                case marla_MESSAGE_USES_CLOSE:
                    req->close_after_done = 1;
                    // Fall through.
                default:
                    req->readStage = marla_CLIENT_REQUEST_READING_REQUEST_BODY;
                    req->remainingContentLen = req->givenContentLen;
                    marla_logMessagef(server, "Reading request body of length %d", req->givenContentLen);
                    break;
                }
                if(req->givenContentLen > 0) {
                    req->totalContentLen = req->givenContentLen;
                    req->remainingContentLen = req->givenContentLen;
                }
                req->writeStage = marla_CLIENT_REQUEST_WRITING_RESPONSE;
            }

            break;
        }
    }
    return 0;
}

static int marla_readRequestChunks(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
read_chunk_size:
    while(req->readStage == marla_CLIENT_REQUEST_READING_CHUNK_SIZE) {
        char buf[marla_MAX_CHUNK_SIZE_LINE];
        memset(buf, 0, sizeof(buf));
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates premature end of stream.
            marla_killRequest(req, "Premature end of chunked request body.\n");
            return -1;
        }
        if(nread < 0) {
            // Error.
            marla_killRequest(req, "Error %d while receiving request body.\n", nread);
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
            else if(foundHexDigit && c == '\r') {
                if(i >= nread - 1) {
                    marla_Connection_putbackRead(cxn, nread);
                    return -1;
                }
                if(buf[i + 1] != '\n') {
                    marla_killRequest(req, "Chunk is not terminated properly.");
                    return -1;
                }
                // Size-body separator.
                foundEnd = 1;
                buf[i] = 0;
                marla_Connection_putbackRead(cxn, nread - (i + 2));
                break;
            }
            else {
                // Garbage.
                marla_killRequest(req, "Error while receiving chunk size.\n");
                return -1;
            }
        }

        if(!foundEnd) {
            if(nread >= marla_MAX_CHUNK_SIZE_LINE) {
                marla_killRequest(req, "Chunk size line too long.\n");
                return -1;
            }

            // Incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }

        if(!foundHexDigit) {
            marla_killRequest(req, "Failed to find any hex digits in chunk size.\n");
            return -1;
        }

        char* endptr = 0;
        long int chunkSize = strtol(buf, &endptr, 16);
        if(*endptr != 0) {
            marla_killRequest(req, "Error while parsing chunk size.\n");
            return -1;
        }
        if(chunkSize < 0 || chunkSize > marla_MAX_CHUNK_SIZE) {
            marla_killRequest(req, "Request chunk size is out of range.\n");
            return -1;
        }
        req->chunkSize = chunkSize;
        req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_BODY;
        if(req->chunkSize == 0) {
            if(req->handler) {
                req->handler(req, marla_EVENT_REQUEST_BODY, 0, 0);
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
                    marla_killRequest(req, "Premature end of request chunk body.\n");
                    return -1;
                }
                break;
            }
            if(nread < 0) {
                // Error.
                marla_killRequest(req, "Error while receiving request chunk body.\n");
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
            if(req->handler) {
                req->handler(req, marla_EVENT_REQUEST_BODY, buf, nread);
            }
        }

        // Consume trailing EOL
        char buf[2];
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates end of stream.
            marla_killRequest(req, "Premature end of request chunk body.\n");
            return -1;
        }
        if(nread < 0) {
            // Error.
            marla_killRequest(req, "Error while receiving request chunk body.\n");
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
            marla_killRequest(req, "Error while receiving request chunk body.\n");
            return -1;
        }
        req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
        goto read_chunk_size;
    }

    return 0;
}

static int marla_processStatusLine(marla_Request* req)
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
                marla_killRequest(req, "Request line contains control characters, so no valid request.\n");
                return -1;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                marla_killRequest(req, "Request line contains delimiters, so no valid request.\n");
                return -1;
            }
            if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                marla_killRequest(req, "Request line contains unwise characters, so no valid request.\n");
                return -1;
            }
            if(c == ' ') {
                marla_Connection_putbackRead(cxn, nread - i);
                req->method[i] = 0;
                foundSpace = 1;
                break;
            }
            if(!isascii(c)) {
                marla_killRequest(req, "Request method contains non-ASCII characters, so no valid request.\n");
                return -1;
            }
        }
        if(nread == MAX_METHOD_LENGTH + 1 && !foundSpace) {
            marla_killRequest(req, "Request method is too long, so no valid request.\n");
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
            marla_killRequest(req, "Request method '%s' is unknown, so no valid request.\n", req->method);
            return -1;
        }

        marla_logMessagef(req->cxn->server, "Found method: %s", req->method);
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
                marla_killRequest(req, "Request target contains control characters, so no valid request.\n");
                return -1;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                marla_killRequest(req, "Request target contains delimiters, so no valid request.\n");
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
            marla_killRequest(req, "Request target is too long, so no valid request.\n");
            return -1;
        }
        if(!foundSpace) {
            // No space found in the fragment found, so incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return -1;
        }
        memset(req->uri + strlen(req->uri), 0, sizeof(req->uri) - strlen(req->uri));

        marla_logMessagef(req->cxn->server, "Found URI: %s", req->uri);

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
                marla_killRequest(req, "Request version is unknown, so no valid request.\n");
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
                marla_killRequest(req, "Unterminated request line.\n");
                return -1;
            }
        }
        else if(givenVersion[versionLen] != '\n') {
            marla_killRequest(req, "Unterminated request line.\n");
            return -1;
        }

        marla_Server_invokeHook(cxn->server, marla_SERVER_HOOK_ROUTE, req);

        req->readStage = marla_CLIENT_REQUEST_READING_FIELD;
    }

    return 0;
}

static int marla_processTrailer(marla_Request* req)
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
            if(fieldLine[i] == '\r' && fieldLine[i + 1] == '\n') {
                if(i >= nread - 1) {
                    marla_Connection_putbackRead(cxn, nread);
                    return -1;
                }
                if(fieldLine[i + 1] != '\n') {
                    marla_killRequest(req, "Trailer line is not terminated properly, so no valid request.");
                    return 1;
                }
                fieldLine[i] = 0;
                foundNewline = 1;
                marla_Connection_putbackRead(cxn, nread - i - 2);
                break;
            }
            char c = fieldLine[i];
            if(c <= 0x1f || c == 0x7f) {
                marla_killRequest(req, "Header line contains control characters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%')) {
                marla_killRequest(req, "Header name contains delimiters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                marla_killRequest(req, "Header name contains unwise characters, so no valid request.\n");
                return -1;
            }
            if(!foundSeparator && !isalnum(c) && c != '-') {
                if(c == ':' && i > 0 && i < nread - 1) {
                    foundSeparator = i;
                    fieldLine[i] = 0;
                    toleratingSpaces = 1;
                    continue;
                }
                marla_killRequest(req, "Header name contains non alphanumeric characters, so no valid request.\n");
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
            marla_killRequest(req, "Request version is too long, so no valid request.\n");
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
            if(req->handler) {
                req->handler(req, marla_EVENT_HEADER, fieldName, fieldValue - fieldName);
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
    marla_clientAccept(cxn);

    // Read in backend requests.
    if(cxn->is_backend) {
        return marla_backendRead(cxn);
    }

    if(cxn->in_read) {
        marla_logMessagecf(cxn->server, "Processing", "Client connection asked to read, but already reading.");
        return -1;
    }
    cxn->in_read =  1;

    marla_logEntercf(cxn->server, "Processing", "Reading from client connection");
    marla_Request* req = 0;
    marla_Server* server = cxn->server;
    if(!cxn->current_request) {
        unsigned char c;
        int nread = marla_Connection_read(cxn, &c, 1);
        if(nread <= 0) {
            cxn->in_read = 0;
            marla_logLeave(server, 0);
            return -1;
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

    if(req->is_backend) {
        marla_die(cxn->server, "Backend request found its way in client connection processing.");
    }

    marla_logMessagecf(cxn->server, "Client processing", "clientRead: %s", marla_nameRequestReadStage(req->readStage));

    if(0 != marla_processStatusLine(req)) {
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return -1;
    }

    if(0 != marla_processClientFields(req)) {
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return -1;
    }

    if(req->readStage == marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE || req->readStage == marla_CLIENT_REQUEST_AWAITING_UPGRADE_WRITE) {
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return 0;
    }

    while(req->readStage == marla_CLIENT_REQUEST_WEBSOCKET) {
        unsigned char buf[marla_BUFSIZE + 1];
        int nread;
        memset(buf, 0, sizeof buf);

        if(req->websocketFrameLen == 0) {
            nread = marla_Connection_read(req->cxn, req->websocket_frame, sizeof(req->websocket_frame));
            if(nread < 2) {
                if(nread > 0) {
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    marla_Connection_putbackRead(req->cxn, nread);
                }
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return -1;
            }

            if(req->websocket_frame[0] << 1 == req->websocket_frame[0]) {
                // The FIN bit was zero.
                req->websocket_fin = 0;
            }
            else {
                // The FIN bit was nonzero.
                req->websocket_fin = 1;
            }

            for(int i = 1; i < 4; ++i) {
                unsigned char c = req->websocket_frame[0] << 1;
                if(c << i == c) {
                    // A reserved bit was zero.
                    marla_killRequest(req, "A reserved bit was zero.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
                }
            }

            switch(req->websocket_frame[0] % 16) {
            case 0:
                // Continuation frame.
                break;
            case 1:
                // Text frame.
                break;
            case 2:
                // Binary frame.
                break;
            case 8:
                // Close frame.
                break;
            case 9:
                // Ping frame.
                break;
            case 10:
                // Pong frame.
                break;
            default:
                // Reserved opcode.
                marla_killRequest(req, "Reserved opcode");
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return 1;
            }
            req->websocket_type = req->websocket_frame[0] % 16;

            unsigned char mask;
            if(req->websocket_frame[1] << 1 == req->websocket_frame[1]) {
                // The Mask bit was zero.
                mask = 0;
            }
            else {
                // The Mask bit was nonzero.
                mask = 1;
            }

            // Get the payload length.
            uint64_t payload_len = (unsigned char)(req->websocket_frame[1] << 1) >> 1;
            if(payload_len == 126) {
                if(req->websocket_type < 0 || req->websocket_type > 2) {
                    marla_killRequest(req, "WebSocket type unrecognized");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
                }
                if(nread < 4) {
                    marla_Connection_putbackRead(req->cxn, nread);
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                payload_len = be16toh(*(uint16_t*)(req->websocket_frame + 3));
            }
            else if(payload_len == 127) {
                if(req->websocket_type < 0 || req->websocket_type > 2) {
                    marla_killRequest(req, "WebSocket type unrecognized");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
                }
                if(nread < 10) {
                    marla_Connection_putbackRead(req->cxn, nread);
                    memset(req->websocket_frame, 0, sizeof req->websocket_frame);
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                payload_len = be64toh(*(uint64_t*)(req->websocket_frame + 3));
            }
            else if(nread > 2) {
                marla_Connection_putbackRead(req->cxn, nread - 2);
                // Payload length is correct as-is.
            }

            // Read the mask.
            if(mask) {
                nread = marla_Connection_read(req->cxn, (unsigned char*)req->websocketMask, 4);
                if(nread < 4) {
                    if(nread > 0) {
                        marla_Connection_putbackRead(req->cxn, nread);
                    }
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
            }

            // Save the frame length.
            req->websocketFrameLen = payload_len;
            req->websocketFrameRead = 0;
        }

        while(req->websocketFrameLen > req->websocketFrameRead) {
            switch(req->websocket_type) {
            case 8:
                // Close frame.
                if(req->websocketFrameRead == 0) {
                    unsigned char code[2];
                    nread = marla_readWebSocket(req, code, 2);
                    if(nread < 2) {
                        if(nread > 0) {
                            marla_Connection_putbackRead(req->cxn, nread);
                        }
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
                        return -1;
                    }
                    if(req->handler) {
                        req->handler(req, marla_EVENT_WEBSOCKET_CLOSING, code, be16toh(*(uint16_t*)code));
                    }
                }
                if(marla_WebSocketRemaining(req) == 0) {
                    if(req->handler) {
                        req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, 0, 0);
                    }
                    req->needWebSocketClose = 1;
                    break;
                }

                unsigned char* closeReasonWritten = req->websocket_closeReason + req->websocketFrameRead - 2;
                nread = marla_readWebSocket(req, req->websocket_closeReason + req->websocketFrameRead - 2, req->websocketFrameLen - req->websocketFrameRead - 2);
                if(nread <= 0) {
                    if(req->handler && nread == 0) {
                        req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, 0, 0);
                    }
                    req->needWebSocketClose = 1;
                    break;
                }
                else if(req->handler) {
                    req->handler(req, marla_EVENT_WEBSOCKET_CLOSE_REASON, closeReasonWritten, nread);
                }
                break;
            case 9:
                // Ping frame.
                nread = marla_readWebSocket(req, req->websocket_ping + req->websocketFrameRead, req->websocketFrameLen - req->websocketFrameRead);
                if(nread <= 0) {
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                break;
            case 10:
                // Pong frame.
                if(req->websocket_pongLen != req->websocketFrameLen) {
                    marla_killRequest(req, "Pong mismatch");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
                }
                nread = marla_readWebSocket(req, buf, sizeof buf);
                if(nread <= 0) {
                    return -1;
                }
                for(int i = 0; i < nread; ++i) {
                    if(req->websocket_pong[i + req->websocketFrameRead] != buf[i]) {
                        // Pong mismatch
                        marla_killRequest(req, "Pong mismatch");
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
                        return 1;
                    }
                }
                break;
            default:
                // Data frame.
                nread = marla_readWebSocket(req, buf, sizeof buf);
                if(nread <= 0) {
                    if(nread == 0) {
                        if(req->handler) {
                            req->handler(req, marla_EVENT_WEBSOCKET_MUST_READ, 0, 0);
                        }
                    }
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 0;
                }
                if(req->handler) {
                    req->handler(req, marla_EVENT_WEBSOCKET_MUST_READ, buf, nread);
                    if(marla_WebSocketRemaining(req) == 0) {
                        req->handler(req, marla_EVENT_WEBSOCKET_MUST_READ, 0, 0);
                    }
                }
            }
        }

        req->websocketFrameLen = 0;
        req->websocketFrameRead = 0;
    }

    if(req->readStage == marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
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
                    marla_killRequest(req, "Premature end of request body.\n");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return 1;
                }
                break;
            }
            if(nread < 0) {
                return -1;
            }
            if(nread < 4 && req->remainingContentLen > 4) {
                // A read too small.
                marla_Connection_putbackRead(cxn, nread);
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return -1;
            }

            // Handle input.
            req->totalContentLen += nread;
            req->remainingContentLen -= nread;
            if(req->handler) {
                req->handler(req, marla_EVENT_REQUEST_BODY, buf, nread);
            }
        }
        if(req->handler) {
            req->handler(req, marla_EVENT_REQUEST_BODY, 0, 0);
        }
        req->readStage = marla_CLIENT_REQUEST_DONE_READING;
    }

    if(0 != marla_readRequestChunks(req)) {
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return -1;
    }

    if(req->backendPeer) {
        marla_backendWrite(req->backendPeer->cxn);
    }

    if(0 != marla_processTrailer(req)) {
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return -1;
    }

    if(req->readStage == marla_CLIENT_REQUEST_DONE_READING) {
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        marla_clientWrite(req->cxn);
        return 0;
    }
    else {
        marla_killRequest(req, "Unexpected request stage.");
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return 1;
    }
    if(cxn->stage == marla_CLIENT_COMPLETE && !cxn->shouldDestroy) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
        marla_logLeave(server, 0);
        cxn->in_read = 0;
        return -1;
    }

    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return 0;
}

int marla_clientWrite(marla_Connection* cxn)
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
        if(rv <= 0) {
            return rv;
        }
    }

    if(cxn->in_write) {
        marla_logMessagecf(cxn->server, "Processing", "Called to write to client, but already writing to client.");
        return -1;
    }
    cxn->in_write = 1;
    marla_Server* server = cxn->server;

    if(marla_Ring_size(output) > marla_Ring_capacity(output) - 4) {
        // Buffer too full.
        cxn->in_write = 0;
        return -1;
    }

    marla_Request* req = cxn->current_request;
    if(!req) {
        cxn->in_write = 0;
        return 0;
    }
    marla_logEntercf(cxn->server, "Processing", "Writing to client with current request's write state: %s", marla_nameRequestWriteStage(req->writeStage));
    if(req->writeStage == marla_CLIENT_REQUEST_WRITING_CONTINUE) {
        if(req->readStage != marla_CLIENT_REQUEST_AWAITING_CONTINUE_WRITE) {
            marla_killRequest(req, "Unexpected read stage %s.", marla_nameRequestReadStage(req->readStage));
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return -1;
        }
        const char* statusLine = "HTTP/1.1 100 Continue\r\n";
        size_t len = strlen(statusLine);
        int nwritten = marla_Connection_write(cxn, statusLine, len);
        if(nwritten == 0) {
            marla_killRequest(req, "Premature connection close.");
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return 0;
        }
        if(nwritten < 0) {
            marla_killRequest(req, "Error %d while writing connection.", nwritten);
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return nwritten;

        }
        if(nwritten <= len) {
            // Only allow writes of the whole thing.
            marla_Connection_putbackWrite(cxn, nwritten);
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return -1;
        }

        if(req->givenContentLen == marla_MESSAGE_IS_CHUNKED) {
            req->readStage = marla_CLIENT_REQUEST_READING_CHUNK_SIZE;
        }
        else if(req->givenContentLen == 0) {
            req->readStage = marla_CLIENT_REQUEST_DONE_READING;
        }
        else if(req->givenContentLen > 0) {
            req->readStage = marla_CLIENT_REQUEST_READING_REQUEST_BODY;
            req->remainingContentLen = req->givenContentLen;
        }

        req->writeStage = marla_CLIENT_REQUEST_WRITING_RESPONSE;
    }

    if(req->writeStage == marla_CLIENT_REQUEST_WRITING_UPGRADE) {
        char out[1024];
        int nwrit = snprintf(out, sizeof(out), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", req->websocket_accept);
        int nwritten = marla_Connection_write(cxn, out, nwrit);
        if(nwritten == 0) {
            marla_killRequest(req, "Premature connection close.\n");
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return -1;
        }
        if(nwritten < 0) {
            marla_killRequest(req, "Error while writing connection.\n");
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return -1;

        }
        if(nwritten < nwrit) {
            // Only allow writes of the whole thing.
            marla_Connection_putbackWrite(cxn, nwritten);
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return -1;
        }

        // Allow the handler to change when WebSocket is going to be used.
        marla_Server_invokeHook(cxn->server, marla_SERVER_HOOK_WEBSOCKET, req);

        req->readStage = marla_CLIENT_REQUEST_WEBSOCKET;
        req->writeStage = marla_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE;

        marla_logMessage(server, "Going websocket");
        marla_clientRead(cxn);
    }

    while(req->writeStage == marla_CLIENT_REQUEST_WRITING_WEBSOCKET_RESPONSE) {
        // Write current output.
        if(marla_Ring_size(req->cxn->output) > 0) {
            int nflushed;
            int rv = marla_Connection_flush(req->cxn, &nflushed);
            if(rv <= 0) {
                marla_logLeave(server, "Responder choked.");
                cxn->in_write = 0;
                return rv;
            }
        }

        // Check if a close frame is needed.
        if(req->needWebSocketClose && !req->doingWebSocketClose) {
            if(marla_writeWebSocketHeader(req, 8, 2 + req->websocket_closeReasonLen) < 0) {
                marla_logLeave(server, 0);
                cxn->in_write = 0;
                return -1;
            }
            req->websocketFrameOutLen = 2 + req->websocket_closeReasonLen;
            req->websocketFrameWritten = 0;
            req->doingWebSocketClose = 1;
        }

        // Finish writing the current frame.
        if(req->websocketFrameOutLen != 0) {
            if(req->doingWebSocketClose) {
                if(req->websocketFrameWritten == 0) {
                    uint16_t closeCode = htobe16(req->websocket_closeCode);
                    int nwritten = marla_writeWebSocket(req, ((unsigned char*)&closeCode), 2);
                    if(nwritten < 2) {
                        if(nwritten > 0) {
                            marla_putbackWebSocketWrite(req, nwritten);
                        }
                        marla_logLeave(server, 0);
                        cxn->in_write = 0;
                        return -1;
                    }
                    marla_logMessagef(req->cxn->server, "Wrote close code of %d.", req->websocket_closeCode);
                    if(req->websocketFrameOutLen == req->websocketFrameWritten) {
                        marla_logMessagef(req->cxn->server, "Wrote close frame without any provided reason");
                        goto shutdown;
                    }
                }
                marla_writeWebSocket(req, req->websocket_closeReason + req->websocketFrameWritten - 2, req->websocket_closeReasonLen - req->websocketFrameWritten + 2);
                if(req->websocketFrameOutLen == req->websocketFrameWritten) {
                    goto shutdown;
                }
                marla_logLeavef(req->cxn->server, "Failed to write enter close frame");
                cxn->in_write = 0;
                return -1;
            }
            else if(req->doingPong) {
                int nwritten = marla_writeWebSocket(req, req->websocket_ping + req->websocketFrameWritten, req->websocket_pingLen - req->websocketFrameWritten);
                if(nwritten <= 0) {
                    marla_logLeave(server, 0);
                    cxn->in_write = 0;
                    return -1;
                }
                if(req->websocketFrameOutLen == req->websocketFrameWritten) {
                    req->websocket_pingLen = 0;
                    req->doingPong = 0;
                }
            }
        }

        // Check if a pong frame is needed.
        if(req->websocket_pingLen > 0) {
            req->websocketFrameWritten = 0;
            req->websocketFrameOutLen = req->websocket_pingLen;
            marla_writeWebSocketHeader(req, 10, req->websocketFrameOutLen);
            req->doingPong = 1;
        }

        // Let the handler respond.
        if(req->handler) {
            int result = -1;
            req->handler(req, marla_EVENT_WEBSOCKET_MUST_WRITE, &result, 0);
            if(result == -1) {
                marla_logLeave(server, 0);
                cxn->in_write = 0;
                return -1;
            }
        }
        else {
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return -1;
        }
    }

    for(int result = 0; req->cxn->stage != marla_CLIENT_COMPLETE && req->writeStage == marla_CLIENT_REQUEST_WRITING_RESPONSE; ) {
        // Write current output.
        if(marla_Ring_size(output) > 0) {
            int nflushed;
            int rv = marla_Connection_flush(cxn, &nflushed);
            if(rv <= 0) {
                marla_logLeave(server, "Responder choked.");
                cxn->in_write = 0;
                return rv;
            }
        }
        else if(result == -1) {
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return -1;
        }
        if(req->handler) {
            req->handler(req, marla_EVENT_MUST_WRITE, &result, 0);
            marla_logMessagef(req->cxn->server, "Handler indicated %d", result);
        }
        if(result == 1 || !req->handler) {
            marla_logMessagef(req->cxn->server, "Done writing request %d to client ", req->id);
            if(marla_Ring_size(cxn->output) > 0) {
                int nflushed;
                int rv = marla_Connection_flush(cxn, &nflushed);
                if(rv <= 0) {
                    marla_logLeave(server, "Responder choked.");
                    cxn->in_write = 0;
                    return rv;
                }
            }
            if(marla_Ring_size(cxn->output) == 0) {
                req->writeStage = marla_CLIENT_REQUEST_DONE_WRITING;
            }
        }
    }

    if(req->writeStage == marla_CLIENT_REQUEST_DONE_WRITING) {
        //fprintf(stderr, "Done writing!\n");
        // Write current output.
        while(marla_Ring_size(cxn->output) > 0) {
            int nflushed;
            int rv = marla_Connection_flush(cxn, &nflushed);
            if(rv <= 0) {
                marla_logLeave(server, 0);
                cxn->in_write = 0;
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
            marla_logLeave(server, "CLOSING AFTER DONE.");
            goto shutdown;
        }
        marla_Request_destroy(req);
        --cxn->requests_in_process;
        if(cxn->stage == marla_CLIENT_COMPLETE) {
            marla_logLeave(server, 0);
            cxn->in_write = 0;
            return 0;
        }
        cxn->in_write = 0;
        marla_clientRead(cxn);
    }
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return 0;

shutdown:
    cxn->stage = marla_CLIENT_COMPLETE;
    if(!cxn->shouldDestroy) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
        marla_logLeave(server, 0);
        cxn->in_write = 0;
        return -1;
    }
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return 1;
}
