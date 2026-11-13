/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

/* Translation of D3D12 descriptions into the Metal ones, kept apart from the
 * COM objects that use them: these are plain functions of their arguments and
 * are the parts most likely to grow as more of D3D12 is covered.
 *
 * The fixed-function state tables (blend, depth, stencil) live in
 * src/dxmt/dxmt_pipeline_state_map.hpp instead, because d3d11 shares them. */

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>

#include "Metal.hpp"
#include "dxmt_format.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

/* D3D12 heap type to Metal resource options.
 *
 * Hazard tracking is off across the board: a D3D12 application is required to
 * issue its own barriers, so letting Metal insert conservative synchronisation
 * as well only costs performance. Placed resources also alias each other by
 * design, which tracking cannot model anyway — an aliasing barrier is what
 * orders them, see dxmt_barrier.hpp. */
WMTResourceOptions
heap_resource_options(const D3D12_HEAP_PROPERTIES &properties) {
  constexpr uint64_t untracked = WMTResourceHazardTrackingModeUntracked;

  D3D12_CPU_PAGE_PROPERTY page = properties.CPUPageProperty;
  if (properties.Type == D3D12_HEAP_TYPE_UPLOAD)
    page = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
  else if (properties.Type == D3D12_HEAP_TYPE_READBACK)
    page = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  else if (properties.Type == D3D12_HEAP_TYPE_DEFAULT)
    page = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;

  switch (page) {
  case D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE:
    return WMTResourceOptions(WMTResourceStorageModeShared | WMTResourceCPUCacheModeWriteCombined | untracked);
  case D3D12_CPU_PAGE_PROPERTY_WRITE_BACK:
    return WMTResourceOptions(WMTResourceStorageModeShared | untracked);
  default:
    /* Unified memory means private is still CPU-addressable through the heap's
     * own resources where D3D12 permits it, and it is the fastest for the GPU. */
    return WMTResourceOptions(WMTResourceStorageModePrivate | untracked);
  }
}

WMTTextureUsage
texture_usage_from_flags(D3D12_RESOURCE_FLAGS flags) {
  uint32_t usage = WMTTextureUsageShaderRead | WMTTextureUsagePixelFormatView;

  if (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    usage |= WMTTextureUsageRenderTarget;
  if (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    usage |= WMTTextureUsageRenderTarget;
  if (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    usage |= WMTTextureUsageShaderWrite;
  if (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
    usage &= ~uint32_t(WMTTextureUsageShaderRead);

  return WMTTextureUsage(usage);
}

WMTTextureType
texture_type_from_desc(const D3D12_RESOURCE_DESC &desc) {
  bool array = desc.DepthOrArraySize > 1;
  bool multisample = desc.SampleDesc.Count > 1;

  switch (desc.Dimension) {
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    return array ? WMTTextureType1DArray : WMTTextureType1D;
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
    return WMTTextureType3D;
  default:
    if (multisample)
      return array ? WMTTextureType2DMultisampleArray : WMTTextureType2DMultisample;
    return array ? WMTTextureType2DArray : WMTTextureType2D;
  }
}

/* Returns false when the format has no Metal equivalent, in which case the
 * caller must fail rather than silently create something else. */
bool
texture_info_from_desc(
    WMT::Device metal_device, const D3D12_RESOURCE_DESC &desc, WMTResourceOptions options, WMTTextureInfo &info
) {
  MTL_DXGI_FORMAT_DESC format_desc;
  if (MTLQueryDXGIFormat(metal_device, desc.Format, format_desc) ||
      format_desc.PixelFormat == WMTPixelFormatInvalid)
    return false;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
    if (desc.Format == DXGI_FORMAT_R32_TYPELESS)
      format_desc.PixelFormat = WMTPixelFormatDepth32Float;
    else if (desc.Format == DXGI_FORMAT_R16_TYPELESS)
      format_desc.PixelFormat = WMTPixelFormatDepth16Unorm;
  }

  info = {};
  info.pixel_format = format_desc.PixelFormat;
  info.width = uint32_t(desc.Width);
  info.height = std::max(desc.Height, 1u);
  info.depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? std::max<uint32_t>(desc.DepthOrArraySize, 1) : 1;
  info.array_length =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : std::max<uint32_t>(desc.DepthOrArraySize, 1);
  info.type = texture_type_from_desc(desc);
  info.mipmap_level_count = std::max<uint32_t>(desc.MipLevels, 1);
  info.sample_count = std::max<uint32_t>(desc.SampleDesc.Count, 1);
  info.usage = texture_usage_from_flags(desc.Flags);
  // Shader writes do not imply atomics. Match the format restrictions used
  // by the D3D11 path; Metal rejects RGBA8 textures carrying ShaderAtomic.
  if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
      (info.pixel_format == WMTPixelFormatR32Uint || info.pixel_format == WMTPixelFormatR32Sint ||
       (info.pixel_format == WMTPixelFormatRG32Uint && metal_device.supportsFamily(WMTGPUFamilyApple8))))
    info.usage = WMTTextureUsage(info.usage | WMTTextureUsageShaderAtomic);
  info.options = options;
  return true;
}



} // namespace dxmt::d3d12
