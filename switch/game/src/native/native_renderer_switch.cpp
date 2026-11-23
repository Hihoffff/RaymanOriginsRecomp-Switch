// Switch glue for the native renderer (tools/native_renderer/vk_renderer.h):
// the Switch counterpart of upstream/rex/src/native_renderer.cpp. Init makes a
// VK_NN_vi_surface on the default NWindow (no SDL); NVK is linked statically
// (shim/volk.h). Draw/Clear/Resolve/Present below are upstream's, unchanged.
//
// SPIR-V: sdmc:/switch/RaymanOriginsRecomp/native/spirv_ubo/<HASH>_{vs,ps}.spv
// (game-derived, built on the PC by tools/native_renderer/build_spirv.sh).
#ifndef RAYMAN_DIAG
#define RAYMAN_DIAG 0  // CMake option RAYMAN_SWITCH_DIAG
#endif
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include "vk_renderer.h"

#define NATIVE_LOG(...) (std::fprintf(stderr, "[native] " __VA_ARGS__), std::fputc('\n', stderr))

extern uint8_t* g_rayman_physbase;  // native_capture_switch.cpp
extern uint8_t* g_rayman_membase;   // native_hooks_switch.cpp

namespace {

native::Renderer* g_renderer = nullptr;
native::Renderer::SurfaceFactory g_makeSurface;
bool g_frameOpen = false;
std::mutex g_mutex;
std::atomic<float> g_renderScale{1.0f};
double g_drawMs = 0;
std::chrono::steady_clock::time_point g_lastPresent = std::chrono::steady_clock::now();

struct ScopedTimer {
  double& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  ~ScopedTimer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
};

constexpr const char* kNativeDir = "sdmc:/switch/RaymanOriginsRecomp/native";
std::string SpirvDir() { return std::string(kNativeDir) + "/spirv_ubo"; }
std::string PipelineCachePath() { return std::string(kNativeDir) + "/pipeline_cache.bin"; }
std::string PipelineListPath() { return std::string(kNativeDir) + "/pipelines.txt"; }

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

std::vector<native::Renderer::PipelineDesc> ReadPipelineList() {
  std::vector<native::Renderer::PipelineDesc> out;
  std::ifstream f(PipelineListPath());
  unsigned long long vs, ps;
  uint32_t blend, stride;
  while (f >> std::hex >> vs >> ps >> blend >> std::dec >> stride) out.push_back({vs, ps, blend, stride});
  return out;
}

// Synchronous on Switch (std::thread::detach is not implemented in newlib
// here); called at most every 5 s from Present.
void SavePipelines(native::Renderer* renderer) {
  auto data = renderer->PipelineCacheData();
  auto descs = renderer->PipelineDescs();
  if (!data.empty()) {
    std::string tmp = PipelineCachePath() + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
      std::fwrite(data.data(), 1, data.size(), f);
      std::fclose(f);
      std::remove(PipelineCachePath().c_str());
      std::rename(tmp.c_str(), PipelineCachePath().c_str());
    }
  }
  std::string tmp = PipelineListPath() + ".tmp";
  if (FILE* f = std::fopen(tmp.c_str(), "w")) {
    for (auto& p : descs)
      std::fprintf(f, "%016llx %016llx %x %u\n", (unsigned long long)p.vs, (unsigned long long)p.ps, p.blend, p.stride);
    std::fclose(f);
    std::remove(PipelineListPath().c_str());
    std::rename(tmp.c_str(), PipelineListPath().c_str());
  }
}

}  // namespace

bool RaymanNativeRendererActive() { return g_renderer != nullptr; }

