#include "marla.h"
#include <string.h>
#include <unistd.h>
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>
#include <parsegraph_user.h>
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

static int readDuplexSource(struct marla_Connection* cxn, void* sink, size_t len)
{
    marla_Ring* ring = ((marla_Ring**)cxn->source)[0];
    return marla_Ring_read(ring, sink, len);
}

static int writeDuplexSource(struct marla_Connection* cxn, void* source, size_t len)
{
    return len;
    //marla_Ring* outputRing = ((marla_Ring**)cxn->source)[1];
    //write(0, source, len);
    //return marla_Ring_write(outputRing, source, len);
}

static void destroyDuplexSource(struct marla_Connection* cxn)
{
    marla_Ring** rings = ((marla_Ring**)cxn->source);
    marla_Ring_free(rings[0]);
    marla_Ring_free(rings[1]);
}

static void backendHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int len)
{
    fprintf(stderr, "BACKEND %s\n", marla_nameClientEvent(ev));

    char buf[marla_BUFSIZE];
    int bufLen;

    switch(ev) {
    case marla_BACKEND_EVENT_NEED_HEADERS:
        strcpy(buf, "Host: localhost:8081\r\n\r\n");
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
        // Writing to the backend means flushing resp->input to the backend
        // connection and potentially generating more content to fill it.
        goto done;
    case marla_BACKEND_EVENT_NEED_TRAILERS:
        goto done;
    default:
        return;
    }

done:
    (*(int*)in) = 1;
    return;
choked:
    (*(int*)in) = -1;
}

static void marla_backendResponderClientHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int len)
{
    fprintf(stderr, "Client %s\n", marla_nameClientEvent(ev));
    marla_BackendResponder* resp;
    switch(ev) {
    case marla_EVENT_HEADER:
        break;
    case marla_EVENT_ACCEPTING_REQUEST:
        // Accept the request.
        (*(int*)in) = 1;

        marla_Request* backendReq = marla_Request_new(req->cxn->server->backend);
        strcpy(backendReq->uri, req->uri);
        strcpy(backendReq->method, req->method);
        backendReq->handler = backendHandler;
        backendReq->handlerData = marla_BackendResponder_new(marla_BUFSIZE, backendReq);

        // Set backend peers.
        backendReq->backendPeer = req;
        req->backendPeer = backendReq;

        // Enqueue the backend request.
        fprintf(stderr, "ENQUEUED!!!\n");
        marla_Backend_enqueue(req->cxn->server->backend, backendReq);

        break;
    case marla_EVENT_REQUEST_BODY:
        fprintf(stderr, "REQUESTBODYWRITE!!!\n");
        resp = req->backendPeer->handlerData;
        if(!resp) {
            fprintf(stderr, "Backend peer must have responder handlerData\n");
            abort();
        }
        // Read the client's request body into resp->input and flush resp->input afterwards.
        break;
    case marla_EVENT_MUST_WRITE:
        // Check the input buffer.
        resp = req->backendPeer->handlerData;
        if(!resp) {
            marla_die(req->cxn->server, "Backend peer must have responder handlerData\n");
        }
        // Write resp->output to the client.
        marla_BackendResponder_flushOutput(resp);
        (*(int*)in) = -1;
        break;
    case marla_EVENT_DESTROYING:
        break;
    default:
        break;
    }
}

void backendHook(struct marla_Request* req, void* hookData)
{
    fprintf(stderr, "HOOK CALLED!!! %s\n", req->uri);
    if(!strncmp(req->uri, "/user", 5)) {
        // Check for suitable termination
        if(req->uri[5] != 0 && req->uri[5] != '/' && req->uri[5] != '?') {
            return;
        }
        // Install backend handler.
        req->handler = marla_backendResponderClientHandler;
    }
}

static int test_backend(struct marla_Server* server)
{
    marla_Server_addHook(server, marla_SERVER_HOOK_ROUTE, backendHook, 0);

    // Create the client connection.
    marla_Ring* clientRings[2];
    clientRings[0] = marla_Ring_new(marla_BUFSIZE);
    clientRings[1] = marla_Ring_new(marla_BUFSIZE);
    marla_Connection* client = marla_Connection_new(server);
    client->source = clientRings;
    client->readSource = readDuplexSource;
    client->writeSource = writeDuplexSource;
    client->destroySource = destroyDuplexSource;

    // Create the backend connection.
    marla_Ring* backendRings[2];
    backendRings[0] = marla_Ring_new(marla_BUFSIZE);
    backendRings[1] = marla_Ring_new(marla_BUFSIZE);
    marla_Connection* backend = marla_Connection_new(server);
    backend->source = backendRings;
    backend->readSource = readDuplexSource;
    backend->writeSource = writeDuplexSource;
    backend->destroySource = destroyDuplexSource;

    client->server->backend = backend;
    client->server->backendPort = server->serverport + 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring_write(clientRings[0], source_str, nwritten);

    // Read from the source.
    marla_clientRead(client);
    marla_Ring_writeStr(backendRings[0], "HTTP/1.1 200 OK\r\n\r\n");
    marla_clientWrite(client);

    if(!client->current_request) {
        return 1;
    }
    if(client->current_request->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        return 1;
    }
    if(client->current_request->writeStage != marla_CLIENT_REQUEST_WRITING_RESPONSE) {
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);

    return 0;
}

int main(int argc, char* argv[])
{
    if(argc < 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return -1;
    }

    struct marla_Server server;
    marla_Server_init(&server);
    strcpy(server.serverport, argv[1]);

    int failed = 0;

    printf("test_backend:");
    if(0 == test_backend(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    return failed;
}
