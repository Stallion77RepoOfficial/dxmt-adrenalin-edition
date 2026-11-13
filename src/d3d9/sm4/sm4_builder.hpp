#pragma once

// Minimal SM4 (Shader Model 4.0) DXBC bytecode assembler.
//
// airconv's DXBC front end (SM50Initialize/convertDXBC) never reads the RDEF
// chunk - constant buffer sizes, resource dimensions and sampler modes all
// come from the DCL_* instructions inside the SHDR/SHEX token stream itself,
// and I/O semantics come from the ISGN/OSGN chunks (read via
// microsoft::DXBCGetInputSignature/DXBCGetOutputSignature). This builder
// therefore only ever emits: a DXBC container header, one ISGN blob, one OSGN
// blob and one SHDR blob - enough for a real, from-scratch DXBC module that
// the existing D3D10ShaderBinary::CShaderCodeParser (already used throughout
// airconv) can parse exactly like compiler output.
//
// Token/operand field layout is taken directly from
// libs/DXBCParser/ShaderBinary.cpp's CShaderCodeParser::ParseInstruction /
// ParseOperand (the decoder side) and encoded with the same
// ENCODE_D3D10_SB_* macros from d3d12tokenizedprogramformat.hpp Microsoft
// ships in this repo, so encoding is the verified inverse of what airconv
// already decodes correctly - not a guessed format.

#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dxmt::sm4 {

using Token = uint32_t;

// A source or destination operand: register file, index/indices, and either
// a 4-component swizzle (source) or write mask (destination).
struct Operand {
  microsoft::D3D10_SB_OPERAND_TYPE type;
  uint32_t index0 = 0; // e.g. temp/input/output/sampler/resource number, or cb slot
  uint32_t index1 = 0; // second dimension, e.g. element within a constant buffer
  bool two_dimensional = false;
  bool zero_component = false; // operand carries no components (e.g. loop aL, vRasterizer)
  uint8_t swizzle[4] = {0, 1, 2, 3}; // component source per output lane (src operands)
  uint8_t mask = 0xf;                // write mask, 4 bits (dst operands)
  bool is_swizzle = true; // true: use swizzle[]; false: use mask (mutually exclusive per D3D10 SB)
  // Select-1 mode: one component, swizzle[0]. What picks the channel gather4
  // reads, on its sampler operand.
  bool select_1 = false;
  bool negate = false;
  bool absolute = false;
  // Immediate32 value (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
  float imm[4] = {0, 0, 0, 0};
  bool imm_is_scalar = false;
  // Dynamic (register-relative) indexing of the last index dimension:
  // cb#[rel_temp.rel_component + index1]. Encoded as
  // D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE with a nested r# operand
  // in SELECT_1 component mode.
  bool last_index_relative = false;
  uint32_t rel_temp = 0;
  uint8_t rel_component = 0;
  // Set on a *destination* operand to request the instruction-level _sat
  // (clamp to [0,1]) flag on whatever instruction writes it (D3D9's _sat
  // destination modifier maps onto SM4's per-instruction saturate bit).
  bool saturate_dst = false;
};

Operand Saturate(Operand o);

// --- Operand factories -----------------------------------------------------
Operand Temp(uint32_t index, uint8_t mask = 0xf);
Operand TempSwizzle(uint32_t index, uint8_t x, uint8_t y, uint8_t z, uint8_t w);
Operand Input(uint32_t index, uint8_t x = 0, uint8_t y = 1, uint8_t z = 2, uint8_t w = 3);
Operand Output(uint32_t index, uint8_t mask = 0xf);
Operand ConstantBuffer(uint32_t cb, uint32_t element, uint8_t x = 0, uint8_t y = 1, uint8_t z = 2, uint8_t w = 3);
// cb#[r{rel_temp}.{rel_component} + element] - dynamically indexed constant
// buffer element (D3D9 relative addressing: c[a0.x + n], c[aL + n]).
Operand ConstantBufferRel(uint32_t cb, uint32_t element, uint32_t rel_temp, uint8_t rel_component,
                          uint8_t x = 0, uint8_t y = 1, uint8_t z = 2, uint8_t w = 3);
