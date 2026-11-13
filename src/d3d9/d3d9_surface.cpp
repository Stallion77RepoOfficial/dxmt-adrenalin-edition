#include "d3d9_surface.hpp"
#include <chrono>
#include "d3d9_device.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <cstring>

namespace dxmt {
namespace {

uint8_t Expand4(uint16_t v) {
  v &= 0xF;
  return static_cast<uint8_t>((v << 4) | v);
}

uint16_t Shrink8(uint8_t v) {
  return static_cast<uint16_t>(v >> 4);
}

void ConvertA4R4G4B4ToBgra8(
    const uint8_t *src, UINT srcPitch, uint8_t *dst, UINT dstPitch,
    UINT width, UINT height, bool forceOpaqueAlpha) {
  for (UINT y = 0; y < height; y++) {
    auto s = reinterpret_cast<const uint16_t *>(src + y * srcPitch);
    auto d = dst + y * dstPitch;
    for (UINT x = 0; x < width; x++) {
      uint16_t p = s[x];
      d[x * 4 + 0] = Expand4(p >> 0);
      d[x * 4 + 1] = Expand4(p >> 4);
      d[x * 4 + 2] = Expand4(p >> 8);
      d[x * 4 + 3] = forceOpaqueAlpha ? 0xFF : Expand4(p >> 12);
    }
  }
}

void ConvertBgra8ToA4R4G4B4(
    const uint8_t *src, UINT srcPitch, uint8_t *dst, UINT dstPitch,
    UINT width, UINT height, bool forceOpaqueAlpha) {
  for (UINT y = 0; y < height; y++) {
    auto s = src + y * srcPitch;
    auto d = reinterpret_cast<uint16_t *>(dst + y * dstPitch);
    for (UINT x = 0; x < width; x++) {
      uint16_t b = Shrink8(s[x * 4 + 0]);
      uint16_t g = Shrink8(s[x * 4 + 1]);
      uint16_t r = Shrink8(s[x * 4 + 2]);
      uint16_t a = forceOpaqueAlpha ? 0xF : Shrink8(s[x * 4 + 3]);
      d[x] = static_cast<uint16_t>((a << 12) | (r << 8) | (g << 4) | b);
    }
  }
}

} // namespace

HRESULT STDMETHODCALLTYPE
D3D9Surface::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(reinterpret_cast<IDirect3DDevice9 *>(device_));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Surface::GetContainer(REFIID riid, void **ppContainer) {
  if (container_) return container_->QueryInterface(riid, ppContainer);
  /* A surface of nothing else belongs to the device -- and is not part of a
   * texture or a swap chain, whatever the caller asks for. */
  return device_->QueryInterface(riid, ppContainer);
}

ID3D11RenderTargetView *
D3D9Surface::GetRTV() {
  if (rtv_) return rtv_.ptr();
  if (!texture_) return nullptr;

  D3D9FormatInfo fi = {};
  if (!GetD3D9FormatInfo(desc_.Format, &fi) || fi.Depth)
    return nullptr;

  D3D11_TEXTURE2D_DESC td = {};
  texture_->GetDesc(&td);

  D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
  rtvd.Format = fi.View;
  if (td.SampleDesc.Count > 1) {
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
  } else {
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvd.Texture2D.MipSlice = subresource_ % (td.MipLevels ? td.MipLevels : 1);
  }

  if (FAILED(device_->GetD3D11Device()->CreateRenderTargetView(texture_.ptr(), &rtvd, &rtv_))) {
    ERR("D3D9Surface: CreateRenderTargetView failed");
    return nullptr;
  }
  return rtv_.ptr();
}

ID3D11ShaderResourceView *
D3D9Surface::GetSRV() {
  if (srv_) return srv_.ptr();
  if (!texture_) return nullptr;

  D3D9FormatInfo fi = {};
  if (!GetD3D9FormatInfo(desc_.Format, &fi) || fi.Depth)
    return nullptr;

  D3D11_TEXTURE2D_DESC td = {};
  texture_->GetDesc(&td);
  if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    return nullptr;

  D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
  srvd.Format = fi.View;
  if (td.SampleDesc.Count > 1) {
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MostDetailedMip = subresource_ % (td.MipLevels ? td.MipLevels : 1);
    srvd.Texture2D.MipLevels = 1;
  }
  if (FAILED(device_->GetD3D11Device()->CreateShaderResourceView(texture_.ptr(), &srvd, &srv_)))
    return nullptr;
  return srv_.ptr();
}

ID3D11DepthStencilView *
D3D9Surface::GetDSV() {
  if (dsv_) return dsv_.ptr();
  if (!texture_) return nullptr;

  D3D9FormatInfo fi = {};
  if (!GetD3D9FormatInfo(desc_.Format, &fi) || !fi.Depth)
    return nullptr;

  D3D11_TEXTURE2D_DESC td = {};
  texture_->GetDesc(&td);

  D3D11_DEPTH_STENCIL_VIEW_DESC dsvd = {};
  dsvd.Format = fi.DSV;
  if (td.SampleDesc.Count > 1) {
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
  } else {
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvd.Texture2D.MipSlice = subresource_ % (td.MipLevels ? td.MipLevels : 1);
  }

  if (FAILED(device_->GetD3D11Device()->CreateDepthStencilView(texture_.ptr(), &dsvd, &dsv_))) {
    ERR("D3D9Surface: CreateDepthStencilView failed");
    return nullptr;
  }
  return dsv_.ptr();
}

/* Valid by D3D9's measure: inside the surface, not empty, and on block
 * boundaries for a block format -- except that the right and bottom edges may
 * end mid-block where the surface does. */
static bool
LockRectValid(const RECT &rect, const D3D9LockLayout &layout) {
  if (rect.left < 0 || rect.top < 0 || rect.left >= rect.right || rect.top >= rect.bottom ||
      static_cast<UINT>(rect.right) > layout.width || static_cast<UINT>(rect.bottom) > layout.height)
    return false;
  LONG wmask = static_cast<LONG>(layout.block_width - 1);
  LONG hmask = static_cast<LONG>(layout.block_height - 1);
  return !((rect.left & wmask) || (rect.top & hmask) ||
           ((rect.right & wmask) && static_cast<UINT>(rect.right) != layout.width) ||
           ((rect.bottom & hmask) && static_cast<UINT>(rect.bottom) != layout.height));
}

bool
D3D9Surface::ReadBack(const D3D9LockLayout &layout) {
  if (!texture_)
    return true;
  auto dev = device_->GetD3D11Device();
  auto ctx = device_->GetD3D11Context();
  D3D11_TEXTURE2D_DESC td = {};
  texture_->GetDesc(&td);
  if (td.SampleDesc.Count > 1)
    return false;

  /* A block-compressed level can be smaller than a block, which a texture of
   * its own cannot; those go through a copy of the whole texture instead of
   * just the one subresource. */
  bool whole = layout.block_width > 1 || layout.block_height > 1;
  UINT staging_sub = whole ? subresource_ : 0;
  if (!readback_) {
    D3D11_TEXTURE2D_DESC sd = td;
    if (!whole) {
      sd.Width = desc_.Width;
      sd.Height = desc_.Height;
      sd.MipLevels = 1;
      sd.ArraySize = 1;
    }
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &readback_))) {
      ERR("D3D9Surface: read-back texture creation failed");
      return false;
    }
  }
  ctx->CopySubresourceRegion(readback_.ptr(), staging_sub, 0, 0, 0, texture_.ptr(), subresource_, nullptr);
  D3D11_MAPPED_SUBRESOURCE ms = {};
  if (FAILED(ctx->Map(readback_.ptr(), staging_sub, D3D11_MAP_READ, 0, &ms)))
    return false;
  auto src = static_cast<const uint8_t *>(ms.pData);
  if (D3D9FormatNeedsBgra8Expansion(desc_.Format)) {
    ConvertBgra8ToA4R4G4B4(src, ms.RowPitch, Memory(layout), layout.row_pitch, layout.width, layout.height,
                           desc_.Format == D3DFMT_X4R4G4B4);
  } else {
    UINT row_bytes = std::min(layout.BlockColumns() * layout.block_bytes, ms.RowPitch);
    for (UINT row = 0; row < layout.BlockRows(); row++)
      std::memcpy(Memory(layout) + row * layout.stored_row_pitch, src + row * ms.RowPitch, row_bytes);
  }
  ctx->Unmap(readback_.ptr(), staging_sub);
  return true;
}

