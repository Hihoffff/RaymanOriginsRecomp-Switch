// Режим vulkan_clear (шаг 3.3 плана docs/PLAN_STAGE3_GRAPHICS.md).
//
// Включается пустым файлом-флагом sdmc:/switch/RaymanOriginsRecomp/vulkan_clear
// (как selftest_only). Создаёт поверхность VK_NN_vi_surface через libnx NWindow,
// swapchain и ~5 секунд заливает экран меняющимся цветом через
// vkCmdClearColorImage + vkQueuePresentKHR, затем корректно всё разрушает.
// Игра при этом не запускается. На SD пишется только лог.
//
// Успех (строки лога):
//   [vulkan_clear] instance ok
//   [vulkan_clear] device 0: "NVIDIA Tegra X1 (NVK GM20B)" ...
//   [vulkan_clear] surface ok
//   [vulkan_clear] swapchain ok: images=2 1280x720 format=...
//   [vulkan_clear] first present ok
//   [vulkan_clear] frames presented: N
//   [vulkan_clear] done
// Провал: любая строка "[vulkan_clear] FAIL: ...", отсутствие "first present
// ok"/"done", либо чёрный/зависший экран при формально чистом логе.

#include "vulkan_common.h"

#include <switch.h>

#include <chrono>

namespace {

constexpr const char* kTag = "[vulkan_clear]";
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr double kRunSeconds = 5.0;

}  // namespace

