#include "marla.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>
#include <ctype.h>

#include "socket_funcs.c"

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
    resp->backendRequestBody = marla_Ring_new(bufSize);
    resp->backendResponse = marla_Ring_new(bufSize);
    resp->clientResponse = marla_Ring_new(bufSize);
    resp->handler = 0;
    resp->handleStage = 0;
    resp->index = 0;
    resp->req = req;
    return resp;
}

void marla_BackendResponder_free(marla_BackendResponder* resp)
{
    marla_Ring_free(resp->backendRequestBody);
    marla_Ring_free(resp->backendResponse);
    marla_Ring_free(resp->clientResponse);
    free(resp);
}

int marla_BackendResponder_writeRequestBody(marla_BackendResponder* resp, unsigned char* in, size_t len)
{
    return marla_Ring_write(resp->backendRequestBody, in, len);
}

int marla_BackendResponder_flushClientResponse(marla_BackendResponder* resp, size_t* nflushed)
{
    if(marla_Ring_isEmpty(resp->clientResponse)) {
        return 1;
    }
    *nflushed = 0;
    marla_Request* req = resp->req->backendPeer;
    marla_Server* server = req->cxn->server;

    void* data;
    size_t len;
    marla_Ring_readSlot(resp->clientResponse, &data, &len);
    if(len == 0) {
        marla_die(server, "Impossible to get a empty read slot from a non-empty ring");
    }
    int true_written = marla_Connection_write(req->cxn, data, len);
    if(true_written <= 0) {
        marla_Ring_putbackRead(resp->clientResponse, len);
        return -1;
    }
    //marla_logMessagef(server, "Flushed %d bytes from backend to client", true_written);
    *nflushed = true_written;
    if(true_written < len) {
        marla_Ring_putbackRead(resp->clientResponse, len - true_written);
    }
    return -1;
}

void marla_Backend_init(marla_Connection* cxn, int fd)
{
    marla_BackendSource* source = malloc(sizeof *source);
    cxn->is_backend = 1;
    cxn->source = source;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->acceptSource = acceptSource;
    cxn->shutdownSource = shutdownSource;
    cxn->destroySource = destroySource;
    cxn->describeSource = describeSource;
    source->fd = fd;
}

int marla_Backend_connect(marla_Server* server)
{
    if(server->backend) {
        return 0;
    }

    // Create the backend socket
    server->backendfd = create_and_connect("localhost", server->backendport);
    if(server->backendfd == -1) {
        marla_logLeave(server, "Failed to connect to backend server.");
        return -1;
    }
    int s = make_socket_non_blocking(server->backendfd);
    if(s == -1) {
        marla_logLeave(server, "Failed to make backend server non-blocking.");
        close(server->backendfd);
        return -1;
    }
    marla_logMessagef(server, "Server is using backend on port %s", server->backendport);

    marla_Connection* backend = marla_Connection_new(server);
    marla_Backend_init(backend, server->backendfd);
    server->backend = backend;

    struct epoll_event event;
    memset(&event, 0, sizeof(struct epoll_event));
    event.data.ptr = backend;
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLOUT | EPOLLET;
    s = epoll_ctl(server->efd, EPOLL_CTL_ADD, server->backendfd, &event);
    if(s == -1) {
        marla_logLeave(server, "Failed to add backend server to epoll queue.");
        return -1;
    }

    return 0;
}

