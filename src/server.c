#include "rainback.h"

void parsegraph_Server_init(struct parsegraph_Server* server)
{
    server->server_status = parsegraph_SERVER_STOPPED;
    pthread_mutex_init(&server->server_mutex, 0);
    server->efd = 0;
    server->sfd = 0;
    server->backendfd = 0;
    server->first_module = 0;
    server->last_module = 0;

    for(int i = 0; i < parsegraph_SERVER_HOOK_MAX; ++i) {
        struct parsegraph_HookList* hookList = server->hooks + i;
        hookList->firstHook = 0;
        hookList->lastHook = 0;
    }
}
