#include "rainback.h"

int main(int argc, char* argv[])
{
    parsegraph_Connection* cxn = parsegraph_Connection_new();
    parsegraph_Connection_destroy(cxn);
    return 0;
}
