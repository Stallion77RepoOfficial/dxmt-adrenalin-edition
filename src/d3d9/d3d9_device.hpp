#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "d3d11.h"
#include "d3d9_format.hpp"
#include "log/log.hpp"
#include <array>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace dxmt::d3d9 {

class D3D9Main;
class D3D9Surface;
class D3D9VertexBuffer;
class D3D9IndexBuffer;
class D3D9VertexShader;
class D3D9PixelShader;
class D3D9VertexDeclaration;

static constexpr UINT kMaxStreams    = 16;
static constexpr UINT kMaxSamplers   = 20; // 16 pixel + 4 vertex
static constexpr UINT kMaxRTs        = 4;
static constexpr UINT kMaxVSConsts   = 256;
static constexpr UINT kMaxPSConsts   = 224;

struct SamplerState {
    D3DTEXTUREADDRESS addressU   = D3DTADDRESS_WRAP;
    D3DTEXTUREADDRESS addressV   = D3DTADDRESS_WRAP;
    D3DTEXTUREADDRESS addressW   = D3DTADDRESS_WRAP;
    D3DTEXTUREFILTERTYPE minFilter = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE magFilter = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE mipFilter = D3DTEXF_NONE;
    DWORD maxAnisotropy = 1;
    DWORD borderColor   = 0;
    float mipLodBias    = 0.0f;
    DWORD maxMipLevel   = 0;
    bool dirty = true;
};

struct StreamSource {
    Com<D3D9VertexBuffer> vertexBuffer;
    UINT offset = 0;
    UINT stride = 0;
    UINT freq   = 1;
};

struct RenderState {
    DWORD zenable             = TRUE;
    DWORD fillmode            = D3DFILL_SOLID;
    DWORD shademode           = D3DSHADE_GOURAUD;
    DWORD zwriteenable        = TRUE;
    DWORD alphatestenable     = FALSE;
    DWORD srcblend            = D3DBLEND_ONE;
    DWORD destblend           = D3DBLEND_ZERO;
    DWORD cullmode            = D3DCULL_CCW;
    DWORD zfunc               = D3DCMP_LESSEQUAL;
    DWORD alpharef            = 0;
    DWORD alphafunc           = D3DCMP_ALWAYS;
    DWORD alphablendenable    = FALSE;
    DWORD stencilenable       = FALSE;
    DWORD stencilfail         = D3DSTENCILOP_KEEP;
    DWORD stencilzfail        = D3DSTENCILOP_KEEP;
    DWORD stencilpass         = D3DSTENCILOP_KEEP;
    DWORD stencilfunc         = D3DCMP_ALWAYS;
    DWORD stencilref          = 0;
    DWORD stencilmask         = 0xFFFFFFFF;
    DWORD stencilwritemask    = 0xFFFFFFFF;
    DWORD colorwriteenable    = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;
    DWORD colorwriteenable1   = 0xF;
    DWORD colorwriteenable2   = 0xF;
    DWORD colorwriteenable3   = 0xF;
    DWORD blendop             = D3DBLENDOP_ADD;
    DWORD separatealphablendenable = FALSE;
    DWORD srcblendalpha       = D3DBLEND_ONE;
    DWORD destblendalpha      = D3DBLEND_ZERO;
    DWORD blendopalpha        = D3DBLENDOP_ADD;
    DWORD blendfactor         = 0xFFFFFFFF;
    DWORD scissortestenable   = FALSE;
    DWORD srgbwriteenable     = FALSE;
    DWORD depthbias           = 0;
    DWORD slopescaledepthbias = 0;
    DWORD twosidedstencilmode = FALSE;
    DWORD ccw_stencilfail     = D3DSTENCILOP_KEEP;
    DWORD ccw_stencilzfail    = D3DSTENCILOP_KEEP;
    DWORD ccw_stencilpass     = D3DSTENCILOP_KEEP;
    DWORD ccw_stencilfunc     = D3DCMP_ALWAYS;
    DWORD multisampleantialias = TRUE;
    DWORD multisamplemask      = 0xFFFFFFFF;

