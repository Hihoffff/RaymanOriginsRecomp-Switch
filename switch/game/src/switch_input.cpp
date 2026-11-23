// Switch input driver: up to 4 players from libnx pads -> Xbox 360 gamepads.
//
// Player N reads HidNpadIdType_No<N> (player 1 also reads Handheld). Every
// supported style works: Pro Controller, a Joy-Con pair, handheld, and a
// single Joy-Con held sideways (one per player for couch co-op).
//
// Face buttons are mapped by position, not by label: the bottom button is the
// Xbox A, so "jump" sits where Rayman players expect it.
//   Pair / Pro / handheld:  B->A  A->B  Y->X  X->Y  L/R->LB/RB  ZL/ZR->LT/RT
//                           +->Start  -->Back  sticks, d-pad
//   Single Joy-Con, sideways: the four face buttons by position after the
//   rotation, SL/SR -> LB/RB, + or - -> Start, the stick (HID rotates it).
//
// Holding - and + together for ~1 s opens the system "Controllers" applet
// (split/join Joy-Cons, assign players); it runs from the Present hook.
#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

#include <rex/input/device_assignment.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

namespace {

using namespace rex;
using namespace rex::input;

constexpr int kPlayers = 4;
constexpr uint32_t kPadIdBase = 0x53575031;  // 'SWP1', 'SWP2', ...

std::atomic<bool> g_applet_requested{false};

DeviceId PadId(int player) { return static_cast<DeviceId>(kPadIdBase + player); }

int16_t Axis(int32_t v) {
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return static_cast<int16_t>(v);
}

class SwitchInputDriver : public InputDriver {
 public:
  SwitchInputDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override {
    padConfigureInput(kPlayers, HidNpadStyleSet_NpadStandard);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    padInitialize(&pads_[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
    padInitialize(&pads_[1], HidNpadIdType_No2);
    padInitialize(&pads_[2], HidNpadIdType_No3);
    padInitialize(&pads_[3], HidNpadIdType_No4);
    REXSYS_INFO("switch-input: {} players, single Joy-Cons sideways; hold -+ for the Controllers applet",
                kPlayers);
    return X_STATUS_SUCCESS;
  }

  // Players are numbered by who pressed a button first, not by HID slot: a
  // controller shows up to the game only after its first press, and the
  // runtime gives each newly seen device the lowest free player number. So
  // whoever presses first is player 1 - the only signed-in profile, the one
  // that owns the saves - whatever controller it is or which slot HID put it
  // in. Others join as players 2-4 by pressing a button (Rayman's drop-in
  // co-op). A disconnected controller frees its number.
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    UpdateAll();
    bool changed = false;
    for (int slot = 0; slot < kPlayers; ++slot) {
      const bool connected = padIsConnected(&pads_[slot]);
      if (!connected && active_[slot]) {
        active_[slot] = false;
        order_.erase(std::find(order_.begin(), order_.end(), slot));
        changed = true;
      } else if (connected && !active_[slot] && padGetButtons(&pads_[slot]) != 0) {
        active_[slot] = true;
        order_.push_back(slot);
        changed = true;
      }
    }
    for (int slot : order_) {
      DeviceInfo info;
      info.id = PadId(slot);
      info.name = "Switch controller (HID No" + std::to_string(slot + 1) + ")";
      out.push_back(info);
    }
    if (changed) {
      std::string list;
      for (int slot : order_) list += " No" + std::to_string(slot + 1);
      REXSYS_INFO("switch-input: active controllers in join order:{}", list.empty() ? " (none)" : list);
    }
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    const int p = static_cast<int>(static_cast<uint32_t>(id) - kPadIdBase);
    if (p < 0 || p >= kPlayers) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard<std::mutex> lock(mutex_);
    UpdateAll();
    if (!active_[p]) return X_ERROR_DEVICE_NOT_CONNECTED;
    PadState& pad = pads_[p];
    if (!padIsConnected(&pad)) return X_ERROR_DEVICE_NOT_CONNECTED;

    const u64 held = padGetButtons(&pad);
    const u32 style = padGetStyleSet(&pad);
    const bool single_left = (style & HidNpadStyleTag_NpadJoyLeft) != 0;
    const bool single_right = (style & HidNpadStyleTag_NpadJoyRight) != 0;

    uint16_t b = 0;
    uint8_t lt = 0, rt = 0;
    HidAnalogStickState ls{}, rs{};
    if (single_left) {
      // Left Joy-Con sideways (stick on the left): the d-pad rotates 90 deg
      // counter-clockwise, so Left is the bottom button, Down the right one.
      if (held & HidNpadButton_Left) b |= X_INPUT_GAMEPAD_A;
      if (held & HidNpadButton_Down) b |= X_INPUT_GAMEPAD_B;
      if (held & HidNpadButton_Up) b |= X_INPUT_GAMEPAD_X;
      if (held & HidNpadButton_Right) b |= X_INPUT_GAMEPAD_Y;
      if (held & HidNpadButton_LeftSL) b |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
      if (held & HidNpadButton_LeftSR) b |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
      if (held & HidNpadButton_Minus) b |= X_INPUT_GAMEPAD_START;
      if (held & HidNpadButton_StickL) b |= X_INPUT_GAMEPAD_LEFT_THUMB;
      // HID does not rotate a sideways Joy-Con's stick: turn it with the
      // controller (90 deg counter-clockwise for the left one).
      const HidAnalogStickState raw = padGetStickPos(&pad, 0);
      ls.x = -raw.y;
      ls.y = raw.x;
      LogRawStick(p, raw);
    } else if (single_right) {
      // Right Joy-Con sideways (stick on the left): the face buttons rotate
      // 90 deg clockwise, so A is the bottom button, X the right one.
      if (held & HidNpadButton_A) b |= X_INPUT_GAMEPAD_A;
      if (held & HidNpadButton_X) b |= X_INPUT_GAMEPAD_B;
      if (held & HidNpadButton_B) b |= X_INPUT_GAMEPAD_X;
      if (held & HidNpadButton_Y) b |= X_INPUT_GAMEPAD_Y;
      if (held & HidNpadButton_RightSL) b |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
      if (held & HidNpadButton_RightSR) b |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
      if (held & HidNpadButton_Plus) b |= X_INPUT_GAMEPAD_START;
      if (held & HidNpadButton_StickR) b |= X_INPUT_GAMEPAD_LEFT_THUMB;
      // The right Joy-Con's only stick drives movement; sideways it is turned
      // 90 deg clockwise.
      const HidAnalogStickState raw = padGetStickPos(&pad, 1);
      ls.x = raw.y;
      ls.y = -raw.x;
      LogRawStick(p, raw);
    } else {
      if (held & HidNpadButton_Up) b |= X_INPUT_GAMEPAD_DPAD_UP;
      if (held & HidNpadButton_Down) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
      if (held & HidNpadButton_Left) b |= X_INPUT_GAMEPAD_DPAD_LEFT;
      if (held & HidNpadButton_Right) b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
      if (held & HidNpadButton_Plus) b |= X_INPUT_GAMEPAD_START;
      if (held & HidNpadButton_Minus) b |= X_INPUT_GAMEPAD_BACK;
      if (held & HidNpadButton_StickL) b |= X_INPUT_GAMEPAD_LEFT_THUMB;
      if (held & HidNpadButton_StickR) b |= X_INPUT_GAMEPAD_RIGHT_THUMB;
      if (held & HidNpadButton_L) b |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
      if (held & HidNpadButton_R) b |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
      if (held & HidNpadButton_B) b |= X_INPUT_GAMEPAD_A;
      if (held & HidNpadButton_A) b |= X_INPUT_GAMEPAD_B;
      if (held & HidNpadButton_Y) b |= X_INPUT_GAMEPAD_X;
      if (held & HidNpadButton_X) b |= X_INPUT_GAMEPAD_Y;
      lt = (held & HidNpadButton_ZL) ? 0xFF : 0;
      rt = (held & HidNpadButton_ZR) ? 0xFF : 0;
      ls = padGetStickPos(&pad, 0);
      rs = padGetStickPos(&pad, 1);
      // Hold - and + for ~1 s: Controllers applet.
      const bool combo = (held & HidNpadButton_Minus) && (held & HidNpadButton_Plus);
      auto now = std::chrono::steady_clock::now();
      if (!combo) {
        combo_since_[p] = {};
      } else if (combo_since_[p] == std::chrono::steady_clock::time_point{}) {
        combo_since_[p] = now;
      } else if (now - combo_since_[p] > std::chrono::milliseconds(900)) {
        combo_since_[p] = {};
        g_applet_requested.store(true);
      }
    }

    if (b != last_buttons_[p] || lt != last_lt_[p] || rt != last_rt_[p]) {
      ++packet_[p];
      if (b && !logged_[p]) {
        logged_[p] = true;
        REXSYS_INFO("switch-input: player {} first press 0x{:04X} (style 0x{:X})", p + 1, b, style);
      }
    }
    last_buttons_[p] = b;
    last_lt_[p] = lt;
    last_rt_[p] = rt;

    std::memset(out, 0, sizeof(*out));
    out->packet_number = packet_[p];
    out->gamepad.buttons = b;
    out->gamepad.left_trigger = lt;
    out->gamepad.right_trigger = rt;
    out->gamepad.thumb_lx = Axis(ls.x);
    out->gamepad.thumb_ly = Axis(ls.y);
    out->gamepad.thumb_rx = Axis(rs.x);
    out->gamepad.thumb_ry = Axis(rs.y);
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* caps) override {
    const int p = static_cast<int>(static_cast<uint32_t>(id) - kPadIdBase);
    if (p < 0 || p >= kPlayers) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    const int p = static_cast<int>(static_cast<uint32_t>(id) - kPadIdBase);
    return (p >= 0 && p < kPlayers) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    const int p = static_cast<int>(static_cast<uint32_t>(id) - kPadIdBase);
    return (p >= 0 && p < kPlayers) ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  // First clear deflection per player, raw (before rotation): tells how HID
  // reports a sideways Joy-Con if the mapping ever looks wrong.
  void LogRawStick(int p, const HidAnalogStickState& raw) {
    if (stick_logged_[p] || (std::abs(raw.x) < 20000 && std::abs(raw.y) < 20000)) return;
    stick_logged_[p] = true;
    REXSYS_INFO("switch-input: player {} single Joy-Con raw stick x={} y={}", p + 1, raw.x, raw.y);
  }

  // At most once per millisecond: GetState runs per user, per poll.
  void UpdateAll() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_update_ < std::chrono::milliseconds(1)) return;
    last_update_ = now;
    for (auto& pad : pads_) padUpdate(&pad);
  }

