#include <array>
#include <cstdio>
#include "vk_renderer.h"

std::array<uint8_t, 24> Fetch(uint32_t width, uint32_t height, uint32_t format) {
  std::array<uint8_t, 24> bytes{};
  auto put = [&](size_t offset, uint32_t value) {
    for (int j = 0; j < 4; ++j) bytes[offset + j] = uint8_t(value >> (24 - j * 8));
  };
  put(4, 0x100000 | format);
  put(8, (height - 1) << 13 | (width - 1));
  return bytes;
}

int main() {
  native::Renderer renderer;
  renderer.frame_ = 11;
  std::array<uint8_t, native::kStateEnd - native::kStateBegin> state{};
  int32_t rect[4] = {0, 0, 1280, 720};
  auto remember = [&](const auto& fetch, int index) {
    auto t = xenos::DecodeFetch(fetch.data());
    auto& image = renderer.resolved_[uint64_t(t.base) << 32 | t.width << 16 | t.height];
    image.width = t.width;
    image.height = t.height;
    image.index = index;
    return &image;
  };
  auto cache = [&](const auto& fetch, int index) {
    auto key = uint64_t(xenos::BE32(fetch.data() + 4)) << 32 | xenos::BE32(fetch.data() + 8);
    auto& texture = renderer.textures_[key];
    texture.index = index;
    texture.checkedFrame = renderer.frame_;
  };
  auto check = [&](const auto& fetch, int expected, const char* description) {
    int actual = renderer.Texture(xenos::DecodeFetch(fetch.data()), fetch.data());
    if (actual == expected) return;
    std::fprintf(stderr, "%s: expected %d, got %d\n", description, expected, actual);
    std::exit(1);
  };
  auto scene = Fetch(1280, 720, 0x06);
  auto atlas = Fetch(256, 256, 0x06);
  auto blocks = Fetch(1280, 720, 0x12);
  auto* first = remember(scene, 3);
  remember(atlas, 4);
  renderer.resolvedByBase_[0x100000] = first;
  cache(scene, 7);
  cache(atlas, 8);
  cache(blocks, 9);

  renderer.Resolve(state.data(), rect, scene.data());
  check(scene, 3, "compatible resolve");
  check(atlas, 8, "same address, different size");
  check(blocks, 9, "same address and size, different format");

  renderer.Resolve(state.data(), rect, atlas.data());
  check(atlas, 4, "most recent resolve reactivates a cached image");
  check(scene, 7, "previous resolve falls back to guest texture");
  renderer.Resolve(state.data(), rect, scene.data());
  check(scene, 3, "original resolve is active again");
  renderer.textureCount_ = renderer.kMaxTextures;
  auto uncached = Fetch(320, 180, 0x06);
  renderer.Resolve(state.data(), rect, uncached.data());
  check(scene, 7, "dropped resolve no longer routes to stale image");
  std::puts("Resolve routing cases passed");
}
