#include "rainback.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/err.h>
#include <openssl/rand.h>

void parsegraph_measureChunk_new(size_t slotLen, int avail, size_t* prefix_len, size_t* availUsed)
{
    size_t suffix_len = 2;

    // |                                                                      | avail<=0xf    | avail>0xf && avail<=0xff | avail>0xff && avail<=0xfff | avail>=0x1000 |
    // +----------------------------------------------------------------------+---------------+--------------------------+----------------------------+---------------+
    // | slotLen - 3 - suffix_len <= 0xf                                      | use           | use                      | use                        | use           |
    // | slotLen - suffix_len - 3 > 0xf && slotLen - 4 - suffix_len <= 0xff   | demote        | use                      | use                        | use           |
    // | slotLen - 5 - suffix_len <= 0xfff && slotLen - 4 - suffix_len > 0xff | demote        | demote                   | use                        | use           |
    // | slotLen - 5 - suffix_len > 0xfff                                     | demote        | demote                   | demote                     | use           |

    for(;;) {
        if(slotLen - 3 - suffix_len < 0xf) {
            *prefix_len = 3;
            break;
        }
        if(slotLen - suffix_len - 3 >= 0xf && slotLen - 4 - suffix_len < 0xff) {
            if(avail <= 0xf) {
                // Demote
                slotLen = avail + 5;
                continue;
            }
            *prefix_len = 4;
        }
        if(slotLen - 5 - suffix_len < 0xfff && slotLen - 4 - suffix_len >= 0xff) {
            if(avail <= 0xff) {
                // Demote
                slotLen = avail + 6;
                continue;
            }
            *prefix_len = 5;
        }
        if(slotLen - 5 - suffix_len >= 0xfff) {
            if(avail <= 0xfff) {
                // Demote
                slotLen = avail + 7;
                continue;
            }
            *prefix_len = 6;
        }
        break;
    }
    *availUsed = slotLen - *prefix_len - suffix_len;
}

void parsegraph_measureChunk(size_t slotLen, int avail, size_t* prefix_len, size_t* availUsed)
{
    // Incorporate message padding into chunk.
    size_t suffix_len = 2;
    *prefix_len = 3; // One byte plus \r\n
    size_t padding = *prefix_len + suffix_len;

    if(slotLen - padding -1 > 0xf && avail > 0xf) {
        *prefix_len = 4; // Two bytes plus \r\n
        padding = *prefix_len + suffix_len;
        if(slotLen - padding -1 > 0xff && avail > 0xff) {
            *prefix_len = 5; // Three bytes plus \r\n
            padding = *prefix_len + suffix_len;
            if(slotLen - padding -1 > 0xfff && avail > 0xfff) {
                *prefix_len = 7; // Four bytes plus \r\n
                padding = *prefix_len + suffix_len;
            }
        }
    }
    if(avail + padding < slotLen) {
        slotLen = avail + padding;
    }

    if(slotLen - padding > 0xff && *prefix_len == 4) {
        slotLen = 0xff + padding;
    }
    else if(slotLen - padding > 0xf && *prefix_len == 3) {
        slotLen = 0xf + padding;
    }
    *availUsed = slotLen - *prefix_len - suffix_len;
}

const char* parsegraph_nameChunkResponseStage(enum parsegraph_ChunkResponseStage stage)
{
    switch(stage) {
    case parsegraph_CHUNK_RESPONSE_GENERATE:
        return "CHUNK_RESPONSE_GENERATE";
    case parsegraph_CHUNK_RESPONSE_HEADER:
        return "CHUNK_RESPONSE_HEADER";
    case parsegraph_CHUNK_RESPONSE_RESPOND:
        return "CHUNK_RESPONSE_RESPOND";
    case parsegraph_CHUNK_RESPONSE_TRAILER:
        return "CHUNK_RESPONSE_TRAILER";
    case parsegraph_CHUNK_RESPONSE_DONE:
        return "CHUNK_RESPONSE_DONE";
    }
    return "";
}

