#include "marla.h"
#include <string.h>
#include <unistd.h>
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>
#include <httpd.h>
#include <http_config.h>
#include <http_protocol.h>
#include <ap_config.h>
#include <apr_dbd.h>
#include <mod_dbd.h>

static int test_file(struct marla_Server* server)
{
    marla_Server_addHook(server, marla_ServerHook_ROUTE, backendHook, 0);

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
    backend->is_backend = 1;
    backend->source = backendRings;
    backend->readSource = readDuplexSource;
    backend->writeSource = writeDuplexSource;
    backend->destroySource = destroyDuplexSource;

    client->backendPeer = backend;
    backend->backendPeer = client;
    server->backendPort = server->serverport + 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r\nHost: localhost:%s\r\nAccept: */*\r\n\r\n", server->serverport);
    marla_Ring_write(clientRings[0], source_str, nwritten);

    // Read from the source.
    marla_clientRead(client);

    unsigned char buf[1024];
    int read = marla_Ring_read(backendRings[1], buf, 1024);
    if(read <= 0) {
        fprintf(stderr, "No bytes written to backend yet");
        return 1;
    }
    marla_clientWrite(client);

    if(!client->current_request) {
        fprintf(stderr, "Client lost its request prematurely\n");
        return 1;
    }
    marla_Request* req = client->current_request;
    if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        fprintf(stderr, "Request not done reading\n");
        marla_dumpRequest(req);
        return 1;
    }
    if(req->writeStage != marla_CLIENT_REQUEST_WRITING_RESPONSE) {
        fprintf(stderr, "Request not responding\n");
        marla_dumpRequest(req);
        return 1;
    }
    marla_Ring_writeStr(backendRings[0], "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    marla_clientRead(backend);
    marla_clientWrite(backend);
    marla_clientWrite(client);
    marla_clientRead(client);
    if(backend->current_request) {
        fprintf(stderr, "Backend should have no request\n");
        marla_dumpRequest(backend->current_request);
        marla_dumpRequest(client->current_request);
        return 1;
    }
    if(client->current_request) {
        fprintf(stderr, "Client should have no request\n");
        marla_dumpRequest(client->current_request);
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

    printf("Testing file ...\n");
    apr_initialize();

    struct marla_Server server;
    marla_Server_init(&server);
    strcpy(server.serverport, argv[1]);

    int failed = 0;

    printf("test_file:");
    if(0 == test_file(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    fprintf(stderr, "%d failures\n", failed);
    apr_terminate();
    return failed;
}
