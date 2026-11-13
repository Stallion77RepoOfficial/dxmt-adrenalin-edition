#include "d3d9_device.hpp"
#include <bit>
#include <chrono>
#include "util_likely.hpp"
#include "d3d9_caps.hpp"
#include "d3d9_format.hpp"
#include "d3d9_interface.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_buffer.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_volume.hpp"
#include "d3d9_query.hpp"
#include "d3d9_swapchain.hpp"
#include "d3d9_stateblock.hpp"
#include "dxmt_info.hpp"
#include "log/log.hpp"
#include "d3d11_1.h"
#include <algorithm>

namespace dxmt {

/* What D3D9's runtime refuses in a resource's pool and usage before it asks
 * the driver about the format: dynamic memory that is also a render target, a
 * depth buffer or managed; render targets and depth buffers anywhere but the
 * default pool; a render target in a depth format or a depth buffer in any
 * other; depth formats outside the default pool. */
static bool
D3D9UsageValid(DWORD Usage, D3DPOOL Pool, D3DFORMAT Format) {
  D3D9FormatInfo fi = {};
  bool depth = GetD3D9FormatInfo(Format, &fi) && fi.Depth;
  bool rt = Usage & D3DUSAGE_RENDERTARGET, ds = Usage & D3DUSAGE_DEPTHSTENCIL;
  if (Pool > D3DPOOL_SCRATCH)
    return false;
  /* Write-only is a buffer's usage; the caller rules it out for textures. */
  if ((Usage & D3DUSAGE_DYNAMIC) && (rt || ds || Pool == D3DPOOL_MANAGED))
    return false;
  if ((rt || ds) && Pool != D3DPOOL_DEFAULT)
    return false;
  if ((rt && depth) || (ds && !depth))
    return false;
  return !depth || Pool == D3DPOOL_DEFAULT;
}

static bool
D3D9LockableDepthFormat(D3DFORMAT Format) {
  return Format == D3DFMT_D16_LOCKABLE || Format == D3DFMT_D32F_LOCKABLE ||
         Format == D3DFMT_D32_LOCKABLE || Format == D3DFMT_S8_LOCKABLE;
}

namespace {

UINT PrimitiveVertexCount(D3DPRIMITIVETYPE type, UINT primCount) {
  switch (type) {
  case D3DPT_POINTLIST: return primCount;
  case D3DPT_LINELIST: return primCount * 2;
  case D3DPT_LINESTRIP: return primCount + 1;
  case D3DPT_TRIANGLELIST: return primCount * 3;
  case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return primCount + 2;
  default: return 0;
  }
}

// D3D11 has no triangle-fan topology; every other D3D9 primitive type maps
// directly.
bool ToD3D11Topology(D3DPRIMITIVETYPE type, D3D11_PRIMITIVE_TOPOLOGY *out) {
  switch (type) {
  case D3DPT_POINTLIST: *out = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; return true;
  case D3DPT_LINELIST: *out = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; return true;
  case D3DPT_LINESTRIP: *out = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; return true;
  case D3DPT_TRIANGLELIST: *out = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; return true;
  case D3DPT_TRIANGLESTRIP: *out = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; return true;
  default: return false; // D3DPT_TRIANGLEFAN: not representable in D3D11, not emulated (yet)
  }
}

} // namespace
} // namespace dxmt

#define D9_STUB(method) return E_NOTIMPL

namespace dxmt {

/* D3D9 does not store the present parameters it was handed: a zero width or
 * height means the device window's client area and a back buffer count of
 * zero means one, and the resolved values are what every later
 * GetPresentParameters reports. Leaving them at zero produces a zero viewport
 * and a swap chain whose reported size is not the one it has. */
static void
ResolvePresentParameters(D3DPRESENT_PARAMETERS *pp, HWND focus_window) {
  HWND window = pp->hDeviceWindow ? pp->hDeviceWindow : focus_window;
  if ((!pp->BackBufferWidth || !pp->BackBufferHeight) && window) {
    RECT client = {};
    if (GetClientRect(window, &client)) {
      if (!pp->BackBufferWidth)  pp->BackBufferWidth  = (UINT)(client.right - client.left);
      if (!pp->BackBufferHeight) pp->BackBufferHeight = (UINT)(client.bottom - client.top);
    }
  }
  if (!pp->BackBufferCount)
    pp->BackBufferCount = 1;
}

/* D3D9 names a sample count directly, except NONMASKABLE, whose quality level
 * picks among what CheckDeviceMultiSampleType reported -- one level, 4x. */
static UINT
D3D9SampleCount(D3DMULTISAMPLE_TYPE type) {
  if (type == D3DMULTISAMPLE_NONMASKABLE)
    return 4;
  return type >= D3DMULTISAMPLE_2_SAMPLES ? (UINT)type : 1;
}

D3D9Device::D3D9Device(
    D3D9Interface         *pParent,
    IDXGIAdapter          *pAdapter,
    HWND                   hFocusWindow,
    DWORD                  BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPP,
    bool                   isEx)
    : parent_(pParent),
      dxgi_adapter_(pAdapter),
      focus_wnd_(hFocusWindow),
      behavior_flags_(BehaviorFlags),
      multithreaded_(BehaviorFlags & D3DCREATE_MULTITHREADED),
      present_params_(*pPP),
      is_ex_(isEx) {
  ResolvePresentParameters(&present_params_, hFocusWindow);
  /* An application reads the size it actually got out of the struct it passed
   * in, so the resolved values go back to the caller. The device window is
   * not among them -- D3D9 leaves that field as it was given. */
  pPP->BackBufferWidth  = present_params_.BackBufferWidth;
  pPP->BackBufferHeight = present_params_.BackBufferHeight;
  pPP->BackBufferCount  = present_params_.BackBufferCount;

  /* A device keeps its IDirect3D9 alive: an application is allowed to release
   * the interface it created the device from and carry on using the device.
   * Held raw rather than through Com<> because the interface is what created
   * this object -- the pairing with the destructor is explicit. */
  if (parent_)
    parent_->AddRef();
}

D3D9Device::~D3D9Device() {
  if (parent_)
    parent_->Release();
}

HRESULT
D3D9Device::Initialize() {
  const D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
  D3D_FEATURE_LEVEL created_feature_level = D3D_FEATURE_LEVEL_11_0;

  HRESULT hr = D3D11CreateDevice(
      dxgi_adapter_.ptr(),
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      0,
      feature_levels, ARRAYSIZE(feature_levels),
      D3D11_SDK_VERSION,
      &d3d11_device_,
      &created_feature_level,
      &d3d11_ctx_);

  if (FAILED(hr)) {
    ERR("D3D9Device::Initialize: D3D11CreateDevice failed");
    return hr;
  }

  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
  if (SUCCEEDED(d3d11_device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
      options.ConstantBufferOffsetting && options.MapNoOverwriteOnDynamicConstantBuffer)
    d3d11_ctx_->QueryInterface(IID_PPV_ARGS(&constant_context_));

  hr = CreateSwapChain();
  if (FAILED(hr)) {
    ERR("D3D9Device::Initialize: CreateSwapChain failed");
    return hr;
  }
  // Default scissor rect covers the full backbuffer
  scissor_rect_ = { 0, 0,
    (LONG)present_params_.BackBufferWidth,
    (LONG)present_params_.BackBufferHeight };
  SetDefaultViewport();

  hr = CreateImplicitSurfaces();
  if (FAILED(hr)) {
    ERR("D3D9Device::Initialize: implicit surface creation failed");
    return hr;
  }

  InitDefaultRenderState();
  InitDefaultSamplerState();
  InitDefaultTextureStageState();
  for (auto &f : stream_freq_) f = 1;

  return S_OK;
}

HRESULT
D3D9Device::CreateSwapChain() {
  Com<IDXGIFactory1> dxgi_factory;
  if (FAILED(dxgi_adapter_->GetParent(IID_PPV_ARGS(&dxgi_factory)))) {
    ERR("D3D9Device::CreateSwapChain: GetParent(IDXGIFactory1) failed");
    return E_FAIL;
  }

  HWND hWnd = present_params_.hDeviceWindow ? present_params_.hDeviceWindow : focus_wnd_;

  /* A device on the desktop window, or on no window at all, is legal in D3D9
   * and renders as usual; there is just nothing to show the frames in. Its
   * back buffer is an ordinary texture and Present only advances the chain. */
  swap_chain_ = nullptr;
  if (!hWnd || hWnd == GetDesktopWindow()) {
    swapchain_samples_ = D3D9SampleCount(present_params_.MultiSampleType);
    return S_OK;
  }

  /* Multisampled when the application asks: the back buffer is resolved at
   * present. A single-sample swap chain here silently dropped every D3D9
   * game's anti-aliasing setting. */
  DXGI_SWAP_CHAIN_DESC scd = {};
  scd.BufferCount       = 1;
  scd.BufferDesc.Width  = present_params_.BackBufferWidth;
  scd.BufferDesc.Height = present_params_.BackBufferHeight;
  scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  scd.BufferDesc.RefreshRate.Numerator   = 0;
  scd.BufferDesc.RefreshRate.Denominator = 1;
  scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.OutputWindow      = hWnd;
  scd.SampleDesc.Count  = D3D9SampleCount(present_params_.MultiSampleType);
  scd.Windowed          = present_params_.Windowed;
  scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
  scd.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

  HRESULT hr = dxgi_factory->CreateSwapChain(d3d11_device_.ptr(), &scd, &swap_chain_);
  if (SUCCEEDED(hr))
    swapchain_samples_ = scd.SampleDesc.Count;
  return hr;
}

/* Present moves every back buffer's contents one place up the chain: buffer 0
 * receives what buffer 1 held, and the presented frame becomes the last one.
 * The application's surface pointers stay where they are. */
void
D3D9Device::RotateBackBuffers() {
  if (extra_backbuffers_.empty() || !backbuffer_)
    return;
  ID3D11Texture2D *first = backbuffer_->GetD3D11Texture();
  if (!rotate_temp_) {
    D3D11_TEXTURE2D_DESC td;
    first->GetDesc(&td);
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(d3d11_device_->CreateTexture2D(&td, nullptr, &rotate_temp_)))
      return;
  }
  d3d11_ctx_->CopyResource(rotate_temp_.ptr(), first);
  d3d11_ctx_->CopyResource(first, extra_backbuffers_[0]->GetD3D11Texture());
  for (size_t i = 0; i + 1 < extra_backbuffers_.size(); i++)
    d3d11_ctx_->CopyResource(extra_backbuffers_[i]->GetD3D11Texture(),
                             extra_backbuffers_[i + 1]->GetD3D11Texture());
  d3d11_ctx_->CopyResource(extra_backbuffers_.back()->GetD3D11Texture(), rotate_temp_.ptr());
}

void
D3D9Device::SetDefaultViewport() {
  D3DVIEWPORT9 vp = {};
  vp.Width = present_params_.BackBufferWidth;
  vp.Height = present_params_.BackBufferHeight;
  vp.MinZ = 0.0f;
  vp.MaxZ = 1.0f;
  SetViewport(&vp);
}

HRESULT
D3D9Device::CreateImplicitSurfaces() {
  /* Built before the back buffer, and kept across Reset, because it is the
   * back buffer's container: GetContainer on the implicit render target has
   * to name the implicit swap chain, not the device. */
  if (!implicit_swapchain_)
    implicit_swapchain_ = new D3D9SwapChain(this);

  // Wrap the swap chain back buffer as the default render target.
  Com<ID3D11Texture2D> bb;
  if (swap_chain_) {
    HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (FAILED(hr)) return hr;
  } else {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = present_params_.BackBufferWidth;
    td.Height = present_params_.BackBufferHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = swapchain_samples_;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | (swapchain_samples_ == 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
    if (FAILED(d3d11_device_->CreateTexture2D(&td, nullptr, &bb)))
      return E_OUTOFMEMORY;
  }

  /* The swap chain is BGRA8 either way; the surface reports the one of the
   * two D3D9 formats the application asked for. */
  D3DFORMAT bb_format = present_params_.BackBufferFormat == D3DFMT_A8R8G8B8 ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
  backbuffer_ = new D3D9Surface(this, bb.ptr(), 0, D3DPOOL_DEFAULT,
                                D3DUSAGE_RENDERTARGET, bb_format,
                                static_cast<IUnknown *>(implicit_swapchain_.ptr()));
  bool lockable_bb = present_params_.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
  backbuffer_->SetLockable(lockable_bb);
  render_targets_[0] = backbuffer_;

  extra_backbuffers_.clear();
  rotate_temp_ = nullptr;
  for (UINT i = 1; i < present_params_.BackBufferCount; i++) {
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    td.BindFlags = D3D11_BIND_RENDER_TARGET | (td.SampleDesc.Count == 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
    td.MiscFlags = 0;
    td.CPUAccessFlags = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    Com<ID3D11Texture2D> tex;
    if (FAILED(d3d11_device_->CreateTexture2D(&td, nullptr, &tex)))
      return E_OUTOFMEMORY;
    extra_backbuffers_.emplace_back(new D3D9Surface(this, tex.ptr(), 0, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET,
                                                    bb_format, static_cast<IUnknown *>(implicit_swapchain_.ptr())));
    extra_backbuffers_.back()->SetLockable(lockable_bb);
  }

  // Implicit auto depth-stencil, if requested.
  if (present_params_.EnableAutoDepthStencil) {
    D3D9FormatInfo fi;
    if (GetD3D9FormatInfo(present_params_.AutoDepthStencilFormat, &fi) && fi.Depth) {
      D3D11_TEXTURE2D_DESC td = {};
      td.Width      = present_params_.BackBufferWidth;
      td.Height     = present_params_.BackBufferHeight;
      td.MipLevels  = 1;
      td.ArraySize  = 1;
      td.Format     = fi.Resource;
      td.SampleDesc.Count = swapchain_samples_; // must match the back buffer it is paired with
      td.Usage      = D3D11_USAGE_DEFAULT;
      td.BindFlags  = D3D11_BIND_DEPTH_STENCIL;

      Com<ID3D11Texture2D> ds;
      if (SUCCEEDED(d3d11_device_->CreateTexture2D(&td, nullptr, &ds))) {
        auto_depth_ = new D3D9Surface(this, ds.ptr(), 0, D3DPOOL_DEFAULT,
                                      D3DUSAGE_DEPTHSTENCIL,
                                      present_params_.AutoDepthStencilFormat);
        auto_depth_->SetLockable(D3D9LockableDepthFormat(present_params_.AutoDepthStencilFormat));
        depth_stencil_ = auto_depth_;
      }
    }
  }

  ApplyRenderTargets();
  return D3D_OK;
}

/* D3D11 refuses a binding whose attachments disagree, and a refused call
 * leaves the previous targets bound, so every draw after it lands in the wrong
 * place. D3D9 accepts such bindings and drivers draw anyway, as wined3d does:
 * a depth buffer of another sample count, or one smaller than the target, is
 * left out and the target drawn without depth. Games bind exactly that, a
 * small non-multisampled target with the multisampled scene depth still set
 * -- Call of Duty 4 lights its models that way. A further target that does not
 * match the first is left out the same way. */
void
D3D9Device::ApplyRenderTargets() {
  ID3D11RenderTargetView *rtvs[kMaxRenderTargets] = {};
  const D3DSURFACE_DESC *first = nullptr;
  for (UINT i = 0; i < kMaxRenderTargets; i++) {
    if (!render_targets_[i])
      continue;
    const D3DSURFACE_DESC &desc = render_targets_[i]->Desc();
    if (first && (D3D9SampleCount(desc.MultiSampleType) != D3D9SampleCount(first->MultiSampleType) ||
                  desc.Width != first->Width || desc.Height != first->Height))
      continue;
    if (!first)
      first = &desc;
    rtvs[i] = render_targets_[i]->GetRTV();
  }
  ID3D11DepthStencilView *dsv = depth_stencil_ ? depth_stencil_->GetDSV() : nullptr;
  if (dsv && first) {
    const D3DSURFACE_DESC &depth = depth_stencil_->Desc();
    if (D3D9SampleCount(depth.MultiSampleType) != D3D9SampleCount(first->MultiSampleType) ||
        depth.Width < first->Width || depth.Height < first->Height)
      dsv = nullptr;
  }
  d3d11_ctx_->OMSetRenderTargets(kMaxRenderTargets, rtvs, dsv);
}

// IUnknown
HRESULT STDMETHODCALLTYPE
D3D9Device::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject) return E_POINTER;
  if (riid == __uuidof(IUnknown) ||
      riid == __uuidof(IDirect3DDevice9) ||
      (riid == __uuidof(IDirect3DDevice9Ex) && parent_ && parent_->IsExtended())) {
    *ppvObject = ref(this);
    return S_OK;
  }
  *ppvObject = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D3D9Device::AddRef()  { return ++ref_; }

/* Every object the device hands out holds a reference on it for as long as
 * its own public count is non-zero. D3D9DeviceChild takes it through these,
 * because it cannot see the device's definition. */
bool D3D9Device::ExtendedInterface() const { return parent_ && parent_->IsExtended(); }

void D3D9DeviceAddRef(D3D9Device *device)  { device->AddRef(); }
void D3D9DeviceRelease(D3D9Device *device) { device->Release(); }

ULONG STDMETHODCALLTYPE D3D9Device::Release() {
  ULONG r = --ref_;
  if (r == 0) delete this;
  return r;
}

// Core device management
HRESULT STDMETHODCALLTYPE D3D9Device::TestCooperativeLevel()     { return D3D_OK; }
UINT    STDMETHODCALLTYPE D3D9Device::GetAvailableTextureMem()   { return 0xFFFFFFFF; }
HRESULT STDMETHODCALLTYPE D3D9Device::EvictManagedResources()    { return D3D_OK; }

HRESULT STDMETHODCALLTYPE
D3D9Device::GetDirect3D(IDirect3D9 **ppD3D9) {
  auto device_lock = LockDevice();
  if (!ppD3D9) return D3DERR_INVALIDCALL;
  *ppD3D9 = ref(parent_);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetDeviceCaps(D3DCAPS9 *pCaps) {
  auto device_lock = LockDevice();
  if (!pCaps) return D3DERR_INVALIDCALL;
  FillD3D9Caps(pCaps);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) {
  auto device_lock = LockDevice();
  if (iSwapChain != 0 || !pMode) return D3DERR_INVALIDCALL;
  pMode->Width       = present_params_.BackBufferWidth;
  pMode->Height      = present_params_.BackBufferHeight;
  pMode->RefreshRate = 60;
  pMode->Format      = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParams) {
  auto device_lock = LockDevice();
  if (!pParams) return D3DERR_INVALIDCALL;
  pParams->AdapterOrdinal = D3DADAPTER_DEFAULT;
  pParams->DeviceType     = D3DDEVTYPE_HAL;
  pParams->hFocusWindow   = focus_wnd_;
  pParams->BehaviorFlags  = behavior_flags_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetCursorProperties(UINT, UINT, IDirect3DSurface9 *) { return D3D_OK; }
void    STDMETHODCALLTYPE D3D9Device::SetCursorPosition(int, int, DWORD)                    {}
BOOL    STDMETHODCALLTYPE D3D9Device::ShowCursor(BOOL)                                      { return FALSE; }

HRESULT STDMETHODCALLTYPE
D3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPP, IDirect3DSwapChain9 **ppSwapChain) {
  auto device_lock = LockDevice();
  if (!ppSwapChain) return D3DERR_INVALIDCALL;
  *ppSwapChain = nullptr;
  if (!pPP) return D3DERR_INVALIDCALL;

  /* An additional swap chain is windowed, and there are none at all while the
   * device is fullscreen -- the display belongs to the implicit chain then. */
  if (!pPP->Windowed || !present_params_.Windowed)
    return D3DERR_INVALIDCALL;

  D3DPRESENT_PARAMETERS pp = *pPP;
  ResolvePresentParameters(&pp, focus_wnd_);
  pPP->BackBufferWidth  = pp.BackBufferWidth;
  pPP->BackBufferHeight = pp.BackBufferHeight;
  pPP->BackBufferCount  = pp.BackBufferCount;
  /* The swap chain remembers the window it resolved to; the caller's struct
   * keeps whatever it was given. */
  pp.hDeviceWindow = pp.hDeviceWindow ? pp.hDeviceWindow : focus_wnd_;

  Com<IDXGIFactory1> dxgi_factory;
  if (FAILED(dxgi_adapter_->GetParent(IID_PPV_ARGS(&dxgi_factory))))
    return D3DERR_DRIVERINTERNALERROR;

  DXGI_SWAP_CHAIN_DESC scd = {};
  scd.BufferCount       = 1;
  scd.BufferDesc.Width  = pp.BackBufferWidth;
  scd.BufferDesc.Height = pp.BackBufferHeight;
  scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  scd.BufferDesc.RefreshRate.Denominator = 1;
  scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.OutputWindow      = pp.hDeviceWindow;
  scd.SampleDesc.Count  = 1;
  scd.Windowed          = pp.Windowed;
  scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

  Com<IDXGISwapChain> sc;
  if (FAILED(dxgi_factory->CreateSwapChain(d3d11_device_.ptr(), &scd, &sc)))
    return D3DERR_INVALIDCALL;

  Com<ID3D11Texture2D> bb;
  if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))))
    return D3DERR_DRIVERINTERNALERROR;

  /* The swap chain first: it is the back buffer's container, and a back
   * buffer whose GetContainer named the device would be the implicit one. */
  auto *swapchain = new D3D9SwapChain(this, std::move(sc), nullptr, pp);
  auto *backbuffer = new D3D9Surface(
      this, bb.ptr(), 0, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
      static_cast<IUnknown *>(swapchain));
  backbuffer->SetLockable(pp.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER);
  swapchain->SetBackBuffer(backbuffer);
  *ppSwapChain = ref(swapchain);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **ppSwapChain) {
  auto device_lock = LockDevice();
  if (!ppSwapChain) return D3DERR_INVALIDCALL;
  /* Cleared before anything else can fail: an application that gets back
   * D3DERR_INVALIDCALL still releases whatever the pointer holds, so leaving
   * it untouched turns a rejected call into a crash. */
  *ppSwapChain = nullptr;
  if (iSwapChain != 0 || !implicit_swapchain_) return D3DERR_INVALIDCALL;
  *ppSwapChain = ref(implicit_swapchain_.ptr());
  return D3D_OK;
}
UINT    STDMETHODCALLTYPE D3D9Device::GetNumberOfSwapChains()                               { return 1; }

