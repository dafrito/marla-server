#include "marla.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <unistd.h>

marla_FileResponder* marla_FileResponder_new(struct marla_Server* server, marla_FileEntry* entry)
{
    marla_FileResponder* resp = malloc(sizeof(*resp));
    resp->server = server;
    resp->entry = entry;
    return resp;
}

void marla_FileResponder_free(marla_FileResponder* resp)
{
    free(resp);
}

marla_FileEntry* marla_Server_getFile(marla_Server* server, const char* pathname)
{
    // Get the entry.
    marla_FileEntry* fe = apr_hash_get(server->fileCache, pathname, APR_HASH_KEY_STRING);
    if(!fe) {
        // Create a file entry.
        fe = marla_FileEntry_new(server, pathname);

        apr_hash_set(server->fileCache, fe->pathname, APR_HASH_KEY_STRING, fe);
        apr_hash_set(server->wdToFileEntry, &fe->wd, sizeof(fe->wd), fe);
    }

    // Return the entry.
    return fe;
}

marla_FileEntry* marla_Server_reloadFile(marla_Server* server, const char* pathname)
{
    // Get the entry.
    marla_FileEntry* fe = apr_hash_get(server->fileCache, pathname, APR_HASH_KEY_STRING);
    if(!fe) {
        // Create the entry initially.
        return marla_Server_getFile(server, pathname);
    }

    // Reload the entry in place.
    marla_FileEntry_reload(fe);

    // Return the entry.
    return fe;
}

marla_FileEntry* marla_FileEntry_new(marla_Server* server, const char* pathname)
{
    struct stat sb;

    // Create the entry struct.
    marla_FileEntry* fileEntry = malloc(sizeof(*fileEntry));
    fileEntry->pathname = strdup(pathname);
    fileEntry->server = server;

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

    // Watch the file for notifications.
    fileEntry->wd = inotify_add_watch(
        server->fileCacheifd, pathname, IN_DELETE_SELF | IN_MOVE_SELF | IN_MODIFY
    );
    if(fileEntry->wd == -1) {
        perror("inotify_add_watch");
        abort();
    }

    // Retrieve the memory-mapped data from the file.
    fileEntry->length = sb.st_size;
    fileEntry->data = mmap(0, fileEntry->length, PROT_READ, MAP_SHARED, fileEntry->fd, 0);

    // Return the new entry.
    return fileEntry;
}

void marla_FileEntry_reload(marla_FileEntry* fileEntry)
{
    struct stat sb;

    // Unmap memory.
    if(0 != munmap(fileEntry->data, fileEntry->length)) {
        perror("munmap");
        abort();
    }

    // Close the file.
    close(fileEntry->fd);

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

    // Retrieve the memory-mapped data from the file.
    fileEntry->length = sb.st_size;
    fileEntry->data = mmap(0, fileEntry->length, PROT_READ, MAP_SHARED, fileEntry->fd, 0);
}

void marla_FileEntry_free(marla_FileEntry* fileEntry)
{
    // Unmap memory.
    if(0 != munmap(fileEntry->data, fileEntry->length)) {
        perror("munmap");
        abort();
    }

    // Close the file.
    close(fileEntry->fd);

    // End the file's watch.
    inotify_rm_watch(fileEntry->server->fileCacheifd, fileEntry->wd);

    // Free the memory.
    free(fileEntry);
}
