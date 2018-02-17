#include "marla.h"
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

int main()
{
    if(marla_BUFSIZE < 512) {
        return 1;
    }
    return 0;
}
