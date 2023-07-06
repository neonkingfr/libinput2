#ifdef __linux__
#include "linux/input.h"
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include "freebsd/input.h"
#endif
