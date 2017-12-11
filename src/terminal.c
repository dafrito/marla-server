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

extern int parsegraph_ClientRequest_NEXT_ID;

void* terminal_operator(void* data)
{
    struct parsegraph_Server* server = (struct parsegraph_Server*)data;
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    nonl();

    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);

    char buf[1024];
    int count = 0;

    nodelay(stdscr, TRUE);
    for(;;) {
        for(int c = getch(); c != ERR; c = getch()) {
            // Process a character of input.
            ++count;
        }

        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 1e9/24;
        if(0 == pthread_mutex_timedlock(&server->server_mutex, &req)) {
            // No more input, draw screen.
            clear();

            int len = snprintf(buf, sizeof buf, "%d request%s served", (parsegraph_ClientRequest_NEXT_ID-1), parsegraph_ClientRequest_NEXT_ID == 2 ? "" : "s");
            addnstr(buf, len);
            refresh();

            if(0 != pthread_mutex_unlock(&server->server_mutex)) {
                perror("unlock");
                abort();
            }
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

    keypad(stdscr, FALSE);
    intrflush(stdscr, TRUE);
    nl();
    nocbreak();
    echo();
    endwin();
}