struct parsegraph_ChunkedPageRequest* parsegraph_ChunkedPageRequest_new(size_t bufSize, parsegraph_ClientRequest* req)
{
    if(!req) {
        fprintf(stderr, "No request given.\n");
        abort();
    }
    struct parsegraph_ChunkedPageRequest* cpr = malloc(sizeof(struct parsegraph_ChunkedPageRequest));
    cpr->req = req;
    cpr->input = parsegraph_Ring_new(bufSize);
    cpr->stage = parsegraph_CHUNK_RESPONSE_GENERATE;
    cpr->handleStage = 0;
    cpr->index = 0;
    cpr->handleData = 0;
    return cpr;
}

void parsegraph_ChunkedPageRequest_free(struct parsegraph_ChunkedPageRequest* cpr)
{
    parsegraph_Ring_free(cpr->input);
    free(cpr);
}

int parsegraph_writeChunk(struct parsegraph_ChunkedPageRequest* cpr, parsegraph_Ring* output)
{
    int avail = parsegraph_Ring_size(cpr->input);
    if(avail == 0) {
        cpr->stage = parsegraph_CHUNK_RESPONSE_TRAILER;
        //fprintf(stderr, "Chunk input buffer was empty.\n");
        return -1;
    }
    if(parsegraph_Ring_size(output) == parsegraph_Ring_capacity(output)) {
        // Output ring is full.
        return -1;
    }
    void* slotData;
    size_t slotLen;
    parsegraph_Ring_writeSlot(output, &slotData, &slotLen);

    // Ignore slots of insufficient size.
    //fprintf(stderr, "output->read_index=%d output->write_index=%d\n", output->read_index, output->write_index);
    if(slotLen <= 5) {
        parsegraph_Ring_putbackWrite(output, slotLen);
        //fprintf(stderr, "presimplifying. output->read_index=%d output->write_index=%d\n", output->read_index, output->write_index);
        parsegraph_Ring_simplify(output);
        //fprintf(stderr, "postsimplifying. output->read_index=%d output->write_index=%d\n", output->read_index, output->write_index);
        parsegraph_Ring_writeSlot(output, &slotData, &slotLen);
        if(slotLen <= 5) {
            //fprintf(stderr, "Slot provided is of insufficient size (%ld). Size=%ld, rindex=%d, windex=%d\n", slotLen, parsegraph_Ring_size(cpr->req->cxn->output), cpr->req->cxn->output->read_index, cpr->req->cxn->output->write_index);
            parsegraph_Ring_putbackWrite(output, slotLen);
            return -1;
        }
    }
    unsigned char* slot = slotData;
    //fprintf(stderr, "CHUNK length: %ld\n", slotLen);

    size_t prefix_len;
    size_t availUsed;
    parsegraph_measureChunk(slotLen, avail, &prefix_len, &availUsed);
    size_t padding = prefix_len + 2;
    if(slotLen > availUsed + prefix_len + 2) {
        parsegraph_Ring_putbackWrite(output, slotLen - (availUsed + prefix_len + 2));
        slotLen = availUsed + prefix_len + 2;
    }

    // slotLen = true size of writeable slot
    // slot = position in output buffer to write chunk
    // prefix_len = length in bytes of the prefix. 3-7 bytes.
    // suffix_len = length of the suffix. always 2 bytes.
    // padding = prefix_len + suffix_len

    //fprintf(stderr, "CHUNK: slot=%lx, avail=%d, slotLen=%ld prefix_len=%ld availUsed=%ld\n", (long unsigned int)slot, avail, slotLen, prefix_len, availUsed);

    // Construct the chunk within the slot.
    int true_prefix_size = snprintf((char*)slot, prefix_len + 1, "%lx\r\n", availUsed);
    if(true_prefix_size < 0) {
        fprintf(stderr, "Failed to generate chunk header.\n");
        abort();
    }
    if(true_prefix_size != prefix_len) {
        fprintf(stderr, "Realized prefix length %d must match the calculated prefix length %ld for %d bytes avail with slotLen of %ld (%ld padding).\n", true_prefix_size, prefix_len, avail, slotLen, padding);
        abort();
    }
    int true_written = parsegraph_Ring_read(cpr->input, slot + prefix_len, availUsed);
    if(true_written != availUsed) {
        fprintf(stderr, "Realized written length %d must match the calculated written length %ld.\n", true_written, availUsed);
        abort();
    }
    slot[slotLen - 2] = '\r';
    slot[slotLen - 1] = '\n';
    //write(0, "CHUNK: ", 7);
    //write(0, slot, slotLen);
    return 0;
}

