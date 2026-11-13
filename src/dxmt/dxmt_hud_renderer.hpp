#pragma once

// DXMT's own present-time HUD overlay. Fully independent from Apple's Metal
// Performance HUD (no MTLHUDEnabled / _CADeveloperHUDProperties): the
// presenter draws it straight into the drawable after the backbuffer blit,
// so DXMT_HUD=1 alone is enough to show it.
//
// Layout mirrors the Metal HUD (top-right panel with live statistics and a
// frame-time graph) and extends it with DXMT-specific information: version
// heading, the DXMT DLLs loaded in the process, and the per-frame translation
// statistics published by the swapchain (HUDState lines).

#include "Metal.hpp"
#include "dxmt_command.hpp"
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace dxmt {

// Shared mailbox between HUDState (producer, swapchain thread) and
// HudRenderer (consumer, encoding thread).
struct HudContents {
  std::mutex mutex;
  bool active = false;
  std::string heading;
  std::vector<std::string> lines;
};

HudContents &GetHudContents();

class HudRenderer {
public:
  HudRenderer(WMT::Device device, InternalCommandLibrary &lib);

  // Appends the HUD draw calls to the presenter's drawable render pass.
  void encode(WMT::RenderCommandEncoder encoder, WMTPixelFormat format, double width, double height);

private:
  struct Vertex {
    float x, y;  // NDC
    float u, v;  // atlas pixel coords, u < 0 -> solid fill
    uint32_t color; // RGBA bytes (R in the lowest byte)
  };

  static constexpr uint32_t kFrameHistory = 128;
  static constexpr uint32_t kSliceCount = 3;          // frames in flight
  static constexpr uint32_t kSliceBytes = 512 * 1024; // vertex budget per frame

  bool ensurePipeline(WMTPixelFormat format);
  void buildFontTexture();
  void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color);
  void fill(float x, float y, float w, float h, uint32_t color);
  void text(float x, float y, float scale, uint32_t color, const std::string &str);

  WMT::Device device_;
  InternalCommandLibrary &lib_;
  WMT::Reference<WMT::Texture> font_;
  WMT::Reference<WMT::RenderPipelineState> pso_;
  WMTPixelFormat pso_format_ = WMTPixelFormatInvalid;
  WMT::Reference<WMT::Buffer> vbuf_;
  void *vbuf_mem_ = nullptr;
  uint32_t slice_ = 0;
  std::vector<Vertex> verts_;
  double width_ = 1, height_ = 1;

  std::chrono::steady_clock::time_point last_frame_{};
  float frame_times_[kFrameHistory] = {}; // milliseconds
  uint32_t frame_cursor_ = 0;
  std::string dll_line_;
};

} // namespace dxmt
