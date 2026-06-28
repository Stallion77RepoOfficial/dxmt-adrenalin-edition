#pragma once

#include "d3d9_private.h"

namespace dxmt {

// Information about how a D3DFORMAT maps onto the D3D11/DXGI world.
struct D3D9FormatInfo {
  DXGI_FORMAT Resource; // format used to create the ID3D11 resource (may be typeless for depth)
  DXGI_FORMAT View;     // format used for SRV / RTV
  DXGI_FORMAT DSV;      // format used for DSV (DXGI_FORMAT_UNKNOWN if not depth)
  bool        Depth;    // true if this is a depth/stencil format
};

// Map a D3DFORMAT to its DXGI equivalents. Returns false for formats we cannot
// represent (caller should fail resource creation in that case).
inline bool
GetD3D9FormatInfo(D3DFORMAT Format, D3D9FormatInfo *pInfo) {
  D3D9FormatInfo info = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false };

  switch (Format) {
    // --- Color, 32 bpp ---
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
      info.Resource = info.View = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
      info.Resource = info.View = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case D3DFMT_A2B10G10R10:
      info.Resource = info.View = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case D3DFMT_G16R16:
      info.Resource = info.View = DXGI_FORMAT_R16G16_UNORM; break;

    // --- Color, 16 bpp ---
    case D3DFMT_R5G6B5:
      info.Resource = info.View = DXGI_FORMAT_B5G6R5_UNORM; break;
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
      info.Resource = info.View = DXGI_FORMAT_B5G5R5A1_UNORM; break;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
      info.Resource = info.View = DXGI_FORMAT_B4G4R4A4_UNORM; break;

    // --- Single / dual channel ---
    case D3DFMT_A8:
      info.Resource = info.View = DXGI_FORMAT_A8_UNORM; break;
    case D3DFMT_L8:
      info.Resource = info.View = DXGI_FORMAT_R8_UNORM; break;
    case D3DFMT_A8L8:
      info.Resource = info.View = DXGI_FORMAT_R8G8_UNORM; break;

    // --- Floating point ---
    case D3DFMT_R16F:
      info.Resource = info.View = DXGI_FORMAT_R16_FLOAT; break;
    case D3DFMT_G16R16F:
      info.Resource = info.View = DXGI_FORMAT_R16G16_FLOAT; break;
    case D3DFMT_A16B16G16R16F:
      info.Resource = info.View = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case D3DFMT_R32F:
      info.Resource = info.View = DXGI_FORMAT_R32_FLOAT; break;
    case D3DFMT_G32R32F:
      info.Resource = info.View = DXGI_FORMAT_R32G32_FLOAT; break;
    case D3DFMT_A32B32G32R32F:
      info.Resource = info.View = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
    case D3DFMT_A16B16G16R16:
      info.Resource = info.View = DXGI_FORMAT_R16G16B16A16_UNORM; break;

    // --- Compressed ---
    case D3DFMT_DXT1:
      info.Resource = info.View = DXGI_FORMAT_BC1_UNORM; break;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
      info.Resource = info.View = DXGI_FORMAT_BC2_UNORM; break;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
      info.Resource = info.View = DXGI_FORMAT_BC3_UNORM; break;

    // --- Depth / stencil ---
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
      info.Depth = true;
      info.Resource = DXGI_FORMAT_R16_TYPELESS;
      info.View     = DXGI_FORMAT_R16_UNORM;
      info.DSV      = DXGI_FORMAT_D16_UNORM; break;
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
      info.Depth = true;
      info.Resource = DXGI_FORMAT_R32_TYPELESS;
      info.View     = DXGI_FORMAT_R32_FLOAT;
      info.DSV      = DXGI_FORMAT_D32_FLOAT; break;
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
      info.Depth = true;
      info.Resource = DXGI_FORMAT_R24G8_TYPELESS;
      info.View     = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      info.DSV      = DXGI_FORMAT_D24_UNORM_S8_UINT; break;

    default:
      return false;
  }

  *pInfo = info;
  return true;
}

// Bytes per pixel for an uncompressed D3DFORMAT (used for lock pitch / UP draws).
// Returns 0 for compressed or unknown formats.
inline UINT
GetD3D9FormatBytesPerPixel(D3DFORMAT Format) {
  switch (Format) {
    case D3DFMT_A32B32G32R32F:                       return 16;
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_G32R32F:                             return 8;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_G16R16:   case D3DFMT_R32F:
    case D3DFMT_G16R16F:                             return 4;
    case D3DFMT_R5G6B5:   case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8L8:
    case D3DFMT_R16F:                                return 2;
    case D3DFMT_A8: case D3DFMT_L8:                  return 1;
    default:                                         return 0;
  }
}

} // namespace dxmt
