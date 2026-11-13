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

#include "dxmt_root_signature.hpp"
#include <cstring>

namespace dxmt {

namespace {

constexpr uint32_t kRootParameterAlignment = 8;

uint32_t
alignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

uint32_t
RootSignatureLayout::append(RootParameterKind kind, uint32_t size, uint32_t num_dwords) {
  uint32_t index = uint32_t(parameters_.size());
  uint32_t offset = alignUp(size_, kRootParameterAlignment);

  parameters_.push_back(RootParameterLayout{kind, offset, size, num_dwords});
  size_ = offset + size;
  return index;
}

uint32_t
RootSignatureLayout::addDescriptorTable() {
  return append(RootParameterKind::DescriptorTable, sizeof(uint64_t), 0);
}

uint32_t
RootSignatureLayout::addConstants(uint32_t num_32bit_values) {
  return append(RootParameterKind::Constants, num_32bit_values * sizeof(uint32_t), num_32bit_values);
}

uint32_t
RootSignatureLayout::addRootDescriptor(RootParameterKind kind) {
  return append(kind, sizeof(uint64_t), 0);
}

void
RootArgumentBuffer::setLayout(const RootSignatureLayout *layout) {
  layout_ = layout;
  storage_.assign(layout ? alignUp(layout->argumentBufferSize(), kRootParameterAlignment) : 0, 0);
}

void
RootArgumentBuffer::setDescriptorTable(uint32_t index, uint64_t descriptor_gpu_address) {
  if (!layout_)
    return;
  const RootParameterLayout *parameter = layout_->parameter(index);
  if (!parameter || parameter->kind != RootParameterKind::DescriptorTable)
    return;

  std::memcpy(storage_.data() + parameter->offset, &descriptor_gpu_address, sizeof(descriptor_gpu_address));
}

void
RootArgumentBuffer::setRootDescriptor(uint32_t index, uint64_t gpu_address) {
  if (!layout_)
    return;
  const RootParameterLayout *parameter = layout_->parameter(index);
  if (!parameter)
    return;
  switch (parameter->kind) {
  case RootParameterKind::ConstantBufferView:
  case RootParameterKind::ShaderResourceView:
  case RootParameterKind::UnorderedAccessView:
    break;
  default:
    return;
  }

  std::memcpy(storage_.data() + parameter->offset, &gpu_address, sizeof(gpu_address));
}

namespace {

/* Bounds-checked reader over the RTS0 chunk. Every accessor validates before
 * touching memory, so a truncated or lying blob makes the parse fail instead of
 * reading past the end. */
class BlobReader {
public:
  BlobReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}

  bool
  u32At(size_t offset, uint32_t &value) const {
    if (offset > size_ || size_ - offset < sizeof(uint32_t))
      return false;
    std::memcpy(&value, data_ + offset, sizeof(value));
    return true;
  }

  bool
  floatAt(size_t offset, float &value) const {
    uint32_t bits;
    if (!u32At(offset, bits))
      return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }

