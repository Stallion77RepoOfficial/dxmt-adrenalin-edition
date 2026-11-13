#pragma once

#include "d3d9_private.h"
#include "d3d9_device_child.hpp"
#include "sm3_translate.hpp"
#include <vector>

namespace dxmt {

class D3D9Device;

// Maps a D3DDECLTYPE to its DXGI_FORMAT equivalent. Returns DXGI_FORMAT_UNKNOWN
// for D3DDECLTYPE_UNUSED / anything unrecognized.
inline DXGI_FORMAT GetD3D9DeclTypeFormat(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1: return DXGI_FORMAT_R32_FLOAT;
  case D3DDECLTYPE_FLOAT2: return DXGI_FORMAT_R32G32_FLOAT;
  case D3DDECLTYPE_FLOAT3: return DXGI_FORMAT_R32G32B32_FLOAT;
  case D3DDECLTYPE_FLOAT4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
  case D3DDECLTYPE_D3DCOLOR: return DXGI_FORMAT_B8G8R8A8_UNORM;
  case D3DDECLTYPE_UBYTE4: return DXGI_FORMAT_R8G8B8A8_UINT;
  case D3DDECLTYPE_SHORT2: return DXGI_FORMAT_R16G16_SINT;
  case D3DDECLTYPE_SHORT4: return DXGI_FORMAT_R16G16B16A16_SINT;
  case D3DDECLTYPE_UBYTE4N: return DXGI_FORMAT_R8G8B8A8_UNORM;
  case D3DDECLTYPE_SHORT2N: return DXGI_FORMAT_R16G16_SNORM;
  case D3DDECLTYPE_SHORT4N: return DXGI_FORMAT_R16G16B16A16_SNORM;
  case D3DDECLTYPE_USHORT2N: return DXGI_FORMAT_R16G16_UNORM;
  case D3DDECLTYPE_USHORT4N: return DXGI_FORMAT_R16G16B16A16_UNORM;
  /* Neither has a format a vertex can be fetched in: both arrive as the raw
   * 32-bit word and the vertex shader decodes them (sm3::InputConversion). */
  case D3DDECLTYPE_UDEC3: return DXGI_FORMAT_R32_UINT;
  case D3DDECLTYPE_DEC3N: return DXGI_FORMAT_R32_UINT;
  case D3DDECLTYPE_FLOAT16_2: return DXGI_FORMAT_R16G16_FLOAT;
  case D3DDECLTYPE_FLOAT16_4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
  default: return DXGI_FORMAT_UNKNOWN;
  }
}

// What the vertex shader has to do to an input of this type for it to read as
// D3D9 delivers it, given the format GetD3D9DeclTypeFormat fetches it with.
inline uint32_t GetD3D9DeclTypeConversion(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT4: return sm3::kInputIntToFloat;
  case D3DDECLTYPE_UDEC3: return sm3::kInputUDec3;
  case D3DDECLTYPE_DEC3N: return sm3::kInputDec3N;
  default: return sm3::kInputAsIs;
  }
}

// IDirect3DVertexDeclaration9 backed by a stored D3DVERTEXELEMENT9 array.
// D3D11 input layouts are built lazily per (vertex shader, declaration) pair
// by the device, since they also depend on the shader's input signature.
class D3D9VertexDeclaration final : public D3D9DeviceChild<IDirect3DVertexDeclaration9> {
public:
  /* `count` includes the D3DDECL_END terminator, because GetDeclaration has to
   * report it: an application sizes its buffer from the count this returns and
   * then expects the terminator to be there.
   *
   * `fvf` is the FVF this declaration was built from, or 0 when it came from
   * an element array. SetVertexDeclaration reports it back through GetFVF, and
   * a declaration with no FVF equivalent correctly reports 0. */
  D3D9VertexDeclaration(D3D9Device *pDevice, const D3DVERTEXELEMENT9 *pElements, UINT count, DWORD fvf = 0)
      : D3D9DeviceChild(pDevice), elements_(pElements, pElements + count), fvf_(fvf) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexDeclaration9)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice);
  /* pNumElements is output only. Whatever it holds on entry is ignored -- the
   * documented way to size the buffer is to call once with a null pElements,
   * so a caller that passes a buffer has already been told how big to make it. */
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *pElements, UINT *pNumElements) {
    if (!pNumElements) return D3DERR_INVALIDCALL;
    if (pElements)
      memcpy(pElements, elements_.data(), elements_.size() * sizeof(D3DVERTEXELEMENT9));
    *pNumElements = (UINT)elements_.size();
    return D3D_OK;
  }

  /* Includes the D3DDECL_END terminator; callers that walk it must stop there. */
  const std::vector<D3DVERTEXELEMENT9> &Elements() const { return elements_; }
  DWORD Fvf() const { return fvf_; }
  uint64_t UniqueId() const { return unique_id_; }

private:
  std::vector<D3DVERTEXELEMENT9> elements_;
  DWORD fvf_;
  uint64_t unique_id_ = NextD3D9ObjectId();
};

// Converts a legacy FVF code to the equivalent D3DVERTEXELEMENT9 array
// (position/blend weights, normal, diffuse/specular color, texcoords).
std::vector<D3DVERTEXELEMENT9> ConvertFVFToDeclaration(DWORD fvf);

} // namespace dxmt
