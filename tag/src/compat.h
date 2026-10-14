/*
 * compat.h - lets the platform-independent code compile both with SDCC (8051)
 * and with a normal C compiler on the PC (host tests in test/).
 */
#ifndef COMPAT_H
#define COMPAT_H

#include <stdint.h>
#include <stdbool.h>

#if defined(__SDCC)
/* SDCC: __code = flash, __xdata = external RAM. The keywords already exist. */
#else
#define __code
#define __xdata
#define __data
#define __idata
#endif

#endif /* COMPAT_H */
