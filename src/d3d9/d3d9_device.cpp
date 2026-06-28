#include "d3d9_device.hpp"
#include "d3d9_caps.hpp"
#include "d3d9_format.hpp"
#include "d3d9_interface.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_buffer.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_trace.hpp"
#include "log/log.hpp"

#define D9_STUB(method) \
  D3D9_TRACE("D3D9Device::" #method " not implemented"); \
  return E_NOTIMPL

namespace dxmt {

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
      present_params_(*pPP),
      is_ex_(isEx) {}

D3D9Device::~D3D9Device() {}

HRESULT
D3D9Device::Initialize() {
  const D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

  D3D9_TRACE("Initialize begin");
  HRESULT hr = D3D11CreateDevice(
      dxgi_adapter_.ptr(),
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      0,
      feature_levels, ARRAYSIZE(feature_levels),
      D3D11_SDK_VERSION,
      &d3d11_device_,
      nullptr,
      &d3d11_ctx_);

  D3D9_TRACE("Initialize D3D11CreateDevice hr=0x", std::hex, hr);
  if (FAILED(hr)) {
    ERR("D3D9Device::Initialize: D3D11CreateDevice failed");
    return hr;
  }

  Com<IDXGIFactory1> dxgi_factory;
  if (FAILED(dxgi_adapter_->GetParent(IID_PPV_ARGS(&dxgi_factory)))) {
    ERR("D3D9Device::Initialize: GetParent(IDXGIFactory1) failed");
    return E_FAIL;
  }

  HWND hWnd = present_params_.hDeviceWindow ? present_params_.hDeviceWindow : focus_wnd_;

  DXGI_SWAP_CHAIN_DESC scd = {};
  scd.BufferCount       = 1;
  scd.BufferDesc.Width  = present_params_.BackBufferWidth;
  scd.BufferDesc.Height = present_params_.BackBufferHeight;
  scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  scd.BufferDesc.RefreshRate.Numerator   = 0;
  scd.BufferDesc.RefreshRate.Denominator = 1;
  scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.OutputWindow      = hWnd;
  scd.SampleDesc.Count  = 1;
  scd.Windowed          = present_params_.Windowed;
  scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
  scd.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

  D3D9_TRACE("Initialize CreateSwapChain ",
             present_params_.BackBufferWidth, "x", present_params_.BackBufferHeight,
             " windowed=", present_params_.Windowed);
  hr = dxgi_factory->CreateSwapChain(d3d11_device_.ptr(), &scd, &swap_chain_);
  D3D9_TRACE("Initialize CreateSwapChain hr=0x", std::hex, hr);
  if (FAILED(hr)) {
    ERR("D3D9Device::Initialize: CreateSwapChain failed");
    return hr;
  }

  // Default scissor rect covers the full backbuffer
  scissor_rect_ = { 0, 0,
    (LONG)present_params_.BackBufferWidth,
    (LONG)present_params_.BackBufferHeight };

  hr = CreateImplicitSurfaces();
  D3D9_TRACE("Initialize CreateImplicitSurfaces hr=0x", std::hex, hr);
  if (FAILED(hr)) {
    ERR("D3D9Device::Initialize: implicit surface creation failed");
    return hr;
  }

  D3D9_TRACE("Initialize success");
  return S_OK;
}

HRESULT
D3D9Device::CreateImplicitSurfaces() {
  // Wrap the swap chain back buffer as the default render target.
  Com<ID3D11Texture2D> bb;
  HRESULT hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&bb));
  if (FAILED(hr)) return hr;

  backbuffer_ = new D3D9Surface(this, bb.ptr(), 0, D3DPOOL_DEFAULT,
                                D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8);
  render_targets_[0] = backbuffer_;

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
      td.SampleDesc.Count = 1;
      td.Usage      = D3D11_USAGE_DEFAULT;
      td.BindFlags  = D3D11_BIND_DEPTH_STENCIL;

      Com<ID3D11Texture2D> ds;
      if (SUCCEEDED(d3d11_device_->CreateTexture2D(&td, nullptr, &ds))) {
        auto_depth_ = new D3D9Surface(this, ds.ptr(), 0, D3DPOOL_DEFAULT,
                                      D3DUSAGE_DEPTHSTENCIL,
                                      present_params_.AutoDepthStencilFormat);
        depth_stencil_ = auto_depth_;
      }
    }
  }

  ApplyRenderTargets();
  return D3D_OK;
}

