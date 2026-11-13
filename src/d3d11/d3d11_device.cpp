#include "com/com_guid.hpp"
#include "d3d11_fence.hpp"
#include "d3d11_input_layout.hpp"
#include "d3d11_interfaces.hpp"
#include "d3d11_multithread.hpp"
#include "d3d11_pipeline.hpp"
#include "d3d11_class_linkage.hpp"
#include "d3d11_inspection.hpp"
#include "d3d11_context.hpp"
#include "d3d11_context_state.hpp"
#include "d3d11_device.hpp"
#include "d3d11_pipeline_cache.hpp"
#include "d3d11_private.h"
#include "d3d11_query.hpp"
#include "d3d11_swapchain.hpp"
#include "d3d11_state_object.hpp"
#include "dxgi_interfaces.h"
#include "../d3d10/d3d10_device.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_device.hpp"
#include "dxmt_format.hpp"
#include "ftl.hpp"
#include "d3d11_resource.hpp"
#include "dxgi_object.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include "d3d11_4.h"
#include "d3d11_on12.hpp"
#include "util_win32_compat.h"
#include <d3d11on12.h>
#include <mutex>

namespace dxmt {

const GUID kRenderdocUUID = {0xa7aa6116,
                             0x9c8d,
                             0x4bba,
                             {0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78}};
const GUID kPixUUID = {0x9f251514,
                       0x9d4d,
                       0x4902,
                       {0x9d, 0x60, 0x18, 0x98, 0x8a, 0xb7, 0xd4, 0xb5}};
const GUID kGpaUUID = {0xccffef16,
                       0x7b69,
                       0x468f,
                       {0xbc, 0xe3, 0xcd, 0x95, 0x33, 0x69, 0xa3, 0x9a}};

/* A resource's usage fixes who may read and write it, and D3D11 refuses a
 * description that contradicts it. Letting one through built resources no
 * backend can hold: a dynamic depth-stencil texture became a linear Metal
 * depth texture, which Metal does not have, and its validation aborts the
 * process outright on arm64. */
static bool
ValidateResourceAccess(bool is_texture, D3D11_USAGE usage, UINT bind_flags, UINT cpu_access,
                       D3D_FEATURE_LEVEL feature_level) {
  constexpr UINT gpu_read_only = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER |
                                 D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_SHADER_RESOURCE;
  switch (usage) {
  case D3D11_USAGE_DEFAULT:
    /* Windows lets these through with CPU access as well. */
    if ((bind_flags == D3D11_BIND_SHADER_RESOURCE && feature_level >= D3D_FEATURE_LEVEL_11_0) ||
        (is_texture && bind_flags == D3D11_BIND_RENDER_TARGET) || bind_flags == D3D11_BIND_UNORDERED_ACCESS)
      return true;
    return !cpu_access;
  case D3D11_USAGE_IMMUTABLE:
    return bind_flags && !(bind_flags & ~gpu_read_only) && !cpu_access;
  case D3D11_USAGE_DYNAMIC:
    return bind_flags && !(bind_flags & ~gpu_read_only) && cpu_access == D3D11_CPU_ACCESS_WRITE;
  case D3D11_USAGE_STAGING:
    return !bind_flags && cpu_access;
  default:
    return false;
  }
}

/* D3D10_SB_TOKENIZED_PROGRAM_TYPE: the stage a DXBC program is written for. */
enum ShaderProgramType : uint32_t {
  kPixelProgram = 0,
  kVertexProgram = 1,
  kGeometryProgram = 2,
  kHullProgram = 3,
  kDomainProgram = 4,
  kComputeProgram = 5,
};

/* What a shader creation call is given has to be a program it can create: a
 * DXBC container whose SHDR or SHEX chunk is written for one of the stages
 * the call makes, in a model the feature level runs -- below 10.0 only the
 * 4.0 programs that carry a level-9 (Aon9) translation. */
static HRESULT
ValidateShaderBytecode(const void *bytecode, SIZE_T length, std::initializer_list<uint32_t> types,
                       D3D_FEATURE_LEVEL feature_level) {
  auto bytes = static_cast<const uint8_t *>(bytecode);
  auto dword = [&](size_t offset) {
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
  };
  if (!bytes || length < 32 || memcmp(bytes, "DXBC", 4))
    return E_INVALIDARG;
  uint64_t chunk_count = dword(28);
  if (32 + 4 * chunk_count > length)
    return E_INVALIDARG;
  const uint8_t *program = nullptr;
  bool level9 = false;
  for (uint64_t i = 0; i < chunk_count; i++) {
    uint64_t offset = dword(32 + 4 * i);
    if (offset + 8 > length || offset + 8 + dword(offset + 4) > length)
      return E_INVALIDARG;
    if (!memcmp(bytes + offset, "SHDR", 4) || !memcmp(bytes + offset, "SHEX", 4)) {
      if (dword(offset + 4) < 8)
        return E_INVALIDARG;
      program = bytes + offset + 8;
    } else if (!memcmp(bytes + offset, "Aon9", 4)) {
      level9 = true;
    }
  }
  if (!program)
    return E_INVALIDARG;
  uint32_t version;
  memcpy(&version, program, sizeof(version));
  uint32_t type = version >> 16, model = ((version >> 4) & 0xf) * 10 + (version & 0xf);
  if (std::find(types.begin(), types.end(), type) == types.end())
    return E_INVALIDARG;
  if (feature_level < D3D_FEATURE_LEVEL_10_0)
    return level9 && model == 40 ? S_OK : E_INVALIDARG;
  if (model > (feature_level >= D3D_FEATURE_LEVEL_11_0 ? 50u : feature_level >= D3D_FEATURE_LEVEL_10_1 ? 41u : 40u))
    return E_INVALIDARG;
  return S_OK;
}

/* A resource whose mips are generated is drawn into and sampled: buffers have
 * no mips, and a texture needs both bindings. */
static bool
ValidateGenerateMips(bool is_texture, UINT bind_flags, UINT misc_flags) {
  if (!(misc_flags & D3D11_RESOURCE_MISC_GENERATE_MIPS))
    return true;
  constexpr UINT needed = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  return is_texture && (bind_flags & needed) == needed;
}

class MTLD3D11DeviceImpl final : public MTLD3D11Device, public IMTLD3D11DeviceExt, public IMTLSwapChainFactory {
friend class MTLD3D11DXGIDevice;
public:
  MTLD3D11DeviceImpl(
      MTLDXGIObject<IMTLDXGIDevice> *container, IMTLDXGIAdapter *pAdapter, D3D_FEATURE_LEVEL FeatureLevel,
      UINT FeatureFlags, Device &device
  ) :
      container_(container),
      feature_level_(FeatureLevel),
      feature_flags_(FeatureFlags),
      features_(container->GetMTLDevice()),
      sampler_states_(this),
      rasterizer_states_(this),
      depthstencil_states_(this),
      device_(device),
      d3dmt_(static_cast<ID3D11Device *>(this), mutex) {
    commandlist_pool_ = InitializeCommandListPool(this);
    pipeline_cache_ = InitializePipelineCache(this);
    context_ = InitializeImmediateContext(this, device_.queue());
    d3d10_ = std::make_unique<MTLD3D10Device>(this, context_.get());
    is_traced_ = !!::GetModuleHandle("dxgitrace.dll");
    format_inspector_.Inspect(GetMTLDevice());
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return container_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() override { return container_->Release(); }

  bool IsTraced() override { return is_traced_; }

  HRESULT STDMETHODCALLTYPE
  CreateBuffer(const D3D11_BUFFER_DESC *pDesc,
               const D3D11_SUBRESOURCE_DATA *pInitialData,
               ID3D11Buffer **ppBuffer) override {
    InitReturnPtr(ppBuffer);

    if (!pDesc)
      return E_INVALIDARG;
    if (pDesc->ByteWidth == 0 && !(pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILE_POOL))
      return E_INVALIDARG; 

    /* A constant buffer is addressed in float4 units, so its size has to be a
     * whole number of them. D3D11 refuses anything else outright. */
    if ((pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) && (pDesc->ByteWidth % 16))
      return E_INVALIDARG;

    /* Initial data means data. A D3D11_SUBRESOURCE_DATA whose pSysMem is null
     * is refused, not read: the upload path copies ByteWidth bytes from it and
     * a null source faulted inside the unix call, which came back as a failed
     * unix call and took the process down. */
    if (pInitialData && !pInitialData->pSysMem)
      return E_INVALIDARG;

    if (!ValidateResourceAccess(false, pDesc->Usage, pDesc->BindFlags, pDesc->CPUAccessFlags, feature_level_) ||
        !ValidateGenerateMips(false, pDesc->BindFlags, pDesc->MiscFlags))
      return E_INVALIDARG;

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
      case D3D11_USAGE_DYNAMIC:
        return dxmt::CreateBuffer(this, pDesc, pInitialData, ppBuffer);
      case D3D11_USAGE_STAGING:
        return CreateStagingBuffer(this, pDesc, pInitialData, ppBuffer);
      default:
        DXMT_UNREACHABLE
      }
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture1D **ppTexture1D) override {

    InitReturnPtr(ppTexture1D);

    if (!pDesc)
      return E_INVALIDARG;

    if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED)
      return E_INVALIDARG; // not supported yet

