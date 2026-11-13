#include "d3d9_vertex_declaration.hpp"
#include "d3d9_device.hpp"

namespace dxmt {

HRESULT STDMETHODCALLTYPE
D3D9VertexDeclaration::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(device_);
  return D3D_OK;
}

std::vector<D3DVERTEXELEMENT9>
ConvertFVFToDeclaration(DWORD fvf) {
  std::vector<D3DVERTEXELEMENT9> out;
  WORD offset = 0;

  auto add = [&](BYTE type, BYTE usage, BYTE usage_index) {
    D3DVERTEXELEMENT9 e = {0, offset, type, D3DDECLMETHOD_DEFAULT, usage, usage_index};
    out.push_back(e);
    static const UINT kSize[] = {4, 8, 12, 16, 4, 4, 4, 8};
    offset += (WORD)kSize[type <= D3DDECLTYPE_SHORT4 ? type : 0];
  };

  /* The position, then the blend weights: XYZBn carries n floats after the
   * position, the last of which is the blend indices instead when a LASTBETA
   * flag says so (always for XYZB5) -- packed as UBYTE4 or D3DCOLOR. */
  DWORD pos = fvf & D3DFVF_POSITION_MASK;
  bool has_blend = pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5;
  bool has_blend_index = has_blend && (pos == D3DFVF_XYZB5 || (fvf & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)));
  UINT blend_count = has_blend ? 1 + ((pos - D3DFVF_XYZB1) >> 1) - (has_blend_index ? 1 : 0) : 0;
  if (pos == D3DFVF_XYZRHW) add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0);
  else if (pos == D3DFVF_XYZW) add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
  else if (pos) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
  if (blend_count) {
    if (pos == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR))
      add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDWEIGHT, 0);
    else
      add((BYTE)(D3DDECLTYPE_FLOAT1 + blend_count - 1), D3DDECLUSAGE_BLENDWEIGHT, 0);
  }
  if (has_blend_index) {
    if ((fvf & D3DFVF_LASTBETA_UBYTE4) || (pos == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR)))
      add(D3DDECLTYPE_UBYTE4, D3DDECLUSAGE_BLENDINDICES, 0);
    else if (fvf & D3DFVF_LASTBETA_D3DCOLOR)
      add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDINDICES, 0);
    else
      add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_BLENDINDICES, 0);
  }

  if (fvf & D3DFVF_NORMAL) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
  if (fvf & D3DFVF_PSIZE) add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
  if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
  if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);

  UINT tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  for (UINT i = 0; i < tex_count; i++) {
    UINT size = (fvf >> (i * 2 + 16)) & 0x3;
    static const BYTE kTypeForSize[] = {D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1};
    add(kTypeForSize[size], D3DDECLUSAGE_TEXCOORD, (BYTE)i);
  }

  D3DVERTEXELEMENT9 end = D3DDECL_END();
  out.push_back(end);
  return out;
}

} // namespace dxmt
