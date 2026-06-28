#pragma once

#include "log/log.hpp"
#include "util_env.hpp"
#include <utility>

namespace dxmt {

inline bool D3D9TraceEnabled() {
  static const bool enabled = env::getEnvVar("DXMT_D3D9_TRACE") == "1";
  return enabled;
}

template <typename... Args>
inline void D3D9Trace(Args &&...args) {
  if (D3D9TraceEnabled())
    Logger::info(str::format("[d3d9] ", std::forward<Args>(args)...));
}

} // namespace dxmt

#define D3D9_TRACE(...) ::dxmt::D3D9Trace(__VA_ARGS__)