    bool blendDirty      = true;
    bool dsDirty         = true;
    bool rsDirty         = true;
};

class D3D9SwapChain final : public ComObjectWithInitialRef<IDirect3DSwapChain9> {
public:
    D3D9SwapChain(D3D9Device *pDevice, D3DPRESENT_PARAMETERS *pPP,
                  IDXGISwapChain *pDXGISwapChain);
    ~D3D9SwapChain();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    HRESULT STDMETHODCALLTYPE Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *pDestSurface) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iBackBuffer, UINT Type, IDirect3DSurface9 **ppBackBuffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *pMode) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters) override;

    IDXGISwapChain* GetDXGISwapChain() const { return m_dxgiSwapChain.ptr(); }
    D3D9Surface* GetBackBuffer() const { return m_backBuffer.ptr(); }

private:
    D3D9Device              *m_device;
    Com<IDXGISwapChain>      m_dxgiSwapChain;
    Com<D3D9Surface>         m_backBuffer;
    D3DPRESENT_PARAMETERS    m_pp;
};

class D3D9Device final : public ComObjectWithInitialRef<IDirect3DDevice9> {
public:
    D3D9Device(D3D9Main *pD3D9, ID3D11Device *pD3D11Device, IDXGISwapChain *pSwapChain,
               UINT adapter, D3DDEVTYPE devType, HWND hFocusWindow, DWORD behaviorFlags,
               D3DPRESENT_PARAMETERS *pPP);
    ~D3D9Device();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override  { return ComObjectWithInitialRef::AddRef(); }
    ULONG   STDMETHODCALLTYPE Release() override { return ComObjectWithInitialRef::Release(); }

    // IDirect3DDevice9 methods
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return D3D_OK; }
    UINT    STDMETHODCALLTYPE GetAvailableTextureMem() override { return 0x40000000; }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **ppD3D9) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *pCaps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) override;
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) override;
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) override { return D3D_OK; }
    void    STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override {}
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **pSwapChain) override;
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) override;
    UINT    STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, UINT Type, IDirect3DSurface9 **ppBackBuffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) override;
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override { return D3D_OK; }
    void    STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp) override {}
    void    STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) override {}
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) override;
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter) override;
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR color) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) override;
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE EndScene() override;
    HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) override;
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *pViewport) override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *pMaterial) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9 *pLight) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *pLight) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL *pEnable) override { if (pEnable) *pEnable = FALSE; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, const float *pPlane) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override { if (pNumPasses) *pNumPasses = 1; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *pRect) override;
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override;
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { return D3D_OK; }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { return D3D_OK; }
    float   STDMETHODCALLTYPE GetNPatchMode() override { return 0.0f; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer, IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override;
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override;
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override;
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *pShader) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **ppShader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) override;
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override;
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) override;
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *pIndexData) override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **ppIndexData) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *pShader) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **ppShader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, const float *pNumSegs, const void *pRectPatchInfo) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, const float *pNumSegs, const void *pTriPatchInfo) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override;

    // Internal accessors
    ID3D11Device*        GetD3D11Device() const  { return m_d3d11Device.ptr(); }
    ID3D11DeviceContext* GetD3D11Context() const { return m_d3d11Context.ptr(); }

    void FlushRenderStates();
    void FlushSamplerStates();
    void FlushShaders();
    void FlushInputLayout(D3DPRIMITIVETYPE primType);
    void FlushConstantBuffers();
    void UploadBufferToGPU(class D3D9VertexBuffer *vb);
    void UploadBufferToGPU(class D3D9IndexBuffer *ib);

