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

void handle_simple(parsegraph_ClientRequest* req, int event, void* in, size_t len)
{
    // Write the reply.
    char resp[1024];
    memset(resp, 0, 1024);
    int rv = snprintf(resp, 1023, "HTTP/1.1 200 OK\r\n\r\n<html><body>Hello, <b>world.</b><br/>%d<pre>%s</pre></body></html>", strlen(requestHeaders), requestHeaders);
    if(rv < 0) {
        dprintf(3, "Failed to generate response.");
        client->nature.client.stage = parsegraph_CLIENT_COMPLETE;
        return;
    }
    int nwritten = parsegraph_Connection_write(cxn, resp, rv);
    if(nwritten <= 0) {
        common_SSL_return(client, nwritten);
        return;
    }
    if(nwritten < rv) {
        parsegraph_Connection_putbackWrite(cxn->output, 
    }
}

struct parsegraph_FormRequest{
int stage;
char formName[MAX_FIELD_VALUE_LENGTH + 1];
long int nameLen;
char formValue[MAX_FIELD_VALUE_LENGTH + 1];
long int valueLen;
};

struct parsegraph_FormRequest* parsegraph_FormRequest_new()
{
    struct parsegraph_FormRequest* session = malloc(sizeof(struct parsegraph_FormRequest));
    session->stage = 0;
    session->nameLen = 0;
    session->valueLen = 0;
    memset(session->formName, 0, sizeof(session->formName));
    memset(session->formValue, 0, sizeof(session->formValue));
    return session;
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

void parsegraph_processFormRequest(struct parsegraph_ClientRequest* req, struct parsegraph_FormRequest* session, void* data, int datalen)
{
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
