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

static int expectReadStage(marla_ClientRequest* req, enum marla_RequestReadStage readStage)
{
    if(req->readStage != readStage) {
        printf("Wanting %s, got %s\n", marla_nameRequestReadStage(readStage), marla_nameRequestReadStage(req->readStage));
        return 1;
    }
    return 0;
}

static int expectWriteStage(marla_ClientRequest* req, enum marla_RequestWriteStage writeStage)
{
    if(req->writeStage != writeStage) {
        printf("Wanting %s, got %s\n", marla_nameRequestWriteStage(writeStage), marla_nameRequestWriteStage(req->writeStage));
        return 1;
    }
    return 0;
}

static int readSource(struct marla_Connection* cxn, void* sink, size_t len)
{
    return marla_Ring_read(cxn->source, sink, len);
}

static int writeSource(struct marla_Connection* cxn, void* source, size_t len)
{
    //write(1, source, len);
    return len;
}

static void destroySource(struct marla_Connection* cxn)
{
    marla_Ring_free(cxn->source);
}

static int readDuplexSource(struct marla_Connection* cxn, void* sink, size_t len)
{
    marla_Ring* ring = ((marla_Ring**)cxn->source)[0];
    return marla_Ring_read(ring, sink, len);
}

static int writeDuplexSource(struct marla_Connection* cxn, void* source, size_t len)
{
    marla_Ring* outputRing = ((marla_Ring**)cxn->source)[1];
    return marla_Ring_write(outputRing, source, len);
}

static void destroyDuplexSource(struct marla_Connection* cxn)
{
    marla_Ring** rings = ((marla_Ring**)cxn->source);
    marla_Ring_free(rings[0]);
    marla_Ring_free(rings[1]);
}

static int test_simple(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);

    return 0;
}

static int test_simple_response(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    marla_Ring* outputRing = marla_Ring_new(marla_BUFSIZE);
    marla_Ring* rings[2];
    rings[0] = ring;
    rings[1] = outputRing;

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = &rings;
    cxn->readSource = readDuplexSource;
    cxn->writeSource = writeDuplexSource;
    cxn->destroySource = destroyDuplexSource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 0) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_ClientRequest* req = cxn->latest_request;
    if(req) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);

    return 0;
}

static int test_chunks(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\nHost: localhost:%s\r\nTransfer-Encoding: chunked\r\n\r\n8\r\nNo time!\r\n0\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    marla_Ring* outputRing = marla_Ring_new(marla_BUFSIZE);
    marla_Ring* rings[2];
    rings[0] = ring;
    rings[1] = outputRing;

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = &rings;
    cxn->readSource = readDuplexSource;
    cxn->writeSource = writeDuplexSource;
    cxn->destroySource = destroyDuplexSource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 0) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_ClientRequest* req = cxn->latest_request;
    if(req) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);

    return 0;
}

static int test_simple_lf(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\nHost: localhost:%s\n\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 0) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_ClientRequest* req = cxn->latest_request;
    if(req) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);

    return 0;
}

static int test_simple_mixed_lf(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\nHost: localhost:%s\r\n\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 0) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_ClientRequest* req = cxn->latest_request;
    if(req) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);
    return 0;
}

static int test_simple_leading_crlf(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\nGET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 0) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_ClientRequest* req = cxn->latest_request;
    if(req) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);
    return 0;
}

static int test_simple_leading_lf(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "\nGET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 0) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_ClientRequest* req = cxn->latest_request;
    if(req) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);
    return 0;
}

static int test_bad_method(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);

    if(cxn->stage != marla_CLIENT_COMPLETE) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);
    return 0;
}

static int test_no_host(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET / HTTP/1.1\r\n\r\n");
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);

    if(cxn->stage != marla_CLIENT_COMPLETE) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);
    return 0;
}

static int test_no_version(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET /\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);

    if(cxn->stage != marla_CLIENT_COMPLETE) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);
    return 0;
}

