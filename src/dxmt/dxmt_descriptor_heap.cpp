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

#include "dxmt_descriptor_heap.hpp"
#include <algorithm>
#include <cstring>
#include "wsi_platform.hpp"

namespace dxmt {

DescriptorHeap::DescriptorHeap(WMT::Device device, uint32_t descriptor_count, bool sampler_heap) :
    entries_(nullptr),
    gpu_address_(0),
    descriptor_count_(descriptor_count),
    sampler_heap_(sampler_heap) {
  WMTBufferInfo info = {};
  info.length = uint64_t(descriptor_count) * sizeof(DescriptorEntry);
  /* Shader-visible heaps are written by the CPU every frame and read by the
   * GPU, so they cannot be private. */
  info.options = WMTResourceStorageModeShared;
  info.memory.set(nullptr);
#if defined(__i386__)
  info.length = (info.length + 16383) & ~uint64_t(16383);
  cpu_memory_ = wsi::aligned_malloc(info.length, 16384);
  if (!cpu_memory_) return;
  info.memory.set(cpu_memory_);
#endif
  info.gpu_address = 0;

  buffer_ = device.newBuffer(info);
  if (!buffer_)
    return;

  entries_ = reinterpret_cast<DescriptorEntry *>(info.memory.get_accessible_or_null());
  gpu_address_ = info.gpu_address;

  if (entries_)
    std::memset(entries_, 0, info.length);
}

DescriptorHeap::~DescriptorHeap() {
  buffer_ = nullptr;
  if (cpu_memory_) wsi::aligned_free(cpu_memory_);
}

void
DescriptorHeap::writeBuffer(uint32_t index, uint64_t gpu_address, uint32_t size_in_bytes) {
  if (auto e = entry(index))
    *e = encodeBufferDescriptor(gpu_address, size_in_bytes);
}

void
DescriptorHeap::writeTypedBuffer(
    uint32_t index, uint64_t gpu_address, uint32_t size_in_bytes, uint64_t texture_view_id,
    uint32_t texture_view_offset_in_elements
) {
  if (auto e = entry(index))
    *e = encodeTypedBufferDescriptor(gpu_address, size_in_bytes, texture_view_id, texture_view_offset_in_elements);
}

void
DescriptorHeap::writeTexture(uint32_t index, uint64_t texture_view_id, float min_lod_clamp) {
  if (auto e = entry(index))
    *e = encodeTextureDescriptor(texture_view_id, min_lod_clamp);
}

void
DescriptorHeap::writeSampler(uint32_t index, uint64_t sampler_resource_id, float lod_bias) {
  if (auto e = entry(index))
    *e = encodeSamplerDescriptor(sampler_resource_id, lod_bias);
}

void
DescriptorHeap::writeNull(uint32_t index) {
  if (auto e = entry(index))
    *e = DescriptorEntry{};
}

void
DescriptorHeap::writeRaw(uint32_t index, const DescriptorEntry &value) {
  if (auto e = entry(index))
    *e = value;
}

DescriptorEntry
DescriptorHeap::readRaw(uint32_t index) const {
  if (auto e = entry(index))
    return *e;
  return DescriptorEntry{};
}

void
DescriptorHeap::copyFrom(uint32_t dst_index, const DescriptorHeap &src, uint32_t src_index, uint32_t count) {
  if (!entries_ || !src.entries_)
    return;
  /* Clamp instead of trusting the caller: a range that runs off either heap
   * would otherwise be a straight out-of-bounds memcpy. */
  if (dst_index >= descriptor_count_ || src_index >= src.descriptor_count_)
    return;
  count = std::min({count, descriptor_count_ - dst_index, src.descriptor_count_ - src_index});

  std::memmove(entries_ + dst_index, src.entries_ + src_index, size_t(count) * sizeof(DescriptorEntry));
}

} // namespace dxmt
