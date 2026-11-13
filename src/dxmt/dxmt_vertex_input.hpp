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

#include "winemetal.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dxmt {

/* Matching a D3D input layout to a converted vertex function.
 *
 * Metal Shader Converter turns each vertex input into a stage_in attribute
 * named lowercase(SemanticName) followed by SemanticIndex — POSITION becomes
 * "position0", TEXCOORD3 becomes "texcoord3", MY_SEMANTIC2 becomes
 * "my_semantic2". The attribute *numbers* it assigns follow the order the
 * shader declares its inputs, counting system values like SV_InstanceID, so
 * they cannot be worked out from the input layout: the numbering has to be read
 * back from the function and the names are what pairs the two up.
 *
 * Verified against the converter in tests/d3d12. */
inline bool
vertexAttributeName(const char *semantic_name, uint32_t semantic_index, char *out, size_t out_size) {
  if (!semantic_name || !out || !out_size)
    return false;

  size_t length = std::strlen(semantic_name);
  /* Leave room for the index digits and the terminator. */
  if (length + 12 > out_size)
    return false;

  for (size_t i = 0; i < length; i++) {
    char c = semantic_name[i];
    out[i] = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
  }

  int written = std::snprintf(out + length, out_size - length, "%u", semantic_index);
  return written > 0 && size_t(written) < out_size - length;
}

/* DXGI_FORMAT to MTLVertexFormat. Only the formats a vertex attribute can
 * legally use are listed; anything else returns Invalid and the caller must
 * reject the input layout rather than bind something else.
 *
 * The DXGI values are spelled out because this header stays free of D3D
 * headers, and they are a stable part of the API. */
inline WMTVertexFormat
vertexFormatFromDXGI(uint32_t dxgi_format) {
  switch (dxgi_format) {
  case 2: /* R32G32B32A32_FLOAT */
    return WMTVertexFormatFloat4;
  case 3: /* R32G32B32A32_UINT */
    return WMTVertexFormatUInt4;
  case 4: /* R32G32B32A32_SINT */
    return WMTVertexFormatInt4;
  case 6: /* R32G32B32_FLOAT */
    return WMTVertexFormatFloat3;
  case 7: /* R32G32B32_UINT */
    return WMTVertexFormatUInt3;
  case 8: /* R32G32B32_SINT */
    return WMTVertexFormatInt3;
  case 10: /* R16G16B16A16_FLOAT */
    return WMTVertexFormatHalf4;
  case 11: /* R16G16B16A16_UNORM */
    return WMTVertexFormatUShort4Normalized;
  case 12: /* R16G16B16A16_UINT */
    return WMTVertexFormatUShort4;
  case 13: /* R16G16B16A16_SNORM */
    return WMTVertexFormatShort4Normalized;
  case 14: /* R16G16B16A16_SINT */
    return WMTVertexFormatShort4;
  case 16: /* R32G32_FLOAT */
    return WMTVertexFormatFloat2;
  case 17: /* R32G32_UINT */
    return WMTVertexFormatUInt2;
  case 18: /* R32G32_SINT */
    return WMTVertexFormatInt2;
  case 24: /* R10G10B10A2_UNORM */
    return WMTVertexFormatUInt1010102Normalized;
  case 28: /* R8G8B8A8_UNORM */
    return WMTVertexFormatUChar4Normalized;
  case 30: /* R8G8B8A8_UINT */
    return WMTVertexFormatUChar4;
  case 31: /* R8G8B8A8_SNORM */
    return WMTVertexFormatChar4Normalized;
  case 32: /* R8G8B8A8_SINT */
    return WMTVertexFormatChar4;
  case 34: /* R16G16_FLOAT */
    return WMTVertexFormatHalf2;
  case 35: /* R16G16_UNORM */
    return WMTVertexFormatUShort2Normalized;
  case 36: /* R16G16_UINT */
    return WMTVertexFormatUShort2;
  case 37: /* R16G16_SNORM */
    return WMTVertexFormatShort2Normalized;
  case 38: /* R16G16_SINT */
    return WMTVertexFormatShort2;
  case 41: /* R32_FLOAT */
    return WMTVertexFormatFloat;
  case 42: /* R32_UINT */
    return WMTVertexFormatUInt;
  case 43: /* R32_SINT */
    return WMTVertexFormatInt;
  case 49: /* R8G8_UNORM */
    return WMTVertexFormatUChar2Normalized;
  case 50: /* R8G8_UINT */
    return WMTVertexFormatUChar2;
  case 51: /* R8G8_SNORM */
    return WMTVertexFormatChar2Normalized;
  case 52: /* R8G8_SINT */
    return WMTVertexFormatChar2;
  case 54: /* R16_FLOAT */
    return WMTVertexFormatHalf;
  case 56: /* R16_UNORM */
    return WMTVertexFormatUShortNormalized;
  case 57: /* R16_UINT */
    return WMTVertexFormatUShort;
  case 58: /* R16_SNORM */
    return WMTVertexFormatShortNormalized;
  case 59: /* R16_SINT */
    return WMTVertexFormatShort;
  case 61: /* R8_UNORM */
    return WMTVertexFormatUCharNormalized;
  case 62: /* R8_UINT */
    return WMTVertexFormatUChar;
  case 63: /* R8_SNORM */
    return WMTVertexFormatCharNormalized;
  case 64: /* R8_SINT */
    return WMTVertexFormatChar;
  case 87: /* B8G8R8A8_UNORM */
    return WMTVertexFormatUChar4Normalized_BGRA;
  default:
    return WMTVertexFormatInvalid;
  }
}

