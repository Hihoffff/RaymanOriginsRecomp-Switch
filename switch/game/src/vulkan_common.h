// Общий bootstrap Vulkan/NVK для режимов vulkan_probe и vulkan_clear
// (шаги 3.2–3.3 плана docs/PLAN_STAGE3_GRAPHICS.md).
//
// Header-only: оба режима собираются только при -DRAYMAN_SWITCH_NVK_ROOT=...
// Код опирается на заметки ветки `switch` upstream (runtime/switch,
// tools/native_renderer/vk_renderer.h): loaderless ICD, VK_NN_vi_surface,
// редирект stdout до любых вызовов WSI.

#pragma once

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace rayman_vk {

// Переменные окружения, которые выставляет runtime ветки `switch` upstream
// перед инициализацией NVK (экспериментальный драйвер на GM20B, отключение
// дискового кэша шейдеров Mesa — на Horizon нет нормальной ФС для кэша).
inline void PrepareEnvironment() {
  setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
  setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
  setenv("MESA_GLSL_CACHE_DISABLE", "1", 1);
  setenv("MESA_VK_ENABLE_SUBMIT_THREAD", "1", 1);
}

// Switch WSI NVK пишет диагностику в stdout. Консоли libnx у нас нет, stdout
// никуда не подключён — направляем его в stderr (nxlink / rayman.log).
inline bool RedirectStdoutToStderr(const char* tag) {
  std::fflush(stdout);
  if (dup2(fileno(stderr), fileno(stdout)) < 0) {
    std::fprintf(stderr, "%s FAIL: dup2 stdout->stderr\n", tag);
    return false;
  }
  clearerr(stdout);
  setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
  return true;
}

inline bool HasExtension(const std::vector<VkExtensionProperties>& list,
                         const char* name) {
  for (const auto& e : list) {
    if (std::strcmp(e.extensionName, name) == 0) return true;
  }
  return false;
}

// Создаёт VkInstance. При need_vi_surface=true требует VK_KHR_surface и
// VK_NN_vi_surface. Логирует результат с префиксом tag.
inline VkInstance CreateInstance(const char* tag, bool need_vi_surface) {
  uint32_t count = 0;
  VkResult r = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "%s FAIL: vkEnumerateInstanceExtensionProperties -> %d\n", tag, r);
    return VK_NULL_HANDLE;
  }
  std::vector<VkExtensionProperties> avail(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count, avail.data());

  std::string names;
  for (const auto& e : avail) {
    if (!names.empty()) names += ' ';
    names += e.extensionName;
  }
  std::fprintf(stderr, "%s instance extensions (%u): %s\n", tag, count, names.c_str());

  const bool has_surface = HasExtension(avail, VK_KHR_SURFACE_EXTENSION_NAME);
  const bool has_vi = HasExtension(avail, VK_NN_VI_SURFACE_EXTENSION_NAME);
  std::fprintf(stderr, "%s VK_KHR_surface: %s, VK_NN_vi_surface: %s\n", tag,
               has_surface ? "present" : "absent", has_vi ? "present" : "absent");
  if (need_vi_surface && !(has_surface && has_vi)) {
    std::fprintf(stderr, "%s FAIL: VK_NN_vi_surface not available\n", tag);
    return VK_NULL_HANDLE;
  }

  std::vector<const char*> exts;
  if (has_surface) exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  if (has_vi) exts.push_back(VK_NN_VI_SURFACE_EXTENSION_NAME);

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "rayman-switch";
  app.applicationVersion = 1;
  app.pEngineName = "rexglue-switch";
  app.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  info.ppEnabledExtensionNames = exts.data();

  VkInstance instance = VK_NULL_HANDLE;
  r = vkCreateInstance(&info, nullptr, &instance);
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "%s FAIL: vkCreateInstance -> %d\n", tag, r);
    return VK_NULL_HANDLE;
  }
  std::fprintf(stderr, "%s instance ok\n", tag);
  return instance;
}

// Перечисляет физические устройства, логирует каждое, возвращает первое.
// Ожидаемое на Tegra X1 имя: "NVIDIA Tegra X1 (NVK GM20B)" (заявление
// upstream, на железе нами не проверено).
inline VkPhysicalDevice PickPhysicalDevice(const char* tag, VkInstance instance) {
  uint32_t count = 0;
  VkResult r = vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (r != VK_SUCCESS || count == 0) {
    std::fprintf(stderr, "%s FAIL: vkEnumeratePhysicalDevices -> %d, count=%u\n", tag, r, count);
    return VK_NULL_HANDLE;
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  std::fprintf(stderr, "%s physical devices: %u\n", tag, count);
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(devices[i], &p);
    std::fprintf(stderr,
                 "%s device %u: \"%s\" type=%d api=%u.%u.%u driver=0x%x vendor=0x%x device=0x%x\n",
                 tag, i, p.deviceName, static_cast<int>(p.deviceType),
                 VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                 VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID);
  }
  return devices[0];
}

// Ищет семейство очередей с graphics (+ present, если задана поверхность).
inline uint32_t FindQueueFamily(const char* tag, VkPhysicalDevice phys,
                                VkSurfaceKHR surface, bool* ok) {
  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &families, nullptr);
  std::vector<VkQueueFamilyProperties> fam(families);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &families, fam.data());
  for (uint32_t i = 0; i < families; ++i) {
    VkBool32 present = VK_TRUE;
    if (surface != VK_NULL_HANDLE) {
      vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
    }
    std::fprintf(stderr, "%s queue family %u: flags=0x%x count=%u present=%s\n", tag, i,
                 fam[i].queueFlags, fam[i].queueCount, present ? "yes" : "no");
    if ((fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
      *ok = true;
      return i;
    }
  }
  std::fprintf(stderr, "%s FAIL: no graphics+present queue family\n", tag);
  *ok = false;
  return 0;
}

// Создаёт логическое устройство (VK_KHR_swapchain) и возвращает очередь.
inline VkDevice CreateDevice(const char* tag, VkPhysicalDevice phys, uint32_t queue_family,
                             VkQueue* out_queue) {
  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = queue_family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;

  const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &queue;
  info.enabledExtensionCount = 1;
  info.ppEnabledExtensionNames = exts;

  VkDevice device = VK_NULL_HANDLE;
  VkResult r = vkCreateDevice(phys, &info, nullptr, &device);
  if (r != VK_SUCCESS) {
    std::fprintf(stderr, "%s FAIL: vkCreateDevice -> %d\n", tag, r);
    return VK_NULL_HANDLE;
  }
  std::fprintf(stderr, "%s device created\n", tag);
  vkGetDeviceQueue(device, queue_family, 0, out_queue);
  return device;
}

}  // namespace rayman_vk
