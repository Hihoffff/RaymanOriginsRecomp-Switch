// Режим vulkan_probe (шаг 3.2 плана docs/PLAN_STAGE3_GRAPHICS.md).
//
// Включается пустым файлом-флагом sdmc:/switch/RaymanOriginsRecomp/vulkan_probe
// (как selftest_only). Создаёт VkInstance, перечисляет устройства и расширения,
// печатает имя устройства в лог и завершается, не запуская игру.
// Только чтение: на SD пишется только обычный лог (nxlink / rayman.log).
//
// Успех (строки лога):
//   [vulkan_probe] instance ok
//   [vulkan_probe] VK_KHR_surface: present, VK_NN_vi_surface: present
//   [vulkan_probe] device 0: "NVIDIA Tegra X1 (NVK GM20B)" ...
//   [vulkan_probe] done
// Провал: любая строка "[vulkan_probe] FAIL: ..." или отсутствие "done".

#include "vulkan_common.h"

bool RunVulkanProbe() {
  constexpr const char* kTag = "[vulkan_probe]";
  std::fprintf(stderr, "%s start\n", kTag);

  rayman_vk::PrepareEnvironment();
  // WSI на этом этапе не трогаем, но Mesa может писать в stdout уже при
  // создании инстанса — редиректим на всякий случай.
  rayman_vk::RedirectStdoutToStderr(kTag);

  VkInstance instance = rayman_vk::CreateInstance(kTag, /*need_vi_surface=*/false);
  if (instance == VK_NULL_HANDLE) return false;

  VkPhysicalDevice phys = rayman_vk::PickPhysicalDevice(kTag, instance);
  if (phys == VK_NULL_HANDLE) {
    vkDestroyInstance(instance, nullptr);
    return false;
  }

  uint32_t ext_count = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> dev_exts(ext_count);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, dev_exts.data());
  std::fprintf(stderr, "%s device extensions: %u (VK_KHR_swapchain: %s)\n", kTag, ext_count,
               rayman_vk::HasExtension(dev_exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                   ? "present"
                   : "absent");

  vkDestroyInstance(instance, nullptr);
  std::fprintf(stderr, "%s done\n", kTag);
  return true;
}
