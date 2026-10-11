// PS4 port (AI-assisted): the Vulkan window system PCSX2 and the frontend use on the PS5
// (VK_KHR_surface, VK_KHR_display, VK_KHR_swapchain), on top of RADV and VideoOut.
//
// RADV on GNM (CEMUPS4/DolphinPS4 port/mesa/ac_ps4_drm.c) has no window system. PS5SX2 drives the
// display through VK_KHR_display (VKSwapChain.cpp CreateOrbisDisplaySurface, frontend/fe_ps5.cpp),
// so rather than change those, this layer answers the window-system entry points itself and passes
// every other one to RADV: the link wraps vk_icdGetInstanceProcAddr (link-ps4.sh), whose callers
// then get this file's __wrap_ version.
//
// One display (VideoOut's main bus), one 1920x1080 60 Hz mode, one plane. A swapchain is 3 linear
// images in host-visible memory registered as VideoOut buffers (GPU address = CPU address);
// presenting queues a flip behind the GPU work submitted so far (ac_ps4_submit_flip), as
// DolphinPS4's VKSwapChain does. Needs proper testing on the console.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
PFN_vkVoidFunction __real_vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
// port/mesa/ac_ps4_drm.c: a VideoOut flip queued behind everything submitted so far.
int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);
}

// OpenOrbis' VideoOut.h has the A8B8G8R8 format only.
#ifndef ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8R8G8B8_SRGB
#define ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8R8G8B8_SRGB 0x80000000
#endif

