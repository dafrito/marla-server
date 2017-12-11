#include "rainback.h"

void parsegraph_Server_addHook(struct parsegraph_Server* server, enum parsegraph_ServerHook serverHook, enum parsegraph_ServerHookStatus(*hookFunc)(struct parsegraph_ClientRequest* req, void*), void* hookData)
{
    struct parsegraph_HookEntry* newHook = malloc(sizeof *newHook);
    newHook->hookFunc = hookFunc;
    newHook->hookData = hookData;
    newHook->nextHook = 0;
    struct parsegraph_HookList* hookList = server->hooks + serverHook;
    if(hookList->lastHook) {
        hookList->lastHook->nextHook = newHook;
        newHook->prevHook = hookList->lastHook;
        hookList->lastHook = newHook;
    }
    else {
        hookList->firstHook = newHook;
        hookList->lastHook = newHook;
        newHook->prevHook = 0;
    }
}

int parsegraph_Server_removeHook(struct parsegraph_Server* server, enum parsegraph_ServerHook serverHook, enum parsegraph_ServerHookStatus(*hookFunc)(struct parsegraph_ClientRequest* req, void*), void* hookData)
{
    struct parsegraph_HookList* hookList = server->hooks + serverHook;
    struct parsegraph_HookEntry* hook = hookList->firstHook;
    while(hook) {
        if(hook->hookFunc != hookFunc || hook->hookData != hookData) {
            hook = hook->nextHook;
            continue;
        }
        if(hook->nextHook && hook->prevHook) {
            hook->prevHook->nextHook = hook->nextHook;
            hook->nextHook->prevHook = hook->prevHook;
        }
        else if(hook->nextHook) {
            hook->nextHook->prevHook = 0;
            hookList->firstHook = hook->nextHook;
        }
        else if(hook->prevHook) {
            hook->prevHook->nextHook = 0;
            hookList->lastHook = hook->prevHook;
        }
        else {
            hookList->firstHook = 0;
            hookList->lastHook = 0;
        }
        free(hook);
        return 0;
    }
    return -1;
}

void parsegraph_Server_invokeHook(struct parsegraph_Server* server, enum parsegraph_ServerHook serverHook, struct parsegraph_ClientRequest* req)
{
    struct parsegraph_HookList* hookList = server->hooks + serverHook;
    struct parsegraph_HookEntry* hook = hookList->firstHook;
    while(hook) {
        enum parsegraph_ServerHookStatus rv = hook->hookFunc(req, hook->hookData);
        if(rv == parsegraph_SERVER_HOOK_STATUS_OK) {
            hook = hook->nextHook;
            continue;
        }
        if(rv == parsegraph_SERVER_HOOK_STATUS_CLOSE) {
            // Close the connection.
            req->cxn->stage = parsegraph_CLIENT_COMPLETE;
            return;
        }
        if(rv == parsegraph_SERVER_HOOK_STATUS_COMPLETE) {
            // Stop handling the hook.
            return;
        }
    }
}
