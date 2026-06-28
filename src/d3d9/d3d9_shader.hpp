#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d11.h"
#include <vector>

namespace dxmt::d3d9 {

class D3D9Device;

class D3D9VertexShader final : public ComObjectWithInitialRef<IDirect3DVertexShader9> {
public:
    D3D9VertexShader(D3D9Device *pDevice, const DWORD *pFunction, UINT size);
    ~D3D9VertexShader();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void *pData, UINT *pSizeOfData) override;

    ID3D11VertexShader* GetD3D11Shader() const { return m_shader.ptr(); }
    const std::vector<BYTE>& GetBytecode() const { return m_bytecode; }
    const std::vector<DWORD>& GetD3D9Function() const { return m_function; }

private:
    D3D9Device             *m_device;
    Com<ID3D11VertexShader> m_shader;
    std::vector<BYTE>       m_bytecode;   // compiled SM4 bytecode
    std::vector<DWORD>      m_function;   // original D3D9 bytecode
};

class D3D9PixelShader final : public ComObjectWithInitialRef<IDirect3DPixelShader9> {
public:
    D3D9PixelShader(D3D9Device *pDevice, const DWORD *pFunction, UINT size);
    ~D3D9PixelShader();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void *pData, UINT *pSizeOfData) override;

    ID3D11PixelShader* GetD3D11Shader() const { return m_shader.ptr(); }
    const std::vector<BYTE>& GetBytecode() const { return m_bytecode; }

private:
    D3D9Device            *m_device;
    Com<ID3D11PixelShader> m_shader;
    std::vector<BYTE>      m_bytecode;
    std::vector<DWORD>     m_function;
};

class D3D9VertexDeclaration final : public ComObjectWithInitialRef<IDirect3DVertexDeclaration9> {
public:
    D3D9VertexDeclaration(D3D9Device *pDevice, const D3DVERTEXELEMENT9 *pElements);
    ~D3D9VertexDeclaration();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) override;

    const std::vector<D3DVERTEXELEMENT9>& GetElements() const { return m_elements; }
    DWORD GetFVF() const { return m_fvf; }

private:
    D3D9Device *m_device;
    std::vector<D3DVERTEXELEMENT9> m_elements;
    DWORD m_fvf = 0;
};

class D3D9StateBlock final : public ComObjectWithInitialRef<IDirect3DStateBlock9> {
public:
    D3D9StateBlock(D3D9Device *pDevice, D3DSTATEBLOCKTYPE type);
    ~D3D9StateBlock();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT STDMETHODCALLTYPE Capture() override;
    HRESULT STDMETHODCALLTYPE Apply() override;

private:
    D3D9Device *m_device;
    D3DSTATEBLOCKTYPE m_type;
};

class D3D9Query final : public ComObjectWithInitialRef<IDirect3DQuery9> {
public:
    D3D9Query(D3D9Device *pDevice, D3DQUERYTYPE type);
    ~D3D9Query();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return m_type; }
    DWORD STDMETHODCALLTYPE GetDataSize() override;
    HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) override;
    HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) override;

private:
    D3D9Device   *m_device;
    D3DQUERYTYPE  m_type;
    Com<ID3D11Query> m_d3d11Query;
};

} // namespace dxmt::d3d9