namespace
{
constexpr uint32_t kWidth = 1920, kHeight = 1080, kRefreshMilliHz = 60000;
constexpr uint32_t kImages = 3;
constexpr VkDeviceSize kBufferAlign = 2 << 20; // VideoOut buffers: 2 MiB aligned

// Non-dispatchable handles of the one display, its mode and plane.
const VkDisplayKHR kDisplay = reinterpret_cast<VkDisplayKHR>(uintptr_t{0x50534431}); // "PSD1"
const VkDisplayModeKHR kMode = reinterpret_cast<VkDisplayModeKHR>(uintptr_t{0x50534D31}); // "PSM1"

std::mutex s_lock;
VkInstance s_instance = VK_NULL_HANDLE;
VkPhysicalDevice s_physical = VK_NULL_HANDLE;
VkDevice s_device = VK_NULL_HANDLE;
VkQueue s_queue = VK_NULL_HANDLE;
uint32_t s_queue_family = 0; // s_queue's family
std::vector<std::string> s_added_instance_exts, s_added_device_exts;

int s_video = -1;
OrbisKernelEqueue s_flip_queue;
int64_t s_flip_arg = 0; // the last flip queued; VideoOut reports the last one shown

struct Surface
{
	VkExtent2D extent;
};

struct Swapchain
{
	VkDevice device = VK_NULL_HANDLE;
	VkExtent2D extent = {};
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
	VkImage images[kImages] = {};
	VkDeviceMemory memory[kImages] = {};
	int64_t shown_by[kImages] = {}; // the flip that last put each image on screen (0: never)
	uint32_t next = 0;
	int buffer_set = -1;
	int first = 0; // its first VideoOut buffer slot (a group of kSlotGroup)
	bool retired = false;
};
Swapchain* s_active = nullptr; // the swapchain whose buffers VideoOut has

// VideoOut has 16 buffer slots, taken here in groups of 4. A buffer set VideoOut won't unregister yet (one of its buffers
// is on screen: 0x80290009 when the shelf handed the display to PCSX2 in v00.06) keeps its group until a later try, and
// the next swapchain takes another group.
constexpr int kSlotGroup = 4, kSlotGroups = 4;
static_assert(kImages <= kSlotGroup, "a swapchain's images fit in one slot group");
bool s_group_used[kSlotGroups] = {};
struct RetiredSet
{
	int set, first;
};
std::vector<RetiredSet> s_retired;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void RetryRetired()
{
	for (size_t i = 0; i < s_retired.size();)
	{
		const int r = sceVideoOutUnregisterBuffers(s_video, s_retired[i].set);
		if (r < 0)
		{
			i++;
			continue;
		}
		Log("old buffer set %d (slots %d+) unregistered now", s_retired[i].set, s_retired[i].first);
		s_group_used[s_retired[i].first / kSlotGroup] = false;
		s_retired.erase(s_retired.begin() + static_cast<long>(i));
	}
}

// RADV's own entry points.
PFN_vkGetDeviceProcAddr s_gdpa = nullptr;
PFN_vkEnumerateInstanceExtensionProperties s_enum_instance_exts = nullptr;
PFN_vkCreateInstance s_create_instance = nullptr;
PFN_vkEnumerateDeviceExtensionProperties s_enum_device_exts = nullptr;
PFN_vkCreateDevice s_create_device = nullptr;
PFN_vkGetDeviceQueue s_get_device_queue = nullptr;
PFN_vkGetDeviceQueue2 s_get_device_queue2 = nullptr;

template <typename T>
T Device(VkDevice device, const char* name)
{
	return reinterpret_cast<T>(s_gdpa(device, name));
}

template <typename T>
T Instance(const char* name)
{
	return reinterpret_cast<T>(__real_vk_icdGetInstanceProcAddr(s_instance, name));
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...)
{
	char line[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	printf("[ps4wsi] %s\n", line);
	fflush(stdout);
}

bool OpenVideoOut()
{
	if (s_video >= 0)
		return true;
	s_video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
	if (s_video < 0)
	{
		Log("sceVideoOutOpen failed: %#x", static_cast<unsigned>(s_video));
		return false;
	}
	sceKernelCreateEqueue(&s_flip_queue, "pcsx2 flip");
	sceVideoOutAddFlipEvent(s_flip_queue, s_video, nullptr);
	Log("VideoOut open (handle %d)", s_video);
	return true;
}

int64_t ShownFlip()
{
	OrbisVideoOutFlipStatus status = {};
	if (sceVideoOutGetFlipStatus(s_video, &status) < 0)
		return s_flip_arg; // can't tell: don't wait forever
	return status.flipArg;
}

// Waits until VideoOut shows flip `arg` or a later one (or until `timeout_ns`).
bool WaitForFlip(int64_t arg, uint64_t timeout_ns)
{
	const uint64_t start = sceKernelGetProcessTime();
	while (ShownFlip() < arg)
	{
		if (timeout_ns != UINT64_MAX && (sceKernelGetProcessTime() - start) * 1000 >= timeout_ns)
			return false;
		OrbisKernelEvent ev;
		int count = 0;
		OrbisKernelUseconds timeout = 5000;
		sceKernelWaitEqueue(s_flip_queue, &ev, 1, &count, &timeout);
	}
	return true;
}

void ReleaseBuffers(Swapchain* sc)
{
	if (sc->buffer_set >= 0)
	{
		WaitForFlip(s_flip_arg, 200000000ull); // the last flip on screen, so no buffer is pending
		const int r = sceVideoOutUnregisterBuffers(s_video, sc->buffer_set);
		Log("buffers unregistered (%#x)", static_cast<unsigned>(r));
		if (r < 0)
			s_retired.push_back({sc->buffer_set, sc->first}); // tried again at the next swapchain
		else
			s_group_used[sc->first / kSlotGroup] = false;
		sc->buffer_set = -1;
	}
	if (s_active == sc)
		s_active = nullptr;
}

void Append(std::vector<VkExtensionProperties>* list, const char* name, uint32_t version, std::vector<std::string>* added)
{
	for (const VkExtensionProperties& e : *list)
		if (std::strcmp(e.extensionName, name) == 0)
			return;
	VkExtensionProperties e = {};
	std::snprintf(e.extensionName, sizeof(e.extensionName), "%s", name);
	e.specVersion = version;
	list->push_back(e);
	if (std::find(added->begin(), added->end(), name) == added->end())
		added->push_back(name);
}

VkResult Fill(const std::vector<VkExtensionProperties>& list, uint32_t* count, VkExtensionProperties* out)
{
	if (!out)
	{
		*count = static_cast<uint32_t>(list.size());
		return VK_SUCCESS;
	}
	const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(list.size()));
	std::copy(list.begin(), list.begin() + n, out);
	*count = n;
	return n < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

// The extension names minus the ones this layer provides (RADV would refuse them).
std::vector<const char*> Strip(const char* const* names, uint32_t count, const std::vector<std::string>& ours)
{
	std::vector<const char*> kept;
	for (uint32_t i = 0; i < count; i++)
		if (std::find(ours.begin(), ours.end(), names[i]) == ours.end())
			kept.push_back(names[i]);
	return kept;
}

// --- instance ------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(
	const char* layer, uint32_t* count, VkExtensionProperties* out)
{
	if (layer)
		return s_enum_instance_exts(layer, count, out);
	uint32_t n = 0;
	s_enum_instance_exts(nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> list(n);
	s_enum_instance_exts(nullptr, &n, list.data());
	list.resize(n);
	Append(&list, VK_KHR_SURFACE_EXTENSION_NAME, 25, &s_added_instance_exts);
	Append(&list, VK_KHR_DISPLAY_EXTENSION_NAME, 23, &s_added_instance_exts);
	return Fill(list, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
	const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc, VkInstance* instance)
{
	if (s_added_instance_exts.empty())
	{
		uint32_t n = 0;
		EnumerateInstanceExtensionProperties(nullptr, &n, nullptr); // fills s_added_instance_exts
	}
	const std::vector<const char*> kept =
		Strip(info->ppEnabledExtensionNames, info->enabledExtensionCount, s_added_instance_exts);
	VkInstanceCreateInfo copy = *info;
	copy.enabledExtensionCount = static_cast<uint32_t>(kept.size());
	copy.ppEnabledExtensionNames = kept.data();
	const VkResult r = s_create_instance(&copy, alloc, instance);
	if (r == VK_SUCCESS)
		s_instance = *instance;
	Log("vkCreateInstance = %d (%u extensions asked, %zu passed to RADV)", r, info->enabledExtensionCount, kept.size());
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
	VkPhysicalDevice physical, const char* layer, uint32_t* count, VkExtensionProperties* out)
{
	if (layer)
		return s_enum_device_exts(physical, layer, count, out);
	uint32_t n = 0;
	s_enum_device_exts(physical, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> list(n);
	s_enum_device_exts(physical, nullptr, &n, list.data());
	list.resize(n);
	Append(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME, 70, &s_added_device_exts);
	return Fill(list, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
	const VkAllocationCallbacks* alloc, VkDevice* device)
{
	if (s_added_device_exts.empty())
	{
		uint32_t n = 0;
		EnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
	}
	const std::vector<const char*> kept =
		Strip(info->ppEnabledExtensionNames, info->enabledExtensionCount, s_added_device_exts);
	VkDeviceCreateInfo copy = *info;
	copy.enabledExtensionCount = static_cast<uint32_t>(kept.size());
	copy.ppEnabledExtensionNames = kept.data();
	const VkResult r = s_create_device(physical, &copy, alloc, device);
	if (r == VK_SUCCESS)
	{
		s_physical = physical;
		s_device = *device;
		s_queue = VK_NULL_HANDLE; // the new device's first queue, not an earlier device's (paraLLEl-GS makes its own)
	}
	Log("vkCreateDevice = %d (%u extensions asked, %zu passed to RADV)", r, info->enabledExtensionCount, kept.size());
	return r;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
	s_get_device_queue(device, family, index, queue);
	if (!s_queue)
	{
		s_queue = *queue;
		s_queue_family = family;
	}
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
{
	s_get_device_queue2(device, info, queue);
	if (!s_queue)
	{
		s_queue = *queue;
		s_queue_family = info->queueFamilyIndex;
	}
}

// --- display ---------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceDisplayPropertiesKHR(
	VkPhysicalDevice, uint32_t* count, VkDisplayPropertiesKHR* out)
{
	if (!out)
	{
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	*out = {};
	out->display = kDisplay;
	out->displayName = "PS4 VideoOut";
	out->physicalDimensions = {1018, 573}; // a 46" TV; nothing uses it
	out->physicalResolution = {kWidth, kHeight};
	out->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceDisplayPlanePropertiesKHR(
	VkPhysicalDevice, uint32_t* count, VkDisplayPlanePropertiesKHR* out)
{
	if (!out)
	{
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	out->currentDisplay = kDisplay;
	out->currentStackIndex = 0;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetDisplayPlaneSupportedDisplaysKHR(
	VkPhysicalDevice, uint32_t plane, uint32_t* count, VkDisplayKHR* out)
{
	if (plane != 0)
	{
		*count = 0;
		return VK_SUCCESS;
	}
	if (!out)
	{
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	out[0] = kDisplay;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetDisplayModePropertiesKHR(
	VkPhysicalDevice, VkDisplayKHR, uint32_t* count, VkDisplayModePropertiesKHR* out)
{
	if (!out)
	{
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	out->displayMode = kMode;
	out->parameters.visibleRegion = {kWidth, kHeight};
	out->parameters.refreshRate = kRefreshMilliHz;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDisplayModeKHR(
	VkPhysicalDevice, VkDisplayKHR, const VkDisplayModeCreateInfoKHR*, const VkAllocationCallbacks*, VkDisplayModeKHR*)
{
	return VK_ERROR_INITIALIZATION_FAILED; // the one mode only
}

VKAPI_ATTR VkResult VKAPI_CALL GetDisplayPlaneCapabilitiesKHR(
	VkPhysicalDevice, VkDisplayModeKHR, uint32_t, VkDisplayPlaneCapabilitiesKHR* caps)
{
	*caps = {};
	caps->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	caps->minDstExtent = caps->maxDstExtent = caps->minSrcExtent = caps->maxSrcExtent = {kWidth, kHeight};
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDisplayPlaneSurfaceKHR(
	VkInstance, const VkDisplaySurfaceCreateInfoKHR* info, const VkAllocationCallbacks*, VkSurfaceKHR* surface)
{
	Surface* s = new Surface{info->imageExtent.width ? info->imageExtent : VkExtent2D{kWidth, kHeight}};
	*surface = reinterpret_cast<VkSurfaceKHR>(s);
	Log("display surface %ux%u", s->extent.width, s->extent.height);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks*)
{
	delete reinterpret_cast<Surface*>(surface);
}

// --- surface ---------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* supported)
{
	*supported = VK_TRUE;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(
	VkPhysicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* caps)
{
	const VkExtent2D extent = reinterpret_cast<Surface*>(surface)->extent;
	*caps = {};
	caps->minImageCount = caps->maxImageCount = kImages;
	caps->currentExtent = caps->minImageExtent = caps->maxImageExtent = extent;
	caps->maxImageArrayLayers = 1;
	caps->supportedTransforms = caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	caps->supportedUsageFlags =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2KHR(
	VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info, VkSurfaceCapabilities2KHR* caps)
{
	return GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, info->surface, &caps->surfaceCapabilities);
}

// R8G8B8A8 is VideoOut's A8B8G8R8 and B8G8R8A8 its A8R8G8B8 (the bytes in memory order). The _SRGB ones are the same
// buffers written through an sRGB view (the shader's linear output encoded on store); VideoOut shows the bytes the same
// way. Listed after the UNORM ones, which PCSX2 and the frontend keep taking; paraLLEl-GS's Granite asks for an sRGB
// backbuffer and found none without them.
constexpr VkSurfaceFormatKHR kFormats[] = {
	{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(
	VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkSurfaceFormatKHR* out)
{
	const uint32_t total = static_cast<uint32_t>(std::size(kFormats));
	if (!out)
	{
		*count = total;
		return VK_SUCCESS;
	}
	const uint32_t n = std::min(*count, total);
	std::copy(kFormats, kFormats + n, out);
	*count = n;
	return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

// FIFO: flips at vblank. IMMEDIATE: flips at the next horizontal sync (no vsync).
constexpr VkPresentModeKHR kPresentModes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfacePresentModesKHR(
	VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkPresentModeKHR* out)
{
	const uint32_t total = static_cast<uint32_t>(std::size(kPresentModes));
	if (!out)
	{
		*count = total;
		return VK_SUCCESS;
	}
	const uint32_t n = std::min(*count, total);
	std::copy(kPresentModes, kPresentModes + n, out);
	*count = n;
	return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

// --- swapchain -------------------------------------------------------------------------------------

void DestroyImages(Swapchain* sc)
{
	const auto destroy_image = Device<PFN_vkDestroyImage>(sc->device, "vkDestroyImage");
	const auto free_memory = Device<PFN_vkFreeMemory>(sc->device, "vkFreeMemory");
	for (uint32_t i = 0; i < kImages; i++)
	{
		if (sc->images[i])
			destroy_image(sc->device, sc->images[i], nullptr);
		if (sc->memory[i])
			free_memory(sc->device, sc->memory[i], nullptr);
		sc->images[i] = VK_NULL_HANDLE;
		sc->memory[i] = VK_NULL_HANDLE;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(
	VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks*, VkSwapchainKHR* out)
{
	std::lock_guard<std::mutex> guard(s_lock);
	if (!OpenVideoOut())
		return VK_ERROR_INITIALIZATION_FAILED;
	uint32_t pixel_format;
	if (info->imageFormat == VK_FORMAT_R8G8B8A8_UNORM || info->imageFormat == VK_FORMAT_R8G8B8A8_SRGB)
		pixel_format = ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB;
	else if (info->imageFormat == VK_FORMAT_B8G8R8A8_UNORM || info->imageFormat == VK_FORMAT_B8G8R8A8_SRGB)
		pixel_format = ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8R8G8B8_SRGB;
	else
	{
		Log("vkCreateSwapchainKHR: format %d not offered", info->imageFormat);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	// The old swapchain's buffers leave VideoOut first: the new ones take the same slots.
	if (s_active)
	{
		ReleaseBuffers(s_active);
	}
	if (info->oldSwapchain)
		reinterpret_cast<Swapchain*>(info->oldSwapchain)->retired = true;

	const auto create_image = Device<PFN_vkCreateImage>(device, "vkCreateImage");
	const auto get_requirements = Device<PFN_vkGetImageMemoryRequirements>(device, "vkGetImageMemoryRequirements");
	const auto allocate = Device<PFN_vkAllocateMemory>(device, "vkAllocateMemory");
	const auto map = Device<PFN_vkMapMemory>(device, "vkMapMemory");
	const auto bind = Device<PFN_vkBindImageMemory>(device, "vkBindImageMemory");
	const auto layout_of = Device<PFN_vkGetImageSubresourceLayout>(device, "vkGetImageSubresourceLayout");
	VkPhysicalDeviceMemoryProperties props;
	Instance<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(s_physical, &props);

	Swapchain* sc = new Swapchain();
	sc->device = device;
	sc->extent = info->imageExtent;
	sc->format = info->imageFormat;
	sc->present_mode = info->presentMode;
	void* addresses[kImages];
	uint32_t pitch = sc->extent.width;
	for (uint32_t i = 0; i < kImages; i++)
	{
		VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = info->imageFormat;
		ici.extent = {sc->extent.width, sc->extent.height, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_LINEAR;
		ici.usage = info->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		VkResult r = create_image(device, &ici, nullptr, &sc->images[i]);
		VkMemoryRequirements req = {};
		if (r == VK_SUCCESS)
			get_requirements(device, sc->images[i], &req);
		uint32_t type = UINT32_MAX;
		const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t t = 0; t < props.memoryTypeCount && type == UINT32_MAX; t++)
			if ((req.memoryTypeBits & (1u << t)) && (props.memoryTypes[t].propertyFlags & want) == want)
				type = t;
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size + kBufferAlign; // bound at a 2 MiB aligned offset
		mai.memoryTypeIndex = type;
		if (r == VK_SUCCESS)
			r = type == UINT32_MAX ? VK_ERROR_OUT_OF_DEVICE_MEMORY : allocate(device, &mai, nullptr, &sc->memory[i]);
		uint8_t* base = nullptr;
		if (r == VK_SUCCESS)
			r = map(device, sc->memory[i], 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&base));
		if (r != VK_SUCCESS)
		{
			Log("vkCreateSwapchainKHR: image %u failed (%d)", i, r);
			DestroyImages(sc);
			delete sc;
			return r;
		}
		const uintptr_t at = reinterpret_cast<uintptr_t>(base);
		VkDeviceSize offset = (at + kBufferAlign - 1) / kBufferAlign * kBufferAlign - at;
		offset = (offset + req.alignment - 1) / req.alignment * req.alignment;
		bind(device, sc->images[i], sc->memory[i], offset);
		const VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
		VkSubresourceLayout layout;
		layout_of(device, sc->images[i], &sub, &layout);
		addresses[i] = base + offset + layout.offset;
		pitch = static_cast<uint32_t>(layout.rowPitch / 4);
	}

	OrbisVideoOutBufferAttribute attribute;
	sceVideoOutSetBufferAttribute(&attribute, pixel_format, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, sc->extent.width,
		sc->extent.height, pitch);
	RetryRetired();
	int group = 0;
	while (group < kSlotGroups && s_group_used[group])
		group++;
	if (group == kSlotGroups)
	{
		Log("vkCreateSwapchainKHR: every VideoOut slot group is still in use");
		DestroyImages(sc);
		delete sc;
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	sc->first = group * kSlotGroup;
	sc->buffer_set = sceVideoOutRegisterBuffers(s_video, sc->first, addresses, kImages, &attribute);
	if (sc->buffer_set >= 0)
		s_group_used[group] = true;
	if (sc->buffer_set < 0)
	{
		Log("sceVideoOutRegisterBuffers failed: %#x", static_cast<unsigned>(sc->buffer_set));
		DestroyImages(sc);
		delete sc;
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	s_active = sc;
	*out = reinterpret_cast<VkSwapchainKHR>(sc);
	Log("swapchain: %u images %ux%u in slots %d+, format %d, pitch %u, %s", kImages, sc->extent.width, sc->extent.height,
		sc->first, info->imageFormat, pitch, sc->present_mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "no vsync");
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks*)
{
	if (!swapchain)
		return;
	std::lock_guard<std::mutex> guard(s_lock);
	Swapchain* sc = reinterpret_cast<Swapchain*>(swapchain);
	if (s_queue)
		Device<PFN_vkQueueWaitIdle>(device, "vkQueueWaitIdle")(s_queue);
	ReleaseBuffers(sc);
	DestroyImages(sc);
	delete sc;
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice, VkSwapchainKHR swapchain, uint32_t* count, VkImage* out)
{
	const Swapchain* sc = reinterpret_cast<const Swapchain*>(swapchain);
	if (!out)
	{
		*count = kImages;
		return VK_SUCCESS;
	}
	const uint32_t n = std::min(*count, kImages);
	std::copy(sc->images, sc->images + n, out);
	*count = n;
	return n < kImages ? VK_INCOMPLETE : VK_SUCCESS;
}

// No semaphores reach RADV for the window system (v00.05: the shelf's first frame lost the device, with an empty
// submission signalling the acquire semaphore; CEMUPS4 took semaphores out of Cemu's presenting for the same reason).
// There is one in-order queue, acquiring waits on the CPU until the image is off screen, and the flip is queued behind
// everything submitted: nothing is left for a semaphore to order. So acquire semaphores are only noted here, and the
// wrapped vkQueueSubmit drops the waits on them, and the signals of semaphores the app presents with. (A present
// semaphore's first signal, before it is known as one, goes through and stays signalled: nothing ever waits on it.)
std::mutex s_semaphore_lock;
std::unordered_set<VkSemaphore> s_acquire_semaphores; // handed to acquire, not yet waited on
std::unordered_set<VkSemaphore> s_present_semaphores; // seen in a present's waits

// A recorded command buffer with nothing in it, for submissions that have none. The GNM winsys refuses a submission
// without an IB (ac_ps4_drm.c ac_drm_cs_submit_raw2: -EINVAL), and RADV then calls the device lost: paraLLEl-GS's
// Granite lost it on its second submission, an empty one that only signals a fence. An empty command buffer still
// becomes a NOP IB, so its fences and semaphores signal as usual. One per device, reusable while in flight.
VkCommandBuffer s_empty_cb = VK_NULL_HANDLE;
VkDevice s_empty_cb_device = VK_NULL_HANDLE;

VkCommandBuffer EmptyCommandBuffer()
{
	std::lock_guard<std::mutex> guard(s_lock);
	if (s_empty_cb && s_empty_cb_device == s_device)
		return s_empty_cb;
	VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.queueFamilyIndex = s_queue_family;
	VkCommandPool pool = VK_NULL_HANDLE;
	if (Device<PFN_vkCreateCommandPool>(s_device, "vkCreateCommandPool")(s_device, &pool_info, nullptr, &pool) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	alloc.commandPool = pool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	VkCommandBuffer cb = VK_NULL_HANDLE;
	if (Device<PFN_vkAllocateCommandBuffers>(s_device, "vkAllocateCommandBuffers")(s_device, &alloc, &cb) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	Device<PFN_vkBeginCommandBuffer>(s_device, "vkBeginCommandBuffer")(cb, &begin);
	Device<PFN_vkEndCommandBuffer>(s_device, "vkEndCommandBuffer")(cb);
	s_empty_cb = cb; // the pool lives as long as the device (the next device makes its own)
	s_empty_cb_device = s_device;
	Log("empty command buffer ready for submissions without one");
	return cb;
}

// Only for a fence handed to acquire (PCSX2 and the frontend pass none): signalled by an empty submission.
VkResult FenceSubmit(VkDevice device, VkFence fence)
{
	VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	VkCommandBuffer cb = EmptyCommandBuffer();
	submit.commandBufferCount = cb ? 1 : 0;
	submit.pCommandBuffers = &cb;
	return Device<PFN_vkQueueSubmit>(device, "vkQueueSubmit")(s_queue, 1, &submit, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
	static PFN_vkQueueSubmit s_real = Device<PFN_vkQueueSubmit>(s_device, "vkQueueSubmit");
	std::vector<VkSubmitInfo> copies;
	std::vector<std::vector<VkSemaphore>> waits(count), signals(count);
	std::vector<std::vector<VkPipelineStageFlags>> stages(count);
	{
		std::lock_guard<std::mutex> guard(s_semaphore_lock);
		if (s_acquire_semaphores.empty() && s_present_semaphores.empty())
			return s_real(queue, count, submits, fence);
		bool changed = false;
		for (uint32_t i = 0; i < count; i++)
		{
			const VkSubmitInfo& in = submits[i];
			for (uint32_t w = 0; w < in.waitSemaphoreCount; w++)
			{
				if (s_acquire_semaphores.erase(in.pWaitSemaphores[w]))
				{
					changed = true;
					continue;
				}
				waits[i].push_back(in.pWaitSemaphores[w]);
				stages[i].push_back(in.pWaitDstStageMask[w]);
			}
			for (uint32_t s = 0; s < in.signalSemaphoreCount; s++)
			{
				if (s_present_semaphores.count(in.pSignalSemaphores[s]))
				{
					changed = true;
					continue;
				}
				signals[i].push_back(in.pSignalSemaphores[s]);
			}
		}
		if (!changed)
			return s_real(queue, count, submits, fence);
	}
	copies.assign(submits, submits + count);
	for (uint32_t i = 0; i < count; i++)
	{
		copies[i].waitSemaphoreCount = static_cast<uint32_t>(waits[i].size());
		copies[i].pWaitSemaphores = waits[i].data();
		copies[i].pWaitDstStageMask = stages[i].data();
		copies[i].signalSemaphoreCount = static_cast<uint32_t>(signals[i].size());
		copies[i].pSignalSemaphores = signals[i].data();
	}
	return s_real(queue, count, copies.data(), fence);
}

// The same for vkQueueSubmit2, which paraLLEl-GS's Granite submits everything with.
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{
	static PFN_vkQueueSubmit2 s_real = [] {
		auto real = Device<PFN_vkQueueSubmit2>(s_device, "vkQueueSubmit2");
		return real ? real : Device<PFN_vkQueueSubmit2>(s_device, "vkQueueSubmit2KHR");
	}();
	std::vector<VkSubmitInfo2> copies;
	std::vector<std::vector<VkSemaphoreSubmitInfo>> waits(count), signals(count);
	bool any_empty = false;
	for (uint32_t i = 0; i < count; i++)
		any_empty |= submits[i].commandBufferInfoCount == 0;
	VkCommandBufferSubmitInfo empty_cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	if (any_empty)
		empty_cb.commandBuffer = EmptyCommandBuffer();
	// Submissions without a command buffer get the empty one (see EmptyCommandBuffer).
	const auto fill_empty = [&](std::vector<VkSubmitInfo2>& list) {
		if (!empty_cb.commandBuffer)
			return;
		for (VkSubmitInfo2& s : list)
		{
			if (s.commandBufferInfoCount == 0)
			{
				s.commandBufferInfoCount = 1;
				s.pCommandBufferInfos = &empty_cb;
			}
		}
	};
	{
		std::lock_guard<std::mutex> guard(s_semaphore_lock);
		if (s_acquire_semaphores.empty() && s_present_semaphores.empty())
		{
			if (!any_empty)
				return s_real(queue, count, submits, fence);
			copies.assign(submits, submits + count);
			fill_empty(copies);
			return s_real(queue, count, copies.data(), fence);
		}
		bool changed = any_empty;
		for (uint32_t i = 0; i < count; i++)
		{
			const VkSubmitInfo2& in = submits[i];
			for (uint32_t w = 0; w < in.waitSemaphoreInfoCount; w++)
			{
				if (s_acquire_semaphores.erase(in.pWaitSemaphoreInfos[w].semaphore))
				{
					changed = true;
					continue;
				}
				waits[i].push_back(in.pWaitSemaphoreInfos[w]);
			}
			for (uint32_t s = 0; s < in.signalSemaphoreInfoCount; s++)
			{
				if (s_present_semaphores.count(in.pSignalSemaphoreInfos[s].semaphore))
				{
					changed = true;
					continue;
				}
				signals[i].push_back(in.pSignalSemaphoreInfos[s]);
			}
		}
		if (!changed)
			return s_real(queue, count, submits, fence);
	}
	copies.assign(submits, submits + count);
	for (uint32_t i = 0; i < count; i++)
	{
		copies[i].waitSemaphoreInfoCount = static_cast<uint32_t>(waits[i].size());
		copies[i].pWaitSemaphoreInfos = waits[i].data();
		copies[i].signalSemaphoreInfoCount = static_cast<uint32_t>(signals[i].size());
		copies[i].pSignalSemaphoreInfos = signals[i].data();
	}
	fill_empty(copies);
	return s_real(queue, count, copies.data(), fence);
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
	VkSemaphore semaphore, VkFence fence, uint32_t* index)
{
	Swapchain* sc = reinterpret_cast<Swapchain*>(swapchain);
	if (sc->retired || sc->buffer_set < 0)
		return VK_ERROR_OUT_OF_DATE_KHR;
	// Round robin: the next image, once a later flip has replaced it on screen.
	const uint32_t i = sc->next;
	if (sc->shown_by[i] != 0 && !WaitForFlip(sc->shown_by[i] + 1, timeout))
		return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
	sc->next = (i + 1) % kImages;
	*index = i;
	if (semaphore)
	{
		std::lock_guard<std::mutex> guard(s_semaphore_lock);
		s_acquire_semaphores.insert(semaphore);
	}
	if (fence)
		return FenceSubmit(device, fence);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info)
{
	s_queue = queue;
	{
		std::lock_guard<std::mutex> guard(s_semaphore_lock);
		for (uint32_t w = 0; w < info->waitSemaphoreCount; w++)
			s_present_semaphores.insert(info->pWaitSemaphores[w]);
	}
	VkResult result = VK_SUCCESS;
	for (uint32_t n = 0; n < info->swapchainCount; n++)
	{
		Swapchain* sc = reinterpret_cast<Swapchain*>(info->pSwapchains[n]);
		VkResult r = VK_SUCCESS;
		if (sc->retired || sc->buffer_set < 0)
			r = VK_ERROR_OUT_OF_DATE_KHR;
		if (r == VK_SUCCESS)
		{
			const uint32_t image = info->pImageIndices[n];
			const unsigned mode =
				sc->present_mode == VK_PRESENT_MODE_FIFO_KHR ? ORBIS_VIDEO_OUT_FLIP_VSYNC : ORBIS_VIDEO_OUT_FLIP_HSYNC;
			const int64_t arg = ++s_flip_arg;
			int flip = ac_ps4_submit_flip(s_video, static_cast<unsigned>(sc->first) + image, mode, arg);
			if (flip != 0)
			{
				// No GPU-side flip: wait for the GPU here, then flip from the CPU.
				Device<PFN_vkQueueWaitIdle>(sc->device, "vkQueueWaitIdle")(queue);
				flip = sceVideoOutSubmitFlip(s_video, sc->first + static_cast<int>(image), mode, arg);
			}
			if (flip < 0)
			{
				static int s_reported;
				if (s_reported++ < 5)
					Log("flip %lld of image %u failed: %#x", static_cast<long long>(arg), image, static_cast<unsigned>(flip));
				r = VK_ERROR_SURFACE_LOST_KHR;
			}
			else
			{
				sc->shown_by[image] = arg;
				static int s_logged;
				if (s_logged++ < 3)
					Log("present %lld: image %u", static_cast<long long>(arg), image);
			}
		}
		if (info->pResults)
			info->pResults[n] = r;
		if (r != VK_SUCCESS && result == VK_SUCCESS)
			result = r;
	}
	return result;
}

PFN_vkVoidFunction Lookup(const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name)
{
	if (PFN_vkVoidFunction ours = Lookup(name))
		return ours;
	return s_gdpa(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name);

PFN_vkVoidFunction Lookup(const char* name)
{
#define OURS(fn) {"vk" #fn, reinterpret_cast<PFN_vkVoidFunction>(&fn)}
	static const struct
	{
		const char* name;
		PFN_vkVoidFunction fn;
	} kTable[] = {
		OURS(GetInstanceProcAddr),
		OURS(GetDeviceProcAddr),
		OURS(EnumerateInstanceExtensionProperties),
		OURS(CreateInstance),
		OURS(EnumerateDeviceExtensionProperties),
		OURS(CreateDevice),
		OURS(GetDeviceQueue),
		OURS(GetDeviceQueue2),
		OURS(GetPhysicalDeviceDisplayPropertiesKHR),
		OURS(GetPhysicalDeviceDisplayPlanePropertiesKHR),
		OURS(GetDisplayPlaneSupportedDisplaysKHR),
		OURS(GetDisplayModePropertiesKHR),
		OURS(CreateDisplayModeKHR),
		OURS(GetDisplayPlaneCapabilitiesKHR),
		OURS(CreateDisplayPlaneSurfaceKHR),
		OURS(DestroySurfaceKHR),
		OURS(GetPhysicalDeviceSurfaceSupportKHR),
		OURS(GetPhysicalDeviceSurfaceCapabilitiesKHR),
		OURS(GetPhysicalDeviceSurfaceCapabilities2KHR),
		OURS(GetPhysicalDeviceSurfaceFormatsKHR),
		OURS(GetPhysicalDeviceSurfacePresentModesKHR),
		OURS(CreateSwapchainKHR),
		OURS(DestroySwapchainKHR),
		OURS(GetSwapchainImagesKHR),
		OURS(AcquireNextImageKHR),
		OURS(QueuePresentKHR),
		OURS(QueueSubmit),
		OURS(QueueSubmit2),
	};
#undef OURS
	for (const auto& entry : kTable)
		if (std::strcmp(entry.name, name) == 0)
			return entry.fn;
	if (std::strcmp(name, "vkQueueSubmit2KHR") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&QueueSubmit2);
	return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name)
{
	if (!s_gdpa)
	{
		s_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(__real_vk_icdGetInstanceProcAddr(nullptr, "vkGetDeviceProcAddr"));
		s_enum_instance_exts = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
			__real_vk_icdGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
		s_create_instance =
			reinterpret_cast<PFN_vkCreateInstance>(__real_vk_icdGetInstanceProcAddr(nullptr, "vkCreateInstance"));
	}
	if (instance && !s_create_device)
	{
		s_instance = instance;
		s_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(__real_vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
		s_enum_device_exts = Instance<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
		s_create_device = Instance<PFN_vkCreateDevice>("vkCreateDevice");
		s_get_device_queue = Instance<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
		s_get_device_queue2 = Instance<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2");
	}
	if (PFN_vkVoidFunction ours = Lookup(name))
	{
		// Instance-level functions only make sense once there is an instance to pass on to.
		const bool needs_instance = std::strcmp(name, "vkGetInstanceProcAddr") != 0 &&
		                            std::strcmp(name, "vkEnumerateInstanceExtensionProperties") != 0 &&
		                            std::strcmp(name, "vkCreateInstance") != 0;
		if (!needs_instance || instance)
			return ours;
	}
	return __real_vk_icdGetInstanceProcAddr(instance, name);
}
} // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vk_icdGetInstanceProcAddr(VkInstance instance, const char* name)
{
	return GetInstanceProcAddr(instance, name);
}
