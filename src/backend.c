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

int marla_BackendResponder_NEXT_ID = 0;

struct marla_BackendResponder* marla_BackendResponder_new(size_t bufSize, marla_Request* req)
{
    if(!req) {
        fprintf(stderr, "Backend request must be given\n");
        abort();
    }
    marla_BackendResponder* resp = malloc(sizeof *resp);
    resp->id = ++marla_BackendResponder_NEXT_ID;
    //fprintf(stderr, "Creating backend responder %d\n", resp->id);
    resp->backendRequestBody = marla_Ring_new(bufSize);
    resp->backendResponse = marla_Ring_new(bufSize);
    resp->handler = 0;
    resp->handleStage = marla_BackendResponderStage_STARTED;
    resp->index = 0;
    resp->req = req;
    return resp;
}

void marla_BackendResponder_free(marla_BackendResponder* resp)
{
    //fprintf(stderr, "Freeing backend responder %d\n", resp->id);
    marla_Ring_free(resp->backendRequestBody);
    marla_Ring_free(resp->backendResponse);
    free(resp);
}

int marla_BackendResponder_writeRequestBody(marla_BackendResponder* resp, unsigned char* in, size_t len)
{
    return marla_Ring_write(resp->backendRequestBody, in, len);
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
        marla_logMessage(server, "Failed to connect to backend server.");
        return -1;
    }
    int s = make_socket_non_blocking(server->backendfd);
    if(s == -1) {
        marla_logMessage(server, "Failed to make backend server non-blocking.");
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
        marla_logMessage(server, "Failed to add backend server to epoll queue.");
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

    ++cxn->requests_in_process;
    for(int loop = 1; loop;) {
        marla_WriteResult wr = marla_backendWrite(cxn);
        int nflushed = 0;
        switch(wr) {
        case marla_WriteResult_CONTINUE:
            continue;
        case marla_WriteResult_DOWNSTREAM_CHOKED:
            wr = marla_Connection_flush(cxn, &nflushed);
            if(nflushed == 0 || wr == marla_WriteResult_CLOSED) {
                loop = 0;
            }
            continue;
        case marla_WriteResult_CLOSED:
        case marla_WriteResult_UPSTREAM_CHOKED:
        case marla_WriteResult_LOCKED:
        case marla_WriteResult_TIMEOUT:
        case marla_WriteResult_KILLED:
            loop = 0;
            continue;
        }
    }
}

void marla_Backend_recover(marla_Server* server)
{
    marla_Connection* oldCxn = server->backend;
    server->backend = 0;
    if(0 != marla_Backend_connect(server)) {
        marla_die(server, "Failed to reconnect to backend");
    }

    for(;;) {
        marla_Request* req = oldCxn->current_request;
        if(!req) {
            break;
        }
        marla_Request* nextReq = req->next_request;
        if(req->readStage < marla_BACKEND_REQUEST_DONE_READING) {
            // Incomplete request, so re-enqueue request.
            if(!req->backendPeer->cxn->shouldDestroy) {
                marla_Backend_enqueue(server, req);
            }
        }
        else if(!req->backendPeer) {
            marla_die(server, "Discovered orphaned request");
        }
        oldCxn->current_request = nextReq;
    }
    oldCxn->shouldDestroy = 1;
}

marla_WriteResult marla_backendWrite(marla_Connection* cxn)
{
    marla_Server* server = cxn->server;
    marla_Request* req = cxn->current_request;
    if(!req) {
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    if(cxn->in_write) {
        marla_logMessagecf(cxn->server, "Processing", "Called to write to backend, but already writing to this backend.");
        return marla_WriteResult_LOCKED;
    }

    cxn->in_write = 1;
    marla_logEntercf(cxn->server, "Processing", "Writing to backend with stage %s", marla_nameRequestWriteStage(req->writeStage));
    char out[marla_BUFSIZE];
    while(cxn->stage != marla_CLIENT_COMPLETE && req) {
        marla_Request_ref(req);
        if(!req->is_backend) {
            marla_die(server, "Client request found its way into the backend's queue.");
        }
        // Write request line to backend.
        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_REQUEST_LINE) {
            if(req->method[0] == 0) {
                marla_killRequest(req, "Method must be provided");
                marla_Request_unref(req);
                goto exit_killed;
            }
            if(req->uri[0] == 0) {
                marla_killRequest(req, "URI must be provided");
                marla_Request_unref(req);
                goto exit_killed;
            }
            int nwrit = snprintf(out, sizeof(out), "%s %s HTTP/1.1\r\n", req->method, req->uri);
            int nw = marla_Connection_write(cxn, out, nwrit);
            if(nw < nwrit) {
                marla_Connection_putbackWrite(cxn, nw);
                marla_Request_unref(req);
                goto exit_downstream_choked;
            }
            int nflushed;
            marla_WriteResult wr = marla_Connection_flush(cxn, &nflushed);
            switch(wr) {
            case marla_WriteResult_CLOSED:
                marla_Connection_putbackWrite(cxn, nw);
                marla_Request_unref(req);
                marla_logMessagef(server, "Backend connection has closed while sending request line.");
                goto exit_closed;
            case marla_WriteResult_UPSTREAM_CHOKED:
                marla_logMessagef(server, "Wrote backend request line.");
                req->writeStage = marla_BACKEND_REQUEST_WRITING_HEADERS;
                break;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                marla_Connection_putbackWrite(cxn, nw);
                marla_Request_unref(req);
                goto exit_downstream_choked;
            default:
                marla_die(server, "Unhandled flush result");
            }
        }
        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_HEADERS) {
            // Write headers.

            int result = 0;
            req->handler(req, marla_BACKEND_EVENT_NEED_HEADERS, &result, 0);
            if(req->cxn->stage == marla_CLIENT_COMPLETE) {
                marla_Request_unref(req);
                goto exit_closed;
            }
            if(result == -1) {
                marla_Request_unref(req);
                goto exit_downstream_choked;
            }
            if(result == 1 || req->writeStage > marla_BACKEND_REQUEST_WRITING_HEADERS) {
                if(req->writeStage == marla_BACKEND_REQUEST_WRITING_HEADERS) {
                    req->writeStage = marla_BACKEND_REQUEST_WRITING_REQUEST_BODY;
                }
            }
        }

        marla_WriteEvent result;
        marla_WriteEvent_init(&result, marla_WriteResult_CONTINUE);
        for(; req->cxn->stage != marla_CLIENT_COMPLETE && req->writeStage == marla_BACKEND_REQUEST_WRITING_REQUEST_BODY; ) {
            switch(result.status) {
            case marla_WriteResult_UPSTREAM_CHOKED:
                marla_Request_unref(req);
                goto exit_upstream_choked;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                if(marla_Ring_size(cxn->output) > 0) {
                    int nflushed;
                    marla_WriteResult wr = marla_Connection_flush(cxn, &nflushed);
                    switch(wr) {
                    case marla_WriteResult_CLOSED:
                        marla_Request_unref(req);
                        marla_logMessagef(server, "Backend connection has closed while sending request line.");
                        goto exit_closed;
                    case marla_WriteResult_UPSTREAM_CHOKED:
                        result.status = marla_WriteResult_CONTINUE;
                        break;
                    case marla_WriteResult_DOWNSTREAM_CHOKED:
                        marla_Request_unref(req);
                        goto exit_downstream_choked;
                    default:
                        marla_die(server, "Unhandled flush result");
                    }
                }
                else {
                    result.status = marla_WriteResult_CONTINUE;
                }
                // Fall through regardless.
            case marla_WriteResult_CONTINUE:
                if(req->handler) {
                    req->handler(req, marla_BACKEND_EVENT_MUST_WRITE, &result, -1);
                    marla_logMessagef(req->cxn->server, "Backend handler indicated %d", result);
                    continue;
                }
                // Fall through otherwise.
            case marla_WriteResult_LOCKED:
            case marla_WriteResult_CLOSED:
                if(marla_Ring_size(cxn->output) > 0) {
                    int nflushed;
                    marla_WriteResult wr = marla_Connection_flush(cxn, &nflushed);
                    switch(wr) {
                    case marla_WriteResult_CLOSED:
                        marla_Request_unref(req);
                        marla_logMessagef(server, "Backend connection has closed.");
                        goto exit_closed;
                    case marla_WriteResult_UPSTREAM_CHOKED:
                        req->writeStage = marla_BACKEND_REQUEST_DONE_WRITING;
                        break;
                    case marla_WriteResult_DOWNSTREAM_CHOKED:
                        marla_Request_unref(req);
                        goto exit_downstream_choked;
                    default:
                        marla_die(server, "Unhandled flush result");
                    }
                }
                else {
                    req->writeStage = marla_BACKEND_REQUEST_DONE_WRITING;
                }
                break;
            default:
                marla_die(server, "Unexpected WriteResult: %d", result.status);
                break;
            }
        }

        if(req->writeStage == marla_BACKEND_REQUEST_DONE_WRITING) {
            if(marla_Ring_size(cxn->output) > 0) {
                int nflushed;
                marla_WriteResult wr = marla_Connection_flush(cxn, &nflushed);
                marla_Request* nextReq;
                switch(wr) {
                case marla_WriteResult_CLOSED:
                    marla_Request_unref(req);
                    marla_logMessagef(server, "Backend connection has closed.");
                    goto exit_closed;
                case marla_WriteResult_UPSTREAM_CHOKED:
                    nextReq = req->next_request;
                    marla_Request_unref(req);
                    req = nextReq;
                    break;
                case marla_WriteResult_DOWNSTREAM_CHOKED:
                    marla_Request_unref(req);
                    goto exit_downstream_choked;
                default:
                    marla_die(server, "Unhandled flush result");
                }
            }
            else {
                marla_Request* nextReq = req->next_request;
                marla_Request_unref(req);
                req = nextReq;
            }
        }
    }
    if(cxn->stage == marla_CLIENT_COMPLETE) {
        goto exit_closed;
    }
    goto exit_upstream_choked;

