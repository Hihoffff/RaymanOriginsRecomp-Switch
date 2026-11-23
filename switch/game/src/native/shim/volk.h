// Switch: no volk. The NVK package (libvulkan.a, mesa-switch) exports every
// vk* entry point directly, and volk's same-named function-pointer globals
// would collide with them at link time. vk_renderer.h includes <volk.h>; this
// shim (first on the include path) gives it plain Vulkan prototypes instead.
#pragma once
// vk_renderer.h defines VK_NO_PROTOTYPES for volk; NVK wants the prototypes.
#undef VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

inline VkResult volkInitialize() { return VK_SUCCESS; }
inline void volkInitializeCustom(PFN_vkGetInstanceProcAddr) {}
inline void volkLoadInstance(VkInstance) {}
inline void volkLoadDevice(VkDevice) {}
