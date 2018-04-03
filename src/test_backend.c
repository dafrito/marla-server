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

AP_DECLARE(void) ap_log_perror_(const char *file, int line, int module_index,
                                int level, apr_status_t status, apr_pool_t *p,
                                const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char exp[512];
    memset(exp, 0, sizeof(exp));
    int len = vsprintf(exp, fmt, args);
    write(3, exp, len);
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

static int test_backend_with_large_content(struct marla_Server* server)
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
    backend->stage = marla_BACKEND_READY;
    backend->is_backend = 1;
    backend->source = backendRings;
    backend->readSource = readDuplexSource;
    backend->writeSource = writeDuplexSource;
    backend->destroySource = destroyDuplexSource;

    client->backendPeer = backend;
    backend->backendPeer = client;
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
            //printf("Backend died");
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

static int test_BackendResponder(struct marla_Server* server)
{
    marla_Connection* cxn = marla_Connection_new(server);
    marla_Request* req = marla_Request_new(cxn);
    marla_BackendResponder* resp = marla_BackendResponder_new(marla_BUFSIZE, req);
    marla_BackendResponder_free(resp);
    marla_Request_unref(req);
    marla_Connection_destroy(cxn);
    return 0;
}

static int test_BackendResponder2(struct marla_Server* server)
{
    marla_Server_addHook(server, marla_ServerHook_ROUTE, backendHook, 0);

    marla_Connection* client = marla_Connection_new(server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    marla_Connection* backend = marla_Connection_new(server);
    marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

    client->backendPeer = backend;
    backend->backendPeer = client;
    backend->is_backend = 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r\nHost: localhost:%s\r\nAccept: */*\r\n\r", server->serverport);
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write first part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    if(client->current_request->readStage != marla_CLIENT_REQUEST_READING_FIELD) {
        fprintf(stderr, "Request is not reading fields\n");
        marla_dumpRequest(client->current_request);
        return 1;
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "\n");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    marla_Request* req = client->current_request;
    if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        fprintf(stderr, "Request is not done reading\n");
        marla_dumpRequest(req);
        return 1;
    }

    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);
    return 0;
}

static int test_BackendResponder3(struct marla_Server* server)
{
    marla_Server_addHook(server, marla_ServerHook_ROUTE, backendHook, 0);

    marla_Connection* client = marla_Connection_new(server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    marla_Connection* backend = marla_Connection_new(server);
    marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

    client->backendPeer = backend;
    backend->backendPeer = client;
    backend->is_backend = 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r\nHost: localhost:%s\r\nAccept: */*\r\n", server->serverport);
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write first part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    if(client->current_request->readStage != marla_CLIENT_REQUEST_READING_FIELD) {
        fprintf(stderr, "Request is not reading fields\n");
        marla_dumpRequest(client->current_request);
        return 1;
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\n");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    marla_Request* req = client->current_request;
    if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        fprintf(stderr, "Request is not done reading\n");
        marla_dumpRequest(req);
        return 1;
    }

    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);
    return 0;
}

static int test_BackendResponder4(struct marla_Server* server)
{
    marla_Server_addHook(server, marla_ServerHook_ROUTE, backendHook, 0);

    marla_Connection* client = marla_Connection_new(server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    marla_Connection* backend = marla_Connection_new(server);
    marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

    client->backendPeer = backend;
    backend->backendPeer = client;
    backend->is_backend = 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1\r");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write first part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    if(client->current_request->readStage != marla_CLIENT_REQUEST_READING_VERSION) {
        fprintf(stderr, "Request is not reading version\n");
        marla_dumpRequest(client->current_request);
        return 1;
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "\nHost: localhost:%s\r\nAccept: */*\r\n\r\n", server->serverport);
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    marla_Request* req = client->current_request;
    if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        fprintf(stderr, "Request is not done reading\n");
        marla_dumpRequest(req);
        return 1;
    }

    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);
    return 0;
}

