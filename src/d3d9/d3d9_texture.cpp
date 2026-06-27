#include "d3d9_texture.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include <algorithm>

namespace dxmt::d3d9 {

// --- D3D9Texture2D ---

D3D9Texture2D::D3D9Texture2D(D3D9Device *pDevice, UINT width, UINT height, UINT levels,
                               DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : m_device(pDevice), m_usage(usage), m_format(format), m_pool(pool) {
    if (!levels) {
        UINT w = width, h = height;
        levels = 1;
        while (w > 1 || h > 1) { w >>= 1; h >>= 1; levels++; }
    }

    bool useDepth = IsDepthFormat(format);
    DXGI_FORMAT dxgiFmt = useDepth ? ConvertDepthFormat(format) : ConvertFormat(format);
    if (dxgiFmt == DXGI_FORMAT_UNKNOWN)
        dxgiFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) {
        for (UINT l = 0; l < levels; l++) {
            UINT w = std::max(1u, width  >> l);
            UINT h = std::max(1u, height >> l);
            auto surf = new D3D9Surface(pDevice, w, h, format, usage, pool, false);
            surf->SetContainer(static_cast<IDirect3DTexture9*>(this));
            m_surfaces.emplace_back(surf);
        }
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width              = width;
    desc.Height             = height;
    desc.MipLevels          = levels;
    desc.ArraySize          = 1;
    desc.Format             = dxgiFmt;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;

    bool isDynamic = (usage & D3DUSAGE_DYNAMIC) != 0;
    if (isDynamic) {
        desc.Usage          = D3D11_USAGE_DYNAMIC;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MipLevels      = 1; // dynamic textures only support 1 mip
        levels              = 1;
    } else {
        desc.Usage          = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
    }

    if (usage & D3DUSAGE_RENDERTARGET) {
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    } else if (useDepth) {
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    } else {
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (!isDynamic) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }

    ID3D11Device *d3d = pDevice->GetD3D11Device();
    HRESULT hr = d3d->CreateTexture2D(&desc, nullptr, &m_d3d11Texture);
    if (FAILED(hr)) {
        Logger::err("D3D9Texture2D: Failed to create D3D11 texture");
        return;
    }

    if (!useDepth && !(usage & D3DUSAGE_DEPTHSTENCIL)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = dxgiFmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = (UINT)-1;
        d3d->CreateShaderResourceView(m_d3d11Texture.ptr(), &srvDesc, &m_srv);
    }

    for (UINT l = 0; l < levels; l++) {
        auto surf = new D3D9Surface(pDevice, m_d3d11Texture.ptr(), l, 0, format, usage);
        surf->SetContainer(static_cast<IDirect3DTexture9*>(this));
        m_surfaces.emplace_back(surf);
    }
}

D3D9Texture2D::~D3D9Texture2D() {}

HRESULT STDMETHODCALLTYPE D3D9Texture2D::QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 ||
        riid == IID_IDirect3DBaseTexture9 || riid == IID_IDirect3DTexture9) {
        *ppvObject = static_cast<IDirect3DTexture9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9Texture2D::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_device);
    (*ppDevice)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Texture2D::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    if (!pDesc || Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
    return m_surfaces[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE D3D9Texture2D::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
    if (!ppSurfaceLevel) return D3DERR_INVALIDCALL;
    if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
    *ppSurfaceLevel = m_surfaces[Level].ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9Texture2D::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
    if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
    return m_surfaces[Level]->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE D3D9Texture2D::UnlockRect(UINT Level) {
    if (Level >= m_surfaces.size()) return D3DERR_INVALIDCALL;
    return m_surfaces[Level]->UnlockRect();
}

// --- D3D9CubeTexture ---

D3D9CubeTexture::D3D9CubeTexture(D3D9Device *pDevice, UINT edgeLen, UINT levels,
                                   DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : m_device(pDevice), m_edgeLen(edgeLen), m_format(format), m_pool(pool) {
    if (!levels) {
        UINT w = edgeLen;
        levels = 1;
        while (w > 1) { w >>= 1; levels++; }
    }
    m_levels = levels;

    DXGI_FORMAT dxgiFmt = ConvertFormat(format);
    if (dxgiFmt == DXGI_FORMAT_UNKNOWN)
        dxgiFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) {
        for (UINT f = 0; f < 6; f++) {
            for (UINT l = 0; l < levels; l++) {
                UINT w = std::max(1u, edgeLen >> l);
                auto surf = new D3D9Surface(pDevice, w, w, format, usage, pool, false);
                surf->SetContainer(static_cast<IDirect3DCubeTexture9*>(this));
                m_surfaces.emplace_back(surf);
            }
        }
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width              = edgeLen;
    desc.Height             = edgeLen;
    desc.MipLevels          = levels;
    desc.ArraySize          = 6;
    desc.Format             = dxgiFmt;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = D3D11_RESOURCE_MISC_TEXTURECUBE;

    if (usage & D3DUSAGE_RENDERTARGET)
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    ID3D11Device *d3d = pDevice->GetD3D11Device();
    HRESULT hr = d3d->CreateTexture2D(&desc, nullptr, &m_d3d11Texture);
    if (FAILED(hr)) {
        Logger::err("D3D9CubeTexture: Failed to create D3D11 cube texture");
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = dxgiFmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = (UINT)-1;
    d3d->CreateShaderResourceView(m_d3d11Texture.ptr(), &srvDesc, &m_srv);

    for (UINT f = 0; f < 6; f++) {
        for (UINT l = 0; l < levels; l++) {
            auto surf = new D3D9Surface(pDevice, m_d3d11Texture.ptr(), l, f, format, usage);
            surf->SetContainer(static_cast<IDirect3DCubeTexture9*>(this));
            m_surfaces.emplace_back(surf);
        }
    }
}

D3D9CubeTexture::~D3D9CubeTexture() {}

HRESULT STDMETHODCALLTYPE D3D9CubeTexture::QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 ||
        riid == IID_IDirect3DBaseTexture9 || riid == IID_IDirect3DCubeTexture9) {
        *ppvObject = static_cast<IDirect3DCubeTexture9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9CubeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_device);
    (*ppDevice)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9CubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    if (!pDesc || Level >= m_levels) return D3DERR_INVALIDCALL;
    return m_surfaces[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE D3D9CubeTexture::GetCubeMapSurface(UINT FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) {
    if (!ppCubeMapSurface || FaceType >= 6 || Level >= m_levels) return D3DERR_INVALIDCALL;
    *ppCubeMapSurface = m_surfaces[FaceType * m_levels + Level].ref();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9CubeTexture::LockRect(UINT FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
    if (FaceType >= 6 || Level >= m_levels) return D3DERR_INVALIDCALL;
    return m_surfaces[FaceType * m_levels + Level]->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE D3D9CubeTexture::UnlockRect(UINT FaceType, UINT Level) {
    if (FaceType >= 6 || Level >= m_levels) return D3DERR_INVALIDCALL;
    return m_surfaces[FaceType * m_levels + Level]->UnlockRect();
}

} // namespace dxmt::d3d9
