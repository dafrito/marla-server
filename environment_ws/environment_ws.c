#include <sqlite3.h>
#include <unistd.h>
#include "prepare.h"
#include "marla.h"

int openWorldStreams = 0;

apr_pool_t* modpool = 0;
ap_dbd_t* controlDBD = 0;
ap_dbd_t* worldStreamDBD = 0;

static int acquireWorldStream();
static int releaseWorldStream();

static void
callback_parsegraph_environment(struct marla_ClientRequest* req, enum marla_ClientEvent reason, void *in, int len)
{
    parsegraph_live_session* session = req->handleData;

    int neededEnvLength = 36;
    int m, rv;
    static unsigned char buf[MAX_INIT_LENGTH];
    switch(reason) {
    case marla_EVENT_WEBSOCKET_CLOSE_REASON:
        if(session->initialData != 0) {
            if(session->initialData->stage != 3 && !session->initialData->error) {
                // World streaming interrupted, decrement usage counter.
                session->initialData->error = 1;
                if(releaseWorldStream() != 0) {
                    return;
                }
            }

            free(session->initialData);
            session->initialData = 0;
        }
        return;

    case marla_EVENT_WEBSOCKET_MUST_READ:
        if(session->envReceived < neededEnvLength) {
            if(session->envReceived + len < neededEnvLength) {
                // Copy the whole thing.
                memcpy(session->env.value + session->envReceived, in, len);
                session->envReceived += len;
                // Await more data.
                return;
            }
            else {
                // Otherwise, just copy the needed portion
                memcpy(session->env.value + session->envReceived, in, neededEnvLength - session->envReceived);
                in += (neededEnvLength - session->envReceived);
                len -= (neededEnvLength - session->envReceived);
                session->envReceived = neededEnvLength;
            }
            if(len == 0) {
                // Only the environment received.
                return;
            }
        }

        // TODO handle more events and commands received

        // Extraneous data should cause an error.
        strcpy(session->error, "Received extraneous data.");
        return;

    case marla_EVENT_HEADER:
        if(!strcmp("Cookie", in)) {
            char* cookie = in + len;
            int cookie_len = strlen(cookie);
            int cookieType = 0;
            char* cookie_saveptr;
            char* tok = strtok_r(cookie, ";", &cookie_saveptr);

            while(tok) {
                // Only one cookie
                char* partSavePtr;
                char* partTok = strtok_r(tok, "=", &partSavePtr);
                if(!partTok) {
                    return;
                }
                if(!strcmp(partTok, "session")) {
                    cookieType = 1;
                }
                else {
                    cookieType = 0;
                }

                partTok = strtok_r(0, "=", &partSavePtr);
                if(!partTok) {
                    return;
                }

                if(cookieType == 1) {
                    // This cookie value is the session identifier; authenticate.
                    marla_logMessagef(req->cxn->server, "Found session cookie.");
                    char* sessionValue = partTok;
                    session->login.username = 0;
                    session->login.userId = -1;
                    if(sessionValue && 0 == parsegraph_deconstructSessionString(session->pool, sessionValue, &session->login.session_selector, &session->login.session_token)) {
                        parsegraph_UserStatus rv = parsegraph_refreshUserLogin(session->pool, controlDBD, &session->login);
                        if(rv != parsegraph_OK) {
                            marla_logMessagef(req->cxn->server, "Failed to refresh session's login.");
                            strcpy(session->error, parsegraph_nameUserStatus(rv));
                            return;
                        }

                        parsegraph_UserStatus idRV = parsegraph_getIdForUsername(session->pool, controlDBD, session->login.username, &(session->login.userId));
                        if(parsegraph_isSeriousUserError(idRV)) {
                            marla_logMessagef(req->cxn->server, "Failed to retrieve ID for authenticated login.");
                            strcpy(session->error, "Failed to retrieve ID for authenticated login.\n");
                            return;
                        }
                    }
                    if(!session->login.username) {
                        marla_logMessagef(req->cxn->server, "Session does not match any user.");
                        strcpy(session->error, "Session does not match any user.");
                        return;
                    }
                    marla_logMessagef(req->cxn->server, "Session matched user %s", session->login.username);
                    break;
                }
                else {
                    // Do nothing.
                }

                tok = strtok_r(0, ";", &cookie_saveptr);
            }
        }
        break;

    case marla_EVENT_ACCEPTING_REQUEST:
        marla_logMessagef(req->cxn->server, "Environment ws is accepting request.");
        *((int*)in) = 1;
        break;

    case marla_EVENT_WEBSOCKET_MUST_WRITE:
        if(strlen(session->error) > 0) {
            if(!session->closed) {
                session->closed = 1;
                //lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, session->error, strlen(session->error));
            }
            marla_logMessagef(req->cxn->server, "Closing connection. %s", session->error);
            return;
        }
        if(session->envReceived < neededEnvLength) {
            return;
        }
        if(!session->login.username) {
            strcpy(session->error, "Session has no username.");
            return;
        }
        if(!session->initialData) {
            if(0 != acquireWorldStream()) {
                strcpy(session->error, "Failed to begin transaction to initialize user.");
                return;
            }

            session->initialData = malloc(sizeof(struct printing_item));
            session->initialData->error = 0;
            session->initialData->stage = 0;
            session->initialData->values = 0;
            session->initialData->nvalues = 0;
            session->initialData->index = 0;
            session->initialData->nextLevel = 0;
            session->initialData->parentLevel = 0;
            if(0 != parsegraph_getEnvironmentRoot(session->pool, worldStreamDBD, &session->env, &session->initialData->listId)) {
                strcpy(session->error, "Error retrieving environment root.");
                session->initialData->error = 1;
                return;
            }
        }
        if(session->initialData->error) {
            return;
        }
        if(session->initialData->listId == 0) {
            if(0 != parsegraph_prepareEnvironment(session)) {
                return;
            }
        }
        if(session->initialData->stage != 3) {
            switch(parsegraph_printItem(req, session, session->initialData)) {
            case 0:
                // Done!
                if(releaseWorldStream() != 0) {
                    session->initialData->error = 1;
                    strcpy(session->error, "Failed to commit prepared environment.");
                    return;
                }
                break;
            case -1:
                // Choked.
                return;
            case -2:
            default:
                // Died.
                releaseWorldStream();
                session->initialData->error = 1;
                strcpy(session->error, "Failed to prepare environment.");
                return;
            }
        }

        // Get storage event log since last transmit.
            // For each event, interpret and send to user.
            // Update event counter.
            // Stop if choked.

        // Get environment event log since last transmit.
            // For each event, interpret and send to user.
            // Update event counter.
            // Stop if choked.

        // Get all chat logs for user since last transmit.
            // For each event, interpret and send to user.
            // Update event counter.
            // Stop if choked.

        /*while(1){
            strcpy(buf + LWS_PRE, "No time.");
            int written = lws_write(wsi, buf + LWS_PRE, strlen("No time."), LWS_WRITE_TEXT);
            if(written < 0) {
                fprintf(stderr, "Failed to write.\n");
                return -1;
            }
            if(written < m) {
                fprintf(stderr, "Partial write.\n");
                return -1;
            }
        }*/
        break;
    default:
        break;
    }

    return;
}

