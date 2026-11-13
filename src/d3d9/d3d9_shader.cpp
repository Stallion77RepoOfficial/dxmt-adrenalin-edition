#include "d3d9_shader.hpp"
#include "d3d9_device.hpp"
#include "sm3_decoder.hpp"
#include "log/log.hpp"

namespace dxmt {

namespace {
// A D3D9 shader token stream has no explicit length prefix; D3DSIO_END
// (0x0000FFFF) terminates it. GetFunction()/the decoder both need the exact
// token count up front.
size_t CountTokens(const DWORD *bytecode) {
  size_t n = 1; // version token
  while (bytecode[n] != 0x0000FFFFu)
    n++;
  return n + 1; // include the end token
}
} // namespace

template <typename Interface, typename D3D11Interface>
HRESULT
D3D9Shader<Interface, D3D11Interface>::CreateFromDXBC(const std::vector<uint8_t> &dxbc, Com<D3D11Interface> &out) {
  if constexpr (std::is_same_v<D3D11Interface, ID3D11VertexShader>) {
    return this->device_->GetD3D11Device()->CreateVertexShader(dxbc.data(), dxbc.size(), nullptr, &out);
  } else {
    return this->device_->GetD3D11Device()->CreatePixelShader(dxbc.data(), dxbc.size(), nullptr, &out);
  }
}

template <typename Interface, typename D3D11Interface>
D3D9Shader<Interface, D3D11Interface>::D3D9Shader(D3D9Device *pDevice, const DWORD *pFunction)
    : D3D9DeviceChild<Interface>(pDevice) {
  size_t token_count = CountTokens(pFunction);
  bytecode_.assign(pFunction, pFunction + token_count);

  auto decoded = sm3::Decode(reinterpret_cast<const uint32_t *>(bytecode_.data()), bytecode_.size());
  if (!decoded) {
    ERR("D3D9Shader: failed to decode SM1-3 bytecode");
    return;
  }
  decoded_ = std::move(*decoded);
  auto translated = sm3::Translate(decoded_);
  if (!translated.ok) {
    ERR("D3D9Shader: unsupported shader construct: ", translated.error);
    return;
  }
  float_constants_used_ = translated.float_constants_used;
  link_outputs_ = translated.link_outputs;
  link_inputs_ = translated.link_inputs;
  vertex_inputs_ = std::move(translated.vertex_inputs);
  translated_dxbc_ = translated.dxbc;

  if (FAILED(CreateFromDXBC(translated_dxbc_, d3d11_shader_))) {
    ERR("D3D9Shader: D3D11 shader creation failed for the translated DXBC");
    d3d11_shader_ = nullptr;
  }
}

template <typename Interface, typename D3D11Interface>
D3D11Interface *
D3D9Shader<Interface, D3D11Interface>::CompileVariant(std::pair<uint64_t, uint32_t> key,
                                                     const sm3::TranslateOptions &options) {
  auto it = variants_.find(key);
  if (it != variants_.end())
    return it->second.ptr();

  auto translated = sm3::Translate(decoded_, options);
  Com<D3D11Interface> variant;
  if (!translated.ok || FAILED(CreateFromDXBC(translated.dxbc, variant))) {
    ERR("D3D9Shader: variant compilation failed (key ", key.first, "/", key.second,
        "), falling back to the plain shader");
    variant = d3d11_shader_;
  }
  variants_.emplace(key, variant);
  return variant.ptr();
}

template <typename Interface, typename D3D11Interface>
D3D11Interface *
D3D9Shader<Interface, D3D11Interface>::GetD3D11ShaderVariant(
    uint32_t alpha_test_func, bool fog, uint32_t fog_table_mode, bool fog_w, bool point_sprite,
    uint32_t sampler_dims, uint16_t shadow_samplers, uint16_t fetch4_samplers, uint16_t fetch4_alpha
) {
  bool tests_alpha = alpha_test_func != 0 && alpha_test_func != D3DCMP_ALWAYS;
  if ((!tests_alpha && !fog && !point_sprite && !sampler_dims && !shadow_samplers && !fetch4_samplers) ||
      !d3d11_shader_)
    return d3d11_shader_.ptr();

  sm3::TranslateOptions options;
  options.alpha_test_func = tests_alpha ? alpha_test_func : 0;
  options.fog = fog;
  options.fog_table_mode = fog ? fog_table_mode : 0;
  options.fog_w = fog && fog_table_mode && fog_w;
  options.point_sprite = point_sprite;
  options.sampler_dims = sampler_dims;
  options.shadow_samplers = shadow_samplers;
  options.fetch4_samplers = fetch4_samplers;
  options.fetch4_alpha = fetch4_alpha & fetch4_samplers;
  /* The alpha-test function is a small enum, so the fog bits go above it.
   * Sampler dimensions only vary below ps_2_0, six samplers at most. */
  uint64_t key = options.alpha_test_func | (fog ? 0x100u : 0u) | (options.fog_table_mode << 12) |
                 (options.fog_w ? 0x10000u : 0u) | (point_sprite ? 0x20000u : 0u) |
                 (uint64_t(sampler_dims & 0xfff) << 24) | (uint64_t(shadow_samplers) << 40);
  return CompileVariant({key, fetch4_samplers | uint32_t(options.fetch4_alpha) << 16}, options);
}

template <typename Interface, typename D3D11Interface>
D3D11Interface *
D3D9Shader<Interface, D3D11Interface>::GetD3D11OutputVariant(uint32_t default_outputs, uint32_t default_white,
                                                             bool point_size, bool clip_planes,
                                                             uint32_t input_conversions) {
  if ((!default_outputs && !point_size && !clip_planes && !input_conversions) || !d3d11_shader_)
    return d3d11_shader_.ptr();
  sm3::TranslateOptions options;
  options.default_outputs = default_outputs;
  options.default_white = default_white & default_outputs;
  options.point_size = point_size;
  options.clip_planes = clip_planes;
  options.input_conversions = input_conversions;
  /* default_white only ever holds the diffuse register, so bits 0 and 2 --
   * never a varying's, sm3::kLinkColor0 being 1 -- are free for the point
   * size and the clip planes. */
  return CompileVariant({uint64_t(default_outputs) << 32 | options.default_white | (point_size ? 1u : 0u) |
                             (clip_planes ? 4u : 0u),
                         input_conversions},
                        options);
}

template <typename Interface, typename D3D11Interface>
HRESULT STDMETHODCALLTYPE
D3D9Shader<Interface, D3D11Interface>::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = ref(this->device_);
  return D3D_OK;
}

template class D3D9Shader<IDirect3DVertexShader9, ID3D11VertexShader>;
template class D3D9Shader<IDirect3DPixelShader9, ID3D11PixelShader>;

} // namespace dxmt
