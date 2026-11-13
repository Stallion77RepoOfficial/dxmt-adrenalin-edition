#include "sm3_translate.hpp"
#include "thread.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>

using namespace dxmt::sm4;

namespace dxmt::sm3 {

namespace {

// Canonical SM4 semantic name for a D3D9 vertex declaration usage. Shared
// convention between this translator's DCL_INPUT/DCL_OUTPUT emission and
// d3d9_vertex_declaration.cpp's D3D11_INPUT_ELEMENT_DESC construction - only
// the two of them need to agree, real D3D9 apps never see these names.
std::string UsageSemanticName(D3DDECLUSAGE usage) {
  switch (usage) {
  case D3DDECLUSAGE_POSITION: case D3DDECLUSAGE_POSITIONT: return "POSITION";
  case D3DDECLUSAGE_BLENDWEIGHT: return "BLENDWEIGHT";
  case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
  case D3DDECLUSAGE_NORMAL: return "NORMAL";
  case D3DDECLUSAGE_PSIZE: return "PSIZE";
  case D3DDECLUSAGE_TEXCOORD: return "TEXCOORD";
  case D3DDECLUSAGE_TANGENT: return "TANGENT";
  case D3DDECLUSAGE_BINORMAL: return "BINORMAL";
  case D3DDECLUSAGE_TESSFACTOR: return "TESSFACTOR";
  case D3DDECLUSAGE_COLOR: return "COLOR";
  case D3DDECLUSAGE_FOG: return "FOG";
  case D3DDECLUSAGE_DEPTH: return "DEPTH";
  case D3DDECLUSAGE_SAMPLE: return "SAMPLE";
  default: return "TEXCOORD";
  }
}

/* Picks components of a source operand that already carries the
 * instruction's swizzle. WithSwizzle replaces a swizzle outright, so using it
 * to read "src.y" of a source written as r1.zyxw would read r1.y instead of
 * r1.y-through-the-swizzle (r1.y here, but r1.x for .zxyw). An immediate --
 * a def'd constant -- has its swizzle folded into the values already, so its
 * values are permuted instead. */
Operand Swz(Operand o, uint8_t x, uint8_t y, uint8_t z, uint8_t w) {
  const uint8_t select[4] = {x, y, z, w};
  if (o.type == microsoft::D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
    if (o.imm_is_scalar)
      return o;
    float v[4];
    for (int i = 0; i < 4; i++)
      v[i] = o.imm[select[i]];
    for (int i = 0; i < 4; i++)
      o.imm[i] = v[i];
    return o;
  }
  if (!o.is_swizzle)
    return WithSwizzle(o, x, y, z, w);
  uint8_t s[4];
  for (int i = 0; i < 4; i++)
    s[i] = o.swizzle[select[i]];
  return WithSwizzle(o, s[0], s[1], s[2], s[3]);
}

/* D3D9's scalar instructions (rcp, rsq, exp, log, ...) read one component:
 * the one the source swizzle routes to .w. SM2+ requires a replicate swizzle
 * there, so this is the requested component; for vs_1_x source written
 * without one it is .w, as the D3D9 runtime defines. SM4's versions of these
 * work per component instead, so the component is replicated explicitly. */
Operand ScalarSrc(Operand o) { return Swz(o, 3, 3, 3, 3); }

struct Translator {
  const ShaderModule &mod;
  const TranslateOptions &opts;
  Sm4Builder b;
  TranslationResult result;

  uint32_t max_d3d9_temp = 0;
  std::map<uint32_t, std::array<float, 4>> def_f;
  std::map<uint32_t, std::array<int32_t, 4>> def_i;
  std::map<uint32_t, bool> def_b;
  // vs dcl'd input register -> (D3DDECLUSAGE, usage index)
  std::map<uint32_t, std::pair<uint8_t, uint8_t>> vs_input_usage;
  // vs3.0 dcl'd o# register -> (D3DDECLUSAGE, usage index)
  std::map<uint32_t, std::pair<uint8_t, uint8_t>> output_dcl_;
  // ps3.0 dcl'd v# register -> semantic; earlier models never declare a usage
  // for their inputs (v# is always COLOR, t# always TEXCOORD).
  std::map<uint32_t, std::pair<uint8_t, uint8_t>> ps_input_dcl_;
  std::set<uint32_t> samplers_used;
  std::map<uint32_t, uint32_t> sampler_dcl_type_; // s# -> D3DSAMPLER_TEXTURE_TYPE from its dcl
  bool has_error = false;
  std::string error;
  int if_depth = 0;

  // Facts Scan() gathers that decide the up-front declarations.
  bool uses_api_bool = false;   // b# read that no defb covers -> cb1
  bool uses_api_int = false;    // loop/rep i# that no defi covers -> cb1
  bool has_rel_const = false;   // c[a0.x+n]/c[aL+n] -> cb0 must span all 256 registers
  bool alpha_test = false;                 // pixel shader alpha-test epilogue requested
  bool fog = false;                        // pixel shader fog epilogue requested
  std::optional<uint32_t> fog_input_;      // the FOG input register it reads

  // a0 lives in a dedicated integer temp (SM4 has no address register).
  std::optional<uint32_t> addr_temp_;
  uint32_t AddrTemp() {
    if (!addr_temp_) addr_temp_ = ScratchTemp();
    return *addr_temp_;
  }
  // Alpha test needs to re-read the final color, but SM4 outputs are
  // write-only - oC0 is redirected here and copied out in the epilogue.
  std::optional<uint32_t> color_temp_;
  // ps_1_x t# registers are both the interpolated texture coordinate and a
  // writable register: once a texture instruction writes tN, later reads see
  // the written value, not the interpolator. Each written tN therefore gets a
  // temp, seeded from its interpolator before the first instruction so reads
  // before any write still see the coordinate.
  std::map<uint32_t, uint32_t> ps_texreg_temp_;
  std::set<uint32_t> ps_texregs_written_;
  /* A pending indexed output write, dispatched after the instruction that
   * produced the value. */
  std::optional<DstParam> indexed_output_;
  std::optional<uint32_t> indexed_output_temp_;

  void FlushIndexedOutput() {
    if (!indexed_output_ || !indexed_output_temp_)
      return;
    DstParam target = *indexed_output_;
    indexed_output_.reset();

    /* The index is aL plus the register's own number. */
    const LoopCtx *loop = nullptr;
    for (auto it = loop_stack_.rbegin(); it != loop_stack_.rend(); ++it)
      if (it->has_al) { loop = &*it; break; }
    if (!loop) {
      Fail("indexed output write outside a loop");
      return;
    }

    Operand value = TempSwizzle(*indexed_output_temp_, 0, 1, 2, 3);
    uint32_t match = ScratchTemp();
    for (uint32_t candidate = 0; candidate < kMaxIndexedOutputs; candidate++) {
      uint32_t reg = OutputLinkReg(target.reg.type, candidate);
      if (reg == kLinkNone || !(reg == 0 || declared_outputs_.count(reg)))
        continue;
      uint8_t mask = target.write_mask & (reg == 0 ? 0b1111 : LinkMask(reg));
      if (!mask)
        continue;
      b.IEq(WithMask(Temp(match), 0b0001),
            TempSwizzle(loop->ctr_temp, 1, 1, 1, 1), ImmI1(int32_t(candidate - target.reg.index)));
      b.If(true, TempSwizzle(match, 0, 0, 0, 0));
      if (color1_temp_ && reg == kLinkColor0 + 1)
        b.Mov(WithMask(Temp(*color1_temp_), mask), value);
      else if (psize_temp_ && reg == kLinkPointSize)
        b.Mov(WithMask(Temp(*psize_temp_), mask), value);
      else if (pos_temp_ && reg == 0)
        b.Mov(WithMask(Temp(*pos_temp_), mask), value);
      else
        b.Mov(WithMask(Output(reg), mask), value);
      b.EndIf();
    }
  }

  static constexpr uint32_t kMaxIndexedOutputs = 12;

  /* D3D9 subroutines are inlined rather than emitted as real functions.
   *
   * They are non-recursive and the call depth is capped at 4 by the shader
   * models themselves, so inlining always terminates and avoids a DXBC function
   * table. The depth counter is still enforced: a malformed shader claiming a
   * cycle must fail rather than expand forever. */
  static constexpr uint32_t kMaxCallDepth = 4;
  std::map<uint32_t, std::pair<size_t, size_t>> subroutines_; // label -> [first, last) instruction
  uint32_t call_depth_ = 0;

  void EmitSubroutine(uint32_t label) {
    auto it = subroutines_.find(label);
    if (it == subroutines_.end()) {
      Fail("call to an undefined label");
      return;
    }
    if (call_depth_ >= kMaxCallDepth) {
      Fail("subroutine call nested deeper than the shader model allows");
      return;
    }
    call_depth_++;
    for (size_t i = it->second.first; i < it->second.second && !has_error; i++)
      Emit(mod.instructions[i]);
    call_depth_--;
  }

  // The ps_1_x 3x2/3x3 matrix texture instructions span several instructions:
  // each *PAD contributes one row of the matrix, and the *TEX/*SPEC/*DEPTH that
  // follows contributes the last row and consumes the result. The rows
  // accumulate here.
  std::optional<uint32_t> texm3_temp_;
  uint32_t texm3_row_ = 0;
  uint32_t Texm3Temp() {
    if (!texm3_temp_) texm3_temp_ = ScratchTemp();
    return *texm3_temp_;
  }
  // dot3 of a stage's interpolated coordinate with the source, into one row.
  void Texm3Row(uint32_t stage, const Operand &source, uint32_t row) {
    static const uint8_t kMask[3] = {0b0001, 0b0010, 0b0100};
    b.Dp3(WithMask(Temp(Texm3Temp()), kMask[row]), TexcoordIn(stage), source);
  }

  /* ps_3_0's vPos and vFace.
   *
   * vPos is the pixel's screen position, which SV_Position already gives at
   * input register 0. vFace is a float that D3D9 defines as positive when the
   * triangle faces the viewer and negative when it does not, while
   * SV_IsFrontFace is a 0/~0 boolean - so it is converted rather than read
   * straight through. */
  std::optional<uint32_t> face_input_;
  std::optional<uint32_t> face_temp_;
  bool uses_vpos_ = false;
  bool uses_vface_ = false;

  // p0 lives in a temp holding a per-component boolean mask, as SM4 has no
  // predicate register file.
  std::optional<uint32_t> predicate_temp_;
  uint32_t PredicateTemp() {
    if (!predicate_temp_) predicate_temp_ = ScratchTemp();
    return *predicate_temp_;
  }
  // While a predicated instruction is being lowered, its result is written
  // here instead of the real destination and merged afterwards.
  std::optional<uint32_t> predication_scratch_;
  /* A destination shift (_x2/_x4/_d2 ...) on a register SM4 cannot read back.
   * The scale is applied to the value on its way out instead of to the
   * register afterwards, so the instruction writes here and the epilogue
   * scales and stores. */
  std::optional<uint32_t> shift_scratch_;
  std::optional<Operand> shift_dst_;
  uint8_t shift_mask_ = 0;
  uint8_t predication_mask_ = 0xf;

  // Where the device puts each stage's bump environment state in cb1.
  static uint32_t BumpMatrixElement(uint32_t stage) {
    return kExtraCBBumpBase + std::min(stage, kExtraCBBumpStages - 1) * kExtraCBBumpStride;
  }
  static uint32_t BumpLuminanceElement(uint32_t stage) { return BumpMatrixElement(stage) + 1; }

  uint32_t TexRegTemp(uint32_t index) {
    auto it = ps_texreg_temp_.find(index);
    return it != ps_texreg_temp_.end() ? it->second : PsInputRegT(index);
  }

  // Active loop/rep nest. ctr_temp holds (x=iterations remaining, y=aL value,
  // z=aL step) as raw integers.
  struct LoopCtx {
    uint32_t ctr_temp;
    bool has_al;
  };
  std::vector<LoopCtx> loop_stack_;

  Translator(const ShaderModule &m, const TranslateOptions &o)
      : mod(m), opts(o), b(m.is_pixel_shader ? Sm4Builder::Stage::Pixel : Sm4Builder::Stage::Vertex) {}

  void Fail(const std::string &msg) {
    if (!has_error) { has_error = true; error = msg; }
  }