bool
D3D9Surface::UploadTo(D3D9Surface *destination, const RECT &rect, const POINT &point) {
  if (desc_.Pool != D3DPOOL_SYSTEMMEM || gpu_newer_ || !destination->texture_ ||
      desc_.Format != destination->desc_.Format)
    return false;
  D3D9LockLayout layout;
  if (!GetD3D9LockLayout(desc_.Format, desc_.Width, desc_.Height, 1, &layout))
    return false;
  if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
      rect.right > static_cast<LONG>(desc_.Width) || rect.bottom > static_cast<LONG>(desc_.Height) ||
      point.x < 0 || point.y < 0 ||
      point.x + rect.right - rect.left > static_cast<LONG>(destination->desc_.Width) ||
      point.y + rect.bottom - rect.top > static_cast<LONG>(destination->desc_.Height))
    return false;
  /* A system-memory texture can be filled through retained decoder buffers.
   * Its last UnlockRect upload need not contain the bytes UpdateTexture is
   * asked to copy now. Upload only the requested rectangle, directly to the
   * destination; the source's GPU mirror is not needed for this transfer. */
  const uint8_t *src = Memory(layout) + (rect.top / layout.block_height) * layout.stored_row_pitch +
                       (rect.left / layout.block_width) * layout.block_bytes;
  D3D11_BOX box = {static_cast<UINT>(point.x), static_cast<UINT>(point.y), 0,
                   static_cast<UINT>(point.x + rect.right - rect.left),
                   static_cast<UINT>(point.y + rect.bottom - rect.top), 1};
  UploadData(layout, src, box, destination->texture_.ptr(), destination->subresource_);
  return true;
}