void marla_Backend_enqueue(marla_Server* server, marla_Request* req)
{
    if(marla_Backend_connect(server) != 0) {
        marla_die(server, "Failed to connect to backend");
    }
    marla_Connection* cxn = server->backend;
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
    if(!req) {
        return 0;
    }
    if(cxn->in_write) {
        marla_logMessagecf(cxn->server, "Processing", "Called to write to backend, but already writing to this backend.");
        return -1;
    }
    cxn->in_write = 0;
    marla_logEntercf(cxn->server, "Processing", "Writing to backend with stage %s", marla_nameRequestWriteStage(req->writeStage));
    char out[marla_BUFSIZE];
    while(req) {
        if(!req->is_backend) {
            marla_die(server, "Client request found its way into the backend's queue.");
        }
        // Write request line to backend.
        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_REQUEST_LINE) {
            if(req->method[0] == 0) {
                marla_killRequest(req, "Method must be provided");
                marla_logLeave(server, 0);
                cxn->in_write = 0;
                return -1;
            }
            if(req->uri[0] == 0) {
                marla_killRequest(req, "URI must be provided");
                marla_logLeave(server, 0);
                cxn->in_write = 0;
                return -1;
            }
            int nwrit = snprintf(out, sizeof(out), "%s %s HTTP/1.1\r\n", req->method, req->uri);
            int nw = marla_Connection_write(cxn, out, nwrit);
            if(nw < nwrit) {
                marla_Connection_putbackWrite(cxn, nw);
                marla_logLeave(server, 0);
                cxn->in_write = 0;
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
                marla_logLeave(server, 0);
                cxn->in_write = 0;
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
            marla_logMessagef(req->cxn->server, "Handler indicated %d", result);
            if(result == -1 || req->cxn->stage == marla_CLIENT_COMPLETE) {
                marla_logLeave(server, 0);
                cxn->in_write = 0;
                return -1;
            }
            if(result == 1 || req->writeStage > marla_BACKEND_REQUEST_WRITING_REQUEST_BODY) {
                if(req->writeStage == marla_BACKEND_REQUEST_WRITING_REQUEST_BODY) {
                    marla_logMessagef(req->cxn->server, "Done writing request to backend");

                    int nflushed;
                    marla_Connection_flush(req->cxn, &nflushed);
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
                marla_logLeave(server, 0);
                cxn->in_write = 0;
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
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return 0;
}

static int marla_readBackendRequestChunks(marla_Request* req)
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

int marla_backendRead(marla_Connection* cxn)
{
    if(cxn->stage == marla_CLIENT_COMPLETE) {
        return -1;
    }

    if(cxn->in_read) {
        marla_logMessagecf(cxn->server, "Processing", "Called to read from backend, but backend is already being read.");
        return -1;
    }
    cxn->in_read = 1;

    marla_logEntercf(cxn->server, "Processing", "Reading from backend.");
    marla_Server* server = cxn->server;
    marla_Request* req = cxn->current_request;
    char out[marla_BUFSIZE];
    memset(out, 0, sizeof out);

    while(req) {
        if(req->readStage == marla_BACKEND_REQUEST_READING_RESPONSE_LINE) {
            int nr = marla_Connection_read(cxn, (unsigned char*)out, MAX_RESPONSE_LINE_LENGTH);
            if(nr <= 0) {
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return -1;
            }
            //marla_logMessagef(cxn->server, "Reading response line from %d byte(s) read.", nr);
            char* start = out;
            int wordIndex = 0;
            for(int i = 0; i < nr; ) {
                if(out[i] == '\n' || (i < nr -1 && out[i] == '\r' && out[i + 1] == '\n')) {
                    int excess = 0;
                    if(out[i] == '\n' && nr - i > 0) {
                        excess = nr - i - 1;
                        //fprintf(stderr, "Before putback:\n");
                        //fprintf(stderr, cxn->input->buf + cxn->input->read_index);
                        marla_Connection_putbackRead(cxn, excess);
                    }
                    if(out[i] == '\r' && nr - i > 1) {
                        excess = nr - i - 2;
                        //fprintf(stderr, "Before putback:\n");
                        //fprintf(stderr, cxn->input->buf + cxn->input->read_index);
                        marla_Connection_putbackRead(cxn, excess);
                    }
                    if(excess > 0) {
                        marla_logMessagef(cxn->server, "Found end of response line. %d excess.", excess);
                    }
                    else {
                        marla_logMessagef(cxn->server, "Found end of response line.");
                    }
                    out[i] = 0;
                    break;
                }
                char c = out[i];
                marla_logMessagef(cxn->server, "Read character '%c'. WordIndex=%d.", c, wordIndex);
                if(c <= 0x1f || c == 0x7f) {
                    marla_killRequest(req, "Response line contains control characters, so no valid request.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                    marla_killRequest(req, "Response line contains delimiters, so no valid request.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                    marla_killRequest(req, "Response line contains unwise characters, so no valid request.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                if(wordIndex < 2 && c == ' ') {
                    if(wordIndex == 0) {
                        //marla_logMessagef(cxn->server, "Found end of version.");
                        out[i] = 0;
                        if(strcmp(start, "HTTP/1.1")) {
                            marla_killRequest(req, "Response line contains unexpected version, so no valid request.");
                            marla_logLeave(server, 0);
                            cxn->in_read = 0;
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
                            marla_killRequest(req, "No status code digits were found, so no valid request.");
                            marla_logLeave(server, 0);
                            cxn->in_read = 0;
                            return -1;
                        }
                        if(endptr != out + i) {
                            marla_killRequest(req, "Response line contains invalid status code, so no valid request. %ld", (endptr-out));
                            marla_logLeave(server, 0);
                            cxn->in_read = 0;
                            return -1;
                        }
                        if(req->statusCode < 100 || req->statusCode > 599) {
                            marla_killRequest(req, "Response line contains invalid status code %d, so no valid request.", req->statusCode);
                            marla_logLeave(server, 0);
                            cxn->in_read = 0;
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
                    marla_killRequest(req, "Response line contains non-ASCII characters, so no valid request.\n");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                if(!start) {
                    start = out + i;
                }
                ++i;
                //marla_logMessagef(cxn->server, "Falling through to next character");
            }

            if(wordIndex != 2) {
                marla_killRequest(req, "Response line ended prematurely.");
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return -1;
            }
            strncpy(req->statusLine, start, sizeof req->statusLine);

            marla_logMessagef(cxn->server, "Read response line: %d %s", req->statusCode, req->statusLine);
            req->readStage = marla_BACKEND_REQUEST_READING_HEADERS;
        }

        //fprintf(stderr, "AFter putback:\n");
        //fprintf(stderr, cxn->input->buf + cxn->input->read_index);

        while(req->readStage == marla_BACKEND_REQUEST_READING_HEADERS) {
            //marla_logMessagecf(cxn->server, "HTTP Headers", "Reading headers");
            int nr = marla_Connection_read(cxn, (unsigned char*)out, MAX_FIELD_NAME_LENGTH + MAX_FIELD_VALUE_LENGTH);
            if(nr <= 0) {
                marla_logLeavef(cxn->server, "Nothing to read.");
                cxn->in_read = 0;
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
            int foundEnd = 0;
            for(; i < nr;) {
                if(out[i] == '\n') {
                    if(lineStage == 0) {
                        if(i == 0) {
                            marla_Connection_putbackRead(cxn, nr - 1);
                            foundEnd = 1;
                            break;
                        }
                        marla_killRequest(req, "Header ended prematurely.");
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
                        return 1;
                    }
                    out[i] = 0;
                    strncpy(responseHeaderValue, start, fieldSize);
                    marla_logMessagef(cxn->server, "Found header value: %s", responseHeaderValue);
                    marla_Connection_putbackRead(cxn, nr - i - 1);
                    foundEnd = 1;
                    break;
                }
                if(out[i] == '\r') {
                    if(nr - i == 0) {
                        marla_Connection_putbackRead(cxn, nr);
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
                        return -1;
                    }
                    if(lineStage == 0) {
                        if(i == 0) {
                            marla_Connection_putbackRead(cxn, nr - 2);
                            foundEnd = 1;
                            break;
                        }
                        marla_killRequest(req, "Header ended prematurely.");
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
                        return 1;
                    }
                    out[i] = 0;
                    strncpy(responseHeaderValue, start, fieldSize);
                    if(responseHeaderValue && *responseHeaderValue != 0) {
                        marla_logMessagef(cxn->server, "Found header value: %s", responseHeaderValue);
                    }
                    marla_Connection_putbackRead(cxn, nr - i - 2);
                    foundEnd = 1;
                    break;
                }
                char c = out[i];
                marla_logMessagef(cxn->server, "Reading header character %c", c);
                if(c <= 0x1f || c == 0x7f) {
                    marla_killRequest(req, "Header contains control characters, so no valid request.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                if(c == '<' || c == '>' || c == '#' || c == '%') {
                    marla_killRequest(req, "Header contains delimiters, so no valid request.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                    marla_killRequest(req, "Header contains unwise characters, so no valid request.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
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
                    marla_killRequest(req, "Response line contains non-ASCII characters, so no valid request.");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }
                ++i;
            }
            if(!foundEnd) {
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return -1;
            }
            if(i == 0) {
                int accept = 1;
                if(req->handler) {
                    req->handler(req, marla_BACKEND_EVENT_ACCEPTING_RESPONSE, &accept, 0);
                }
                if(!accept) {
                    marla_killRequest(req, "Request explicitly rejected.\n");
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
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
                case marla_MESSAGE_USES_CLOSE:
                    req->close_after_done = 1;
                    // Fall through.
                default:
                    req->readStage = marla_BACKEND_REQUEST_READING_RESPONSE_BODY;
                    req->remainingContentLen = req->givenContentLen;
                }
                break;
            }
            if(lineStage != 1) {
                marla_killRequest(req, "Response line ended prematurely.");
                marla_logLeave(server, 0);
                cxn->in_read = 0;
                return -1;
            }
            if(responseHeaderValue != 0 && responseHeaderKey != 0) {
                marla_logMessagecf(cxn->server, "HTTP Headers", "%s=%s", responseHeaderKey, responseHeaderValue);
            }
            else if(responseHeaderKey != 0) {
                marla_logMessagecf(cxn->server, "HTTP Headers", "%s", responseHeaderKey);
            }
            // Process HTTP header (i.e. responseHeaderKey and responseHeaderValue)
            if(responseHeaderKey[0] == 'C') {
                if(!strcmp(responseHeaderKey, "Content-Length")) {
                    char* endptr;
                    long contentLen = strtol(responseHeaderValue, &endptr, 10);
                    if(endptr == responseHeaderValue) {
                        marla_killRequest(req, "Content-Length is malformed");
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
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
                            marla_logMessage(server, "Backend request will close once done.");
                            req->givenContentLen = marla_MESSAGE_USES_CLOSE;
                            req->close_after_done = 1;
                        }
                        else if(!strcasecmp(fieldToken, "Upgrade")) {
                            req->expect_upgrade = 1;
                        }
                        else if(strcasecmp(fieldToken, "keep-alive")) {
                            marla_killRequest(req, "Connection is not understood, so no valid request.\n");
                            marla_logLeave(server, 0);
                            cxn->in_read = 0;
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
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
                    return -1;
                }

                if(!strcasecmp(responseHeaderValue, "chunked")) {
                    req->givenContentLen = marla_MESSAGE_IS_CHUNKED;
                }
            }
            else if(!strcmp(responseHeaderKey, "Location")) {
                strncpy(req->redirectLocation, responseHeaderValue, sizeof(req->redirectLocation));
            }
            else if(!strcmp(responseHeaderKey, "Set-Cookie")) {
                strncpy(req->setCookieHeader, responseHeaderValue, sizeof(req->setCookieHeader));
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
                        marla_logLeave(server, 0);
                        cxn->in_read = 0;
                        return 1;
                    }
                    break;
                }
                if(nread < 0) {
                    marla_logLeave(server, 0);
                    cxn->in_read = 0;
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
                //fprintf(stderr, "Consuming %d bytes\n", nread);
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
            if(req->expect_trailer) {
                req->readStage = marla_BACKEND_REQUEST_READING_TRAILER;
            }
            else {
                req->readStage = marla_BACKEND_REQUEST_DONE_READING;
            }
            if(req->handler) {
                req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, 0, 0);
            }
            marla_logMessagef(req->cxn->server, "Now at %s", marla_nameRequestReadStage(req->readStage));
        }

        if(0 != marla_readBackendRequestChunks(req)) {
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return -1;
        }

        if(0 != marla_processBackendTrailer(req)) {
            marla_logLeave(server, 0);
            cxn->in_read = 0;
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
    }

    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return 0;
}

void marla_backendHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int len)
{
    marla_Server* server = req->cxn->server;
    marla_logEntercf(server, "Handling", "Handling backend event on backend: %s", marla_nameClientEvent(ev));
    marla_BackendResponder* resp;

    char buf[2*marla_BUFSIZE];
    int bufLen;

    switch(ev) {
    case marla_BACKEND_EVENT_RESPONSE_BODY:
        if(len == 0) {
            marla_logLeave(server, 0);
            return;
        }
        resp = req->handlerData;
        if(!resp) {
            marla_die(req->cxn->server, "Backend peer must have responder handlerData\n");
        }
        marla_logMessagef(server, "Forwarding %d bytes from backend's response to client.", len);
        marla_Ring_write(resp->backendResponse, in, len);
        marla_clientWrite(req->backendPeer->cxn);
        marla_logLeave(server, 0);
        return;
    case marla_BACKEND_EVENT_NEED_HEADERS:
        if(req->contentType[0] == 0) {
            if(req->backendPeer->contentType[0] != 0) {
                strncpy(req->contentType, req->backendPeer->contentType, sizeof req->contentType);
            }
            else {
                strncpy(req->contentType, "text/plain", sizeof req->contentType);
            }
        }
        req->givenContentLen = marla_MESSAGE_IS_CHUNKED;
        if(req->backendPeer->cookieHeader[0] != 0) {
            sprintf(buf, "Host: localhost:8081\r\nTransfer-Encoding: chunked\r\nCookie: %s\r\nContent-Type: %s\r\nAccept: */*\r\n\r\n", req->backendPeer->cookieHeader, req->contentType);
        }
        else {
            sprintf(buf, "Host: localhost:8081\r\nTransfer-Encoding: chunked\r\nContent-Type: %s\r\nAccept: */*\r\n\r\n", req->contentType);
        }
        bufLen = strlen(buf);
        int nwritten = marla_Connection_write(req->cxn, buf, bufLen);
        if(nwritten < bufLen) {
            if(nwritten > 0) {
                marla_Connection_putbackWrite(req->cxn, nwritten);
            }
            goto choked;
        }
        goto done;
    case marla_BACKEND_EVENT_MUST_WRITE:
        resp = req->handlerData;
        for(;;) {
            switch(marla_writeChunk(server, resp->backendRequestBody, req->cxn->output)) {
            case 0:
                continue;
            case 1:
                if(!marla_Ring_isEmpty(resp->backendRequestBody)) {
                    marla_die(req->cxn->server, "Chunk writer indicated no more request data, but there is data is still to be written");
                }
                if(req->backendPeer->readStage <= marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
                    marla_logMessagef(req->cxn->server, "Client has not completed writing their request. %s", marla_nameRequestReadStage(req->backendPeer->readStage));
                    goto choked;
                }
                switch(marla_writeChunkTrailer(req->cxn->output)) {
                case -1:
                    marla_logMessage(req->cxn->server, "Failed to write request chunk trailer.");
                    goto choked;
                case 1:
                    goto done;
                }
                marla_die(req->cxn->server, "Unreachable");
            case -1:
                marla_logMessage(req->cxn->server, "Chunk writer choked");
                goto choked;
            }
        }
        marla_die(req->cxn->server, "Unreachable");
    case marla_BACKEND_EVENT_NEED_TRAILERS:
        goto done;
    case marla_EVENT_DESTROYING:
        resp = req->handlerData;
        if(resp) {
            marla_BackendResponder_free(resp);
            req->handlerData = 0;
            req->handler = 0;
        }
        if(req->backendPeer) {
            req->backendPeer->backendPeer = 0;
            marla_logMessagef(server, "Destroying peer of request: %s", marla_nameRequestReadStage(req->backendPeer->readStage));
        }
        req->backendPeer = 0;
    default:
        marla_logLeave(server, 0);
        return;
    }

    marla_die(server, "Unreachable");
done:
    marla_logLeave(server, 0);
    (*(int*)in) = 1;
    return;
choked:
    marla_logLeave(server, 0);
    (*(int*)in) = -1;
    return;
}

void marla_backendClientHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int len)
{
    marla_Server* server = req->cxn->server;
    marla_logEntercf(server, "Handling", "Handling backend event on client: %s\n", marla_nameClientEvent(ev));
    marla_BackendResponder* resp = 0;
    switch(ev) {
    case marla_EVENT_HEADER:
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_ACCEPTING_REQUEST:
        // Accept the request.
        (*(int*)in) = 1;

        marla_Request* backendReq = marla_Request_new(req->cxn->server->backend);
        strcpy(backendReq->uri, req->uri);
        strcpy(backendReq->method, req->method);
        backendReq->handler = marla_backendHandler;
        backendReq->handlerData = marla_BackendResponder_new(marla_BUFSIZE, backendReq);

        // Set backend peers.
        backendReq->backendPeer = req;
        req->backendPeer = backendReq;

        // Enqueue the backend request.
        marla_logMessagef(server, "ENQUEUED backend request %d!!!\n", req->id);
        marla_Backend_enqueue(server, backendReq);
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_REQUEST_BODY:
        marla_logMessage(server, "EVENT_REQUEST_BODY!!!\n");
        if(len == 0) {
            marla_logLeave(server, 0);
            return;
        }
        if(!req->backendPeer) {
            marla_die(req->cxn->server, "Backend request missing.");
        }
        resp = req->backendPeer->handlerData;
        marla_BackendResponder_writeRequestBody(resp, in, len);
        marla_backendWrite(req->backendPeer->cxn);
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_MUST_WRITE:
        if(req->readStage <= marla_CLIENT_REQUEST_READING_FIELD) {
            goto choked;
        }
        if(!req->backendPeer || req->backendPeer->error[0] != 0) {
            marla_killRequest(req, "Client lost its backend");
            goto done;
        }
        if(req->backendPeer->readStage <= marla_BACKEND_REQUEST_READING_HEADERS) {
            int rv = marla_backendRead(req->backendPeer->cxn);
            if(req->backendPeer->readStage <= marla_BACKEND_REQUEST_READING_HEADERS) {
                marla_logMessagef(req->cxn->server, "Not a good time to write %s %s %d", marla_nameRequestReadStage(req->backendPeer->readStage), marla_nameRequestReadStage(req->readStage), rv);
                goto choked;
            }
        }
        resp = req->backendPeer->handlerData;
        if(resp->handleStage == 0) {
            marla_logMessagef(req->cxn->server, "Generating initial client response from backend input");
            if(resp->handler) {
                resp->handler(resp);
            }
            else {
                resp->handleStage = 1;
            }
            if(resp->handleStage != 1) {
                goto choked;
            }
        }
        if(resp->handleStage == 1) {
            marla_logMessagef(req->cxn->server, "Sending response line to client from backend");
            char buf[marla_BUFSIZE];
            if(req->statusCode == 0) {
                req->statusCode = req->backendPeer->statusCode;
            }
            if(req->statusLine[0] == 0) {
                const char* statusLine;
                switch(req->statusCode) {
                case 100:
                    statusLine = "Continue";
                    break;
                case 101:
                    statusLine = "Switching Protocols";
                    break;
                case 200:
                    statusLine = "OK";
                    break;
                case 201:
                    statusLine = "Created";
                    break;
                case 202:
                    statusLine = "Accepted";
                    break;
                case 203:
                    statusLine = "Non-Authoritative Information";
                    break;
                case 204:
                    statusLine = "No Content";
                    break;
                case 205:
                    statusLine = "Reset Content";
                    break;
                case 300:
                    statusLine = "Multiple Choices";
                    break;
                case 301:
                    statusLine = "Moved Permanently";
                    break;
                case 302:
                    statusLine = "Found";
                    break;
                case 303:
                    statusLine = "See Other";
                    break;
                case 305:
                    statusLine = "Use Proxy";
                    break;
                case 307:
                    statusLine = "Temporary Redirect";
                    break;
                case 400:
                    statusLine = "Bad Request";
                    break;
                case 402:
                    statusLine = "Payment Required";
                    break;
                case 403:
                    statusLine = "Forbidden";
                    break;
                case 404:
                    statusLine = "Not Found";
                    break;
                case 405:
                    statusLine = "Method Not Allowed";
                    break;
                case 406:
                    statusLine = "Not Acceptable";
                    break;
                case 408:
                    statusLine = "Request Timeout";
                    break;
                case 409:
                    statusLine = "Conflict";
                    break;
                case 410:
                    statusLine = "Gone";
                    break;
                case 411:
                    statusLine = "Length Required";
                    break;
                case 413:
                    statusLine = "Payload Too Large";
                    break;
                case 414:
                    statusLine = "URI Too Long";
                    break;
                case 415:
                    statusLine = "Unsupported Media Type";
                    break;
                case 417:
                    statusLine = "Expectation Failed";
                    break;
                case 426:
                    statusLine = "Upgrade Required";
                    break;
                case 500:
                    statusLine = "Internal Server Error";
                    break;
                case 501:
                    statusLine = "Not Implemented";
                    break;
                case 502:
                    statusLine = "Bad Gateway";
                    break;
                case 503:
                    statusLine = "Service Unavailable";
                    break;
                case 504:
                    statusLine = "Gateway Timeout";
                    break;
                case 505:
                    statusLine = "HTTP Version Not Supported";
                    break;
                default:
                    marla_die(req->cxn->server, "Unsupported status code %d", req->statusCode);
                }
                strncpy(req->statusLine, statusLine, sizeof req->statusLine);
            }

            int len = snprintf(buf, sizeof buf, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\n",
                req->backendPeer->statusCode,
                req->backendPeer->statusLine,
                req->backendPeer->contentType
            );
            if(len < 0) {
                marla_die(server, "Failed to generate initial backend request headers");
            }
            marla_logMessagef(req->cxn->server, "Sending chunked client request: %d", req->backendPeer->givenContentLen);
            int true_written = marla_Connection_write(req->cxn, buf, len);
            if(true_written < len) {
                marla_Connection_putbackWrite(req->cxn, true_written);
                goto choked;
            }
            ++resp->handleStage;
        }

        while(resp->handleStage == 2) {
            if(req->backendPeer->redirectLocation[0] == 0) {
                ++resp->handleStage;
                break;
            }
            marla_logMessagef(req->cxn->server, "Sending Location headers to client from backend");
            char buf[marla_BUFSIZE];
            int len = snprintf(buf, sizeof buf, "Location: %s\r\n", req->backendPeer->redirectLocation);
            if(len < 0) {
                marla_die(server, "Failed to generate backend request headers");
            }
            marla_logMessagef(req->cxn->server, "Sending chunked client request: %d", req->backendPeer->givenContentLen);
            int true_written = marla_Connection_write(req->cxn, buf, len);
            if(true_written < len) {
                marla_Connection_putbackWrite(req->cxn, true_written);
                goto choked;
            }
            ++resp->handleStage;
        }

        while(resp->handleStage == 3) {
            if(req->backendPeer->setCookieHeader[0] == 0) {
                ++resp->handleStage;
                break;
            }
            marla_logMessagef(req->cxn->server, "Sending Set-Cookie headers to client from backend");
            char buf[marla_BUFSIZE];
            int len = snprintf(buf, sizeof buf, "Set-Cookie: %s\r\n", req->backendPeer->setCookieHeader);
            if(len < 0) {
                marla_die(server, "Failed to generate backend request headers");
            }
            marla_logMessagef(req->cxn->server, "Sending chunked client request: %d", req->backendPeer->givenContentLen);
            int true_written = marla_Connection_write(req->cxn, buf, len);
            if(true_written < len) {
                marla_Connection_putbackWrite(req->cxn, true_written);
                goto choked;
            }
            ++resp->handleStage;
        }

        while(resp->handleStage == 4) {
            marla_logMessagef(req->cxn->server, "Sending terminal header to client from backend");
            char buf[marla_BUFSIZE];
            int len = snprintf(buf, sizeof buf, "\r\n");
            if(len < 0) {
                marla_die(server, "Failed to generate backend request headers");
            }
            int true_written = marla_Connection_write(req->cxn, buf, len);
            if(true_written < len) {
                marla_Connection_putbackWrite(req->cxn, true_written);
                goto choked;
            }
            ++resp->handleStage;
        }

        while(resp->handleStage == 5) {
            //marla_logMessagef(req->cxn->server, "Writing backend response to client");
            size_t nflushed;
            switch(marla_writeChunk(server, resp->backendResponse, resp->clientResponse)) {
            case 0:
                continue;
            case -1:
                // Downstream choked.
                switch(marla_BackendResponder_flushClientResponse(resp, &nflushed)) {
                case 1:
                    marla_die(server, "Impossible");
                    break;
                case -1:
                    if(nflushed == 0) {
                        goto choked;
                    }
                    continue;
                }
            case 1:
                if(!marla_Ring_isEmpty(resp->backendResponse)) {
                    marla_die(req->cxn->server, "Chunk writer indicated no more data, but there is data is still to be written");
                }
                if(req->backendPeer->readStage < marla_BACKEND_REQUEST_DONE_READING) {
                    marla_backendRead(req->backendPeer->cxn);
                    if(req->backendPeer->readStage < marla_BACKEND_REQUEST_DONE_READING) {
                        marla_logMessagef(req->cxn->server, "Backend has not finished writing their response. %s", marla_nameRequestReadStage(req->backendPeer->readStage));
                        goto choked;
                    }
                    continue;
                }
                switch(marla_writeChunkTrailer(resp->clientResponse)) {
                case -1:
                    marla_logMessage(req->cxn->server, "Failed to write chunk trailer.");
                    goto choked;
                case 1:
                    marla_logMessage(req->cxn->server, "Wrote chunk trailer.");
                    ++resp->handleStage;
                    continue;
                }
                marla_die(server, "Unreachable");
            }
        }

        while(resp->handleStage == 6) {
            if(marla_Ring_isEmpty(resp->clientResponse)) {
                ++resp->handleStage;
                break;
            }
            marla_logMessagef(server, "Flushing input data.");
            size_t nflushed;
            switch(marla_BackendResponder_flushClientResponse(resp, &nflushed)) {
            case -1:
                if(nflushed == 0) {
                    goto choked;
                }
                continue;
            case 0:
                continue;
            case 1:
                continue;
            }
        }

        if(resp->handleStage == 7) {
            marla_logMessagef(server, "Processed all input data.");
            goto done;
        }

        marla_die(server, "Invalid BackendResponder handleStage=%d", resp->handleStage);
        break;
    case marla_EVENT_DESTROYING:
        marla_logLeave(server, 0);
        return;
    default:
        marla_logLeave(server, "Defaulted");
        return;
    }

    marla_die(server, "Unreachable");
done:
    if(resp) {
        resp->handleStage = 7;
    }
    (*(int*)in) = 1;
    marla_logLeave(server, "Done");
    return;
choked:
    (*(int*)in) = -1;
    marla_logLeave(server, "Choked");
    return;
}
