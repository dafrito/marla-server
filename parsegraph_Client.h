#ifndef parsegraph_Client_INCLUDED
#define parsegraph_Client_INCLUDED

#include <sys/epoll.h>
#include <openssl/ssl.h>

enum parsegraph_ClientStage {
parsegraph_CLIENT_ACCEPTED = 0, /* struct has been created and socket FD has been set */
parsegraph_CLIENT_SECURED = 1, /* SSL has been accepted */
parsegraph_CLIENT_COMPLETE = 2 /* Done with connection */
};

typedef struct {
parsegraph_ClientStage stage;
int fd;
SSL_CTX* ctx;
SSL* ssl;
int shouldDestroy;
int wantsWrite;
int wantsRead;
struct epoll_event poll;
} parsegraph_Client;

parsegraph_Client* parsegraph_Client_new(SSL_CTX* ctx, int fd);
void parsegraph_Client_handle(parsegraph_Client* client, int event);
void parsegraph_Client_destroy(parsegraph_Client* client);

#endif // parsegraph_Client_INCLUDED
