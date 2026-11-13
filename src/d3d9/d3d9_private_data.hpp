#pragma once

#include "d3d9_private.h"
#include <cstdint>
#include <vector>

namespace dxmt {

/* SetPrivateData / GetPrivateData / FreePrivateData of a resource or volume.
 *
 * An entry is a copy of the caller's bytes or, with D3DSPD_IUNKNOWN, an
 * interface the entry holds a reference on until it is replaced or freed or
 * the object it belongs to is destroyed. Getting an interface entry back adds
 * a reference for the caller. Applications use this to find their own object
 * from a resource, so storing nothing -- as every resource used to -- left
 * them with nothing to find. */
class D3D9PrivateData {
public:
  D3D9PrivateData() = default;
  D3D9PrivateData(const D3D9PrivateData &) = delete;
  D3D9PrivateData &operator=(const D3D9PrivateData &) = delete;
  ~D3D9PrivateData();

  HRESULT Set(REFGUID guid, const void *data, DWORD size, DWORD flags);
  HRESULT Get(REFGUID guid, void *data, DWORD *size);
  HRESULT Free(REFGUID guid);

private:
  struct Entry {
    GUID guid;
    std::vector<uint8_t> bytes; // the data, or the interface pointer
    IUnknown *object = nullptr; // D3DSPD_IUNKNOWN entries only
  };

  Entry *Find(REFGUID guid);

  std::vector<Entry> entries_;
};

} // namespace dxmt
