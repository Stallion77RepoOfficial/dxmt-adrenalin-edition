#include "d3d9_interface.hpp"
#include "d3d9_caps.hpp"
#include "d3d9_format.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <atomic>

namespace dxmt {

D3D9Interface::D3D9Interface(bool Extended) : extended_(Extended) {
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
      (extended_ && riid == __uuidof(IDirect3D9Ex))) {
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

/* The display modes this adapter reports.
 *
 * These used to answer "32 modes" and then hand back the desktop resolution
 * for every one of them. An application that only wants to know the current
 * mode never notices; one that builds a resolution list from the enumeration
 * gets thirty-two copies of a single entry, and then cannot find the
 * resolution its own configuration asks for. Splinter Cell does exactly that,
 * and faulted on the empty result inside its window driver.
 *
 * The list below is the set of modes a D3D8 or D3D9 application expects to
 * choose from, clipped to what the display can actually show. Windowed
 * rendering does not need a real mode set, so nothing here has to match a
 * scanout mode -- what matters is that the entries are distinct, ordered, and
 * include the sizes games look for. */
static const struct { UINT Width, Height; } kAdapterModes[] = {
    { 640,  480}, { 720,  480}, { 800,  600}, {1024,  768}, {1152,  864},
    {1280,  720}, {1280,  800}, {1280,  960}, {1280, 1024}, {1360,  768},
    {1440,  900}, {1600,  900}, {1600, 1200}, {1680, 1050}, {1920, 1080},
    {1920, 1200}, {2560, 1440}, {2560, 1600}, {3840, 2160},
};

/* The formats a display mode can be enumerated in. D3D9 takes the format as a
 * parameter; D3D8 has no such parameter and enumerates all of them, which is
 * why the 16-bit entries have to be here rather than only X8R8G8B8. */
static bool
IsEnumerableDisplayFormat(D3DFORMAT Format) {
  switch (Format) {
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    return true;
  default:
    return false;
  }
}

UINT
D3D9Interface::CountAdapterModes(D3DFORMAT Format, D3DDISPLAYMODE *pOut, UINT Wanted) {
  if (!IsEnumerableDisplayFormat(Format))
    return 0;

  D3DDISPLAYMODE desktop = {};
  GetAdapterDisplayMode(0, &desktop);

  UINT count = 0;
  for (auto &mode : kAdapterModes) {
    /* A mode larger than the display cannot be presented, and offering one
     * invites an application to pick it. */
    if (desktop.Width && (mode.Width > desktop.Width || mode.Height > desktop.Height))
      continue;
    if (pOut && count == Wanted) {
      pOut->Width = mode.Width;
      pOut->Height = mode.Height;
      pOut->RefreshRate = desktop.RefreshRate ? desktop.RefreshRate : 60;
      pOut->Format = Format;
      return count + 1;
    }
    count++;
  }
  /* The desktop resolution itself, last, so it is always available even when
   * it is not one of the standard sizes above. */
  if (desktop.Width) {
    if (pOut && count == Wanted) {
      *pOut = desktop;
      pOut->Format = Format;
      return count + 1;
    }
    count++;
  }
  return pOut ? 0 : count;
}

UINT STDMETHODCALLTYPE
D3D9Interface::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
  if (Adapter != 0) return 0;
  return CountAdapterModes(Format, nullptr, 0);
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
  if (Adapter != 0 || !pMode) return D3DERR_INVALIDCALL;
  return CountAdapterModes(Format, pMode, Mode) ? D3D_OK : D3DERR_INVALIDCALL;
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

/* The four formats D3D9 can drive a display with. A2R10G10B10 is fullscreen
 * only -- the desktop is never in it, so nothing can be presented to a window
 * in it either. */
static bool
IsD3D9DisplayFormat(D3DFORMAT Format, BOOL Windowed) {
  switch (Format) {
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_X8R8G8B8:
    return true;
  case D3DFMT_A2R10G10B10:
    return !Windowed;
  default:
    return false;
  }
}

/* A display format and its alpha twin are the same surface to the display
 * hardware, which is why a fullscreen X8R8G8B8 mode takes an A8R8G8B8 back
 * buffer. D3DFMT_UNKNOWN for anything without one. */
static D3DFORMAT
D3D9AlphaTwin(D3DFORMAT Format) {
  switch (Format) {
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5: return D3DFMT_A1R5G5B5;
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8: return D3DFMT_A8R8G8B8;
  default:              return D3DFMT_UNKNOWN;
  }
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CheckDeviceType(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat,
    D3DFORMAT BackBufferFormat, BOOL Windowed) {
  if (Adapter != 0)                 return D3DERR_INVALIDCALL;
  if (DevType != D3DDEVTYPE_HAL)    return D3DERR_NOTAVAILABLE;

  if (!IsD3D9DisplayFormat(DisplayFormat, Windowed))
    return D3DERR_NOTAVAILABLE;
  /* Fullscreen means setting a mode, so there has to be one to set. */
  if (!Windowed && !GetAdapterModeCount(Adapter, DisplayFormat))
    return D3DERR_NOTAVAILABLE;

  /* Windowed, D3DFMT_UNKNOWN asks for the desktop's own format; fullscreen it
   * is simply not a back buffer format. */
  D3DFORMAT back = BackBufferFormat;
  if (Windowed && back == D3DFMT_UNKNOWN)
    back = DisplayFormat;

  if (FAILED(CheckDeviceFormat(Adapter, DevType, DisplayFormat,
                               D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, back)))
    return D3DERR_NOTAVAILABLE;

  /* Windowed output is composited, so anything the desktop format can be
   * converted from will do. Fullscreen scans the back buffer out directly:
   * it has to be the display format itself or its alpha twin. */
  if (Windowed)
    return CheckDeviceFormatConversion(Adapter, DevType, back, DisplayFormat);

  if (back == DisplayFormat)
    return D3D_OK;
  D3DFORMAT twin = D3D9AlphaTwin(DisplayFormat);
  return (twin != D3DFMT_UNKNOWN && twin == D3D9AlphaTwin(back))
             ? D3D_OK : D3DERR_NOTAVAILABLE;
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
  } else if (info.Depth && RType != D3DRTYPE_TEXTURE) {
    // Depth formats are only valid as depth buffers or (shadow-map) textures;
    // a plain surface of one is not something CreateOffscreenPlainSurface
    // makes.
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
  /* Metal has 2x and 4x (and 8x, not offered here); a 3-sample texture cannot
   * be created, so reporting it supported failed the device creation that
   * followed. NONMASKABLE has one quality level, which is 4x. */
  if (MultiSampleType != D3DMULTISAMPLE_NONE && MultiSampleType != D3DMULTISAMPLE_NONMASKABLE &&
      MultiSampleType != D3DMULTISAMPLE_2_SAMPLES && MultiSampleType != D3DMULTISAMPLE_4_SAMPLES)
    return D3DERR_NOTAVAILABLE;
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

/* StretchRect converts between the colour formats this device creates
 * surfaces in; nothing converts from formats it cannot create at all -- the
 * YUV video formats among them -- nor into depth or block-compressed formats.
 * Answering yes to those sends a video player down a path that ends in a
 * failed surface creation or a frame that never arrives. */
HRESULT STDMETHODCALLTYPE
D3D9Interface::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT SourceFormat,
                                           D3DFORMAT TargetFormat) {
  if (Adapter != 0 || DevType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;
  if (SourceFormat == TargetFormat) return D3D_OK;
  D3D9FormatInfo source, target;
  if (!GetD3D9FormatInfo(SourceFormat, &source) || !GetD3D9FormatInfo(TargetFormat, &target) ||
      source.Depth || target.Depth || D3D9FormatIsDxt(TargetFormat))
    return D3DERR_NOTAVAILABLE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DevType, D3DCAPS9 *pCaps) {
  if (!pCaps) return D3DERR_INVALIDCALL;
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

/* Creating a device switches the calling thread's x87 unit to what the D3D
 * runtime was written for -- single precision, round to nearest, every
 * exception masked -- unless the application keeps its own setting with
 * D3DCREATE_FPU_PRESERVE. A 32-bit game's x87 arithmetic runs at whatever
 * precision this leaves behind, and on Windows that is single. */
static void
SetupFPU(DWORD behavior_flags) {
  if (behavior_flags & D3DCREATE_FPU_PRESERVE)
    return;
  /* On ARM64EC the game's x87 state belongs to the emulator, not to this code. */
#if (defined(__i386__) || defined(__x86_64__)) && !defined(__arm64ec__)
  uint16_t cw;
  __asm__ __volatile__("fnstcw %0" : "=m"(cw));
  cw = (cw & ~0xf3f) | 0x3f;
  __asm__ __volatile__("fldcw %0" : : "m"(cw));
#endif
}

HRESULT STDMETHODCALLTYPE
D3D9Interface::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP,
    IDirect3DDevice9 **ppDevice) {

  if (!ppDevice || !pPP) return D3DERR_INVALIDCALL;
  if (Adapter != 0 || DeviceType != D3DDEVTYPE_HAL) return D3DERR_INVALIDCALL;

  SetupFPU(BehaviorFlags);
  auto device = new D3D9Device(this, dxgi_adapter_.ptr(), hFocusWindow, BehaviorFlags, pPP, false);
  HRESULT hr = device->Initialize();
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

  SetupFPU(BehaviorFlags);
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
