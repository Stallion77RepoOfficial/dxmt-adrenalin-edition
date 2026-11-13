#include "sm4_builder.hpp"
#include <algorithm>
#include "DXBCParser/BlobContainer.h"
#include <cstring>

using namespace microsoft;

namespace dxmt::sm4 {

// ---------------------------------------------------------------------------
// Operand factories
// ---------------------------------------------------------------------------

Operand Temp(uint32_t index, uint8_t mask) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_TEMP;
  o.index0 = index;
  o.mask = mask;
  o.is_swizzle = false;
  return o;
}

Operand TempSwizzle(uint32_t index, uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_TEMP;
  o.index0 = index;
  o.swizzle[0] = x; o.swizzle[1] = y; o.swizzle[2] = z; o.swizzle[3] = w;
  o.is_swizzle = true;
  return o;
}

Operand Input(uint32_t index, uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_INPUT;
  o.index0 = index;
  o.swizzle[0] = x; o.swizzle[1] = y; o.swizzle[2] = z; o.swizzle[3] = w;
  o.is_swizzle = true;
  return o;
}

Operand Output(uint32_t index, uint8_t mask) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_OUTPUT;
  o.index0 = index;
  o.mask = mask;
  o.is_swizzle = false;
  return o;
}

Operand ConstantBuffer(uint32_t cb, uint32_t element, uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER;
  o.index0 = cb;
  o.index1 = element;
  o.two_dimensional = true;
  o.swizzle[0] = x; o.swizzle[1] = y; o.swizzle[2] = z; o.swizzle[3] = w;
  o.is_swizzle = true;
  return o;
}

Operand ConstantBufferRel(uint32_t cb, uint32_t element, uint32_t rel_temp, uint8_t rel_component,
                          uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
  Operand o = ConstantBuffer(cb, element, x, y, z, w);
  o.last_index_relative = true;
  o.rel_temp = rel_temp;
  o.rel_component = rel_component;
  return o;
}

Operand Sampler(uint32_t index) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_SAMPLER;
  o.index0 = index;
  o.zero_component = true;
  return o;
}

Operand Resource(uint32_t index, uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_RESOURCE;
  o.index0 = index;
  o.swizzle[0] = x; o.swizzle[1] = y; o.swizzle[2] = z; o.swizzle[3] = w;
  o.is_swizzle = true;
  return o;
}

Operand Imm4(float x, float y, float z, float w) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_IMMEDIATE32;
  o.imm[0] = x; o.imm[1] = y; o.imm[2] = z; o.imm[3] = w;
  o.imm_is_scalar = false;
  return o;
}

Operand Imm1(float x) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_IMMEDIATE32;
  o.imm[0] = x;
  o.imm_is_scalar = true;
  return o;
}

Operand ImmI4(int32_t x, int32_t y, int32_t z, int32_t w) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_IMMEDIATE32;
  int32_t v[4] = {x, y, z, w};
  memcpy(o.imm, v, sizeof(v));
  o.imm_is_scalar = false;
  return o;
}

Operand ImmI1(int32_t x) {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_IMMEDIATE32;
  memcpy(&o.imm[0], &x, sizeof(x));
  o.imm_is_scalar = true;
  return o;
}

Operand OutputDepth() {
  Operand o{};
  o.type = D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH;
  o.zero_component = true;
  return o;
}

Operand WithSwizzle(Operand o, uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
  o.swizzle[0] = x; o.swizzle[1] = y; o.swizzle[2] = z; o.swizzle[3] = w;
  o.is_swizzle = true;
  return o;
}

Operand WithMask(Operand o, uint8_t mask) {
  o.mask = mask;
  o.is_swizzle = false;
  return o;
}

Operand Neg(Operand o) { o.negate = true; return o; }
Operand Abs(Operand o) { o.absolute = true; return o; }
Operand Saturate(Operand o) { o.saturate_dst = true; return o; }

// ---------------------------------------------------------------------------
// Token encoding
// ---------------------------------------------------------------------------

