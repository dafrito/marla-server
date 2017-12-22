#include "rainback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>
#include <ctype.h>

static int readSource(parsegraph_Connection* cxn, void* sink, size_t len)
{
    parsegraph_BackendSource* cxnSource = cxn->source;
    return read(cxnSource->fd, sink, len);
}

static int writeSource(parsegraph_Connection* cxn, void* source, size_t len)
{
    parsegraph_BackendSource* cxnSource = cxn->source;
    return write(cxnSource->fd, source, len);
}

static void acceptSource(parsegraph_Connection* cxn)
{
    // Accepted and secured.
    cxn->stage = parsegraph_BACKEND_READY;
}

static int shutdownSource(parsegraph_Connection* cxn)
{
    return 1;
}

static void destroySource(parsegraph_Connection* cxn)
{
    parsegraph_BackendSource* source = cxn->source;
    close(source->fd);
    free(source);

    cxn->source = 0;
    cxn->readSource = 0;
    cxn->writeSource = 0;
    cxn->acceptSource = 0;
    cxn->shutdownSource = 0;
    cxn->destroySource = 0;
}

void parsegraph_Backend_init(parsegraph_Connection* cxn, int fd)
{
    parsegraph_BackendSource* source = malloc(sizeof *source);
    cxn->source = source;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->acceptSource = acceptSource;
    cxn->shutdownSource = shutdownSource;
    cxn->destroySource = destroySource;
    source->fd = fd;
}

void parsegraph_Backend_enqueue(parsegraph_Connection* cxn, parsegraph_ClientRequest* req)
{
    req->cxn = cxn;
    req->stage = parsegraph_BACKEND_REQUEST_FRESH;

    if(!cxn->current_request) {
        cxn->current_request = req;
        cxn->latest_request = req;
    }
    else {
        cxn->latest_request->next_request = req;
        cxn->latest_request = req;
    }
}

void parsegraph_backendWrite(parsegraph_Connection* cxn)
{
    parsegraph_ClientRequest* req = cxn->current_request;
    char out[1024];
    while(req) {
        // Write request line, headers, and body to backend.
        if(req->stage == parsegraph_BACKEND_REQUEST_FRESH) {
            int nwrit = snprintf(out, sizeof(out), "GET %s HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", req->uri, req->server->backendPort);
            int nw = parsegraph_Connection_write(cxn, out, nwrit);
            if(nw < nwrit) {
                parsegraph_Connection_putbackWrite(cxn, nw);
                return;
            }
            //req->stage = parsegraph_BACKEND_REQUEST_WRITING_REQUEST_BODY;
            //req->stage = parsegraph_BACKEND_REQUEST_WRITTEN;
        }
        //if(req->stage == parsegraph_BACKEND_REQUEST_WRITING_REQUEST_BODY) {
            // TODO Write client's request to backend's output,
            // Writing client's request body to backend.
            //parsegraph_Connection_write(req->cxn, , );
            // Also potentially write backend's response body to client.
            //parsegraph_Connection_write(req->cxn, , );

            // No more client request body.
            //req->stage = parsegraph_BACKEND_REQUEST_WRITING_RESPONSE_BODY;
        //}
        //if(req->stage == parsegraph_BACKEND_REQUEST_WRITING_RESPONSE_BODY) {
            // TODO Write backend's response to client's output.
            //parsegraph_Connection_write(req->cxn, , );

            //req->stage = parsegraph_BACKEND_REQUEST_WRITTEN;
            //req = req->next_request;
        //}
    }
}

