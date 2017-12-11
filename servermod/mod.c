#include "rainback.h"

struct HandleData {
void(*default_handler)(struct parsegraph_ClientRequest*, enum parsegraph_ClientEvent, void*, int);
void* default_handleData;
};

static void request_handler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    unsigned char resp[parsegraph_BUFSIZE];
    unsigned char buf[parsegraph_BUFSIZE + 1];
    int nread;
    memset(buf, 0, sizeof buf);
    struct HandleData* hd = (struct HandleData*)req->handleData;
    switch(ev) {
    case parsegraph_EVENT_RESPOND:
        memset(resp, 0, sizeof(resp));

        const unsigned char* header = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        int nwritten = parsegraph_Connection_write(req->cxn, header, strlen(header));
        if(nwritten <= 0) {
            return;
        }
        int cs = 0;
        int message_len;
        if(!strncmp(req->uri, "/user", strlen("/user"))) {
            // User accounts are sent to the backend proxy.
            parsegraph_Backend_enqueue();
        }
        else if(!strcmp(req->uri, "/about")) {
            strcpy(buf, "No time.");
            message_len = strlen(buf);
        }
        else {
            message_len = snprintf(buf, sizeof buf, "<!DOCTYPE html><html><head><script>function run() { WS=new WebSocket(\"wss://localhost:%s/\"); WS.onopen = function() { alert('Hello'); }; setInterval(function() { WS.send('Hello'); console.log('written'); }, 500); }</script></head><body onload='run()'>Hello, <b>world.</b><p>This is request %d<p><form method=post><input type=text name=address></input><br/><input type=submit value='Submit'></input></form></body></html>", SERVERPORT ? SERVERPORT : "443", req->id);
        }
        for(int i = 0; i <= message_len; ++i) {
            if((i == message_len) || (i && !(i & (sizeof(buf) - 2)))) {
                if(i & (sizeof(buf) - 2)) {
                    buf[i & (sizeof(buf) - 2)] = 0;
                }
                int rv = snprintf(resp, 1023, "%x\r\n", cs);
                if(rv < 0) {
                    dprintf(3, "Failed to generate response.");
                    req->cxn->stage = parsegraph_CLIENT_COMPLETE;
                    return;
                }
                memcpy(resp + rv, buf, cs);
                resp[rv + cs] = '\r';
                resp[rv + cs + 1] = '\n';

                int nwritten = parsegraph_Connection_write(req->cxn, resp, rv + cs + 2);
                if(nwritten <= 0) {
                    return;
                }
                cs = 0;
            }
            if(i == message_len) {
                break;
            }
            buf[i & (sizeof(buf) - 2)] = buf[i];
            ++cs;
        }

        nwritten = parsegraph_Connection_write(req->cxn, "0\r\n\r\n", 5);
        if(nwritten <= 0) {
            return;
        }

        // Mark connection as complete.
        req->stage = parsegraph_CLIENT_REQUEST_DONE;
        break;
    default:
        if(hd) {
            void* myData = req->handleData;
            req->handleData = hd->default_handleData;
            hd->default_handler(req, ev, data, datalen);
            req->handleData = myData;
        }
        break;
    }
}

static enum parsegraph_ServerHookStatus routeHook(struct parsegraph_ClientRequest* req, void* hookData)
{
    if(!strcmp(req->uri, "/contact")) {
        struct HandleData* hd = malloc(sizeof *hd);
        hd->default_handler = req->handle;
        hd->default_handleData = req->handleData;
        req->handleData = hd;
        req->handle = request_handler;
    }
    return parsegraph_SERVER_HOOK_STATUS_OK;
}

void module_servermod_init(struct parsegraph_Server* server, enum parsegraph_ServerModuleEvent e)
{
    parsegraph_Server_addHook(server, parsegraph_SERVER_HOOK_ROUTE, routeHook, 0);
    printf("Module servermod loaded.\n");
}