void EmitOperand(std::vector<Token> &out, const Operand &op) {
  Token t0 = ENCODE_D3D10_SB_OPERAND_TYPE(op.type);

  if (op.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
    t0 |= ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(
        op.imm_is_scalar ? D3D10_SB_OPERAND_1_COMPONENT : D3D10_SB_OPERAND_4_COMPONENT);
    t0 |= ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_0D);
    out.push_back(t0);
    /* An immediate is emitted without the extended operand token, so a
     * neg/abs modifier on it has to be folded into the value: both are
     * sign-bit operations on a float, which is all the modifier would do.
     * Dropping them turned "-c0" on a def'd constant into "+c0". */
    auto bits = [&](int i) {
      Token v;
      memcpy(&v, &op.imm[i], sizeof(v));
      if (op.absolute)
        v &= 0x7fffffffu;
      if (op.negate)
        v ^= 0x80000000u;
      return v;
    };
    if (op.imm_is_scalar) {
      out.push_back(bits(0));
    } else {
      for (int i = 0; i < 4; i++)
        out.push_back(bits(i));
    }
    return;
  }

  bool has_modifier = op.negate || op.absolute;

  if (op.zero_component) {
    t0 |= ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_0_COMPONENT);
  } else {
    t0 |= ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT);
    if (op.select_1) {
      t0 |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE);
      t0 |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(op.swizzle[0]);
    } else if (op.is_swizzle) {
      t0 |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE);
      t0 |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE(op.swizzle[0], op.swizzle[1], op.swizzle[2], op.swizzle[3]);
    } else {
      t0 |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE);
      t0 |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_MASK((Token)op.mask << D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT);
    }
  }

  auto dim = op.two_dimensional ? D3D10_SB_OPERAND_INDEX_2D : D3D10_SB_OPERAND_INDEX_1D;
  int last_dim = op.two_dimensional ? 1 : 0;
  t0 |= ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(dim);
  t0 |= ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32);
  if (op.two_dimensional)
    t0 |= ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, D3D10_SB_OPERAND_INDEX_IMMEDIATE32);
  if (op.last_index_relative) {
    // Overrides the representation set just above for the last dimension.
    t0 &= ~(D3D10_SB_OPERAND_INDEX_REPRESENTATION_MASK(last_dim));
    t0 |= ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(last_dim, D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE);
  }
  t0 |= ENCODE_D3D10_SB_OPERAND_EXTENDED(has_modifier);

  out.push_back(t0);

  if (has_modifier) {
    D3D10_SB_OPERAND_MODIFIER mod = op.negate && op.absolute
        ? D3D10_SB_OPERAND_MODIFIER_ABSNEG
        : (op.negate ? D3D10_SB_OPERAND_MODIFIER_NEG : D3D10_SB_OPERAND_MODIFIER_ABS);
    out.push_back(ENCODE_D3D10_SB_EXTENDED_OPERAND_MODIFIER(mod));
  }

  out.push_back(op.index0);
  if (op.two_dimensional)
    out.push_back(op.index1);
  if (op.last_index_relative) {
    // The relative register follows the immediate index as a complete nested
    // operand: r{rel_temp} in single-component-select mode.
    Token rt = ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_TEMP);
    rt |= ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT);
    rt |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE);
    rt |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(op.rel_component);
    rt |= ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D);
    rt |= ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32);
    out.push_back(rt);
    out.push_back(op.rel_temp);
  }
}

void Sm4Builder::EmitSimple(D3D10_SB_OPCODE_TYPE op, std::initializer_list<Operand> operands) {
  std::vector<Token> body;
  bool saturate = false;
  for (auto &o : operands) {
    EmitOperand(body, o);
    saturate |= o.saturate_dst;
  }
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(op);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  if (saturate)
    t0 |= ENCODE_D3D10_SB_INSTRUCTION_SATURATE(1);
  code_.push_back(t0);
  code_.insert(code_.end(), body.begin(), body.end());
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

void Sm4Builder::DeclConstantBuffer(uint32_t cb, uint32_t size_in_vec4) {
  std::vector<Token> body;
  Operand cbOperand{};
  cbOperand.type = D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER;
  cbOperand.index0 = cb;
  cbOperand.index1 = size_in_vec4;
  cbOperand.two_dimensional = true;
  cbOperand.zero_component = true;
  EmitOperand(body, cbOperand);
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER);
  t0 |= ENCODE_D3D10_SB_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(D3D10_SB_CONSTANT_BUFFER_IMMEDIATE_INDEXED);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls_.push_back(t0);
  decls_.insert(decls_.end(), body.begin(), body.end());
}

