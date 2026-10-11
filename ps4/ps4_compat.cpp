// PS4 port (AI-assisted): C library pieces the PS5 SDK's FreeBSD libc has and OpenOrbis' doesn't.
// Needs proper testing on the console.

#include <sys/cpuset.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>

extern "C" {
// GSRenderer.cpp pins the process's other threads with this. Not available here yet: the pinning is
// skipped (it logs the error) and threads stay where CEMUPS4's runtime puts them (port/ps4_runtime.cpp
// spreads new threads over all of the app's cores).
int cpuset_setaffinity(cpulevel_t, cpuwhich_t, id_t, size_t, const cpuset_t*)
{
	errno = ENOSYS;
	return -1;
}

int cpuset_getaffinity(cpulevel_t, cpuwhich_t, id_t, size_t, cpuset_t*)
{
	errno = ENOSYS;
	return -1;
}

// CEMUPS4's runtime asks the emulator for its state in a crash report (port/ps4_crashlog.cpp, ps4_sampler.cpp);
// create-fself rejects a weak symbol nothing defines. PS5SX2 installs its own crash printer (ProsperoCrash.cpp), so
// these say nothing for now.
void ps4_crash_guest_info() {}
void ps4_crash_guest_regs(int) {}
void ps4_crash_jit_info(int, unsigned long long, int) {}
void ps4_crash_note() {}
void ps4_crash_restart() {}
void ps4_recompiler_status(int) {}
void ps4_hang_guest_threads(int) {}

// ---- the system libraries the eboot imports ----

int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceSysmoduleLoadModule(uint16_t id);
void ps4_boot_trace(const char* stage);

// A call into a system library whose module isn't loaded ends the app without a signal (PRX_NOT_RESOLVED_FUNCTION;
// DolphinPS4 and CEMUPS4 load theirs the same way). The PS5 loads a title's modules for it, so PS5SX2 never does: this
// runs first in main() (main-boot.cpp, ORBIS_PS4). The list is the eboot's imports (llvm-readelf -d: NEEDED), less
// libkernel, libSceLibcInternal and libSceSysmodule, which come with the process. Needs proper testing on the console.
void orbis_ps4_load_system_modules()
{
	struct Module
	{
		const char* name;
		uint32_t id;
		bool internal;
	};
	static const Module kModules[] = {
		{"SystemService", 0x80000010, true}, {"UserService", 0x80000011, true}, {"VideoOut", 0x80000022, true},
		{"AudioOut", 0x80000001, true},      {"Pad", 0x80000024, true},         {"Net", 0x8000001C, true},
		{"NetCtl", 0x80000009, true},        {"Ssl", 0x8000000B, true},         {"Http", 0x8000000A, true},
		{"CommonDialog", 0x80000018, true},  {"Keyboard", 0x80000008, true},    {"ImeDialog", 0x0096, false},
		{"Mouse", 0x00A9, false},            {"Random", 0x00BA, false},
	};
	int failed = 0;
	for (const Module& m : kModules)
	{
		const int rc = m.internal ? sceSysmoduleLoadModuleInternal(m.id) : sceSysmoduleLoadModule(static_cast<uint16_t>(m.id));
		if (rc != 0)
		{
			char line[128];
			std::snprintf(line, sizeof(line), "[pcsx2] loading system module %s (%#x) failed: %#x", m.name, m.id,
				static_cast<unsigned>(rc));
			ps4_boot_trace(line);
			failed++;
		}
	}
	char line[96];
	std::snprintf(line, sizeof(line), "[pcsx2] system modules loaded (%d of %zu failed)", failed, std::size(kModules));
	ps4_boot_trace(line);
}

// Fastmem maps views of the guest's RAM block (direct memory) a second time at fixed addresses (LnxHostSys.cpp,
// SharedMemoryMappingArea). The PS4 refuses such aliases (EBUSY, 0x80020010, on every page in v00.06, which then took
// minutes of retries) until the process enables them, as DolphinPS4's MemArenaPS4 does. 1 when aliasing is on.
int sceKernelEnableDmemAliasing(void);
int sceKernelEnableDmemAliasing2(void);
int g_orbis_ps4_dmem_aliasing = 0;
int orbis_ps4_enable_dmem_aliasing()
{
	const int r1 = sceKernelEnableDmemAliasing();
	const int r2 = r1 == 0 ? 0 : sceKernelEnableDmemAliasing2();
	g_orbis_ps4_dmem_aliasing = r1 == 0 || r2 == 0;
	char line[128];
	std::snprintf(line, sizeof(line), "[pcsx2] direct memory aliasing: %#x, aliasing2: %#x (fastmem %s)",
		static_cast<unsigned>(r1), static_cast<unsigned>(r2), g_orbis_ps4_dmem_aliasing ? "possible" : "off");
	ps4_boot_trace(line);
	return g_orbis_ps4_dmem_aliasing;
}

// ---- PS5 system calls PS5SX2 makes that the PS4 has no library for ----

constexpr int kNotSupported = static_cast<int>(0x80020016); // a SCE error code: the caller's failure path runs

// libSceHttp2 (frontend/fe_ps5.cpp's cover downloads): the PS4 has libSceHttp only. Every request fails, so the
// shelf shows the covers already on disk. TODO: port fe_ps5.cpp's downloads to libSceHttp or curl.
int sceHttp2Init(int, int, size_t, int) { return kNotSupported; }
int sceHttp2Term(int) { return 0; }
int sceHttp2CreateTemplate(int, const char*, int, int) { return kNotSupported; }
int sceHttp2DeleteTemplate(int) { return 0; }
int sceHttp2CreateRequestWithURL(int, const char*, const char*, uint64_t) { return kNotSupported; }
int sceHttp2DeleteRequest(int) { return 0; }
int sceHttp2SendRequest(int, const void*, size_t) { return kNotSupported; }
int sceHttp2GetStatusCode(int, int*) { return kNotSupported; }
int sceHttp2ReadData(int, void*, size_t) { return kNotSupported; }
int sceHttp2SetResolveTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetConnectTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetSendTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetRecvTimeOut(int, uint32_t) { return 0; }
int sceHttp2AbortRequest(int) { return 0; }
int sceHttp2SetTimeOut(int, uint32_t) { return 0; }
int sceHttp2SetAutoRedirect(int, int) { return 0; }

// The PS5's VideoOut buffer API (orbis-shims/demo_renderer.cpp: the CPU overlay and the plain game list). Refused,
// so those screens don't open; the game picks itself when it is the only one. TODO: the PS4's sceVideoOut*Buffers.
int sceVideoOutSetBufferAttribute2(void*, uint64_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t, uint64_t)
{
	return kNotSupported;
}
int sceVideoOutRegisterBuffers2(int32_t, int32_t, int32_t, const void*, int32_t, const void*, uint32_t, void*)
{
	return kNotSupported;
}

// The DualSense's trigger-rumble mode: the DualShock 4 has none.
int scePadSetVibrationMode(int32_t, int32_t) { return 0; }

// PS5 kernel queries, answered with the PS4's.
int sceKernelAvailableFlexibleMemorySize(size_t* size);
int sceKernelConfiguredFlexibleMemorySize(unsigned long long* size)
{
	size_t available = 0;
	const int rc = sceKernelAvailableFlexibleMemorySize(&available);
	*size = available;
	return rc;
}

int sceKernelGetSystemSwVersion(void* version);
int sceKernelGetProsperoSystemSwVersion(void* version)
{
	return sceKernelGetSystemSwVersion(version); // same layout: size, version string, version number
}
}