bool RunVulkanClear() {
  std::fprintf(stderr, "%s start\n", kTag);

  rayman_vk::PrepareEnvironment();
  // WSI NVK пишет диагностику в stdout при создании swapchain — редирект до
  // любых оконных/Vulkan-вызовов (урок ветки switch upstream).
  if (!rayman_vk::RedirectStdoutToStderr(kTag)) return false;

  NWindow* window = nwindowGetDefault();
  if (window == nullptr) {
    std::fprintf(stderr, "%s FAIL: nwindowGetDefault\n", kTag);
    return false;
  }
  Result rc = nwindowSetDimensions(window, kWidth, kHeight);
  if (R_FAILED(rc)) std::fprintf(stderr, "%s warn: nwindowSetDimensions -> 0x%x\n", kTag, rc);
  rc = nwindowSetCrop(window, 0, 0, kWidth, kHeight);
  if (R_FAILED(rc)) std::fprintf(stderr, "%s warn: nwindowSetCrop -> 0x%x\n", kTag, rc);
  rc = nwindowSetSwapInterval(window, 1);
  if (R_FAILED(rc)) std::fprintf(stderr, "%s warn: nwindowSetSwapInterval -> 0x%x\n", kTag, rc);

  VkInstance instance = rayman_vk::CreateInstance(kTag, /*need_vi_surface=*/true);
  if (instance == VK_NULL_HANDLE) return false;

  bool ok = false;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  // Объявлены заранее: goto fail не должен пересекать инициализации.
  uint32_t queue_family = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkSurfaceCapabilitiesKHR caps{};
  VkSurfaceFormatKHR surface_format{};
  VkExtent2D extent{kWidth, kHeight};
  std::vector<VkImage> images;
  std::vector<VkCommandBuffer> cmd_buffers;
  VkSemaphore acquire_sem = VK_NULL_HANDLE;
  VkSemaphore submit_sem = VK_NULL_HANDLE;
  VkFence frame_fence = VK_NULL_HANDLE;
  uint64_t frames = 0;

  const auto cleanup = [&]() {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
      if (acquire_sem != VK_NULL_HANDLE) vkDestroySemaphore(device, acquire_sem, nullptr);
      if (submit_sem != VK_NULL_HANDLE) vkDestroySemaphore(device, submit_sem, nullptr);
      if (frame_fence != VK_NULL_HANDLE) vkDestroyFence(device, frame_fence, nullptr);
      if (cmd_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, cmd_pool, nullptr);
      if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::fprintf(stderr, "%s cleanup ok\n", kTag);
  };

  phys = rayman_vk::PickPhysicalDevice(kTag, instance);
  if (phys == VK_NULL_HANDLE) goto fail;

  {
    VkViSurfaceCreateInfoNN info{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
    info.window = window;
    VkResult r = vkCreateViSurfaceNN(instance, &info, nullptr, &surface);
    if (r != VK_SUCCESS) {
      std::fprintf(stderr, "%s FAIL: vkCreateViSurfaceNN -> %d\n", kTag, r);
      goto fail;
    }
    std::fprintf(stderr, "%s surface ok\n", kTag);
  }

  queue_family = rayman_vk::FindQueueFamily(kTag, phys, surface, &ok);
  if (!ok) goto fail;

  device = rayman_vk::CreateDevice(kTag, phys, queue_family, &queue);
  if (device == VK_NULL_HANDLE) goto fail;

  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
  std::fprintf(stderr,
               "%s surface caps: minImages=%u maxImages=%u extent=%ux%u usage=0x%x transforms=0x%x\n",
               kTag, caps.minImageCount, caps.maxImageCount, caps.currentExtent.width,
               caps.currentExtent.height, caps.supportedUsageFlags, caps.supportedTransforms);

  {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, formats.data());
    if (count == 0) {
      std::fprintf(stderr, "%s FAIL: no surface formats\n", kTag);
      goto fail;
    }
    surface_format = formats[0];
    for (const auto& f : formats) {
      std::fprintf(stderr, "%s surface format: %d colorspace=%d\n", kTag,
                   static_cast<int>(f.format), static_cast<int>(f.colorSpace));
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
        surface_format = f;
      }
    }
  }

  if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
  {
    uint32_t image_count = caps.minImageCount > 2 ? caps.minImageCount : 2;
    if (caps.maxImageCount != 0 && image_count > caps.maxImageCount) {
      image_count = caps.maxImageCount;
    }
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
      transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface;
    sc.minImageCount = image_count;
    sc.imageFormat = surface_format.format;
    sc.imageColorSpace = surface_format.colorSpace;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sc.preTransform = transform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    VkResult r = vkCreateSwapchainKHR(device, &sc, nullptr, &swapchain);
    if (r != VK_SUCCESS) {
      std::fprintf(stderr, "%s FAIL: vkCreateSwapchainKHR -> %d\n", kTag, r);
      goto fail;
    }
  }

  {
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
    std::fprintf(stderr, "%s swapchain ok: images=%u %ux%u format=%d\n", kTag, count, extent.width,
                 extent.height, static_cast<int>(surface_format.format));
  }

  {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = queue_family;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkResult r = vkCreateCommandPool(device, &pool, nullptr, &cmd_pool);
    if (r != VK_SUCCESS) {
      std::fprintf(stderr, "%s FAIL: vkCreateCommandPool -> %d\n", kTag, r);
      goto fail;
    }
  }

  cmd_buffers.resize(images.size());
  {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = cmd_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = static_cast<uint32_t>(cmd_buffers.size());
    VkResult r = vkAllocateCommandBuffers(device, &alloc, cmd_buffers.data());
    if (r != VK_SUCCESS) {
      std::fprintf(stderr, "%s FAIL: vkAllocateCommandBuffers -> %d\n", kTag, r);
      goto fail;
    }
  }

  {
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateSemaphore(device, &sem, nullptr, &acquire_sem) != VK_SUCCESS ||
        vkCreateSemaphore(device, &sem, nullptr, &submit_sem) != VK_SUCCESS ||
        vkCreateFence(device, &fence, nullptr, &frame_fence) != VK_SUCCESS) {
      std::fprintf(stderr, "%s FAIL: vkCreateSemaphore/Fence\n", kTag);
      goto fail;
    }
  }

  {
    std::vector<bool> image_used(images.size(), false);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(int(kRunSeconds * 1000));
    bool first_present = true;
    while (std::chrono::steady_clock::now() < deadline) {
      uint32_t index = 0;
      VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquire_sem,
                                         VK_NULL_HANDLE, &index);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "%s FAIL: vkAcquireNextImageKHR -> %d (frame %llu)\n", kTag, r,
                     (unsigned long long)frames);
        break;
      }

      VkCommandBuffer cmd = cmd_buffers[index];
      vkResetCommandBuffer(cmd, 0);
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(cmd, &begin);

      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = images[index];
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.oldLayout =
          image_used[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcAccessMask = 0;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);

      // Меняющийся цвет: без libm, треугольные волны по счётчику кадров.
      VkClearColorValue color{};
      color.float32[0] = static_cast<float>((frames * 3) % 256) / 255.0f;
      color.float32[1] = static_cast<float>((frames * 5) % 256) / 255.0f;
      color.float32[2] = static_cast<float>((frames * 7) % 256) / 255.0f;
      color.float32[3] = 1.0f;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                           &range);

      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = 0;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
      vkEndCommandBuffer(cmd);
      image_used[index] = true;

      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.waitSemaphoreCount = 1;
      submit.pWaitSemaphores = &acquire_sem;
      submit.pWaitDstStageMask = &wait_stage;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &cmd;
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &submit_sem;
      r = vkQueueSubmit(queue, 1, &submit, frame_fence);
      if (r != VK_SUCCESS) {
        std::fprintf(stderr, "%s FAIL: vkQueueSubmit -> %d (frame %llu)\n", kTag, r,
                     (unsigned long long)frames);
        break;
      }
      vkWaitForFences(device, 1, &frame_fence, VK_TRUE, UINT64_MAX);
      vkResetFences(device, 1, &frame_fence);

      VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
      present.waitSemaphoreCount = 1;
      present.pWaitSemaphores = &submit_sem;
      present.swapchainCount = 1;
      present.pSwapchains = &swapchain;
      present.pImageIndices = &index;
      r = vkQueuePresentKHR(queue, &present);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "%s FAIL: vkQueuePresentKHR -> %d (frame %llu)\n", kTag, r,
                     (unsigned long long)frames);
        break;
      }
      ++frames;
      if (first_present) {
        std::fprintf(stderr, "%s first present ok\n", kTag);
        first_present = false;
      }
    }
  }

  std::fprintf(stderr, "%s frames presented: %llu\n", kTag, (unsigned long long)frames);
  ok = frames > 0;

  cleanup();
  if (!ok) {
    std::fprintf(stderr, "%s FAIL: no frames presented\n", kTag);
    return false;
  }
  std::fprintf(stderr, "%s done\n", kTag);
  return true;

fail:
  cleanup();
  return false;
}