void Sm4Builder::DeclSampler(uint32_t slot, bool comparison) {
  std::vector<Token> body;
  EmitOperand(body, Sampler(slot));
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_SAMPLER);
  t0 |= ENCODE_D3D10_SB_SAMPLER_MODE(comparison ? D3D10_SB_SAMPLER_MODE_COMPARISON : D3D10_SB_SAMPLER_MODE_DEFAULT);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls_.push_back(t0);
  decls_.insert(decls_.end(), body.begin(), body.end());
}

static void DeclResourceCommon(std::vector<Token> &decls, uint32_t slot, D3D10_SB_RESOURCE_DIMENSION dim) {
  std::vector<Token> body;
  // DCL_RESOURCE's operand carries no component selection, just the t# index.
  Operand r{};
  r.type = D3D10_SB_OPERAND_TYPE_RESOURCE;
  r.index0 = slot;
  r.zero_component = true;
  EmitOperand(body, r);

  Token retTypeTok = ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, 0) |
                      ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, 1) |
                      ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, 2) |
                      ENCODE_D3D10_SB_RESOURCE_RETURN_TYPE(D3D10_SB_RETURN_TYPE_FLOAT, 3);
  body.push_back(retTypeTok);

  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_RESOURCE);
  t0 |= ENCODE_D3D10_SB_RESOURCE_DIMENSION(dim);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls.push_back(t0);
  decls.insert(decls.end(), body.begin(), body.end());
}

void Sm4Builder::DeclResourceTexture1D(uint32_t slot) {
  DeclResourceCommon(decls_, slot, D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D);
}
void Sm4Builder::DeclResourceTexture2D(uint32_t slot) {
  DeclResourceCommon(decls_, slot, D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D);
}
void Sm4Builder::DeclResourceTexture3D(uint32_t slot) {
  DeclResourceCommon(decls_, slot, D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D);
}
void Sm4Builder::DeclResourceTextureCube(uint32_t slot) {
  DeclResourceCommon(decls_, slot, D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE);
}

void Sm4Builder::DeclInput(uint32_t reg, uint8_t mask, const std::string &semantic, uint32_t semantic_index) {
  std::vector<Token> body;
  EmitOperand(body, WithMask(Input(reg), mask));
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_INPUT);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls_.push_back(t0);
  decls_.insert(decls_.end(), body.begin(), body.end());
  inputs_.push_back({semantic, semantic_index, reg, mask, true});
}

static D3D10_SB_INTERPOLATION_MODE ToSBInterp(dxmt::sm4::Interpolation i) {
  switch (i) {
  case dxmt::sm4::Interpolation::Constant: return D3D10_SB_INTERPOLATION_CONSTANT;
  case dxmt::sm4::Interpolation::LinearCentroid: return D3D10_SB_INTERPOLATION_LINEAR_CENTROID;
  case dxmt::sm4::Interpolation::LinearNoPerspective: return D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE;
  case dxmt::sm4::Interpolation::LinearNoPerspectiveCentroid: return D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_CENTROID;
  default: return D3D10_SB_INTERPOLATION_LINEAR;
  }
}

void Sm4Builder::DeclIOWithSemantic(uint32_t reg, uint8_t mask, const std::string &semantic, uint32_t semantic_index,
                                    Interpolation interp) {
  if (stage_ == Stage::Vertex) {
    std::vector<Token> body;
    EmitOperand(body, WithMask(Output(reg), mask));
    Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_OUTPUT);
    t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
    decls_.push_back(t0);
    decls_.insert(decls_.end(), body.begin(), body.end());
    outputs_.push_back({semantic, semantic_index, reg, mask, true});
  } else {
    std::vector<Token> body;
    EmitOperand(body, WithMask(Input(reg), mask));
    Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_INPUT_PS);
    t0 |= ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(ToSBInterp(interp));
    t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
    decls_.push_back(t0);
    decls_.insert(decls_.end(), body.begin(), body.end());
    inputs_.push_back({semantic, semantic_index, reg, mask, true});
  }
}

