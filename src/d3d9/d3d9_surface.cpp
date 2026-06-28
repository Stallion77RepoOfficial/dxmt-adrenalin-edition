#include "d3d9_surface.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include <cstring>

namespace dxmt::d3d9 {

D3D9Surface::D3D9Surface(D3D9Device *pDevice, ID3D11Texture2D *pTexture,
                          UINT mipLevel, UINT arraySlice,
                          D3DFORMAT fmt, DWORD usage)
    : m_device(pDevice), m_mipLevel(mipLevel), m_arraySlice(arraySlice), m_ownTexture(false) {
    m_texture = pTexture;

    D3D11_TEXTURE2D_DESC texDesc = {};
    pTexture->GetDesc(&texDesc);

    UINT w = texDesc.Width  >> mipLevel; if (!w) w = 1;
    UINT h = texDesc.Height >> mipLevel; if (!h) h = 1;

    m_desc.Width              = w;
    m_desc.Height             = h;
    m_desc.Format             = fmt;
    m_desc.Type               = D3DRTYPE_SURFACE;
    m_desc.Usage              = usage;
    m_desc.Pool               = D3DPOOL_DEFAULT;
    m_desc.MultiSampleType    = D3DMULTISAMPLE_NONE;
    m_desc.MultiSampleQuality = 0;

    // Create views based on usage
    ID3D11Device *d3d = pDevice->GetD3D11Device();

    if (usage & D3DUSAGE_RENDERTARGET) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = texDesc.Format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = mipLevel;
        d3d->CreateRenderTargetView(pTexture, &rtvDesc, &m_rtv);
    } else if (IsDepthFormat(fmt)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = ConvertFormat(fmt);
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Texture2D.MipSlice = mipLevel;
        d3d->CreateDepthStencilView(pTexture, &dsvDesc, &m_dsv);
    }
}

D3D9Surface::D3D9Surface(D3D9Device *pDevice, UINT width, UINT height,
                          D3DFORMAT fmt, DWORD usage, D3DPOOL pool, bool lockable)
    : m_device(pDevice), m_mipLevel(0), m_arraySlice(0), m_ownTexture(true) {
    m_desc.Width              = width;
    m_desc.Height             = height;
    m_desc.Format             = fmt;
    m_desc.Type               = D3DRTYPE_SURFACE;
    m_desc.Usage              = usage;
    m_desc.Pool               = pool;
    m_desc.MultiSampleType    = D3DMULTISAMPLE_NONE;
    m_desc.MultiSampleQuality = 0;

    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) {
        UINT bpp = GetFormatBPP(fmt);
        m_sysMemData.resize(width * height * bpp, 0);
        return;
    }

    DXGI_FORMAT dxgiFmt = IsDepthFormat(fmt) ? ConvertDepthFormat(fmt) : ConvertFormat(fmt);
    if (dxgiFmt == DXGI_FORMAT_UNKNOWN)
        dxgiFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width              = width;
    texDesc.Height             = height;
    texDesc.MipLevels          = 1;
    texDesc.ArraySize          = 1;
    texDesc.Format             = dxgiFmt;
    texDesc.SampleDesc.Count   = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage              = (usage & D3DUSAGE_DYNAMIC) ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    texDesc.CPUAccessFlags     = (usage & D3DUSAGE_DYNAMIC) ? D3D11_CPU_ACCESS_WRITE : 0;

    if (usage & D3DUSAGE_RENDERTARGET) {
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    } else if (IsDepthFormat(fmt)) {
        texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    } else {
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (lockable) {
            texDesc.Usage = D3D11_USAGE_DYNAMIC;
            texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        }
    }

    ID3D11Device *d3d = pDevice->GetD3D11Device();
    HRESULT hr = d3d->CreateTexture2D(&texDesc, nullptr, &m_texture);
    if (FAILED(hr)) {
        Logger::err("D3D9Surface: Failed to create D3D11 texture");
        return;
    }

    if (usage & D3DUSAGE_RENDERTARGET) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = dxgiFmt;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        d3d->CreateRenderTargetView(m_texture.ptr(), &rtvDesc, &m_rtv);
        d3d->CreateShaderResourceView(m_texture.ptr(), nullptr, &m_srv);
    } else if (IsDepthFormat(fmt)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = ConvertFormat(fmt);
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Texture2D.MipSlice = 0;
        d3d->CreateDepthStencilView(m_texture.ptr(), &dsvDesc, &m_dsv);
    }
}

D3D9Surface::~D3D9Surface() {}

HRESULT STDMETHODCALLTYPE D3D9Surface::QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 || riid == IID_IDirect3DSurface9) {
        *ppvObject = static_cast<IDirect3DSurface9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Surface::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_device);
    (*ppDevice)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Surface::GetContainer(REFIID riid, void **ppContainer) {
    if (!ppContainer) return D3DERR_INVALIDCALL;
    if (!m_container) return D3DERR_INVALIDCALL;
    return m_container->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE D3D9Surface::GetDesc(D3DSURFACE_DESC *pDesc) {
    if (!pDesc) return D3DERR_INVALIDCALL;
    *pDesc = m_desc;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Surface::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
    if (!pLockedRect) return D3DERR_INVALIDCALL;

    // System memory surfaces: return direct pointer
    if (!m_sysMemData.empty()) {
        UINT bpp = GetFormatBPP(m_desc.Format);
        pLockedRect->Pitch = m_desc.Width * bpp;
        if (pRect) {
            pLockedRect->pBits = m_sysMemData.data() + pRect->top * pLockedRect->Pitch + pRect->left * bpp;
        } else {
            pLockedRect->pBits = m_sysMemData.data();
        }
        return D3D_OK;
    }

    if (!m_texture) return D3DERR_INVALIDCALL;

    ID3D11DeviceContext *ctx = m_device->GetD3D11Context();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    UINT subresource = D3D11CalcSubresource(m_mipLevel, m_arraySlice, 1);
    D3D11_MAP mapType = (Flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_WRITE_DISCARD;

    HRESULT hr = ctx->Map(m_texture.ptr(), subresource, mapType, 0, &mapped);
    if (FAILED(hr)) {
        // Fallback: use staging texture
        Logger::warn("D3D9Surface::LockRect: Map failed, trying staging copy");
        return D3DERR_INVALIDCALL;
    }

    pLockedRect->Pitch = mapped.RowPitch;
    if (pRect) {
        UINT bpp = GetFormatBPP(m_desc.Format);
        pLockedRect->pBits = (BYTE*)mapped.pData + pRect->top * mapped.RowPitch + pRect->left * bpp;
    } else {
        pLockedRect->pBits = mapped.pData;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Surface::UnlockRect() {
    if (!m_sysMemData.empty())
        return D3D_OK;
    if (!m_texture) return D3DERR_INVALIDCALL;
    ID3D11DeviceContext *ctx = m_device->GetD3D11Context();
    UINT subresource = D3D11CalcSubresource(m_mipLevel, m_arraySlice, 1);
    ctx->Unmap(m_texture.ptr(), subresource);
    return D3D_OK;
}

} // namespace dxmt::d3d9