  /* Checks that count records of record_size bytes fit starting at offset,
   * without the multiplication overflowing. */
  bool
  fits(size_t offset, size_t count, size_t record_size) const {
    if (offset > size_)
      return false;
    size_t available = size_ - offset;
    return record_size == 0 || count <= available / record_size;
  }

private:
  const uint8_t *data_;
  size_t size_;
};

constexpr uint32_t kDxbcMagic = 0x43425844;      /* 'DXBC' */
constexpr uint32_t kRootSignatureChunk = 0x30535452; /* 'RTS0' */
constexpr size_t kDxbcChunkCountOffset = 28;
constexpr size_t kDxbcChunkOffsetsOffset = 32;

/* Locates the RTS0 chunk inside a DXBC container. */
bool
findRootSignatureChunk(const uint8_t *data, size_t size, const uint8_t *&chunk, size_t &chunk_size) {
  BlobReader container(data, size);
  uint32_t magic = 0;
  if (!container.u32At(0, magic) || magic != kDxbcMagic)
    return false;

  uint32_t chunk_count = 0;
  if (!container.u32At(kDxbcChunkCountOffset, chunk_count))
    return false;
  if (!container.fits(kDxbcChunkOffsetsOffset, chunk_count, sizeof(uint32_t)))
    return false;

  for (uint32_t i = 0; i < chunk_count; i++) {
    uint32_t offset = 0;
    if (!container.u32At(kDxbcChunkOffsetsOffset + i * sizeof(uint32_t), offset))
      return false;

    uint32_t tag = 0, length = 0;
    if (!container.u32At(offset, tag) || !container.u32At(size_t(offset) + 4, length))
      return false;
    if (tag != kRootSignatureChunk)
      continue;

    size_t payload = size_t(offset) + 8;
    if (payload > size || size - payload < length)
      return false;

    chunk = data + payload;
    chunk_size = length;
    return true;
  }

  return false;
}

RootParameterKind
parameterKindFromType(uint32_t type, bool &ok) {
  ok = true;
  switch (type) {
  case 0:
    return RootParameterKind::DescriptorTable;
  case 1:
    return RootParameterKind::Constants;
  case 2:
    return RootParameterKind::ConstantBufferView;
  case 3:
    return RootParameterKind::ShaderResourceView;
  case 4:
    return RootParameterKind::UnorderedAccessView;
  default:
    ok = false;
    return RootParameterKind::DescriptorTable;
  }
}

/* The inverse, for the serializer. */
uint32_t
rootParameterTypeFromKind(RootParameterKind kind) {
  switch (kind) {
  case RootParameterKind::DescriptorTable:
    return 0;
  case RootParameterKind::Constants:
    return 1;
  case RootParameterKind::ConstantBufferView:
    return 2;
  case RootParameterKind::ShaderResourceView:
    return 3;
  case RootParameterKind::UnorderedAccessView:
    return 4;
  }
  return 0;
}

} // namespace

namespace {

void
pushU32(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 24));
}

void
pushFloat(std::vector<uint8_t> &out, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  pushU32(out, bits);
}

void
patchU32(std::vector<uint8_t> &out, size_t offset, uint32_t value) {
  out[offset + 0] = uint8_t(value);
  out[offset + 1] = uint8_t(value >> 8);
  out[offset + 2] = uint8_t(value >> 16);
  out[offset + 3] = uint8_t(value >> 24);
}

} // namespace

