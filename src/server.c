#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include "marla.h"
#include <dlfcn.h>

const char* marla_nameServerStatus(enum marla_ServerStatus ss)
{
    switch(ss) {
    case marla_SERVER_STOPPED:
        return "SERVER_STOPPED";
    case marla_SERVER_STARTED:
        return "SERVER_STARTED";
    case marla_SERVER_WAITING_FOR_INPUT:
        return "SERVER_WAITING_FOR_INPUT";
    case marla_SERVER_WAITING_FOR_LOCK:
        return "SERVER_WAITING_FOR_LOCK";
    case marla_SERVER_PROCESSING:
        return "SERVER_PROCESSING";
    case marla_SERVER_DESTROYING:
        return "SERVER_DESTROYING";
    }
    return "";
}

void marla_Server_init(struct marla_Server* server)
{
    server->pool = 0;
    if(APR_SUCCESS != apr_pool_create(&server->pool, 0)) {
        fprintf(stderr, "Failed to create file cache pool.\n");
        abort();
    }

    // Create the file cache.
    server->fileCache = apr_hash_make(server->pool);
    server->wdToFileEntry = apr_hash_make(server->pool);

    server->server_status = marla_SERVER_STOPPED;
    pthread_mutex_init(&server->server_mutex, 0);
    server->has_terminal = 0;
    server->idleTimeouts = 0;
    server->logfd = -1;
    server->wantsLogWrite = 0;
    server->using_ssl = 0;
    server->efd = 0;
    server->sfd = 0;
    server->fileCacheifd = 0;
    server->first_module = 0;
    server->last_module = 0;
    server->log = marla_Ring_new(marla_LOGBUFSIZE);
    server->undertaker = 0;
    server->undertakerData = 0;
    memset(server->serverport, 0, sizeof server->serverport);
    memset(server->backendport, 0, sizeof server->backendport);
    memset(server->db_path, 0, sizeof server->db_path);

    server->first_connection = 0;
    server->last_connection = 0;

    for(int i = 0; i < marla_ServerHook_MAX; ++i) {
        struct marla_HookList* hookList = server->hooks + i;
        hookList->firstHook = 0;
        hookList->lastHook = 0;
    }
}

void marla_Server_flushLog(struct marla_Server* server)
{
    if(server->wantsLogWrite || server->logfd == 0) {
        return;
    }
    void* readSlot;
    size_t slotLen;
    marla_Ring_readSlot(server->log, &readSlot, &slotLen);
    if(slotLen == 0) {
        // Nothing to write.
        return;
    }
    int true_written = write(server->logfd, readSlot, slotLen);
    if(true_written <= 0) {
        server->wantsLogWrite = 1;
        marla_Ring_putbackRead(server->log, slotLen);
        return;
    }
    if(true_written < slotLen) {
        marla_Ring_putbackRead(server->log, slotLen - true_written);
    }
}

int clearFileCache(void *rec, const void *key, apr_ssize_t klen, const void *value)
{
    marla_FileEntry* fileEntry = (marla_FileEntry*)value;
    marla_FileEntry_free(fileEntry);
    return 1;
}

void marla_Server_free(struct marla_Server* server)
{
    while(server->first_connection) {
        marla_Connection_destroy(server->first_connection);
    }

    for(enum marla_ServerHook serverHook = 0; serverHook < marla_ServerHook_MAX; ++serverHook) {
        struct marla_HookList* hookList = server->hooks + serverHook;
        struct marla_HookEntry* hook = hookList->firstHook;
        while(hook) {
            struct marla_HookEntry* nhook = hook->nextHook;
            free(hook);
            hook = nhook;
        }
    }

    struct marla_ServerModule* serverModule = server->last_module;
    while(serverModule) {
        struct marla_ServerModule* nextModule = serverModule->prevModule;
        serverModule->moduleFunc(server, marla_EVENT_SERVER_MODULE_STOP);
        dlclose(serverModule->moduleHandle);
        free(serverModule);
        serverModule = nextModule;
    }

    // Destroy existing marla_FileEntry objects.
    apr_hash_do(clearFileCache, server, server->fileCache);

    // Destroy the server's pool. Hashes are now invalid past this point.
    apr_pool_destroy(server->pool);

    marla_Ring_free(server->log);
}
