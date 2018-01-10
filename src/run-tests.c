#include "marla.h"
#include <stdio.h>
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

static const char* name = "marla";

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

    method = SSLv23_server_method();

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

static int create_and_bind(const char* host, const char *port)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC;     /* Return IPv4 and IPv6 choices */
    hints.ai_socktype = SOCK_STREAM; /* We want a TCP socket */
    hints.ai_flags = AI_PASSIVE;     /* All interfaces */

    struct addrinfo *result;
    int rv = getaddrinfo(host, port, &hints, &result);
    if(rv != 0) {
        perror("getaddrinfo");
        return -1;
    }

    int sfd;
    struct addrinfo* rp = NULL;
    for(rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if(sfd == -1) {
            continue;
        }

        if(connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;
        }

        close(sfd);
    }

    if(rp == NULL) {
        fprintf(stderr, "Could not connect\n");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(result);

    return sfd;
}

int main(int argc, const char** argv)
{
    if(argc < 3) {
        fprintf(stderr, "Usage: %s <host> <port>\n", name);
        exit(EXIT_FAILURE);
    }

    struct marla_Server server;
    marla_Server_init(&server);

    init_openssl();
    SSL_CTX *ctx = create_context();
    configure_context(ctx);

    int sfd = create_and_bind(argv[1], argv[2]);
    if(sfd == -1) {
        exit(EXIT_FAILURE);
    }

    int rv = make_socket_non_blocking(sfd);
    if(rv == -1) {
        exit(EXIT_FAILURE);
    }

    rv = listen(sfd, SOMAXCONN);
    if(rv == -1) {
        perror ("listen");
        abort ();
    }

    int efd = epoll_create1(0);
    if(efd == -1) {
        perror("epoll_create");
        abort();
    }

    struct epoll_event event;
    event.data.fd = sfd;
    event.events = EPOLLIN | EPOLLET;
    rv = epoll_ctl (efd, EPOLL_CTL_ADD, sfd, &event);
    if(rv == -1) {
        perror("epoll_ctl");
        abort();
    }

    struct epoll_event* events = (struct epoll_event*)calloc(MAXEVENTS, sizeof event);
    while(1) {
        int n, i;

      n = epoll_wait (efd, events, MAXEVENTS, -1);
      if(n < 0) {
        goto destroy;
      }
      for(i = 0; i < n; i++) {
        if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))) {
            // An error has occured on this fd, or the socket is not ready for reading (why were we notified then?)
            fprintf (stderr, "epoll error\n");
            marla_Connection* source = (marla_Connection*)events[i].data.ptr;
            marla_Connection_destroy(source);
            fsync(3);
            continue;
        }
        else if (sfd == events[i].data.fd) {
            // Event is from server socket.

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

                  rv = getnameinfo (&in_addr, in_len,
                                   hbuf, sizeof hbuf,
                                   sbuf, sizeof sbuf,
                                   NI_NUMERICHOST | NI_NUMERICSERV);
                  if (rv == 0)
                    {
                      printf("Accepted connection on descriptor %d "
                             "(host=%s, port=%s)\n", infd, hbuf, sbuf);
                    }

                  /* Make the incoming socket non-blocking and add it to the
                     list of fds to monitor. */
                  rv = make_socket_non_blocking (infd);
                  if (rv == -1) {
                    abort ();
                  }

                  marla_Connection* cxn = marla_Connection_new();
                  if(1 != marla_SSL_init(cxn, ctx, infd)) {
                     perror("Unable to create connection");
                     abort();
                  }

                  event.data.ptr = cxn;
                  event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                  rv = epoll_ctl (efd, EPOLL_CTL_ADD, infd, &event);
                  if (rv == -1)
                    {
                      perror ("epoll_ctl");
                      abort ();
                    }
                }
              continue;
            }
          else
            {
                /* Connection is ready */
                marla_Connection* cxn = (marla_Connection*)events[i].data.ptr;
                marla_Connection_handle(cxn, &server, events[i].events);
                if(cxn->shouldDestroy) {
                  marla_Connection_destroy(cxn);
                }
            }
        }
    }

destroy:
  free (events);
  close (sfd);
  SSL_CTX_free(ctx);
  cleanup_openssl();

  return EXIT_SUCCESS;
}