bool
serializeRootSignature(const RootSignatureDescription &description, std::vector<uint8_t> &out) {
  if (description.version != 1 && description.version != 2)
    return false;

  const size_t range_record_size = description.version == 1 ? 20 : 24;
  const size_t descriptor_payload_size = description.version == 1 ? 8 : 12;

  /* The chunk is built on its own first: every offset inside it is relative to
   * its payload, so it can be laid out without knowing where the container
   * will put it. */
  std::vector<uint8_t> chunk;
  const uint32_t parameter_count = uint32_t(description.parameters.size());
  const uint32_t static_sampler_count = uint32_t(description.static_samplers.size());
  const uint32_t parameters_offset = 24;

  pushU32(chunk, description.version);
  pushU32(chunk, parameter_count);
  pushU32(chunk, parameters_offset);
  pushU32(chunk, static_sampler_count);
  pushU32(chunk, 0); /* static sampler offset, patched once its position is known */
  pushU32(chunk, description.flags);

  /* Parameter records first, with their payload offsets left blank: the
   * payloads go after all the records, so their positions are not known yet. */
  std::vector<size_t> payload_offset_slots;
  payload_offset_slots.reserve(parameter_count);
  for (const RootParameterInfo &parameter : description.parameters) {
    pushU32(chunk, rootParameterTypeFromKind(parameter.kind));
    pushU32(chunk, parameter.shader_visibility);
    payload_offset_slots.push_back(chunk.size());
    pushU32(chunk, 0);
  }

  /* Payloads. A descriptor table's payload is a count and an offset to its
   * ranges, which are emitted after all the payloads. */
  std::vector<size_t> range_offset_slots;
  for (uint32_t i = 0; i < parameter_count; i++) {
    const RootParameterInfo &parameter = description.parameters[i];
    patchU32(chunk, payload_offset_slots[i], uint32_t(chunk.size()));

    switch (parameter.kind) {
    case RootParameterKind::DescriptorTable:
      pushU32(chunk, parameter.range_count);
      range_offset_slots.push_back(chunk.size());
      pushU32(chunk, 0);
      break;
    case RootParameterKind::Constants:
      pushU32(chunk, parameter.shader_register);
      pushU32(chunk, parameter.register_space);
      pushU32(chunk, parameter.num_32bit_values);
      break;
    default:
      pushU32(chunk, parameter.shader_register);
      pushU32(chunk, parameter.register_space);
      /* 1.1 descriptors carry a flags word the 1.0 layout does not have. */
      if (descriptor_payload_size == 12)
        pushU32(chunk, parameter.descriptor_flags);
      break;
    }
  }

  size_t table_index = 0;
  for (const RootParameterInfo &parameter : description.parameters) {
    if (parameter.kind != RootParameterKind::DescriptorTable)
      continue;
    if (size_t(parameter.first_range) + parameter.range_count > description.ranges.size())
      return false;

    patchU32(chunk, range_offset_slots[table_index++], uint32_t(chunk.size()));
    for (uint32_t r = 0; r < parameter.range_count; r++) {
      const RootDescriptorRange &range = description.ranges[parameter.first_range + r];
      pushU32(chunk, uint32_t(range.range_type));
      pushU32(chunk, range.descriptor_count);
      pushU32(chunk, range.base_shader_register);
      pushU32(chunk, range.register_space);
      if (range_record_size == 24)
        pushU32(chunk, range.flags);
      pushU32(chunk, range.offset_in_descriptors_from_table_start);
    }
  }

  patchU32(chunk, 16, uint32_t(chunk.size()));
  for (const StaticSamplerInfo &sampler : description.static_samplers) {
    pushU32(chunk, sampler.filter);
    pushU32(chunk, sampler.address_u);
    pushU32(chunk, sampler.address_v);
    pushU32(chunk, sampler.address_w);
    pushFloat(chunk, sampler.mip_lod_bias);
    pushU32(chunk, sampler.max_anisotropy);
    pushU32(chunk, sampler.comparison_func);
    pushU32(chunk, sampler.border_color);
    pushFloat(chunk, sampler.min_lod);
    pushFloat(chunk, sampler.max_lod);
    pushU32(chunk, sampler.shader_register);
    pushU32(chunk, sampler.register_space);
    pushU32(chunk, sampler.shader_visibility);
  }

  /* Container: magic, a checksum nothing here verifies, one, total size, chunk
   * count, then one offset per chunk. */
  out.clear();
  pushU32(out, kDxbcMagic);
  for (int i = 0; i < 4; i++)
    pushU32(out, 0);
  pushU32(out, 1);
  const size_t total_size_slot = out.size();
  pushU32(out, 0);
  pushU32(out, 1);
  const size_t chunk_offset_slot = out.size();
  pushU32(out, 0);

  patchU32(out, chunk_offset_slot, uint32_t(out.size()));
  pushU32(out, kRootSignatureChunk);
  pushU32(out, uint32_t(chunk.size()));
  out.insert(out.end(), chunk.begin(), chunk.end());
  patchU32(out, total_size_slot, uint32_t(out.size()));
  return true;
}

