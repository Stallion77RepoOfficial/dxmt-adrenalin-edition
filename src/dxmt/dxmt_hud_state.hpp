#pragma once

#include "Metal.hpp"
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

class HUDState {
public:
  // Registers the heading label when either DXMT_HUD or Apple's Metal HUD is
  // enabled. DXMT_HUD controls the optional extra DXMT statistics lines.
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

  HUDState(WMT::DeveloperHUDProperties hud) : hud_(hud){};

private:
  WMT::DeveloperHUDProperties hud_;
  std::vector<WMT::Reference<WMT::String>> line_labels_;
  WMT::Reference<WMT::Object> pool_;
  unsigned current_line_ = 0;
  bool enabled_ = false;
  uint32_t elements_ = 0;
};

} // namespace dxmt