void Sm4Builder::DeclOutputPosition(uint32_t reg) {
  std::vector<Token> body;
  EmitOperand(body, WithMask(Output(reg), 0xf));
  body.push_back(ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION));
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_OUTPUT_SIV);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls_.push_back(t0);
  decls_.insert(decls_.end(), body.begin(), body.end());
  outputs_.push_back({"SV_Position", 0, reg, 0xf, true});
}

void Sm4Builder::DeclOutputClipDistance(uint32_t reg, uint32_t semantic_index) {
  std::vector<Token> body;
  EmitOperand(body, WithMask(Output(reg), 0xf));
  body.push_back(ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_CLIP_DISTANCE));
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_OUTPUT_SIV);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls_.push_back(t0);
  decls_.insert(decls_.end(), body.begin(), body.end());
  outputs_.push_back({"SV_ClipDistance", semantic_index, reg, 0xf, true});
}

void Sm4Builder::DeclInputPositionPS(uint32_t reg) {
  std::vector<Token> body;
  EmitOperand(body, WithMask(Input(reg), 0xf));
  body.push_back(ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION));
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_INPUT_PS_SIV);
  t0 |= ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls_.push_back(t0);
  decls_.insert(decls_.end(), body.begin(), body.end());
  inputs_.push_back({"SV_Position", 0, reg, 0xf, true});
}

void Sm4Builder::DeclInputIsFrontFace(uint32_t reg) {
  std::vector<Token> body;
  EmitOperand(body, WithMask(Input(reg), 0b0001));
  body.push_back(ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_IS_FRONT_FACE));
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_INPUT_PS_SGV);
  t0 |= ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(D3D10_SB_INTERPOLATION_CONSTANT);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls_.push_back(t0);
  decls_.insert(decls_.end(), body.begin(), body.end());
  /* Declared with an unsigned component type: SV_IsFrontFace is 0 or ~0, not a
   * float, and D3D9's vFace sign convention is applied by the translator. */
  inputs_.push_back({"SV_IsFrontFace", 0, reg, 0b0001, false});
}

void Sm4Builder::DeclOutputTarget(uint32_t reg, uint32_t rt_index) {
  std::vector<Token> body;
  EmitOperand(body, WithMask(Output(reg), 0xf));
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_OUTPUT);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  decls_.push_back(t0);
  decls_.insert(decls_.end(), body.begin(), body.end());
  outputs_.push_back({"SV_Target", rt_index, reg, 0xf, true});
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

void Sm4Builder::Mov(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_MOV, {dst, src}); }
void Sm4Builder::Add(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_ADD, {dst, a, b}); }
void Sm4Builder::Mul(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_MUL, {dst, a, b}); }
void Sm4Builder::Mad(Operand dst, Operand a, Operand b, Operand c) { EmitSimple(D3D10_SB_OPCODE_MAD, {dst, a, b, c}); }
void Sm4Builder::Div(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_DIV, {dst, a, b}); }
void Sm4Builder::Dp2(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_DP2, {dst, a, b}); }
void Sm4Builder::Dp3(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_DP3, {dst, a, b}); }
void Sm4Builder::Dp4(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_DP4, {dst, a, b}); }
void Sm4Builder::Min(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_MIN, {dst, a, b}); }
void Sm4Builder::Max(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_MAX, {dst, a, b}); }
void Sm4Builder::Rsq(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_RSQ, {dst, src}); }
void Sm4Builder::Rcp(Operand dst, Operand src) { EmitSimple(D3D11_SB_OPCODE_RCP, {dst, src}); }
void Sm4Builder::Frc(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_FRC, {dst, src}); }
void Sm4Builder::Log(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_LOG, {dst, src}); }
void Sm4Builder::ExpI(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_EXP, {dst, src}); }
void Sm4Builder::Sqrt(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_SQRT, {dst, src}); }
void Sm4Builder::SinCos(Operand dst_sin, Operand dst_cos, Operand src) {
  EmitSimple(D3D10_SB_OPCODE_SINCOS, {dst_sin, dst_cos, src});
}
void Sm4Builder::Lt(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_LT, {dst, a, b}); }
void Sm4Builder::Ge(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_GE, {dst, a, b}); }
void Sm4Builder::Eq(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_EQ, {dst, a, b}); }
void Sm4Builder::Ne(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_NE, {dst, a, b}); }
void Sm4Builder::Movc(Operand dst, Operand cond, Operand a, Operand b) {
  EmitSimple(D3D10_SB_OPCODE_MOVC, {dst, cond, a, b});
}

