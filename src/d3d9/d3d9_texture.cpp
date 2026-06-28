#include "d3d9_texture.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"

namespace dxmt {

D3D9Texture::D3D9Texture(
    D3D9Device *pDevice, UINT Width, UINT Height, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool)
    : device_(pDevice), format_(Format), pool_(Pool), usage_(Usage) {
  D3D9FormatInfo fi;
  if (!GetD3D9FormatInfo(Format, &fi))
    return; // leaves texture_ null -> IsValid() == false

  // Levels == 0 requests a full mip chain.
  if (Levels == 0) {
    Levels = 1;
    UINT dim = Width > Height ? Width : Height;
    while (dim > 1) { dim >>= 1; ++Levels; }
  }
  levels_ = Levels;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width            = Width;
  td.Height           = Height;
  td.MipLevels        = Levels;
  td.ArraySize        = 1;
  td.Format           = fi.Resource;
  td.SampleDesc.Count = 1;
  td.Usage            = D3D11_USAGE_DEFAULT;
  td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
  if (Usage & D3DUSAGE_RENDERTARGET) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
  if (Usage & D3DUSAGE_DEPTHSTENCIL) td.BindFlags  = D3D11_BIND_DEPTH_STENCIL;
  if (Usage & D3DUSAGE_AUTOGENMIPMAP)
    td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

  if (FAILED(device_->GetD3D11Device()->CreateTexture2D(&td, nullptr, &texture_)))
    return;

  surfaces_.resize(levels_);
}

ID3D11ShaderResourceView *
D3D9Texture::GetSRV() {
  if (srv_) return srv_.ptr();
  if (!texture_) return nullptr;

  D3D9FormatInfo fi;
  GetD3D9FormatInfo(format_, &fi);

  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format                    = fi.View;
  sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MostDetailedMip = 0;
  sd.Texture2D.MipLevels       = levels_;
  device_->GetD3D11Device()->CreateShaderResourceView(texture_.ptr(), &sd, &srv_);
  return srv_.ptr();
}

HRESULT STDMETHODCALLTYPE
D3D9Texture::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(device_);
  return D3D_OK;
}

void STDMETHODCALLTYPE
D3D9Texture::GenerateMipSubLevels() {
  if (srv_ || GetSRV())
    device_->GetD3D11Context()->GenerateMips(srv_.ptr());
}

HRESULT STDMETHODCALLTYPE
D3D9Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
  if (!ppSurfaceLevel || Level >= levels_) return D3DERR_INVALIDCALL;
  if (!surfaces_[Level]) {
    surfaces_[Level] = new D3D9Surface(
        device_, texture_.ptr(), Level, pool_, usage_, format_,
        static_cast<IUnknown *>(this));
  }
  *ppSurfaceLevel = surfaces_[Level].ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Texture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
  Com<IDirect3DSurface9> surface;
  HRESULT hr = GetSurfaceLevel(Level, &surface);
  if (FAILED(hr)) return hr;
  return surface->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
D3D9Texture::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  Com<IDirect3DSurface9> surface;
  HRESULT hr = GetSurfaceLevel(Level, &surface);
  if (FAILED(hr)) return hr;
  return surface->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE
D3D9Texture::UnlockRect(UINT Level) {
  Com<IDirect3DSurface9> surface;
  HRESULT hr = GetSurfaceLevel(Level, &surface);
  if (FAILED(hr)) return hr;
  return surface->UnlockRect();
}

} // namespace dxmt