static void routeHook(struct marla_ClientRequest* req, void* hookData)
{
    if(!strcmp(req->uri, "/environment/live")) {
        struct parsegraph_live_session* hd = malloc(sizeof *hd);
        req->handleData = hd;
        req->handle = callback_parsegraph_environment;
        // Initialize the session structure.
        marla_logMessagef(req->cxn->server, "Handling /environment/live websocket connection");
        if(0 != initialize_parsegraph_live_session(hd)) {
            //strcpy(session->error, "Error initializing session.");
            //return;
        }
    }
}

static int prepareDBD(ap_dbd_t** dbdPointer)
{
    ap_dbd_t* dbd = apr_palloc(modpool, sizeof(*dbd));
    if(dbd == NULL) {
        fprintf(stderr, "Failed initializing DBD memory");
        return -1;
    }
    *dbdPointer = dbd;
    int rv = apr_dbd_get_driver(modpool, "sqlite3", &dbd->driver);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed creating DBD driver, APR status of %d.\n", rv);
        return -1;
    }
    const char* db_path = "/home/dafrito/var/parsegraph/users.sqlite";
    rv = apr_dbd_open(dbd->driver, modpool, db_path, &dbd->handle);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed connecting to database at %s, APR status of %d.\n", db_path, rv);
        return -1;
    }
    dbd->prepared = apr_hash_make(modpool);

    // Prepare the database connection.
    /*rv = parsegraph_prepareLoginStatements(modpool, dbd);
    if(rv != 0) {
        fprintf(stderr, "Failed preparing user SQL statements, status of %d.\n", rv);
        return -1;
    }

    rv = parsegraph_List_prepareStatements(modpool, dbd);
    if(rv != 0) {
        fprintf(stderr, "Failed preparing list SQL statements, status of %d: %s\n", rv, apr_dbd_error(dbd->driver, dbd->handle, rv));
        return -1;
    }

    parsegraph_EnvironmentStatus erv;
    erv = parsegraph_prepareEnvironmentStatements(modpool, dbd);
    if(erv != parsegraph_Environment_OK) {
        fprintf(stderr, "Failed preparing environment SQL statements, status of %d.\n", rv);
        return -1;
    }*/

    return 0;
}

