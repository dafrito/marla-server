#include "marla.h"
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

#include "socket_funcs.c"

void on_sigusr1()
{
    // Do nothing, but don't ignore it, so that it interrupts epoll_wait
}

static int exit_value = EXIT_SUCCESS;
extern void* terminal_operator(void* data);

struct marla_Server server;

int main(int argc, const char**argv)
{
    int s;
    struct epoll_event *events = 0;

    const size_t MIN_ARGS = 4;

    apr_pool_initialize();

    marla_Server_init(&server);

    if(0 != pthread_mutex_lock(&server.server_mutex)) {
        fprintf(stderr, "Failed to acquire server mutex");
        exit(EXIT_FAILURE);
    }

    marla_logEnterc(&server, "Server initializations", "Initializing server");

    // Validate command-line.
    if(argc < MIN_ARGS) {
        fprintf(stderr, "Usage: %s [port] [backend-port] [logging-port] modulepath?modulefunc modulepath?modulefunc\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Do not die from SIGPIPE.
    signal(SIGUSR1, on_sigusr1);
    signal(SIGPIPE, SIG_IGN);

    server.server_status = marla_SERVER_STARTED;

    // Create epoll queue.
    server.efd = epoll_create1(0);
    if(server.efd == -1) {
        perror("Creating main epoll queue for server");
        marla_logLeave(&server, "Failed to create epoll queue.");
        exit(EXIT_FAILURE);
    }

    // Create the logging socket
    server.logfd = create_and_connect("localhost", argv[3]);
    if(server.logfd != -1) {
        strcpy(server.logaddress, argv[3]);
        marla_logMessagef(&server, "Server is logging on port %s", server.logaddress);
        s = make_socket_non_blocking(server.logfd);
        if(s == -1) {
            exit_value = EXIT_FAILURE;
            marla_logLeave(&server, "Failed to make logging server non-blocking.");
            goto destroy;
        }

        struct epoll_event event;
        memset(&event, 0, sizeof(struct epoll_event));
        event.data.fd = server.logfd;
        event.events = EPOLLOUT | EPOLLRDHUP | EPOLLET;
        s = epoll_ctl(server.efd, EPOLL_CTL_ADD, server.logfd, &event);
        if(s == -1) {
            perror("Adding logging file descriptor to epoll queue");
            exit_value = EXIT_FAILURE;
            marla_logLeave(&server, "Failed to add logging server to epoll queue.");
            goto destroy;
        }
    }

    // Create the server socket
    server.sfd = create_and_bind(argv[1]);
    if(server.sfd == -1) {
        perror("Creating main server socket for server");
        marla_logLeave(&server, "Failed to create server socket.");
        exit(EXIT_FAILURE);
    }
    strcpy(server.serverport, argv[1]);
    marla_logMessagef(&server, "Server is using port %s", server.serverport);
    s = make_socket_non_blocking(server.sfd);
    if(s == -1) {
        exit_value = EXIT_FAILURE;
        marla_logLeave(&server, "Failed to make server socket non-blocking.");
        goto destroy;
    }
    s = listen(server.sfd, SOMAXCONN);
    if(s == -1) {
        perror ("listen");
        exit_value = EXIT_FAILURE;
        marla_logLeave(&server, "Failed to listen to server socket.");
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
            marla_logLeave(&server, "Failed to add server socket to epoll queue.");
            goto destroy;
        }
    }

    // Create the backend socket
    strcpy(server.backendport, argv[2]);

    if(argc > MIN_ARGS) {
        for(int n = MIN_ARGS; n < argc; ++n) {
            const char* arg = argv[n];
            if(!strcmp(arg, "-nocurses")) {
                use_curses = 0;
                continue;
            }
            if(!strcmp(arg, "-nossl")) {
                use_ssl = 0;
                continue;
            }
            if(!strcmp(arg, "-ssl")) {
                use_ssl = 1;
                continue;
            }
            if(!strcmp(arg, "-curses")) {
                use_curses = 1;
                continue;
            }
            char* loc = index(arg, '?');
            if(loc == 0) {
                fprintf(stderr, "A module symbol must be provided.\nUsage: %s [port] [backend-port] modulepath?modulefunc...\n", argv[0]);
                marla_logLeave(&server, "Failed to read module symbol.");
                exit(EXIT_FAILURE);
            }
            char modulename[1024];
            memset(modulename, 0, sizeof modulename);
            strncpy(modulename, arg, loc - arg);
            void* loaded = dlopen(modulename, RTLD_NOW|RTLD_GLOBAL);
            if(!loaded) {
                fprintf(stderr, "Failed to open module \"%s\": %s\nUsage: %s [port] [backend-port] modulepath?modulefunc...\n", modulename, dlerror(), argv[0]);
                marla_logLeave(&server, "Failed to open module.");
                exit(EXIT_FAILURE);
            }
            void* loadedFunc = dlsym(loaded, loc + 1);
            if(!loadedFunc) {
                fprintf(stderr, "Failed to locate function \"%s\"\nUsage: %s [port] [backend-port] modulepath?modulefunc...\n", loc + 1, argv[0]);
                marla_logLeave(&server, "Failed to locate module initializer.");
                exit(EXIT_FAILURE);
            }
            void(*moduleFunc)(struct marla_Server*, enum marla_ServerModuleEvent) = loadedFunc;
            moduleFunc(&server, marla_EVENT_SERVER_MODULE_START);

            struct marla_ServerModule* serverModule = malloc(sizeof *serverModule);
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

    // Create the SSL context
    SSL_CTX *ctx = 0;
    if(use_ssl) {
        marla_logMessage(&server, "Using SSL.");
        init_openssl();
        ctx = create_context();
        configure_context(ctx);
    }

    events = (struct epoll_event*)calloc(MAXEVENTS, sizeof(struct epoll_event));
    memset(events, 0, sizeof(struct epoll_event)*MAXEVENTS);

    // Create terminal interface thread.
    if(use_curses && 0 != pthread_create(&server.terminal_thread, 0, terminal_operator, &server)) {
        fprintf(stderr, "Failed to create terminal thread");
        marla_logLeave(&server, "Failed to create terminal thread");
        exit(EXIT_FAILURE);
    }
    else if(!use_curses) {
        marla_logMessage(&server, "curses disabled.");
    }
    else {
        server.has_terminal = 1;
        marla_logMessage(&server, "curses enabled.");
    }
    marla_logLeave(&server, "Entering event loop.");

    while(1) {
        int n, i;

        if(server.server_status == marla_SERVER_DESTROYING) {
            break;
        }
        server.server_status = marla_SERVER_WAITING_FOR_INPUT;
        if(0 != pthread_mutex_unlock(&server.server_mutex)) {
            fprintf(stderr, "Failed to release server mutex");
            exit_value = EXIT_FAILURE;
            goto destroy_without_unlock;
        }
wait:   n = epoll_wait(server.efd, events, MAXEVENTS, -1);
        if(n <= 0) {
            if(server.server_status != marla_SERVER_DESTROYING && (n == 0 || errno == EINTR)) {
                goto wait;
            }
            server.server_status = marla_SERVER_DESTROYING;
            goto destroy;
        }

        // Acquire the server's lock for processing.
        if(0 != pthread_mutex_lock(&server.server_mutex)) {
            fprintf(stderr, "Failed to acquire server mutex");
            exit_value = EXIT_FAILURE;
            goto destroy_without_unlock;
        }

        // Set the status.
        if(server.server_status == marla_SERVER_DESTROYING) {
            marla_logMessagec(&server, "Server processing", "Server is being destroyed.");
            goto destroy;
        }
        server.server_status = marla_SERVER_PROCESSING;

        for(i = 0; i < n; i++) {
            if(events[i].data.fd == server.logfd) {
                if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
                    if(events[i].events & EPOLLRDHUP) {
                        continue;
                    }
                    //fprintf(stderr, "epoll error on logfd: %d\n", events[i].events);
                    continue;
                }
                if(events[i].events & EPOLLOUT) {
                    server.wantsLogWrite = 0;
                    marla_Server_flushLog(&server);
                }
                continue;
            }
            else if (server.sfd == events[i].data.fd) {
                // Event is from server socket.
                if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
                    server.server_status = marla_SERVER_DESTROYING;
                    marla_logMessage(&server, "Server socket died. Destroying server.");
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
                            break;
                        }
                        else {
                            perror("Error accepting connection");
                            break;
                        }
                    }

                    s = getnameinfo(&in_addr, in_len, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV);
                    if(s == 0) {
                        marla_logMessagecf(&server, "Server socket connections", "Accepted connection on descriptor %d "
                        "(host=%s, port=%s)\n", infd, hbuf, sbuf);
                    }

                    /* Make the incoming socket non-blocking and add it to the
                    list of fds to monitor. */
                    s = make_socket_non_blocking (infd);
                    if(s != 0) {
                        close(infd);
                        continue;
                    }

                    marla_Connection* cxn = marla_Connection_new(&server);
                    if(!cxn) {
                        perror("Unable to create connection");
                        close(infd);
                        continue;
                    }
                    if(use_ssl) {
                        s = marla_SSL_init(cxn, ctx, infd);
                        if(s <= 0) {
                            perror("Unable to initialize SSL connection");
                            close(infd);
                            continue;
                        }
                    }
                    else {
                        marla_cleartext_init(cxn, infd);
                    }

                    struct epoll_event event;
                    memset(&event, 0, sizeof(struct epoll_event));
                    event.data.ptr = cxn;
                    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
                    s = epoll_ctl(server.efd, EPOLL_CTL_ADD, infd, &event);
                    if(s == -1) {
                        perror ("epoll_ctl");
                        marla_Connection_destroy(cxn);
                        close(infd);
                        continue;
                    }
                }
                continue;
            }
            else {
                //marla_logMessagef(&server, "%d", events[i].events);
                if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (events[i].events & EPOLLRDHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
                    marla_Connection* cxn = (marla_Connection*)events[i].data.ptr;
                    // An error has occured on this fd, or the socket is not ready for reading (why were we notified then?)
                    if(events[i].events & EPOLLRDHUP) {
                        if(cxn == server.backend) {
                            marla_logMessagef(&server, "Backend connection done sending data.");
                            marla_Connection_destroy(cxn);
                        }
                        marla_logMessagef(&server, "Connection done sending data.");
                        continue;
                    }
                    if(events[i].events & EPOLLHUP) {
                        marla_Connection_destroy(cxn);
                        if(cxn == server.backend) {
                            marla_logMessagef(&server, "Backend connection done accepting connections.");
                        }
                        continue;
                    }
                    marla_Connection_destroy(cxn);
                    //fprintf(stderr, "epoll error: %d\n", events[i].events);
                    marla_logMessagef(&server, "Epoll error %d (EPOLLERR=%d, EPOLLHUP=%d)", events[i].events, events[i].events&EPOLLERR, events[i].events&EPOLLHUP);
                    continue;
                }
                marla_Connection* cxn = (marla_Connection*)events[i].data.ptr;
                {
                    char buf[marla_BUFSIZE];
                    memset(buf, 0, sizeof buf);
                    cxn->describeSource(cxn, buf, sizeof buf);

                    if(events[i].events & EPOLLOUT && events[i].events & EPOLLIN) {
                        marla_logEntercf(&server, "Client processing", "Received client EPOLLIN and EPOLLOUT socket event on %s.", buf);
                    }
                    else if(events[i].events & EPOLLIN) {
                        marla_logEntercf(&server, "Client processing", "Received client EPOLLIN socket event on %s.", buf);
                    }
                    else if(events[i].events & EPOLLOUT) {
                        marla_logEntercf(&server, "Client processing", "Received client EPOLLOUT socket event on %s.", buf);
                    }
                    else {
                        marla_die(&server, "Unexpected epoll event");
                    }
                }
                /* Connection is ready */
                if(events[i].events & EPOLLIN) {
                    cxn->wantsRead = 0;
                }
                if(events[i].events & EPOLLOUT) {
                    cxn->wantsWrite = 0;
                }
                if(cxn->stage == marla_CLIENT_COMPLETE && !cxn->shouldDestroy) {
                    //fprintf(stderr, "Attempting shutdown for connection\n");
                    // Client needs shutdown.
                    if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
                        cxn->shouldDestroy = 1;
                    }
                    if(cxn->shouldDestroy) {
                        //fprintf(stderr, "Connection should be destroyed\n");
                    }
                }
                else if(marla_clientAccept(cxn) == 0) {
                    if(events[i].events & EPOLLIN) {
                        cxn->wantsRead = 0;
                        // Available for read.
                        marla_clientRead(cxn);
                    }
                    if(events[i].events & EPOLLOUT) {
                        // Available for write.
                        cxn->wantsWrite = 0;
                        marla_clientWrite(cxn);
                    }

                    if(cxn->stage == marla_CLIENT_COMPLETE) {
                        marla_logMessage(&server, "Connection will be destroyed.");
                    }

                    if(cxn->stage != marla_CLIENT_COMPLETE && marla_Ring_size(cxn->output) > 0) {
                        int nflushed;
                        int rv = marla_Connection_flush(cxn, &nflushed);
                        if(rv <= 0) {
                            //fprintf(stderr, "Responder choked.\n");
                            return rv;
                        }
                    }
                }

                // Double-check if the shutdown needs to be run.
                if(cxn->stage == marla_CLIENT_COMPLETE) {
                    marla_logMessage(cxn->server, "Attempting shutdown for connection");
                    // Client needs shutdown.
                    if(!cxn->shutdownSource || 1 == cxn->shutdownSource(cxn)) {
                        cxn->shouldDestroy = 1;
                    }
                    if(cxn->shouldDestroy) {
                        marla_logMessage(cxn->server, "Connection should be destroyed");
                    }
                }
                if(cxn->shouldDestroy) {
                    marla_Connection_destroy(cxn);
                    marla_logLeave(&server, "Destroying connection.");
                }
                else {
                    char buf[marla_BUFSIZE];
                    memset(buf, 0, sizeof buf);
                    cxn->describeSource(cxn, buf, sizeof buf);
                    marla_logLeavef(&server, "Waiting for %s to become available.", buf);
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
    for(struct marla_ServerModule* serverModule = server.first_module; serverModule != 0; serverModule = serverModule->nextModule) {
        serverModule->moduleFunc(serverModule->moduleHandle, marla_EVENT_SERVER_MODULE_STOP);
    }

    if(events) {
        free(events);
    }
    if(use_ssl) {
        SSL_CTX_free(ctx);
        cleanup_openssl();
    }
    if(server.sfd > 0) {
        close(server.sfd);
    }
    if(server.logfd > 0) {
        close(server.logfd);
    }
    if(server.backend) {
        marla_Connection_destroy(server.backend);
    }
    if(use_curses && server.has_terminal) {
        void* retval;
        pthread_join(server.terminal_thread, &retval);
    }
    marla_Server_free(&server);
    apr_pool_terminate();
    return 0;
}
