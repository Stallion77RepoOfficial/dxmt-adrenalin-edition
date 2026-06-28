#include "dxmt_hud_state.hpp"
#include "Metal.hpp"
#include "util_env.hpp"
#include <string>

namespace dxmt {

namespace {

constexpr uint32_t kAllElements =
    static_cast<uint32_t>(HudElement::Version) | static_cast<uint32_t>(HudElement::Compat) |
    static_cast<uint32_t>(HudElement::Commit) | static_cast<uint32_t>(HudElement::Sync) |
    static_cast<uint32_t>(HudElement::Encode) | static_cast<uint32_t>(HudElement::RenderPass) |
    static_cast<uint32_t>(HudElement::Scaler);

// Parses the DXMT_HUD environment variable. Compatible with DXVK_HUD syntax:
//   ""/"0"  -> disabled
//   "1"     -> default set
//   "full"  -> everything
//   "a,b,c" -> comma-separated element list
uint32_t
parseHudElements(const std::string &config) {
  if (config.empty() || config == "0")
    return 0;

  if (config == "full")
    return kAllElements;

  if (config == "1")
    return static_cast<uint32_t>(HudElement::Version) | static_cast<uint32_t>(HudElement::Commit) |
           static_cast<uint32_t>(HudElement::Sync) | static_cast<uint32_t>(HudElement::Encode) |
           static_cast<uint32_t>(HudElement::RenderPass);

  // Always keep the version heading so the HUD doubles as proof that DXMT is
  // the active translation layer.
  uint32_t mask = static_cast<uint32_t>(HudElement::Version);
  size_t start = 0;
  while (start <= config.size()) {
    size_t comma = config.find(',', start);
    std::string token = config.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (token == "version" || token == "devinfo")
      mask |= static_cast<uint32_t>(HudElement::Version);
    else if (token == "compat")
      mask |= static_cast<uint32_t>(HudElement::Compat);
    else if (token == "submissions")
      mask |= static_cast<uint32_t>(HudElement::Commit);
    else if (token == "fps" || token == "frametimes")
      mask |= static_cast<uint32_t>(HudElement::Sync) | static_cast<uint32_t>(HudElement::Encode);
    else if (token == "drawcalls")
      mask |= static_cast<uint32_t>(HudElement::RenderPass);
    else if (token == "scale")
      mask |= static_cast<uint32_t>(HudElement::Scaler);
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return mask;
}

} // namespace

void
HUDState::initialize(const std::string &heading) {
  elements_ = parseHudElements(env::getEnvVar("DXMT_HUD"));
  enabled_ = elements_ != 0;
  if (!enabled_)
    return;

  using namespace WMT;
  auto pool = MakeAutoreleasePool();
  auto str_dxmt_version = MakeString("com.github.3shain.dxmt-version", WMTUTF8StringEncoding);
  hud_.addLabel(str_dxmt_version, String::string("com.apple.hud-graph.default", WMTUTF8StringEncoding));
  hud_.updateLabel(str_dxmt_version, String::string(heading.c_str(), WMTUTF8StringEncoding));
  line_labels_.push_back(std::move(str_dxmt_version));
}

void
HUDState::begin() {
  if (!enabled_)
    return;
  pool_ = WMT::MakeAutoreleasePool();
  current_line_ = 1;
}

void
HUDState::printLine(const char *c_str) {
  if (!enabled_)
    return;
  using namespace WMT;
  while (current_line_ >= line_labels_.size()) {
    String prev = line_labels_.back();
    line_labels_.push_back(MakeString(
        ("com.github.3shain.dxmt-line" + std::to_string(line_labels_.size())).c_str(), WMTUTF8StringEncoding
    ));
    hud_.addLabel(line_labels_.back(), prev);
  }
  hud_.updateLabel(line_labels_[current_line_], String::string(c_str, WMTUTF8StringEncoding));
  current_line_++;
}

void
HUDState::end() {
  if (!enabled_)
    return;
  while (line_labels_.size() > current_line_) {
    hud_.remove(line_labels_.back());
    line_labels_.pop_back();
  }
  pool_ = nullptr;
}

} // namespace dxmt
