#include "d3d9_main.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <cstring>
#include <cstdlib>

namespace dxmt::d3d9 {

D3D9Main::D3D9Main() {
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&m_dxgiFactory));
    if (FAILED(hr))
        Logger::err("D3D9Main: Failed to create DXGI factory");
}

D3D9Main::~D3D9Main() {}

HRESULT STDMETHODCALLTYPE D3D9Main::QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
        *ppvObject = static_cast<IDirect3D9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Main::RegisterSoftwareDevice(void *) {
    return D3D_OK;
}

UINT STDMETHODCALLTYPE D3D9Main::GetAdapterCount() {
    UINT count = 0;
    Com<IDXGIAdapter> adapter;
    while (SUCCEEDED(m_dxgiFactory->EnumAdapters(count, &adapter)))
        count++;
    return count ? count : 1;
}

HRESULT STDMETHODCALLTYPE D3D9Main::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
    if (!pIdentifier) return D3DERR_INVALIDCALL;
    memset(pIdentifier, 0, sizeof(*pIdentifier));

    Com<IDXGIAdapter> adapter;
    if (FAILED(m_dxgiFactory->EnumAdapters(Adapter, &adapter))) {
        strcpy(pIdentifier->Description, "Default Adapter");
        return D3D_OK;
    }
    DXGI_ADAPTER_DESC desc = {};
    adapter->GetDesc(&desc);

    // Convert wide string to narrow
    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1,
        pIdentifier->Description, sizeof(pIdentifier->Description), nullptr, nullptr);
    strcpy(pIdentifier->Driver, "dxmt_d3d9.dll");
    pIdentifier->VendorId  = desc.VendorId;
    pIdentifier->DeviceId  = desc.DeviceId;
    pIdentifier->SubSysId  = desc.SubSysId;
    pIdentifier->Revision  = desc.Revision;
    return D3D_OK;
}

UINT STDMETHODCALLTYPE D3D9Main::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
    // Return a reasonable default
    return 1;
}

