#include "d3d9_interface.hpp"
#include "d3d9_caps.hpp"
#include "d3d9_format.hpp"
#include "d3d9_device.hpp"
#include "d3d9_trace.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <atomic>

namespace dxmt {

D3D9Interface::D3D9Interface() {
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory_)))) {
    ERR("D3D9Interface: Failed to create DXGI factory");
    return;
  }
  dxgi_factory_->EnumAdapters(0, &dxgi_adapter_);
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::QueryInterface(REFIID riid, void **ppvObject) {
  if (ppvObject == nullptr)
    return E_POINTER;

  if (riid == __uuidof(IUnknown) ||
      riid == __uuidof(IDirect3D9) ||
      riid == __uuidof(IDirect3D9Ex)) {
    *ppvObject = ref(this);
    return S_OK;
  }

  *ppvObject = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D3D9Interface::AddRef()  { return ++ref_; }
ULONG STDMETHODCALLTYPE D3D9Interface::Release() {
  ULONG r = --ref_;
  if (r == 0) delete this;
  return r;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::RegisterSoftwareDevice(void *) { return D3D_OK; }

UINT STDMETHODCALLTYPE
D3D9Interface::GetAdapterCount() { return 1; }

HRESULT STDMETHODCALLTYPE
D3D9Interface::GetAdapterIdentifier(UINT Adapter, DWORD /*Flags*/, D3DADAPTER_IDENTIFIER9 *pId) {
  if (Adapter != 0 || !pId) return D3DERR_INVALIDCALL;

  DXGI_ADAPTER_DESC desc = {};
  if (dxgi_adapter_) dxgi_adapter_->GetDesc(&desc);

  strcpy(pId->Driver,      "dxmt.dll");
  WideCharToMultiByte(CP_ACP, 0, desc.Description, -1,
                      pId->Description, sizeof(pId->Description), nullptr, nullptr);
  strcpy(pId->DeviceName,  "\\\\.\\DISPLAY1");
  pId->DriverVersion.QuadPart = 1;
  pId->VendorId       = desc.VendorId;
  pId->DeviceId       = desc.DeviceId;
  pId->SubSysId       = desc.SubSysId;
  pId->Revision       = desc.Revision;
  pId->WHQLLevel      = 0;
  return D3D_OK;
}

UINT STDMETHODCALLTYPE
D3D9Interface::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
  if (Adapter != 0) return 0;
  // Report a reasonable number — apps only use this to enumerate, then pick
  // the desktop resolution anyway.
  return (Format == D3DFMT_X8R8G8B8 || Format == D3DFMT_R5G6B5) ? 32 : 0;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
  if (Adapter != 0 || !pMode) return D3DERR_INVALIDCALL;
  // Return the current desktop mode for every index so apps always have something.
  return GetAdapterDisplayMode(Adapter, pMode);
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
  if (Adapter != 0 || !pMode) return D3DERR_INVALIDCALL;

  Com<IDXGIOutput> output;
  if (dxgi_adapter_ && SUCCEEDED(dxgi_adapter_->EnumOutputs(0, &output))) {
    DXGI_OUTPUT_DESC desc = {};
    output->GetDesc(&desc);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(desc.Monitor, &mi);
    pMode->Width       = mi.rcMonitor.right  - mi.rcMonitor.left;
    pMode->Height      = mi.rcMonitor.bottom - mi.rcMonitor.top;
    pMode->RefreshRate = 60;
    pMode->Format      = D3DFMT_X8R8G8B8;
    return D3D_OK;
  }

  pMode->Width = 1920; pMode->Height = 1080;
  pMode->RefreshRate = 60; pMode->Format = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT, D3DFORMAT, BOOL) {
  if (Adapter != 0)                 return D3DERR_INVALIDCALL;
  if (DevType != D3DDEVTYPE_HAL)    return D3DERR_NOTAVAILABLE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT /*AdapterFormat*/, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
  if (Adapter != 0 || DevType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;

  // Format support is derived from the DXGI mapping we can actually create on
  // the Metal/D3D11 backend, gated by the requested usage. This is general
  // (no game-specific allowances): a format is reported supported only if we
  // can really back it, mirroring how the d3d11 layer derives caps from the
  // backend rather than blanket-approving.
  D3D9FormatInfo info;
  if (!GetD3D9FormatInfo(CheckFormat, &info))
    return D3DERR_NOTAVAILABLE; // no representable DXGI equivalent

  const bool isCompressed =
      CheckFormat == D3DFMT_DXT1 || CheckFormat == D3DFMT_DXT2 ||
      CheckFormat == D3DFMT_DXT3 || CheckFormat == D3DFMT_DXT4 ||
      CheckFormat == D3DFMT_DXT5;

  // Depth/stencil capability.
  if (Usage & D3DUSAGE_DEPTHSTENCIL) {
    if (!info.Depth) return D3DERR_NOTAVAILABLE;
  } else if (info.Depth && RType != D3DRTYPE_SURFACE && RType != D3DRTYPE_TEXTURE) {
    // Depth formats are only valid as surfaces or (shadow-map) textures.
    return D3DERR_NOTAVAILABLE;
  }

  // Render-target capability: depth and block-compressed formats cannot be RTs.
  if (Usage & D3DUSAGE_RENDERTARGET) {
    if (info.Depth || isCompressed) return D3DERR_NOTAVAILABLE;
  }

  // Auto-generated mipmaps require a filterable, non-depth, non-compressed format.
  if (Usage & D3DUSAGE_AUTOGENMIPMAP) {
    if (info.Depth || isCompressed) return D3DERR_NOTAVAILABLE;
  }

  // Block-compressed data only makes sense for textures/volumes, not plain
  // offscreen surfaces or as a render target.
  if (isCompressed && RType == D3DRTYPE_SURFACE) return D3DERR_NOTAVAILABLE;

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQuality) {
  if (Adapter != 0 || DevType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;
  if (MultiSampleType > D3DMULTISAMPLE_4_SAMPLES)  return D3DERR_NOTAVAILABLE;
  if (pQuality) *pQuality = 1;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT /*AdapterFormat*/, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) {
  if (Adapter != 0 || DevType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;

  // The render target must be a representable color format and the depth
  // surface a representable depth format. Any such pair is compatible on our
  // backend (no shared-bit-depth restriction on Metal).
  D3D9FormatInfo rt, ds;
  if (!GetD3D9FormatInfo(RenderTargetFormat, &rt) || rt.Depth)
    return D3DERR_NOTAVAILABLE;
  if (!GetD3D9FormatInfo(DepthStencilFormat, &ds) || !ds.Depth)
    return D3DERR_NOTAVAILABLE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT, D3DFORMAT) {
  if (Adapter != 0 || DevType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DevType, D3DCAPS9 *pCaps) {
  if (!pCaps) return D3DERR_INVALIDCALL;
  D3D9_TRACE("GetDeviceCaps adapter=", Adapter, " devType=", DevType);
  if (Adapter != 0 || DevType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;
  FillD3D9Caps(pCaps);
  return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE
D3D9Interface::GetAdapterMonitor(UINT Adapter) {
  if (Adapter != 0) return nullptr;
  Com<IDXGIOutput> output;
  if (dxgi_adapter_ && SUCCEEDED(dxgi_adapter_->EnumOutputs(0, &output))) {
    DXGI_OUTPUT_DESC desc = {};
    output->GetDesc(&desc);
    return desc.Monitor;
  }
  return nullptr;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP,
    IDirect3DDevice9 **ppDevice) {

  D3D9_TRACE("CreateDevice adapter=", Adapter, " devType=", DeviceType,
             " flags=0x", std::hex, BehaviorFlags);
  if (!ppDevice || !pPP) return D3DERR_INVALIDCALL;
  if (Adapter != 0 || DeviceType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;

  auto device = new D3D9Device(this, dxgi_adapter_.ptr(), hFocusWindow, BehaviorFlags, pPP, false);
  HRESULT hr = device->Initialize();
  D3D9_TRACE("CreateDevice Initialize hr=0x", std::hex, hr);
  if (FAILED(hr)) { device->Release(); return hr; }

  *ppDevice = device;
  return D3D_OK;
}

// IDirect3D9Ex
UINT STDMETHODCALLTYPE
D3D9Interface::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *) {
  return GetAdapterModeCount(Adapter, D3DFMT_X8R8G8B8);
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER *, UINT Mode, D3DDISPLAYMODEEX *pMode) {
  if (!pMode) return D3DERR_INVALIDCALL;
  D3DDISPLAYMODE m = {};
  HRESULT hr = EnumAdapterModes(Adapter, D3DFMT_X8R8G8B8, Mode, &m);
  if (SUCCEEDED(hr)) {
    pMode->Width = m.Width; pMode->Height = m.Height;
    pMode->RefreshRate = m.RefreshRate; pMode->Format = m.Format;
    pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  D3DDISPLAYMODE m = {};
  HRESULT hr = GetAdapterDisplayMode(Adapter, &m);
  if (SUCCEEDED(hr) && pMode) {
    pMode->Width = m.Width; pMode->Height = m.Height;
    pMode->RefreshRate = m.RefreshRate; pMode->Format = m.Format;
    pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  }
  if (pRotation) *pRotation = D3DDISPLAYROTATION_IDENTITY;
  return hr;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CreateDeviceEx(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP,
    D3DDISPLAYMODEEX * /*pFullscreen*/, IDirect3DDevice9Ex **ppDevice) {

  if (!ppDevice || !pPP) return D3DERR_INVALIDCALL;
  if (Adapter != 0 || DeviceType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;

  auto device = new D3D9Device(this, dxgi_adapter_.ptr(), hFocusWindow, BehaviorFlags, pPP, true);
  HRESULT hr = device->Initialize();
  if (FAILED(hr)) { device->Release(); return hr; }

  *ppDevice = device;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::GetAdapterLUID(UINT Adapter, LUID *pLUID) {
  if (Adapter != 0 || !pLUID) return D3DERR_INVALIDCALL;
  if (dxgi_adapter_) {
    DXGI_ADAPTER_DESC desc = {};
    dxgi_adapter_->GetDesc(&desc);
    *pLUID = desc.AdapterLuid;
    return D3D_OK;
  }
  return D3DERR_INVALIDCALL;
}

} // namespace dxmt
