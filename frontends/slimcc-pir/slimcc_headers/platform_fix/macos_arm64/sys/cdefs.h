#ifndef _SLIMCC_PLAT_MACOS_SYS_CDEFS_H
#define _SLIMCC_PLAT_MACOS_SYS_CDEFS_H

#if !defined(__GNUC__)
# define __GNUC__ 4
# define __GNUC_MINOR__ 2
# define __SLIMCC_FAKE_GNUC_SYS_CDEFS__
#endif

#include_next <sys/cdefs.h>

#ifdef __SLIMCC_FAKE_GNUC_SYS_CDEFS__
# undef __SLIMCC_FAKE_GNUC_SYS_CDEFS__
# undef __GNUC__
# undef __GNUC_MINOR__
#endif

#endif