/* Byte size of a vertex format, needed to advance the packing cursor for
 * D3D12_APPEND_ALIGNED_ELEMENT. */
inline uint32_t
vertexFormatSizeInBytes(WMTVertexFormat format) {
  switch (format) {
  case WMTVertexFormatUChar:
  case WMTVertexFormatChar:
  case WMTVertexFormatUCharNormalized:
  case WMTVertexFormatCharNormalized:
    return 1;
  case WMTVertexFormatUChar2:
  case WMTVertexFormatChar2:
  case WMTVertexFormatUChar2Normalized:
  case WMTVertexFormatChar2Normalized:
  case WMTVertexFormatUShort:
  case WMTVertexFormatShort:
  case WMTVertexFormatUShortNormalized:
  case WMTVertexFormatShortNormalized:
  case WMTVertexFormatHalf:
    return 2;
  case WMTVertexFormatUChar4:
  case WMTVertexFormatChar4:
  case WMTVertexFormatUChar4Normalized:
  case WMTVertexFormatChar4Normalized:
  case WMTVertexFormatUChar4Normalized_BGRA:
  case WMTVertexFormatUShort2:
  case WMTVertexFormatShort2:
  case WMTVertexFormatUShort2Normalized:
  case WMTVertexFormatShort2Normalized:
  case WMTVertexFormatHalf2:
  case WMTVertexFormatFloat:
  case WMTVertexFormatInt:
  case WMTVertexFormatUInt:
  case WMTVertexFormatInt1010102Normalized:
  case WMTVertexFormatUInt1010102Normalized:
    return 4;
  case WMTVertexFormatUShort4:
  case WMTVertexFormatShort4:
  case WMTVertexFormatUShort4Normalized:
  case WMTVertexFormatShort4Normalized:
  case WMTVertexFormatHalf4:
  case WMTVertexFormatFloat2:
  case WMTVertexFormatInt2:
  case WMTVertexFormatUInt2:
    return 8;
  case WMTVertexFormatFloat3:
  case WMTVertexFormatInt3:
  case WMTVertexFormatUInt3:
    return 12;
  case WMTVertexFormatFloat4:
  case WMTVertexFormatInt4:
  case WMTVertexFormatUInt4:
    return 16;
  default:
    return 0;
  }
}

/* One entry of a D3D input layout, in terms this header can work with. */
struct VertexInputElement {
  const char *semantic_name;
  uint32_t semantic_index;
  uint32_t dxgi_format;
  uint32_t input_slot;
  /* D3D12_APPEND_ALIGNED_ELEMENT (0xffffffff) packs after the previous element
   * in the same slot. */
  uint32_t aligned_byte_offset;
  bool per_instance;
  uint32_t instance_step_rate;
};

constexpr uint32_t kAppendAlignedElement = 0xffffffffu;

/* Key for the per-stride pipeline cache.
 *
 * Metal bakes vertex buffer strides into the pipeline while D3D supplies them
 * per draw, so a pipeline is only valid for the strides it was built with and
 * the cache has to be keyed on them. Only slots the layout reads take part: a
 * stride change on a slot nothing reads cannot change the pipeline, and
 * including it would recompile for no reason.
 *
 * Getting this wrong the other way is worse — too coarse a key hands back a
 * pipeline built for different strides, and every vertex reads from the wrong
 * offset. */