static int initialize_module(struct marla_Server* server)
{
    struct timeval time;
    gettimeofday(&time,NULL);

    // microsecond has 1 000 000
    // Assuming you did not need quite that accuracy
    // Also do not assume the system clock has that accuracy.
    srand((time.tv_sec * 1000) + (time.tv_usec / 1000));

    // Create the process-wide pool.
    int rv = apr_pool_create(&modpool, 0);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed initializing APR pool for lwsws module: APR status of %d.\n", rv);
        return -1;
    }

    // Load APR and DBD.
    dlopen(NULL, RTLD_NOW|RTLD_GLOBAL);
    dlopen("/usr/lib64/libaprutil-1.so", RTLD_NOW|RTLD_GLOBAL);
    char rverr[255];
    apr_dso_handle_t* res_handle;
    rv = apr_dso_load(&res_handle, "/usr/lib64/apr-util-1/apr_dbd_sqlite3-1.so", modpool);
    if(rv != APR_SUCCESS) {
        apr_dso_error(res_handle, rverr, 255);
        fprintf(stderr, "Failed loading DSO: %s", rverr);
        return -1;
    }

    // Create the PID file.
    int pidFile = open("rainback.pid", O_WRONLY | O_TRUNC | O_CREAT, 0664);
    if(pidFile < 0) {
        fprintf(stderr, "Error %d while creating pid file: %s\n", errno, strerror(errno));
        return -1;
    }
    char buf[256];
    int written = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if(written < 0) {
        fprintf(stderr, "Error %d while formatting pid number: %s\n", errno, strerror(errno));
        return -1;
    }
    if(written > sizeof(buf) - 1) {
        fprintf(stderr, "Buffer too small to write PID number.\n");
        return -1;
    }
    int pidStrSize = written;
    written = write(pidFile, buf, written);
    if(written < 0) {
        fprintf(stderr, "Error %d while formatting pid number: %s\n", errno, strerror(errno));
        return -1;
    }
    if(written < pidStrSize) {
        fprintf(stderr, "Partial write of PID to file not tolerated.\n");
        return -1;
    }
    if(close(pidFile) < 0) {
        fprintf(stderr, "Error %d while closing pid file: %s\n", errno, strerror(errno));
        return -1;
    }

    // Initialize DBD.
    rv = apr_dbd_init(modpool);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed initializing DBD, APR status of %d.\n", rv);
        return -1;
    }

    if(0 != prepareDBD(&controlDBD)) {
        return -1;
    }
    if(0 != prepareDBD(&worldStreamDBD)) {
        return -1;
    }

    marla_Server_addHook(server, marla_SERVER_HOOK_ROUTE, routeHook, 0);

    marla_logMessagef(server, "Completed environment ws initialization");

    return 0;
}

static int destroy_module(struct marla_Server* server)
{
    if(remove("rainback.pid") < 0) {
        fprintf(stderr, "Error %d while removing pid file: %s\n", errno, strerror(errno));
    }

    // Close the world streaming DBD connection.
    int rv = apr_dbd_close(worldStreamDBD->driver, worldStreamDBD->handle);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed closing world streaming database connection, APR status of %d.\n", rv);
        return -1;
    }

    // Close the control DBD connection.
    rv = apr_dbd_close(controlDBD->driver, controlDBD->handle);
    if(rv != APR_SUCCESS) {
        fprintf(stderr, "Failed closing control database connection, APR status of %d.\n", rv);
        return -1;
    }

    apr_pool_destroy(modpool);
    modpool = NULL;

    apr_terminate();
    return 0;
}

static int acquireWorldStream()
{
    if(openWorldStreams++ == 0) {
        int nrows;
        int dbrv = apr_dbd_query(worldStreamDBD->driver, worldStreamDBD->handle, &nrows, "BEGIN IMMEDIATE");
        if(dbrv != 0) {
            openWorldStreams = 0;
            return -1;
        }
    }
    return 0;
}

static int releaseWorldStream()
{
    if(openWorldStreams == 0) {
        // Invalid.
        return -1;
    }
    if(--openWorldStreams > 0) {
        return 0;
    }

    int nrows;
    int dbrv = apr_dbd_query(worldStreamDBD->driver, worldStreamDBD->handle, &nrows, "ROLLBACK");
    if(dbrv != 0) {
        return -1;
    }
    return 0;
}

