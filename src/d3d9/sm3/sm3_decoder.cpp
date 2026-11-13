#include "sm3_decoder.hpp"
#include <cstring>

namespace dxmt::sm3 {

namespace {

D3DSHADER_PARAM_REGISTER_TYPE DecodeRegType(uint32_t token) {
  uint32_t v = ((token & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
               ((token & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
  return (D3DSHADER_PARAM_REGISTER_TYPE)v;
}

DstParam DecodeDst(uint32_t token) {
  DstParam d;
  d.reg.type = DecodeRegType(token);
  d.reg.index = token & D3DSP_REGNUM_MASK;
  d.write_mask = (uint8_t)((token & D3DSP_WRITEMASK_ALL) >> 16);
  uint32_t mod = token & D3DSP_DSTMOD_MASK;
  d.saturate = (mod & D3DSPDM_SATURATE) != 0;
  d.partial_precision = (mod & D3DSPDM_PARTIALPRECISION) != 0;
  d.centroid = (mod & D3DSPDM_MSAMPCENTROID) != 0;
  d.shift_scale = (int8_t)(((int32_t)(token & D3DSP_DSTSHIFT_MASK)) >> D3DSP_DSTSHIFT_SHIFT);
  if (d.shift_scale > 7) d.shift_scale -= 16; // sign-extend the 4-bit field
  return d;
}

SrcParam DecodeSrc(uint32_t token) {
  SrcParam s;
  s.reg.type = DecodeRegType(token);
  s.reg.index = token & D3DSP_REGNUM_MASK;
  s.reg.relative_addressing = (token & (1u << 13)) != 0; // D3DVS_ADDRESSMODE_RELATIVE bit
  uint32_t swiz = (token & D3DVS_SWIZZLE_MASK) >> D3DVS_SWIZZLE_SHIFT;
  s.swizzle[0] = (uint8_t)(swiz & 0x3);
  s.swizzle[1] = (uint8_t)((swiz >> 2) & 0x3);
  s.swizzle[2] = (uint8_t)((swiz >> 4) & 0x3);
  s.swizzle[3] = (uint8_t)((swiz >> 6) & 0x3);
  s.modifier = (D3DSHADER_PARAM_SRCMOD_TYPE)(token & D3DSP_SRCMOD_MASK);
  return s;
}

// Fixed operand shape for opcodes whose encoding never changed across
// versions: number of source parameters, and whether there is a destination.
// Instructions with version-dependent shapes (sincos/sgn legacy extra
// operands) or a completely different token layout (dcl/def/texkill/comment)
// are handled directly in Decode() instead of through this table.
struct Shape {
  bool has_dst;
  uint8_t num_src;
};

bool FixedShape(D3DSHADER_INSTRUCTION_OPCODE_TYPE op, Shape &out) {
  switch (op) {
  case D3DSIO_NOP: out = {false, 0}; return true;
  case D3DSIO_MOV: case D3DSIO_MOVA:
  case D3DSIO_RCP: case D3DSIO_RSQ: case D3DSIO_FRC:
  case D3DSIO_EXP: case D3DSIO_EXPP: case D3DSIO_LOG: case D3DSIO_LOGP:
  case D3DSIO_ABS: case D3DSIO_NRM: case D3DSIO_LIT:
    out = {true, 1}; return true;
  case D3DSIO_ADD: case D3DSIO_SUB: case D3DSIO_MUL:
  case D3DSIO_DP3: case D3DSIO_DP4: case D3DSIO_MIN: case D3DSIO_MAX:
  case D3DSIO_SLT: case D3DSIO_SGE: case D3DSIO_CRS: case D3DSIO_POW:
  case D3DSIO_M4x4: case D3DSIO_M4x3: case D3DSIO_M3x4: case D3DSIO_M3x3: case D3DSIO_M3x2:
  case D3DSIO_DST:
    out = {true, 2}; return true;
  case D3DSIO_MAD: case D3DSIO_LRP: case D3DSIO_CMP: case D3DSIO_CND: case D3DSIO_DP2ADD:
    out = {true, 3}; return true;
  case D3DSIO_DSX: case D3DSIO_DSY:
    out = {true, 1}; return true; // ps_2_x screen-space derivative
  case D3DSIO_TEXREG2AR: case D3DSIO_TEXREG2GB: case D3DSIO_TEXREG2RGB:
  case D3DSIO_TEXDP3: case D3DSIO_TEXDP3TEX:
    out = {true, 1}; return true; // ps_1_x address-mode texture reads
  case D3DSIO_TEXDEPTH:
    out = {true, 0}; return true; // ps_1_4: the "dst" register supplies r/g
  case D3DSIO_TEXBEM: case D3DSIO_TEXBEML:
    out = {true, 1}; return true; // bump environment map
  case D3DSIO_TEXM3x2PAD: case D3DSIO_TEXM3x2TEX: case D3DSIO_TEXM3x2DEPTH:
  case D3DSIO_TEXM3x3PAD: case D3DSIO_TEXM3x3TEX: case D3DSIO_TEXM3x3:
  case D3DSIO_TEXM3x3VSPEC: case D3DSIO_TEXM3x3DIFF:
    out = {true, 1}; return true; // ps_1_x matrix texture ops
  case D3DSIO_TEXM3x3SPEC:
    out = {true, 2}; return true; // ...plus an explicit eye vector
  case D3DSIO_BEM:
    out = {true, 2}; return true; // ps_1_4 bump perturbation without a sample
  case D3DSIO_TEXLDD:
    out = {true, 4}; return true; // ps_3_0: coord, sampler, ddx, ddy
  case D3DSIO_TEXKILL:
    out = {true, 0}; return true; // the "dst" token is really the coordinate register being tested
  case D3DSIO_TEXCOORD:
    out = {true, 0}; return true; // ps <= 1.3 implicit texcoord read (ps_1_4 shape is version-special-cased below)
  case D3DSIO_IF: case D3DSIO_REP:
    out = {false, 1}; return true;
  case D3DSIO_IFC: case D3DSIO_BREAKC:
  case D3DSIO_LOOP: // loop aL, i# - two register operands
    out = {false, 2}; return true;
  case D3DSIO_ELSE: case D3DSIO_ENDIF: case D3DSIO_ENDLOOP: case D3DSIO_ENDREP:
  case D3DSIO_BREAK: case D3DSIO_RET:
    out = {false, 0}; return true;
  default:
    return false;
  }
}

} // namespace

std::optional<ShaderModule> Decode(const uint32_t *tokens, size_t token_count) {
  if (token_count < 2)
    return std::nullopt;

  ShaderModule mod;
  uint32_t version_token = tokens[0];
  uint32_t kind = version_token >> 16;
  if (kind == 0xFFFF)
    mod.is_pixel_shader = true;
  else if (kind == 0xFFFE)
    mod.is_pixel_shader = false;
  else
    return std::nullopt;
  mod.major_version = (version_token >> 8) & 0xff;
  mod.minor_version = version_token & 0xff;
  if (mod.major_version < 1 || mod.major_version > 3)
    return std::nullopt;

  size_t pos = 1;
  bool have_length_field = mod.major_version >= 2;

  while (pos < token_count) {
    uint32_t token = tokens[pos];
    if (token == D3DSIO_END)
      break;

    D3DSHADER_INSTRUCTION_OPCODE_TYPE raw_op =
        (D3DSHADER_INSTRUCTION_OPCODE_TYPE)(token & D3DSI_OPCODE_MASK);

    if (raw_op == D3DSIO_COMMENT) {
      uint32_t size = (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT;
      pos += 1 + size;
      continue;
    }

    size_t inst_start = pos;
    pos++;

    Instruction inst;
    inst.opcode = raw_op;
    inst.control = (token & D3DSP_OPCODESPECIFICCONTROL_MASK) >> D3DSP_OPCODESPECIFICCONTROL_SHIFT;
    inst.coissue = (token & D3DSI_COISSUE) != 0;
    inst.predicated = (token & D3DSHADER_INSTRUCTION_PREDICATED) != 0;

    // SM2.0+ appends one extra token after any operand whose relative-
    // addressing bit is set; it names the offset register (a0 or aL) and the
    // component to read from it. SM1.x never emits the extra token - relative
    // addressing there is always implicitly a0.x, which the RegisterRef
    // defaults already express.
    auto readRelativeToken = [&](RegisterRef &reg) -> bool {
      if (!reg.relative_addressing || mod.major_version < 2)
        return true;
      if (pos >= token_count) return false;
      SrcParam rel = DecodeSrc(tokens[pos++]);
      reg.rel_type = rel.reg.type;
      reg.rel_component = rel.swizzle[0];
      return true;
    };
    auto readDst = [&]() -> bool {
      if (pos >= token_count) return false;
      inst.dst = DecodeDst(tokens[pos++]);
      inst.dst->reg.relative_addressing = (tokens[pos - 1] & (1u << 13)) != 0;
      if (!readRelativeToken(inst.dst->reg))
        return false;
      // A predicated instruction carries its p# predicate source right after
      // the destination.
      if (inst.predicated) {
        if (pos >= token_count) return false;
        inst.predicate = DecodeSrc(tokens[pos++]);
      }
      return true;
    };
    auto readSrc = [&]() -> bool {
      if (pos >= token_count) return false;
      inst.src.push_back(DecodeSrc(tokens[pos++]));
      return readRelativeToken(inst.src.back().reg);
    };

    bool ok = true;
    switch (raw_op) {
    case D3DSIO_DCL: {
      if (pos >= token_count) { ok = false; break; }
      // DCL's first token (usage/usageindex or sampler texture type) sits
      // where the source-modifier token normally would; save it in control.
      inst.control = tokens[pos++];
      ok = readDst();
      break;
    }
    case D3DSIO_DEF: {
      ok = readDst();
      for (int i = 0; i < 4 && ok; i++) {
        if (pos >= token_count) { ok = false; break; }
        std::memcpy(&inst.def_f[i], &tokens[pos++], sizeof(float));
      }
      break;
    }
    case D3DSIO_DEFI: {
      ok = readDst();
      for (int i = 0; i < 4 && ok; i++) {
        if (pos >= token_count) { ok = false; break; }
        inst.def_i[i] = (int32_t)tokens[pos++];
      }
      break;
    }
    case D3DSIO_DEFB: {
      ok = readDst();
      if (ok && pos < token_count)
        inst.def_b = tokens[pos++] != 0;
      else
        ok = false;
      break;
    }
    case D3DSIO_SINCOS: {
      // Shader model 2 carries two extra constant-register sources holding
      // range-reduction constants, from before hardware had sin/cos; 3.0 drops
      // them. This covers both 2_0 and 2_x, and pixel shaders as well as
      // vertex shaders - the ranges are wined3d's, cross-checked in
      // tests/sm3. Only src[0] is ever used.
      ok = readDst() && readSrc();
      if (ok && mod.major_version == 2)
        ok = readSrc() && readSrc();
      break;
    }
    case D3DSIO_SGN: {
      // Same extra operands, and vertex shaders only.
      ok = readDst() && readSrc();
      if (ok && !mod.is_pixel_shader && mod.major_version == 2)
        ok = readSrc() && readSrc();
      break;
    }
    case D3DSIO_TEXCOORD: {
      // ps_1_4 encodes an explicit source register instead of an implicit one.
      ok = readDst();
      if (ok && mod.is_pixel_shader && mod.major_version == 1 && mod.minor_version == 4)
        ok = readSrc();
      break;
    }
    case D3DSIO_TEXKILL: {
      ok = readDst();
      break;
    }
    case D3DSIO_TEX: {
      // ps <= 1.3: implicit sample, dst only. ps_1_4: dst + coord. ps >= 2.0:
      // dst + coord + sampler, with D3DSI_TEXLD_PROJECT/BIAS selecting the
      // texldp/texldb variant (captured in inst.control, already read from
      // the opcode token above).
      ok = readDst();
      if (ok && mod.is_pixel_shader && mod.major_version == 1 && mod.minor_version == 4)
        ok = readSrc();
      else if (ok && !(mod.is_pixel_shader && mod.major_version == 1))
        ok = readSrc() && readSrc();
      break;
    }
    case D3DSIO_TEXLDL: {
      ok = readDst() && readSrc() && readSrc();
      break;
    }
    case D3DSIO_SETP: {
      ok = readDst() && readSrc() && readSrc();
      break;
    }
    case D3DSIO_CALL: case D3DSIO_LABEL: {
      ok = readSrc();
      break;
    }
    case D3DSIO_CALLNZ: {
      ok = readSrc() && readSrc();
      break;
    }
    case D3DSIO_BREAKP: {
      ok = readSrc();
      break;
    }
    case D3DSIO_PHASE: {
      break; // no operands
    }
    default: {
      Shape shape;
      if (!FixedShape(raw_op, shape)) {
        ok = false;
        break;
      }
      if (shape.has_dst)
        ok = readDst();
      for (uint8_t i = 0; ok && i < shape.num_src; i++)
        ok = readSrc();
      break;
    }
    }

    if (!ok)
      return std::nullopt;

    // For version >= 2.0 the instruction token carries an authoritative
    // length; trust it over our own operand-count bookkeeping so the cursor
    // stays correct even for opcodes whose payload we didn't fully model
    // (predication tokens on predicated instructions, for example).
    if (have_length_field) {
      uint32_t declared_len = (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
      size_t declared_end = inst_start + 1 + declared_len;
      if (declared_end < pos || declared_end > token_count)
        return std::nullopt;
      pos = declared_end;
    }

    mod.instructions.push_back(std::move(inst));
  }

  return mod;
}

} // namespace dxmt::sm3
