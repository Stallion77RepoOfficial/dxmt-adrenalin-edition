#include "d3d9_volume.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <cstring>

namespace dxmt {

// ---------------------------------------------------------------------------
// D3D9Volume
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE
D3D9Volume::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(reinterpret_cast<IDirect3DDevice9 *>(device_));
  return D3D_OK;
}

/* Valid by D3D9's measure, as for a surface's rectangle, in three dimensions
 * -- and for a volume D3D9 refuses anything else. */
static bool
LockBoxValid(const D3DBOX &box, const D3D9LockLayout &layout) {
  if (box.Left >= box.Right || box.Top >= box.Bottom || box.Front >= box.Back ||
      box.Right > layout.width || box.Bottom > layout.height || box.Back > layout.depth)
    return false;
  UINT wmask = layout.block_width - 1, hmask = layout.block_height - 1;
  return !((box.Left & wmask) || (box.Top & hmask) ||
           ((box.Right & wmask) && box.Right != layout.width) ||
           ((box.Bottom & hmask) && box.Bottom != layout.height));
}

bool
D3D9Volume::ReadBack(const D3D9LockLayout &layout) {
  if (!texture_)
    return true;
  auto dev = device_->GetD3D11Device();
  auto ctx = device_->GetD3D11Context();
  D3D11_TEXTURE3D_DESC td = {};
  texture_->GetDesc(&td);
  bool whole = layout.block_width > 1 || layout.block_height > 1;
  UINT staging_sub = whole ? mip_level_ : 0;
  if (!readback_) {
    D3D11_TEXTURE3D_DESC sd = td;
    if (!whole) {
      sd.Width = desc_.Width;
      sd.Height = desc_.Height;
      sd.Depth = desc_.Depth;
      sd.MipLevels = 1;
    }
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture3D(&sd, nullptr, &readback_))) {
      ERR("D3D9Volume: read-back texture creation failed");
      return false;
    }
  }
  ctx->CopySubresourceRegion(readback_.ptr(), staging_sub, 0, 0, 0, texture_.ptr(), mip_level_, nullptr);
  D3D11_MAPPED_SUBRESOURCE ms = {};
  if (FAILED(ctx->Map(readback_.ptr(), staging_sub, D3D11_MAP_READ, 0, &ms)))
    return false;
  auto src = static_cast<const uint8_t *>(ms.pData);
  UINT row_bytes = std::min(layout.BlockColumns() * layout.block_bytes, ms.RowPitch);
  for (UINT slice = 0; slice < layout.depth; slice++) {
    for (UINT row = 0; row < layout.BlockRows(); row++)
      std::memcpy(Memory(layout) + slice * layout.stored_slice_pitch + row * layout.stored_row_pitch,
                  src + slice * ms.DepthPitch + row * ms.RowPitch, row_bytes);
  }
  ctx->Unmap(readback_.ptr(), staging_sub);
  return true;
}

