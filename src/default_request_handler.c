#include "rainback.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>
#include <openssl/rand.h>

static int percentDecodeEnplace(char* data, int datalen)
{
    int d = 0;
    for(int s = 0; s < datalen; ++s) {
        char c = data[s];
        if(c == '+') {
            c = ' ';
        }
        else if(c == '%') {
            if(s >= datalen - 2) {
                return -1;
            }

            char hex[3];
            hex[0] = data[s+1];
            hex[1] = data[s+2];
            hex[2] = 0;
            char* endptr = 0;
            long int z = strtol(hex, &endptr, 16);
            if(endptr == hex + 3 && z >= 0 && z <= 255) {
                c = (char)z;
            }
            else {
                return -1;
            }

            // Move to the first hex character.
            ++s;

            // Move to the second hex character.
            ++s;
        }
        data[d++] = c;
    }
    for(int i = d; i < datalen; ++i) {
        data[i] = 0;
    }
    return d;
}

struct parsegraph_FormRequest{
int stage;
char formName[MAX_FIELD_VALUE_LENGTH + 1];
long int nameLen;
char formValue[MAX_FIELD_VALUE_LENGTH + 1];
long int valueLen;
};

void parsegraph_default_request_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    if(req->stage == parsegraph_CLIENT_REQUEST_WEBSOCKET) {
        parsegraph_default_websocket_handler(req, ev, data, datalen);
        return;
    }

    int* acceptor;
    unsigned char resp[parsegraph_BUFSIZE];
    unsigned char buf[parsegraph_BUFSIZE + 1];
    int nread;
    memset(buf, 0, sizeof buf);
    switch(ev) {
    case parsegraph_EVENT_HEADER:
        break;
    case parsegraph_EVENT_ACCEPTING_REQUEST:
        acceptor = data;
        *acceptor = 1;
        if(!strcasecmp(req->method, "POST")) {
            req->handleData = malloc(sizeof(struct parsegraph_FormRequest));
            struct parsegraph_FormRequest* session = req->handleData;
            session->stage = 0;
            session->nameLen = 0;
            session->valueLen = 0;
            memset(session->formName, 0, sizeof(session->formName));
            memset(session->formValue, 0, sizeof(session->formValue));
        }
        else {
            req->handleData = 0;
        }
        break;
    case parsegraph_EVENT_FORM_FIELD:
        if(!strcasecmp(req->method, "POST")) {
            struct parsegraph_FormRequest* session = req->handleData;
            fprintf(stdout, "%s=%s\n", session->formName, session->formValue);
            fflush(stdout);
        }
        break;
    case parsegraph_EVENT_REQUEST_BODY:
        if(!strcasecmp(req->method, "POST")) {
            struct parsegraph_FormRequest* session = req->handleData;
            if(datalen == 0 && session->stage != 0) {
                if(session->stage == 1 && session->valueLen == 0) {
                    dprintf(3, "Unexpected end of form fields.");
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }

                int d = percentDecodeEnplace(session->formName, session->nameLen);
                if(d < 0) {
                    dprintf(3, "Illegal form name.");
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                session->nameLen = d;

                d = percentDecodeEnplace(session->formValue, session->valueLen);
                if(d < 0) {
                    dprintf(3, "Illegal form name.");
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                session->valueLen = d;

                req->handle(req, parsegraph_EVENT_FORM_FIELD, 0, 0);

                session->stage = 0;
                session->nameLen = 0;
                session->valueLen = 0;
                memset(session->formName, 0, sizeof(session->formName));
                memset(session->formValue, 0, sizeof(session->formValue));
            }
            for(int i = 0; i < datalen; ++i) {
                char c = ((char*)data)[i];
                // URLs are written only with the graphic printable characters of the
                // US-ASCII coded character set. The octets 80-FF hexadecimal are not
                // used in US-ASCII, and the octets 00-1F and 7F hexadecimal represent
                // control characters; these must be encoded.
                if(c >= 0x80 || c <= 0x1f || c == 0x7f) {
                    snprintf(req->error, sizeof req->error, "Illegal form character.");
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                if(c == '=') {
                    // Name-value separator.
                    if(session->stage == 0) {
                        session->stage = 1;
                    }
                    else {
                        snprintf(req->error, sizeof req->error, "Illegal form name-value separator.");
                        req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                }
                else if(c == '&') {
                    // Pair separator.
                    if(session->stage != 1) {
                        snprintf(req->error, sizeof req->error, "Illegal form pair separator.");
                        req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }

                    int d = percentDecodeEnplace(session->formName, session->nameLen);
                    if(d < 0) {
                        snprintf(req->error, sizeof req->error, "Illegal form name.");
                        req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    session->nameLen = d;

                    d = percentDecodeEnplace(session->formValue, session->valueLen);
                    if(d < 0) {
                        snprintf(req->error, sizeof req->error, "Illegal form value.");
                        req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                        return;
                    }
                    session->valueLen = d;

                    req->handle(req, parsegraph_EVENT_FORM_FIELD, 0, 0);

                    session->stage = 0;
                    session->nameLen = 0;
                    session->valueLen = 0;
                    memset(session->formName, 0, sizeof(session->formName));
                    memset(session->formValue, 0, sizeof(session->formValue));
                }
                else {
                    if(session->stage == 0) {
                        // Starting to read.
                        session->formName[session->nameLen++] = c;
                    }
                    else {
                        session->formValue[session->valueLen++] = c;
                    }
                }
            }
        }
        break;
    case parsegraph_EVENT_READ:
        nread = parsegraph_Connection_read(req->cxn, resp, parsegraph_BUFSIZE);
        if(nread <= 0) {
            return;
        }

        break;
    case parsegraph_EVENT_RESPOND:
        memset(resp, 0, sizeof(resp));

        const unsigned char* header = "HTTP/1.1 404 Not Found\r\nTransfer-Encoding: chunked\r\n\r\n";
        int nwritten = parsegraph_Connection_write(req->cxn, header, strlen(header));
        if(nwritten <= 0) {
            return;
        }

        int cs = 0;
        int message_len = snprintf(buf, sizeof buf, "<!DOCTYPE html><html><head><script>function run() { }, 500); }</script></head><body onload='run()'><h1>404 Not Found</h1><p>This is request %d</body></html>", req->id);

        for(int i = 0; i <= message_len; ++i) {
            if((i == message_len) || (i && !(i & (sizeof(buf) - 2)))) {
                if(i & (sizeof(buf) - 2)) {
                    buf[i & (sizeof(buf) - 2)] = 0;
                }
                int rv = snprintf(resp, 1023, "%x\r\n", cs);
                if(rv < 0) {
                    dprintf(3, "Failed to generate response.");
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                memcpy(resp + rv, buf, cs);
                resp[rv + cs] = '\r';
                resp[rv + cs + 1] = '\n';

                int nwritten = parsegraph_Connection_write(req->cxn, resp, rv + cs + 2);
                if(nwritten <= 0) {
                    return;
                }
                cs = 0;
            }
            if(i == message_len) {
                break;
            }
            buf[i & (sizeof(buf) - 2)] = buf[i];
            ++cs;
        }

        nwritten = parsegraph_Connection_write(req->cxn, "0\r\n\r\n", 5);
        if(nwritten <= 0) {
            return;
        }

        // Mark connection as complete.
        req->stage = parsegraph_CLIENT_REQUEST_DONE;
        break;
    case parsegraph_EVENT_DESTROYING:
        if(req->handleData) {
            free(req->handleData);
            req->handleData = 0;
        }
        break;
    }
}

void (*default_request_handler)(struct parsegraph_ClientRequest*, enum parsegraph_ClientEvent, void*, int) = parsegraph_default_request_handler;
