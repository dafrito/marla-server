#include "marla.h"

void marla_Server_addHook(struct marla_Server* server, enum marla_ServerHook serverHook, void(*hookFunc)(struct marla_Request* req, void*), void* hookData)
{
    struct marla_HookEntry* newHook = malloc(sizeof *newHook);
    newHook->hookFunc = hookFunc;
    newHook->hookData = hookData;
    newHook->nextHook = 0;
    struct marla_HookList* hookList = server->hooks + serverHook;
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

int marla_Server_removeHook(struct marla_Server* server, enum marla_ServerHook serverHook, void(*hookFunc)(struct marla_Request* req, void*), void* hookData)
{
    struct marla_HookList* hookList = server->hooks + serverHook;
    struct marla_HookEntry* hook = hookList->firstHook;
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

void marla_Server_invokeHook(struct marla_Server* server, enum marla_ServerHook serverHook, struct marla_Request* req)
{
    struct marla_HookList* hookList = server->hooks + serverHook;
    struct marla_HookEntry* hook = hookList->firstHook;
    while(hook) {
        hook->hookFunc(req, hook->hookData);
        hook = hook->nextHook;
    }
}
