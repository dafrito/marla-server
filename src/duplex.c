#include "marla.h"

static int readDuplexSource(struct marla_Connection* cxn, void* sink, size_t len)
{
    marla_DuplexSource* source = cxn->source;
    int rv = marla_Ring_read(source->input, sink, len);
    if(rv <= 0) {
        return -1;
    }
    return rv;
}

static int writeDuplexSource(struct marla_Connection* cxn, void* writeSource, size_t len)
{
    marla_DuplexSource* cxnSource = cxn->source;
    int rv = marla_Ring_write(cxnSource->output, writeSource, len);
    if(rv <= 0) {
        return -1;
    }
    return rv;
}

static void acceptDuplexSource(marla_Connection* cxn)
{
    // Accepted and secured.
    cxn->stage = marla_CLIENT_SECURED;
}

static int shutdownDuplexSource(marla_Connection* cxn)
{
    return 1;
}

static void destroyDuplexSource(marla_Connection* cxn)
{
    marla_DuplexSource* source = cxn->source;
    marla_Ring_free(source->input);
    marla_Ring_free(source->output);
    free(source);
}

int marla_writeDuplex(marla_Connection* cxn, void* source, size_t len)
{
    marla_DuplexSource* cxnSource = cxn->source;
    return marla_Ring_write(cxnSource->input, source, len);
}

int marla_readDuplex(marla_Connection* cxn, void* sink, size_t len)
{
    marla_DuplexSource* source = cxn->source;
    return marla_Ring_read(source->output, sink, len);
}

static int describeDuplexSource(marla_Connection* cxn, char* sink, size_t len)
{
    marla_DuplexSource* source = cxn->source;
    memset(sink, 0, len);
    snprintf(sink, len, "Duplex(i=%ld, o=%ld)", marla_Ring_size(source->input), marla_Ring_size(source->output));
    return 0;
}

void marla_Duplex_drainInput(marla_Connection* cxn)
{
    marla_DuplexSource* source = cxn->source;
    source->drain_input = 1;
    marla_Ring_clear(source->input);
}

void marla_Duplex_drainOutput(marla_Connection* cxn)
{
    marla_DuplexSource* source = cxn->source;
    source->drain_output = 1;
    marla_Ring_clear(source->output);
}

void marla_Duplex_plugInput(marla_Connection* cxn)
{
    marla_DuplexSource* source = cxn->source;
    source->drain_input = 0;
}

void marla_Duplex_plugOutput(marla_Connection* cxn)
{
    marla_DuplexSource* source = cxn->source;
    source->drain_output = 0;
}

void marla_Duplex_init(marla_Connection* cxn, size_t input_size, size_t output_size)
{
    marla_DuplexSource* source = malloc(sizeof(marla_DuplexSource));
    source->input = marla_Ring_new(input_size);
    source->output = marla_Ring_new(output_size);
    source->drain_input = 0;
    source->drain_output = 0;
    cxn->source = source;
    cxn->readSource = readDuplexSource;
    cxn->writeSource = writeDuplexSource;
    cxn->acceptSource = acceptDuplexSource;
    cxn->shutdownSource = shutdownDuplexSource;
    cxn->destroySource = destroyDuplexSource;
    cxn->describeSource = describeDuplexSource;
}
