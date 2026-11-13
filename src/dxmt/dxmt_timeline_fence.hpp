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

#include "Metal.hpp"
#include "rc/util_rc_ptr.hpp"
#include <atomic>
#include <cstdint>

namespace dxmt {

/* ID3D12Fence is a monotonic 64-bit timeline that both the CPU and the GPU can
 * signal and wait on. MTLSharedEvent is the same object, so this is a thin
 * wrapper rather than an emulation. The one thing it adds is the completed
 * value the API promises to expose without a round trip.
 *
 * Ordering across queues comes from the timeline itself, so nothing here needs
 * a lock: MTLSharedEvent is safe to signal and read concurrently. */
class TimelineFence {
public:
  void incRef();
  void decRef();

  static Rc<TimelineFence> create(WMT::Device device, uint64_t initial_value);

  WMT::SharedEvent
  event() const {
    return event_;
  }

  /* ID3D12Fence::GetCompletedValue. Not const: reading the timeline goes
   * through the event object, whose accessors are non-const. */
  uint64_t
  completedValue() {
    return event_.signaledValue();
  }

  /* ID3D12Fence::Signal, the CPU-side one. D3D12 allows any value, including
   * one that moves the timeline backwards; Metal does not, and a backwards
   * signal would deadlock every waiter above it, so it is dropped. */
  void
  signal(uint64_t value) {
    if (value > event_.signaledValue())
      event_.signalValue(value);
  }

  /* ID3D12Fence::SetEventOnCompletion. The listener must stay alive until the
   * value is reached. Passing a null NT handle is a plain no-op, matching a
   * caller that only wanted the completion check. */
  void
  setWin32EventAtValue(WMT::Object listener, void *nt_event_handle, uint64_t value) {
    if (nt_event_handle)
      MTLSharedEvent_setWin32EventAtValue(event_.handle, listener.handle, nt_event_handle, value);
  }

  /* Blocking CPU wait. timeout is in milliseconds; returns false on timeout. */
  bool
  waitUntil(uint64_t value, uint64_t timeout_ms) {
    return event_.waitUntilSignaledValue(value, timeout_ms);
  }

private:
  TimelineFence(WMT::Reference<WMT::SharedEvent> &&event) : event_(std::move(event)) {}

  WMT::Reference<WMT::SharedEvent> event_;
  std::atomic<uint32_t> refcount_ = {0u};
};

} // namespace dxmt
