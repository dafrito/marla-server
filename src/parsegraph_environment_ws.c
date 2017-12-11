#include <sqlite3.h>
#include "prepare.h"
#include "rainback.h"

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
    va_end(args);
}

static void
callback_parsegraph_environment(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent reason, void *in, size_t len)
{
    parsegraph_live_session* session = req->handleData;

    int neededEnvLength = 36;
    int m, rv;
    static unsigned char buf[MAX_INIT_LENGTH];
    switch(reason) {
    case parsegraph_EVENT_WEBSOCKET_CLOSE_REASON:
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

    case parsegraph_EVENT_WEBSOCKET_MUST_READ:
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

    case parsegraph_EVENT_WEBSOCKET_ESTABLISHED:
        // Initialize the session structure.
        if(0 != initialize_parsegraph_live_session(session)) {
            strcpy(session->error, "Error initializing session.");
            return;
        }

        // Retrieve the cookie.
        size_t cookieLen = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_COOKIE);
        if(cookieLen > sizeof(buf)) {
            strcpy(session->error, "Session cookie is too long.");
            return;
        }
        char* cookie = buf;
        rv = lws_hdr_copy(wsi, cookie, sizeof(buf), WSI_TOKEN_HTTP_COOKIE);
        if(rv < 0) {
            snprintf(session->error, session->errorBufSize, "Failed to retrieve session. Error returned was %d\n", rv);
            return;
        }

        // Authenticate.
        char* sessionValue = strstr(cookie, "session=") + strlen("session=");
        session->login.username = 0;
        session->login.userId = -1;
        if(sessionValue && 0 == parsegraph_deconstructSessionString(session->pool, sessionValue, &session->login.session_selector, &session->login.session_token)) {
            parsegraph_UserStatus rv = parsegraph_refreshUserLogin(session->pool, controlDBD, &session->login);
            if(rv != parsegraph_OK) {
                strcpy(session->error, parsegraph_nameUserStatus(rv));
                return;
            }

            parsegraph_UserStatus idRV = parsegraph_getIdForUsername(session->pool, controlDBD, session->login.username, &(session->login.userId));
            if(parsegraph_isSeriousUserError(idRV)) {
                strcpy(session->error, "Failed to retrieve ID for authenticated login.\n");
                return;
            }
        }
        if(!session->login.username) {
            strcpy(session->error, "Session does not match any user.");
            return;
        }
        break;

    case parsegraph_EVENT_WEBSOCKET_MUST_WRITE:
    case parsegraph_EVENT_WEBSOCKET_RESPOND:
        if(strlen(session->error) > 0) {
            if(!session->closed) {
                session->closed = 1;
                lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, session->error, strlen(session->error));
            }
            fprintf(stderr, "Closing connection. %s", session->error);
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
            switch(parsegraph_printItem(session, session->initialData)) {
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

            if(lws_send_pipe_choked(wsi)) {
                lws_callback_on_writable(wsi);
                break;
            }
        }*/
        break;

    default:
        break;
    }

    return;
}
