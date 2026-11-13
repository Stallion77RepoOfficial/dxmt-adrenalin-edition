#include "d3d9_texture.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include <algorithm>

namespace dxmt {

D3D9Texture::D3D9Texture(
    D3D9Device *pDevice, UINT Width, UINT Height, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool)
    : D3D9DeviceChild(pDevice), width_(Width), height_(Height), format_(Format), pool_(Pool), usage_(Usage) {
  // Levels == 0 requests a full mip chain.
  if (Levels == 0) {
    Levels = 1;
    UINT dim = Width > Height ? Width : Height;
    while (dim > 1) { dim >>= 1; ++Levels; }
  }

  /* A scratch texture never reaches the GPU; its levels are memory. */
  if (Pool == D3DPOOL_SCRATCH) {
    D3D9LockLayout layout;
    cpu_only_ = GetD3D9LockLayout(Format, Width, Height, 1, &layout);
    if (cpu_only_) {
      levels_ = Levels;
      surfaces_.resize(levels_);
    }
    return;
  }

  D3D9FormatInfo fi = {};
  if (!GetD3D9FormatInfo(Format, &fi))
    return; // leaves texture_ null -> IsValid() == false
  if (D3D9FormatNeedsBgra8Expansion(Format))
    fi.Resource = fi.View = fi.SRV = DXGI_FORMAT_B8G8R8A8_UNORM;
  levels_ = Levels;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width            = Width;
  td.Height           = Height;
  td.MipLevels        = Levels;
  td.ArraySize        = 1;
  td.Format           = fi.ResourceTypeless ? fi.ResourceTypeless : fi.Resource;
  td.SampleDesc.Count = 1;
  td.Usage            = D3D11_USAGE_DEFAULT;
  td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
  if (Usage & D3DUSAGE_RENDERTARGET) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
  // Not `=`: a depth *texture* is bound as depth and sampled as a texture,
  // and dropping the shader-resource flag is what made the whole INTZ family
  // unusable -- the format was accepted and the texture then could not be
  // read. The resource format for these is typeless, which is what lets D3D11
  // carry both bindings on one texture.
  if (Usage & D3DUSAGE_DEPTHSTENCIL) td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
  if ((Usage & D3DUSAGE_AUTOGENMIPMAP) && D3D11CanGenerateMips(device_->GetD3D11Device(), td.Format)) {
    td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
  }

  if (FAILED(device_->GetD3D11Device()->CreateTexture2D(&td, nullptr, &texture_)))
    return;

  surfaces_.resize(levels_);
}

/* D3D9 asks for sRGB with a sampler state; D3D11 and Metal answer with a view
 * format. So there are two views of one texture and the sampler state picks --
 * which is why the texture itself is created typeless. A format with no sRGB
 * variant keeps the single linear view and the state is ignored, as it is on
 * hardware. */
ID3D11ShaderResourceView *
D3D9Texture::GetSRV(bool SRGB) {
  Com<ID3D11ShaderResourceView> &slot = SRGB ? srv_srgb_ : srv_;
  if (slot) return slot.ptr();
  if (!texture_) return nullptr;

  D3D9FormatInfo fi = {};
  GetD3D9FormatInfo(format_, &fi);

  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format                    = SRGB && fi.ViewSRGB ? fi.ViewSRGB : fi.SRV;
  sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MostDetailedMip = 0;
  sd.Texture2D.MipLevels       = levels_;
  device_->GetD3D11Device()->CreateShaderResourceView(texture_.ptr(), &sd, &slot);
  return slot.ptr();
}

HRESULT STDMETHODCALLTYPE
D3D9Texture::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(device_);
  return D3D_OK;
}

void STDMETHODCALLTYPE
D3D9Texture::GenerateMipSubLevels() {
  auto device_lock = this->device_->LockDevice();
  if (srv_ || GetSRV()) {
    device_->GetD3D11Context()->GenerateMips(srv_.ptr());
    MarkGpuWritten(1);
  }
}

/* A texture with a system-memory copy -- managed, system memory, scratch --
 * keeps it in one block, the way Windows does (GetD3D9TextureMemoryLayout).
 * Made when a level is first asked for. */
uint8_t *
D3D9Texture::Memory(UINT Level) {
  if (pool_ == D3DPOOL_DEFAULT)
    return nullptr;
  if (memory_.empty()) {
    size_t size = GetD3D9TextureMemoryLayout(format_, width_, height_, 1, levels_, 1, &memory_offsets_);
    if (!size)
      return nullptr;
    memory_.assign(size, 0);
  }
  return memory_.data() + memory_offsets_[Level];
}