void
D3D9Device::ApplyRenderTargets() {
  ID3D11RenderTargetView *rtvs[kMaxRenderTargets] = {};
  for (UINT i = 0; i < kMaxRenderTargets; i++)
    rtvs[i] = render_targets_[i] ? render_targets_[i]->GetRTV() : nullptr;
  ID3D11DepthStencilView *dsv = depth_stencil_ ? depth_stencil_->GetDSV() : nullptr;
  d3d11_ctx_->OMSetRenderTargets(kMaxRenderTargets, rtvs, dsv);
}

// IUnknown
HRESULT STDMETHODCALLTYPE
D3D9Device::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject) return E_POINTER;
  if (riid == __uuidof(IUnknown) ||
      riid == __uuidof(IDirect3DDevice9) ||
      (is_ex_ && riid == __uuidof(IDirect3DDevice9Ex))) {
    *ppvObject = ref(this);
    return S_OK;
  }
  *ppvObject = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D3D9Device::AddRef()  { return ++ref_; }
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
  if (!ppD3D9) return D3DERR_INVALIDCALL;
  *ppD3D9 = ref(parent_);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetDeviceCaps(D3DCAPS9 *pCaps) {
  if (!pCaps) return D3DERR_INVALIDCALL;
  FillD3D9Caps(pCaps);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) {
  if (iSwapChain != 0 || !pMode) return D3DERR_INVALIDCALL;
  pMode->Width       = present_params_.BackBufferWidth;
  pMode->Height      = present_params_.BackBufferHeight;
  pMode->RefreshRate = 60;
  pMode->Format      = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParams) {
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

HRESULT STDMETHODCALLTYPE D3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **) { D9_STUB(CreateAdditionalSwapChain); }
HRESULT STDMETHODCALLTYPE D3D9Device::GetSwapChain(UINT, IDirect3DSwapChain9 **)            { D9_STUB(GetSwapChain); }
UINT    STDMETHODCALLTYPE D3D9Device::GetNumberOfSwapChains()                               { return 1; }

