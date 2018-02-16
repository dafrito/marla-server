#include "marla.h"

void marla_measureChunk(size_t slotLen, int avail, size_t* prefix_len, size_t* availUsed)
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

const char* marla_nameChunkResponseStage(enum marla_ChunkResponseStage stage)
{
    switch(stage) {
    case marla_CHUNK_RESPONSE_GENERATE:
        return "CHUNK_RESPONSE_GENERATE";
    case marla_CHUNK_RESPONSE_HEADER:
        return "CHUNK_RESPONSE_HEADER";
    case marla_CHUNK_RESPONSE_RESPOND:
        return "CHUNK_RESPONSE_RESPOND";
    case marla_CHUNK_RESPONSE_TRAILER:
        return "CHUNK_RESPONSE_TRAILER";
    case marla_CHUNK_RESPONSE_DONE:
        return "CHUNK_RESPONSE_DONE";
    }
    return "";
}
struct marla_ChunkedPageRequest* marla_ChunkedPageRequest_new(size_t bufSize, marla_Request* req)
{
    if(!req) {
        fprintf(stderr, "No request given.\n");
        abort();
    }
    struct marla_ChunkedPageRequest* cpr = malloc(sizeof(struct marla_ChunkedPageRequest));
    cpr->req = req;
    cpr->input = marla_Ring_new(bufSize);
    cpr->stage = marla_CHUNK_RESPONSE_GENERATE;
    cpr->index = 0;
    cpr->handleData = 0;
    cpr->handleStage = 0;
    return cpr;
}

int marla_ChunkedPageRequest_write(marla_ChunkedPageRequest* cpr, unsigned char* in, size_t len)
{
    return marla_Ring_write(cpr->input, in, len);
}

marla_WriteResult marla_writeChunk(marla_Server* server, marla_Ring* input, marla_Ring* output)
{
    if(marla_Ring_isFull(output)) {
        //marla_logMessage(server, "Output ring is full.");
        return marla_WriteResult_DOWNSTREAM_CHOKED;
    }
    int avail = marla_Ring_size(input);
    if(avail == 0) {
        //marla_logMessage(server, "Chunk input buffer was empty.");
        return marla_WriteResult_UPSTREAM_CHOKED;
    }
    void* slotData;
    size_t slotLen;
    marla_Ring_writeSlot(output, &slotData, &slotLen);

    // Ignore slots of insufficient size.
    //marla_logMessagef(server, "output->read_index=%d output->write_index=%d\n", output->read_index, output->write_index);
    if(slotLen <= 5) {
        marla_Ring_putbackWrite(output, slotLen);
        //fprintf(stderr, "presimplifying. output->read_index=%d output->write_index=%d\n", output->read_index, output->write_index);
        marla_Ring_simplify(output);
        //fprintf(stderr, "postsimplifying. output->read_index=%d output->write_index=%d\n", output->read_index, output->write_index);
        marla_Ring_writeSlot(output, &slotData, &slotLen);
        if(slotLen <= 5) {
            marla_logMessagef(server, "Slot provided is of insufficient size (%ld). Size=%ld, rindex=%d, windex=%d\n", slotLen, marla_Ring_size(output), output->read_index, output->write_index);
            marla_Ring_putbackWrite(output, slotLen);
            return marla_WriteResult_UPSTREAM_CHOKED;
        }
    }
    unsigned char* slot = slotData;
    //marla_logMessagef(server, "CHUNK length: %ld\n", slotLen);

    size_t prefix_len;
    size_t availUsed;
    marla_measureChunk(slotLen, avail, &prefix_len, &availUsed);
    size_t padding = prefix_len + 2;
    if(slotLen > availUsed + prefix_len + 2) {
        marla_Ring_putbackWrite(output, slotLen - (availUsed + prefix_len + 2));
        slotLen = availUsed + prefix_len + 2;
    }

    // slotLen = true size of writeable slot
    // slot = position in output buffer to write chunk
    // prefix_len = length in bytes of the prefix. 3-7 bytes.
    // suffix_len = length of the suffix. always 2 bytes.
    // padding = prefix_len + suffix_len

    //marla_logMessagef(server, "CHUNK: slot=%lx, avail=%d, slotLen=%ld prefix_len=%ld availUsed=%ld\n", (long unsigned int)slot, avail, slotLen, prefix_len, availUsed);

    // Construct the chunk within the slot.
    int true_prefix_size = snprintf((char*)slot, prefix_len + 1, "%lx\r\n", availUsed);
    if(true_prefix_size < 0) {
        marla_die(server, "snprintf failed to generate chunk header.");
    }
    if(true_prefix_size != prefix_len) {
        marla_die(server, "Realized prefix length %d must match the calculated prefix length %ld for %d bytes avail with slotLen of %ld (%ld padding).\n", true_prefix_size, prefix_len, avail, slotLen, padding);
    }
    int true_written = marla_Ring_read(input, slot + prefix_len, availUsed);
    if(true_written != availUsed) {
        marla_die(server, "Realized written length %d must match the calculated written length %ld.\n", true_written, availUsed);
    }
    slot[slotLen - 2] = '\r';
    slot[slotLen - 1] = '\n';
    return marla_WriteResult_CONTINUE;
}