void
D3D9Texture::MarkGpuWritten(UINT first_level) {
  gpu_written_from_ = std::min(gpu_written_from_, first_level);
  for (UINT level = first_level; level < levels_; level++) {
    if (surfaces_[level])
      surfaces_[level]->MarkGpuWritten();
  }
}

HRESULT STDMETHODCALLTYPE
D3D9Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
  auto device_lock = this->device_->LockDevice();
  if (!ppSurfaceLevel || Level >= levels_) return D3DERR_INVALIDCALL;
  if (!surfaces_[Level]) {
    if (cpu_only_)
      surfaces_[Level] = new D3D9Surface(
          device_, std::max(width_ >> Level, 1u), std::max(height_ >> Level, 1u), pool_, usage_, format_,
          static_cast<IUnknown *>(this), /* SharesContainerRefCount */ true);
    else
      surfaces_[Level] = new D3D9Surface(
          device_, texture_.ptr(), Level, pool_, usage_, format_,
          static_cast<IUnknown *>(this), /* SharesContainerRefCount */ true);
    if (uint8_t *memory = Memory(Level))
      surfaces_[Level]->SetMemory(memory);
    surfaces_[Level]->SetLockable(pool_ != D3DPOOL_DEFAULT || (usage_ & D3DUSAGE_DYNAMIC));
    if (Level >= gpu_written_from_)
      surfaces_[Level]->MarkGpuWritten();
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
  auto device_lock = this->device_->LockDevice();
  Com<IDirect3DSurface9> surface;
  HRESULT hr = GetSurfaceLevel(Level, &surface);
  if (FAILED(hr)) return hr;
  return surface->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE
D3D9Texture::UnlockRect(UINT Level) {
  auto device_lock = this->device_->LockDevice();
  Com<IDirect3DSurface9> surface;
  HRESULT hr = GetSurfaceLevel(Level, &surface);
  if (FAILED(hr)) return hr;
  return surface->UnlockRect();
}

// ---------------------------------------------------------------------------
// D3D9CubeTexture
// ---------------------------------------------------------------------------

D3D9CubeTexture::D3D9CubeTexture(
    D3D9Device *pDevice, UINT EdgeLength, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool)
    : D3D9DeviceChild(pDevice), edge_(EdgeLength), format_(Format), pool_(Pool), usage_(Usage) {
  D3D9TextureVtables::cube.store(*reinterpret_cast<const void *const *>(static_cast<IDirect3DCubeTexture9 *>(this)),
                                 std::memory_order_relaxed);
  if (Levels == 0) {
    Levels = 1;
    UINT dim = EdgeLength;
    while (dim > 1) { dim >>= 1; ++Levels; }
  }

  if (Pool == D3DPOOL_SCRATCH) {
    D3D9LockLayout layout;
    cpu_only_ = GetD3D9LockLayout(Format, EdgeLength, EdgeLength, 1, &layout);
    if (cpu_only_) {
      levels_ = Levels;
      surfaces_.resize(6u * levels_);
    }
    return;
  }

  D3D9FormatInfo fi = {};
  if (!GetD3D9FormatInfo(Format, &fi))
    return; // leaves texture_ null -> IsValid() == false
  if (D3D9FormatNeedsBgra8Expansion(Format))
    fi.Resource = fi.View = fi.SRV = DXGI_FORMAT_B8G8R8A8_UNORM;
  levels_ = Levels;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width            = EdgeLength;
  td.Height           = EdgeLength;
  td.MipLevels        = Levels;
  td.ArraySize        = 6;
  td.Format           = fi.ResourceTypeless ? fi.ResourceTypeless : fi.Resource;
  td.SampleDesc.Count = 1;
  td.Usage            = D3D11_USAGE_DEFAULT;
  td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags        = D3D11_RESOURCE_MISC_TEXTURECUBE;
  if (Usage & D3DUSAGE_RENDERTARGET) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
  // Not `=`: a depth *texture* is bound as depth and sampled as a texture,
  // and dropping the shader-resource flag is what made the whole INTZ family
  // unusable -- the format was accepted and the texture then could not be
  // read. The resource format for these is typeless, which is what lets D3D11
  // carry both bindings on one texture.
  if (Usage & D3DUSAGE_DEPTHSTENCIL) td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
  if ((Usage & D3DUSAGE_AUTOGENMIPMAP) && D3D11CanGenerateMips(device_->GetD3D11Device(), td.Format)) {
    td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
  }

  if (FAILED(device_->GetD3D11Device()->CreateTexture2D(&td, nullptr, &texture_)))
    return;

  surfaces_.resize(6u * levels_);
}

/* D3D9 asks for sRGB with a sampler state; D3D11 and Metal answer with a view
 * format. So there are two views of one texture and the sampler state picks --
 * which is why the texture itself is created typeless. A format with no sRGB
 * variant keeps the single linear view and the state is ignored, as it is on
 * hardware. */
ID3D11ShaderResourceView *
D3D9CubeTexture::GetSRV(bool SRGB) {
  Com<ID3D11ShaderResourceView> &slot = SRGB ? srv_srgb_ : srv_;
  if (slot) return slot.ptr();
  if (!texture_) return nullptr;

  D3D9FormatInfo fi = {};
  GetD3D9FormatInfo(format_, &fi);

  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format                      = SRGB && fi.ViewSRGB ? fi.ViewSRGB : fi.SRV;
  sd.ViewDimension               = D3D11_SRV_DIMENSION_TEXTURECUBE;
  sd.TextureCube.MostDetailedMip = 0;
  sd.TextureCube.MipLevels       = levels_;
  device_->GetD3D11Device()->CreateShaderResourceView(texture_.ptr(), &sd, &slot);
  return slot.ptr();
}

HRESULT STDMETHODCALLTYPE
D3D9CubeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(device_);
  return D3D_OK;
}