HRESULT STDMETHODCALLTYPE
D3D9Device::Reset(D3DPRESENT_PARAMETERS *pPP) {
  if (!pPP) return D3DERR_INVALIDCALL;
  present_params_ = *pPP;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::Present(const RECT *, const RECT *, HWND, const RGNDATA *) {
  DXGI_PRESENT_PARAMETERS pp = {};
  Com<IDXGISwapChain1> sc1;
  if (SUCCEEDED(swap_chain_->QueryInterface(IID_PPV_ARGS(&sc1))))
    sc1->Present1(1, 0, &pp);
  else
    swap_chain_->Present(1, 0);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppBackBuffer) {
  if (!ppBackBuffer || iSwapChain != 0 || iBackBuffer != 0 || !backbuffer_)
    return D3DERR_INVALIDCALL;
  *ppBackBuffer = ref(backbuffer_.ptr());
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::GetRasterStatus(UINT, D3DRASTER_STATUS *pRS) {
  if (pRS) { pRS->InVBlank = FALSE; pRS->ScanLine = 0; }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::SetDialogBoxMode(BOOL)          { return D3D_OK; }
void    STDMETHODCALLTYPE D3D9Device::SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) {}
void    STDMETHODCALLTYPE D3D9Device::GetGammaRamp(UINT, D3DGAMMARAMP *pRamp) { if (pRamp) memset(pRamp, 0, sizeof(*pRamp)); }

// Resource creation stubs — implemented in later phases
HRESULT STDMETHODCALLTYPE D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *) {
  if (!ppTexture) return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  if (Width == 0 || Height == 0) return D3DERR_INVALIDCALL;

  auto *texture = new D3D9Texture(this, Width, Height, Levels, Usage, Format, Pool);
  if (!texture->IsValid()) { delete texture; return D3DERR_INVALIDCALL; }
  *ppTexture = ref(texture);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVolumeTexture(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9 **, HANDLE *)                  { D9_STUB(CreateVolumeTexture); }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *)                                  { D9_STUB(CreateCubeTexture); }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *) {
  if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
  *ppVertexBuffer = nullptr;
  if (Length == 0) return D3DERR_INVALIDCALL;

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
  if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
  *ppIndexBuffer = nullptr;
  if (Length == 0) return D3DERR_INVALIDCALL;
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
D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL /*Lockable*/, IDirect3DSurface9 **ppSurface, HANDLE *) {
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

  *ppSurface = ref(new D3D9Surface(this, tex.ptr(), 0, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET, Format));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL /*Discard*/, IDirect3DSurface9 **ppSurface, HANDLE *) {
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

  *ppSurface = ref(new D3D9Surface(this, tex.ptr(), 0, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL, Format));
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D3D9Device::UpdateSurface(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *)                                          { D9_STUB(UpdateSurface); }
HRESULT STDMETHODCALLTYPE D3D9Device::UpdateTexture(IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *)                                                                { D9_STUB(UpdateTexture); }
HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderTargetData(IDirect3DSurface9 *, IDirect3DSurface9 *)                                                                  { D9_STUB(GetRenderTargetData); }
HRESULT STDMETHODCALLTYPE D3D9Device::GetFrontBufferData(UINT, IDirect3DSurface9 *)                                                                                  { D9_STUB(GetFrontBufferData); }
HRESULT STDMETHODCALLTYPE D3D9Device::StretchRect(IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE)                       { D9_STUB(StretchRect); }
HRESULT STDMETHODCALLTYPE D3D9Device::ColorFill(IDirect3DSurface9 *, const RECT *, D3DCOLOR)                                                                        { D9_STUB(ColorFill); }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateOffscreenPlainSurface(UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *)                                   { D9_STUB(CreateOffscreenPlainSurface); }

// Render targets / depth
HRESULT STDMETHODCALLTYPE
D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  if (RenderTargetIndex >= kMaxRenderTargets) return D3DERR_INVALIDCALL;
  // Setting RT0 to null is illegal in D3D9.
  if (RenderTargetIndex == 0 && !pRenderTarget) return D3DERR_INVALIDCALL;
  render_targets_[RenderTargetIndex] = static_cast<D3D9Surface *>(pRenderTarget);
  ApplyRenderTargets();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRT) {
  if (!ppRT || RenderTargetIndex >= kMaxRenderTargets) return D3DERR_INVALIDCALL;
  *ppRT = render_targets_[RenderTargetIndex] ? ref(render_targets_[RenderTargetIndex].ptr()) : nullptr;
  return *ppRT ? D3D_OK : D3DERR_NOTFOUND;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pSurface) {
  depth_stencil_ = static_cast<D3D9Surface *>(pSurface);
  ApplyRenderTargets();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZ) {
  if (!ppZ) return D3DERR_INVALIDCALL;
  *ppZ = depth_stencil_ ? ref(depth_stencil_.ptr()) : nullptr;
  return *ppZ ? D3D_OK : D3DERR_NOTFOUND;
}
HRESULT STDMETHODCALLTYPE D3D9Device::BeginScene()                                           { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::EndScene()                                             { return D3D_OK; }

HRESULT STDMETHODCALLTYPE
D3D9Device::Clear(DWORD, const D3DRECT *, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
  if (Flags & D3DCLEAR_TARGET) {
    float c[4] = {
      ((Color >> 16) & 0xFF) / 255.0f,
      ((Color >>  8) & 0xFF) / 255.0f,
      ((Color >>  0) & 0xFF) / 255.0f,
      ((Color >> 24) & 0xFF) / 255.0f,
    };
    for (UINT i = 0; i < kMaxRenderTargets; i++) {
      if (render_targets_[i])
        if (auto rtv = render_targets_[i]->GetRTV())
          d3d11_ctx_->ClearRenderTargetView(rtv, c);
    }
  }

  if ((Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && depth_stencil_) {
    UINT df = 0;
    if (Flags & D3DCLEAR_ZBUFFER) df |= D3D11_CLEAR_DEPTH;
    if (Flags & D3DCLEAR_STENCIL) df |= D3D11_CLEAR_STENCIL;
    if (auto dsv = depth_stencil_->GetDSV())
      d3d11_ctx_->ClearDepthStencilView(dsv, df, Z, (UINT8)Stencil);
  }
  return D3D_OK;
}

// Transform / material / lighting — state stored for FFP, used when shader is active
HRESULT STDMETHODCALLTYPE D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX *) { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX *pM)     { if (pM) memset(pM, 0, sizeof(*pM)); return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX *) { return D3D_OK; }

HRESULT STDMETHODCALLTYPE
D3D9Device::SetViewport(const D3DVIEWPORT9 *pVP) {
  if (!pVP) return D3DERR_INVALIDCALL;
  D3D11_VIEWPORT vp = {
    (float)pVP->X, (float)pVP->Y,
    (float)pVP->Width, (float)pVP->Height,
    pVP->MinZ, pVP->MaxZ
  };
  d3d11_ctx_->RSSetViewports(1, &vp);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetViewport(D3DVIEWPORT9 *pVP) {
  if (!pVP) return D3DERR_INVALIDCALL;
  UINT n = 1;
  D3D11_VIEWPORT vp = {};
  d3d11_ctx_->RSGetViewports(&n, &vp);
  pVP->X = (DWORD)vp.TopLeftX; pVP->Y = (DWORD)vp.TopLeftY;
  pVP->Width = (DWORD)vp.Width; pVP->Height = (DWORD)vp.Height;
  pVP->MinZ = vp.MinDepth; pVP->MaxZ = vp.MaxDepth;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetMaterial(const D3DMATERIAL9 *)   { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetMaterial(D3DMATERIAL9 *pM)       { if (pM) memset(pM, 0, sizeof(*pM)); return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetLight(DWORD, const D3DLIGHT9 *)  { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetLight(DWORD, D3DLIGHT9 *pL)      { if (pL) memset(pL, 0, sizeof(*pL)); return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::LightEnable(DWORD, BOOL)            { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetLightEnable(DWORD, BOOL *pE)     { if (pE) *pE = FALSE; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetClipPlane(DWORD, const float *)  { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetClipPlane(DWORD, float *pP)      { if (pP) memset(pP, 0, sizeof(float)*4); return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderState(D3DRENDERSTATETYPE, DWORD)       { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderState(D3DRENDERSTATETYPE, DWORD *pV)   { if (pV) *pV = 0; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9 **) { D9_STUB(CreateStateBlock); }
HRESULT STDMETHODCALLTYPE D3D9Device::BeginStateBlock()                               { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::EndStateBlock(IDirect3DStateBlock9 **)          { D9_STUB(EndStateBlock); }
HRESULT STDMETHODCALLTYPE D3D9Device::SetClipStatus(const D3DCLIPSTATUS9 *)           { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetClipStatus(D3DCLIPSTATUS9 *pCS)             { if (pCS) memset(pCS, 0, sizeof(*pCS)); return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetTexture(DWORD, IDirect3DBaseTexture9 **ppT)  { if (ppT) *ppT = nullptr; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetTexture(DWORD, IDirect3DBaseTexture9 *)      { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD *pV)  { if (pV) *pV = 0; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD)      { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD *pV) { if (pV) *pV = 0; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD)     { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::ValidateDevice(DWORD *pPasses)      { if (pPasses) *pPasses = 1; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetPaletteEntries(UINT, const PALETTEENTRY *)           { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetPaletteEntries(UINT, PALETTEENTRY *pE)               { if (pE) memset(pE, 0, 256*sizeof(PALETTEENTRY)); return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetCurrentTexturePalette(UINT)                         { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetCurrentTexturePalette(UINT *pN) { if (pN) *pN = 0; return D3D_OK; }

HRESULT STDMETHODCALLTYPE
D3D9Device::SetScissorRect(const RECT *pRect) {
  if (!pRect) return D3DERR_INVALIDCALL;
  scissor_rect_ = *pRect;
  D3D11_RECT r = { pRect->left, pRect->top, pRect->right, pRect->bottom };
  d3d11_ctx_->RSSetScissorRects(1, &r);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
D3D9Device::GetScissorRect(RECT *pRect) {
  if (!pRect) return D3DERR_INVALIDCALL;
  *pRect = scissor_rect_;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetSoftwareVertexProcessing(BOOL)        { return D3D_OK; }
BOOL    STDMETHODCALLTYPE D3D9Device::GetSoftwareVertexProcessing()             { return FALSE; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetNPatchMode(float)                      { return D3D_OK; }
float   STDMETHODCALLTYPE D3D9Device::GetNPatchMode()                           { return 0.0f; }

// Draw calls — stubs until shader + state pipeline is implemented
HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT)                                                                                          { D9_STUB(DrawPrimitive); }
HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)                                                                  { D9_STUB(DrawIndexedPrimitive); }
HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, const void *, UINT)                                                                          { D9_STUB(DrawPrimitiveUP); }
HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT)                               { D9_STUB(DrawIndexedPrimitiveUP); }
HRESULT STDMETHODCALLTYPE D3D9Device::ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9 *, IDirect3DVertexDeclaration9 *, DWORD)                                    { D9_STUB(ProcessVertices); }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **)                                                   { D9_STUB(CreateVertexDeclaration); }
HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *)                                                                                  { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppD)                                                                              { if (ppD) *ppD = nullptr; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetFVF(DWORD)                                                                                                                        { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetFVF(DWORD *pFVF)                                                                                                                 { if (pFVF) *pFVF = 0; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexShader(const DWORD *, IDirect3DVertexShader9 **)                                                                         { D9_STUB(CreateVertexShader); }
HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShader(IDirect3DVertexShader9 *)                                                                                            { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppS)                                                                                        { if (ppS) *ppS = nullptr; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantF(UINT, const float *, UINT)                                                                                  { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantF(UINT, float *, UINT)                                                                                        { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantI(UINT, const int *, UINT)                                                                                    { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantI(UINT, int *, UINT)                                                                                          { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantB(UINT, const BOOL *, UINT)                                                                                   { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantB(UINT, BOOL *, UINT)                                                                                         { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetStreamSource(UINT, IDirect3DVertexBuffer9 *, UINT, UINT)                                                                          { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetStreamSource(UINT, IDirect3DVertexBuffer9 **ppV, UINT *pO, UINT *pS) { if (ppV) *ppV = nullptr; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetStreamSourceFreq(UINT, UINT)                                                                                                      { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetStreamSourceFreq(UINT, UINT *pS)                                                                                                  { if (pS) *pS = 1; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetIndices(IDirect3DIndexBuffer9 *)                                                                                                  { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppI)                                                                                              { if (ppI) *ppI = nullptr; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::CreatePixelShader(const DWORD *, IDirect3DPixelShader9 **)                                                                           { D9_STUB(CreatePixelShader); }
HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShader(IDirect3DPixelShader9 *)                                                                                              { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppS)                                                                                         { if (ppS) *ppS = nullptr; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantF(UINT, const float *, UINT)                                                                                   { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShaderConstantF(UINT, float *, UINT)                                                                                         { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantI(UINT, const int *, UINT)                                                                                     { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShaderConstantI(UINT, int *, UINT)                                                                                           { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantB(UINT, const BOOL *, UINT)                                                                                    { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShaderConstantB(UINT, BOOL *, UINT)                                                                                          { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::DrawRectPatch(UINT, const float *, const D3DRECTPATCH_INFO *)                                                                        { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::DrawTriPatch(UINT, const float *, const D3DTRIPATCH_INFO *)                                                                          { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::DeletePatch(UINT)                                                                                                                    { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateQuery(D3DQUERYTYPE, IDirect3DQuery9 **)                                                                                        { D9_STUB(CreateQuery); }

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
HRESULT STDMETHODCALLTYPE D3D9Device::CreateRenderTargetEx(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *, DWORD)                 { D9_STUB(CreateRenderTargetEx); }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateOffscreenPlainSurfaceEx(UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *, DWORD)                                 { D9_STUB(CreateOffscreenPlainSurfaceEx); }
HRESULT STDMETHODCALLTYPE D3D9Device::CreateDepthStencilSurfaceEx(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *, DWORD)          { D9_STUB(CreateDepthStencilSurfaceEx); }
HRESULT STDMETHODCALLTYPE D3D9Device::ResetEx(D3DPRESENT_PARAMETERS *pPP, D3DDISPLAYMODEEX *)                                                                              { return Reset(pPP); }
HRESULT STDMETHODCALLTYPE D3D9Device::GetDisplayModeEx(UINT iSC, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRot) {
  D3DDISPLAYMODE m = {};
  GetDisplayMode(iSC, &m);
  if (pMode) { pMode->Width = m.Width; pMode->Height = m.Height; pMode->RefreshRate = m.RefreshRate; pMode->Format = m.Format; pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE; }
  if (pRot) *pRot = D3DDISPLAYROTATION_IDENTITY;
  return D3D_OK;
}

} // namespace dxmt
