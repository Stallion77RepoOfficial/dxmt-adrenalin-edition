#pragma once

// Decoder for D3D9 Shader Model 1-3 bytecode (vs_1_1..vs_3_0, ps_1_1..ps_3_0).
//
// Token bit layout is taken directly from the D3DSIO_*/D3DSP_*/D3DSPR_*
// macros in d3d9types.h (the same values Wine, Mesa and every other D3D9
// reimplementation decode against) - not reverse engineered or guessed.
// Below shader model 2.0 the per-instruction length field is not populated,
// so opcodes are given a fixed operand count (matches how every other D3D9
// bytecode consumer handles versions < 2.0).

#include <windows.h>
#include "d3d9types.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dxmt::sm3 {

struct RegisterRef {
  D3DSHADER_PARAM_REGISTER_TYPE type = D3DSPR_TEMP;
  uint32_t index = 0;
  bool relative_addressing = false; // c[a0.x + index] / c[aL + index]
  // Which register supplies the relative offset. SM2.0+ encodes it in an
  // extra token following the operand (a0 or aL, with a component swizzle);
  // SM1.x has no extra token - relative addressing is always implicitly a0.x.
  D3DSHADER_PARAM_REGISTER_TYPE rel_type = D3DSPR_ADDR;
  uint8_t rel_component = 0;
};

struct DstParam {
  RegisterRef reg;
  uint8_t write_mask = 0xf; // bit0=x bit1=y bit2=z bit3=w
  bool saturate = false;
  bool partial_precision = false;
  bool centroid = false;
  int8_t shift_scale = 0; // ps_1_x only
};

struct SrcParam {
  RegisterRef reg;
  uint8_t swizzle[4] = {0, 1, 2, 3};
  D3DSHADER_PARAM_SRCMOD_TYPE modifier = D3DSPSM_NONE;
};

struct Instruction {
  D3DSHADER_INSTRUCTION_OPCODE_TYPE opcode = D3DSIO_NOP;
  uint32_t control = 0; // opcode-specific control bits [23:16] of the instruction token
  bool coissue = false;
  bool predicated = false; // (p0) / (!p0) prefix
  // The predicate operand of a predicated instruction, carrying its component
  // swizzle and any D3DSPSM_NOT modifier.
  std::optional<SrcParam> predicate;
  std::optional<DstParam> dst;
  std::vector<SrcParam> src;
  // DEF/DEFI/DEFB immediate payload.
  float def_f[4] = {0, 0, 0, 0};
  int32_t def_i[4] = {0, 0, 0, 0};
  bool def_b = false;
};

struct ShaderModule {
  bool is_pixel_shader = false;
  uint32_t major_version = 0;
  uint32_t minor_version = 0;
  std::vector<Instruction> instructions;
};

// Parses raw D3D9 shader bytecode (as passed to CreateVertexShader/
// CreatePixelShader). Returns std::nullopt if the bytecode is structurally
// invalid (bad version token, truncated stream, or an opcode this decoder has
// never heard of) - callers should map that to D3DERR_INVALIDCALL rather than
// guessing at malformed input.
std::optional<ShaderModule> Decode(const uint32_t *tokens, size_t token_count);

} // namespace dxmt::sm3
