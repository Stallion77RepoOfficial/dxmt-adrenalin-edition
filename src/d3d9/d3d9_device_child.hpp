#pragma once

#include "d3d9_private.h"
#include "d3d9_private_data.hpp"
#include "com/com_object.hpp"
#include <atomic>
#include <cstdint>

namespace dxmt {

class D3D9Device;

/* A process-unique identity for caches keyed on objects, never reused. An
 * object's address is no such thing: the memory of a freed declaration goes
 * straight to the next one of the same size, and a cache keyed on the address
 * then hands the dead object's entry to the new one. */
inline uint64_t
NextD3D9ObjectId() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

/* Private references, reachable through a base class.
 *
 * A binding slot that can hold any of the three texture classes cannot name a
 * concrete type, and the COM interfaces have no private count -- so the one
 * thing such a slot needs is virtual. */
class D3D9PrivateRef {
public:
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;

protected:
  ~D3D9PrivateRef() = default;
};

/* The device reference is taken and given back at run time rather than in a
 * constructor, so it cannot be a Com<> member and the device stays incomplete
 * here. Defined in d3d9_device.cpp. */
void D3D9DeviceAddRef(D3D9Device *device);
void D3D9DeviceRelease(D3D9Device *device);

/* Base for every object a D3D9 device hands out.
 *
 * D3D9's rule, which Wine's suite states outright in `device.c`: an object
 * obtained from the device holds a reference on it, and that reference belongs
 * to the object's *public* count rather than to its lifetime. The first AddRef
 * takes it; the release that returns the count to zero gives it back.
 *
 * For an ordinary child the two coincide -- nothing else holds it, so zero is
 * also where it dies. For an implicit one (the back buffer, the auto
 * depth-stencil, the implicit swap chain) the device additionally holds a
 * private reference, so the object survives a count of zero and the next
 * GetRenderTarget hands back the same pointer, which is exactly what D3D9
 * promises and what the suite checks.
 *
 * Tying the device reference to the public count is also what keeps the graph
 * acyclic. The device's own bindings are private references -- D3D9 requires
 * SetTexture, SetIndices and SetStreamSource to leave the public count alone
 * anyway -- so a bound object's count can still fall to zero, let go of the
 * device, and let the device be destroyed while it is still bound. */
template <typename Interface>
class D3D9DeviceChild : public ComObjectClamp<Interface>, public D3D9PrivateRef {

public:
  explicit D3D9DeviceChild(D3D9Device *pDevice) : device_(pDevice) {}

  ULONG STDMETHODCALLTYPE AddRef() override {
    uint32_t refCount = this->m_refCount++;
    if (unlikely(!refCount)) {
      this->AddRefPrivate();
      if (device_)
        D3D9DeviceAddRef(device_);
    }
    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    /* D3D9 clamps rather than wrapping: releasing at zero is legal on an
     * implicit object and returns zero again. */
    ULONG refCount = this->m_refCount;
    if (unlikely(refCount == 0ul))
      return 0ul;

    refCount = --this->m_refCount;
    if (refCount == 0ul) {
      /* ReleasePrivate may destroy this object, so the device is read out
       * first and released afterwards -- and released last, so that whatever
       * the destructor does with the device still has one. */
      D3D9Device *device = device_;
      this->ReleasePrivate();
      if (device)
        D3D9DeviceRelease(device);
    }
    return refCount;
  }

  void AddRefPrivate() final { ComObject<Interface>::AddRefPrivate(); }
  void ReleasePrivate() final { ComObject<Interface>::ReleasePrivate(); }

protected:
  D3D9Device *device_;
};

} // namespace dxmt
