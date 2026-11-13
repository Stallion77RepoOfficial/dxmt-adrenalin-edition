#pragma once

#include "d3d9_private.h"
#include "d3d9_surface.hpp"
#include "d3d9_device_child.hpp"

namespace dxmt {

class D3D9Device;

// IDirect3DSwapChain9 in one of two flavours:
//  - the device's implicit swap chain (GetSwapChain(0)): everything forwards
//    to the device, whose DXGI swap chain / back buffer it merely names;
//  - an additional swap chain (CreateAdditionalSwapChain): owns its own DXGI
//    swap chain and back-buffer surface targeting a second window.
class D3D9SwapChain final : public D3D9DeviceChild<IDirect3DSwapChain9Ex> {
public:
  // Implicit swap chain wrapper.
  explicit D3D9SwapChain(D3D9Device *pDevice);
  // Additional swap chain owning its own DXGI objects.
  D3D9SwapChain(
      D3D9Device *pDevice, Com<IDXGISwapChain> swapchain, D3D9Surface *backbuffer,
      const D3DPRESENT_PARAMETERS &pp);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;

  HRESULT STDMETHODCALLTYPE Present(const RECT *pSrc, const RECT *pDst, HWND hDest, const RGNDATA *pDirtyRegion, DWORD dwFlags) final;
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *pDst) final;
  HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) final;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) final;
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *pPP) final;

  // IDirect3DSwapChain9Ex
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) final;
  HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) final;
  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) final;

  /* Set once, right after construction: the back buffer names this swap chain
   * as its container, so it cannot be built before the swap chain exists. */
  void SetBackBuffer(D3D9Surface *pBackBuffer) { backbuffer_ = pBackBuffer; }

private:
  bool                   is_implicit_;
  Com<IDXGISwapChain>    swapchain_;  // additional only
  /* Private, like every implicit object: a back buffer's public count starts
   * at zero and returns there without the surface being freed, and it is not
   * forwarded to the swap chain that contains it. */
  Com<D3D9Surface, false> backbuffer_; // additional only
  D3DPRESENT_PARAMETERS  pp_ = {};
  UINT                   present_count_ = 0;
};

} // namespace dxmt