HRESULT STDMETHODCALLTYPE
D3D9Device::Reset(D3DPRESENT_PARAMETERS *pPP) {
  auto device_lock = LockDevice();
  if (!pPP) return D3DERR_INVALIDCALL;
  ResolvePresentParameters(pPP, focus_wnd_);

  // Every reference to the old swapchain buffers must be released before
  // ResizeBuffers, including whatever the app still has bound as a render
  // target/depth-stencil.
  for (auto &rt : render_targets_) rt = nullptr;
  depth_stencil_ = nullptr;
  if (backbuffer_) backbuffer_->DetachFromContainer();
  for (auto &bb : extra_backbuffers_) bb->DetachFromContainer();
  backbuffer_ = nullptr;
  auto_depth_ = nullptr;
  extra_backbuffers_.clear();
  rotate_temp_ = nullptr;
  d3d11_ctx_->OMSetRenderTargets(0, nullptr, nullptr);
  input_layout_cache_.clear();

  present_params_ = *pPP;

  UINT width = present_params_.BackBufferWidth;
  UINT height = present_params_.BackBufferHeight;
  HRESULT hr;
  /* ResizeBuffers keeps the sample count, so a Reset that changes the
   * multisampling needs a new swap chain. */
  if (!swap_chain_ || D3D9SampleCount(present_params_.MultiSampleType) != swapchain_samples_)
    hr = CreateSwapChain();
  else
    hr = swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr)) {
    ERR("D3D9Device::Reset: swap chain resize failed");
    return D3DERR_INVALIDCALL;
  }

  scissor_rect_ = {0, 0, (LONG)width, (LONG)height};
  SetDefaultViewport();

  hr = CreateImplicitSurfaces();
  if (FAILED(hr)) {
    ERR("D3D9Device::Reset: implicit surface creation failed");
    return D3DERR_INVALIDCALL;
  }

  recording_ = nullptr;
  /* D3D9 resets the device state across Reset(); D3D9Ex keeps it -- Wine's
   * d3d9ex test_backbuffer_resize draws straight after a Reset with the FVF
   * it set before. */
  if (ExtendedInterface())
    return D3D_OK;
  InitDefaultRenderState();
  InitDefaultSamplerState();
  InitDefaultTextureStageState();
  vertex_shader_ = nullptr;
  pixel_shader_ = nullptr;
  vertex_decl_ = nullptr;
  fvf_ = 0;
  for (auto &s : streams_) s = {};
  for (auto &f : stream_freq_) f = 1;
  index_buffer_ = nullptr;
  for (auto &t : textures_) t.Set(nullptr);

  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::Present(const RECT *, const RECT *, HWND, const RGNDATA *) {
  auto device_lock = LockDevice();
  /* DXMT_FPS=1 reports the frame rate and the draw calls behind it. Both
   * numbers are needed together: this game presents at the display's rate
   * with zero draws while it sits on a video or waits for input, and a frame
   * rate alone would read that as healthy performance. */
  if (fps_report_) {
    static uint64_t frames = 0;
    static auto t0 = std::chrono::steady_clock::now();
    if (++frames % 60 == 0) {
      auto now = std::chrono::steady_clock::now();
      double sec = std::chrono::duration<double>(now - t0).count();
      Logger::info(str::format("fps ", 60.0 / sec, " over 60 frames, draws/frame ",
                               draws_since_report_ / 60,
                               ", prepare ", 1000.0 * diag_prepare_s_ / 60, " ms/frame",
                               ", upload ", 1000.0 * diag_upload_s_ / 60, " ms/frame",
                               ", bind ", 1000.0 * diag_bind_s_ / 60, " ms/frame",
                               ", present ", 1000.0 * diag_present_s_ / 60, " ms/frame",
                               ", locks/frame ", locks_since_report_ / 60,
                               ", lock ", 1000.0 * diag_lock_s_ / 60, " ms/frame"));
      draws_since_report_ = 0;
      locks_since_report_ = 0;
      diag_prepare_s_ = diag_upload_s_ = diag_bind_s_ = diag_present_s_ = diag_lock_s_ = 0;
      t0 = now;
    }
  }

  /* The application's requested presentation interval, not a hard-coded one.
   * D3D9 spells "as fast as possible" as D3DPRESENT_INTERVAL_IMMEDIATE, and
   * forcing 1 here capped every game at the display's refresh rate however
   * little work it had to do. */
  UINT sync = 1;
  switch (present_params_.PresentationInterval) {
  case D3DPRESENT_INTERVAL_IMMEDIATE: sync = 0; break;
  case D3DPRESENT_INTERVAL_TWO:       sync = 2; break;
  case D3DPRESENT_INTERVAL_THREE:     sync = 3; break;
  case D3DPRESENT_INTERVAL_FOUR:      sync = 4; break;
  default:                            sync = 1; break;
  }

  DXGI_PRESENT_PARAMETERS pp = {};
  Com<IDXGISwapChain1> sc1;
  auto present_t0 = fps_report_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  if (swap_chain_) { // none when there is no window to show the frame in
    if (SUCCEEDED(swap_chain_->QueryInterface(IID_PPV_ARGS(&sc1))))
      sc1->Present1(sync, 0, &pp);
    else
      swap_chain_->Present(sync, 0);
  }
  RotateBackBuffers();
  if (fps_report_)
    diag_present_s_ += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - present_t0).count();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppBackBuffer) {
  auto device_lock = LockDevice();
  if (!ppBackBuffer) return D3DERR_INVALIDCALL;
  *ppBackBuffer = nullptr;
  if (iSwapChain != 0 || !backbuffer_ || iBackBuffer > extra_backbuffers_.size())
    return D3DERR_INVALIDCALL;
  *ppBackBuffer = ref(iBackBuffer ? extra_backbuffers_[iBackBuffer - 1].ptr() : backbuffer_.ptr());
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::GetRasterStatus(UINT, D3DRASTER_STATUS *pRS) {
  auto device_lock = LockDevice();
  if (pRS) { pRS->InVBlank = FALSE; pRS->ScanLine = 0; }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::SetDialogBoxMode(BOOL)          { return D3D_OK; }
void    STDMETHODCALLTYPE D3D9Device::SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) {}
void    STDMETHODCALLTYPE D3D9Device::GetGammaRamp(UINT, D3DGAMMARAMP *pRamp) { if (pRamp) memset(pRamp, 0, sizeof(*pRamp)); }

// Resource creation stubs — implemented in later phases
HRESULT STDMETHODCALLTYPE D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *) {
  auto device_lock = LockDevice();
  if (!ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  if (Width == 0 || Height == 0) return D3DERR_INVALIDCALL;
  if ((Usage & D3DUSAGE_WRITEONLY) || !D3D9UsageValid(Usage, Pool, Format)) return D3DERR_INVALIDCALL;
  /* A DXTn top level is whole blocks or nothing, on every Windows driver. */
  if (D3D9FormatIsDxt(Format) && ((Width | Height) & 3)) return D3DERR_INVALIDCALL;

  auto *texture = new D3D9Texture(this, Width, Height, Levels, Usage, Format, Pool);
  if (!texture->IsValid()) { delete texture; return D3DERR_INVALIDCALL; }
  *ppTexture = ref(texture);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *) {
  auto device_lock = LockDevice();
  if (!ppVolumeTexture) return D3DERR_INVALIDCALL;
  *ppVolumeTexture = nullptr;
  if (Width == 0 || Height == 0 || Depth == 0) return D3DERR_INVALIDCALL;
  /* A volume is never rendered to nor a depth buffer. */
  if ((Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_WRITEONLY)) ||
      !D3D9UsageValid(Usage, Pool, Format))
    return D3DERR_INVALIDCALL;
  if (D3D9FormatIsDxt(Format) && ((Width | Height) & 3)) return D3DERR_INVALIDCALL;
  /* D3DPTEXTURECAPS_VOLUMEMAP_POW2, as Windows drivers report it, and the
   * runtime holds them to it for everything but scratch memory. */
  auto pow2 = [](UINT v) { return !(v & (v - 1)); };
  if (Pool != D3DPOOL_SCRATCH && !(pow2(Width) && pow2(Height) && pow2(Depth))) return D3DERR_INVALIDCALL;

  auto *texture = new D3D9VolumeTexture(this, Width, Height, Depth, Levels, Usage, Format, Pool);
  if (!texture->IsValid()) { delete texture; return D3DERR_INVALIDCALL; }
  *ppVolumeTexture = ref(texture);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *) {
  auto device_lock = LockDevice();
  if (!ppCubeTexture) return D3DERR_INVALIDCALL;
  *ppCubeTexture = nullptr;
  if (EdgeLength == 0) return D3DERR_INVALIDCALL;
  /* No depth cube maps, as CheckDeviceFormat says. */
  D3D9FormatInfo cube_fi = {};
  if ((Usage & D3DUSAGE_WRITEONLY) || !D3D9UsageValid(Usage, Pool, Format) ||
      (GetD3D9FormatInfo(Format, &cube_fi) && cube_fi.Depth))
    return D3DERR_INVALIDCALL;
  if (D3D9FormatIsDxt(Format) && (EdgeLength & 3)) return D3DERR_INVALIDCALL;
  /* D3DPTEXTURECAPS_CUBEMAP_POW2, likewise. */
  if (Pool != D3DPOOL_SCRATCH && (EdgeLength & (EdgeLength - 1))) return D3DERR_INVALIDCALL;

  auto *texture = new D3D9CubeTexture(this, EdgeLength, Levels, Usage, Format, Pool);
  if (!texture->IsValid()) { delete texture; return D3DERR_INVALIDCALL; }
  *ppCubeTexture = ref(texture);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *) {
  auto device_lock = LockDevice();
  if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
  *ppVertexBuffer = nullptr;
  if (Length == 0) return D3DERR_INVALIDCALL;
  /* Buffers have no scratch memory, and are neither render targets nor depth
   * buffers. */
  if (Pool == D3DPOOL_SCRATCH || (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) ||
      !D3D9UsageValid(Usage, Pool, D3DFMT_VERTEXDATA))
    return D3DERR_INVALIDCALL;

  D3DVERTEXBUFFER_DESC desc = {};
  desc.Format = D3DFMT_VERTEXDATA;
  desc.Type   = D3DRTYPE_VERTEXBUFFER;
  desc.Usage  = Usage;
  desc.Pool   = Pool;
  desc.Size   = Length;
  desc.FVF    = FVF;

  auto *buffer = new D3D9VertexBuffer(this, Length, Usage, Pool, desc);
  if (!buffer->GetD3D11Buffer()) { delete buffer; return D3DERR_OUTOFVIDEOMEMORY; }
  *ppVertexBuffer = ref(buffer);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *) {
  auto device_lock = LockDevice();
  if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
  *ppIndexBuffer = nullptr;
  if (Length == 0) return D3DERR_INVALIDCALL;
  if (Pool == D3DPOOL_SCRATCH || (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) ||
      !D3D9UsageValid(Usage, Pool, D3DFMT_INDEX16))
    return D3DERR_INVALIDCALL;
  if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32) return D3DERR_INVALIDCALL;

  D3DINDEXBUFFER_DESC desc = {};
  desc.Format = Format;
  desc.Type   = D3DRTYPE_INDEXBUFFER;
  desc.Usage  = Usage;
  desc.Pool   = Pool;
  desc.Size   = Length;

  auto *buffer = new D3D9IndexBuffer(this, Length, Usage, Pool, desc);
  if (!buffer->GetD3D11Buffer()) { delete buffer; return D3DERR_OUTOFVIDEOMEMORY; }
  *ppIndexBuffer = ref(buffer);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *) {
  auto device_lock = LockDevice();
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;

  D3D9FormatInfo fi;
  if (!GetD3D9FormatInfo(Format, &fi) || fi.Depth) return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width      = Width;
  td.Height     = Height;
  td.MipLevels  = 1;
  td.ArraySize  = 1;
  td.Format     = fi.Resource;
  td.SampleDesc.Count   = MultiSample ? (UINT)MultiSample : 1;
  td.SampleDesc.Quality = MultisampleQuality;
  td.Usage      = D3D11_USAGE_DEFAULT;
  td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  Com<ID3D11Texture2D> tex;
  if (FAILED(d3d11_device_->CreateTexture2D(&td, nullptr, &tex))) return D3DERR_INVALIDCALL;

  auto *surface = new D3D9Surface(this, tex.ptr(), 0, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET, Format);
  surface->SetLockable(Lockable);
  *ppSurface = ref(surface);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL /*Discard*/, IDirect3DSurface9 **ppSurface, HANDLE *) {
  auto device_lock = LockDevice();
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;

  D3D9FormatInfo fi;
  if (!GetD3D9FormatInfo(Format, &fi) || !fi.Depth) return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width      = Width;
  td.Height     = Height;
  td.MipLevels  = 1;
  td.ArraySize  = 1;
  td.Format     = fi.Resource;
  td.SampleDesc.Count   = MultiSample ? (UINT)MultiSample : 1;
  td.SampleDesc.Quality = MultisampleQuality;
  td.Usage      = D3D11_USAGE_DEFAULT;
  td.BindFlags  = D3D11_BIND_DEPTH_STENCIL;

  Com<ID3D11Texture2D> tex;
  if (FAILED(d3d11_device_->CreateTexture2D(&td, nullptr, &tex))) return D3DERR_INVALIDCALL;

  auto *surface = new D3D9Surface(this, tex.ptr(), 0, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL, Format);
  surface->SetLockable(D3D9LockableDepthFormat(Format));
  *ppSurface = ref(surface);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::UpdateSurface(IDirect3DSurface9 *pSrc, const RECT *pSrcRect, IDirect3DSurface9 *pDst, const POINT *pDstPoint) {
  auto device_lock = LockDevice();
  if (!pSrc || !pDst) return D3DERR_INVALIDCALL;
  auto src = static_cast<D3D9Surface *>(pSrc);
  auto dst = static_cast<D3D9Surface *>(pDst);
  if (!src->GetD3D11Texture() || !dst->GetD3D11Texture()) return D3DERR_INVALIDCALL;
  /* One format on both sides, neither multisampled, and the rectangle inside
   * both surfaces -- a copy D3D11 does not check runs off the end of the
   * destination on the GPU. */
  const auto &sd = src->Desc(), &dd = dst->Desc();
  if (sd.Format != dd.Format || sd.MultiSampleType != D3DMULTISAMPLE_NONE ||
      dd.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;
  RECT copy = pSrcRect ? *pSrcRect : RECT{0, 0, (LONG)sd.Width, (LONG)sd.Height};
  LONG px = pDstPoint ? pDstPoint->x : 0, py = pDstPoint ? pDstPoint->y : 0;
  if (copy.left < 0 || copy.top < 0 || copy.left >= copy.right || copy.top >= copy.bottom ||
      (UINT)copy.right > sd.Width || (UINT)copy.bottom > sd.Height || px < 0 || py < 0 ||
      (UINT)(px + copy.right - copy.left) > dd.Width || (UINT)(py + copy.bottom - copy.top) > dd.Height)
    return D3DERR_INVALIDCALL;

  UINT dstX = pDstPoint ? (UINT)pDstPoint->x : 0;
  UINT dstY = pDstPoint ? (UINT)pDstPoint->y : 0;

  if (src->UploadTo(dst, copy, POINT{px, py})) {
    dst->MarkGpuWritten();
    return D3D_OK;
  }

  if (pSrcRect) {
    D3D11_BOX box;
    box.left = pSrcRect->left; box.top = pSrcRect->top; box.front = 0;
    box.right = pSrcRect->right; box.bottom = pSrcRect->bottom; box.back = 1;
    d3d11_ctx_->CopySubresourceRegion(
        dst->GetD3D11Texture(), dst->GetSubresource(), dstX, dstY, 0,
        src->GetD3D11Texture(), src->GetSubresource(), &box);
  } else {
    d3d11_ctx_->CopySubresourceRegion(
        dst->GetD3D11Texture(), dst->GetSubresource(), dstX, dstY, 0,
        src->GetD3D11Texture(), src->GetSubresource(), nullptr);
  }
  dst->MarkGpuWritten();
  return D3D_OK;
}

static ID3D11Resource *
GetD3D9BaseTextureResource(IDirect3DBaseTexture9 *tex) {
  switch (tex->GetType()) {
    case D3DRTYPE_TEXTURE:       return static_cast<D3D9Texture *>(tex)->GetD3D11Texture();
    case D3DRTYPE_CUBETEXTURE:   return static_cast<D3D9CubeTexture *>(tex)->GetD3D11Texture();
    case D3DRTYPE_VOLUMETEXTURE: return static_cast<D3D9VolumeTexture *>(tex)->GetD3D11Texture();
    default:                     return nullptr;
  }
}

HRESULT STDMETHODCALLTYPE
D3D9Device::UpdateTexture(IDirect3DBaseTexture9 *pSrc, IDirect3DBaseTexture9 *pDst) {
  auto device_lock = LockDevice();
  if (!pSrc || !pDst || pSrc->GetType() != pDst->GetType()) return D3DERR_INVALIDCALL;

  auto srcRes = GetD3D9BaseTextureResource(pSrc);
  auto dstRes = GetD3D9BaseTextureResource(pDst);
  if (!srcRes || !dstRes) return D3DERR_INVALIDCALL;

  // Mip chains may differ in length; align on the most-detailed (largest) end
  // and copy however many levels both textures have in common.
  UINT srcLevels = pSrc->GetLevelCount();
  UINT dstLevels = pDst->GetLevelCount();
  UINT levels = std::min(srcLevels, dstLevels);
  UINT sliceCount = pDst->GetType() == D3DRTYPE_CUBETEXTURE ? 6 : 1;

  for (UINT slice = 0; slice < sliceCount; slice++) {
    for (UINT level = 0; level < levels; level++) {
      bool uploaded = false;
      if (pSrc->GetType() == D3DRTYPE_VOLUMETEXTURE) {
        Com<IDirect3DVolume9> source, destination;
        if (SUCCEEDED(static_cast<D3D9VolumeTexture *>(pSrc)->GetVolumeLevel(level, &source)) &&
            SUCCEEDED(static_cast<D3D9VolumeTexture *>(pDst)->GetVolumeLevel(level, &destination)))
          uploaded = static_cast<D3D9Volume *>(source.ptr())->UploadTo(
              static_cast<D3D9Volume *>(destination.ptr()));
      } else {
        Com<IDirect3DSurface9> source, destination;
        HRESULT hr = pSrc->GetType() == D3DRTYPE_CUBETEXTURE
            ? static_cast<D3D9CubeTexture *>(pSrc)->GetCubeMapSurface(
                  static_cast<D3DCUBEMAP_FACES>(slice), level, &source)
            : static_cast<D3D9Texture *>(pSrc)->GetSurfaceLevel(level, &source);
        HRESULT dstHr = pDst->GetType() == D3DRTYPE_CUBETEXTURE
            ? static_cast<D3D9CubeTexture *>(pDst)->GetCubeMapSurface(
                  static_cast<D3DCUBEMAP_FACES>(slice), level, &destination)
            : static_cast<D3D9Texture *>(pDst)->GetSurfaceLevel(level, &destination);
        if (SUCCEEDED(hr) && SUCCEEDED(dstHr)) {
          auto src = static_cast<D3D9Surface *>(source.ptr());
          RECT rect = {0, 0, static_cast<LONG>(src->Desc().Width), static_cast<LONG>(src->Desc().Height)};
          uploaded = src->UploadTo(static_cast<D3D9Surface *>(destination.ptr()), rect, POINT{0, 0});
        }
      }
      if (uploaded)
        continue;
      UINT srcSub = level + slice * srcLevels;
      UINT dstSub = level + slice * dstLevels;
      d3d11_ctx_->CopySubresourceRegion(dstRes, dstSub, 0, 0, 0, srcRes, srcSub, nullptr);
    }
  }
  switch (pDst->GetType()) {
    case D3DRTYPE_TEXTURE:       static_cast<D3D9Texture *>(pDst)->MarkGpuWritten(); break;
    case D3DRTYPE_CUBETEXTURE:   static_cast<D3D9CubeTexture *>(pDst)->MarkGpuWritten(); break;
    case D3DRTYPE_VOLUMETEXTURE: static_cast<D3D9VolumeTexture *>(pDst)->MarkGpuWritten(); break;
    default: break;
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetRenderTargetData(IDirect3DSurface9 *pRT, IDirect3DSurface9 *pDst) {
  auto device_lock = LockDevice();
  if (!pRT || !pDst) return D3DERR_INVALIDCALL;
  auto src = static_cast<D3D9Surface *>(pRT);
  auto dst = static_cast<D3D9Surface *>(pDst);
  if (!src->GetD3D11Texture() || !dst->GetD3D11Texture()) return D3DERR_INVALIDCALL;
  d3d11_ctx_->CopySubresourceRegion(
      dst->GetD3D11Texture(), dst->GetSubresource(), 0, 0, 0,
      src->GetD3D11Texture(), src->GetSubresource(), nullptr);
  dst->MarkGpuWritten();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDst) {
  auto device_lock = LockDevice();
  // The closest thing to the front buffer we can read back is the current
  // back buffer contents (they match right after Present).
  if (iSwapChain != 0 || !pDst || !backbuffer_) return D3DERR_INVALIDCALL;
  auto dst = static_cast<D3D9Surface *>(pDst);
  if (!dst->GetD3D11Texture()) return D3DERR_INVALIDCALL;
  d3d11_ctx_->CopySubresourceRegion(
      dst->GetD3D11Texture(), dst->GetSubresource(), 0, 0, 0,
      backbuffer_->GetD3D11Texture(), backbuffer_->GetSubresource(), nullptr);
  dst->MarkGpuWritten();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::StretchRect(IDirect3DSurface9 *pSrc, const RECT *pSrcRect, IDirect3DSurface9 *pDst, const RECT *pDstRect, D3DTEXTUREFILTERTYPE Filter) {
  auto device_lock = LockDevice();
  if (!pSrc || !pDst) return D3DERR_INVALIDCALL;
  auto src = static_cast<D3D9Surface *>(pSrc);
  auto dst = static_cast<D3D9Surface *>(pDst);
  if (!src->GetD3D11Texture() || !dst->GetD3D11Texture()) return D3DERR_INVALIDCALL;
  const auto &srcDesc = src->Desc();
  const auto &dstDesc = dst->Desc();
  dst->MarkGpuWritten();

  RECT sr = pSrcRect ? *pSrcRect : RECT{0, 0, (LONG)srcDesc.Width, (LONG)srcDesc.Height};
  RECT dr = pDstRect ? *pDstRect : RECT{0, 0, (LONG)dstDesc.Width, (LONG)dstDesc.Height};
  bool sameSize = (sr.right - sr.left) == (dr.right - dr.left) &&
                  (sr.bottom - sr.top) == (dr.bottom - dr.top);
  bool sameFormat = srcDesc.Format == dstDesc.Format;

  bool src_ms = srcDesc.MultiSampleType != D3DMULTISAMPLE_NONE;
  bool dst_ms = dstDesc.MultiSampleType != D3DMULTISAMPLE_NONE;
  bool whole = sr.left == 0 && sr.top == 0 && (UINT)sr.right == srcDesc.Width && (UINT)sr.bottom == srcDesc.Height &&
               dr.left == 0 && dr.top == 0 && (UINT)dr.right == dstDesc.Width && (UINT)dr.bottom == dstDesc.Height;
  if (sr.left < 0 || sr.top < 0 || sr.left >= sr.right || sr.top >= sr.bottom || (UINT)sr.right > srcDesc.Width ||
      (UINT)sr.bottom > srcDesc.Height || dr.left < 0 || dr.top < 0 || dr.left >= dr.right || dr.top >= dr.bottom ||
      (UINT)dr.right > dstDesc.Width || (UINT)dr.bottom > dstDesc.Height)
    return D3DERR_INVALIDCALL;

  D3D9FormatInfo src_fi = {};
  GetD3D9FormatInfo(srcDesc.Format, &src_fi);

  /* A multisampled source is resolved: straight into the destination when
   * that is the whole of a single-sample surface of the same format, which
   * is all a resolve can do; into a single-sample copy otherwise, which the
   * rest then reads. A destination of the same sample count takes a plain
   * copy of the whole. */
  ID3D11Texture2D *source = src->GetD3D11Texture();
  UINT source_sub = src->GetSubresource();
  ID3D11ShaderResourceView *source_srv = nullptr;
  if (src_ms) {
    if (!dst_ms && sameFormat && whole) {
      d3d11_ctx_->ResolveSubresource(dst->GetD3D11Texture(), dst->GetSubresource(), source, source_sub,
                                     src_fi.Resource);
      return D3D_OK;
    }
    if (dst_ms && sameFormat && whole && srcDesc.MultiSampleType == dstDesc.MultiSampleType) {
      d3d11_ctx_->CopySubresourceRegion(dst->GetD3D11Texture(), dst->GetSubresource(), 0, 0, 0, source, source_sub,
                                        nullptr);
      return D3D_OK;
    }
    if (!(source = ResolveToScratch(src, &source_srv)))
      return D3DERR_INVALIDCALL;
    source_sub = 0;
  } else {
    source_srv = src->GetSRV();
  }

  /* Same size, same format, single-sample on both ends: a copy. */
  if (sameSize && sameFormat && !dst_ms) {
    D3D11_BOX box;
    box.left = sr.left; box.top = sr.top; box.front = 0;
    box.right = sr.right; box.bottom = sr.bottom; box.back = 1;
    d3d11_ctx_->CopySubresourceRegion(
        dst->GetD3D11Texture(), dst->GetSubresource(), dr.left, dr.top, 0, source, source_sub, &box);
    return D3D_OK;
  }

  // Scaling, a format conversion or a multisampled destination: a draw.
  if (!source_srv)
    return D3DERR_INVALIDCALL;
  return BlitSurface(source_srv, srcDesc.Width, srcDesc.Height, sr, dst, dr, Filter == D3DTEXF_LINEAR);
}

ID3D11Texture2D *
D3D9Device::ResolveToScratch(D3D9Surface *src, ID3D11ShaderResourceView **srv) {
  const auto &desc = src->Desc();
  D3D9FormatInfo fi = {};
  if (!GetD3D9FormatInfo(desc.Format, &fi) || fi.Depth)
    return nullptr;
  D3D11_TEXTURE2D_DESC td = {};
  if (resolve_scratch_)
    resolve_scratch_->GetDesc(&td);
  if (!resolve_scratch_ || td.Width != desc.Width || td.Height != desc.Height || td.Format != fi.Resource) {
    resolve_scratch_ = nullptr;
    resolve_scratch_srv_ = nullptr;
    td = {};
    td.Width = desc.Width;
    td.Height = desc.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fi.Resource;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d11_device_->CreateTexture2D(&td, nullptr, &resolve_scratch_)))
      return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = fi.Resource;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(d3d11_device_->CreateShaderResourceView(resolve_scratch_.ptr(), &sd, &resolve_scratch_srv_))) {
      resolve_scratch_ = nullptr;
      return nullptr;
    }
  }
  d3d11_ctx_->ResolveSubresource(resolve_scratch_.ptr(), 0, src->GetD3D11Texture(), src->GetSubresource(),
                                 fi.Resource);
  *srv = resolve_scratch_srv_.ptr();
  return resolve_scratch_.ptr();
}

bool
D3D9Device::EnsureBlitObjects() {
  if (blit_vs_)
    return true;
  using namespace sm4;

  // VS: POSITION (clip-space xy) + TEXCOORD passthrough.
  {
    Sm4Builder b(Sm4Builder::Stage::Vertex);
    b.DeclInput(0, 0xf, "POSITION", 0);
    b.DeclInput(1, 0xf, "TEXCOORD", 0);
    b.DeclOutputPosition(0);
    b.DeclIOWithSemantic(1, 0xf, "TEXCOORD", 0);
    b.Mov(WithMask(Output(0), 0b0011), Input(0));
    b.Mov(WithMask(Output(0), 0b1100), Imm4(0, 0, 0, 1));
    b.Mov(Output(1), Input(1));
    b.Ret();
    std::vector<uint8_t> dxbc;
    if (!b.Finish(dxbc) ||
        FAILED(d3d11_device_->CreateVertexShader(dxbc.data(), dxbc.size(), nullptr, &blit_vs_)))
      return false;

    D3D11_INPUT_ELEMENT_DESC il[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(d3d11_device_->CreateInputLayout(il, 2, dxbc.data(), dxbc.size(), &blit_layout_)))
      return false;
  }
  // PS: sample t0 at the interpolated coordinate.
  {
    Sm4Builder b(Sm4Builder::Stage::Pixel);
    b.DeclInputPositionPS(0);
    b.DeclIOWithSemantic(1, 0xf, "TEXCOORD", 0);
    b.DeclResourceTexture2D(0);
    b.DeclSampler(0);
    b.DeclOutputTarget(0, 0);
    b.Sample(Output(0), Input(1), 0, 0);
    b.Ret();
    std::vector<uint8_t> dxbc;
    if (!b.Finish(dxbc) ||
        FAILED(d3d11_device_->CreatePixelShader(dxbc.data(), dxbc.size(), nullptr, &blit_ps_)))
      return false;
  }

  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = 4 * 4 * sizeof(float); // 4 verts x (pos.xy, uv.xy)
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(d3d11_device_->CreateBuffer(&bd, nullptr, &blit_vb_)))
    return false;

  D3D11_SAMPLER_DESC sd = {};
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  if (FAILED(d3d11_device_->CreateSamplerState(&sd, &blit_sampler_point_)))
    return false;
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  if (FAILED(d3d11_device_->CreateSamplerState(&sd, &blit_sampler_linear_)))
    return false;

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(d3d11_device_->CreateRasterizerState(&rd, &blit_rasterizer_)))
    return false;
  return true;
}

