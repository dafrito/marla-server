#include "marla.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>
#include <ctype.h>

static int describeSource(marla_Connection* cxn, char* sink, size_t len)
{
    marla_BackendSource* cxnSource = cxn->source;
    memset(sink, 0, len);
    snprintf(sink, len, "FD %d backend", cxnSource->fd);
    return 0;
}

static int readSource(marla_Connection* cxn, void* sink, size_t len)
{
    marla_BackendSource* cxnSource = cxn->source;
    return read(cxnSource->fd, sink, len);
}

static int writeSource(marla_Connection* cxn, void* source, size_t len)
{
    marla_BackendSource* cxnSource = cxn->source;
    return write(cxnSource->fd, source, len);
}

static void acceptSource(marla_Connection* cxn)
{
    // Accepted and secured.
    cxn->stage = marla_BACKEND_READY;
}

static int shutdownSource(marla_Connection* cxn)
{
    return 1;
}

static void destroySource(marla_Connection* cxn)
{
    marla_BackendSource* source = cxn->source;
    close(source->fd);
    free(source);

    cxn->source = 0;
    cxn->readSource = 0;
    cxn->writeSource = 0;
    cxn->acceptSource = 0;
    cxn->shutdownSource = 0;
    cxn->destroySource = 0;
    cxn->describeSource = 0;
}

struct marla_BackendResponder* marla_BackendResponder_new(size_t bufSize, marla_Request* req)
{
    if(!req) {
        fprintf(stderr, "Backend request must be given\n");
        abort();
    }
    marla_BackendResponder* resp = malloc(sizeof *resp);
    resp->input = marla_Ring_new(bufSize);
    resp->output = marla_Ring_new(bufSize);
    resp->handler = 0;
    resp->handleStage = 0;
    resp->index = 0;
    resp->req = req;
    return resp;
}

int marla_BackendResponder_flushOutput(marla_BackendResponder* resp)
{
    marla_Request* req = resp->req->backendPeer;
    int total = 0;
    for(;;) {
        void* data;
        size_t len;
        marla_Ring_readSlot(resp->output, &data, &len);
        if(len == 0) {
            break;
        }
        if(len > 0) {
            int true_written = marla_Connection_write(req->cxn, data, len);
            marla_logMessagef(req->cxn->server, "Flushed %d bytes from backend to client", true_written);
            total += true_written;
            if(true_written < len) {
                marla_Ring_putbackRead(resp->output, len - true_written);
                return total;
            }
        }
    }
    return total;
}

void marla_Backend_init(marla_Connection* cxn, int fd)
{
    marla_BackendSource* source = malloc(sizeof *source);
    cxn->source = source;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->acceptSource = acceptSource;
    cxn->shutdownSource = shutdownSource;
    cxn->destroySource = destroySource;
    cxn->describeSource = describeSource;
    source->fd = fd;
}

void marla_Backend_enqueue(marla_Connection* cxn, marla_Request* req)
{
    req->cxn = cxn;
    req->readStage = marla_BACKEND_REQUEST_READING_RESPONSE_LINE;
    req->writeStage = marla_BACKEND_REQUEST_WRITING_REQUEST_LINE;
    req->is_backend = 1;

    if(!cxn->current_request) {
        cxn->current_request = req;
        cxn->latest_request = req;
    }
    else {
        cxn->latest_request->next_request = req;
        cxn->latest_request = req;
    }

    marla_backendWrite(cxn);
    int nflushed;
    marla_Connection_flush(cxn, &nflushed);
    marla_backendRead(cxn);
    marla_Connection_flush(cxn, &nflushed);
}

