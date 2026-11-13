#include "d3d10_util.hpp"
#include <cstring>
#include <cstdint>
#include "com/com_pointer.hpp"

namespace dxmt {

void
GetD3D10Device(ID3D11DeviceChild *d3d11, ID3D10Device **ppDevice) {
  Com<ID3D11Device> d3d11_device;
  d3d11->GetDevice(&d3d11_device);
  d3d11_device->QueryInterface(IID_PPV_ARGS(ppDevice));
}

void
GetD3D10Resource(ID3D11View *d3d11, ID3D10Resource **ppResource) {
  Com<ID3D11Resource> d3d11_resource;
  d3d11->GetResource(&d3d11_resource);
  d3d11_resource->QueryInterface(IID_PPV_ARGS(ppResource));
}

UINT
ConvertD3D11ResourceFlags(UINT MiscFlags) {
  UINT ret = 0;
  if (MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS)
    ret |= D3D10_RESOURCE_MISC_GENERATE_MIPS;
  if (MiscFlags & D3D11_RESOURCE_MISC_SHARED)
    ret |= D3D10_RESOURCE_MISC_SHARED;
  if (MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
    ret |= D3D10_RESOURCE_MISC_TEXTURECUBE;
  if (MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
    ret |= D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE)
    ret |= D3D10_RESOURCE_MISC_GDI_COMPATIBLE;
  return ret;
}

UINT
ConvertD3D10ResourceFlags(UINT MiscFlags) {
  UINT ret = 0;
  if (MiscFlags & D3D10_RESOURCE_MISC_GENERATE_MIPS)
    ret |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
  if (MiscFlags & D3D10_RESOURCE_MISC_SHARED)
    ret |= D3D11_RESOURCE_MISC_SHARED;
  if (MiscFlags & D3D10_RESOURCE_MISC_TEXTURECUBE)
    ret |= D3D11_RESOURCE_MISC_TEXTURECUBE;
  if (MiscFlags & D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX)
    ret |= D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (MiscFlags & D3D10_RESOURCE_MISC_GDI_COMPATIBLE)
    ret |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
  return ret;
}

} // namespace dxmt
namespace dxmt {

bool
IsShaderModel4OrOlder(const void *pShaderBytecode, SIZE_T BytecodeLength) {
  /* DXBC container: 'DXBC', 16-byte checksum, uint32 one, uint32 total size,
   * uint32 chunk count, then chunk offsets. The version token lives in the
   * first dword of the SHDR/SHEX chunk body, high nibble major. */
  if (!pShaderBytecode || BytecodeLength < 32)
    return false;

  auto *bytes = static_cast<const uint8_t *>(pShaderBytecode);
  auto read32 = [&](SIZE_T offset) -> uint32_t {
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
  };

  if (memcmp(bytes, "DXBC", 4))
    return false;

  uint32_t chunk_count = read32(28);
  if (BytecodeLength < 32 + SIZE_T(chunk_count) * 4)
    return false;

  for (uint32_t i = 0; i < chunk_count; i++) {
    uint32_t offset = read32(32 + SIZE_T(i) * 4);
    if (offset > BytecodeLength || BytecodeLength - offset < 12)
      continue;
    if (memcmp(bytes + offset, "SHDR", 4) && memcmp(bytes + offset, "SHEX", 4))
      continue;
    uint32_t version = read32(offset + 8);
    return ((version >> 4) & 0xf) <= 4;
  }

  /* No shader chunk at all is not a shader; let the caller reject it. */
  return false;
}

} // namespace dxmt
