#include "marla.h"
#include <string.h>
#include <unistd.h>

static int ensure_po2(size_t given)
{
    size_t candidate = 1;
    while(candidate < given) {
        candidate <<= 1;
    }
    return candidate == given;
}

marla_Ring* marla_Ring_new(size_t capacity)
{
    if(!ensure_po2(capacity)) {
        fprintf(stderr, "Rings must not be created with non power-of-two sizes, but %ld was given.\n", capacity);
        exit(EXIT_FAILURE);
    }
    marla_Ring* rv = malloc(sizeof(marla_Ring));
    rv->capacity = capacity;
    rv->buf = calloc(capacity, 1);
    rv->read_index = 0;
    rv->write_index = 0;
    return rv;
}

size_t marla_Ring_size(marla_Ring* ring)
{
    size_t size = ring->write_index-ring->read_index;
    if(size > marla_Ring_capacity(ring)) {
        return size&(marla_Ring_capacity(ring)-1);
    }
    return size;
}

size_t marla_Ring_capacity(marla_Ring* ring)
{
    return ring->capacity;
}

int marla_Ring_isFull(marla_Ring* ring)
{
    return marla_Ring_size(ring) == marla_Ring_capacity(ring);
}

int marla_Ring_isEmpty(marla_Ring* ring)
{
    return marla_Ring_size(ring) == 0;
}

int marla_Ring_read(marla_Ring* ring, unsigned char* sink, size_t size)
{
    int nread = 0;
    for(unsigned i = 0; i < size; ++i) {
        if(marla_Ring_size(ring) == 0) {
            break;
        }
        sink[i] = ring->buf[(ring->read_index++) & (ring->capacity-1)];
        ++nread;
    }
    return nread;
}

int marla_Ring_readc(marla_Ring* ring, unsigned char* c)
{
    if(marla_Ring_size(ring) == 0) {
        return 0;
    }
    *c = ring->buf[(ring->read_index++) & (ring->capacity-1)];
    return 1;
}

size_t marla_Ring_write(marla_Ring* ring, const void* source, size_t size)
{
    size_t nwritten = 0;
    for(unsigned i = 0; i < size; ++i) {
        if(marla_Ring_size(ring) == marla_Ring_capacity(ring)) {
            return nwritten;
        }
        ++nwritten;
        marla_Ring_writec(ring, ((unsigned char*)source)[i]);
    }
    return nwritten;
}

int marla_Ring_writeStr(marla_Ring* ring, const char* source)
{
    return marla_Ring_write(ring, source, strlen(source));
}

void marla_Ring_writeSlot(marla_Ring* ring, void** slot, size_t* slotLen)
{
    if(!slotLen) {
        fprintf(stderr, "slotLen must be given.");
        abort();
    }
    if(!slot) {
        fprintf(stderr, "slot must be given.");
        abort();
    }
    if(marla_Ring_size(ring) == marla_Ring_capacity(ring)) {
        *slot = 0;
        *slotLen = 0;
        return;
    }
    int capmask = ring->capacity - 1;
    size_t index = ring->write_index & capmask;
    *slot = ring->buf + index;

    int rindex = ring->read_index & capmask;
    if(index > rindex) {
        //printf("windex(%ld) > rindex(%d)\n", index, rindex);
        *slotLen = ring->capacity - index;
    }
    else if(index == rindex) {
        //printf("windex(%ld) == rindex(%d)\n", index, rindex);
        *slotLen = marla_Ring_capacity(ring) - index;
    }
    else {
        // index < rindex
        //printf("windex(%ld) < rindex(%d)\n", index, rindex);
        *slotLen = rindex - index;
    }
    ring->write_index += *slotLen;
    //printf("windex(%d) %zu\n", ring->write_index & capmask, *slotLen);
}

void marla_Ring_readSlot(marla_Ring* ring, void** slot, size_t* slotLen)
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
        *slotLen = marla_Ring_size(ring);
    }
    else if(windex < rindex) {
        // Write index < read index
        *slotLen = marla_Ring_capacity(ring) - rindex;
    }
    else if(ring->write_index > ring->read_index) {
        *slotLen = ring->capacity - rindex;
        //printf("SPECIAL THIRD CASE: write_index=%d read_index=%d %d\n", ring->write_index, ring->read_index, *slotLen);
    }
    else {
        *slotLen = 0;
    }
    ring->read_index += *slotLen;
}

void marla_Ring_simplify(marla_Ring* ring)
{
    size_t size = marla_Ring_size(ring);
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

int marla_Ring_writec(marla_Ring* ring, unsigned char source)
{
    if(marla_Ring_isFull(ring)) {
        return 0;
    }
    ring->buf[(ring->write_index++) & (ring->capacity-1)] = source;
    return 1;
}

void marla_Ring_putbackRead(marla_Ring* ring, size_t count)
{
    ring->read_index -= count;
}

void marla_Ring_putbackWrite(marla_Ring* ring, size_t count)
{
    ring->write_index -= count;
}

void marla_Ring_clear(marla_Ring* ring)
{
    ring->read_index = ring->write_index;
}

void marla_Ring_dump(marla_Ring* ring, const char* name)
{
    int len = marla_Ring_size(ring);
    unsigned char* tmp = malloc(len + 1);
    marla_Ring_read(ring, tmp, len);
    tmp[len] = 0;
    printf("%s(%d): %s\n", name, len, tmp);
    free(tmp);
    marla_Ring_putbackRead(ring, len);
}

void marla_Ring_free(marla_Ring* ring)
{
    free(ring->buf);
    free(ring);
}
