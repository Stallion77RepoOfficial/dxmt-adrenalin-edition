#pragma once

// Translates a decoded SM1-3 shader module into a real SM4.0 DXBC module via
// dxmt::sm4::Sm4Builder, reusing the (already AGX-verified) SM4 encoder
// rather than any bespoke Metal/AIR codegen. Register semantics differ across
// SM1-3 shader versions (implicit ps_1_x texture reads, vs<3.0 named output
// registers, vs3.0/ps3.0 generic numbered I/O) - all of that is resolved here
// so the rest of the pipeline only ever sees ordinary SM4.

#include "sm3_decoder.hpp"
#include "sm4_builder.hpp"
#include <string>

namespace dxmt::sm3 {

// One D3D9 declared vertex input, already resolved to a register + semantic.
struct VertexInputSlot {
  uint32_t reg;
  std::string semantic;
  uint32_t semantic_index;
  uint8_t mask;
  /* What the input links to in a D3D9 vertex declaration. D3D9 matches
   * declaration elements to shader inputs by (usage, usage index), never by
   * position. Taken from the input's dcl; a vs_1_x shader without dcls (as
   * d3d8 hands them over) uses its register's fixed D3DVSDE meaning. */
  uint8_t d3d9_usage = 0;
  uint8_t d3d9_usage_index = 0;
};

// Layout of the "extra constants" buffer (cb1) shared between the translator
// and D3D9Device::UploadShaderConstants. Element == vec4 index.
//  [0..15]  i0..i15  (SetVertexShaderConstantI / SetPixelShaderConstantI, raw int4)
//  [16..31] b0..b15  (SetVertexShaderConstantB / SetPixelShaderConstantB, 0/1 in .x)
//  [32].x   alpha-test reference value as float in [0,1] (pixel stage only)
constexpr uint32_t kExtraCBIntBase = 0;
constexpr uint32_t kExtraCBBoolBase = 16;
constexpr uint32_t kExtraCBMiscElement = 32;
// Bump environment mapping state, which texbem/texbeml/bem read but the shader
// never carries: the device copies D3DTSS_BUMPENVMAT00/01/10/11 and
// BUMPENVLSCALE/LOFFSET here, two vec4 per texture stage.
//   stage s + 0 : (mat00, mat01, mat10, mat11)
//   stage s + 1 : (lscale, loffset, 0, 0)
constexpr uint32_t kExtraCBBumpBase = 33;
constexpr uint32_t kExtraCBBumpStride = 2;
constexpr uint32_t kExtraCBBumpStages = 8;
// Fog, for the epilogue a user pixel shader gets when fog is enabled. D3D9
// applies fog after the pixel shader in fixed function hardware; D3D11 has no
// such stage, so it is baked into the shader the way alpha test is.
//   [49] fog colour, rgb
//   [50] (fog start, fog end, fog density, unused), for the table modes the
//        pixel shader computes itself
constexpr uint32_t kExtraCBFogColor = kExtraCBBumpBase + kExtraCBBumpStride * kExtraCBBumpStages;
constexpr uint32_t kExtraCBFogParams = kExtraCBFogColor + 1;
//   [51] (point size, minimum, maximum, unused), for point lists
constexpr uint32_t kExtraCBPointSize = kExtraCBFogParams + 1;
//   [52] .x D3DRS_DEPTHBIAS. D3D9 adds it to the depth as is; a rasterizer's
//        constant bias counts in steps of the depth format, which for the
//        float depth Metal stands in for D24 with shrink with the depth --
//        so the vertex stage adds it to z instead.
constexpr uint32_t kExtraCBDepthBias = kExtraCBPointSize + 1;
//   [53..60] the user clip planes in clip space, all zeros for one not
//            enabled -- which clips nothing (see TranslateOptions::clip_planes)
constexpr uint32_t kExtraCBClipPlanes = kExtraCBDepthBias + 1;
constexpr uint32_t kExtraCBSizeVec4 = kExtraCBClipPlanes + 8;
/* The two output registers the eight clip distances go out in: past every
 * link register, so they never collide with a varying. */
constexpr uint32_t kClipDistanceRegister = 32;

/* Where a varying lives between the vertex and the pixel stage.
 *
 * D3D9 matches a pixel shader input to a vertex shader output by semantic,
 * (usage, usage index). The SM4 these shaders become -- and airconv's Metal
 * linkage after it, which names each varying after its register -- matches by
 * register number instead. So every stage puts a semantic in the same
 * register: the ones the fixed-function pipeline knows at fixed places, any
 * other one a vs_3_0/ps_3_0 pair declares at a register handed out once per
 * process, so shaders translated independently of each other still agree.
 * Register 0 is SV_Position. */
constexpr uint32_t kLinkColor0 = 1; // COLOR0; COLOR1 is the next one
constexpr uint32_t kLinkTexcoord0 = 3; // TEXCOORD0..7
constexpr uint32_t kLinkFog = 11;
constexpr uint32_t kLinkPointSize = 12;
constexpr uint32_t kLinkFirstDynamic = 13;
constexpr uint32_t kLinkRegisterCount = 32;
constexpr uint32_t kLinkNone = ~0u;
// Register of a D3DDECLUSAGE_* semantic, or kLinkNone once all are taken.
uint32_t LinkRegister(uint8_t usage, uint8_t usage_index);
inline uint32_t LinkBit(uint32_t reg) { return 1u << reg; }
// Single-component varyings; every other one is a full vec4.
inline uint8_t LinkMask(uint32_t reg) { return reg == kLinkFog || reg == kLinkPointSize ? 0b0001 : 0b1111; }
// Declares link register reg as a varying: a vertex output or a pixel input,
// whichever stage the builder is for.
void DeclareLinkIO(sm4::Sm4Builder &b, uint32_t reg);

/* How a vertex input converts on entry, two bits per v# register in
 * TranslateOptions::input_conversions.
 *
 * D3D9 hands a vertex shader its integer vertex data as floats: UBYTE4 as
 * 0..255, SHORT2 and SHORT4 as -32768..32767. The D3D11 integer formats they
 * are fetched through hand over the integer's bits, which a float shader reads
 * as a number next to zero -- a normal of (0, 0, 0), a model lit black. DEC3N
 * and UDEC3 have no format a vertex can be fetched in at all: they arrive as
 * the raw 32-bit word and are decoded here, w being 1. */
enum InputConversion : uint32_t {
  kInputAsIs = 0,
  kInputIntToFloat = 1,
  kInputUDec3 = 2,
  kInputDec3N = 3,
};
inline uint32_t InputConversionOf(uint32_t conversions, uint32_t reg) {
  return reg < 16 ? (conversions >> (reg * 2)) & 3 : kInputAsIs;
}
// Writes input converted as conversion says into temp. A DEC3N decode also
// uses scratch.
void EmitInputConversion(sm4::Sm4Builder &b, uint32_t temp, uint32_t scratch, const sm4::Operand &input,
                         uint32_t conversion);

struct TranslateOptions {
  // D3DCMP_* function for a fixed-function alpha-test epilogue baked into the
  // pixel shader (D3D11 dropped alpha test, so it lives in the shader). 0 or
  // D3DCMP_ALWAYS = no test.
  uint32_t alpha_test_func = 0;
  // Blend the shader's colour towards the fog colour before it leaves the
  // pixel shader. The factor is the FOG input the vertex stage writes -- the
  // fixed-function vertex shader computes it for both vertex and table fog
  // modes, so the pixel side only has to consume it. Without this a user
  // pixel shader ignored fog entirely, which is most of what Wine's visual
  // suite fails on in both d3d8 and d3d9.
  bool fog = false;
  // D3DFOG_* table mode. D3D9 computes table fog per pixel from the depth,
  // which is the only way to get it right when the vertex stage does not
  // provide a factor at all -- a user vertex shader that never writes oFog --
  // and the only way to get the EXP and EXP2 curves right in any case, since
  // a per-vertex approximation of them interpolates linearly between samples.
  // D3DFOG_NONE means take the factor from the FOG input instead.
  uint32_t fog_table_mode = 0;
  // Table fog on the clip-space w (perspective projection) instead of depth.
  bool fog_w = false;
  /* Vertex shaders only, for point lists: give the points their size -- oPts
   * if the shader writes it, D3DRS_POINTSIZE if not, clamped to the minimum
   * and maximum -- on the SV_PointSize output. Otherwise oPts goes nowhere:
   * nothing reads it outside a point list. */
  bool point_size = false;
  /* Pixel shaders only, for point sprites: every texture coordinate the
   * shader reads becomes the position within the point. */
  bool point_sprite = false;
  /* Pixel shaders only. Below ps_2_0 a sampler has no dcl giving its texture
   * type; it is whatever is bound, two bits per sampler: 0 2D, 1 cube,
   * 2 volume. And D3D9 samples a depth texture -- D16, D24S8, D32 and the
   * like, not INTZ -- as a shadow map: the result is the comparison of the
   * coordinate's z with the stored depth, not the depth. One bit per sampler. */
  uint32_t sampler_dims = 0;
  uint16_t shadow_samplers = 0;
  /* ATI's Fetch4 (D3DSAMP_MIPMAPLODBIAS = 'GET4'): a single-channel texture
   * returns the 2x2 footprint a bilinear sample would blend, one texel per
   * channel. fetch4_alpha marks those whose one channel is alpha (A8). */
  uint16_t fetch4_samplers = 0;
  uint16_t fetch4_alpha = 0;
  /* Vertex shaders only: link registers the bound pixel stage reads that this
   * shader never writes. They are declared and given D3D9's value for an
   * unwritten varying -- Metal refuses to link a fragment input no vertex
   * output feeds. Registers in default_white get 1.0 (diffuse), the rest 0. */
  uint32_t default_outputs = 0;
  uint32_t default_white = 0;
  /* Vertex shaders only: D3D9's user clip planes, D3DRS_CLIPPLANEENABLE.
   * Eight SV_ClipDistances, each the dot product of the position with a
   * plane from the extra constant buffer. */
  bool clip_planes = false;
  /* Vertex shaders only: InputConversion per v#, two bits each. */
  uint32_t input_conversions = 0;
};

struct TranslationResult {
  bool ok = false;
  std::string error;
  std::vector<uint8_t> dxbc;
  // Vertex shader only: v# -> semantic, in declaration/register order, for
  // building the matching D3D11_INPUT_ELEMENT_DESC array.
  std::vector<VertexInputSlot> vertex_inputs;
  // Highest float/int/bool constant register index referenced (+1 = count),
  // so the device only needs to upload as many vec4s as are actually used.
  uint32_t float_constants_used = 0;
  uint32_t int_constants_used = 0;
  uint32_t bool_constants_used = 0;
  // Vertex shader only: whether it writes oFog. A pixel shader may only take
  // the fog epilogue when something upstream actually produces the factor --
  // declaring an input no stage writes leaves the pipeline unlinkable.
  bool writes_fog = false;
  uint32_t sampler_count = 0;
  // LinkBit()s of the varyings a vertex shader writes / a pixel shader reads.
  uint32_t link_outputs = 0;
  uint32_t link_inputs = 0;
};

TranslationResult Translate(const ShaderModule &module, const TranslateOptions &options = {});

} // namespace dxmt::sm3
