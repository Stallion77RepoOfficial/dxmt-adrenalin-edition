#include "dxmt_device.hpp"
#include "config/config.hpp"
#include "dxmt_command_queue.hpp"
#include "Metal.hpp"

namespace dxmt {

class DeviceImpl : public Device {
public:
  virtual WMT::Device
  device() override {
    return device_;
  };
  virtual CommandQueue &
  queue() override {
    return cmd_queue_;
  };

  virtual WMTMetalVersion metalVersion() override {
    return metal_version_;
  };

  virtual uint64_t maxObjectThreadgroups() final {
    return max_object_threadgroups_;
  };

  DeviceImpl(const DEVICE_DESC &desc) : device_(desc.device), cmd_queue_(device_) {
    uint64_t macos_major_version = 0, macos_minor_version = 0;
    int version_conf = Config::getInstance().getOption<int>("dxmt.shaderMetalVersion", 0);
    switch (version_conf) {
    case WMTMetal310:
    case WMTMetal320:
    case WMTMetal400:
      metal_version_ = (WMTMetalVersion)version_conf;
      break;
    default:
      metal_version_ = WMTMetalVersionMax;
      break;
    }
    WMTGetOSVersion(&macos_major_version, &macos_minor_version, nullptr);
    // macOS 26 betas reported themselves as 16, hence the second check.
    if (macos_major_version >= 26 || macos_major_version == 16) {
      metal_version_ = std::min(WMTMetal400, metal_version_);
    } else if (macos_major_version >= 15) {
      metal_version_ = std::min(WMTMetal320, metal_version_);
    } else {
      metal_version_ = std::min(WMTMetal310, metal_version_);
    }
    if (!device_.supportsFamily(WMTGPUFamilyApple7)) {
      WARN("Experimental non-Apple GPU support");
      metal_version_ = WMTMetal310; // later Metal feature-set gates assume an Apple GPU
      max_object_threadgroups_ = 1024;
      // macOS 26 bug: setShouldMaximizeConcurrentCompilation crashes on AMDGPU
      if (!(macos_major_version >= 16 && macos_major_version <= 26 && macos_minor_version < 2))
        device_.setShouldMaximizeConcurrentCompilation(true);
    } else {
      max_object_threadgroups_ = -1ull;
      /* Skipped on 26.0 and 26.1 only. Testing major >= 26 also matched 27.0,
       * 27.1, 28.0, ... and left concurrent compilation off there, which
       * measured up to 2x slower parallel PSO creation on macOS 27.0. */
      if (!(macos_major_version == 26 && macos_minor_version < 2))
        device_.setShouldMaximizeConcurrentCompilation(true);
    }
  }

private:
  WMT::Reference<WMT::Device> device_;
  CommandQueue cmd_queue_;
  WMTMetalVersion metal_version_;
  uint64_t max_object_threadgroups_;
};

std::unique_ptr<Device>
CreateDXMTDevice(const DEVICE_DESC &desc) {
  return std::make_unique<DeviceImpl>(desc);
};

} // namespace dxmt