int parsegraph_ChunkedPageRequest_process(struct parsegraph_ChunkedPageRequest* cpr)
{
    if(!cpr) {
        fprintf(stderr, "No chunked page request.\n");
        abort();
    }

    if(cpr->stage == parsegraph_CHUNK_RESPONSE_GENERATE) {
        if(!cpr->handler) {
            parsegraph_killClientRequest(cpr->req, "No handler available to generate content.\n");
            return -1;
        }
        cpr->stage = parsegraph_CHUNK_RESPONSE_HEADER;
    }

    if(cpr->stage == parsegraph_CHUNK_RESPONSE_HEADER) {
        const char* header = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n";
        int needed = strlen(header);
        int nwritten = parsegraph_Connection_write(cpr->req->cxn, header, needed);
        if(nwritten < needed) {
            if(nwritten > 0) {
                parsegraph_Connection_putbackWrite(cpr->req->cxn, nwritten);
            }
            //fprintf(stderr, "Failed to write complete response header.\n");
            return -1;
        }
        cpr->stage = parsegraph_CHUNK_RESPONSE_RESPOND;
    }

    while(cpr->stage == parsegraph_CHUNK_RESPONSE_RESPOND) {
        if(!cpr->handler) {
            parsegraph_killClientRequest(cpr->req, "No handler available to generate content.\n");
            return -1;
        }
        //parsegraph_Connection_flush(cpr->req->cxn, 0);
        cpr->handler(cpr);
        if(0 != parsegraph_writeChunk(cpr, cpr->req->cxn->output)) {
            if(cpr->stage != parsegraph_CHUNK_RESPONSE_RESPOND) {
                break;
            }
            int nflushed = 0;
            if(parsegraph_Connection_flush(cpr->req->cxn, &nflushed) <= 0) {
                fprintf(stderr, "writeChunk choked.\n");
                return -1;
            }
        }
    }

    if(cpr->stage == parsegraph_CHUNK_RESPONSE_TRAILER) {
        int nwritten = parsegraph_Connection_write(cpr->req->cxn, "0\r\n\r\n", 5);
        if(nwritten < 5) {
            if(nwritten > 0) {
                parsegraph_Connection_putbackWrite(cpr->req->cxn, nwritten);
            }
            //fprintf(stderr, "writing trailer choked.\n");
            return -1;
        }
        cpr->stage = parsegraph_CHUNK_RESPONSE_DONE;
    }

    if(cpr->stage == parsegraph_CHUNK_RESPONSE_DONE) {
        // Mark connection as complete.
        cpr->req->writeStage = parsegraph_CLIENT_REQUEST_DONE_WRITING;
    }

    return 0;
}

void parsegraph_chunkedRequestHandler(struct parsegraph_ClientRequest* req, enum parsegraph_ClientEvent ev, void* data, int datalen)
{
    struct parsegraph_ChunkedPageRequest* cpr;
    switch(ev) {
    case parsegraph_EVENT_ACCEPTING_REQUEST:
        // Indicate accepted.
        *((int*)data) = 1;
        break;
    case parsegraph_EVENT_RESPOND:
        cpr = req->handleData;
        int rv = parsegraph_ChunkedPageRequest_process(cpr);
        if(rv != 0) {
            // Indicate choked.
            *((int*)data) = 1;
        }
        break;
    default:
        break;
    }
}