bool
parseSerializedRootSignature(const void *blob, size_t size, RootSignatureDescription &out) {
  if (!blob || !size)
    return false;

  const uint8_t *chunk = nullptr;
  size_t chunk_size = 0;
  if (!findRootSignatureChunk(static_cast<const uint8_t *>(blob), size, chunk, chunk_size))
    return false;

  BlobReader reader(chunk, chunk_size);
  uint32_t version = 0, parameter_count = 0, parameters_offset = 0;
  uint32_t static_sampler_count = 0, static_samplers_offset = 0, flags = 0;

  if (!reader.u32At(0, version) || !reader.u32At(4, parameter_count) || !reader.u32At(8, parameters_offset) ||
      !reader.u32At(12, static_sampler_count) || !reader.u32At(16, static_samplers_offset) ||
      !reader.u32At(20, flags))
    return false;
  /* 1 is root signature 1.0, 2 is 1.1. The layouts differ only in the extra
   * flags word on descriptors and ranges. */
  if (version != 1 && version != 2)
    return false;

  constexpr size_t kParameterRecordSize = 12;
  if (!reader.fits(parameters_offset, parameter_count, kParameterRecordSize))
    return false;

  out = RootSignatureDescription{};
  out.version = version;
  out.flags = flags;
  out.parameters.reserve(parameter_count);

  const size_t range_record_size = version == 1 ? 20 : 24;
  const size_t descriptor_payload_size = version == 1 ? 8 : 12;

  for (uint32_t i = 0; i < parameter_count; i++) {
    size_t record = size_t(parameters_offset) + i * kParameterRecordSize;
    uint32_t type = 0, visibility = 0, payload = 0;
    if (!reader.u32At(record, type) || !reader.u32At(record + 4, visibility) || !reader.u32At(record + 8, payload))
      return false;

    bool known = false;
    RootParameterInfo info = {};
    info.kind = parameterKindFromType(type, known);
    if (!known)
      return false;
    info.shader_visibility = visibility;

    switch (info.kind) {
    case RootParameterKind::DescriptorTable: {
      uint32_t range_count = 0, ranges_offset = 0;
      if (!reader.u32At(payload, range_count) || !reader.u32At(size_t(payload) + 4, ranges_offset))
        return false;
      if (!reader.fits(ranges_offset, range_count, range_record_size))
        return false;

      info.first_range = uint32_t(out.ranges.size());
      info.range_count = range_count;

      for (uint32_t r = 0; r < range_count; r++) {
        size_t range_record = size_t(ranges_offset) + r * range_record_size;
        uint32_t range_type = 0, descriptor_count = 0, base_register = 0, space = 0;
        if (!reader.u32At(range_record, range_type) || !reader.u32At(range_record + 4, descriptor_count) ||
            !reader.u32At(range_record + 8, base_register) || !reader.u32At(range_record + 12, space))
          return false;
        if (range_type > uint32_t(DescriptorRangeType::Sampler))
          return false;

        RootDescriptorRange range = {};
        range.range_type = DescriptorRangeType(range_type);
        range.descriptor_count = descriptor_count;
        range.base_shader_register = base_register;
        range.register_space = space;
        /* 1.0 ranges have no flags word, so the trailing offset moves. */
        if (version == 1) {
          if (!reader.u32At(range_record + 16, range.offset_in_descriptors_from_table_start))
            return false;
          /* 1.0 descriptors and their data are volatile. Samplers have no
           * data-volatility flag. These are the D3D12 1.1 flag values. */
          range.flags = range.range_type == DescriptorRangeType::Sampler ? 1 : 3;
        } else {
          if (!reader.u32At(range_record + 16, range.flags) ||
              !reader.u32At(range_record + 20, range.offset_in_descriptors_from_table_start))
            return false;
        }
        out.ranges.push_back(range);
      }

      out.layout.addDescriptorTable();
      break;
    }
    case RootParameterKind::Constants: {
      if (!reader.u32At(payload, info.shader_register) || !reader.u32At(size_t(payload) + 4, info.register_space) ||
          !reader.u32At(size_t(payload) + 8, info.num_32bit_values))
        return false;
      out.layout.addConstants(info.num_32bit_values);
      break;
    }
    default: {
      if (!reader.fits(payload, 1, descriptor_payload_size))
        return false;
      if (!reader.u32At(payload, info.shader_register) || !reader.u32At(size_t(payload) + 4, info.register_space))
        return false;
      info.descriptor_flags = 2; /* 1.0 root descriptors: DATA_VOLATILE. */
      if (version == 2 && !reader.u32At(size_t(payload) + 8, info.descriptor_flags))
        return false;
      out.layout.addRootDescriptor(info.kind);
      break;
    }
    }

    out.parameters.push_back(info);
  }

  constexpr size_t kStaticSamplerRecordSize = 52;
  if (!reader.fits(static_samplers_offset, static_sampler_count, kStaticSamplerRecordSize))
    return false;
  out.static_samplers.reserve(static_sampler_count);

  for (uint32_t i = 0; i < static_sampler_count; i++) {
    size_t record = size_t(static_samplers_offset) + i * kStaticSamplerRecordSize;
    StaticSamplerInfo sampler = {};
    if (!reader.u32At(record, sampler.filter) || !reader.u32At(record + 4, sampler.address_u) ||
        !reader.u32At(record + 8, sampler.address_v) || !reader.u32At(record + 12, sampler.address_w) ||
        !reader.floatAt(record + 16, sampler.mip_lod_bias) || !reader.u32At(record + 20, sampler.max_anisotropy) ||
        !reader.u32At(record + 24, sampler.comparison_func) || !reader.u32At(record + 28, sampler.border_color) ||
        !reader.floatAt(record + 32, sampler.min_lod) || !reader.floatAt(record + 36, sampler.max_lod) ||
        !reader.u32At(record + 40, sampler.shader_register) || !reader.u32At(record + 44, sampler.register_space) ||
        !reader.u32At(record + 48, sampler.shader_visibility))
      return false;
    out.static_samplers.push_back(sampler);
  }

  return true;
}

