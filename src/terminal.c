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
#include <ncurses.h>
#include <locale.h>

extern int marla_Request_NEXT_ID;
extern int marla_Request_numKilled;

static char status_line[255];

static const char* help_lines[] = {
    "F1 Statistics",
    "F2 Connections",
    "F4 Exit",
};

enum TerminalPageMode {
TerminalPageMode_Statistics,
TerminalPageMode_Connections
};

static enum TerminalPageMode firstPage = TerminalPageMode_Statistics;
static enum TerminalPageMode lastPage = TerminalPageMode_Connections;
static enum TerminalPageMode defaultPage = TerminalPageMode_Statistics;

static void display_connection(marla_Server* server, marla_Connection* cxn, int* y, int WINY)
{
    char buf[1024];
    int len;
    char sourceStr[64];
    if(*y >= WINY - 1) {
        return;
    }
    cxn->describeSource(cxn, sourceStr, sizeof(sourceStr));
    move(++(*y), 0);
    if(cxn->requests_in_process > 0) {
        len = snprintf(buf, sizeof buf, "%s | %4ld request | input %4ld ri %4ld wi %4ld cap | output %4ld ri %4ld wi %4ld cap | %s | %s | %s",
            marla_nameConnectionStage(cxn->stage),
            cxn->requests_in_process,
            cxn->input->write_index & (cxn->input->capacity-1),
            cxn->input->read_index & (cxn->input->capacity-1),
            cxn->input->capacity,
            cxn->output->write_index & (cxn->output->capacity-1),
            cxn->output->read_index & (cxn->output->capacity-1),
            cxn->output->capacity,
            sourceStr,
            (cxn->current_request ? marla_nameRequestWriteStage(cxn->current_request->writeStage) : ""),
            cxn->is_backend ?
                (cxn->current_request ? marla_nameRequestReadStage(cxn->current_request->readStage) : "") :
                (cxn->latest_request ? marla_nameRequestReadStage(cxn->latest_request->readStage) : "")
        );
    }
    else {
        len = snprintf(buf, sizeof buf, "%s | %4ld request | input %4ld ri %4ld wi %4ld cap | output %4ld ri %4ld wi %4ld cap | %s",
            marla_nameConnectionStage(cxn->stage),
            cxn->requests_in_process,
            cxn->input->write_index & (cxn->input->capacity-1),
            cxn->input->read_index & (cxn->input->capacity-1),
            cxn->input->capacity,
            cxn->output->write_index & (cxn->output->capacity-1),
            cxn->output->read_index & (cxn->output->capacity-1),
            cxn->output->capacity,
            sourceStr
        );
    }
    addnstr(buf, len);
}

static void display_connections(marla_Server* server, int y)
{
    int WINY, WINX;
    getmaxyx(stdscr, WINY, WINX);
    struct marla_Connection* cxn = server->first_connection;
    while(cxn) {
        display_connection(server, cxn, &y, WINY);
        cxn = cxn->next_connection;
    }
}

