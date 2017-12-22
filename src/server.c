#include "rainback.h"

const char* parsegraph_nameServerStatus(enum parsegraph_ServerStatus ss)
{
    switch(ss) {
    case parsegraph_SERVER_STOPPED:
        return "SERVER_STOPPED";
    case parsegraph_SERVER_STARTED:
        return "SERVER_STARTED";
    case parsegraph_SERVER_WAITING_FOR_INPUT:
        return "SERVER_WAITING_FOR_INPUT";
    case parsegraph_SERVER_WAITING_FOR_LOCK:
        return "SERVER_WAITING_FOR_LOCK";
    case parsegraph_SERVER_PROCESSING:
        return "SERVER_PROCESSING";
    case parsegraph_SERVER_DESTROYING:
        return "SERVER_DESTROYING";
    }
    return "";
}

void parsegraph_Server_init(struct parsegraph_Server* server)
{
    server->server_status = parsegraph_SERVER_STOPPED;
    pthread_mutex_init(&server->server_mutex, 0);
    server->efd = 0;
    server->sfd = 0;
    server->backendfd = 0;
    server->first_module = 0;
    server->last_module = 0;
    memset(server->serverport, 0, sizeof server->serverport);
    memset(server->backendport, 0, sizeof server->backendport);

    server->first_connection = 0;
    server->last_connection = 0;

    for(int i = 0; i < parsegraph_SERVER_HOOK_MAX; ++i) {
        struct parsegraph_HookList* hookList = server->hooks + i;
        hookList->firstHook = 0;
        hookList->lastHook = 0;
    }
}
