#include "marla.h"
#include <time.h>
#include <errno.h>

static void idle_tick(marla_Server* server)
{
    for(marla_Connection* cxn = server->first_connection; cxn;) {
        if(server->server_status == marla_SERVER_DESTROYING) {
            break;
        }

        struct timespec currentProcessTime;
        clock_gettime(CLOCK_MONOTONIC, &currentProcessTime);

        if(currentProcessTime.tv_sec - cxn->lastProcessTime.tv_sec < 0) {
            // Skip recently processed connections
            cxn = cxn->next_connection;
            continue;
        }

        if(cxn->stage == marla_CLIENT_ACCEPTED) {
            if(marla_clientAccept(cxn) != 0) {
                cxn = cxn->next_connection;
                continue;
            }
        }

        if(cxn->stage == marla_CLIENT_SECURED) {
            // Process connections
            for(int loop = 1; loop && cxn->stage != marla_CLIENT_COMPLETE;) {
                marla_WriteResult wr = marla_clientRead(cxn);
                size_t refilled = 0;
                switch(wr) {
                case marla_WriteResult_CONTINUE:
                    continue;
                case marla_WriteResult_UPSTREAM_CHOKED:
                    marla_Connection_refill(cxn, &refilled);
                    if(refilled <= 0) {
                        loop = 0;
                    }
                    continue;
                case marla_WriteResult_DOWNSTREAM_CHOKED:
                    if(cxn->stage != marla_CLIENT_COMPLETE && marla_Ring_size(cxn->output) > 0) {
                        int nflushed;
                        int rv = marla_Connection_flush(cxn, &nflushed);
                        if(rv <= 0) {
                            //fprintf(stderr, "Responder choked.\n");
                            loop = 0;
                        }
                    }
                    else {
                        loop = 0;
                    }
                    continue;
                default:
                    //fprintf(stderr, "Connection %d's read returned %s\n", cxn->id, marla_nameWriteResult(wr));
                    loop = 0;
                    continue;
                }
            }
            for(int loop = 1; loop && cxn->stage != marla_CLIENT_COMPLETE;) {
                marla_WriteResult wr = marla_clientWrite(cxn);
                size_t refilled = 0;
                switch(wr) {
                case marla_WriteResult_CONTINUE:
                    continue;
                case marla_WriteResult_UPSTREAM_CHOKED:
                    marla_Connection_refill(cxn, &refilled);
                    if(refilled <= 0) {
                        loop = 0;
                    }
                    continue;
                case marla_WriteResult_DOWNSTREAM_CHOKED:
                    if(cxn->stage != marla_CLIENT_COMPLETE && marla_Ring_size(cxn->output) > 0) {
                        int nflushed;
                        int rv = marla_Connection_flush(cxn, &nflushed);
                        if(rv <= 0) {
                            //fprintf(stderr, "Responder choked.\n");
                            loop = 0;
                        }
                    }
                    else {
                        loop = 0;
                    }
                    continue;
                default:
                    //fprintf(stderr, "Connection %d's write returned %s\n", cxn->id, marla_nameWriteResult(wr));
                    loop = 0;
                    continue;
                }
            }
        }

        if(cxn->stage == marla_CLIENT_COMPLETE && !cxn->shouldDestroy) {
            // Client needs shutdown.
            if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
                cxn->shouldDestroy = 1;
            }
        }

        if(cxn->shouldDestroy) {
            marla_Connection* next = cxn->next_connection;
            marla_logLeave(server, "Destroying connection.");
            marla_Connection_destroy(cxn);
            if(next) {
                cxn = next;
            }
            else {
                cxn = server->first_connection;
            }
        }
        else {
            cxn = cxn->next_connection;
        }
    }
}

void* idle_operator(void* data)
{
    struct marla_Server* server = (struct marla_Server*)data;

    for(;;) {
        struct timespec req;
        req.tv_sec = 1;
        req.tv_nsec = 0;
        switch(pthread_mutex_timedlock(&server->server_mutex, &req)) {
        case 0:
            server->idleTimeouts = 0;
            idle_tick(server);
            if(0 != pthread_mutex_unlock(&server->server_mutex)) {
                perror("unlock");
                abort();
            }
            break;
        case ETIMEDOUT:
            ++server->idleTimeouts;
            if(server->idleTimeouts > 30) {
                server->server_status = marla_SERVER_DESTROYING;
                break;
            }
            continue;
        default:
            server->idleTimeouts = 0;
            server->server_status = marla_SERVER_DESTROYING;
            break;
        }
        if(server->server_status == marla_SERVER_DESTROYING) {
            break;
        }
        struct timespec rem;
        for(int rv = nanosleep(&req, &rem); rv == -1; rv = nanosleep(&rem, &rem)) {
            if(errno == EINTR) {
                continue;
            }
            perror("nanosleep");
            abort();
        }
    }

    return 0;
}
