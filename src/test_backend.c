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
    int rv = marla_Ring_read(ring, sink, len);
    if(rv <= 0) {
        return -1;
    }
    return rv;
}

static int writeDuplexSource(struct marla_Connection* cxn, void* source, size_t len)
{
    marla_Ring* outputRing = ((marla_Ring**)cxn->source)[1];
    int rv = marla_Ring_write(outputRing, source, len);
    if(rv <= 0) {
        return -1;
    }
    return rv;
}

static void destroyDuplexSource(struct marla_Connection* cxn)
{
    //fprintf(stderr, "Destroying duplex source\n");
    marla_Ring** rings = ((marla_Ring**)cxn->source);
    marla_Ring_free(rings[0]);
    marla_Ring_free(rings[1]);
}

void backendHook(struct marla_Request* req, void* hookData)
{
    //fprintf(stderr, "HOOK CALLED!!! %s\n", req->uri);
    if(!strncmp(req->uri, "/user", 5)) {
        // Check for suitable termination
        if(req->uri[5] != 0 && req->uri[5] != '/' && req->uri[5] != '?') {
            return;
        }
        // Install backend handler.
        req->handler = marla_backendClientHandler;
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
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r\nHost: localhost:%s\r\nAccept: */*\r\n\r\n", server->serverport);
    marla_Ring_write(clientRings[0], source_str, nwritten);

    // Read from the source.
    marla_clientRead(client);
    marla_Ring_writeStr(backendRings[0], "HTTP/1.1 200 OK\r\n\r\n");
    marla_clientWrite(client);

    if(client->current_request) {
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);

    return 0;
}

static int test_backend_with_large_content(struct marla_Server* server)
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
    backend->stage = marla_BACKEND_READY;
    backend->is_backend = 1;
    backend->source = backendRings;
    backend->readSource = readDuplexSource;
    backend->writeSource = writeDuplexSource;
    backend->destroySource = destroyDuplexSource;

    client->server->backend = backend;
    client->server->backendPort = server->serverport + 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r\nHost: localhost:%s\r\nAccept: */*\r\n\r\n", server->serverport);
    marla_Ring_write(clientRings[0], source_str, nwritten);

    unsigned char that_big_buf[9998];

    // Read from the source.
    marla_clientRead(client);
    marla_clientWrite(client);

    if(!client->current_request) {
        return 1;
    }
    marla_Request* req = client->current_request;
    if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        printf("ReadStage");
        return 1;
    }
    if(req->writeStage != marla_CLIENT_REQUEST_WRITING_RESPONSE) {
        printf("WriteStage");
        return 1;
    }
    if(req->backendPeer->writeStage != marla_BACKEND_REQUEST_DONE_WRITING) {
        printf("Backend WriteStage: %s\n", marla_nameRequestWriteStage(req->backendPeer->writeStage));
        return 1;
    }
    if(req->backendPeer->readStage != marla_BACKEND_REQUEST_READING_RESPONSE_LINE) {
        printf("Backend ReadStage: %s\n", marla_nameRequestReadStage(req->backendPeer->readStage));
        return 1;
    }
    marla_Ring_writeStr(backendRings[0], "HTTP/1.1 200 OK\r\nContent-Length: 9998\r\n\r\n");
    marla_clientWrite(client);
    marla_clientRead(backend);
    if(req->backendPeer->readStage != marla_BACKEND_REQUEST_READING_RESPONSE_BODY) {
        printf("Backend ReadStage at %s\n", marla_nameRequestReadStage(req->backendPeer->readStage));
        return 1;
    }

    unsigned char written_buf[9998];
    for(int i = 0; i < 9998; ++i) {
        switch(i%4) {
        case 0:
            written_buf[i] = 'A';
            break;
        case 1:
            written_buf[i] = 'B';
            break;
        case 2:
            written_buf[i] = 'C';
            break;
        case 3:
            written_buf[i] = 'D';
            break;
        }
    }
    int write_index = 0;
    int index = 0;
    while(index < sizeof(that_big_buf)) {
        //printf("Streaming backend: index=%d write_index=%d\n", index, write_index);
        int nread = marla_Ring_read(backendRings[0], that_big_buf + index, sizeof(that_big_buf) - index);
        if(nread < 0) {
            printf("Backend died");
            return 1;
        }
        index += nread;

        if(client->requests_in_process == 0) {
            //fprintf(stderr, "Lost the request: %d\n", index);
            //fprintf(stderr, (char*)that_big_buf);
            marla_Connection_destroy(client);
            marla_Connection_destroy(backend);
            return 1;
        }

        marla_clientWrite(client);
        marla_clientRead(backend);
        marla_clientWrite(backend);
        marla_clientRead(client);

        write_index += marla_Ring_write(backendRings[0], written_buf + write_index, sizeof(written_buf) - write_index);
    }
    //fprintf(stderr, (char*)that_big_buf);

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

    printf("test_backend_with_large_content:");
    if(0 == test_backend_with_large_content(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    return failed;
}
