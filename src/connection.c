#include "marla.h"

const char* marla_nameConnectionStage(enum marla_ConnectionStage stage)
{
    switch(stage) {
    case marla_CLIENT_ACCEPTED:
        return "CLIENT_ACCEPTED";
    case marla_CLIENT_SECURED:
        return "CLIENT_SECURED";
    case marla_BACKEND_READY:
        return "BACKEND_READY";
    case marla_CLIENT_COMPLETE:
        return "CLIENT_COMPLETE";
    }
    return "?";
}

static int next_connection_id = 1;

marla_Connection* marla_Connection_new(struct marla_Server* server)
{
    if(!server) {
        fprintf(stderr, "A connection must be provided a server when constructed.\n");
        abort();
    }
    marla_Connection* cxn = (marla_Connection*)malloc(sizeof(*cxn));
    if(!cxn) {
        return 0;
    }

    cxn->id = ++next_connection_id;

    cxn->server = server;
    cxn->prev_connection = 0;
    cxn->next_connection = 0;
    cxn->lastProcessTime.tv_sec = 0;
    cxn->lastProcessTime.tv_nsec = 0;

    // Initialize flags.
    cxn->shouldDestroy = 0;
    cxn->wantsWrite = 0;
    cxn->wantsRead = 0;
    cxn->flushed = 0;
    cxn->in_read = 0;
    cxn->in_write = 0;
    cxn->is_backend = 0;

    cxn->source = 0;
    cxn->describeSource = 0;
    cxn->readSource = 0;
    cxn->writeSource = 0;
    cxn->acceptSource = 0;
    cxn->shutdownSource = 0;
    cxn->destroySource = 0;

    // Initialize the buffer.
    cxn->input = marla_Ring_new(marla_BUFSIZE);
    cxn->output = marla_Ring_new(marla_BUFSIZE);

    cxn->stage = marla_CLIENT_ACCEPTED;
    cxn->requests_in_process = 0;
    cxn->latest_request = 0;
    cxn->current_request = 0;

    if(cxn->server) {
        if(!cxn->server->last_connection) {
            cxn->server->first_connection = cxn;
            cxn->server->last_connection = cxn;
        }
        else {
            cxn->server->last_connection->next_connection = cxn;
            cxn->prev_connection = cxn->server->last_connection;
            cxn->server->last_connection = cxn;
        }
    }

    return cxn;
}

int marla_Connection_refill(marla_Connection* cxn, size_t* total)
{
    marla_Ring* input = cxn->input;
    for(; !cxn->wantsRead;) {
        void* ringBuf;
        size_t slotLen;
        marla_Ring_writeSlot(input, &ringBuf, &slotLen);
        if(slotLen == 0) {
            marla_logMessagecf(cxn->server, "I/O", "Connection %d's input buffer is full.", cxn->id);
            break;
        }

        int nread = cxn->readSource(cxn, ringBuf, slotLen);
        marla_logMessagecf(cxn->server, "I/O", "Read %d bytes from source into input slot of size %d.", nread, slotLen);
        if(nread <= 0) {
            marla_Ring_putbackWrite(input, slotLen);
            return nread;
        }
        if(total) {
            *total += nread;
        }
        if(nread < slotLen) {
            marla_Ring_putbackWrite(input, slotLen - nread);
            break;
        }
    }
    return -1;
}

int marla_Connection_read(marla_Connection* cxn, unsigned char* sink, size_t requested)
{
    //marla_Connection_refill(cxn, 0);
    if(cxn->shouldDestroy) {
        return -1;
    }
    if(requested == 0) {
        return -1;
    }
    marla_Ring* const input = cxn->input;
    //marla_logMessagef(cxn->server, "Requesting %ld bytes of input data. %d/%d bytes in cxn->input.", requested, marla_Ring_size(input), marla_Ring_capacity(input));
    if(marla_Ring_isEmpty(input)) {
        return -1;
    }
    int true_read = marla_Ring_read(input, sink, requested);
    return true_read;
}

void marla_Connection_putbackRead(marla_Connection* cxn, size_t amount)
{
    marla_logMessagecf(cxn->server, "I/O", "Putting back %d bytes read", amount);
    //printf("Putting back %d bytes read\n", amount);
    return marla_Ring_putbackRead(cxn->input, amount);
}