int marla_backendWrite(marla_Connection* cxn)
{
    marla_Server* server = cxn->server;
    marla_Request* req = cxn->current_request;
    char out[marla_BUFSIZE];
    while(req) {
        if(!req->is_backend) {
            marla_die(server, "Client request found its way into the backend's queue.");
        }
        marla_logMessagef(cxn->server, "Reading req: %s\n", marla_nameRequestWriteStage(req->writeStage));
        // Write request line to backend.
        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_REQUEST_LINE) {
            if(req->method[0] == 0) {
                marla_killRequest(req, "Method must be provided");
                return -1;
            }
            if(req->uri[0] == 0) {
                marla_killRequest(req, "URI must be provided");
                return -1;
            }
            int nwrit = snprintf(out, sizeof(out), "%s %s HTTP/1.1\r\n", req->method, req->uri);
            int nw = marla_Connection_write(cxn, out, nwrit);
            if(nw < nwrit) {
                marla_Connection_putbackWrite(cxn, nw);
                return -1;
            }
            marla_logMessagef(server, "Wrote backend request line.");
            req->writeStage = marla_BACKEND_REQUEST_WRITING_HEADERS;
        }
        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_HEADERS) {
            // Write headers.

            int result = 0;
            req->handler(req, marla_BACKEND_EVENT_NEED_HEADERS, &result, 0);
            if(result == -1 || req->cxn->stage == marla_CLIENT_COMPLETE) {
                return -1;
            }
            if(result == 1 || req->writeStage > marla_BACKEND_REQUEST_WRITING_HEADERS) {
                if(req->writeStage == marla_BACKEND_REQUEST_WRITING_HEADERS) {
                    req->writeStage = marla_BACKEND_REQUEST_WRITING_REQUEST_BODY;
                }
            }
        }
        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_REQUEST_BODY) {
            // Write request body.

            int result = 0;
            req->handler(req, marla_BACKEND_EVENT_MUST_WRITE, &result, 0);
            if(result == -1 || req->cxn->stage == marla_CLIENT_COMPLETE) {
                return -1;
            }
            if(result == 1 || req->writeStage > marla_BACKEND_REQUEST_WRITING_REQUEST_BODY) {
                if(req->writeStage == marla_BACKEND_REQUEST_WRITING_REQUEST_BODY) {
                    req->writeStage = marla_BACKEND_REQUEST_DONE_WRITING;
                }
            }
            //req->writeStage = marla_BACKEND_REQUEST_WRITING_TRAILERS;
        }

        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_TRAILERS) {
            // Write trailers.

            int result = 0;
            req->handler(req, marla_BACKEND_EVENT_NEED_TRAILERS, &result, 0);
            if(result == -1 || req->cxn->stage == marla_CLIENT_COMPLETE) {
                return -1;
            }
            if(result == 1 || req->writeStage > marla_BACKEND_REQUEST_WRITING_TRAILERS) {
                if(req->writeStage == marla_BACKEND_REQUEST_WRITING_TRAILERS) {
                    req->writeStage = marla_BACKEND_REQUEST_DONE_WRITING;
                }
            }
        }
        if(req->writeStage == marla_BACKEND_REQUEST_DONE_WRITING) {
            req = req->next_request;
        }
    }
    return 0;
}

int marla_readBackendRequestChunks(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
read_chunk_size:
    while(req->readStage == marla_BACKEND_REQUEST_READING_CHUNK_SIZE) {
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
        req->readStage = marla_BACKEND_REQUEST_READING_CHUNK_BODY;
        if(req->chunkSize == 0) {
            if(req->handler) {
                req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, 0, 0);
            }
            if(req->expect_trailer) {
                req->readStage = marla_BACKEND_REQUEST_READING_TRAILER;
            }
            else {
                req->readStage = marla_BACKEND_REQUEST_DONE_READING;
            }
        }
    }

    if(req->readStage == marla_BACKEND_REQUEST_READING_CHUNK_BODY) {
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
                req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, buf, nread);
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
        req->readStage = marla_BACKEND_REQUEST_READING_CHUNK_SIZE;
        goto read_chunk_size;
    }

    return 0;
}