void STDMETHODCALLTYPE
D3D9CubeTexture::GenerateMipSubLevels() {
  auto device_lock = this->device_->LockDevice();
  if (srv_ || GetSRV()) {
    device_->GetD3D11Context()->GenerateMips(srv_.ptr());
    MarkGpuWritten(1);
  }
}

uint8_t *
D3D9CubeTexture::Memory(UINT Index) {
  if (pool_ == D3DPOOL_DEFAULT)
    return nullptr;
  if (memory_.empty()) {
    size_t size = GetD3D9TextureMemoryLayout(format_, edge_, edge_, 1, levels_, 6, &memory_offsets_);
    if (!size)
      return nullptr;
    memory_.assign(size, 0);
  }
  return memory_.data() + memory_offsets_[Index];
}

void
D3D9CubeTexture::MarkGpuWritten(UINT first_level) {
  gpu_written_from_ = std::min(gpu_written_from_, first_level);
  for (UINT face = 0; face < 6; face++) {
    for (UINT level = first_level; level < levels_; level++) {
      if (surfaces_[face * levels_ + level])
        surfaces_[face * levels_ + level]->MarkGpuWritten();
    }
  }
}

HRESULT STDMETHODCALLTYPE
D3D9CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) {
  auto device_lock = this->device_->LockDevice();
  if (!ppCubeMapSurface || Level >= levels_ || (UINT)FaceType >= 6)
    return D3DERR_INVALIDCALL;
  UINT idx = (UINT)FaceType * levels_ + Level;
  if (!surfaces_[idx]) {
    UINT edge = std::max(edge_ >> Level, 1u);
    if (cpu_only_)
      surfaces_[idx] = new D3D9Surface(
          device_, edge, edge, pool_, usage_, format_, static_cast<IUnknown *>(this),
          /* SharesContainerRefCount */ true);
    else
      // D3D11 subresource of (mip, array slice) is mip + slice * mipCount.
      surfaces_[idx] = new D3D9Surface(
          device_, texture_.ptr(), Level + (UINT)FaceType * levels_, pool_,
          usage_, format_, static_cast<IUnknown *>(this),
          /* SharesContainerRefCount */ true);
    if (uint8_t *memory = Memory(idx))
      surfaces_[idx]->SetMemory(memory);
    surfaces_[idx]->SetLockable(pool_ != D3DPOOL_DEFAULT || (usage_ & D3DUSAGE_DYNAMIC));
    if (Level >= gpu_written_from_)
      surfaces_[idx]->MarkGpuWritten();
  }
  *ppCubeMapSurface = surfaces_[idx].ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9CubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
  Com<IDirect3DSurface9> surface;
  HRESULT hr = GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, Level, &surface);
  if (FAILED(hr)) return hr;
  return surface->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
D3D9CubeTexture::LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  auto device_lock = this->device_->LockDevice();
  Com<IDirect3DSurface9> surface;
  HRESULT hr = GetCubeMapSurface(FaceType, Level, &surface);
  if (FAILED(hr)) return hr;
  return surface->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE
D3D9CubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
  auto device_lock = this->device_->LockDevice();
  Com<IDirect3DSurface9> surface;
  HRESULT hr = GetCubeMapSurface(FaceType, Level, &surface);
  if (FAILED(hr)) return hr;
  return surface->UnlockRect();
}

} // namespace dxmt
