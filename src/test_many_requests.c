#include "marla.h"
#include <string.h>

static void handler(marla_Request* req, marla_ClientEvent ev, void* in, int len)
{
    //fprintf(stderr, "%s\n", marla_nameClientEvent(ev));
    marla_WriteEvent* we;
    switch(ev) {
    case marla_EVENT_ACCEPTING_REQUEST:
        //fprintf(stderr, "ACCEPTING\n");
        *(int*)in = 1;
        break;
    case marla_EVENT_REQUEST_BODY:
        we = in;
        if(we->length == 0) {
            req->readStage = marla_CLIENT_REQUEST_DONE_READING;
        }
        break;
    case marla_EVENT_MUST_WRITE:
        req->writeStage = marla_CLIENT_REQUEST_AFTER_RESPONSE;
        break;
    default:
        return;
    }
}

static void router(marla_Request* req, void* hd)
{
    req->handler = handler;
}

static int test_many_requests(char* serverport)
{
    marla_Server server;
    marla_Server_init(&server);
    marla_Server_addHook(&server, marla_ServerHook_ROUTE, router, 0);
    strcpy(server.serverport, serverport);

    for(int i = 0; i < 10000; ++i) {
        fprintf(stderr, "Iteration %d\n", i);
        marla_Connection* cxn = marla_Connection_new(&server);
        marla_Duplex_init(cxn, marla_BUFSIZE, marla_BUFSIZE);

        char buf[1024];
        int len = snprintf(buf, sizeof buf, "GET / HTTP/1.1\r\nHost: localhost:%s\r\n\r\n", server.serverport);
        marla_writeDuplex(cxn, buf, len);

        marla_clientRead(cxn);

        if(cxn->requests_in_process > 0) {
            marla_Request* req = cxn->current_request;
            marla_dumpRequest(req);
            return 1;
        }

        marla_Connection_destroy(cxn);
    }

    marla_Server_free(&server);
    return 0;
}

int main(int argc, char** argv)
{
    printf("test_many_requests.\n");
    apr_initialize();
    if(argc < 2) {
        fprintf(stderr, "Too few arguments given; provide serverport.");
        return 1;
    }
    test_many_requests(argv[1]);
    apr_terminate();
    return 0;
}
