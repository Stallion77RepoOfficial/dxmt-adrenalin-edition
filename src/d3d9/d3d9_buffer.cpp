#include "d3d9_buffer.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include <cstring>

namespace dxmt::d3d9 {

// --- D3D9VertexBuffer ---

D3D9VertexBuffer::D3D9VertexBuffer(D3D9Device *pDevice, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
    : m_device(pDevice), m_length(length), m_usage(usage), m_fvf(fvf), m_pool(pool) {
    m_dynamic = (usage & D3DUSAGE_DYNAMIC) != 0;
    m_sysMemData.resize(length, 0);

    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH)
        return;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = length;
    desc.StructureByteStride = 0;
    if (m_dynamic) {
        desc.Usage          = D3D11_USAGE_DYNAMIC;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    } else {
        desc.Usage          = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
    }
    desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    desc.MiscFlags      = 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = m_sysMemData.data();

    ID3D11Device *d3d = pDevice->GetD3D11Device();
    HRESULT hr = d3d->CreateBuffer(&desc, &initData, &m_buffer);
    if (FAILED(hr))
        Logger::err("D3D9VertexBuffer: Failed to create D3D11 buffer");
}

D3D9VertexBuffer::~D3D9VertexBuffer() {}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 || riid == IID_IDirect3DVertexBuffer9) {
        *ppvObject = static_cast<IDirect3DVertexBuffer9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_device);
    (*ppDevice)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
    if (!ppbData) return D3DERR_INVALIDCALL;
    m_locked     = true;
    m_lockOffset = OffsetToLock;
    m_lockSize   = SizeToLock ? SizeToLock : m_length - OffsetToLock;
    m_lockFlags  = Flags;
    *ppbData = m_sysMemData.data() + OffsetToLock;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::Unlock() {
    if (!m_locked) return D3D_OK;
    m_locked = false;
    m_device->UploadBufferToGPU(this);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
    if (!pDesc) return D3DERR_INVALIDCALL;
    pDesc->Format = D3DFMT_VERTEXDATA;
    pDesc->Type   = D3DRTYPE_VERTEXBUFFER;
    pDesc->Usage  = m_usage;
    pDesc->Pool   = m_pool;
    pDesc->Size   = m_length;
    pDesc->FVF    = m_fvf;
    return D3D_OK;
}

// --- D3D9IndexBuffer ---

D3D9IndexBuffer::D3D9IndexBuffer(D3D9Device *pDevice, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : m_device(pDevice), m_length(length), m_usage(usage), m_format(format), m_pool(pool) {
    m_dynamic = (usage & D3DUSAGE_DYNAMIC) != 0;
    m_sysMemData.resize(length, 0);

    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH)
        return;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth      = length;
    if (m_dynamic) {
        desc.Usage          = D3D11_USAGE_DYNAMIC;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    } else {
        desc.Usage          = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
    }
    desc.BindFlags      = D3D11_BIND_INDEX_BUFFER;
    desc.MiscFlags      = 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = m_sysMemData.data();

    ID3D11Device *d3d = pDevice->GetD3D11Device();
    HRESULT hr = d3d->CreateBuffer(&desc, &initData, &m_buffer);
    if (FAILED(hr))
        Logger::err("D3D9IndexBuffer: Failed to create D3D11 buffer");
}

D3D9IndexBuffer::~D3D9IndexBuffer() {}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::QueryInterface(REFIID riid, void **ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 || riid == IID_IDirect3DIndexBuffer9) {
        *ppvObject = static_cast<IDirect3DIndexBuffer9*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_device);
    (*ppDevice)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
    if (!ppbData) return D3DERR_INVALIDCALL;
    m_locked = true;
    *ppbData = m_sysMemData.data() + OffsetToLock;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::Unlock() {
    if (!m_locked) return D3D_OK;
    m_locked = false;
    m_device->UploadBufferToGPU(this);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D3D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
    if (!pDesc) return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type   = D3DRTYPE_INDEXBUFFER;
    pDesc->Usage  = m_usage;
    pDesc->Pool   = m_pool;
    pDesc->Size   = m_length;
    return D3D_OK;
}

DXGI_FORMAT D3D9IndexBuffer::GetFormat() const {
    return (m_format == D3DFMT_INDEX16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
}

} // namespace dxmt::d3d9
