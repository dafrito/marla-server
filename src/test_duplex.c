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
        return;
    case marla_EVENT_REQUEST_BODY:
        return;
    case marla_EVENT_MUST_WRITE:
        break;
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

int main()
{
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

    return 0;
}