Operand Sampler(uint32_t index);
Operand Resource(uint32_t index, uint8_t x = 0, uint8_t y = 1, uint8_t z = 2, uint8_t w = 3);
Operand Imm4(float x, float y, float z, float w);
Operand Imm1(float x);
// Immediate32 operands holding raw integer bit patterns (for the int ALU ops).
Operand ImmI4(int32_t x, int32_t y, int32_t z, int32_t w);
Operand ImmI1(int32_t x);
Operand OutputDepth();

Operand WithSwizzle(Operand o, uint8_t x, uint8_t y, uint8_t z, uint8_t w);
Operand WithMask(Operand o, uint8_t mask);
Operand Neg(Operand o);
Operand Abs(Operand o);

enum class Interpolation {
  Constant,
  Linear,
  LinearCentroid,
  LinearNoPerspective,
  LinearNoPerspectiveCentroid,
};

// SV_* system-value kinds this builder can declare for I/O registers.
enum class SystemValue {
  None,
  Position,     // SV_Position (vertex shader output / pixel shader input)
  VertexID,
  InstanceID,
  IsFrontFace,
  Target,       // SV_Target (pixel shader output)
  Depth,        // SV_Depth (pixel shader output)
};

struct Signature {
  std::string semantic_name;
  uint32_t semantic_index = 0;
  uint32_t reg = 0;
  uint8_t mask = 0xf;
  bool is_float = true; // false => uint component type (used for SV_VertexID/InstanceID)
};

// Builds one SM4.0 vertex or pixel shader and assembles it into a complete
// in-memory DXBC container (header + ISGN + OSGN + SHDR).
class Sm4Builder {
public:
  enum class Stage { Vertex, Pixel };
  explicit Sm4Builder(Stage stage) : stage_(stage) {}

  // --- resource declarations (call before any instruction emission) --------
  void DeclConstantBuffer(uint32_t cb, uint32_t size_in_vec4);
  // comparison: a sampler for sample_c, which D3D9 depth textures need.
  void DeclSampler(uint32_t slot, bool comparison = false);
  void DeclResourceTexture1D(uint32_t slot);
  void DeclResourceTexture2D(uint32_t slot);
  void DeclResourceTexture3D(uint32_t slot);
  void DeclResourceTextureCube(uint32_t slot);

  // --- I/O declarations ------------------------------------------------------
  // Vertex shader input (matches a vertex declaration slot by register number).
  void DeclInput(uint32_t reg, uint8_t mask, const std::string &semantic, uint32_t semantic_index);
  // Vertex shader output / pixel shader input with an ordinary semantic.
  void DeclIOWithSemantic(uint32_t reg, uint8_t mask, const std::string &semantic, uint32_t semantic_index,
                          Interpolation interp = Interpolation::Linear);
  // Vertex shader SV_Position output.
  void DeclOutputPosition(uint32_t reg);
  // Four clip distances in one register (vertex shaders).
  void DeclOutputClipDistance(uint32_t reg, uint32_t semantic_index);
  // Pixel shader SV_Position input (always constant/unmodified interpolation).
  void DeclInputPositionPS(uint32_t reg);
  // SV_IsFrontFace, which backs D3D9's vFace. Declared as unsigned: the value
  // is 0 or ~0 rather than a float.
  void DeclInputIsFrontFace(uint32_t reg);
  // Pixel shader SV_Target output.
  void DeclOutputTarget(uint32_t reg, uint32_t rt_index);

  // --- register allocation ---------------------------------------------------
  uint32_t AllocTemp() { return next_temp_++; }
  uint32_t TempCount() const { return next_temp_; }
  // Reserves temp indices [0, count) as already in use without handing any
  // of them out via AllocTemp() - used when the caller has its own register
  // numbering (SM1-3 r# registers map straight onto sm4 temp indices) and
  // AllocTemp() is only used for additional scratch registers on top.
  void ReserveTemps(uint32_t count) {
    if (count > next_temp_) next_temp_ = count;
  }