void
IRRootSignatureBuilder::build(const RootSignatureDescription &description) {
  /* Parameters point into the range array, so it is filled first and neither
   * vector may grow afterwards. */
  ranges_.clear();
  ranges_.reserve(description.ranges.size());
  for (const RootDescriptorRange &range : description.ranges) {
    ranges_.push_back(IRDescriptorRange1{
        uint32_t(range.range_type), range.descriptor_count, range.base_shader_register, range.register_space,
        range.flags, range.offset_in_descriptors_from_table_start
    });
  }

  parameters_.clear();
  parameters_.reserve(description.parameters.size());
  for (const RootParameterInfo &parameter : description.parameters) {
    IRRootParameter1 out = {};
    out.parameter_type = uint32_t(parameter.kind);
    out.shader_visibility = parameter.shader_visibility;

    switch (parameter.kind) {
    case RootParameterKind::DescriptorTable:
      out.descriptor_table.num_ranges = parameter.range_count;
      out.descriptor_table.ranges = parameter.range_count ? ranges_.data() + parameter.first_range : nullptr;
      break;
    case RootParameterKind::Constants:
      out.constants.shader_register = parameter.shader_register;
      out.constants.register_space = parameter.register_space;
      out.constants.num_32bit_values = parameter.num_32bit_values;
      break;
    default:
      out.descriptor.shader_register = parameter.shader_register;
      out.descriptor.register_space = parameter.register_space;
      out.descriptor.flags = parameter.descriptor_flags;
      break;
    }
    parameters_.push_back(out);
  }

  static_samplers_.clear();
  static_samplers_.reserve(description.static_samplers.size());
  for (const StaticSamplerInfo &sampler : description.static_samplers) {
    static_samplers_.push_back(IRStaticSamplerDescriptor{
        sampler.filter, sampler.address_u, sampler.address_v, sampler.address_w, sampler.mip_lod_bias,
        sampler.max_anisotropy, sampler.comparison_func, sampler.border_color, sampler.min_lod, sampler.max_lod,
        sampler.shader_register, sampler.register_space, sampler.shader_visibility
    });
  }

  /* Always emitted as 1.1: the parser normalises both input versions into the
   * same description, and 1.1 is the superset. */
  versioned_.version = 2;
  versioned_.desc_1_1.num_parameters = uint32_t(parameters_.size());
  versioned_.desc_1_1.parameters = parameters_.empty() ? nullptr : parameters_.data();
  versioned_.desc_1_1.num_static_samplers = uint32_t(static_samplers_.size());
  versioned_.desc_1_1.static_samplers = static_samplers_.empty() ? nullptr : static_samplers_.data();
  versioned_.desc_1_1.flags = description.flags;
}

void
RootArgumentBuffer::setConstants(
    uint32_t index, uint32_t dest_offset_in_dwords, const void *src, uint32_t num_dwords
) {
  if (!layout_ || !src)
    return;
  const RootParameterLayout *parameter = layout_->parameter(index);
  if (!parameter || parameter->kind != RootParameterKind::Constants)
    return;
  /* A write that runs past the declared constant count would spill into the
   * next root parameter, so drop it rather than corrupt the neighbour. */
  if (dest_offset_in_dwords > parameter->num_dwords || num_dwords > parameter->num_dwords - dest_offset_in_dwords)
    return;

  std::memcpy(
      storage_.data() + parameter->offset + dest_offset_in_dwords * sizeof(uint32_t), src,
      size_t(num_dwords) * sizeof(uint32_t)
  );
}

} // namespace dxmt
