#ifndef RUBYGC_H
#define RUBYGC_H
#include "include/ruby/internal/attr/weakref.h"

RBIMPL_DYNAMIC_HOOK void GC_Init();
void GC_Init_default();

#endif
