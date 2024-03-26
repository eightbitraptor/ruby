#include <stdio.h>
#include <stdlib.h>
#include "internal/rubygc.h"

void GC_Init_default(void)
{
    if (getenv("RUBY_SHARED_GC")){
        fprintf(stderr, "=== DEFAULT GC\n");
    }
}
