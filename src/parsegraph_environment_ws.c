#include <sqlite3.h>
#include "prepare.h"

apr_pool_t* modpool = 0;
ap_dbd_t* controlDBD = 0;
ap_dbd_t* worldStreamDBD = 0;

int openWorldStreams = 0;

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
    lwsl_err(exp);
    va_end(args);
}

int read_data(parsegraph_live_server* server, parsegraph_live_session* session, void* in, size_t len)
{
    if (server->messages[server->receiveHead].payload) {
        free(server->messages[server->receiveHead].payload);
    }
    server->messages[server->receiveHead].payload = malloc(LWS_PRE + len);
    server->messages[server->receiveHead].len = len;
    memcpy((char*)server->messages[server->receiveHead].payload + LWS_PRE, in, len);

    // Advance the session's ring buffer position.
    if (server->receiveHead == (MAX_MESSAGE_QUEUE - 1)) {
        server->receiveHead = 0;
    }
    else {
        server->receiveHead++;
    }

    return 0;
}

int process_message(struct lws* wsi, parsegraph_live_server* server, parsegraph_live_session* session, a_message* message)
{
    // Write the next message.
    int m = server->messages[session->processHead].len;
    int n = lws_write(wsi, (unsigned char *)
           server->messages[session->processHead].payload +
           LWS_PRE, m, LWS_WRITE_TEXT);
    if (n < 0) {
        lwsl_err("ERROR %d writing to mirror socket\n", n);
        return -1;
    }
    if (n < m) {
        lwsl_err("mirror partial write %d vs %d\n", n, m);
    }

    // Advance the session's ring buffer position.
    if (session->processHead == (MAX_MESSAGE_QUEUE - 1)) {
        session->processHead = 0;
    }
    else {
        session->processHead++;
    }

    return 0;
}

static int
callback_parsegraph_environment(struct lws *wsi, enum lws_callback_reasons reason, void* sessionData, void *in, size_t len)
{
    parsegraph_live_session* session = sessionData;
    parsegraph_live_server* server = lws_protocol_vh_priv_get(
        lws_get_vhost(wsi), lws_get_protocol(wsi)
    );

    int neededEnvLength = 36;
    int m, rv;
    static unsigned char buf[LWS_PRE + MAX_INIT_LENGTH];
    switch(reason) {
    case LWS_CALLBACK_PROTOCOL_INIT: /* per vhost */
        server = lws_protocol_vh_priv_zalloc(
            lws_get_vhost(wsi), lws_get_protocol(wsi), sizeof(*server)
        );
        rv = apr_pool_create(&server->pool, modpool);
        if(0 != rv) {
            lwsl_err("Error %d creating vhost pool.\n", rv);
            lws_callback_on_writable(wsi);
            return 0;
        }
        break;

    case LWS_CALLBACK_PROTOCOL_DESTROY: /* per vhost */
        if(!server) {
            break;
        }
        for(int n = 0; n < ARRAY_SIZE(server->messages); n++) {
            if (server->messages[n].payload) {
                free(server->messages[n].payload);
                server->messages[n].payload = NULL;
            }
        }
        apr_pool_destroy(server->pool);
        server->pool = 0;
        break;

    case LWS_CALLBACK_CLOSED:
        if(session->initialData != 0) {
            if(session->initialData->stage != 3 && !session->initialData->error) {
                // World streaming interrupted, decrement usage counter.
                session->initialData->error = 1;
                if(releaseWorldStream() != 0) {
                    return -1;
                }
            }

            free(session->initialData);
            session->initialData = 0;
        }
        return 0;

    case LWS_CALLBACK_RECEIVE:
        if(session->envReceived < neededEnvLength) {
            if(session->envReceived + len < neededEnvLength) {
                // Copy the whole thing.
                memcpy(session->env.value + session->envReceived, in, len);
                session->envReceived += len;
                // Await more data.
                lws_rx_flow_control(wsi, 1);
                return 0;
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
                //lws_rx_flow_control(wsi, 0);
                lws_callback_on_writable(wsi);
                return 0;
            }
        }

        // TODO handle more events and commands received

        // Extraneous data should cause an error.
        strcpy(session->error, "Received extraneous data.");
        lws_callback_on_writable(wsi);
        lws_rx_flow_control(wsi, 0);
        return 0;

    case LWS_CALLBACK_ESTABLISHED:
        // Initialize the session structure.
        if(0 != initialize_parsegraph_live_session(wsi, server, session)) {
            strcpy(session->error, "Error initializing session.");
            lws_callback_on_writable(wsi);
            return 0;
        }

        // Retrieve the cookie.
        size_t cookieLen = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_COOKIE);
        if(cookieLen > sizeof(buf)) {
            strcpy(session->error, "Session cookie is too long.");
            lws_callback_on_writable(wsi);
            return 0;
        }
        char* cookie = buf;
        rv = lws_hdr_copy(wsi, cookie, sizeof(buf), WSI_TOKEN_HTTP_COOKIE);
        if(rv < 0) {
            snprintf(session->error, session->errorBufSize, "Failed to retrieve session. Error returned was %d\n", rv);
            lws_callback_on_writable(wsi);
            return 0;
        }

        // Authenticate.
        char* sessionValue = strstr(cookie, "session=") + strlen("session=");
        session->login.username = 0;
        session->login.userId = -1;
        if(sessionValue && 0 == parsegraph_deconstructSessionString(session->pool, sessionValue, &session->login.session_selector, &session->login.session_token)) {
            parsegraph_UserStatus rv = parsegraph_refreshUserLogin(session->pool, controlDBD, &session->login);
            if(rv != parsegraph_OK) {
                strcpy(session->error, parsegraph_nameUserStatus(rv));
                lws_callback_on_writable(wsi);
                return 0;
            }

            parsegraph_UserStatus idRV = parsegraph_getIdForUsername(session->pool, controlDBD, session->login.username, &(session->login.userId));
            if(parsegraph_isSeriousUserError(idRV)) {
                strcpy(session->error, "Failed to retrieve ID for authenticated login.\n");
                lws_callback_on_writable(wsi);
                return 0;
            }
        }
        if(!session->login.username) {
            strcpy(session->error, "Session does not match any user.");
            lws_callback_on_writable(wsi);
            return 0;
        }
        lws_rx_flow_control(wsi, 1);
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        if(strlen(session->error) > 0) {
            if(!session->closed) {
                session->closed = 1;
                lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, session->error, strlen(session->error));
            }
            lwsl_err("Closing connection. %s", session->error);
            return -1;
        }
        if(session->envReceived < neededEnvLength) {
            return 0;
        }
        if(!session->login.username) {
            strcpy(session->error, "Session has no username.");
            lws_callback_on_writable(wsi);
            return 0;
        }
        if(!session->initialData) {
            if(0 != acquireWorldStream()) {
                strcpy(session->error, "Failed to begin transaction to initialize user.");
                lws_callback_on_writable(wsi);
                return 0;
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
                lws_callback_on_writable(wsi);
                return 0;
            }
        }
        if(session->initialData->error) {
            lws_callback_on_writable(wsi);
            return 0;
        }
        if(session->initialData->listId == 0) {
            if(0 != parsegraph_prepareEnvironment(wsi, session)) {
                lws_callback_on_writable(wsi);
                return 0;
            }
        }
        if(session->initialData->stage != 3) {
            switch(parsegraph_printItem(wsi, session, session->initialData)) {
            case 0:
                // Done!
                if(releaseWorldStream() != 0) {
                    session->initialData->error = 1;
                    strcpy(session->error, "Failed to commit prepared environment.");
                    lws_callback_on_writable(wsi);
                    return 0;
                }
                break;
            case -1:
                // Choked.
                break;
            case -2:
            default:
                // Died.
                releaseWorldStream();
                session->initialData->error = 1;
                strcpy(session->error, "Failed to prepare environment.");
                lws_callback_on_writable(wsi);
                return 0;
            }
            if(lws_send_pipe_choked(wsi)) {
                break;
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

            if(lws_send_pipe_choked(wsi)) {
                lws_callback_on_writable(wsi);
                break;
            }
        }*/
        break;

    default:
        break;
    }

    return 0;
}