HRESULT STDMETHODCALLTYPE
D3D9Volume::LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) {
  auto device_lock = this->device_->LockDevice();
  if (!pLockedVolume) return D3DERR_INVALIDCALL;

  D3D9LockLayout layout;
  D3DBOX box = {0, 0, desc_.Width, desc_.Height, 0, desc_.Depth};
  if (pBox)
    box = *pBox;
  /* A default-pool volume locks only if it is dynamic. */
  bool lockable = desc_.Pool != D3DPOOL_DEFAULT || (desc_.Usage & D3DUSAGE_DYNAMIC);
  if (lock_active_ || !lockable ||
      !GetD3D9LockLayout(desc_.Format, desc_.Width, desc_.Height, desc_.Depth, &layout) ||
      (pBox && !LockBoxValid(box, layout))) {
    pLockedVolume->pBits = nullptr;
    pLockedVolume->RowPitch = 0;
    pLockedVolume->SlicePitch = 0;
    return D3DERR_INVALIDCALL;
  }

  uint8_t *memory = Memory(layout);
  /* Volumes are never rendered to, so every GPU write to one is marked. */
  bool cpu_copy = true;
  bool read_back = cpu_copy ? gpu_newer_ : !(Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
  if (read_back) {
    if (!ReadBack(layout))
      return D3DERR_INVALIDCALL;
    gpu_newer_ = false;
  }

  pLockedVolume->RowPitch = static_cast<INT>(layout.row_pitch);
  pLockedVolume->SlicePitch = static_cast<INT>(layout.slice_pitch);
  pLockedVolume->pBits = memory + layout.Offset(box.Left, box.Top, box.Front);
  lock_box_ = box;
  lock_active_ = true;
  lock_readonly_ = (Flags & D3DLOCK_READONLY) != 0;
  return D3D_OK;
}

bool
D3D9Volume::UploadTo(D3D9Volume *destination) {
  if (desc_.Pool != D3DPOOL_SYSTEMMEM || gpu_newer_ || !destination->texture_ ||
      desc_.Format != destination->desc_.Format || desc_.Width != destination->desc_.Width ||
      desc_.Height != destination->desc_.Height || desc_.Depth != destination->desc_.Depth)
    return false;
  D3D9LockLayout layout;
  if (!GetD3D9LockLayout(desc_.Format, desc_.Width, desc_.Height, desc_.Depth, &layout))
    return false;
  D3D11_BOX box = {0, 0, 0, layout.width, layout.height, layout.depth};
  device_->GetD3D11Context()->UpdateSubresource(
      destination->texture_.ptr(), destination->mip_level_, &box, Memory(layout),
      layout.stored_row_pitch, layout.stored_slice_pitch);
  return true;
}

HRESULT STDMETHODCALLTYPE
D3D9Volume::UnlockBox() {
  auto device_lock = this->device_->LockDevice();
  if (!lock_active_) return D3DERR_INVALIDCALL;
  lock_active_ = false;
  if (lock_readonly_)
    return D3D_OK;
  D3D9LockLayout layout;
  if (!texture_ || !GetD3D9LockLayout(desc_.Format, desc_.Width, desc_.Height, desc_.Depth, &layout))
    return D3D_OK;
  /* The box was validated, so only its edges need rounding out to blocks --
   * and for the formats whose memory is not laid out as reported, it is the
   * whole level. */
  UINT bw = layout.block_width, bh = layout.block_height;
  D3DBOX locked = layout.broken_pitch ? D3DBOX{0, 0, layout.width, layout.height, 0, layout.depth} : lock_box_;
  D3D11_BOX box = {locked.Left / bw * bw, locked.Top / bh * bh, locked.Front,
                   std::min((locked.Right + bw - 1) / bw * bw, layout.width),
                   std::min((locked.Bottom + bh - 1) / bh * bh, layout.height), locked.Back};
  const uint8_t *src = Memory(layout) + static_cast<size_t>(box.front) * layout.stored_slice_pitch +
                       (box.top / bh) * layout.stored_row_pitch + (box.left / bw) * layout.block_bytes;
  device_->GetD3D11Context()->UpdateSubresource(texture_.ptr(), mip_level_, &box, src, layout.stored_row_pitch,
                                                layout.stored_slice_pitch);
  return D3D_OK;
}

// ---------------------------------------------------------------------------
// D3D9VolumeTexture
// ---------------------------------------------------------------------------

D3D9VolumeTexture::D3D9VolumeTexture(
    D3D9Device *pDevice, UINT Width, UINT Height, UINT Depth, UINT Levels,
    DWORD Usage, D3DFORMAT Format, D3DPOOL Pool)
    : D3D9DeviceChild(pDevice), width_(Width), height_(Height), depth_(Depth), format_(Format), pool_(Pool),
      usage_(Usage) {
  D3D9TextureVtables::volume.store(
      *reinterpret_cast<const void *const *>(static_cast<IDirect3DVolumeTexture9 *>(this)), std::memory_order_relaxed);
  if (Levels == 0) {
    Levels = 1;
    UINT dim = Width > Height ? Width : Height;
    if (Depth > dim) dim = Depth;
    while (dim > 1) { dim >>= 1; ++Levels; }
  }

  /* A scratch texture never reaches the GPU; its levels are memory. */
  if (Pool == D3DPOOL_SCRATCH) {
    D3D9LockLayout layout;
    cpu_only_ = GetD3D9LockLayout(Format, Width, Height, Depth, &layout);
    if (cpu_only_) {
      levels_ = Levels;
      volumes_.resize(levels_);
    }
    return;
  }

  D3D9FormatInfo fi = {};
  if (!GetD3D9FormatInfo(Format, &fi) || fi.Depth)
    return; // leaves texture_ null -> IsValid() == false
  levels_ = Levels;

  D3D11_TEXTURE3D_DESC td = {};
  td.Width     = Width;
  td.Height    = Height;
  td.Depth     = Depth;
  td.MipLevels = Levels;
  td.Format    = fi.ResourceTypeless ? fi.ResourceTypeless : fi.Resource;
  td.Usage     = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if ((Usage & D3DUSAGE_AUTOGENMIPMAP) && D3D11CanGenerateMips(device_->GetD3D11Device(), td.Format)) {
    td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
  }

  if (FAILED(device_->GetD3D11Device()->CreateTexture3D(&td, nullptr, &texture_)))
    return;

  volumes_.resize(levels_);
}

/* D3D9 asks for sRGB with a sampler state; D3D11 and Metal answer with a view
 * format. So there are two views of one texture and the sampler state picks --
 * which is why the texture itself is created typeless. A format with no sRGB
 * variant keeps the single linear view and the state is ignored, as it is on
 * hardware. */
ID3D11ShaderResourceView *
D3D9VolumeTexture::GetSRV(bool SRGB) {
  Com<ID3D11ShaderResourceView> &slot = SRGB ? srv_srgb_ : srv_;
  if (slot) return slot.ptr();
  if (!texture_) return nullptr;

  D3D9FormatInfo fi = {};
  GetD3D9FormatInfo(format_, &fi);

  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format                    = SRGB && fi.ViewSRGB ? fi.ViewSRGB : fi.SRV;
  sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE3D;
  sd.Texture3D.MostDetailedMip = 0;
  sd.Texture3D.MipLevels       = levels_;
  device_->GetD3D11Device()->CreateShaderResourceView(texture_.ptr(), &sd, &slot);
  return slot.ptr();
}

HRESULT STDMETHODCALLTYPE
D3D9VolumeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(device_);
  return D3D_OK;
}