void
D3D9Surface::Upload(const D3D9LockLayout &layout, const RECT &rect) {
  if (!texture_)
    return;
  /* The part of the locked rectangle inside the surface, out to whole blocks.
   * A rectangle D3D9 lets through without it being valid can cover nothing
   * at all. */
  LONG bw = static_cast<LONG>(layout.block_width), bh = static_cast<LONG>(layout.block_height);
  /* Where the memory's layout is not the one reported, what a rectangle
   * covers is anyone's guess; the whole level goes. */
  RECT full = {0, 0, static_cast<LONG>(layout.width), static_cast<LONG>(layout.height)};
  const RECT &r = layout.broken_pitch ? full : rect;
  LONG left = std::max<LONG>(r.left, 0) / bw * bw;
  LONG top = std::max<LONG>(r.top, 0) / bh * bh;
  LONG right = std::min<LONG>((std::min<LONG>(r.right, layout.width) + bw - 1) / bw * bw, layout.width);
  LONG bottom = std::min<LONG>((std::min<LONG>(r.bottom, layout.height) + bh - 1) / bh * bh, layout.height);
  if (left >= right || top >= bottom)
    return;

  D3D11_BOX box = {static_cast<UINT>(left), static_cast<UINT>(top), 0,
                   static_cast<UINT>(right), static_cast<UINT>(bottom), 1};
  const uint8_t *src = Memory(layout) + (top / bh) * layout.stored_row_pitch + (left / bw) * layout.block_bytes;
  UploadData(layout, src, box, texture_.ptr(), subresource_);
}

