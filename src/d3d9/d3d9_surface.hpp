#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d11.h"
#include <vector>

namespace dxmt::d3d9 {

class D3D9Device;

class D3D9Surface final : public ComObjectWithInitialRef<IDirect3DSurface9> {
public:
    D3D9Surface(D3D9Device *pDevice, ID3D11Texture2D *pTexture, UINT mipLevel, UINT arraySlice,
                D3DFORMAT fmt, DWORD usage);
    D3D9Surface(D3D9Device *pDevice, UINT width, UINT height, D3DFORMAT fmt, DWORD usage,
                D3DPOOL pool, bool lockable);
    ~D3D9Surface();

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
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }

    // IDirect3DSurface9
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect() override;
    HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override { return E_NOTIMPL; }

    ID3D11Texture2D* GetD3D11Texture() const { return m_texture.ptr(); }
    ID3D11RenderTargetView* GetRTV() const { return m_rtv.ptr(); }
    ID3D11DepthStencilView* GetDSV() const { return m_dsv.ptr(); }
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.ptr(); }
    UINT GetMipLevel() const { return m_mipLevel; }
    UINT GetArraySlice() const { return m_arraySlice; }
    D3DFORMAT GetFormat() const { return m_desc.Format; }

    void SetContainer(IUnknown *pContainer) { m_container = pContainer; }

private:
    D3D9Device        *m_device;
    Com<ID3D11Texture2D>        m_texture;
    Com<ID3D11RenderTargetView> m_rtv;
    Com<ID3D11DepthStencilView> m_dsv;
    Com<ID3D11ShaderResourceView> m_srv;
    IUnknown          *m_container = nullptr;
    D3DSURFACE_DESC    m_desc;
    UINT               m_mipLevel;
    UINT               m_arraySlice;
    std::vector<BYTE>  m_sysMemData;
    bool               m_ownTexture = false;
};

} // namespace dxmt::d3d9
