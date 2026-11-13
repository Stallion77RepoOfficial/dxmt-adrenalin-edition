#pragma once

#include "Metal.hpp"
#include <map>

namespace dxmt {
enum class FormatCapability : int {
  None = 0,
  Atomic = 0x1,
  Filter = 0x2,
  Write = 0x4,
  Color = 0x8,
  Blend = 0x10,
  MSAA = 0x20,
  Sparse = 0x40,
  Resolve = 0x80,
  DepthStencil = 0x100,
  TextureBufferRead = 0x200,
  TextureBufferWrite = 0x400,
  TextureBufferReadWrite = 0x800
};

class FormatCapabilityInspector {
public:
  std::map<WMTPixelFormat, FormatCapability> textureCapabilities{};
  void Inspect(WMT::Device device);
};

WMTPixelFormat Forget_sRGB(WMTPixelFormat format);
WMTPixelFormat Recall_sRGB(WMTPixelFormat format);

inline bool
Is_sRGBVariant(WMTPixelFormat format) {
  return Forget_sRGB(format) != format;
}

bool IsBlockCompressionFormat(WMTPixelFormat format);

uint32_t DepthStencilPlanarFlags(WMTPixelFormat format);

enum MTL_DXGI_FORMAT_FLAG {
  MTL_DXGI_FORMAT_TYPELESS = 1,
  MTL_DXGI_FORMAT_BC = 2,
  MTL_DXGI_FORMAT_BACKBUFFER = 4,
  MTL_DXGI_FORMAT_DEPTH_PLANER = 16,
  MTL_DXGI_FORMAT_STENCIL_PLANER = 32,
  MTL_DXGI_FORMAT_EMULATED_D24 = 256,
  MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL = 512,
};

struct MTL_DXGI_FORMAT_DESC {
  WMTPixelFormat PixelFormat;
  WMTAttributeFormat AttributeFormat;
  union {
    uint32_t BytesPerTexel;
    uint32_t BlockSize;
  };
  uint32_t Flag;
};

/* View formats only DXMT's D3D9 layer creates, far above every DXGI_FORMAT.
 * D3D9 fills the channels a format lacks differently from D3D10 and later --
 * luminance in all three colour channels, a missing channel as 1 -- and Metal
 * expresses that as the view's swizzle. */
enum DXMT_D3D9_VIEW_FORMAT : uint32_t {
  DXMT_FORMAT_R8_UNORM_RRR1 = 0x10000, // L8
  DXMT_FORMAT_R8G8_UNORM_RRRG,         // A8L8
  DXMT_FORMAT_R16_UNORM_RRR1,          // L16
  DXMT_FORMAT_R8G8_SNORM_RG11,         // V8U8
  DXMT_FORMAT_R16G16_SNORM_RG11,       // V16U16
  DXMT_FORMAT_R16G16_UNORM_RG11,       // G16R16
  DXMT_FORMAT_R16_FLOAT_R111,          // R16F
  DXMT_FORMAT_R16G16_FLOAT_RG11,       // G16R16F
  DXMT_FORMAT_R32_FLOAT_R111,          // R32F
  DXMT_FORMAT_R32G32_FLOAT_RG11,       // G32R32F
  DXMT_FORMAT_B5G5R5X1_UNORM,          // X1R5G5B5
  DXMT_FORMAT_R24_UNORM_X8_RRRR,       // INTZ: depth in every channel
  DXMT_FORMAT_R16_UNORM_R001,          // DF16: depth in red, 0 0 1
  DXMT_FORMAT_BC4_UNORM_RRRR,          // ATI1
  DXMT_FORMAT_BC5_UNORM_GR11,          // ATI2
};

int32_t MTLQueryDXGIFormat(WMT::Device device, uint32_t format, MTL_DXGI_FORMAT_DESC &description);

uint32_t MTLGetTexelSize(WMTPixelFormat format);

WMTPixelFormat MTLGetUnsignedIntegerFormat(WMTPixelFormat format);

bool IsUnorm8RenderTargetFormat(WMTPixelFormat format);

bool IsIntegerFormat(WMTPixelFormat format);

void SanitizeRTVClearColor(WMTPixelFormat format, WMTClearColor &color);

} // namespace dxmt