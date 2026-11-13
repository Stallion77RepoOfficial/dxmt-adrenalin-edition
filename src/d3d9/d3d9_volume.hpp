#pragma once

#include "d3d9_private.h"
#include "d3d9_format.hpp"
#include "d3d9_device_child.hpp"
#include <vector>

namespace dxmt {

class D3D9Device;
class D3D9VolumeTexture;

// IDirect3DVolume9 backed by one mip level of an ID3D11Texture3D. Always owned
// by a D3D9VolumeTexture, whose lifetime it borrows via `container_`.
class D3D9Volume final : public D3D9DeviceChild<IDirect3DVolume9> {
public:
  D3D9Volume(
      D3D9Device      *pDevice,
      ID3D11Texture3D *pTexture,
      UINT             MipLevel,
      D3DPOOL          Pool,
      DWORD            Usage,
      D3DFORMAT        Format,
      IUnknown        *pContainer)
      : D3D9DeviceChild(pDevice),
        texture_(pTexture),
        mip_level_(MipLevel),
        container_(pContainer) {
    D3D11_TEXTURE3D_DESC td = {};
    texture_->GetDesc(&td);
    desc_.Format = Format;
    desc_.Type   = D3DRTYPE_VOLUME;
    desc_.Usage  = Usage;
    desc_.Pool   = Pool;
    desc_.Width  = td.Width  >> MipLevel ? td.Width  >> MipLevel : 1;
    desc_.Height = td.Height >> MipLevel ? td.Height >> MipLevel : 1;
    desc_.Depth  = td.Depth  >> MipLevel ? td.Depth  >> MipLevel : 1;
  }

  /* A level of a scratch volume texture: memory, with no GPU texture behind
   * it (see the matching surface constructor). */
  D3D9Volume(
      D3D9Device      *pDevice,
      UINT             Width,
      UINT             Height,
      UINT             Depth,
      D3DPOOL          Pool,
      DWORD            Usage,
      D3DFORMAT        Format,
      IUnknown        *pContainer)
      : D3D9DeviceChild(pDevice),
        mip_level_(0),
        container_(pContainer) {
    desc_.Format = Format;
    desc_.Type   = D3DRTYPE_VOLUME;
    desc_.Usage  = Usage;
    desc_.Pool   = Pool;
    desc_.Width  = Width;
    desc_.Height = Height;
    desc_.Depth  = Depth;
  }

  /* One mip level of a volume texture, and never anything else, so the count
   * is always the container's -- as it is for a texture's mip surfaces. */
  ULONG STDMETHODCALLTYPE AddRef() final { return container_->AddRef(); }
  ULONG STDMETHODCALLTYPE Release() final { return container_->Release(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVolume9)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  // IDirect3DVolume9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) final {
    return private_data_.Set(guid, data, size, flags);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, DWORD *size) final {
    return private_data_.Get(guid, data, size);
  }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) final { return private_data_.Free(guid); }
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) final {
    if (container_) return container_->QueryInterface(riid, ppContainer);
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC *pDesc) final {
    if (!pDesc) return D3DERR_INVALIDCALL;
    *pDesc = desc_;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockBox() final;

  void MarkGpuWritten() { gpu_newer_ = true; }
  void SetMemory(uint8_t *memory) { memory_ = memory; }
  bool UploadTo(D3D9Volume *destination);

private:
  D3D9PrivateData private_data_;
  Com<ID3D11Texture3D>  texture_;
  UINT                  mip_level_;
  IUnknown             *container_; // not owned; owns this volume privately
  D3DVOLUME_DESC        desc_ = {};

  bool ReadBack(const D3D9LockLayout &layout);
  uint8_t *Memory(const D3D9LockLayout &layout) {
    if (memory_)
      return memory_;
    if (shadow_.empty())
      shadow_.assign(layout.Size(), 0);
    return shadow_.data();
  }

  /* A CPU copy of the level, the way a surface keeps one (d3d9_surface.hpp). */
  std::vector<uint8_t>  shadow_;
  uint8_t              *memory_ = nullptr;
  Com<ID3D11Texture3D>  readback_;
  bool                  gpu_newer_ = false;
  D3DBOX                lock_box_ = {};
  bool                  lock_active_ = false;
  bool                  lock_readonly_ = false;
};

// IDirect3DVolumeTexture9 backed by an ID3D11Texture3D.
class D3D9VolumeTexture final : public D3D9DeviceChild<IDirect3DVolumeTexture9> {
public:
  D3D9VolumeTexture(
      D3D9Device *pDevice, UINT Width, UINT Height, UINT Depth, UINT Levels,
      DWORD Usage, D3DFORMAT Format, D3DPOOL Pool);

  bool IsValid() const { return texture_ != nullptr || cpu_only_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) ||
        riid == __uuidof(IDirect3DVolumeTexture9)) {
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
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_VOLUMETEXTURE; }

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) final { DWORD o = lod_; lod_ = LODNew; return o; }
  DWORD STDMETHODCALLTYPE GetLOD() final { return lod_; }
  DWORD STDMETHODCALLTYPE GetLevelCount() final { return levels_; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) final { filter_ = FilterType; return D3D_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() final { return filter_; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() final;

  // IDirect3DVolumeTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) final;
  HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) final;
  HRESULT STDMETHODCALLTYPE LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level) final;
  HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX *) final { return D3D_OK; }

  // Internal accessors used when binding the texture to a sampler.
  ID3D11Texture3D          *GetD3D11Texture() const { return texture_.ptr(); }
  ID3D11ShaderResourceView *GetSRV(bool SRGB = false);

  void MarkGpuWritten(UINT first_level = 0);

private:
  uint8_t *Memory(UINT Level);

  std::vector<uint8_t>              memory_;
  std::vector<size_t>               memory_offsets_;
  UINT                              gpu_written_from_ = ~0u;
  UINT                              width_ = 0, height_ = 0, depth_ = 0;
  bool                              cpu_only_ = false; // a scratch texture: no GPU texture at all
  D3D9PrivateData private_data_;
  Com<ID3D11Texture3D>              texture_;
  Com<ID3D11ShaderResourceView>     srv_;
  Com<ID3D11ShaderResourceView>     srv_srgb_;
  /* Private: a volume shares this texture's public count, so holding one
   * publicly would be the texture referencing itself. */
  std::vector<Com<D3D9Volume, false>> volumes_; // one per mip level, lazily created
  UINT                              levels_ = 0;
  D3DFORMAT                         format_ = D3DFMT_UNKNOWN;
  D3DPOOL                           pool_ = D3DPOOL_DEFAULT;
  DWORD                             usage_ = 0;
  DWORD                             priority_ = 0;
  DWORD                             lod_ = 0;
  D3DTEXTUREFILTERTYPE              filter_ = D3DTEXF_LINEAR;
};

} // namespace dxmt
