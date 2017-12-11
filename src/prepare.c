#include "prepare.h"

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

int parsegraph_printItem(parsegraph_live_session* session, struct printing_item* level)
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
                lwsl_err("Error encountered retrieving list name.\n");
                goto die;
            }
            if(value == 0) {
                value = "";
                typeId = 0;
            }
            int written = snprintf(buf + LWS_PRE, sizeof(buf) - LWS_PRE, "{\"id\":%d, \"value\":\"%s\", \"type\":%d, \"items\":[", level->listId, value, typeId);
            if(written < 0) {
                lwsl_err("Failed to write to string buffer.\n");
                goto die;
            }
            if(written >= sizeof(buf) - LWS_PRE) {
                lwsl_err("String buffer overflowed.\n");
                goto die;
            }

            //fprintf(stderr, "printing stage 1\n");
            int n = lws_write(wsi, buf + LWS_PRE, written, LWS_WRITE_TEXT);
            if(n < 0) {
                lwsl_err("Error while writing item.\n");
                goto die;
            }
            if(n < written) {
                lwsl_err("Failed to completely write item data.\n");
                goto die;
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
                    lwsl_err("Failed to list items for item.\n");
                    goto die;
                }
            }

            //fprintf(stderr, "printing stage 2\n");
            if(level->index < level->nvalues) {
                // Create the level for the child.
                struct printing_item* subLevel = malloc(sizeof(*level));
                if(!subLevel) {
                    lwsl_err("Memory allocation error.");
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
            if(lws_send_pipe_choked(wsi)) {
                goto choked;
            }
        }

        if(level->stage == 2) {
            // Ready to print end of child list and object.
            //fprintf(stderr, "printing stage 3\n");
            int written;
            if(level->parentLevel && (level->parentLevel->index < level->parentLevel->nvalues)) {
                strcpy(buf + LWS_PRE, "]},");
                written = lws_write(wsi, buf + LWS_PRE, strlen("]},"), LWS_WRITE_TEXT);
            }
            else {
                strcpy(buf + LWS_PRE, "]}");
                written = lws_write(wsi, buf + LWS_PRE, strlen("]}"), LWS_WRITE_TEXT);
            }
            if(written < 0) {
                lwsl_err("Error while writing end of JSON.\n");
                goto die;
            }
            if(written < 2) {
                lwsl_err("Partial write of end of JSON.\n");
                goto die;
            }
            //lwsl_err("reached stage 3\n");

            // Suffix printed.
            level->stage = 3;
        }

        if(level->stage != 3) {
            lwsl_err("Level failed to completely write. Stage=%d\n", level->stage);
            goto die;
        }

        // Move to the parent and free this level.
        struct printing_item* childLevel = level;
        level = level->parentLevel;
        if(level) {
            free(childLevel);
            level->nextLevel = 0;
        }
        if(lws_send_pipe_choked(wsi)) {
            goto choked;
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
