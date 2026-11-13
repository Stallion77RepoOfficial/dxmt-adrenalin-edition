#include "d3d9_private_data.hpp"
#include "thread.hpp"
#include <cstring>
#include <mutex>

namespace dxmt {

/* One lock for every store: the calls are rare, and a lock per object would
 * cost every surface and buffer for the few that ever carry private data.
 * References are given back with it released -- the object let go of may be
 * another resource, whose destruction frees its own entries. */
static dxmt::mutex private_data_lock;

D3D9PrivateData::~D3D9PrivateData() {
  for (Entry &entry : entries_) {
    if (entry.object)
      entry.object->Release();
  }
}

D3D9PrivateData::Entry *
D3D9PrivateData::Find(REFGUID guid) {
  for (Entry &entry : entries_) {
    if (entry.guid == guid)
      return &entry;
  }
  return nullptr;
}

HRESULT
D3D9PrivateData::Set(REFGUID guid, const void *data, DWORD size, DWORD flags) {
  Entry entry;
  entry.guid = guid;
  if (flags & D3DSPD_IUNKNOWN) {
    /* The data is the interface pointer itself, and the size must say so. A
     * refused call leaves any entry already under the GUID as it was. */
    if (size != sizeof(IUnknown *))
      return D3DERR_INVALIDCALL;
    entry.object = static_cast<IUnknown *>(const_cast<void *>(data));
    entry.bytes.resize(sizeof(IUnknown *));
    std::memcpy(entry.bytes.data(), &entry.object, sizeof(IUnknown *));
    if (entry.object)
      entry.object->AddRef();
  } else {
    if (size && !data)
      return D3DERR_INVALIDCALL;
    auto bytes = static_cast<const uint8_t *>(data);
    entry.bytes.assign(bytes, bytes + size);
  }

  IUnknown *replaced = nullptr;
  {
    std::lock_guard<dxmt::mutex> lock(private_data_lock);
    if (Entry *existing = Find(guid)) {
      replaced = existing->object;
      *existing = std::move(entry);
    } else {
      entries_.push_back(std::move(entry));
    }
  }
  if (replaced)
    replaced->Release();
  return D3D_OK;
}

HRESULT
D3D9PrivateData::Get(REFGUID guid, void *data, DWORD *size) {
  std::lock_guard<dxmt::mutex> lock(private_data_lock);
  /* A missing entry is reported before size is looked at: a null size is
   * fine for a GUID that has nothing under it. */
  Entry *entry = Find(guid);
  if (!entry)
    return D3DERR_NOTFOUND;
  if (!size)
    return D3DERR_INVALIDCALL;

  DWORD capacity = *size;
  *size = static_cast<DWORD>(entry->bytes.size());
  if (!data)
    return D3D_OK;
  if (capacity < entry->bytes.size())
    return D3DERR_MOREDATA;

  std::memcpy(data, entry->bytes.data(), entry->bytes.size());
  if (entry->object)
    entry->object->AddRef();
  return D3D_OK;
}

HRESULT
D3D9PrivateData::Free(REFGUID guid) {
  IUnknown *released = nullptr;
  {
    std::lock_guard<dxmt::mutex> lock(private_data_lock);
    Entry *entry = Find(guid);
    if (!entry)
      return D3DERR_NOTFOUND;
    released = entry->object;
    entries_.erase(entries_.begin() + (entry - entries_.data()));
  }
  if (released)
    released->Release();
  return D3D_OK;
}

} // namespace dxmt
