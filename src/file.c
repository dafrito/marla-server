#include "marla.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <apr_file_info.h>

marla_FileResponder* marla_FileResponder_new(struct marla_Server* server, marla_FileEntry* entry)
{
    marla_FileResponder* resp = malloc(sizeof(*resp));
    resp->server = server;
    resp->entry = entry;
    resp->pos = 0;
    resp->handleStage = marla_FileResponderStage_WRITING_HEADER;
    return resp;
}

void marla_FileResponder_free(marla_FileResponder* resp)
{
    free(resp);
}

static void invokeServerUpdater(marla_FileEntry* fe)
{
    if(fe->server->fileUpdated) {
        fe->server->fileUpdated(fe);
    }
}

marla_FileEntry* marla_Server_getFile(marla_Server* server, const char* pathname, const char* watchpath)
{
    // Get the entry.
    marla_FileEntry* fe = apr_hash_get(server->fileCache, pathname, APR_HASH_KEY_STRING);
    if(!fe) {
        // Create a file entry.
        fe = marla_FileEntry_new(server, pathname, watchpath);

        if(fe->wd != -1) {
            apr_hash_set(server->fileCache, fe->pathname, APR_HASH_KEY_STRING, fe);
            apr_hash_set(server->wdToPathname, &fe->wd, sizeof(fe->wd), fe->watchpath);
        }
    }

    fe->callback = invokeServerUpdater;

    // Return the entry.
    return fe;
}

marla_FileEntry* marla_FileEntry_new(marla_Server* server, const char* pathname, const char* watchpath)
{
    struct stat sb;

    // Create the entry struct.
    marla_FileEntry* fileEntry = malloc(sizeof(*fileEntry));
    fileEntry->watchpath = strdup(watchpath);
    fileEntry->pathname = strdup(pathname);
    fileEntry->server = server;
    fileEntry->type = "application/octet-stream";
    fileEntry->callback = 0;
    fileEntry->callbackData = 0;

    // Open the file.
    fileEntry->fd = open(pathname, O_RDONLY);
    if(fileEntry->fd == -1) {
        perror("open");
        abort();
    }

    // Retrieve the file's modification info.
    if(fstat(fileEntry->fd, &sb) == -1) {
        perror("fstat");
        abort();
    }

    // Save the file's modification time.
    fileEntry->modtime = sb.st_mtim;

    // Watch the file's directory for notifications.
    if(server->fileCacheifd > 0) {
        fprintf(stderr, "Added watch for %s\n", watchpath);
        fileEntry->wd = inotify_add_watch(server->fileCacheifd, fileEntry->watchpath, IN_MODIFY);
        if(fileEntry->wd == -1) {
            perror("inotify_add_watch");
            abort();
        }
    }
    else {
        fileEntry->wd = -1;
    }

    // Retrieve the data from the file.
    fileEntry->length = sb.st_size;
    if(fileEntry->length > 0) {
        fileEntry->data = malloc(sb.st_size + 1);
        fileEntry->data[sb.st_size] = 0;
        size_t remaining = sb.st_size;
        size_t index = 0;
        while(remaining > 0) {
            int nread = read(fileEntry->fd, fileEntry->data + index, remaining);
            if(nread <= 0) {
                perror("read");
                abort();
            }
            remaining -= nread;
            index += nread;
        }
    }
    else {
        fileEntry->data = 0;
    }

    // Close the file.
    close(fileEntry->fd);
    fileEntry->fd = -1;

    char* sep = rindex(fileEntry->pathname, '.');
    if(!sep) {
        fprintf(stderr, "Path given has no extension");
        abort();
    }

    // Set Content-Type for known built-in types.
    if(!strcmp(sep, ".webm")) {
        fileEntry->type = "video/webm";
    }
    else if(!strcmp(sep, ".png")) {
        fileEntry->type = "image/png";
    }
    else if(!strcmp(sep, ".jpg")) {
        fileEntry->type = "image/jpeg";
    }
    else if(!strcmp(sep, ".jpeg")) {
        fileEntry->type = "image/jpeg";
    }
    else if(!strcmp(sep, ".htm")) {
        fileEntry->type = "text/html";
    }
    else if(!strcmp(sep, ".html")) {
        fileEntry->type = "text/html";
    }
    else if(!strcmp(sep, ".css")) {
        fileEntry->type = "text/css";
    }
    else if(!strcmp(sep, ".csv")) {
        fileEntry->type = "text/csv";
    }
    else if(!strcmp(sep, ".gif")) {
        fileEntry->type = "text/gif";
    }
    else if(!strcmp(sep, ".txt")) {
        fileEntry->type = "text/plain";
    }
    else if(!strcmp(sep, ".js")) {
        fileEntry->type = "application/javascript";
    }
    else if(!strcmp(sep, ".json")) {
        fileEntry->type = "application/json";
    }
    else if(!strcmp(sep, ".midi")) {
        fileEntry->type = "audio/midi";
    }
    else if(!strcmp(sep, ".mpeg")) {
        fileEntry->type = "video/mpeg";
    }
    else if(!strcmp(sep, ".pdf")) {
        fileEntry->type = "application/pdf";
    }
    else if(!strcmp(sep, ".svg")) {
        fileEntry->type = "application/svg+xml";
    }
    else if(!strcmp(sep, ".xml")) {
        fileEntry->type = "application/xml";
    }
    else if(!strcmp(sep, ".wav")) {
        fileEntry->type = "audio/x-wav";
    }
    else if(!strcmp(sep, ".wav")) {
        fileEntry->type = "audio/x-wav";
    }
    else if(!strcmp(sep, ".avi")) {
        fileEntry->type = "video/x-msvideo";
    }

    // Return the new entry.
    return fileEntry;
}