    /* Initial data means data. A subresource whose pSysMem is null is refused
     * rather than read -- the upload copies from it, and a null source faults
     * inside the unix call, which surfaces as a failed unix call and takes the
     * process down instead of failing the creation. */
    if (pInitialData) {
      unsigned subresources = pDesc->MipLevels ? pDesc->MipLevels * pDesc->ArraySize : 1;
      for (unsigned i = 0; i < subresources; i++)
        if (!pInitialData[i].pSysMem)
          return E_INVALIDARG;
    }

    if (!ValidateResourceAccess(true, pDesc->Usage, pDesc->BindFlags, pDesc->CPUAccessFlags, feature_level_) ||
        !ValidateGenerateMips(true, pDesc->BindFlags, pDesc->MiscFlags))
      return E_INVALIDARG;

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
        return CreateDeviceTexture1D(this, pDesc, pInitialData, ppTexture1D);
      case D3D11_USAGE_DYNAMIC: {
        HRESULT hr = CreateDynamicLinearTexture1D(this, pDesc, pInitialData, ppTexture1D);
        if (SUCCEEDED(hr))
          return hr;
        return CreateDynamicTexture1D(this, pDesc, pInitialData, ppTexture1D);
      }
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture1D(this, pDesc, pInitialData, ppTexture1D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture2D **ppTexture2D) override {
    D3D11_TEXTURE2D_DESC1 desc1;
    UpgradeResourceDescription(pDesc, desc1);
    return CreateTexture2D1(&desc1, pInitialData,
                            (ID3D11Texture2D1 **)ppTexture2D);
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture3D **ppTexture3D) override {
    D3D11_TEXTURE3D_DESC1 desc1;
    UpgradeResourceDescription(pDesc, desc1);
    return CreateTexture3D1(&desc1, pInitialData,
                            (ID3D11Texture3D1 **)ppTexture3D);
  }

  HRESULT STDMETHODCALLTYPE CreateShaderResourceView(
      ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
      ID3D11ShaderResourceView **ppSRView) override {
    if (pDesc) {
      D3D11_SHADER_RESOURCE_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      return CreateShaderResourceView1(pResource, &desc1,
                                       (ID3D11ShaderResourceView1 **)ppSRView);
    }
    return CreateShaderResourceView1(pResource, nullptr,
                                     (ID3D11ShaderResourceView1 **)ppSRView);
  }

  HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(
      ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc,
      ID3D11UnorderedAccessView **ppUAView) override {
    if (pDesc) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      return CreateUnorderedAccessView1(
          pResource, &desc1, (ID3D11UnorderedAccessView1 **)ppUAView);
    }
    return CreateUnorderedAccessView1(pResource, nullptr,
                                      (ID3D11UnorderedAccessView1 **)ppUAView);
  }

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(
      ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
      ID3D11RenderTargetView **ppRTView) override {
    if (pDesc) {
      D3D11_RENDER_TARGET_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      return CreateRenderTargetView1(pResource, &desc1,
                                     (ID3D11RenderTargetView1 **)ppRTView);
    }
    return CreateRenderTargetView1(pResource, nullptr,
                                   (ID3D11RenderTargetView1 **)ppRTView);
  }

  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(
      ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
      ID3D11DepthStencilView **ppDepthStencilView) override {
    InitReturnPtr(ppDepthStencilView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppDepthStencilView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateDepthStencilView(pDesc, ppDepthStencilView);
  }

  HRESULT STDMETHODCALLTYPE CreateInputLayout(
      const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements,
      const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
      ID3D11InputLayout **ppInputLayout) override {
    InitReturnPtr(ppInputLayout);

    if (!pInputElementDescs)
      return E_INVALIDARG;
    if (!pShaderBytecodeWithInputSignature)
      return E_INVALIDARG;

    // TODO: must get shader reflection info

    if (!ppInputLayout) {
      return S_FALSE;
    }

    return pipeline_cache_->AddInputLayout(
        pShaderBytecodeWithInputSignature, pInputElementDescs, NumElements,
        (IMTLD3D11InputLayout **)ppInputLayout);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                     ID3D11ClassLinkage *pClassLinkage,
                     ID3D11VertexShader **ppVertexShader) override {
    InitReturnPtr(ppVertexShader);
    if (HRESULT hr = ValidateShaderBytecode(pShaderBytecode, BytecodeLength, {kVertexProgram}, feature_level_); FAILED(hr))
      return hr;
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddVertexShader(pShaderBytecode, BytecodeLength,
                                            ppVertexShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                       ID3D11ClassLinkage *pClassLinkage,
                       ID3D11GeometryShader **ppGeometryShader) override {
    InitReturnPtr(ppGeometryShader);
    if (HRESULT hr = ValidateShaderBytecode(pShaderBytecode, BytecodeLength, {kGeometryProgram}, feature_level_); FAILED(hr))
      return hr;
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppGeometryShader)
      return S_FALSE;

    return pipeline_cache_->AddGeometryShader(pShaderBytecode, BytecodeLength,
                                              ppGeometryShader);
  }

  HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(
      const void *pShaderBytecode, SIZE_T BytecodeLength,
      const D3D11_SO_DECLARATION_ENTRY *pSODeclaration, UINT NumEntries,
      const UINT *pBufferStrides, UINT NumStrides, UINT RasterizedStream,
      ID3D11ClassLinkage *pClassLinkage,
      ID3D11GeometryShader **ppGeometryShader) override {
    InitReturnPtr(ppGeometryShader);
    if (HRESULT hr = ValidateShaderBytecode(pShaderBytecode, BytecodeLength,
                                            {kVertexProgram, kDomainProgram, kGeometryProgram}, feature_level_);
        FAILED(hr))
      return hr;
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (NumEntries > 0 && RasterizedStream == D3D11_SO_NO_RASTERIZED_STREAM &&
        ((char *)pShaderBytecode)[0] == 'D' &&
        ((char *)pShaderBytecode)[1] == 'X' &&
        ((char *)pShaderBytecode)[2] == 'B' &&
        ((char *)pShaderBytecode)[3] == 'C') {
      // FIXME: ensure the input shader is a vertex shader
      WARN("Emulate stream output");

      Com<IMTLD3D11StreamOutputLayout> so_layout;
      HRESULT hr = pipeline_cache_->AddStreamOutputLayout(
          pShaderBytecode, NumEntries, pSODeclaration, NumStrides,
          pBufferStrides, RasterizedStream, &so_layout);
      if (SUCCEEDED(hr))
        return so_layout->QueryInterface(IID_PPV_ARGS(ppGeometryShader));
    }
    ERR("CreateGeometryShaderWithStreamOutput: not supported, expect problem");
    return pipeline_cache_->AddGeometryShader(pShaderBytecode, BytecodeLength,
                                              ppGeometryShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                    ID3D11ClassLinkage *pClassLinkage,
                    ID3D11PixelShader **ppPixelShader) override {
    InitReturnPtr(ppPixelShader);
    if (HRESULT hr = ValidateShaderBytecode(pShaderBytecode, BytecodeLength, {kPixelProgram}, feature_level_); FAILED(hr))
      return hr;
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddPixelShader(pShaderBytecode, BytecodeLength,
                                           ppPixelShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                   ID3D11ClassLinkage *pClassLinkage,
                   ID3D11HullShader **ppHullShader) override {
    InitReturnPtr(ppHullShader);
    if (HRESULT hr = ValidateShaderBytecode(pShaderBytecode, BytecodeLength, {kHullProgram}, feature_level_); FAILED(hr))
      return hr;
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppHullShader)
      return S_FALSE;

    return pipeline_cache_->AddHullShader(pShaderBytecode, BytecodeLength,
                                          ppHullShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                     ID3D11ClassLinkage *pClassLinkage,
                     ID3D11DomainShader **ppDomainShader) override {
    InitReturnPtr(ppDomainShader);
    if (HRESULT hr = ValidateShaderBytecode(pShaderBytecode, BytecodeLength, {kDomainProgram}, feature_level_); FAILED(hr))
      return hr;
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppDomainShader)
      return S_FALSE;

    return pipeline_cache_->AddDomainShader(pShaderBytecode, BytecodeLength,
                                            ppDomainShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                      ID3D11ClassLinkage *pClassLinkage,
                      ID3D11ComputeShader **ppComputeShader) override {
    InitReturnPtr(ppComputeShader);
    if (HRESULT hr = ValidateShaderBytecode(pShaderBytecode, BytecodeLength, {kComputeProgram}, feature_level_); FAILED(hr))
      return hr;
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddComputeShader(pShaderBytecode, BytecodeLength,
                                             ppComputeShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateClassLinkage(ID3D11ClassLinkage **ppLinkage) override {
    *ppLinkage = ref(new MTLD3D11ClassLinkage(this));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc,
                   ID3D11BlendState **ppBlendState) override {
    InitReturnPtr(ppBlendState);
    if (!pBlendStateDesc)
      return E_INVALIDARG;
    ID3D11BlendState1 *pBlendState1 = nullptr;
    D3D11_BLEND_DESC1 desc;
    desc.AlphaToCoverageEnable = pBlendStateDesc->AlphaToCoverageEnable;
    desc.IndependentBlendEnable = pBlendStateDesc->IndependentBlendEnable;

    for (uint32_t i = 0; i < 8; i++) {
      desc.RenderTarget[i].BlendEnable =
          pBlendStateDesc->RenderTarget[i].BlendEnable;
      desc.RenderTarget[i].LogicOpEnable = FALSE;
      desc.RenderTarget[i].LogicOp = D3D11_LOGIC_OP_NOOP;
      desc.RenderTarget[i].SrcBlend = pBlendStateDesc->RenderTarget[i].SrcBlend;
      desc.RenderTarget[i].DestBlend =
          pBlendStateDesc->RenderTarget[i].DestBlend;
      desc.RenderTarget[i].BlendOp = pBlendStateDesc->RenderTarget[i].BlendOp;
      desc.RenderTarget[i].SrcBlendAlpha =
          pBlendStateDesc->RenderTarget[i].SrcBlendAlpha;
      desc.RenderTarget[i].DestBlendAlpha =
          pBlendStateDesc->RenderTarget[i].DestBlendAlpha;
      desc.RenderTarget[i].BlendOpAlpha =
          pBlendStateDesc->RenderTarget[i].BlendOpAlpha;
      desc.RenderTarget[i].RenderTargetWriteMask =
          pBlendStateDesc->RenderTarget[i].RenderTargetWriteMask;
    }
    /* No output pointer only validates the description. */
    auto hr = CreateBlendState1(&desc, ppBlendState ? &pBlendState1 : nullptr);
    if (ppBlendState)
      *ppBlendState = pBlendState1;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateDepthStencilState(
      const D3D11_DEPTH_STENCIL_DESC *pDesc,
      ID3D11DepthStencilState **ppDepthStencilState) override {
    return depthstencil_states_.CreateStateObject(
        pDesc, (IMTLD3D11DepthStencilState **)ppDepthStencilState);
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc,
                        ID3D11RasterizerState **ppRasterizerState) override {
    InitReturnPtr(ppRasterizerState);
    if (!pRasterizerDesc)
      return E_INVALIDARG;
    ID3D11RasterizerState2 *pRasterizerState = nullptr;
    D3D11_RASTERIZER_DESC2 desc;
    desc.FillMode = pRasterizerDesc->FillMode;
    desc.CullMode = pRasterizerDesc->CullMode;
    desc.FrontCounterClockwise = pRasterizerDesc->FrontCounterClockwise;
    desc.DepthBias = pRasterizerDesc->DepthBias;
    desc.DepthBiasClamp = pRasterizerDesc->DepthBiasClamp;
    desc.SlopeScaledDepthBias = pRasterizerDesc->SlopeScaledDepthBias;
    desc.DepthClipEnable = pRasterizerDesc->DepthClipEnable;
    desc.ScissorEnable = pRasterizerDesc->ScissorEnable;
    desc.MultisampleEnable = pRasterizerDesc->MultisampleEnable;
    desc.AntialiasedLineEnable = pRasterizerDesc->AntialiasedLineEnable;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    auto hr = CreateRasterizerState2(&desc, ppRasterizerState ? &pRasterizerState : nullptr);
    if (ppRasterizerState)
      *ppRasterizerState = pRasterizerState;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc,
                     ID3D11SamplerState **ppSamplerState) override {
    return sampler_states_.CreateStateObject(
        pSamplerDesc, (D3D11SamplerState **)ppSamplerState);
  }

  HRESULT STDMETHODCALLTYPE
  CreateQuery(const D3D11_QUERY_DESC *pQueryDesc, ID3D11Query **ppQuery) override {
    if (!pQueryDesc) {
      InitReturnPtr(ppQuery);
      return E_INVALIDARG;
    }
    D3D11_QUERY_DESC1 desc;
    desc.Query = pQueryDesc->Query;
    desc.MiscFlags = pQueryDesc->MiscFlags;
    desc.ContextType = D3D11_CONTEXT_TYPE_ALL;
    return CreateQuery1(&desc, reinterpret_cast<ID3D11Query1 **>(ppQuery));
  }

  HRESULT STDMETHODCALLTYPE
  CreatePredicate(const D3D11_QUERY_DESC *pPredicateDesc,
                  ID3D11Predicate **ppPredicate) override {
    /* Only the two whole-pipeline yes-or-no queries are made this way; the
     * per-stream overflow predicates come from CreateQuery. */
    if (pPredicateDesc && pPredicateDesc->Query != D3D11_QUERY_OCCLUSION_PREDICATE &&
        pPredicateDesc->Query != D3D11_QUERY_SO_OVERFLOW_PREDICATE) {
      InitReturnPtr(ppPredicate);
      return E_INVALIDARG;
    }
    return CreateQuery(pPredicateDesc,
                       reinterpret_cast<ID3D11Query **>(ppPredicate));
  }

  HRESULT STDMETHODCALLTYPE
  CreateCounter(const D3D11_COUNTER_DESC *pCounterDesc,
                ID3D11Counter **ppCounter) override {
    InitReturnPtr(ppCounter);
    WARN("CreateCounter: not supported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext(
      UINT ContextFlags,
      ID3D11DeviceContext **ppDeferredContext) override{
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext *>(ppDeferredContext3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) override {
    return ImportSharedTexture(this, hResource, ReturnedInterface, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
      CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport) override {
    UINT support = FormatSupport(Format);
    /* What the feature level does not have, no format has: no buffers or
     * comparison sampling below 10.0, no gathers below 10.1. */
    if (feature_level_ < D3D_FEATURE_LEVEL_10_0)
      support &= ~(D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON);
    if (feature_level_ < D3D_FEATURE_LEVEL_10_1)
      support &= ~(D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON);
    if (pFormatSupport)
      *pFormatSupport = support;
    /* A format with nothing to offer is not supported: E_FAIL, as D3D11 has
     * it, for an unknown format as much as for one Metal cannot hold. */
    return support ? S_OK : E_FAIL;
  }

  UINT
  FormatSupport(DXGI_FORMAT Format) {
    if (Format == DXGI_FORMAT_UNKNOWN)
      return D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;

    MTL_DXGI_FORMAT_DESC metal_format;
    if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), Format, metal_format)))
      return 0;

    UINT outFormatSupport = 0;

    if (metal_format.PixelFormat) {
      // All graphics and compute kernels can read or sample a texture with any pixel format.
      outFormatSupport |= D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                          D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD |
                          D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;

      /* UNCHECKED */
      outFormatSupport |= D3D11_FORMAT_SUPPORT_TEXTURE1D | D3D11_FORMAT_SUPPORT_TEXTURE2D |
                          D3D11_FORMAT_SUPPORT_TEXTURE3D | D3D11_FORMAT_SUPPORT_TEXTURECUBE |
                          D3D11_FORMAT_SUPPORT_MIP |
                          D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;

      if (!(metal_format.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))) {
        outFormatSupport |= D3D11_FORMAT_SUPPORT_BUFFER;
        /* Generating mips filters one level into the next: integer formats,
         * which cannot be filtered, have none generated. */
        auto capability = GetMTLPixelFormatCapability(metal_format.PixelFormat);
        if (any_bit_set(capability & FormatCapability::Filter) && any_bit_set(capability & FormatCapability::Color))
          outFormatSupport |= D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
      }

      if (metal_format.Flag & MTL_DXGI_FORMAT_BACKBUFFER) {
        outFormatSupport |= D3D11_FORMAT_SUPPORT_DISPLAY;
      }
    }

    if (metal_format.AttributeFormat) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER;
    }

    if (metal_format.PixelFormat == WMTPixelFormatR32Uint ||
        metal_format.PixelFormat == WMTPixelFormatR16Uint) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER;
    }

    auto Capability = GetMTLPixelFormatCapability(metal_format.PixelFormat);

    if (any_bit_set(Capability & FormatCapability::Color)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    }

    if (any_bit_set(Capability & FormatCapability::Blend)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_BLENDABLE;
    }

    if (any_bit_set(Capability & FormatCapability::DepthStencil)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL |
                          D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON |
                          D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON;
    }

    if (any_bit_set(Capability & FormatCapability::Resolve)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
    }

    if (any_bit_set(Capability & FormatCapability::MSAA)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    }

    if (any_bit_set(Capability & FormatCapability::Write)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    }

    if (Format == DXGI_FORMAT_R32_FLOAT || Format == DXGI_FORMAT_R32_UINT ||
        Format == DXGI_FORMAT_R32_SINT || Format == DXGI_FORMAT_R32G32_FLOAT ||
        Format == DXGI_FORMAT_R32G32_UINT ||
        Format == DXGI_FORMAT_R32G32_SINT ||
        Format == DXGI_FORMAT_R32G32B32_FLOAT ||
        Format == DXGI_FORMAT_R32G32B32_UINT ||
        Format == DXGI_FORMAT_R32G32B32_SINT ||
        Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
        Format == DXGI_FORMAT_R32G32B32A32_UINT ||
        Format == DXGI_FORMAT_R32G32B32A32_SINT)
      outFormatSupport |= D3D11_FORMAT_SUPPORT_SO_BUFFER;

    return outFormatSupport;
  }

  HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(
      DXGI_FORMAT Format, UINT SampleCount, UINT *pNumQualityLevels) override {
    return CheckMultisampleQualityLevels1(Format, SampleCount, 0,
                                          pNumQualityLevels);
  }

  void STDMETHODCALLTYPE
  CheckCounterInfo(D3D11_COUNTER_INFO *pCounterInfo) override {
    if (!pCounterInfo)
      return;
    pCounterInfo->LastDeviceDependentCounter = D3D11_COUNTER{D3D11_COUNTER_DEVICE_DEPENDENT_0};
    pCounterInfo->NumSimultaneousCounters = 0;
    pCounterInfo->NumDetectableParallelUnits = 0;
  }

  HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC *pDesc,
                                         D3D11_COUNTER_TYPE *pType,
                                         UINT *pActiveCounters, LPSTR szName,
                                         UINT *pNameLength, LPSTR szUnits,
                                         UINT *pUnitsLength,
                                         LPSTR szDescription,
                                         UINT *pDescriptionLength) override {
    WARN("CheckCounter: not supported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(D3D11_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    // Format support queries are special in that they use in-out
    // structs
    case D3D11_FEATURE_FORMAT_SUPPORT: {
      auto info =
          static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return E_INVALIDARG;

      return CheckFormatSupport(info->InFormat, &info->OutFormatSupport);
    }
    case D3D11_FEATURE_FORMAT_SUPPORT2: {
      auto info = static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT2 *>(
          pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return E_INVALIDARG;
      info->OutFormatSupport2 = 0;

      if (info->InFormat == DXGI_FORMAT_UNKNOWN) {
        info->OutFormatSupport2 |=
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
            D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE |
            D3D11_FORMAT_SUPPORT2_SHAREABLE;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC metal_format;
      if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), info->InFormat,
                                    metal_format))) {
        return E_INVALIDARG;
      }
      auto Capability = GetMTLPixelFormatCapability(metal_format.PixelFormat);

      if (any_bit_set(Capability & FormatCapability::TextureBufferRead)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
      }

      if (any_bit_set(Capability & FormatCapability::TextureBufferWrite)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
      }

      if (any_bit_set(Capability & FormatCapability::TextureBufferReadWrite)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
                                   D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
      }

      if (any_bit_set(Capability & FormatCapability::Atomic)) {
        info->OutFormatSupport2 |=
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_SHAREABLE;
      }

#ifndef DXMT_NO_PRIVATE_API
      if (any_bit_set(Capability & FormatCapability::Blend)) {
        /* UNCHECKED */
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP;
      }
#endif

      if (any_bit_set(Capability & FormatCapability::Sparse)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_TILED;
      }

      return S_OK;
    }
    default:
      // For everything else, we can use the device feature struct
      // that we already initialized during device creation.
      return features_.GetFeatureData(Feature, FeatureSupportDataSize,
                                       pFeatureSupportData);
    }
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize,
                                           void *pData) override {
    return container_->GetPrivateData(guid, pDataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize,
                                           const void *pData) override {
    return container_->SetPrivateData(guid, DataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override {
    return container_->SetPrivateDataInterface(guid, pData);
  }

  D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override {
    return feature_level_;
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return feature_flags_ & 0x7fffffff; }

  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
    // The physical device can't be removed (unless we deal with an eGPU), but
    // the command queue stops waiting on the GPU once it detects a hang, which
    // is the moral equivalent of a TDR-triggered removal.
    if (device_.queue().IsDeviceWedged())
      return DXGI_ERROR_DEVICE_HUNG;
    return S_OK;
  }

  void STDMETHODCALLTYPE
  GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  UINT STDMETHODCALLTYPE GetExceptionMode() override {
    ERR("Not implemented");
    return 0;
  }

  void STDMETHODCALLTYPE
  GetImmediateContext1(ID3D11DeviceContext1 **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext1(
      UINT ContextFlags,
      ID3D11DeviceContext1 **ppDeferredContext) override{
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext1 *>(ppDeferredContext3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
      CreateBlendState1(const D3D11_BLEND_DESC1 *pBlendStateDesc,
                        ID3D11BlendState1 **ppBlendState) override {
    return pipeline_cache_->AddBlendState(pBlendStateDesc,
                                          (IMTLD3D11BlendState **)ppBlendState);
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState1(const D3D11_RASTERIZER_DESC1 *pRasterizerDesc,
                         ID3D11RasterizerState1 **ppRasterizerState) override {
    InitReturnPtr(ppRasterizerState);
    if (!pRasterizerDesc)
      return E_INVALIDARG;
    ID3D11RasterizerState2 *pRasterizerState = nullptr;
    D3D11_RASTERIZER_DESC2 desc;
    desc.FillMode = pRasterizerDesc->FillMode;
    desc.CullMode = pRasterizerDesc->CullMode;
    desc.FrontCounterClockwise = pRasterizerDesc->FrontCounterClockwise;
    desc.DepthBias = pRasterizerDesc->DepthBias;
    desc.DepthBiasClamp = pRasterizerDesc->DepthBiasClamp;
    desc.SlopeScaledDepthBias = pRasterizerDesc->SlopeScaledDepthBias;
    desc.DepthClipEnable = pRasterizerDesc->DepthClipEnable;
    desc.ScissorEnable = pRasterizerDesc->ScissorEnable;
    desc.MultisampleEnable = pRasterizerDesc->MultisampleEnable;
    desc.AntialiasedLineEnable = pRasterizerDesc->AntialiasedLineEnable;
    desc.ForcedSampleCount = pRasterizerDesc->ForcedSampleCount;
    desc.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    auto hr = CreateRasterizerState2(&desc, ppRasterizerState ? &pRasterizerState : nullptr);
    if (ppRasterizerState)
      *ppRasterizerState = pRasterizerState;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateDeviceContextState(
      UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
      UINT SDKVersion, REFIID EmulatedInterface,
      D3D_FEATURE_LEVEL *pChosenFeatureLevel,
      ID3DDeviceContextState **ppContextState) override {
    InitReturnPtr(ppContextState);
    if (pChosenFeatureLevel)
      *pChosenFeatureLevel = (D3D_FEATURE_LEVEL)0;
    if (!pFeatureLevels || !FeatureLevels)
      return E_INVALIDARG;

    /* The first level asked for that this device reaches. */
    D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0;
    for (UINT i = 0; i < FeatureLevels && !chosen; i++) {
      if (pFeatureLevels[i] <= GetFeatureLevel())
        chosen = pFeatureLevels[i];
    }
    if (!chosen)
      return E_INVALIDARG;
    if (pChosenFeatureLevel)
      *pChosenFeatureLevel = chosen;

    if (ppContextState == nullptr) {
      return S_FALSE;
    }
    *ppContextState = ref(new MTLD3D11DeviceContextState(this));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResource1(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) override {
    return ImportSharedTextureFromNtHandle(this, hResource, ReturnedInterface, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResourceByName(LPCWSTR lpName, DWORD dwDesiredAccess, REFIID ReturnedInterface, void **ppResource)
      override {
    return ImportSharedTextureByName(this, lpName, dwDesiredAccess, ReturnedInterface, ppResource);
  }

  void STDMETHODCALLTYPE
  GetImmediateContext2(ID3D11DeviceContext2 **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE
  CreateDeferredContext2(UINT ContextFlags, ID3D11DeviceContext2 **ppDeferredContext) override {
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext2 *>(ppDeferredContext3);
    return hr;
  }

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D11Resource *resource, UINT *tile_count,
      D3D11_PACKED_MIP_DESC *mip_desc, D3D11_TILE_SHAPE *tile_shape,
      UINT *subresource_tiling_count, UINT first_subresource_tiling,
      D3D11_SUBRESOURCE_TILING *subresource_tiling) override{
    /* No resource is tiled: every resource has no tiles. */
    if (tile_count)
      *tile_count = 0;
    if (mip_desc)
      *mip_desc = {};
    if (tile_shape)
      *tile_shape = {};
    if (subresource_tiling_count)
      *subresource_tiling_count = 0;
  }

  HRESULT STDMETHODCALLTYPE
  CheckMultisampleQualityLevels1(DXGI_FORMAT Format, UINT SampleCount, UINT Flags, UINT *pNumQualityLevels) override {
    if (!pNumQualityLevels)
      return E_INVALIDARG;
    if (Flags) {
      ERR("CheckMultisampleQualityLevels1: unsupported flags ", Flags);
      return E_INVALIDARG;
    }
    /* An unknown format has no levels; one that is not a format at all is
     * refused before anything is written. */
    MTL_DXGI_FORMAT_DESC desc = {};
    if (Format != DXGI_FORMAT_UNKNOWN &&
        (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), Format, desc)) || desc.PixelFormat == WMTPixelFormatInvalid)) {
      return E_INVALIDARG;
    }
    *pNumQualityLevels = 0;
    if (!SampleCount || SampleCount > D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT)
      return E_FAIL;
    if (Format == DXGI_FORMAT_UNKNOWN)
      return S_OK;
    if (SampleCount == 1) {
      *pNumQualityLevels = 1;
      return S_OK;
    }
    /* A format that cannot be a multisampled render target -- block
     * compressed ones, for a start -- has no levels at any count. */
    if (!any_bit_set(GetMTLPixelFormatCapability(desc.PixelFormat) & FormatCapability::MSAA) &&
        !(desc.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_TYPELESS)))
      return S_OK;

    // MSDN:
    // FEATURE_LEVEL_11_0 devices are required to support 4x MSAA for all render
    // target formats, and 8x MSAA for all render target formats except
    // R32G32B32A32 formats.

    // seems some pixel format doesn't support MSAA
    // https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf

    if (GetMTLDevice().supportsTextureSampleCount(SampleCount)) {
      *pNumQualityLevels = 1; // always 1: in metal there is no concept of
                              // Quality Level (so is it in vulkan iirc)
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture2D1(const D3D11_TEXTURE2D_DESC1 *pDesc,
                   const D3D11_SUBRESOURCE_DATA *pInitialData,
                   ID3D11Texture2D1 **ppTexture2D) override {
    InitReturnPtr(ppTexture2D);

    if (!pDesc)
      return E_INVALIDARG;

    if ((pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED))
      return E_INVALIDARG; // not supported yet

    /* Initial data means data. A subresource whose pSysMem is null is refused
     * rather than read -- the upload copies from it, and a null source faults
     * inside the unix call, which surfaces as a failed unix call and takes the
     * process down instead of failing the creation. */
    if (pInitialData) {
      unsigned subresources = pDesc->MipLevels ? pDesc->MipLevels * pDesc->ArraySize : 1;
      for (unsigned i = 0; i < subresources; i++)
        if (!pInitialData[i].pSysMem)
          return E_INVALIDARG;
    }

    if (!ValidateResourceAccess(true, pDesc->Usage, pDesc->BindFlags, pDesc->CPUAccessFlags, feature_level_) ||
        !ValidateGenerateMips(true, pDesc->BindFlags, pDesc->MiscFlags))
      return E_INVALIDARG;

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
        return CreateDeviceTexture2D(this, pDesc, pInitialData, ppTexture2D);
      case D3D11_USAGE_DYNAMIC: {
        HRESULT hr = CreateDynamicLinearTexture2D(this, pDesc, pInitialData, ppTexture2D);
        if (SUCCEEDED(hr))
          return hr;
        return CreateDynamicTexture2D(this, pDesc, pInitialData, ppTexture2D);
      }
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture2D(this, pDesc, pInitialData, ppTexture2D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture3D1(const D3D11_TEXTURE3D_DESC1 *pDesc,
                   const D3D11_SUBRESOURCE_DATA *pInitialData,
                   ID3D11Texture3D1 **ppTexture3D) override {
    InitReturnPtr(ppTexture3D);

    if (!pDesc)
      return E_INVALIDARG;

    if ((pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED))
      return E_INVALIDARG; // not supported yet

    /* Initial data means data. A subresource whose pSysMem is null is refused
     * rather than read -- the upload copies from it, and a null source faults
     * inside the unix call, which surfaces as a failed unix call and takes the
     * process down instead of failing the creation. */
    if (pInitialData) {
      unsigned subresources = pDesc->MipLevels ? pDesc->MipLevels : 1;
      for (unsigned i = 0; i < subresources; i++)
        if (!pInitialData[i].pSysMem)
          return E_INVALIDARG;
    }

    if (!ValidateResourceAccess(true, pDesc->Usage, pDesc->BindFlags, pDesc->CPUAccessFlags, feature_level_) ||
        !ValidateGenerateMips(true, pDesc->BindFlags, pDesc->MiscFlags))
      return E_INVALIDARG;

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
        return CreateDeviceTexture3D(this, pDesc, pInitialData, ppTexture3D);
      case D3D11_USAGE_DYNAMIC:
        return CreateDynamicTexture3D(this, pDesc, pInitialData, ppTexture3D);
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture3D(this, pDesc, pInitialData, ppTexture3D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState2(const D3D11_RASTERIZER_DESC2 *pRasterizerDesc,
                         ID3D11RasterizerState2 **ppRasterizerState) override {
    return rasterizer_states_.CreateStateObject(
        pRasterizerDesc, (IMTLD3D11RasterizerState **)ppRasterizerState);
  }

  HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(
      ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
      ID3D11ShaderResourceView1 **ppSRView) override {
    InitReturnPtr(ppSRView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppSRView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateShaderResourceView(pDesc, ppSRView);
  }

  HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView1(
      ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
      ID3D11UnorderedAccessView1 **ppUAView) override {
    InitReturnPtr(ppUAView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppUAView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateUnorderedAccessView(pDesc, ppUAView);
  }

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView1(
      ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
      ID3D11RenderTargetView1 **ppRTView) override {
    InitReturnPtr(ppRTView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppRTView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateRenderTargetView(pDesc, ppRTView);
  }

  HRESULT STDMETHODCALLTYPE
  CreateQuery1(const D3D11_QUERY_DESC1 *pQueryDesc, ID3D11Query1 **ppQuery) override {
    InitReturnPtr(ppQuery);

    if (!pQueryDesc)
      return E_INVALIDARG;

    if (!ppQuery)
      return S_FALSE;

    switch (pQueryDesc->Query) {
    case D3D11_QUERY_EVENT:
      *ppQuery = ref(new MTLD3D11EventQueryImpl<BOOL>(this, pQueryDesc));
      return S_OK;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
      return CreateOcclusionQuery(this, pQueryDesc, ppQuery);
    case D3D11_QUERY_TIMESTAMP:
      return CreateTimestampQuery(this, pQueryDesc, ppQuery);
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      *ppQuery = ref(new MTLD3D11EventQueryImpl<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(this, pQueryDesc));
      return S_OK;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      *ppQuery = ref(new MTLD3D11DummyQuery<D3D11_QUERY_DATA_PIPELINE_STATISTICS>(this, pQueryDesc));
      return S_OK;
    }
    /* Stream-output counters have no Metal equivalent to read, so these
     * report zero written and nothing overflowed. Refusing to create them
     * instead would be worse: an application that uses stream output at all
     * creates these at startup and treats the failure as no stream output,
     * losing the feature entirely over a statistic it mostly logs. */
    case D3D11_QUERY_SO_STATISTICS:
    case D3D11_QUERY_SO_STATISTICS_STREAM0:
    case D3D11_QUERY_SO_STATISTICS_STREAM1:
    case D3D11_QUERY_SO_STATISTICS_STREAM2:
    case D3D11_QUERY_SO_STATISTICS_STREAM3: {
      *ppQuery = ref(new MTLD3D11DummyQuery<D3D11_QUERY_DATA_SO_STATISTICS>(this, pQueryDesc));
      return S_OK;
    }
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: {
      *ppQuery = ref(new MTLD3D11DummyQuery<BOOL>(this, pQueryDesc));
      return S_OK;
    }
    default:
      ERR("CreateQuery1: query type not implemented: ", pQueryDesc->Query);
      return E_NOTIMPL;
    }
  }

  void STDMETHODCALLTYPE
  GetImmediateContext3(ID3D11DeviceContext3 **ppImmediateContext) override{
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext3(
    UINT ContextFlags, ID3D11DeviceContext3 **ppDeferredContext) override {
    *ppDeferredContext = std::move(dxmt::CreateDeferredContext(this, ContextFlags));
    return S_OK;
  }

  /* The CPU side of a mapped default resource. Nothing reports map-on-default
   * yet, but a caller that uses these anyway gets what a mapped resource
   * would give it: the write lands as UpdateSubresource lands it, and the read
   * comes through a staging copy of the subresource. */
  void STDMETHODCALLTYPE
  WriteToSubresource(
      ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData,
      UINT SrcRowPitch, UINT SrcDepthPitch
  ) override {
    if (!pDstResource || !pSrcData)
      return;
    context_->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
  }

  void STDMETHODCALLTYPE
  ReadFromSubresource(
      void *pDstData, UINT DstRowPitch, UINT DstDepthPitch, ID3D11Resource *SrcResource, UINT SrcSubresource,
      const D3D11_BOX *pSrcBox
  ) override {
    if (!pDstData || !SrcResource)
      return;
    D3D11_RESOURCE_DIMENSION dimension;
    SrcResource->GetType(&dimension);
    Com<ID3D11Resource> staging;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 1, height = 1, depth = 1, mip = 0;
    switch (dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC desc;
      static_cast<ID3D11Buffer *>(SrcResource)->GetDesc(&desc);
      width = desc.ByteWidth;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      desc.StructureByteStride = 0;
      if (FAILED(CreateBuffer(&desc, nullptr, reinterpret_cast<ID3D11Buffer **>(&staging))))
        return;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC desc;
      static_cast<ID3D11Texture1D *>(SrcResource)->GetDesc(&desc);
      mip = SrcSubresource % desc.MipLevels;
      format = desc.Format;
      width = std::max(1u, desc.Width >> mip);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      if (FAILED(CreateTexture1D(&desc, nullptr, reinterpret_cast<ID3D11Texture1D **>(&staging))))
        return;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC desc;
      static_cast<ID3D11Texture2D *>(SrcResource)->GetDesc(&desc);
      mip = SrcSubresource % desc.MipLevels;
      format = desc.Format;
      width = std::max(1u, desc.Width >> mip);
      height = std::max(1u, desc.Height >> mip);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      if (FAILED(CreateTexture2D(&desc, nullptr, reinterpret_cast<ID3D11Texture2D **>(&staging))))
        return;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC desc;
      static_cast<ID3D11Texture3D *>(SrcResource)->GetDesc(&desc);
      mip = SrcSubresource;
      format = desc.Format;
      width = std::max(1u, desc.Width >> mip);
      height = std::max(1u, desc.Height >> mip);
      depth = std::max(1u, desc.Depth >> mip);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      if (FAILED(CreateTexture3D(&desc, nullptr, reinterpret_cast<ID3D11Texture3D **>(&staging))))
        return;
      break;
    }
    default:
      return;
    }
    D3D11_BOX box = pSrcBox ? *pSrcBox : D3D11_BOX{0, 0, 0, width, height, depth};
    if (box.left >= box.right || box.top >= box.bottom || box.front >= box.back)
      return;
    context_->CopySubresourceRegion(staging.ptr(), SrcSubresource, box.left, box.top, box.front, SrcResource,
                                    SrcSubresource, &box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context_->Map(staging.ptr(), SrcSubresource, D3D11_MAP_READ, 0, &mapped)))
      return;
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
      memcpy(pDstData, static_cast<const char *>(mapped.pData) + box.left, box.right - box.left);
    } else {
      MTL_DXGI_FORMAT_DESC metal_format;
      if (SUCCEEDED(MTLQueryDXGIFormat(GetMTLDevice(), format, metal_format)) && metal_format.BytesPerTexel) {
        /* Compressed formats are laid out in rows of 4x4 blocks. */
        bool bc = metal_format.Flag & MTL_DXGI_FORMAT_BC;
        UINT x = bc ? box.left >> 2 : box.left, y = bc ? box.top >> 2 : box.top;
        UINT row_bytes = (bc ? (align(box.right - box.left, 4u) >> 2) : box.right - box.left) * metal_format.BytesPerTexel;
        UINT rows = bc ? align(box.bottom - box.top, 4u) >> 2 : box.bottom - box.top;
        for (UINT z = 0; z < box.back - box.front; z++)
          for (UINT row = 0; row < rows; row++)
            memcpy(static_cast<char *>(pDstData) + z * DstDepthPitch + row * DstRowPitch,
                   static_cast<const char *>(mapped.pData) + (box.front + z) * mapped.DepthPitch +
                       (y + row) * mapped.RowPitch + x * metal_format.BytesPerTexel,
                   row_bytes);
      }
    }
    context_->Unmap(staging.ptr(), SrcSubresource);
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return container_->GetMTLDevice();
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return container_->GetLocalD3DKMT();
  }

  HRESULT
  CreateGraphicsPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledGraphicsPipeline **ppPipeline) override {
    pipeline_cache_->GetGraphicsPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  HRESULT
  CreateComputePipeline(MTL_COMPUTE_PIPELINE_DESC *pDesc,
                        MTLCompiledComputePipeline **ppPipeline) override {
    pipeline_cache_->GetComputePipeline(pDesc, ppPipeline);
    return S_OK;
  };

  virtual HRESULT
  CreateGeometryPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledGeometryPipeline **ppPipeline) override {
    pipeline_cache_->GetGeometryPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  HRESULT
  CreateTessellationMeshPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledTessellationMeshPipeline **ppPipeline) override {
    pipeline_cache_->GetTessellationPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  Device &GetDXMTDevice() override { return device_; };

  void CreateCommandList(ID3D11CommandList** pCommandList) final {
    commandlist_pool_->CreateCommandList(pCommandList);
  };

  virtual void STDMETHODCALLTYPE SetShaderExtensionSlot(UINT Slot) final {
    // TODO
  };

  virtual FormatCapability
  GetMTLPixelFormatCapability(WMTPixelFormat Format) final {
    Format = ORIGINAL_FORMAT(Format);
    if (!format_inspector_.textureCapabilities.contains(Format))
      return FormatCapability(0);
    return format_inspector_.textureCapabilities.at(Format);
  };

  virtual IMTLD3D11DeviceContext *GetImmediateContextPrivate() final {
    return context_.get();
  };

  virtual unsigned int GetDirectXVersion() final {
    return feature_flags_ & 0x80000000 ? 10 : 11;
  };

  virtual HRESULT STDMETHODCALLTYPE RegisterDeviceRemovedEvent(HANDLE Event,
                                                               DWORD *pCookie) final {
    // no device to remove
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE UnregisterDeviceRemoved(DWORD Cookie) final {
    // just do nothing
  };

  virtual HRESULT STDMETHODCALLTYPE OpenSharedFence(HANDLE Handle, REFIID riid,
                                                    void **ppFence) final {
    return dxmt::OpenSharedFence(this, Handle, riid, ppFence);
  };

  virtual HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue,
                                                D3D11_FENCE_FLAG Flags,
                                                REFIID riid, void **ppFence) final {
    if (feature_flags_ & D3D11_CREATE_DEVICE_VIDEO_SUPPORT)
      return E_FAIL;
    return dxmt::CreateFence(this, InitialValue, Flags, riid, ppFence);
  };

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
  ) override {
    return dxmt::CreateSwapChain(pFactory, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  void SetD3D12Device(IUnknown *pDevice) final {
    std::lock_guard lock(d3d12_device_mutex_);
    d3d12_device_ = pDevice;
  }

  bool HasD3D12Device() {
    std::lock_guard lock(d3d12_device_mutex_);
    return d3d12_device_ != nullptr;
  }

  HRESULT QueryD3D12Device(REFIID riid, void **ppDevice) {
    std::lock_guard lock(d3d12_device_mutex_);
    return d3d12_device_ ? d3d12_device_->QueryInterface(riid, ppDevice) : DXGI_ERROR_DEVICE_REMOVED;
  }

private:
  std::mutex d3d12_device_mutex_;
  IUnknown *d3d12_device_ = nullptr;
  MTLDXGIObject<IMTLDXGIDevice> *container_;
  D3D_FEATURE_LEVEL feature_level_;
  UINT feature_flags_;
  MTLD3D11Inspection features_;
  FormatCapabilityInspector format_inspector_;

  bool is_traced_;

  StateObjectCache<D3D11_SAMPLER_DESC, D3D11SamplerState> sampler_states_;
  StateObjectCache<D3D11_RASTERIZER_DESC2, IMTLD3D11RasterizerState> rasterizer_states_;
  StateObjectCache<D3D11_DEPTH_STENCIL_DESC, IMTLD3D11DepthStencilState> depthstencil_states_;

  std::unique_ptr<MTLD3D11CommandListPoolBase> commandlist_pool_;
  std::unique_ptr<MTLD3D11PipelineCacheBase> pipeline_cache_;

  Device& device_;
  /** ensure destructor called first */
  std::unique_ptr<MTLD3D11DeviceContextBase> context_;
  std::unique_ptr<MTLD3D10Device> d3d10_;
  D3D11Multithread d3dmt_;
};

/* ID3D11On12Device of a device that DXMT's D3D12 records through. Both APIs
 * encode into this device's one command stream in call order, so a wrapped
 * resource is the d3d11 resource behind the D3D12 one, and acquiring or
 * releasing it orders nothing that is not ordered already. */
class MTLD3D11On12Device final : public ID3D11On12Device1 {
public:
  MTLD3D11On12Device(MTLDXGIObject<IMTLDXGIDevice> *container, MTLD3D11DeviceImpl *device)
      : container_(container), device_(device) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() final { return container_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() final { return container_->Release(); }

  HRESULT STDMETHODCALLTYPE
  CreateWrappedResource(IUnknown *pResource12, const D3D11_RESOURCE_FLAGS *pFlags11, D3D12_RESOURCE_STATES InState,
                        D3D12_RESOURCE_STATES OutState, REFIID riid, void **ppResource11) final {
    InitReturnPtr(ppResource11);
    if (!pResource12 || !ppResource11)
      return E_INVALIDARG;
    Com<IMTLD3D12ResourceInterop> interop;
    if (FAILED(pResource12->QueryInterface(IID_PPV_ARGS(&interop))))
      return E_INVALIDARG;
    Com<ID3D11Resource> resource;
    HRESULT hr = interop->GetD3D11Resource(&resource);
    if (FAILED(hr)) {
      ERR("D3D11On12: this D3D12 resource has no d3d11 resource behind it");
      return hr;
    }
    return resource->QueryInterface(riid, ppResource11);
  }

  void STDMETHODCALLTYPE ReleaseWrappedResources(ID3D11Resource *const *ppResources, UINT NumResources) final {}

  void STDMETHODCALLTYPE AcquireWrappedResources(ID3D11Resource *const *ppResources, UINT NumResources) final {}

  HRESULT STDMETHODCALLTYPE GetD3D12Device(REFIID riid, ID3D12Device **ppDevice) final {
    InitReturnPtr(ppDevice);
    if (!ppDevice)
      return E_INVALIDARG;
    return device_->QueryD3D12Device(riid, reinterpret_cast<void **>(ppDevice));
  }

private:
  MTLDXGIObject<IMTLDXGIDevice> *container_;
  MTLD3D11DeviceImpl *device_;
};

/**
 * \brief D3D11 device container
 *
 * Stores all the objects that contribute to the D3D11
 * device implementation, including the DXGI device.
 */
class MTLD3D11DXGIDevice final : public MTLDXGIObject<IMTLDXGIDevice> {
public:
  friend class MTLDXGIMetalLayerFactory;

  MTLD3D11DXGIDevice(std::unique_ptr<Device> &&device, IMTLDXGIAdapter *adapter,
                     D3D_FEATURE_LEVEL feature_level, UINT feature_flags)
      : adapter_(adapter), device(std::move(device)),
        cmd_queue_(this->device->queue()),
        d3d11_device_(this, adapter, feature_level, feature_flags,
                      *this->device.get()),
        d3d11on12_(this, &d3d11_device_) {
    if (adapter_->GetLocalD3DKMT()) {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = adapter_->GetLocalD3DKMT();
      if (D3DKMTCreateDevice(&create))
        WARN("Failed to create D3DKMT device");
      else
        local_kmt_ = create.hDevice;
    }
  }

  ~MTLD3D11DXGIDevice() {
    if (local_kmt_) {
      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = local_kmt_;
      D3DKMTDestroyDevice(&destroy);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) ||
        riid == __uuidof(IDXGIDevice2) || riid == __uuidof(IDXGIDevice3) ||
        riid == __uuidof(IMTLDXGIDevice)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) ||
        riid == __uuidof(ID3D11Device2) || riid == __uuidof(ID3D11Device3) ||
        riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5)) {
      *ppvObject = ref_and_cast<ID3D11Device>(&d3d11_device_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1)) {
      *ppvObject = ref_and_cast<ID3D10Device>(d3d11_device_.d3d10_.get());
      return S_OK;
    }

    if (riid == __uuidof(IMTLSwapChainFactory)) {
      *ppvObject = ref_and_cast<IMTLSwapChainFactory>(&d3d11_device_);
      return S_OK;
    }

    if (riid == __uuidof(IMTLD3D11DeviceExt)) {
      *ppvObject = ref_and_cast<IMTLD3D11DeviceExt>(&d3d11_device_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Multithread)) {
      *ppvObject = ref_and_cast<ID3D11Multithread>(&d3d11_device_.d3dmt_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11On12Device) || riid == __uuidof(ID3D11On12Device1)) {
      if (!d3d11_device_.HasD3D12Device())
        return E_NOINTERFACE;
      *ppvObject = ref_and_cast<ID3D11On12Device1>(&d3d11on12_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Debug))
      return E_NOINTERFACE;

    if (riid == kRenderdocUUID || riid == kPixUUID || riid == kGpaUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(IMTLDXGIDevice), riid)) {
      WARN("D3D11Device: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return adapter_->QueryInterface(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    if (pAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    *pAdapter = adapter_.ref();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count,
                DXGI_USAGE usage, const DXGI_SHARED_RESOURCE *shared_resource,
                IDXGISurface **surface) override{
    /* A DXGI surface of this device is a single-level 2D texture, bound as
     * the usage asks. */
    if (!desc || !surface_count || !surface)
      return E_INVALIDARG;
    if (shared_resource) {
      ERR_ONCE("CreateSurface: shared surfaces are created unshared");
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = desc->Width;
    td.Height = desc->Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = desc->Format;
    td.SampleDesc = desc->SampleDesc;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (usage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
      td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (usage & DXGI_USAGE_SHADER_INPUT)
      td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (usage & DXGI_USAGE_UNORDERED_ACCESS)
      td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    for (UINT i = 0; i < surface_count; i++) {
      Com<ID3D11Texture2D> texture;
      HRESULT hr = d3d11_device_.CreateTexture2D(&td, nullptr, &texture);
      if (SUCCEEDED(hr))
        hr = texture->QueryInterface(IID_PPV_ARGS(&surface[i]));
      if (FAILED(hr)) {
        while (i)
          surface[--i]->Release();
        return hr;
      }
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  QueryResourceResidency(IUnknown *const *ppResources, DXGI_RESIDENCY *pResidency, UINT ResourceCount) override {
    if (!ppResources || !pResidency)
      return E_INVALIDARG;

    // Basically in DXMT D3D11 implementation, all resources are assumed to be resident.
    for (uint32_t i = 0; i < ResourceCount; i++)
      pResidency[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override {
    if (Priority < -7 || Priority > 7)
      return E_INVALIDARG;

    WARN("SetGPUThreadPriority: Ignoring");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *pPriority) override {
    *pPriority = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
    cmd_queue_.SetMaxLatency(MaxLatency);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override {
    if (pMaxLatency) {
      *pMaxLatency = cmd_queue_.GetMaxLatency();
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
      OfferResources(UINT NumResources, IDXGIResource *const *ppResources,
                     DXGI_OFFER_RESOURCE_PRIORITY Priority) override {
    // stub
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT NumResources,
                                             IDXGIResource *const *ppResources,
                                             WINBOOL *pDiscarded) override {
    // stub
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE hEvent) override {
    return E_FAIL;
  }

  void STDMETHODCALLTYPE Trim() override { WARN("DXGIDevice3::Trim: no-op"); };

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return adapter_->GetMTLDevice();
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGISwapChain1 **ppSwapChain) override {

    return dxmt::CreateSwapChain(pFactory, &d3d11_device_, hWnd, pDesc, pFullscreenDesc,
                                 ppSwapChain);
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final { return local_kmt_; }

private:
  Com<IMTLDXGIAdapter> adapter_;
  D3DKMT_HANDLE local_kmt_ = 0;
  std::unique_ptr<Device> device;
  CommandQueue &cmd_queue_;
  MTLD3D11DeviceImpl d3d11_device_;
  MTLD3D11On12Device d3d11on12_;
};

Com<IMTLDXGIDevice> CreateD3D11Device(std::unique_ptr<Device> &&device,
                                      IMTLDXGIAdapter *adapter,
                                      D3D_FEATURE_LEVEL feature_level,
                                      UINT feature_flags) {
  return Com<IMTLDXGIDevice>::transfer(new MTLD3D11DXGIDevice(
      std::move(device), adapter, feature_level, feature_flags));
};
} // namespace dxmt