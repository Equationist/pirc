#ifndef _SLIMCC_PLAT_MACOS_STDLIB_H
#define _SLIMCC_PLAT_MACOS_STDLIB_H

#include_next <stdlib.h>

#ifndef alloca
#define alloca(sz) __builtin_alloca(sz)
#endif

#endif