static int test_no_target(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "GET HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server->serverport);
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);

    if(cxn->stage != marla_CLIENT_COMPLETE) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);

    return 0;
}

static int test_nothing(struct marla_Server* server)
{
    // Create the test input.
    char source_str[1024];
    memset(source_str, 0, sizeof(source_str));
    int nwritten = snprintf(source_str, sizeof(source_str) - 1, "\r\n\r\n");
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_write(ring, source_str, nwritten);

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    marla_ClientRequest* req = cxn->current_request;
    if(!req || req->readStage != marla_CLIENT_REQUEST_READ_FRESH || req->writeStage != marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);

    return 0;
}

static int test_stages_reading_method(struct marla_Server* server)
{
    // Create the test input.
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_writec(ring, 'G');

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);

    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_ClientRequest* req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_METHOD) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    marla_Ring_writec(ring, 'E');
    marla_Ring_writec(ring, 'T');

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_METHOD) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    marla_Ring_writec(ring, ' ');

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_PAST_METHOD) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_PAST_METHOD) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);

    return 0;
}

static int test_stages_reading_target(struct marla_Server* server)
{
    // Create the test input.
    marla_Ring* ring = marla_Ring_new(marla_BUFSIZE);
    marla_Ring_writeStr(ring, "GET /");

    // Create the connection.
    marla_Connection* cxn = marla_Connection_new(server);
    cxn->source = ring;
    cxn->readSource = readSource;
    cxn->writeSource = writeSource;
    cxn->destroySource = destroySource;

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_ClientRequest* req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_REQUEST_TARGET) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_REQUEST_TARGET) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    marla_Ring_writec(ring, ' ');

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_PAST_REQUEST_TARGET) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_PAST_REQUEST_TARGET) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    marla_Ring_writeStr(ring, "HTTP/1.");

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_VERSION) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_VERSION) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    marla_Ring_writeStr(ring, "1");

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_VERSION) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_VERSION) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    marla_Ring_writec(ring, '\n');

    // Read from the source.
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_FIELD) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    marla_clientRead(cxn);
    marla_clientWrite(cxn);
    if(cxn->requests_in_process != 1) {
        marla_Connection_destroy(cxn);
        return 1;
    }
    req = cxn->current_request;
    if(0 != expectReadStage(req, marla_CLIENT_REQUEST_READING_FIELD) ||
        0 != expectWriteStage(req, marla_CLIENT_REQUEST_WRITE_AWAITING_ACCEPT)
    ) {
        marla_Connection_destroy(cxn);
        return 1;
    }

    // Destroy the connection and test input.
    marla_Connection_destroy(cxn);
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

    printf("test_simple:");
    if(0 == test_simple(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_simple_lf:");
    if(0 == test_simple_lf(&server) && 0 == test_simple_lf(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_simple_mixed_lf:");
    if(0 == test_simple_mixed_lf(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_simple_leading_lf:");
    if(0 == test_simple_leading_lf(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_simple_leading_crlf:");
    if(0 == test_simple_leading_crlf(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_bad_method:");
    if(0 == test_bad_method(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_no_host:");
    if(0 == test_no_host(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_no_version:");
    if(0 == test_no_version(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_no_target:");
    if(0 == test_no_target(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_nothing:");
    if(0 == test_nothing(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }
    printf("test_stages_reading_method:");
    if(0 == test_stages_reading_method(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED.\n");
        ++failed;
    }
    printf("test_stages_reading_target:");
    if(0 == test_stages_reading_target(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED.\n");
        ++failed;
    }

    printf("test_simple_response:");
    if(0 == test_simple_response(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("test_chunks:");
    if(0 == test_chunks(&server)) {
        printf("PASSED\n");
    }
    else {
        printf("FAILED\n");
        ++failed;
    }

    printf("Tests finished.\n");
    return failed;
}
