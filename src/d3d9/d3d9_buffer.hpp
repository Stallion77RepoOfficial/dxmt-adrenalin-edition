#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d11.h"

namespace dxmt::d3d9 {

class D3D9Device;

class D3D9VertexBuffer final : public ComObjectWithInitialRef<IDirect3DVertexBuffer9> {
public:
    D3D9VertexBuffer(D3D9Device *pDevice, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool);
    ~D3D9VertexBuffer();

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
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }

    HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *pDesc) override;

    ID3D11Buffer* GetD3D11Buffer() const { return m_buffer.ptr(); }
    UINT GetStride() const { return m_stride; }
    DWORD GetFVF() const { return m_fvf; }
    UINT GetSize() const { return m_length; }
    const BYTE* GetData() const { return m_sysMemData.data(); }

    void SetStride(UINT stride) { m_stride = stride; }

private:
    D3D9Device         *m_device;
    Com<ID3D11Buffer>   m_buffer;
    std::vector<BYTE>   m_sysMemData;
    UINT   m_length;
    DWORD  m_usage;
    DWORD  m_fvf;
    D3DPOOL m_pool;
    UINT   m_stride = 0;
    bool   m_locked = false;
    UINT   m_lockOffset = 0;
    UINT   m_lockSize = 0;
    DWORD  m_lockFlags = 0;
    bool   m_dynamic = false;
};

class D3D9IndexBuffer final : public ComObjectWithInitialRef<IDirect3DIndexBuffer9> {
public:
    D3D9IndexBuffer(D3D9Device *pDevice, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~D3D9IndexBuffer();

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
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }

    HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *pDesc) override;

    ID3D11Buffer* GetD3D11Buffer() const { return m_buffer.ptr(); }
    DXGI_FORMAT GetFormat() const;
    UINT GetSize() const { return m_length; }
    D3DFORMAT GetD3D9Format() const { return m_format; }
    const BYTE* GetData() const { return m_sysMemData.data(); }

private:
    D3D9Device         *m_device;
    Com<ID3D11Buffer>   m_buffer;
    std::vector<BYTE>   m_sysMemData;
    UINT      m_length;
    DWORD     m_usage;
    D3DFORMAT m_format;
    D3DPOOL   m_pool;
    bool      m_locked = false;
    bool      m_dynamic = false;
};

} // namespace dxmt::d3d9
