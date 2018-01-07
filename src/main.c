#include "rainback.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <locale.h>
#include <parsegraph_user.h>
#include <parsegraph_environment.h>
#include <parsegraph_List.h>
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>

#define MAXEVENTS 64

static int use_curses = 1;
static int use_ssl = 1;

AP_DECLARE(void) ap_log_perror_(const char *file, int line, int module_index,
                                int level, apr_status_t status, apr_pool_t *p,
                                const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char exp[512];
    memset(exp, 0, sizeof(exp));
    vsprintf(exp, fmt, args);
    dprintf(3, exp);
    va_end(args);
}

static void init_openssl()
{
    SSL_load_error_strings();	
    OpenSSL_add_ssl_algorithms();
}

static void cleanup_openssl()
{
    EVP_cleanup();
}

static SSL_CTX *create_context()
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    method = TLS_server_method();

    ctx = SSL_CTX_new(method);
    if (!ctx) {
	perror("Unable to create SSL context");
	ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }

    return ctx;
}

static void configure_context(SSL_CTX *ctx)
{
    SSL_CTX_set_ecdh_auto(ctx, 1);

    /* Set the key and cert */
    if (SSL_CTX_use_certificate_file(ctx, "certificate.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }
}

static int
make_socket_non_blocking (int sfd)
{
  int flags, s;

  flags = fcntl (sfd, F_GETFL, 0);
  if (flags == -1)
    {
      perror ("fcntl");
      return -1;
    }

  flags |= O_NONBLOCK;
  s = fcntl (sfd, F_SETFL, flags);
  if (s == -1)
    {
      perror ("fcntl");
      return -1;
    }

  return 0;
}

static int
create_and_bind (const char *port)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int s, sfd;

  memset (&hints, 0, sizeof (struct addrinfo));
  hints.ai_family = AF_UNSPEC;     /* Return IPv4 and IPv6 choices */
  hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */
  hints.ai_flags = AI_PASSIVE;     /* All interfaces */

  s = getaddrinfo (NULL, port, &hints, &result);
  if (s != 0)
    {
      fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (s));
      return -1;
    }

  for (rp = result; rp != NULL; rp = rp->ai_next)
    {
      sfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (sfd == -1)
        continue;

      s = bind (sfd, rp->ai_addr, rp->ai_addrlen);
      if (s == 0)
        {
          /* We managed to bind successfully! */
          break;
        }
        else {
          fprintf(stderr, "Could not bind: %s (errno=%d)\n", strerror(errno), errno);
      }

      close (sfd);
    }

  if (rp == NULL)
    {
      fprintf(stderr, "Could not bind on any sockets.");
      return -1;
    }

  freeaddrinfo (result);

  return sfd;
}

static int
create_and_connect(const char* port)
{
   struct addrinfo *result, *rp;
   int sfd, s;

   /* Obtain address(es) matching host/port */

    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* TCP socket */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          /* Any protocol */

    s = getaddrinfo("localhost", port, &hints, &result);
    if (s != 0) {
       fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
       exit(EXIT_FAILURE);
    }

    /* getaddrinfo() returns a list of address structures.
      Try each address until we successfully connect(2).
      If socket(2) (or connect(2)) fails, we (close the socket
      and) try the next address. */

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype,
                rp->ai_protocol);
        if (sfd == -1)
            continue;

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
            break;                  /* Success */

        close(sfd);
    }

    if (rp == NULL) {               /* No address succeeded */
        return -1;
    }

    freeaddrinfo(result);           /* No longer needed */

    return sfd;
}

void on_sigusr1()
{
    // Do nothing, but don't ignore it, so that it interrupts epoll_wait
}

static int exit_value = EXIT_SUCCESS;
extern void* terminal_operator(void* data);

struct parsegraph_Server server;