char data_version_storage[255];
int cb_get_data_version(void* userdata, int numCols, char** values, char** columnName)
{
    strncpy(data_version_storage, values[0], 254);
    fprintf(stderr, values[0]);
    return 0;
}

void run_conn_sql(const char* sql)
{
    sqlite3_exec(apr_dbd_native_handle(controlDBD->driver, controlDBD->handle), sql, cb_get_data_version, 0, 0);
}

AP_DECLARE(void) ap_log_perror_(const char *file, int line, int module_index,
                                int level, apr_status_t status, apr_pool_t *p,
                                const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char exp[512];
    memset(exp, 0, sizeof(exp));
    vsprintf(exp, fmt, args);
    va_end(args);
}

int
module_environment_ws_init(struct marla_Server* server, enum marla_ServerModuleEvent ev)
{
    if(ev == marla_EVENT_SERVER_MODULE_START) {
        return initialize_module(server);
    }
    if(ev == marla_EVENT_SERVER_MODULE_STOP) {
        return destroy_module(server);
    }
}

int initialize_parsegraph_live_session(parsegraph_live_session* session)
{
    session->envReceived = 0;
    session->errorBufSize = 255;
    memset(session->error, 0, session->errorBufSize + 1);
    session->closed = 0;
    session->initialData = 0;
    int rv = apr_pool_create(&session->pool, 0);
    if(0 != rv) {
        return -1;
    }
    return 0;
}

int parsegraph_prepareEnvironment(parsegraph_live_session* session)
{
    if(parsegraph_OK != parsegraph_beginTransaction(session->pool, worldStreamDBD, session->env.value)) {
        strcpy(session->error, "Failed to begin transaction for preparing environment.");
        return -1;
    }

    // Prepare the environment.
    if(parsegraph_List_OK != parsegraph_List_new(session->pool, worldStreamDBD, session->env.value, &session->initialData->listId)) {
        session->initialData->listId = 0;
        strcpy(session->error, "Failed to prepare environment.");
        session->initialData->error = 1;
        parsegraph_rollbackTransaction(session->pool, worldStreamDBD, session->env.value);
        return -1;
    }

    if(parsegraph_Environment_OK != parsegraph_setEnvironmentRoot(session->pool, worldStreamDBD, &session->env, session->initialData->listId)) {
        session->initialData->listId = 0;
        session->initialData->error = 1;
        strcpy(session->error, "Failed to prepare environment.");
        parsegraph_rollbackTransaction(session->pool, worldStreamDBD, session->env.value);
        return -1;
    }

    int metaList;
    if(parsegraph_List_OK != parsegraph_List_appendItem(session->pool, worldStreamDBD, session->initialData->listId, parsegraph_BlockType_MetaList, "", &metaList)) {
        session->initialData->error = 1;
        strcpy(session->error, "Failed to prepare environment.");
        parsegraph_rollbackTransaction(session->pool, worldStreamDBD, session->env.value);
        return -1;
    }

    int worldList;
    if(parsegraph_List_OK != parsegraph_List_appendItem(session->pool, worldStreamDBD, session->initialData->listId, parsegraph_BlockType_WorldList, "", &worldList)) {
        session->initialData->error = 1;
        strcpy(session->error, "Failed to prepare environment.");
        parsegraph_rollbackTransaction(session->pool, worldStreamDBD, session->env.value);
        return -1;
    }

    char buf[1024];
    char nbuf[1024];
    int numMultislots = 4 + rand() % 64;
    int child;
    for(int i = 0; i < numMultislots; ++i) {
        int rowSize = 1 + rand() % 32;
        int columnSize = 1 + rand() % 24;
        snprintf(buf, sizeof(buf), "[%d, %d, %d, %d, %d, %d]", rand() % 4, rowSize, columnSize, 55 + rand() % 200, 55 + rand() % 200, 55 + rand() % 200);
        if(parsegraph_List_OK != parsegraph_List_appendItem(session->pool, worldStreamDBD, worldList, parsegraph_BlockType_Multislot, buf, &child)) {
            session->initialData->error = 1;
            strcpy(session->error, "Failed to prepare environment.");
            parsegraph_rollbackTransaction(session->pool, worldStreamDBD, session->env.value);
            return -1;
        }
    }

    if(parsegraph_OK != parsegraph_commitTransaction(session->pool, worldStreamDBD, session->env.value)) {
        parsegraph_rollbackTransaction(session->pool, worldStreamDBD, session->env.value);
        session->initialData->error = 1;
        strcpy(session->error, "Failed to prepare environment.");
        return -1;
    }
    return 0;
}

