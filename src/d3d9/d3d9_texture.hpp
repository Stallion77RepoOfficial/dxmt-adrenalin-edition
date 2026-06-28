#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d11.h"
#include "d3d9_surface.hpp"
#include <vector>
#include <memory>

namespace dxmt::d3d9 {

class D3D9Device;

class D3D9Texture2D final : public ComObjectWithInitialRef<IDirect3DTexture9> {
public:
    D3D9Texture2D(D3D9Device *pDevice, UINT width, UINT height, UINT levels,
                  DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~D3D9Texture2D();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    // IDirect3DResource9
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return D3DERR_NOTFOUND; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return D3DERR_NOTFOUND; }
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { return 0; }
    DWORD   STDMETHODCALLTYPE GetPriority() override { return 0; }
    void    STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }

    // IDirect3DBaseTexture9
    DWORD   STDMETHODCALLTYPE SetLOD(DWORD LODNew) override { return 0; }
    DWORD   STDMETHODCALLTYPE GetLOD() override { return 0; }
    DWORD   STDMETHODCALLTYPE GetLevelCount() override { return (DWORD)m_surfaces.size(); }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
    void    STDMETHODCALLTYPE GenerateMipSubLevels() override {}

    // IDirect3DTexture9
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *pDirtyRect) override { return S_OK; }

    ID3D11Texture2D* GetD3D11Texture() const { return m_d3d11Texture.ptr(); }
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.ptr(); }

private:
    D3D9Device *m_device;
    Com<ID3D11Texture2D>         m_d3d11Texture;
    Com<ID3D11ShaderResourceView> m_srv;
    std::vector<Com<D3D9Surface>> m_surfaces;
    DWORD  m_usage;
    D3DFORMAT m_format;
    D3DPOOL   m_pool;
};

class D3D9CubeTexture final : public ComObjectWithInitialRef<IDirect3DCubeTexture9> {
public:
    D3D9CubeTexture(D3D9Device *pDevice, UINT edgeLen, UINT levels,
                    DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~D3D9CubeTexture();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override { return D3DERR_NOTFOUND; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override { return D3DERR_NOTFOUND; }
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override { return 0; }
    DWORD   STDMETHODCALLTYPE GetPriority() override { return 0; }
    void    STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_CUBETEXTURE; }

    DWORD   STDMETHODCALLTYPE SetLOD(DWORD LODNew) override { return 0; }
    DWORD   STDMETHODCALLTYPE GetLOD() override { return 0; }
    DWORD   STDMETHODCALLTYPE GetLevelCount() override { return m_levels; }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override { return S_OK; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_LINEAR; }
    void    STDMETHODCALLTYPE GenerateMipSubLevels() override {}

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(UINT FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT FaceType, UINT Level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(UINT FaceType, const RECT *pDirtyRect) override { return S_OK; }

    ID3D11Texture2D* GetD3D11Texture() const { return m_d3d11Texture.ptr(); }
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.ptr(); }

private:
    D3D9Device *m_device;
    Com<ID3D11Texture2D>         m_d3d11Texture;
    Com<ID3D11ShaderResourceView> m_srv;
    std::vector<Com<D3D9Surface>> m_surfaces; // 6 faces * levels
    UINT      m_levels;
    UINT      m_edgeLen;
    D3DFORMAT m_format;
    D3DPOOL   m_pool;
};

} // namespace dxmt::d3d9