void marla_Connection_putbackWrite(marla_Connection* cxn, size_t amount)
{
    marla_logMessagecf(cxn->server, "I/O", "Putting back %d bytes written", amount);
    //printf("Putting back %d bytes written\n", amount);
    return marla_Ring_putbackWrite(cxn->output, amount);
}

int marla_Connection_write(marla_Connection* cxn, const void* source, size_t requested)
{
    if(marla_Ring_isFull(cxn->output)) {
        return -1;
    }
    return marla_Ring_write(cxn->output, source, requested);
}

marla_WriteResult marla_Connection_flush(marla_Connection* cxn, int* outnflushed)
{
    void* buf;
    size_t len;

    int nflushed = 0;
    marla_WriteResult wr;
    for(;;) {
        marla_Ring_readSlot(cxn->output, &buf, &len);
        if(len == 0) {
            wr = marla_WriteResult_UPSTREAM_CHOKED;
            break;
        }
        int true_flushed = cxn->writeSource(cxn, buf, len);
        if(true_flushed <= 0) {
            marla_Ring_putbackRead(cxn->output, len);
            wr = marla_WriteResult_DOWNSTREAM_CHOKED;
            break;
        }
        nflushed += true_flushed;
        marla_logMessagecf(cxn->server, "I/O", "%d bytes flushed to source on connection %d.", true_flushed, cxn->id);
        //printf("%d bytes flushed to source on connection %d. Size=%d\n", true_flushed, cxn->id, marla_Ring_size(cxn->output));
        if(true_flushed < len) {
            // Partial write.
            marla_Ring_putbackRead(cxn->output, len - true_flushed);
            //marla_Ring_dump(cxn->output, "cxn->output");
            wr = marla_WriteResult_DOWNSTREAM_CHOKED;
            break;
        }
    }
    if(outnflushed) {
        *outnflushed = nflushed;
    }
    cxn->flushed += nflushed;
    if(cxn->shouldDestroy) {
        return marla_WriteResult_CLOSED;
    }
    return wr;
}

void marla_Connection_destroy(marla_Connection* cxn)
{
    marla_logMessagef(cxn->server, "Destroying connection %d", cxn->id);

    // Force reads and writes to fail.
    cxn->in_write = 1;
    cxn->in_read = 1;

    if(cxn->destroySource) {
        cxn->destroySource(cxn);
        cxn->destroySource = 0;
    }

    if(!cxn->is_backend) {
        for(marla_Request* req = cxn->current_request; req != 0;) {
            if(req->readStage == marla_BACKEND_REQUEST_READING_RESPONSE_BODY && (req->close_after_done || req->requestLen == marla_MESSAGE_USES_CLOSE)) {
                req->readStage = marla_BACKEND_REQUEST_DONE_READING;
            }
            req = req->next_request;
        }

        for(marla_Request* req = cxn->current_request; req != 0;) {
            marla_Request* nextReq = req->next_request;
            marla_Request_unref(req);
            req = nextReq;
        }
    }

    if(cxn->prev_connection && cxn->next_connection) {
        cxn->next_connection->prev_connection = cxn->prev_connection;
        cxn->prev_connection->next_connection = cxn->next_connection;
        cxn->next_connection = 0;
        cxn->prev_connection = 0;
    }
    else if(cxn->prev_connection) {
        if(cxn->server && cxn->server->last_connection == cxn) {
            cxn->server->last_connection = cxn->prev_connection;
        }
        cxn->prev_connection->next_connection = 0;
        cxn->next_connection = 0;
        cxn->prev_connection = 0;
    }
    else if(cxn->next_connection) {
        if(cxn->server && cxn->server->first_connection == cxn) {
            cxn->server->first_connection = cxn->next_connection;
        }
        cxn->next_connection->prev_connection = 0;
        cxn->next_connection = 0;
        cxn->prev_connection = 0;
    }
    else {
        if(cxn->server && cxn->server->first_connection == cxn) {
            cxn->server->first_connection = cxn->next_connection;
        }
        if(cxn->server && cxn->server->last_connection == cxn) {
            cxn->server->last_connection = cxn->prev_connection;
        }
    }

    marla_Ring_free(cxn->input);
    marla_Ring_free(cxn->output);

    if(cxn->server->backend == cxn) {
        cxn->server->backend = 0;
        cxn->server->backendfd = 0;
    }

    free(cxn);
}
