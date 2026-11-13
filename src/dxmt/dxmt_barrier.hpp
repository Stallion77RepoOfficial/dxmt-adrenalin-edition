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

/* D3D12 resource barriers translated to what Metal actually needs.
 *
 * Metal has no resource states. What it has is three separate mechanisms, and
 * picking the right one is the whole job:
 *
 *   nothing         resources with tracked hazards are synchronised by the
 *                   driver across encoders, so most transitions cost nothing.
 *   memory barrier  ordering *within* one encoder, which tracking does not
 *                   provide: two dispatches in the same compute pass writing
 *                   and then reading the same UAV need this.
 *   encoder split   a change that Metal expresses as a different pass: a render
 *                   target becoming a shader resource, or anything touching the
 *                   blit encoder. Ending the encoder is the synchronisation.
 *
 * Untracked resources (which placed resources in a heap generally are) get no
 * driver help across encoders, so a split additionally needs an MTLFence.
 *
 * The state values mirror D3D12_RESOURCE_STATES so the d3d12 layer can pass
 * them straight through; they are duplicated here to keep the core free of
 * D3D headers. */
enum ResourceState : uint32_t {
  ResourceStateCommon = 0,
  ResourceStateVertexAndConstantBuffer = 0x1,
  ResourceStateIndexBuffer = 0x2,
  ResourceStateRenderTarget = 0x4,
  ResourceStateUnorderedAccess = 0x8,
  ResourceStateDepthWrite = 0x10,
  ResourceStateDepthRead = 0x20,
  ResourceStateNonPixelShaderResource = 0x40,
  ResourceStatePixelShaderResource = 0x80,
  ResourceStateStreamOut = 0x100,
  ResourceStateIndirectArgument = 0x200,
  ResourceStateCopyDest = 0x400,
  ResourceStateCopySource = 0x800,
  ResourceStateResolveDest = 0x1000,
  ResourceStateResolveSource = 0x2000,
  ResourceStateShadingRateSource = 0x1000000,
  /* D3D12_RESOURCE_STATE_PRESENT is COMMON. */
  ResourceStatePresent = ResourceStateCommon,
};

/* States that write, and so can be the source of a hazard. */
constexpr uint32_t kWritableResourceStates = ResourceStateRenderTarget | ResourceStateUnorderedAccess |
                                             ResourceStateDepthWrite | ResourceStateStreamOut |
                                             ResourceStateCopyDest | ResourceStateResolveDest;

/* States that only ever appear inside a render pass as an attachment. */
constexpr uint32_t kAttachmentResourceStates =
    ResourceStateRenderTarget | ResourceStateDepthWrite | ResourceStateDepthRead;

/* States serviced by the blit encoder. */
constexpr uint32_t kTransferResourceStates =
    ResourceStateCopyDest | ResourceStateCopySource | ResourceStateResolveDest | ResourceStateResolveSource;

enum class BarrierKind : uint32_t {
  /* Metal's own tracking covers it. */
  None = 0,
  /* Emit a memory barrier into the current encoder. */
  MemoryBarrier = 1,
  /* End the current encoder; the next one starts after this work. */
  EncoderSplit = 2,
};

struct BarrierAction {
  BarrierKind kind;
  /* MemoryBarrier only. */
  WMTBarrierScope scope;
  WMTRenderStages stages_before;
  WMTRenderStages stages_after;
  /* EncoderSplit only: the resource is untracked, so the split must be paired
   * with an MTLFence update and wait. */
  bool needs_fence;

  bool
  operator==(const BarrierAction &other) const {
    return kind == other.kind && scope == other.scope && stages_before == other.stages_before &&
           stages_after == other.stages_after && needs_fence == other.needs_fence;
  }
};

struct BarrierResource {
  bool is_texture;
  /* False for resources created with untracked hazard mode, which is the usual
   * case for anything placed in a heap. */
  bool hazard_tracked;
};

/* D3D12_RESOURCE_BARRIER_TYPE_TRANSITION. */
BarrierAction translateTransitionBarrier(const BarrierResource &resource, uint32_t state_before, uint32_t state_after);

/* D3D12_RESOURCE_BARRIER_TYPE_UAV. Ordering between two dispatches or draws
 * that both touch the resource, with no state change. */
BarrierAction translateUnorderedAccessBarrier(const BarrierResource &resource);

/* D3D12_RESOURCE_BARRIER_TYPE_ALIASING. Two placed resources share memory, so
 * the previous tenant's work must finish before the new one starts. Tracking
 * never covers this: the driver sees two unrelated resources. */
BarrierAction translateAliasingBarrier();

} // namespace dxmt
