/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include "winemetal.h"
#include <cstdint>

namespace dxmt {

/* Fixed-function state from D3D to Metal.
 *
 * None of these enumerations line up, so every one is a table rather than a
 * cast. D3D also has values Metal has no equivalent for, and those are reported
 * rather than approximated: silently substituting a different blend factor
 * produces a picture that is wrong in a way nothing reports.
 *
 * The D3D values are written out because this header stays free of D3D headers,
 * and D3D11 and D3D12 use the same numbering for all of these.
 */

/* D3D11_BLEND / D3D12_BLEND. */
enum class D3DBlend : uint32_t {
  Zero = 1,
  One = 2,
  SrcColor = 3,
  InvSrcColor = 4,
  SrcAlpha = 5,
  InvSrcAlpha = 6,
  DestAlpha = 7,
  InvDestAlpha = 8,
  DestColor = 9,
  InvDestColor = 10,
  SrcAlphaSat = 11,
  BlendFactor = 14,
  InvBlendFactor = 15,
  Src1Color = 16,
  InvSrc1Color = 17,
  Src1Alpha = 18,
  InvSrc1Alpha = 19,
};

/* D3D11_BLEND_OP / D3D12_BLEND_OP. */
enum class D3DBlendOp : uint32_t {
  Add = 1,
  Subtract = 2,
  RevSubtract = 3,
  Min = 4,
  Max = 5,
};

/* D3D11_COMPARISON_FUNC / D3D12_COMPARISON_FUNC. */
enum class D3DComparison : uint32_t {
  Never = 1,
  Less = 2,
  Equal = 3,
  LessEqual = 4,
  Greater = 5,
  NotEqual = 6,
  GreaterEqual = 7,
  Always = 8,
};

/* D3D11_STENCIL_OP / D3D12_STENCIL_OP. */
enum class D3DStencilOp : uint32_t {
  Keep = 1,
  Zero = 2,
  Replace = 3,
  IncrSat = 4,
  DecrSat = 5,
  Invert = 6,
  Incr = 7,
  Decr = 8,
};

/* Each returns false for a value with no Metal equivalent, leaving the output
 * untouched, so a caller can refuse the pipeline instead of drawing wrong. */
inline bool
blendFactorFromD3D(uint32_t value, WMTBlendFactor &out) {
  switch (D3DBlend(value)) {
  case D3DBlend::Zero:
    out = WMTBlendFactorZero;
    return true;
  case D3DBlend::One:
    out = WMTBlendFactorOne;
    return true;
  case D3DBlend::SrcColor:
    out = WMTBlendFactorSourceColor;
    return true;
  case D3DBlend::InvSrcColor:
    out = WMTBlendFactorOneMinusSourceColor;
    return true;
  case D3DBlend::SrcAlpha:
    out = WMTBlendFactorSourceAlpha;
    return true;
  case D3DBlend::InvSrcAlpha:
    out = WMTBlendFactorOneMinusSourceAlpha;
    return true;
  case D3DBlend::DestAlpha:
    out = WMTBlendFactorDestinationAlpha;
    return true;
  case D3DBlend::InvDestAlpha:
    out = WMTBlendFactorOneMinusDestinationAlpha;
    return true;
  case D3DBlend::DestColor:
    out = WMTBlendFactorDestinationColor;
    return true;
  case D3DBlend::InvDestColor:
    out = WMTBlendFactorOneMinusDestinationColor;
    return true;
  case D3DBlend::SrcAlphaSat:
    out = WMTBlendFactorSourceAlphaSaturated;
    return true;
  case D3DBlend::BlendFactor:
    out = WMTBlendFactorBlendColor;
    return true;
  case D3DBlend::InvBlendFactor:
    out = WMTBlendFactorOneMinusBlendColor;
    return true;
  case D3DBlend::Src1Color:
    out = WMTBlendFactorSource1Color;
    return true;
  case D3DBlend::InvSrc1Color:
    out = WMTBlendFactorOneMinusSource1Color;
    return true;
  case D3DBlend::Src1Alpha:
    out = WMTBlendFactorSource1Alpha;
    return true;
  case D3DBlend::InvSrc1Alpha:
    out = WMTBlendFactorOneMinusSource1Alpha;
    return true;
  default:
    return false;
  }
}

inline bool
blendOperationFromD3D(uint32_t value, WMTBlendOperation &out) {
  switch (D3DBlendOp(value)) {
  case D3DBlendOp::Add:
    out = WMTBlendOperationAdd;
    return true;
  case D3DBlendOp::Subtract:
    out = WMTBlendOperationSubtract;
    return true;
  case D3DBlendOp::RevSubtract:
    out = WMTBlendOperationReverseSubtract;
    return true;
  case D3DBlendOp::Min:
    out = WMTBlendOperationMin;
    return true;
  case D3DBlendOp::Max:
    out = WMTBlendOperationMax;
    return true;
  default:
    return false;
  }
}

inline bool
compareFunctionFromD3D(uint32_t value, WMTCompareFunction &out) {
  switch (D3DComparison(value)) {
  case D3DComparison::Never:
    out = WMTCompareFunctionNever;
    return true;
  case D3DComparison::Less:
    out = WMTCompareFunctionLess;
    return true;
  case D3DComparison::Equal:
    out = WMTCompareFunctionEqual;
    return true;
  case D3DComparison::LessEqual:
    out = WMTCompareFunctionLessEqual;
    return true;
  case D3DComparison::Greater:
    out = WMTCompareFunctionGreater;
    return true;
  case D3DComparison::NotEqual:
    out = WMTCompareFunctionNotEqual;
    return true;
  case D3DComparison::GreaterEqual:
    out = WMTCompareFunctionGreaterEqual;
    return true;
  case D3DComparison::Always:
    out = WMTCompareFunctionAlways;
    return true;
  default:
    return false;
  }
}

inline bool
stencilOperationFromD3D(uint32_t value, WMTStencilOperation &out) {
  switch (D3DStencilOp(value)) {
  case D3DStencilOp::Keep:
    out = WMTStencilOperationKeep;
    return true;
  case D3DStencilOp::Zero:
    out = WMTStencilOperationZero;
    return true;
  case D3DStencilOp::Replace:
    out = WMTStencilOperationReplace;
    return true;
  case D3DStencilOp::IncrSat:
    out = WMTStencilOperationIncrementClamp;
    return true;
  case D3DStencilOp::DecrSat:
    out = WMTStencilOperationDecrementClamp;
    return true;
  case D3DStencilOp::Invert:
    out = WMTStencilOperationInvert;
    return true;
  /* D3D's INCR and DECR wrap; Metal spells those out separately. */
  case D3DStencilOp::Incr:
    out = WMTStencilOperationIncrementWrap;
    return true;
  case D3DStencilOp::Decr:
    out = WMTStencilOperationDecrementWrap;
    return true;
  default:
    return false;
  }
}

/* One render target's blend state, in D3D terms. */
struct D3DRenderTargetBlend {
  bool blend_enable;
  uint32_t src_blend;
  uint32_t dst_blend;
  uint32_t blend_op;
  uint32_t src_blend_alpha;
  uint32_t dst_blend_alpha;
  uint32_t blend_op_alpha;
  uint8_t write_mask;
};

/* Fills the colour attachment's blend fields. With blending off the factors are
 * not consulted, matching D3D, so a disabled target with nonsense factors still
 * makes a valid pipeline. */
inline bool
colorAttachmentBlendFromD3D(const D3DRenderTargetBlend &blend, WMTColorAttachmentBlendInfo &out) {
  out.blending_enabled = blend.blend_enable;
  out.write_mask = blend.write_mask;

  if (!blend.blend_enable) {
    out.rgb_blend_operation = WMTBlendOperationAdd;
    out.alpha_blend_operation = WMTBlendOperationAdd;
    out.src_rgb_blend_factor = WMTBlendFactorOne;
    out.dst_rgb_blend_factor = WMTBlendFactorZero;
    out.src_alpha_blend_factor = WMTBlendFactorOne;
    out.dst_alpha_blend_factor = WMTBlendFactorZero;
    return true;
  }

  return blendFactorFromD3D(blend.src_blend, out.src_rgb_blend_factor) &&
         blendFactorFromD3D(blend.dst_blend, out.dst_rgb_blend_factor) &&
         blendFactorFromD3D(blend.src_blend_alpha, out.src_alpha_blend_factor) &&
         blendFactorFromD3D(blend.dst_blend_alpha, out.dst_alpha_blend_factor) &&
         blendOperationFromD3D(blend.blend_op, out.rgb_blend_operation) &&
         blendOperationFromD3D(blend.blend_op_alpha, out.alpha_blend_operation);
}

/* Depth-stencil state, in D3D terms. */
struct D3DStencilFace {
  uint32_t fail_op;
  uint32_t depth_fail_op;
  uint32_t pass_op;
  uint32_t func;
};

struct D3DDepthStencil {
  bool depth_enable;
  bool depth_write_all;
  uint32_t depth_func;
  bool stencil_enable;
  uint8_t stencil_read_mask;
  uint8_t stencil_write_mask;
  D3DStencilFace front;
  D3DStencilFace back;
};

inline bool
depthStencilFromD3D(const D3DDepthStencil &state, WMTDepthStencilInfo &out) {
  out = {};

  /* Depth testing off is Always plus no writes; Metal has no separate switch. */
  if (!state.depth_enable) {
    out.depth_compare_function = WMTCompareFunctionAlways;
    out.depth_write_enabled = false;
  } else {
    if (!compareFunctionFromD3D(state.depth_func, out.depth_compare_function))
      return false;
    out.depth_write_enabled = state.depth_write_all;
  }

  auto face = [&](const D3DStencilFace &source, WMTStencilInfo &target) {
    target.enabled = state.stencil_enable;
    target.read_mask = state.stencil_read_mask;
    target.write_mask = state.stencil_write_mask;
    if (!state.stencil_enable) {
      target.stencil_compare_function = WMTCompareFunctionAlways;
      target.stencil_fail_op = WMTStencilOperationKeep;
      target.depth_fail_op = WMTStencilOperationKeep;
      target.depth_stencil_pass_op = WMTStencilOperationKeep;
      return true;
    }
    return compareFunctionFromD3D(source.func, target.stencil_compare_function) &&
           stencilOperationFromD3D(source.fail_op, target.stencil_fail_op) &&
           stencilOperationFromD3D(source.depth_fail_op, target.depth_fail_op) &&
           stencilOperationFromD3D(source.pass_op, target.depth_stencil_pass_op);
  };

  return face(state.front, out.front_stencil) && face(state.back, out.back_stencil);
}

} // namespace dxmt