void Sm4Builder::Sample(Operand dst, Operand coord, uint32_t resource, uint32_t sampler) {
  has_sample_resources_ = true;
  EmitSimple(D3D10_SB_OPCODE_SAMPLE, {dst, coord, Resource(resource), Sampler(sampler)});
}
void Sm4Builder::SampleL(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand lod) {
  has_sample_resources_ = true;
  EmitSimple(D3D10_SB_OPCODE_SAMPLE_L, {dst, coord, Resource(resource), Sampler(sampler), lod});
}
void Sm4Builder::SampleB(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand bias) {
  has_sample_resources_ = true;
  EmitSimple(D3D10_SB_OPCODE_SAMPLE_B, {dst, coord, Resource(resource), Sampler(sampler), bias});
}
void Sm4Builder::SampleD(
    Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand ddx, Operand ddy
) {
  has_sample_resources_ = true;
  EmitSimple(D3D10_SB_OPCODE_SAMPLE_D, {dst, coord, Resource(resource), Sampler(sampler), ddx, ddy});
}
void Sm4Builder::SampleC(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand ref) {
  has_sample_resources_ = true;
  // The comparison result is one value; the .xxxx resource swizzle spreads it.
  EmitSimple(D3D10_SB_OPCODE_SAMPLE_C, {dst, coord, Resource(resource, 0, 0, 0, 0), Sampler(sampler), ref});
}
void Sm4Builder::SampleCLz(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, Operand ref) {
  has_sample_resources_ = true;
  EmitSimple(D3D10_SB_OPCODE_SAMPLE_C_LZ, {dst, coord, Resource(resource, 0, 0, 0, 0), Sampler(sampler), ref});
}
void Sm4Builder::Gather4(Operand dst, Operand coord, uint32_t resource, uint32_t sampler, uint8_t channel) {
  has_sample_resources_ = true;
  /* The channel gathered is the sampler operand's selected component. The
   * resource's swizzle applies to the four texels returned, so it stays
   * .xyzw -- putting the channel there, as this did, handed back one texel
   * four times. */
  Operand s = Sampler(sampler);
  s.zero_component = false;
  s.select_1 = true;
  s.swizzle[0] = channel;
  EmitSimple(D3D10_1_SB_OPCODE_GATHER4, {dst, coord, Resource(resource, 0, 1, 2, 3), s});
}
void Sm4Builder::ResInfoRcp(Operand dst, uint32_t resource) {
  /* resinfo_rcpfloat: (1/width, 1/height, 1/depth, levels) of level 0. */
  std::vector<Token> body;
  EmitOperand(body, dst);
  EmitOperand(body, Imm4(0, 0, 0, 0));
  EmitOperand(body, Resource(resource, 0, 1, 2, 3));
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RESINFO);
  t0 |= ENCODE_D3D10_SB_RESINFO_INSTRUCTION_RETURN_TYPE(D3D10_SB_RESINFO_INSTRUCTION_RETURN_RCPFLOAT);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  code_.push_back(t0);
  code_.insert(code_.end(), body.begin(), body.end());
}
void Sm4Builder::DerivRtx(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_DERIV_RTX, {dst, src}); }
void Sm4Builder::DerivRty(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_DERIV_RTY, {dst, src}); }

void Sm4Builder::If(bool test_nonzero, Operand cond) {
  std::vector<Token> body;
  EmitOperand(body, cond);
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_IF);
  t0 |= ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(
      test_nonzero ? D3D10_SB_INSTRUCTION_TEST_NONZERO : D3D10_SB_INSTRUCTION_TEST_ZERO);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  code_.push_back(t0);
  code_.insert(code_.end(), body.begin(), body.end());
}

void Sm4Builder::Else() {
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_ELSE);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1);
  code_.push_back(t0);
}

void Sm4Builder::EndIf() {
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_ENDIF);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1);
  code_.push_back(t0);
}

