#pragma once

#include "d3d9_private.h"
#include "dxmt_format.hpp"
#include <vector>
#include <algorithm>

namespace dxmt {

// Information about how a D3DFORMAT maps onto the D3D11/DXGI world.
struct D3D9FormatInfo {
  DXGI_FORMAT Resource; // format used to create the ID3D11 resource (may be typeless for depth)
  DXGI_FORMAT View;     // format used for SRV / RTV
  DXGI_FORMAT DSV;      // format used for DSV (DXGI_FORMAT_UNKNOWN if not depth)
  bool        Depth;    // true if this is a depth/stencil format
  // D3D9 makes sRGB a sampler state, D3D11 and Metal make it a property of the
  // view. So a texture a game may set D3DSAMP_SRGBTEXTURE on has to be created
  // typeless and read through whichever of the two views the state selects.
  // Both are UNKNOWN for a format with no sRGB variant, and the texture is
  // then created from Resource as before.
  DXGI_FORMAT ResourceTypeless;
  DXGI_FORMAT ViewSRGB;
  // Format a texture is sampled through. D3D9 fills the channels a format
  // lacks its own way -- L8 reads (l, l, l, 1), V8U8 (u, v, 1, 1), an X
  // channel 1 -- which DXMT's private view formats express as a swizzle
  // (dxmt_format.hpp). A render target never takes one, so this is apart
  // from View.
  DXGI_FORMAT SRV;
};

// Map a D3DFORMAT to its DXGI equivalents. Returns false for formats we cannot
// represent (caller should fail resource creation in that case).
inline bool
GetD3D9FormatInfo(D3DFORMAT Format, D3D9FormatInfo *pInfo) {
  D3D9FormatInfo info = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                          false, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };

