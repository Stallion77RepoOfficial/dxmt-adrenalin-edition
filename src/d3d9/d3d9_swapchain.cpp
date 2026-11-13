#include "d3d9_swapchain.hpp"
#include "d3d9_device.hpp"

namespace dxmt {

D3D9SwapChain::D3D9SwapChain(D3D9Device *pDevice)
    : D3D9DeviceChild(pDevice), is_implicit_(true) {
  pp_ = device_->PresentParams();
}

D3D9SwapChain::D3D9SwapChain(
    D3D9Device *pDevice, Com<IDXGISwapChain> swapchain, D3D9Surface *backbuffer,
    const D3DPRESENT_PARAMETERS &pp)
    : D3D9DeviceChild(pDevice), is_implicit_(false),
      swapchain_(std::move(swapchain)), backbuffer_(backbuffer), pp_(pp) {}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject) return E_POINTER;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DSwapChain9) ||
      (riid == __uuidof(IDirect3DSwapChain9Ex) && device_->ExtendedInterface())) {
    *ppvObject = ref(this);
    return S_OK;
  }
  *ppvObject = nullptr;
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::Present(const RECT *pSrc, const RECT *pDst, HWND hDest, const RGNDATA *pDirtyRegion, DWORD) {
  auto device_lock = this->device_->LockDevice();
  present_count_++;
  if (is_implicit_)
    return device_->Present(pSrc, pDst, hDest, pDirtyRegion);
  return SUCCEEDED(swapchain_->Present(1, 0)) ? D3D_OK : D3DERR_DRIVERINTERNALERROR;
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetFrontBufferData(IDirect3DSurface9 *pDst) {
  auto device_lock = this->device_->LockDevice();
  if (is_implicit_)
    return device_->GetFrontBufferData(0, pDst);
  if (!pDst || !backbuffer_) return D3DERR_INVALIDCALL;
  auto dst = static_cast<D3D9Surface *>(pDst);
  if (!dst->GetD3D11Texture()) return D3DERR_INVALIDCALL;
  device_->GetD3D11Context()->CopySubresourceRegion(
      dst->GetD3D11Texture(), dst->GetSubresource(), 0, 0, 0,
      backbuffer_->GetD3D11Texture(), backbuffer_->GetSubresource(), nullptr);
  dst->MarkGpuWritten();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) {
  auto device_lock = this->device_->LockDevice();
  if (is_implicit_)
    return device_->GetBackBuffer(0, iBackBuffer, Type, ppBackBuffer);
  if (!ppBackBuffer || iBackBuffer != 0 || !backbuffer_) return D3DERR_INVALIDCALL;
  *ppBackBuffer = ref(backbuffer_.ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetRasterStatus(D3DRASTER_STATUS *pRS) {
  return device_->GetRasterStatus(0, pRS);
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetDisplayMode(D3DDISPLAYMODE *pMode) {
  return device_->GetDisplayMode(0, pMode);
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(static_cast<IDirect3DDevice9 *>(device_));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *pPP) {
  if (!pPP) return D3DERR_INVALIDCALL;
  *pPP = is_implicit_ ? device_->PresentParams() : pp_;
  return D3D_OK;
}

/* IDirect3DSwapChain9Ex. Present statistics describe a frame queue this does
 * not keep, so the counter is the one thing reported honestly and the rest is
 * zero -- an application reads them to pace itself and gets a steady answer. */
HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetLastPresentCount(UINT *pLastPresentCount) {
  if (!pLastPresentCount) return D3DERR_INVALIDCALL;
  *pLastPresentCount = present_count_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) {
  if (!pPresentationStatistics) return D3DERR_INVALIDCALL;
  memset(pPresentationStatistics, 0, sizeof(*pPresentationStatistics));
  pPresentationStatistics->PresentCount = present_count_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9SwapChain::GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  if (pRotation) *pRotation = D3DDISPLAYROTATION_IDENTITY;
  if (!pMode) return D3DERR_INVALIDCALL;
  D3DDISPLAYMODE mode = {};
  HRESULT hr = GetDisplayMode(&mode);
  if (FAILED(hr)) return hr;
  pMode->Size             = sizeof(*pMode);
  pMode->Width            = mode.Width;
  pMode->Height           = mode.Height;
  pMode->RefreshRate      = mode.RefreshRate;
  pMode->Format           = mode.Format;
  pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  return D3D_OK;
}

} // namespace dxmt
