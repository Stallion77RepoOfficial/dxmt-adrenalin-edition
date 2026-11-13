#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dxmt {

// Individual pieces of information that the HUD can display. Selected at runtime
// through the DXMT_HUD environment variable (DXVK_HUD-compatible syntax).
enum class HudElement : uint32_t {
  Version    = 1u << 0, // DXMT version + DirectX feature level heading
  Compat     = 1u << 1, // feature-compatibility flag line
  Commit     = 1u << 2, // command buffer commit timing
  Sync       = 1u << 3, // sync / present latency timing
  Encode     = 1u << 4, // encode timing breakdown
  RenderPass = 1u << 5, // render / clear pass counters
  Scaler     = 1u << 6, // MetalFX scaler info
};

// Text side of DXMT's own HUD overlay. The swapchain composes statistics
// lines each frame through begin()/printLine()/end(); end() publishes them to
// the shared HudContents mailbox that the presenter-side HudRenderer draws
// from. Fully independent of Apple's Metal Performance HUD: DXMT_HUD alone
// enables it, MTL_HUD_ENABLED is neither read nor written.
class HUDState {
public:
  HUDState() = default;

  // Parses DXMT_HUD and publishes the heading (version + feature level).
  void initialize(const std::string &heading);

  void begin();

  void
  printLine(const std::string &str) {
    printLine(str.c_str());
  }

  void printLine(const char *c_str);

  void end();

  bool enabled() const { return enabled_; }

  bool
  shows(HudElement element) const {
    return enabled_ && (elements_ & static_cast<uint32_t>(element));
  }

private:
  std::string heading_;
  std::vector<std::string> staging_lines_;
  bool enabled_ = false;
  uint32_t elements_ = 0;
};

} // namespace dxmt
