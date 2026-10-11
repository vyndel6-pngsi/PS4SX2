// PS4 port (AI-assisted): FreeBSD's <sys/cpuset.h>, which OpenOrbis' C library doesn't have, for
// GSRenderer.cpp's thread placement (cpuset_t, CPU_ZERO/CPU_SET, cpuset_setaffinity). The PS4's
// game process sees at most 8 cores. cpuset_setaffinity is ps4/ps4_compat.cpp's.
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_SETSIZE 64
typedef struct
{
	unsigned long __bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpuset_t;

#define CPU_ZERO(s) __builtin_memset((s), 0, sizeof(cpuset_t))
#define CPU_SET(n, s) ((s)->__bits[(n) / (8 * sizeof(unsigned long))] |= 1ul << ((n) % (8 * sizeof(unsigned long))))
#define CPU_CLR(n, s) ((s)->__bits[(n) / (8 * sizeof(unsigned long))] &= ~(1ul << ((n) % (8 * sizeof(unsigned long)))))
#define CPU_ISSET(n, s) (((s)->__bits[(n) / (8 * sizeof(unsigned long))] >> ((n) % (8 * sizeof(unsigned long)))) & 1ul)

typedef int cpulevel_t;
typedef int cpuwhich_t;
typedef long cpusetid_t;
#define CPU_LEVEL_ROOT 1
#define CPU_LEVEL_CPUSET 2
#define CPU_LEVEL_WHICH 3
#define CPU_WHICH_TID 1
#define CPU_WHICH_PID 2

int cpuset_setaffinity(cpulevel_t level, cpuwhich_t which, id_t id, size_t setsize, const cpuset_t* mask);
int cpuset_getaffinity(cpulevel_t level, cpuwhich_t which, id_t id, size_t setsize, cpuset_t* mask);

#ifdef __cplusplus
}
#endif
