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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxmt {

/* A D3D12 root signature becomes the layout of the "top-level argument buffer"
 * that Metal Shader Converter compiles shaders against, bound at
 * kTopLevelArgumentBufferBindPoint.
 *
 * The layout is not a fixed stride. Measured against the converter (4.0.1):
 * every root parameter starts at an 8-byte aligned offset, root constants are
 * stored inline and occupy num32BitValues * 4 bytes, and every other parameter
 * kind occupies 8 bytes holding a GPU address. Root CBV/SRV/UAV store the
 * resource address directly; a descriptor table stores the address of its first
 * descriptor inside a descriptor heap. */
enum class RootParameterKind : uint32_t {
  DescriptorTable = 0,
  Constants = 1,
  ConstantBufferView = 2,
  ShaderResourceView = 3,
  UnorderedAccessView = 4,
};

struct RootParameterLayout {
  RootParameterKind kind;
  /* Byte offset into the top-level argument buffer. */
  uint32_t offset;
  /* Bytes occupied: 8, or num_dwords * 4 for Constants. */
  uint32_t size;
  /* Constants only, otherwise 0. */
  uint32_t num_dwords;
};

class RootSignatureLayout {
public:
  void
  reset() {
    parameters_.clear();
    size_ = 0;
  }

  uint32_t addDescriptorTable();
  uint32_t addConstants(uint32_t num_32bit_values);
  /* kind must be one of the root descriptor kinds (CBV/SRV/UAV). */
  uint32_t addRootDescriptor(RootParameterKind kind);

  uint32_t
  parameterCount() const {
    return uint32_t(parameters_.size());
  }

  const RootParameterLayout *
  parameter(uint32_t index) const {
    return index < parameters_.size() ? &parameters_[index] : nullptr;
  }

  /* Size of the top-level argument buffer this layout describes. */
  uint32_t
  argumentBufferSize() const {
    return size_;
  }

private:
  uint32_t append(RootParameterKind kind, uint32_t size, uint32_t num_dwords);

  std::vector<RootParameterLayout> parameters_;
  uint32_t size_ = 0;
};

/* CPU-side shadow of the top-level argument buffer.
 *
 * D3D12 root arguments are set incrementally on a command list and are only
 * meaningful at draw time, so they are accumulated here and the whole block is
 * copied into a GPU-visible allocation when a draw is recorded. Setters ignore
 * out-of-range parameters and kind mismatches instead of trapping: a command
 * list may legally set arguments that the currently bound root signature does
 * not use, and the D3D12 runtime treats that as a no-op. */
class RootArgumentBuffer {
public:
  /* Points at a layout that must outlive this object. Passing nullptr leaves
   * the buffer empty and turns every setter into a no-op. */
  void setLayout(const RootSignatureLayout *layout);

  const RootSignatureLayout *
  layout() const {
    return layout_;
  }

  const void *
  data() const {
    return storage_.data();
  }

  uint32_t
  size() const {
    return uint32_t(storage_.size());
  }

  void setDescriptorTable(uint32_t index, uint64_t descriptor_gpu_address);
  void setRootDescriptor(uint32_t index, uint64_t gpu_address);
  void setConstants(uint32_t index, uint32_t dest_offset_in_dwords, const void *src, uint32_t num_dwords);

private:
  const RootSignatureLayout *layout_ = nullptr;
  std::vector<uint8_t> storage_;
};

/* Everything a serialized root signature says, beyond the argument buffer
 * layout: which registers each parameter binds and what its descriptor tables
 * contain. The pipeline needs this to match a table against the shader's
 * expectations, and the command list needs it to validate root argument writes.
 *
 * The enumerator values are D3D12's, so a caller can compare against
 * D3D12_DESCRIPTOR_RANGE_TYPE and friends without a translation table. */
enum class DescriptorRangeType : uint32_t {
  ShaderResourceView = 0,
  UnorderedAccessView = 1,
  ConstantBufferView = 2,
  Sampler = 3,
};

/* D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND: place this range straight after the
 * previous one. */
constexpr uint32_t kDescriptorRangeOffsetAppend = 0xffffffffu;

struct RootDescriptorRange {
  DescriptorRangeType range_type;
  /* 0xffffffff for an unbounded range. */
  uint32_t descriptor_count;
  uint32_t base_shader_register;
  uint32_t register_space;
  uint32_t flags;
  uint32_t offset_in_descriptors_from_table_start;
};

struct RootParameterInfo {
  RootParameterKind kind;
  uint32_t shader_visibility;
  /* Constants and root descriptors. */
  uint32_t shader_register;
  uint32_t register_space;
  uint32_t descriptor_flags;
  /* Constants only. */
  uint32_t num_32bit_values;
  /* Descriptor tables: a window into RootSignatureDescription::ranges. */
  uint32_t first_range;
  uint32_t range_count;
};

struct StaticSamplerInfo {
  uint32_t filter;
  uint32_t address_u;
  uint32_t address_v;
  uint32_t address_w;
  float mip_lod_bias;
  uint32_t max_anisotropy;
  uint32_t comparison_func;
  uint32_t border_color;
  float min_lod;
  float max_lod;
  uint32_t shader_register;
  uint32_t register_space;
  uint32_t shader_visibility;
};

struct RootSignatureDescription {
  /* 1 for root signature 1.0, 2 for 1.1. */
  uint32_t version = 0;
  uint32_t flags = 0;
  RootSignatureLayout layout;
  std::vector<RootParameterInfo> parameters;
  std::vector<RootDescriptorRange> ranges;
  std::vector<StaticSamplerInfo> static_samplers;
};

/* Parses the blob an application passes to ID3D12Device::CreateRootSignature.
 *
 * That is a DXBC container holding an RTS0 chunk, whether it came from
 * D3D12SerializeRootSignature or was embedded in a compiled shader. The blob is
 * application data and may be malformed or hostile, so every offset is checked
 * against the chunk bounds and a bad one fails the parse rather than reading
 * out of bounds.
 *
 * Returns false and leaves `out` unspecified if the blob is not a root
 * signature this understands. */
/* The inverse of parseSerializedRootSignature: builds the DXBC container with
 * its RTS0 chunk that D3D12SerializeRootSignature is supposed to return.
 *
 * Kept next to the parser deliberately. The format is not documented and was
 * worked out by measurement, so the two have to move together -- and being
 * exact inverses is what makes them testable without a reference
 * implementation. */
bool serializeRootSignature(const RootSignatureDescription &description, std::vector<uint8_t> &out);

bool parseSerializedRootSignature(const void *blob, size_t size, RootSignatureDescription &out);

/* Metal Shader Converter takes a root signature as an
 * IRVersionedRootSignatureDescriptor, whose layout is D3D12's
 * D3D12_VERSIONED_ROOT_SIGNATURE_DESC. The structures below mirror it so a
 * descriptor can be rebuilt without pulling D3D headers into the core; the gate
 * asserts they match the converter's own definitions field for field.
 *
 * A shader that carries its root signature inline needs none of this. One
 * compiled against a separate root signature object does: without it the
 * converter has nothing to lay the top-level argument buffer out from. */
/* Metal Shader Converter runs in the 64-bit Unix host even for WoW64.
 * Pointer-bearing mirror fields therefore always use the host ABI. */
template <typename T> struct alignas(8) IRHostPointer {
  uint64_t address;
  IRHostPointer &operator=(const T *pointer) {
    address = uint64_t(reinterpret_cast<uintptr_t>(pointer));
    return *this;
  }
  operator const T *() const { return reinterpret_cast<const T *>(uintptr_t(address)); }
  const T &operator[](size_t index) const { return static_cast<const T *>(*this)[index]; }
};

struct IRDescriptorRange1 {
  uint32_t range_type;
  uint32_t num_descriptors;
  uint32_t base_shader_register;
  uint32_t register_space;
  uint32_t flags;
  uint32_t offset_in_descriptors_from_table_start;
};

struct IRRootDescriptorTable1 {
  uint32_t num_ranges;
  IRHostPointer<IRDescriptorRange1> ranges;
};

struct IRRootConstants {
  uint32_t shader_register;
  uint32_t register_space;
  uint32_t num_32bit_values;
};

struct IRRootDescriptor1 {
  uint32_t shader_register;
  uint32_t register_space;
  uint32_t flags;
};

struct IRRootParameter1 {
  uint32_t parameter_type;
  union {
    IRRootDescriptorTable1 descriptor_table;
    IRRootConstants constants;
    IRRootDescriptor1 descriptor;
  };
  uint32_t shader_visibility;
};

struct IRStaticSamplerDescriptor {
  uint32_t filter;
  uint32_t address_u;
  uint32_t address_v;
  uint32_t address_w;
  float mip_lod_bias;
  uint32_t max_anisotropy;
  uint32_t comparison_func;
  uint32_t border_color;
  float min_lod;
  float max_lod;
  uint32_t shader_register;
  uint32_t register_space;
  uint32_t shader_visibility;
};

struct IRRootSignatureDescriptor1 {
  uint32_t num_parameters;
  IRHostPointer<IRRootParameter1> parameters;
  uint32_t num_static_samplers;
  IRHostPointer<IRStaticSamplerDescriptor> static_samplers;
  uint32_t flags;
};

struct IRVersionedRootSignatureDescriptorMirror {
  uint32_t version;
  union {
    IRRootSignatureDescriptor1 desc_1_0;
    IRRootSignatureDescriptor1 desc_1_1;
  };
};

static_assert(sizeof(IRRootDescriptorTable1) == 16);
static_assert(offsetof(IRRootParameter1, shader_visibility) == 24);
static_assert(sizeof(IRRootParameter1) == 32);
static_assert(sizeof(IRRootSignatureDescriptor1) == 40);
static_assert(sizeof(IRVersionedRootSignatureDescriptorMirror) == 48);

/* Owns the arrays the descriptor points into, so it has to outlive any use of
 * descriptor(). Rebuilding is cheap but not free, so callers keep one per root
 * signature object rather than building it per pipeline. */
class IRRootSignatureBuilder {
public:
  void build(const RootSignatureDescription &description);

  const IRVersionedRootSignatureDescriptorMirror *
  descriptor() const {
    return &versioned_;
  }

private:
  std::vector<IRDescriptorRange1> ranges_;
  std::vector<IRRootParameter1> parameters_;
  std::vector<IRStaticSamplerDescriptor> static_samplers_;
  IRVersionedRootSignatureDescriptorMirror versioned_ = {};
};

} // namespace dxmt
