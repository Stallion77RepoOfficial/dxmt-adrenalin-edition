#include "d3d9_device.hpp"
#include "d3d9_main.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_buffer.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <cstring>
#include <algorithm>

namespace dxmt::d3d9 {

// ---------------------------------------------------------------------------
// SwapChain
// ---------------------------------------------------------------------------

D3D9SwapChain::D3D9SwapChain(D3D9Device *pDevice, D3DPRESENT_PARAMETERS *pPP,
                              IDXGISwapChain *pDXGISwapChain)
    : m_device(pDevice), m_pp(*pPP) {
    m_dxgiSwapChain = pDXGISwapChain;

    // Get back buffer texture
    Com<ID3D11Texture2D> backBufTex;
    pDXGISwapChain->GetBuffer(0, IID_PPV_ARGS(&backBufTex));

    // Create a D3D9Surface wrapping the back buffer (RTV is created inside D3D9Surface)
    auto surf = new D3D9Surface(pDevice, backBufTex.ptr(), 0, 0,
                                 D3DFMT_A8R8G8B8, D3DUSAGE_RENDERTARGET);
    m_backBuffer = surf;
}

D3D9SwapChain::~D3D9SwapChain() {}

HRESULT STDMETHODCALLTYPE D3D9SwapChain::QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DSwapChain9) {
        *ppvObject = static_cast<IDirect3DSwapChain9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9SwapChain::Present(const RECT *pSourceRect, const RECT *pDestRect,
                                                   HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags) {
    UINT syncInterval = (m_pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE) ? 0 : 1;
    return m_dxgiSwapChain->Present(syncInterval, 0);
}

HRESULT STDMETHODCALLTYPE D3D9SwapChain::GetBackBuffer(UINT iBackBuffer, UINT Type, IDirect3DSurface9 **ppBackBuffer) {
    if (!ppBackBuffer) return D3DERR_INVALIDCALL;
    if (iBackBuffer != 0) return D3DERR_INVALIDCALL;
    *ppBackBuffer = m_backBuffer.ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9SwapChain::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) {
    if (!pRasterStatus) return D3DERR_INVALIDCALL;
    pRasterStatus->InVBlank = FALSE;
    pRasterStatus->ScanLine = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9SwapChain::GetDisplayMode(D3DDISPLAYMODE *pMode) {
    if (!pMode) return D3DERR_INVALIDCALL;
    pMode->Width       = m_pp.BackBufferWidth;
    pMode->Height      = m_pp.BackBufferHeight;
    pMode->RefreshRate = m_pp.FullScreen_RefreshRateInHz ? m_pp.FullScreen_RefreshRateInHz : 60;
    pMode->Format      = m_pp.BackBufferFormat;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9SwapChain::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_device);
    (*ppDevice)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *pPP) {
    if (!pPP) return D3DERR_INVALIDCALL;
    *pPP = m_pp;
    return D3D_OK;
}

// ---------------------------------------------------------------------------
// D3D9Device
// ---------------------------------------------------------------------------

D3D9Device::D3D9Device(D3D9Main *pD3D9, ID3D11Device *pD3D11Device, IDXGISwapChain *pSwapChain,
                        UINT adapter, D3DDEVTYPE devType, HWND hFocusWindow, DWORD behaviorFlags,
                        D3DPRESENT_PARAMETERS *pPP)
    : m_d3d9(pD3D9), m_adapterOrdinal(adapter), m_devType(devType),
      m_focusWindow(hFocusWindow), m_behaviorFlags(behaviorFlags), m_pp(*pPP) {
    m_d3d11Device = pD3D11Device;
    pD3D11Device->GetImmediateContext(&m_d3d11Context);

    // Create swap chain wrapper
    m_swapChain = new D3D9SwapChain(this, pPP, pSwapChain);

    // Set back buffer as initial render target
    m_renderTargets[0] = m_swapChain->GetBackBuffer();

    // Set default viewport
    m_viewport.X      = 0;
    m_viewport.Y      = 0;
    m_viewport.Width  = pPP->BackBufferWidth;
    m_viewport.Height = pPP->BackBufferHeight;
    m_viewport.MinZ   = 0.0f;
    m_viewport.MaxZ   = 1.0f;

    // Create constant buffers
    CreateConstantBuffers();

    // Set initial D3D11 state
    FlushRenderStates();

    // Create an auto depth-stencil if requested
    if (pPP->EnableAutoDepthStencil) {
        CreateDepthStencilSurface(pPP->BackBufferWidth, pPP->BackBufferHeight,
            pPP->AutoDepthStencilFormat, pPP->MultiSampleType, 0, TRUE,
            (IDirect3DSurface9**)&m_depthStencil, nullptr);
    }

    // Bind initial RT and DS
    if (m_renderTargets[0]) {
        ID3D11RenderTargetView *rtv = m_renderTargets[0]->GetRTV();
        ID3D11DepthStencilView *dsv = m_depthStencil ? m_depthStencil->GetDSV() : nullptr;
        m_d3d11Context->OMSetRenderTargets(1, &rtv, dsv);
    }

    // Set initial viewport
    D3D11_VIEWPORT vp;
    vp.TopLeftX = (float)m_viewport.X;
    vp.TopLeftY = (float)m_viewport.Y;
    vp.Width    = (float)m_viewport.Width;
    vp.Height   = (float)m_viewport.Height;
    vp.MinDepth = m_viewport.MinZ;
    vp.MaxDepth = m_viewport.MaxZ;
    m_d3d11Context->RSSetViewports(1, &vp);

    Logger::info("D3D9Device created");
}

D3D9Device::~D3D9Device() {}

void D3D9Device::CreateConstantBuffers() {
    D3D11_BUFFER_DESC desc = {};
    desc.Usage          = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    desc.MiscFlags      = 0;

    desc.ByteWidth = kMaxVSConsts * 16;
    m_d3d11Device->CreateBuffer(&desc, nullptr, &m_vsCB);

    desc.ByteWidth = kMaxPSConsts * 16;
    m_d3d11Device->CreateBuffer(&desc, nullptr, &m_psCB);

    // Fixed-function buffers (world/view/proj matrices + material + lighting)
    desc.ByteWidth = 256; // enough for matrices
    m_d3d11Device->CreateBuffer(&desc, nullptr, &m_ffVsCB);
    m_d3d11Device->CreateBuffer(&desc, nullptr, &m_ffPsCB);
}

HRESULT STDMETHODCALLTYPE D3D9Device::QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
        *ppvObject = static_cast<IDirect3DDevice9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDirect3D(IDirect3D9 **ppD3D9) {
    if (!ppD3D9) return D3DERR_INVALIDCALL;
    *ppD3D9 = static_cast<IDirect3D9*>(m_d3d9);
    (*ppD3D9)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDeviceCaps(D3DCAPS9 *pCaps) {
    return m_d3d9->GetDeviceCaps(m_adapterOrdinal, m_devType, pCaps);
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) {
    if (!pMode) return D3DERR_INVALIDCALL;
    return m_swapChain->GetDisplayMode(pMode);
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
    if (!pParameters) return D3DERR_INVALIDCALL;
    pParameters->AdapterOrdinal = m_adapterOrdinal;
    pParameters->DeviceType     = m_devType;
    pParameters->hFocusWindow   = m_focusWindow;
    pParameters->BehaviorFlags  = m_behaviorFlags;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPP, IDirect3DSwapChain9 **ppSwapChain) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **ppSwapChain) {
    if (!ppSwapChain) return D3DERR_INVALIDCALL;
    if (iSwapChain != 0) return D3DERR_INVALIDCALL;
    *ppSwapChain = m_swapChain.ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::Reset(D3DPRESENT_PARAMETERS *pPP) {
    if (!pPP) return D3DERR_INVALIDCALL;
    m_pp = *pPP;
    // Simplified reset: just update viewport
    m_viewport.Width  = pPP->BackBufferWidth;
    m_viewport.Height = pPP->BackBufferHeight;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::Present(const RECT *pSourceRect, const RECT *pDestRect,
                                               HWND hDestWindowOverride, const RGNDATA *pDirtyRegion) {
    return m_swapChain->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, 0);
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, UINT Type, IDirect3DSurface9 **ppBackBuffer) {
    if (!ppBackBuffer) return D3DERR_INVALIDCALL;
    if (iSwapChain != 0) return D3DERR_INVALIDCALL;
    return m_swapChain->GetBackBuffer(iBackBuffer, Type, ppBackBuffer);
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) {
    return m_swapChain->GetRasterStatus(pRasterStatus);
}

// Resource creation
HRESULT STDMETHODCALLTYPE D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) {
    if (!ppTexture) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppTexture);
    *ppTexture = new D3D9Texture2D(this, Width, Height, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle) {
    return E_NOTIMPL; // TODO
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) {
    if (!ppCubeTexture) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppCubeTexture);
    *ppCubeTexture = new D3D9CubeTexture(this, EdgeLength, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) {
    if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppVertexBuffer);
    *ppVertexBuffer = new D3D9VertexBuffer(this, Length, Usage, FVF, Pool);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) {
    if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppIndexBuffer);
    *ppIndexBuffer = new D3D9IndexBuffer(this, Length, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
    if (!ppSurface) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppSurface);
    *ppSurface = new D3D9Surface(this, Width, Height, Format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, Lockable);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
    if (!ppSurface) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppSurface);
    *ppSurface = new D3D9Surface(this, Width, Height, Format, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, FALSE);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) {
    if (!ppSurface) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppSurface);
    *ppSurface = new D3D9Surface(this, Width, Height, Format, 0, Pool, TRUE);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::UpdateSurface(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint) {
    if (!pSourceSurface || !pDestinationSurface) return D3DERR_INVALIDCALL;
    auto *src = static_cast<D3D9Surface*>(pSourceSurface);
    auto *dst = static_cast<D3D9Surface*>(pDestinationSurface);
    if (!src->GetD3D11Texture() || !dst->GetD3D11Texture()) return D3DERR_INVALIDCALL;

    D3D11_BOX srcBox = {};
    D3DSURFACE_DESC srcDesc = {}, dstDesc = {};
    src->GetDesc(&srcDesc);
    dst->GetDesc(&dstDesc);

    UINT srcLeft = pSourceRect ? pSourceRect->left : 0;
    UINT srcTop  = pSourceRect ? pSourceRect->top  : 0;
    UINT srcRight  = pSourceRect ? pSourceRect->right  : srcDesc.Width;
    UINT srcBottom = pSourceRect ? pSourceRect->bottom : srcDesc.Height;
    srcBox = { srcLeft, srcTop, 0, srcRight, srcBottom, 1 };

    UINT dstX = pDestPoint ? pDestPoint->x : 0;
    UINT dstY = pDestPoint ? pDestPoint->y : 0;

    m_d3d11Context->CopySubresourceRegion(
        dst->GetD3D11Texture(), D3D11CalcSubresource(dst->GetMipLevel(), dst->GetArraySlice(), 1),
        dstX, dstY, 0,
        src->GetD3D11Texture(), D3D11CalcSubresource(src->GetMipLevel(), src->GetArraySlice(), 1),
        &srcBox);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) {
    if (!pSourceTexture || !pDestinationTexture) return D3DERR_INVALIDCALL;
    // Basic implementation: copy all subresources
    Com<ID3D11Resource> srcRes, dstRes;
    if (pSourceTexture->GetType() == D3DRTYPE_TEXTURE) {
        auto *src = static_cast<D3D9Texture2D*>(pSourceTexture);
        auto *dst = static_cast<D3D9Texture2D*>(pDestinationTexture);
        if (src->GetD3D11Texture() && dst->GetD3D11Texture())
            m_d3d11Context->CopyResource(dst->GetD3D11Texture(), src->GetD3D11Texture());
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) {
    if (!pRenderTarget || !pDestSurface) return D3DERR_INVALIDCALL;
    auto *src = static_cast<D3D9Surface*>(pRenderTarget);
    auto *dst = static_cast<D3D9Surface*>(pDestSurface);
    if (!src->GetD3D11Texture() || !dst->GetD3D11Texture()) return D3DERR_INVALIDCALL;
    m_d3d11Context->CopyResource(dst->GetD3D11Texture(), src->GetD3D11Texture());
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter) {
    // Simplified: copy the entire surface
    if (!pSourceSurface || !pDestSurface) return D3DERR_INVALIDCALL;
    auto *src = static_cast<D3D9Surface*>(pSourceSurface);
    auto *dst = static_cast<D3D9Surface*>(pDestSurface);
    if (!src->GetD3D11Texture() || !dst->GetD3D11Texture()) return D3DERR_INVALIDCALL;
    m_d3d11Context->CopyResource(dst->GetD3D11Texture(), src->GetD3D11Texture());
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color) {
    if (!pSurface) return D3DERR_INVALIDCALL;
    auto *surf = static_cast<D3D9Surface*>(pSurface);
    if (surf->GetRTV()) {
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float g = ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color >>  0) & 0xFF) / 255.0f;
        float a = ((color >> 24) & 0xFF) / 255.0f;
        float clearColor[4] = { r, g, b, a };
        m_d3d11Context->ClearRenderTargetView(surf->GetRTV(), clearColor);
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
    if (RenderTargetIndex >= kMaxRTs) return D3DERR_INVALIDCALL;
    m_renderTargets[RenderTargetIndex] = static_cast<D3D9Surface*>(pRenderTarget);

    // Rebind render targets
    ID3D11RenderTargetView *rtvs[kMaxRTs] = {};
    UINT count = 0;
    for (UINT i = 0; i < kMaxRTs; i++) {
        if (m_renderTargets[i])
            rtvs[i] = m_renderTargets[i]->GetRTV();
        if (rtvs[i]) count = i + 1;
    }
    ID3D11DepthStencilView *dsv = m_depthStencil ? m_depthStencil->GetDSV() : nullptr;
    m_d3d11Context->OMSetRenderTargets(count, rtvs, dsv);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) {
    if (!ppRenderTarget || RenderTargetIndex >= kMaxRTs) return D3DERR_INVALIDCALL;
    *ppRenderTarget = m_renderTargets[RenderTargetIndex].ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
    m_depthStencil = static_cast<D3D9Surface*>(pNewZStencil);
    ID3D11RenderTargetView *rtvs[kMaxRTs] = {};
    UINT count = 0;
    for (UINT i = 0; i < kMaxRTs; i++) {
        if (m_renderTargets[i])
            rtvs[i] = m_renderTargets[i]->GetRTV();
        if (rtvs[i]) count = i + 1;
    }
    ID3D11DepthStencilView *dsv = m_depthStencil ? m_depthStencil->GetDSV() : nullptr;
    m_d3d11Context->OMSetRenderTargets(count, rtvs, dsv);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) {
    if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
    *ppZStencilSurface = m_depthStencil.ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::EndScene() {
    m_d3d11Context->Flush();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
    if (Flags & D3DCLEAR_TARGET) {
        float r = ((Color >> 16) & 0xFF) / 255.0f;
        float g = ((Color >>  8) & 0xFF) / 255.0f;
        float b = ((Color >>  0) & 0xFF) / 255.0f;
        float a = ((Color >> 24) & 0xFF) / 255.0f;
        float clearColor[4] = { r, g, b, a };
        for (auto &rt : m_renderTargets) {
            if (rt && rt->GetRTV())
                m_d3d11Context->ClearRenderTargetView(rt->GetRTV(), clearColor);
        }
    }
    if (m_depthStencil && m_depthStencil->GetDSV()) {
        UINT clearFlags = 0;
        if (Flags & D3DCLEAR_ZBUFFER)  clearFlags |= D3D11_CLEAR_DEPTH;
        if (Flags & D3DCLEAR_STENCIL)  clearFlags |= D3D11_CLEAR_STENCIL;
        if (clearFlags)
            m_d3d11Context->ClearDepthStencilView(m_depthStencil->GetDSV(), clearFlags, Z, (UINT8)Stencil);
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
    if (!pMatrix) return D3DERR_INVALIDCALL;
    if (State == D3DTS_VIEW)       m_view  = *pMatrix;
    else if (State == D3DTS_PROJECTION) m_proj = *pMatrix;
    else if (State == D3DTS_WORLD || State == (D3DTRANSFORMSTATETYPE)(256)) m_world = *pMatrix;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) {
    if (!pMatrix) return D3DERR_INVALIDCALL;
    if (State == D3DTS_VIEW)       *pMatrix = m_view;
    else if (State == D3DTS_PROJECTION) *pMatrix = m_proj;
    else if (State == D3DTS_WORLD || State == (D3DTRANSFORMSTATETYPE)(256)) *pMatrix = m_world;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
    D3DMATRIX current = {};
    GetTransform(State, &current);
    // Matrix multiply: result = current * pMatrix
    D3DMATRIX result = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                result.m[i][j] += current.m[i][k] * pMatrix->m[k][j];
    SetTransform(State, &result);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetViewport(const D3DVIEWPORT9 *pViewport) {
    if (!pViewport) return D3DERR_INVALIDCALL;
    m_viewport = *pViewport;
    D3D11_VIEWPORT vp;
    vp.TopLeftX = (float)pViewport->X;
    vp.TopLeftY = (float)pViewport->Y;
    vp.Width    = (float)pViewport->Width;
    vp.Height   = (float)pViewport->Height;
    vp.MinDepth = pViewport->MinZ;
    vp.MaxDepth = pViewport->MaxZ;
    m_d3d11Context->RSSetViewports(1, &vp);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetViewport(D3DVIEWPORT9 *pViewport) {
    if (!pViewport) return D3DERR_INVALIDCALL;
    *pViewport = m_viewport;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    switch (State) {
    case D3DRS_ZENABLE:                  m_rs.zenable = Value; m_rs.dsDirty = true; break;
    case D3DRS_FILLMODE:                 m_rs.fillmode = Value; m_rs.rsDirty = true; break;
    case D3DRS_ZWRITEENABLE:             m_rs.zwriteenable = Value; m_rs.dsDirty = true; break;
    case D3DRS_SRCBLEND:                 m_rs.srcblend = Value; m_rs.blendDirty = true; break;
    case D3DRS_DESTBLEND:                m_rs.destblend = Value; m_rs.blendDirty = true; break;
    case D3DRS_CULLMODE:                 m_rs.cullmode = Value; m_rs.rsDirty = true; break;
    case D3DRS_ZFUNC:                    m_rs.zfunc = Value; m_rs.dsDirty = true; break;
    case D3DRS_ALPHABLENDENABLE:         m_rs.alphablendenable = Value; m_rs.blendDirty = true; break;
    case D3DRS_BLENDOP:                  m_rs.blendop = Value; m_rs.blendDirty = true; break;
    case D3DRS_SEPARATEALPHABLENDENABLE: m_rs.separatealphablendenable = Value; m_rs.blendDirty = true; break;
    case D3DRS_SRCBLENDALPHA:            m_rs.srcblendalpha = Value; m_rs.blendDirty = true; break;
    case D3DRS_DESTBLENDALPHA:           m_rs.destblendalpha = Value; m_rs.blendDirty = true; break;
    case D3DRS_BLENDOPALPHA:             m_rs.blendopalpha = Value; m_rs.blendDirty = true; break;
    case D3DRS_BLENDFACTOR:              m_rs.blendfactor = Value; m_rs.blendDirty = true; break;
    case D3DRS_COLORWRITEENABLE:         m_rs.colorwriteenable = Value; m_rs.blendDirty = true; break;
    case D3DRS_COLORWRITEENABLE1:        m_rs.colorwriteenable1 = Value; m_rs.blendDirty = true; break;
    case D3DRS_COLORWRITEENABLE2:        m_rs.colorwriteenable2 = Value; m_rs.blendDirty = true; break;
    case D3DRS_COLORWRITEENABLE3:        m_rs.colorwriteenable3 = Value; m_rs.blendDirty = true; break;
    case D3DRS_STENCILENABLE:            m_rs.stencilenable = Value; m_rs.dsDirty = true; break;
    case D3DRS_STENCILFAIL:              m_rs.stencilfail = Value; m_rs.dsDirty = true; break;
    case D3DRS_STENCILZFAIL:             m_rs.stencilzfail = Value; m_rs.dsDirty = true; break;
    case D3DRS_STENCILPASS:              m_rs.stencilpass = Value; m_rs.dsDirty = true; break;
    case D3DRS_STENCILFUNC:              m_rs.stencilfunc = Value; m_rs.dsDirty = true; break;
    case D3DRS_STENCILREF:               m_rs.stencilref = Value; m_rs.dsDirty = true; break;
    case D3DRS_STENCILMASK:              m_rs.stencilmask = Value; m_rs.dsDirty = true; break;
    case D3DRS_STENCILWRITEMASK:         m_rs.stencilwritemask = Value; m_rs.dsDirty = true; break;
    case D3DRS_TWOSIDEDSTENCILMODE:      m_rs.twosidedstencilmode = Value; m_rs.dsDirty = true; break;
    case D3DRS_CCW_STENCILFAIL:          m_rs.ccw_stencilfail = Value; m_rs.dsDirty = true; break;
    case D3DRS_CCW_STENCILZFAIL:         m_rs.ccw_stencilzfail = Value; m_rs.dsDirty = true; break;
    case D3DRS_CCW_STENCILPASS:          m_rs.ccw_stencilpass = Value; m_rs.dsDirty = true; break;
    case D3DRS_CCW_STENCILFUNC:          m_rs.ccw_stencilfunc = Value; m_rs.dsDirty = true; break;
    case D3DRS_SCISSORTESTENABLE:        m_rs.scissortestenable = Value; m_rs.rsDirty = true; break;
    case D3DRS_DEPTHBIAS:                m_rs.depthbias = Value; m_rs.rsDirty = true; break;
    case D3DRS_SLOPESCALEDEPTHBIAS:      m_rs.slopescaledepthbias = Value; m_rs.rsDirty = true; break;
    case D3DRS_MULTISAMPLEANTIALIAS:     m_rs.multisampleantialias = Value; m_rs.rsDirty = true; break;
    default: break;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) {
    if (!pValue) return D3DERR_INVALIDCALL;
    switch (State) {
    case D3DRS_ZENABLE:             *pValue = m_rs.zenable; break;
    case D3DRS_FILLMODE:            *pValue = m_rs.fillmode; break;
    case D3DRS_ZWRITEENABLE:        *pValue = m_rs.zwriteenable; break;
    case D3DRS_SRCBLEND:            *pValue = m_rs.srcblend; break;
    case D3DRS_DESTBLEND:           *pValue = m_rs.destblend; break;
    case D3DRS_CULLMODE:            *pValue = m_rs.cullmode; break;
    case D3DRS_ZFUNC:               *pValue = m_rs.zfunc; break;
    case D3DRS_ALPHABLENDENABLE:    *pValue = m_rs.alphablendenable; break;
    case D3DRS_BLENDOP:             *pValue = m_rs.blendop; break;
    case D3DRS_STENCILENABLE:       *pValue = m_rs.stencilenable; break;
    case D3DRS_SCISSORTESTENABLE:   *pValue = m_rs.scissortestenable; break;
    case D3DRS_COLORWRITEENABLE:    *pValue = m_rs.colorwriteenable; break;
    default:                         *pValue = 0; break;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) {
    if (!ppSB) return D3DERR_INVALIDCALL;
    *ppSB = new D3D9StateBlock(this, Type);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) {
    return CreateStateBlock(D3DSBT_ALL, ppSB);
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
    if (!ppTexture || Stage >= kMaxSamplers) return D3DERR_INVALIDCALL;
    *ppTexture = m_textures[Stage].ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
    if (Stage >= kMaxSamplers) return D3DERR_INVALIDCALL;
    m_textures[Stage] = pTexture;

    // Bind to D3D11
    ID3D11ShaderResourceView *srv = nullptr;
    if (pTexture) {
        if (pTexture->GetType() == D3DRTYPE_TEXTURE)
            srv = static_cast<D3D9Texture2D*>(pTexture)->GetSRV();
        else if (pTexture->GetType() == D3DRTYPE_CUBETEXTURE)
            srv = static_cast<D3D9CubeTexture*>(pTexture)->GetSRV();
    }
    m_d3d11Context->PSSetShaderResources(Stage, 1, &srv);
    m_d3d11Context->VSSetShaderResources(Stage, 1, &srv);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
    if (!pValue || Stage >= 8) return D3DERR_INVALIDCALL;
    *pValue = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    return D3D_OK; // Fixed-function texture stages ignored for now
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) {
    if (!pValue || Sampler >= kMaxSamplers) return D3DERR_INVALIDCALL;
    auto &ss = m_samplerStates[Sampler];
    switch (Type) {
    case D3DSAMP_ADDRESSU:    *pValue = ss.addressU; break;
    case D3DSAMP_ADDRESSV:    *pValue = ss.addressV; break;
    case D3DSAMP_ADDRESSW:    *pValue = ss.addressW; break;
    case D3DSAMP_MAGFILTER:   *pValue = ss.magFilter; break;
    case D3DSAMP_MINFILTER:   *pValue = ss.minFilter; break;
    case D3DSAMP_MIPFILTER:   *pValue = ss.mipFilter; break;
    case D3DSAMP_MAXANISOTROPY: *pValue = ss.maxAnisotropy; break;
    case D3DSAMP_BORDERCOLOR: *pValue = ss.borderColor; break;
    case D3DSAMP_MIPMAPLODBIAS: *pValue = *(DWORD*)&ss.mipLodBias; break;
    case D3DSAMP_MAXMIPLEVEL: *pValue = ss.maxMipLevel; break;
    default:                   *pValue = 0; break;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (Sampler >= kMaxSamplers) return D3DERR_INVALIDCALL;
    auto &ss = m_samplerStates[Sampler];
    ss.dirty = true;
    switch (Type) {
    case D3DSAMP_ADDRESSU:    ss.addressU = (D3DTEXTUREADDRESS)Value; break;
    case D3DSAMP_ADDRESSV:    ss.addressV = (D3DTEXTUREADDRESS)Value; break;
    case D3DSAMP_ADDRESSW:    ss.addressW = (D3DTEXTUREADDRESS)Value; break;
    case D3DSAMP_MAGFILTER:   ss.magFilter = (D3DTEXTUREFILTERTYPE)Value; break;
    case D3DSAMP_MINFILTER:   ss.minFilter = (D3DTEXTUREFILTERTYPE)Value; break;
    case D3DSAMP_MIPFILTER:   ss.mipFilter = (D3DTEXTUREFILTERTYPE)Value; break;
    case D3DSAMP_MAXANISOTROPY: ss.maxAnisotropy = Value; break;
    case D3DSAMP_BORDERCOLOR: ss.borderColor = Value; break;
    case D3DSAMP_MIPMAPLODBIAS: ss.mipLodBias = *(float*)&Value; break;
    case D3DSAMP_MAXMIPLEVEL: ss.maxMipLevel = Value; break;
    default: break;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetScissorRect(const RECT *pRect) {
    if (!pRect) return D3DERR_INVALIDCALL;
    m_scissorRect = *pRect;
    D3D11_RECT r = { pRect->left, pRect->top, pRect->right, pRect->bottom };
    m_d3d11Context->RSSetScissorRects(1, &r);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetScissorRect(RECT *pRect) {
    if (!pRect) return D3DERR_INVALIDCALL;
    *pRect = m_scissorRect;
    return D3D_OK;
}

// Shader management
HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) {
    if (!ppDecl || !pVertexElements) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppDecl);
    *ppDecl = new D3D9VertexDeclaration(this, pVertexElements);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
    m_vertexDecl = static_cast<D3D9VertexDeclaration*>(pDecl);
    m_inputLayout = nullptr; // invalidate cached input layout
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
    if (!ppDecl) return D3DERR_INVALIDCALL;
    *ppDecl = m_vertexDecl.ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetFVF(DWORD FVF) {
    m_fvf = FVF;
    m_vertexDecl = nullptr;
    m_inputLayout = nullptr;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetFVF(DWORD *pFVF) {
    if (!pFVF) return D3DERR_INVALIDCALL;
    *pFVF = m_fvf;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppShader);
    UINT size = 0;
    const DWORD *p = pFunction;
    while (p && *p != 0x0000FFFF) { p++; size += 4; }
    size += 4;
    *ppShader = new D3D9VertexShader(this, pFunction, size);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShader(IDirect3DVertexShader9 *pShader) {
    m_vertexShader = static_cast<D3D9VertexShader*>(pShader);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_vertexShader.ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    UINT count = std::min(Vector4fCount, kMaxVSConsts - StartRegister);
    memcpy(&m_vsConstF[StartRegister * 4], pConstantData, count * 16);
    m_vsConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    memcpy(pConstantData, &m_vsConstF[StartRegister * 4], Vector4fCount * 16);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) {
    if (StreamNumber >= kMaxStreams) return D3DERR_INVALIDCALL;
    auto &stream = m_streams[StreamNumber];
    stream.vertexBuffer = static_cast<D3D9VertexBuffer*>(pStreamData);
    stream.offset = OffsetInBytes;
    stream.stride = Stride;
    if (stream.vertexBuffer)
        stream.vertexBuffer->SetStride(Stride);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) {
    if (!ppStreamData || StreamNumber >= kMaxStreams) return D3DERR_INVALIDCALL;
    auto &stream = m_streams[StreamNumber];
    *ppStreamData = stream.vertexBuffer.ref();
    if (pOffsetInBytes) *pOffsetInBytes = stream.offset;
    if (pStride)        *pStride        = stream.stride;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
    if (StreamNumber >= kMaxStreams) return D3DERR_INVALIDCALL;
    m_streams[StreamNumber].freq = Setting;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) {
    if (!pSetting || StreamNumber >= kMaxStreams) return D3DERR_INVALIDCALL;
    *pSetting = m_streams[StreamNumber].freq;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
    m_indexBuffer = static_cast<D3D9IndexBuffer*>(pIndexData);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) {
    if (!ppIndexData) return D3DERR_INVALIDCALL;
    *ppIndexData = m_indexBuffer.ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    InitReturnPtr(ppShader);
    UINT size = 0;
    const DWORD *p = pFunction;
    while (p && *p != 0x0000FFFF) { p++; size += 4; }
    size += 4;
    *ppShader = new D3D9PixelShader(this, pFunction, size);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShader(IDirect3DPixelShader9 *pShader) {
    m_pixelShader = static_cast<D3D9PixelShader*>(pShader);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_pixelShader.ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    UINT count = std::min(Vector4fCount, kMaxPSConsts - StartRegister);
    memcpy(&m_psConstF[StartRegister * 4], pConstantData, count * 16);
    m_psConstDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    memcpy(pConstantData, &m_psConstF[StartRegister * 4], Vector4fCount * 16);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
    if (!ppQuery) return D3DERR_INVALIDCALL;
    *ppQuery = new D3D9Query(this, Type);
    return D3D_OK;
}

// ---------------------------------------------------------------------------
// State flush helpers
// ---------------------------------------------------------------------------

void D3D9Device::UpdateBlendState() {
    D3D11_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable  = FALSE;
    desc.IndependentBlendEnable = (m_rs.colorwriteenable1 != 0xF || m_rs.colorwriteenable2 != 0xF || m_rs.colorwriteenable3 != 0xF);

    for (int i = 0; i < 4; i++) {
        auto &rt = desc.RenderTarget[i];
        rt.BlendEnable = m_rs.alphablendenable ? TRUE : FALSE;
        rt.SrcBlend    = ConvertBlend((D3DBLEND)m_rs.srcblend);
        rt.DestBlend   = ConvertBlend((D3DBLEND)m_rs.destblend);
        rt.BlendOp     = ConvertBlendOp((D3DBLENDOP)m_rs.blendop);
        if (m_rs.separatealphablendenable) {
            rt.SrcBlendAlpha  = ConvertBlend((D3DBLEND)m_rs.srcblendalpha);
            rt.DestBlendAlpha = ConvertBlend((D3DBLEND)m_rs.destblendalpha);
            rt.BlendOpAlpha   = ConvertBlendOp((D3DBLENDOP)m_rs.blendopalpha);
        } else {
            rt.SrcBlendAlpha  = rt.SrcBlend;
            rt.DestBlendAlpha = rt.DestBlend;
            rt.BlendOpAlpha   = rt.BlendOp;
        }
        DWORD cwe = (i == 0) ? m_rs.colorwriteenable :
                    (i == 1) ? m_rs.colorwriteenable1 :
                    (i == 2) ? m_rs.colorwriteenable2 : m_rs.colorwriteenable3;
        rt.RenderTargetWriteMask = 0;
        if (cwe & D3DCOLORWRITEENABLE_RED)   rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (cwe & D3DCOLORWRITEENABLE_GREEN) rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (cwe & D3DCOLORWRITEENABLE_BLUE)  rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (cwe & D3DCOLORWRITEENABLE_ALPHA) rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }

    m_blendState = nullptr;
    m_d3d11Device->CreateBlendState(&desc, &m_blendState);

    float blendFactor[4];
    blendFactor[0] = ((m_rs.blendfactor >> 16) & 0xFF) / 255.0f;
    blendFactor[1] = ((m_rs.blendfactor >>  8) & 0xFF) / 255.0f;
    blendFactor[2] = ((m_rs.blendfactor >>  0) & 0xFF) / 255.0f;
    blendFactor[3] = ((m_rs.blendfactor >> 24) & 0xFF) / 255.0f;
    m_d3d11Context->OMSetBlendState(m_blendState.ptr(), blendFactor, 0xFFFFFFFF);
}

void D3D9Device::UpdateDepthStencilState() {
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable    = m_rs.zenable ? TRUE : FALSE;
    desc.DepthWriteMask = m_rs.zwriteenable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc      = ConvertCmpFunc((D3DCMPFUNC)m_rs.zfunc);
    desc.StencilEnable  = m_rs.stencilenable ? TRUE : FALSE;
    desc.StencilReadMask  = (UINT8)m_rs.stencilmask;
    desc.StencilWriteMask = (UINT8)m_rs.stencilwritemask;
    desc.FrontFace.StencilFailOp      = ConvertStencilOp((D3DSTENCILOP)m_rs.stencilfail);
    desc.FrontFace.StencilDepthFailOp = ConvertStencilOp((D3DSTENCILOP)m_rs.stencilzfail);
    desc.FrontFace.StencilPassOp      = ConvertStencilOp((D3DSTENCILOP)m_rs.stencilpass);
    desc.FrontFace.StencilFunc        = ConvertCmpFunc((D3DCMPFUNC)m_rs.stencilfunc);
    if (m_rs.twosidedstencilmode) {
        desc.BackFace.StencilFailOp      = ConvertStencilOp((D3DSTENCILOP)m_rs.ccw_stencilfail);
        desc.BackFace.StencilDepthFailOp = ConvertStencilOp((D3DSTENCILOP)m_rs.ccw_stencilzfail);
        desc.BackFace.StencilPassOp      = ConvertStencilOp((D3DSTENCILOP)m_rs.ccw_stencilpass);
        desc.BackFace.StencilFunc        = ConvertCmpFunc((D3DCMPFUNC)m_rs.ccw_stencilfunc);
    } else {
        desc.BackFace = desc.FrontFace;
    }
    m_dsState = nullptr;
    m_d3d11Device->CreateDepthStencilState(&desc, &m_dsState);
    m_d3d11Context->OMSetDepthStencilState(m_dsState.ptr(), m_rs.stencilref);
}

void D3D9Device::UpdateRasterizerState() {
    D3D11_RASTERIZER_DESC desc = {};
    desc.FillMode              = ConvertFillMode((D3DFILLMODE)m_rs.fillmode);
    desc.CullMode              = ConvertCullMode((D3DCULL)m_rs.cullmode);
    desc.FrontCounterClockwise = FALSE; // D3D9 uses clockwise front face
    desc.DepthBias             = 0;
    desc.DepthBiasClamp        = 0.0f;
    desc.SlopeScaledDepthBias  = *(float*)&m_rs.slopescaledepthbias;
    desc.DepthClipEnable       = TRUE;
    desc.ScissorEnable         = m_rs.scissortestenable ? TRUE : FALSE;
    desc.MultisampleEnable     = m_rs.multisampleantialias ? TRUE : FALSE;
    desc.AntialiasedLineEnable = FALSE;
    m_rastState = nullptr;
    m_d3d11Device->CreateRasterizerState(&desc, &m_rastState);
    m_d3d11Context->RSSetState(m_rastState.ptr());
}

void D3D9Device::UpdateSamplerState(UINT sampler) {
    auto &ss = m_samplerStates[sampler];
    if (!ss.dirty) return;
    ss.dirty = false;

    bool aniso = ss.maxAnisotropy > 1;
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter         = ConvertFilter(ss.minFilter, ss.magFilter, ss.mipFilter, aniso);
    desc.AddressU       = ConvertTextureAddress(ss.addressU);
    desc.AddressV       = ConvertTextureAddress(ss.addressV);
    desc.AddressW       = ConvertTextureAddress(ss.addressW);
    desc.MipLODBias     = ss.mipLodBias;
    desc.MaxAnisotropy  = ss.maxAnisotropy;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    DWORD bc = ss.borderColor;
    desc.BorderColor[0] = ((bc >> 16) & 0xFF) / 255.0f;
    desc.BorderColor[1] = ((bc >>  8) & 0xFF) / 255.0f;
    desc.BorderColor[2] = ((bc >>  0) & 0xFF) / 255.0f;
    desc.BorderColor[3] = ((bc >> 24) & 0xFF) / 255.0f;
    desc.MinLOD         = (float)ss.maxMipLevel;
    desc.MaxLOD         = D3D11_FLOAT32_MAX;

    m_d3d11Samplers[sampler] = nullptr;
    m_d3d11Device->CreateSamplerState(&desc, &m_d3d11Samplers[sampler]);

    ID3D11SamplerState *samplerPtr = m_d3d11Samplers[sampler].ptr();
    m_d3d11Context->PSSetSamplers(sampler, 1, &samplerPtr);
    m_d3d11Context->VSSetSamplers(sampler, 1, &samplerPtr);
}

void D3D9Device::FlushRenderStates() {
    if (m_rs.blendDirty) {
        UpdateBlendState();
        m_rs.blendDirty = false;
    }
    if (m_rs.dsDirty) {
        UpdateDepthStencilState();
        m_rs.dsDirty = false;
    }
    if (m_rs.rsDirty) {
        UpdateRasterizerState();
        m_rs.rsDirty = false;
    }
}

void D3D9Device::FlushSamplerStates() {
    for (UINT i = 0; i < kMaxSamplers; i++) {
        if (m_samplerStates[i].dirty)
            UpdateSamplerState(i);
    }
}

void D3D9Device::FlushConstantBuffers() {
    if (m_vsConstDirty && m_vsCB) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(m_d3d11Context->Map(m_vsCB.ptr(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, m_vsConstF.data(), kMaxVSConsts * 16);
            m_d3d11Context->Unmap(m_vsCB.ptr(), 0);
        }
        ID3D11Buffer *cb = m_vsCB.ptr();
        m_d3d11Context->VSSetConstantBuffers(0, 1, &cb);
        m_vsConstDirty = false;
    }
    if (m_psConstDirty && m_psCB) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(m_d3d11Context->Map(m_psCB.ptr(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, m_psConstF.data(), kMaxPSConsts * 16);
            m_d3d11Context->Unmap(m_psCB.ptr(), 0);
        }
        ID3D11Buffer *cb = m_psCB.ptr();
        m_d3d11Context->PSSetConstantBuffers(0, 1, &cb);
        m_psConstDirty = false;
    }
}

void D3D9Device::UploadBufferToGPU(D3D9VertexBuffer *vb) {
    if (!vb->GetD3D11Buffer()) return;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_d3d11Context->Map(vb->GetD3D11Buffer(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, vb->GetData(), vb->GetSize());
        m_d3d11Context->Unmap(vb->GetD3D11Buffer(), 0);
    }
}

void D3D9Device::UploadBufferToGPU(D3D9IndexBuffer *ib) {
    if (!ib->GetD3D11Buffer()) return;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = m_d3d11Context->Map(ib->GetD3D11Buffer(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, ib->GetData(), ib->GetSize());
        m_d3d11Context->Unmap(ib->GetD3D11Buffer(), 0);
    }
}

void D3D9Device::FlushInputLayout(D3DPRIMITIVETYPE primType) {
    // Set primitive topology
    m_d3d11Context->IASetPrimitiveTopology(ConvertPrimType(primType));

    // Bind vertex buffers
    for (UINT i = 0; i < kMaxStreams; i++) {
        auto &stream = m_streams[i];
        if (stream.vertexBuffer && stream.vertexBuffer->GetD3D11Buffer()) {
            ID3D11Buffer *buf = stream.vertexBuffer->GetD3D11Buffer();
            UINT stride = stream.stride;
            UINT offset = stream.offset;
            m_d3d11Context->IASetVertexBuffers(i, 1, &buf, &stride, &offset);
        }
    }

    // Bind index buffer
    if (m_indexBuffer && m_indexBuffer->GetD3D11Buffer()) {
        m_d3d11Context->IASetIndexBuffer(m_indexBuffer->GetD3D11Buffer(), m_indexBuffer->GetFormat(), 0);
    }
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
    FlushRenderStates();
    FlushSamplerStates();
    FlushConstantBuffers();
    FlushInputLayout(PrimitiveType);
    UINT vertexCount = PrimCountToVertexCount(PrimitiveType, PrimitiveCount);
    m_d3d11Context->Draw(vertexCount, StartVertex);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) {
    FlushRenderStates();
    FlushSamplerStates();
    FlushConstantBuffers();
    FlushInputLayout(PrimitiveType);
    UINT indexCount = PrimCountToVertexCount(PrimitiveType, primCount);
    m_d3d11Context->DrawIndexed(indexCount, startIndex, BaseVertexIndex);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    if (!pVertexStreamZeroData) return D3DERR_INVALIDCALL;

    UINT vertexCount = PrimCountToVertexCount(PrimitiveType, PrimitiveCount);
    UINT dataSize = vertexCount * VertexStreamZeroStride;

    // Create a temporary dynamic vertex buffer
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = dataSize;
    desc.Usage          = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pVertexStreamZeroData;

    Com<ID3D11Buffer> tmpVB;
    if (FAILED(m_d3d11Device->CreateBuffer(&desc, &initData, &tmpVB)))
        return D3DERR_INVALIDCALL;

    ID3D11Buffer *buf = tmpVB.ptr();
    UINT offset = 0;
    m_d3d11Context->IASetVertexBuffers(0, 1, &buf, &VertexStreamZeroStride, &offset);
    m_d3d11Context->IASetPrimitiveTopology(ConvertPrimType(PrimitiveType));

    FlushRenderStates();
    FlushSamplerStates();
    FlushConstantBuffers();
    m_d3d11Context->Draw(vertexCount, 0);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    if (!pVertexStreamZeroData || !pIndexData) return D3DERR_INVALIDCALL;

    UINT indexCount  = PrimCountToVertexCount(PrimitiveType, PrimitiveCount);
    UINT indexStride = (IndexDataFormat == D3DFMT_INDEX16) ? 2 : 4;
    UINT dataSize    = NumVertices * VertexStreamZeroStride;

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = dataSize;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = pVertexStreamZeroData;
    Com<ID3D11Buffer> tmpVB;
    m_d3d11Device->CreateBuffer(&vbDesc, &vbData, &tmpVB);

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = indexCount * indexStride;
    ibDesc.Usage = D3D11_USAGE_DYNAMIC;
    ibDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = pIndexData;
    Com<ID3D11Buffer> tmpIB;
    m_d3d11Device->CreateBuffer(&ibDesc, &ibData, &tmpIB);

    ID3D11Buffer *vb = tmpVB.ptr();
    UINT stride = VertexStreamZeroStride, offset = 0;
    m_d3d11Context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    m_d3d11Context->IASetIndexBuffer(tmpIB.ptr(),
        (IndexDataFormat == D3DFMT_INDEX16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
    m_d3d11Context->IASetPrimitiveTopology(ConvertPrimType(PrimitiveType));

    FlushRenderStates();
    FlushSamplerStates();
    FlushConstantBuffers();
    m_d3d11Context->DrawIndexed(indexCount, 0, 0);
    return D3D_OK;
}

} // namespace dxmt::d3d9
