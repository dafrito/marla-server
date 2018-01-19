#ifndef parsegraph_environment_ws_prepare_INCLUDED
#define parsegraph_environment_ws_prepare_INCLUDED

#include <string.h>
#include <parsegraph_user.h>
#include <parsegraph_environment.h>
#include <parsegraph_List.h>
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>

#include "marla.h"

#define MAX_MESSAGE_QUEUE 512
#define MAX_INIT_LENGTH 4096

extern apr_pool_t* modpool;
extern ap_dbd_t* controlDBD;
extern ap_dbd_t* worldStreamDBD;

typedef struct a_message {
    void *payload;
    size_t len;
} a_message;

struct list_item_progress {
    int listId;

};

struct printing_item {
int stage;
int error;
parsegraph_List_item** values;
size_t nvalues;
int listId;
struct printing_item* parentLevel;
struct printing_item* nextLevel;
size_t index;
};

struct parsegraph_live_session {
    char error[256];
    size_t errorBufSize;
    int closed;
    apr_pool_t* pool;
    int processHead;
    struct parsegraph_user_login login;
    parsegraph_GUID env;
    size_t envReceived;
    struct printing_item* initialData;
};
typedef struct parsegraph_live_session parsegraph_live_session;

typedef struct parsegraph_live_server {
    apr_pool_t* pool;
    a_message messages[MAX_MESSAGE_QUEUE];
    int receiveHead;
} parsegraph_live_server;

int initialize_parsegraph_live_session(parsegraph_live_session* session);
int parsegraph_printItem(marla_Request* req, parsegraph_live_session* session, struct printing_item* level);
int parsegraph_prepareEnvironment(parsegraph_live_session* session);

#endif // parsegraph_environment_ws_prepare_INCLUDED