// Main thread, after the runtime is set up: window surface + renderer.
void RaymanNativeRendererInit() {
  if (g_renderer) return;
  setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
  setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
  setenv("MESA_GLSL_CACHE_DISABLE", "1", 1);
  setenv("MESA_VK_ENABLE_SUBMIT_THREAD", "1", 1);
  // NVK's Switch WSI prints to stdout; there is no console: send it to stderr.
  std::fflush(stdout);
  dup2(fileno(stderr), fileno(stdout));

  constexpr uint32_t w = 1280, h = 720;
  NWindow* window = nwindowGetDefault();
  if (!window) {
    NATIVE_LOG("no default NWindow");
    return;
  }
  nwindowSetDimensions(window, w, h);
  nwindowSetCrop(window, 0, 0, w, h);
  nwindowSetSwapInterval(window, 1);
  // RAYMAN_NATIVE_PRESENT=immediate is available; NVK on Switch has no
  // mailbox, and immediate (tearing) measured no faster: keep FIFO.

  std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
  native::Renderer::SurfaceFactory makeSurface = [window](VkInstance instance) {
    VkViSurfaceCreateInfoNN info{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
    info.window = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult r = vkCreateViSurfaceNN(instance, &info, nullptr, &surface);
    if (r != VK_SUCCESS) NATIVE_LOG("vkCreateViSurfaceNN -> %d", int(r));
    return surface;
  };
  NATIVE_LOG("native renderer: starting (Switch, %ux%u, SPIR-V %s)", w, h, SpirvDir().c_str());
  auto* renderer = new native::Renderer();
  renderer->log = [](const char* stage) { NATIVE_LOG("native renderer: %s", stage); };
  g_makeSurface = makeSurface;
  renderer->SetPipelineCacheData(ReadFile(PipelineCachePath()));
  if (!renderer->Init(nullptr, exts, makeSurface, w, h)) {
    NATIVE_LOG("renderer init failed: %s", renderer->error().c_str());
    delete renderer;
    return;
  }
  if (renderer->presentMode_ != VK_PRESENT_MODE_FIFO_KHR) {
    nwindowSetSwapInterval(window, 0);
    NATIVE_LOG("native renderer: swap interval 0 (present mode %d)", int(renderer->presentMode_));
  }
  std::string dir = SpirvDir();
  renderer->SetShaderSource([dir](uint64_t hash, bool vertex) {
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llX_%s.spv", (unsigned long long)hash, vertex ? "vs" : "ps");
    std::ifstream f(dir + name, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    if (words.empty()) NATIVE_LOG("missing SPIR-V %s", name);
    return words;
  });
  renderer->SetMemory([](uint32_t address, uint32_t size) -> const uint8_t* {
    if (!g_rayman_physbase || uint64_t(address) + size > 0x20000000u) return nullptr;
    return g_rayman_physbase + address;
  });
  {
    auto start = std::chrono::steady_clock::now();
    auto known = ReadPipelineList();
    uint32_t made = renderer->Prewarm(known);
    NATIVE_LOG("pipelines: %u of %zu known created up front in %.0f ms", made, known.size(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
  g_renderer = renderer;
  NATIVE_LOG("native renderer ready (%ux%u), SPIR-V from %s", w, h, dir.c_str());
}

namespace {
void OpenFrame() {
  if (!g_frameOpen) {
    g_renderer->BeginFrame();
    g_frameOpen = true;
  }
}
}  // namespace

// Render thread, from the draw hooks (native_capture.cpp).
void RaymanNativeRendererDraw(const native::DrawCall& call) {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  ScopedTimer timer{g_drawMs};
  OpenFrame();
  g_renderer->Draw(call);
}

// Render thread, from the Clear / Resolve hooks. state = device + native::kStateBegin.
void RaymanNativeRendererClear(const uint8_t* state, uint32_t argb) {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  OpenFrame();
  g_renderer->Clear(state, argb);
}

void RaymanNativeRendererResolve(const uint8_t* state, const int32_t* rect, const uint8_t* destFetch) {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  OpenFrame();
  g_renderer->Resolve(state, rect, destFetch);
}

// Render thread, from the Present hook.
void RaymanNativeRendererPresent() {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_frameOpen) {
    return;
  }
  // With RAYMAN_CAPTURE=1, save the native frame every 10 s next to the
  // emulated captures (captures/native_NNN.ppm).
  static auto start = std::chrono::steady_clock::now();
  static const int interval = std::getenv("RAYMAN_CAPTURE_INTERVAL") ? std::atoi(std::getenv("RAYMAN_CAPTURE_INTERVAL")) : 10;
  static int nextShot = interval;
  bool shot = std::getenv("RAYMAN_CAPTURE") &&
              std::chrono::steady_clock::now() - start >= std::chrono::seconds(nextShot);
  std::vector<uint8_t> pixels;
#if defined(__ANDROID__)
  // In the background SDL drops the window (the property goes null) and hands
  // out a new one when the app comes back: skip frames meanwhile, then build a
  // surface and swapchain for the new window. Behind the lock screen or the
  // notification shade the window can stay: hold the game there too.
  auto currentWindow = [] {
    return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
  };
  void* window = currentWindow();
  if (!window || g_background) {
    g_renderer->DiscardFrame();
    g_frameOpen = false;
    // Hold the game until it is visible again: its logic advances once per
    // presented frame, so it pauses instead of running unseen.
    NATIVE_LOG("native renderer: app in the background, game held");
    while (g_background || !currentWindow()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    NATIVE_LOG("native renderer: back in the foreground");
    g_lastPresent = std::chrono::steady_clock::now();
    g_drawMs = 0;
    return;
  }
  if (window != g_nativeWindow) {
    NATIVE_LOG("native renderer: new window %p, recreating the surface", window);
    if (!g_renderer->Recreate(g_makeSurface)) NATIVE_LOG("recreate failed: %s", g_renderer->error().c_str());
  }
#endif
  if (g_renderer->stale()) {
    NATIVE_LOG("native renderer: swapchain out of date, re-creating");
    if (!g_renderer->Recreate()) NATIVE_LOG("swapchain: %s", g_renderer->error().c_str());
  }
  float scale = g_renderScale.load(std::memory_order_relaxed);
  if (scale != g_renderer->renderScale()) {
    g_renderer->SetRenderScale(scale);
    NATIVE_LOG("render scale %.0f%% (%.0fx%.0f)", g_renderer->renderScale() * 100,
               g_renderer->width() * g_renderer->renderScale(), g_renderer->height() * g_renderer->renderScale());
  }
  g_renderer->EndFrame(shot ? &pixels : nullptr);
  g_frameOpen = false;
  if (shot && !pixels.empty()) {
    char name[64];
    std::snprintf(name, sizeof(name), "captures/native_%03d.ppm", nextShot);
    if (FILE* f = std::fopen(name, "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", g_renderer->width(), g_renderer->height());
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::fwrite(&pixels[i], 1, 3, f);
      std::fclose(f);
      NATIVE_LOG("saved %s", name);
    }
    nextShot += interval;
  }
  // Frame budget breakdown. Slow frames are logged with what they created, so
  // hitches can be told apart: CPU in the draw hooks (decode, new pipelines),
  // the end of the frame (recording, acquire), or waiting on the GPU.
  auto now = std::chrono::steady_clock::now();
  double frameMs = std::chrono::duration<double, std::milli>(now - g_lastPresent).count();
  g_lastPresent = now;
  auto& st = g_renderer->stats();
  static double sumFrame = 0, sumDraw = 0, sumRecord = 0, sumWait = 0, worst = 0;
  static uint32_t sumDraws = 0;
  sumFrame += frameMs, sumDraw += g_drawMs, sumRecord += st.recordMs, sumWait += st.waitMs, sumDraws += st.draws;
  worst = std::max(worst, frameMs);
  // Only real hitches: a line per >20 ms frame was tens of SD writes a second.
  if (RAYMAN_DIAG && frameMs > 50.0)
    NATIVE_LOG("slow frame %.1f ms: draw hooks %.1f, end %.1f, gpu wait %.1f | %u draws, +%u pipelines, +%u textures, "
               "%u re-uploads, %zu KB uploaded",
               frameMs, g_drawMs, st.recordMs, st.waitMs, st.draws, st.newPipelines, st.newTextures, st.reuploads,
               st.uploadBytes >> 10);
  g_drawMs = 0;
  // New pipelines since the last save: persist them (at most every 5 s).
  static uint32_t savedPipelines = st.pipelines;
  static auto lastSave = now;
  if (st.pipelines != savedPipelines && now - lastSave > std::chrono::seconds(5)) {
    SavePipelines(g_renderer);
    savedPipelines = st.pipelines;
    lastSave = now;
  }
  static int frames = 0;
  static uint32_t resolveMismatches = 0;
  resolveMismatches += st.resolveMismatches;
  if (++frames % 120 == 0) {
    if (RAYMAN_DIAG)
    NATIVE_LOG("perf over 120 frames: %.1f fps, avg frame %.1f ms (draw hooks %.1f, end %.1f, gpu wait %.1f), "
               "worst %.1f ms, %u draws/frame",
               120000.0 / sumFrame, sumFrame / 120, sumDraw / 120, sumRecord / 120, sumWait / 120, worst, sumDraws / 120);
    sumFrame = sumDraw = sumRecord = sumWait = worst = 0;
    sumDraws = 0;
  }
  if (frames % 300 == 0) {
    // Taken even without diagnostics: the renderer accumulates them.
    auto skips = g_renderer->TakeSkipReasons();
    auto notes = g_renderer->TakeNotes();
    if (RAYMAN_DIAG) {
      auto& s = g_renderer->stats();
      NATIVE_LOG("native frame %d: %u draws, %u skipped, %u pipelines, %u textures", frames, s.draws, s.skipped,
                 s.pipelines, s.textures);
      if (resolveMismatches)
        NATIVE_LOG("  %u texture fetches with a mismatched resolve in the last 300 frames", resolveMismatches);
      for (auto& [reason, n] : skips) NATIVE_LOG("  skipped x%u: %s", n, reason.c_str());
      for (auto& [note, n] : notes) NATIVE_LOG("  note x%u: %s", n, note.c_str());
    }
    resolveMismatches = 0;
  }
}
