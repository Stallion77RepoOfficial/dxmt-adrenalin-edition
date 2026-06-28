#include "d3d9_surface.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"

namespace dxmt {

HRESULT STDMETHODCALLTYPE
D3D9Surface::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(reinterpret_cast<IDirect3DDevice9 *>(device_));
  return D3D_OK;
}

ID3D11RenderTargetView *
D3D9Surface::GetRTV() {
  if (rtv_) return rtv_.ptr();

  D3D9FormatInfo fi;
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

ID3D11DepthStencilView *
D3D9Surface::GetDSV() {
  if (dsv_) return dsv_.ptr();

  D3D9FormatInfo fi;
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

HRESULT STDMETHODCALLTYPE
D3D9Surface::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT * /*pRect*/, DWORD Flags) {
  if (!pLockedRect) return D3DERR_INVALIDCALL;

  auto dev = device_->GetD3D11Device();
  auto ctx = device_->GetD3D11Context();

  D3D11_TEXTURE2D_DESC td = {};
  texture_->GetDesc(&td);

  // Create (once) a staging texture to read/write the subresource through.
  if (!lock_staging_) {
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.MiscFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &lock_staging_))) {
      ERR("D3D9Surface::LockRect: staging texture creation failed");
      return D3DERR_INVALIDCALL;
    }
  }

  // Unless the app discards, pull current GPU contents into the staging copy.
  if (!(Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)))
    ctx->CopySubresourceRegion(lock_staging_.ptr(), subresource_, 0, 0, 0,
                               texture_.ptr(), subresource_, nullptr);

  D3D11_MAP map = (Flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
  D3D11_MAPPED_SUBRESOURCE ms = {};
  if (FAILED(ctx->Map(lock_staging_.ptr(), subresource_, map, 0, &ms)))
    return D3DERR_INVALIDCALL;

  pLockedRect->Pitch = (INT)ms.RowPitch;
  pLockedRect->pBits = ms.pData;
  lock_readonly_ = (Flags & D3DLOCK_READONLY) != 0;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Surface::UnlockRect() {
  if (!lock_staging_) return D3DERR_INVALIDCALL;
  auto ctx = device_->GetD3D11Context();
  ctx->Unmap(lock_staging_.ptr(), subresource_);
  // Push edits back to the GPU resource.
  if (!lock_readonly_)
    ctx->CopySubresourceRegion(texture_.ptr(), subresource_, 0, 0, 0,
                               lock_staging_.ptr(), subresource_, nullptr);
  return D3D_OK;
}

} // namespace dxmt