  switch (Format) {
    // --- Color, 32 bpp ---
    case D3DFMT_A8R8G8B8:
      info.Resource = info.View = DXGI_FORMAT_B8G8R8A8_UNORM;
      info.ResourceTypeless = DXGI_FORMAT_B8G8R8A8_TYPELESS;
      info.ViewSRGB = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case D3DFMT_X8R8G8B8:
      info.Resource = info.View = DXGI_FORMAT_B8G8R8A8_UNORM;
      info.ResourceTypeless = DXGI_FORMAT_B8G8R8A8_TYPELESS;
      info.SRV = DXGI_FORMAT_B8G8R8X8_UNORM;
      info.ViewSRGB = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; break;
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
      info.Resource = info.View = DXGI_FORMAT_R8G8B8A8_UNORM;
      info.ResourceTypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
      info.ViewSRGB = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case D3DFMT_A2B10G10R10:
      info.Resource = info.View = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case D3DFMT_G16R16:
      info.Resource = info.View = DXGI_FORMAT_R16G16_UNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R16G16_UNORM_RG11; break;

    // --- Color, 16 bpp ---
    case D3DFMT_R5G6B5:
      info.Resource = info.View = DXGI_FORMAT_B5G6R5_UNORM; break;
    case D3DFMT_X1R5G5B5:
      info.Resource = info.View = DXGI_FORMAT_B5G5R5A1_UNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_B5G5R5X1_UNORM; break;
    case D3DFMT_A1R5G5B5:
      info.Resource = info.View = DXGI_FORMAT_B5G5R5A1_UNORM; break;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
      // Metal does not support DXGI_FORMAT_B4G4R4A4_UNORM. Store these
      // legacy 16-bit formats in BGRA8 and convert on CPU locks/uploads.
      info.Resource = info.View = DXGI_FORMAT_B8G8R8A8_UNORM; break;

    // --- Single / dual channel ---
    case D3DFMT_A8:
      info.Resource = info.View = DXGI_FORMAT_A8_UNORM; break;
    case D3DFMT_L8:
      info.Resource = info.View = DXGI_FORMAT_R8_UNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R8_UNORM_RRR1; break;
    case D3DFMT_A8L8:
      info.Resource = info.View = DXGI_FORMAT_R8G8_UNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R8G8_UNORM_RRRG; break;
    case D3DFMT_L16:
      info.Resource = info.View = DXGI_FORMAT_R16_UNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R16_UNORM_RRR1; break;

    // --- Bump map ---
    // Signed formats, which is the whole point of them: the texel carries a
    // normal offset in -1..1. Only the ones with a direct DXGI equivalent are
    // here; L6V5U5, X8L8V8U8, A2W10V10U10 and CxV8U8 mix signed and unsigned
    // channels in one texel and have none, so they stay unsupported rather
    // than being mapped to something that reads back wrong.
    case D3DFMT_V8U8:
      info.Resource = info.View = DXGI_FORMAT_R8G8_SNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R8G8_SNORM_RG11; break;
    case D3DFMT_Q8W8V8U8:
      info.Resource = info.View = DXGI_FORMAT_R8G8B8A8_SNORM; break;
    case D3DFMT_V16U16:
      info.Resource = info.View = DXGI_FORMAT_R16G16_SNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R16G16_SNORM_RG11; break;

    // --- Floating point ---
    case D3DFMT_R16F:
      info.Resource = info.View = DXGI_FORMAT_R16_FLOAT;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R16_FLOAT_R111; break;
    case D3DFMT_G16R16F:
      info.Resource = info.View = DXGI_FORMAT_R16G16_FLOAT;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R16G16_FLOAT_RG11; break;
    case D3DFMT_A16B16G16R16F:
      info.Resource = info.View = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case D3DFMT_R32F:
      info.Resource = info.View = DXGI_FORMAT_R32_FLOAT;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R32_FLOAT_R111; break;
    case D3DFMT_G32R32F:
      info.Resource = info.View = DXGI_FORMAT_R32G32_FLOAT;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_R32G32_FLOAT_RG11; break;
    case D3DFMT_A32B32G32R32F:
      info.Resource = info.View = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
    case D3DFMT_A16B16G16R16:
      info.Resource = info.View = DXGI_FORMAT_R16G16B16A16_UNORM; break;

    // --- Compressed ---
    case D3DFMT_DXT1:
      info.Resource = info.View = DXGI_FORMAT_BC1_UNORM;
      info.ResourceTypeless = DXGI_FORMAT_BC1_TYPELESS;
      info.ViewSRGB = DXGI_FORMAT_BC1_UNORM_SRGB; break;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
      info.Resource = info.View = DXGI_FORMAT_BC2_UNORM;
      info.ResourceTypeless = DXGI_FORMAT_BC2_TYPELESS;
      info.ViewSRGB = DXGI_FORMAT_BC2_UNORM_SRGB; break;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
      info.Resource = info.View = DXGI_FORMAT_BC3_UNORM;
      info.ResourceTypeless = DXGI_FORMAT_BC3_TYPELESS;
      info.ViewSRGB = DXGI_FORMAT_BC3_UNORM_SRGB; break;

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
    // D24FS8 differs from D24S8 only in how the depth range is interpreted,
    // which is a viewport property here rather than a format one.
    case D3DFMT_D24FS8:
      info.Depth = true;
      info.Resource = DXGI_FORMAT_R24G8_TYPELESS;
      info.View     = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      info.DSV      = DXGI_FORMAT_D24_UNORM_S8_UINT; break;

    // --- Vendor FOURCC formats ---
    // Not in D3DFORMAT at all: they are agreed conventions a game discovers
    // with CheckDeviceFormat and uses if present. They are also what a D3D9
    // game from 2007 onwards is built around, so refusing them costs far more
    // than the enumerated formats above.
    //
    // INTZ is the important one -- a depth buffer that can also be sampled.
    // Without it there is no depth-based screen-space effect at all: no SSAO,
    // no soft particles, no depth of field, no deferred lighting. DF16 and
    // DF24 are the same idea from the other vendor.
    /* Sampled, the three read their depth into different channels: INTZ
     * into all four, DF24 and DF16 into red with (0, 0, 1) after it. */
    case (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'):
      info.Depth = true;
      info.Resource = DXGI_FORMAT_R24G8_TYPELESS;
      info.View     = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      info.SRV      = (DXGI_FORMAT)DXMT_FORMAT_R24_UNORM_X8_RRRR;
      info.DSV      = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
    case (D3DFORMAT)MAKEFOURCC('D', 'F', '2', '4'):
      info.Depth = true;
      info.Resource = DXGI_FORMAT_R24G8_TYPELESS;
      info.View     = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      info.DSV      = DXGI_FORMAT_D24_UNORM_S8_UINT; break;
    case (D3DFORMAT)MAKEFOURCC('D', 'F', '1', '6'):
      info.Depth = true;
      info.Resource = DXGI_FORMAT_R16_TYPELESS;
      info.View     = DXGI_FORMAT_R16_UNORM;
      info.SRV      = (DXGI_FORMAT)DXMT_FORMAT_R16_UNORM_R001;
      info.DSV      = DXGI_FORMAT_D16_UNORM; break;
    // 3Dc normal map compression, which is BC4 and BC5 under their original
    // names. Exact, not an approximation. Sampled, ATI1 repeats its channel
    // in all four, and ATI2 reads its first channel into green -- a normal
    // map's X lands where D3D9 shaders look for it.
    case (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '1'):
      info.Resource = info.View = DXGI_FORMAT_BC4_UNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_BC4_UNORM_RRRR; break;
    case (D3DFORMAT)MAKEFOURCC('A', 'T', 'I', '2'):
      info.Resource = info.View = DXGI_FORMAT_BC5_UNORM;
      info.SRV = (DXGI_FORMAT)DXMT_FORMAT_BC5_UNORM_GR11; break;

    // --- Remaining enumerated formats, with a direct equivalent ---
    case D3DFMT_Q16W16V16U16:
      info.Resource = info.View = DXGI_FORMAT_R16G16B16A16_SNORM; break;
    case D3DFMT_G8R8_G8B8:
      info.Resource = info.View = DXGI_FORMAT_G8R8_G8B8_UNORM; break;
    case D3DFMT_R8G8_B8G8:
      info.Resource = info.View = DXGI_FORMAT_R8G8_B8G8_UNORM; break;
    case D3DFMT_A2B10G10R10_XR_BIAS:
      info.Resource = info.View = DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM; break;

    default:
      // Everything left is refused on purpose, and CheckDeviceFormat says so
      // rather than a resource being created that reads back wrong:
      //
      //   A2R10G10B10   DXGI has only R10G10B10A2, the opposite channel
      //                 order. Metal has BGR10A2, so this becomes mappable
      //                 once a format can be named that DXGI cannot express.
      //   R8G8B8        24 bpp, no equivalent anywhere, and D3D9 hardware
      //                 largely refused it too.
      //   P8, A8P8      palettised; there is no palette to sample through.
      //   L6V5U5, X8L8V8U8, A2W10V10U10, CxV8U8
      //                 signed and unsigned channels in one texel.
      //   A4L4, A1, R3G3B2, A8R3G3B2
      //                 sub-byte channel layouts with no equivalent.
      //   D15S1, D24X4S4
      //                 legacy depth layouts no modern format matches.
      //   UYVY, YUY2, MULTI2_ARGB8
      //                 video and multi-element formats no game path needs.
      //   VERTEXDATA, INDEX16, INDEX32, BINARYBUFFER
      //                 buffer formats, never a texture, handled elsewhere.
      return false;
  }

  if (!info.SRV)
    info.SRV = info.View;
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
    case D3DFMT_Q16W16V16U16:                        return 8;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_G16R16:   case D3DFMT_R32F:
    case D3DFMT_G16R16F:  case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:   case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE: case D3DFMT_D24S8:
    case D3DFMT_D24X8:    case D3DFMT_D24FS8:        return 4;
    case D3DFMT_R5G6B5:   case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8L8:
    case D3DFMT_R16F:     case D3DFMT_L16:
    case D3DFMT_V8U8:     case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:                        return 2;
    case D3DFMT_A8: case D3DFMT_L8:                  return 1;
    case D3DFMT_D32_LOCKABLE:                        return 4;
    case D3DFMT_S8_LOCKABLE:                         return 1;
    /* Formats no GPU texture here takes, whose layout a scratch resource --
     * memory only -- still needs. */
    case D3DFMT_A2R10G10B10: case D3DFMT_X8L8V8U8:
    case D3DFMT_A2W10V10U10: case D3DFMT_D24X4S4:    return 4;
    case D3DFMT_R8G8B8:                              return 3;
    case D3DFMT_A8P8: case D3DFMT_A8R3G3B2:
    case D3DFMT_L6V5U5: case D3DFMT_CxV8U8:
    case D3DFMT_D15S1:                               return 2;
    case D3DFMT_P8: case D3DFMT_A4L4:
    case D3DFMT_R3G3B2:                              return 1;
    default:                                         return 0;
  }
}

