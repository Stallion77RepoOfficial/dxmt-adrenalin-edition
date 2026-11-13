#pragma once

#include "d3d9_private.h"
#include "d3d9_format.hpp"
#include "d3d9_device_child.hpp"
#include <vector>

namespace dxmt {

class D3D9Device;

// IDirect3DSurface9 backed by a single subresource of an ID3D11Texture2D.
//
// A surface may either own its D3D11 texture (render targets, depth buffers,
// offscreen plain surfaces, wrapped back buffers) or reference a subresource
// of a texture it belongs to (mip level / cube face of a D3D9 texture).
//
// `container_` is what GetContainer reports: the owning texture for a
// subresource surface, the swap chain for a back buffer, and the device when
// there is nothing else (the auto depth-stencil). Whether the surface also
// *shares* that container's reference count is a separate question -- a mip
// level does, so releasing it releases the texture and D3D9 reports one count
// for both, while a back buffer keeps its own and the swap chain's count does
// not move with it.
class D3D9Surface final : public D3D9DeviceChild<IDirect3DSurface9> {
public:
  D3D9Surface(
      D3D9Device      *pDevice,
      ID3D11Texture2D *pTexture,
      UINT             Subresource,
      D3DPOOL          Pool,
      DWORD            Usage,
      D3DFORMAT        Format,
      IUnknown        *pContainer = nullptr,
      bool             SharesContainerRefCount = false)
      : D3D9DeviceChild(pDevice),
        texture_(pTexture),
        subresource_(Subresource),
        container_(pContainer),
        shares_container_refcount_(SharesContainerRefCount) {
    D3D11_TEXTURE2D_DESC td = {};
    texture_->GetDesc(&td);
    desc_.Format            = Format;
    desc_.Type              = D3DRTYPE_SURFACE;
    desc_.Usage             = Usage;
    desc_.Pool              = Pool;
    /* One sample is D3DMULTISAMPLE_NONE, not the sample count: 1 is
     * D3DMULTISAMPLE_NONMASKABLE, which every plain surface reported. */
    desc_.MultiSampleType   = td.SampleDesc.Count > 1 ? (D3DMULTISAMPLE_TYPE)td.SampleDesc.Count
                                                     : D3DMULTISAMPLE_NONE;
    desc_.MultiSampleQuality= td.SampleDesc.Count > 1 ? td.SampleDesc.Quality : 0;
    UINT mip = Subresource % (td.MipLevels ? td.MipLevels : 1);
    desc_.Width             = td.Width  >> mip ? td.Width  >> mip : 1;
    desc_.Height            = td.Height >> mip ? td.Height >> mip : 1;
  }

  /* A surface with no GPU texture: a scratch surface, or a level or face of a
   * scratch texture. Nothing on the GPU can use one -- D3D9 refuses scratch
   * resources everywhere but a lock -- so it is only the memory a lock hands
   * out, in any format D3D9 knows the layout of. */
  D3D9Surface(
      D3D9Device      *pDevice,
      UINT             Width,
      UINT             Height,
      D3DPOOL          Pool,
      DWORD            Usage,
      D3DFORMAT        Format,
      IUnknown        *pContainer = nullptr,
      bool             SharesContainerRefCount = false)
      : D3D9DeviceChild(pDevice),
        subresource_(0),
        container_(pContainer),
        shares_container_refcount_(SharesContainerRefCount) {
    desc_.Format = Format;
    desc_.Type   = D3DRTYPE_SURFACE;
    desc_.Usage  = Usage;
    desc_.Pool   = Pool;
    desc_.Width  = Width;
    desc_.Height = Height;
  }

  /* A surface that is one subresource of a texture has no count of its own:
   * D3D9 reports the container's, and an application that holds the surface
   * holds the texture. The container owns this surface privately, so it
   * outlives every such reference. */
  ULONG STDMETHODCALLTYPE AddRef() final {
    if (shares_container_refcount_)
      return container_->AddRef();
    return D3D9DeviceChild::AddRef();
  }

