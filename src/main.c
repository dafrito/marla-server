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
#include <apr_pools.h>
#include <dlfcn.h>
#include <apr_dso.h>
#include <sys/inotify.h>
#include <apr_file_info.h>

#define MAXEVENTS 64

static int use_curses = 1;
static int use_ssl = 1;
static char ssl_certificate_path[1024];
static char ssl_key_path[1024];

static int create_and_bind(const char *given_port)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, sfd;

    memset(&hints, 0, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC;     /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */
    hints.ai_flags = AI_PASSIVE;     /* All interfaces */

    char portbuf[512];
    strcpy(portbuf, given_port);
    const char* hostname = 0;
    const char* port = portbuf;
    char* sep = index(portbuf, ':');
    if(sep) {
        *sep = 0;
        hostname = portbuf;
        port = sep + 1;
    }

    s = getaddrinfo (hostname, port, &hints, &result);
    if(s != 0) {
        fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (s));
        return -1;
    }

    for(rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if(sfd == -1) {
            continue;
        }

        s = bind(sfd, rp->ai_addr, rp->ai_addrlen);
        if(s == 0) {
            /* We managed to bind successfully! */
            break;
        }
        else {
            fprintf(stderr, "Could not bind: %s (errno=%d)\n", strerror(errno), errno);
        }

        close(sfd);
    }

    if(rp == NULL) {
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

static void configure_context(SSL_CTX *ctx, const char* certfile, const char* keyfile)
{
    SSL_CTX_set_ecdh_auto(ctx, 1);

    /* Set the key and cert */
    if (SSL_CTX_use_certificate_file(ctx, certfile, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) <= 0 ) {
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
extern void* idle_operator(void* data);

struct marla_Server server;

void on_sigint()
{
    // Do nothing, but don't ignore it, so that it interrupts epoll_wait
    server.server_status = marla_SERVER_DESTROYING;
}

static void handle_exit()
{
}

void marla_processConnection(marla_Connection* cxn)
{

}

static void process_connection(struct epoll_event ep)
{
    //marla_logMessagef(&server, "%d", ep.events);
    if((ep.events & EPOLLERR) || (ep.events & EPOLLHUP) || (ep.events & EPOLLRDHUP) || (!(ep.events & EPOLLIN) && !(ep.events & EPOLLOUT))) {
        marla_Connection* cxn = (marla_Connection*)ep.data.ptr;
        // An error has occured on this fd, or the socket is not ready for reading (why were we notified then?)
        if(ep.events & EPOLLRDHUP) {
            if(cxn->is_backend) {
                marla_logMessagef(&server, "Backend connection done sending data.");
                if(cxn->requests_in_process == 0) {
                    marla_Connection_destroy(cxn);
                }
            }
            else if(cxn->requests_in_process == 0) {
                marla_Connection_destroy(cxn);
            }
            marla_logMessagef(&server, "Connection done sending data.");
            //fprintf(stderr, "Client connection %d done sending data.\n", cxn->id);
            return;
        }
        if(ep.events & EPOLLHUP) {
            if(cxn->requests_in_process == 0) {
                marla_Connection_destroy(cxn);
            }
            if(cxn->is_backend) {
                marla_logMessagef(&server, "Backend connection done accepting connections.");
            }
            else {
                //fprintf(stderr, "Client connection %d hung up.\n", cxn->id);
            }
            return;
        }
        if(cxn->requests_in_process == 0) {
            marla_Connection_destroy(cxn);
        }
        //fprintf(stderr, "epoll error: %d\n", ep.events);
        marla_logMessagef(&server, "Epoll error %d (EPOLLERR=%d, EPOLLHUP=%d)", ep.events, ep.events&EPOLLERR, ep.events&EPOLLHUP);
        return;
    }
    marla_Connection* cxn = (marla_Connection*)ep.data.ptr;
    fprintf(stderr, "Received epoll for %s connection %d\n", cxn->is_backend ? "backend" : "client", cxn->id);
    {
        char buf[marla_BUFSIZE];
        memset(buf, 0, sizeof buf);
        cxn->describeSource(cxn, buf, sizeof buf);

        if(ep.events & EPOLLOUT && ep.events & EPOLLIN) {
            marla_logEntercf(&server, "Processing", "Received client EPOLLIN and EPOLLOUT socket event on %s.", buf);
        }
        else if(ep.events & EPOLLIN) {
            marla_logEntercf(&server, "Processing", "Received client EPOLLIN socket event on %s.", buf);
        }
        else if(ep.events & EPOLLOUT) {
            marla_logEntercf(&server, "Processing", "Received client EPOLLOUT socket event on %s.", buf);
        }
        else {
            marla_die(&server, "Unexpected epoll event");
        }
    }
    /* Connection is ready */
    if(ep.events & EPOLLIN) {
        cxn->wantsRead = 0;
    }
    if(ep.events & EPOLLOUT) {
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
        //fprintf(stderr, "Processing connection %d\n", cxn->id);
        if(ep.events & EPOLLIN) {
            cxn->wantsRead = 0;
            // Available for read.
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
                        marla_WriteResult wr = marla_Connection_flush(cxn, &nflushed);
                        switch(wr) {
                        case marla_WriteResult_UPSTREAM_CHOKED:
                            continue;
                        case marla_WriteResult_DOWNSTREAM_CHOKED:
                            //fprintf(stderr, "Responder choked.\n");
                            loop = 0;
                            continue;
                        case marla_WriteResult_CLOSED:
                            cxn->stage = marla_CLIENT_COMPLETE;
                            loop = 0;
                            continue;
                        default:
                            marla_die(cxn->server, "Unexpected flush result");
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
        }
        if(ep.events & EPOLLOUT) {
            // Available for write.
            cxn->wantsWrite = 0;
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
                    while(loop && cxn->stage != marla_CLIENT_COMPLETE && marla_Ring_size(cxn->output) > 0) {
                        int nflushed;
                        marla_WriteResult wr = marla_Connection_flush(cxn, &nflushed);
                        switch(wr) {
                        case marla_WriteResult_UPSTREAM_CHOKED:
                            continue;
                        case marla_WriteResult_DOWNSTREAM_CHOKED:
                            //fprintf(stderr, "Responder choked.\n");
                            loop = 0;
                            continue;
                        case marla_WriteResult_CLOSED:
                            cxn->stage = marla_CLIENT_COMPLETE;
                            loop = 0;
                            continue;
                        default:
                            marla_die(cxn->server, "Unexpected flush result");
                        }
                    }
                    loop = 0;
                    continue;
                default:
                    //fprintf(stderr, "Connection %d's write returned %s\n", cxn->id, marla_nameWriteResult(wr));
                    loop = 0;
                    continue;
                }
            }
        }

        if(cxn->stage == marla_CLIENT_COMPLETE) {
            marla_logMessage(&server, "Connection will be destroyed.");
        }

        while(cxn->stage != marla_CLIENT_COMPLETE && !cxn->shouldDestroy && marla_Ring_size(cxn->output) > 0) {
            int nflushed;
            marla_WriteResult wr = marla_Connection_flush(cxn, &nflushed);
            switch(wr) {
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                //fprintf(stderr, "Responder choked.\n");
                return;
            case marla_WriteResult_UPSTREAM_CHOKED:
                continue;
            case marla_WriteResult_CLOSED:
                break;
            default:
                marla_die(cxn->server, "Unexpected flush result");
            }
        }
    }

    // Double-check if the shutdown needs to be run.
    if(cxn->stage == marla_CLIENT_COMPLETE && !cxn->shouldDestroy) {
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
        if(cxn->is_backend) {
            marla_Backend_recover(cxn);
        }
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

int main(int argc, const char**argv)
{
    atexit(handle_exit);
    int s;
    struct epoll_event *events = 0;

    const size_t MIN_ARGS = 4;

    apr_initialize();

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
    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    server.server_status = marla_SERVER_STARTED;

    // Create epoll queue.
    server.efd = epoll_create1(0);
    if(server.efd == -1) {
        perror("Creating main epoll queue for server");
        marla_logLeave(&server, "Failed to create epoll queue.");
        exit(EXIT_FAILURE);
    }

    // Create the file cache's inotify instance.
    server.fileCacheifd = inotify_init1(O_NONBLOCK);
    {
        // Add the inotifiy instance to epoll.
        struct epoll_event ev;
        ev.data.fd = server.fileCacheifd;
        ev.events = EPOLLIN | EPOLLET;
        if(0 != epoll_ctl(server.efd, EPOLL_CTL_ADD, server.fileCacheifd, &ev)) {
            perror("epoll_ctl");
            marla_logLeave(&server, "Failed to create inotify queue for file cache.");
            exit(EXIT_FAILURE);
        }
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

    strcpy(ssl_key_path, "key.pem");
    strcpy(ssl_certificate_path, "certificate.pem");

    uid_t uid = geteuid();
    if(uid != 0) {
        char dbPathBuf[PATH_MAX];
        char* homeDir = getenv("HOME");
        strcpy(dbPathBuf, homeDir);
        ssize_t len = strlen(dbPathBuf);
        char c = dbPathBuf[len - 1];
        if(c != '\\') {
            // HOME is not terminated with a /.
            strcat(dbPathBuf, "/var/parsegraph/users.sqlite");
        }
        else {
            // HOME is terminated with a /.
            strcat(dbPathBuf, "var/parsegraph/users.sqlite");
        }
        strcpy(server.db_path, dbPathBuf);
    }
    else {
        strcpy(server.db_path, "/var/parsegraph/users.sqlite");
    }
    strcpy(server.documentRoot, getenv("PWD"));
    strcpy(server.dataRoot, getenv("PWD"));

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
            if(n < argc - 1) {
                if(!strcmp(arg, "-key")) {
                    strncpy(ssl_key_path, argv[n+1], sizeof ssl_key_path);
                    ++n;
                    continue;
                }
                if(!strcmp(arg, "-cert")) {
                    strncpy(ssl_certificate_path, argv[n+1], sizeof ssl_certificate_path);
                    ++n;
                    continue;
                }
                if(!strcmp(arg, "-db")) {
                    strncpy(server.db_path, argv[n+1], sizeof server.db_path);
                    ++n;
                    continue;
                }
                if(!strcmp(arg, "-doc")) {
                    strncpy(server.documentRoot, argv[n+1], sizeof server.documentRoot);
                    ++n;
                    continue;
                }
                if(!strcmp(arg, "-data")) {
                    strncpy(server.dataRoot, argv[n+1], sizeof server.dataRoot);
                    ++n;
                    continue;
                }
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
            serverModule->prevModule = 0;
            if(server.last_module) {
                server.last_module->nextModule = serverModule;
                serverModule->prevModule = server.last_module;
                server.last_module = serverModule;
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
        configure_context(ctx, ssl_certificate_path, ssl_key_path);
        server.using_ssl = 1;
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

    if(0 != pthread_create(&server.idle_thread, 0, idle_operator, &server)) {
        fprintf(stderr, "Failed to create idle thread");
        marla_logLeave(&server, "Failed to create idle thread");
        exit(EXIT_FAILURE);
    }

    for(;;) {
        int n, i;

        if(server.server_status == marla_SERVER_DESTROYING) {
            break;
        }
        server.server_status = marla_SERVER_WAITING_FOR_INPUT;
        if(0 != pthread_mutex_unlock(&server.server_mutex)) {
            fprintf(stderr, "Failed to release server mutex\n");
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
            fprintf(stderr, "Failed to acquire server mutex\n");
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
            // Process one epoll event.
            if(events[i].data.fd == server.fileCacheifd) {
                // epoll event is from the file cache inotify descriptor.
                char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
                struct inotify_event* ev = 0;
                char* filepath = 0;
                for(int loop = 1; loop;) {
                    // Attempt to read the next event.
                    ev = (struct inotify_event*)buf;
                    ssize_t nread = read(server.fileCacheifd, buf, sizeof buf);
                    if(nread < 0) {
                        // Done reading events.
                        loop = 0;
                        continue;
                    }

                    if(ev->len > 0) {
                        // Read the filepath.
                        filepath = buf + sizeof(struct inotify_event);
                        fprintf(stderr, "INOTIFY %ld on %s!!!!\n", nread, filepath);
                    }
                    else {
                        filepath = 0;
                        fprintf(stderr, "INOTIFY %ld!!!!\n", nread);
                    }

                    // Process one event.
                    const char* pathname = apr_hash_get(server.wdToPathname, &ev->wd, sizeof(ev->wd));
                    if(!pathname) {
                        fprintf(stderr, "Failed to find pathanem for given watch descriptor %d\n", ev->wd);
                        abort();
                    }

                    char* pathbuf = NULL;
                    if(filepath) {
                        apr_filepath_merge(&pathbuf, pathname, filepath, APR_FILEPATH_TRUENAME | APR_FILEPATH_NOTABOVEROOT, server.pool);
                    }
                    else {
                        pathbuf = (char*)pathname;
                    }

                    marla_FileEntry* fe = apr_hash_get(server.fileCache, pathbuf, APR_HASH_KEY_STRING);
                    if(!fe) {
                        fprintf(stderr, "Nothing found for %s\n", pathbuf);
                        continue;
                    }
                    if(ev->mask & IN_IGNORED) {
                        apr_hash_set(server.wdToPathname, &fe->wd, sizeof(fe->wd), 0);
                        fe->wd = inotify_add_watch(
                            server.fileCacheifd, fe->watchpath, IN_DELETE_SELF | IN_MOVE_SELF | IN_MODIFY
                        );
                        apr_hash_set(server.wdToPathname, &fe->wd, sizeof(fe->wd), fe);
                        fprintf(stderr, "Re-adding watch %d\n", fe->wd);
                    }

                    if(access(fe->pathname, R_OK) != 0) {
                        // File was deleted.
                        continue;
                    }

                    // Reload the file.
                    marla_FileEntry_reload(fe);
                }

                // Done reading events.
                continue;
            }
            if(events[i].data.fd == server.logfd) {
                // epoll event is from the logging port.
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
                for(;;) {
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
                            marla_Connection_destroy(cxn);
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

                    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                    process_connection(event);
                }
                continue;
            }
            else {
                process_connection(events[i]);
            }
        }
    }

destroy:
    if(0 != pthread_mutex_unlock(&server.server_mutex)) {
        //fprintf(stderr, "Failed to release server mutex");
        exit_value = EXIT_FAILURE;
    }
destroy_without_unlock:
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
    if(use_curses && server.has_terminal) {
        void* retval;
        pthread_join(server.terminal_thread, &retval);
        server.has_terminal = 0;
    }
    {
        void* retval;
        pthread_join(server.idle_thread, &retval);
    }
    marla_Server_free(&server);
    apr_terminate();
    return 0;
}
