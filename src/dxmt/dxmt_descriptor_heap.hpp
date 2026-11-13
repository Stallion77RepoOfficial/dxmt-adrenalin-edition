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

#include "Metal.hpp"
#include <algorithm>
#include <cstdint>

namespace dxmt {

/* A D3D12 descriptor heap is a flat array of descriptors that shaders index
 * into. Metal Shader Converter compiles DXIL against exactly that model: the
 * resource heap is bound at buffer index kIRDescriptorHeapBindPoint and the
 * sampler heap at kIRSamplerHeapBindPoint, and a descriptor table root
 * parameter is a pointer into one of them.
 *
 * So a heap is just an MTLBuffer holding an array of entries in the converter's
 * layout, CreateShaderResourceView is a write into that array, and
 * CopyDescriptors is a memcpy. The bind points below are the converter's ABI
 * (metal_irconverter_runtime.h) and cannot be renumbered. */
enum DescriptorHeapBindPoint : uint32_t {
  kResourceHeapBindPoint = 0,
  kSamplerHeapBindPoint = 1,
  kTopLevelArgumentBufferBindPoint = 2,
  kHullDomainArgumentBufferBindPoint = 3,
  kDrawArgumentsBindPoint = 4,
  kUniformsBindPoint = 5,
  kVertexBufferBindPoint = 6,
  kStageInAttributeStartIndex = 11,
};

/* Mirrors IRDescriptorTableEntry. One entry describes a buffer, a texture or a
 * sampler; which fields matter depends on the descriptor kind. */
struct DescriptorEntry {
  uint64_t gpu_address;
  uint64_t texture_view_id;
  uint64_t metadata;
};

static_assert(sizeof(DescriptorEntry) == 24, "must match IRDescriptorTableEntry");

/* Metadata packing, also from the converter's ABI. Note the layout differs per
 * descriptor kind: for buffers the metadata is a size/offset/typed triple, for
 * textures and samplers it is a float bit pattern. */
constexpr uint64_t kDescriptorBufferSizeOffset = 0;
constexpr uint64_t kDescriptorBufferSizeMask = 0xffffffffull;
constexpr uint64_t kDescriptorTextureViewOffset = 32;
constexpr uint64_t kDescriptorTextureViewMask = 0xffull;
constexpr uint64_t kDescriptorTypedBufferOffset = 63;

inline uint64_t
encodeBufferMetadata(uint32_t size_in_bytes, uint32_t texture_view_offset_in_elements, bool typed) {
  return ((uint64_t(size_in_bytes) & kDescriptorBufferSizeMask) << kDescriptorBufferSizeOffset) |
         ((uint64_t(texture_view_offset_in_elements) & kDescriptorTextureViewMask) << kDescriptorTextureViewOffset) |
         (uint64_t(typed) << kDescriptorTypedBufferOffset);
}

/* The converter keeps minLODClamp and sampler LOD bias as raw float bits in the
 * low half of the metadata word. */
inline uint64_t
encodeFloatMetadata(float value) {
  uint32_t bits;
  __builtin_memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/* Free encoders so the descriptor layout can be checked without a Metal device;
 * the DescriptorHeap writers are thin wrappers over these. */
inline DescriptorEntry
encodeBufferDescriptor(uint64_t gpu_address, uint32_t size_in_bytes) {
  return DescriptorEntry{gpu_address, 0, encodeBufferMetadata(size_in_bytes, 0, false)};
}

inline DescriptorEntry
encodeTypedBufferDescriptor(
    uint64_t gpu_address, uint32_t size_in_bytes, uint64_t texture_view_id, uint32_t texture_view_offset_in_elements
) {
  return DescriptorEntry{
      gpu_address, texture_view_id, encodeBufferMetadata(size_in_bytes, texture_view_offset_in_elements, true)
  };
}

inline DescriptorEntry
encodeTextureDescriptor(uint64_t texture_view_id, float min_lod_clamp) {
  return DescriptorEntry{0, texture_view_id, encodeFloatMetadata(min_lod_clamp)};
}

inline DescriptorEntry
encodeSamplerDescriptor(uint64_t sampler_resource_id, float lod_bias) {
  /* Samplers put their resource id in the address slot, not the view slot. */
  return DescriptorEntry{sampler_resource_id, 0, encodeFloatMetadata(lod_bias)};
}

/* One contiguous run produced by planDescriptorCopy. */
struct DescriptorCopySegment {
  uint32_t dst_range;
  uint32_t dst_offset;
  uint32_t src_range;
  uint32_t src_offset;
  uint32_t count;
};

/* D3D12's CopyDescriptors takes two independent lists of ranges: descriptors
 * are read from the source ranges in order and written to the destination
 * ranges in order, and the two lists need not line up or even be the same
 * length. This resolves the pairing into contiguous runs.
 *
 * A null sizes array means every range in that list holds one descriptor, which
 * is what the API specifies. Copying stops when the source runs out, so a
 * caller asking for more than it supplied gets a short copy rather than reads
 * past the end.
 *
 * Returns the number of segments the copy needs. Segments beyond out_capacity
 * are counted but not written, so passing a null out is a way to size the
 * buffer first. */
inline uint32_t
planDescriptorCopy(
    uint32_t dst_range_count, const uint32_t *dst_range_sizes, uint32_t src_range_count,
    const uint32_t *src_range_sizes, DescriptorCopySegment *out, uint32_t out_capacity
) {
  uint32_t produced = 0;
  uint32_t src_range = 0;
  uint32_t src_offset = 0;

  for (uint32_t dst_range = 0; dst_range < dst_range_count; dst_range++) {
    uint32_t dst_size = dst_range_sizes ? dst_range_sizes[dst_range] : 1;

    for (uint32_t dst_offset = 0; dst_offset < dst_size;) {
      if (src_range >= src_range_count)
        return produced;

      uint32_t src_size = src_range_sizes ? src_range_sizes[src_range] : 1;
      if (src_offset >= src_size) {
        src_range++;
        src_offset = 0;
        continue;
      }

      uint32_t count = std::min(dst_size - dst_offset, src_size - src_offset);
      if (out && produced < out_capacity)
        out[produced] = DescriptorCopySegment{dst_range, dst_offset, src_range, src_offset, count};
      produced++;

      dst_offset += count;
      src_offset += count;
    }
  }

  return produced;
}


class DescriptorHeap {
public:
  DescriptorHeap(WMT::Device device, uint32_t descriptor_count, bool sampler_heap);
  ~DescriptorHeap();

  DescriptorHeap(const DescriptorHeap &) = delete;
  DescriptorHeap &operator=(const DescriptorHeap &) = delete;

  WMT::Buffer
  buffer() const {
    return buffer_;
  }

  uint64_t
  gpuAddress() const {
    return gpu_address_;
  }

  uint32_t
  descriptorCount() const {
    return descriptor_count_;
  }

  bool
  isSamplerHeap() const {
    return sampler_heap_;
  }

  /* GPU address of a descriptor, which is what a descriptor table root
   * parameter stores in the top-level argument buffer. */
  uint64_t
  descriptorAddress(uint32_t index) const {
    return gpu_address_ + uint64_t(index) * sizeof(DescriptorEntry);
  }

  /* Writes. Out-of-range indices are ignored rather than trapping: D3D12 lets
   * an application build descriptors it never binds, and a bad index must not
   * take the process down. */
  void writeBuffer(uint32_t index, uint64_t gpu_address, uint32_t size_in_bytes);
  /* A buffer the shader may also read as a texture (typed//formatted buffer
   * SRV or UAV). texture_view_id is the gpuResourceID of the texture view over
   * the buffer. */
  void writeTypedBuffer(
      uint32_t index, uint64_t gpu_address, uint32_t size_in_bytes, uint64_t texture_view_id,
      uint32_t texture_view_offset_in_elements
  );
  void writeTexture(uint32_t index, uint64_t texture_view_id, float min_lod_clamp);
  void writeSampler(uint32_t index, uint64_t sampler_resource_id, float lod_bias);
  void writeNull(uint32_t index);

  /* For callers that already hold an encoded entry, and for moving one between
   * heaps a descriptor at a time. */
  void writeRaw(uint32_t index, const DescriptorEntry &entry);
  DescriptorEntry readRaw(uint32_t index) const;

  /* CopyDescriptors. Both heaps must be of the same kind. */
  void copyFrom(uint32_t dst_index, const DescriptorHeap &src, uint32_t src_index, uint32_t count);

private:
  DescriptorEntry *
  entry(uint32_t index) const {
    return entries_ && index < descriptor_count_ ? entries_ + index : nullptr;
  }

  WMT::Reference<WMT::Buffer> buffer_;
  DescriptorEntry *entries_;
  uint64_t gpu_address_;
  uint32_t descriptor_count_;
  bool sampler_heap_;
  void *cpu_memory_ = nullptr;
};

} // namespace dxmt