  // --- instruction emission ---------------------------------------------------
  void Mov(Operand dst, Operand src);
  void Add(Operand dst, Operand a, Operand b);
  void Mul(Operand dst, Operand a, Operand b);
  void Mad(Operand dst, Operand a, Operand b, Operand c);
  void Div(Operand dst, Operand a, Operand b);
  void Dp2(Operand dst, Operand a, Operand b);
  void Dp3(Operand dst, Operand a, Operand b);
  void Dp4(Operand dst, Operand a, Operand b);
  void Min(Operand dst, Operand a, Operand b);
  void Max(Operand dst, Operand a, Operand b);
  void Rsq(Operand dst, Operand src);
  void Rcp(Operand dst, Operand src);
  void Frc(Operand dst, Operand src);
  void Log(Operand dst, Operand src);   // log2
  void ExpI(Operand dst, Operand src);  // exp2
  void Sqrt(Operand dst, Operand src);
  void SinCos(Operand dst_sin, Operand dst_cos, Operand src);
  void Lt(Operand dst, Operand a, Operand b);
  void Ge(Operand dst, Operand a, Operand b);
  void Eq(Operand dst, Operand a, Operand b);
  void Ne(Operand dst, Operand a, Operand b);
  void Movc(Operand dst, Operand cond, Operand a, Operand b);
  void Sample(Operand dst, Operand coord, uint32_t resource, uint32_t sampler);
  void SampleL(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand lod);
  void SampleB(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand bias);
  // texldd: explicit screen-space gradients, so a sample inside divergent
  // control flow still picks a sane mip level.
  void SampleD(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand ddx, Operand ddy);
  // Depth comparison: ref against the texel, through the sampler's function.
  void SampleC(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand ref);
  void SampleCLz(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand ref);
  // The given channel of the four texels a bilinear sample would blend,
  // as (-u+v, +u+v, +u-v, -u-v) -- SM4.1 gather4.
  void Gather4(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, uint8_t channel = 0);
  // The reciprocal of the resource's level-0 size, in .xyz.
  void ResInfoRcp(Operand dst, uint32_t resource);
  // dsx/dsy: derivative of a value across the pixel quad.
  void DerivRtx(Operand dst, Operand src);
  void DerivRty(Operand dst, Operand src);
  void If(bool test_nonzero, Operand cond);
  void Else();
  void EndIf();
  void Discard(bool test_nonzero, Operand cond);
  void Ret();
  // Integer ALU / conversion (needed for D3D9 address registers and loops).
  void IAdd(Operand dst, Operand a, Operand b);
  void ILt(Operand dst, Operand a, Operand b);
  void IGe(Operand dst, Operand a, Operand b);
  void IEq(Operand dst, Operand a, Operand b);
  void INe(Operand dst, Operand a, Operand b);
  void And(Operand dst, Operand a, Operand b);
  void UShr(Operand dst, Operand a, Operand b);
  void Ftoi(Operand dst, Operand src);
  void Itof(Operand dst, Operand src);
  void RoundNe(Operand dst, Operand src);
  void RoundZ(Operand dst, Operand src);
  // Structured loop control flow.
  void Loop();
  void EndLoop();
  void Break();
  void BreakC(bool test_nonzero, Operand cond);

  // Assembles the final DXBC container. Returns false (with no partial data)
  // if the builder produced more registers/instructions than this minimal
  // assembler tracks correctly.
  bool Finish(std::vector<uint8_t> &out_dxbc);

private:
  Stage stage_;
  uint32_t next_temp_ = 0;
  std::vector<Token> decls_;
  std::vector<Token> code_;
  std::vector<Signature> inputs_;
  std::vector<Signature> outputs_;
  bool has_sample_resources_ = false;

  void EmitSimple(microsoft::D3D10_SB_OPCODE_TYPE op,
                  std::initializer_list<Operand> operands);
};

// Encodes one operand's tokens (type/mask-or-swizzle/modifier/index) and
// appends them to `out`. Exposed at namespace scope since resource/sampler
// declarations need it outside of instruction emission too.
void EmitOperand(std::vector<Token> &out, const Operand &op);

} // namespace dxmt::sm4
