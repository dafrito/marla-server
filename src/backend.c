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

struct marla_BackendResponder* marla_BackendResponder_new(size_t bufSize, marla_ClientRequest* req)
{
    if(!req) {
        fprintf(stderr, "Backend request must be given\n");
        abort();
    }
    marla_BackendResponder* resp = malloc(sizeof *resp);
    resp->input = marla_Ring_new(bufSize);
    resp->output = marla_Ring_new(bufSize);
    resp->handleStage = 0;
    resp->index = 0;
    resp->req = req;
    return resp;
}

void marla_BackendResponder_flushOutput(marla_BackendResponder* resp)
{
    marla_ClientRequest* req = resp->req->backendPeer;
    for(;;) {
        void* data;
        size_t len;
        marla_Ring_readSlot(resp->output, &data, &len);
        if(len == 0) {
            return;
        }
        if(len > 0) {
            int true_written = marla_Connection_write(req->cxn, data, len);
            if(true_written < len) {
                marla_Ring_putbackRead(resp->output, len - true_written);
                return;
            }
        }
    }
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

void marla_Backend_enqueue(marla_Connection* cxn, marla_ClientRequest* req)
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
    marla_backendRead(cxn);
}

int marla_backendWrite(marla_Connection* cxn)
{
    marla_ClientRequest* req = cxn->current_request;
    char out[marla_BUFSIZE];
    while(req) {
        fprintf(stderr, "Reading req: %s\n", marla_nameRequestWriteStage(req->writeStage));
        // Write request line to backend.
        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_REQUEST_LINE) {
            if(req->method[0] == 0) {
                marla_killClientRequest(req, "Method must be provided");
                return -1;
            }
            if(req->uri[0] == 0) {
                marla_killClientRequest(req, "URI must be provided");
                return -1;
            }
            int nwrit = snprintf(out, sizeof(out), "%s %s HTTP/1.1\r\n", req->method, req->uri);
            int nw = marla_Connection_write(cxn, out, nwrit);
            if(nw < nwrit) {
                marla_Connection_putbackWrite(cxn, nw);
                return -1;
            }
            req->writeStage = marla_BACKEND_REQUEST_WRITING_HEADERS;
        }
        if(req->writeStage == marla_BACKEND_REQUEST_WRITING_HEADERS) {
            // Write headers.

            int result = 0;
            req->handle(req, marla_BACKEND_EVENT_NEED_HEADERS, &result, 0);
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
            req->handle(req, marla_BACKEND_EVENT_MUST_WRITE, &result, 0);
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
            req->handle(req, marla_BACKEND_EVENT_NEED_TRAILERS, &result, 0);
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

int marla_backendRead(marla_Connection* cxn)
{
    marla_ClientRequest* req = cxn->current_request;
    char out[marla_BUFSIZE];
    while(req) {
        if(req->readStage == marla_BACKEND_REQUEST_READING_RESPONSE_LINE) {
            int nr = marla_Connection_read(cxn, (unsigned char*)out, sizeof out);
            if(nr <= 0) {
                return -1;
            }
            char* start = out;
            int lineStage = 0;
            for(int i = 0; i < nr; ++i) {
                if(out[i] == '\n' || (i < nr -1 && out[i] == '\r' && out[i + 1] == '\n')) {
                    // Found the end-of-line.
                    out[i] = 0;
                    break;
                }
                else {
                    char c = out[i];
                    if(c <= 0x1f || c == 0x7f) {
                        snprintf(req->error, sizeof req->error, "Response line contains control characters, so no valid request.\n");
                        cxn->stage = marla_CLIENT_COMPLETE;
                        return -1;
                    }
                    if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                        snprintf(req->error, sizeof req->error, "Response line contains delimiters, so no valid request.\n");
                        cxn->stage = marla_CLIENT_COMPLETE;
                        return -1;
                    }
                    if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                        snprintf(req->error, sizeof req->error, "Response line contains unwise characters, so no valid request.\n");
                        cxn->stage = marla_CLIENT_COMPLETE;
                        return -1;
                    }
                    if(lineStage < 2 && c == ' ') {
                        out[i] = 0;
                        if(lineStage == 0) {
                            if(strcmp(start, "HTTP/1.1")) {
                                snprintf(req->error, sizeof req->error, "Response line contains unexpected version, so no valid request.\n");
                                cxn->stage = marla_CLIENT_COMPLETE;
                                return -1;
                            }
                        }
                        else {
                            char* endptr = 0;
                            req->statusCode = strtol(start, &endptr, 10);
                            if(endptr != out + i) {
                                snprintf(req->error, sizeof req->error, "Response line contains invalid status code, so no valid request.\n");
                                cxn->stage = marla_CLIENT_COMPLETE;
                                return -1;
                            }
                        }
                        while(i < nr && out[i] == ' ') {
                            ++i;
                        }
                        if(i == nr) {
                            break;
                        }
                        start = out + i;
                        ++lineStage;
                    }
                    if(!isascii(c)) {
                        snprintf(req->error, sizeof req->error, "Response line contains non-ASCII characters, so no valid request.\n");
                        cxn->stage = marla_CLIENT_COMPLETE;
                        return -1;
                    }
                }
            }

            if(lineStage != 2) {
                snprintf(req->error, sizeof req->error, "Response line ended prematurely.\n");
                cxn->stage = marla_CLIENT_COMPLETE;
                return -1;
            }
            strncpy(req->statusLine, start, sizeof req->statusLine);

            req->readStage = marla_BACKEND_REQUEST_READING_HEADERS;
        }

        if(req->readStage == marla_BACKEND_REQUEST_READING_HEADERS) {
            int nr = marla_Connection_read(cxn, (unsigned char*)out, sizeof out);
            if(nr <= 0) {
                return -1;
            }
            char responseHeaderKey[MAX_FIELD_VALUE_LENGTH + 1];
            char responseHeaderValue[MAX_FIELD_VALUE_LENGTH + 1];
            memset(responseHeaderKey, 0, sizeof responseHeaderKey);
            memset(responseHeaderValue, 0, sizeof responseHeaderValue);
            char* start = out;
            int lineStage = 0;
            int i = 0;
            for(; i < nr; ++i) {
                if(out[i] == '\n' || (i < nr -1 && out[i] == '\r' && out[i + 1] == '\n')) {
                    // Found the end-of-line.
                    out[i] = 0;
                    if(lineStage == 0 && i == 0) {
                        // End of headers.
                        break;
                    }
                    if(lineStage != 1) {
                        snprintf(req->error, sizeof req->error, "Response line contains control characters, so no valid request.\n");
                        cxn->stage = marla_CLIENT_COMPLETE;
                        return -1;
                    }

                    strncpy(responseHeaderValue, start, sizeof responseHeaderValue);
                    break;
                }
                char c = out[i];
                if(c <= 0x1f || c == 0x7f) {
                    snprintf(req->error, sizeof req->error, "Response line contains control characters, so no valid request.\n");
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                    snprintf(req->error, sizeof req->error, "Response line contains delimiters, so no valid request.\n");
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                    snprintf(req->error, sizeof req->error, "Response line contains unwise characters, so no valid request.\n");
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
                if(lineStage == 0 && c == ':') {
                    if(lineStage == 0) {
                        strncpy(responseHeaderKey, start, sizeof responseHeaderKey);
                    }
                    else {
                    }
                    while(i < nr && out[i] == ' ') {
                        ++i;
                    }
                    if(i == nr) {
                        break;
                    }
                    start = out + i;
                    ++lineStage;
                }
                if(!isascii(c)) {
                    snprintf(req->error, sizeof req->error, "Response line contains non-ASCII characters, so no valid request.\n");
                    cxn->stage = marla_CLIENT_COMPLETE;
                    return -1;
                }
            }
            if(i == 0) {
                // End of response headers.
                req->readStage = marla_BACKEND_REQUEST_READING_RESPONSE_BODY;
                // or chunked, or done
            }
            else if(lineStage != 1) {
                snprintf(req->error, sizeof req->error, "Response line ended prematurely.\n");
                cxn->stage = marla_CLIENT_COMPLETE;
                return -1;
            }
            else {
                // TODO Process HTTP header (i.e. responseHeaderKey and responseHeaderValue)
                if(!strcmp(responseHeaderKey, "Transfer-Encoding")) {

                }
                else {

                }
            }
        }

        if(req->readStage == marla_BACKEND_REQUEST_READING_RESPONSE_BODY) {
            // TODO Read the data in, feed it to the original request.
            char transfer[marla_BUFSIZE];
            int nr = marla_Connection_read(req->cxn, (unsigned char*)transfer, sizeof marla_BUFSIZE);
            if(nr < 0) {
                return -1;
            }

            if(req->expect_trailer) {
                req->readStage = marla_BACKEND_REQUEST_READING_RESPONSE_TRAILER;
            }
            req->readStage = marla_BACKEND_REQUEST_DONE_READING;
        }

        if(req->readStage == marla_BACKEND_REQUEST_READING_RESPONSE_TRAILER) {
            // TODO Read trailer.

            req->readStage = marla_BACKEND_REQUEST_DONE_READING;
        }

        if(req->readStage == marla_BACKEND_REQUEST_DONE_READING) {
            req = req->next_request;
        }
    }

    return 0;
}