void marla_ChunkedPageRequest_free(struct marla_ChunkedPageRequest* cpr)
{
    marla_Ring_free(cpr->input);
    free(cpr);
}

marla_WriteResult marla_ChunkedPageRequest_process(struct marla_ChunkedPageRequest* cpr)
{
    marla_Request* req = cpr->req;
    marla_Server* server = req->cxn->server;

    if(cpr->stage == marla_CHUNK_RESPONSE_GENERATE) {
        if(!cpr->handler) {
            marla_killRequest(cpr->req, "No handler available to generate content.");
            return marla_WriteResult_KILLED;
        }
        cpr->stage = marla_CHUNK_RESPONSE_HEADER;
    }

    if(cpr->stage == marla_CHUNK_RESPONSE_HEADER) {
        const char* header = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n";
        int needed = strlen(header);
        int nwritten = marla_Connection_write(cpr->req->cxn, header, needed);
        if(nwritten < needed) {
            if(nwritten > 0) {
                marla_Connection_putbackWrite(cpr->req->cxn, nwritten);
            }
            marla_logMessage(server, "Failed to write complete response header.");
            return marla_WriteResult_DOWNSTREAM_CHOKED;
        }
        cpr->stage = marla_CHUNK_RESPONSE_RESPOND;
    }

    while(cpr->stage == marla_CHUNK_RESPONSE_RESPOND) {
        int done_indicated = 0;
        if(!cpr->handler) {
            marla_killRequest(cpr->req, "No handler available to generate content.");
            return marla_WriteResult_KILLED;
        }
        //marla_Connection_flush(cpr->req->cxn, 0);
        marla_WriteResult wr = cpr->handler(cpr);
        switch(wr) {
        case marla_WriteResult_CONTINUE:
            done_indicated = marla_Ring_isEmpty(cpr->input);
            break;
        case marla_WriteResult_DOWNSTREAM_CHOKED:
            break;
        case marla_WriteResult_UPSTREAM_CHOKED:
        case marla_WriteResult_TIMEOUT:
        case marla_WriteResult_LOCKED:
        case marla_WriteResult_KILLED:
        case marla_WriteResult_CLOSED:
            return wr;
        }
        int nflushed = 0;
        switch(marla_writeChunk(cpr->req->cxn->server, cpr->input, cpr->req->cxn->output)) {
        case marla_WriteResult_UPSTREAM_CHOKED:
            if(!marla_Ring_isEmpty(cpr->input)) {
                marla_die(server, "writeChunk indicated upstream choked, but upstream has data.");
            }
            if(done_indicated) {
                wr = marla_writeChunkTrailer(cpr->req->cxn->output);
                switch(wr) {
                case marla_WriteResult_CONTINUE:
                    ++cpr->stage;
                    continue;
                case marla_WriteResult_DOWNSTREAM_CHOKED:
                    if(marla_Ring_isEmpty(cpr->req->cxn->output)) {
                        return wr;
                    }
                    if(marla_Connection_flush(cpr->req->cxn, &nflushed) <= 0) {
                        //fprintf(stderr, "writeChunk choked.\n");
                        return wr;
                    }
                    continue;
                default:
                    return wr;
                }
            }
            continue;
        case marla_WriteResult_CONTINUE:
            continue;
        case marla_WriteResult_DOWNSTREAM_CHOKED:
            if(marla_Connection_flush(cpr->req->cxn, &nflushed) <= 0) {
                return marla_WriteResult_DOWNSTREAM_CHOKED;
            }
            continue;
        default:
            marla_die(server, "Unreachable");
        }
    }

    if(cpr->stage == marla_CHUNK_RESPONSE_TRAILER) {
        cpr->stage = marla_CHUNK_RESPONSE_DONE;
    }

    if(cpr->stage == marla_CHUNK_RESPONSE_DONE) {
        // Mark connection as complete.
        cpr->req->writeStage = marla_CLIENT_REQUEST_DONE_WRITING;
    }

    return 0;
}

marla_WriteResult marla_writeChunkTrailer(marla_Ring* output)
{
    int nwritten = marla_Ring_writeStr(output, "0\r\n\r\n");
    if(nwritten < 5) {
        if(nwritten > 0) {
            marla_Ring_putbackWrite(output, nwritten);
        }
        return marla_WriteResult_DOWNSTREAM_CHOKED;
    }
    return marla_WriteResult_CONTINUE;
}

void marla_chunkedRequestHandler(struct marla_Request* req, enum marla_ClientEvent ev, void* data, int datalen)
{
    struct marla_ChunkedPageRequest* cpr;
    struct marla_WriteEvent* we;
    switch(ev) {
    case marla_EVENT_ACCEPTING_REQUEST:
        // Indicate accepted.
        *((int*)data) = 1;
        break;
    case marla_EVENT_MUST_WRITE:
        cpr = req->handlerData;
        int rv = marla_ChunkedPageRequest_process(cpr);
        if(rv != 0) {
            // Indicate choked.
            *((int*)data) = 1;
        }
        break;
    case marla_EVENT_REQUEST_BODY:
        we = data;
        if(we->length == 0) {
            req->readStage = marla_CLIENT_REQUEST_DONE_READING;
        }
        break;
    case marla_EVENT_DESTROYING:
        cpr = req->handlerData;
        marla_ChunkedPageRequest_free(cpr);
        break;
    default:
        break;
    }
}