int parsegraph_printItem(marla_ClientRequest* req, parsegraph_live_session* session, struct printing_item* level)
{
    //fprintf(stderr, "PRINTING item\n");
    static char buf[65536];
    if(!level || level->error) {
        return -2;
    }

    // Get to the deepest level.
    while(level->nextLevel != 0) {
        level = level->nextLevel;
    }

    while(level != 0) {
        if(level->stage == 0) {
            // Print the item.
            const char* value;
            int typeId;
            if(0 != parsegraph_List_getName(session->pool, worldStreamDBD, level->listId, &value, &typeId)) {
                //lwsl_err("Error encountered retrieving list name.\n");
                goto die;
            }
            if(value == 0) {
                value = "";
                typeId = 0;
            }
            int written = snprintf(buf, sizeof(buf), "{\"id\":%d, \"value\":\"%s\", \"type\":%d, \"items\":[", level->listId, value, typeId);
            if(written < 0) {
                //lwsl_err("Failed to write to string buffer.\n");
                goto die;
            }
            if(written >= sizeof(buf)) {
                //lwsl_err("String buffer overflowed.\n");
                goto die;
            }

            //fprintf(stderr, "printing stage 1\n");
            if(marla_writeWebSocketHeader(req, 1, written) < 0) {
                goto choked;
            }
            int nwritten = marla_Connection_write(req->cxn, buf, written);
            if(nwritten < written) {
                if(nwritten > 0) {
                    marla_Connection_putbackWrite(req->cxn, nwritten);
                }
                goto choked;
            }
            // Item printed.

            //lwsl_err("stage 1 complete\n");
            level->stage = 1;
        }

        if(level->stage == 1) {
            // Ready to print any children.
            //lwsl_err("beginning stage 2\n");
            if(!level->values) {
                int rv = parsegraph_List_listItems(session->pool, worldStreamDBD, level->listId, &level->values, &level->nvalues);
                if(rv != 0) {
                    //lwsl_err("Failed to list items for item.\n");
                    goto die;
                }
            }

            //fprintf(stderr, "printing stage 2\n");
            if(level->index < level->nvalues) {
                // Create the level for the child.
                struct printing_item* subLevel = malloc(sizeof(*level));
                if(!subLevel) {
                    //lwsl_err("Memory allocation error.");
                    goto die;
                }
                // Retrieve the id of the child from the list.
                subLevel->listId = level->values[level->index++]->id;
                subLevel->parentLevel = level;
                subLevel->nextLevel = 0;
                subLevel->stage = 0;
                subLevel->error = 0;
                subLevel->index = 0;
                subLevel->values = 0;
                subLevel->nvalues = 0;
                level->nextLevel = subLevel;

                // Iterate to the child.
                level = subLevel;
                continue;
            }

            // All children written.
            level->stage = 2;
        }

        if(level->stage == 2) {
            // Ready to print end of child list and object.
            //fprintf(stderr, "printing stage 3\n");
            int written;
            if(level->parentLevel && (level->parentLevel->index < level->parentLevel->nvalues)) {
                strcpy(buf, "]},");
                if(marla_writeWebSocketHeader(req, 1, strlen("]},")) < 0) {
                    goto choked;
                }
                written = marla_Connection_write(req->cxn, buf, strlen("]},"));
                if(written < strlen("]},")) {
                    if(written > 0) {
                        marla_Connection_putbackWrite(req->cxn, written);
                    }
                    goto choked;
                }
            }
            else {
                strcpy(buf, "]}");
                written = marla_Connection_write(req->cxn, buf, strlen("]}"));
                if(written < strlen("]}")) {
                    if(written > 0) {
                        marla_Connection_putbackWrite(req->cxn, written);
                    }
                    goto choked;
                }
            }
            //lwsl_err("reached stage 3\n");

            // Suffix printed.
            level->stage = 3;
        }

        if(level->stage != 3) {
            goto choked;
        }

        // Move to the parent and free this level.
        struct printing_item* childLevel = level;
        level = level->parentLevel;
        if(level) {
            free(childLevel);
            level->nextLevel = 0;
        }
    }

    return 0;
choked:
    if(session->initialData->stage == 3) {
        return 0;
    }
    return -1;
die:
    level->error = 1;
    struct printing_item* par = level->parentLevel;
    while(par != 0) {
        par->error = 1;
        par = par->parentLevel;
    }
    return -2;
}