/* The addressable unit of a format's memory layout: a 4x4 block for the
 * block-compressed formats, a single pixel otherwise. A locked sub-rectangle
 * starts at (top / height) rows of blocks and (left / width) blocks in.
 * Returns false when the layout is not known here. */
inline bool
GetD3D9FormatBlockLayout(D3DFORMAT Format, UINT *pBlockWidth, UINT *pBlockHeight, UINT *pBytesPerBlock) {
  switch ((DWORD)Format) {
    case D3DFMT_DXT1:
    case MAKEFOURCC('A', 'T', 'I', '1'):
      *pBlockWidth = *pBlockHeight = 4; *pBytesPerBlock = 8; return true;
    case D3DFMT_DXT2: case D3DFMT_DXT3:
    case D3DFMT_DXT4: case D3DFMT_DXT5:
    case MAKEFOURCC('A', 'T', 'I', '2'):
      *pBlockWidth = *pBlockHeight = 4; *pBytesPerBlock = 16; return true;
    /* Two pixels sharing their chroma in one 32-bit word. */
    case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8:
    case D3DFMT_YUY2: case D3DFMT_UYVY:
      *pBlockWidth = 2; *pBlockHeight = 1; *pBytesPerBlock = 4; return true;
    default:
      *pBlockWidth = *pBlockHeight = 1;
      *pBytesPerBlock = GetD3D9FormatBytesPerPixel(Format);
      return *pBytesPerBlock != 0;
  }
}

