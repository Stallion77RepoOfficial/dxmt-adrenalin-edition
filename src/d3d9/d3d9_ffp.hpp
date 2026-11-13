#pragma once

// Fixed-function pipeline emulation: generates SM4 vertex/pixel shaders from
// the D3D9 fixed-function state (transforms, lighting, texture stages, fog,
// alpha test) through the same Sm4Builder + D3D11 path the programmable
// shaders use. Shaders are cached on a state-derived key; the accompanying
// constant buffers are refilled from device state before each FFP draw.

#include "d3d9_private.h"
#include "sm3_translate.hpp"
#include "com/com_object.hpp"
#include <cstring>
#include <map>
#include <vector>

namespace dxmt {

class D3D9Device;

// One vertex-declaration input consumed by a generated FFP vertex shader,
// positionally matched against declaration elements exactly like the
// translated-shader path.
struct FFPVertexInput {
  uint32_t reg;
  std::string semantic;
  uint32_t semantic_index;
};

// Everything that changes the generated shader (as opposed to the constant
// buffer contents). Plain bytes so it can key a std::map.
/* Where an input's conversion sits in FFPShaderKey::input_conversions. */
enum FFPInputSlot : uint32_t {
  kFFPInputPosition = 0,
  kFFPInputNormal = 1,
  kFFPInputDiffuse = 2,
  kFFPInputSpecular = 3,
  kFFPInputPointSize = 4,
  kFFPInputTexcoord0 = 5, // through kFFPInputTexcoord0 + 7
};

struct FFPShaderKey {
  // Vertex side.
  uint8_t pretransformed = 0; // POSITIONT (XYZRHW) input
  uint8_t has_normal = 0;
  uint8_t has_diffuse = 0;
  uint8_t has_specular = 0;
  uint8_t texcoord_sets = 0;     // TEXCOORD sets the declaration provides, one bit each
  uint8_t texcoord_size[8] = {}; // components of each provided set
  uint8_t lighting = 0;
  uint8_t specular_enable = 0;
  uint8_t color_vertex = 0;
  uint8_t diffuse_source = 0;  // D3DMCS_*
  uint8_t ambient_source = 0;
  uint8_t specular_source = 0;
  uint8_t emissive_source = 0;
  uint8_t normalize_normals = 0;
  uint8_t light_count = 0;
  uint8_t light_type[8] = {}; // D3DLIGHT_* per enabled light, in slot order
  uint8_t fog_enable = 0;
  // D3D9 has two fog stages and they belong to different shaders. The vertex
  // mode makes the vertex shader compute a factor and pass it on; the table
  // mode makes the pixel shader compute one per pixel from the depth. A table
  // mode other than NONE wins, and then the vertex side does nothing.
  uint8_t fog_mode = 0;       // D3DFOG_*, vertex stage
  uint8_t fog_table_mode = 0; // D3DFOG_*, pixel stage
  uint8_t range_fog = 0;      // vertex fog on the distance to the eye rather than its depth
  /* Table fog runs on the clip-space w under a perspective projection and on
   * the depth otherwise -- D3D9's "w fog", which is what FOGSTART and FOGEND
   * are expressed in for the application. */
  uint8_t fog_w = 0;
  uint8_t texture_transform[8] = {}; // D3DTSS_TEXTURETRANSFORMFLAGS count field per stage
  uint8_t stage_texgen[8] = {};      // D3DTSS_TCI_* >> 16 per stage
  /* Stages whose texture coordinate the vertex shader hands on, at link
   * register kLinkTexcoord0 + stage, and other link registers the pixel stage
   * reads that the fixed-function vertex pipeline has no value for. */
  uint8_t vs_texcoord_outputs = 0;
  uint32_t vs_default_outputs = 0;
  /* sm3::InputConversion of each input, two bits per slot of
   * FFPInputSlot: integer vertex data reads as floats in D3D9. */
  uint32_t input_conversions = 0;
  /* Point lists. The vertex side gives each point its size -- the vertex's
   * PSIZE or D3DRS_POINTSIZE, scaled with the distance under
   * D3DRS_POINTSCALEENABLE, clamped to the size range -- and with
   * D3DRS_POINTSPRITEENABLE the pixel side samples every stage at the
   * position within the point instead of a texture coordinate. */
  uint8_t point_size = 0;
  uint8_t point_scale = 0;
  uint8_t has_psize = 0;
  uint8_t point_sprite = 0;
  uint8_t clip_planes = 0; // D3DRS_CLIPPLANEENABLE, planes in world space
  // Pixel side.
  uint8_t stage_color_op[8] = {};
  uint8_t stage_color_arg1[8] = {};
  uint8_t stage_color_arg2[8] = {};
  uint8_t stage_color_arg0[8] = {};
  uint8_t stage_alpha_op[8] = {};
  uint8_t stage_alpha_arg1[8] = {};
  uint8_t stage_alpha_arg2[8] = {};
  uint8_t stage_alpha_arg0[8] = {};
  uint8_t stage_texcoord_index[8] = {};
  uint8_t stage_texture_type[8] = {}; // 0=none 1=2D 2=cube 3=volume
  uint8_t stage_result_temp[8] = {};  // D3DTSS_RESULTARG == D3DTA_TEMP
  uint8_t stage_projected[8] = {};    // D3DTTFF_PROJECTED
  uint8_t stage_fetch4 = 0;           // stages sampling with ATI Fetch4
  uint8_t stage_fetch4_alpha = 0;     // ... of an alpha-only texture
  uint8_t alpha_test_func = 0;        // D3DCMP_* or 0

