#include "parsegraph_Client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>

static void common_SSL_return(parsegraph_Client* client, int rv)
{
    switch(SSL_get_error(client->ssl, rv)) {
    case SSL_ERROR_WANT_WRITE:
        client->wantsWrite = 1;
        break;
    case SSL_ERROR_WANT_READ:
        client->wantsRead = 1;
        break;
    default:
        client->shouldDestroy = 1;
        break;
    }
}

parsegraph_Client* parsegraph_Client_new(SSL_CTX* ctx, int fd)
{
    parsegraph_Client* client = (parsegraph_Client*)malloc(sizeof(*client));
    if(!client) {
        return 0;
    }
    client->stage = parsegraph_CLIENT_ACCEPTED;
    client->ctx = ctx;
    client->fd = fd;
    client->shouldDestroy = 0;
    client->wantsWrite = 0;
    client->wantsRead = 0;
    client->ssl = SSL_new(ctx);
    if(1 != SSL_set_fd(client->ssl, client->fd)) {
        free(client);
        return 0;
    }
    return client;
}

void parsegraph_Client_shutdown(parsegraph_Client* client)
{
    client->stage = parsegraph_CLIENT_COMPLETE;
}

void parsegraph_Client_destroy(parsegraph_Client* client)
{
    SSL_free(client->ssl);

    /* Closing the descriptor will make epoll remove it
     from the set of descriptors which are monitored. */
    close(client->fd);
    free(client);
}

void parsegraph_Client_handle(parsegraph_Client* client, int event)
{
    // Ensure the client can be handled.
    if(client->wantsRead) {
        if(event & EPOLLIN) {
            client->wantsRead = 0;
        }
        else {
            return;
        }
    }
    if(client->wantsWrite) {
        if(event & EPOLLOUT) {
            client->wantsWrite = 0;
        }
        else {
            return;
        }
    }

    if(client->stage == parsegraph_CLIENT_ACCEPTED) {
        // Client has just connected.
        int rv = SSL_accept(client->ssl);
        if(rv == 0) {
            // Shutdown controlled
            client->shouldDestroy = 1;
            return;
        }
        else if(rv != 1) {
            common_SSL_return(client, rv);
            return;
        }

        // Accepted and secured.
        client->stage = parsegraph_CLIENT_SECURED;
    }
    if(client->stage == parsegraph_CLIENT_SECURED) {
        // Ready to read payload from client.
        char buf[1024];
        int nread = SSL_read(client->ssl, buf, sizeof(buf));
        if(nread <= 0) {
            common_SSL_return(client, nread);
            return;
        }
        else {
            write(1, buf, nread);
            const char reply[] = "HTTP/1.1 200 OK\r\n\r\n<html><body>Hello, <b>world.</b></body></html>";
            int nwritten = SSL_write(client->ssl, reply, strlen(reply));
            if(nwritten <= 0) {
                common_SSL_return(client, nwritten);
                return;
            }
        }

        client->stage = parsegraph_CLIENT_COMPLETE;
    }
    if(client->stage == parsegraph_CLIENT_COMPLETE) {
        // Client needs shutdown.
        int rv = 0;
        while(rv == 0) {
            rv = SSL_shutdown(client->ssl);
            if(rv < 0) {
                common_SSL_return(client, rv);
                return;
            }
            else if(rv == 1) {
                client->shouldDestroy = 1;
                return;
            }
        }
    }
}