void parsegraph_backendRead(parsegraph_Connection* cxn)
{
    parsegraph_ClientRequest* req = cxn->current_request;
    char out[1024];
    while(req) {
        // BACKEND EPOLLIN: Async read response line, headers, and body from backend.
        if(req->stage == parsegraph_BACKEND_REQUEST_FRESH) {
            continue;
        }
        //if(req->stage == parsegraph_BACKEND_REQUEST_WRITING_REQUEST_BODY) {
            // TODO Read from request's input
            //parsegraph_Connection_read(req->cxn, out, sizeof out);
            //continue;
        //}
        if(req->stage == parsegraph_BACKEND_REQUEST_WRITTEN) {
            int nr = parsegraph_Connection_read(cxn, out, sizeof out);
            if(nr <= 0) {
                return;
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
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                        snprintf(req->error, sizeof req->error, "Response line contains delimiters, so no valid request.\n");
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                        snprintf(req->error, sizeof req->error, "Response line contains unwise characters, so no valid request.\n");
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    if(lineStage < 2 && c == ' ') {
                        out[i] = 0;
                        if(lineStage == 0) {
                            if(strcmp(start, "HTTP/1.1")) {
                                snprintf(req->error, sizeof req->error, "Response line contains unexpected version, so no valid request.\n");
                                cxn->stage = parsegraph_CLIENT_COMPLETE;
                                return;
                            }
                        }
                        else {
                            char* endptr = 0;
                            req->statusCode = strtol(start, &endptr, 10);
                            if(endptr != out + i) {
                                snprintf(req->error, sizeof req->error, "Response line contains invalid status code, so no valid request.\n");
                                cxn->stage = parsegraph_CLIENT_COMPLETE;
                                return;
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
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                }
            }

            if(lineStage != 2) {
                snprintf(req->error, sizeof req->error, "Response line ended prematurely.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            strncpy(req->statusLine, start, sizeof req->statusLine);

            req->stage = parsegraph_BACKEND_REQUEST_READING_HEADERS;
        }

        if(req->stage == parsegraph_BACKEND_REQUEST_READING_HEADERS) {
            int nr = parsegraph_Connection_read(cxn, out, sizeof out);
            if(nr <= 0) {
                return;
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
                        cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }

                    strncpy(responseHeaderValue, start, sizeof responseHeaderValue);
                    break;
                }
                char c = out[i];
                if(c <= 0x1f || c == 0x7f) {
                    snprintf(req->error, sizeof req->error, "Response line contains control characters, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                if(c == '<' || c == '>' || c == '#' || c == '%' || c == '"') {
                    snprintf(req->error, sizeof req->error, "Response line contains delimiters, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                if(c == '{' || c == '}' || c == '|' || c == '\\' || c == '^' || c == '[' || c == ']' || c == '`') {
                    snprintf(req->error, sizeof req->error, "Response line contains unwise characters, so no valid request.\n");
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
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
                    cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
            }
            if(i == 0) {
                // End of response headers.
                req->stage = parsegraph_BACKEND_REQUEST_READING_RESPONSE_BODY;
                // or chunked, or done
            }
            else if(lineStage != 1) {
                snprintf(req->error, sizeof req->error, "Response line ended prematurely.\n");
                cxn->stage = parsegraph_CLIENT_COMPLETE;
                return;
            }
            else {
                // TODO Process HTTP header (i.e. responseHeaderKey and responseHeaderValue)
                if(!strcmp(responseHeaderKey, "Transfer-Encoding")) {

                }
                else {

                }
            }
        }

        if(req->stage == parsegraph_BACKEND_REQUEST_READING_RESPONSE_BODY) {
            // TODO Read the data in, feed it to the original request.
            char transfer[parsegraph_BUFSIZE];
            int nr = parsegraph_Connection_read(req->cxn, transfer, sizeof parsegraph_BUFSIZE);
            if(nr < 0) {
                return;
            }

            if(req->expect_trailer) {
                req->stage = parsegraph_BACKEND_REQUEST_READING_RESPONSE_TRAILER;
            }
            req->stage = parsegraph_BACKEND_REQUEST_DONE;
        }

        if(req->stage == parsegraph_BACKEND_REQUEST_READING_RESPONSE_TRAILER) {
            // TODO Read trailer.

            req->stage = parsegraph_BACKEND_REQUEST_DONE_READING;
        }

        if(req->stage == parsegraph_BACKEND_REQUEST_DONE_READING) {
            req = req->next_request;
        }
    }
}
