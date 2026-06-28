#pragma once

#include "d3d9_private.h"
#include "d3d9_surface.hpp"
#include "com/com_object.hpp"
#include <vector>

namespace dxmt {

class D3D9Device;

// IDirect3DTexture9 backed by an ID3D11Texture2D. Owns the texture and its
// shader-resource view; each mip level is exposed as a D3D9Surface that borrows
// this texture's lifetime (via the surface container mechanism).
class D3D9Texture final : public ComObjectClamp<IDirect3DTexture9> {
public:
  D3D9Texture(
      D3D9Device *pDevice, UINT Width, UINT Height, UINT Levels,
      DWORD Usage, D3DFORMAT Format, D3DPOOL Pool);

  bool IsValid() const { return texture_ != nullptr; }

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
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void *, DWORD, DWORD) final { return D3D_OK; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void *, DWORD *) final { return D3DERR_NOTFOUND; }
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) final { return D3D_OK; }
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
  ID3D11ShaderResourceView *GetSRV();

private:
  D3D9Device                       *device_;
  Com<ID3D11Texture2D>              texture_;
  Com<ID3D11ShaderResourceView>     srv_;
  std::vector<Com<D3D9Surface>>     surfaces_; // one per mip level, lazily created
  UINT                              levels_ = 0;
  D3DFORMAT                         format_ = D3DFMT_UNKNOWN;
  D3DPOOL                           pool_ = D3DPOOL_DEFAULT;
  DWORD                             usage_ = 0;
  DWORD                             priority_ = 0;
  DWORD                             lod_ = 0;
  D3DTEXTUREFILTERTYPE              filter_ = D3DTEXF_LINEAR;
};

} // namespace dxmt
