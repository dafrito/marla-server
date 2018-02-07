#include "marla.h"
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>
#include <httpd.h>
#include <http_config.h>
#include <http_protocol.h>
#include <ap_config.h>
#include <apr_dbd.h>
#include <mod_dbd.h>

AP_DECLARE(void) ap_log_perror_(const char *file, int line, int module_index,
                                int level, apr_status_t status, apr_pool_t *p,
                                const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char exp[512];
    memset(exp, 0, sizeof(exp));
    vsprintf(exp, fmt, args);
    dprintf(3, exp);
    va_end(args);
}


void duplexHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int len)
{
    marla_Server* server = req->cxn->server;
    switch(ev) {
    case marla_EVENT_HEADER:
        return;
    case marla_EVENT_ACCEPTING_REQUEST:
        (*(int*)in) = 1;
        return;
    case marla_EVENT_REQUEST_BODY:
        return;
    case marla_EVENT_MUST_WRITE:
        marla_Ring_writeStr(req->cxn->output, "HTTP/1.1 200 OK\r\n\r\n");
        goto done;
    case marla_EVENT_DESTROYING:
        return;
    default:
        return;
    }

    marla_die(server, "Unreachable");
done:
    (*(int*)in) = 1;
    marla_logLeave(server, "Done");
    return;
choked:
    (*(int*)in) = -1;
    marla_logLeave(server, "Choked");
    return;
}

void duplexHook(struct marla_Request* req, void* hookData)
{
    req->handler = duplexHandler;
}

void setToDataHandler(struct marla_Request* req, void* hookData)
{
    req->handler = hookData;
}

int test1()
{
    fprintf(stderr, "test duplex\n");
    marla_Server server;
    marla_Server_init(&server);
    marla_Server_addHook(&server, marla_SERVER_HOOK_ROUTE, duplexHook, 0);

    marla_Connection* client = marla_Connection_new(&server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r\nHost: localhost:%s\r\nAccept: */*\r\n\r\n", server.serverport);
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        return 1;
    }

    marla_clientRead(client);
    marla_clientWrite(client);

    memset(source_str, 0, sizeof(source_str));
    nwritten = marla_readDuplex(client, source_str, sizeof(source_str));
    //fprintf(stderr, "%d: %s\n", nwritten, source_str);

    marla_Connection_destroy(client);
    marla_Server_free(&server);
    return 0;
}

static char headerKey[1024];
static char headerValue[1024];

void headerHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int len)
{
    marla_Server* server = req->cxn->server;
    switch(ev) {
    case marla_EVENT_HEADER:
        strcpy(headerKey, in);
        strcpy(headerValue, in + len);
        return;
    case marla_EVENT_ACCEPTING_REQUEST:
        (*(int*)in) = 1;
        return;
    case marla_EVENT_REQUEST_BODY:
        return;
    case marla_EVENT_MUST_WRITE:
        marla_Ring_writeStr(req->cxn->output, "HTTP/1.1 200 OK\r\n\r\n");
        goto done;
    case marla_EVENT_DESTROYING:
        return;
    default:
        return;
    }

    marla_die(server, "Unreachable");
done:
    (*(int*)in) = 1;
    marla_logLeave(server, "Done");
    return;
choked:
    (*(int*)in) = -1;
    marla_logLeave(server, "Choked");
    return;
}

int test2()
{
    marla_Server server;
    marla_Server_init(&server);
    marla_Server_addHook(&server, marla_SERVER_HOOK_ROUTE, setToDataHandler, headerHandler);

    marla_Connection* client = marla_Connection_new(&server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r\n");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        return 1;
    }

    marla_clientRead(client);
    marla_clientWrite(client);

    memset(headerKey, 0, sizeof(headerKey));
    memset(headerValue, 0, sizeof(headerValue));

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "Host: localhost\r\n");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        return 1;
    }
    marla_clientRead(client);
    marla_clientWrite(client);

    if(strcmp(headerKey, "Host")) {
        return 1;
    }
    if(strcmp(headerValue, "localhost")) {
        return 1;
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "Accept: */*\r\n");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        return 1;
    }
    marla_clientRead(client);
    marla_clientWrite(client);

    if(strcmp(headerKey, "Accept")) {
        fprintf(stderr, "No accept header: %s\n", headerKey);
        return 1;
    }
    if(strcmp(headerValue, "*/*")) {
        return 1;
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\n");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        return 1;
    }
    marla_clientRead(client);
    marla_clientWrite(client);

    memset(source_str, 0, sizeof(source_str));
    nwritten = marla_readDuplex(client, source_str, sizeof(source_str));
    if(nwritten == 0) {
        return 1;
    }
    else {
        //write(2, source_str, nwritten);
    }

    marla_Connection_destroy(client);
    marla_Server_free(&server);
    return 0;
}

int main()
{
    int fails = 0;
    int rv = test1();
    if(rv != 0) {
        fprintf(stderr, "test1 failed\n");
        ++fails;
    }

    rv = test2();
    if(rv != 0) {
        fprintf(stderr, "test2 failed\n");
        ++fails;
    }
    return fails;
}
