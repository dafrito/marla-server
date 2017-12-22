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

int parsegraph_writeChunk(struct parsegraph_ChunkedPageRequest* cpr, parsegraph_Ring* output)
{
    size_t remaining = cpr->message_len - cpr->written;
    void* slotData;
    size_t slotLen;
    parsegraph_Ring_writeSlot(output, &slotData, &slotLen);
    char* slot = slotData;

    // Ignore slots of insufficient size.
    if(slotLen < 5) {
        if(slotLen > 0) {
            parsegraph_Ring_putbackWrite(output, slotLen);
        }
        return -1;
    }

    // Incorporate message padding into chunk.
    int suffix_len = 2;
    int prefix_len = 3; // One byte plus \r\n
    int padding = prefix_len + suffix_len;
    if(slotLen - padding > 0xf) {
        prefix_len = 4; // Two bytes plus \r\n
        padding = prefix_len + suffix_len;
        if(slotLen - padding > 0xff) {
            prefix_len = 5; // Three bytes plus \r\n
            padding = prefix_len + suffix_len;
            if(slotLen - padding > 0xfff) {
                prefix_len = 6; // Four bytes plus \r\n
                padding = prefix_len + suffix_len;
            }
        }
    }
    if(slotLen - padding > remaining) {
        parsegraph_Ring_putbackWrite(output, slotLen - padding - remaining);
        slotLen = padding + remaining;
    }

    // Construct the chunk within the slot.
    int true_prefix_size = snprintf(slot, prefix_len + 1, "%x\r\n", slotLen - padding);
    if(true_prefix_size < 0) {
        //fprintf(stderr, "Failed to generate chunk header.\n");
        return -1;
    }
    if(true_prefix_size != prefix_len) {
        //fprintf(stderr, "Realized prefix length %d must match the calculated prefix length %d for %d bytes remaining with slotLen of %d.\n", true_prefix_size, prefix_len, remaining, slotLen);
        return -1;
    }
    memcpy(slot + true_prefix_size, cpr->resp + cpr->written, slotLen - padding);
    slot[slotLen - suffix_len] = '\r';
    slot[slotLen - suffix_len + 1] = '\n';

    cpr->written += slotLen - padding;

    if(cpr->written == cpr->message_len) {
        cpr->stage = 3;
    }
    return 0;
}

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

static void makePage(struct parsegraph_ClientRequest* req, struct parsegraph_ChunkedPageRequest* cpr)
{
    cpr->message_len = snprintf(cpr->resp, sizeof(cpr->resp), "<!DOCTYPE html><html><head><script>function run() { WS=new WebSocket(\"ws://localhost:%s/\"); WS.onopen = function() { console.log('Default handler.'); }; setInterval(function() { WS.send('Hello'); console.log('written'); }, 1000); }</script></head><body onload='run()'>Hello, <b>world.</b><p>This is request %d</body></html>", SERVERPORT ? SERVERPORT : "443", req->id);
}

void parsegraph_default_request_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    if(req->stage == parsegraph_CLIENT_REQUEST_WEBSOCKET) {
        parsegraph_default_websocket_handler(req, ev, data, datalen);
        return;
    }

    struct parsegraph_ChunkedPageRequest* cpr;
    int* acceptor;
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
            struct parsegraph_ChunkedPageRequest* cpr = malloc(sizeof(struct parsegraph_ChunkedPageRequest));
            memset(cpr->resp, 0, sizeof(cpr->resp));
            cpr->written = 0;
            cpr->message_len = 0;
            cpr->stage = 0;
            req->handleData = cpr;
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
        nread = parsegraph_Connection_read(req->cxn, buf, sizeof buf);
        if(nread <= 0) {
            return;
        }

        break;
    case parsegraph_EVENT_GENERATE:
        makePage(req, req->handleData);
        break;
    case parsegraph_EVENT_RESPOND:
        //fprintf(stderr, "Default responder\n");
        cpr = req->handleData;
        if(!req->handleData) {
            fprintf(stderr, "No handle data.\n");
            req->cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }

        if(cpr->stage == 0) {
            req->handle(req, parsegraph_EVENT_GENERATE, 0, 0);
            cpr->stage = 1;
        }

        if(cpr->stage == 1) {
            const unsigned char* header = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
            int needed = strlen(header);
            int nwritten = parsegraph_Connection_write(req->cxn, header, needed);
            if(nwritten < needed) {
                if(nwritten > 0) {
                    parsegraph_Connection_putbackWrite(req->cxn, nwritten);
                }
                return;
            }
            cpr->stage = 2;
        }

        while(cpr->stage == 2) {
            parsegraph_writeChunk(cpr, req->cxn->output);
        }

        if(cpr->stage == 3) {
            int nwritten = parsegraph_Connection_write(req->cxn, "0\r\n\r\n", 5);
            if(nwritten < 5) {
                if(nwritten > 0) {
                    parsegraph_Connection_putbackWrite(req->cxn, nwritten);
                }
                return;
            }
            cpr->stage = 4;
        }

        if(cpr->stage == 4) {
            // Mark connection as complete.
            req->stage = parsegraph_CLIENT_REQUEST_DONE;
        }
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