void marla_FileEntry_reload(marla_FileEntry* fileEntry)
{
    struct stat sb;

    if(fileEntry->data) {
        free(fileEntry->data);
        fileEntry->data = 0;
    }

    // Re-open the file.
    fileEntry->fd = open(fileEntry->pathname, O_RDONLY);
    if(fileEntry->fd == -1) {
        perror("open");
        abort();
    }

    // Retrieve the file's modification info.
    if(fstat(fileEntry->fd, &sb) == -1) {
        perror("fstat");
        abort();
    }

    // Save the file's modification time.
    fileEntry->modtime = sb.st_mtim;

    // Retrieve the data from the file.
    fileEntry->length = sb.st_size;
    if(fileEntry->length > 0) {
        fileEntry->data = malloc(sb.st_size + 1);
        fileEntry->data[sb.st_size] = 0;
        size_t remaining = sb.st_size;
        size_t index = 0;
        while(remaining > 0) {
            int nread = read(fileEntry->fd, fileEntry->data + index, remaining);
            if(nread <= 0) {
                perror("read");
                abort();
            }
            remaining -= nread;
            index += nread;
        }
    }
    else {
        fileEntry->data = 0;
    }

    // Close the file.
    close(fileEntry->fd);
    fileEntry->fd = -1;
    fprintf(stderr, "Reloaded %s\n", fileEntry->pathname);
    if(fileEntry->callback) {
        fileEntry->callback(fileEntry);
    }
}

void marla_FileEntry_free(marla_FileEntry* fileEntry)
{
    if(fileEntry->data) {
        free(fileEntry->data);
        fileEntry->data = 0;
    }

    if(fileEntry->wd != -1) {
        // End the file's watch.
        inotify_rm_watch(fileEntry->server->fileCacheifd, fileEntry->wd);
    }

    // Free the memory.
    free(fileEntry->pathname);
    free(fileEntry->watchpath);
    free(fileEntry);
}

void marla_fileHandlerAcceptRequest(marla_Request* req)
{
    marla_Server* server = req->cxn->server;

    if(strstr(req->uri, "../")) {
        marla_killRequest(req, 400, "Path contains unsupported characters.");
        return;
    }
    if(req->uri[0] != '/') {
        marla_killRequest(req, 400, "Path given does not begin with a slash");
        return;
    }
    if(req->uri[1] == '/') {
        marla_killRequest(req, 400, "Path contains a redundant slash");
        return;
    }

    char *pathbuf = NULL;
    switch(apr_filepath_merge(&pathbuf, server->documentRoot, req->uri + 1, APR_FILEPATH_TRUENAME | APR_FILEPATH_NOTABOVEROOT, req->cxn->server->pool)) {
    case APR_EPATHWILD:
        marla_killRequest(req, 400, "Path contains unsupported characters.");
        return;
    case APR_SUCCESS:
        // pathbuf contains the merged path.
        break;
    }

    if(pathbuf != strstr(pathbuf, server->documentRoot)) {
        marla_killRequest(req, 400, "Path contains unsupported characters.");
        return;
    }

    // Path is accepted.
    // Open the file.
    if(access(pathbuf, F_OK) != -1) {
        // file exists
    } else {
        marla_killRequest(req, 404, "File not found.");
        return;
    }

    char* sep = rindex(req->uri, '.');
    if(!sep) {
        marla_killRequest(req, 400, "Path given has no extension");
        return;
    }

    marla_FileEntry* fe = marla_Server_getFile(server, pathbuf, server->documentRoot);
    if(!fe) {
        fprintf(stderr, "The handlerData must be set for request %d\n.", req->id);
        abort();
    }
    req->handlerData = marla_FileResponder_new(server, fe);
}

