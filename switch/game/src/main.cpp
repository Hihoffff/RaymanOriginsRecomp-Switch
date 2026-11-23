// Rayman Origins (Xbox 360) recompiled - Nintendo Switch headless bring-up host.
//
// Stage 2 of the Switch plan: no graphics, no audio, no input. Loads
// default.xex from the SD card, runs the recompiled game on the ReXGlue
// runtime and writes a log to sdmc:/switch/RaymanOriginsRecomp/rayman.log.
//
// The game data (default.xex and files) is NOT part of this repository:
// copy your own dump to sdmc:/switch/RaymanOriginsRecomp/game/

#ifndef RAYMAN_DIAG
#define RAYMAN_DIAG 0  // CMake option RAYMAN_SWITCH_DIAG
#endif
#include <switch.h>
#include <ctime>
#include <vector>
#include <algorithm>
#include <dirent.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <csignal>
#include <unwind.h>
#include <cstdio>
#include <atomic>
#include <exception>
#include <typeinfo>
#include <cstdlib>
#include <memory>
#include <chrono>
#include <thread>
#include <string>

#include <rex/audio/nop/nop_audio_system.h>
#include <rex/cvar.h>
#include <rex/graphics/null_gpu.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/image_info.h>
#include <rex/runtime.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/thread/timer_queue.h>

#include "rayman_init.h"  // generated: PPCImageConfig, function table

// Diagnostic mid-asm hook declared by the generated code (audio dump on desktop).
void RaymanAudioDump(rex::ppc::Register&) {}

namespace {
constexpr const char* kRoot = "sdmc:/switch/RaymanOriginsRecomp";
constexpr const char* kRootAbs = "/switch/RaymanOriginsRecomp";
constexpr const char* kLogDir = "sdmc:/switch/RaymanOriginsRecomp/logs";
constexpr int kKeepLogs = 10;

void Say(const char* msg) {
  std::fprintf(stderr, "[switch] %s\n", msg);
  std::fflush(stderr);
}

// Diagnostics (session 09): the process used to vanish silently during
// Runtime::Setup (abort() -> _exit with live threads -> hbloader error dialog).
// Print the reason, flush logs and park the thread instead, so the log
// survives and the NRO stays mapped (close the app with HOME -> X).
[[noreturn]] void ParkForever() {
  for (;;) svcSleepThread(1000000000LL);
}
void OnAbort(int) {
  Say("!! abort() called");
  rex::FlushLogging();
  std::fflush(stderr);
  ParkForever();
}
namespace { int main_marker_dummy; }
_Unwind_Reason_Code BtCb(struct _Unwind_Context* c, void* arg) {
  int* n = static_cast<int*>(arg);
  if (*n < 40) std::fprintf(stderr, "[switch] bt#%d pc=%p\n", (*n)++, (void*)_Unwind_GetIP(c));
  return _URC_NO_REASON;
}
void OnTerminate() {
  Say("!! std::terminate");
  { int n = 0; std::fprintf(stderr, "[switch] bt anchor &BtCb=%p\n", (void*)&BtCb); _Unwind_Backtrace(BtCb, &n); }
  if (auto ep = std::current_exception()) {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[switch] exception: %s (%s)\n", e.what(), typeid(e).name());
    } catch (...) {
      Say("unknown exception type");
    }
  }
  rex::FlushLogging();
  std::fflush(stderr);
  ParkForever();
}

// hbloader unmaps the NRO as soon as main() returns; background threads that
// are still running then execute unmapped code (crash 2168-0001, session 09).
// Declared first in main() so it is destroyed last, after the Runtime.
struct ExitGuard {
  ~ExitGuard() {
    Say("exit: stopping timer queue");
    rex::thread::ShutdownTimerQueue();
    Say("exit: done");
  }
};
}  // namespace

bool RunSelfTest(const std::string& game_dir);  // selftest.cpp
std::unique_ptr<rex::system::IInputSystem> CreateSwitchInputSystem();  // switch_input.cpp
std::unique_ptr<rex::system::IAudioSystem> CreateSwitchAudioSystem(
    rex::runtime::FunctionDispatcher* fd);  // switch_audio.cpp
#ifdef RAYMAN_SWITCH_VULKAN
extern uint8_t* g_rayman_membase;   // native/native_hooks_switch.cpp
extern uint8_t* g_rayman_physbase;  // native/native_capture_switch.cpp
void RaymanNativeRendererInit();    // native/native_renderer_switch.cpp
#endif
#ifdef RAYMAN_SWITCH_VULKAN
bool RunVulkanProbe();  // vulkan_probe.cpp
bool RunVulkanClear();  // vulkan_clear.cpp
#endif


