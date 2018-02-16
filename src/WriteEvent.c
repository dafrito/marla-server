#include "marla.h"

void marla_WriteEvent_init(marla_WriteEvent* writeEvent, enum marla_WriteResult st)
{
    writeEvent->status = st;
    writeEvent->index = 0;
    writeEvent->length = 0;
    writeEvent->buf = 0;
}

const char* marla_nameWriteResult(marla_WriteResult wr)
{
    switch(wr) {
    case marla_WriteResult_CLOSED:
        return "CLOSED";
    case marla_WriteResult_LOCKED:
        return "LOCKED";
    case marla_WriteResult_KILLED:
        return "KILLED";
    case marla_WriteResult_CONTINUE:
        return "CONTINUE";
    case marla_WriteResult_UPSTREAM_CHOKED:
        return "UPSTREAM_CHOKED";
    case marla_WriteResult_DOWNSTREAM_CHOKED:
        return "DOWNSTREAM_CHOKED";
    case marla_WriteResult_TIMEOUT:
        return "TIMEOUT";
    }
    return "?";
}
