#include "dxmt_hud_state.hpp"
#include "dxmt_hud_renderer.hpp"
#include "util_env.hpp"
#include "log/log.hpp"
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
  heading_ = heading;
  if (!enabled_)
    return;

  auto &contents = GetHudContents();
  std::lock_guard<std::mutex> lock(contents.mutex);
  contents.active = true;
  if (elements_ & static_cast<uint32_t>(HudElement::Version))
    contents.heading = heading_;
}

void
HUDState::begin() {
  if (!enabled_)
    return;
  staging_lines_.clear();
}

void
HUDState::printLine(const char *c_str) {
  if (!enabled_)
    return;
  staging_lines_.emplace_back(c_str);
}

void
HUDState::end() {
  if (!enabled_)
    return;
  /* The HUD is on screen, which is no use when the run is being read back from
   * a log -- a game on another machine, or a session with no eyes on it.
   * DXMT_HUD_LOG=1 mirrors the same lines into the module log. */
  static const bool mirror_to_log = env::getEnvVar("DXMT_HUD_LOG") == "1";
  if (mirror_to_log) {
    for (auto &line : staging_lines_)
      Logger::info(line);
  }
  auto &contents = GetHudContents();
  std::lock_guard<std::mutex> lock(contents.mutex);
  if (elements_ & static_cast<uint32_t>(HudElement::Version))
    contents.heading = heading_;
  contents.lines = staging_lines_;
}

} // namespace dxmt