void Sm4Builder::Discard(bool test_nonzero, Operand cond) {
  std::vector<Token> body;
  EmitOperand(body, cond);
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DISCARD);
  t0 |= ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(
      test_nonzero ? D3D10_SB_INSTRUCTION_TEST_NONZERO : D3D10_SB_INSTRUCTION_TEST_ZERO);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  code_.push_back(t0);
  code_.insert(code_.end(), body.begin(), body.end());
}

void Sm4Builder::Ret() {
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RET);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1);
  code_.push_back(t0);
}

void Sm4Builder::IAdd(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_IADD, {dst, a, b}); }
void Sm4Builder::ILt(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_ILT, {dst, a, b}); }
void Sm4Builder::IGe(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_IGE, {dst, a, b}); }
void Sm4Builder::IEq(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_IEQ, {dst, a, b}); }
void Sm4Builder::INe(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_INE, {dst, a, b}); }
void Sm4Builder::And(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_AND, {dst, a, b}); }
void Sm4Builder::UShr(Operand dst, Operand a, Operand b) { EmitSimple(D3D10_SB_OPCODE_USHR, {dst, a, b}); }
void Sm4Builder::Ftoi(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_FTOI, {dst, src}); }
void Sm4Builder::Itof(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_ITOF, {dst, src}); }
void Sm4Builder::RoundNe(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_ROUND_NE, {dst, src}); }
void Sm4Builder::RoundZ(Operand dst, Operand src) { EmitSimple(D3D10_SB_OPCODE_ROUND_Z, {dst, src}); }

void Sm4Builder::Loop() {
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_LOOP);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1);
  code_.push_back(t0);
}

void Sm4Builder::EndLoop() {
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_ENDLOOP);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1);
  code_.push_back(t0);
}

void Sm4Builder::Break() {
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_BREAK);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1);
  code_.push_back(t0);
}

void Sm4Builder::BreakC(bool test_nonzero, Operand cond) {
  std::vector<Token> body;
  EmitOperand(body, cond);
  Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_BREAKC);
  t0 |= ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(
      test_nonzero ? D3D10_SB_INSTRUCTION_TEST_NONZERO : D3D10_SB_INSTRUCTION_TEST_ZERO);
  t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH((UINT)(1 + body.size()));
  code_.push_back(t0);
  code_.insert(code_.end(), body.begin(), body.end());
}

// ---------------------------------------------------------------------------
// DXBC container assembly
// ---------------------------------------------------------------------------

namespace {

D3D_REGISTER_COMPONENT_TYPE ComponentType(bool is_float) {
  return is_float ? D3D10_REGISTER_COMPONENT_FLOAT32 : D3D10_REGISTER_COMPONENT_UINT32;
}

// Builds an ISGN/OSGN blob matching the on-disk layout CSignatureParser::
// ReadSignature4 expects: a small header, one fixed 24-byte record per
// parameter, then a string table of NUL-terminated semantic names.
std::vector<uint8_t> BuildSignatureBlob(const std::vector<Signature> &params) {
  struct Record {
    uint32_t semantic_name_offset;
    uint32_t semantic_index;
    uint32_t system_value;   // D3D10_NAME
    uint32_t component_type; // D3D10_REGISTER_COMPONENT_TYPE
    uint32_t reg;
    uint8_t mask;
    uint8_t rw_mask;
    uint16_t _pad = 0;
  };
  static_assert(sizeof(Record) == 24, "signature record must be 24 bytes");

  std::vector<uint8_t> out;
  uint32_t header[2] = {(uint32_t)params.size(), 8};
  out.resize(8);
  memcpy(out.data(), header, 8);

  std::vector<Record> records(params.size());
  size_t string_offset = 8 + records.size() * sizeof(Record);
  std::string strings;
  for (size_t i = 0; i < params.size(); i++) {
    records[i].semantic_name_offset = (uint32_t)(string_offset + strings.size());
    records[i].semantic_index = params[i].semantic_index;
    records[i].system_value = D3D10_NAME_UNDEFINED;
    records[i].component_type = ComponentType(params[i].is_float);
    records[i].reg = params[i].reg;
    records[i].mask = params[i].mask;
    records[i].rw_mask = params[i].mask; // always-reads / never-writes: claim full mask
    strings += params[i].semantic_name;
    strings += '\0';
  }

  out.resize(string_offset);
  memcpy(out.data() + 8, records.data(), records.size() * sizeof(Record));
  out.insert(out.end(), strings.begin(), strings.end());
  return out;
}

void AppendBlob(std::vector<uint8_t> &out, DXBCFourCC fourcc, const std::vector<uint8_t> &data) {
  DXBCBlobHeader hdr{fourcc, (uint32_t)data.size()};
  size_t base = out.size();
  out.resize(base + sizeof(hdr) + data.size());
  memcpy(out.data() + base, &hdr, sizeof(hdr));
  memcpy(out.data() + base + sizeof(hdr), data.data(), data.size());
}

} // namespace

