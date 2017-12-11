#include "prepare.h"
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
#include <ncurses.h>
#include <locale.h>

#define MAXEVENTS 64

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

apr_pool_t* modpool = 0;
ap_dbd_t* controlDBD = 0;
ap_dbd_t* worldStreamDBD = 0;

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

static int pidFile;

static int
init_parsegraph_environment_ws()
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
        fprintf(stderr, "Failed initializing APR. APR status of %d.\n", rv);
        return -1;
    }

    // Create the process-wide pool.
    rv = apr_pool_create(&modpool, 0);
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
    pidFile = open("rainback.pid", O_WRONLY | O_TRUNC | O_CREAT, 0664);
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

    return 0;
}

static int
destroy_parsegraph_environment_ws()
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

extern const char* SERVERPORT;

static int
create_and_bind (const char *port)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int s, sfd;
  SERVERPORT = port;

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
   int sfd, s, j;
   size_t len;
   ssize_t nread;

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

static int exit_value = EXIT_SUCCESS;
extern void* terminal_operator(void* data);

int main(int argc, const char**argv)
{
    int s;
    struct epoll_event *events;
    struct parsegraph_Server server;

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
    signal(SIGPIPE, SIG_IGN);

    server.server_status = parsegraph_SERVER_STARTED;

    // Create the SSL context
    init_openssl();
    SSL_CTX *ctx = create_context();
    configure_context(ctx);

    // Initialize environment_ws
    if(0 != init_parsegraph_environment_ws()) {
        fprintf(stderr, "Failed to initialize environment_ws module");
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    /*// Create terminal interface thread.
    if(0 != pthread_create(&terminal_thread, 0, terminal_operator, server)) {
        fprintf(stderr, "Failed to create terminal thread");
        exit(EXIT_FAILURE);
    }*/

    // Create epoll queue.
    server.efd = epoll_create1(0);
    if(server.efd == -1) {
        perror("epoll_create");
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    // Create the server socket
    server.sfd = create_and_bind(argv[1]);
    if(server.sfd == -1) {
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }
    s = make_socket_non_blocking(server.sfd);
    if(s == -1) {
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }
    s = listen(server.sfd, SOMAXCONN);
    if(s == -1) {
        perror ("listen");
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }
    else {
        struct epoll_event event;
        event.data.fd = server.sfd;
        event.events = EPOLLIN | EPOLLET;
        s = epoll_ctl (server.efd, EPOLL_CTL_ADD, server.sfd, &event);
        if(s == -1) {
            perror("Adding server file descriptor to epoll queue");
            abort();
            exit_value = EXIT_FAILURE;
            goto end_terminal;
        }
    }

    // Create the backend socket
    server.backendfd = create_and_connect(argv[2]);
    if(server.backendfd == -1) {
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }
    else {
        s = make_socket_non_blocking(server.backendfd);
        if(s == -1) {
            exit_value = EXIT_FAILURE;
            goto end_terminal;
        }
        struct epoll_event event;
        event.data.fd = server.backendfd;
        event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        s = epoll_ctl (server.efd, EPOLL_CTL_ADD, server.backendfd, &event);
        if(s == -1) {
            perror("Adding backend file descriptor to epoll queue");
            abort();
            exit_value = EXIT_FAILURE;
            goto end_terminal;
        }
    }

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
                fprintf(stderr, "Failed to open module \"%s\"\nUsage: %s [port] [backend-port] modulepath?modulefunc...\n", modulename, argv[0]);
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
    while(1) {
        int n, i;

        if(0 != pthread_mutex_unlock(&server.server_mutex)) {
            fprintf(stderr, "Failed to release server mutex");
            exit_value = EXIT_FAILURE;
            goto end_terminal;
        }

      n = epoll_wait (server.efd, events, MAXEVENTS, -1);
      if(n < 0) {
        goto destroy;
      }
    if(0 != pthread_mutex_lock(&server.server_mutex)) {
        fprintf(stderr, "Failed to acquire server mutex");
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }
      for(i = 0; i < n; i++) {
        if (events[i].data.fd == server.backendfd) {
            if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
            }
            continue;
        }
        else if (server.sfd == events[i].data.fd) {
            // Event is from server socket.
            if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
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
                    printf("Accepted connection on descriptor %d "
                    "(host=%s, port=%s)\n", infd, hbuf, sbuf);
                }

                /* Make the incoming socket non-blocking and add it to the
                list of fds to monitor. */
                s = make_socket_non_blocking (infd);
                if(s != 0) {
                    close(infd);
                    continue;
                }

                parsegraph_Connection* cxn = parsegraph_Connection_new(ctx, infd);
                if(!cxn) {
                    perror("Unable to create connection");
                    close(infd);
                    continue;
                }
                s = parsegraph_SSL_init(cxn, ctx, infd);
                if(s <= 0) {
                    perror("Unable to initialize SSL connection");
                    close(infd);
                    continue;
                }

                  struct epoll_event event;
                  event.data.ptr = cxn;
                  event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                  s = epoll_ctl (server.efd, EPOLL_CTL_ADD, infd, &event);
                  if (s == -1)
                    {
                      perror ("epoll_ctl");
                      parsegraph_Connection_destroy(cxn);
                      close(infd);
                        continue;
                    }
                }
              continue;
            }
          else
            {
                if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
                    // An error has occured on this fd, or the socket is not ready for reading (why were we notified then?)
                    printf("epoll error: %d\n", events[i].events);
                    parsegraph_Connection* source = (parsegraph_Connection*)events[i].data.ptr;
                    parsegraph_Connection_destroy(source);
                    continue;
                }
                /* Connection is ready */
                parsegraph_Connection* cxn = (parsegraph_Connection*)events[i].data.ptr;
                parsegraph_Connection_handle(cxn, &server, events[i].events);
                if(cxn->shouldDestroy) {
                  parsegraph_Connection_destroy(cxn);
                }
            }
        }
    }

destroy:
    free(events);
    close(server.sfd);
    close(server.backendfd);
    SSL_CTX_free(ctx);
    cleanup_openssl();
    destroy_parsegraph_environment_ws();

end_terminal:
    if(0 != pthread_mutex_unlock(&server.server_mutex)) {
        fprintf(stderr, "Failed to release server mutex");
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }
    //pthread_exit(&terminal_thread);
    exit(exit_value);
}