int main(int argc, const char**argv)
{
    int s;
    struct epoll_event *events;

    apr_pool_initialize();

    parsegraph_Server_init(&server);

    if(0 != pthread_mutex_lock(&server.server_mutex)) {
        fprintf(stderr, "Failed to acquire server mutex");
        exit(EXIT_FAILURE);
    }

    // Validate command-line.
    if(argc < 3) {
        fprintf(stderr, "Usage: %s [port] [backend-port] modulepath?modulefunc modulepath?modulefunc\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Do not die from SIGPIPE.
    signal(SIGUSR1, on_sigusr1);
    signal(SIGPIPE, SIG_IGN);

    server.server_status = parsegraph_SERVER_STARTED;

    // Create the SSL context
    SSL_CTX *ctx = 0;
    if(use_ssl) {
        init_openssl();
        ctx = create_context();
        configure_context(ctx);
    }

    // Create epoll queue.
    server.efd = epoll_create1(0);
    if(server.efd == -1) {
        perror("Creating main epoll queue for server");
        exit(EXIT_FAILURE);
    }

    // Create the server socket
    server.sfd = create_and_bind(argv[1]);
    if(server.sfd == -1) {
        perror("Creating main server socket for server");
        exit(EXIT_FAILURE);
    }
    strcpy(server.serverport, argv[1]);
    s = make_socket_non_blocking(server.sfd);
    if(s == -1) {
        exit_value = EXIT_FAILURE;
        goto destroy;
    }
    s = listen(server.sfd, SOMAXCONN);
    if(s == -1) {
        perror ("listen");
        exit_value = EXIT_FAILURE;
        goto destroy;
    }
    else {
        struct epoll_event event;
        memset(&event, 0, sizeof(struct epoll_event));
        event.data.fd = server.sfd;
        event.events = EPOLLIN | EPOLLET;
        s = epoll_ctl (server.efd, EPOLL_CTL_ADD, server.sfd, &event);
        if(s == -1) {
            perror("Adding server file descriptor to epoll queue");
            exit_value = EXIT_FAILURE;
            goto destroy;
        }
    }

    // Create the backend socket
    server.backendfd = create_and_connect(argv[2]);
    if(server.backendfd == -1) {
        perror("Connecting to backend server");
        exit_value = EXIT_FAILURE;
        goto destroy;
    }
    else {
        strcpy(server.backendport, argv[2]);
        s = make_socket_non_blocking(server.backendfd);
        if(s == -1) {
            exit_value = EXIT_FAILURE;
            goto destroy;
        }

        parsegraph_Connection* backend = parsegraph_Connection_new(&server);
        parsegraph_Backend_init(backend, server.backendfd);
        server.backend = backend;

        struct epoll_event event;
        memset(&event, 0, sizeof(struct epoll_event));
        event.data.ptr = backend;
        event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        s = epoll_ctl(server.efd, EPOLL_CTL_ADD, server.backendfd, &event);
        if(s == -1) {
            perror("Adding backend file descriptor to epoll queue");
            exit_value = EXIT_FAILURE;
            goto destroy;
        }
    }
    server.backendPort = argv[2];

    if(argc > 3) {
        for(int n = 3; n < argc; ++n) {
            const char* arg = argv[n];
            char* loc = index(arg, '?');
            if(loc == 0) {
                fprintf(stderr, "A module symbol must be provided.\nUsage: %s [port] [backend-port] modulepath?modulefunc...\n", argv[0]);
                exit(EXIT_FAILURE);
            }
            char modulename[1024];
            memset(modulename, 0, sizeof modulename);
            strncpy(modulename, arg, loc - arg);
            void* loaded = dlopen(modulename, RTLD_NOW|RTLD_GLOBAL);
            if(!loaded) {
                fprintf(stderr, "Failed to open module \"%s\": %s\nUsage: %s [port] [backend-port] modulepath?modulefunc...\n", modulename, dlerror(), argv[0]);
                exit(EXIT_FAILURE);
            }
            void* loadedFunc = dlsym(loaded, loc + 1);
            if(!loadedFunc) {
                fprintf(stderr, "Failed to locate function \"%s\"\nUsage: %s [port] [backend-port] modulepath?modulefunc...\n", loc + 1, argv[0]);
                exit(EXIT_FAILURE);
            }
            void(*moduleFunc)(struct parsegraph_Server*, enum parsegraph_ServerModuleEvent) = loadedFunc;
            moduleFunc(&server, parsegraph_EVENT_SERVER_MODULE_START);

            struct parsegraph_ServerModule* serverModule = malloc(sizeof *serverModule);
            serverModule->moduleFunc = moduleFunc;
            serverModule->moduleDef = arg;
            serverModule->moduleHandle = loaded;
            serverModule->nextModule = 0;
            if(server.last_module) {
                server.last_module->nextModule = serverModule;
                server.last_module = serverModule;
                serverModule->prevModule = server.last_module;
            }
            else {
                server.first_module = serverModule;
                server.last_module = serverModule;
                serverModule->prevModule = 0;
            }
        }
    }

    events = (struct epoll_event*)calloc(MAXEVENTS, sizeof(struct epoll_event));
    memset(events, 0, sizeof(struct epoll_event)*MAXEVENTS);

    // Create terminal interface thread.
    if(use_curses && 0 != pthread_create(&server.terminal_thread, 0, terminal_operator, &server)) {
        fprintf(stderr, "Failed to create terminal thread");
        exit(EXIT_FAILURE);
    }

    while(1) {
        int n, i;

        if(server.server_status == parsegraph_SERVER_DESTROYING) {
            break;
        }
        server.server_status = parsegraph_SERVER_WAITING_FOR_INPUT;
        if(0 != pthread_mutex_unlock(&server.server_mutex)) {
            fprintf(stderr, "Failed to release server mutex");
            exit_value = EXIT_FAILURE;
            goto destroy_without_unlock;
        }
wait:   n = epoll_wait(server.efd, events, MAXEVENTS, -1);
        if(n <= 0) {
            if(server.server_status != parsegraph_SERVER_DESTROYING && (n == 0 || errno == EINTR)) {
                goto wait;
            }
            server.server_status = parsegraph_SERVER_DESTROYING;
            goto destroy;
        }

        // Acquire the server's lock for processing.
        if(0 != pthread_mutex_lock(&server.server_mutex)) {
            fprintf(stderr, "Failed to acquire server mutex");
            exit_value = EXIT_FAILURE;
            goto destroy_without_unlock;
        }

        // Set the status.
        if(server.server_status == parsegraph_SERVER_DESTROYING) {
            goto destroy;
        }
        server.server_status = parsegraph_SERVER_PROCESSING;

        for(i = 0; i < n; i++) {
            if(events[i].data.fd == server.backendfd) {
                if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
                }
                continue;
            }
            else if (server.sfd == events[i].data.fd) {
                // Event is from server socket.
                if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
                    server.server_status = parsegraph_SERVER_DESTROYING;
                    goto destroy;
                }

                // Accept socket connections.
                while(1) {
                    struct sockaddr in_addr;
                    socklen_t in_len;
                    int infd;
                    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

                    in_len = sizeof in_addr;
                    infd = accept(server.sfd, &in_addr, &in_len);
                    if(infd == -1) {
                        if((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                            // Done processing connections.
                            break;
                        }
                        else {
                            perror("Error accepting connection");
                            break;
                        }
                    }

                    s = getnameinfo(&in_addr, in_len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
                    if(s == 0) {
                        //printf("Accepted connection on descriptor %d "
                        //"(host=%s, port=%s)\n", infd, hbuf, sbuf);
                    }

                    /* Make the incoming socket non-blocking and add it to the
                    list of fds to monitor. */
                    s = make_socket_non_blocking (infd);
                    if(s != 0) {
                        close(infd);
                        continue;
                    }

                    parsegraph_Connection* cxn = parsegraph_Connection_new(&server);
                    if(!cxn) {
                        perror("Unable to create connection");
                        close(infd);
                        continue;
                    }
                    if(use_ssl) {
                        s = parsegraph_SSL_init(cxn, ctx, infd);
                        if(s <= 0) {
                            perror("Unable to initialize SSL connection");
                            close(infd);
                            continue;
                        }
                    }
                    else {
                        parsegraph_cleartext_init(cxn, infd);
                    }

                    struct epoll_event event;
                    memset(&event, 0, sizeof(struct epoll_event));
                    event.data.ptr = cxn;
                    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                    s = epoll_ctl(server.efd, EPOLL_CTL_ADD, infd, &event);
                    if(s == -1) {
                        perror ("epoll_ctl");
                        parsegraph_Connection_destroy(cxn);
                        close(infd);
                        continue;
                    }
                }
                continue;
            }
            else {
                if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
                    // An error has occured on this fd, or the socket is not ready for reading (why were we notified then?)
                    //printf("epoll error: %d\n", events[i].events);
                    parsegraph_Connection* source = (parsegraph_Connection*)events[i].data.ptr;
                    parsegraph_Connection_destroy(source);
                    continue;
                }
                /* Connection is ready */
                parsegraph_Connection* cxn = (parsegraph_Connection*)events[i].data.ptr;
                if(events[i].events & EPOLLIN) {
                    cxn->wantsRead = 0;
                    // Available for read.
                    parsegraph_clientRead(cxn);
                }
                if(events[i].events & EPOLLOUT) {
                    // Available for write.
                    cxn->wantsWrite = 0;
                    parsegraph_clientWrite(cxn);
                }
                if(cxn->shouldDestroy) {
                    parsegraph_Connection_destroy(cxn);
                }
            }
        }
    }

destroy:
    if(0 != pthread_mutex_unlock(&server.server_mutex)) {
        //fprintf(stderr, "Failed to release server mutex");
        exit_value = EXIT_FAILURE;
    }
destroy_without_unlock:
    for(struct parsegraph_ServerModule* serverModule = server.first_module; serverModule != 0; serverModule = serverModule->nextModule) {
        serverModule->moduleFunc(serverModule->moduleHandle, parsegraph_EVENT_SERVER_MODULE_STOP);
    }

    free(events);
    if(use_ssl) {
        SSL_CTX_free(ctx);
        cleanup_openssl();
    }
    close(server.sfd);
    close(server.backendfd);
    if(use_curses && server.terminal_thread) {
        void* retval;
        pthread_join(server.terminal_thread, &retval);
    }
    apr_pool_terminate();
}