  // --- pass 1: scan for temp count, DEF constants, DCL outputs --------------
  void Scan() {
    /* Subroutine bodies run from the label to its ret. */
    for (size_t i = 0; i < mod.instructions.size(); i++) {
      if (mod.instructions[i].opcode != D3DSIO_LABEL)
        continue;
      if (mod.instructions[i].src.empty()) {
        Fail("label without an identifier");
        return;
      }
      size_t body = i + 1, end = body;
      while (end < mod.instructions.size() && mod.instructions[end].opcode != D3DSIO_RET)
        end++;
      subroutines_[mod.instructions[i].src[0].reg.index] = {body, end};
    }

    for (auto &inst : mod.instructions) {
      if (inst.dst && inst.dst->reg.type == D3DSPR_TEMP)
        max_d3d9_temp = std::max(max_d3d9_temp, inst.dst->reg.index + 1);
      // texcoord's "destination" names the interpolator, not a written value.
      if (inst.dst && inst.dst->reg.type == D3DSPR_TEXTURE && mod.is_pixel_shader &&
          inst.opcode != D3DSIO_TEXCOORD)
        ps_texregs_written_.insert(inst.dst->reg.index);
      for (auto &s : inst.src)
        if (s.reg.type == D3DSPR_TEMP)
          max_d3d9_temp = std::max(max_d3d9_temp, s.reg.index + 1);

      for (auto &s : inst.src) {
        if (s.reg.type == D3DSPR_MISCTYPE) {
          if (s.reg.index == 0) uses_vpos_ = true;
          if (s.reg.index == 1) uses_vface_ = true;
        }
        if (s.reg.relative_addressing && s.reg.type == D3DSPR_CONST)
          has_rel_const = true;
        if (s.reg.type == D3DSPR_CONSTBOOL && !def_b.count(s.reg.index)) {
          uses_api_bool = true;
          result.bool_constants_used = std::max(result.bool_constants_used, s.reg.index + 1);
        }
      }
      if (inst.opcode == D3DSIO_LOOP || inst.opcode == D3DSIO_REP) {
        size_t i_src = inst.opcode == D3DSIO_LOOP ? 1 : 0;
        if (inst.src.size() > i_src && inst.src[i_src].reg.type == D3DSPR_CONSTINT &&
            !def_i.count(inst.src[i_src].reg.index)) {
          uses_api_int = true;
          result.int_constants_used = std::max(result.int_constants_used, inst.src[i_src].reg.index + 1);
        }
      }

      switch (inst.opcode) {
      case D3DSIO_DEF:
        def_f[inst.dst->reg.index] = {inst.def_f[0], inst.def_f[1], inst.def_f[2], inst.def_f[3]};
        break;
      case D3DSIO_DEFI:
        def_i[inst.dst->reg.index] = {inst.def_i[0], inst.def_i[1], inst.def_i[2], inst.def_i[3]};
        break;
      case D3DSIO_DEFB:
        def_b[inst.dst->reg.index] = inst.def_b;
        break;
      case D3DSIO_DCL: {
        if (inst.dst->reg.type == D3DSPR_OUTPUT || inst.dst->reg.type == D3DSPR_TEXCRDOUT) {
          uint32_t usage = inst.control & D3DSP_DCL_USAGE_MASK;
          uint32_t usage_index = (inst.control & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT;
          output_dcl_[inst.dst->reg.index] = {(uint8_t)usage, (uint8_t)usage_index};
        } else if (!mod.is_pixel_shader && inst.dst->reg.type == D3DSPR_INPUT) {
          uint32_t usage = inst.control & D3DSP_DCL_USAGE_MASK;
          uint32_t usage_index = (inst.control & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT;
          vs_input_usage[inst.dst->reg.index] = {(uint8_t)usage, (uint8_t)usage_index};
        } else if (mod.is_pixel_shader && inst.dst->reg.type == D3DSPR_INPUT && mod.major_version >= 3) {
          // ps<3.0 also emits `dcl v#` but with a meaningless usage of 0
          // (POSITION); only ps3.0 carries real input semantics.
          uint32_t usage = inst.control & D3DSP_DCL_USAGE_MASK;
          uint32_t usage_index = (inst.control & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT;
          // The position reaches a pixel shader as vPos; the runtime refuses
          // position0 as an input semantic (position1 and up are ordinary).
          if (usage == D3DDECLUSAGE_POSITION && usage_index == 0) {
            Fail("ps_3_0 input declared as position0");
            return;
          }
          ps_input_dcl_[inst.dst->reg.index] = {(uint8_t)usage, (uint8_t)usage_index};
        } else if (inst.dst->reg.type == D3DSPR_SAMPLER) {
          samplers_used.insert(inst.dst->reg.index);
          sampler_dcl_type_[inst.dst->reg.index] = inst.control & D3DSP_TEXTURETYPE_MASK;
        }
        break;
      }
      case D3DSIO_TEXLDL:
      case D3DSIO_TEXLDD:
        if (inst.src.size() >= 2 && inst.src[1].reg.type == D3DSPR_SAMPLER)
          samplers_used.insert(inst.src[1].reg.index);
        break;
      case D3DSIO_TEX:
        if (inst.src.size() >= 2 && inst.src[1].reg.type == D3DSPR_SAMPLER)
          samplers_used.insert(inst.src[1].reg.index); // ps>=2.0: dst, coord, sampler
        else
          samplers_used.insert(inst.dst->reg.index); // ps<2.0: dst register number selects the stage
        break;
      case D3DSIO_TEXREG2AR:
      case D3DSIO_TEXREG2GB:
      case D3DSIO_TEXREG2RGB:
      case D3DSIO_TEXDP3TEX:
      case D3DSIO_TEXBEM:
      case D3DSIO_TEXBEML:
      case D3DSIO_TEXM3x2TEX:
      case D3DSIO_TEXM3x3TEX:
      case D3DSIO_TEXM3x3SPEC:
      case D3DSIO_TEXM3x3VSPEC:
        // ps_1_x: the destination register number is the texture stage.
        samplers_used.insert(inst.dst->reg.index);
        break;
      default:
        break;
      }
    }
  }

  // --- register/operand resolution -------------------------------------------
  uint32_t next_scratch_temp = 0;
  uint32_t ScratchTemp() { return max_d3d9_temp + next_scratch_temp++; }

  // Vertex-shader v# input registers, discovered lazily from DCL or first use
  // (SM1.x vertex shaders reference v0..v15 without ever declaring them).
  std::set<uint32_t> vs_inputs_seen;
  // v# registers read through a temp holding their converted value
  // (TranslateOptions::input_conversions).
  std::map<uint32_t, uint32_t> input_temp_;

  // Pixel shader: D3D9 input register -> the link register its varying
  // arrives in. v# is diffuse/specular below ps_3_0 and whatever its dcl says
  // from there on; t# (ps<3.0) is always the texture coordinate of its stage.
  // A v# with no varying behind it reads as zero.
  std::map<uint32_t, uint32_t> ps_vinput_reg_;
  uint32_t PsInputRegV(uint32_t d3d9_index) {
    auto it = ps_vinput_reg_.find(d3d9_index);
    return it != ps_vinput_reg_.end() ? it->second : kLinkNone;
  }
  static uint32_t PsInputRegT(uint32_t d3d9_index) { return kLinkTexcoord0 + d3d9_index; }
  void DeclareInput(uint32_t reg) {
    if (reg == kLinkNone || (result.link_inputs & LinkBit(reg)))
      return;
    DeclareLinkIO(b, reg);
    result.link_inputs |= LinkBit(reg);
  }

  std::optional<Operand> ResolveSrcBase(const SrcParam &s) {
    if (s.reg.relative_addressing && s.reg.type != D3DSPR_CONST) {
      Fail("relative addressing is only supported on float constants (c#)");
      return std::nullopt;
    }
    switch (s.reg.type) {
    case D3DSPR_TEMP:
      return TempSwizzle(s.reg.index, s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
    case D3DSPR_CONST: {
      if (s.reg.relative_addressing) {
        if (mod.is_pixel_shader) {
          Fail("pixel shaders cannot address constants dynamically");
          return std::nullopt;
        }
        uint32_t rel_temp;
        uint8_t rel_comp;
        if (s.reg.rel_type == D3DSPR_LOOP) {
          const LoopCtx *al = nullptr;
          for (auto it2 = loop_stack_.rbegin(); it2 != loop_stack_.rend(); ++it2)
            if (it2->has_al) { al = &*it2; break; }
          if (!al) {
            Fail("aL-relative addressing outside of a loop");
            return std::nullopt;
          }
          rel_temp = al->ctr_temp;
          rel_comp = 1; // aL value lives in .y of the counter temp
        } else {
          rel_temp = AddrTemp();
          rel_comp = s.reg.rel_component;
        }
        return ConstantBufferRel(0, s.reg.index, rel_temp, rel_comp,
                                 s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
      }
      auto it = def_f.find(s.reg.index);
      if (it != def_f.end()) {
        const auto &v = it->second;
        float c[4] = {v[s.swizzle[0]], v[s.swizzle[1]], v[s.swizzle[2]], v[s.swizzle[3]]};
        return Imm4(c[0], c[1], c[2], c[3]);
      }
      result.float_constants_used = std::max(result.float_constants_used, s.reg.index + 1);
      return WithSwizzle(ConstantBuffer(0, s.reg.index), s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
    }
    case D3DSPR_PREDICATE:
      return TempSwizzle(PredicateTemp(), s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
    case D3DSPR_MISCTYPE:
      /* 0 is vPos, 1 is vFace. */
      if (s.reg.index == 0) {
        if (!mod.is_pixel_shader) {
          Fail("vPos is only available to pixel shaders");
          return std::nullopt;
        }
        return Input(0, s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
      }
      if (s.reg.index == 1) {
        if (!face_temp_) {
          Fail("vFace used but not declared");
          return std::nullopt;
        }
        return TempSwizzle(*face_temp_, s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
      }
      Fail("unknown misc register " + std::to_string((int)s.reg.index));
      return std::nullopt;
    case D3DSPR_INPUT: {
      if (!mod.is_pixel_shader) {
        vs_inputs_seen.insert(s.reg.index);
        auto converted = input_temp_.find(s.reg.index);
        if (converted != input_temp_.end())
          return TempSwizzle(converted->second, s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
        return Input(s.reg.index, s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
      }
      uint32_t reg = PsInputRegV(s.reg.index);
      if (reg == kLinkNone)
        return Imm4(0, 0, 0, 0);
      if (reg == kPointCoordInput)
        return TempSwizzle(*pointcoord_temp_, s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
      return Input(reg, s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
    }
    case D3DSPR_TEXTURE: // == D3DSPR_ADDR; only D3DSPR_TEXTURE is meaningful as a *source* register (ps)
      if (!mod.is_pixel_shader) {
        /* a0 is write-only in D3D9: mova writes it and c[a0.x + n] reads it as
         * an index. It is never a value an instruction can read. */
        Fail("the address register a0 cannot be read as a source");
        return std::nullopt;
      }
      // ps < 3.0 texture coordinate registers were bound as ordinary inputs,
      // unless a texture instruction writes this one, in which case it lives in
      // a temp for the whole shader.
      if (ps_texreg_temp_.count(s.reg.index))
        return TempSwizzle(ps_texreg_temp_[s.reg.index], s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
      return WithSwizzle(TexcoordIn(s.reg.index), s.swizzle[0], s.swizzle[1], s.swizzle[2], s.swizzle[3]);
    case D3DSPR_CONSTBOOL: {
      auto it = def_b.find(s.reg.index);
      if (it != def_b.end()) {
        float v = it->second ? 1.0f : 0.0f;
        return Imm4(v, v, v, v);
      }
      // API-set bool (SetVertexShaderConstantB/SetPixelShaderConstantB): the
      // device uploads 0/1 into the extra-constants buffer. b# is only legal
      // as an if/callnz condition, so a raw-bits nonzero test is sufficient.
      return WithSwizzle(ConstantBuffer(1, kExtraCBBoolBase + s.reg.index), 0, 0, 0, 0);
    }
    default:
      Fail("unsupported source register type " + std::to_string((int)s.reg.type));
      return std::nullopt;
    }
  }

  std::optional<Operand> ResolveSrc(const SrcParam &s) {
    auto base = ResolveSrcBase(s);
    if (!base) return std::nullopt;
    switch (s.modifier) {
    case D3DSPSM_NONE: return base;
    case D3DSPSM_NEG: return Neg(*base);
    case D3DSPSM_ABS: return Abs(*base);
    case D3DSPSM_ABSNEG: return Neg(Abs(*base));
    case D3DSPSM_BIAS: case D3DSPSM_BIASNEG: {
      uint32_t t = ScratchTemp();
      b.Add(Temp(t), *base, Imm4(-0.5f, -0.5f, -0.5f, -0.5f));
      return s.modifier == D3DSPSM_BIASNEG ? Neg(TempSwizzle(t, 0, 1, 2, 3)) : Operand(TempSwizzle(t, 0, 1, 2, 3));
    }
    case D3DSPSM_X2: case D3DSPSM_X2NEG: {
      uint32_t t = ScratchTemp();
      b.Add(Temp(t), *base, *base);
      return s.modifier == D3DSPSM_X2NEG ? Neg(TempSwizzle(t, 0, 1, 2, 3)) : Operand(TempSwizzle(t, 0, 1, 2, 3));
    }
    case D3DSPSM_SIGN: case D3DSPSM_SIGNNEG: {
      /* _bx2: signed scaling, (x - 0.5) * 2. */
      uint32_t t = ScratchTemp();
      b.Mad(Temp(t), *base, Imm4(2, 2, 2, 2), Imm4(-1, -1, -1, -1));
      return s.modifier == D3DSPSM_SIGNNEG ? Neg(TempSwizzle(t, 0, 1, 2, 3)) : Operand(TempSwizzle(t, 0, 1, 2, 3));
    }
    case D3DSPSM_COMP: {
      /* Complement: 1 - x. */
      uint32_t t = ScratchTemp();
      b.Add(Temp(t), Imm4(1, 1, 1, 1), Neg(*base));
      return TempSwizzle(t, 0, 1, 2, 3);
    }
    case D3DSPSM_DZ: case D3DSPSM_DW: {
      /* Projective divide of .xy by .z or .w. The remaining components are
       * undefined in D3D9, so they are left as the divided value rather than
       * being given a made-up meaning. */
      uint32_t t = ScratchTemp();
      uint8_t component = s.modifier == D3DSPSM_DZ ? 2 : 3;
      b.Rcp(WithMask(Temp(t), 0b1000), WithSwizzle(*base, component, component, component, component));
      b.Mul(WithMask(Temp(t), 0b0111), *base, TempSwizzle(t, 3, 3, 3, 3));
      return TempSwizzle(t, 0, 1, 2, 3);
    }
    default:
      Fail("unsupported source modifier");
      return std::nullopt;
    }
  }

  // Destination write mask as sm4 bits (bit0=x .. bit3=w) - identical layout
  // to D3D9's, no conversion needed.
  std::optional<Operand> ResolveDst(const DstParam &d, uint8_t *out_mask = nullptr) {
    if (out_mask) *out_mask = d.write_mask;
    // A ps_1_x shift modifier is applied by a separate mul after the main op;
    // D3D9 shifts before saturating, so saturation moves onto that mul too.
    auto sat = [&](Operand o) { return (d.saturate && d.shift_scale == 0) ? Saturate(o) : o; };

    if (d.reg.relative_addressing) {
      /* vs_3_0 can index its output registers with aL. There are at most a
       * dozen of them, so the write is expanded into a comparison chain rather
       * than needing SM4 indexable outputs. Nothing else in D3D9 may be
       * relatively addressed as a destination. */
      if (d.reg.type != D3DSPR_TEXCRDOUT && d.reg.type != D3DSPR_ATTROUT) {
        Fail("only output registers may be relatively addressed as a destination");
        return std::nullopt;
      }
      indexed_output_ = d;
      /* The value is computed into a temp and dispatched afterwards. */
      if (!indexed_output_temp_)
        indexed_output_temp_ = ScratchTemp();
      return sat(WithMask(Temp(*indexed_output_temp_), d.write_mask));
    }
    if (predication_scratch_)
      return sat(WithMask(Temp(*predication_scratch_), d.write_mask));
    if (shift_scratch_)
      return sat(WithMask(Temp(*shift_scratch_), d.write_mask));

    switch (d.reg.type) {
    case D3DSPR_PREDICATE:
      return WithMask(Temp(PredicateTemp()), d.write_mask);
    case D3DSPR_TEMP:
      return sat(WithMask(Temp(d.reg.index), d.write_mask));
    case D3DSPR_RASTOUT:
    case D3DSPR_ATTROUT:
    case D3DSPR_TEXCRDOUT: { // == D3DSPR_OUTPUT
      uint32_t reg = OutputLinkReg(d.reg.type, d.reg.index);
      if (reg == kLinkFog)
        result.writes_fog = true;
      /* oFog and oPts are scalars, but vs_1_x writes them with a full mask. */
      uint8_t mask = reg == kLinkNone ? 0 : d.write_mask & (reg == 0 ? 0b1111 : LinkMask(reg));
      if (!mask)
        return WithMask(Temp(SinkTemp()), d.write_mask);
      if (out_mask) *out_mask = mask;
      Operand o = color1_temp_ && reg == kLinkColor0 + 1 ? WithMask(Temp(*color1_temp_), mask)
                  : psize_temp_ && reg == kLinkPointSize   ? WithMask(Temp(*psize_temp_), mask)
                  : pos_temp_ && reg == 0                  ? WithMask(Temp(*pos_temp_), mask)
                                                           : WithMask(Output(reg), mask);
      /* Below vs_3_0 the diffuse and specular outputs are clamped to [0, 1]. */
      if (d.reg.type == D3DSPR_ATTROUT && mod.major_version < 3)
        return Saturate(o);
      return sat(o);
    }
    case D3DSPR_COLOROUT:
      // With the alpha-test epilogue enabled the final color has to be re-read,
      // but SM4 outputs are write-only - route oC0 through a temp instead.
      if (color_temp_ && d.reg.index == 0)
        return sat(WithMask(Temp(*color_temp_), d.write_mask));
      return sat(WithMask(Output(d.reg.index), d.write_mask)); // reg N == DCL_OUTPUT_TARGET N in EmitOutputDecls
    case D3DSPR_DEPTHOUT:
      return sat(OutputDepth());
    case D3DSPR_TEXTURE: // == D3DSPR_ADDR; only a pixel shader writes t#
      if (mod.is_pixel_shader && ps_texreg_temp_.count(d.reg.index))
        return sat(WithMask(Temp(ps_texreg_temp_[d.reg.index]), d.write_mask));
      Fail("write to a texture register outside a pixel shader");
      return std::nullopt;
    default:
      Fail("unsupported destination register type " + std::to_string((int)d.reg.type));
      return std::nullopt;
    }
  }

  // --- vertex shader outputs -----------------------------------------------
  // Every output lands in the link register of its semantic: vs<3.0 names the
  // semantic by register type (oD#, oT#, oFog, oPts), vs_3_0 in its dcl. All
  // of them are declared up front and seeded with D3D9's value for a varying
  // nothing wrote before the first instruction runs, so a write only some
  // paths -- or only some components -- make still leaves a defined value.
  uint32_t OutputLinkReg(uint32_t type, uint32_t index) const {
    uint32_t reg = OutputLinkRegAnyPrimitive(type, index);
    return reg == kLinkPointSize && !opts.point_size ? kLinkNone : reg;
  }
  uint32_t OutputLinkRegAnyPrimitive(uint32_t type, uint32_t index) const {
    if (mod.major_version >= 3) {
      auto it = output_dcl_.find(index);
      uint8_t usage = it != output_dcl_.end() ? it->second.first : (uint8_t)D3DDECLUSAGE_TEXCOORD;
      uint8_t usage_index = it != output_dcl_.end() ? it->second.second : (uint8_t)index;
      if ((usage == D3DDECLUSAGE_POSITION || usage == D3DDECLUSAGE_POSITIONT) && usage_index == 0)
        return 0;
      return LinkRegister(usage, usage_index);
    }
    switch (type) {
    case D3DSPR_RASTOUT: return index == 0 ? 0 : index == 1 ? kLinkFog : kLinkPointSize;
    case D3DSPR_ATTROUT: return index < 2 ? kLinkColor0 + index : kLinkNone;
    case D3DSPR_TEXCRDOUT: return index < 8 ? kLinkTexcoord0 + index : kLinkNone;
    default: return kLinkNone;
    }
  }

  std::set<uint32_t> declared_outputs_;
  void DeclareOutput(uint32_t reg) {
    if (reg == 0 || reg == kLinkNone || !declared_outputs_.insert(reg).second)
      return;
    DeclareLinkIO(b, reg);
    result.link_outputs |= LinkBit(reg);
  }

  /* A vertex shader that leaves oFog unwritten hands the pixel stage its
   * specular alpha as the fog factor instead -- D3D9's rule -- when it writes
   * oD1 at all. Outputs cannot be read back, so oD1 is kept here then. */
  std::optional<uint32_t> color1_temp_;
  // oPts, kept to be clamped to the point size range on the way out.
  std::optional<uint32_t> psize_temp_;
  // The position, kept to receive the depth bias on the way out.
  std::optional<uint32_t> pos_temp_;
  /* Point sprites: the position within the point, (x, y, 0, 1), which every
   * texture coordinate the pixel shader reads becomes. */
  std::optional<uint32_t> pointcoord_input_;
  std::optional<uint32_t> pointcoord_temp_;
  static constexpr uint32_t kPointCoordInput = kLinkRegisterCount + 1; // ps_vinput_reg_ marker
  // 0 2D, 1 cube, 2 volume: from the sampler's dcl, else the bound texture.
  uint32_t SamplerDim(uint32_t s) const {
    auto it = sampler_dcl_type_.find(s);
    if (it != sampler_dcl_type_.end())
      return it->second == D3DSTT_CUBE ? 1 : it->second == D3DSTT_VOLUME ? 2 : 0;
    return s < 16 ? (opts.sampler_dims >> (s * 2)) & 3 : 0;
  }
  bool Shadow(uint32_t s) const {
    return mod.is_pixel_shader && s < 16 && ((opts.shadow_samplers >> s) & 1) && SamplerDim(s) == 0;
  }
  bool Fetch4(uint32_t s) const {
    return mod.is_pixel_shader && s < 16 && ((opts.fetch4_samplers >> s) & 1) && SamplerDim(s) == 0;
  }
  /* Fetch4 returns the footprint as (+u-v, -u+v, +u+v, -u-v) in D3D9's
   * channel order; gather4 has the same texels in (-u+v, +u+v, +u-v, -u-v).
   * And the footprint is a different one: AMD's starts at the texel the
   * coordinate falls in, where a bilinear one -- gather4's -- starts half a
   * texel before, so the coordinate moves on by half a texel. It replaces
   * level selection too, so every texture instruction reads the same
   * footprint. */
  bool EmitFetch4(Operand dst, Operand coord, uint32_t s) {
    if (!Fetch4(s))
      return false;
    uint32_t t = ScratchTemp();
    b.ResInfoRcp(WithMask(Temp(t), 0b0011), s);
    b.Mad(WithMask(Temp(t), 0b0011), TempSwizzle(t, 0, 1, 0, 0), Imm4(0.5f, 0.5f, 0, 0), coord);
    b.Gather4(Temp(t), TempSwizzle(t, 0, 1, 1, 1), s, s, (opts.fetch4_alpha >> s) & 1 ? 3 : 0);
    b.Mov(dst, TempSwizzle(t, 2, 0, 1, 3));
    return true;
  }
  /* Every texture read goes through these, so a depth texture compares
   * against the coordinate's z instead of returning the depth. The
   * comparison takes no bias or gradients; level 0 stands in for an explicit
   * level. */
  void SampleTex(Operand dst, Operand coord, uint32_t s) {
    if (EmitFetch4(dst, coord, s))
      return;
    if (Shadow(s))
      return b.SampleC(dst, coord, s, s, Swz(coord, 2, 2, 2, 2));
    b.Sample(dst, coord, s, s);
  }
  void SampleTexL(Operand dst, Operand coord, uint32_t s, Operand lod) {
    if (EmitFetch4(dst, coord, s))
      return;
    if (Shadow(s))
      return b.SampleCLz(dst, coord, s, s, Swz(coord, 2, 2, 2, 2));
    b.SampleL(dst, coord, s, s, lod);
  }
  void SampleTexB(Operand dst, Operand coord, uint32_t s, Operand bias) {
    if (EmitFetch4(dst, coord, s))
      return;
    if (Shadow(s))
      return b.SampleC(dst, coord, s, s, Swz(coord, 2, 2, 2, 2));
    b.SampleB(dst, coord, s, s, bias);
  }
  /* ps_2_x and up may swizzle the sampler register, which swizzles what the
   * lookup returns: sample into a scratch register when that is not .xyzw. */
  template <typename Lookup> void WithSamplerSwizzle(Operand dst, const SrcParam &sampler, Lookup lookup) {
    const uint8_t *sw = sampler.swizzle;
    if (sw[0] == 0 && sw[1] == 1 && sw[2] == 2 && sw[3] == 3)
      return lookup(dst);
    uint32_t t = ScratchTemp();
    lookup(Temp(t));
    b.Mov(dst, TempSwizzle(t, sw[0], sw[1], sw[2], sw[3]));
  }
  void SampleTexD(Operand dst, Operand coord, uint32_t s, Operand ddx, Operand ddy) {
    if (EmitFetch4(dst, coord, s))
      return;
    if (Shadow(s))
      return b.SampleC(dst, coord, s, s, Swz(coord, 2, 2, 2, 2));
    b.SampleD(dst, coord, s, s, ddx, ddy);
  }

  Operand TexcoordIn(uint32_t index) {
    if (pointcoord_temp_)
      return TempSwizzle(*pointcoord_temp_, 0, 1, 2, 3);
    return Input(PsInputRegT(index));
  }
  // Where writes to an output with no varying behind it go.
  std::optional<uint32_t> sink_temp_;
  uint32_t SinkTemp() {
    if (!sink_temp_) sink_temp_ = ScratchTemp();
    return *sink_temp_;
  }

  // --- instruction lowering ---------------------------------------------------
  void Emit(const Instruction &inst) {
    if (has_error) return;

    /* A predicated instruction only writes the components where the predicate
     * is set. The result is computed into a scratch temp and merged with movc,
     * which is exact per component - D3D9 predication is per component, not per
     * instruction, so an if/endif around the whole thing would be wrong
     * whenever the predicate swizzle is not replicated.
     *
     * That needs a readable destination. SM4 outputs are write-only, so a
     * predicated write straight to an output falls back to if/endif, which is
     * exact as long as the predicate is uniform across the components being
     * written - and a shader predicating an output per component is not
     * something the fallback can express, so it is refused instead. */
    std::optional<Operand> predicate;
    std::optional<Operand> predicated_dst;
    bool predicated_branch = false;
    if (inst.predicated) {
      if (!inst.predicate) {
        Fail("predicated instruction without a predicate operand");
        return;
      }
      predicate = ResolveSrcBase(*inst.predicate);
      if (!predicate) {
        Fail("unresolved predicate operand");
        return;
      }
      if (inst.predicate->modifier == D3DSPSM_NOT) {
        /* Invert by swapping the movc operands, or by testing zero. */
      } else if (inst.predicate->modifier != D3DSPSM_NONE) {
        Fail("unsupported predicate modifier");
        return;
      }

      auto real_dst = inst.dst ? ResolveDst(*inst.dst) : std::nullopt;
      if (!real_dst) {
        Fail("predicated instruction without a destination");
        return;
      }
      if (IsTempOperand(*real_dst)) {
        predicated_dst = real_dst;
        predication_scratch_ = ScratchTemp();
        predication_mask_ = inst.dst->write_mask;
      } else {
        predicated_branch = true;
        b.If(inst.predicate->modifier != D3DSPSM_NOT, *predicate);
      }
    }

    /* A shift on a destination SM4 can read back is handled after the write.
     * One it cannot -- an output, or a ps_1_x texture register -- has to be
     * computed into a temp first, because the epilogue's read of the
     * destination would not compile. Splinter Cell's shaders do this. */
    if (!predication_scratch_ && inst.dst && inst.dst->shift_scale != 0) {
      /* Resolve once, here, and judge by what SM4 will actually see. The D3D9
       * register type is not the answer: oC0 resolves to a temp when the
       * alpha-test epilogue needs the colour re-read, and a ps_1_x texture
       * register does not. */
      shift_dst_ = ResolveDst(*inst.dst);
      shift_mask_ = inst.dst->write_mask;
      if (shift_dst_ && !IsTempOperand(*shift_dst_))
        shift_scratch_ = ScratchTemp();
    }

    auto src = [&](size_t i) -> Operand {
      auto o = ResolveSrc(inst.src[i]);
      return o ? *o : Operand{};
    };
    auto dstOp = [&]() -> Operand {
      auto o = ResolveDst(*inst.dst);
      return o ? *o : Operand{};
    };

    switch (inst.opcode) {
    case D3DSIO_NOP: case D3DSIO_DCL: case D3DSIO_DEF: case D3DSIO_DEFB: case D3DSIO_DEFI:
    case D3DSIO_PHASE:
      return;
    case D3DSIO_MOV: case D3DSIO_MOVA:
      // Writes to the a0 address register convert float->int: mova rounds to
      // nearest even, the vs_1_x-era plain "mov a0" truncates toward zero.
      if (!mod.is_pixel_shader && inst.dst->reg.type == D3DSPR_ADDR) {
        auto s0 = ResolveSrc(inst.src[0]);
        if (!s0) break;
        uint32_t a = AddrTemp();
        if (inst.opcode == D3DSIO_MOVA) {
          uint32_t f = ScratchTemp();
          b.RoundNe(WithMask(Temp(f), inst.dst->write_mask), *s0);
          b.Ftoi(WithMask(Temp(a), inst.dst->write_mask), TempSwizzle(f, 0, 1, 2, 3));
        } else {
          b.Ftoi(WithMask(Temp(a), inst.dst->write_mask), *s0);
        }
        break;
      }
      b.Mov(dstOp(), src(0)); break;
    case D3DSIO_ADD: b.Add(dstOp(), src(0), src(1)); break;
    case D3DSIO_SUB: b.Add(dstOp(), src(0), Neg(src(1))); break;
    case D3DSIO_MUL: b.Mul(dstOp(), src(0), src(1)); break;
    case D3DSIO_MAD: b.Mad(dstOp(), src(0), src(1), src(2)); break;
    case D3DSIO_DP3: b.Dp3(dstOp(), src(0), src(1)); break;
    case D3DSIO_DP4: b.Dp4(dstOp(), src(0), src(1)); break;
    case D3DSIO_DP2ADD: {
      uint32_t t = ScratchTemp();
      b.Dp2(WithMask(Temp(t), 0b1111), src(0), src(1));
      b.Add(dstOp(), TempSwizzle(t, 0, 0, 0, 0), src(2));
      break;
    }
    case D3DSIO_MIN: b.Min(dstOp(), src(0), src(1)); break;
    case D3DSIO_MAX: b.Max(dstOp(), src(0), src(1)); break;
    case D3DSIO_SLT: b.Lt(dstOp(), src(0), src(1)); break;
    case D3DSIO_SGE: b.Ge(dstOp(), src(0), src(1)); break;
    case D3DSIO_FRC: b.Frc(dstOp(), src(0)); break;
    case D3DSIO_RCP: b.Rcp(dstOp(), ScalarSrc(src(0))); break;
    case D3DSIO_RSQ: b.Rsq(dstOp(), Abs(ScalarSrc(src(0)))); break; // D3D9 rsq takes abs(src) implicitly
    case D3DSIO_EXPP:
      if (!mod.is_pixel_shader && mod.major_version < 2) {
        /* vs_1_x expp is a vector result: (2^floor(w), frac(w), 2^w, 1).
         * From vs_2_0 on it is a reduced-precision exp, handled below. */
        Operand s = ScalarSrc(src(0));
        uint32_t t = ScratchTemp();
        b.Frc(WithMask(Temp(t), 0b0010), s);
        b.Add(WithMask(Temp(t), 0b0001), s, Neg(TempSwizzle(t, 1, 1, 1, 1)));
        b.ExpI(WithMask(Temp(t), 0b0001), TempSwizzle(t, 0, 0, 0, 0));
        b.ExpI(WithMask(Temp(t), 0b0100), s);
        b.Mov(WithMask(Temp(t), 0b1000), Imm4(1, 1, 1, 1));
        b.Mov(dstOp(), TempSwizzle(t, 0, 1, 2, 3));
        break;
      }
      b.ExpI(dstOp(), ScalarSrc(src(0)));
      break;
    case D3DSIO_EXP: b.ExpI(dstOp(), ScalarSrc(src(0))); break;
    case D3DSIO_LOG: case D3DSIO_LOGP: b.Log(dstOp(), Abs(ScalarSrc(src(0)))); break; // D3D9 log takes abs(src) implicitly
    case D3DSIO_ABS: b.Mov(dstOp(), Abs(src(0))); break;
    case D3DSIO_LRP: {
      // lrp dst, t, a, b => a + t*(b-a)
      uint32_t s = ScratchTemp();
      b.Add(Temp(s), src(2), Neg(src(1)));
      b.Mad(dstOp(), src(0), TempSwizzle(s, 0, 1, 2, 3), src(1));
      break;
    }
    case D3DSIO_CMP: {
      // cmp dst, src0, src1, src2 => (src0 >= 0) ? src1 : src2, a REAL float
      // comparison - SM4's movc instead tests its condition operand's raw
      // bits against zero, so the >= 0 test has to be computed explicitly.
      uint32_t t = ScratchTemp();
      b.Ge(Temp(t), src(0), Imm4(0, 0, 0, 0));
      b.Movc(dstOp(), TempSwizzle(t, 0, 1, 2, 3), src(1), src(2));
      break;
    }
    case D3DSIO_CND: {
      // ps.1.x cnd dst, src0, src1, src2 => (src0.a > 0.5) ? src1 : src2,
      // broadcast from the alpha component alone.
      uint32_t t = ScratchTemp();
      b.Lt(Temp(t), Imm4(0.5f, 0.5f, 0.5f, 0.5f), WithSwizzle(src(0), 3, 3, 3, 3));
      b.Movc(dstOp(), TempSwizzle(t, 0, 1, 2, 3), src(1), src(2));
      break;
    }
    case D3DSIO_POW: {
      // pow dst, a, b => exp2(b * log2(|a|)); both sources are scalar
      // (replicate swizzle), and the result is replicated.
      uint32_t t = ScratchTemp();
      b.Log(Temp(t), Abs(Swz(src(0), 0, 0, 0, 0)));
      uint32_t t2 = ScratchTemp();
      b.Mul(Temp(t2), TempSwizzle(t, 0, 1, 2, 3), Swz(src(1), 0, 0, 0, 0));
      b.ExpI(dstOp(), TempSwizzle(t2, 0, 1, 2, 3));
      break;
    }
    case D3DSIO_CRS: {
      // crs dst, a, b => a.yzx*b.zxy - a.zxy*b.yzx
      uint32_t t1 = ScratchTemp(), t2 = ScratchTemp();
      b.Mul(Temp(t1), Swz(src(0), 1, 2, 0, 0), Swz(src(1), 2, 0, 1, 0));
      b.Mul(Temp(t2), Swz(src(0), 2, 0, 1, 0), Swz(src(1), 1, 2, 0, 0));
      b.Add(dstOp(), TempSwizzle(t1, 0, 1, 2, 3), Neg(TempSwizzle(t2, 0, 1, 2, 3)));
      break;
    }
    case D3DSIO_NRM: {
      // nrm dst, a => a * rsq(dot(a,a)) (w component of dst is left as a copy of x)
      uint32_t t = ScratchTemp();
      b.Dp3(WithMask(Temp(t), 0b1111), src(0), src(0));
      b.Rsq(WithMask(Temp(t), 0b1111), TempSwizzle(t, 0, 0, 0, 0));
      b.Mul(dstOp(), src(0), TempSwizzle(t, 0, 0, 0, 0));
      break;
    }
    case D3DSIO_SINCOS: {
      /* D3D9 sincos writes cos to .x and sin to .y. SM4's sincos takes the
       * sine destination first, so the two land in t.y and t.x. */
      uint32_t t = ScratchTemp();
      b.SinCos(WithMask(Temp(t), 0b0010), WithMask(Temp(t), 0b0001), Swz(src(0), 0, 0, 0, 0));
      b.Mov(dstOp(), TempSwizzle(t, 0, 1, 0, 1));
      break;
    }
    case D3DSIO_SGN: {
      // sgn dst, a => (a>0) - (a<0), computed via two Ge compares.
      uint32_t gt = ScratchTemp(), lt = ScratchTemp();
      b.Lt(Temp(gt), Imm4(0, 0, 0, 0), src(0));    // gt = 0 < a  (a>0 ? 1 : 0, as ~0/0 mask reinterpreted below)
      b.Lt(Temp(lt), src(0), Imm4(0, 0, 0, 0));    // lt = a < 0
      b.Movc(Temp(gt), TempSwizzle(gt,0,1,2,3), Imm4(1,1,1,1), Imm4(0,0,0,0));
      b.Movc(Temp(lt), TempSwizzle(lt,0,1,2,3), Imm4(1,1,1,1), Imm4(0,0,0,0));
      b.Add(dstOp(), TempSwizzle(gt, 0, 1, 2, 3), Neg(TempSwizzle(lt, 0, 1, 2, 3)));
      break;
    }
    case D3DSIO_DST: {
      // dst dst, a, b => (1, a.y*b.y, a.z, b.w). Computed in a temp first:
      // the destination may be one of the sources.
      uint32_t t = ScratchTemp();
      b.Mul(WithMask(Temp(t), 0b0010), src(0), src(1));
      b.Mov(WithMask(Temp(t), 0b0001), Imm4(1, 1, 1, 1));
      b.Mov(WithMask(Temp(t), 0b0100), Swz(src(0), 2, 2, 2, 2));
      b.Mov(WithMask(Temp(t), 0b1000), Swz(src(1), 3, 3, 3, 3));
      b.Mov(dstOp(), TempSwizzle(t, 0, 1, 2, 3));
      break;
    }
    case D3DSIO_LIT: {
      /* (1, max(x,0), (x > 0 && y > 0) ? pow(y, clamp(w)) : 0, 1), with the
       * power clamped to +-127.9961 as the D3D9 reference does. The specular
       * term must be zero on the unlit side; computing pow unconditionally
       * lit back faces. Built in a temp: the destination may be the source. */
      Operand s = src(0);
      uint32_t t = ScratchTemp();
      b.Max(WithMask(Temp(t), 0b0001), Swz(s, 3, 3, 3, 3), Imm4(-127.9961f, -127.9961f, -127.9961f, -127.9961f));
      b.Min(WithMask(Temp(t), 0b0001), TempSwizzle(t, 0, 0, 0, 0), Imm4(127.9961f, 127.9961f, 127.9961f, 127.9961f));
      b.Log(WithMask(Temp(t), 0b0100), Swz(s, 1, 1, 1, 1));
      b.Mul(WithMask(Temp(t), 0b0100), TempSwizzle(t, 2, 2, 2, 2), TempSwizzle(t, 0, 0, 0, 0));
      b.ExpI(WithMask(Temp(t), 0b0100), TempSwizzle(t, 2, 2, 2, 2));
      uint32_t c = ScratchTemp();
      b.Lt(WithMask(Temp(c), 0b0001), Imm4(0, 0, 0, 0), Swz(s, 0, 0, 0, 0));
      b.Lt(WithMask(Temp(c), 0b0010), Imm4(0, 0, 0, 0), Swz(s, 1, 1, 1, 1));
      b.And(WithMask(Temp(c), 0b0001), TempSwizzle(c, 0, 0, 0, 0), TempSwizzle(c, 1, 1, 1, 1));
      b.Movc(WithMask(Temp(t), 0b0100), TempSwizzle(c, 0, 0, 0, 0), TempSwizzle(t, 2, 2, 2, 2), Imm4(0, 0, 0, 0));
      b.Max(WithMask(Temp(t), 0b0010), Swz(s, 0, 0, 0, 0), Imm4(0, 0, 0, 0));
      b.Mov(WithMask(Temp(t), 0b1001), Imm4(1, 1, 1, 1));
      b.Mov(dstOp(), TempSwizzle(t, 0, 1, 2, 3));
      break;
    }
    case D3DSIO_M4x4: case D3DSIO_M4x3: case D3DSIO_M3x4: case D3DSIO_M3x3: case D3DSIO_M3x2: {
      static const int kRows[] = {4, 3, 4, 3, 2};
      int rows = inst.opcode == D3DSIO_M4x4 ? 4 : inst.opcode == D3DSIO_M4x3 ? 3
               : inst.opcode == D3DSIO_M3x4 ? 4 : inst.opcode == D3DSIO_M3x3 ? 3 : 2;
      (void)kRows;
      static const uint8_t kMask[4] = {0b0001, 0b0010, 0b0100, 0b1000};
      if (inst.src[1].reg.type != D3DSPR_CONST && inst.src[1].reg.type != D3DSPR_TEMP) {
        Fail("matrix-multiply source row register must be c# or r#");
        break;
      }
      Operand vec = src(0);
      for (int r = 0; r < rows; r++) {
        SrcParam row = inst.src[1];
        row.reg.index += r;
        auto rowOp = ResolveSrc(row);
        if (!rowOp) break;
        if (inst.opcode == D3DSIO_M4x4 || inst.opcode == D3DSIO_M3x4)
          b.Dp4(WithMask(dstOp(), kMask[r]), vec, *rowOp);
        else
          b.Dp3(WithMask(dstOp(), kMask[r]), vec, *rowOp);
      }
      break;
    }
    case D3DSIO_TEXCOORD: {
      /* ps_1_4 texcrd: copy a coordinate register, after any _dz/_dw divide
       * its source modifier asks for, into a temp under the write mask.
       * Rejecting it failed every ps_1_4 shader that reads a coordinate as
       * data -- the cnd/cmp/bem families in Wine's suite, and games' DX8.1
       * paths. */
      if (inst.dst->reg.type == D3DSPR_TEMP && !inst.src.empty()) {
        b.Mov(dstOp(), src(0));
        break;
      }
      // ps<=1.3: "destination" tN IS the interpolated coordinate register -
      // every later read of tN already resolves to that same input via
      // PsInputReg(), so there is nothing to actually emit here.
      if (inst.dst->reg.type != D3DSPR_TEXTURE && inst.dst->reg.type != D3DSPR_TEXCRDOUT)
        Fail("texcoord: unexpected destination register");
      break;
    }
    case D3DSIO_TEX: {
      uint32_t sampler;
      Operand coord;
      if (inst.src.size() >= 2 && inst.src[1].reg.type == D3DSPR_SAMPLER) {
        // ps >= 2.0: dst, coord, sampler. D3DSI_TEXLD_PROJECT/BIAS on the
        // opcode token select the texldp/texldb variant.
        sampler = inst.src[1].reg.index;
        coord = src(0);
      } else {
        // ps < 2.0: dst register number selects both the coordinate input
        // (ps_1_4 has an explicit coord src; ps<1.4 reuses the dst register
        // number as the implicit coordinate register too) and the sampler stage.
        sampler = inst.dst->reg.index;
        coord = inst.src.empty() ? TexcoordIn(sampler) : src(0);
      }
      /* inst.control holds the opcode-specific bits shifted down to bit 0;
       * the D3DSI_ constants have them where they are in the token. Testing
       * one against the other is what made every texldp and texldb a plain
       * texld. */
      constexpr uint32_t kBias = D3DSI_TEXLD_BIAS >> D3DSP_OPCODESPECIFICCONTROL_SHIFT;
      constexpr uint32_t kProject = D3DSI_TEXLD_PROJECT >> D3DSP_OPCODESPECIFICCONTROL_SHIFT;
      auto lookup = [&](Operand dst) {
        if (inst.control & kBias) {
          SampleTexB(dst, coord, sampler, WithSwizzle(coord, 3, 3, 3, 3));
        } else if (inst.control & kProject) {
          uint32_t t = ScratchTemp();
          b.Rcp(WithMask(Temp(t), 0b1111), WithSwizzle(coord, 3, 3, 3, 3));
          b.Mul(WithMask(Temp(t), 0b1111), coord, TempSwizzle(t, 0, 0, 0, 0));
          SampleTex(dst, TempSwizzle(t, 0, 1, 2, 3), sampler);
        } else {
          SampleTex(dst, coord, sampler);
        }
      };
      if (inst.src.size() >= 2 && inst.src[1].reg.type == D3DSPR_SAMPLER)
        WithSamplerSwizzle(dstOp(), inst.src[1], lookup);
      else
        lookup(dstOp());
      break;
    }
    case D3DSIO_TEXM3x2PAD:
    case D3DSIO_TEXM3x3PAD: {
      /* A row of the matrix; no result of its own. The rows are numbered by
       * order of appearance, which is how D3D9 defines the sequence. */
      if (inst.src.empty()) {
        Fail("texm3x*pad: expected a source register");
        break;
      }
      Texm3Row(inst.dst->reg.index, src(0), texm3_row_ < 2 ? texm3_row_ : 1);
      texm3_row_++;
      break;
    }
    case D3DSIO_TEXM3x2TEX: {
      /* Second row, then sample this stage with (u, v). */
      if (inst.src.empty()) {
        Fail("texm3x2tex: expected a source register");
        break;
      }
      uint32_t stage = inst.dst->reg.index;
      Texm3Row(stage, src(0), 1);
      texm3_row_ = 0;
      SampleTex(dstOp(), TempSwizzle(Texm3Temp(), 0, 1, 0, 0), stage);
      break;
    }
    case D3DSIO_TEXM3x2DEPTH: {
      /* ps_1_4: second row, then depth = u / v, with the D3D-defined 1.0 for a
       * zero denominator. */
      if (inst.src.empty()) {
        Fail("texm3x2depth: expected a source register");
        break;
      }
      Texm3Row(inst.dst->reg.index, src(0), 1);
      texm3_row_ = 0;
      uint32_t m = Texm3Temp();
      uint32_t q = ScratchTemp();
      b.Div(WithMask(Temp(q), 0b0001), TempSwizzle(m, 0, 0, 0, 0), TempSwizzle(m, 1, 1, 1, 1));
      uint32_t zero = ScratchTemp();
      b.Eq(WithMask(Temp(zero), 0b0001), TempSwizzle(m, 1, 1, 1, 1), Imm1(0));
      b.Movc(WithMask(Temp(q), 0b0001), TempSwizzle(zero, 0, 0, 0, 0), Imm1(1.0f), TempSwizzle(q, 0, 0, 0, 0));
      b.Mov(OutputDepth(), TempSwizzle(q, 0, 0, 0, 0));
      break;
    }
    case D3DSIO_TEXM3x3TEX:
    case D3DSIO_TEXM3x3: {
      /* Third row. texm3x3tex samples with the resulting vector; texm3x3
       * writes it straight out with w = 1. */
      if (inst.src.empty()) {
        Fail("texm3x3: expected a source register");
        break;
      }
      uint32_t stage = inst.dst->reg.index;
      Texm3Row(stage, src(0), 2);
      texm3_row_ = 0;
      if (inst.opcode == D3DSIO_TEXM3x3TEX) {
        SampleTex(dstOp(), TempSwizzle(Texm3Temp(), 0, 1, 2, 2), stage);
      } else {
        Operand out = dstOp();
        b.Mov(WithMask(out, 0b0111), TempSwizzle(Texm3Temp(), 0, 1, 2, 2));
        b.Mov(WithMask(out, 0b1000), Imm1(1.0f));
      }
      break;
    }
    case D3DSIO_TEXM3x3SPEC:
    case D3DSIO_TEXM3x3VSPEC: {
      /* Third row gives the normal; the result is the eye vector reflected
       * about it, used as the sampling direction:
       *   R = 2 * N * dot(N, E) / dot(N, N) - E
       * texm3x3spec takes the eye vector from a second source; the vspec form
       * gathers it from the w components of the three coordinate registers. */
      if (inst.src.empty()) {
        Fail("texm3x3spec: expected a source register");
        break;
      }
      uint32_t stage = inst.dst->reg.index;
      Texm3Row(stage, src(0), 2);
      texm3_row_ = 0;

      uint32_t normal = Texm3Temp();
      uint32_t eye = ScratchTemp();
      if (inst.opcode == D3DSIO_TEXM3x3SPEC) {
        if (inst.src.size() < 2) {
          Fail("texm3x3spec: expected an eye-vector source");
          break;
        }
        b.Mov(WithMask(Temp(eye), 0b0111), src(1));
      } else {
        /* The three coordinate registers are the two pads plus this one. */
        uint32_t first = stage >= 2 ? stage - 2 : 0;
        b.Mov(WithMask(Temp(eye), 0b0001), WithSwizzle(TexcoordIn(first), 3, 3, 3, 3));
        b.Mov(WithMask(Temp(eye), 0b0010), WithSwizzle(TexcoordIn(first + 1), 3, 3, 3, 3));
        b.Mov(WithMask(Temp(eye), 0b0100), WithSwizzle(TexcoordIn(stage), 3, 3, 3, 3));
      }

      uint32_t ne = ScratchTemp();
      uint32_t nn = ScratchTemp();
      b.Dp3(WithMask(Temp(ne), 0b0001), TempSwizzle(normal, 0, 1, 2, 2), TempSwizzle(eye, 0, 1, 2, 2));
      b.Dp3(WithMask(Temp(nn), 0b0001), TempSwizzle(normal, 0, 1, 2, 2), TempSwizzle(normal, 0, 1, 2, 2));
      uint32_t scale = ScratchTemp();
      b.Div(WithMask(Temp(scale), 0b0001), TempSwizzle(ne, 0, 0, 0, 0), TempSwizzle(nn, 0, 0, 0, 0));
      b.Add(WithMask(Temp(scale), 0b0001), TempSwizzle(scale, 0, 0, 0, 0), TempSwizzle(scale, 0, 0, 0, 0));

      uint32_t reflection = ScratchTemp();
      b.Mul(WithMask(Temp(reflection), 0b0111), TempSwizzle(normal, 0, 1, 2, 2), TempSwizzle(scale, 0, 0, 0, 0));
      b.Add(WithMask(Temp(reflection), 0b0111), TempSwizzle(reflection, 0, 1, 2, 2), Neg(TempSwizzle(eye, 0, 1, 2, 2)));

      SampleTex(dstOp(), TempSwizzle(reflection, 0, 1, 2, 2), stage);
      break;
    }
    case D3DSIO_TEXM3x3DIFF:
      /* Reserved by D3D and never implemented by any hardware, so there is
       * nothing to be compatible with. */
      Fail("texm3x3diff was never implemented by any D3D9 driver");
      break;
    case D3DSIO_TEXBEM:
    case D3DSIO_TEXBEML: {
      /* Bump environment mapping. The perturbation matrix belongs to the
       * destination stage and lives in the extra constant buffer, because it
       * is texture stage state rather than anything the shader carries:
       *   u' = t(m).u + mat00 * src.r + mat10 * src.g
       *   v' = t(m).v + mat01 * src.r + mat11 * src.g
       * texbeml additionally scales the result by src.b * lscale + loffset. */
      if (inst.src.empty()) {
        Fail("texbem: expected a source register");
        break;
      }
      uint32_t stage = inst.dst->reg.index;
      Operand source = src(0);
      Operand matrix = ConstantBuffer(1, BumpMatrixElement(stage));
      Operand coord = TexcoordIn(stage);

      uint32_t t = ScratchTemp();
      /* du = mat00 * src.r + mat10 * src.g, dv = mat01 * src.r + mat11 * src.g */
      b.Mul(WithMask(Temp(t), 0b0011), WithSwizzle(matrix, 0, 1, 0, 0), WithSwizzle(source, 0, 0, 0, 0));
      b.Mad(WithMask(Temp(t), 0b0011), WithSwizzle(matrix, 2, 3, 0, 0), WithSwizzle(source, 1, 1, 1, 1),
            TempSwizzle(t, 0, 1, 0, 0));
      b.Add(WithMask(Temp(t), 0b0011), TempSwizzle(t, 0, 1, 0, 0), WithSwizzle(coord, 0, 1, 0, 0));

      SampleTex(dstOp(), TempSwizzle(t, 0, 1, 0, 0), stage);

      if (inst.opcode == D3DSIO_TEXBEML) {
        Operand luminance = ConstantBuffer(1, BumpLuminanceElement(stage));
        uint32_t l = ScratchTemp();
        b.Mad(WithMask(Temp(l), 0b0001), WithSwizzle(source, 2, 2, 2, 2), WithSwizzle(luminance, 0, 0, 0, 0),
              WithSwizzle(luminance, 1, 1, 1, 1));
        /* Only the colour channels are scaled; alpha comes through unchanged. */
        Operand scaled = dstOp();
        b.Mul(WithMask(scaled, 0b0111), scaled, TempSwizzle(l, 0, 0, 0, 0));
      }
      break;
    }
    case D3DSIO_BEM: {
      /* ps_1_4: the same perturbation without the sample, writing only .rg. */
      if (inst.src.size() < 2) {
        Fail("bem: expected two source registers");
        break;
      }
      uint32_t stage = inst.dst->reg.index;
      Operand matrix = ConstantBuffer(1, BumpMatrixElement(stage));
      Operand base = src(0);
      Operand perturb = src(1);

      uint32_t t = ScratchTemp();
      b.Mul(WithMask(Temp(t), 0b0011), WithSwizzle(matrix, 0, 1, 0, 0), WithSwizzle(perturb, 0, 0, 0, 0));
      b.Mad(WithMask(Temp(t), 0b0011), WithSwizzle(matrix, 2, 3, 0, 0), WithSwizzle(perturb, 1, 1, 1, 1),
            TempSwizzle(t, 0, 1, 0, 0));
      b.Add(WithMask(dstOp(), 0b0011), TempSwizzle(t, 0, 1, 0, 0), WithSwizzle(base, 0, 1, 0, 0));
      break;
    }
    /* ps_1_x address-mode texture reads. Each one samples the stage named by
     * the destination register, using components of a previously loaded
     * register as the coordinate, so they are a swizzle plus an ordinary
     * sample. The sampler index is the destination register number, as with
     * every other ps_1_x texture instruction. */
    case D3DSIO_TEXREG2AR:
    case D3DSIO_TEXREG2GB:
    case D3DSIO_TEXREG2RGB: {
      if (inst.src.empty()) {
        Fail("texreg2*: expected a source register");
        break;
      }
      uint32_t sampler = inst.dst->reg.index;
      Operand source = src(0);
      Operand coord;
      if (inst.opcode == D3DSIO_TEXREG2AR)
        coord = WithSwizzle(source, 3, 0, 0, 0); /* (a, r) */
      else if (inst.opcode == D3DSIO_TEXREG2GB)
        coord = WithSwizzle(source, 1, 2, 0, 0); /* (g, b) */
      else
        coord = WithSwizzle(source, 0, 1, 2, 2); /* (r, g, b) */
      SampleTex(dstOp(), coord, sampler);
      break;
    }
    case D3DSIO_TEXDP3: {
      /* Three-component dot of this stage's interpolated coordinate with the
       * source, replicated across the destination. No sampling. */
      if (inst.src.empty()) {
        Fail("texdp3: expected a source register");
        break;
      }
      b.Dp3(dstOp(), TexcoordIn(inst.dst->reg.index), src(0));
      break;
    }
    case D3DSIO_TEXDP3TEX: {
      /* The dot product becomes a one-dimensional texture coordinate. */
      if (inst.src.empty()) {
        Fail("texdp3tex: expected a source register");
        break;
      }
      uint32_t sampler = inst.dst->reg.index;
      uint32_t t = ScratchTemp();
      b.Dp3(WithMask(Temp(t), 0b1111), TexcoordIn(sampler), src(0));
      SampleTex(dstOp(), TempSwizzle(t, 0, 0, 0, 0), sampler);
      break;
    }
    case D3DSIO_TEXDEPTH: {
      /* ps_1_4: the destination register holds the depth as r/g, and the
       * instruction writes their quotient to the depth output. A zero
       * denominator is defined by D3D to leave depth at 1.0. */
      Operand source = TempSwizzle(inst.dst->reg.index, 0, 1, 0, 0);
      uint32_t q = ScratchTemp();
      b.Div(WithMask(Temp(q), 0b0001), WithSwizzle(source, 0, 0, 0, 0), WithSwizzle(source, 1, 1, 1, 1));
      uint32_t zero = ScratchTemp();
      b.Eq(WithMask(Temp(zero), 0b0001), WithSwizzle(source, 1, 1, 1, 1), Imm1(0));
      b.Movc(WithMask(Temp(q), 0b0001), TempSwizzle(zero, 0, 0, 0, 0), Imm1(1.0f), TempSwizzle(q, 0, 0, 0, 0));
      b.Mov(OutputDepth(), TempSwizzle(q, 0, 0, 0, 0));
      break;
    }
    case D3DSIO_TEXLDD: {
      // ps_3_0: dst, coord, sampler, ddx, ddy. Explicit gradients keep the mip
      // selection right where the hardware's own derivatives would be wrong,
      // which is why a shader reaches for this inside divergent control flow.
      if (inst.src.size() < 4 || inst.src[1].reg.type != D3DSPR_SAMPLER) {
        Fail("texldd: expected coord, sampler, ddx, ddy");
        break;
      }
      uint32_t sampler = inst.src[1].reg.index;
      WithSamplerSwizzle(dstOp(), inst.src[1], [&](Operand dst) { SampleTexD(dst, src(0), sampler, src(2), src(3)); });
      break;
    }
    case D3DSIO_DSX:
      b.DerivRtx(dstOp(), src(0));
      break;
    case D3DSIO_DSY:
      b.DerivRty(dstOp(), src(0));
      break;
    case D3DSIO_TEXLDL: {
      if (inst.src.size() < 2 || inst.src[1].reg.type != D3DSPR_SAMPLER) {
        Fail("texldl: expected an explicit sampler source");
        break;
      }
      uint32_t sampler = inst.src[1].reg.index;
      Operand coord = src(0);
      WithSamplerSwizzle(dstOp(), inst.src[1], [&](Operand dst) { SampleTexL(dst, coord, sampler, WithSwizzle(coord, 3, 3, 3, 3)); });
      break;
    }
    case D3DSIO_TEXKILL: {
      // texkill's "destination" is really the (already-bound) register being
      // tested: r# reads as a temp, t# reads as the interpolated coordinate.
      Operand coord;
      if (inst.dst->reg.type == D3DSPR_TEMP)
        coord = TempSwizzle(inst.dst->reg.index, 0, 1, 2, 3);
      else if (inst.dst->reg.type == D3DSPR_TEXTURE || inst.dst->reg.type == D3DSPR_TEXCRDOUT)
        coord = TexcoordIn(inst.dst->reg.index);
      else {
        Fail("texkill: unexpected register");
        break;
      }
      // Discard if any component is < 0: reduce via a real (well-defined)
      // float min rather than combining per-component boolean bit-patterns,
      // then a single lt+discard_nz on the reduced scalar.
      uint32_t m = ScratchTemp();
      b.Min(WithMask(Temp(m), 0b0011), WithSwizzle(coord, 0, 1, 0, 0), WithSwizzle(coord, 2, 3, 0, 0));
      b.Min(WithMask(Temp(m), 0b0001), TempSwizzle(m, 0, 0, 0, 0), TempSwizzle(m, 1, 1, 1, 1));
      uint32_t t = ScratchTemp();
      b.Lt(WithMask(Temp(t), 0b0001), TempSwizzle(m, 0, 0, 0, 0), Imm1(0));
      b.Discard(true, TempSwizzle(t, 0, 0, 0, 0));
      break;
    }
    case D3DSIO_IF: {
      b.If(true, src(0));
      if_depth++;
      break;
    }
    case D3DSIO_IFC: {
      uint32_t t = ScratchTemp();
      EmitComparison(t, inst.control, inst.src[0], inst.src[1]);
      b.If(true, TempSwizzle(t, 0, 0, 0, 0));
      if_depth++;
      break;
    }
    case D3DSIO_ELSE: b.Else(); break;
    case D3DSIO_ENDIF: b.EndIf(); if_depth--; break;
    case D3DSIO_LOOP: case D3DSIO_REP: {
      // loop aL, i# : body runs i#.x times, aL = i#.y then += i#.z per pass.
      // rep i#      : body runs i#.x times, no aL.
      // SM4 loops are condition-free; the counter lives in an int temp
      // (x=remaining, y=aL, z=step) tested at the top and stepped at the end.
      bool is_loop = inst.opcode == D3DSIO_LOOP;
      size_t i_src = is_loop ? 1 : 0;
      if (inst.src.size() <= i_src || inst.src[i_src].reg.type != D3DSPR_CONSTINT) {
        Fail("loop/rep: expected an i# register source");
        break;
      }
      uint32_t i_reg = inst.src[i_src].reg.index;
      uint32_t ctr = ScratchTemp();
      auto it = def_i.find(i_reg);
      if (it != def_i.end())
        b.Mov(Temp(ctr), ImmI4(it->second[0], it->second[1], it->second[2], 0));
      else
        b.Mov(Temp(ctr), ConstantBuffer(1, kExtraCBIntBase + i_reg));
      b.Loop();
      b.BreakC(false, TempSwizzle(ctr, 0, 0, 0, 0)); // break once no iterations remain
      loop_stack_.push_back({ctr, is_loop});
      break;
    }
    case D3DSIO_ENDLOOP: case D3DSIO_ENDREP: {
      if (loop_stack_.empty()) {
        Fail("endloop/endrep without a matching loop/rep");
        break;
      }
      LoopCtx lc = loop_stack_.back();
      loop_stack_.pop_back();
      b.IAdd(WithMask(Temp(lc.ctr_temp), 0b0001), TempSwizzle(lc.ctr_temp, 0, 0, 0, 0), ImmI4(-1, -1, -1, -1));
      if (lc.has_al)
        b.IAdd(WithMask(Temp(lc.ctr_temp), 0b0010), TempSwizzle(lc.ctr_temp, 1, 1, 1, 1),
               TempSwizzle(lc.ctr_temp, 2, 2, 2, 2));
      b.EndLoop();
      break;
    }
    case D3DSIO_BREAK:
      if (loop_stack_.empty()) { Fail("break outside of a loop"); break; }
      b.Break();
      break;
    case D3DSIO_BREAKC: {
      if (loop_stack_.empty()) { Fail("breakc outside of a loop"); break; }
      uint32_t t = ScratchTemp();
      EmitComparison(t, inst.control, inst.src[0], inst.src[1]);
      b.BreakC(true, TempSwizzle(t, 0, 0, 0, 0));
      break;
    }
    case D3DSIO_LABEL:
    case D3DSIO_RET:
      /* Bodies are collected in the scan and emitted at the call site; the
       * markers themselves produce nothing. */
      break;
    case D3DSIO_CALL:
      if (inst.src.empty()) {
        Fail("call: expected a label");
        break;
      }
      EmitSubroutine(inst.src[0].reg.index);
      break;
    case D3DSIO_CALLNZ: {
      if (inst.src.size() < 2) {
        Fail("callnz: expected a label and a condition");
        break;
      }
      auto condition = ResolveSrcBase(inst.src[1]);
      if (!condition) {
        Fail("callnz: unresolved condition");
        break;
      }
      /* D3DSPSM_NOT on the condition inverts the test. */
      b.If(inst.src[1].modifier != D3DSPSM_NOT, *condition);
      EmitSubroutine(inst.src[0].reg.index);
      b.EndIf();
      break;
    }
    case D3DSIO_SETP:
      if (inst.src.size() < 2) {
        Fail("setp: expected two sources");
        break;
      }
      /* The comparison writes an all-ones/all-zeroes mask per component, which
       * is exactly what movc and breakc consume. */
      EmitComparison(PredicateTemp(), inst.control, inst.src[0], inst.src[1]);
      break;
    case D3DSIO_BREAKP: {
      if (inst.src.empty()) {
        Fail("breakp: expected a predicate source");
        break;
      }
      /* D3DSPSM_NOT on the predicate inverts the test rather than the value. */
      bool test_nonzero = inst.src[0].modifier != D3DSPSM_NOT;
      auto predicate = ResolveSrcBase(inst.src[0]);
      if (!predicate) {
        Fail("breakp: unresolved predicate");
        break;
      }
      b.BreakC(test_nonzero, *predicate);
      break;
    }
    default:
      Fail("unsupported opcode " + std::to_string((int)inst.opcode));
      break;
    }

    FlushIndexedOutput();

    /* Close out predication before anything else touches the destination. */
    if (predication_scratch_) {
      uint32_t scratch = *predication_scratch_;
      predication_scratch_.reset();
      if (!has_error && predicated_dst) {
        Operand destination = WithMask(*predicated_dst, predication_mask_);
        bool inverted = inst.predicate && inst.predicate->modifier == D3DSPSM_NOT;
        /* movc picks per component: predicate set takes the new value. */
        b.Movc(destination, *predicate, inverted ? *predicated_dst : TempSwizzle(scratch, 0, 1, 2, 3),
               inverted ? TempSwizzle(scratch, 0, 1, 2, 3) : *predicated_dst);
      }
    }
    if (predicated_branch && !has_error)
      b.EndIf();

    // ps_1_x destination shift modifier (_x2/_x4/_d2/...): multiply the just-
    // written register by 2^shift, then saturate if _sat was also present.
    if (!has_error && inst.dst && inst.dst->shift_scale != 0) {
      float scale = std::exp2f((float)inst.dst->shift_scale);
      Operand factor = Imm4(scale, scale, scale, scale);
      if (shift_scratch_) {
        /* Went into a temp: scale it there, then store to the real
         * destination, which may be write-only. */
        uint32_t scratch = *shift_scratch_;
        Operand out = WithMask(*shift_dst_, shift_mask_);
        shift_scratch_.reset();
        shift_dst_.reset();
        if (inst.dst->saturate)
          out = Saturate(out);
        b.Mul(out, TempSwizzle(scratch, 0, 1, 2, 3), factor);
      } else if (shift_dst_) {
        /* A temp SM4 can read back: scale it in place. */
        Operand d = WithMask(*shift_dst_, shift_mask_);
        uint32_t reg = shift_dst_->index0;
        shift_dst_.reset();
        if (inst.dst->saturate)
          d = Saturate(d);
        b.Mul(d, TempSwizzle(reg, 0, 1, 2, 3), factor);
      } else {
        Fail("shift modifier on an unresolvable destination");
        return;
      }
    }
  }

  static bool IsTempOperand(const Operand &o) { return o.type == microsoft::D3D10_SB_OPERAND_TYPE_TEMP; }

  void EmitComparison(uint32_t dst_temp, uint32_t control, const SrcParam &a, const SrcParam &c) {
    auto ao = ResolveSrc(a);
    auto co = ResolveSrc(c);
    if (!ao || !co) return;
    D3DSHADER_COMPARISON cmp = (D3DSHADER_COMPARISON)control;
    switch (cmp) {
    case D3DSPC_GT: b.Lt(Temp(dst_temp), *co, *ao); break;
    case D3DSPC_EQ: b.Eq(Temp(dst_temp), *ao, *co); break;
    case D3DSPC_GE: b.Ge(Temp(dst_temp), *ao, *co); break;
    case D3DSPC_LT: b.Lt(Temp(dst_temp), *ao, *co); break;
    case D3DSPC_NE: b.Ne(Temp(dst_temp), *ao, *co); break;
    case D3DSPC_LE: b.Ge(Temp(dst_temp), *co, *ao); break;
    default: Fail("unsupported comparison"); break;
    }
  }

  // --- top-level driver -----------------------------------------------------
  void Run() {
    Scan();
    if (has_error) return;

    alpha_test = mod.is_pixel_shader && opts.alpha_test_func != 0 && opts.alpha_test_func != D3DCMP_ALWAYS;
    // ps_3_0 does its own fog; D3D9 applies none after it.
    fog = mod.is_pixel_shader && opts.fog && mod.major_version < 3;

    if (result.float_constants_used || !def_f.empty())
      result.float_constants_used = std::max(result.float_constants_used, 1u);
    for (auto &inst : mod.instructions)
      for (auto &s : inst.src)
        if (s.reg.type == D3DSPR_CONST)
          result.float_constants_used = std::max(result.float_constants_used, s.reg.index + 1);
    // Dynamic indexing (c[a0.x+n]) defeats static max-register tracking; the
    // buffer has to span the whole 256-register file.
    if (has_rel_const && !mod.is_pixel_shader)
      result.float_constants_used = 256;
    // Constant buffers must be declared before any other DCL: airconv's
    // control-flow scan pass fills cbufferMap in declaration order and
    // resolves cb-relative operands against it.
    if (result.float_constants_used)
      b.DeclConstantBuffer(0, result.float_constants_used);
    bool point_size = !mod.is_pixel_shader && opts.point_size;
    if (uses_api_bool || uses_api_int || alpha_test || fog || !mod.is_pixel_shader)
      b.DeclConstantBuffer(1, kExtraCBSizeVec4);

    for (uint32_t s : samplers_used) {
      switch (SamplerDim(s)) {
      case 1: b.DeclResourceTextureCube(s); break;
      case 2: b.DeclResourceTexture3D(s); break;
      default: b.DeclResourceTexture2D(s); break;
      }
      b.DeclSampler(s, Shadow(s));
    }

    if (mod.is_pixel_shader) {
      b.DeclInputPositionPS(0);
      // Two distinct input register files: v# (ps3.0 carries a dcl'd usage,
      // earlier models are fixed diffuse/specular COLOR) and t# (always
      // TEXCOORD, read directly in ps<2.0 via texcoord/tex/texkill dst or as
      // an ordinary source in ps2.x). Declare lazily over the registers the
      // shader actually reads.
      std::set<uint32_t> ps_v_inputs, ps_t_inputs;
      for (auto &inst : mod.instructions) {
        if (inst.opcode == D3DSIO_DCL || inst.opcode == D3DSIO_DEF || inst.opcode == D3DSIO_DEFI ||
            inst.opcode == D3DSIO_DEFB)
          continue;
        for (auto &s : inst.src) {
          if (s.reg.type == D3DSPR_INPUT) ps_v_inputs.insert(s.reg.index);
          if (s.reg.type == D3DSPR_TEXTURE) ps_t_inputs.insert(s.reg.index);
        }
        // ps<2.0 tex/texcoord/texkill address the coordinate through their t#
        // "destination"; ps>=2.0 destinations are ordinary temps.
        if ((inst.opcode == D3DSIO_TEXCOORD || inst.opcode == D3DSIO_TEX || inst.opcode == D3DSIO_TEXKILL) &&
            inst.dst && (inst.dst->reg.type == D3DSPR_TEXTURE || inst.dst->reg.type == D3DSPR_TEXCRDOUT))
          ps_t_inputs.insert(inst.dst->reg.index);
        // The ps_1_x matrix, bump and dot-product texture instructions read the
        // coordinate of the stage their destination names as well; only the
        // ones addressing through another register, or r5, do not.
        if (mod.major_version < 2 && inst.dst && inst.dst->reg.type == D3DSPR_TEXTURE &&
            inst.opcode != D3DSIO_TEXREG2AR && inst.opcode != D3DSIO_TEXREG2GB &&
            inst.opcode != D3DSIO_TEXREG2RGB && inst.opcode != D3DSIO_TEXDEPTH)
          ps_t_inputs.insert(inst.dst->reg.index);
      }
      bool point_sprite = opts.point_sprite;
      for (uint32_t i : ps_v_inputs) {
        uint32_t reg = i < 2 ? kLinkColor0 + i : kLinkNone;
        if (mod.major_version >= 3) {
          auto dcl = ps_input_dcl_.find(i);
          bool texcoord = dcl == ps_input_dcl_.end() || dcl->second.first == D3DDECLUSAGE_TEXCOORD;
          if (point_sprite && texcoord) {
            ps_vinput_reg_[i] = kPointCoordInput;
            continue;
          }
          reg = dcl != ps_input_dcl_.end() ? LinkRegister(dcl->second.first, dcl->second.second)
                                           : LinkRegister(D3DDECLUSAGE_TEXCOORD, (uint8_t)i);
        }
        DeclareInput(reg);
        ps_vinput_reg_[i] = reg;
      }
      if (fog && opts.fog_table_mode == 0) {
        // The table modes need no input at all -- they read the depth.
        fog_input_ = kLinkFog;
        DeclareInput(kLinkFog);
      }
      if (!point_sprite)
        for (uint32_t i : ps_t_inputs)
          DeclareInput(PsInputRegT(i));
      /* System values, not varyings: registers this shader leaves free. */
      uint32_t free_reg = kLinkRegisterCount - 1;
      auto next_free = [&]() {
        while (free_reg > 0 && (result.link_inputs & LinkBit(free_reg)))
          free_reg--;
        return free_reg--;
      };
      if (uses_vface_) {
        face_input_ = next_free();
        b.DeclInputIsFrontFace(*face_input_);
      }
      if (point_sprite) {
        pointcoord_input_ = next_free();
        b.DeclIOWithSemantic(*pointcoord_input_, 0b0011, "SV_PointCoord", 0);
      }
      // Every ps output register in D3D9 is oC0..oC3; declare them lazily too.
      std::set<uint32_t> ps_outputs;
      for (auto &inst : mod.instructions)
        if (inst.dst && inst.dst->reg.type == D3DSPR_COLOROUT)
          ps_outputs.insert(inst.dst->reg.index);
      for (uint32_t o : ps_outputs)
        b.DeclOutputTarget(o, o);
    } else {
      b.DeclOutputPosition(0);
      if (mod.major_version >= 3) {
        for (auto &[index, usage] : output_dcl_)
          DeclareOutput(OutputLinkReg(D3DSPR_OUTPUT, index));
      } else {
        for (auto &inst : mod.instructions)
          if (inst.dst && inst.opcode != D3DSIO_DCL &&
              (inst.dst->reg.type == D3DSPR_RASTOUT || inst.dst->reg.type == D3DSPR_ATTROUT ||
               inst.dst->reg.type == D3DSPR_TEXCRDOUT))
            DeclareOutput(OutputLinkReg(inst.dst->reg.type, inst.dst->reg.index));
      }
      for (uint32_t reg = 1; reg < kLinkRegisterCount; reg++)
        if (opts.default_outputs & LinkBit(reg))
          DeclareOutput(reg);
      if (point_size) {
        if (declared_outputs_.count(kLinkPointSize))
          psize_temp_ = ScratchTemp();
        DeclareOutput(kLinkPointSize);
      }
      for (auto &inst : mod.instructions)
        if (inst.dst && inst.dst->reg.type == D3DSPR_INPUT)
          vs_inputs_seen.insert(inst.dst->reg.index);
        else
          for (auto &s : inst.src)
            if (s.reg.type == D3DSPR_INPUT) vs_inputs_seen.insert(s.reg.index);
      for (uint32_t v : vs_inputs_seen) {
        // The ISGN only needs a distinct semantic per register so
        // ExtractMTLInputLayoutElements can bind it; which declaration element
        // feeds the register is decided by the D3D9 usage recorded with it.
        std::string sem = "TEXCOORD";
        b.DeclInput(v, 0xf, sem, v);
        VertexInputSlot slot{v, sem, v, 0xf};
        auto dcl = vs_input_usage.find(v);
        if (dcl != vs_input_usage.end()) {
          slot.d3d9_usage = dcl->second.first;
          slot.d3d9_usage_index = dcl->second.second;
        } else {
          // No dcl: a d3d8-style vs_1_x shader, whose v# registers have the
          // fixed D3DVSDE meanings the d3d8 declaration converter also uses.
          static const std::pair<uint8_t, uint8_t> kD3DVSDE[] = {
            {D3DDECLUSAGE_POSITION, 0}, {D3DDECLUSAGE_BLENDWEIGHT, 0}, {D3DDECLUSAGE_BLENDINDICES, 0},
            {D3DDECLUSAGE_NORMAL, 0},   {D3DDECLUSAGE_PSIZE, 0},       {D3DDECLUSAGE_COLOR, 0},
            {D3DDECLUSAGE_COLOR, 1},    {D3DDECLUSAGE_TEXCOORD, 0},    {D3DDECLUSAGE_TEXCOORD, 1},
            {D3DDECLUSAGE_TEXCOORD, 2}, {D3DDECLUSAGE_TEXCOORD, 3},    {D3DDECLUSAGE_TEXCOORD, 4},
            {D3DDECLUSAGE_TEXCOORD, 5}, {D3DDECLUSAGE_TEXCOORD, 6},    {D3DDECLUSAGE_TEXCOORD, 7},
            {D3DDECLUSAGE_POSITION, 1}, {D3DDECLUSAGE_NORMAL, 1},
          };
          if (v < std::size(kD3DVSDE)) {
            slot.d3d9_usage = kD3DVSDE[v].first;
            slot.d3d9_usage_index = kD3DVSDE[v].second;
          } else {
            slot.d3d9_usage = D3DDECLUSAGE_TEXCOORD;
            slot.d3d9_usage_index = (uint8_t)v;
          }
        }
        result.vertex_inputs.push_back(slot);
      }
    }

    // Pixel shaders below 2.0 have no oC# registers: whatever ends up in r0 is
    // the final color. That also gives the alpha-test epilogue its readable
    // color source; for ps>=2.0 the oC0 writes get redirected into a temp.
    bool ps_legacy_r0_output = mod.is_pixel_shader && mod.major_version < 2;
    if (alpha_test && !ps_legacy_r0_output)
      color_temp_ = ScratchTemp();

    for (uint32_t index : ps_texregs_written_)
      ps_texreg_temp_[index] = ScratchTemp();
    if (!mod.is_pixel_shader && (opts.default_outputs & LinkBit(kLinkFog)) &&
        declared_outputs_.count(kLinkColor0 + 1))
      color1_temp_ = ScratchTemp();
    if (pointcoord_input_)
      pointcoord_temp_ = ScratchTemp();
    if (!mod.is_pixel_shader)
      pos_temp_ = ScratchTemp();
    if (face_input_)
      face_temp_ = ScratchTemp();
    std::optional<uint32_t> dec3n_temp;
    if (!mod.is_pixel_shader) {
      for (uint32_t v : vs_inputs_seen) {
        uint32_t conversion = InputConversionOf(opts.input_conversions, v);
        if (conversion == kInputAsIs)
          continue;
        input_temp_[v] = ScratchTemp();
        if (conversion == kInputDec3N && !dec3n_temp)
          dec3n_temp = ScratchTemp();
      }
    }

    b.ReserveTemps(max_d3d9_temp + next_scratch_temp);
    /* What D3D9 hands the pixel stage for a varying nothing wrote: zero,
     * except that the diffuse colour is white. */
    for (uint32_t reg : declared_outputs_) {
      float v = reg == kLinkColor0 && (mod.major_version < 3 || (opts.default_white & LinkBit(reg))) ? 1.0f
                                                                                                : 0.0f;
      b.Mov(WithMask(Output(reg), LinkMask(reg)), Imm4(v, v, v, v));
    }
    if (color1_temp_)
      b.Mov(Temp(*color1_temp_), Imm4(0, 0, 0, 0));
    if (psize_temp_)
      b.Mov(Temp(*psize_temp_), ConstantBuffer(1, kExtraCBPointSize, 0, 0, 0, 0));
    if (pos_temp_)
      b.Mov(Temp(*pos_temp_), Imm4(0, 0, 0, 0));
    if (pointcoord_temp_) {
      b.Mov(WithMask(Temp(*pointcoord_temp_), 0b0011), Input(*pointcoord_input_, 0, 1, 0, 0));
      b.Mov(WithMask(Temp(*pointcoord_temp_), 0b1100), Imm4(0, 0, 0, 1));
    }
    // Seed each written t# from its interpolator: a read before the first
    // texture instruction still has to see the coordinate.
    for (auto &[index, temp] : ps_texreg_temp_)
      b.Mov(WithMask(Temp(temp), 0b1111), TexcoordIn(index));

    for (auto &[v, temp] : input_temp_)
      EmitInputConversion(b, temp, dec3n_temp.value_or(0), Input(v, 0, 1, 2, 3),
                          InputConversionOf(opts.input_conversions, v));

    /* D3D9's vFace is a signed float, SV_IsFrontFace a 0/~0 boolean. */
    if (face_temp_ && face_input_)
      b.Movc(WithMask(Temp(*face_temp_), 0b1111), Input(*face_input_, 0, 0, 0, 0), Imm4(1, 1, 1, 1),
             Imm4(-1, -1, -1, -1));

    /* Subroutine bodies are emitted at their call sites, so the main pass runs
     * from the top and stops at the first label: D3D9 puts every subroutine
     * after the main body. */
    for (auto &inst : mod.instructions) {
      if (inst.opcode == D3DSIO_LABEL)
        break;
      Emit(inst);
      if (has_error) break;
    }

    if (!has_error && color1_temp_) {
      b.Mov(Output(kLinkColor0 + 1), TempSwizzle(*color1_temp_, 0, 1, 2, 3));
      b.Mov(WithMask(Output(kLinkFog), 0b0001), TempSwizzle(*color1_temp_, 3, 3, 3, 3));
    }
    if (!has_error && pos_temp_ && opts.clip_planes) {
      /* D3D9's user clip planes are in clip space for a programmable vertex
       * stage: the distance to each is its dot product with the position --
       * the one the shader wrote, before the depth bias below moves it. */
      b.DeclOutputClipDistance(kClipDistanceRegister, 0);
      b.DeclOutputClipDistance(kClipDistanceRegister + 1, 1);
      for (uint32_t i = 0; i < 8; i++)
        b.Dp4(WithMask(Output(kClipDistanceRegister + i / 4), uint8_t(1u << (i % 4))),
              TempSwizzle(*pos_temp_, 0, 1, 2, 3), ConstantBuffer(1, kExtraCBClipPlanes + i, 0, 1, 2, 3));
    }
    if (!has_error && pos_temp_) {
      // z + D3DRS_DEPTHBIAS * w: the bias lands on the depth after the divide.
      b.Mov(WithMask(Output(0), 0b1011), TempSwizzle(*pos_temp_, 0, 1, 2, 3));
      b.Mad(WithMask(Output(0), 0b0100), ConstantBuffer(1, kExtraCBDepthBias, 0, 0, 0, 0),
            TempSwizzle(*pos_temp_, 3, 3, 3, 3), TempSwizzle(*pos_temp_, 2, 2, 2, 2));
    }
    if (!has_error && point_size) {
      // clamp(oPts or D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN, D3DRS_POINTSIZE_MAX)
      uint32_t t = ScratchTemp();
      Operand size = psize_temp_ ? TempSwizzle(*psize_temp_, 0, 0, 0, 0)
                                 : ConstantBuffer(1, kExtraCBPointSize, 0, 0, 0, 0);
      b.Max(WithMask(Temp(t), 0b0001), size, ConstantBuffer(1, kExtraCBPointSize, 1, 1, 1, 1));
      b.Min(WithMask(Output(kLinkPointSize), 0b0001), TempSwizzle(t, 0, 0, 0, 0),
            ConstantBuffer(1, kExtraCBPointSize, 2, 2, 2, 2));
    }
    if (!has_error && mod.is_pixel_shader) {
      std::optional<uint32_t> color_src;
      if (ps_legacy_r0_output && max_d3d9_temp > 0)
        color_src = 0; // r0
      else if (color_temp_)
        color_src = *color_temp_;

      /* D3D9 order: pixel shader, then fog, then alpha test. Fog only
       * touches rgb, so the alpha the test reads is the same either way --
       * but the order is the documented one and costs nothing to keep. */
      if (fog && color_src)
        EmitFog(*color_src);
      if (alpha_test && color_src)
        EmitAlphaTest(*color_src);
      if (ps_legacy_r0_output && color_src) {
        b.DeclOutputTarget(0, 0);
        b.Mov(Output(0), TempSwizzle(*color_src, 0, 1, 2, 3));
      } else if (color_temp_) {
        b.Mov(Output(0), TempSwizzle(*color_temp_, 0, 1, 2, 3));
      }
    }
    b.Ret();

    if (has_error) {
      result.ok = false;
      result.error = error;
      return;
    }
    if (if_depth != 0 || !loop_stack_.empty()) {
      result.ok = false;
      result.error = "unbalanced control flow (if/endif or loop/endloop)";
      return;
    }
    // Scratch registers were handed out past the r# range without informing
    // the builder; dcl_temps has to cover them too.
    b.ReserveTemps(max_d3d9_temp + next_scratch_temp);
    result.sampler_count = (uint32_t)samplers_used.size();
    result.ok = b.Finish(result.dxbc);
    if (!result.ok)
      result.error = "sm4 assembly failed";
  }

  // Blends the shader's colour towards the fog colour, which is what the
  // fixed-function fog stage did after the pixel shader on D3D9 hardware.
  //
  //   rgb = fogColour + (rgb - fogColour) * factor
  //
  // written as a subtract and a multiply-add, the same shape the fixed
  // function pixel shader generator uses. The factor arrives interpolated on
  // the FOG input; alpha is left alone, as fog never affected it.
  void EmitFog(uint32_t color_reg) {
    constexpr float kLog2E = 1.442695041f;
    Operand fog_color = ConstantBuffer(1, kExtraCBFogColor);
    Operand params = ConstantBuffer(1, kExtraCBFogParams);
    uint32_t f = ScratchTemp();
    Operand factor;

    if (opts.fog_table_mode == 0) {
      if (!fog_input_)
        return;
      // D3D9 clamps the factor, and a vertex shader writes oFog unclamped.
      b.Mov(Saturate(WithMask(Temp(f), 0b0010)), Input(*fog_input_, 0, 0, 0, 0));
      factor = TempSwizzle(f, 1, 1, 1, 1);
    } else {
      /* Table fog runs per pixel on what FOGSTART and FOGEND are expressed
       * in: the clip-space w under a perspective projection, the depth under
       * an orthographic one. SV_Position, always input register 0, has both. */
      b.Mov(WithMask(Temp(f), 0b0001), opts.fog_w ? Input(0, 3, 3, 3, 3) : Input(0, 2, 2, 2, 2));
      switch (opts.fog_table_mode) {
      case D3DFOG_LINEAR: {
        // f = (end - d) / (end - start)
        uint32_t den = ScratchTemp();
        b.Add(WithMask(Temp(f), 0b0010), WithSwizzle(params, 1, 1, 1, 1), Neg(TempSwizzle(f, 0, 0, 0, 0)));
        b.Add(WithMask(Temp(den), 0b0001), WithSwizzle(params, 1, 1, 1, 1),
              Neg(WithSwizzle(params, 0, 0, 0, 0)));
        b.Rcp(WithMask(Temp(den), 0b0001), TempSwizzle(den, 0, 0, 0, 0));
        b.Mul(Saturate(WithMask(Temp(f), 0b0010)), TempSwizzle(f, 1, 1, 1, 1),
              TempSwizzle(den, 0, 0, 0, 0));
        break;
      }
      case D3DFOG_EXP:
        // f = exp(-d * density), written base 2
        b.Mul(WithMask(Temp(f), 0b0010), TempSwizzle(f, 0, 0, 0, 0), WithSwizzle(params, 2, 2, 2, 2));
        b.Mul(WithMask(Temp(f), 0b0010), TempSwizzle(f, 1, 1, 1, 1),
              Imm4(-kLog2E, -kLog2E, -kLog2E, -kLog2E));
        b.ExpI(Saturate(WithMask(Temp(f), 0b0010)), TempSwizzle(f, 1, 1, 1, 1));
        break;
      default: // D3DFOG_EXP2: f = exp(-(d * density)^2)
        b.Mul(WithMask(Temp(f), 0b0010), TempSwizzle(f, 0, 0, 0, 0), WithSwizzle(params, 2, 2, 2, 2));
        b.Mul(WithMask(Temp(f), 0b0010), TempSwizzle(f, 1, 1, 1, 1), TempSwizzle(f, 1, 1, 1, 1));
        b.Mul(WithMask(Temp(f), 0b0010), TempSwizzle(f, 1, 1, 1, 1),
              Imm4(-kLog2E, -kLog2E, -kLog2E, -kLog2E));
        b.ExpI(Saturate(WithMask(Temp(f), 0b0010)), TempSwizzle(f, 1, 1, 1, 1));
        break;
      }
      factor = TempSwizzle(f, 1, 1, 1, 1);
    }

    uint32_t t = ScratchTemp();
    b.Add(WithMask(Temp(t), 0b0111), TempSwizzle(color_reg, 0, 1, 2, 3), Neg(fog_color));
    b.Mad(WithMask(Temp(color_reg), 0b0111), TempSwizzle(t, 0, 1, 2, 3), factor, fog_color);
  }

  // Discards the fragment when the D3D9 alpha test fails: the discard fires
  // on the *inverse* of the D3DCMP_* pass condition, comparing the final
  // color's alpha against the reference value the device uploads to cb1.
  void EmitAlphaTest(uint32_t color_reg) {
    Operand alpha = TempSwizzle(color_reg, 3, 3, 3, 3);
    Operand ref = WithSwizzle(ConstantBuffer(1, kExtraCBMiscElement), 0, 0, 0, 0);
    if (opts.alpha_test_func == D3DCMP_NEVER) {
      b.Discard(true, Imm1(1.0f));
      return;
    }
    uint32_t t = ScratchTemp();
    Operand tw = WithMask(Temp(t), 0b0001);
    Operand tr = TempSwizzle(t, 0, 0, 0, 0);
    switch (opts.alpha_test_func) {
    case D3DCMP_LESS:         b.Ge(tw, alpha, ref); break; // fail: alpha >= ref
    case D3DCMP_EQUAL:        b.Ne(tw, alpha, ref); break;
    case D3DCMP_LESSEQUAL:    b.Lt(tw, ref, alpha); break;
    case D3DCMP_GREATER:      b.Ge(tw, ref, alpha); break;
    case D3DCMP_NOTEQUAL:     b.Eq(tw, alpha, ref); break;
    case D3DCMP_GREATEREQUAL: b.Lt(tw, alpha, ref); break;
    default: return;
    }
    b.Discard(true, tr);
  }
};

} // namespace

namespace {
dxmt::mutex link_lock;
std::vector<std::pair<uint8_t, uint8_t>> link_dynamic; // kLinkFirstDynamic + i -> (usage, index)
} // namespace

void EmitInputConversion(Sm4Builder &b, uint32_t temp, uint32_t scratch, const Operand &input,
                         uint32_t conversion) {
  if (conversion == kInputAsIs) {
    b.Mov(Temp(temp), input);
    return;
  }
  Operand value = TempSwizzle(temp, 0, 1, 2, 3);
  if (conversion == kInputUDec3 || conversion == kInputDec3N) {
    /* Both arrive as the whole 32-bit word: split it into its ten-bit fields. */
    b.UShr(Temp(temp), WithSwizzle(input, 0, 0, 0, 0), ImmI4(0, 10, 20, 0));
    b.And(Temp(temp), value, ImmI4(0x3ff, 0x3ff, 0x3ff, 0));
    b.Itof(Temp(temp), value);
  } else {
    b.Itof(Temp(temp), input);
  }
  if (conversion == kInputDec3N) {
    /* Ten-bit two's complement over 511: 512..1023 are -512..-1, and -512
     * reads as -1 the way -511 does. */
    Operand offset = TempSwizzle(scratch, 0, 1, 2, 3);
    b.Ge(Temp(scratch), value, Imm4(512, 512, 512, 512));
    b.Movc(Temp(scratch), offset, Imm4(-1024, -1024, -1024, -1024), Imm4(0, 0, 0, 0));
    b.Add(Temp(temp), value, offset);
    b.Mul(Temp(temp), value, Imm4(1 / 511.0f, 1 / 511.0f, 1 / 511.0f, 1 / 511.0f));
    b.Max(Temp(temp), value, Imm4(-1, -1, -1, -1));
  }
  if (conversion == kInputUDec3 || conversion == kInputDec3N)
    b.Mov(WithMask(Temp(temp), 0b1000), Imm4(1, 1, 1, 1));
}

uint32_t LinkRegister(uint8_t usage, uint8_t usage_index) {
  if (usage == D3DDECLUSAGE_POSITIONT)
    usage = D3DDECLUSAGE_POSITION;
  switch (usage) {
  case D3DDECLUSAGE_COLOR:
    if (usage_index < 2) return kLinkColor0 + usage_index;
    break;
  case D3DDECLUSAGE_TEXCOORD:
    if (usage_index < 8) return kLinkTexcoord0 + usage_index;
    break;
  case D3DDECLUSAGE_FOG:
    if (usage_index == 0) return kLinkFog;
    break;
  case D3DDECLUSAGE_PSIZE:
    if (usage_index == 0) return kLinkPointSize;
    break;
  default:
    break;
  }
  std::lock_guard<dxmt::mutex> guard(link_lock);
  for (size_t i = 0; i < link_dynamic.size(); i++)
    if (link_dynamic[i] == std::make_pair(usage, usage_index))
      return kLinkFirstDynamic + (uint32_t)i;
  if (kLinkFirstDynamic + link_dynamic.size() >= kLinkRegisterCount)
    return kLinkNone;
  link_dynamic.emplace_back(usage, usage_index);
  return kLinkFirstDynamic + (uint32_t)link_dynamic.size() - 1;
}

void DeclareLinkIO(sm4::Sm4Builder &b, uint32_t reg) {
  /* The names only label the signature; the register number is what links. */
  if (reg == kLinkColor0 || reg == kLinkColor0 + 1)
    return b.DeclIOWithSemantic(reg, LinkMask(reg), "COLOR", reg - kLinkColor0);
  if (reg >= kLinkTexcoord0 && reg < kLinkTexcoord0 + 8)
    return b.DeclIOWithSemantic(reg, LinkMask(reg), "TEXCOORD", reg - kLinkTexcoord0);
  if (reg == kLinkFog)
    return b.DeclIOWithSemantic(reg, LinkMask(reg), "FOG", 0);
  if (reg == kLinkPointSize) // airconv makes this [[point_size]], not a varying
    return b.DeclIOWithSemantic(reg, LinkMask(reg), "SV_PointSize", 0);
  std::pair<uint8_t, uint8_t> semantic = {D3DDECLUSAGE_TEXCOORD, (uint8_t)(8 + reg)};
  {
    std::lock_guard<dxmt::mutex> guard(link_lock);
    if (reg >= kLinkFirstDynamic && reg - kLinkFirstDynamic < link_dynamic.size())
      semantic = link_dynamic[reg - kLinkFirstDynamic];
  }
  b.DeclIOWithSemantic(reg, LinkMask(reg), UsageSemanticName((D3DDECLUSAGE)semantic.first), semantic.second);
}

TranslationResult Translate(const ShaderModule &module, const TranslateOptions &options) {
  Translator t(module, options);
  t.Run();
  return t.result;
}

} // namespace dxmt::sm3
