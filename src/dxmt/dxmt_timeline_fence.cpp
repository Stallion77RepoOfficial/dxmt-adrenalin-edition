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

#include "dxmt_timeline_fence.hpp"

namespace dxmt {

void
TimelineFence::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
TimelineFence::decRef() {
  /* acq_rel: the thread that frees the object must observe every other
   * thread's writes to it. x86's ordering hid a plain release; arm64 does not. */
  if (refcount_.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
    delete this;
};

Rc<TimelineFence>
TimelineFence::create(WMT::Device device, uint64_t initial_value) {
  auto event = device.newSharedEvent();
  if (!event)
    return nullptr;

  Rc<TimelineFence> ret = new TimelineFence(std::move(event));
  if (initial_value)
    ret->event_.signalValue(initial_value);
  return ret;
}

} // namespace dxmt
