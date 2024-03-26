#include <stdio.h>
#include <stdlib.h>

void GC_Init(void)
{
    if (getenv("RUBY_SHARED_GC")){
        fprintf(stderr, "=== OVERRIDE GC\n");
    }
}
