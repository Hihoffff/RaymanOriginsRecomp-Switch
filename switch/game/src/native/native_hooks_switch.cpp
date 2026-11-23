// Switch: the game's D3D Present hook (from upstream/rex/src/hooks.cpp, frame
// pacing part only). sub_826D5E38 is the game's Present - the only path to
// VdSwap. After the original runs: frame limiter (60), native capture and the
// native renderer's present, and a [perf] line every 2 s.
#ifndef RAYMAN_DIAG
#define RAYMAN_DIAG 0  // CMake option RAYMAN_SWITCH_DIAG
#endif
#include <switch.h>

#include <rex/hook.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

uint8_t* g_rayman_membase = nullptr;  // set in main.cpp after Runtime::Setup

REX_EXTERN(__imp__sub_826D5E38);
void RaymanNativeCaptureFrame();     // native_capture_switch.cpp
void RaymanNativeRendererPresent();  // native_renderer_switch.cpp
void SwitchInputPollApplet();        // switch_input.cpp

REX_HOOK_RAW(sub_826D5E38) {
  using clock = std::chrono::steady_clock;
  static clock::time_point window_start = clock::now(), last = window_start;
  static int frames = 0;
  static double worst_ms = 0;
  __imp__sub_826D5E38(ctx, base);
  {
    // The game's logic advances once per presented frame: cap it at 60.
    static clock::time_point next = clock::now();
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
    next += period;
    auto now_before = clock::now();
    if (next < now_before - period * 2) {
      next = now_before;
    } else {
      std::this_thread::sleep_until(next);
    }
  }
  RaymanNativeCaptureFrame();
  RaymanNativeRendererPresent();
  // Applet messages (sleep/wake, HOME, focus, operation mode): nobody else
  // pumps them. Non-blocking; libnx's default handler acks sleep etc.
  SwitchInputPollApplet();
  if (!appletMainLoop()) {
    static bool said = false;
    if (!said) {
      said = true;
      std::fprintf(stderr, "[switch] applet: exit requested\n");
    }
  }
  auto now = clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  worst_ms = ms > worst_ms ? ms : worst_ms;
  ++frames;
  double window = std::chrono::duration<double>(now - window_start).count();
  if (window >= 2.0) {
    if (RAYMAN_DIAG) std::fprintf(stderr, "[perf] %.1f fps (avg %.1f ms, worst %.1f ms)\n", frames / window,
                 window * 1000.0 / frames, worst_ms);
    window_start = now;
    frames = 0;
    worst_ms = 0;
  }
}