static int test_BackendResponder5(struct marla_Server* server)
{
    marla_Server_addHook(server, marla_ServerHook_ROUTE, backendHook, 0);

    marla_Connection* client = marla_Connection_new(server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    marla_Connection* backend = marla_Connection_new(server);
    marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

    client->backendPeer = backend;
    backend->backendPeer = client;
    backend->is_backend = 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write first part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    if(client->current_request->readStage != marla_CLIENT_REQUEST_READING_VERSION) {
        fprintf(stderr, "Request is not reading version\n");
        marla_dumpRequest(client->current_request);
        return 1;
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\nHost: localhost:%s\r\nAccept: */*\r\n\r\n", server->serverport);
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    marla_Request* req = client->current_request;
    if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        fprintf(stderr, "Request is not done reading\n");
        marla_dumpRequest(req);
        return 1;
    }

    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);
    return 0;
}

static void generate_random_bytes(char* dest, size_t len, unsigned int seed)
{
    for(int i = 0; i < len - 1; ++i) {
        dest[i] = 65 + (rand_r(&seed) % (90 - 65));
    }
    dest[len - 1] = 0;
}

static void generate_regular_bytes(char* dest, size_t len, unsigned int seed)
{
    char c;
    for(int i = 0; i < len - 1; ++i) {
        if(i % 64 == 0) {
            c = 65 + (rand_r(&seed) % (90 - 65));
        }
        dest[i] = c;
    }
    dest[len - 1] = 0;
}

static int ensure_equal(char* input_buf, char* output_buf, size_t FILE_SIZE, size_t index, size_t outdex)
{
    for(int i = 0; i < FILE_SIZE; ++i) {
        if(index > i && outdex > i) {
            if(input_buf[i] != output_buf[i]) {
                printf("input doesn't match output at index: %d\n", i);
                //input_buf[strlen(output_buf)] = 0;
                printf("%s versus\n%s\n", input_buf, output_buf);
                return 1;
            }
        }
        else {
            break;
        }
    }
    return 0;
}

static int test_BackendResponder_test_backend_upload()
{
    marla_Server server;
    marla_Server_init(&server);

    marla_Server_addHook(&server, marla_ServerHook_ROUTE, backendHook, 0);

    marla_Connection* client = marla_Connection_new(&server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    marla_Connection* backend = marla_Connection_new(&server);
    marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

    client->backendPeer = backend;
    backend->backendPeer = client;
    backend->is_backend = 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "POST /user HTTP/1.1");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write first part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }

    size_t FILE_SIZE = 50123;
    char input_buf[FILE_SIZE + 1];
    char output_buf[FILE_SIZE + 1];

    generate_random_bytes(input_buf, FILE_SIZE + 1, 23423324);

    memset(output_buf, 0, sizeof output_buf);

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\nHost: localhost:%s\r\nAccept: */*\r\nContent-Length: %ld\r\n\r\n", server.serverport, FILE_SIZE);
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    while(!client->current_request || client->current_request->readStage != marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
        marla_clientRead(client);

        // Wipe any written request to the backend.
        marla_Duplex_drainInput(backend);
        marla_Duplex_plugInput(backend);
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", FILE_SIZE);
    if(marla_writeDuplex(backend, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    if(!backend->current_request) {
        fprintf(stderr, "Backend has no request\n");
        return 1;
    }

    marla_clientWrite(client);
    marla_Duplex_drainOutput(client);
    marla_Duplex_plugOutput(client);

    int iterations = 0;
    int request_index = 0;
    int index = 0;
    int outdex = 0;
    while(index < FILE_SIZE || outdex < FILE_SIZE || request_index < FILE_SIZE) {
        ++iterations;
        printf("Iteration %d.", iterations);
        if(request_index < FILE_SIZE) {
            int nread = marla_writeDuplex(client, input_buf + request_index, FILE_SIZE - request_index);
            if(nread < 0) {
                return 1;
            }
            //fprintf(stderr, "Wrote %d from client.\n", nread);
            request_index += nread;
        }
        if(index < FILE_SIZE) {
            int nread = marla_writeDuplex(backend, input_buf + index, FILE_SIZE - index);
            if(nread < 0) {
                return 1;
            }
            //fprintf(stderr, "Wrote %d for backend to read.\n", nread);
            index += nread;
        }

        marla_clientRead(client);
        marla_backendRead(backend);
        marla_clientWrite(client);
        if(client->current_request) {
            //marla_dumpRequest(client->current_request);
        }
        else {
            //fprintf(stderr, "No client request\n");
        }
        if(backend->current_request) {
            //marla_dumpRequest(backend->current_request);
        }
        else {
            //fprintf(stderr, "No backend request\n");
        }
        //marla_clientRead(client);
        //marla_backendRead(backend);
        //marla_backendWrite(backend);

        if(outdex < FILE_SIZE) {
            int nwritten = marla_readDuplex(client, output_buf + outdex, FILE_SIZE - outdex);
            //fprintf(stderr, "%d\n", nwritten);
            if(nwritten < 0) {
                return 1;
            }
            outdex += nwritten;
        }

        if(ensure_equal(input_buf, output_buf, FILE_SIZE, index, outdex)) {
            return 1;
        }
        int common_index = index;
        if(common_index > outdex) {
            common_index = outdex;
        }
        //fprintf(stderr, "%d bytes confirmed\n", common_index);
    }

    if(index != outdex) {
        fprintf(stderr, "input != outdex. %d != %d\n", index, outdex);
        return 1;
    }

    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);
    marla_Server_free(&server);
    return 0;
}

static int test_BackendResponder_test_backend_download(struct marla_Server* server)
{
    marla_Server_addHook(server, marla_ServerHook_ROUTE, backendHook, 0);

    marla_Connection* client = marla_Connection_new(server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    marla_Connection* backend = marla_Connection_new(server);
    marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

    client->backendPeer = backend;
    backend->backendPeer = client;
    backend->is_backend = 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write first part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    if(client->current_request->readStage != marla_CLIENT_REQUEST_READING_VERSION) {
        fprintf(stderr, "Request is not reading version\n");
        marla_dumpRequest(client->current_request);
        return 1;
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\nHost: localhost:%s\r\nAccept: */*\r\n\r\n", server->serverport);
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }
    marla_Request* req = client->current_request;
    if(req->readStage != marla_CLIENT_REQUEST_DONE_READING) {
        fprintf(stderr, "Request is not done reading\n");
        marla_dumpRequest(req);
        return 1;
    }

    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);
    return 0;
}

static int test_backend_with_slow_response_handler()
{
    marla_Server server;
    marla_Server_init(&server);

    marla_Server_addHook(&server, marla_ServerHook_ROUTE, backendHook, 0);

    marla_Connection* client = marla_Connection_new(&server);
    marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

    marla_Connection* backend = marla_Connection_new(&server);
    marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

    client->backendPeer = backend;
    backend->backendPeer = client;
    backend->is_backend = 1;

    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1");
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write first part of request.\n");
        return 1;
    }

    marla_clientRead(client);
    if(!client->current_request) {
        return 1;
    }

    size_t FILE_SIZE = 50123;
    char input_buf[FILE_SIZE + 1];
    char output_buf[FILE_SIZE + 1];
    generate_random_bytes(input_buf, FILE_SIZE + 1, 23423324);

    memset(output_buf, 0, sizeof output_buf);

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\nHost: localhost:%s\r\nAccept: */*\r\nContent-Length: %ld\r\n\r\n", server.serverport, FILE_SIZE);
    if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    while(!client->current_request || client->current_request->readStage != marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
        marla_clientRead(client);

        // Wipe any written request to the backend.
        marla_Duplex_drainInput(backend);
        marla_Duplex_plugInput(backend);
    }

    nwritten = snprintf(source_str, sizeof(source_str) - 1, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", FILE_SIZE);
    if(marla_writeDuplex(backend, source_str, nwritten) != nwritten) {
        fprintf(stderr, "Failed to write last part of request.\n");
        return 1;
    }

    if(!backend->current_request) {
        fprintf(stderr, "Backend has no request\n");
        return 1;
    }

    marla_clientWrite(client);
    marla_Duplex_drainOutput(client);
    marla_Duplex_plugOutput(client);

    int iterations = 0;
    int index = 0;
    int outdex = 0;
    while(index < FILE_SIZE || outdex < FILE_SIZE) {
        ++iterations;
        if(iterations > 90) {
            fprintf(stderr, "Taking too long\n");
            return 1;
        }
        if(index < FILE_SIZE) {
            int nread = marla_writeDuplex(backend, input_buf + index, FILE_SIZE - index);
            if(nread < 0) {
                return 1;
            }
            //fprintf(stderr, "Wrote %d for backend to read.\n", nread);
            index += nread;
        }

        marla_backendRead(backend);
        marla_clientWrite(client);
        if(client->current_request) {
            //marla_dumpRequest(client->current_request);
        }
        else {
            //fprintf(stderr, "No client request\n");
        }
        if(backend->current_request) {
            //marla_dumpRequest(backend->current_request);
        }
        else {
            //fprintf(stderr, "No backend request\n");
        }
        //marla_clientRead(client);
        //marla_backendRead(backend);
        //marla_backendWrite(backend);

        if(outdex < FILE_SIZE) {
            int nwritten = marla_readDuplex(client, output_buf + outdex, FILE_SIZE - outdex);
            //fprintf(stderr, "%d\n", nwritten);
            if(nwritten < 0) {
                return 1;
            }
            outdex += nwritten;
        }

        //fprintf(stderr, "index=%d, outdex=%d\n", index, outdex);
        for(int i = 0; i < FILE_SIZE; ++i) {
            if(index > i && outdex > i) {
                if(input_buf[i] != output_buf[i]) {
                    fprintf(stderr, "input doesn't match output at index: %d\n", i);
                    fprintf(stderr, "%s versus %s", input_buf, output_buf);
                    return 1;
                }
            }
            else {
                break;
            }
        }
    }

    if(index != outdex) {
        fprintf(stderr, "input != outdex. %d != %d\n", index, outdex);
        return 1;
    }

    marla_Connection_destroy(client);
    marla_Connection_destroy(backend);
    marla_Server_free(&server);
    return 0;
}

marla_WriteResult marla_readBackendResponseBody(marla_Request* req);

static int test_readBackendResponseBody()
{
    // If output throttle is less than the input throttle, DOWNSTREAM will choke.
    for(int max_input_throttle = 128; max_input_throttle <= 2048; max_input_throttle *= 2) {
    for(int max_output_throttle = 64; max_output_throttle <= 2048; max_output_throttle *= 2) {
    //for(int max_input_throttle = 1024; max_input_throttle <= 2048; max_input_throttle *= 2) {
    //for(int max_output_throttle = 1024; max_output_throttle <= 2048; max_output_throttle *= 2) {
            marla_Server server;
            marla_Server_init(&server);

            marla_Server_addHook(&server, marla_ServerHook_ROUTE, backendHook, 0);

            marla_Connection* client = marla_Connection_new(&server);
            marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

            marla_Connection* backend = marla_Connection_new(&server);
            marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

            client->backendPeer = backend;
            backend->backendPeer = client;
            backend->is_backend = 1;

            // Create the test input.
            char source_str[1024];
            memset(source_str, 0, sizeof(source_str));
            int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1");
            if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write first part of request.\n");
                return 1;
            }

            marla_clientRead(client);
            if(!client->current_request) {
                return 1;
            }

            //size_t FILE_SIZE = 50123;
            size_t FILE_SIZE = 8192;
            char input_buf[FILE_SIZE + 1];
            char output_buf[FILE_SIZE + 1];
            generate_regular_bytes(input_buf, FILE_SIZE + 1, 23423324);

            memset(output_buf, 0, sizeof output_buf);

            nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\nHost: localhost:%s\r\nAccept: */*\r\nContent-Length: %ld\r\n\r\n", server.serverport, FILE_SIZE);
            if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write last part of request.\n");
                return 1;
            }

            while(!client->current_request || client->current_request->readStage != marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
                marla_clientRead(client);

                // Wipe any written request to the backend.
                marla_Duplex_drainInput(backend);
                marla_Duplex_plugInput(backend);
            }

            nwritten = snprintf(source_str, sizeof(source_str) - 1, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", FILE_SIZE);
            if(marla_writeDuplex(backend, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write last part of request.\n");
                return 1;
            }

            if(!backend->current_request) {
                fprintf(stderr, "Backend has no request\n");
                return 1;
            }

            marla_clientWrite(client);
            marla_Duplex_drainOutput(client);
            marla_Duplex_plugOutput(client);
            printf("output throttle=%d input throttle=%d\n", max_output_throttle, max_input_throttle);
            int iterations = 0;
            int index = 0;
            int outdex = 0;
            while(index < FILE_SIZE || outdex < FILE_SIZE) {
                ++iterations;
                /*if(iterations > 360) {
                    printf("Taking too long\n");
                    return 1;
                }*/
                printf("Iteration %d: index=%d, outdex=%d\n", iterations, index, outdex);
                if(index < FILE_SIZE) {
                    int max_write = FILE_SIZE - index;
                    if(max_write > max_input_throttle) {
                        max_write = max_input_throttle;
                    }
                    int nread = marla_writeDuplex(backend, input_buf + index, max_write);
                    if(nread < 0) {
                        return 1;
                    }
                    //fprintf(stderr, "Wrote %d for backend to read.\n", nread);
                    index += nread;
                }

                marla_Ring_dump(client->output, "client->output 1");
                marla_backendRead(backend);
                marla_Ring_dump(client->output, "client->output 2");
                marla_clientWrite(client);
                marla_Ring_dump(client->output, "client->output 3");
                if(client->current_request) {
                    //marla_dumpRequest(client->current_request);
                }
                else {
                    //fprintf(stderr, "No client request\n");
                }
                if(backend->current_request) {
                    //marla_dumpRequest(backend->current_request);
                }
                else {
                    //fprintf(stderr, "No backend request\n");
                }
                //marla_clientRead(client);
                //marla_backendRead(backend);
                //marla_backendWrite(backend);

                char buf[marla_BUFSIZE + 1];
                int true_read = marla_readDuplex(client, buf, sizeof buf - 1);
                buf[true_read] = 0;
                printf("Duplex output(%d): ", true_read);
                printf("%s", buf);
                printf("\n");
                marla_putbackDuplexRead(client, true_read);

                if(outdex < FILE_SIZE) {
                    int max_read = FILE_SIZE - outdex;
                    if(max_read > max_output_throttle) {
                        max_read = max_output_throttle;
                    }
                    int nwritten = marla_readDuplex(client, output_buf + outdex, max_read);
                    printf("%d output (size=%zu: %s\n", nwritten, marla_Ring_size(((marla_DuplexSource*)client->source)->output), output_buf + outdex);
                    if(nwritten < 0) {
                        return 1;
                    }
                    outdex += nwritten;
                }

                if(client->current_request && client->current_request->error[0] != 0) {
                    return 1;
                }
                if(backend->current_request && backend->current_request->error[0] != 0) {
                    return 1;
                }

                //printf("index=%d, outdex=%d\n", index, outdex);
                if(ensure_equal(input_buf, output_buf, FILE_SIZE, index, outdex)) {
                    return 1;
                }
            }

            marla_Connection_destroy(client);
            marla_Connection_destroy(backend);
            marla_Server_free(&server);
        }
    }
    return 0;
}

static int test_readBackendResponseBody2()
{
    // If output throttle is less than the input throttle, DOWNSTREAM will choke.
    for(int max_input_throttle = 512; max_input_throttle <= 1024; max_input_throttle += 16) {
    for(int max_output_throttle = 512; max_output_throttle <= 1024; max_output_throttle += 16) {
            marla_Server server;
            marla_Server_init(&server);

            marla_Server_addHook(&server, marla_ServerHook_ROUTE, backendHook, 0);

            marla_Connection* client = marla_Connection_new(&server);
            marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

            marla_Connection* backend = marla_Connection_new(&server);
            marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

            client->backendPeer = backend;
            backend->backendPeer = client;
            backend->is_backend = 1;

            // Create the test input.
            char source_str[1024];
            memset(source_str, 0, sizeof(source_str));
            int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1");
            if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write first part of request.\n");
                return 1;
            }

            marla_clientRead(client);
            if(!client->current_request) {
                return 1;
            }

            //size_t FILE_SIZE = 50123;
            size_t FILE_SIZE = 8192;
            char input_buf[FILE_SIZE + 1];
            char output_buf[FILE_SIZE + 1];
            generate_regular_bytes(input_buf, FILE_SIZE + 1, 23423324);

            memset(output_buf, 0, sizeof output_buf);

            nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\nHost: localhost:%s\r\nAccept: */*\r\nContent-Length: %ld\r\n\r\n", server.serverport, FILE_SIZE);
            if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write last part of request.\n");
                return 1;
            }

            while(!client->current_request || client->current_request->readStage != marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
                marla_clientRead(client);

                // Wipe any written request to the backend.
                marla_Duplex_drainInput(backend);
                marla_Duplex_plugInput(backend);
            }

            nwritten = snprintf(source_str, sizeof(source_str) - 1, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", FILE_SIZE);
            if(marla_writeDuplex(backend, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write last part of request.\n");
                return 1;
            }

            if(!backend->current_request) {
                fprintf(stderr, "Backend has no request\n");
                return 1;
            }

            marla_clientWrite(client);
            marla_Duplex_drainOutput(client);
            marla_Duplex_plugOutput(client);
            printf("output throttle=%d input throttle=%d\n", max_output_throttle, max_input_throttle);
            int iterations = 0;
            int index = 0;
            int outdex = 0;
            while(index < FILE_SIZE || outdex < FILE_SIZE) {
                ++iterations;
                /*if(iterations > 360) {
                    printf("Taking too long\n");
                    return 1;
                }*/
                printf("Iteration %d: index=%d, outdex=%d\n", iterations, index, outdex);
                if(index < FILE_SIZE) {
                    int max_write = FILE_SIZE - index;
                    if(max_write > max_input_throttle) {
                        max_write = max_input_throttle;
                    }
                    int nread = marla_writeDuplex(backend, input_buf + index, max_write);
                    if(nread < 0) {
                        return 1;
                    }
                    //fprintf(stderr, "Wrote %d for backend to read.\n", nread);
                    index += nread;
                }

                marla_Ring_dump(client->output, "client->output 1");
                marla_backendRead(backend);
                marla_Ring_dump(client->output, "client->output 2");
                marla_clientWrite(client);
                marla_Ring_dump(client->output, "client->output 3");
                if(client->current_request) {
                    //marla_dumpRequest(client->current_request);
                }
                else {
                    //fprintf(stderr, "No client request\n");
                }
                if(backend->current_request) {
                    //marla_dumpRequest(backend->current_request);
                }
                else {
                    //fprintf(stderr, "No backend request\n");
                }
                //marla_clientRead(client);
                //marla_backendRead(backend);
                //marla_backendWrite(backend);

                char buf[marla_BUFSIZE + 1];
                int true_read = marla_readDuplex(client, buf, sizeof buf - 1);
                buf[true_read] = 0;
                printf("Duplex output(%d): ", true_read);
                printf("%s", buf);
                printf("\n");
                marla_putbackDuplexRead(client, true_read);

                if(outdex < FILE_SIZE) {
                    int max_read = FILE_SIZE - outdex;
                    if(max_read > max_output_throttle) {
                        max_read = max_output_throttle;
                    }
                    int nwritten = marla_readDuplex(client, output_buf + outdex, max_read);
                    printf("%d output (size=%zu: %s\n", nwritten, marla_Ring_size(((marla_DuplexSource*)client->source)->output), output_buf + outdex);
                    if(nwritten < 0) {
                        return 1;
                    }
                    outdex += nwritten;
                }

                if(client->current_request && client->current_request->error[0] != 0) {
                    return 1;
                }
                if(backend->current_request && backend->current_request->error[0] != 0) {
                    return 1;
                }

                //printf("index=%d, outdex=%d\n", index, outdex);
                if(ensure_equal(input_buf, output_buf, FILE_SIZE, index, outdex)) {
                    return 1;
                }
            }

            marla_Connection_destroy(client);
            marla_Connection_destroy(backend);
            marla_Server_free(&server);
        }
    }
    return 0;
}

static int test_readBackendResponseBody3()
{
    // If output throttle is less than the input throttle, DOWNSTREAM will choke.
    for(int max_input_throttle = 512; max_input_throttle <= 522; ++max_input_throttle) {
    for(int max_output_throttle = 512; max_output_throttle <= 522; ++max_output_throttle) {
            marla_Server server;
            marla_Server_init(&server);

            marla_Server_addHook(&server, marla_ServerHook_ROUTE, backendHook, 0);

            marla_Connection* client = marla_Connection_new(&server);
            marla_Duplex_init(client, marla_BUFSIZE, marla_BUFSIZE);

            marla_Connection* backend = marla_Connection_new(&server);
            marla_Duplex_init(backend, marla_BUFSIZE, marla_BUFSIZE);

            client->backendPeer = backend;
            backend->backendPeer = client;
            backend->is_backend = 1;

            // Create the test input.
            char source_str[1024];
            memset(source_str, 0, sizeof(source_str));
            int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /user HTTP/1.1");
            if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write first part of request.\n");
                return 1;
            }

            marla_clientRead(client);
            if(!client->current_request) {
                return 1;
            }

            //size_t FILE_SIZE = 50123;
            size_t FILE_SIZE = 8192;
            char input_buf[FILE_SIZE + 1];
            char output_buf[FILE_SIZE + 1];
            generate_regular_bytes(input_buf, FILE_SIZE + 1, 23423324);

            memset(output_buf, 0, sizeof output_buf);

            nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\nHost: localhost:%s\r\nAccept: */*\r\nContent-Length: %ld\r\n\r\n", server.serverport, FILE_SIZE);
            if(marla_writeDuplex(client, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write last part of request.\n");
                return 1;
            }

            while(!client->current_request || client->current_request->readStage != marla_CLIENT_REQUEST_READING_REQUEST_BODY) {
                marla_clientRead(client);

                // Wipe any written request to the backend.
                marla_Duplex_drainInput(backend);
                marla_Duplex_plugInput(backend);
            }

            nwritten = snprintf(source_str, sizeof(source_str) - 1, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", FILE_SIZE);
            if(marla_writeDuplex(backend, source_str, nwritten) != nwritten) {
                fprintf(stderr, "Failed to write last part of request.\n");
                return 1;
            }

            if(!backend->current_request) {
                fprintf(stderr, "Backend has no request\n");
                return 1;
            }

            marla_clientWrite(client);
            marla_Duplex_drainOutput(client);
            marla_Duplex_plugOutput(client);
            printf("output throttle=%d input throttle=%d\n", max_output_throttle, max_input_throttle);
            int iterations = 0;
            int index = 0;
            int outdex = 0;
            while(index < FILE_SIZE || outdex < FILE_SIZE) {
                ++iterations;
                /*if(iterations > 360) {
                    printf("Taking too long\n");
                    return 1;
                }*/
                printf("Iteration %d: index=%d, outdex=%d\n", iterations, index, outdex);
                if(index < FILE_SIZE) {
                    int max_write = FILE_SIZE - index;
                    if(max_write > max_input_throttle) {
                        max_write = max_input_throttle;
                    }
                    int nread = marla_writeDuplex(backend, input_buf + index, max_write);
                    if(nread < 0) {
                        return 1;
                    }
                    //fprintf(stderr, "Wrote %d for backend to read.\n", nread);
                    index += nread;
                }

                marla_Ring_dump(client->output, "client->output 1");
                marla_backendRead(backend);
                marla_Ring_dump(client->output, "client->output 2");
                marla_clientWrite(client);
                marla_Ring_dump(client->output, "client->output 3");
                if(client->current_request) {
                    //marla_dumpRequest(client->current_request);
                }
                else {
                    //fprintf(stderr, "No client request\n");
                }
                if(backend->current_request) {
                    //marla_dumpRequest(backend->current_request);
                }
                else {
                    //fprintf(stderr, "No backend request\n");
                }
                //marla_clientRead(client);
                //marla_backendRead(backend);
                //marla_backendWrite(backend);

                char buf[marla_BUFSIZE + 1];
                int true_read = marla_readDuplex(client, buf, sizeof buf - 1);
                buf[true_read] = 0;
                printf("Duplex output(%d): ", true_read);
                printf("%s", buf);
                printf("\n");
                marla_putbackDuplexRead(client, true_read);

                if(outdex < FILE_SIZE) {
                    int max_read = FILE_SIZE - outdex;
                    if(max_read > max_output_throttle) {
                        max_read = max_output_throttle;
                    }
                    int nwritten = marla_readDuplex(client, output_buf + outdex, max_read);
                    printf("%d output (size=%zu: %s\n", nwritten, marla_Ring_size(((marla_DuplexSource*)client->source)->output), output_buf + outdex);
                    if(nwritten < 0) {
                        return 1;
                    }
                    outdex += nwritten;
                }

                if(client->current_request && client->current_request->error[0] != 0) {
                    return 1;
                }
                if(backend->current_request && backend->current_request->error[0] != 0) {
                    return 1;
                }

                //printf("index=%d, outdex=%d\n", index, outdex);
                if(ensure_equal(input_buf, output_buf, FILE_SIZE, index, outdex)) {
                    return 1;
                }
            }

            marla_Connection_destroy(client);
            marla_Connection_destroy(backend);
            marla_Server_free(&server);
        }
    }
    return 0;
}

int main(int argc, char* argv[])
{
    if(argc < 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return -1;
    }

    printf("Testing backend ...\n");

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

    printf("test_BackendResponder:");
    if(0 == test_BackendResponder(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_BackendResponder2:");
    if(0 == test_BackendResponder2(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_BackendResponder3:");
    if(0 == test_BackendResponder3(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_BackendResponder4:");
    if(0 == test_BackendResponder4(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_BackendResponder5:");
    if(0 == test_BackendResponder5(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_BackendResponder_test_backend_download:");
    if(0 == test_BackendResponder_test_backend_download(&server)) {
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
    marla_Server_free(&server);

    printf("test_BackendResponder_test_backend_upload:");
    if(0 == test_BackendResponder_test_backend_upload()) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_backend_with_slow_response_handler:");
    if(0 == test_backend_with_slow_response_handler()) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_readBackendResponseBody: ");
    if(0 == test_readBackendResponseBody()) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_readBackendResponseBody2: ");
    if(0 == test_readBackendResponseBody2()) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_readBackendResponseBody3: ");
    if(0 == test_readBackendResponseBody3()) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    fprintf(stderr, "%d failures\n", failed);
    return failed;
}