  ULONG STDMETHODCALLTYPE Release() final {
    if (shares_container_refcount_)
      return container_->Release();
    return D3D9DeviceChild::Release();
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
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) final {
    return private_data_.Set(guid, data, size, flags);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, DWORD *size) final {
    return private_data_.Get(guid, data, size);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) final { return private_data_.Free(guid); }
  DWORD   STDMETHODCALLTYPE SetPriority(DWORD Priority) final { DWORD o = priority_; priority_ = Priority; return o; }
  DWORD   STDMETHODCALLTYPE GetPriority() final { return priority_; }
  void    STDMETHODCALLTYPE PreLoad() final {}
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_SURFACE; }

  // IDirect3DSurface9
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final;

  /* A D3D9Ex Reset gives the swap chain new back buffers. One the application
   * still holds keeps its contents and size, and belongs to the device from
   * then on. */
  void DetachFromContainer() { container_ = nullptr; }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    *pDesc = desc_;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockRect() final;
  HRESULT STDMETHODCALLTYPE GetDC(HDC *) final { return D3DERR_INVALIDCALL; }
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) final { return D3DERR_INVALIDCALL; }

  /* The GPU wrote this subresource -- a copy, a blit, a fill -- so for a pool
   * whose CPU copy is otherwise the authority, the next lock reads back. */
  void MarkGpuWritten() { gpu_newer_ = true; }
  /* The level's part of its texture's memory, which then stands in for a copy
   * of its own (see GetD3D9TextureMemoryLayout). */
  void SetMemory(uint8_t *memory) { memory_ = memory; }
  /* D3D9 locks a default-pool surface only where it was asked for: a dynamic
   * texture, a lockable render target or back buffer, a lockable depth
   * format. Everything else in system memory, managed or scratch locks. */
  void SetLockable(bool lockable) { lockable_ = lockable; }

  // Returns false when the GPU copy is newer or this is not system memory.
  bool UploadTo(D3D9Surface *destination, const RECT &rect, const POINT &point);

  // Internal accessors
  ID3D11Texture2D *GetD3D11Texture() const { return texture_.ptr(); }
  UINT             GetSubresource()  const { return subresource_; }
  const D3DSURFACE_DESC &Desc()      const { return desc_; }

  ID3D11RenderTargetView *GetRTV();
  ID3D11DepthStencilView *GetDSV();
  // SRV over this surface's single subresource (for shader-based blits);
  // null when the backing texture wasn't created with SHADER_RESOURCE.
  ID3D11ShaderResourceView *GetSRV();

private:
  D3D9PrivateData private_data_;
  Com<ID3D11Texture2D>     texture_;
  UINT                     subresource_;
  IUnknown                *container_;   // not owned; the container holds this surface privately
  bool                     shares_container_refcount_ = false;
  D3DSURFACE_DESC          desc_ = {};
  DWORD                    priority_ = 0;

  Com<ID3D11RenderTargetView> rtv_;
  Com<ID3D11DepthStencilView> dsv_;
  Com<ID3D11ShaderResourceView> srv_;
  bool ReadBack(const D3D9LockLayout &layout);
  void Upload(const D3D9LockLayout &layout, const RECT &rect);
  void UploadData(const D3D9LockLayout &layout, const uint8_t *src, const D3D11_BOX &box,
                  ID3D11Texture2D *target, UINT subresource);
  uint8_t *Memory(const D3D9LockLayout &layout) {
    if (memory_)
      return memory_;
    if (shadow_.empty())
      shadow_.assign(layout.Size(), 0);
    return shadow_.data();
  }

  /* What a lock hands out: a CPU copy of the subresource in D3D9's layout of
   * the format. Unless the surface can be rendered to, the copy is the
   * authority, as the system-memory copy of a managed texture is on Windows:
   * a lock reads nothing back unless the GPU wrote the surface since, and an
   * unlock uploads what the lock covered. A render target or depth buffer is
   * the GPU's, and a lock reads it back unless the caller discards. */
  std::vector<uint8_t>         shadow_;
  uint8_t                     *memory_ = nullptr; // the texture's, when it has one
  Com<ID3D11Texture2D>         readback_;     // staging copy a read-back goes through
  bool                         gpu_newer_ = false;
  RECT                         lock_rect_ = {};
  bool                         lock_active_ = false;
  bool                         lock_readonly_ = false;
  bool                         lockable_ = true;
};

} // namespace dxmt