  bool operator<(const FFPShaderKey &o) const {
    return memcmp(this, &o, sizeof(*this)) < 0;
  }
};

struct FFPShaders {
  Com<ID3D11VertexShader> vs;
  Com<ID3D11PixelShader> ps;
  std::vector<uint8_t> vs_dxbc; // for CreateInputLayout
  std::vector<FFPVertexInput> vs_inputs;
};

// Layout of the FFP vertex constant buffer, in vec4 elements. All matrices
// are uploaded transposed so the shader's dp4-per-column matches D3D's
// row-vector convention.
namespace ffp {
constexpr uint32_t kVSWorldViewProj = 0;  // 4
constexpr uint32_t kVSWorldView = 4;      // 4
constexpr uint32_t kVSNormalMatrix = 8;   // 4 (inverse-transpose of world-view)
constexpr uint32_t kVSMaterialDiffuse = 12;
constexpr uint32_t kVSMaterialAmbient = 13;
constexpr uint32_t kVSMaterialSpecular = 14; // .w = power
constexpr uint32_t kVSMaterialEmissive = 15;
constexpr uint32_t kVSGlobalAmbient = 16;
constexpr uint32_t kVSFogParams = 17;     // x=start y=end z=density w=1/(end-start)
constexpr uint32_t kVSViewportScale = 18; // x=2/w y=-2/h (XYZRHW)
constexpr uint32_t kVSViewportOffset = 19;
constexpr uint32_t kVSTexTransform = 20;  // 8 matrices x 4 = 20..51
constexpr uint32_t kVSLights = 52;        // 8 lights x 6 = 52..99
constexpr uint32_t kVSPointSize = 100;    // size, minimum, maximum, viewport height
constexpr uint32_t kVSPointScale = 101;   // D3DRS_POINTSCALE_A, _B, _C; .w D3DRS_DEPTHBIAS
constexpr uint32_t kVSClipPlanes = 102;   // 8 user clip planes in view space, zeros where disabled
constexpr uint32_t kVSSizeVec4 = 110;
// Per light (base = kVSLights + i*6):
//  +0 diffuse, +1 specular (.w = spot falloff), +2 ambient,
//  +3 position (view space, .w = range), +4 direction (view space, .w = cos(theta/2)),
//  +5 attenuation (a0,a1,a2, .w = cos(phi/2))

constexpr uint32_t kPSTextureFactor = 0;
constexpr uint32_t kPSFogColor = 1;
constexpr uint32_t kPSAlphaRef = 2; // .x
constexpr uint32_t kPSStageConstant = 3; // 8 entries: 3..10
// Bump environment state, two vec4 per stage:
//   +0 (mat00, mat01, mat10, mat11)
//   +1 (lscale, loffset, 0, 0)
constexpr uint32_t kPSFogParams = 11;  // x=start y=end z=density, for table fog
constexpr uint32_t kPSBumpBase = 12;
constexpr uint32_t kPSBumpStride = 2;
constexpr uint32_t kPSSizeVec4 = kPSBumpBase + kPSBumpStride * 8;

/* Builds the fixed-function pixel shader for a key. Device-free, so the
 * generated DXBC can be checked without a D3D11 device. */
bool BuildPixelShaderDXBC(const struct ::dxmt::FFPShaderKey &key, std::vector<uint8_t> &ps_dxbc);
// Link registers (sm3::LinkBit) the fixed-function vertex shader of a key
// writes, and the ones its pixel shader reads given the stages that sample.
uint32_t VertexOutputs(const struct ::dxmt::FFPShaderKey &key);
uint32_t PixelInputs(const struct ::dxmt::FFPShaderKey &key, uint8_t sampled_stages);
} // namespace ffp

class FFPShaderCache {
public:
  explicit FFPShaderCache(D3D9Device *device) : device_(device) {}

  // Builds the key from current device state + the active vertex declaration.
  // vs/ps say which halves of the pipeline are fixed-function; the other half
  // leaves its part of the key zero. With a programmable pixel shader bound,
  // ps_inputs are the link registers it reads, which the fixed-function vertex
  // shader then has to write; with the fixed-function one, they are set to
  // the ones it reads.
  FFPShaderKey BuildKey(const std::vector<D3DVERTEXELEMENT9> &decl_elements, bool vs, bool ps,
                        uint32_t &ps_inputs, bool points);
  // Returns (creating if necessary) the shader pair for a key.
  FFPShaders *Get(const FFPShaderKey &key);
  // Fills the FFP constant buffers from current device state. Buffers must be
  // ffp::kVSSizeVec4*4 and ffp::kPSSizeVec4*4 floats respectively.
  void FillConstants(const FFPShaderKey &key, float *vs_out, float *ps_out);

private:
  bool Generate(const FFPShaderKey &key, FFPShaders &out);

  D3D9Device *device_;
  std::map<FFPShaderKey, FFPShaders> cache_;
};

} // namespace dxmt