inline uint64_t
vertexStrideCacheKey(
    const VertexInputElement *elements, uint32_t element_count, const uint32_t *strides, uint32_t stride_count
) {
  uint64_t key = 1469598103934665603ull; /* FNV-1a offset basis */

  for (uint32_t i = 0; i < element_count; i++) {
    uint32_t slot = elements[i].input_slot;
    uint32_t stride = slot < stride_count && strides ? strides[slot] : 0;
    key = (key ^ slot) * 1099511628211ull;
    key = (key ^ stride) * 1099511628211ull;
  }

  return key;
}

/* Assembled vertex descriptor. Owns its arrays, so it must outlive the
 * WMTVertexDescriptorInfo taken from it. */
struct VertexDescriptorBuild {
  std::vector<WMTVertexAttributeDesc> attributes;
  std::vector<WMTVertexBufferLayoutDesc> layouts;

  WMTVertexDescriptorInfo
  info() const {
    WMTVertexDescriptorInfo out = {};
    out.attributes.set(attributes.empty() ? nullptr : attributes.data());
    out.layouts.set(layouts.empty() ? nullptr : layouts.data());
    out.num_attributes = uint32_t(attributes.size());
    out.num_layouts = uint32_t(layouts.size());
    return out;
  }
};

/* Builds the descriptor for one pipeline.
 *
 * `strides` gives the byte stride of each input slot. D3D supplies those on the
 * vertex buffer view at draw time while Metal needs them here, which is why a
 * pipeline is only valid for the strides it was built with and they belong in
 * the pipeline cache key.
 *
 * Elements whose semantic the shader does not consume are skipped: an
 * application may declare more inputs than a given shader reads, and Metal
 * rejects a descriptor with attributes the function has no slot for.
 *
 * Returns false if an element has a format with no Metal equivalent or names a
 * slot with no stride, both of which would otherwise produce a pipeline that
 * reads the wrong bytes. */
inline bool
buildVertexDescriptor(
    const VertexInputElement *elements, uint32_t element_count, const WMTVertexAttributeInfo *shader_attributes,
    uint32_t shader_attribute_count, const uint32_t *strides, uint32_t stride_count, uint32_t vertex_buffer_bind_point,
    VertexDescriptorBuild &out
) {
  out.attributes.clear();
  out.layouts.clear();

  /* Running offset per slot, for APPEND_ALIGNED_ELEMENT. */
  std::vector<uint32_t> next_offset(stride_count, 0);
  std::vector<bool> slot_used(stride_count, false);

  for (uint32_t i = 0; i < element_count; i++) {
    const VertexInputElement &element = elements[i];
    if (element.input_slot >= stride_count)
      return false;

    WMTVertexFormat format = vertexFormatFromDXGI(element.dxgi_format);
    if (format == WMTVertexFormatInvalid)
      return false;

    uint32_t offset = element.aligned_byte_offset == kAppendAlignedElement ? next_offset[element.input_slot]
                                                                          : element.aligned_byte_offset;
    /* Advance the append cursor whether or not the shader reads this element:
     * the layout's packing does not depend on what is consumed. */
    next_offset[element.input_slot] = offset + vertexFormatSizeInBytes(format);

    char name[64] = {};
    if (!vertexAttributeName(element.semantic_name, element.semantic_index, name, sizeof(name)))
      return false;

    const WMTVertexAttributeInfo *matched = nullptr;
    for (uint32_t a = 0; a < shader_attribute_count; a++) {
      if (std::strcmp(shader_attributes[a].name, name) == 0) {
        matched = &shader_attributes[a];
        break;
      }
    }
    if (!matched)
      continue;

    out.attributes.push_back(
        WMTVertexAttributeDesc{matched->index, format, offset, vertex_buffer_bind_point + element.input_slot}
    );
    slot_used[element.input_slot] = true;
  }

  for (uint32_t slot = 0; slot < stride_count; slot++) {
    if (!slot_used[slot])
      continue;
    /* A zero stride would make every vertex read the same bytes. */
    if (!strides[slot])
      return false;
    out.layouts.push_back(WMTVertexBufferLayoutDesc{
        vertex_buffer_bind_point + slot, strides[slot], WMTVertexStepFunctionPerVertex, 1
    });
  }

  /* Per-instance slots override the step function decided above. */
  for (uint32_t i = 0; i < element_count; i++) {
    if (!elements[i].per_instance)
      continue;
    for (WMTVertexBufferLayoutDesc &layout : out.layouts) {
      if (layout.index != vertex_buffer_bind_point + elements[i].input_slot)
        continue;
      layout.step_function = WMTVertexStepFunctionPerInstance;
      layout.step_rate = elements[i].instance_step_rate ? elements[i].instance_step_rate : 1;
    }
  }

  return true;
}

} // namespace dxmt
