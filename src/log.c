#include "marla.h"
#include <time.h>
#include <unistd.h>

void marla_logAction(marla_Server* server, const char* scope, const char* category, const char* message)
{
    if(!message) {
        message = "";
    }
    struct timespec now;
    if(0 != clock_gettime(CLOCK_MONOTONIC, &now)) {
        fprintf(stderr, "Failed to get time for logging\n");
        abort();
    }
    char buf[marla_BUFSIZE];
    int nwritten;
    if(!scope || *scope == 0) {
        if(!category || *category == 0) {
            nwritten = snprintf(buf, sizeof buf, "%ld %s\n", now.tv_sec, message);
        }
        else {
            nwritten = snprintf(buf, sizeof buf, "%ld (%s) %s\n", now.tv_sec, category, message);
        }
    }
    else {
        if(!category || *category == 0) {
            nwritten = snprintf(buf, sizeof buf, "%s %ld %s\n", scope, now.tv_sec, message);
        }
        else {
            nwritten = snprintf(buf, sizeof buf, "%s %ld (%s) %s\n", scope, now.tv_sec, category, message);
        }
    }
    if(nwritten < 0) {
        perror("marla_logAction");
        abort();
    }
    if(nwritten >= sizeof buf) {
        buf[1019] = '.';
        buf[1020] = '.';
        buf[1021] = '.';
        buf[1022] = '\n';
        buf[1023] = 0;
    }

    if(!server->logfd) {
        //write(2, buf, nwritten);
        return;
    }
    int true_written = marla_Ring_write(server->log, buf, nwritten);
    if(true_written != nwritten) {
        fprintf(stderr, "Logging overflow\n");
        abort();
    }
    marla_Server_flushLog(server);
}

void marla_logEnter(marla_Server* server, const char* message)
{
    marla_logEnterc(server, "", message);
}

void marla_logEnterc(marla_Server* server, const char* category, const char* message)
{
    marla_logAction(server, ">", category, message);
}

void marla_logEntercf(marla_Server* server, const char* category, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[marla_LOGBUFSIZE];
    int true_written = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if(true_written >= sizeof buf) {
        buf[marla_LOGBUFSIZE - 5] = '.';
        buf[marla_LOGBUFSIZE - 3] = '.';
        buf[marla_LOGBUFSIZE - 2] = '.';
        buf[marla_LOGBUFSIZE - 1] = '\n';
        buf[marla_LOGBUFSIZE - 0] = 0;
    }
    marla_logEnterc(server, category, buf);
}

void marla_logLeave(marla_Server* server, const char* message)
{
    marla_logLeavec(server, "", message);
}

void marla_logLeavec(marla_Server* server, const char* category, const char* message)
{
    marla_logAction(server, "<", category, message);
}

void marla_logReset(marla_Server* server, const char* message)
{
    marla_logResetc(server, "", message);
}

void marla_logResetc(marla_Server* server, const char* category, const char* message)
{
    marla_logAction(server, "!", category, message);
}

void marla_logMessage(marla_Server* server, const char* message)
{
    marla_logMessagec(server, "", message);
}

void marla_logMessagec(marla_Server* server, const char* category, const char* message)
{
    marla_logAction(server, "", category, message);
}

void marla_logMessagecf(marla_Server* server, const char* category, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[marla_LOGBUFSIZE];
    int true_written = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if(true_written >= sizeof buf) {
        buf[marla_LOGBUFSIZE - 5] = '.';
        buf[marla_LOGBUFSIZE - 3] = '.';
        buf[marla_LOGBUFSIZE - 2] = '.';
        buf[marla_LOGBUFSIZE - 1] = '\n';
        buf[marla_LOGBUFSIZE - 0] = 0;
    }
    marla_logAction(server, "", category, buf);
}

void marla_logLeavef(marla_Server* server, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[marla_LOGBUFSIZE];
    int true_written = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if(true_written >= sizeof buf) {
        buf[marla_LOGBUFSIZE - 5] = '.';
        buf[marla_LOGBUFSIZE - 3] = '.';
        buf[marla_LOGBUFSIZE - 2] = '.';
        buf[marla_LOGBUFSIZE - 1] = '\n';
        buf[marla_LOGBUFSIZE - 0] = 0;
    }
    marla_logLeave(server, buf);
}

void marla_logEnterf(marla_Server* server, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[marla_LOGBUFSIZE];
    int true_written = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if(true_written >= sizeof buf) {
        buf[marla_LOGBUFSIZE - 5] = '.';
        buf[marla_LOGBUFSIZE - 3] = '.';
        buf[marla_LOGBUFSIZE - 2] = '.';
        buf[marla_LOGBUFSIZE - 1] = '\n';
        buf[marla_LOGBUFSIZE - 0] = 0;
    }
    marla_logEnter(server, buf);
}

void marla_logMessagef(marla_Server* server, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[marla_LOGBUFSIZE];
    int true_written = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if(true_written >= sizeof buf) {
        buf[marla_LOGBUFSIZE - 5] = '.';
        buf[marla_LOGBUFSIZE - 3] = '.';
        buf[marla_LOGBUFSIZE - 2] = '.';
        buf[marla_LOGBUFSIZE - 1] = '\n';
        buf[marla_LOGBUFSIZE - 0] = 0;
    }
    marla_logAction(server, "", "", buf);
}
