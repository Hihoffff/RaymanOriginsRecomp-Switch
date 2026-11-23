// Switch self-test: exercises the platform layer (memory, threads, fibers)
// and reports system facts in ONE run, so a single trip to the console yields
// as much information as possible. Output goes to stderr (log file or nxlink).

#include <switch.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include <rex/memory/utils.h>
#include <rex/thread.h>
#include <rex/thread/fiber.h>

namespace {

using Clock = std::chrono::steady_clock;

double Ms(Clock::time_point a) {
  return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
}

void Line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Line(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "[selftest] ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
  va_end(ap);
}

void MemInfo(const char* when) {
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  Line("memory %s: total %llu MiB, used %llu MiB", when, (unsigned long long)(total >> 20),
       (unsigned long long)(used >> 20));
}

void SystemFacts(const std::string& game_dir) {
  const u32 v = hosversionGet();
  Line("HOS %u.%u.%u, applet type %d, atmosphere=%d", HOSVER_MAJOR(v), HOSVER_MINOR(v),
       HOSVER_MICRO(v), (int)appletGetAppletType(), (int)hosversionIsAtmosphere());
  Line("SVC hints: MapProcessMemory=%d UnmapProcessMemory=%d MapProcessCodeMemory=%d "
       "UnmapProcessCodeMemory=%d",
       envIsSyscallHinted(0x74), envIsSyscallHinted(0x75), envIsSyscallHinted(0x77),
       envIsSyscallHinted(0x78));
  MemInfo("at start");

  struct stat st {};
  const std::string xex = game_dir + "/default.xex";
  if (stat(xex.c_str(), &st) == 0) {
    Line("game: default.xex found, %lld bytes", (long long)st.st_size);
  } else {
    Line("game: default.xex NOT FOUND at %s", xex.c_str());
  }
  int files = 0;
  if (DIR* d = opendir(game_dir.c_str())) {
    while (readdir(d)) ++files;
    closedir(d);
  }
  Line("game: %d directory entries in %s", files, game_dir.c_str());
}

bool MemoryTest() {
  bool ok = true;
  auto t0 = Clock::now();
  uint8_t* base = rex::memory::SwitchReserveGuestWindow(0x100000000ull);
  Line("memory: reserve 4 GiB window -> %p (%.1f ms)", (void*)base, Ms(t0));
  if (!base) return false;

  // 16 MiB committed in the 0x40000000 region, pattern write/read.
  t0 = Clock::now();
  void* p = rex::memory::AllocFixed(base + 0x40000000, 16u << 20,
                                    rex::memory::AllocationType::kReserveCommit,
                                    rex::memory::PageAccess::kReadWrite);
  Line("memory: commit 16 MiB @guest 0x40000000 -> %p (%.1f ms)", p, Ms(t0));
  if (p) {
    auto* b = static_cast<volatile uint8_t*>(p);
    for (size_t i = 0; i < (16u << 20); i += 4096) b[i] = static_cast<uint8_t>(i >> 12);
    bool pat = true;
    for (size_t i = 0; i < (16u << 20); i += 4096)
      if (b[i] != static_cast<uint8_t>(i >> 12)) pat = false;
    Line("memory: pattern write/read %s", pat ? "OK" : "MISMATCH");
    ok &= pat;
  } else {
    ok = false;
  }
  MemInfo("after 16 MiB");

  // The runtime pre-commits the whole 512 MiB physical heap at startup.
  t0 = Clock::now();
  void* ph = rex::memory::AllocFixed(base + 0xA0000000ull, 512u << 20,
                                     rex::memory::AllocationType::kCommit,
                                     rex::memory::PageAccess::kReadWrite);
  Line("memory: commit 512 MiB physical @guest 0xA0000000 -> %p (%.1f ms)", ph, Ms(t0));
  ok &= ph != nullptr;
  MemInfo("after 512 MiB");
  return ok;
}

bool ThreadTest() {
  bool ok = true;
  std::atomic<int> counter{0};
  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
  auto t0 = Clock::now();
  for (int i = 0; i < 4; ++i) {
    rex::thread::Thread::CreationParameters params;
    params.stack_size = 256 * 1024;
    auto t = rex::thread::Thread::Create(params, [&counter] {
      for (int k = 0; k < 1000; ++k) counter.fetch_add(1);
    });
    if (!t) {
      Line("threads: Thread::Create failed for #%d", i);
      return false;
    }
    threads.push_back(std::move(t));
  }
  for (auto& t : threads) rex::thread::Wait(t.get(), false);
  Line("threads: 4 threads joined, counter=%d (expect 4000), %.1f ms", counter.load(), Ms(t0));
  ok &= counter.load() == 4000;

  // Suspended-create + Resume.
  std::atomic<bool> ran{false};
  rex::thread::Thread::CreationParameters sp;
  sp.stack_size = 256 * 1024;
  sp.create_suspended = true;
  auto st = rex::thread::Thread::Create(sp, [&ran] { ran = true; });
  if (st) {
    rex::thread::Sleep(std::chrono::milliseconds(100));
    const bool before = ran.load();
    st->Resume();
    rex::thread::Wait(st.get(), false);
    Line("threads: create_suspended ran-before-resume=%d ran-after=%d (expect 0 1)", before,
         ran.load());
    ok &= !before && ran.load();
  } else {
    Line("threads: create_suspended Create failed");
    ok = false;
  }

  // Event signalling between threads.
  auto ev = rex::thread::Event::CreateManualResetEvent(false);
  rex::thread::Thread::CreationParameters ep;
  ep.stack_size = 256 * 1024;
  auto waiter = rex::thread::Thread::Create(ep, [&] {
    auto r = rex::thread::Wait(ev.get(), false, std::chrono::milliseconds(2000));
    ran = (r == rex::thread::WaitResult::kSuccess);
  });
  ran = false;
  rex::thread::Sleep(std::chrono::milliseconds(50));
  ev->Set();
  if (waiter) rex::thread::Wait(waiter.get(), false);
  Line("threads: event wake-up %s", ran.load() ? "OK" : "FAILED");
  ok &= ran.load();
  return ok;
}

rex::thread::Fiber* g_main_fiber = nullptr;
rex::thread::Fiber* g_other_fiber = nullptr;
int g_fiber_steps = 0;

void FiberEntry(void*) {
  ++g_fiber_steps;
  rex::thread::Fiber::SwitchTo(g_main_fiber);
  ++g_fiber_steps;
  rex::thread::Fiber::SwitchTo(g_main_fiber);
}

bool FiberTest() {
  g_main_fiber = rex::thread::Fiber::ConvertCurrentThread();
  g_other_fiber = rex::thread::Fiber::Create(256 * 1024, &FiberEntry, nullptr);
  rex::thread::Fiber::SwitchTo(g_other_fiber);
  rex::thread::Fiber::SwitchTo(g_other_fiber);
  Line("fibers: steps=%d (expect 2)", g_fiber_steps);
  return g_fiber_steps == 2;
}

}  // namespace

bool RunSelfTest(const std::string& game_dir) {
  Line("=== self-test begin ===");
  SystemFacts(game_dir);
  const bool mem = MemoryTest();
  const bool thr = ThreadTest();
  const bool fib = FiberTest();
  Line("=== self-test end: memory=%s threads=%s fibers=%s ===", mem ? "OK" : "FAIL",
       thr ? "OK" : "FAIL", fib ? "OK" : "FAIL");
  return mem && thr && fib;
}