/* The DXTn formats, whose top level D3D9 refuses to create unless it is a
 * whole number of blocks across and down. */
inline bool
D3D9FormatIsDxt(D3DFORMAT Format) {
  return Format == D3DFMT_DXT1 || Format == D3DFMT_DXT2 || Format == D3DFMT_DXT3 ||
         Format == D3DFMT_DXT4 || Format == D3DFMT_DXT5;
}

/* The memory a lock hands out for one subresource: D3D9's own layout of the
 * format, rows of blocks a DWORD-aligned pitch apart, as Windows lays out a
 * system-memory copy. */
struct D3D9LockLayout {
  UINT width = 0, height = 0, depth = 0;
  UINT block_width = 1, block_height = 1, block_bytes = 0;
  /* What a lock reports. */
  UINT row_pitch = 0;
  UINT slice_pitch = 0;
  /* How the memory is laid out, rows of blocks this far apart. The same as
   * the above, except for ATI1N and ATI2N: those report a pitch of a byte a
   * pixel -- how AMD defined them, which every Windows driver kept and
   * applications were written against -- while the memory holds their
   * blocks packed back to back. */
  UINT stored_row_pitch = 0;
  UINT stored_slice_pitch = 0;
  bool broken_pitch = false;
  size_t size = 0;         // what the level's data needs
  size_t level_bytes = 0;  // how far the next level starts in a texture's memory

  UINT BlockColumns() const { return (width + block_width - 1) / block_width; }
  UINT BlockRows() const { return (height + block_height - 1) / block_height; }
  size_t Size() const { return size; }
  /* Where a lock of a box starting at (left, top, front) points, as D3D9
   * works it out: by blocks, or by bytes for the formats above. Signed, since
   * D3D9 lets a 2D lock through with a rectangle outside the surface. */
  intptr_t Offset(LONG left, LONG top, UINT front) const {
    intptr_t offset = static_cast<intptr_t>(front) * slice_pitch;
    if (broken_pitch)
      return offset + static_cast<intptr_t>(top) * row_pitch + left;
    return offset + static_cast<intptr_t>(top / static_cast<LONG>(block_height)) * row_pitch +
           static_cast<intptr_t>(left / static_cast<LONG>(block_width)) * block_bytes;
  }
};

