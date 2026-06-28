#pragma once

#include "d3d9_private.h"
#include "com/com_object.hpp"
#include <vector>

namespace dxmt {

class D3D9Device;

// Common implementation for IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9.
//
// Every D3D9 buffer keeps a CPU-side shadow copy. Locks hand out a pointer into
// that shadow (honouring offset/size and the read-only flag); unlocks push the
// touched range to the backing ID3D11Buffer. This handles all D3DPOOLs and the
// D3DLOCK_DISCARD / D3DLOCK_NOOVERWRITE hints uniformly: dynamic buffers use
// Map(WRITE_DISCARD/NO_OVERWRITE), everything else uses UpdateSubresource. The
// shadow also makes GetData-style read-back and partial locks trivial.
template <typename Interface, typename DescType>
class D3D9Buffer : public ComObjectClamp<Interface> {
public:
  D3D9Buffer(D3D9Device *pDevice, UINT Length, DWORD Usage, D3DPOOL Pool, const DescType &Desc);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(Interface)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_NOTFOUND; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3D_OK; }
  DWORD   STDMETHODCALLTYPE SetPriority(DWORD Priority) final { DWORD o = priority_; priority_ = Priority; return o; }
  DWORD   STDMETHODCALLTYPE GetPriority() final { return priority_; }
  void    STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return desc_.Type; }

  // IDirect3D{Vertex,Index}Buffer9
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags);
  HRESULT STDMETHODCALLTYPE Unlock();
  HRESULT STDMETHODCALLTYPE GetDesc(DescType *pDesc) {
    if (!pDesc) return D3DERR_INVALIDCALL;
    *pDesc = desc_;
    return D3D_OK;
  }

  // Internal accessors used by the device when binding the buffer.
  ID3D11Buffer *GetD3D11Buffer() const { return buffer_.ptr(); }
  UINT          Length()        const { return length_; }

protected:
  D3D9Device          *device_;
  Com<ID3D11Buffer>    buffer_;
  std::vector<uint8_t> shadow_;
  UINT                 length_;
  DescType             desc_ = {};
  DWORD                priority_ = 0;
  bool                 dynamic_ = false;

  // Active lock state.
  UINT  lock_offset_   = 0;
  UINT  lock_size_     = 0;
  DWORD lock_flags_    = 0;
  bool  locked_        = false;
};

class D3D9VertexBuffer final : public D3D9Buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC> {
public:
  using D3D9Buffer::D3D9Buffer;
};

class D3D9IndexBuffer final : public D3D9Buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC> {
public:
  using D3D9Buffer::D3D9Buffer;
};

} // namespace dxmt
