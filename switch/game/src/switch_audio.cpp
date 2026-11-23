// Switch audio: the guest's XAudio frames -> libnx audout (48 kHz, stereo,
// s16). The guest renders frames of 256 samples x 6 channels (float, big
// endian, channel-sequential) and waits on a semaphore that the driver
// releases once per frame consumed. Here the frames are consumed at the rate
// audout plays them, so the hardware paces the guest audio thread (the role
// SDL's pull callback has on desktop).
//
// Buffers: kBuffers in flight, kFramesPerBuffer guest frames each
// (2 x 256 samples = 10.7 ms, ~43 ms of latency). A frame that is not ready
// in time becomes silence rather than stalling the output.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace {

using namespace rex;

constexpr uint32_t kChannelSamples = 256;  // per guest frame, per channel
constexpr uint32_t kGuestChannels = 6;
constexpr uint32_t kFrameFloats = kChannelSamples * kGuestChannels;
constexpr uint32_t kFramesPerBuffer = 2;
constexpr uint32_t kBuffers = 4;
constexpr uint32_t kBufferSamples = kChannelSamples * kFramesPerBuffer;     // stereo pairs
constexpr uint32_t kBufferBytes = kBufferSamples * 2 * sizeof(int16_t);     // 2048
constexpr uint32_t kBufferAlloc = (kBufferBytes + 0xFFF) & ~0xFFFu;         // audout: 0x1000

class SwitchAudioDriver : public audio::AudioDriver {
 public:
  SwitchAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {
    worker_ = std::thread([this] { Run(); });
  }
  ~SwitchAudioDriver() override { Shutdown(); }

  void SubmitFrame(uint32_t frame_ptr) override {
    const float* in = memory_->TranslateVirtual<const float*>(frame_ptr);
    std::vector<float> frame;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!pool_.empty()) {
        frame = std::move(pool_.back());
        pool_.pop_back();
      }
    }
    frame.resize(kFrameFloats);
    std::memcpy(frame.data(), in, kFrameFloats * sizeof(float));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queued_.push_back(std::move(frame));
    }
    cv_.notify_one();
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXAPU_INFO("switch-audio: first frame submitted");
  }

  void Shutdown() {
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

 private:
  void Run() {
    Result rc = audoutInitialize();
    if (R_SUCCEEDED(rc)) rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
      REXAPU_ERROR("switch-audio: audout init failed 0x{:X}; falling back to silence pacing", rc);
      RunSilent();
      return;
    }
    REXAPU_INFO("switch-audio: audout started, {} Hz, {} ch, {} buffers x {} samples",
                audoutGetSampleRate(), audoutGetChannelCount(), kBuffers, kBufferSamples);

    AudioOutBuffer bufs[kBuffers] = {};
    for (auto& b : bufs) {
      b.buffer = memalign(0x1000, kBufferAlloc);
      std::memset(b.buffer, 0, kBufferAlloc);
      b.buffer_size = kBufferAlloc;
      b.data_size = kBufferBytes;
      b.data_offset = 0;
    }

    uint32_t in_flight = 0;
    uint64_t underruns = 0, played = 0;
    float stereo[kChannelSamples * 2];
    while (!stop_.load()) {
      AudioOutBuffer* buf;
      if (in_flight < kBuffers) {
        buf = &bufs[in_flight++];
      } else {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        audoutWaitPlayFinish(&released, &count, 100000000ull);  // 100 ms
        if (!released || count == 0) continue;
        buf = released;
      }
      auto* out = static_cast<int16_t*>(buf->buffer);
      const audio::StereoFold fold = audio::GetStereoFold();
      const float gain = audio::GetOutputGain();
      for (uint32_t f = 0; f < kFramesPerBuffer; ++f) {
        std::vector<float> frame;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          // Give the guest a moment to deliver; then play silence instead.
          cv_.wait_for(lock, std::chrono::milliseconds(3),
                       [this] { return !queued_.empty() || stop_.load(); });
          if (!queued_.empty()) {
            frame = std::move(queued_.front());
            queued_.pop_front();
          }
        }
        int16_t* dst = out + f * kChannelSamples * 2;
        if (frame.empty()) {
          std::memset(dst, 0, kChannelSamples * 2 * sizeof(int16_t));
          ++underruns;
          continue;
        }
        audio::conversion::sequential_6_BE_to_interleaved_2_LE(stereo, frame.data(),
                                                               kChannelSamples, fold, gain);
        for (uint32_t i = 0; i < kChannelSamples * 2; ++i) {
          const float v = std::clamp(stereo[i], -1.0f, 1.0f);
          dst[i] = static_cast<int16_t>(v * 32767.0f);
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          pool_.push_back(std::move(frame));
        }
        semaphore_->Release(1, nullptr);
        ++played;
      }
      armDCacheFlush(buf->buffer, kBufferAlloc);
      audoutAppendAudioOutBuffer(buf);
      if ((played + underruns) % (187 * 2 * 10) == 0) {  // ~every 10 s
        REXAPU_INFO("switch-audio: {} frames played, {} silent slots", played, underruns);
      }
    }
    audoutStopAudioOut();
    audoutExit();
    for (auto& b : bufs) free(b.buffer);
  }

  // No audio device: keep the guest audio thread running at real-time pace.
  void RunSilent() {
    auto next = std::chrono::steady_clock::now();
    while (!stop_.load()) {
      next += std::chrono::microseconds(5333);
      std::this_thread::sleep_until(next);
      std::lock_guard<std::mutex> lock(mutex_);
      if (!queued_.empty()) {
        pool_.push_back(std::move(queued_.front()));
        queued_.pop_front();
        semaphore_->Release(1, nullptr);
      }
    }
  }

  rex::thread::Semaphore* semaphore_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::vector<float>> queued_;
  std::vector<std::vector<float>> pool_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

class SwitchAudioSystem : public audio::AudioSystem {
 public:
  explicit SwitchAudioSystem(runtime::FunctionDispatcher* fd) : AudioSystem(fd) {}
  ~SwitchAudioSystem() override = default;

  static std::unique_ptr<audio::AudioSystem> Create(runtime::FunctionDispatcher* fd) {
    return std::make_unique<SwitchAudioSystem>(fd);
  }

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        audio::AudioDriver** out_driver) override {
    REXAPU_INFO("switch-audio: creating driver {}", index);
    *out_driver = new SwitchAudioDriver(memory_, semaphore);
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(audio::AudioDriver* driver) override {
    if (auto* d = dynamic_cast<SwitchAudioDriver*>(driver)) d->Shutdown();
    delete driver;
  }
};

}  // namespace

std::unique_ptr<rex::system::IAudioSystem> CreateSwitchAudioSystem(
    rex::runtime::FunctionDispatcher* fd) {
  return SwitchAudioSystem::Create(fd);
}