HRESULT
D3D9Device::BlitSurface(ID3D11ShaderResourceView *srv, UINT src_width, UINT src_height, const RECT &sr,
                        D3D9Surface *dst, const RECT &dr, bool linear) {
  auto rtv = dst->GetRTV();
  if (!srv || !rtv || !EnsureBlitObjects())
    return D3DERR_INVALIDCALL;

  const auto &dstDesc = dst->Desc();

  float u0 = (float)sr.left / src_width, u1 = (float)sr.right / src_width;
  float v0 = (float)sr.top / src_height, v1 = (float)sr.bottom / src_height;
  float x0 = (float)dr.left / dstDesc.Width * 2.0f - 1.0f;
  float x1 = (float)dr.right / dstDesc.Width * 2.0f - 1.0f;
  float y0 = 1.0f - (float)dr.top / dstDesc.Height * 2.0f;
  float y1 = 1.0f - (float)dr.bottom / dstDesc.Height * 2.0f;
  float verts[16] = {
    x0, y0, u0, v0,
    x1, y0, u1, v0,
    x0, y1, u0, v1,
    x1, y1, u1, v1,
  };
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (FAILED(d3d11_ctx_->Map(blit_vb_.ptr(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    return D3DERR_DRIVERINTERNALERROR;
  memcpy(m.pData, verts, sizeof(verts));
  d3d11_ctx_->Unmap(blit_vb_.ptr(), 0);

  // The app-visible viewport/scissor must survive; everything else the next
  // DrawPrepare() rebinds from scratch anyway.
  UINT n_vp = 1;
  D3D11_VIEWPORT saved_vp = {};
  d3d11_ctx_->RSGetViewports(&n_vp, &saved_vp);

  D3D11_VIEWPORT vp = {0, 0, (float)dstDesc.Width, (float)dstDesc.Height, 0, 1};
  d3d11_ctx_->RSSetViewports(1, &vp);
  d3d11_ctx_->RSSetState(blit_rasterizer_.ptr());
  d3d11_ctx_->OMSetRenderTargets(1, &rtv, nullptr);
  d3d11_ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  d3d11_ctx_->OMSetDepthStencilState(nullptr, 0);
  d3d11_ctx_->IASetInputLayout(blit_layout_.ptr());
  ID3D11Buffer *vb = blit_vb_.ptr();
  UINT stride = 16, offset = 0;
  d3d11_ctx_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  d3d11_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  d3d11_ctx_->VSSetShader(blit_vs_.ptr(), nullptr, 0);
  d3d11_ctx_->PSSetShader(blit_ps_.ptr(), nullptr, 0);
  d3d11_ctx_->PSSetShaderResources(0, 1, &srv);
  ID3D11SamplerState *sampler = linear ? blit_sampler_linear_.ptr() : blit_sampler_point_.ptr();
  d3d11_ctx_->PSSetSamplers(0, 1, &sampler);
  d3d11_ctx_->Draw(4, 0);

  // Restore app-visible state; mark cached state dirty so the next draw
  // re-applies the D3D9-derived objects this blit replaced.
  ID3D11ShaderResourceView *null_srv = nullptr;
  d3d11_ctx_->PSSetShaderResources(0, 1, &null_srv);
  d3d11_ctx_->RSSetViewports(1, &saved_vp);
  ApplyRenderTargets();
  render_state_dirty_ = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR Color) {
  auto device_lock = LockDevice();
  if (!pSurface) return D3DERR_INVALIDCALL;
  auto surface = static_cast<D3D9Surface *>(pSurface);
  auto rtv = surface->GetRTV();
  if (!rtv) return D3DERR_INVALIDCALL;
  surface->MarkGpuWritten();

  float c[4] = {
    ((Color >> 16) & 0xFF) / 255.0f,
    ((Color >>  8) & 0xFF) / 255.0f,
    ((Color >>  0) & 0xFF) / 255.0f,
    ((Color >> 24) & 0xFF) / 255.0f,
  };

  if (pRect) {
    Com<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(d3d11_ctx_->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
      D3D11_RECT r = { pRect->left, pRect->top, pRect->right, pRect->bottom };
      ctx1->ClearView(rtv, c, &r, 1);
      return D3D_OK;
    }
  }
  d3d11_ctx_->ClearRenderTargetView(rtv, c);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *) {
  auto device_lock = LockDevice();
  if (!ppSurface) return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;

  if (!D3D9UsageValid(0, Pool, Format)) return D3DERR_INVALIDCALL;
  /* A scratch surface is memory only; see D3D9Surface. */
  if (Pool == D3DPOOL_SCRATCH) {
    D3D9LockLayout layout;
    if (!Width || !Height || !GetD3D9LockLayout(Format, Width, Height, 1, &layout) ||
        (D3D9FormatIsDxt(Format) && ((Width | Height) & 3)))
      return D3DERR_INVALIDCALL;
    *ppSurface = ref(new D3D9Surface(this, Width, Height, Pool, 0, Format));
    return D3D_OK;
  }

  D3D9FormatInfo fi;
  if (!GetD3D9FormatInfo(Format, &fi) || fi.Depth) return D3DERR_INVALIDCALL;
  if (Width == 0 || Height == 0) return D3DERR_INVALIDCALL;
  if (D3D9FormatIsDxt(Format) && ((Width | Height) & 3)) return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width      = Width;
  td.Height     = Height;
  td.MipLevels  = 1;
  td.ArraySize  = 1;
  td.Format     = fi.Resource;
  td.SampleDesc.Count = 1;
  td.Usage      = D3D11_USAGE_DEFAULT;
  td.BindFlags  = 0;

  Com<ID3D11Texture2D> tex;
  if (FAILED(d3d11_device_->CreateTexture2D(&td, nullptr, &tex))) return D3DERR_INVALIDCALL;

  *ppSurface = ref(new D3D9Surface(this, tex.ptr(), 0, Pool, 0, Format));
  return D3D_OK;
}

// Render targets / depth
HRESULT STDMETHODCALLTYPE
D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  auto device_lock = LockDevice();
  if (RenderTargetIndex >= kMaxRenderTargets) return D3DERR_INVALIDCALL;
  // Setting RT0 to null is illegal in D3D9.
  if (RenderTargetIndex == 0 && !pRenderTarget) return D3DERR_INVALIDCALL;
  /* Only a surface made to be rendered to can be. */
  if (pRenderTarget && !(static_cast<D3D9Surface *>(pRenderTarget)->Desc().Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_INVALIDCALL;
  render_targets_[RenderTargetIndex] = static_cast<D3D9Surface *>(pRenderTarget);
  ApplyRenderTargets();
  /* A new render target 0 brings the viewport and the scissor rect back to
   * its full size. Applications rendering into a target of another size --
   * a shadow map, a reflection -- rely on that rather than setting both. */
  if (RenderTargetIndex == 0) {
    D3DSURFACE_DESC desc;
    pRenderTarget->GetDesc(&desc);
    ApplyViewport({0, 0, desc.Width, desc.Height, 0.0f, 1.0f});
    scissor_rect_ = {0, 0, (LONG)desc.Width, (LONG)desc.Height};
    D3D11_RECT r = {0, 0, (LONG)desc.Width, (LONG)desc.Height};
    d3d11_ctx_->RSSetScissorRects(1, &r);
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRT) {
  auto device_lock = LockDevice();
  if (!ppRT) return D3DERR_INVALIDCALL;
  *ppRT = nullptr;
  if (RenderTargetIndex >= kMaxRenderTargets) return D3DERR_INVALIDCALL;
  *ppRT = render_targets_[RenderTargetIndex] ? ref(render_targets_[RenderTargetIndex].ptr()) : nullptr;
  return *ppRT ? D3D_OK : D3DERR_NOTFOUND;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pSurface) {
  auto device_lock = LockDevice();
  if (pSurface && !(static_cast<D3D9Surface *>(pSurface)->Desc().Usage & D3DUSAGE_DEPTHSTENCIL))
    return D3DERR_INVALIDCALL;
  depth_stencil_ = static_cast<D3D9Surface *>(pSurface);
  ApplyRenderTargets();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZ) {
  auto device_lock = LockDevice();
  if (!ppZ) return D3DERR_INVALIDCALL;
  *ppZ = depth_stencil_ ? ref(depth_stencil_.ptr()) : nullptr;
  return *ppZ ? D3D_OK : D3DERR_NOTFOUND;
}
HRESULT STDMETHODCALLTYPE D3D9Device::BeginScene()                                           { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::EndScene()                                             { return D3D_OK; }

HRESULT STDMETHODCALLTYPE
D3D9Device::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  auto device_lock = LockDevice();
  if (Count && !pRects) return D3DERR_INVALIDCALL;

  // D3D9's Clear is clipped to the current viewport (and the given rects);
  // D3D11's ClearRenderTargetView is not. Figure out whether the viewport
  // actually restricts anything before falling back to per-rect ClearView.
  const D3DVIEWPORT9 &vp = viewport_;
  bool vp_partial = false;
  if (render_targets_[0]) {
    const auto &rt_desc = render_targets_[0]->Desc();
    vp_partial = vp.X > 0 || vp.Y > 0 || vp.Width < rt_desc.Width || vp.Height < rt_desc.Height;
  }
  bool partial = Count > 0 || vp_partial;

  D3D11_RECT vp_rect = {
    (LONG)vp.X, (LONG)vp.Y,
    (LONG)(vp.X + vp.Width), (LONG)(vp.Y + vp.Height),
  };
  auto clampToViewport = [&](const D3DRECT &r) -> D3D11_RECT {
    return {
      std::max(r.x1, vp_rect.left), std::max(r.y1, vp_rect.top),
      std::min(r.x2, vp_rect.right), std::min(r.y2, vp_rect.bottom),
    };
  };

  if (Flags & D3DCLEAR_TARGET) {
    float c[4] = {
      ((Color >> 16) & 0xFF) / 255.0f,
      ((Color >>  8) & 0xFF) / 255.0f,
      ((Color >>  0) & 0xFF) / 255.0f,
      ((Color >> 24) & 0xFF) / 255.0f,
    };
    Com<ID3D11DeviceContext1> ctx1;
    if (partial)
      d3d11_ctx_->QueryInterface(IID_PPV_ARGS(&ctx1));
    for (UINT i = 0; i < kMaxRenderTargets; i++) {
      if (!render_targets_[i]) continue;
      auto rtv = render_targets_[i]->GetRTV();
      if (!rtv) continue;
      if (partial && ctx1) {
        if (Count) {
          for (DWORD r = 0; r < Count; r++) {
            D3D11_RECT rect = clampToViewport(pRects[r]);
            if (rect.right > rect.left && rect.bottom > rect.top)
              ctx1->ClearView(rtv, c, &rect, 1);
          }
        } else {
          ctx1->ClearView(rtv, c, &vp_rect, 1);
        }
      } else {
        d3d11_ctx_->ClearRenderTargetView(rtv, c);
      }
    }
  }

  if ((Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && depth_stencil_) {
    UINT df = 0;
    if (Flags & D3DCLEAR_ZBUFFER) df |= D3D11_CLEAR_DEPTH;
    if (Flags & D3DCLEAR_STENCIL) df |= D3D11_CLEAR_STENCIL;
    if (auto dsv = depth_stencil_->GetDSV()) {
      // D3D11 can only clear a whole depth-stencil view. A viewport/rect-
      // limited depth clear over-clears here; log once so mis-rendering in
      // split-screen-style apps is attributable.
      if (partial) {
        static bool warned = false;
        if (!warned) {
          warned = true;
          WARN("D3D9Device::Clear: depth clear clipped by viewport/rects is approximated by a full clear");
        }
      }
      d3d11_ctx_->ClearDepthStencilView(dsv, df, Z, (UINT8)Stencil);
    }
  }
  return D3D_OK;
}

// Transform / material / lighting - fixed-function state storage.
namespace {
const D3DMATRIX kIdentityMatrix = {{{
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
}}};
} // namespace

HRESULT STDMETHODCALLTYPE
D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  auto device_lock = LockDevice();
  if (!pMatrix) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordTransform(State, *pMatrix); return D3D_OK; }
  transforms_[State] = *pMatrix;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pM) {
  auto device_lock = LockDevice();
  if (!pM) return D3DERR_INVALIDCALL;
  auto it = transforms_.find(State);
  *pM = it != transforms_.end() ? it->second : kIdentityMatrix;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  auto device_lock = LockDevice();
  if (!pMatrix) return D3DERR_INVALIDCALL;
  auto it = transforms_.find(State);
  const D3DMATRIX &cur = it != transforms_.end() ? it->second : kIdentityMatrix;
  D3DMATRIX out = {};
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      for (int k = 0; k < 4; k++)
        out.m[r][c] += cur.m[r][k] * pMatrix->m[k][c];
  transforms_[State] = out;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::SetViewport(const D3DVIEWPORT9 *pVP) {
  auto device_lock = LockDevice();
  if (!pVP) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordViewport(*pVP); return D3D_OK; }
  ApplyViewport(*pVP);
  return D3D_OK;
}

void
D3D9Device::ApplyViewport(const D3DVIEWPORT9 &viewport) {
  viewport_ = viewport;
  /* D3D9 puts pixel centres on integer coordinates, D3D10 and later half a
   * pixel further on, so the same geometry covers different pixels. Moving
   * the viewport by that half pixel restores D3D9's rasterisation -- without
   * it every D3D9 title is offset by half a pixel, its 2D art and text
   * resampled and its post-processing misaligned. The 1/128 short of a half
   * keeps edges that fall exactly on a centre on the side D3D9 drivers put
   * them, as wined3d and DXVK both do. */
  constexpr float kPixelCentre = 0.5f - 1.0f / 128.0f;
  D3D11_VIEWPORT vp = {
    (float)viewport.X + kPixelCentre, (float)viewport.Y + kPixelCentre,
    (float)viewport.Width, (float)viewport.Height,
    viewport.MinZ, viewport.MaxZ
  };
  d3d11_ctx_->RSSetViewports(1, &vp);
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetViewport(D3DVIEWPORT9 *pVP) {
  auto device_lock = LockDevice();
  if (!pVP) return D3DERR_INVALIDCALL;
  *pVP = viewport_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::SetMaterial(const D3DMATERIAL9 *pMaterial) {
  auto device_lock = LockDevice();
  if (!pMaterial) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordMaterial(*pMaterial); return D3D_OK; }
  material_ = *pMaterial;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetMaterial(D3DMATERIAL9 *pM) {
  auto device_lock = LockDevice();
  if (!pM) return D3DERR_INVALIDCALL;
  *pM = material_;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetLight(DWORD Index, const D3DLIGHT9 *pLight) {
  auto device_lock = LockDevice();
  if (!pLight) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordLight(Index, *pLight); return D3D_OK; }
  auto &slot = lights_[Index];
  slot.light = *pLight;
  slot.present = true;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetLight(DWORD Index, D3DLIGHT9 *pL) {
  auto device_lock = LockDevice();
  if (!pL) return D3DERR_INVALIDCALL;
  auto it = lights_.find(Index);
  if (it == lights_.end() || !it->second.present) return D3DERR_INVALIDCALL;
  *pL = it->second.light;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::LightEnable(DWORD Index, BOOL Enable) {
  auto device_lock = LockDevice();
  if (recording_) { recording_->RecordLightEnable(Index, Enable); return D3D_OK; }
  auto &slot = lights_[Index];
  if (!slot.present) {
    // Enabling a light that was never set enables D3D9's default light.
    slot.light = {};
    slot.light.Type = D3DLIGHT_DIRECTIONAL;
    slot.light.Diffuse = {1.0f, 1.0f, 1.0f, 0.0f};
    slot.light.Direction = {0.0f, 0.0f, 1.0f};
    slot.present = true;
  }
  slot.enabled = Enable != FALSE;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetLightEnable(DWORD Index, BOOL *pE) {
  auto device_lock = LockDevice();
  if (!pE) return D3DERR_INVALIDCALL;
  auto it = lights_.find(Index);
  *pE = it != lights_.end() && it->second.enabled ? TRUE : FALSE;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetClipPlane(DWORD Index, const float *pPlane) {
  auto device_lock = LockDevice();
  if (!pPlane || Index >= kMaxClipPlanes) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordClipPlane(Index, pPlane); return D3D_OK; }
  memcpy(clip_planes_[Index], pPlane, 4 * sizeof(float));
  vs_extra_dirty_ = true;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetClipPlane(DWORD Index, float *pP) {
  auto device_lock = LockDevice();
  if (!pP || Index >= kMaxClipPlanes) return D3DERR_INVALIDCALL;
  memcpy(pP, clip_planes_[Index], 4 * sizeof(float));
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
  auto device_lock = LockDevice();
  if ((UINT)State >= kMaxRenderStates) return D3D_OK; // states beyond our table are silently accepted
  if (recording_) { recording_->RecordRenderState(State, Value); return D3D_OK; }
  if (render_state_[State] == Value) return D3D_OK;
  render_state_[State] = Value;
  render_state_dirty_ = true;
  // The alpha-test reference and the fog colour ride in the pixel stage's
  // extra constant buffer.
  if (State == D3DRS_ALPHAREF || State == D3DRS_FOGCOLOR || State == D3DRS_FOGSTART ||
      State == D3DRS_FOGEND || State == D3DRS_FOGDENSITY)
    ps_extra_dirty_ = true;
  // The point size range rides in the vertex stage's.
  if (State == D3DRS_POINTSIZE || State == D3DRS_POINTSIZE_MIN || State == D3DRS_POINTSIZE_MAX ||
      State == D3DRS_DEPTHBIAS || State == D3DRS_CLIPPLANEENABLE || State == D3DRS_CLIPPING)
    vs_extra_dirty_ = true;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) {
  auto device_lock = LockDevice();
  if (!pValue) return D3DERR_INVALIDCALL;
  *pValue = (UINT)State < kMaxRenderStates ? render_state_[State] : 0;
  return D3D_OK;
}

void
D3D9Device::InitDefaultRenderState() {
  memset(render_state_, 0, sizeof(render_state_));
  render_state_[D3DRS_ZENABLE] = D3DZB_TRUE;
  render_state_[D3DRS_ZWRITEENABLE] = TRUE;
  render_state_[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
  render_state_[D3DRS_CULLMODE] = D3DCULL_CCW;
  render_state_[D3DRS_FILLMODE] = D3DFILL_SOLID;
  render_state_[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
  render_state_[D3DRS_ALPHABLENDENABLE] = FALSE;
  render_state_[D3DRS_SRCBLEND] = D3DBLEND_ONE;
  render_state_[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
  render_state_[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
  render_state_[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
  render_state_[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
  render_state_[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
  render_state_[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
  render_state_[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
  render_state_[D3DRS_COLORWRITEENABLE] = 0xF;
  render_state_[D3DRS_COLORWRITEENABLE1] = 0xF;
  render_state_[D3DRS_COLORWRITEENABLE2] = 0xF;
  render_state_[D3DRS_COLORWRITEENABLE3] = 0xF;
  render_state_[D3DRS_ALPHATESTENABLE] = FALSE;
  render_state_[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
  render_state_[D3DRS_ALPHAREF] = 0;
  render_state_[D3DRS_STENCILENABLE] = FALSE;
  render_state_[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
  render_state_[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  render_state_[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
  render_state_[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
  render_state_[D3DRS_STENCILREF] = 0;
  render_state_[D3DRS_STENCILMASK] = 0xFFFFFFFF;
  render_state_[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
  render_state_[D3DRS_TWOSIDEDSTENCILMODE] = FALSE;
  render_state_[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
  render_state_[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
  render_state_[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
  render_state_[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
  render_state_[D3DRS_SCISSORTESTENABLE] = FALSE;
  render_state_[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
  render_state_[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
  render_state_[D3DRS_LIGHTING] = TRUE;
  render_state_[D3DRS_COLORVERTEX] = TRUE;
  render_state_[D3DRS_CLIPPING] = TRUE;
  render_state_[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
  render_state_[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
  render_state_[D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL;
  render_state_[D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL;
  render_state_[D3DRS_FOGENABLE] = FALSE;
  render_state_[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
  render_state_[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
  float one = 1.0f, point_size_max = kMaxPointSize;
  memcpy(&render_state_[D3DRS_FOGEND], &one, sizeof(float));
  memcpy(&render_state_[D3DRS_FOGDENSITY], &one, sizeof(float));
  memcpy(&render_state_[D3DRS_POINTSIZE], &one, sizeof(float));
  memcpy(&render_state_[D3DRS_POINTSIZE_MIN], &one, sizeof(float));
  memcpy(&render_state_[D3DRS_POINTSIZE_MAX], &point_size_max, sizeof(float));
  memcpy(&render_state_[D3DRS_POINTSCALE_A], &one, sizeof(float));
  render_state_dirty_ = true;
  ps_extra_dirty_ = true;
  vs_extra_dirty_ = true;
}

namespace {
D3D11_BLEND ToD3D11Blend(DWORD b) {
  switch (b) {
  case D3DBLEND_ZERO: return D3D11_BLEND_ZERO;
  case D3DBLEND_ONE: return D3D11_BLEND_ONE;
  case D3DBLEND_SRCCOLOR: return D3D11_BLEND_SRC_COLOR;
  case D3DBLEND_INVSRCCOLOR: return D3D11_BLEND_INV_SRC_COLOR;
  case D3DBLEND_SRCALPHA: return D3D11_BLEND_SRC_ALPHA;
  case D3DBLEND_INVSRCALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
  case D3DBLEND_DESTALPHA: return D3D11_BLEND_DEST_ALPHA;
  case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
  case D3DBLEND_DESTCOLOR: return D3D11_BLEND_DEST_COLOR;
  case D3DBLEND_INVDESTCOLOR: return D3D11_BLEND_INV_DEST_COLOR;
  case D3DBLEND_SRCALPHASAT: return D3D11_BLEND_SRC_ALPHA_SAT;
  case D3DBLEND_BLENDFACTOR: return D3D11_BLEND_BLEND_FACTOR;
  case D3DBLEND_INVBLENDFACTOR: return D3D11_BLEND_INV_BLEND_FACTOR;
  case D3DBLEND_SRCCOLOR2: return D3D11_BLEND_SRC1_COLOR;
  case D3DBLEND_INVSRCCOLOR2: return D3D11_BLEND_INV_SRC1_COLOR;
  default: return D3D11_BLEND_ONE;
  }
}

/* D3DBLEND_BOTHSRCALPHA / BOTHINVSRCALPHA are D3D6-era shorthands, valid as
 * a source factor only, that set the destination factor as well (and are
 * still reached through d3d8). Returns the pair to use. */
void ResolveD3D9BlendPair(DWORD src, DWORD dst, DWORD *src_out, DWORD *dst_out) {
  if (src == D3DBLEND_BOTHSRCALPHA) {
    *src_out = D3DBLEND_SRCALPHA;
    *dst_out = D3DBLEND_INVSRCALPHA;
  } else if (src == D3DBLEND_BOTHINVSRCALPHA) {
    *src_out = D3DBLEND_INVSRCALPHA;
    *dst_out = D3DBLEND_SRCALPHA;
  } else {
    *src_out = src;
    *dst_out = dst;
  }
}
} // namespace

namespace {
// D3DCMP_* and D3DSTENCILOP_* share their numeric values with the D3D11
// enums; out-of-range input falls back to the D3D9 default.
D3D11_COMPARISON_FUNC ToD3D11Cmp(DWORD v, D3D11_COMPARISON_FUNC fallback) {
  return (v >= D3DCMP_NEVER && v <= D3DCMP_ALWAYS) ? (D3D11_COMPARISON_FUNC)v : fallback;
}
D3D11_STENCIL_OP ToD3D11StencilOp(DWORD v) {
  return (v >= D3DSTENCILOP_KEEP && v <= D3DSTENCILOP_DECR) ? (D3D11_STENCIL_OP)v : D3D11_STENCIL_OP_KEEP;
}
} // namespace

void
D3D9Device::ApplyRenderState() {
  if (!render_state_dirty_) return;
  render_state_dirty_ = false;
  const DWORD *rs9 = render_state_;

  // --- depth-stencil ---------------------------------------------------------
  {
    uint64_t key =
        (uint64_t)(rs9[D3DRS_ZENABLE] != D3DZB_FALSE) |
        (uint64_t)(rs9[D3DRS_ZWRITEENABLE] ? 1 : 0) << 1 |
        (uint64_t)(rs9[D3DRS_ZFUNC] & 0xF) << 2 |
        (uint64_t)(rs9[D3DRS_STENCILENABLE] ? 1 : 0) << 6 |
        (uint64_t)(rs9[D3DRS_TWOSIDEDSTENCILMODE] ? 1 : 0) << 7 |
        (uint64_t)(rs9[D3DRS_STENCILFAIL] & 0xF) << 8 |
        (uint64_t)(rs9[D3DRS_STENCILZFAIL] & 0xF) << 12 |
        (uint64_t)(rs9[D3DRS_STENCILPASS] & 0xF) << 16 |
        (uint64_t)(rs9[D3DRS_STENCILFUNC] & 0xF) << 20 |
        (uint64_t)(rs9[D3DRS_CCW_STENCILFAIL] & 0xF) << 24 |
        (uint64_t)(rs9[D3DRS_CCW_STENCILZFAIL] & 0xF) << 28 |
        (uint64_t)(rs9[D3DRS_CCW_STENCILPASS] & 0xF) << 32 |
        (uint64_t)(rs9[D3DRS_CCW_STENCILFUNC] & 0xF) << 36 |
        (uint64_t)(rs9[D3DRS_STENCILMASK] & 0xFF) << 40 |
        (uint64_t)(rs9[D3DRS_STENCILWRITEMASK] & 0xFF) << 48;
    auto it = dss_cache_.find(key);
    if (it == dss_cache_.end()) {
      D3D11_DEPTH_STENCIL_DESC dsd = {};
      dsd.DepthEnable = rs9[D3DRS_ZENABLE] != D3DZB_FALSE;
      dsd.DepthWriteMask = rs9[D3DRS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
      dsd.DepthFunc = ToD3D11Cmp(rs9[D3DRS_ZFUNC], D3D11_COMPARISON_LESS_EQUAL);
      dsd.StencilEnable = rs9[D3DRS_STENCILENABLE] != 0;
      dsd.StencilReadMask = (UINT8)rs9[D3DRS_STENCILMASK];
      dsd.StencilWriteMask = (UINT8)rs9[D3DRS_STENCILWRITEMASK];
      // D3D9's STENCIL* states apply to clockwise (front-facing) triangles,
      // CCW_STENCIL* to counter-clockwise ones - active only in two-sided mode.
      dsd.FrontFace.StencilFailOp = ToD3D11StencilOp(rs9[D3DRS_STENCILFAIL]);
      dsd.FrontFace.StencilDepthFailOp = ToD3D11StencilOp(rs9[D3DRS_STENCILZFAIL]);
      dsd.FrontFace.StencilPassOp = ToD3D11StencilOp(rs9[D3DRS_STENCILPASS]);
      dsd.FrontFace.StencilFunc = ToD3D11Cmp(rs9[D3DRS_STENCILFUNC], D3D11_COMPARISON_ALWAYS);
      if (rs9[D3DRS_TWOSIDEDSTENCILMODE]) {
        dsd.BackFace.StencilFailOp = ToD3D11StencilOp(rs9[D3DRS_CCW_STENCILFAIL]);
        dsd.BackFace.StencilDepthFailOp = ToD3D11StencilOp(rs9[D3DRS_CCW_STENCILZFAIL]);
        dsd.BackFace.StencilPassOp = ToD3D11StencilOp(rs9[D3DRS_CCW_STENCILPASS]);
        dsd.BackFace.StencilFunc = ToD3D11Cmp(rs9[D3DRS_CCW_STENCILFUNC], D3D11_COMPARISON_ALWAYS);
      } else {
        dsd.BackFace = dsd.FrontFace;
      }
      Com<ID3D11DepthStencilState> dss;
      if (FAILED(d3d11_device_->CreateDepthStencilState(&dsd, &dss)))
        return;
      it = dss_cache_.emplace(key, std::move(dss)).first;
    }
    d3d11_ctx_->OMSetDepthStencilState(it->second.ptr(), rs9[D3DRS_STENCILREF]);
  }

  // --- rasterizer --------------------------------------------------------------
  {
    std::array<uint32_t, 4> key = {
      (rs9[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? 1u : 0u) |
          ((rs9[D3DRS_CULLMODE] & 0x3) << 1) |
          ((rs9[D3DRS_SCISSORTESTENABLE] ? 1u : 0u) << 3) |
          ((rs9[D3DRS_MULTISAMPLEANTIALIAS] ? 1u : 0u) << 4),
      0, rs9[D3DRS_SLOPESCALEDEPTHBIAS], 0,
    };
    auto it = rs_cache_.find(key);
    if (it == rs_cache_.end()) {
      D3D11_RASTERIZER_DESC rd = {};
      rd.FillMode = rs9[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
      switch (rs9[D3DRS_CULLMODE]) {
      case D3DCULL_NONE: rd.CullMode = D3D11_CULL_NONE; break;
      case D3DCULL_CW: rd.CullMode = D3D11_CULL_FRONT; break;
      default: rd.CullMode = D3D11_CULL_BACK; break; // D3DCULL_CCW (D3D9 default)
      }
      rd.FrontCounterClockwise = FALSE; // D3D9's winding convention: CW is front
      rd.DepthClipEnable = TRUE;
      rd.ScissorEnable = rs9[D3DRS_SCISSORTESTENABLE] != 0;
      rd.MultisampleEnable = rs9[D3DRS_MULTISAMPLEANTIALIAS] != 0;
      // The constant D3DRS_DEPTHBIAS is the vertex stage's (see
      // sm3::kExtraCBDepthBias); the slope-scaled part stays here.
      float slope;
      memcpy(&slope, &rs9[D3DRS_SLOPESCALEDEPTHBIAS], sizeof(float));
      rd.DepthBias = 0;
      rd.SlopeScaledDepthBias = slope;
      Com<ID3D11RasterizerState> rs;
      if (FAILED(d3d11_device_->CreateRasterizerState(&rd, &rs)))
        return;
      it = rs_cache_.emplace(key, std::move(rs)).first;
    }
    d3d11_ctx_->RSSetState(it->second.ptr());
  }

  // --- blend -------------------------------------------------------------------
  {
    bool separate_alpha = rs9[D3DRS_SEPARATEALPHABLENDENABLE] != 0;
    uint64_t key =
        (uint64_t)(rs9[D3DRS_ALPHABLENDENABLE] ? 1 : 0) |
        (uint64_t)(rs9[D3DRS_SRCBLEND] & 0x1F) << 1 |
        (uint64_t)(rs9[D3DRS_DESTBLEND] & 0x1F) << 6 |
        (uint64_t)(rs9[D3DRS_BLENDOP] & 0x7) << 11 |
        (uint64_t)(separate_alpha ? 1 : 0) << 14 |
        (uint64_t)(rs9[D3DRS_SRCBLENDALPHA] & 0x1F) << 15 |
        (uint64_t)(rs9[D3DRS_DESTBLENDALPHA] & 0x1F) << 20 |
        (uint64_t)(rs9[D3DRS_BLENDOPALPHA] & 0x7) << 25 |
        (uint64_t)(rs9[D3DRS_COLORWRITEENABLE] & 0xF) << 28 |
        (uint64_t)(rs9[D3DRS_COLORWRITEENABLE1] & 0xF) << 32 |
        (uint64_t)(rs9[D3DRS_COLORWRITEENABLE2] & 0xF) << 36 |
        (uint64_t)(rs9[D3DRS_COLORWRITEENABLE3] & 0xF) << 40;
    auto it = bs_cache_.find(key);
    if (it == bs_cache_.end()) {
      D3D11_BLEND_DESC bd = {};
      bd.IndependentBlendEnable = TRUE;
      DWORD blendop = rs9[D3DRS_BLENDOP];
      DWORD blendop_a = separate_alpha ? rs9[D3DRS_BLENDOPALPHA] : blendop;
      static const D3DRENDERSTATETYPE write_states[4] = {
        D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3,
      };
      DWORD src_rgb, dst_rgb, src_alpha, dst_alpha;
      ResolveD3D9BlendPair(rs9[D3DRS_SRCBLEND], rs9[D3DRS_DESTBLEND], &src_rgb, &dst_rgb);
      ResolveD3D9BlendPair(rs9[D3DRS_SRCBLENDALPHA], rs9[D3DRS_DESTBLENDALPHA], &src_alpha, &dst_alpha);
      for (UINT i = 0; i < 4; i++) {
        auto &rt = bd.RenderTarget[i];
        rt.BlendEnable = rs9[D3DRS_ALPHABLENDENABLE] != 0;
        rt.SrcBlend = ToD3D11Blend(src_rgb);
        rt.DestBlend = ToD3D11Blend(dst_rgb);
        rt.BlendOp = blendop ? (D3D11_BLEND_OP)blendop : D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = separate_alpha ? ToD3D11Blend(src_alpha) : rt.SrcBlend;
        rt.DestBlendAlpha = separate_alpha ? ToD3D11Blend(dst_alpha) : rt.DestBlend;
        rt.BlendOpAlpha = blendop_a ? (D3D11_BLEND_OP)blendop_a : D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = (UINT8)(rs9[write_states[i]] & 0xF);
      }
      // A color-only blend factor in the alpha slots is invalid in D3D11.
      for (UINT i = 0; i < 4; i++) {
        auto &rt = bd.RenderTarget[i];
        if (rt.SrcBlendAlpha == D3D11_BLEND_SRC_COLOR) rt.SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
        if (rt.SrcBlendAlpha == D3D11_BLEND_INV_SRC_COLOR) rt.SrcBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        if (rt.SrcBlendAlpha == D3D11_BLEND_DEST_COLOR) rt.SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
        if (rt.SrcBlendAlpha == D3D11_BLEND_INV_DEST_COLOR) rt.SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
        if (rt.DestBlendAlpha == D3D11_BLEND_SRC_COLOR) rt.DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
        if (rt.DestBlendAlpha == D3D11_BLEND_INV_SRC_COLOR) rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        if (rt.DestBlendAlpha == D3D11_BLEND_DEST_COLOR) rt.DestBlendAlpha = D3D11_BLEND_DEST_ALPHA;
        if (rt.DestBlendAlpha == D3D11_BLEND_INV_DEST_COLOR) rt.DestBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
        if (rt.SrcBlendAlpha == D3D11_BLEND_SRC1_COLOR) rt.SrcBlendAlpha = D3D11_BLEND_SRC1_ALPHA;
        if (rt.SrcBlendAlpha == D3D11_BLEND_INV_SRC1_COLOR) rt.SrcBlendAlpha = D3D11_BLEND_INV_SRC1_ALPHA;
        if (rt.DestBlendAlpha == D3D11_BLEND_SRC1_COLOR) rt.DestBlendAlpha = D3D11_BLEND_SRC1_ALPHA;
        if (rt.DestBlendAlpha == D3D11_BLEND_INV_SRC1_COLOR) rt.DestBlendAlpha = D3D11_BLEND_INV_SRC1_ALPHA;
      }
      Com<ID3D11BlendState> bs;
      if (FAILED(d3d11_device_->CreateBlendState(&bd, &bs)))
        return;
      it = bs_cache_.emplace(key, std::move(bs)).first;
    }
    DWORD factor = rs9[D3DRS_BLENDFACTOR];
    float bf[4] = {
      ((factor >> 16) & 0xFF) / 255.0f,
      ((factor >> 8) & 0xFF) / 255.0f,
      ((factor >> 0) & 0xFF) / 255.0f,
      ((factor >> 24) & 0xFF) / 255.0f,
    };
    d3d11_ctx_->OMSetBlendState(it->second.ptr(), bf, rs9[D3DRS_MULTISAMPLEMASK]);
  }
}
HRESULT STDMETHODCALLTYPE
D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) {
  auto device_lock = LockDevice();
  if (!ppSB) return D3DERR_INVALIDCALL;
  *ppSB = nullptr;
  Com<D3D9StateBlock> block = new D3D9StateBlock(this);
  switch (Type) {
  case D3DSBT_ALL: block->MarkAll(); break;
  case D3DSBT_VERTEXSTATE: block->MarkVertexState(); break;
  case D3DSBT_PIXELSTATE: block->MarkPixelState(); break;
  default: return D3DERR_INVALIDCALL;
  }
  block->Capture();
  *ppSB = block.takeOwnership();
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::BeginStateBlock() {
  auto device_lock = LockDevice();
  if (recording_) return D3DERR_INVALIDCALL;
  recording_ = new D3D9StateBlock(this);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) {
  auto device_lock = LockDevice();
  if (!ppSB) return D3DERR_INVALIDCALL;
  *ppSB = nullptr;
  if (!recording_) return D3DERR_INVALIDCALL;
  /* The public reference is the caller's, and it is what makes the block take
   * a reference on the device -- BeginStateBlock did not. */
  *ppSB = ref(recording_.ptr());
  recording_ = nullptr;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::SetClipStatus(const D3DCLIPSTATUS9 *)           { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetClipStatus(D3DCLIPSTATUS9 *pCS)             { if (pCS) memset(pCS, 0, sizeof(*pCS)); return D3D_OK; }
namespace {
// D3D9 addresses the four vertex-texture samplers at 257..260
// (D3DVERTEXTEXTURESAMPLER0..3); everything else uses 0..15 directly. The
// displacement-map sampler (256) has no D3D11 analogue and is not supported.
bool MapSamplerStage(DWORD Stage, UINT *out, UINT total) {
  if (Stage < 16) { *out = Stage; return true; }
  if (Stage >= D3DVERTEXTEXTURESAMPLER0 && Stage <= D3DVERTEXTEXTURESAMPLER3) {
    *out = 16 + (Stage - D3DVERTEXTEXTURESAMPLER0);
    return *out < total;
  }
  return false;
}
} // namespace

HRESULT STDMETHODCALLTYPE
D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
  auto device_lock = LockDevice();
  UINT slot;
  if (!ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  if (!MapSamplerStage(Stage, &slot, kTotalSamplers)) return D3DERR_INVALIDCALL;
  *ppTexture = textures_[slot] ? ref(textures_[slot].texture) : nullptr;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
  auto device_lock = LockDevice();
  UINT slot;
  if (!MapSamplerStage(Stage, &slot, kTotalSamplers)) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordTexture(slot, pTexture); return D3D_OK; }
  textures_[slot].Set(pTexture);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pV) {
  auto device_lock = LockDevice();
  if (!pV || Stage >= kMaxTextureStages || (UINT)Type >= kMaxTextureStageStates) return D3DERR_INVALIDCALL;
  *pV = tss_[Stage][Type];
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  auto device_lock = LockDevice();
  if (Stage >= kMaxTextureStages || (UINT)Type >= kMaxTextureStageStates) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordTextureStageState(Stage, Type, Value); return D3D_OK; }
  tss_[Stage][Type] = Value;
  /* The bump environment states are the only stage states a shader reads, and
   * they reach it through the pixel stage's extra constant buffer. */
  switch (Type) {
  case D3DTSS_BUMPENVMAT00:
  case D3DTSS_BUMPENVMAT01:
  case D3DTSS_BUMPENVMAT10:
  case D3DTSS_BUMPENVMAT11:
  case D3DTSS_BUMPENVLSCALE:
  case D3DTSS_BUMPENVLOFFSET:
    ps_extra_dirty_ = true;
    break;
  default:
    break;
  }
  return D3D_OK;
}

void
D3D9Device::InitDefaultTextureStageState() {
  memset(tss_, 0, sizeof(tss_));
  for (UINT s = 0; s < kMaxTextureStages; s++) {
    tss_[s][D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
    tss_[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
    tss_[s][D3DTSS_COLORARG2] = D3DTA_CURRENT;
    tss_[s][D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
    tss_[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
    tss_[s][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
    tss_[s][D3DTSS_TEXCOORDINDEX] = s;
    tss_[s][D3DTSS_COLORARG0] = D3DTA_CURRENT;
    tss_[s][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
    tss_[s][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    tss_[s][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
    /* The bump-mapping states, BUMPENVLSCALE included, all start at 0. */
  }
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pV) {
  auto device_lock = LockDevice();
  UINT slot;
  if (!pV || !MapSamplerStage(Sampler, &slot, kTotalSamplers) || Type < 1 || Type > D3DSAMP_DMAPOFFSET)
    return D3DERR_INVALIDCALL;
  *pV = sampler_state_[slot][Type];
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  auto device_lock = LockDevice();
  UINT slot;
  if (!MapSamplerStage(Sampler, &slot, kTotalSamplers) || Type < 1 || Type > D3DSAMP_DMAPOFFSET)
    return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordSamplerState(slot, Type, Value); return D3D_OK; }
  if (sampler_state_[slot][Type] != Value) {
    sampler_state_[slot][Type] = Value;
    samplers_dirty_ |= 1u << slot;
  }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::ValidateDevice(DWORD *pPasses)      { if (pPasses) *pPasses = 1; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetPaletteEntries(UINT, const PALETTEENTRY *)           { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetPaletteEntries(UINT, PALETTEENTRY *pE)               { if (pE) memset(pE, 0, 256*sizeof(PALETTEENTRY)); return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetCurrentTexturePalette(UINT)                         { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetCurrentTexturePalette(UINT *pN) { if (pN) *pN = 0; return D3D_OK; }

HRESULT STDMETHODCALLTYPE
D3D9Device::SetScissorRect(const RECT *pRect) {
  auto device_lock = LockDevice();
  if (!pRect) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordScissorRect(*pRect); return D3D_OK; }
  scissor_rect_ = *pRect;
  D3D11_RECT r = { pRect->left, pRect->top, pRect->right, pRect->bottom };
  d3d11_ctx_->RSSetScissorRects(1, &r);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetScissorRect(RECT *pRect) {
  auto device_lock = LockDevice();
  if (!pRect) return D3DERR_INVALIDCALL;
  *pRect = scissor_rect_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetSoftwareVertexProcessing(BOOL)        { return D3D_OK; }
BOOL    STDMETHODCALLTYPE D3D9Device::GetSoftwareVertexProcessing()             { return FALSE; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetNPatchMode(float)                      { return D3D_OK; }
float   STDMETHODCALLTYPE D3D9Device::GetNPatchMode()                           { return 0.0f; }

// --- vertex declaration / FVF -----------------------------------------------
HRESULT STDMETHODCALLTYPE
D3D9Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) {
  auto device_lock = LockDevice();
  if (!ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = nullptr;
  if (!pVertexElements) return D3DERR_INVALIDCALL;
  UINT count = 0;
  while (pVertexElements[count].Stream != 0xFF) count++;
  /* Plus the terminator: GetDeclaration reports it, and an application sizes
   * its buffer from that count. */
  *ppDecl = ref(new D3D9VertexDeclaration(this, pVertexElements, count + 1));
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
  auto device_lock = LockDevice();
  if (recording_) { recording_->RecordVertexDeclaration(static_cast<D3D9VertexDeclaration *>(pDecl)); return D3D_OK; }
  auto *decl = static_cast<D3D9VertexDeclaration *>(pDecl);
  vertex_decl_ = decl;
  /* The two are one piece of state. Setting a declaration replaces the FVF
   * with whatever that declaration is equivalent to -- 0 for one built from an
   * element array, which is most of them. Leaving the old FVF behind is what
   * made GetFVF report a stale value. */
  fvf_ = decl ? decl->Fvf() : 0;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
  auto device_lock = LockDevice();
  if (!ppDecl) return D3DERR_INVALIDCALL;
  *ppDecl = vertex_decl_ ? ref(vertex_decl_.ptr()) : nullptr;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetFVF(DWORD FVF) {
  auto device_lock = LockDevice();
  if (recording_) { recording_->RecordFVF(FVF); return D3D_OK; }
  fvf_ = FVF;
  /* A non-zero FVF *is* a vertex declaration -- D3D9 builds an implicit one and
   * hands it back from GetVertexDeclaration. Setting the current declaration to
   * null instead made GetVertexDeclaration return null, which applications
   * release unconditionally.
   *
   * FVF 0 is the exception: it clears the FVF and leaves whatever declaration
   * is current alone. */
  if (FVF)
    vertex_decl_ = ImplicitDeclarationForFVF(FVF);
  return D3D_OK;
}

/* One declaration object per FVF, so repeated SetFVF calls with the same value
 * keep handing back the same object rather than allocating per draw. */
D3D9VertexDeclaration *
D3D9Device::ImplicitDeclarationForFVF(DWORD fvf) {
  auto &slot = fvf_decls_[fvf];
  if (!slot) {
    auto elements = ConvertFVFToDeclaration(fvf);
    slot = new D3D9VertexDeclaration(this, elements.data(), (UINT)elements.size(), fvf);
  }
  return slot.ptr();
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetFVF(DWORD *pFVF) {
  auto device_lock = LockDevice();
  if (!pFVF) return D3DERR_INVALIDCALL;
  *pFVF = fvf_;
  return D3D_OK;
}

const std::vector<D3DVERTEXELEMENT9> *
D3D9Device::CurrentDeclarationElements() {
  auto *decl = CurrentDeclaration();
  return decl ? &decl->Elements() : nullptr;
}

D3D9VertexDeclaration *
D3D9Device::CurrentDeclaration() {
  if (vertex_decl_) return vertex_decl_.ptr();
  if (!fvf_) return nullptr;
  return ImplicitDeclarationForFVF(fvf_);
}

void
D3D9Device::BindNullStream() {
  if (!null_stream_buffer_) {
    static const float zeroes[4] = {};
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(zeroes);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {zeroes};
    if (FAILED(d3d11_device_->CreateBuffer(&bd, &init, &null_stream_buffer_)))
      return;
  }
  ID3D11Buffer *buf = null_stream_buffer_.ptr();
  UINT stride = 0, offset = 0;
  d3d11_ctx_->IASetVertexBuffers(kNullStreamSlot, 1, &buf, &stride, &offset);
}

void
D3D9Device::UseInputLayout(const CachedInputLayout &cached) {
  d3d11_ctx_->IASetInputLayout(cached.layout.ptr());
  if (cached.null_stream)
    BindNullStream();
}

// --- shaders -----------------------------------------------------------------
HRESULT STDMETHODCALLTYPE
D3D9Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
  auto device_lock = LockDevice();
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  if (!pFunction) return D3DERR_INVALIDCALL;
  auto *shader = new D3D9VertexShader(this, pFunction);
  if (!shader->IsValid()) { delete shader; return D3DERR_INVALIDCALL; }
  *ppShader = ref(shader);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetVertexShader(IDirect3DVertexShader9 *pShader) {
  auto device_lock = LockDevice();
  if (recording_) { recording_->RecordVertexShader(static_cast<D3D9VertexShader *>(pShader)); return D3D_OK; }
  vertex_shader_ = static_cast<D3D9VertexShader *>(pShader);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
  auto device_lock = LockDevice();
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = vertex_shader_ ? ref(vertex_shader_.ptr()) : nullptr;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
  auto device_lock = LockDevice();
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  if (!pFunction) return D3DERR_INVALIDCALL;
  auto *shader = new D3D9PixelShader(this, pFunction);
  if (!shader->IsValid()) { delete shader; return D3DERR_INVALIDCALL; }
  *ppShader = ref(shader);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetPixelShader(IDirect3DPixelShader9 *pShader) {
  auto device_lock = LockDevice();
  if (recording_) { recording_->RecordPixelShader(static_cast<D3D9PixelShader *>(pShader)); return D3D_OK; }
  pixel_shader_ = static_cast<D3D9PixelShader *>(pShader);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
  auto device_lock = LockDevice();
  if (!ppShader) return D3DERR_INVALIDCALL;
  *ppShader = pixel_shader_ ? ref(pixel_shader_.ptr()) : nullptr;
  return D3D_OK;
}

// --- shader constants ---------------------------------------------------------
/* Start + count wraps around for a start near UINT_MAX: register -1 passed
 * the old sum check and the copy landed 16 GB past the constant array. */
static inline bool
ConstantRangeValid(UINT start, UINT count, UINT limit) {
  return start <= limit && count <= limit - start;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, Vector4fCount, kMaxFloatConstants)) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordVSConstantF(StartRegister, pConstantData, Vector4fCount); return D3D_OK; }
  size_t bytes = Vector4fCount * 4 * sizeof(float);
  if (!memcmp(&vs_const_f_[StartRegister * 4], pConstantData, bytes)) return D3D_OK;
  memcpy(&vs_const_f_[StartRegister * 4], pConstantData, bytes);
  vs_const_dirty_ = true;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, Vector4fCount, kMaxFloatConstants)) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &vs_const_f_[StartRegister * 4], Vector4fCount * 4 * sizeof(float));
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, Vector4iCount, kMaxOtherConstants)) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordVSConstantI(StartRegister, pConstantData, Vector4iCount); return D3D_OK; }
  size_t bytes = Vector4iCount * 4 * sizeof(int);
  if (!memcmp(&vs_const_i_[StartRegister * 4], pConstantData, bytes)) return D3D_OK;
  memcpy(&vs_const_i_[StartRegister * 4], pConstantData, bytes);
  vs_extra_dirty_ = true;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, Vector4iCount, kMaxOtherConstants)) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &vs_const_i_[StartRegister * 4], Vector4iCount * 4 * sizeof(int));
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, BoolCount, kMaxOtherConstants)) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordVSConstantB(StartRegister, pConstantData, BoolCount); return D3D_OK; }
  for (UINT i = 0; i < BoolCount; i++) {
    uint32_t value = pConstantData[i] ? 1 : 0;
    if (vs_const_b_[StartRegister + i] != value) {
      vs_const_b_[StartRegister + i] = value;
      vs_extra_dirty_ = true;
    }
  }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, BoolCount, kMaxOtherConstants)) return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < BoolCount; i++)
    pConstantData[i] = vs_const_b_[StartRegister + i] ? TRUE : FALSE;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, Vector4fCount, kMaxFloatConstants)) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordPSConstantF(StartRegister, pConstantData, Vector4fCount); return D3D_OK; }
  size_t bytes = Vector4fCount * 4 * sizeof(float);
  if (!memcmp(&ps_const_f_[StartRegister * 4], pConstantData, bytes)) return D3D_OK;
  memcpy(&ps_const_f_[StartRegister * 4], pConstantData, bytes);
  ps_const_dirty_ = true;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, Vector4fCount, kMaxFloatConstants)) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &ps_const_f_[StartRegister * 4], Vector4fCount * 4 * sizeof(float));
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, Vector4iCount, kMaxOtherConstants)) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordPSConstantI(StartRegister, pConstantData, Vector4iCount); return D3D_OK; }
  size_t bytes = Vector4iCount * 4 * sizeof(int);
  if (!memcmp(&ps_const_i_[StartRegister * 4], pConstantData, bytes)) return D3D_OK;
  memcpy(&ps_const_i_[StartRegister * 4], pConstantData, bytes);
  ps_extra_dirty_ = true;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, Vector4iCount, kMaxOtherConstants)) return D3DERR_INVALIDCALL;
  memcpy(pConstantData, &ps_const_i_[StartRegister * 4], Vector4iCount * 4 * sizeof(int));
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, BoolCount, kMaxOtherConstants)) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordPSConstantB(StartRegister, pConstantData, BoolCount); return D3D_OK; }
  for (UINT i = 0; i < BoolCount; i++) {
    uint32_t value = pConstantData[i] ? 1 : 0;
    if (ps_const_b_[StartRegister + i] != value) {
      ps_const_b_[StartRegister + i] = value;
      ps_extra_dirty_ = true;
    }
  }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  auto device_lock = LockDevice();
  if (!pConstantData || !ConstantRangeValid(StartRegister, BoolCount, kMaxOtherConstants)) return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < BoolCount; i++)
    pConstantData[i] = ps_const_b_[StartRegister + i] ? TRUE : FALSE;
  return D3D_OK;
}

// --- stream / index sources --------------------------------------------------
HRESULT STDMETHODCALLTYPE
D3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) {
  auto device_lock = LockDevice();
  if (StreamNumber >= kMaxStreams) return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); return D3D_OK; }
  auto &stream = streams_[StreamNumber];
  /* Unbinding a stream leaves its offset and stride behind: D3D9 ignores both
   * arguments when the buffer is null, and GetStreamSource still reports the
   * stride the stream was last set with. */
  if (!pStreamData) {
    stream.buffer = nullptr;
    return D3D_OK;
  }
  stream.buffer = static_cast<D3D9VertexBuffer *>(pStreamData);
  stream.offset = OffsetInBytes;
  stream.stride = Stride;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) {
  auto device_lock = LockDevice();
  if (!ppStreamData) return D3DERR_INVALIDCALL;
  *ppStreamData = nullptr;
  if (StreamNumber >= kMaxStreams) return D3DERR_INVALIDCALL;
  auto &s = streams_[StreamNumber];
  *ppStreamData = s.buffer ? ref(s.buffer.ptr()) : nullptr;
  if (pOffsetInBytes) *pOffsetInBytes = s.offset;
  if (pStride) *pStride = s.stride;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
  auto device_lock = LockDevice();
  if (StreamNumber >= kMaxStreams) return D3DERR_INVALIDCALL;
  /* Both flags on one stream are invalid, per-instance data cannot be stream
   * 0's, and a divider of zero is no divider. INDEXEDDATA is fine anywhere. */
  if ((Setting & D3DSTREAMSOURCE_INDEXEDDATA) && (Setting & D3DSTREAMSOURCE_INSTANCEDATA))
    return D3DERR_INVALIDCALL;
  if ((Setting & D3DSTREAMSOURCE_INSTANCEDATA) && StreamNumber == 0)
    return D3DERR_INVALIDCALL;
  if (!Setting)
    return D3DERR_INVALIDCALL;
  if (recording_) { recording_->RecordStreamFreq(StreamNumber, Setting); return D3D_OK; }
  stream_freq_[StreamNumber] = Setting;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) {
  auto device_lock = LockDevice();
  if (!pSetting || StreamNumber >= kMaxStreams) return D3DERR_INVALIDCALL;
  *pSetting = stream_freq_[StreamNumber] ? stream_freq_[StreamNumber] : 1;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
  auto device_lock = LockDevice();
  if (recording_) { recording_->RecordIndices(pIndexData); return D3D_OK; }
  index_buffer_ = static_cast<D3D9IndexBuffer *>(pIndexData);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
D3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) {
  auto device_lock = LockDevice();
  if (!ppIndexData) return D3DERR_INVALIDCALL;
  *ppIndexData = index_buffer_ ? ref(index_buffer_.ptr()) : nullptr;
  return D3D_OK;
}

// --- draw pipeline plumbing ---------------------------------------------------

/* The declaration element feeding a vertex shader input. D3D9 links a
 * declaration element to a shader input by usage and usage index, never by
 * position: a declaration routinely carries elements a given shader does not
 * read (a shadow pass sharing the main pass's vertex format), or lists them in
 * a different order than the shader's dcls. */
static const D3DVERTEXELEMENT9 *
FindDeclarationElement(const std::vector<D3DVERTEXELEMENT9> &elements, const sm3::VertexInputSlot &input) {
  for (auto &e : elements) {
    if (e.Stream == 0xFF)
      break; /* D3DDECL_END */
    if (e.Type == D3DDECLTYPE_UNUSED)
      continue;
    BYTE usage = e.Usage == D3DDECLUSAGE_POSITIONT ? (BYTE)D3DDECLUSAGE_POSITION : e.Usage;
    if (usage == input.d3d9_usage && e.UsageIndex == input.d3d9_usage_index)
      return &e;
  }
  return nullptr;
}

/* What the bound vertex shader has to convert its inputs from, for the
 * declaration bound with it. Kept for the last pair: most draws repeat it. */
uint32_t
D3D9Device::VertexInputConversions() {
  auto *decl = CurrentDeclaration();
  if (!decl || !vertex_shader_)
    return 0;
  if (decl->UniqueId() == conversions_declaration_ && vertex_shader_->UniqueId() == conversions_shader_)
    return conversions_;
  uint32_t conversions = 0;
  for (auto &in : vertex_shader_->VertexInputs())
    if (auto *match = in.reg < 16 ? FindDeclarationElement(decl->Elements(), in) : nullptr)
      conversions |= GetD3D9DeclTypeConversion(match->Type) << (in.reg * 2);
  conversions_declaration_ = decl->UniqueId();
  conversions_shader_ = vertex_shader_->UniqueId();
  conversions_ = conversions;
  return conversions;
}

HRESULT
D3D9Device::BindInputLayout() {
  auto *decl = CurrentDeclaration();
  if (!decl || !vertex_shader_) return D3DERR_INVALIDCALL;

  InputLayoutKey key = {decl->UniqueId(), vertex_shader_->UniqueId(), false, {}};
  for (UINT i = 0; i < kMaxStreams; i++)
    key.stream_freq[i] = stream_freq_[i]; // instancing flags change the layout
  auto it = input_layout_cache_.find(key);
  if (it != input_layout_cache_.end()) {
    UseInputLayout(it->second);
    return D3D_OK;
  }

  /* D3D9 links a declaration element to a shader input by usage and usage
   * index, never by position: a declaration routinely carries elements a
   * given shader does not read (a shadow pass sharing the main pass's
   * vertex format), or lists them in a different order than the shader's
   * dcls. Matching positionally fed such inputs the wrong element. The
   * shader-side semantic is the translator's internal TEXCOORD<v#>. */
  auto &elements = decl->Elements();
  auto &vs_inputs = vertex_shader_->VertexInputs();
  std::vector<D3D11_INPUT_ELEMENT_DESC> descs;
  descs.reserve(vs_inputs.size());
  bool null_stream = false;
  for (auto &in : vs_inputs) {
    const D3DVERTEXELEMENT9 *match = FindDeclarationElement(elements, in);
    D3D11_INPUT_ELEMENT_DESC d = {};
    d.SemanticName = in.semantic.c_str();
    d.SemanticIndex = in.semantic_index;
    if (!match) {
      d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      d.InputSlot = kNullStreamSlot;
      d.AlignedByteOffset = 0;
      d.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
      null_stream = true;
      descs.push_back(d);
      continue;
    }
    d.Format = GetD3D9DeclTypeFormat(match->Type);
    d.InputSlot = match->Stream;
    d.AlignedByteOffset = match->Offset;
    UINT freq = match->Stream < kMaxStreams ? stream_freq_[match->Stream] : 1;
    if (freq & D3DSTREAMSOURCE_INSTANCEDATA) {
      d.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
      d.InstanceDataStepRate = std::max(1u, freq & 0x3FFFFFFFu);
    } else {
      d.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
      d.InstanceDataStepRate = 0;
    }
    descs.push_back(d);
  }

  auto &dxbc = vertex_shader_->TranslatedDXBC();
  CachedInputLayout cached;
  cached.null_stream = null_stream;
  HRESULT hr = d3d11_device_->CreateInputLayout(
      descs.data(), (UINT)descs.size(), dxbc.data(), dxbc.size(), &cached.layout);
  if (FAILED(hr)) return hr;
  UseInputLayout(input_layout_cache_.emplace(key, std::move(cached)).first->second);
  return D3D_OK;
}

UINT
D3D9Device::InstancedDrawCount() const {
  // SetStreamSourceFreq(n, D3DSTREAMSOURCE_INDEXEDDATA | N) turns indexed
  // draws into N-instance draws; the geometry stream is usually 0 but need
  // not be.
  for (UINT i = 0; i < kMaxStreams; i++)
    if (stream_freq_[i] & D3DSTREAMSOURCE_INDEXEDDATA)
      return std::max(1u, stream_freq_[i] & 0x3FFFFFFFu);
  return 0;
}

bool
D3D9Device::EnsureFanIndexBuffer(UINT primCount) {
  if (fan_index_capacity_ >= primCount && fan_index_buffer_)
    return true;
  UINT capacity = std::max(primCount, std::max(1024u, fan_index_capacity_ * 2));
  std::vector<uint32_t> indices(capacity * 3);
  for (UINT i = 0; i < capacity; i++) {
    indices[i * 3 + 0] = 0;
    indices[i * 3 + 1] = i + 1;
    indices[i * 3 + 2] = i + 2;
  }
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = capacity * 3 * sizeof(uint32_t);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  D3D11_SUBRESOURCE_DATA init = {indices.data()};
  Com<ID3D11Buffer> buf;
  if (FAILED(d3d11_device_->CreateBuffer(&bd, &init, &buf)))
    return false;
  fan_index_buffer_ = std::move(buf);
  fan_index_capacity_ = capacity;
  return true;
}

void
D3D9Device::UploadShaderConstants(bool programmable_vs, bool programmable_ps) {
  constexpr UINT kRingBytes = 64 * 1024;
  constexpr UINT kExtraCBBytes = (sm3::kExtraCBSizeVec4 * 16 + 255) & ~255u;
  auto ensure = [&](Com<ID3D11Buffer> &buf, UINT bytes) {
    if (buf) return;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d3d11_device_->CreateBuffer(&bd, nullptr, &buf);
  };
  UINT float_bytes = constant_context_ ? kRingBytes : sizeof(vs_const_f_);
  if (programmable_vs) {
    ensure(vs_cbuffer_, float_bytes);
    ensure(vs_cbuffer_extra_, kExtraCBBytes);
  }
  if (programmable_ps) {
    ensure(ps_cbuffer_, float_bytes);
    ensure(ps_cbuffer_extra_, kExtraCBBytes);
  }

  auto upload = [&](ID3D11Buffer *buf, const void *data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(d3d11_ctx_->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, data, size);
    d3d11_ctx_->Unmap(buf, 0);
    return true;
  };

  auto upload_float = [&](ID3D11Buffer *buf, ConstantUpload &state, bool &dirty,
                          const float *values, UINT used) {
    // A later shader may read more registers without any SetConstantF call.
    used = std::clamp(used, 1u, kMaxFloatConstants);
    if ((!dirty && used <= state.uploaded) || !buf) return;
    UINT bytes = (used * 16 + 255) & ~255u;
    UINT offset = 0;
    D3D11_MAP mode = D3D11_MAP_WRITE_DISCARD;
    if (constant_context_ && state.cursor + bytes <= kRingBytes && state.cursor) {
      offset = state.cursor;
      mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(d3d11_ctx_->Map(buf, 0, mode, 0, &mapped))) return;
    auto destination = static_cast<uint8_t *>(mapped.pData) + offset;
    memcpy(destination, values, used * 16);
    memset(destination + used * 16, 0, bytes - used * 16);
    d3d11_ctx_->Unmap(buf, 0);
    state.first = offset / 16;
    state.count = bytes / 16;
    state.cursor = offset + bytes;
    state.uploaded = used;
    dirty = false;
  };
  // Separate rings prevent a VS wrap from discarding an unchanged PS snapshot.
  if (programmable_vs)
    upload_float(vs_cbuffer_.ptr(), vs_upload_, vs_const_dirty_, vs_const_f_, vertex_shader_->FloatConstantsUsed());
  if (programmable_ps)
    upload_float(ps_cbuffer_.ptr(), ps_upload_, ps_const_dirty_, ps_const_f_, pixel_shader_->FloatConstantsUsed());

  // cb1 layout (see sm3_translate.hpp): 16 int4 registers, 16 bool registers
  // (0/1 in .x), then one misc vec4 whose .x is the alpha-test reference.
  auto fillExtra = [&](uint8_t *out, const int *ints, const uint32_t *bools) {
    memset(out, 0, kExtraCBBytes);
    memcpy(out + sm3::kExtraCBIntBase * 16, ints, kMaxOtherConstants * 4 * sizeof(int));
    for (UINT i = 0; i < kMaxOtherConstants; i++)
      memcpy(out + (sm3::kExtraCBBoolBase + i) * 16, &bools[i], sizeof(uint32_t));
    float alpha_ref = (render_state_[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
    memcpy(out + sm3::kExtraCBMiscElement * 16, &alpha_ref, sizeof(float));

    // Fog colour, for the epilogue a user pixel shader gets when fog is on.
    DWORD fog = render_state_[D3DRS_FOGCOLOR];
    float fog_rgba[4] = {
        ((fog >> 16) & 0xFF) / 255.0f, ((fog >> 8) & 0xFF) / 255.0f,
        (fog & 0xFF) / 255.0f, 1.0f,
    };
    memcpy(out + sm3::kExtraCBFogColor * 16, fog_rgba, sizeof(fog_rgba));

    // And the table-mode parameters, which the shader needs when it computes
    // the factor itself rather than reading it off the FOG input.
    auto as_float = [](DWORD bits) { float v; memcpy(&v, &bits, sizeof(v)); return v; };
    float fog_params[4] = {
        as_float(render_state_[D3DRS_FOGSTART]), as_float(render_state_[D3DRS_FOGEND]),
        as_float(render_state_[D3DRS_FOGDENSITY]), 0.0f,
    };
    memcpy(out + sm3::kExtraCBFogParams * 16, fog_params, sizeof(fog_params));

    float point_size[4] = {
        as_float(render_state_[D3DRS_POINTSIZE]), as_float(render_state_[D3DRS_POINTSIZE_MIN]),
        std::min(as_float(render_state_[D3DRS_POINTSIZE_MAX]), kMaxPointSize), 0.0f,
    };
    memcpy(out + sm3::kExtraCBPointSize * 16, point_size, sizeof(point_size));
    float depth_bias = as_float(render_state_[D3DRS_DEPTHBIAS]);
    memcpy(out + sm3::kExtraCBDepthBias * 16, &depth_bias, sizeof(depth_bias));
    // The user clip planes as given -- clip space, for a programmable vertex
    // stage -- zero for those not enabled.
    if (UserClipPlanesEnabled()) {
      for (UINT i = 0; i < kMaxClipPlanes; i++) {
        if (render_state_[D3DRS_CLIPPLANEENABLE] & (1u << i))
          memcpy(out + (sm3::kExtraCBClipPlanes + i) * 16, clip_planes_[i], 4 * sizeof(float));
      }
    }

    // Bump environment state. These are DWORD-typed stage states holding float
    // bit patterns, which is how D3D9 passes floats through SetTextureStageState.
    auto stage_float = [&](UINT stage, D3DTEXTURESTAGESTATETYPE type) {
      float value;
      DWORD bits = tss_[stage][type];
      memcpy(&value, &bits, sizeof(value));
      return value;
    };
    for (UINT stage = 0; stage < sm3::kExtraCBBumpStages && stage < kMaxTextureStages; stage++) {
      float matrix[4] = {stage_float(stage, D3DTSS_BUMPENVMAT00), stage_float(stage, D3DTSS_BUMPENVMAT01),
                         stage_float(stage, D3DTSS_BUMPENVMAT10), stage_float(stage, D3DTSS_BUMPENVMAT11)};
      float luminance[4] = {stage_float(stage, D3DTSS_BUMPENVLSCALE), stage_float(stage, D3DTSS_BUMPENVLOFFSET), 0,
                            0};
      size_t base = (sm3::kExtraCBBumpBase + stage * sm3::kExtraCBBumpStride) * 16;
      memcpy(out + base, matrix, sizeof(matrix));
      memcpy(out + base + 16, luminance, sizeof(luminance));
    }
  };
  if (programmable_vs && vs_extra_dirty_ && vs_cbuffer_extra_) {
    uint8_t staging[kExtraCBBytes];
    fillExtra(staging, vs_const_i_, vs_const_b_);
    if (upload(vs_cbuffer_extra_.ptr(), staging, sizeof(staging))) vs_extra_dirty_ = false;
  }
  if (programmable_ps && ps_extra_dirty_ && ps_cbuffer_extra_) {
    uint8_t staging[kExtraCBBytes];
    fillExtra(staging, ps_const_i_, ps_const_b_);
    if (upload(ps_cbuffer_extra_.ptr(), staging, sizeof(staging))) ps_extra_dirty_ = false;
  }

  auto bind = [&](bool vertex, ID3D11Buffer *floats, ID3D11Buffer *extra, const ConstantUpload &state) {
    ID3D11Buffer *buffers[2] = {floats, extra};
    if (constant_context_) {
      UINT first[2] = {state.first, 0};
      UINT count[2] = {state.count, kExtraCBBytes / 16};
      if (vertex) constant_context_->VSSetConstantBuffers1(0, 2, buffers, first, count);
      else constant_context_->PSSetConstantBuffers1(0, 2, buffers, first, count);
    } else {
      if (vertex) d3d11_ctx_->VSSetConstantBuffers(0, 2, buffers);
      else d3d11_ctx_->PSSetConstantBuffers(0, 2, buffers);
    }
  };
  if (programmable_vs) bind(true, vs_cbuffer_.ptr(), vs_cbuffer_extra_.ptr(), vs_upload_);
  if (programmable_ps) bind(false, ps_cbuffer_.ptr(), ps_cbuffer_extra_.ptr(), ps_upload_);
}

/* The private reference that keeps a bound texture alive. Reached through
 * the resource type for the same reason as the SRV below: the three texture
 * classes share no concrete base. */
static D3D9PrivateRef *
GetD3D9BaseTexturePrivateRef(IDirect3DBaseTexture9 *tex) {
  if (!tex) return nullptr;
  switch (D3D9TextureType(tex)) {
    case D3DRTYPE_TEXTURE:       return static_cast<D3D9Texture *>(tex);
    case D3DRTYPE_CUBETEXTURE:   return static_cast<D3D9CubeTexture *>(tex);
    case D3DRTYPE_VOLUMETEXTURE: return static_cast<D3D9VolumeTexture *>(tex);
    default:                     return nullptr;
  }
}

void
D3D9Device::TextureBinding::Set(IDirect3DBaseTexture9 *tex) {
  /* Constructed first so the new reference is taken before the old one is
   * dropped, which matters when the same texture is rebound. */
  owner = Com<D3D9PrivateRef, false>(GetD3D9BaseTexturePrivateRef(tex));
  texture = tex;
}

/* D3D9 samples a texture of these depth formats as a shadow map: the result
 * is the coordinate's depth compared with the stored one. INTZ, DF16 and DF24
 * exist to read the depth itself, and do not compare. */
static bool
IsShadowTexture(IDirect3DBaseTexture9 *tex) {
  if (!tex || D3D9TextureType(tex) != D3DRTYPE_TEXTURE)
    return false;
  switch (static_cast<D3D9Texture *>(tex)->Format()) {
  case D3DFMT_D16_LOCKABLE: case D3DFMT_D32: case D3DFMT_D15S1: case D3DFMT_D24S8: case D3DFMT_D24X8:
  case D3DFMT_D24X4S4: case D3DFMT_D16: case D3DFMT_D32F_LOCKABLE: case D3DFMT_D24FS8:
    return true;
  default:
    return false;
  }
}

static ID3D11ShaderResourceView *
GetD3D9BaseTextureSRV(IDirect3DBaseTexture9 *tex, bool SRGB) {
  if (!tex) return nullptr;
  switch (D3D9TextureType(tex)) {
    case D3DRTYPE_TEXTURE:       return static_cast<D3D9Texture *>(tex)->GetSRV(SRGB);
    case D3DRTYPE_CUBETEXTURE:   return static_cast<D3D9CubeTexture *>(tex)->GetSRV(SRGB);
    case D3DRTYPE_VOLUMETEXTURE: return static_cast<D3D9VolumeTexture *>(tex)->GetSRV(SRGB);
    default:                     return nullptr;
  }
}

void
D3D9Device::InitDefaultSamplerState() {
  for (UINT s = 0; s < kTotalSamplers; s++) {
    memset(sampler_state_[s], 0, sizeof(sampler_state_[s]));
    sampler_state_[s][D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
    sampler_state_[s][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
    sampler_state_[s][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
    sampler_state_[s][D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
    sampler_state_[s][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
    sampler_state_[s][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
    sampler_state_[s][D3DSAMP_MAXANISOTROPY] = 1;
  }
  samplers_dirty_ = kAllSamplers;
}

ID3D11SamplerState *
D3D9Device::GetOrCreateSampler(UINT stage, bool comparison) {
  const DWORD *ss = sampler_state_[stage];

  auto addressMode = [](DWORD v) {
    // D3DTADDRESS_* and D3D11_TEXTURE_ADDRESS_* share their numeric values.
    if (v < D3DTADDRESS_WRAP || v > D3DTADDRESS_MIRRORONCE) return D3D11_TEXTURE_ADDRESS_WRAP;
    return (D3D11_TEXTURE_ADDRESS_MODE)v;
  };

  DWORD minf = ss[D3DSAMP_MINFILTER], magf = ss[D3DSAMP_MAGFILTER], mipf = ss[D3DSAMP_MIPFILTER];
  D3D11_FILTER filter;
  if (minf == D3DTEXF_ANISOTROPIC || magf == D3DTEXF_ANISOTROPIC)
    filter = D3D11_FILTER_ANISOTROPIC;
  else
    filter = (D3D11_FILTER)((minf >= D3DTEXF_LINEAR ? 0x10 : 0) |
                            (magf >= D3DTEXF_LINEAR ? 0x04 : 0) |
                            (mipf >= D3DTEXF_LINEAR ? 0x01 : 0));

  if (comparison) // shadow maps: pass when the reference is at most the depth
    filter = (D3D11_FILTER)(filter | 0x80);

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = filter;
  sd.ComparisonFunc = comparison ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_NEVER;
  sd.AddressU = addressMode(ss[D3DSAMP_ADDRESSU]);
  sd.AddressV = addressMode(ss[D3DSAMP_ADDRESSV]);
  sd.AddressW = addressMode(ss[D3DSAMP_ADDRESSW]);
  /* 'GET4' and 'GET1' switch ATI Fetch4 on and off through this state; they
   * are no bias. */
  DWORD lod_bias = ss[D3DSAMP_MIPMAPLODBIAS] == kD3D9Fetch4On || ss[D3DSAMP_MIPMAPLODBIAS] == kD3D9Fetch4Off
                       ? 0
                       : ss[D3DSAMP_MIPMAPLODBIAS];
  memcpy(&sd.MipLODBias, &lod_bias, sizeof(float));
  sd.MaxAnisotropy = std::max<UINT>(1, ss[D3DSAMP_MAXANISOTROPY]);
  // D3DSAMP_MAXMIPLEVEL is the most-detailed mip the sampler may touch
  // (larger index = smaller mip), which is exactly D3D11's MinLOD.
  sd.MinLOD = (float)ss[D3DSAMP_MAXMIPLEVEL];
  // MIPFILTER NONE disables mipmapping altogether: clamp to the top level.
  sd.MaxLOD = mipf == D3DTEXF_NONE ? sd.MinLOD : D3D11_FLOAT32_MAX;
  DWORD border = ss[D3DSAMP_BORDERCOLOR];
  sd.BorderColor[0] = ((border >> 16) & 0xFF) / 255.0f;
  sd.BorderColor[1] = ((border >> 8) & 0xFF) / 255.0f;
  sd.BorderColor[2] = ((border >> 0) & 0xFF) / 255.0f;
  sd.BorderColor[3] = ((border >> 24) & 0xFF) / 255.0f;

  std::array<uint32_t, 10> key = {
    (uint32_t)sd.Filter, (uint32_t)sd.AddressU, (uint32_t)sd.AddressV, (uint32_t)sd.AddressW,
    lod_bias, sd.MaxAnisotropy, ss[D3DSAMP_MAXMIPLEVEL],
    border, mipf == D3DTEXF_NONE, comparison,
  };
  auto it = sampler_cache_.find(key);
  if (it != sampler_cache_.end())
    return it->second.ptr();

  Com<ID3D11SamplerState> sampler;
  if (FAILED(d3d11_device_->CreateSamplerState(&sd, &sampler)))
    return nullptr;
  sampler_cache_.emplace(key, sampler);
  return sampler.ptr();
}

void
D3D9Device::BindTexturesAndSamplers() {
  // A depth texture can change a slot to a comparison sampler without any
  // SetSamplerState call. Account for both sources of sampler changes.
  uint32_t dirty = samplers_dirty_ | (shadow_samplers_ ^ resolved_shadow_samplers_);
  samplers_dirty_ = 0;
  resolved_shadow_samplers_ = shadow_samplers_;
  while (dirty) {
    UINT slot = std::countr_zero(dirty);
    uint32_t bit = 1u << slot;
    dirty &= ~bit;
    resolved_samplers_[slot] = GetOrCreateSampler(slot, slot < kMaxSamplers && ((shadow_samplers_ >> slot) & 1));
    if (!resolved_samplers_[slot])
      samplers_dirty_ |= bit; // Retry a failed creation on the next draw.
  }

  ID3D11ShaderResourceView *srvs[kMaxSamplers];
  for (UINT i = 0; i < kMaxSamplers; i++) {
    /* D3DSAMP_SRGBTEXTURE selects which of the texture's two views to read
     * through. It is a sampler state in D3D9 and a view format everywhere
     * below, so it is resolved here rather than in the sampler. */
    srvs[i] = GetD3D9BaseTextureSRV(textures_[i].texture, sampler_state_[i][D3DSAMP_SRGBTEXTURE] != 0);
  }
  d3d11_ctx_->PSSetShaderResources(0, kMaxSamplers, srvs);
  d3d11_ctx_->PSSetSamplers(0, kMaxSamplers, resolved_samplers_.data());

  // D3D9's four vertex-texture samplers (D3DVERTEXTEXTURESAMPLER0..3) map to
  // the first VS resource/sampler slots.
  ID3D11ShaderResourceView *vs_srvs[kMaxVertexSamplers];
  for (UINT i = 0; i < kMaxVertexSamplers; i++) {
    vs_srvs[i] = GetD3D9BaseTextureSRV(textures_[kMaxSamplers + i].texture,
                                       sampler_state_[kMaxSamplers + i][D3DSAMP_SRGBTEXTURE] != 0);
  }
  d3d11_ctx_->VSSetShaderResources(0, kMaxVertexSamplers, vs_srvs);
  // Keep rebinding the resolved objects: StretchRect temporarily replaces
  // context bindings, and render-target changes may unbind conflicting SRVs.
  d3d11_ctx_->VSSetSamplers(0, kMaxVertexSamplers, resolved_samplers_.data() + kMaxSamplers);
}

HRESULT
D3D9Device::BindInputLayoutFFP(D3D9VertexDeclaration *decl, FFPShaders *shaders, const FFPShaderKey &key) {
  /* The FFP cache never frees a shader set, so its address identifies the
   * generated shaders -- and through them the key -- for the device's life. */
  InputLayoutKey cache_key = {decl->UniqueId(), (uint64_t)(uintptr_t)shaders, true, {}};
  auto it = input_layout_cache_.find(cache_key);
  if (it != input_layout_cache_.end()) {
    UseInputLayout(it->second);
    return D3D_OK;
  }
  auto &elements = decl->Elements();

  // The generated FFP shader declares its inputs by (usage-derived semantic,
  // usage index); match declaration elements by that pair rather than by
  // position, since the generator's input order is fixed.
  auto findElement = [&](const FFPVertexInput &in) -> const D3DVERTEXELEMENT9 * {
    for (auto &e : elements) {
      if (e.Type == D3DDECLTYPE_UNUSED) continue;
      const char *sem;
      switch (e.Usage) {
      case D3DDECLUSAGE_POSITION: case D3DDECLUSAGE_POSITIONT: sem = "POSITION"; break;
      case D3DDECLUSAGE_NORMAL: sem = "NORMAL"; break;
      case D3DDECLUSAGE_COLOR: sem = "COLOR"; break;
      case D3DDECLUSAGE_TEXCOORD: sem = "TEXCOORD"; break;
      case D3DDECLUSAGE_PSIZE: sem = "PSIZE"; break;
      default: continue;
      }
      if (in.semantic == sem && in.semantic_index == e.UsageIndex)
        return &e;
    }
    return nullptr;
  };

  std::vector<D3D11_INPUT_ELEMENT_DESC> descs;
  for (auto &in : shaders->vs_inputs) {
    const D3DVERTEXELEMENT9 *e = findElement(in);
    if (!e) return D3DERR_INVALIDCALL; // shader consumes an input the declaration lacks
    D3D11_INPUT_ELEMENT_DESC d = {};
    d.SemanticName = in.semantic.c_str();
    d.SemanticIndex = in.semantic_index;
    d.Format = GetD3D9DeclTypeFormat(e->Type);
    d.InputSlot = e->Stream;
    d.AlignedByteOffset = e->Offset;
    d.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    descs.push_back(d);
  }

  CachedInputLayout cached;
  HRESULT hr = d3d11_device_->CreateInputLayout(
      descs.data(), (UINT)descs.size(), shaders->vs_dxbc.data(), shaders->vs_dxbc.size(), &cached.layout);
  if (FAILED(hr)) return hr;
  UseInputLayout(input_layout_cache_.emplace(cache_key, std::move(cached)).first->second);
  return D3D_OK;
}

void
D3D9Device::UploadFFPConstants(const FFPShaderKey &key, bool bind_vs, bool bind_ps) {
  auto ensure = [&](Com<ID3D11Buffer> &buf, UINT bytes) {
    if (buf) return;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d3d11_device_->CreateBuffer(&bd, nullptr, &buf);
  };
  ensure(ffp_vs_cbuffer_, ffp::kVSSizeVec4 * 16);
  ensure(ffp_ps_cbuffer_, ffp::kPSSizeVec4 * 16);

  // Refilled every FFP draw: the inputs (transforms, lights, material,
  // texture-stage constants) have no dirty tracking of their own yet.
  float vs_data[ffp::kVSSizeVec4 * 4];
  float ps_data[ffp::kPSSizeVec4 * 4];
  ffp_->FillConstants(key, vs_data, ps_data);

  D3D11_MAPPED_SUBRESOURCE m = {};
  if (bind_vs && ffp_vs_cbuffer_ && SUCCEEDED(d3d11_ctx_->Map(ffp_vs_cbuffer_.ptr(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    memcpy(m.pData, vs_data, sizeof(vs_data));
    d3d11_ctx_->Unmap(ffp_vs_cbuffer_.ptr(), 0);
  }
  if (bind_ps && ffp_ps_cbuffer_ && SUCCEEDED(d3d11_ctx_->Map(ffp_ps_cbuffer_.ptr(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    memcpy(m.pData, ps_data, sizeof(ps_data));
    d3d11_ctx_->Unmap(ffp_ps_cbuffer_.ptr(), 0);
  }
  if (bind_vs) {
    ID3D11Buffer *cb = ffp_vs_cbuffer_.ptr();
    d3d11_ctx_->VSSetConstantBuffers(0, 1, &cb);
  }
  if (bind_ps) {
    ID3D11Buffer *cb = ffp_ps_cbuffer_.ptr();
    d3d11_ctx_->PSSetConstantBuffers(0, 1, &cb);
  }
}

HRESULT
D3D9Device::DrawPrepare(D3DPRIMITIVETYPE type) {
  /* Timed only under DXMT_FPS=1: a clock read is a syscall under Wine
   * (~350 ns on an M4), and this runs on every draw. */
  struct DiagEnd {
    std::chrono::steady_clock::time_point t;
    double *acc;
    ~DiagEnd() {
      if (acc)
        *acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
    }
  } diag_end = fps_report_ ? DiagEnd{std::chrono::steady_clock::now(), &diag_prepare_s_} : DiagEnd{{}, nullptr};
  /* Pre-transformed vertices (POSITIONT, D3DFVF_XYZRHW) skip vertex
   * processing altogether: a vertex shader left bound does not run on them. */
  bool pretransformed = false;
  if (vertex_shader_)
    if (auto *elements = CurrentDeclarationElements())
      for (auto &e : *elements)
        pretransformed |= e.Usage == D3DDECLUSAGE_POSITIONT && e.Type != D3DDECLTYPE_UNUSED;
  bool ffp_vs = !vertex_shader_ || pretransformed;
  bool ffp_ps = !pixel_shader_;
  /* D3D10 and later rasterise every point a pixel wide; D3D9 sizes them, and
   * with point sprites maps a texture across each. Both need shader variants. */
  bool points = type == D3DPT_POINTLIST;
  bool point_sprite = points && render_state_[D3DRS_POINTSPRITEENABLE];

  /* The pixel stage is chosen first: what it reads decides what the vertex
   * stage has to write. D3D9 links the two by semantic, the SM4 they become
   * by register, so every varying sits in the link register of its semantic
   * (sm3::kLinkColor0 and on), and a varying the pixel stage reads that the
   * vertex stage never writes gets a vertex variant writing D3D9's default
   * for it -- Metal will not link a fragment input nothing feeds.
   *
   * D3D11 has no fixed-function alpha test or fog either; a programmable
   * pixel shader gets a variant with both baked in. Fog is D3D9's
   * post-shader blend and only exists below ps_3_0. A table mode computes
   * the factor per pixel from the depth (clip-space w under a perspective
   * projection); otherwise the factor is the FOG varying: oFog from a
   * programmable vertex shader -- whatever FOGVERTEXMODE says, and zero if it
   * never writes it -- or what the fixed-function vertex shader computes. */
  ID3D11PixelShader *ps = nullptr;
  uint32_t ps_inputs = 0;
  shadow_samplers_ = 0;
  if (!ffp_ps) {
    /* What the samplers are bound to: depth textures read as shadow maps, and
     * below ps_2_0, which has no sampler dcls, the texture type. */
    uint32_t sampler_dims = 0;
    uint16_t fetch4 = 0, fetch4_alpha = 0;
    for (UINT s = 0; s < 16; s++) {
      IDirect3DBaseTexture9 *tex = textures_[s].texture;
      if (!tex) continue;
      if (IsShadowTexture(tex)) shadow_samplers_ |= 1u << s;
      int channel = D3D9Fetch4Channel(tex, sampler_state_[s][D3DSAMP_MIPMAPLODBIAS]);
      if (channel >= 0) {
        fetch4 |= 1u << s;
        if (channel == 3) fetch4_alpha |= 1u << s;
      }
      if (pixel_shader_->MajorVersion() < 2) {
        D3DRESOURCETYPE type = D3D9TextureType(tex);
        sampler_dims |= (type == D3DRTYPE_CUBETEXTURE ? 1u : type == D3DRTYPE_VOLUMETEXTURE ? 2u : 0u) << (s * 2);
      }
    }
    uint32_t alpha_func = render_state_[D3DRS_ALPHATESTENABLE] ? render_state_[D3DRS_ALPHAFUNC] : 0;
    bool fog_on = render_state_[D3DRS_FOGENABLE] && pixel_shader_->MajorVersion() < 3;
    uint32_t fog_table_mode = fog_on ? render_state_[D3DRS_FOGTABLEMODE] : 0;
    bool fog_w = false;
    if (fog_table_mode) {
      D3DMATRIX proj;
      GetTransform(D3DTS_PROJECTION, &proj);
      fog_w = !(proj._14 == 0.0f && proj._24 == 0.0f && proj._34 == 0.0f && proj._44 == 1.0f);
    }
    ps = alpha_func || fog_on || point_sprite || sampler_dims || shadow_samplers_ || fetch4
             ? pixel_shader_->GetD3D11ShaderVariant(alpha_func, fog_on, fog_table_mode, fog_w, point_sprite,
                                                    sampler_dims, shadow_samplers_, fetch4, fetch4_alpha)
             : pixel_shader_->GetD3D11Shader();
    ps_inputs = pixel_shader_->LinkInputs();
    if (fog_on && !fog_table_mode)
      ps_inputs |= sm3::LinkBit(sm3::kLinkFog);
  }

  FFPShaders *ffp_shaders = nullptr;
  FFPShaderKey ffp_key = {};
  if (ffp_vs || ffp_ps) {
    if (!ffp_)
      ffp_ = std::make_unique<FFPShaderCache>(this);
    auto *elements = CurrentDeclarationElements();
    if (ffp_vs && !elements) return D3DERR_INVALIDCALL;
    static const std::vector<D3DVERTEXELEMENT9> kNoElements;
    ffp_key = ffp_->BuildKey(elements ? *elements : kNoElements, ffp_vs, ffp_ps, ps_inputs, points);
    ffp_shaders = ffp_->Get(ffp_key);
    if (!ffp_shaders) return D3DERR_INVALIDCALL;
    if (ffp_ps)
      ps = ffp_shaders->ps.ptr();
  }

  ID3D11VertexShader *vs;
  if (ffp_vs) {
    vs = ffp_shaders->vs.ptr();
  } else {
    uint32_t missing = ps_inputs & ~vertex_shader_->LinkOutputs();
    // An unwritten diffuse colour is white to a fixed-function or ps<3.0 stage.
    uint32_t white = ffp_ps || pixel_shader_->MajorVersion() < 3 ? sm3::LinkBit(sm3::kLinkColor0) : 0;
    bool clip = UserClipPlanesEnabled();
    uint32_t conversions = VertexInputConversions();
    vs = missing || points || clip || conversions
             ? vertex_shader_->GetD3D11OutputVariant(missing, white, points, clip, conversions)
             : vertex_shader_->GetD3D11Shader();
  }

  HRESULT hr = ffp_vs ? BindInputLayoutFFP(CurrentDeclaration(), ffp_shaders, ffp_key)
                      : BindInputLayout();
  if (FAILED(hr)) return hr;

  d3d11_ctx_->VSSetShader(vs, nullptr, 0);
  d3d11_ctx_->PSSetShader(ps, nullptr, 0);
  if (unlikely(fps_report_)) {
    auto t = std::chrono::steady_clock::now();
    UploadShaderConstants(!ffp_vs, !ffp_ps);
    diag_upload_s_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
  } else {
    UploadShaderConstants(!ffp_vs, !ffp_ps);
  }
  if (ffp_vs || ffp_ps)
    UploadFFPConstants(ffp_key, ffp_vs, ffp_ps);
  if (unlikely(fps_report_)) {
    auto t = std::chrono::steady_clock::now();
    BindTexturesAndSamplers();
    diag_bind_s_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
  } else {
    BindTexturesAndSamplers();
  }
  ApplyRenderState();

  for (UINT i = 0; i < kMaxStreams; i++) {
    if (!streams_[i].buffer) continue;
    ID3D11Buffer *buf = streams_[i].buffer->GetD3D11Buffer();
    UINT stride = streams_[i].stride, offset = streams_[i].offset;
    d3d11_ctx_->IASetVertexBuffers(i, 1, &buf, &stride, &offset);
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
  auto device_lock = LockDevice();
  draws_since_report_++;
  if (!PrimitiveCount) return D3DERR_INVALIDCALL;
  HRESULT hr = DrawPrepare(PrimitiveType);
  if (FAILED(hr)) return hr;

  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    // D3D11 dropped fans: draw (0, i+1, i+2) triangles through the shared
    // fan->list index buffer, offset to the fan's first vertex.
    if (!EnsureFanIndexBuffer(PrimitiveCount)) return D3DERR_INVALIDCALL;
    d3d11_ctx_->IASetIndexBuffer(fan_index_buffer_.ptr(), DXGI_FORMAT_R32_UINT, 0);
    d3d11_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11_ctx_->DrawIndexed(PrimitiveCount * 3, 0, StartVertex);
    return D3D_OK;
  }

  D3D11_PRIMITIVE_TOPOLOGY topology;
  if (!ToD3D11Topology(PrimitiveType, &topology)) return D3DERR_INVALIDCALL;
  d3d11_ctx_->IASetPrimitiveTopology(topology);
  d3d11_ctx_->Draw(PrimitiveVertexCount(PrimitiveType, PrimitiveCount), StartVertex);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT /*MinVertexIndex*/, UINT /*NumVertices*/,
    UINT StartIndex, UINT PrimitiveCount) {
  auto device_lock = LockDevice();
  draws_since_report_++;
  if (!PrimitiveCount || !index_buffer_) return D3DERR_INVALIDCALL;
  auto *ib = index_buffer_.ptr();
  D3DINDEXBUFFER_DESC ibd;
  ib->GetDesc(&ibd);
  UINT index_size = ibd.Format == D3DFMT_INDEX32 ? 4 : 2;

  HRESULT hr = DrawPrepare(PrimitiveType);
  if (FAILED(hr)) return hr;

  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    // Rewrite the fan's indices into a triangle list from the buffer's CPU
    // shadow (fans are too rare to justify a GPU-side pass).
    UINT index_count = PrimitiveCount + 2;
    if ((StartIndex + index_count) * index_size > ib->Length()) return D3DERR_INVALIDCALL;
    const uint8_t *src = ib->ShadowData() + StartIndex * index_size;
    auto readIndex = [&](UINT i) -> uint32_t {
      if (index_size == 4) return reinterpret_cast<const uint32_t *>(src)[i];
      return reinterpret_cast<const uint16_t *>(src)[i];
    };
    std::vector<uint32_t> list(PrimitiveCount * 3);
    for (UINT i = 0; i < PrimitiveCount; i++) {
      list[i * 3 + 0] = readIndex(0);
      list[i * 3 + 1] = readIndex(i + 1);
      list[i * 3 + 2] = readIndex(i + 2);
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (UINT)(list.size() * sizeof(uint32_t));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {list.data()};
    Com<ID3D11Buffer> temp;
    if (FAILED(d3d11_device_->CreateBuffer(&bd, &init, &temp))) return D3DERR_INVALIDCALL;
    d3d11_ctx_->IASetIndexBuffer(temp.ptr(), DXGI_FORMAT_R32_UINT, 0);
    d3d11_ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11_ctx_->DrawIndexed(PrimitiveCount * 3, 0, BaseVertexIndex);
    return D3D_OK;
  }

  D3D11_PRIMITIVE_TOPOLOGY topology;
  if (!ToD3D11Topology(PrimitiveType, &topology)) return D3DERR_INVALIDCALL;
  d3d11_ctx_->IASetIndexBuffer(
      ib->GetD3D11Buffer(),
      ibd.Format == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
  d3d11_ctx_->IASetPrimitiveTopology(topology);
  UINT index_count = PrimitiveVertexCount(PrimitiveType, PrimitiveCount);
  if (UINT instances = InstancedDrawCount())
    d3d11_ctx_->DrawIndexedInstanced(index_count, instances, StartIndex, BaseVertexIndex, 0);
  else
    d3d11_ctx_->DrawIndexed(index_count, StartIndex, BaseVertexIndex);
  return D3D_OK;
}

/* Writes into the ring and returns the byte offset written at, or ~0u if it
 * could not be done. The buffer is created on first use and replaced only when
 * a single draw needs more than it holds. */
UINT
D3D9Device::WriteToUploadRing(UploadRing &ring, D3D11_BIND_FLAG Bind, const void *pData, UINT Size, UINT Alignment) {
  if (!Size) return ~0u;

  if (ring.capacity < Size) {
    /* 1 MB to start with, then whatever the outsized draw asked for, rounded
     * up -- a menu's worth of text is far below this and never reallocates. */
    UINT capacity = 1u << 20;
    while (capacity < Size) capacity <<= 1;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth      = capacity;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = Bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Buffer> buffer;
    if (FAILED(d3d11_device_->CreateBuffer(&bd, nullptr, &buffer)))
      return ~0u;
    ring.buffer   = std::move(buffer);
    ring.capacity = capacity;
    ring.offset   = 0;
  }

  /* D3D11 wants a 4-byte aligned offset for both kinds; vertices get 16 so a
   * stride never straddles a cache line for no reason. The offset does not
   * have to be a multiple of the stride -- a vertex is addressed as
   * offset + index * stride. */
  UINT offset = (ring.offset + Alignment - 1) & ~(Alignment - 1);
  D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (offset + Size > ring.capacity) {
    /* Wrapped: the previous contents may still be in flight, so this is the
     * one place the buffer has to be renamed rather than appended to. */
    offset = 0;
    map = D3D11_MAP_WRITE_DISCARD;
  }

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(d3d11_ctx_->Map(ring.buffer.ptr(), 0, map, 0, &mapped)))
    return ~0u;
  memcpy(static_cast<uint8_t *>(mapped.pData) + offset, pData, Size);
  d3d11_ctx_->Unmap(ring.buffer.ptr(), 0);

  ring.offset = offset + Size;
  return offset;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  auto device_lock = LockDevice();
  /* A zero stride is an error, nothing to draw is not: D3D_OK, and the
   * stream stays bound. */
  if (!VertexStreamZeroStride) return D3DERR_INVALIDCALL;
  if (!PrimitiveCount) return D3D_OK;
  if (!pVertexStreamZeroData) return D3DERR_INVALIDCALL;
  draws_since_report_++;
  bool is_fan = PrimitiveType == D3DPT_TRIANGLEFAN;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (!is_fan && !ToD3D11Topology(PrimitiveType, &topology)) return D3DERR_INVALIDCALL;
  UINT vertex_count = PrimitiveVertexCount(PrimitiveType, PrimitiveCount);

  UINT vertex_offset = WriteToUploadRing(
      up_vertices_, D3D11_BIND_VERTEX_BUFFER, pVertexStreamZeroData,
      vertex_count * VertexStreamZeroStride, 16);
  if (vertex_offset == ~0u) return D3DERR_INVALIDCALL;

  HRESULT hr = DrawPrepare(PrimitiveType);
  if (FAILED(hr)) return hr;
  ID3D11Buffer *buf = up_vertices_.buffer.ptr();
  UINT stride = VertexStreamZeroStride, offset = vertex_offset;
  d3d11_ctx_->IASetVertexBuffers(0, 1, &buf, &stride, &offset);
  d3d11_ctx_->IASetPrimitiveTopology(topology);
  if (is_fan) {
    if (!EnsureFanIndexBuffer(PrimitiveCount)) return D3DERR_INVALIDCALL;
    d3d11_ctx_->IASetIndexBuffer(fan_index_buffer_.ptr(), DXGI_FORMAT_R32_UINT, 0);
    d3d11_ctx_->DrawIndexed(PrimitiveCount * 3, 0, 0);
  } else {
    d3d11_ctx_->Draw(vertex_count, 0);
  }
  // D3D9 clears the stream 0 binding after a UP draw.
  streams_[0] = {};
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
    const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
  auto device_lock = LockDevice();
  if (!VertexStreamZeroStride) return D3DERR_INVALIDCALL;
  if (!PrimitiveCount) return D3D_OK;
  if (!pIndexData || !pVertexStreamZeroData) return D3DERR_INVALIDCALL;
  draws_since_report_++;
  bool is_fan = PrimitiveType == D3DPT_TRIANGLEFAN;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (!is_fan && !ToD3D11Topology(PrimitiveType, &topology)) return D3DERR_INVALIDCALL;
  UINT index_count = PrimitiveVertexCount(PrimitiveType, PrimitiveCount);
  UINT index_size = IndexDataFormat == D3DFMT_INDEX32 ? 4 : 2;

  // Fans rewrite the supplied indices into a triangle list on the CPU.
  std::vector<uint32_t> fan_list;
  if (is_fan) {
    auto readIndex = [&](UINT i) -> uint32_t {
      if (index_size == 4) return reinterpret_cast<const uint32_t *>(pIndexData)[i];
      return reinterpret_cast<const uint16_t *>(pIndexData)[i];
    };
    fan_list.resize(PrimitiveCount * 3);
    for (UINT i = 0; i < PrimitiveCount; i++) {
      fan_list[i * 3 + 0] = readIndex(0);
      fan_list[i * 3 + 1] = readIndex(i + 1);
      fan_list[i * 3 + 2] = readIndex(i + 2);
    }
    pIndexData = fan_list.data();
    index_count = PrimitiveCount * 3;
    index_size = 4;
  }

  UINT vertex_offset = WriteToUploadRing(
      up_vertices_, D3D11_BIND_VERTEX_BUFFER, pVertexStreamZeroData,
      (MinVertexIndex + NumVertices) * VertexStreamZeroStride, 16);
  if (vertex_offset == ~0u) return D3DERR_INVALIDCALL;

  UINT index_offset = WriteToUploadRing(
      up_indices_, D3D11_BIND_INDEX_BUFFER, pIndexData,
      index_count * index_size, 4);
  if (index_offset == ~0u) return D3DERR_INVALIDCALL;

  HRESULT hr = DrawPrepare(PrimitiveType);
  if (FAILED(hr)) return hr;
  ID3D11Buffer *vbuf = up_vertices_.buffer.ptr();
  UINT stride = VertexStreamZeroStride, offset = vertex_offset;
  d3d11_ctx_->IASetVertexBuffers(0, 1, &vbuf, &stride, &offset);
  d3d11_ctx_->IASetIndexBuffer(up_indices_.buffer.ptr(),
                               index_size == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                               index_offset);
  d3d11_ctx_->IASetPrimitiveTopology(topology);
  d3d11_ctx_->DrawIndexed(index_count, 0, 0);
  // D3D9 clears the stream 0 and index bindings after an indexed UP draw.
  streams_[0] = {};
  index_buffer_ = nullptr;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawRectPatch(UINT, const float *, const D3DRECTPATCH_INFO *)                                                                        { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::DrawTriPatch(UINT, const float *, const D3DTRIPATCH_INFO *)                                                                          { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::DeletePatch(UINT)                                                                                                                    { return D3D_OK; }
HRESULT STDMETHODCALLTYPE
D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
  auto device_lock = LockDevice();
  // ppQuery == NULL is the documented support-check form: it only probes.
  if (!ppQuery) return D3D9Query::IsSupportedType(Type) ? D3D_OK : D3DERR_NOTAVAILABLE;
  *ppQuery = nullptr;
  if (!D3D9Query::IsSupportedType(Type)) return D3DERR_NOTAVAILABLE;
  auto *query = new D3D9Query(this, Type);
  if (!query->IsValid()) { delete query; return D3DERR_INVALIDCALL; }
  *ppQuery = ref(query);
  return D3D_OK;
}

// IDirect3DDevice9Ex
HRESULT STDMETHODCALLTYPE D3D9Device::SetConvolutionMonoKernel(UINT, UINT, float *, float *)                                                                               { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::ComposeRects(IDirect3DSurface9 *, IDirect3DSurface9 *, IDirect3DVertexBuffer9 *, UINT, IDirect3DVertexBuffer9 *, D3DCOMPOSERECTSOP, int, int) { D9_STUB(ComposeRects); }
HRESULT STDMETHODCALLTYPE D3D9Device::PresentEx(const RECT *pSrc, const RECT *pDst, HWND hDest, const RGNDATA *pDR, DWORD)                                                { return Present(pSrc, pDst, hDest, pDR); }
HRESULT STDMETHODCALLTYPE D3D9Device::GetGPUThreadPriority(INT *pP)                                                                                                        { if (pP) *pP = 0; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetGPUThreadPriority(INT)                                                                                                            { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::WaitForVBlank(UINT)                                                                                                                  { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::CheckResourceResidency(IDirect3DResource9 **, UINT32)                                                                                { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetMaximumFrameLatency(UINT)                                                                                                         { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetMaximumFrameLatency(UINT *pL)                                                                                                     { if (pL) *pL = 3; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::CheckDeviceState(HWND)                                                                                                               { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD /*Usage*/) {
  auto device_lock = LockDevice();
  return CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE D3D9Device::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD /*Usage*/) {
  auto device_lock = LockDevice();
  return CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE D3D9Device::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD /*Usage*/) {
  auto device_lock = LockDevice();
  return CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE D3D9Device::ResetEx(D3DPRESENT_PARAMETERS *pPP, D3DDISPLAYMODEEX *)                                                                              { return Reset(pPP); }
HRESULT STDMETHODCALLTYPE D3D9Device::GetDisplayModeEx(UINT iSC, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRot) {
  auto device_lock = LockDevice();
  D3DDISPLAYMODE m = {};
  GetDisplayMode(iSC, &m);
  if (pMode) { pMode->Width = m.Width; pMode->Height = m.Height; pMode->RefreshRate = m.RefreshRate; pMode->Format = m.Format; pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE; }
  if (pRot) *pRot = D3DDISPLAYROTATION_IDENTITY;
  return D3D_OK;
}

} // namespace dxmt