void
D3D9Surface::UploadData(const D3D9LockLayout &layout, const uint8_t *src, const D3D11_BOX &box,
                       ID3D11Texture2D *target, UINT subresource) {
  auto ctx = device_->GetD3D11Context();
  if (D3D9FormatNeedsBgra8Expansion(desc_.Format)) {
    UINT width = box.right - box.left, height = box.bottom - box.top;
    std::vector<uint8_t> expanded(static_cast<size_t>(width) * 4 * height);
    ConvertA4R4G4B4ToBgra8(src, layout.stored_row_pitch, expanded.data(), width * 4, width, height,
                           desc_.Format == D3DFMT_X4R4G4B4);
    ctx->UpdateSubresource(target, subresource, &box, expanded.data(), width * 4, 0);
  } else {
    ctx->UpdateSubresource(target, subresource, &box, src, layout.stored_row_pitch, 0);
  }
}

HRESULT STDMETHODCALLTYPE
D3D9Surface::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  auto device_lock = this->device_->LockDevice();
  if (!pLockedRect) return D3DERR_INVALIDCALL;
  /* A second lock fails and leaves the caller's struct alone, as does a lock
   * of a surface that cannot be. */
  if (lock_active_ || !lockable_) return D3DERR_INVALIDCALL;

  D3D9LockLayout layout;
  if (!GetD3D9LockLayout(desc_.Format, desc_.Width, desc_.Height, 1, &layout))
    return D3DERR_INVALIDCALL;

  RECT rect = {0, 0, static_cast<LONG>(desc_.Width), static_cast<LONG>(desc_.Height)};
  if (pRect) {
    rect = *pRect;
    /* An invalid rectangle still locks, as on Windows: the pointer is worked
     * out from it all the same. Only a block-compressed surface the CPU has
     * no copy of refuses one. */
    if (!LockRectValid(rect, layout) && (layout.block_width > 1 || layout.block_height > 1) &&
        desc_.Pool == D3DPOOL_DEFAULT)
      return D3DERR_INVALIDCALL;
  }

  /* A lock is a rendezvous with the GPU when it reads back, so it is timed
   * like the draw work -- under DXMT_FPS=1 only, since every clock read is a
   * syscall under Wine. */
  struct LockTimer {
    D3D9Device *device;
    std::chrono::steady_clock::time_point t0;
    ~LockTimer() {
      if (device)
        device->AccountLock(std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count());
    }
  } lock_timer = device_->DiagnosticsEnabled() ? LockTimer{device_, std::chrono::steady_clock::now()}
                                               : LockTimer{nullptr, {}};

  uint8_t *memory = Memory(layout);
  /* Only a surface that can be rendered to changes without this layer seeing
   * it happen; every other GPU write -- a copy, a blit, a fill -- marks it. */
  bool cpu_copy = !(desc_.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
  bool read_back = cpu_copy ? gpu_newer_ : !(Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
  if (read_back) {
    if (!ReadBack(layout))
      return D3DERR_INVALIDCALL;
    gpu_newer_ = false;
  }

  pLockedRect->Pitch = static_cast<INT>(layout.row_pitch);
  pLockedRect->pBits = memory + layout.Offset(rect.left, rect.top, 0);
  lock_rect_ = rect;
  lock_active_ = true;
  lock_readonly_ = (Flags & D3DLOCK_READONLY) != 0;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Surface::UnlockRect() {
  auto device_lock = this->device_->LockDevice();
  if (!lock_active_) return D3DERR_INVALIDCALL;
  lock_active_ = false;
  if (lock_readonly_)
    return D3D_OK;
  D3D9LockLayout layout;
  if (!GetD3D9LockLayout(desc_.Format, desc_.Width, desc_.Height, 1, &layout))
    return D3D_OK;
  Upload(layout, lock_rect_);
  return D3D_OK;
}

} // namespace dxmt