private:
    void CreateDefaultInputLayout();
    void BuildInputLayoutFromDecl(D3D9VertexDeclaration *decl, const void *vsBytecode, size_t vsSize, ID3D11InputLayout **ppLayout);
    void BuildInputLayoutFromFVF(DWORD fvf, const void *vsBytecode, size_t vsSize, ID3D11InputLayout **ppLayout);
    void UpdateBlendState();
    void UpdateDepthStencilState();
    void UpdateRasterizerState();
    void UpdateSamplerState(UINT sampler);
    void CreateConstantBuffers();
    void BuildFF_VS(std::vector<BYTE> &bytecode);
    void BuildFF_PS(std::vector<BYTE> &bytecode);
    static UINT CalcPrimCount(D3DPRIMITIVETYPE type, UINT vtxCount);

    D3D9Main            *m_d3d9;
    Com<ID3D11Device>    m_d3d11Device;
    Com<ID3D11DeviceContext> m_d3d11Context;

    Com<D3D9SwapChain>   m_swapChain;

    // Render targets and depth stencil
    std::array<Com<D3D9Surface>, kMaxRTs> m_renderTargets;
    Com<D3D9Surface>    m_depthStencil;

    // Streams
    std::array<StreamSource, kMaxStreams> m_streams;
    Com<D3D9IndexBuffer> m_indexBuffer;

    // Shaders
    Com<D3D9VertexShader>       m_vertexShader;
    Com<D3D9PixelShader>        m_pixelShader;
    Com<D3D9VertexDeclaration>  m_vertexDecl;
    DWORD m_fvf = 0;

    // Textures
    std::array<Com<IDirect3DBaseTexture9>, kMaxSamplers> m_textures;

    // Sampler states
    std::array<SamplerState, kMaxSamplers> m_samplerStates;

    // Texture stage states
    struct TextureStageState {
        DWORD colorOp   = D3DTOP_MODULATE;
        DWORD colorArg1 = 2; // D3DTA_TEXTURE
        DWORD colorArg2 = 0; // D3DTA_DIFFUSE
        DWORD alphaOp   = D3DTOP_SELECTARG1;
        DWORD alphaArg1 = 2;
        DWORD alphaArg2 = 0;
    };
    std::array<TextureStageState, 8> m_tssState;

    // Render states
    RenderState m_rs;

    // D3D11 state objects (rebuilt as needed)
    Com<ID3D11BlendState>        m_blendState;
    Com<ID3D11DepthStencilState> m_dsState;
    Com<ID3D11RasterizerState>   m_rastState;
    std::array<Com<ID3D11SamplerState>, kMaxSamplers> m_d3d11Samplers;
    Com<ID3D11InputLayout>       m_inputLayout;

    // Constant buffers for shaders
    Com<ID3D11Buffer>  m_vsCB;   // VS constants (c0-c255)
    Com<ID3D11Buffer>  m_psCB;   // PS constants
    Com<ID3D11Buffer>  m_ffVsCB; // fixed-function VS constants (transforms, etc.)
    Com<ID3D11Buffer>  m_ffPsCB; // fixed-function PS constants

    // Shader constants
    std::array<float, kMaxVSConsts * 4> m_vsConstF = {};
    std::array<int,   kMaxVSConsts * 4> m_vsConstI = {};
    std::array<BOOL,  kMaxVSConsts>     m_vsConstB = {};
    std::array<float, kMaxPSConsts * 4> m_psConstF = {};

    bool m_vsConstDirty = true;
    bool m_psConstDirty = true;

    // Fixed-function fallback shaders
    Com<ID3D11VertexShader> m_ffVertexShader;
    Com<ID3D11PixelShader>  m_ffPixelShader;
    std::vector<BYTE> m_ffVsBytecode;
    bool m_ffShadersBuilt = false;

    // Viewport / scissor
    D3DVIEWPORT9 m_viewport = {0, 0, 0, 0, 0.0f, 1.0f};
    RECT m_scissorRect = {0, 0, 0, 0};

    // Transforms (for fixed-function)
    D3DMATRIX m_world = {};
    D3DMATRIX m_view  = {};
    D3DMATRIX m_proj  = {};

    // Creation info
    UINT       m_adapterOrdinal;
    D3DDEVTYPE m_devType;
    HWND       m_focusWindow;
    DWORD      m_behaviorFlags;
    D3DPRESENT_PARAMETERS m_pp;
};

} // namespace dxmt::d3d9