HRESULT STDMETHODCALLTYPE D3D9Main::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
    if (!pMode) return D3DERR_INVALIDCALL;
    pMode->Width       = 1920;
    pMode->Height      = 1080;
    pMode->RefreshRate = 60;
    pMode->Format      = Format;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Main::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
    if (!pMode) return D3DERR_INVALIDCALL;
    pMode->Width       = 1920;
    pMode->Height      = 1080;
    pMode->RefreshRate = 60;
    pMode->Format      = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Main::CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) {
    if (DevType != D3DDEVTYPE_HAL) return D3DERR_INVALIDDEVICE;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Main::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
    DXGI_FORMAT fmt = ConvertFormat(CheckFormat);
    if (fmt == DXGI_FORMAT_UNKNOWN && CheckFormat != D3DFMT_UNKNOWN)
        return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Main::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels) {
    if (pQualityLevels) *pQualityLevels = 1;
    if (MultiSampleType > D3DMULTISAMPLE_4_SAMPLES) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Main::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) {
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Main::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Main::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) {
    if (!pCaps) return D3DERR_INVALIDCALL;
    memset(pCaps, 0, sizeof(*pCaps));

    pCaps->DeviceType         = D3DDEVTYPE_HAL;
    pCaps->AdapterOrdinal     = Adapter;
    pCaps->Caps               = D3DCAPS_READ_SCANLINE;
    pCaps->Caps2              = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_CANAUTOGENMIPMAP;
    pCaps->Caps3              = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
    pCaps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    pCaps->DevCaps            = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY |
                                D3DDEVCAPS_TLVERTEX_SYSTEMMEMORY | D3DDEVCAPS_TLVERTEX_VIDEOMEMORY |
                                D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY |
                                D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                                D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_HWRASTERIZATION |
                                D3DDEVCAPS_PUREDEVICE | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX;
    pCaps->PrimitiveMiscCaps  = 0x002FB5EF;
    pCaps->RasterCaps         = D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_DEPTHBIAS |
                                D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_FOGVERTEX |
                                D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_SCISSORTEST |
                                D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_ZTEST |
                                D3DPRASTERCAPS_COLORPERSPECTIVE;
    pCaps->ZCmpCaps           = 0xFF;
    pCaps->SrcBlendCaps       = 0x3FFF;
    pCaps->DestBlendCaps      = 0x3FFF;
    pCaps->AlphaCmpCaps       = 0xFF;
    pCaps->ShadeCaps          = D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_COLORGOURAUDRGB |
                                D3DPSHADECAPS_FOGGOURAUD | D3DPSHADECAPS_SPECULARGOURAUDRGB;
    pCaps->TextureCaps        = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_CUBEMAP |
                                D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_MIPCUBEMAP |
                                D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_VOLUMEMAP |
                                D3DPTEXTURECAPS_MIPVOLUMEMAP;
    pCaps->TextureFilterCaps  = D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
                                D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFPOINT |
                                D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFLINEAR |
                                D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    pCaps->CubeTextureFilterCaps  = pCaps->TextureFilterCaps;
    pCaps->VolumeTextureFilterCaps = pCaps->TextureFilterCaps;
    pCaps->TextureAddressCaps     = D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_CLAMP |
                                    D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRROR |
                                    D3DPTADDRESSCAPS_MIRRORONCE | D3DPTADDRESSCAPS_WRAP;
    pCaps->VolumeTextureAddressCaps = pCaps->TextureAddressCaps;
    pCaps->MaxTextureWidth    = 16384;
    pCaps->MaxTextureHeight   = 16384;
    pCaps->MaxVolumeExtent    = 2048;
    pCaps->MaxTextureRepeat   = 8192;
    pCaps->MaxTextureAspectRatio = 8192;
    pCaps->MaxAnisotropy      = 16;
    pCaps->MaxVertexW         = 1e10f;
    pCaps->StencilCaps        = 0x1FF;
    pCaps->TextureOpCaps      = 0x03FFFFFF;
    pCaps->MaxTextureBlendStages = 8;
    pCaps->MaxSimultaneousTextures = 8;
    pCaps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 |
                                  D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS |
                                  D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TWEENING;
    pCaps->MaxActiveLights    = 8;
    pCaps->MaxUserClipPlanes  = 6;
    pCaps->MaxVertexBlendMatrices = 4;
    pCaps->MaxVertexBlendMatrixIndex = 8;
    pCaps->MaxPointSize       = 256.0f;
    pCaps->MaxPrimitiveCount  = 0xFFFFFFFF;
    pCaps->MaxVertexIndex     = 0xFFFFFFFF;
    pCaps->MaxStreams          = 16;
    pCaps->MaxStreamStride    = 512;
    pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
    pCaps->MaxVertexShaderConst = 256;
    pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
    pCaps->PixelShader1xMaxValue = 8.0f;
    pCaps->DevCaps2           = 0x0000001F;
    pCaps->NumSimultaneousRTs = 4;
    pCaps->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MAGFPOINT |
                                   D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFLINEAR;
    pCaps->MaxVShaderInstructionsExecuted = 0xFFFFFFFF;
    pCaps->MaxPShaderInstructionsExecuted = 0xFFFFFFFF;
    pCaps->MaxVertexShader30InstructionSlots = 32768;
    pCaps->MaxPixelShader30InstructionSlots  = 32768;
    pCaps->DeclTypes          = 0x000BFFFF;
    return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE D3D9Main::GetAdapterMonitor(UINT Adapter) {
    return nullptr;
}

HRESULT STDMETHODCALLTYPE D3D9Main::CreateDevice(
        UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface) {
    if (!ppReturnedDeviceInterface || !pPresentationParameters)
        return D3DERR_INVALIDCALL;
    InitReturnPtr(ppReturnedDeviceInterface);

    // Create D3D11 device
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    Com<ID3D11Device>        d3d11Device;
    Com<ID3D11DeviceContext> d3d11Context;
    D3D_FEATURE_LEVEL        featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, &d3d11Device, &featureLevel, &d3d11Context);

    if (FAILED(hr)) {
        Logger::err(str::format("D3D9Main::CreateDevice: D3D11CreateDevice failed: ", hr));
        return D3DERR_INVALIDDEVICE;
    }

    // Create DXGI swap chain
    Com<IDXGIDevice>  dxgiDevice;
    Com<IDXGIAdapter> dxgiAdapter;
    d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    dxgiDevice->GetAdapter(&dxgiAdapter);

    Com<IDXGIFactory> dxgiFactory;
    dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));

    HWND hwnd = pPresentationParameters->hDeviceWindow;
    if (!hwnd) hwnd = hFocusWindow;

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    scDesc.BufferDesc.Width  = pPresentationParameters->BackBufferWidth;
    scDesc.BufferDesc.Height = pPresentationParameters->BackBufferHeight;
    scDesc.BufferDesc.Format = pPresentationParameters->BackBufferFormat == D3DFMT_UNKNOWN
        ? DXGI_FORMAT_B8G8R8A8_UNORM
        : ConvertFormat(pPresentationParameters->BackBufferFormat);
    if (scDesc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN)
        scDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.BufferDesc.RefreshRate.Numerator   = pPresentationParameters->FullScreen_RefreshRateInHz;
    scDesc.BufferDesc.RefreshRate.Denominator = 1;
    scDesc.SampleDesc.Count   = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount        = pPresentationParameters->BackBufferCount ? pPresentationParameters->BackBufferCount : 1;
    scDesc.OutputWindow       = hwnd;
    scDesc.Windowed           = pPresentationParameters->Windowed;
    scDesc.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;
    scDesc.Flags              = 0;

    Com<IDXGISwapChain> swapChain;
    hr = dxgiFactory->CreateSwapChain(d3d11Device.ptr(), &scDesc, &swapChain);
    if (FAILED(hr)) {
        Logger::err(str::format("D3D9Main::CreateDevice: CreateSwapChain failed: ", hr));
        return D3DERR_INVALIDDEVICE;
    }

    auto device = new D3D9Device(this, d3d11Device.ptr(), swapChain.ptr(),
                                  Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                  pPresentationParameters);
    *ppReturnedDeviceInterface = device;
    return D3D_OK;
}

} // namespace dxmt::d3d9
