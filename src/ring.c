#include "rainback.h"

int ensure_po2(size_t given)
{
    size_t candidate = 1;
    while(candidate < given) {
        candidate <<= 1;
    }
    return candidate == given;
}

parsegraph_Ring* parsegraph_Ring_new(size_t capacity)
{
    if(!ensure_po2(capacity)) {
        fprintf(stderr, "Rings must not be created with non power-of-two sizes, but %d was given.\n", capacity);
        abort();
    }
    parsegraph_Ring* rv = malloc(sizeof(parsegraph_Ring));
    rv->capacity = capacity;
    rv->buf = calloc(capacity, 1);
    rv->read_index = 0;
    rv->write_index = 0;
    return rv;
}

unsigned int parsegraph_Ring_size(parsegraph_Ring* ring)
{
    return ring->write_index - ring->read_index;
}

size_t parsegraph_Ring_capacity(parsegraph_Ring* ring)
{
    return ring->capacity;
}

int parsegraph_Ring_read(parsegraph_Ring* ring, char* sink, size_t size)
{
    int nread = 0;
    for(unsigned i = 0; i < size; ++i) {
        if(parsegraph_Ring_size(ring) == 0) {
            break;
        }
        sink[i] = ring->buf[(ring->read_index++) & (ring->capacity-1)];
        ++nread;
    }
    return nread;
}

char parsegraph_Ring_readc(parsegraph_Ring* ring)
{
    if(parsegraph_Ring_size(ring) == 0) {
        return 0;
    }
    return ring->buf[(ring->read_index++) & (ring->capacity-1)];
}

int parsegraph_Ring_write(parsegraph_Ring* ring, const char* source, size_t size)
{
    int nwritten = 0;
    for(unsigned i = 0; i < size; ++i) {
        if(nwritten == parsegraph_Ring_capacity(ring)) {
            return nwritten;
        }
        ++nwritten;
        parsegraph_Ring_writec(ring, source[i]);
    }
    return nwritten;
}

void parsegraph_Ring_writeSlot(parsegraph_Ring* ring, void** slot, size_t* slotLen)
{
    if(parsegraph_Ring_size(ring) == parsegraph_Ring_capacity(ring)) {
        *slot = 0;
        *slotLen = 0;
        return;
    }
    int capmask = ring->capacity - 1;
    size_t index = ring->write_index & capmask;
    *slot = ring->buf + index;

    int rindex = ring->read_index & capmask;
    if(index > rindex) {
        *slotLen = ring->capacity - index;
    }
    else if(index == rindex) {
        *slotLen = parsegraph_Ring_capacity(ring) - index;
    }
    else {
        // index < rindex
        *slotLen = rindex - index;
    }
    ring->write_index += *slotLen;
}

void parsegraph_Ring_readSlot(parsegraph_Ring* ring, void** slot, size_t* slotLen)
{
    int capmask = ring->capacity - 1;
    size_t windex = ring->write_index & capmask;
    size_t rindex = ring->read_index & capmask;
    *slot = ring->buf + rindex;

    if(windex > rindex) {
        // Write index > read index
        *slotLen = parsegraph_Ring_size(ring);
    }
    else if(windex < rindex) {
        // Write index < read index
        *slotLen = parsegraph_Ring_capacity(ring) - rindex;
    }
    else if(ring->write_index - ring->read_index > 0) {
        *slotLen = ring->capacity;
    }
    else {
        *slotLen = 0;
    }
    ring->read_index += *slotLen;
}

void parsegraph_Ring_writec(parsegraph_Ring* ring, char source)
{
    ring->buf[(ring->write_index++) & (ring->capacity-1)] = source;
}

void parsegraph_Ring_putback(parsegraph_Ring* ring, size_t count)
{
    ring->read_index -= count;
}

void parsegraph_Ring_putbackWrite(parsegraph_Ring* ring, size_t count)
{
    ring->write_index -= count;
}

void parsegraph_Ring_free(parsegraph_Ring* ring)
{
    free(ring->buf);
    free(ring);
}
