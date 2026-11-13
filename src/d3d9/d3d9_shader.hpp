#pragma once

#include "d3d9_private.h"
#include "d3d9_device_child.hpp"
#include "sm3_translate.hpp"
#include <map>
#include <unordered_map>
#include <vector>

namespace dxmt {

class D3D9Device;

// Shared plumbing for IDirect3DVertexShader9 / IDirect3DPixelShader9: decode
// the D3D9 SM1-3 bytecode via sm3::Decode, translate it to a real SM4 DXBC
// module via sm3::Translate, then hand that straight to the existing D3D11
// shader compiler (ID3D11Device::Create{Vertex,Pixel}Shader) - this reuses
// D3D11's whole airconv/Metal pipeline instead of duplicating it.
template <typename Interface, typename D3D11Interface>
class D3D9Shader : public D3D9DeviceChild<Interface> {
public:
  D3D9Shader(D3D9Device *pDevice, const DWORD *pFunction);

  bool IsValid() const { return d3d11_shader_ != nullptr; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice);
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData, UINT *pSizeOfData) {
    if (!pSizeOfData) return D3DERR_INVALIDCALL;
    UINT size = (UINT)(bytecode_.size() * sizeof(DWORD));
    if (!pData) { *pSizeOfData = size; return D3D_OK; }
    if (*pSizeOfData < size) return D3DERR_INVALIDCALL;
    memcpy(pData, bytecode_.data(), size);
    return D3D_OK;
  }

  D3D11Interface *GetD3D11Shader() const { return d3d11_shader_.ptr(); }
  // Pixel shaders only: variant with a baked-in alpha-test epilogue for the
  // given D3DCMP_* function (D3D11 has no fixed-function alpha test).
  // Variants are compiled on first use and cached; 0/D3DCMP_ALWAYS returns
  // the plain shader.
  // The pixel shader D3D11 needs is a function of two pieces of render state
  // D3D11 has no fixed-function stage for: the alpha test and fog. Both are
  // baked into the shader, so the variant is keyed on the pair.
  D3D11Interface *GetD3D11ShaderVariant(uint32_t alpha_test_func, bool fog, uint32_t fog_table_mode,
                                        bool fog_w, bool point_sprite, uint32_t sampler_dims,
                                        uint16_t shadow_samplers, uint16_t fetch4_samplers,
                                        uint16_t fetch4_alpha);
  // Vertex shaders only: variant that also writes the link registers in
  // default_outputs, which the bound pixel stage reads but this shader never
  // writes, with D3D9's value for them (1.0 for those in default_white).
  // With point_size, it also sizes the points of a point list, with
  // clip_planes it clips against D3D9's user clip planes, and it converts its
  // inputs as input_conversions (sm3::InputConversion) says.
  D3D11Interface *GetD3D11OutputVariant(uint32_t default_outputs, uint32_t default_white, bool point_size,
                                        bool clip_planes, uint32_t input_conversions);
  // sm3::LinkBit()s of the varyings it writes (vertex) or reads (pixel).
  uint32_t LinkOutputs() const { return link_outputs_; }
  uint32_t LinkInputs() const { return link_inputs_; }
  uint32_t MajorVersion() const { return decoded_.major_version; }
  uint32_t FloatConstantsUsed() const { return float_constants_used_; }
  const std::vector<sm3::VertexInputSlot> &VertexInputs() const { return vertex_inputs_; }
  // Translated SM4 DXBC (not the original D3D9 bytecode returned by
  // GetFunction()) - CreateInputLayout needs this to match declaration
  // elements against the shader's actual input signature.
  const std::vector<uint8_t> &TranslatedDXBC() const { return translated_dxbc_; }
  uint64_t UniqueId() const { return unique_id_; }

private:
  HRESULT CreateFromDXBC(const std::vector<uint8_t> &dxbc, Com<D3D11Interface> &out);

  Com<D3D11Interface> d3d11_shader_;
  sm3::ShaderModule decoded_; // kept for compiling alpha-test variants
  std::map<std::pair<uint64_t, uint32_t>, Com<D3D11Interface>> variants_;
  std::vector<DWORD> bytecode_;
  std::vector<uint8_t> translated_dxbc_;
  uint32_t float_constants_used_ = 0;
  std::vector<sm3::VertexInputSlot> vertex_inputs_; // vertex shaders only
  uint32_t link_outputs_ = 0;
  uint32_t link_inputs_ = 0;
  uint64_t unique_id_ = NextD3D9ObjectId();
  D3D11Interface *CompileVariant(std::pair<uint64_t, uint32_t> key, const sm3::TranslateOptions &options);
};

class D3D9VertexShader final : public D3D9Shader<IDirect3DVertexShader9, ID3D11VertexShader> {
public:
  using D3D9Shader::D3D9Shader;
};

class D3D9PixelShader final : public D3D9Shader<IDirect3DPixelShader9, ID3D11PixelShader> {
public:
  using D3D9Shader::D3D9Shader;
};

} // namespace dxmt
