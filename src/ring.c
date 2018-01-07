#include "rainback.h"

static int ensure_po2(size_t given)
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
        fprintf(stderr, "Rings must not be created with non power-of-two sizes, but %ld was given.\n", capacity);
        exit(-1);
    }
    parsegraph_Ring* rv = malloc(sizeof(parsegraph_Ring));
    rv->capacity = capacity;
    rv->buf = calloc(capacity, 1);
    rv->read_index = 0;
    rv->write_index = 0;
    return rv;
}

size_t parsegraph_Ring_size(parsegraph_Ring* ring)
{
    return ring->write_index-ring->read_index;
}

size_t parsegraph_Ring_capacity(parsegraph_Ring* ring)
{
    return ring->capacity;
}

int parsegraph_Ring_read(parsegraph_Ring* ring, unsigned char* sink, size_t size)
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

unsigned char parsegraph_Ring_readc(parsegraph_Ring* ring)
{
    if(parsegraph_Ring_size(ring) == 0) {
        return 0;
    }
    return ring->buf[(ring->read_index++) & (ring->capacity-1)];
}

size_t parsegraph_Ring_write(parsegraph_Ring* ring, const void* source, size_t size)
{
    size_t nwritten = 0;
    for(unsigned i = 0; i < size; ++i) {
        if(parsegraph_Ring_size(ring) == parsegraph_Ring_capacity(ring)) {
            return nwritten;
        }
        ++nwritten;
        parsegraph_Ring_writec(ring, ((unsigned char*)source)[i]);
    }
    return nwritten;
}

int parsegraph_Ring_writeStr(parsegraph_Ring* ring, const char* source)
{
    return parsegraph_Ring_write(ring, source, strlen(source));
}

void parsegraph_Ring_writeSlot(parsegraph_Ring* ring, void** slot, size_t* slotLen)
{
    if(!slotLen) {
        fprintf(stderr, "slotLen must be given.");
        abort();
    }
    if(!slot) {
        fprintf(stderr, "slot must be given.");
        abort();
    }
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
        //fprintf(stderr, "windex(%ld) > rindex(%d)\n", index, rindex);
        *slotLen = ring->capacity - index;
    }
    else if(index == rindex) {
        //fprintf(stderr, "windex(%ld) == rindex(%d)\n", index, rindex);
        *slotLen = parsegraph_Ring_capacity(ring) - index;
    }
    else {
        // index < rindex
        //fprintf(stderr, "windex(%ld) < rindex(%d)\n", index, rindex);
        *slotLen = rindex - index;
    }
    ring->write_index += *slotLen;
    //fprintf(stderr, "windex(%d)\n", ring->write_index & capmask);
}

void parsegraph_Ring_readSlot(parsegraph_Ring* ring, void** slot, size_t* slotLen)
{
    if(!slotLen) {
        fprintf(stderr, "slotLen must be given.\n");
        abort();
    }
    if(!slot) {
        fprintf(stderr, "slot must be given.\n");
        abort();
    }
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
    else if(ring->write_index > ring->read_index) {
        *slotLen = ring->capacity - rindex;
    }
    else {
        *slotLen = 0;
    }
    ring->read_index += *slotLen;
}

void parsegraph_Ring_simplify(parsegraph_Ring* ring)
{
    size_t size = parsegraph_Ring_size(ring);
    if(size == ring->capacity) {
        return;
    }
    if(size == 0) {
        ring->read_index = 0;
        ring->write_index = 0;
        return;
    }

    int capmask = ring->capacity - 1;
    size_t rindex = ring->read_index & capmask;

    /* check if any left over */
    if(rindex + size > ring->capacity) {
        /* move the leading part over with space for the rest */
        size_t leading_part_length = rindex + size - ring->capacity;
        size_t trailing_part_length = size - leading_part_length;
        memmove(ring->buf + trailing_part_length, ring->buf, leading_part_length);
        //fprintf(stderr, "Moving %ld bytes\n", leading_part_length);

        /* move the trailing part to the front. */
        memmove(ring->buf, ring->buf + ring->capacity - trailing_part_length, trailing_part_length);
        //fprintf(stderr, "Moving %ld bytes\n", trailing_part_length);
    }
    else if(rindex + size != ring->capacity) {
        /* only one move needed */
        memmove(ring->buf, ring->buf + rindex, size);
        //fprintf(stderr, "Moving %ld bytes\n", size);
    }
    else {
        // Full, nothing to do.
        return;
    }

    /* adjust indices */
    ring->write_index = size & capmask;
    ring->read_index = 0;
}

void parsegraph_Ring_writec(parsegraph_Ring* ring, unsigned char source)
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
