/* PS5SX2 (2026-10-05, AI-assisted): libarchive's configuration for the texture pack downloads (ps5/frontend/fe_texpacks.cpp).
 * config_ps5.h is what libarchive 3.8.9's own CMake checks found with the PS5 payload SDK's toolchain (prospero-cmake),
 * with zlib switched on by hand at its end: the port builds zlib's inflate itself (ps5/third_party/zlib).
 * config_linux.h is the same build on Linux (Ubuntu 24.04, zlib), for the PC preview and its tests. */
#if defined(__PROSPERO__)
#include "config_ps5.h"
#elif defined(__PS4__)
#include "config_ps4.h" /* PS4 port (AI-assisted) */
#else
#include "config_linux.h"
#endif
