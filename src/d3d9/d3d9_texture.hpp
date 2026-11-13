#pragma once

#include "d3d9_private.h"
#include "d3d9_surface.hpp"
#include "d3d9_device_child.hpp"
#include <atomic>
#include <vector>

namespace dxmt {

class D3D9Device;

// IDirect3DTexture9 backed by an ID3D11Texture2D. Owns the texture and its
// shader-resource view; each mip level is exposed as a D3D9Surface that borrows
// this texture's lifetime (via the surface container mechanism).
class D3D9Texture final : public D3D9DeviceChild<IDirect3DTexture9> {
public:
  D3D9Texture(
      D3D9Device *pDevice, UINT Width, UINT Height, UINT Levels,
      DWORD Usage, D3DFORMAT Format, D3DPOOL Pool);

  bool IsValid() const { return texture_ != nullptr || cpu_only_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) ||
        riid == __uuidof(IDirect3DTexture9)) {
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
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_TEXTURE; }

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) final { DWORD o = lod_; lod_ = LODNew; return o; }
  DWORD STDMETHODCALLTYPE GetLOD() final { return lod_; }
  DWORD STDMETHODCALLTYPE GetLevelCount() final { return levels_; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) final { filter_ = FilterType; return D3D_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() final { return filter_; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() final;

  // IDirect3DTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) final;
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) final;
  HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) final;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *) final { return D3D_OK; }

  // Internal accessors used when binding the texture to a sampler.
  ID3D11Texture2D          *GetD3D11Texture() const { return texture_.ptr(); }
  ID3D11ShaderResourceView *GetSRV(bool SRGB = false);
  D3DFORMAT Format() const { return format_; }

  /* The GPU wrote levels first_level and up; see D3D9Surface::MarkGpuWritten.
   * A level whose surface is made later starts out marked. */
  void MarkGpuWritten(UINT first_level = 0);

private:
  uint8_t *Memory(UINT Level);

  std::vector<uint8_t>              memory_;
  std::vector<size_t>               memory_offsets_;
  UINT                              gpu_written_from_ = ~0u;
  UINT                              width_ = 0;
  UINT                              height_ = 0;
  bool                              cpu_only_ = false; // a scratch texture: no GPU texture at all
  D3D9PrivateData private_data_;
  Com<ID3D11Texture2D>              texture_;
  Com<ID3D11ShaderResourceView>     srv_;
  Com<ID3D11ShaderResourceView>     srv_srgb_;
  /* Private: a mip surface shares this texture's public count, so holding one
   * publicly would be the texture referencing itself. */
  std::vector<Com<D3D9Surface, false>> surfaces_; // one per mip level, lazily created
  UINT                              levels_ = 0;
  D3DFORMAT                         format_ = D3DFMT_UNKNOWN;
  D3DPOOL                           pool_ = D3DPOOL_DEFAULT;
  DWORD                             usage_ = 0;
  DWORD                             priority_ = 0;
  DWORD                             lod_ = 0;
  D3DTEXTUREFILTERTYPE              filter_ = D3DTEXF_LINEAR;
};

// IDirect3DCubeTexture9 backed by a 6-slice ID3D11Texture2D array with the
// TEXTURECUBE misc flag. Each (face, mip) pair is exposed as a D3D9Surface.
class D3D9CubeTexture final : public D3D9DeviceChild<IDirect3DCubeTexture9> {
public:
  D3D9CubeTexture(
      D3D9Device *pDevice, UINT EdgeLength, UINT Levels,
      DWORD Usage, D3DFORMAT Format, D3DPOOL Pool);

