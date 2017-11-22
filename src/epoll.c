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
        fprintf(stderr, "Could not connect\n");
        exit(EXIT_FAILURE);
    }

    printf("FOUND A BACKEND\n");
    freeaddrinfo(result);           /* No longer needed */

    return sfd;
}

void* terminal_operator(void* data)
{
    /*setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    nonl();

    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);

    int c = getch();

    keypad(stdscr, FALSE);
    intrflush(stdscr, TRUE);
    nl();
    nocbreak();
    echo();
    endwin();*/
}

static pthread_t terminal_thread;
static int exit_value = EXIT_SUCCESS;

int main(int argc, const char**argv)
{
    int sfd, backendfd, s;
    int efd;
    struct epoll_event *events;

    if(0 != pthread_create(&terminal_thread, 0, terminal_operator, 0)) {
        fprintf(stderr, "Failed to create terminal thread");
        exit(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN);

    init_openssl();
    SSL_CTX *ctx = create_context();
    configure_context(ctx);

    if(argc != 3) {
        fprintf(stderr, "Usage: %s [port] [backend-port]\n", argv[0]);
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    sfd = create_and_bind(argv[1]);
    if(sfd == -1) {
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    s = make_socket_non_blocking(sfd);
    if(s == -1) {
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    s = listen(sfd, SOMAXCONN);
    if(s == -1) {
        perror ("listen");
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    backendfd = create_and_connect(argv[2]);
    if(backendfd == -1) {
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    s = make_socket_non_blocking(backendfd);
    if(s == -1) {
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    efd = epoll_create1(0);
    if(efd == -1) {
        perror("epoll_create");
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    struct epoll_event event;
    event.data.fd = sfd;
    event.events = EPOLLIN | EPOLLET;
    s = epoll_ctl (efd, EPOLL_CTL_ADD, sfd, &event);
    if(s == -1) {
        perror("Adding server file descriptor to epoll queue");
        abort();
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    event.data.fd = backendfd;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET;
    s = epoll_ctl (efd, EPOLL_CTL_ADD, backendfd, &event);
    if(s == -1) {
        perror("Adding backend file descriptor to epoll queue");
        abort();
        exit_value = EXIT_FAILURE;
        goto end_terminal;
    }

    events = (struct epoll_event*)calloc(MAXEVENTS, sizeof event);
    while(1) {
        int n, i;

      n = epoll_wait (efd, events, MAXEVENTS, -1);
      if(n < 0) {
        goto destroy;
      }
      for(i = 0; i < n; i++) {
        if (events[i].data.fd == backendfd) {
            if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
                goto destroy;
            }
        }
        else if (sfd == events[i].data.fd) {
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
                infd = accept(sfd, &in_addr, &in_len);
                if(infd == -1) {
                    if((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                        // We have processed all incoming connections.
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

                  event.data.ptr = cxn;
                  event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                  s = epoll_ctl (efd, EPOLL_CTL_ADD, infd, &event);
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
                parsegraph_Connection_handle(cxn, events[i].events);
                if(cxn->shouldDestroy) {
                  parsegraph_Connection_destroy(cxn);
                }
            }
        }
    }

destroy:
    free(events);
    close(sfd);
    SSL_CTX_free(ctx);
    cleanup_openssl();

end_terminal:
    pthread_exit(&terminal_thread);
    return exit_value;
}
