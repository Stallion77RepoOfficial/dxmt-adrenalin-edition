#pragma once

#include "d3d9_private.h"

namespace dxmt {

class D3D9Device;

class D3D9Interface final : public IDirect3D9Ex {
public:
  /* Extended when it came from Direct3DCreate9Ex. It decides what this
   * interface, and every device and swap chain below it, will answer to a
   * QueryInterface for the Ex form -- and only that: which of CreateDevice
   * and CreateDeviceEx built a device makes no difference. */
  explicit D3D9Interface(bool Extended);

  bool IsExtended() const { return extended_; }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  ULONG   STDMETHODCALLTYPE AddRef() final;
  ULONG   STDMETHODCALLTYPE Release() final;

  // IDirect3D9
  HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *pInitializeFunction) final;
  UINT    STDMETHODCALLTYPE GetAdapterCount() final;
  HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) final;
  UINT    STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) final;
  // Shared by the count and the enumeration: with pOut null it counts the
  // modes, with pOut set it fills in mode number Wanted and returns non-zero.
  UINT    CountAdapterModes(D3DFORMAT Format, D3DDISPLAYMODE *pOut, UINT Wanted);
  HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels) final;
  HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) final;
  HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE Adapter2, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) final;
  HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) final;
  HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) final;
  HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface) final;

  // IDirect3D9Ex
  UINT    STDMETHODCALLTYPE GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) final;
  HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode) final;
  HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) final;
  HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode, IDirect3DDevice9Ex **ppReturnedDeviceInterface) final;
  HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID *pLUID) final;

private:
  std::atomic<ULONG> ref_ = {1};
  bool               extended_;
  Com<IDXGIFactory1> dxgi_factory_;
  Com<IDXGIAdapter>  dxgi_adapter_;
};

} // namespace dxmt