  bool IsValid() const { return texture_ != nullptr || cpu_only_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IDirect3DResource9) ||
        riid == __uuidof(IDirect3DBaseTexture9) ||
        riid == __uuidof(IDirect3DCubeTexture9)) {
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
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final { return D3DRTYPE_CUBETEXTURE; }

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) final { DWORD o = lod_; lod_ = LODNew; return o; }
  DWORD STDMETHODCALLTYPE GetLOD() final { return lod_; }
  DWORD STDMETHODCALLTYPE GetLevelCount() final { return levels_; }
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) final { filter_ = FilterType; return D3D_OK; }
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() final { return filter_; }
  void STDMETHODCALLTYPE GenerateMipSubLevels() final;

  // IDirect3DCubeTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) final;
  HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) final;
  HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) final;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES, const RECT *) final { return D3D_OK; }

  // Internal accessors used when binding the texture to a sampler.
  ID3D11Texture2D          *GetD3D11Texture() const { return texture_.ptr(); }
  ID3D11ShaderResourceView *GetSRV(bool SRGB = false);

  void MarkGpuWritten(UINT first_level = 0);

private:
  uint8_t *Memory(UINT Index); // index face * levels + level

  std::vector<uint8_t>              memory_;
  std::vector<size_t>               memory_offsets_;
  UINT                              gpu_written_from_ = ~0u;
  UINT                              edge_ = 0;
  bool                              cpu_only_ = false;
  D3D9PrivateData private_data_;
  Com<ID3D11Texture2D>              texture_;
  Com<ID3D11ShaderResourceView>     srv_;
  Com<ID3D11ShaderResourceView>     srv_srgb_;
  std::vector<Com<D3D9Surface, false>> surfaces_; // [face * levels_ + level]
  UINT                              levels_ = 0;
  D3DFORMAT                         format_ = D3DFMT_UNKNOWN;
  D3DPOOL                           pool_ = D3DPOOL_DEFAULT;
  DWORD                             usage_ = 0;
  DWORD                             priority_ = 0;
  DWORD                             lod_ = 0;
  D3DTEXTUREFILTERTYPE              filter_ = D3DTEXF_LINEAR;
};

/* D3D11 generates mips only for a texture it can also draw into, in a format
 * it can filter; an AUTOGENMIPMAP texture of any other format is made without
 * them, as D3D9 reports for it (D3DOK_NOAUTOGEN). */
inline bool
D3D11CanGenerateMips(ID3D11Device *device, DXGI_FORMAT format) {
  UINT support = 0;
  return SUCCEEDED(device->CheckFormatSupport(format, &support)) && (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN);
}

/* A bound texture's class, read off its interface vtable rather than asked
 * for: D3D9 binds a texture without calling into it -- Wine's
 * test_get_set_texture overwrites lpVtbl and binds the texture anyway -- and
 * asking cost an indirect call per bound texture per draw. The cube and
 * volume classes record their vtables when one is made; any other vtable is a
 * 2D texture's. */
struct D3D9TextureVtables {
  static inline std::atomic<const void *> cube{nullptr};
  static inline std::atomic<const void *> volume{nullptr};
};

inline D3DRESOURCETYPE
D3D9TextureType(IDirect3DBaseTexture9 *tex) {
  const void *vtbl = *reinterpret_cast<const void *const *>(tex);
  if (vtbl && vtbl == D3D9TextureVtables::cube.load(std::memory_order_relaxed))
    return D3DRTYPE_CUBETEXTURE;
  if (vtbl && vtbl == D3D9TextureVtables::volume.load(std::memory_order_relaxed))
    return D3DRTYPE_VOLUMETEXTURE;
  return D3DRTYPE_TEXTURE;
}

// Fetch4 channel of a bound texture -- 2D ones only -- see D3D9Fetch4Channel.
inline int
D3D9Fetch4Channel(IDirect3DBaseTexture9 *tex, DWORD lod_bias) {
  if (!tex || D3D9TextureType(tex) != D3DRTYPE_TEXTURE)
    return -1;
  return D3D9Fetch4Channel(static_cast<D3D9Texture *>(tex)->Format(), lod_bias);
}

} // namespace dxmt
