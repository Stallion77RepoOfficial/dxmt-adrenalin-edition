#include "d3d9_device.hpp"
#include "d3d9_buffer.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace dxmt {
namespace {

UINT ElementBytes(BYTE type) {
  if (type <= D3DDECLTYPE_FLOAT4) return (type + 1) * sizeof(float);
  if (type == D3DDECLTYPE_D3DCOLOR) return sizeof(DWORD);
  return 0;
}

D3DMATRIX MatrixProduct(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX result = {};
  for (UINT r = 0; r < 4; r++)
    for (UINT c = 0; c < 4; c++)
      for (UINT k = 0; k < 4; k++) result.m[r][c] += a.m[r][k] * b.m[k][c];
  return result;
}

} // namespace

// Legacy Direct3D adapters read transformed vertices back to the CPU before
// drawing them. Keep this unlit fixed-function path in the buffer shadows;
// it needs neither a GPU round trip nor shader compilation.
HRESULT STDMETHODCALLTYPE
D3D9Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                          IDirect3DVertexBuffer9 *pDestBuffer,
                          IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) {
  auto device_lock = LockDevice();
  constexpr DWORD allowed_flags = D3DPV_DONOTCOPYDATA | D3DLOCK_DISCARD |
      D3DLOCK_NOOVERWRITE | D3DLOCK_NOSYSLOCK | D3DLOCK_DONOTWAIT;
  if (!pDestBuffer || (Flags & ~allowed_flags)) return D3DERR_INVALIDCALL;
  Com<IDirect3DDevice9> owner;
  if (FAILED(pDestBuffer->GetDevice(&owner)) || owner.ptr() != this) return D3DERR_INVALIDCALL;
  if (!VertexCount) return D3D_OK;

  // Do not report success for vertex programs or fixed-function operations
  // whose results this CPU path cannot produce yet.
  if (pVertexDecl || vertex_shader_ || render_state_[D3DRS_LIGHTING] ||
      render_state_[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE ||
      render_state_[D3DRS_INDEXEDVERTEXBLENDENABLE] ||
      (render_state_[D3DRS_FOGENABLE] && render_state_[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE))
    return E_NOTIMPL;
  for (UINT stage = 0; stage < 8; stage++)
    if (tss_[stage][D3DTSS_TEXTURETRANSFORMFLAGS] != D3DTTFF_DISABLE ||
        (tss_[stage][D3DTSS_TEXCOORDINDEX] & 0xffff0000)) return E_NOTIMPL;

  const auto *inputs = CurrentDeclarationElements();
  D3DVERTEXBUFFER_DESC desc = {};
  if (!inputs || FAILED(pDestBuffer->GetDesc(&desc)) || !desc.FVF) return D3DERR_INVALIDCALL;
  auto *output_decl = ImplicitDeclarationForFVF(desc.FVF);
  if (!output_decl) return D3DERR_INVALIDCALL;
  const auto &outputs = output_decl->Elements();
  const D3DVERTEXELEMENT9 *position = nullptr;
  UINT required[kMaxStreams] = {}, destination_stride = 0;
  for (const auto &e : *inputs) {
    if (e.Stream == 0xff) break;
    UINT bytes = ElementBytes(e.Type);
    if (!bytes) return E_NOTIMPL;
    if (e.Stream >= kMaxStreams || e.Method != D3DDECLMETHOD_DEFAULT) return D3DERR_INVALIDCALL;
    required[e.Stream] = std::max(required[e.Stream], UINT(e.Offset) + bytes);
    if ((e.Usage == D3DDECLUSAGE_POSITION || e.Usage == D3DDECLUSAGE_POSITIONT) && !e.UsageIndex)
      position = &e;
  }
  if (!position || (position->Type != D3DDECLTYPE_FLOAT3 && position->Type != D3DDECLTYPE_FLOAT4))
    return E_NOTIMPL;
  if (position->Usage == D3DDECLUSAGE_POSITIONT && position->Type != D3DDECLTYPE_FLOAT4)
    return D3DERR_INVALIDCALL;

  struct AttributeCopy {
    const D3DVERTEXELEMENT9 *source, *destination;
    UINT bytes;
  };
  std::array<AttributeCopy, 16> copies;
  UINT copy_count = 0, position_offset = ~0u;
  for (const auto &out : outputs) {
    if (out.Stream == 0xff) break;
    UINT bytes = ElementBytes(out.Type);
    if (!bytes || out.Stream) return E_NOTIMPL;
    destination_stride = std::max(destination_stride, UINT(out.Offset) + bytes);
    if (out.Usage == D3DDECLUSAGE_POSITIONT && !out.UsageIndex && out.Type == D3DDECLTYPE_FLOAT4) {
      position_offset = out.Offset;
      continue;
    }
    if (out.Usage != D3DDECLUSAGE_COLOR && out.Usage != D3DDECLUSAGE_TEXCOORD && out.Usage != D3DDECLUSAGE_PSIZE)
      return E_NOTIMPL;
    const D3DVERTEXELEMENT9 *source = nullptr;
    for (const auto &in : *inputs) {
      if (in.Stream == 0xff) break;
      if (in.Usage == out.Usage && in.UsageIndex == out.UsageIndex) { source = &in; break; }
    }
    if (source && source->Type != out.Type &&
        !(source->Type <= D3DDECLTYPE_FLOAT4 && out.Type <= D3DDECLTYPE_FLOAT4)) return E_NOTIMPL;
    if (copy_count == copies.size()) return E_NOTIMPL;
    copies[copy_count++] = {source, &out, source ? std::min(bytes, ElementBytes(source->Type)) : 0};
  }
  if (position_offset == ~0u || !destination_stride) return E_NOTIMPL;
  uint64_t destination_offset = uint64_t(DestIndex) * destination_stride;
  uint64_t destination_bytes = uint64_t(VertexCount) * destination_stride;
  if (destination_offset > desc.Size || destination_bytes > desc.Size - destination_offset)
    return D3DERR_INVALIDCALL;

  const uint8_t *source_data[kMaxStreams] = {};
  std::array<std::vector<uint8_t>, kMaxStreams> snapshots;
  for (UINT stream = 0; stream < kMaxStreams; stream++) {
    if (!required[stream]) continue;
    const auto &binding = streams_[stream];
    if (!binding.buffer || binding.stride < required[stream]) return D3DERR_INVALIDCALL;
    uint64_t offset = uint64_t(binding.offset) + uint64_t(SrcStartIndex) * binding.stride;
    uint64_t bytes = uint64_t(VertexCount - 1) * binding.stride + required[stream];
    if (offset > binding.buffer->Length() || bytes > binding.buffer->Length() - offset)
      return D3DERR_INVALIDCALL;
    source_data[stream] = binding.buffer->ShadowData() + offset;
    // An expanding XYZRHW destination can overwrite the next XYZ source
    // vertex even when source and destination start at the same offset.
    if (binding.buffer.ptr() == pDestBuffer) {
      snapshots[stream].assign(source_data[stream], source_data[stream] + bytes);
      source_data[stream] = snapshots[stream].data();
    }
  }

  D3DMATRIX world, view, projection;
  GetTransform(D3DTS_WORLD, &world);
  GetTransform(D3DTS_VIEW, &view);
  GetTransform(D3DTS_PROJECTION, &projection);
  D3DMATRIX transform = MatrixProduct(MatrixProduct(world, view), projection);
  void *mapped = nullptr;
  HRESULT hr = pDestBuffer->Lock(UINT(destination_offset), UINT(destination_bytes), &mapped, Flags & ~D3DPV_DONOTCOPYDATA);
  if (FAILED(hr)) return hr;
  for (UINT vertex = 0; vertex < VertexCount; vertex++) {
    uint8_t *destination = static_cast<uint8_t *>(mapped) + vertex * destination_stride;
    const uint8_t *source = source_data[position->Stream] + vertex * streams_[position->Stream].stride + position->Offset;
    float input[4] = {0, 0, 0, 1}, output[4];
    memcpy(input, source, ElementBytes(position->Type));
    if (position->Usage == D3DDECLUSAGE_POSITIONT) memcpy(output, input, sizeof(output));
    else {
      float clip[4] = {};
      for (UINT c = 0; c < 4; c++)
        for (UINT r = 0; r < 4; r++) clip[c] += input[r] * transform.m[r][c];
      float rhw = 1.0f / clip[3];
      output[0] = viewport_.X + (1 + clip[0] * rhw) * viewport_.Width * 0.5f;
      output[1] = viewport_.Y + (1 - clip[1] * rhw) * viewport_.Height * 0.5f;
      output[2] = viewport_.MinZ + clip[2] * rhw * (viewport_.MaxZ - viewport_.MinZ);
      output[3] = rhw;
    }
    memcpy(destination + position_offset, output, sizeof(output));
    if (Flags & D3DPV_DONOTCOPYDATA) continue;
    for (UINT i = 0; i < copy_count; i++) {
      const auto &copy = copies[i];
      if (copy.source)
        memcpy(destination + copy.destination->Offset,
               source_data[copy.source->Stream] + vertex * streams_[copy.source->Stream].stride + copy.source->Offset, copy.bytes);
      else if (copy.destination->Usage == D3DDECLUSAGE_COLOR) {
        DWORD color = copy.destination->UsageIndex ? 0 : 0xffffffff;
        memcpy(destination + copy.destination->Offset, &color, sizeof(color));
      }
    }
  }
  return pDestBuffer->Unlock();
}

} // namespace dxmt