  std::mutex mutex_;
  std::array<PadState, kPlayers> pads_{};
  std::chrono::steady_clock::time_point last_update_{};
  std::array<std::chrono::steady_clock::time_point, kPlayers> combo_since_{};
  std::array<uint16_t, kPlayers> last_buttons_{};
  std::array<uint8_t, kPlayers> last_lt_{}, last_rt_{};
  std::array<uint32_t, kPlayers> packet_{};
  std::array<bool, kPlayers> logged_{};
  std::array<bool, kPlayers> stick_logged_{};
  std::array<bool, kPlayers> active_{};  // pressed a button since connecting
  std::vector<int> order_;               // active slots, in join order
};

}  // namespace

// Called once per frame from the Present hook: shows the system Controllers
// applet when a player held - and +. Blocks (the game pauses) while it is up.
void SwitchInputPollApplet() {
  if (!g_applet_requested.exchange(false)) return;
  HidLaControllerSupportArg arg;
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = 1;
  arg.hdr.player_count_max = kPlayers;
  arg.hdr.enable_single_mode = 0;  // allow several players, single Joy-Cons
  HidLaControllerSupportResultInfo info{};
  Result rc = hidLaShowControllerSupport(&info, &arg);
  REXSYS_INFO("switch-input: Controllers applet -> 0x{:X}, {} players", rc, (int)info.player_count);
}

std::unique_ptr<rex::system::IInputSystem> CreateSwitchInputSystem() {
  auto system = std::make_unique<rex::input::InputSystem>(nullptr);
  // AddDriver does not call Setup (CreateDefaultInputSystem does it itself),
  // and without a device assignment GetState answers "not connected".
  auto driver = std::make_unique<SwitchInputDriver>();
  driver->Setup();
  system->AddDriver(std::move(driver));
  system->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  return system;
}