void* terminal_operator(void* data)
{
    signal(SIGWINCH, SIG_IGN);

    struct marla_Server* server = (struct marla_Server*)data;
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    nonl();
    memset(status_line, 0, sizeof(status_line));
    pid_t curpid = getpid();

    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);

    char buf[1024];
    int count = 0;

    enum TerminalPageMode mode = defaultPage;
    memset(status_line, 0, sizeof(status_line));
    strcpy(status_line, "F1 Statistics");
    nodelay(stdscr, TRUE);
    for(;;) {
        for(int c = getch(); c != ERR; c = getch()) {
            // Process a character of input.
            switch(c) {
            case KEY_F(4):
                keypad(stdscr, FALSE);
                intrflush(stdscr, TRUE);
                nl();
                nocbreak();
                echo();
                endwin();
                server->server_status = marla_SERVER_DESTROYING;
                kill(0, SIGUSR1);
                return 0;
            case KEY_LEFT:
                if(mode > firstPage) {
                    --mode;
                }
                break;
            case KEY_RIGHT:
                if(mode < lastPage) {
                    ++mode;
                }
                break;
            case KEY_F(1):
                memset(status_line, 0, sizeof(status_line));
                strcpy(status_line, "F1 Statistics");
                mode = TerminalPageMode_Statistics;
                break;
            case KEY_F(2):
                memset(status_line, 0, sizeof(status_line));
                strcpy(status_line, "F2 Connections");
                mode = TerminalPageMode_Connections;
                break;
            }
            ++count;
        }

        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 1e9/12;
        if(0 == pthread_mutex_timedlock(&server->server_mutex, &req)) {
            // No more input, draw screen.
            clear();
            int len;
            int y = 0;

            struct tm *tmp;
            time_t t = time(NULL);
            tmp = localtime(&t);
            if(tmp == NULL) {
               perror("localtime");
               exit(EXIT_FAILURE);
            }
            len = strftime(buf, sizeof(buf), "%F %T", tmp);
            if(len == 0) {
               fprintf(stderr, "strftime returned 0");
               exit(EXIT_FAILURE);
            }
            addnstr(buf, len);

            move(++y, 0);
            len = snprintf(buf, sizeof buf, "Marla: PID %d, server port %s %s, backend port %s, logging port %s", curpid, server->serverport, marla_nameServerStatus(server->server_status), server->backendport, server->logaddress);
            addnstr(buf, len);

            // Print status line
            move(++y, 0);
            for(int i = 0; i < 3; ++i) {
                if(mode == i) {
                    attron(A_REVERSE);
                }
                addstr(help_lines[i]);
                if(mode == i) {
                    attroff(A_REVERSE);
                }
                addch(' ');
            }

            if(mode == TerminalPageMode_Statistics) {
                move(++y, 0);
                len = snprintf(buf, sizeof buf, "%d request%s served", (marla_Request_NEXT_ID-1), marla_Request_NEXT_ID == 2 ? "" : "s");
                addnstr(buf, len);
                if(marla_Request_numKilled > 0) {
                    move(++y, 0);
                    len = snprintf(buf, sizeof buf, "%d request%s killed", marla_Request_numKilled, marla_Request_numKilled > 1 ? "s" : "");
                    addnstr(buf, len);
                }
                move(++y, 0);
                len = snprintf(buf, sizeof buf, "%ld bytes in log buffer", marla_Ring_size(server->log));
                addnstr(buf, len);
                move(++y, 0);
                len = snprintf(buf, sizeof buf, "sizeof(marla_Request): %ld bytes", sizeof(marla_Request));
                addnstr(buf, len);
                move(++y, 0);
                len = snprintf(buf, sizeof buf, "sizeof(marla_Connection): %ld bytes", sizeof(marla_Connection));
                addnstr(buf, len);
                move(++y, 0);
                len = snprintf(buf, sizeof buf, "marla_BUFSIZE: %d bytes", marla_BUFSIZE);
                addnstr(buf, len);
                move(++y, 0);
                len = snprintf(buf, sizeof buf, "marla_LOGBUFSIZE: %d bytes", marla_LOGBUFSIZE);
                addnstr(buf, len);
            }
            else if(mode == TerminalPageMode_Connections) {
                display_connections(server, y);
            }

            refresh();

            if(0 != pthread_mutex_unlock(&server->server_mutex)) {
                perror("unlock");
                abort();
            }
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

    keypad(stdscr, FALSE);
    intrflush(stdscr, TRUE);
    nl();
    nocbreak();
    echo();
    endwin();
    return 0;
}

void marla_die(marla_Server* server, const char* fmt, ...)
{
    server->server_status = marla_SERVER_DESTROYING;
    kill(0, SIGUSR1);
    if(server->has_terminal) {
        void* retval;
        pthread_join(server->terminal_thread, &retval);
        server->has_terminal = 0;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    write(2, "\n", 1);
    va_end(ap);
    abort();
}
