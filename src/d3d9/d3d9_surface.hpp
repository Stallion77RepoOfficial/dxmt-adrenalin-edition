#pragma once

#include "d3d9_private.h"
#include "d3d9_format.hpp"
#include "com/com_object.hpp"

namespace dxmt {

class D3D9Device;

// IDirect3DSurface9 backed by a single subresource of an ID3D11Texture2D.
//
// A surface may either own its D3D11 texture (render targets, depth buffers,
// offscreen plain surfaces, wrapped back buffers) or reference a subresource
// of a texture it belongs to (mip level / cube face of a D3D9 texture). In the
// latter case `container_` keeps the owning resource alive.
class D3D9Surface final : public ComObjectClamp<IDirect3DSurface9> {
public:
  D3D9Surface(
      D3D9Device      *pDevice,
      ID3D11Texture2D *pTexture,
      UINT             Subresource,
      D3DPOOL          Pool,
      DWORD            Usage,
      D3DFORMAT        Format,
      IUnknown        *pContainer = nullptr)
      : device_(pDevice),
        texture_(pTexture),
        subresource_(Subresource),
        container_(pContainer) {
    D3D11_TEXTURE2D_DESC td = {};
    texture_->GetDesc(&td);
    desc_.Format            = Format;
    desc_.Type              = D3DRTYPE_SURFACE;
    desc_.Usage             = Usage;
    desc_.Pool              = Pool;
    desc_.MultiSampleType   = (D3DMULTISAMPLE_TYPE)td.SampleDesc.Count;
    desc_.MultiSampleQuality= td.SampleDesc.Quality;
    UINT mip = Subresource % (td.MipLevels ? td.MipLevels : 1);
    desc_.Width             = td.Width  >> mip ? td.Width  >> mip : 1;
    desc_.Height            = td.Height >> mip ? td.Height >> mip : 1;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DSurface9)) {
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
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_SURFACE; }

  // IDirect3DSurface9
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final {
    if (container_) return container_->QueryInterface(riid, ppContainer);
    return GetDevice(reinterpret_cast<IDirect3DDevice9 **>(ppContainer));
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    *pDesc = desc_;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockRect() final;
  HRESULT STDMETHODCALLTYPE GetDC(HDC *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) final { return D3DERR_INVALIDCALL; }

  // Internal accessors
  ID3D11Texture2D *GetD3D11Texture() const { return texture_.ptr(); }
  UINT             GetSubresource()  const { return subresource_; }
  const D3DSURFACE_DESC &Desc()      const { return desc_; }

  ID3D11RenderTargetView *GetRTV();
  ID3D11DepthStencilView *GetDSV();

private:
  D3D9Device              *device_;
  Com<ID3D11Texture2D>     texture_;
  UINT                     subresource_;
  IUnknown                *container_; // not owned; surfaces of a texture borrow its lifetime via private ref
  D3DSURFACE_DESC          desc_ = {};
  DWORD                    priority_ = 0;

  Com<ID3D11RenderTargetView> rtv_;
  Com<ID3D11DepthStencilView> dsv_;
  Com<ID3D11Texture2D>        lock_staging_; // staging copy used by LockRect
  bool                        lock_readonly_ = false;
};

} // namespace dxmt
