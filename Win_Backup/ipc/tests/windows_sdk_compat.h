#ifndef CROSSIPC_WINDOWS_SDK_COMPAT_H
#define CROSSIPC_WINDOWS_SDK_COMPAT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* WinNT status macros collide with the similarly named CrossIPC enum values. */
#ifdef STATUS_TIMEOUT
#undef STATUS_TIMEOUT
#endif
#ifdef STATUS_INTERRUPTED
#undef STATUS_INTERRUPTED
#endif

#endif /* CROSSIPC_WINDOWS_SDK_COMPAT_H */