void marla_fileHandlerRequestBody(marla_Request* req, marla_WriteEvent* we)
{
    if(we->length == 0) {
        req->readStage = marla_CLIENT_REQUEST_DONE_READING;
        return;
    }
    marla_killRequest(req, 400, "Bad request");
}

marla_WriteResult marla_writeFileHandlerResponse(marla_Request* req, marla_WriteEvent* we)
{
    marla_Server* server = req->cxn->server;
    marla_FileResponder* resp = req->handlerData;
    if(!resp) {
        fprintf(stderr, "The handlerData must be set for request %d\n.", req->id);
        abort();
    }

    if(resp->handleStage == marla_FileResponderStage_WRITING_HEADER) {
        marla_logMessagef(req->cxn->server, "Sending headers for %d-byte response to client", resp->entry->length, resp->pos);
        char buf[marla_BUFSIZE];
        int responseLen = snprintf(buf, sizeof buf, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n%s\r\n",
            resp->entry->type,
            resp->entry->length,
            req->close_after_done ? "Connection: close\r\n" : ""
        );
        int true_written = marla_Connection_write(req->cxn, buf, responseLen);
        if(true_written < responseLen) {
            if(true_written > 0) {
                marla_Connection_putbackWrite(req->cxn, true_written);
            }
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        resp->handleStage = marla_FileResponderStage_BODY;
    }

    while(resp->handleStage == marla_FileResponderStage_BODY) {
        int nwritten = marla_Connection_write(req->cxn, resp->entry->data + resp->pos, resp->entry->length - resp->pos);
        if(nwritten <= 0) {
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        resp->pos += nwritten;
        if(resp->pos == resp->entry->length) {
            resp->handleStage = marla_FileResponderStage_FLUSHING;
        }
    }

    while(resp->handleStage == marla_FileResponderStage_FLUSHING) {
        marla_logMessagef(server, "Flushing %d bytes of input data.", marla_Ring_size(req->cxn->output));
        while(!marla_Ring_isEmpty(req->cxn->output)) {
            int nflushed;
            marla_WriteResult wr = marla_Connection_flush(req->cxn, &nflushed);
            switch(wr) {
            case marla_WriteResult_CLOSED:
                return marla_WriteResult_CLOSED;
            case marla_WriteResult_UPSTREAM_CHOKED:
                continue;
            case marla_WriteResult_DOWNSTREAM_CHOKED:
                marla_logMessage(server, "Choked while flushing client response");
                return marla_WriteResult_DOWNSTREAM_CHOKED;
            default:
                marla_die(server, "Unhandled flush result");
            }
        }
        ++resp->handleStage;
    }

    if(resp->handleStage == marla_FileResponderStage_DONE) {
        marla_logMessagef(server, "Processed all input data.");
        req->writeStage = marla_CLIENT_REQUEST_AFTER_RESPONSE;
        return marla_WriteResult_CONTINUE;
    }

    marla_die(server, "Invalid handleStage=%d", resp->handleStage);
    return marla_WriteResult_KILLED;
}

void marla_fileHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* in, int given_len)
{
    marla_Server* server = req->cxn->server;
    marla_logEntercf(server, "Handling", "Handling client event for file-based request: %s\n", marla_nameClientEvent(ev));
    marla_WriteEvent* we;
    switch(ev) {
    case marla_EVENT_HEADER:
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_ACCEPTING_REQUEST:
        // Accept the request.
        marla_fileHandlerAcceptRequest(req);
        (*(int*)in) = 1;
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_REQUEST_BODY:
        marla_fileHandlerRequestBody(req, in);
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_MUST_WRITE:
        we = in;
        we->status = marla_writeFileHandlerResponse(req, we);
        marla_logLeave(server, 0);
        return;
    case marla_EVENT_DESTROYING:
        marla_logLeave(server, 0);
        return;
    default:
        marla_logLeave(server, "Defaulted");
        return;
    }

    marla_die(server, "Unreachable");
}
