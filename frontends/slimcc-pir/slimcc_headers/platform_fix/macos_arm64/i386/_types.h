#ifndef _SLIMCC_PLAT_MACOS_I386_TYPES_H
#define _SLIMCC_PLAT_MACOS_I386_TYPES_H

#if !defined(__GNUC__)
# define __GNUC__ 4
# define __SLIMCC_FAKE_GNUC_I386_TYPES__
#endif

#include_next <i386/_types.h>

#ifdef __SLIMCC_FAKE_GNUC_I386_TYPES__
# undef __SLIMCC_FAKE_GNUC_I386_TYPES__
# undef __GNUC__
#endif

#endif