static const struct lws_protocols protocols[] = {
    {
        "parsegraph-environment-protocol",
        callback_parsegraph_environment,
        sizeof(struct parsegraph_live_session),
        10, /* rx buf size must be >= permessage-deflate rx size */
    },
};

int prepareDBD(ap_dbd_t** dbdPointer)
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
    rv = parsegraph_prepareLoginStatements(modpool, dbd);
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
    }

    return 0;
}

static int pidFile;

LWS_VISIBLE int
init_parsegraph_environment_ws(struct lws_context *context, struct lws_plugin_capability *c)
{
    struct timeval time;
    gettimeofday(&time,NULL);

    // microsecond has 1 000 000
    // Assuming you did not need quite that accuracy
    // Also do not assume the system clock has that accuracy.
    srand((time.tv_sec * 1000) + (time.tv_usec / 1000));

    // Initialize the APR.
    apr_status_t rv;
    rv = apr_initialize();
    if(rv != APR_SUCCESS) {
        lwsl_err("Failed initializing APR. APR status of %d.\n", rv);
        return -1;
    }
    if (c->api_magic != LWS_PLUGIN_API_MAGIC) {
        lwsl_err("Plugin API %d, library API %d", LWS_PLUGIN_API_MAGIC,
             c->api_magic);
        return 1;
    }

    // Create the process-wide pool.
    rv = apr_pool_create(&modpool, 0);
    if(rv != APR_SUCCESS) {
        lwsl_err("Failed initializing APR pool for lwsws module: APR status of %d.\n", rv);
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
        lwsl_err("Failed loading DSO: %s", rverr);
        return -1;
    }

    // Create the PID file.
    pidFile = open("/home/dafrito/src/parsegraph/server/lwsws.pid", O_WRONLY | O_TRUNC | O_CREAT, 0664);
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

    // Setup Libwebsockets module.
    c->protocols = protocols;
    c->count_protocols = ARRAY_SIZE(protocols);
    c->extensions = NULL;
    c->count_extensions = 0;

    return 0;
}

LWS_VISIBLE int
destroy_parsegraph_environment_ws(struct lws_context *context)
{
    if(remove("/home/dafrito/src/parsegraph/server/lwsws.pid") < 0) {
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
