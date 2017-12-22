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

static char status_line[255];

static const char* help_lines[] = {
    "F1 Main Menu",
    "F2 Connections",
    "F3 Pages",
    "F4 Statistics",
    "F5 Log",
    "F8 Exit",
};

enum TerminalPageMode {
TerminalPageMode_Menu,
TerminalPageMode_Connections,
TerminalPageMode_Pages,
TerminalPageMode_Statistics,
TerminalPageMode_Log
};

void* terminal_operator(void* data)
{
    signal(SIGWINCH, SIG_IGN);

    struct parsegraph_Server* server = (struct parsegraph_Server*)data;
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

    int end_terminal = 0;

    enum TerminalPageMode mode = TerminalPageMode_Menu;
    nodelay(stdscr, TRUE);
    for(;;) {
        for(int c = getch(); c != ERR; c = getch()) {
            // Process a character of input.
            switch(c) {
            case KEY_F(8):
                //if(server->server_status == parsegraph_SERVER_DESTROYING) {
                    end_terminal = 1;
                //}
                server->server_status = parsegraph_SERVER_DESTROYING;
                kill(0, SIGUSR1);
                break;
            case KEY_LEFT:
                if(mode > 0) {
                    --mode;
                }
                break;
            case KEY_RIGHT:
                if(mode < 4) {
                    ++mode;
                }
                break;
            case KEY_F(1):
                memset(status_line, 0, sizeof(status_line));
                mode = TerminalPageMode_Menu;
                break;
            case KEY_F(2):
                memset(status_line, 0, sizeof(status_line));
                strcpy(status_line, "F2 Connections");
                mode = TerminalPageMode_Connections;
                break;
            case KEY_F(3):
                memset(status_line, 0, sizeof(status_line));
                strcpy(status_line, "F3 Pages");
                mode = TerminalPageMode_Pages;
                break;
            case KEY_F(4):
                memset(status_line, 0, sizeof(status_line));
                strcpy(status_line, "F4 Statistics");
                mode = TerminalPageMode_Statistics;
                break;
            case KEY_F(5):
                memset(status_line, 0, sizeof(status_line));
                strcpy(status_line, "F5 Log");
                mode = TerminalPageMode_Log;
                break;
            }
            ++count;
        }

        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 1e9/24;
        if(0 == pthread_mutex_timedlock(&server->server_mutex, &req)) {
            // No more input, draw screen.
            clear();
            int len;
            int y = 0;
            int WINY, WINX;
            getmaxyx(stdscr, WINY, WINX);

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
            len = snprintf(buf, sizeof buf, "Marla: PID %d, server port %s %s, backend port %s", curpid, server->serverport, parsegraph_nameServerStatus(server->server_status), server->backendport);
            addnstr(buf, len);

            // Print status line
            move(++y, 0);
            for(int i = 0; i < 6; ++i) {
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
                len = snprintf(buf, sizeof buf, "%d request%s served", (parsegraph_ClientRequest_NEXT_ID-1), parsegraph_ClientRequest_NEXT_ID == 2 ? "" : "s");
                addnstr(buf, len);
            }
            else if(mode == TerminalPageMode_Connections) {
                struct parsegraph_Connection* cxn = server->first_connection;

                char sourceStr[64];
                while(cxn) {
                    cxn->describeSource(cxn, sourceStr, sizeof(sourceStr));
                    move(++y, 0);
                    if(cxn->requests_in_process > 1) {
                        len = snprintf(buf, sizeof buf, "%s | %4d request | input %4d ri %4d wi %4d cap | output %4d ri %4d wi %4d cap | %s | %s | %s",
                            parsegraph_nameConnectionStage(cxn->stage),
                            cxn->requests_in_process,
                            cxn->input->write_index & (cxn->input->capacity-1),
                            cxn->input->read_index & (cxn->input->capacity-1),
                            cxn->input->capacity,
                            cxn->output->write_index & (cxn->output->capacity-1),
                            cxn->output->read_index & (cxn->output->capacity-1),
                            cxn->output->capacity,
                            sourceStr,
                            (cxn->current_request ? parsegraph_nameRequestStage(cxn->current_request->stage) : ""),
                            (cxn->latest_request ? parsegraph_nameRequestStage(cxn->latest_request->stage) : "")
                        );
                    }
                    else if(cxn->requests_in_process == 1) {
                        len = snprintf(buf, sizeof buf, "%s | %4d request | input %4d ri %4d wi %4d cap | output %4d ri %4d wi %4d cap | %s | %s",
                            parsegraph_nameConnectionStage(cxn->stage),
                            cxn->requests_in_process,
                            cxn->input->write_index & (cxn->input->capacity-1),
                            cxn->input->read_index & (cxn->input->capacity-1),
                            cxn->input->capacity,
                            cxn->output->write_index & (cxn->output->capacity-1),
                            cxn->output->read_index & (cxn->output->capacity-1),
                            cxn->output->capacity,
                            sourceStr,
                            (cxn->current_request ? parsegraph_nameRequestStage(cxn->current_request->stage) : "")
                        );
                    }
                    else {
                        len = snprintf(buf, sizeof buf, "%s | %4d request | input %4d ri %4d wi %4d cap | output %4d ri %4d wi %4d cap | %s",
                            parsegraph_nameConnectionStage(cxn->stage),
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
                    cxn = cxn->next_connection;
                }
            }

            refresh();

            if(0 != pthread_mutex_unlock(&server->server_mutex)) {
                perror("unlock");
                abort();
            }
        }
        if(end_terminal && server->server_status == parsegraph_SERVER_DESTROYING) {
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
}
