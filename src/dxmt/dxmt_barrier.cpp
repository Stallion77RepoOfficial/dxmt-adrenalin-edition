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

#include "dxmt_barrier.hpp"

namespace dxmt {

namespace {

BarrierAction
none() {
  return BarrierAction{BarrierKind::None, WMTBarrierScope(0), WMTRenderStages(0), WMTRenderStages(0), false};
}

BarrierAction
split(const BarrierResource &resource) {
  return BarrierAction{
      BarrierKind::EncoderSplit, WMTBarrierScope(0), WMTRenderStages(0), WMTRenderStages(0), !resource.hazard_tracked
  };
}

WMTBarrierScope
scopeFor(const BarrierResource &resource) {
  return resource.is_texture ? WMTBarrierScopeTextures : WMTBarrierScopeBuffers;
}

/* Which render stages can touch a resource in a given state. Compute work is
 * not a render stage, so a compute-only barrier reports no stages and the
 * caller emits the compute encoder's scope-only barrier. */
WMTRenderStages
stagesFor(uint32_t state) {
  uint32_t stages = 0;

  if (state & (ResourceStateVertexAndConstantBuffer | ResourceStateIndexBuffer | ResourceStateIndirectArgument |
               ResourceStateStreamOut))
    stages |= WMTRenderStagePreRaster;
  if (state & ResourceStateNonPixelShaderResource)
    stages |= WMTRenderStagePreRaster;
  if (state & ResourceStatePixelShaderResource)
    stages |= WMTRenderStageFragment;
  if (state & (ResourceStateRenderTarget | ResourceStateDepthWrite | ResourceStateDepthRead))
    stages |= WMTRenderStageFragment;
  /* A UAV can be written from any stage; assume the broadest. */
  if (state & ResourceStateUnorderedAccess)
    stages |= WMTRenderStagePreRaster | WMTRenderStageFragment;

  return WMTRenderStages(stages);
}

} // namespace

BarrierAction
translateTransitionBarrier(const BarrierResource &resource, uint32_t state_before, uint32_t state_after) {
  if (state_before == state_after)
    return none();

  bool writes_before = (state_before & kWritableResourceStates) != 0;
  bool writes_after = (state_after & kWritableResourceStates) != 0;

  /* Read to read is not a hazard, whatever the states say. */
  if (!writes_before && !writes_after)
    return none();

  /* Attachment and blit usage are separate Metal passes, so the encoder has to
   * end regardless of who is tracking hazards. Depth-read to depth-write stays
   * inside one render pass: both are attachment states. */
  bool attachment_before = (state_before & kAttachmentResourceStates) != 0;
  bool attachment_after = (state_after & kAttachmentResourceStates) != 0;
  bool transfer_before = (state_before & kTransferResourceStates) != 0;
  bool transfer_after = (state_after & kTransferResourceStates) != 0;

  if (attachment_before != attachment_after || transfer_before != transfer_after)
    return split(resource);
  if (transfer_before && transfer_after)
    return split(resource);

  /* What is left is shader access changing on both sides, which the driver
   * orders across encoders when it is tracking the resource. */
  if (resource.hazard_tracked)
    return none();

  return BarrierAction{
      BarrierKind::MemoryBarrier, scopeFor(resource), stagesFor(state_before), stagesFor(state_after), false
  };
}

BarrierAction
translateUnorderedAccessBarrier(const BarrierResource &resource) {
  /* Always a real barrier: this orders two commands inside the same encoder,
   * which is exactly the case hazard tracking does not cover. */
  WMTRenderStages stages = stagesFor(ResourceStateUnorderedAccess);

  return BarrierAction{BarrierKind::MemoryBarrier, scopeFor(resource), stages, stages, false};
}

BarrierAction
translateAliasingBarrier() {
  /* The two resources are unrelated as far as Metal is concerned, so tracking
   * cannot help and a fence is always required. */
  return BarrierAction{
      BarrierKind::EncoderSplit, WMTBarrierScope(0), WMTRenderStages(0), WMTRenderStages(0), true
  };
}

} // namespace dxmt