// Creates logs/session_YYYYMMDD-HHMMSS.log, deleting the oldest session_*.log
// files so that at most kKeepLogs remain (nothing else in logs/ is touched).
[[maybe_unused]] std::string OpenSessionLog() {
  mkdir(kLogDir, 0777);
  std::vector<std::string> old;
  if (DIR* d = opendir(kLogDir)) {
    while (dirent* e = readdir(d)) {
      const std::string n = e->d_name;
      if (n.rfind("session_", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
        old.push_back(n);
    }
    closedir(d);
  }
  std::sort(old.begin(), old.end());  // names sort by time
  for (size_t i = 0; i + kKeepLogs <= old.size(); ++i) {
    remove((std::string(kLogDir) + "/" + old[i]).c_str());
  }
  const time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char name[64];
  strftime(name, sizeof(name), "session_%Y%m%d-%H%M%S.log", &tmv);
  const std::string path = std::string(kLogDir) + "/" + name;
  if (!std::freopen(path.c_str(), "w", stderr)) return {};
  return path;
}

// CPU clock: games run at 1020 MHz by default; 1785 MHz is the stock
// "boost mode" rate the system itself uses for loading. Set it through clkrst
// (like sys-clk), re-apply when the system resets it (sleep, dock change),
// restore the original rate on exit. An empty file `cpu_boost_off` disables it.
namespace {
constexpr u32 kBoostCpuHz = 1785000000u;
std::atomic<bool> g_boost_run{false};
u32 g_cpu_hz_orig = 0;

void CpuBoostStart() {
  if (FILE* f = std::fopen("sdmc:/switch/RaymanOriginsRecomp/cpu_boost_off", "rb")) {
    std::fclose(f);
    Say("cpu boost: off (cpu_boost_off present)");
    return;
  }
  if (R_FAILED(clkrstInitialize())) {
    Say("cpu boost: clkrstInitialize failed");
    return;
  }
  ClkrstSession s;
  if (R_FAILED(clkrstOpenSession(&s, PcvModuleId_CpuBus, 3))) {
    Say("cpu boost: clkrstOpenSession failed");
    clkrstExit();
    return;
  }
  clkrstGetClockRate(&s, &g_cpu_hz_orig);
  Result rc = clkrstSetClockRate(&s, kBoostCpuHz);
  u32 now_hz = 0;
  clkrstGetClockRate(&s, &now_hz);
  clkrstCloseSession(&s);
  std::fprintf(stderr, "[switch] cpu boost: %u MHz -> %u MHz (rc=0x%X)\n", g_cpu_hz_orig / 1000000,
               now_hz / 1000000, rc);
  g_boost_run = true;
  // Leaked on purpose: detach() throws on Horizon (no pthread_detach).
  new std::thread([] {
    while (g_boost_run) {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      ClkrstSession s2;
      if (R_FAILED(clkrstOpenSession(&s2, PcvModuleId_CpuBus, 3))) continue;
      u32 hz = 0;
      clkrstGetClockRate(&s2, &hz);
      if (g_boost_run && hz != kBoostCpuHz) {
        clkrstSetClockRate(&s2, kBoostCpuHz);
        std::fprintf(stderr, "[switch] cpu boost: re-applied (was %u MHz)\n", hz / 1000000);
      }
      clkrstCloseSession(&s2);
    }
  });
  std::atexit([] {
    g_boost_run = false;
    ClkrstSession s3;
    if (g_cpu_hz_orig && R_SUCCEEDED(clkrstOpenSession(&s3, PcvModuleId_CpuBus, 3))) {
      clkrstSetClockRate(&s3, g_cpu_hz_orig);
      clkrstCloseSession(&s3);
    }
  });
}
}  // namespace

int main(int argc, char** argv) {
  ExitGuard exit_guard;
  std::set_terminate(OnTerminate);
  std::signal(SIGABRT, OnAbort);
  std::atexit([] { Say("atexit handler ran"); });
  mkdir(kRoot, 0777);
  // Launched through `nxlink`: stdout/stderr stream live to the PC. Otherwise
  // log to the SD card.
  socketInitializeDefault();
  const int nxlink_sock = nxlinkStdio();
  if (nxlink_sock < 0) {
    // One file per session in logs/, the last kKeepLogs kept, so several
    // play sessions can be read back later. Fully buffered and flushed every
    // 2 s: line-by-line SD writes from game threads cost frames.
#if RAYMAN_DIAG
    const std::string path = OpenSessionLog();
#else
    // Diagnostics off: one rayman.log per run, warnings and errors only.
    const std::string path = std::freopen("sdmc:/switch/RaymanOriginsRecomp/rayman.log", "w", stderr)
                                 ? std::string("rayman.log")
                                 : std::string();
#endif
    if (!path.empty()) {
      static char err_buf[64 * 1024], out_buf[64 * 1024];
      setvbuf(stderr, err_buf, _IOFBF, sizeof(err_buf));
      std::fflush(stdout);
      dup2(fileno(stderr), fileno(stdout));  // spdlog writes to stdout
      setvbuf(stdout, out_buf, _IOFBF, sizeof(out_buf));
      new std::thread([] {  // leaked on purpose (detach() throws on Horizon)
        for (;;) {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          std::fflush(stdout);
          std::fflush(stderr);
        }
      });
    }
  } else {
    // Non-blocking: when Wi-Fi stalls (power save, sleep) a blocking send
    // froze every thread that logs, i.e. the whole game, until the PC side
    // dropped the connection. Now lines that don't fit are lost instead.
    fcntl(nxlink_sock, F_SETFL, fcntl(nxlink_sock, F_GETFL, 0) | O_NONBLOCK);
    setvbuf(stderr, nullptr, _IOLBF, BUFSIZ);
  }
  Say("start");

  RunSelfTest(std::string(kRoot) + "/game");
  if (FILE* f = std::fopen("sdmc:/switch/RaymanOriginsRecomp/selftest_only", "rb")) {
    std::fclose(f);
    Say("selftest_only file present: stopping after the self-test");
    return 0;
  }
#ifdef RAYMAN_SWITCH_VULKAN
  // Диагностические режимы этапа 3 (план 3.2–3.3): включаются пустыми
  // файлами-флагами рядом с selftest_only, игра при этом не запускается.
  if (FILE* f = std::fopen("sdmc:/switch/RaymanOriginsRecomp/vulkan_probe", "rb")) {
    std::fclose(f);
    Say("vulkan_probe file present: running Vulkan probe mode");
    return RunVulkanProbe() ? 0 : 1;
  }
  if (FILE* f = std::fopen("sdmc:/switch/RaymanOriginsRecomp/vulkan_clear", "rb")) {
    std::fclose(f);
    Say("vulkan_clear file present: running Vulkan clear mode");
    return RunVulkanClear() ? 0 : 1;
  }
#endif

  CpuBoostStart();

  auto remaining = rex::cvar::Init(argc, argv);
  (void)remaining;
  rex::cvar::ApplyEnvironment();
#if !RAYMAN_DIAG
  rex::cvar::SetFlagByName("log_level", "warn");
#endif
  rex::InitLoggingEarly();
  Say("logging up");

  // std::filesystem treats "sdmc:/..." as a relative path and prepends the cwd
// (-> "sdmc:/switch/sdmc:/switch/..."), so pass device-less absolute paths.
  const std::string game = std::string(kRootAbs) + "/game";
  const std::string user = std::string(kRootAbs) + "/user";
  const std::string cache = std::string(kRootAbs) + "/cache";
  mkdir(user.c_str(), 0777);
  mkdir(cache.c_str(), 0777);

  auto runtime = std::make_unique<rex::Runtime>(game, user, std::filesystem::path{}, cache,
                                                std::filesystem::path{});

  rex::RuntimeConfig config;
  // Statically linked null GPU (Horizon has no dlopen for the rexgpu-null
  // plugin): keeps the guest GPU protocol alive - ring buffer, fences, event
  // writes, interrupt callback, vblank - without drawing anything.
  config.graphics = rex::graphics::null_gpu::CreateStaticNullGraphicsSystem();
  // audout output; an empty file `audio_off` keeps the silent driver.
  if (FILE* f = std::fopen("sdmc:/switch/RaymanOriginsRecomp/audio_off", "rb")) {
    std::fclose(f);
    Say("audio_off file present: silent audio driver");
    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);
  } else {
    config.audio_factory = [](rex::runtime::FunctionDispatcher* fd)
        -> std::unique_ptr<rex::system::IAudioSystem> { return CreateSwitchAudioSystem(fd); };
  }
  config.input_factory = [](bool) -> std::unique_ptr<rex::system::IInputSystem> {
    return CreateSwitchInputSystem();
  };
  config.kernel_init = rex::kernel::InitializeKernel;

  Say("Runtime::Setup");
  rex::X_STATUS status = runtime->Setup(PPCImageConfig, std::move(config));
  if ((status != 0)) {
    std::fprintf(stderr, "[switch] Runtime::Setup failed: %08X\n", status);
    return 1;
  }
  if (PPCImageConfig.register_modules) {
    PPCImageConfig.register_modules(runtime->kernel_state());
  }

#ifdef RAYMAN_SWITCH_VULKAN
  // Native renderer (the macOS path: D3D hooks -> own Vulkan + the game's
  // SPIR-V shaders). An empty file `native_off` next to the game turns it off.
  if (FILE* f = std::fopen("sdmc:/switch/RaymanOriginsRecomp/native_off", "rb")) {
    std::fclose(f);
    Say("native_off file present: native renderer disabled");
  } else {
    setenv("RAYMAN_NATIVE_RENDER", "main", 1);
    g_rayman_membase = runtime->virtual_membase();
    g_rayman_physbase = runtime->memory()->physical_membase();
    Say("native renderer: init");
    RaymanNativeRendererInit();
  }
#endif

  Say("LoadXexImage");
  status = runtime->LoadXexImage("game:\\default.xex");
  if ((status != 0)) {
    std::fprintf(stderr, "[switch] LoadXexImage failed: %08X\n", status);
    return 1;
  }

  Say("PrepareModuleLaunch");
  auto main_thread = runtime->PrepareModuleLaunch();
  if (!main_thread) {
    Say("PrepareModuleLaunch failed");
    return 1;
  }
  main_thread->Resume();
  Say("guest main thread running");
  main_thread->Wait(0, 0, 0, nullptr);
  Say("guest main thread exited");
  return 0;
}