inline bool
GetD3D9LockLayout(D3DFORMAT Format, UINT Width, UINT Height, UINT Depth, D3D9LockLayout *pLayout) {
  D3D9LockLayout layout;
  if (!GetD3D9FormatBlockLayout(Format, &layout.block_width, &layout.block_height, &layout.block_bytes))
    return false;
  layout.width = Width;
  layout.height = Height;
  layout.depth = Depth;
  layout.broken_pitch = (DWORD)Format == MAKEFOURCC('A', 'T', 'I', '1') || (DWORD)Format == MAKEFOURCC('A', 'T', 'I', '2');
  if (layout.broken_pitch) {
    layout.stored_row_pitch = layout.BlockColumns() * layout.block_bytes;
    layout.stored_slice_pitch = layout.stored_row_pitch * layout.BlockRows();
    layout.row_pitch = Width;
    layout.slice_pitch = Width * Height;
    /* A level small enough runs on into the next one's memory, as it does on
     * Windows; only the block layout's whole size has to be there. */
    layout.level_bytes = static_cast<size_t>(layout.slice_pitch) * Depth;
    layout.size = std::max<size_t>(layout.level_bytes, static_cast<size_t>(layout.stored_slice_pitch) * Depth);
  } else {
    layout.row_pitch = layout.stored_row_pitch = (layout.BlockColumns() * layout.block_bytes + 3) & ~3u;
    layout.slice_pitch = layout.stored_slice_pitch = layout.row_pitch * layout.BlockRows();
    layout.size = layout.level_bytes = static_cast<size_t>(layout.slice_pitch) * Depth;
  }
  *pLayout = layout;
  return true;
}

/* Where each subresource of a texture lives in the one block of memory
 * Windows gives a texture with a system-memory copy: level after level, and
 * for a cube face after face, each face starting 16-byte aligned. Some
 * applications walk from one level's memory into the next. Offsets are
 * indexed face * Levels + level; returns the block's size, 0 when the format
 * has no known layout. */
inline size_t
GetD3D9TextureMemoryLayout(D3DFORMAT Format, UINT Width, UINT Height, UINT Depth, UINT Levels, UINT Faces,
                           std::vector<size_t> *pOffsets) {
  size_t offset = 0, end = 0;
  pOffsets->assign(static_cast<size_t>(Faces) * Levels, 0);
  for (UINT face = 0; face < Faces; face++) {
    for (UINT level = 0; level < Levels; level++) {
      D3D9LockLayout layout;
      UINT w = Width >> level, h = Height >> level, d = Depth >> level;
      if (!GetD3D9LockLayout(Format, w ? w : 1, h ? h : 1, d ? d : 1, &layout))
        return 0;
      (*pOffsets)[face * Levels + level] = offset;
      end = std::max(end, offset + layout.Size());
      offset += layout.level_bytes;
    }
    offset = (offset + 15) & ~static_cast<size_t>(15);
  }
  return std::max(offset, end);
}

/* ATI Fetch4 is asked for through D3DSAMP_MIPMAPLODBIAS with these values
 * instead of a bias, and works on single-channel formats only. Returns the
 * channel gathered -- 0 red, 3 alpha -- or -1 when the sampler does not
 * fetch4 that format. ATI1 accepts the state but samples as it always does,
 * one texel in every channel. */
constexpr DWORD kD3D9Fetch4On = MAKEFOURCC('G', 'E', 'T', '4');
constexpr DWORD kD3D9Fetch4Off = MAKEFOURCC('G', 'E', 'T', '1');
inline int
D3D9Fetch4Channel(D3DFORMAT Format, DWORD LodBias) {
  if (LodBias != kD3D9Fetch4On)
    return -1;
  switch ((DWORD)Format) {
  case D3DFMT_L8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_R32F:
  case MAKEFOURCC('D', 'F', '1', '6'): case MAKEFOURCC('D', 'F', '2', '4'):
  case MAKEFOURCC('I', 'N', 'T', 'Z'):
    return 0;
  case D3DFMT_A8:
    return 3;
  default:
    return -1;
  }
}

inline bool
D3D9FormatNeedsBgra8Expansion(D3DFORMAT Format) {
  return Format == D3DFMT_A4R4G4B4 || Format == D3DFMT_X4R4G4B4;
}

} // namespace dxmt
