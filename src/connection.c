#include "marla.h"

// SSL source
    // int marla_Source_New(SSL_CTX* ctx, int fd)
    // void marla_Source_Destroy()
    // int marla_Source_Init(SSL*)
    // int marla_Source_Read(void* sink, size_t len)
    // int marla_Source_Write(void* source, size_t len)

// Loopback source
    // int marla_Source_New(int bufsize)
    // void marla_Source_Destroy()
    // int marla_Source_Init()
    // int marla_Source_Read(void* sink, size_t len)
    // int marla_Source_Write(void* source, size_t len)
    // int marla_Source_Feed(void* source, size_t len) <-- loopback
    // int marla_Source_Consume(void* sink, size_t len) <-- loopback

// marla_Connection {
//     void* source;

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

int marla_Connection_read(marla_Connection* cxn, unsigned char* sink, size_t requested)
{
    if(requested == 0) {
        return -1;
    }
    marla_Ring* const input = cxn->input;

    int sinkread = 0;
    int partialRead = 0;
    while(sinkread < requested) {
        marla_logMessagef(cxn->server, "Requesting %ld bytes of input data. %d/%d bytes in cxn->input.", requested-sinkread, marla_Ring_size(input),marla_Ring_capacity(input));
        int nbufread = marla_Ring_read(input, sink + sinkread, requested - sinkread);
        if(nbufread < 0) {
            // Pass error return value through to caller.
            marla_Ring_putbackRead(input, sinkread);
            marla_logMessagef(cxn->server, "Failed to read from input buffer.");
            return nbufread;
        }
        sinkread += nbufread;

        if(partialRead) {
            marla_logMessage(cxn->server, "Nothing more to read.");
            break;
        }

        // Refill buffer.
        if(cxn->wantsRead) {
            marla_logMessagef(cxn->server, "Connection wants read.");
            break;
        }
        void* ringBuf;
        size_t slotLen;
        marla_Ring_writeSlot(input, &ringBuf, &slotLen);
        if(slotLen == 0) {
            marla_logMessage(cxn->server, "Input buffer is full.");
            break;
        }
        int nread = cxn->readSource(cxn, ringBuf, slotLen);
        marla_logMessagef(cxn->server, "Read %d bytes from source into input slot of size %d.", nread, slotLen);
        if(nread <= 0) {
            if(cxn->shouldDestroy) {
                // Error; put everything back.
                marla_Ring_putbackWrite(input, sinkread + slotLen);
                return nread;
            }
            marla_Ring_putbackWrite(input, slotLen);
            marla_logMessagef(cxn->server, "Connection is done reading. %d bytes read", sinkread);
            if(sinkread <= 0) {
                return -1;
            }
            return sinkread;
        }
        if(nread < slotLen) {
            marla_Ring_putbackWrite(input, slotLen - nread);
            partialRead = 1;
        }
    }
    marla_logMessagef(cxn->server, "Read %d bytes in total.", sinkread);
    if(sinkread == 0) {
        return -1;
    }
    return sinkread;
}

void marla_Connection_putbackRead(marla_Connection* cxn, size_t amount)
{
    //marla_logMessagef(cxn->server, "Putting back %d bytes read", amount);
    return marla_Ring_putbackRead(cxn->input, amount);
}

void marla_Connection_putbackWrite(marla_Connection* cxn, size_t amount)
{
    //marla_logMessagef(cxn->server, "Putting back %d bytes written", amount);
    return marla_Ring_putbackWrite(cxn->output, amount);
}

int marla_Connection_write(marla_Connection* cxn, const void* source, size_t requested)
{
    return marla_Ring_write(cxn->output, source, requested);
}

int marla_Connection_flush(marla_Connection* cxn, int* outnflushed)
{
    void* buf;
    size_t len;

    int nflushed = 0;
    while(1) {
        marla_Ring_readSlot(cxn->output, &buf, &len);
        if(len == 0) {
            break;
        }
        int nsslwritten = cxn->writeSource(cxn, buf, len);
        if(nsslwritten <= 0) {
            if(outnflushed) {
                *outnflushed = nflushed;
            }
            cxn->flushed += nflushed;
            return nflushed;
        }
        nflushed += nsslwritten;
        //fprintf(stderr, "%s: %d bytes flushed to source.\n", __FUNCTION__, nsslwritten);
        marla_Ring_putbackRead(cxn->output, len - nsslwritten);
        if(nsslwritten < len) {
            // Partial write.
            break;
        }
    }

    if(outnflushed) {
        *outnflushed = nflushed;
    }
    cxn->flushed += nflushed;
    return nflushed;
}

void marla_Connection_destroy(marla_Connection* cxn)
{
    marla_logMessage(cxn->server, "Destroying connection");
    if(cxn->destroySource) {
        cxn->destroySource(cxn);
        cxn->destroySource = 0;
    }

    for(marla_Request* req = cxn->current_request; req != 0;) {
        marla_Request* nextReq = req->next_request;
        marla_Request_destroy(req);
        req = nextReq;
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