void STDMETHODCALLTYPE
D3D9VolumeTexture::GenerateMipSubLevels() {
  auto device_lock = this->device_->LockDevice();
  if (srv_ || GetSRV()) {
    device_->GetD3D11Context()->GenerateMips(srv_.ptr());
    MarkGpuWritten(1);
  }
}

uint8_t *
D3D9VolumeTexture::Memory(UINT Level) {
  if (pool_ == D3DPOOL_DEFAULT)
    return nullptr;
  if (memory_.empty()) {
    size_t size = GetD3D9TextureMemoryLayout(format_, width_, height_, depth_, levels_, 1, &memory_offsets_);
    if (!size)
      return nullptr;
    memory_.assign(size, 0);
  }
  return memory_.data() + memory_offsets_[Level];
}

void
D3D9VolumeTexture::MarkGpuWritten(UINT first_level) {
  gpu_written_from_ = std::min(gpu_written_from_, first_level);
  for (UINT level = first_level; level < levels_; level++) {
    if (volumes_[level])
      volumes_[level]->MarkGpuWritten();
  }
}

HRESULT STDMETHODCALLTYPE
D3D9VolumeTexture::GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) {
  auto device_lock = this->device_->LockDevice();
  if (!ppVolumeLevel || Level >= levels_) return D3DERR_INVALIDCALL;
  if (!volumes_[Level]) {
    if (cpu_only_)
      volumes_[Level] = new D3D9Volume(
          device_, std::max(width_ >> Level, 1u), std::max(height_ >> Level, 1u), std::max(depth_ >> Level, 1u),
          pool_, usage_, format_, static_cast<IUnknown *>(this));
    else
      volumes_[Level] = new D3D9Volume(
          device_, texture_.ptr(), Level, pool_, usage_, format_,
          static_cast<IUnknown *>(this));
    if (uint8_t *memory = Memory(Level))
      volumes_[Level]->SetMemory(memory);
    if (Level >= gpu_written_from_)
      volumes_[Level]->MarkGpuWritten();
  }
  *ppVolumeLevel = volumes_[Level].ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9VolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) {
  Com<IDirect3DVolume9> volume;
  HRESULT hr = GetVolumeLevel(Level, &volume);
  if (FAILED(hr)) return hr;
  return volume->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
D3D9VolumeTexture::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) {
  auto device_lock = this->device_->LockDevice();
  Com<IDirect3DVolume9> volume;
  HRESULT hr = GetVolumeLevel(Level, &volume);
  if (FAILED(hr)) return hr;
  return volume->LockBox(pLockedVolume, pBox, Flags);
}

HRESULT STDMETHODCALLTYPE
D3D9VolumeTexture::UnlockBox(UINT Level) {
  auto device_lock = this->device_->LockDevice();
  Com<IDirect3DVolume9> volume;
  HRESULT hr = GetVolumeLevel(Level, &volume);
  if (FAILED(hr)) return hr;
  return volume->UnlockBox();
}

} // namespace dxmt