static int marla_processBackendTrailer(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    while(req->readStage == marla_BACKEND_REQUEST_READING_TRAILER) {
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

int marla_backendRead(marla_Connection* cxn)
{
    marla_logMessagef(cxn->server, "Reading from backend.");
    marla_Request* req = cxn->current_request;
    char out[marla_BUFSIZE];
    while(req) {
        if(req->readStage == marla_BACKEND_REQUEST_READING_RESPONSE_LINE) {
            int nr = marla_Connection_read(cxn, (unsigned char*)out, sizeof out);
            if(nr <= 0) {
                return -1;
            }
            //marla_logMessagef(cxn->server, "Reading response line from %d byte(s) read.", nr);
            char* start = out;
            int wordIndex = 0;
            for(int i = 0; i < nr; ) {
                if(out[i] == '\n' || (i < nr -1 && out[i] == '\r' && out[i + 1] == '\n')) {
                    marla_logMessagef(cxn->server, "Found end of response line.");
                    if(out[i] == '\n' && nr - i > 0) {
                        marla_Connection_putbackRead(cxn, nr - i - 1);
                    }
                    if(out[i] == '\r' && nr - i > 1) {
                        marla_Connection_putbackRead(cxn, nr - i - 2);
                    }
                    out[i] = 0;
                    break;
                }
                char c = out[i];
                //marla_logMessagef(cxn->server, "Read character '%c'. WordIndex=%d.", c, wordIndex);
                if(c <= 0x1f || c == 0x7f) {
                    snprintf(req->error, sizeof req->error, "Response line contains control characters, so no valid request.\n");
                    marla_logMessagef(cxn->server, req->error);
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                    snprintf(req->error, sizeof req->error, "Response line contains delimiters, so no valid request.\n");
                    marla_logMessagef(cxn->server, req->error);
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                    snprintf(req->error, sizeof req->error, "Response line contains unwise characters, so no valid request.\n");
                    marla_logMessagef(cxn->server, req->error);
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(wordIndex < 2 && c == ' ') {
                    if(wordIndex == 0) {
                        //marla_logMessagef(cxn->server, "Found end of version.");
                        out[i] = 0;
                        if(strcmp(start, "HTTP/1.1")) {
                            snprintf(req->error, sizeof req->error, "Response line contains unexpected version, so no valid request.\n");
                            marla_logMessagef(cxn->server, req->error);
                            cxn->stage = marla_CLIENT_COMPLETE;
                            return -1;
                        }
                        ++i;
                        //marla_logMessagef(cxn->server, "Moving to next character: %c", out[i]);
                    }
                    else {
                        out[i] = 0;
                        //marla_logMessagef(cxn->server, "Found end of status code '%s'", start);
                        char* endptr = 0;
                        req->statusCode = strtol(start, &endptr, 10);
                        if(start == endptr) {
                            snprintf(req->error, sizeof req->error, "No status code digits were found, so no valid request.");
                            marla_logMessagef(cxn->server, req->error);
                            cxn->stage = marla_CLIENT_COMPLETE;
                            return -1;
                        }
                        if(endptr != out + i) {
                            snprintf(req->error, sizeof req->error, "Response line contains invalid status code, so no valid request. %ld\n", (endptr-out));
                            marla_logMessagef(cxn->server, req->error);
                            cxn->stage = marla_CLIENT_COMPLETE;
                            return -1;
                        }
                        if(req->statusCode < 100 || req->statusCode > 599) {
                            snprintf(req->error, sizeof req->error, "Response line contains invalid status code %d, so no valid request.", req->statusCode);
                            marla_logMessagef(cxn->server, req->error);
                            cxn->stage = marla_CLIENT_COMPLETE;
                            return -1;
                        }
                        ++i;
                    }
                    start = 0;
                    while(i < nr && out[i] == ' ') {
                        //marla_logMessagef(cxn->server, "Skipping space");
                        ++i;
                    }
                    if(i == nr) {
                        break;
                    }
                    start = out + i;
                    ++wordIndex;
                    continue;
                }
                if(!isascii(c)) {
                    snprintf(req->error, sizeof req->error, "Response line contains non-ASCII characters, so no valid request.\n");
                    marla_logMessagef(cxn->server, req->error);
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(!start) {
                    start = out + i;
                }
                ++i;
                //marla_logMessagef(cxn->server, "Falling through to next character");
            }

            if(wordIndex != 2) {
                snprintf(req->error, sizeof req->error, "Response line ended prematurely.\n");
                marla_logMessagef(cxn->server, req->error);
                cxn->stage = marla_CLIENT_COMPLETE;
                return -1;
            }
            strncpy(req->statusLine, start, sizeof req->statusLine);

            marla_logMessagef(cxn->server, "Read response line: %d %s", req->statusCode, req->statusLine);
            req->readStage = marla_BACKEND_REQUEST_READING_HEADERS;
        }

        while(req->readStage == marla_BACKEND_REQUEST_READING_HEADERS) {
            //marla_logMessagecf(cxn->server, "HTTP Headers", "Reading headers");
            int nr = marla_Connection_read(cxn, (unsigned char*)out, sizeof out);
            if(nr <= 0) {
                marla_logMessagef(cxn->server, "Nothing to read.");
                return -1;
            }
            marla_logMessagef(cxn->server, "Read %d bytes for headers.", nr);
            int fieldSize = MAX_FIELD_VALUE_LENGTH + 1;
            char responseHeader[2 * (MAX_FIELD_VALUE_LENGTH + 1)];
            memset(responseHeader, 0, sizeof responseHeader);
            char* responseHeaderKey = responseHeader;
            char* responseHeaderValue = responseHeader + fieldSize;
            char* start = out;
            int lineStage = 0;
            int i = 0;
            for(; i < nr;) {
                if(out[i] == '\n') {
                    if(lineStage == 0) {
                        if(i == 0) {
                            marla_Connection_putbackRead(cxn, nr - 1);
                            break;
                        }
                        marla_killRequest(req, "Header ended prematurely.");
                        return 1;
                    }
                    out[i] = 0;
                    strncpy(responseHeaderValue, start, fieldSize);
                    //marla_logMessagef(cxn->server, "Found header value: %s", responseHeaderValue);
                    marla_Connection_putbackRead(cxn, nr - i - 1);
                    break;
                }
                if(out[i] == '\r') {
                    if(nr - i == 0) {
                        marla_Connection_putbackRead(cxn, nr);
                        return -1;
                    }
                    if(lineStage == 0) {
                        if(i == 0) {
                            marla_Connection_putbackRead(cxn, nr - 2);
                            break;
                        }
                        marla_killRequest(req, "Header ended prematurely.");
                        return 1;
                    }
                    out[i] = 0;
                    strncpy(responseHeaderValue, start, fieldSize);
                    //marla_logMessagef(cxn->server, "Found header value: %s", responseHeaderValue);
                    marla_Connection_putbackRead(cxn, nr - i - 2);
                    break;
                }
                char c = out[i];
                //marla_logMessagef(cxn->server, "Reading header character %c", c);
                if(c <= 0x1f || c == 0x7f) {
                    snprintf(req->error, sizeof req->error, "Response line contains control characters, so no valid request.\n");
                    marla_logMessagef(cxn->server, req->error);
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                    snprintf(req->error, sizeof req->error, "Response line contains delimiters, so no valid request.\n");
                    marla_logMessagef(cxn->server, req->error);
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                    snprintf(req->error, sizeof req->error, "Response line contains unwise characters, so no valid request.\n");
                    marla_logMessagef(cxn->server, req->error);
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(lineStage == 0 && c == ':') {
                    out[i] = 0;
                    strncpy(responseHeaderKey, start, fieldSize);
                    //marla_logMessagef(cxn->server, "Found header key: %s", responseHeaderKey);
                    ++i;

                    while(i < nr && out[i] == ' ') {
                        ++i;
                    }
                    if(i == nr) {
                        break;
                    }
                    start = out + i;
                    ++lineStage;
                    continue;
                }
                if(!isascii(c)) {
                    snprintf(req->error, sizeof req->error, "Response line contains non-ASCII characters, so no valid request.\n");
                    marla_logMessagef(cxn->server, req->error);
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                ++i;
            }
            if(i == 0) {
                int accept = 1;
                if(req->handler) {
                    req->handler(req, marla_BACKEND_EVENT_ACCEPTING_RESPONSE, &accept, 0);
                }
                if(!accept) {
                    marla_killRequest(req, "Request explicitly rejected.\n");
                    return -1;
                }

                marla_logMessagef(cxn->server, "End of response headers.");
                req->readStage = marla_BACKEND_REQUEST_READING_RESPONSE_BODY;
                switch(req->givenContentLen) {
                case marla_MESSAGE_IS_CHUNKED:
                    req->readStage = marla_BACKEND_REQUEST_READING_CHUNK_SIZE;
                    break;
                case 0:
                case marla_MESSAGE_LENGTH_UNKNOWN:
                    req->readStage = marla_BACKEND_REQUEST_DONE_READING;
                    break;
                default:
                case marla_MESSAGE_USES_CLOSE:
                    req->close_after_done = 1;
                    req->readStage = marla_BACKEND_REQUEST_READING_RESPONSE_BODY;
                    req->remainingContentLen = req->givenContentLen;
                }
                break;
            }
            if(lineStage != 1) {
                snprintf(req->error, sizeof req->error, "Response line ended prematurely.\n");
                marla_logMessagef(cxn->server, req->error);
                cxn->stage = marla_CLIENT_COMPLETE;
                return -1;
            }
            marla_logMessagecf(cxn->server, "HTTP Headers", "%s=%s", responseHeaderKey, responseHeaderValue);
            // TODO Process HTTP header (i.e. responseHeaderKey and responseHeaderValue)
            if(responseHeaderKey[0] == 'C') {
                if(!strcmp(responseHeaderKey, "Content-Length")) {
                    char* endptr;
                    long contentLen = strtol(responseHeaderValue, &endptr, 10);
                    if(endptr == responseHeaderValue) {
                        marla_killRequest(req, "Content-Length is malformed");
                        return 1;
                    }
                    req->givenContentLen = contentLen;
                }
                else if(!strcmp(responseHeaderKey, "Connection")) {
                    char* sp;
                    char* fieldToken = strtok_r(responseHeaderValue, ", ", &sp);
                    int hasMultiple = 1;
                    if(!fieldToken) {
                        fieldToken = responseHeaderValue;
                        hasMultiple = 0;
                    }
                    while(fieldToken) {
                        if(!strcasecmp(fieldToken, "close")) {
                            req->givenContentLen = marla_MESSAGE_USES_CLOSE;
                            req->close_after_done = 1;
                        }
                        else if(!strcasecmp(fieldToken, "Upgrade")) {
                            req->expect_upgrade = 1;
                        }
                        else if(strcasecmp(fieldToken, "keep-alive")) {
                            marla_killRequest(req, "Connection is not understood, so no valid request.\n");
                            return -1;
                        }
                        if(hasMultiple) {
                            fieldToken = strtok_r(0, ", ", &sp);
                        }
                    }
                }
                else if(!strcmp(responseHeaderKey, "Content-Type")) {
                    strncpy(req->contentType, responseHeaderValue, MAX_FIELD_VALUE_LENGTH);
                }
                else if(req->handler) {
                    req->handler(req, marla_BACKEND_EVENT_HEADER, responseHeader, responseHeaderValue - responseHeaderKey);
                }
            }
            if(!strcmp(responseHeaderKey, "Transfer-Encoding")) {
                if(req->givenContentLen != marla_MESSAGE_LENGTH_UNKNOWN) {
                    marla_killRequest(req, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                    return -1;
                }

                if(!strcasecmp(responseHeaderValue, "chunked")) {
                    req->givenContentLen = marla_MESSAGE_IS_CHUNKED;
                }
            }
            else if(req->handler) {
                req->handler(req, marla_BACKEND_EVENT_HEADER, responseHeader, responseHeaderValue - responseHeaderKey);
            }
        }

        if(req->readStage == marla_BACKEND_REQUEST_READING_RESPONSE_BODY) {
            while(req->remainingContentLen != 0) {
                // Read response body.
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
                        return 1;
                    }
                    break;
                }
                if(nread < 0) {
                    // Error.
                    marla_killRequest(req, "Error while receiving request body.\n");
                    return 1;
                }
                if(nread < 4 && req->remainingContentLen > 4) {
                    // A read too small.
                    marla_Connection_putbackRead(cxn, nread);
                    return -1;
                }

                // Handle input.
                req->totalContentLen += nread;
                req->remainingContentLen -= nread;
                if(req->remainingContentLen == 0) {
                    if(req->expect_trailer) {
                        req->readStage = marla_BACKEND_REQUEST_READING_TRAILER;
                    }
                    else {
                        req->readStage = marla_BACKEND_REQUEST_DONE_READING;
                    }
                }
                if(req->handler) {
                    req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, buf, nread);
                }
            }
            if(req->handler) {
                req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, 0, 0);
            }
            if(req->expect_trailer) {
                req->readStage = marla_BACKEND_REQUEST_READING_TRAILER;
            }
            else {
                req->readStage = marla_BACKEND_REQUEST_DONE_READING;
            }
            marla_logMessagef(req->cxn->server, "Now at %s", marla_nameRequestReadStage(req->readStage));
        }

        if(0 != marla_readBackendRequestChunks(req)) {
            return -1;
        }

        if(0 != marla_processBackendTrailer(req)) {
            return -1;
        }

        if(req->readStage == marla_BACKEND_REQUEST_DONE_READING) {
            if(req->backendPeer) {
                marla_clientWrite(req->backendPeer->cxn);
            }
            req = req->next_request;
        }
        else {
            marla_killRequest(req, "Unexpected request stage.\n");
            return 1;
        }
        if(cxn->stage == marla_CLIENT_COMPLETE && !cxn->shouldDestroy) {
            // Client needs shutdown.
            if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
                cxn->shouldDestroy = 1;
            }
            return -1;
        }
    }

    return 0;
}
