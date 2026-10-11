/* PS4 port (AI-assisted): FreeBSD's <sys/disk.h> ioctls libarchive asks a device's size with
 * (archive_read_open_filename.c); OpenOrbis' C library has no such header. */
#pragma once
#include <sys/ioctl.h>
#include <sys/types.h>
#define DIOCGSECTORSIZE _IOR('d', 128, unsigned int)
#define DIOCGMEDIASIZE _IOR('d', 129, off_t)