bool Sm4Builder::Finish(std::vector<uint8_t> &out_dxbc) {
  if (next_temp_ > 0) {
    Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_TEMPS);
    t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(2);
    decls_.push_back(t0);
    decls_.push_back(next_temp_);
  }

  {
    Token t0 = ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS);
    t0 |= ENCODE_D3D10_SB_GLOBAL_FLAGS(D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED);
    t0 |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1);
    // Global flags must appear before any other declaration.
    decls_.insert(decls_.begin(), t0);
  }

  std::vector<Token> program;
  program.push_back(ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(
      stage_ == Stage::Vertex ? D3D10_SB_VERTEX_SHADER : D3D10_SB_PIXEL_SHADER, 4, 0));
  program.push_back(0); // length, patched below
  program.insert(program.end(), decls_.begin(), decls_.end());
  program.insert(program.end(), code_.begin(), code_.end());
  if (code_.empty() || DECODE_D3D10_SB_OPCODE_TYPE(code_.back()) != D3D10_SB_OPCODE_RET)
    program.push_back(ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_RET) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1));
  program[1] = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_LENGTH((UINT)program.size());

  std::vector<uint8_t> shdr(program.size() * sizeof(Token));
  memcpy(shdr.data(), program.data(), shdr.size());

  /* A signature lists its registers in nondecreasing order -- the parser
   * rejects anything else -- while declarations come in whatever order the
   * shader needed them. */
  auto by_register = [](const Signature &a, const Signature &b) { return a.reg < b.reg; };
  std::stable_sort(inputs_.begin(), inputs_.end(), by_register);
  std::stable_sort(outputs_.begin(), outputs_.end(), by_register);
  auto isgn = BuildSignatureBlob(inputs_);
  auto osgn = BuildSignatureBlob(outputs_);

  std::vector<std::vector<uint8_t>> blobs = {isgn, osgn, shdr};
  static const DXBCFourCC kinds[] = {DXBC_InputSignature, DXBC_OutputSignature, DXBC_GenericShader};

  DXBCHeader header{};
  header.DXBCHeaderFourCC = DXBC_FOURCC_NAME;
  header.Version = {DXBC_MAJOR_VERSION, DXBC_MINOR_VERSION};
  header.BlobCount = (uint32_t)blobs.size();
  // header.Hash is left zeroed (default-initialized above) - ReadDXBC never verifies it.

  size_t index_size = blobs.size() * sizeof(uint32_t);
  size_t offset = sizeof(DXBCHeader) + index_size;
  std::vector<uint32_t> blob_offsets(blobs.size());
  for (size_t i = 0; i < blobs.size(); i++) {
    blob_offsets[i] = (uint32_t)offset;
    offset += sizeof(DXBCBlobHeader) + blobs[i].size();
  }
  header.ContainerSizeInBytes = (uint32_t)offset;

  out_dxbc.clear();
  out_dxbc.resize(sizeof(DXBCHeader));
  memcpy(out_dxbc.data(), &header, sizeof(header));
  out_dxbc.insert(out_dxbc.end(), reinterpret_cast<uint8_t *>(blob_offsets.data()),
                  reinterpret_cast<uint8_t *>(blob_offsets.data()) + index_size);
  for (size_t i = 0; i < blobs.size(); i++)
    AppendBlob(out_dxbc, kinds[i], blobs[i]);

  return true;
}

} // namespace dxmt::sm4