exit_killed:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_KILLED;
exit_closed:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    cxn->shouldDestroy = 1;
    return marla_WriteResult_CLOSED;
exit_downstream_choked:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_DOWNSTREAM_CHOKED;
exit_upstream_choked:
    marla_logLeave(server, 0);
    cxn->in_write = 0;
    return marla_WriteResult_UPSTREAM_CHOKED;
}

static marla_WriteResult marla_processBackendResponseLine(marla_Request* req)
{
    if(req->readStage > marla_BACKEND_REQUEST_READING_RESPONSE_LINE) {
        return marla_WriteResult_CONTINUE;
    }
    if(req->readStage < marla_BACKEND_REQUEST_READING_RESPONSE_LINE) {
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;
    char out[marla_BUFSIZE];
    memset(out, 0, sizeof out);
    int nr = marla_Connection_read(cxn, (unsigned char*)out, MAX_RESPONSE_LINE_LENGTH);
    if(nr == 0) {
        return marla_WriteResult_CLOSED;
    }
    if(nr < 0) {
        return marla_WriteResult_UPSTREAM_CHOKED;
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
                marla_logMessagef(server, "Found end of response line. %d excess.", excess);
            }
            else {
                marla_logMessagef(server, "Found end of response line.");
            }
            out[i] = 0;
            break;
        }
        char c = out[i];
        //marla_logMessagef(server, "Read character '%c'. WordIndex=%d.", c, wordIndex);
        if(c <= 0x1f || c == 0x7f) {
            marla_killRequest(req, "Backend response line contains control characters, so no valid response.");
            return marla_WriteResult_KILLED;
        }
        if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
            marla_killRequest(req, "Backend response line contains delimiters, so no valid response.");
            return marla_WriteResult_KILLED;
        }
        if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
            marla_killRequest(req, "Backend response line contains unwise characters, so no valid response.");
            return marla_WriteResult_KILLED;
        }
        if(wordIndex < 2 && c == ' ') {
            if(wordIndex == 0) {
                //marla_logMessagef(server, "Found end of version.");
                out[i] = 0;
                if(strcmp(start, "HTTP/1.1")) {
                    marla_killRequest(req, "Response line contains unexpected version, so no valid request.");
                    return marla_WriteResult_KILLED;
                }
                ++i;
                //marla_logMessagef(server, "Moving to next character: %c", out[i]);
            }
            else {
                out[i] = 0;
                //marla_logMessagef(server, "Found end of status code '%s'", start);
                char* endptr = 0;
                req->statusCode = strtol(start, &endptr, 10);
                if(start == endptr) {
                    marla_killRequest(req, "No status code digits were found, so no valid request.");
                    return marla_WriteResult_KILLED;
                }
                if(endptr != out + i) {
                    marla_killRequest(req, "Response line contains invalid status code, so no valid request. %ld", (endptr-out));
                    return marla_WriteResult_KILLED;
                }
                if(req->statusCode < 100 || req->statusCode > 599) {
                    marla_killRequest(req, "Response line contains invalid status code %d, so no valid request.", req->statusCode);
                    return marla_WriteResult_KILLED;
                }
                ++i;
            }
            start = 0;
            while(i < nr && out[i] == ' ') {
                //marla_logMessagef(server, "Skipping space");
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
            return marla_WriteResult_KILLED;
        }
        if(!start) {
            start = out + i;
        }
        ++i;
        //marla_logMessagef(server, "Falling through to next character");
    }

    if(wordIndex != 2) {
        marla_killRequest(req, "Response line ended prematurely.");
        return marla_WriteResult_KILLED;
    }
    strncpy(req->statusLine, start, sizeof req->statusLine);

    marla_logMessagef(server, "Read response line: %d %s", req->statusCode, req->statusLine);
    req->readStage = marla_BACKEND_REQUEST_READING_HEADERS;
    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult marla_processBackendResponseFields(marla_Request* req)
{
    if(req->readStage > marla_BACKEND_REQUEST_READING_HEADERS) {
        return marla_WriteResult_CONTINUE;
    }
    if(req->readStage < marla_BACKEND_REQUEST_READING_HEADERS) {
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    char out[marla_BUFSIZE];
    memset(out, 0, sizeof out);
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;

    while(req->readStage == marla_BACKEND_REQUEST_READING_HEADERS) {
        //marla_logMessagecf(cxn->server, "HTTP Headers", "Reading headers");
        int nr = marla_Connection_read(cxn, (unsigned char*)out, MAX_FIELD_NAME_LENGTH + MAX_FIELD_VALUE_LENGTH);
        if(nr <= 0) {
            marla_logMessagef(server, "Nothing to read.");
            return marla_WriteResult_KILLED;
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
                    return marla_WriteResult_KILLED;
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
                    return marla_WriteResult_UPSTREAM_CHOKED;
                }
                if(lineStage == 0) {
                    if(i == 0) {
                        marla_Connection_putbackRead(cxn, nr - 2);
                        foundEnd = 1;
                        break;
                    }
                    marla_killRequest(req, "Header ended prematurely.");
                    return marla_WriteResult_KILLED;
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
            //marla_logMessagef(cxn->server, "Reading header character %c", c);
            if((c <= 0x1f && c != '\t') || c == 0x7f) {
                marla_killRequest(req, "Backend response header contains control characters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(c == '<' || c == '>' || c == '#' || c == '%') {
                marla_killRequest(req, "Backend response header contains delimiters, so no valid request.");
                return marla_WriteResult_KILLED;
            }
            if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                marla_killRequest(req, "Backend response header contains unwise characters, so no valid request.");
                return marla_WriteResult_KILLED;
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
                return marla_WriteResult_KILLED;
            }
            ++i;
        }
        if(!foundEnd) {
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
        if(i == 0) {
            int accept = 1;
            if(req->handler) {
                req->handler(req, marla_BACKEND_EVENT_ACCEPTING_RESPONSE, &accept, 0);
            }
            if(!accept) {
                marla_killRequest(req, "Request explicitly rejected.\n");
                return marla_WriteResult_KILLED;
            }

            marla_logMessagef(cxn->server, "End of response headers.");
            req->readStage = marla_BACKEND_REQUEST_READING_RESPONSE_BODY;
            switch(req->responseLen) {
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
                req->remainingContentLen = req->responseLen;
            }
            break;
        }
        if(lineStage != 1) {
            marla_killRequest(req, "Response line ended prematurely.");
            return marla_WriteResult_KILLED;
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
                    return marla_WriteResult_KILLED;
                }
                if(req->responseLen != marla_MESSAGE_LENGTH_UNKNOWN) {
                    marla_killRequest(req, "Content-Length specified twice.");
                    return marla_WriteResult_KILLED;
                }
                req->responseLen = contentLen;
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
                        if(req->responseLen == marla_MESSAGE_LENGTH_UNKNOWN) {
                            req->responseLen = marla_MESSAGE_USES_CLOSE;
                        }
                        req->close_after_done = 1;
                    }
                    else if(!strcasecmp(fieldToken, "Upgrade")) {
                        req->expect_upgrade = 1;
                    }
                    else if(strcasecmp(fieldToken, "keep-alive")) {
                        marla_killRequest(req, "Connection is not understood, so no valid request.\n");
                        return marla_WriteResult_KILLED;
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
        else if(!strcmp(responseHeaderKey, "Transfer-Encoding")) {
            if(req->responseLen != marla_MESSAGE_LENGTH_UNKNOWN) {
                marla_killRequest(req, "Content-Length/Transfer-Encoding header value was set twice, so no valid request.\n");
                return marla_WriteResult_KILLED;
            }

            if(!strcasecmp(responseHeaderValue, "chunked")) {
                req->responseLen = marla_MESSAGE_IS_CHUNKED;
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

    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult marla_consumeTrailingEol(marla_Request* req)
{
    marla_Connection* cxn = req->cxn;
    // Consume trailing EOL
    char buf[2];
    int nread = marla_Connection_read(cxn, (unsigned char*)buf, sizeof(buf));
    if(nread == 0) {
        // Zero-length read indicates end of stream.
        marla_killRequest(req, "Premature end of response chunk body.\n");
        return marla_WriteResult_KILLED;
    }
    if(nread < 0) {
        return marla_WriteResult_UPSTREAM_CHOKED;
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
        marla_killRequest(req, "Error while receiving response chunk body.\n");
        return marla_WriteResult_KILLED;
    }

    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult marla_readBackendResponseChunks(marla_Request* req)
{
    if(req->readStage < marla_BACKEND_REQUEST_READING_CHUNK_SIZE) {
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;
read_chunk_size:
    while(req->readStage == marla_BACKEND_REQUEST_READING_CHUNK_SIZE) {
        char buf[marla_MAX_CHUNK_SIZE_LINE];
        memset(buf, 0, sizeof(buf));
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, sizeof(buf));
        if(nread == 0) {
            // Zero-length read indicates premature end of stream.
            marla_killRequest(req, "Premature end of chunked response body.");
            return marla_WriteResult_CLOSED;
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
                nread = i;
                break;
            }
            else if(foundHexDigit && c == '\r') {
                if(i >= nread - 1) {
                    marla_Connection_putbackRead(cxn, nread);
                    return marla_WriteResult_UPSTREAM_CHOKED;
                }
                if(buf[i + 1] != '\n') {
                    marla_killRequest(req, "Backend response chunk is not terminated properly.");
                    return marla_WriteResult_KILLED;
                }
                // Size-body separator.
                foundEnd = 1;
                buf[i] = 0;
                marla_Connection_putbackRead(cxn, nread - (i + 2));
                nread = i + 1;
                break;
            }
            else {
                // Garbage.
                marla_killRequest(req, "Error while receiving backend response chunk size.\n");
                return marla_WriteResult_KILLED;
            }
        }

        if(!foundEnd) {
            if(nread >= marla_MAX_CHUNK_SIZE_LINE) {
                marla_killRequest(req, "Backend response chunk size line too long.\n");
                return marla_WriteResult_KILLED;
            }

            // Incomplete read.
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }

        if(!foundHexDigit) {
            marla_killRequest(req, "Failed to find any hex digits in backend response chunk size.\n");
            return marla_WriteResult_KILLED;
        }

        char* endptr = 0;
        long int chunkSize = strtol(buf, &endptr, 16);
        if(*endptr != 0) {
            marla_killRequest(req, "Error while parsing backend response chunk size.\n");
            return marla_WriteResult_KILLED;
        }
        if(chunkSize < 0 || chunkSize > marla_MAX_CHUNK_SIZE) {
            marla_killRequest(req, "Backend response chunk size is out of range.\n");
            return marla_WriteResult_KILLED;
        }
        req->chunkSize = chunkSize;
        req->readStage = marla_BACKEND_REQUEST_READING_CHUNK_BODY;
        if(req->chunkSize == 0) {
            if(req->handler) {
                marla_WriteEvent we;
                marla_WriteEvent_init(&we, marla_WriteResult_CONTINUE);
                for(; cxn->stage != marla_CLIENT_COMPLETE && req->readStage == marla_BACKEND_REQUEST_READING_CHUNK_BODY;) {
                    req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, &we, -1);
                    marla_WriteResult wr;
                    switch(we.status) {
                    case marla_WriteResult_CONTINUE:
                        if(req->readStage == marla_BACKEND_REQUEST_AFTER_RESPONSE) {
                            wr = marla_consumeTrailingEol(req);
                            if(wr != marla_WriteResult_CONTINUE) {
                                req->readStage = marla_BACKEND_REQUEST_READING_CHUNK_SIZE;
                                marla_Connection_putbackRead(cxn, nread);
                                return wr;
                            }
                        }
                        continue;
                    case marla_WriteResult_KILLED:
                    case marla_WriteResult_LOCKED:
                    case marla_WriteResult_TIMEOUT:
                    case marla_WriteResult_CLOSED:
                    case marla_WriteResult_UPSTREAM_CHOKED:
                    case marla_WriteResult_DOWNSTREAM_CHOKED:
                        req->readStage = marla_BACKEND_REQUEST_READING_CHUNK_SIZE;
                        marla_Connection_putbackRead(cxn, nread);
                        return we.status;
                    }
                }
            }
        }
    }

    if(req->readStage == marla_BACKEND_REQUEST_READING_CHUNK_BODY) {
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
                    marla_killRequest(req, "Premature end of backend response chunk body.\n");
                    return marla_WriteResult_CLOSED;
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
            req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, &result, -1);
            result.buf = 0;
            req->lastReadIndex = result.index;

            switch(result.status) {
            case marla_WriteResult_CONTINUE:
                if(result.index < result.length) {
                    req->totalContentLen -= nread - result.index;
                    req->chunkSize += nread - result.index;
                    marla_Connection_putbackRead(cxn, nread - result.index);
                    result.index = 0;
                    continue;
                }
                // Everything read from this chunk; move on.
                result.index = 0;
                req->lastReadIndex = 0;
                marla_Connection_refill(req->cxn, 0);
                continue;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                // This chunk body was being streamed to some other downstream source, which could not process
                // all of this chunk's input.
                marla_Connection_putbackRead(cxn, nread);
                req->totalContentLen -= nread;
                req->chunkSize += nread;
                return marla_WriteResult_DOWNSTREAM_CHOKED;
            case marla_WriteResult_UPSTREAM_CHOKED:
                if(result.length > 0 && result.index < result.length) {
                    marla_killRequest(req, "Backend response handler indicated upstream choked despite partial read.");
                    return marla_WriteResult_KILLED;
                }
                // Continue to read the next chunk.
                req->lastReadIndex = 0;
                result.index = 0;
                marla_Connection_refill(req->cxn, 0);
                break;
            case marla_WriteResult_LOCKED:
                marla_logMessage(server, "Backend response handler locked.");
                marla_Connection_putbackRead(cxn, nread);
                req->totalContentLen -= nread;
                req->chunkSize += nread;
                return marla_WriteResult_LOCKED;
            case marla_WriteResult_TIMEOUT:
                marla_logMessage(server, "Backend response handler timed out.");
                marla_Connection_putbackRead(cxn, nread);
                req->totalContentLen -= nread;
                req->chunkSize += nread;
                return marla_WriteResult_TIMEOUT;
            case marla_WriteResult_CLOSED:
                marla_logMessage(server, "Backend response handler closed.");
                return marla_WriteResult_CLOSED;
            case marla_WriteResult_KILLED:
                marla_logMessage(server, "Backend response killed.");
                return marla_WriteResult_KILLED;
            }
        }

        marla_WriteResult wr = marla_consumeTrailingEol(req);
        if(wr != marla_WriteResult_CONTINUE) {
            return wr;
        }
        req->readStage = marla_BACKEND_REQUEST_READING_CHUNK_SIZE;
        goto read_chunk_size;
    }

    return marla_WriteResult_CONTINUE;
}

marla_WriteResult marla_readBackendResponseBody(marla_Request* req)
{
    if(req->readStage > marla_BACKEND_REQUEST_READING_RESPONSE_BODY) {
        return marla_WriteResult_CONTINUE;
    }
    if(req->readStage < marla_BACKEND_REQUEST_READING_RESPONSE_BODY) {
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    char out[marla_BUFSIZE];
    memset(out, 0, sizeof out);
    marla_Connection* cxn = req->cxn;
    marla_Server* server = cxn->server;

    marla_WriteEvent result;
    marla_WriteEvent_init(&result, marla_WriteResult_CONTINUE);
    result.index = req->lastReadIndex;

    while(req->remainingContentLen != 0) {
        // Read response body.
        char buf[marla_BUFSIZE];
        memset(buf, 0, sizeof(buf));
        int requestedLen = req->remainingContentLen;
        if(requestedLen > sizeof(buf)) {
            requestedLen = sizeof(buf);
        }
        int nread = marla_Connection_read(cxn, (unsigned char*)buf, requestedLen);
        //printf("Read %d bytes of backend response body: %s.\n", nread, buf);
        if(nread == 0) {
            // Zero-length read indicates end of stream.
            if(req->remainingContentLen > 0) {
                marla_killRequest(req, "Premature end of backend response body.\n");
                return marla_WriteResult_KILLED;
            }
            break;
        }
        if(nread < 0) {
            return marla_WriteResult_UPSTREAM_CHOKED;
        }

        if(!req->handler) {
            req->totalContentLen += nread;
            req->remainingContentLen -= nread;
            continue;
        }

        // Handle input.
        marla_logMessagef(server, "I/O", "Consuming %d bytes of backend response body.", nread);
        req->totalContentLen += nread;
        req->remainingContentLen -= nread;

        result.buf = &buf;
        result.length = nread;
        req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, &result, -1);
        //fprintf(stderr, "RESPONSE_BODY gave %s\n", marla_nameWriteResult(result.status));
        result.buf = 0;

        switch(result.status) {
        case marla_WriteResult_CONTINUE:
            if(result.index < result.length) {
                marla_killRequest(req, "Partial read with continue.");
                req->totalContentLen -= nread - result.index;
                req->remainingContentLen += nread - result.index;
                marla_Connection_putbackRead(cxn, nread - result.index);
                result.index = 0;
                req->lastReadIndex = result.index;
                continue;
            }
            //printf("Continuing to read after continue.\n");
            result.index = 0;
            req->lastReadIndex = 0;
            marla_Connection_refill(req->cxn, 0);
            continue;
        case marla_WriteResult_DOWNSTREAM_CHOKED:
            // This request body was being streamed to some other downstream source, which could not process
            // all of this chunk's input.
            //printf("Stopping after downstream choked.\n");
            marla_Connection_putbackRead(cxn, nread);
            req->totalContentLen -= nread;
            req->remainingContentLen += nread;
            req->lastReadIndex = result.index;
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        case marla_WriteResult_UPSTREAM_CHOKED:
            if(result.length > 0 && result.index < result.length) {
                marla_killRequest(req, "Backend response handler indicated upstream choked despite partial read.");
                return 1;
            }
            //printf("Continuing to read after upstream choked. %d/%d\n", result.index, result.length);
            req->lastReadIndex = 0;
            result.index = 0;
            marla_Connection_refill(req->cxn, 0);
            continue;
        case marla_WriteResult_TIMEOUT:
            marla_logMessage(server, "Backend response handler timed out.");
            req->totalContentLen -= nread;
            req->remainingContentLen += nread;
            marla_Connection_putbackRead(cxn, nread);
            req->lastReadIndex = result.index;
            return marla_WriteResult_TIMEOUT;
        case marla_WriteResult_CLOSED:
            marla_logMessage(server, "Backend response handler indicated closed.");
            return marla_WriteResult_CLOSED;
        case marla_WriteResult_KILLED:
            marla_logMessage(server, "Backend response handler killed.");
            return marla_WriteResult_KILLED;
        case marla_WriteResult_LOCKED:
            marla_logMessage(server, "Backend response handler is locked.");
            //printf("Resetting once locked\n");
            if(result.index < result.length) {
                req->totalContentLen -= nread;
                req->remainingContentLen += nread;
                marla_Connection_putbackRead(cxn, nread);
                req->lastReadIndex = result.index;
                return marla_WriteResult_LOCKED;
            }
            else {
                req->lastReadIndex = 0;
                result.index = 0;
                marla_Connection_refill(req->cxn, 0);
                continue;
            }
        }
    }

    if(req->handler) {
        marla_WriteEvent_init(&result, marla_WriteResult_CONTINUE);
        for(; cxn->stage != marla_CLIENT_COMPLETE && req->readStage < marla_BACKEND_REQUEST_AFTER_RESPONSE;) {
            req->handler(req, marla_BACKEND_EVENT_RESPONSE_BODY, &result, -1);
            switch(result.status) {
            case marla_WriteResult_CONTINUE:
                continue;
            case marla_WriteResult_UPSTREAM_CHOKED:
            case marla_WriteResult_DOWNSTREAM_CHOKED:
            case marla_WriteResult_TIMEOUT:
            case marla_WriteResult_LOCKED:
            case marla_WriteResult_KILLED:
            case marla_WriteResult_CLOSED:
                return result.status;
            }
        }
    }
    else {
        req->readStage = marla_BACKEND_REQUEST_AFTER_RESPONSE;
    }

    return marla_WriteResult_CONTINUE;
}

static marla_WriteResult marla_processBackendTrailer(marla_Request* req)
{
    if(req->readStage < marla_BACKEND_REQUEST_READING_RESPONSE_TRAILERS) {
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    marla_Connection* cxn = req->cxn;
    while(req->readStage == marla_BACKEND_REQUEST_READING_RESPONSE_TRAILERS) {
        char fieldLine[MAX_FIELD_NAME_LENGTH + 2 + MAX_FIELD_VALUE_LENGTH + 2];
        memset(fieldLine, 0, sizeof(fieldLine));
        int nread = marla_Connection_read(cxn, (unsigned char*)fieldLine, sizeof(fieldLine));
        if(nread == 0) {
            return marla_WriteResult_CLOSED;
        }
        if(nread < 0) {
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
            if(fieldLine[i] == '\r' && fieldLine[i + 1] == '\n') {
                if(i >= nread - 1) {
                    marla_Connection_putbackRead(cxn, nread);
                    return marla_WriteResult_UPSTREAM_CHOKED;
                }
                if(fieldLine[i + 1] != '\n') {
                    marla_killRequest(req, "Trailer line is not terminated properly, so no valid request.");
                    return marla_WriteResult_KILLED;
                }
                fieldLine[i] = 0;
                foundNewline = 1;
                marla_Connection_putbackRead(cxn, nread - i - 2);
                break;
            }
            char c = fieldLine[i];
            if((c <= 0x1f && c != '\t') || c == 0x7f) {
                marla_killRequest(req, "Backend response trailer contains control characters, so no valid request.\n");
                return marla_WriteResult_KILLED;
            }
            if(!foundSeparator && (c == '<' || c == '>' || c == '#' || c == '%')) {
                marla_killRequest(req, "Backend response trailer contains delimiters, so no valid request.\n");
                return marla_WriteResult_KILLED;
            }
            if(!foundSeparator && (c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`')) {
                marla_killRequest(req, "Backend response trailer contains unwise characters, so no valid request.\n");
                return marla_WriteResult_KILLED;
            }
            if(!foundSeparator && !isalnum(c) && c != '-' && c != '\t') {
                if(c == ':' && i > 0 && i < nread - 1) {
                    foundSeparator = i;
                    fieldLine[i] = 0;
                    toleratingSpaces = 1;
                    continue;
                }
                marla_killRequest(req, "Header name contains non alphanumeric characters, so no valid request.\n");
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
            marla_killRequest(req, "Backend response header is too long, so no valid response.\n");
            return marla_WriteResult_KILLED;
        }
        if(!foundNewline) {
            // Incomplete;
            marla_Connection_putbackRead(cxn, nread);
            return marla_WriteResult_UPSTREAM_CHOKED;
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
            req->readStage = marla_BACKEND_REQUEST_DONE_READING;
            break;
        }
    }

    return marla_WriteResult_CONTINUE;
}

marla_WriteResult marla_backendRead(marla_Connection* cxn)
{
    if(cxn->stage == marla_CLIENT_COMPLETE) {
        return marla_WriteResult_CLOSED;
    }

    if(cxn->in_read) {
        marla_logMessagecf(cxn->server, "Processing", "Called to read from backend, but backend is already being read.");
        return marla_WriteResult_LOCKED;
    }
    cxn->in_read = 1;

    marla_logEntercf(cxn->server, "Processing", "Reading from backend.");
    marla_Connection_refill(cxn, 0);
    marla_Server* server = cxn->server;
    marla_Request* req = cxn->current_request;
    char out[marla_BUFSIZE];
    memset(out, 0, sizeof out);

    while(req) {
        marla_Request_ref(req);
        marla_WriteResult wr;

        wr = marla_processBackendResponseLine(req);
        if(wr != marla_WriteResult_CONTINUE) {
            if(wr == marla_WriteResult_CLOSED) {
                goto exit_closed;
            }
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }

        wr = marla_processBackendResponseFields(req);
        if(wr != marla_WriteResult_CONTINUE) {
            if(wr == marla_WriteResult_CLOSED) {
                goto exit_closed;
            }
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }

        wr = marla_readBackendResponseBody(req);
        if(wr != marla_WriteResult_CONTINUE) {
            if(wr == marla_WriteResult_CLOSED) {
                goto exit_closed;
            }
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }

        wr = marla_readBackendResponseChunks(req);
        if(wr != marla_WriteResult_CONTINUE) {
            if(wr == marla_WriteResult_CLOSED) {
                goto exit_closed;
            }
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }

        if(req->readStage == marla_BACKEND_REQUEST_AFTER_RESPONSE) {
            if(req->expect_trailer) {
                req->readStage = marla_BACKEND_REQUEST_READING_RESPONSE_TRAILERS;
            }
            else {
                req->readStage = marla_BACKEND_REQUEST_DONE_READING;
            }
        }

        wr = marla_processBackendTrailer(req);
        if(wr != marla_WriteResult_CONTINUE) {
            if(wr == marla_WriteResult_CLOSED) {
                goto exit_closed;
            }
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return wr;
        }

        if(req->readStage == marla_BACKEND_REQUEST_DONE_READING) {
            if(cxn->current_request == cxn->latest_request) {
                cxn->current_request = 0;
                cxn->latest_request = 0;
            }
            else {
                cxn->current_request = req->next_request;
            }
            --cxn->requests_in_process;

            req->next_request = 0;
            if(req->next_request) {
                req->next_request = 0;
            }

            if(req->backendPeer) {
                for(int loop = 1; loop;) {
                    marla_WriteResult wr = marla_clientWrite(req->backendPeer->cxn);
                    switch(wr) {
                    case marla_WriteResult_CONTINUE:
                        continue;
                    case marla_WriteResult_CLOSED:
                        // Client just SIGPIPE'd on the backend.
                        goto exit_closed;
                    case marla_WriteResult_LOCKED:
                    case marla_WriteResult_DOWNSTREAM_CHOKED:
                    default:
                        loop = 0;
                        continue;
                    }
                }
                if(cxn->stage == marla_CLIENT_COMPLETE || cxn->shouldDestroy) {
                    marla_Request_unref(req);
                    goto shutdown;
                }
                if(req->backendPeer && req->backendPeer->writeStage < marla_CLIENT_REQUEST_DONE_WRITING) {
                    // Peer still needs this request.
                    if(req->close_after_done) {
                        marla_logMessage(server, "Closing after request completed.");
                        marla_Request_unref(req);
                        goto shutdown;
                    }
                    marla_Request_unref(req);
                    req = cxn->current_request;
                    marla_logMessage(server, "Peer still needs this request; it will not be destroyed.");
                    continue;
                }
            }
            if(req->close_after_done) {
                marla_logMessage(server, "Closing after request completed.");
                marla_Request_unref(req);
                goto shutdown;
            }

            marla_Request_unref(req);
            req = cxn->current_request;
            if(cxn->stage == marla_CLIENT_COMPLETE) {
                goto shutdown;
            }
        }
        else {
            marla_killRequest(req, "Unexpected request stage.");
            marla_Request_unref(req);
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return marla_WriteResult_KILLED;
        }
        if(cxn->stage == marla_CLIENT_COMPLETE && !cxn->shouldDestroy) {
            // Client needs shutdown.
            if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
                cxn->shouldDestroy = 1;
            }
            marla_logLeave(server, 0);
            cxn->in_read = 0;
            return marla_WriteResult_CLOSED;
        }
    }

    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return marla_WriteResult_UPSTREAM_CHOKED;

shutdown:
    cxn->stage = marla_CLIENT_COMPLETE;
    if(!cxn->shouldDestroy) {
        // Client needs shutdown.
        if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
            cxn->shouldDestroy = 1;
        }
    }
    marla_logLeave(server, 0);
    cxn->in_read = 0;
    return marla_WriteResult_CLOSED;
exit_closed:
    marla_logLeave(server, 0);
    cxn->in_read = 0;
    cxn->shouldDestroy = 1;
    return marla_WriteResult_CLOSED;
}

void marla_backendHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int len)
{
    marla_Server* server = req->cxn->server;
    marla_logEntercf(server, "Handling", "Handling backend event on backend: %s", marla_nameClientEvent(ev));
    marla_BackendResponder* resp;

    char buf[2*marla_BUFSIZE];
    int bufLen;

    marla_WriteEvent* we;

    switch(ev) {
    case marla_BACKEND_EVENT_NEED_HEADERS:
        if(req->contentType[0] == 0) {
            if(req->backendPeer->contentType[0] != 0) {
                strncpy(req->contentType, req->backendPeer->contentType, sizeof req->contentType);
            }
            else {
                strncpy(req->contentType, "text/plain", sizeof req->contentType);
            }
        }
        if(req->acceptHeader[0] == 0) {
            if(req->backendPeer->acceptHeader[0] != 0) {
                strncpy(req->acceptHeader, req->backendPeer->acceptHeader, sizeof req->acceptHeader);
            }
            else {
                strncpy(req->acceptHeader, "*/*", sizeof req->acceptHeader);
            }
        }
        // Write the request headers to the backend.
        if(req->backendPeer->cookieHeader[0] != 0) {
            sprintf(buf, "Host: localhost:8081\r\nTransfer-Encoding: chunked\r\nCookie: %s\r\nContent-Type: %s\r\nAccept: %s\r\n\r\n", req->backendPeer->cookieHeader, req->contentType, req->acceptHeader);
        }
        else {
            sprintf(buf, "Host: localhost:8081\r\nTransfer-Encoding: chunked\r\nContent-Type: %s\r\nAccept: %s\r\n\r\n", req->contentType, req->acceptHeader);
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
    case marla_BACKEND_EVENT_RESPONSE_BODY:
        we = in;
        if(we->length == 0) {
            we->status = marla_WriteResult_CONTINUE;
            req->readStage = marla_BACKEND_REQUEST_AFTER_RESPONSE;
            marla_logLeave(server, 0);
            return;
        }
        resp = req->handlerData;
        if(!resp) {
            marla_die(req->cxn->server, "Backend peer must have responder handlerData\n");
        }
        len = we->length - we->index;
        in = we->buf + we->index;
        marla_logMessagef(server, "Forwarding %d bytes from backend's response to client request %d.", len, req->backendPeer->id);
        //printf("Forwarding %d (%d/%d) bytes from backend's response to client request %d.\n", len, we->index, we->length, req->backendPeer->id);
        int true_read = marla_Ring_write(resp->backendResponse, in, len);
        if(true_read < len) {
            //printf("Could only forward %d\n", true_read);
            we->index += true_read;
        }
        else {
            //printf("Forwarded all %d.\n", true_read);
            we->index += len;
        }
        for(int loop = 1; loop;) {
            we->status = marla_clientWrite(req->backendPeer->cxn);
            //printf("client write gave %s.\n", marla_nameWriteResult(we->status));
            switch(we->status) {
            case marla_WriteResult_CONTINUE:
                continue;
            default:
                loop = 0;
                continue;
            }
        }
        marla_logLeave(server, 0);
        return;
    case marla_BACKEND_EVENT_MUST_WRITE:
        marla_logMessage(server, "MUST_WRITE");
        we = in;
        resp = req->handlerData;
        for(;;) {
write_chunk:
            we->status = marla_writeChunk(server, resp->backendRequestBody, req->cxn->output);
            switch(we->status) {
            case marla_WriteResult_CONTINUE:
                continue;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
            case marla_WriteResult_LOCKED:
            case marla_WriteResult_KILLED:
            case marla_WriteResult_CLOSED:
            case marla_WriteResult_TIMEOUT:
                marla_logLeave(server, 0);
                return;
            case marla_WriteResult_UPSTREAM_CHOKED:
                marla_logMessage(server, "Upstream choked while writing chunk.");
                if(!marla_Ring_isEmpty(resp->backendRequestBody)) {
                    marla_die(req->cxn->server, "Chunk writer indicated no more request data, but there is data is still to be written");
                }
                for(; req->backendPeer && req->backendPeer->readStage <= marla_CLIENT_REQUEST_READING_REQUEST_BODY;) {
                    switch(marla_clientRead(req->backendPeer->cxn)) {
                    case marla_WriteResult_CONTINUE:
                        continue;
                    case marla_WriteResult_UPSTREAM_CHOKED:
                    case marla_WriteResult_DOWNSTREAM_CHOKED:
                    case marla_WriteResult_LOCKED:
                    case marla_WriteResult_KILLED:
                    case marla_WriteResult_CLOSED:
                    case marla_WriteResult_TIMEOUT:
                        // Found some more data (ignore the status), continue.
                        if(!marla_Ring_isEmpty(resp->backendRequestBody)) {
                            goto write_chunk;
                        }
                        if(req->backendPeer->readStage == marla_CLIENT_REQUEST_DONE_READING) {
                            continue;
                        }
                        marla_logMessagef(req->cxn->server, "Client has not completed writing their request. %s", marla_nameRequestReadStage(req->backendPeer->readStage));
                        marla_logLeave(server, 0);
                        return;
                    }
                }
                we->status = marla_writeChunkTrailer(req->cxn->output);
                if(we->status == marla_WriteResult_CONTINUE) {
                    marla_logMessage(server, "Finished writing chunk trailer");
                    req->writeStage = marla_BACKEND_REQUEST_DONE_WRITING;
                }
                else {
                    marla_logMessage(server, "Failed to write chunk trailer");
                }
                marla_logLeave(server, 0);
                return;
            }
        }
        marla_die(req->cxn->server, "Unreachable");
    case marla_EVENT_DESTROYING:
        if(req->backendPeer) {
            marla_logMessagef(server,
                "Destroying peer (request %d) of backend request %d: %s",
                req->backendPeer->id,req->id,
                marla_nameRequestReadStage(req->backendPeer->readStage)
            );
            marla_clientRead(req->backendPeer->cxn);
            if(req->backendPeer) {
                marla_clientWrite(req->backendPeer->cxn);
                if(req->backendPeer) {
                    req->backendPeer->backendPeer = 0;
                }
            }
        }
        req->backendPeer = 0;
        resp = req->handlerData;
        if(resp) {
            marla_BackendResponder_free(resp);
            req->handlerData = 0;
            req->handler = 0;
        }
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

static marla_WriteResult marla_writeBackendClientHandlerResponse(marla_Request* req, marla_WriteEvent* we)
{
    marla_Server* server = req->cxn->server;

    if(req->readStage <= marla_CLIENT_REQUEST_READING_FIELD) {
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    if(!req->backendPeer) {
        marla_killRequest(req, "Client request %d lost its backend", req->id);
        return marla_WriteResult_KILLED;
    }
    if(req->backendPeer->error[0] != 0) {
        marla_killRequest(req, "Client request %d' backend request errored: %s", req->id, req->backendPeer->error);
        return marla_WriteResult_KILLED;
    }
    if(req->backendPeer->readStage <= marla_BACKEND_REQUEST_READING_HEADERS) {
        int rv = marla_backendRead(req->backendPeer->cxn);
        if(!req->backendPeer) {
            marla_killRequest(req, "Client request %d lost its backend", req->id);
            return marla_WriteResult_KILLED;
        }
        if(req->backendPeer->readStage <= marla_BACKEND_REQUEST_READING_HEADERS) {
            marla_logMessagef(req->cxn->server, "Not a good time to write %s %s %d", marla_nameRequestReadStage(req->backendPeer->readStage), marla_nameRequestReadStage(req->readStage), rv);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
    }
    marla_BackendResponder* resp = req->backendPeer->handlerData;

    //fprintf(stderr, "HandleStage: %s\n", marla_nameBackendResponderStage(resp->handleStage));
    if(resp->handleStage == marla_BackendResponderStage_STARTED) {
        marla_logMessagef(req->cxn->server, "Generating initial client response from backend input");
        if(resp->handler) {
            resp->handler(resp);
        }
        else {
            resp->handleStage = marla_BackendResponderStage_RESPONSE_LINE;
        }
        if(resp->handleStage != marla_BackendResponderStage_RESPONSE_LINE) {
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
    }
    if(resp->handleStage == marla_BackendResponderStage_RESPONSE_LINE) {
        marla_logMessagef(req->cxn->server, "Sending response line to client from backend");
        char buf[marla_BUFSIZE];
        if(req->statusCode == 0) {
            req->statusCode = req->backendPeer->statusCode;
        }
        if(req->statusLine[0] == 0) {
            const char* statusLine = marla_getDefaultStatusLine(req->statusCode);
            strncpy(req->statusLine, statusLine, sizeof req->statusLine);
        }

        int responseLen;
        // Write the backend's response header to the client.
        if(req->backendPeer->responseLen == marla_MESSAGE_USES_CLOSE || req->backendPeer->responseLen == marla_MESSAGE_LENGTH_UNKNOWN) {
            marla_logMessagef(req->cxn->server, "Sending connection-bound response to client: %d", req->backendPeer->requestLen);
            responseLen = snprintf(buf, sizeof buf, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\n",
                req->backendPeer->statusCode,
                req->backendPeer->statusLine,
                req->backendPeer->contentType
            );
            if(responseLen < 0) {
                marla_die(server, "Failed to generate initial backend response headers");
            }
        }
        else if(req->backendPeer->responseLen == marla_MESSAGE_IS_CHUNKED) {
            marla_logMessagef(req->cxn->server, "Sending chunked response to client: %d", req->backendPeer->requestLen);
            responseLen = snprintf(buf, sizeof buf, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\n",
                req->backendPeer->statusCode,
                req->backendPeer->statusLine,
                req->backendPeer->contentType
            );
            if(responseLen < 0) {
                marla_die(server, "Failed to generate initial backend response headers");
            }
        }
        else {
            req->remainingResponseLen = req->backendPeer->responseLen;
            marla_logMessagef(req->cxn->server, "Sending %d-byte response to client", req->backendPeer->responseLen);
            responseLen = snprintf(buf, sizeof buf, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n",
                req->backendPeer->statusCode,
                req->backendPeer->statusLine,
                req->backendPeer->contentType,
                req->backendPeer->responseLen
            );
            if(responseLen < 0) {
                marla_die(server, "Failed to generate initial backend response headers");
            }
        }
        int true_written = marla_Connection_write(req->cxn, buf, responseLen);
        if(true_written < responseLen) {
            if(true_written > 0) {
                marla_Connection_putbackWrite(req->cxn, true_written);
            }
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        ++resp->handleStage;
    }

    while(resp->handleStage == marla_BackendResponderStage_LOCATION_HEADER) {
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
        int true_written = marla_Connection_write(req->cxn, buf, len);
        if(true_written < len) {
            if(true_written > 0) {
                marla_Connection_putbackWrite(req->cxn, true_written);
            }
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        ++resp->handleStage;
    }

    while(resp->handleStage == marla_BackendResponderStage_SET_COOKIE_HEADER) {
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
        marla_logMessagef(req->cxn->server, "Sending chunked client request: %d", req->backendPeer->responseLen);
        int true_written = marla_Connection_write(req->cxn, buf, len);
        if(true_written < len) {
            if(true_written > 0) {
                marla_Connection_putbackWrite(req->cxn, true_written);
            }
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        ++resp->handleStage;
    }

    while(resp->handleStage == marla_BackendResponderStage_TERMINAL_HEADER) {
        marla_logMessagef(req->cxn->server, "Sending terminal header to client from backend");
        int true_written = marla_Connection_write(req->cxn, "\r\n", 2);
        if(true_written < 2) {
            if(true_written > 0) {
                marla_Connection_putbackWrite(req->cxn, true_written);
            }
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        ++resp->handleStage;
    }

    while(resp->handleStage == marla_BackendResponderStage_RESPONSE) {
        //marla_logMessagef(req->cxn->server, "Writing backend response to client");
        while(!marla_Ring_isEmpty(req->cxn->output)) {
            int nflushed;
            marla_WriteResult wr = marla_Connection_flush(req->cxn, &nflushed);
            switch(wr) {
            case marla_WriteResult_CLOSED:
                return marla_WriteResult_CLOSED;
            case marla_WriteResult_UPSTREAM_CHOKED:
                continue;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                //fprintf(stderr, "Choked while flushing client response\n");
                return marla_WriteResult_DOWNSTREAM_CHOKED;
            default:
                marla_die(server, "Unhandled flush result");
            }
        }

        if(req->backendPeer) {
            //fprintf(stderr, "Invoking backend\n");
            switch(marla_backendRead(req->backendPeer->cxn)) {
            case marla_WriteResult_CONTINUE:
                continue;
            default:
                break;
            }
        }

        if(req->backendPeer->responseLen == marla_MESSAGE_IS_CHUNKED) {
            //fprintf(stderr, "Writing chunked response\n");
            switch(marla_writeChunk(server, resp->backendResponse, req->cxn->output)) {
            case marla_WriteResult_CONTINUE:
                continue;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                continue;
            case marla_WriteResult_UPSTREAM_CHOKED:
                if(!marla_Ring_isEmpty(resp->backendResponse)) {
                    marla_die(req->cxn->server, "Chunk writer indicated no more data, but there is data is still to be written");
                }
                if(req->backendPeer->readStage < marla_BACKEND_REQUEST_DONE_READING) {
                    marla_backendRead(req->backendPeer->cxn);
                    if(req->backendPeer->readStage < marla_BACKEND_REQUEST_DONE_READING) {
                        marla_logMessagef(req->cxn->server, "Backend has not finished writing their response. %s", marla_nameRequestReadStage(req->backendPeer->readStage));
                        return marla_WriteResult_UPSTREAM_CHOKED;
                    }
                    continue;
                }
                switch(marla_writeChunkTrailer(req->cxn->output)) {
                case marla_WriteResult_CONTINUE:
                    marla_logMessage(req->cxn->server, "Wrote chunk trailer.");
                    ++resp->handleStage;
                    continue;
                default:
                    marla_logMessage(req->cxn->server, "Failed to write chunk trailer.");
                    return marla_WriteResult_DOWNSTREAM_CHOKED;
                }
            default:
                marla_die(server, "Unreachable");
            }
        }
        else {
            //printf("Writing fixed-length response\n");
            void* in;
            size_t len;
            marla_Ring_writeSlot(req->cxn->output, &in, &len);
            if(len == 0) {
                //printf("Unable to write any more!\n");
                continue;
            }

            //unsigned char tmp[marla_BUFSIZE + 1];
            //int nread = marla_Ring_read(resp->backendResponse, tmp, sizeof tmp - 1);
            //if(nread > 0) {
                //tmp[nread] = 0;
                //printf("Backend response: %s\n", tmp);
                //marla_Ring_putbackRead(resp->backendResponse, nread);
            //}

            int nread = marla_Ring_read(resp->backendResponse, in, len);
            if(nread > 0) {
                //unsigned char c = ((unsigned char*)in)[nread];
                //((unsigned char*)in)[nread] = 0;
                //printf("Wrote %d bytes to output: %s\n", nread, in);
                //((unsigned char*)in)[nread] = c;
            }


            if(nread < len) {
                marla_Ring_putbackWrite(req->cxn->output, len - nread);
                //printf("Putting back %d bytes\n", len - nread);
                if(marla_Ring_isEmpty(resp->backendResponse) && (
                    !req->backendPeer || req->backendPeer->readStage == marla_BACKEND_REQUEST_DONE_READING)
                ) {
                    //fprintf(stderr, "Finished writing fixed-length response.\n");
                    resp->handleStage = marla_BackendResponderStage_FLUSHING;
                    break;
                }

                //nread = marla_Ring_read(req->cxn->output, tmp, sizeof tmp - 1);
                //if(nread > 0) {
                    //tmp[nread] = 0;
                    //printf("Client response: %s\n", tmp);
                    //marla_Ring_putbackRead(req->cxn->output, nread);
                //}

                //marla_dumpRequest(req->backendPeer);
                marla_logMessagef(server, "Choked after writing %ld\n", nread);
                //printf("Choked after writing %zu response bytes into %zu-byte output buffer slot. Size=%d\n", nread, len, marla_Ring_size(req->cxn->output));
                return marla_WriteResult_UPSTREAM_CHOKED;
            }
            else {
                //printf("Wrote %ld bytes.\n", nread);
            }
            //for(int i = 0; i < len; ++i) {
                //unsigned char c = ((unsigned char*)in)[i];
                //if(c == 0) {
                    //marla_killRequest(req, "Wrote a null!");
                    //return marla_WriteResult_KILLED;
                //}
            //}
            if(req->responseLen != marla_MESSAGE_USES_CLOSE) {
                req->remainingResponseLen -= nread;
                if(req->remainingResponseLen < 0) {
                    marla_killRequest(req, "Wrote too much in response.");
                    return marla_WriteResult_KILLED;
                }
                if(req->remainingResponseLen == 0) {
                    ++resp->handleStage;
                    continue;
                }
            }
        }
    }

    while(resp->handleStage == marla_BackendResponderStage_FLUSHING) {
        marla_logMessagef(server, "Flushing %d bytes of input data.", marla_Ring_size(req->cxn->output));
        while(!marla_Ring_isEmpty(req->cxn->output)) {
            int nflushed;
            marla_WriteResult wr = marla_Connection_flush(req->cxn, &nflushed);
            switch(wr) {
            case marla_WriteResult_CLOSED:
                return marla_WriteResult_CLOSED;
            case marla_WriteResult_UPSTREAM_CHOKED:
                continue;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                marla_logMessage(server, "Choked while flushing client response");
                return marla_WriteResult_DOWNSTREAM_CHOKED;
            default:
                marla_die(server, "Unhandled flush result");
            }
        }
        ++resp->handleStage;
    }

    if(resp->handleStage == marla_BackendResponderStage_DONE) {
        marla_logMessagef(server, "Processed all input data.");
        req->writeStage = marla_CLIENT_REQUEST_AFTER_RESPONSE;
        return marla_WriteResult_CONTINUE;
    }

    marla_die(server, "Invalid BackendResponder handleStage=%d", resp->handleStage);
    return marla_WriteResult_KILLED;
}

static void marla_backendClientHandlerAcceptRequest(struct marla_Request* req)
{
    marla_Request* backendReq = marla_Request_new(req->cxn->server->backend);
    strcpy(backendReq->uri, req->uri);
    strcpy(backendReq->method, req->method);
    backendReq->handler = marla_backendHandler;
    backendReq->handlerData = marla_BackendResponder_new(marla_BUFSIZE, backendReq);

    // Set backend peers.
    backendReq->backendPeer = req;
    req->backendPeer = backendReq;

    // Enqueue the backend request.
    marla_Backend_enqueue(req->cxn->server, backendReq);
}

static void marla_backendClientHandlerRequestBody(struct marla_Request* req, marla_WriteEvent* we)
{
    marla_BackendResponder* resp = req->backendPeer->handlerData;
    if(we->length == 0) {
        req->readStage = marla_CLIENT_REQUEST_DONE_READING;
        return;
    }
    if(!req->backendPeer) {
        marla_die(req->cxn->server, "Backend request missing.");
    }
    if(we->length < we->index) {
        marla_die(req->cxn->server, "Backend request index, %zu, is greater than length, %zu.", we->index, we->length);
    }
    for(int loop = 1; loop && we->length - we->index > 0;) {
        //fprintf(stderr, "Writing %d/%zu bytes for backend request.\n", we->index, we->length);
        int true_written = marla_BackendResponder_writeRequestBody(resp, we->buf + we->index, we->length - we->index);
        if(true_written < we->length - we->index) {
            we->index += true_written;
        }
        else {
            we->index = we->length;
        }

        we->status = marla_backendWrite(req->backendPeer->cxn);
        switch(we->status) {
        case marla_WriteResult_UPSTREAM_CHOKED:
            if(true_written > 0) {
                we->status = marla_WriteResult_CONTINUE;
                continue;
            }
            loop = 0;
            continue;
        case marla_WriteResult_CONTINUE:
            continue;
        default:
            loop = 0;
            continue;
        }
    }
}

void marla_backendClientHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int given_len)
{
    marla_Server* server = req->cxn->server;
    marla_logEntercf(server, "Handling", "Handling backend event on client: %s\n", marla_nameClientEvent(ev));
    marla_WriteEvent* we;
    switch(ev) {
    case marla_EVENT_HEADER:
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_ACCEPTING_REQUEST:
        // Accept the request.
        marla_backendClientHandlerAcceptRequest(req);
        (*(int*)in) = 1;
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_REQUEST_BODY:
        marla_backendClientHandlerRequestBody(req, in);
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_MUST_WRITE:
        we = in;
        we->status = marla_writeBackendClientHandlerResponse(req, we);
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_DESTROYING:
        marla_logLeave(server, 0);
        return;
    default:
        marla_logLeave(server, "Defaulted");
        return;
    }

    marla_die(server, "Unreachable");
}

const char* marla_nameBackendResponderStage(enum marla_BackendResponderStage stage)
{
    switch(stage) {
    case marla_BackendResponderStage_STARTED: return "STARTED";
    case marla_BackendResponderStage_RESPONSE_LINE: return "RESPONSE_LINE";
    case marla_BackendResponderStage_LOCATION_HEADER: return "LOCATION_HEADER";
    case marla_BackendResponderStage_SET_COOKIE_HEADER: return "SET_COOKIE_HEADER";
    case marla_BackendResponderStage_TERMINAL_HEADER: return "TERMINAL_HEADER";
    case marla_BackendResponderStage_RESPONSE: return "RESPONSE";
    case marla_BackendResponderStage_FLUSHING: return "FLUSHING";
    case marla_BackendResponderStage_DONE: return "DONE";
    }
    return "?";
}
