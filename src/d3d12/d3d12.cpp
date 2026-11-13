#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <initguid.h>

#include "dxgi_interfaces.h"

#include "../d3d11/d3d11_device.hpp"
#include "../d3d11/d3d11_on12.hpp"
#include "../d3d11/d3d11_resource.hpp"
#include "../d3d11/d3d11_shader.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_texture.hpp"
#include "Metal.hpp"
#include "dxmt_descriptor_heap.hpp"
#include "dxmt_root_signature.hpp"
#include "dxmt_pipeline_state_map.hpp"
#include "dxmt_vertex_input.hpp"
#include "msc_public.h"
#include "d3d12_state_map.hpp"
#include "dxmt_barrier.hpp"
#include "dxmt_timeline_fence.hpp"
#include "log/log.hpp"
#include "dxmt_format.hpp"
#include "dxmt_statistics.hpp"
#include "sha1/sha1_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <cstdio>
#include <functional>
#include <deque>
#include <map>
#include <limits>
#include <memory>
#include <malloc.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxmt {
/* Each DLL carries its own logger instance; d3d12 needs one now that it links
 * against the shared core, which logs. */
Logger Logger::s_instance("d3d12.log");
} // namespace dxmt

namespace dxmt::d3d12 {
namespace {

static bool d3d12_diagnostics() {
  static const bool enabled = GetEnvironmentVariableA("DXMT_D3D12_DIAGNOSTICS", nullptr, 0) != 0;
  return enabled;
}

static void dump_pipeline_shader(const char *stage, const D3D12_SHADER_BYTECODE &bytecode) {
  static const std::wstring directory = [] {
    WCHAR path[32768];
    DWORD count = GetEnvironmentVariableW(L"DXMT_D3D12_SHADER_DUMP_DIRECTORY", path, std::size(path));
    return count && count < std::size(path) ? std::wstring(path, count) : std::wstring();
  }();
  if (directory.empty() || !bytecode.pShaderBytecode || !bytecode.BytecodeLength ||
      bytecode.BytecodeLength > UINT_MAX)
    return;
  const auto hash = dxmt::Sha1HashState::compute(bytecode.pShaderBytecode, bytecode.BytecodeLength).string();
  const std::string name = std::string(stage) + "_" + hash + ".cso";
  const std::wstring path = directory + L"\\" + std::wstring(name.begin(), name.end());
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  WriteFile(file, bytecode.pShaderBytecode, DWORD(bytecode.BytecodeLength), &written, nullptr);
  CloseHandle(file);
}

static DWORD query_readback_delay() {
  static const DWORD delay = [] {
    char value[16] = {};
    GetEnvironmentVariableA("DXMT_D3D12_QUERY_READBACK_DELAY_MS", value, sizeof(value));
    return DWORD(std::min(2000ul, std::strtoul(value, nullptr, 10)));
  }();
  return delay;
}

struct BridgeCpuStatistics {
  std::atomic<uint64_t> query_jobs = 0, query_queue_ticks = 0, query_wait_ticks = 0,
                        query_cpu_ticks = 0, query_prepare_ticks = 0, gpu_barriers = 0,
                        execute_calls = 0, command_lists = 0, record_draw_ticks = 0,
                        legacy_draw_ticks = 0, legacy_bind_ticks = 0, gpu_va_ticks = 0,
                        gpu_va_lookups = 0, gpu_va_steps = 0, replay_ticks = 0, execute_ticks = 0,
                        map_ticks = 0, indirect_ticks = 0;
};
BridgeCpuStatistics &bridge_cpu_statistics() { static BridgeCpuStatistics stats; return stats; }

class PresentTrace {
public:
  PresentTrace() {
    WCHAR path[32768];
    DWORD count = GetEnvironmentVariableW(L"DXMT_D3D12_FRAME_TRACE", path, std::size(path));
    if (count && count < std::size(path)) file_ = _wfopen(path, L"w");
    if (file_) {
      QueryPerformanceFrequency(&frequency_);
      std::fprintf(file_, "swapchain,frame,start_ms,end_ms,present_ms,hresult,encode_ms,sync_ms,commit_ms,present_latency_ms,async_skips,render_passes,blit_passes,compute_passes,command_buffers,slow_commands,query_jobs,query_queue_ms,query_wait_ms,query_cpu_ms,query_prepare_ms,gpu_barriers,execute_calls,command_lists,record_draw_ms,legacy_draw_ms,legacy_bind_ms,gpu_va_ms,gpu_va_lookups,gpu_va_steps,replay_ms,execute_ms,map_ms,indirect_ms,foreground,sync_interval\n");
    }
  }
  ~PresentTrace() { if (file_) std::fclose(file_); }
  bool enabled() const { return file_ != nullptr; }
  LONGLONG begin() const {
    if (!file_) return 0;
    LARGE_INTEGER tick; QueryPerformanceCounter(&tick); return tick.QuadPart;
  }
  void end(const void *chain, LONGLONG start, HRESULT hr, const dxmt::FrameStatistics &stats, int foreground, UINT sync_interval) {
    if (!start) return;
    LARGE_INTEGER tick; QueryPerformanceCounter(&tick);
    const double scale = 1000.0 / frequency_.QuadPart;
    std::lock_guard<std::mutex> lock(mutex_);
    auto &bridge = bridge_cpu_statistics();
    std::fprintf(file_, "%p,%llu,%.6f,%.6f,%.6f,%08x,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%u,%u,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%llu,%llu", chain,
                 (unsigned long long)frame_++, start * scale, tick.QuadPart * scale,
                 (tick.QuadPart - start) * scale, unsigned(hr),
                 stats.encode_prepare_interval.count() / 1000000.0, stats.sync_interval.count() / 1000000.0,
                 stats.commit_interval.count() / 1000000.0, stats.present_latency_interval.count() / 1000000.0,
                 stats.async_pipeline_skips, stats.render_pass_count, stats.blit_pass_count, stats.compute_pass_count,
                 stats.command_buffer_count, (unsigned long long)stats.slow_command_count,
                 (unsigned long long)bridge.query_jobs.exchange(0), bridge.query_queue_ticks.exchange(0) * scale,
                 bridge.query_wait_ticks.exchange(0) * scale, bridge.query_cpu_ticks.exchange(0) * scale,
                 bridge.query_prepare_ticks.exchange(0) * scale, (unsigned long long)bridge.gpu_barriers.exchange(0),
                 (unsigned long long)bridge.execute_calls.exchange(0), (unsigned long long)bridge.command_lists.exchange(0),
                 bridge.record_draw_ticks.exchange(0) * scale, bridge.legacy_draw_ticks.exchange(0) * scale,
                 bridge.legacy_bind_ticks.exchange(0) * scale, bridge.gpu_va_ticks.exchange(0) * scale,
                 (unsigned long long)bridge.gpu_va_lookups.exchange(0), (unsigned long long)bridge.gpu_va_steps.exchange(0));
    std::fprintf(file_, ",%.6f,%.6f,%.6f,%.6f,%d,%u\n",
                 bridge.replay_ticks.exchange(0) * scale, bridge.execute_ticks.exchange(0) * scale,
                 bridge.map_ticks.exchange(0) * scale, bridge.indirect_ticks.exchange(0) * scale,
                 foreground, sync_interval);
    if (frame_ % 60 == 0) std::fflush(file_);
  }
private:
  FILE *file_ = nullptr;
  LARGE_INTEGER frequency_ = {};
  uint64_t frame_ = 0;
  std::mutex mutex_;
};
PresentTrace &present_trace() { static PresentTrace trace; return trace; }

class CpuTimingScope {
public:
  explicit CpuTimingScope(std::atomic<uint64_t> &counter) : counter_(counter), start_(present_trace().begin()) {}
  ~CpuTimingScope() { if (start_) counter_.fetch_add(present_trace().begin() - start_); }
private:
  std::atomic<uint64_t> &counter_;
  LONGLONG start_;
};

template <typename T> void add_ref(T *ptr) {
  if (ptr)
    ptr->AddRef();
}

template <typename T> void release(T *&ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

template <typename T> class ComPtr {
public:
  ComPtr() = default;
  ComPtr(T *ptr) : ptr_(ptr) { add_ref(ptr_); }
  ComPtr(const ComPtr &other) : ptr_(other.ptr_) { add_ref(ptr_); }
  ComPtr(ComPtr &&other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
  ~ComPtr() { release(ptr_); }

  ComPtr &operator=(const ComPtr &other) {
    if (this == &other)
      return *this;
    T *next = other.ptr_;
    add_ref(next);
    release(ptr_);
    ptr_ = next;
    return *this;
  }

  ComPtr &operator=(ComPtr &&other) noexcept {
    if (this == &other)
      return *this;
    release(ptr_);
    ptr_ = other.ptr_;
    other.ptr_ = nullptr;
    return *this;
  }

  T *get() const { return ptr_; }
  T **put() {
    release(ptr_);
    return &ptr_;
  }
  T *detach() {
    T *ret = ptr_;
    ptr_ = nullptr;
    return ret;
  }
  T *operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

private:
  T *ptr_ = nullptr;
};

class UnknownRef {
public:
  ULONG add_ref() { return ++refcount_; }
  ULONG release_ref() {
    ULONG ref = --refcount_;
    if (!ref)
      delete this;
    return ref;
  }

protected:
  virtual ~UnknownRef() = default;

private:
  std::atomic<ULONG> refcount_{1};
};

class Blob final : public ID3DBlob, public UnknownRef {
public:
  explicit Blob(SIZE_T size) : data_(size ? size : 1) {}
  Blob(const void *data, SIZE_T size) : data_(size ? size : 1) {
    if (data && size)
      std::memcpy(data_.data(), data, size);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3DBlob) ||
        riid == __uuidof(ID3D10Blob)) {
      *ppvObject = static_cast<ID3DBlob *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  void *STDMETHODCALLTYPE GetBufferPointer() final { return data_.data(); }
  SIZE_T STDMETHODCALLTYPE GetBufferSize() final { return data_.size(); }

private:
  std::vector<uint8_t> data_;
};

/* Both descriptions own their arrays. Pointers returned by either interface
 * stay valid until the reader is released, including after version conversion. */
class RootSignatureDeserializer final : public ID3D12RootSignatureDeserializer,
                                        public ID3D12VersionedRootSignatureDeserializer,
                                        public UnknownRef {
public:
  bool init(const void *blob, SIZE_T size) {
    dxmt::RootSignatureDescription parsed;
    if (!dxmt::parseSerializedRootSignature(blob, size, parsed))
      return false;
    original_version_ = parsed.version == 1 ? D3D_ROOT_SIGNATURE_VERSION_1_0 : D3D_ROOT_SIGNATURE_VERSION_1_1;
    ranges10_.resize(parsed.ranges.size());
    ranges11_.resize(parsed.ranges.size());
    for (size_t i = 0; i < parsed.ranges.size(); ++i) {
      const auto &r = parsed.ranges[i];
      ranges10_[i] = {D3D12_DESCRIPTOR_RANGE_TYPE(r.range_type), r.descriptor_count, r.base_shader_register,
                      r.register_space, r.offset_in_descriptors_from_table_start};
      ranges11_[i] = {D3D12_DESCRIPTOR_RANGE_TYPE(r.range_type), r.descriptor_count, r.base_shader_register,
                      r.register_space, D3D12_DESCRIPTOR_RANGE_FLAGS(r.flags), r.offset_in_descriptors_from_table_start};
    }
    parameters10_.resize(parsed.parameters.size());
    parameters11_.resize(parsed.parameters.size());
    for (size_t i = 0; i < parsed.parameters.size(); ++i) {
      const auto &p = parsed.parameters[i];
      auto &a = parameters10_[i];
      auto &b = parameters11_[i];
      a.ParameterType = b.ParameterType = D3D12_ROOT_PARAMETER_TYPE(p.kind);
      a.ShaderVisibility = b.ShaderVisibility = D3D12_SHADER_VISIBILITY(p.shader_visibility);
      switch (p.kind) {
      case dxmt::RootParameterKind::DescriptorTable:
        a.DescriptorTable = {p.range_count, p.range_count ? ranges10_.data() + p.first_range : nullptr};
        b.DescriptorTable = {p.range_count, p.range_count ? ranges11_.data() + p.first_range : nullptr};
        break;
      case dxmt::RootParameterKind::Constants:
        a.Constants = b.Constants = {p.shader_register, p.register_space, p.num_32bit_values};
        break;
      default:
        a.Descriptor = {p.shader_register, p.register_space};
        b.Descriptor = {p.shader_register, p.register_space, D3D12_ROOT_DESCRIPTOR_FLAGS(p.descriptor_flags)};
        break;
      }
    }
    for (const auto &s : parsed.static_samplers) {
      samplers_.push_back({D3D12_FILTER(s.filter), D3D12_TEXTURE_ADDRESS_MODE(s.address_u),
          D3D12_TEXTURE_ADDRESS_MODE(s.address_v), D3D12_TEXTURE_ADDRESS_MODE(s.address_w), s.mip_lod_bias,
          s.max_anisotropy, D3D12_COMPARISON_FUNC(s.comparison_func), D3D12_STATIC_BORDER_COLOR(s.border_color),
          s.min_lod, s.max_lod, s.shader_register, s.register_space, D3D12_SHADER_VISIBILITY(s.shader_visibility)});
    }
    desc10_.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc10_.Desc_1_0 = {UINT(parameters10_.size()), parameters10_.data(), UINT(samplers_.size()), samplers_.data(),
                        D3D12_ROOT_SIGNATURE_FLAGS(parsed.flags)};
    desc11_.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc11_.Desc_1_1 = {UINT(parameters11_.size()), parameters11_.data(), UINT(samplers_.size()), samplers_.data(),
                        D3D12_ROOT_SIGNATURE_FLAGS(parsed.flags)};
    return true;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) final {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12RootSignatureDeserializer))
      *object = static_cast<ID3D12RootSignatureDeserializer *>(this);
    else if (iid == __uuidof(ID3D12VersionedRootSignatureDeserializer))
      *object = static_cast<ID3D12VersionedRootSignatureDeserializer *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  const D3D12_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE GetRootSignatureDesc() final {
    return &desc10_.Desc_1_0;
  }
  HRESULT STDMETHODCALLTYPE GetRootSignatureDescAtVersion(
      D3D_ROOT_SIGNATURE_VERSION version, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC **out) final {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (version == D3D_ROOT_SIGNATURE_VERSION_1_0) *out = &desc10_;
    else if (version == D3D_ROOT_SIGNATURE_VERSION_1_1) *out = &desc11_;
    else return E_INVALIDARG;
    return S_OK;
  }
  const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *STDMETHODCALLTYPE GetUnconvertedRootSignatureDesc() final {
    return original_version_ == D3D_ROOT_SIGNATURE_VERSION_1_0 ? &desc10_ : &desc11_;
  }
private:
  D3D_ROOT_SIGNATURE_VERSION original_version_ = D3D_ROOT_SIGNATURE_VERSION_1_0;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc10_ = {}, desc11_ = {};
  std::vector<D3D12_DESCRIPTOR_RANGE> ranges10_;
  std::vector<D3D12_DESCRIPTOR_RANGE1> ranges11_;
  std::vector<D3D12_ROOT_PARAMETER> parameters10_;
  std::vector<D3D12_ROOT_PARAMETER1> parameters11_;
  std::vector<D3D12_STATIC_SAMPLER_DESC> samplers_;
};

class DxmtD3D12Device;
class DxmtD3D12Resource;
class DxmtD3D12DescriptorHeap;
class DxmtD3D12PipelineState;
class DxmtD3D12GraphicsCommandList;
class DxmtD3D12CommandQueue;
class DxmtD3D12SwapChainFactory;
class DxmtD3D12DXGIDevice;
class QueryResolveWorker;

struct CpuReadbackDependency {
  WMT::Reference<WMT::SharedEvent> event;
  uint64_t value = 0;
};

enum class DescriptorType {
  Empty,
  Cbv,
  Srv,
  Uav,
  Rtv,
  Dsv,
  Sampler,
};

struct Descriptor {
  DescriptorType type = DescriptorType::Empty;
  ComPtr<DxmtD3D12Resource> resource;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11DepthStencilView> dsv;
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
  dxmt::DescriptorEntry gpu_entry = {};
  WMT::Reference<WMT::Texture> texture_view;
  /* Shader-visible descriptors also live in a Metal buffer the shaders index;
   * these locate this descriptor's slot in it. */
  DxmtD3D12DescriptorHeap *owner = nullptr;
  uint32_t index = 0;
  /* Samplers are Metal objects in their own right and have to stay alive for
   * as long as the descriptor references them. */
  WMT::Reference<WMT::SamplerState> sampler;
  ComPtr<ID3D11SamplerState> sampler11;

  /* Copies the descriptor's contents without disturbing its identity: an
   * application copying descriptors between heaps must not move their slots. */
  void
  assign_contents(const Descriptor &other) {
    type = other.type;
    resource = other.resource;
    rtv = other.rtv;
    dsv = other.dsv;
    srv = other.srv;
    uav = other.uav;
    cbv = other.cbv;
    gpu_entry = other.gpu_entry;
    texture_view = other.texture_view;
    sampler = other.sampler;
    sampler11 = other.sampler11;
  }
};

class D3D12ObjectMethods {
public:
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) {
    return DXGI_ERROR_NOT_FOUND;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *) { return S_OK; }
};

class D3D12ChildMethods : public D3D12ObjectMethods {
public:
  explicit D3D12ChildMethods(DxmtD3D12Device *device) : device_(device) {}
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device);

protected:
  ComPtr<DxmtD3D12Device> device_;
};

#define DXMT_D3D12_OBJECT_OVERRIDES                                             \
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,        \
                                           void *data) final {                   \
    return D3D12ChildMethods::GetPrivateData(guid, data_size, data);             \
  }                                                                              \
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,         \
                                           const void *data) final {             \
    return D3D12ChildMethods::SetPrivateData(guid, data_size, data);             \
  }                                                                              \
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,                \
                                                    const IUnknown *data) final { \
    return D3D12ChildMethods::SetPrivateDataInterface(guid, data);               \
  }                                                                              \
  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) final {                   \
    return D3D12ChildMethods::SetName(name);                                     \
  }                                                                              \
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) final {        \
    return D3D12ChildMethods::GetDevice(riid, device);                           \
  }

class DxmtD3D12CommandSignature final : public ID3D12CommandSignature,
                                      public D3D12ChildMethods, public UnknownRef {
public:
  DxmtD3D12CommandSignature(DxmtD3D12Device *device, D3D12_INDIRECT_ARGUMENT_TYPE type,
                           UINT stride, UINT argument_size)
      : D3D12ChildMethods(device), type_(type), stride_(stride), argument_size_(argument_size) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) final {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Object) &&
        iid != __uuidof(ID3D12DeviceChild) && iid != __uuidof(ID3D12Pageable) &&
        iid != __uuidof(ID3D12CommandSignature)) return E_NOINTERFACE;
    *out = static_cast<ID3D12CommandSignature *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES
  D3D12_INDIRECT_ARGUMENT_TYPE type() const { return type_; }
  UINT stride() const { return stride_; }
  UINT argument_size() const { return argument_size_; }
private:
  D3D12_INDIRECT_ARGUMENT_TYPE type_;
  UINT stride_, argument_size_;
};

class DxmtD3D12QueryHeap final : public ID3D12QueryHeap,
                               public D3D12ChildMethods, public UnknownRef {
public:
  DxmtD3D12QueryHeap(DxmtD3D12Device *device, ID3D11Device *native, const D3D12_QUERY_HEAP_DESC &desc)
      : D3D12ChildMethods(device), native_(native), desc_(desc), queries_(desc.Count) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) final {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Object) &&
        iid != __uuidof(ID3D12DeviceChild) && iid != __uuidof(ID3D12Pageable) &&
        iid != __uuidof(ID3D12QueryHeap)) return E_NOINTERFACE;
    *out = static_cast<ID3D12QueryHeap *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES
  bool accepts(D3D12_QUERY_TYPE type, UINT index) const {
    return index < desc_.Count && ((type == D3D12_QUERY_TYPE_TIMESTAMP &&
        (desc_.Type == D3D12_QUERY_HEAP_TYPE_TIMESTAMP || desc_.Type == D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP)) ||
        ((type == D3D12_QUERY_TYPE_OCCLUSION || type == D3D12_QUERY_TYPE_BINARY_OCCLUSION) &&
         desc_.Type == D3D12_QUERY_HEAP_TYPE_OCCLUSION));
  }
  ID3D11Query *query(UINT index, bool fresh = false) {
    if (index >= queries_.size()) return nullptr;
    if (fresh) queries_[index] = nullptr;
    if (!queries_[index]) {
      D3D11_QUERY_DESC q = {};
      q.Query = desc_.Type == D3D12_QUERY_HEAP_TYPE_OCCLUSION ? D3D11_QUERY_OCCLUSION : D3D11_QUERY_TIMESTAMP;
      if (FAILED(native_->CreateQuery(&q, queries_[index].put()))) return nullptr;
    }
    return queries_[index].get();
  }
private:
  ComPtr<ID3D11Device> native_;
  D3D12_QUERY_HEAP_DESC desc_;
  std::vector<ComPtr<ID3D11Query>> queries_;
};

class DxmtD3D12RootSignature final : public ID3D12RootSignature,
                                     public D3D12ChildMethods,
                                     public UnknownRef {
public:
  explicit DxmtD3D12RootSignature(DxmtD3D12Device *device)
      : D3D12ChildMethods(device) {}

  /* Fails only if the blob is not a root signature; an application is entitled
   * to a clear E_INVALIDARG rather than a device that renders nothing. */
  bool parse(const void *blob, SIZE_T size) {
    if (!dxmt::parseSerializedRootSignature(blob, size, description_))
      return false;
    ir_builder_.build(description_);
    return initialize_samplers();
  }

  const dxmt::RootSignatureDescription &description() const { return description_; }
  const dxmt::RootSignatureLayout &layout() const { return description_.layout; }
  const std::vector<ComPtr<ID3D11SamplerState>> &samplers() const { return samplers_; }
  bool initialize_samplers();

  /* The shader converter wants the root signature as a
   * D3D12_VERSIONED_ROOT_SIGNATURE_DESC, which is the layout its own
   * IRVersionedRootSignatureDescriptor uses. A shader that carries its root
   * signature inline does not need this, but one compiled against a separate
   * root signature object does. */
  const dxmt::IRVersionedRootSignatureDescriptorMirror *converter_descriptor() const {
    return ir_builder_.descriptor();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12RootSignature)) {
      *ppvObject = static_cast<ID3D12RootSignature *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES

private:
  dxmt::RootSignatureDescription description_;
  dxmt::IRRootSignatureBuilder ir_builder_;
  std::vector<ComPtr<ID3D11SamplerState>> samplers_;
};

class DxmtD3D12Fence final : public ID3D12Fence,
                             public D3D12ChildMethods,
                             public UnknownRef {
public:
  DxmtD3D12Fence(DxmtD3D12Device *device, UINT64 value)
      : D3D12ChildMethods(device), value_(value) {}

  /* A real MTLSharedEvent timeline, when the device could provide one. D3D12
   * fences are shared between the CPU and the GPU, and without this the value
   * only ever moves when the CPU sets it, so a queue signal is invisible. */
  void adopt(Rc<dxmt::TimelineFence> &&timeline) { timeline_ = std::move(timeline); }
  dxmt::TimelineFence *timeline() const { return timeline_.ptr(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12Fence)) {
      *ppvObject = static_cast<ID3D12Fence *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES

  UINT64 STDMETHODCALLTYPE GetCompletedValue() final {
    return timeline_.ptr() ? timeline_->completedValue() : value_.load();
  }
  /* Signalling the event straight away would be the same as not waiting at
   * all, so this is attached to the timeline and left to fire when the value
   * is actually reached. */
  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) final;
  HRESULT STDMETHODCALLTYPE Signal(UINT64 value) final;
  bool scheduled(UINT64 value) const { return scheduled_.load() >= value; }
  void mark_scheduled(UINT64 value) {
    auto previous = scheduled_.load();
    while (previous < value && !scheduled_.compare_exchange_weak(previous, value)) {}
  }

private:
  std::atomic<UINT64> value_;
  std::atomic<UINT64> scheduled_{0};
  Rc<dxmt::TimelineFence> timeline_;
};

class DxmtD3D12CommandAllocator final : public ID3D12CommandAllocator,
                                        public D3D12ChildMethods,
                                        public UnknownRef {
public:
  explicit DxmtD3D12CommandAllocator(DxmtD3D12Device *device)
      : D3D12ChildMethods(device) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandAllocator)) {
      *ppvObject = static_cast<ID3D12CommandAllocator *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES
  HRESULT STDMETHODCALLTYPE Reset() final { return S_OK; }
};

struct FormatBlock {
  UINT width = 1, height = 1, bytes = 0;
};

/* Upload footprints describe the DXGI storage, independently of the Metal
 * view used to render it. Unsupported video formats are rejected. */
FormatBlock format_block(DXGI_FORMAT f) {
  if (f >= DXGI_FORMAT_R32G32B32A32_TYPELESS && f <= DXGI_FORMAT_R32G32B32A32_SINT) return {1, 1, 16};
  if (f >= DXGI_FORMAT_R32G32B32_TYPELESS && f <= DXGI_FORMAT_R32G32B32_SINT) return {1, 1, 12};
  if (f >= DXGI_FORMAT_R16G16B16A16_TYPELESS && f <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) return {1, 1, 8};
  if (f >= DXGI_FORMAT_R10G10B10A2_TYPELESS && f <= DXGI_FORMAT_X24_TYPELESS_G8_UINT) return {1, 1, 4};
  if (f >= DXGI_FORMAT_R8G8_TYPELESS && f <= DXGI_FORMAT_R16_SINT) return {1, 1, 2};
  if (f >= DXGI_FORMAT_R8_TYPELESS && f <= DXGI_FORMAT_A8_UNORM) return {1, 1, 1};
  if (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) return {4, 4, 8};
  if (f >= DXGI_FORMAT_BC2_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB) return {4, 4, 16};
  if (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM) return {4, 4, 8};
  if (f >= DXGI_FORMAT_BC5_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) return {4, 4, 16};
  if (f >= DXGI_FORMAT_B8G8R8A8_UNORM && f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return {1, 1, 4};
  if (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB) return {4, 4, 16};
  switch (f) {
  case DXGI_FORMAT_R1_UNORM: return {8, 1, 1};
  case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return {1, 1, 4};
  case DXGI_FORMAT_R8G8_B8G8_UNORM:
  case DXGI_FORMAT_G8R8_G8B8_UNORM: return {2, 1, 4};
  case DXGI_FORMAT_B5G6R5_UNORM:
  case DXGI_FORMAT_B5G5R5A1_UNORM:
  case DXGI_FORMAT_B4G4R4A4_UNORM: return {1, 1, 2};
  default: return {};
  }
}

UINT bytes_per_pixel(DXGI_FORMAT format) {
  return format_block(format).bytes;
}

UINT align_uint(UINT value, UINT alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

/* Mip levels and array slices as D3D12 counts them. A texture with either has
 * one subresource per combination, indexed
 *
 *   mip + slice * MipLevels + plane * MipLevels * ArraySize
 *
 * and Metal addresses the same thing as a level and a slice, so translating a
 * copy or a readback is a matter of taking that index apart. */
struct SubresourceLocation {
  uint32_t mip;
  uint32_t slice;
  uint32_t plane;
};

UINT subresource_mip_levels(const D3D12_RESOURCE_DESC &desc) {
  return std::max<UINT>(1, desc.MipLevels);
}

UINT subresource_array_size(const D3D12_RESOURCE_DESC &desc) {
  /* DepthOrArraySize is the depth for a 3D texture, which has one subresource
   * per mip however deep it is. */
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : std::max<UINT>(1, desc.DepthOrArraySize);
}

UINT subresource_count(const D3D12_RESOURCE_DESC &desc) {
  return subresource_mip_levels(desc) * subresource_array_size(desc);
}

bool decompose_subresource(const D3D12_RESOURCE_DESC &desc, UINT index, SubresourceLocation &out) {
  UINT mips = subresource_mip_levels(desc);
  UINT slices = subresource_array_size(desc);
  if (index >= mips * slices)
    return false;
  out.mip = index % mips;
  out.slice = (index / mips) % slices;
  out.plane = index / (mips * slices);
  return true;
}

/* Each mip halves, rounding up, and never goes below one. */
UINT mip_extent(UINT base, UINT mip) {
  UINT value = base >> mip;
  return value ? value : 1;
}

WMTSize mip_size(const D3D12_RESOURCE_DESC &desc, UINT mip) {
  UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                   ? mip_extent(std::max<UINT>(1, desc.DepthOrArraySize), mip)
                   : 1;
  return {mip_extent(UINT(desc.Width), mip), mip_extent(std::max<UINT>(1, desc.Height), mip), depth};
}

/* ID3D12Heap is an MTLHeap in placement mode: a block of memory that resources
 * are created into at explicit offsets, which is exactly what D3D12 placed
 * resources are. The heap is created by the device, which owns the Metal
 * device handle, and handed over complete. */
class DxmtD3D12Heap final : public ID3D12Heap, public D3D12ChildMethods, public UnknownRef {
public:
  DxmtD3D12Heap(DxmtD3D12Device *device, const D3D12_HEAP_DESC &desc, WMT::Reference<WMT::Heap> &&heap) :
      D3D12ChildMethods(device),
      desc_(desc),
      heap_(std::move(heap)) {}

  ~DxmtD3D12Heap() {
    cpu_buffer_ = nullptr;
    if (cpu_memory_) _aligned_free(cpu_memory_);
  }

  WMT::Reference<WMT::Buffer> cpu_buffer(WMT::Device device, UINT64 offset, UINT64 length,
                                        WMTBufferInfo &info) {
    if (offset > desc_.SizeInBytes || length > desc_.SizeInBytes - offset) return {};
    std::lock_guard lock(cpu_mutex_);
    if (!cpu_buffer_) {
      if (desc_.SizeInBytes > SIZE_MAX - 65535) return {};
      size_t bytes = (size_t(desc_.SizeInBytes) + 65535) & ~size_t(65535);
      cpu_memory_ = _aligned_malloc(bytes, 65536);
      if (!cpu_memory_) return {};
      WMTBufferInfo backing = {};
      backing.length = bytes;
      backing.options = resource_options();
      backing.memory.set(cpu_memory_);
      cpu_buffer_ = device.newBuffer(backing);
      cpu_address_ = backing.gpu_address;
      if (!cpu_buffer_) { _aligned_free(cpu_memory_); cpu_memory_ = nullptr; return {}; }
    }
    info.memory.set(static_cast<uint8_t *>(cpu_memory_) + offset);
    info.gpu_address = cpu_address_ + offset;
    return cpu_buffer_;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Heap)) {
      *ppvObject = static_cast<ID3D12Heap *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES

  D3D12_HEAP_DESC *STDMETHODCALLTYPE GetDesc(D3D12_HEAP_DESC *__ret) final {
    *__ret = desc_;
    return __ret;
  }

  WMT::Heap
  metal_heap() const {
    return heap_;
  }

  WMTResourceOptions
  resource_options() const {
    return heap_resource_options(desc_.Properties);
  }

private:
  D3D12_HEAP_DESC desc_;
  WMT::Reference<WMT::Heap> heap_;
  WMT::Reference<WMT::Buffer> cpu_buffer_;
  void *cpu_memory_ = nullptr;
  uint64_t cpu_address_ = 0;
  std::mutex cpu_mutex_;
};

D3D12_HEAP_PROPERTIES
heap_desc_properties(DxmtD3D12Heap *heap) {
  D3D12_HEAP_DESC desc = {};
  if (heap)
    heap->GetDesc(&desc);
  else
    desc.Properties = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  return desc.Properties;
}

class DxmtD3D12Resource final : public ID3D12Resource,
                                public IMTLD3D12ResourceInterop,
                                public D3D12ChildMethods,
                                public UnknownRef {
public:
  /* Placed resources retain their Metal heap storage. Their D3D11 views wrap
   * that same allocation so DXBC and native Metal commands see the same data. */
  DxmtD3D12Resource(
      DxmtD3D12Device *device, const D3D12_RESOURCE_DESC &desc, DxmtD3D12Heap *heap, UINT64 heap_offset,
      WMT::Reference<WMT::Buffer> &&buffer, uint64_t gpu_address, void *mapped, uint64_t buffer_offset = 0
  ) :
      D3D12ChildMethods(device),
      desc_(desc),
      metal_buffer_(std::move(buffer)),
      heap_(heap),
      heap_offset_(heap_offset),
      mapped_(mapped),
      metal_buffer_base_offset_(buffer_offset) {
    heap_properties_ = heap_desc_properties(heap);
    gpu_va_ = gpu_address;
    register_gpu_address();
  }

  DxmtD3D12Resource(
      DxmtD3D12Device *device, const D3D12_RESOURCE_DESC &desc, DxmtD3D12Heap *heap, UINT64 heap_offset,
      WMT::Reference<WMT::Texture> &&texture, uint64_t gpu_resource_id
  ) :
      D3D12ChildMethods(device),
      desc_(desc),
      metal_texture_(std::move(texture)),
      metal_texture_id_(gpu_resource_id),
      heap_(heap),
      heap_offset_(heap_offset) {
    heap_properties_ = heap_desc_properties(heap);
    /* Textures have no GPU address in either API; the handle stands in as a
     * unique identity, as it does for committed resources. */
    gpu_va_ = 0;
    register_gpu_address();
  }

  DxmtD3D12Resource(DxmtD3D12Device *device, const D3D12_HEAP_PROPERTIES &heap,
                    const D3D12_RESOURCE_DESC &desc)
      : D3D12ChildMethods(device), heap_properties_(heap), desc_(desc) {
  }

  DxmtD3D12Resource(DxmtD3D12Device *device, ID3D11Resource *resource,
                    const D3D12_RESOURCE_DESC &desc)
      : D3D12ChildMethods(device), desc_(desc), d3d11_resource_(resource) {
  }

  ~DxmtD3D12Resource() {
    unregister_gpu_address();
    release(generic_buffer_);
    release(vertex_buffer_);
    release(index_buffer_);
    release(constant_buffer_);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12Resource)) {
      *ppvObject = static_cast<ID3D12Resource *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IMTLD3D12ResourceInterop)) {
      *ppvObject = static_cast<IMTLD3D12ResourceInterop *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES

  /* What D3D11On12 wraps. A placed resource lives in Metal memory with no d3d11
   * resource over it, so it cannot be wrapped yet. */
  HRESULT STDMETHODCALLTYPE GetD3D11Resource(ID3D11Resource **resource) final {
    if (!resource)
      return E_POINTER;
    *resource = d3d11_resource();
    if (!*resource)
      return E_NOTIMPL;
    (*resource)->AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE *, void **data) final {
    CpuTimingScope map_time(bridge_cpu_statistics().map_ticks);
    if (!data)
      return E_POINTER;
    /* A placed buffer in a CPU-visible heap is mapped for real; one in a
     * private heap is not mappable at all, which is what D3D12 says too. */
    if (metal_buffer()) {
      if (heap_properties_.Type == D3D12_HEAP_TYPE_DEFAULT)
        return E_INVALIDARG;
      if (!mapped_)
        return E_INVALIDARG;
      *data = mapped_;
      return S_OK;
    }
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
      return E_INVALIDARG;
    if (cpu_data_.empty())
      cpu_data_.resize(static_cast<size_t>(std::max<UINT64>(1, desc_.Width)));
    *data = cpu_data_.data();
    return S_OK;
  }
  void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE *) final {}
  D3D12_RESOURCE_DESC *STDMETHODCALLTYPE GetDesc(D3D12_RESOURCE_DESC *__ret) final {
    *__ret = desc_;
    return __ret;
  }
  D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() final {
    return gpu_va_;
  }
  HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *, const void *,
                                               UINT, UINT) final {
    return E_NOTIMPL;
  }
  /* Reads a placed texture straight out of its heap. Only meaningful where the
   * heap is CPU-visible, which is the caller's choice to make: on a private
   * heap Metal has nothing to hand back. */
  HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *dst, UINT dst_row_pitch, UINT dst_depth_pitch,
                                                UINT subresource, const D3D12_BOX *box) final {
    if (!dst || !dst_row_pitch)
      return E_INVALIDARG;
    if (!metal_texture_ || desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return E_INVALIDARG;

    SubresourceLocation location = {};
    if (!decompose_subresource(desc_, subresource, location))
      return E_INVALIDARG;
    /* Depth-stencil is the only planar case here and Metal reads its planes
     * through separate views, not a slice index. */
    if (location.plane)
      return E_NOTIMPL;

    WMTOrigin origin = {0, 0, 0};
    WMTSize size = mip_size(desc_, location.mip);
    if (box) {
      origin = {box->left, box->top, box->front};
      size = {box->right - box->left, box->bottom - box->top, std::max(1u, box->back - box->front)};
    }
    if (!size.width || !size.height || !size.depth)
      return E_INVALIDARG;

    metal_texture_.getBytes(dst, dst_row_pitch, dst_depth_pitch ? dst_depth_pitch : dst_row_pitch * size.height,
                            origin, size, location.mip, location.slice);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *heap_properties,
                                               D3D12_HEAP_FLAGS *flags) final {
    if (heap_properties)
      *heap_properties = heap_properties_;
    if (flags)
      *flags = D3D12_HEAP_FLAG_NONE;
    return S_OK;
  }

  bool is_placed() const { return heap_.get() != nullptr; }
  WMT::Buffer metal_buffer() const {
    return dxmt_buffer_ ? dxmt_buffer_->current()->buffer() : WMT::Buffer(metal_buffer_);
  }

  /* Address-range bookkeeping. Defined out of line: the device type they need
   * is declared further down. */
  void register_gpu_address();
  void unregister_gpu_address();
  uint64_t gpu_address_size() const {
    return desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? std::max<UINT64>(1, desc_.Width) : 1;
  }
  WMT::Texture metal_texture() const {
    return dxmt_texture_ ? dxmt_texture_->current()->texture() : WMT::Texture(metal_texture_);
  }
  dxmt::GenericAccessTracker &access_tracker() { return access_tracker_; }
  uint64_t metal_texture_id() const {
    return dxmt_texture_ ? dxmt_texture_->current()->gpuResourceID : metal_texture_id_;
  }
  UINT64 heap_offset() const { return heap_offset_; }
  uint64_t metal_buffer_offset() const {
    return dxmt_buffer_ ? dxmt_buffer_->current()->currentSuballocationOffset() : metal_buffer_base_offset_;
  }

  ID3D11Resource *d3d11_resource();
  bool initialize_backing();
  template <PipelineStage stage>
  void track_access(ArgumentEncodingContext &enc, int flags, UINT level = 0, UINT slice = 0) {
    if (dxmt_buffer_)
      enc.access<stage>(dxmt_buffer_, 0, UINT(desc_.Width), flags);
    else if (dxmt_texture_)
      enc.access<stage>(dxmt_texture_, level, slice, flags);
    else
      enc.trackExternalResource<stage>(access_tracker_, flags);
  }
  ID3D11Buffer *buffer_for_bind(UINT bind);
  ID3D11Buffer *vertex_buffer() { return buffer_for_bind(D3D11_BIND_VERTEX_BUFFER); }
  ID3D11Buffer *index_buffer() { return buffer_for_bind(D3D11_BIND_INDEX_BUFFER); }
  ID3D11Buffer *constant_buffer() { return buffer_for_bind(D3D11_BIND_CONSTANT_BUFFER); }
  void update_buffer(ID3D11Buffer *buffer, UINT byte_width);
  const uint8_t *cpu_data() const { return cpu_data_.data(); }
  size_t cpu_size() const { return cpu_data_.size(); }
  const D3D12_RESOURCE_DESC &desc() const { return desc_; }
  void *readback_write_pointer() const {
    return heap_properties_.Type == D3D12_HEAP_TYPE_READBACK &&
                   desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && metal_buffer()
               ? mapped_ : nullptr;
  }
  void set_cpu_write(CpuReadbackDependency dependency) { cpu_write_ = std::move(dependency); }
  void order_cpu_write();

  /* Grows the shadow on demand so a copy into a resource that has not been
   * mapped yet still lands somewhere. */
  uint8_t *cpu_data_for_write(size_t required) {
    if (cpu_data_.size() < required)
      cpu_data_.resize(required);
    return cpu_data_.data();
  }

private:
  CpuReadbackDependency cpu_write_;
  D3D12_HEAP_PROPERTIES heap_properties_ = {D3D12_HEAP_TYPE_DEFAULT,
                                            D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                            D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC desc_ = {};
  D3D12_GPU_VIRTUAL_ADDRESS gpu_va_ = 0;
  std::vector<uint8_t> cpu_data_;
  /* Placed resources only. */
  WMT::Reference<WMT::Buffer> metal_buffer_;
  WMT::Reference<WMT::Texture> metal_texture_;
  uint64_t metal_texture_id_ = 0;
  ComPtr<DxmtD3D12Heap> heap_;
  UINT64 heap_offset_ = 0;
  void *mapped_ = nullptr;
  uint64_t metal_buffer_base_offset_ = 0;
  ComPtr<ID3D11Resource> d3d11_resource_;
  ID3D11Buffer *generic_buffer_ = nullptr;
  ID3D11Buffer *vertex_buffer_ = nullptr;
  ID3D11Buffer *index_buffer_ = nullptr;
  ID3D11Buffer *constant_buffer_ = nullptr;
  dxmt::GenericAccessTracker access_tracker_;
  Rc<dxmt::Buffer> dxmt_buffer_;
  Rc<dxmt::Texture> dxmt_texture_;
};

struct ResidentResource {
  WMT::Reference<WMT::Resource> object;
  ComPtr<DxmtD3D12Resource> resource;
  WMTResourceUsage usage;
};

class DxmtD3D12DescriptorHeap final : public ID3D12DescriptorHeap,
                                      public D3D12ChildMethods,
                                      public UnknownRef {
public:
  /* gpu_heap is the Metal-side array the converted shaders index into; it is
   * null for non-shader-visible heaps, which exist only as CPU staging. */
  DxmtD3D12DescriptorHeap(DxmtD3D12Device *device, const D3D12_DESCRIPTOR_HEAP_DESC &desc,
                          std::unique_ptr<dxmt::DescriptorHeap> &&gpu_heap)
      : D3D12ChildMethods(device), desc_(desc),
        descriptors_(std::max<UINT>(1, desc.NumDescriptors)), gpu_heap_(std::move(gpu_heap)) {
    for (uint32_t i = 0; i < descriptors_.size(); i++) {
      descriptors_[i].owner = this;
      descriptors_[i].index = i;
    }
  }

  dxmt::DescriptorHeap *
  gpu_heap() const {
    return gpu_heap_.get();
  }

  void invalidate_residency() { residency_dirty_ = true; }

  /* Descriptor writes invalidate this list. Stable heaps pay for one scan,
   * rather than scanning every slot for every draw or dispatch. */
  const std::vector<ResidentResource> &residency() {
    if (!residency_dirty_) return resident_;
    resident_.clear();
    std::unordered_map<obj_handle_t, size_t> indices;
    for (auto &descriptor : descriptors_) {
      auto *resource = descriptor.resource.get();
      if (!resource) continue;
      WMT::Resource object;
      if (descriptor.texture_view) object = WMT::Resource(descriptor.texture_view.handle);
      else if (auto buffer = resource->metal_buffer()) object = WMT::Resource(buffer.handle);
      else if (auto texture = resource->metal_texture()) object = WMT::Resource(texture.handle);
      if (!object) continue;
      auto usage = descriptor.type == DescriptorType::Uav ?
          WMTResourceUsage(WMTResourceUsageRead | WMTResourceUsageWrite) : WMTResourceUsageRead;
      auto [entry, inserted] = indices.emplace(object.handle, resident_.size());
      if (inserted) resident_.push_back({WMT::Reference<WMT::Resource>(object), resource, usage});
      else resident_[entry->second].usage = WMTResourceUsage(resident_[entry->second].usage | usage);
    }
    residency_dirty_ = false;
    return resident_;
  }

  /* Address of a descriptor inside the Metal heap. This is what a root
   * descriptor table stores in the top-level argument buffer. */
  uint64_t
  gpu_descriptor_address(uint32_t index) const {
    return gpu_heap_ ? gpu_heap_->descriptorAddress(index) : 0;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = static_cast<ID3D12DescriptorHeap *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES

  D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) final {
    *__ret = desc_;
    return __ret;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart(
      D3D12_CPU_DESCRIPTOR_HANDLE *__ret) final {
    *__ret = {reinterpret_cast<SIZE_T>(descriptors_.data())};
    return __ret;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart(
      D3D12_GPU_DESCRIPTOR_HANDLE *__ret) final {
    *__ret = {reinterpret_cast<UINT64>(descriptors_.data())};
    return __ret;
  }

  static Descriptor *from_cpu(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    return reinterpret_cast<Descriptor *>(handle.ptr);
  }
  static Descriptor *from_gpu(D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    return reinterpret_cast<Descriptor *>(static_cast<uintptr_t>(handle.ptr));
  }

private:
  D3D12_DESCRIPTOR_HEAP_DESC desc_ = {};
  std::vector<Descriptor> descriptors_;
  std::unique_ptr<dxmt::DescriptorHeap> gpu_heap_;
  bool residency_dirty_ = true;
  std::vector<ResidentResource> resident_;
};

/* Keep the complete encoding in CPU heaps too: copying it into a visible
 * heap must preserve buffer offsets, view identities, and sampler metadata. */
void
write_gpu_descriptor(Descriptor *descriptor, const dxmt::DescriptorEntry &entry) {
  if (!descriptor) return;
  descriptor->gpu_entry = entry;
  if (!descriptor->owner) return;
  descriptor->owner->invalidate_residency();
  if (dxmt::DescriptorHeap *heap = descriptor->owner->gpu_heap())
    heap->writeRaw(descriptor->index, entry);
}

class DxmtD3D12PipelineState;
class DxmtD3D12GraphicsCommandList;
class DxmtD3D12SwapChainFactory;
class DxmtD3D12DXGIDevice;

D3D11_TEXTURE_ADDRESS_MODE address_mode(D3D12_TEXTURE_ADDRESS_MODE mode) {
  return static_cast<D3D11_TEXTURE_ADDRESS_MODE>(mode);
}

/* D3D12_FILTER is a bit field rather than an enumeration of combinations, so
 * the individual filters are decoded rather than switched over. */
constexpr UINT kFilterMipLinear = 0x1;
constexpr UINT kFilterMagLinear = 0x4;
constexpr UINT kFilterMinLinear = 0x10;
constexpr UINT kFilterAnisotropic = 0x40;
constexpr UINT kFilterComparison = 0x80;

WMTSamplerAddressMode sampler_address_mode(D3D12_TEXTURE_ADDRESS_MODE mode) {
  switch (mode) {
  case D3D12_TEXTURE_ADDRESS_MODE_WRAP:
    return WMTSamplerAddressModeRepeat;
  case D3D12_TEXTURE_ADDRESS_MODE_MIRROR:
    return WMTSamplerAddressModeMirrorRepeat;
  case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
    return WMTSamplerAddressModeClampToBorderColor;
  case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE:
    return WMTSamplerAddressModeMirrorClampToEdge;
  default:
    return WMTSamplerAddressModeClampToEdge;
  }
}

/* D3D12 gives a free border colour, Metal offers three fixed ones; pick the
 * nearest. */
WMTSamplerBorderColor sampler_border_color(const FLOAT color[4]) {
  if (!color)
    return WMTSamplerBorderColorTransparentBlack;
  bool opaque = color[3] >= 0.5f;
  bool white = (color[0] + color[1] + color[2]) >= 1.5f;
  if (!opaque)
    return WMTSamplerBorderColorTransparentBlack;
  return white ? WMTSamplerBorderColorOpaqueWhite : WMTSamplerBorderColorOpaqueBlack;
}

D3D11_FILTER filter_mode(D3D12_FILTER filter) {
  return static_cast<D3D11_FILTER>(filter);
}

struct LegacyShaderBindingMask {
  uint16_t cbvs = 0, samplers = 0;
  uint64_t srvs_lo = 0, srvs_hi = 0, uavs = 0;

  bool srv(UINT slot) const {
    return slot < 64 ? (srvs_lo & (uint64_t(1) << slot)) != 0
                     : slot < 128 && (srvs_hi & (uint64_t(1) << (slot - 64))) != 0;
  }
  static LegacyShaderBindingMask reflect(IUnknown *object) {
    if (!object) return {};
    ComPtr<IMTLD3D11Shader> shader;
    if (SUCCEEDED(object->QueryInterface(__uuidof(IMTLD3D11Shader),
                                         reinterpret_cast<void **>(shader.put())))) {
      if (auto *managed = shader->GetManagedShader()) {
        const auto &r = managed->reflection();
        return {r.ConstantBufferSlotMask, r.SamplerSlotMask, r.SRVSlotMaskLo, r.SRVSlotMaskHi, r.UAVSlotMask};
      }
    }
    // Unknown reflection must preserve all bindings, never drop a shader input.
    return {UINT16_MAX, UINT16_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
  }
};

class DxmtD3D12PipelineState final : public ID3D12PipelineState,
                                     public D3D12ChildMethods,
                                     public UnknownRef {
public:
  DxmtD3D12PipelineState(DxmtD3D12Device *device) : D3D12ChildMethods(device) {}

  HRESULT init(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc);
  HRESULT init_compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc);
  ID3D11ComputeShader *legacy_compute_shader() const { return cs_.get(); }
  const LegacyShaderBindingMask &legacy_bindings(UINT stage) const { return legacy_bindings_[stage]; }
  uint64_t legacy_compute_uav_mask() const { return legacy_bindings_[3].uavs; }
  void apply(ID3D11DeviceContext *context, const FLOAT *blend_factor, UINT stencil_ref);

  WMT::ComputePipelineState
  metal_compute_pipeline() const {
    return compute_pipeline_;
  }

  /* Metal takes the threadgroup size at dispatch; D3D12 bakes it into the
   * shader, so it comes back out through reflection. */
  WMTSize
  compute_threadgroup_size() const {
    return compute_threadgroup_size_;
  }

  /* True once the shaders have been translated to Metal. The d3d11 execution
   * path cannot use them yet, but a pipeline that got this far is one a real
   * D3D12 application can create: d3d11 rejects DXIL outright. */
  bool has_metal_functions() const { return vertex_function_ && fragment_function_; }
  WMT::Function vertex_function() const { return vertex_function_; }
  WMT::Function fragment_function() const { return fragment_function_; }

  /* The Metal pipeline for a given set of vertex buffer strides.
   *
   * D3D12 carries strides on the vertex buffer views, so they are only known at
   * draw time, while Metal bakes them into the pipeline. One pipeline per
   * distinct stride set is the cost of keeping hardware vertex fetch; the
   * alternative is fetching vertices in the shader, which costs on every
   * vertex instead of once per new stride combination. Applications reuse a
   * handful of strides, so this cache settles quickly. */
  WMT::RenderPipelineState metal_pipeline(const uint32_t *strides, uint32_t stride_count);
  WMT::DepthStencilState metal_depth_stencil() const { return metal_depth_stencil_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12PipelineState)) {
      *ppvObject = static_cast<ID3D12PipelineState *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES
  HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob **blob) final {
    if (!blob)
      return E_POINTER;
    *blob = new Blob(1);
    return S_OK;
  }

private:
  ComPtr<ID3D11InputLayout> input_layout_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11GeometryShader> gs_;
  ComPtr<ID3D11ComputeShader> cs_;
  // Vertex, pixel, geometry, compute. Use the same declaration reflection
  // that selects shader inputs in the shared D3D11 encoder.
  std::array<LegacyShaderBindingMask, 4> legacy_bindings_ = {};
  /* Metal side: the libraries have to outlive the functions taken from them. */
  WMT::Reference<WMT::Library> vertex_library_;
  WMT::Reference<WMT::Library> fragment_library_;
  WMT::Reference<WMT::Function> vertex_function_;
  WMT::Reference<WMT::Function> fragment_function_;
  WMT::Reference<WMT::Library> compute_library_;
  WMT::Reference<WMT::Function> compute_function_;
  WMT::Reference<WMT::ComputePipelineState> compute_pipeline_;
  WMTSize compute_threadgroup_size_ = {};

  /* Everything the Metal pipeline needs that does not depend on strides, kept
   * from the creation-time descriptor because the application's pointers are
   * not guaranteed to outlive the call. */
  WMTRenderPipelineInfo metal_pipeline_info_ = {};
  std::vector<std::string> semantic_names_;
  std::vector<dxmt::VertexInputElement> input_elements_;
  std::vector<WMTVertexAttributeInfo> shader_attributes_;
  std::mutex pipeline_mutex_;
  std::unordered_map<uint64_t, WMT::Reference<WMT::RenderPipelineState>> pipelines_by_stride_;
  WMTDepthStencilInfo metal_depth_stencil_info_ = {};
  WMT::Reference<WMT::DepthStencilState> metal_depth_stencil_;

  void capture_input_layout(const D3D12_INPUT_LAYOUT_DESC &layout);
  void capture_pipeline_info(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc);
  ComPtr<ID3D11RasterizerState> rasterizer_;
  ComPtr<ID3D11BlendState> blend_;
  ComPtr<ID3D11DepthStencilState> depth_stencil_;
  UINT sample_mask_ = UINT_MAX;
};

class DxmtD3D12SwapChain final : public IDXGISwapChain4, public UnknownRef {
public:
  DxmtD3D12SwapChain(DxmtD3D12Device *device, IDXGISwapChain1 *inner,
                     const DXGI_SWAP_CHAIN_DESC1 &desc, DxmtD3D12CommandQueue *queue)
      : device_(device), queue_(queue), inner_(inner), desc1_(desc) {}
  HRESULT initialize_buffers();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) ||
        riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
      *ppvObject = static_cast<IDXGISwapChain4 *>(this);
      AddRef();
      return S_OK;
    }
    return inner_->QueryInterface(riid, ppvObject);
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) final {
    return inner_->SetPrivateData(guid, data_size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                    const IUnknown *object) final {
    return inner_->SetPrivateDataInterface(guid, object);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) final {
    return inner_->GetPrivateData(guid, data_size, data);
  }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) final {
    return inner_->GetParent(riid, parent);
  }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) final;

  HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) final {
    return Present1(sync_interval, flags, nullptr);
  }
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID riid, void **surface) final;
  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput *target) final {
    return inner_->SetFullscreenState(fullscreen, target);
  }
  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen, IDXGIOutput **target) final {
    return inner_->GetFullscreenState(fullscreen, target);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) final {
    HRESULT hr = inner_->GetDesc(desc);
    if (SUCCEEDED(hr) && desc) {
      desc->BufferDesc.Width = desc1_.Width;
      desc->BufferDesc.Height = desc1_.Height;
      desc->BufferDesc.Format = desc1_.Format;
      desc->BufferCount = desc1_.BufferCount;
      desc->SwapEffect = desc1_.SwapEffect;
      desc->BufferUsage = desc1_.BufferUsage;
      desc->Flags = desc1_.Flags;
    }
    return hr;
  }
  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height,
                                           DXGI_FORMAT format, UINT flags) final;
  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *desc) final {
    return inner_->ResizeTarget(desc);
  }
  HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) final {
    return inner_->GetContainingOutput(output);
  }
  HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) final {
    return inner_->GetFrameStatistics(stats);
  }
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *count) final {
    return inner_->GetLastPresentCount(count);
  }
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *desc) final {
    if (!desc) return E_POINTER;
    *desc = desc1_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc) final {
    return inner_->GetFullscreenDesc(desc);
  }
  HRESULT STDMETHODCALLTYPE GetHwnd(HWND *hwnd) final { return inner_->GetHwnd(hwnd); }
  HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void **window) final {
    return inner_->GetCoreWindow(riid, window);
  }
  HRESULT STDMETHODCALLTYPE Present1(UINT sync_interval, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS *params) final;

  BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() final {
    return inner_->IsTemporaryMonoSupported();
  }
  HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **output) final {
    return inner_->GetRestrictToOutput(output);
  }
  HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *color) final {
    return inner_->SetBackgroundColor(color);
  }
  HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *color) final {
    return inner_->GetBackgroundColor(color);
  }
  HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) final {
    return inner_->SetRotation(rotation);
  }
  HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *rotation) final {
    return inner_->GetRotation(rotation);
  }
  HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *width, UINT *height) final {
    if (width)
      *width = desc1_.Width;
    if (height)
      *height = desc1_.Height;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) final {
    ComPtr<IDXGISwapChain2> chain;
    HRESULT hr = inner_->QueryInterface(IID_PPV_ARGS(chain.put()));
    return SUCCEEDED(hr) ? chain->SetMaximumFrameLatency(max_latency) : hr;
  }
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) final {
    ComPtr<IDXGISwapChain2> chain;
    HRESULT hr = inner_->QueryInterface(IID_PPV_ARGS(chain.put()));
    return SUCCEEDED(hr) ? chain->GetMaximumFrameLatency(max_latency) : hr;
  }
  HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() final {
    ComPtr<IDXGISwapChain2> chain;
    return SUCCEEDED(inner_->QueryInterface(IID_PPV_ARGS(chain.put()))) ? chain->GetFrameLatencyWaitableObject() : nullptr;
  }
  HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) final {
    return E_NOTIMPL;
  }
  UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() final { return current_; }
  HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE color_space,
                                                   UINT *support) final {
    if (!support)
      return E_POINTER;
    *support = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE color_space) final {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags,
                                            const UINT *node_mask,
                                            IUnknown *const *queues) final {
    return ResizeBuffers(count, width, height, format, flags);
  }
  HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size,
                                           void *metadata) final {
    return S_OK;
  }

private:
  ComPtr<DxmtD3D12Device> device_;
  ComPtr<DxmtD3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain1> inner_;
  DXGI_SWAP_CHAIN_DESC1 desc1_ = {};
  UINT current_ = 0;
  std::vector<ComPtr<DxmtD3D12Resource>> buffers_;
};

class DxmtD3D12SwapChainFactory final : public IMTLSwapChainFactory,
                                      public UnknownRef {
public:
  explicit DxmtD3D12SwapChainFactory(DxmtD3D12Device *device, DxmtD3D12CommandQueue *queue = nullptr)
      : device_(device), queue_(queue) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMTLSwapChainFactory)) {
      *ppvObject = static_cast<IMTLSwapChainFactory *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 *factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
      IDXGISwapChain1 **swap_chain) final;

private:
  ComPtr<DxmtD3D12Device> device_;
  ComPtr<DxmtD3D12CommandQueue> queue_;
};

class DxmtD3D12DXGIDevice final : public IMTLDXGIDevice,
                                  public UnknownRef {
public:
  explicit DxmtD3D12DXGIDevice(DxmtD3D12Device *device, DxmtD3D12CommandQueue *queue = nullptr)
      : device_(device), queue_(queue) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) ||
        riid == __uuidof(IDXGIDevice2) || riid == __uuidof(IDXGIDevice3) ||
        riid == __uuidof(IMTLDXGIDevice)) {
      *ppvObject = static_cast<IMTLDXGIDevice *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) final;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                    const IUnknown *object) final;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) final;
  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) final;
  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **adapter) final;
  HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC *, UINT, DXGI_USAGE,
                                          const DXGI_SHARED_RESOURCE *,
                                          IDXGISurface **) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown *const *,
                                                   DXGI_RESIDENCY *residency,
                                                   UINT resource_count) final;
  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) final;
  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *priority) final;
  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) final;
  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) final;
  HRESULT STDMETHODCALLTYPE OfferResources(UINT, IDXGIResource *const *,
                                           DXGI_OFFER_RESOURCE_PRIORITY) final {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT resource_count, IDXGIResource *const *,
                                             BOOL *discarded) final;
  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) final {
    if (event)
      SetEvent(event);
    return S_OK;
  }
  void STDMETHODCALLTYPE Trim() final {}
  WMT::Device STDMETHODCALLTYPE GetMTLDevice() final;
  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final;
  HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 *factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
      IDXGISwapChain1 **swap_chain) final;

private:
  HRESULT query_inner(IMTLDXGIDevice **inner) const;

  ComPtr<DxmtD3D12Device> device_;
  ComPtr<DxmtD3D12CommandQueue> queue_;
};

class DxmtD3D12CommandQueue final : public ID3D12CommandQueue,
                                    public D3D12ChildMethods,
                                    public UnknownRef {
public:
  DxmtD3D12CommandQueue(DxmtD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC &desc);
  ~DxmtD3D12CommandQueue();
  void submit(std::function<void()> work);
  bool has_pending_work() const { return !pending_.empty(); }
  void set_cpu_readback(CpuReadbackDependency dependency) { cpu_readback_ = std::move(dependency); }


  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = static_cast<ID3D12CommandQueue *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IMTLSwapChainFactory)) {
      auto object = new DxmtD3D12SwapChainFactory(device_.get(), this);
      *ppvObject = static_cast<IMTLSwapChainFactory *>(object);
      return S_OK;
    }
    if (riid == __uuidof(IMTLDXGIDevice)) {
      auto object = new DxmtD3D12DXGIDevice(device_.get(), this);
      *ppvObject = static_cast<IMTLDXGIDevice *>(object);
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES

  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *, UINT, const D3D12_TILED_RESOURCE_COORDINATE *,
                                            const D3D12_TILE_REGION_SIZE *, ID3D12Heap *, UINT,
                                            const D3D12_TILE_RANGE_FLAGS *, const UINT *, const UINT *,
                                            D3D12_TILE_MAPPING_FLAGS) final {}
  void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *,
                                          ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *,
                                          const D3D12_TILE_REGION_SIZE *, D3D12_TILE_MAPPING_FLAGS) final {}
  void STDMETHODCALLTYPE ExecuteCommandLists(UINT command_list_count,
                                             ID3D12CommandList *const *command_lists) final;
  void STDMETHODCALLTYPE SetMarker(UINT, const void *, UINT) final {}
  void STDMETHODCALLTYPE BeginEvent(UINT, const void *, UINT) final {}
  void STDMETHODCALLTYPE EndEvent() final {}
  /* A signal completes after this queue's preceding GPU work and CPU query
   * writes. Independent queues need not wait for those CPU writes. */
  HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *fence, UINT64 value) final;
  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *fence, UINT64 value) final;
  HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *frequency) final {
    if (!frequency)
      return E_POINTER;
    *frequency = 1000000000ull;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *gpu_timestamp,
                                                UINT64 *cpu_timestamp) final {
    if (gpu_timestamp)
      *gpu_timestamp = 0;
    if (cpu_timestamp)
      *cpu_timestamp = GetTickCount64();
    return S_OK;
  }
  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) final {
    *__ret = desc_;
    return __ret;
  }

private:
  friend class DxmtD3D12Device;
  enum class OperationKind { Work, Wait, Signal };
  struct Operation {
    OperationKind kind;
    std::function<void()> work;
    ComPtr<DxmtD3D12Fence> fence;
    UINT64 value = 0;
  };
  std::deque<Operation> pending_;
  HRESULT emit_signal(ID3D12Fence *fence, UINT64 value);
  HRESULT emit_wait(ID3D12Fence *fence, UINT64 value);
  D3D12_COMMAND_QUEUE_DESC desc_ = {};
  CpuReadbackDependency cpu_readback_;
  WMT::Reference<WMT::SharedEvent> completion_event_;
  uint64_t completion_value_ = 0;
  std::unique_ptr<QueryResolveWorker> signal_worker_;
};

/* Query readback can wait for an already submitted chunk independently of
 * the application's submission thread. One worker per device preserves the
 * order of result writes and their shared-event values. */
class QueryResolveWorker {
  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> pending;
    std::atomic<DWORD> thread_id = 0;
    bool stopped = false;
  };
public:
  explicit QueryResolveWorker(const wchar_t *name = L"dxmt-d12-query")
      : state_(std::make_shared<State>()),
        thread_(std::function<void()>([state = state_, name = std::wstring(name)] {
    state->thread_id.store(GetCurrentThreadId());
    SetThreadDescription(GetCurrentThread(), name.c_str());
    if (GetEnvironmentVariableA("DXMT_D3D12_QUERY_PRIORITY_HIGH", nullptr, 0))
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->condition.wait(lock, [&] { return state->stopped || !state->pending.empty(); });
        if (state->pending.empty()) return;
        task = std::move(state->pending.front());
        state->pending.pop_front();
      }
      auto pool = WMT::MakeAutoreleasePool();
      task();
    }
  })) {}
  ~QueryResolveWorker() {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->stopped = true;
    }
    state_->condition.notify_one();
    // A retained readback resource can release the device on its worker.
    // The loop owns State independently, so it can safely finish that task.
    if (state_->thread_id.load() == GetCurrentThreadId()) thread_.detach();
    else thread_.join();
  }
  void submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->pending.push_back(std::move(task));
    }
    state_->condition.notify_one();
  }
private:
  std::shared_ptr<State> state_;
  dxmt::thread thread_;
};

class DxmtD3D12Device final : public ID3D12Device, public IMTLD3D12DeviceInterop, public UnknownRef {
public:
  DxmtD3D12Device(ID3D11Device *device, ID3D11DeviceContext *context)
      : d3d11_device_(device), context_(context) {
    /* D3D12CreateDevice builds the d3d11 device from this same DLL, so it is
     * always one of ours and the Metal device can be reached through it. The
     * probe still guards the cast: a foreign ID3D11Device would otherwise be
     * reinterpreted, and every Metal call after that would be wild. */
    IMTLD3D11DeviceContext *probe = nullptr;
    if (context && SUCCEEDED(context->QueryInterface(__uuidof(IMTLD3D11DeviceContext), (void **)&probe))) {
      probe->Release();
      auto *dxmt_capable = static_cast<MTLD3D11Device *>(device);
      metal_device_ = dxmt_capable->GetMTLDevice();
      /* The same core d3d11 encodes through; sharing it is what lets a d3d12
       * fence be the same timeline the GPU signals. */
      dxmt_device_ = &dxmt_capable->GetDXMTDevice();
      aliasing_event_ = metal_device_.newEvent();
      dxmt_capable->SetD3D12Device(static_cast<ID3D12Device *>(this));
    }
    if (context) context->QueryInterface(IID_PPV_ARGS(context1_.put()));
  }

  WMT::Device
  metal_device() const {
    return metal_device_;
  }

  dxmt::Device *
  dxmt_device() const {
    return dxmt_device_;
  }

  /* Flushes whatever has been recorded into the shared command chunk. Goes
   * through the d3d11 immediate context because the chunk is its own and its
   * commit path carries the state checks. */
  void submit_recorded_work(bool commit = true);
  void encode_gpu_barrier();
  void synchronize_compute_for_diagnostics();
  static bool asynchronous_query_resolve_enabled() {
    static const bool enabled = [] {
      char value[8] = {};
      DWORD length = GetEnvironmentVariableA("DXMT_D3D12_ASYNC_QUERY_RESOLVE", value, sizeof(value));
      return !(length == 1 && value[0] == '0');
    }();
    return enabled;
  }
  bool defer_query_resolve(std::vector<ComPtr<ID3D11Query>> queries, D3D12_QUERY_TYPE type,
                           DxmtD3D12Resource *destination, UINT64 offset);
  HRESULT initialization_status() const {
    if (!metal_device_ || !dxmt_device_) return DXGI_ERROR_UNSUPPORTED;
    return aliasing_event_ ? S_OK : E_OUTOFMEMORY;
  }
  ID3D11DeviceContext1 *context1() const { return context1_.get(); }
  ID3D11Buffer *upload_root_constants(const void *bytes, UINT size, UINT &first, UINT &count) {
    if (!size || size > 256 || !bytes) return nullptr;
    if (!root_constants_) {
      D3D11_BUFFER_DESC desc = {};
      desc.ByteWidth = 65536;
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (FAILED(d3d11_device_->CreateBuffer(&desc, nullptr, root_constants_.put()))) return nullptr;
    }
    if (root_constants_offset_ + 256 > 65536) root_constants_offset_ = 0;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(root_constants_.get(), 0, root_constants_offset_ ? D3D11_MAP_WRITE_NO_OVERWRITE :
                              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return nullptr;
    std::memcpy(static_cast<uint8_t *>(mapped.pData) + root_constants_offset_, bytes, size);
    context_->Unmap(root_constants_.get(), 0);
    first = root_constants_offset_ / 16;
    count = 16;
    root_constants_offset_ += 256;
    return root_constants_.get();
  }

  uint64_t buffer_address(DxmtD3D12Resource *resource, D3D12_GPU_VIRTUAL_ADDRESS location) const;
  dxmt::DescriptorEntry encode_resource_descriptor(DxmtD3D12Resource *resource) const;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12Device)) {
      *ppvObject = static_cast<ID3D12Device *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IMTLD3D12DeviceInterop)) {
      *ppvObject = static_cast<IMTLD3D12DeviceInterop *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetD3D11Device(ID3D11Device **device) final {
    if (!device)
      return E_POINTER;
    *device = d3d11_device_.get();
    if (!*device)
      return E_FAIL;
    (*device)->AddRef();
    return S_OK;
  }

  /* An 11on12 device the application still holds outlives this one. */
  ~DxmtD3D12Device() {
    query_worker_.reset();
    if (dxmt_device_)
      static_cast<MTLD3D11Device *>(d3d11_device_.get())->SetD3D12Device(nullptr);
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) final {
    return DXGI_ERROR_NOT_FOUND;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) final {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) final {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *) final { return S_OK; }

  UINT STDMETHODCALLTYPE GetNodeCount() final { return 1; }
  HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *desc,
                                               REFIID riid, void **queue) final {
    if (!queue)
      return E_POINTER;
    *queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC local = desc ? *desc : D3D12_COMMAND_QUEUE_DESC{};
    if (local.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && local.Type != D3D12_COMMAND_LIST_TYPE_COPY &&
        local.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return E_NOTIMPL;
    auto object = new DxmtD3D12CommandQueue(this, local);
    HRESULT hr = object->QueryInterface(riid, queue);
    object->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                   REFIID riid, void **allocator) final {
    if (!allocator)
      return E_POINTER;
    *allocator = nullptr;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COPY &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return E_NOTIMPL;
    auto object = new DxmtD3D12CommandAllocator(this);
    HRESULT hr = object->QueryInterface(riid, allocator);
    object->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
      void **pipeline_state) final {
    if (!pipeline_state)
      return E_POINTER;
    *pipeline_state = nullptr;
    auto object = new DxmtD3D12PipelineState(this);
    HRESULT hr = object->init(desc);
    if (FAILED(hr) && d3d12_diagnostics())
      Logger::warn(str::format("D3D12 graphics PSO failed hr=", hr));
    if (SUCCEEDED(hr))
      hr = object->QueryInterface(riid, pipeline_state);
    object->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
      const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pipeline) final {
    if (!pipeline)
      return E_POINTER;
    *pipeline = nullptr;
    if (!desc)
      return E_INVALIDARG;

    auto object = new DxmtD3D12PipelineState(this);
    HRESULT hr = object->init_compute(desc);
    if (FAILED(hr)) {
      if (d3d12_diagnostics()) Logger::warn(str::format("D3D12 compute PSO failed hr=", hr));
      object->Release();
      return hr;
    }
    hr = object->QueryInterface(riid, pipeline);
    object->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE type,
                                              ID3D12CommandAllocator *,
                                              ID3D12PipelineState *initial_state,
                                              REFIID riid, void **command_list) final;
  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void *data, UINT size) final;
  HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                                                 REFIID riid, void **heap) final {
    if (!heap)
      return E_POINTER;
    *heap = nullptr;
    if (!desc)
      return E_INVALIDARG;

    /* Only shader-visible heaps get Metal storage. RTV and DSV heaps are never
     * shader-visible in D3D12 and are pure CPU bookkeeping here too. */
    std::unique_ptr<dxmt::DescriptorHeap> gpu_heap;
    if ((desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && metal_device_) {
      bool sampler_heap = desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
      gpu_heap = std::make_unique<dxmt::DescriptorHeap>(metal_device_, std::max<UINT>(1, desc->NumDescriptors),
                                                        sampler_heap);
      if (!gpu_heap->buffer())
        return E_OUTOFMEMORY;
    }

    auto object = new DxmtD3D12DescriptorHeap(this, *desc, std::move(gpu_heap));
    HRESULT hr = object->QueryInterface(riid, heap);
    object->Release();
    return hr;
  }
  UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) final {
    return sizeof(Descriptor);
  }
  HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT, const void *blob, SIZE_T blob_size,
                                                REFIID riid, void **root_signature) final {
    if (!root_signature)
      return E_POINTER;
    *root_signature = nullptr;
    auto object = new DxmtD3D12RootSignature(this);
    if (!object->parse(blob, blob_size)) {
      object->Release();
      return E_INVALIDARG;
    }
    HRESULT hr = object->QueryInterface(riid, root_signature);
    object->Release();
    return hr;
  }
  void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) final {
    Descriptor *dst = DxmtD3D12DescriptorHeap::from_cpu(descriptor);
    dst->type = DescriptorType::Cbv;
    dst->cbv = desc ? *desc : D3D12_CONSTANT_BUFFER_VIEW_DESC{};
    dst->resource = resource_from_gpu_va(dst->cbv.BufferLocation);
    write_gpu_descriptor(dst, dxmt::encodeBufferDescriptor(buffer_address(dst->resource.get(),
                                                                          dst->cbv.BufferLocation),
                                                           dst->cbv.SizeInBytes));

  }
  void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *resource,
                                                  const D3D12_SHADER_RESOURCE_VIEW_DESC *,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) final;
  void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *resource, ID3D12Resource *,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC *,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE descriptor) final;
  void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *resource,
                                                const D3D12_RENDER_TARGET_VIEW_DESC *,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) final;
  void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *resource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC *,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) final;
  void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE descriptor) final;
  void STDMETHODCALLTYPE CopyDescriptors(UINT dst_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *dst_range_starts,
                                         const UINT *dst_range_sizes, UINT src_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *src_range_starts,
                                         const UINT *src_range_sizes,
                                         D3D12_DESCRIPTOR_HEAP_TYPE) final;
  void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT descriptor_count,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE src,
                                               D3D12_DESCRIPTOR_HEAP_TYPE) final;
  /* An application sizes its heaps from this, so a guess here means placed
   * resources that do not fit or a heap several times larger than it needs to
   * be. Metal is asked for the real numbers. Callers pass an array to learn the
   * size of a group of nonoverlapping resources, including each placement's
   * alignment and final padding. Aliasing is an explicit application choice. */
  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE GetResourceAllocationInfo(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT, UINT resource_desc_count,
      const D3D12_RESOURCE_DESC *resource_descs) final {
    D3D12_RESOURCE_ALLOCATION_INFO info = {0, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};

    for (UINT i = 0; i < resource_desc_count; i++) {
      const D3D12_RESOURCE_DESC &desc = resource_descs[i];
      WMTSizeAndAlign measured = {0, 0};

      if (metal_device_) {
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
          measured = metal_device_.heapBufferSizeAndAlign(
              std::max<UINT64>(1, desc.Width), heap_resource_options({D3D12_HEAP_TYPE_DEFAULT,
                                                                      D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                                                      D3D12_MEMORY_POOL_UNKNOWN, 1, 1})
          );
        } else {
          WMTTextureInfo texture_info;
          if (texture_info_from_desc(metal_device_, desc, WMTResourceStorageModePrivate, texture_info))
            measured = metal_device_.heapTextureSizeAndAlign(texture_info);
        }
      }

      if (!measured.size) {
        /* Unsupported format or no device: fall back to a byte count that is at
         * least not smaller than the resource. */
        measured.size = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                            ? std::max<UINT64>(1, desc.Width)
                            : desc.Width * std::max(desc.Height, 1u) * bytes_per_pixel(desc.Format);
        measured.align = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      }

      UINT64 alignment = std::max<UINT64>(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, measured.align);
      alignment = std::max(alignment, desc.Alignment);
      if (!measured.size || !desc.Width || !alignment || (alignment & (alignment - 1))) {
        info.SizeInBytes = UINT64_MAX;
        break;
      }
      info.Alignment = std::max(info.Alignment, alignment);
      UINT64 start = (info.SizeInBytes + alignment - 1) & ~(alignment - 1);
      UINT64 size = (measured.size + alignment - 1) & ~(alignment - 1);
      if (start < info.SizeInBytes || size < measured.size || start > UINT64_MAX - size) {
        info.SizeInBytes = UINT64_MAX;
        break;
      }
      info.SizeInBytes = start + size;
    }

    if (info.SizeInBytes != UINT64_MAX)
      info.SizeInBytes = (info.SizeInBytes + info.Alignment - 1) & ~(info.Alignment - 1);
    *__ret = info;
    return __ret;
  }
  D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE GetCustomHeapProperties(
      D3D12_HEAP_PROPERTIES *__ret, UINT, D3D12_HEAP_TYPE heap_type) final {
    *__ret = {heap_type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    return __ret;
  }
  HRESULT STDMETHODCALLTYPE CreateCommittedResource(
      const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS,
      const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES,
      const D3D12_CLEAR_VALUE *, REFIID riid, void **resource) final;
  HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid, void **heap) final;
  HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *heap, UINT64 heap_offset,
                                                 const D3D12_RESOURCE_DESC *desc,
                                                 D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE *,
                                                 REFIID riid, void **resource) final;
  HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *,
                                                   D3D12_RESOURCE_STATES,
                                                   const D3D12_CLEAR_VALUE *,
                                                   REFIID, void **) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *, const SECURITY_ATTRIBUTES *,
                                               DWORD, const WCHAR *, HANDLE *) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void **) final { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(const WCHAR *, DWORD, HANDLE *) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable *const *) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable *const *) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS,
                                        REFIID riid, void **fence) final {
    if (!fence)
      return E_POINTER;
    *fence = nullptr;
    if (!metal_device_)
      return E_FAIL;
    auto timeline = dxmt::TimelineFence::create(metal_device_, initial_value);
    if (!timeline) {
      ERR("D3D12: failed to allocate the shared event for a fence");
      return E_OUTOFMEMORY;
    }
    auto object = new DxmtD3D12Fence(this, initial_value);
    object->adopt(std::move(timeline));
    HRESULT hr = object->QueryInterface(riid, fence);
    object->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() final { return query_resolve_status_.load(); }
  /* One footprint per subresource, laid out back to back in a buffer the
   * application then uploads from. Each mip has its own extent and its own
   * aligned pitch, so a single layout for the whole texture -- which is what
   * this used to produce -- puts every mip but the first at the wrong offset. */
  void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc, UINT first_subresource,
                                               UINT subresource_count_in, UINT64 base_offset,
                                               D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
                                               UINT *row_count, UINT64 *row_size,
                                               UINT64 *total_bytes) final {
    if (layouts) std::memset(layouts, 0xff, sizeof(*layouts) * subresource_count_in);
    if (row_count) std::memset(row_count, 0xff, sizeof(*row_count) * subresource_count_in);
    if (row_size) std::memset(row_size, 0xff, sizeof(*row_size) * subresource_count_in);
    if (total_bytes) *total_bytes = UINT64_MAX;
    if (!desc || !desc->Width || !desc->Height || !desc->DepthOrArraySize || desc->SampleDesc.Count != 1)
      return;
    UINT available = subresource_count(*desc);
    if (first_subresource > available || subresource_count_in > available - first_subresource)
      return;
    FormatBlock block = desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? FormatBlock{1, 1, 1}
                                                                         : format_block(desc->Format);
    if (!block.bytes || desc->Width > UINT_MAX - block.width + 1)
      return;
    UINT64 offset = 0, total = 0;

    for (UINT i = 0; i < subresource_count_in; i++) {
      SubresourceLocation location = {};
      if (!decompose_subresource(*desc, first_subresource + i, location))
        break;
      WMTSize extent = mip_size(*desc, location.mip);
      UINT width = align_uint(extent.width, block.width), height = align_uint(extent.height, block.height);
      UINT rows = height / block.height;
      UINT64 bytes = UINT64(width / block.width) * block.bytes;
      if (bytes > UINT_MAX - D3D12_TEXTURE_DATA_PITCH_ALIGNMENT + 1)
        return;
      UINT pitch = align_uint(UINT(bytes), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

      if (layouts) {
        layouts[i].Offset = base_offset + offset;
        layouts[i].Footprint.Format = desc->Format;
        layouts[i].Footprint.Width = width;
        layouts[i].Footprint.Height = height;
        layouts[i].Footprint.Depth = extent.depth;
        layouts[i].Footprint.RowPitch = pitch;
      }
      if (row_count)
        row_count[i] = rows;
      if (row_size)
        row_size[i] = bytes;

      /* Each subresource starts aligned, as D3D12 requires of a placed
       * footprint. */
      UINT64 size = UINT64(pitch) * rows * (extent.depth - 1) + UINT64(pitch) * (rows - 1) + bytes;
      if (offset > UINT64_MAX - size || base_offset > UINT64_MAX - offset)
        return;
      total = offset + size;
      offset = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
               ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }

    if (total_bytes)
      *total_bytes = total;
  }
  HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc, REFIID iid, void **out) final {
    if (out) *out = nullptr;
    if (!desc || !desc->Count || desc->NodeMask > 1) return E_INVALIDARG;
    if (desc->Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION && desc->Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP &&
        desc->Type != D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP) return E_NOTIMPL;
    if (!out) return S_FALSE;
    try {
      auto heap = new DxmtD3D12QueryHeap(this, d3d11_device(), *desc);
      HRESULT hr = heap->QueryInterface(iid, out); heap->Release(); return hr;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) final { return S_OK; }
  HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                                   ID3D12RootSignature *root, REFIID iid,
                                                   void **out) final {
    if (out) *out = nullptr;
    if (!desc || !desc->pArgumentDescs || !desc->NumArgumentDescs || desc->NodeMask > 1)
      return E_INVALIDARG;
    if (desc->NumArgumentDescs != 1) return E_NOTIMPL;
    UINT size = 0;
    auto type = desc->pArgumentDescs[0].Type;
    switch (type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: size = sizeof(D3D12_DRAW_ARGUMENTS); break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: size = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: size = sizeof(D3D12_DISPATCH_ARGUMENTS); break;
    default: return E_NOTIMPL;
    }
    if (root || desc->ByteStride < size || (desc->ByteStride & 3)) return E_INVALIDARG;
    if (!out) return S_FALSE;
    auto signature = new DxmtD3D12CommandSignature(this, type, desc->ByteStride, size);
    HRESULT hr = signature->QueryInterface(iid, out); signature->Release(); return hr;
  }
  void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *, UINT *, D3D12_PACKED_MIP_INFO *,
                                           D3D12_TILE_SHAPE *, UINT *, UINT,
                                           D3D12_SUBRESOURCE_TILING *) final {}
  LUID *STDMETHODCALLTYPE GetAdapterLuid(LUID *__ret) final {
    *__ret = {};
    return __ret;
  }

  ID3D11Device *d3d11_device() const { return d3d11_device_.get(); }
  ID3D11DeviceContext *context() const { return context_.get(); }

  /* Two kinds of address live in the same space. A placed buffer has a real
   * Metal GPU address; everything else is still backed by d3d11 and stands in
   * its own host pointer. Both go in this map, so a lookup is a lookup and
   * never a cast of one kind of address into the other -- reinterpreting a
   * Metal GPU address as a host pointer is a wild dereference.
   *
   * Ranges, not points: D3D12 lets an application offset into a resource and
   * hand the result to a root descriptor. */
  void register_gpu_address(uint64_t base, uint64_t size, DxmtD3D12Resource *resource) {
    if (!base || !size)
      return;
    std::lock_guard<std::mutex> lock(gpu_address_mutex_);
    auto aliases = gpu_addresses_.equal_range(base);
    for (auto it = aliases.first; it != aliases.second; ++it) {
      if (it->second.resource == resource) {
        it->second.size = size;
        return;
      }
    }
    gpu_addresses_.emplace(base, GpuAddressRange{size, resource});
  }
  void unregister_gpu_address(uint64_t base, DxmtD3D12Resource *resource) {
    if (!base)
      return;
    std::lock_guard<std::mutex> lock(gpu_address_mutex_);
    auto aliases = gpu_addresses_.equal_range(base);
    for (auto it = aliases.first; it != aliases.second;) {
      if (it->second.resource == resource) it = gpu_addresses_.erase(it);
      else ++it;
    }
  }
  DxmtD3D12Resource *resource_from_gpu_va(D3D12_GPU_VIRTUAL_ADDRESS va) const {
    if (!va)
      return nullptr;
    CpuTimingScope lookup_time(bridge_cpu_statistics().gpu_va_ticks);
    const bool profiling = present_trace().enabled();
    if (profiling) bridge_cpu_statistics().gpu_va_lookups.fetch_add(1);
    std::lock_guard<std::mutex> lock(gpu_address_mutex_);
    /* Placed buffers can overlap, including identical base addresses. Prefer
     * the newest matching alias, retaining older live ranges when one dies. */
    auto it = gpu_addresses_.upper_bound(va);
    uint64_t steps = 0;
    while (it != gpu_addresses_.begin()) {
      --it;
      ++steps;
      if (va - it->first < it->second.size) {
        if (profiling) bridge_cpu_statistics().gpu_va_steps.fetch_add(steps);
        return it->second.resource;
      }
    }
    if (profiling) bridge_cpu_statistics().gpu_va_steps.fetch_add(steps);
    return nullptr;
  }

  void register_queue(DxmtD3D12CommandQueue *queue);
  void unregister_queue(DxmtD3D12CommandQueue *queue);
  void pump_queues();
  std::recursive_mutex &submission_mutex() { return submission_mutex_; }
  DxmtD3D12CommandQueue *active_queue() const { return active_queue_; }
  DxmtD3D12CommandQueue *exchange_active_queue(DxmtD3D12CommandQueue *queue) {
    return std::exchange(active_queue_, queue);
  }
  void fail_query_resolve() { query_resolve_status_.store(DXGI_ERROR_DEVICE_REMOVED); }


private:
  DxmtD3D12CommandQueue *active_queue_ = nullptr;
  std::vector<DxmtD3D12CommandQueue *> queues_;
  std::recursive_mutex submission_mutex_;
  WMT::Device metal_device_;
  WMT::Reference<WMT::Event> aliasing_event_;
  uint64_t aliasing_event_value_ = 0;
  dxmt::Device *dxmt_device_ = nullptr;
  ComPtr<ID3D11Device> d3d11_device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11DeviceContext1> context1_;
  ComPtr<ID3D11Buffer> root_constants_;
  UINT root_constants_offset_ = 0;
  std::unique_ptr<QueryResolveWorker> query_worker_;
  WMT::Reference<WMT::SharedEvent> query_result_event_;
  uint64_t query_result_event_value_ = 0;
  std::atomic<HRESULT> query_resolve_status_ = S_OK;

  struct GpuAddressRange {
    uint64_t size;
    DxmtD3D12Resource *resource;
  };
  mutable std::mutex gpu_address_mutex_;
  std::multimap<uint64_t, GpuAddressRange> gpu_addresses_;
};

class LogicalQueueScope {
public:
  LogicalQueueScope(DxmtD3D12Device &device, DxmtD3D12CommandQueue *queue)
      : device_(device), previous_(device.exchange_active_queue(queue)) {}
  ~LogicalQueueScope() { device_.exchange_active_queue(previous_); }
private:
  DxmtD3D12Device &device_;
  DxmtD3D12CommandQueue *previous_;
};

void DxmtD3D12Resource::order_cpu_write() {
  if (!cpu_write_.value) return;
  auto dependency = std::exchange(cpu_write_, {});
  device_->submit_recorded_work(false);
  device_->dxmt_device()->queue().CurrentChunk()->emitcc(
      [dependency](ArgumentEncodingContext &enc) mutable {
        enc.waitEvent(WMT::Reference<WMT::Event>(dependency.event), dependency.value);
      });
}

void DxmtD3D12Resource::register_gpu_address() {
  if (device_)
    device_->register_gpu_address(gpu_va_, gpu_address_size(), this);
}

void DxmtD3D12Resource::unregister_gpu_address() {
  if (device_)
    device_->unregister_gpu_address(gpu_va_, this);
}

HRESULT DxmtD3D12Fence::SetEventOnCompletion(UINT64 value, HANDLE event) {
  static std::atomic<unsigned> traced = 0;
  if (d3d12_diagnostics() && traced.fetch_add(1) < 32)
    Logger::warn(str::format("D3D12 event fence=", this, " value=", value, " completed=", GetCompletedValue(), " event=", event));
  dxmt::TimelineFence *timeline = timeline_.ptr();
  dxmt::Device *dxmt_device = device_ ? device_->dxmt_device() : nullptr;
  if (!timeline || !dxmt_device)
    return E_FAIL;
  /* D3D12 specifies a blocking CPU wait when the event handle is null. */
  if (!event) return timeline->waitUntil(value, INFINITE) ? S_OK : E_FAIL;

  if (timeline->completedValue() >= value) {
    SetEvent(event);
    return S_OK;
  }

  /* The listener belongs to the queue and outlives every fence on it. */
  timeline->setWin32EventAtValue(WMT::Object(dxmt_device->queue().GetSharedEventListener()), event, value);
  return S_OK;
}

void DxmtD3D12Device::submit_recorded_work(bool commit) {
  if (!d3d11_device_)
    return;
  IMTLD3D11DeviceContext *immediate = nullptr;
  if (SUCCEEDED(context_->QueryInterface(__uuidof(IMTLD3D11DeviceContext), (void **)&immediate))) {
    immediate->PrepareFlush();
    if (commit) immediate->Commit();
    immediate->Release();
  }
}

void DxmtD3D12Device::encode_gpu_barrier() {
  if (present_trace().enabled()) bridge_cpu_statistics().gpu_barriers.fetch_add(1);
  IMTLD3D11DeviceContext *immediate = nullptr;
  if (FAILED(context_->QueryInterface(__uuidof(IMTLD3D11DeviceContext), (void **)&immediate)))
    return;
  immediate->PrepareFlush();
  immediate->Release();

  // End the preceding pass and order its GPU accesses before later work.
  // This also covers aliases, whose D3D11 trackers are unrelated.
  // A private event keeps these markers off the queue's completion timeline.
  const uint64_t value = ++aliasing_event_value_;
  dxmt_device_->queue().CurrentChunk()->emitcc(
      [event = aliasing_event_, value](ArgumentEncodingContext &enc) mutable {
        enc.signalEvent(WMT::Reference<WMT::Event>(event), value);
        enc.waitEvent(std::move(event), value);
      });
}

bool DxmtD3D12Device::defer_query_resolve(std::vector<ComPtr<ID3D11Query>> queries,
                                        D3D12_QUERY_TYPE type, DxmtD3D12Resource *destination,
                                        UINT64 offset) {
  if (!asynchronous_query_resolve_enabled() || queries.empty() || !destination ||
      !destination->metal_buffer()) return false;
  const LONGLONG prepare_start = present_trace().begin();
  using SharedBuffer = HRESULT (WINAPI *)(ID3D11Device *, const D3D11_BUFFER_DESC *, ID3D11Buffer **);
  using WaitForSubmission = HRESULT (WINAPI *)(ID3D11Device *, UINT64);
  auto wait_for_submission = reinterpret_cast<WaitForSubmission>(
      GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "DXMTWaitForSubmission"));
  auto create_shared = reinterpret_cast<SharedBuffer>(
      GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "DXMTCreateSharedBuffer"));
  if (!create_shared || !wait_for_submission) return false;
  static const bool host_readback = [] {
    char value[8] = {};
    return !GetEnvironmentVariableA("DXMT_D3D12_HOST_READBACK", value, sizeof(value)) || value[0] != '0';
  }();
  if (host_readback && active_queue_ && destination->readback_write_pointer()) {
    if (d3d12_diagnostics()) {
      static std::atomic<unsigned> traced_host_readbacks = 0;
      if (traced_host_readbacks.fetch_add(1) < 8)
        Logger::warn(str::format("D3D12 host readback queue=", active_queue_, " type=", UINT(type),
                                 " queries=", queries.size(), " offset=", offset));
    }
    if (!query_result_event_) query_result_event_ = metal_device_.newSharedEvent();
    if (!query_result_event_) return false;
    if (!query_worker_) query_worker_ = std::make_unique<QueryResolveWorker>();
    CpuReadbackDependency dependency{query_result_event_, ++query_result_event_value_};
    active_queue_->set_cpu_readback(dependency);
    destination->set_cpu_write(dependency);
    auto *mapped = reinterpret_cast<uint8_t *>(destination->readback_write_pointer()) + offset;
    const uint64_t producer = dxmt_device_->queue().CurrentSeqId();
    submit_recorded_work();
    const LONGLONG queued_at = present_trace().begin();
    query_worker_->submit([this, wait_for_submission, producer, queries = std::move(queries),
                           target = ComPtr<DxmtD3D12Resource>(destination), mapped,
                           dependency, type, queued_at]() mutable {
      const LONGLONG started_at = present_trace().begin();
      HRESULT status = wait_for_submission(d3d11_device_.get(), producer);
      const LONGLONG ready_at = present_trace().begin();
      if (DWORD delay = query_readback_delay()) Sleep(delay);
      for (size_t i = 0; i < queries.size() && SUCCEEDED(status); ++i) {
        UINT64 value = 0;
        status = context_->GetData(queries[i].get(), &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (status == S_FALSE) status = E_FAIL;
        if (SUCCEEDED(status)) {
          if (type == D3D12_QUERY_TYPE_BINARY_OCCLUSION) value = value != 0;
          std::memcpy(mapped + i * sizeof(value), &value, sizeof(value));
        }
      }
      if (FAILED(status)) {
        ERR("D3D12: host query readback failed: ", status);
        fail_query_resolve();
      }
      std::atomic_thread_fence(std::memory_order_release);
      dependency.event.signalValue(dependency.value);
      if (queued_at) {
        auto &stats = bridge_cpu_statistics();
        stats.query_jobs.fetch_add(1);
        stats.query_queue_ticks.fetch_add(started_at - queued_at);
        stats.query_wait_ticks.fetch_add(ready_at - started_at);
        stats.query_cpu_ticks.fetch_add(present_trace().begin() - ready_at);
      }
    });
    if (prepare_start) bridge_cpu_statistics().query_prepare_ticks.fetch_add(present_trace().begin() - prepare_start);
    return true;
  }
  const UINT bytes = UINT(queries.size() * sizeof(UINT64));
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = bytes;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Buffer> upload;
  if (FAILED(create_shared(d3d11_device_.get(), &desc, upload.put()))) return false;
  auto source = GetResourceCommon(upload.get())->buffer();
  auto *allocation = source ? source->current() : nullptr;
  if (!allocation || allocation->flags().test(BufferAllocationFlag::CpuInvisible) ||
      allocation->flags().test(BufferAllocationFlag::GpuManaged)) return false;
  auto *mapped = static_cast<UINT64 *>(allocation->mappedMemory(allocation->currentSuballocation()));
  if (!mapped) return false;
  if (!query_result_event_) query_result_event_ = metal_device_.newSharedEvent();
  if (!query_result_event_) return false;
  if (!query_worker_) {
    query_worker_ = std::make_unique<QueryResolveWorker>();
    if (d3d12_diagnostics()) WARN("D3D12 asynchronous query readback enabled");
  }

  // Finish the producer chunk first. Its CPU fence advances only after the
  // query readback objects have published their actual GPU results.
  auto *queue = &dxmt_device_->queue();
  const uint64_t producer = queue->CurrentSeqId();
  submit_recorded_work();
  const uint64_t value = ++query_result_event_value_;
  const LONGLONG queued_at = present_trace().begin();
  query_worker_->submit([this, wait_for_submission, producer, queries = std::move(queries), source, mapped,
                         type, event = query_result_event_, value, queued_at]() mutable {
    const LONGLONG started_at = present_trace().begin();
    HRESULT status = wait_for_submission(d3d11_device_.get(), producer);
    const LONGLONG ready_at = present_trace().begin();
    if (DWORD delay = query_readback_delay()) Sleep(delay);
    for (size_t i = 0; i < queries.size() && SUCCEEDED(status); ++i) {
      status = context_->GetData(queries[i].get(), mapped + i, sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (status == S_FALSE) status = E_FAIL;
      if (SUCCEEDED(status) && type == D3D12_QUERY_TYPE_BINARY_OCCLUSION) mapped[i] = mapped[i] != 0;
    }
    if (FAILED(status)) {
      ERR("D3D12: asynchronous query result failed: ", status);
      query_resolve_status_.store(DXGI_ERROR_DEVICE_REMOVED);
    }
    std::atomic_thread_fence(std::memory_order_release);
    // Release later GPU work even on device loss so teardown cannot deadlock.
    event.signalValue(value);
    if (queued_at) {
      auto &stats = bridge_cpu_statistics();
      stats.query_jobs.fetch_add(1);
      stats.query_queue_ticks.fetch_add(started_at - queued_at);
      stats.query_wait_ticks.fetch_add(ready_at - started_at);
      stats.query_cpu_ticks.fetch_add(present_trace().begin() - ready_at);
    }
  });

  queue->CurrentChunk()->emitcc(
      [source, target = ComPtr<DxmtD3D12Resource>(destination), offset, bytes,
       event = query_result_event_, value](ArgumentEncodingContext &enc) mutable {
        enc.waitEvent(WMT::Reference<WMT::Event>(event), value);
        enc.startBlitPass();
        auto [buffer, source_offset] = enc.access<PipelineStage::Compute>(source, 0, bytes, ResourceAccess::Read);
        target->track_access<PipelineStage::Compute>(enc, ResourceAccess::Write);
        auto &copy = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
        copy.type = WMTBlitCommandCopyFromBufferToBuffer;
        copy.src = buffer->buffer();
        copy.src_offset = source_offset;
        copy.dst = target->metal_buffer();
        copy.dst_offset = target->metal_buffer_offset() + offset;
        copy.copy_length = bytes;
        enc.endPass();
      });
  if (prepare_start) bridge_cpu_statistics().query_prepare_ticks.fetch_add(present_trace().begin() - prepare_start);
  return true;
}

void DxmtD3D12Device::synchronize_compute_for_diagnostics() {
  static const bool gpu_order = [] {
    bool value = GetEnvironmentVariableA("DXMT_D3D12_GPU_ORDER_COMPUTE", nullptr, 0) != 0;
    if (value) WARN("D3D12 GPU compute ordering enabled for diagnostics");
    return value;
  }();
  if (gpu_order) encode_gpu_barrier();
  static const bool enabled = [] {
    bool value = GetEnvironmentVariableA("DXMT_D3D12_SYNC_COMPUTE", nullptr, 0) != 0;
    if (value) WARN("D3D12 compute completion waits enabled for diagnostics");
    return value;
  }();
  if (!enabled) return;
  ComPtr<IMTLD3D11DeviceContext> immediate;
  if (SUCCEEDED(context_->QueryInterface(__uuidof(IMTLD3D11DeviceContext),
                                         reinterpret_cast<void **>(immediate.put()))))
    immediate->WaitUntilGPUIdle();
}

HRESULT D3D12ChildMethods::GetDevice(REFIID riid, void **device) {
  return device_ ? device_->QueryInterface(riid, device) : E_NOINTERFACE;
}

bool DxmtD3D12RootSignature::initialize_samplers() {
  if (d3d12_diagnostics()) {
    static std::atomic<unsigned> traced_roots = 0;
    for (const auto &p : description_.parameters)
      if ((p.kind == dxmt::RootParameterKind::ShaderResourceView ||
           p.kind == dxmt::RootParameterKind::UnorderedAccessView) && traced_roots.fetch_add(1) < 32)
        Logger::warn(str::format("D3D12 legacy root descriptor kind=", UINT(p.kind),
                                 " visibility=", p.shader_visibility, " register=", p.shader_register));
  }
  static std::atomic<unsigned> traced_signatures = 0;
  if (d3d12_diagnostics() && traced_signatures.fetch_add(1) < 12) {
    Logger::warn(str::format("D3D12 signature=", this, " parameters=", description_.parameters.size()));
    for (const auto &parameter : description_.parameters) {
      Logger::warn(str::format("D3D12 root kind=", UINT(parameter.kind), " visibility=", parameter.shader_visibility,
                               " register=", parameter.shader_register, " space=", parameter.register_space));
      for (UINT i = 0; i < parameter.range_count; ++i) {
        const auto &range = description_.ranges[parameter.first_range + i];
        Logger::warn(str::format("D3D12 range kind=", UINT(range.range_type), " register=", range.base_shader_register,
                                 " space=", range.register_space, " count=", range.descriptor_count));
      }
    }
  }
  samplers_.clear();
  for (const auto &s : description_.static_samplers) {
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER(s.filter);
    desc.AddressU = D3D11_TEXTURE_ADDRESS_MODE(s.address_u);
    desc.AddressV = D3D11_TEXTURE_ADDRESS_MODE(s.address_v);
    desc.AddressW = D3D11_TEXTURE_ADDRESS_MODE(s.address_w);
    desc.MipLODBias = s.mip_lod_bias;
    desc.MaxAnisotropy = std::max(1u, s.max_anisotropy);
    desc.ComparisonFunc = D3D11_COMPARISON_FUNC(s.comparison_func);
    desc.MinLOD = s.min_lod;
    desc.MaxLOD = s.max_lod;
    if (s.border_color == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE)
      std::fill(std::begin(desc.BorderColor), std::end(desc.BorderColor), 1.0f);
    else if (s.border_color == D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK)
      desc.BorderColor[3] = 1.0f;
    ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device_->d3d11_device()->CreateSamplerState(&desc, sampler.put()))) return false;
    samplers_.push_back(std::move(sampler));
  }
  return true;
}

ID3D11Resource *DxmtD3D12Resource::d3d11_resource() {
  if (d3d11_resource_)
    return d3d11_resource_.get();
  if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return buffer_for_bind(D3D11_BIND_SHADER_RESOURCE);

  D3D11_TEXTURE2D_DESC tex = {};
  tex.Width = static_cast<UINT>(desc_.Width);
  tex.Height = desc_.Height;
  tex.MipLevels = std::max<UINT16>(1, desc_.MipLevels);
  tex.ArraySize = std::max<UINT16>(1, desc_.DepthOrArraySize);
  tex.Format = desc_.Format;
  tex.SampleDesc = desc_.SampleDesc.Count ? desc_.SampleDesc : DXGI_SAMPLE_DESC{1, 0};
  tex.Usage = D3D11_USAGE_DEFAULT;
  tex.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    tex.BindFlags |= D3D11_BIND_RENDER_TARGET;
  if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
    tex.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
  }
  if (desc_.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
    tex.BindFlags &= ~D3D11_BIND_SHADER_RESOURCE;
  if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    tex.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
  using ImportTexture = HRESULT (WINAPI *)(ID3D11Device *, D3D11_RESOURCE_DIMENSION, const void *,
                                           dxmt::Texture *, ID3D11Resource **);
  auto import_texture = reinterpret_cast<ImportTexture>(
      GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "DXMTCreateTextureFromAllocation"));
  auto alias = [&](D3D11_RESOURCE_DIMENSION dimension, const void *description) {
    return import_texture && dxmt_texture_ &&
           SUCCEEDED(import_texture(device_->d3d11_device(), dimension, description, dxmt_texture_.ptr(),
                                    d3d11_resource_.put()));
  };
  if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    if (is_placed()) {
      D3D11_TEXTURE3D_DESC1 volume = {tex.Width, tex.Height, desc_.DepthOrArraySize, tex.MipLevels, tex.Format,
                                     tex.Usage, tex.BindFlags, 0, 0, D3D11_TEXTURE_LAYOUT_UNDEFINED};
      return alias(D3D11_RESOURCE_DIMENSION_TEXTURE3D, &volume) ? d3d11_resource_.get() : nullptr;
    }
    D3D11_TEXTURE3D_DESC volume = {tex.Width, tex.Height, desc_.DepthOrArraySize, tex.MipLevels, tex.Format,
                                  tex.Usage, tex.BindFlags, 0, 0};
    ID3D11Texture3D *texture = nullptr;
    if (FAILED(device_->d3d11_device()->CreateTexture3D(&volume, nullptr, &texture))) return nullptr;
    d3d11_resource_ = texture;
    texture->Release();
    return d3d11_resource_.get();
  }
  if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
    D3D11_TEXTURE1D_DESC line = {tex.Width, tex.MipLevels, tex.ArraySize, tex.Format, tex.Usage,
                                tex.BindFlags, 0, 0};
    if (is_placed())
      return alias(D3D11_RESOURCE_DIMENSION_TEXTURE1D, &line) ? d3d11_resource_.get() : nullptr;
    ID3D11Texture1D *texture = nullptr;
    if (FAILED(device_->d3d11_device()->CreateTexture1D(&line, nullptr, &texture))) return nullptr;
    d3d11_resource_ = texture;
    texture->Release();
    return d3d11_resource_.get();
  }
  if (tex.Width == tex.Height && tex.ArraySize >= 6 && tex.ArraySize % 6 == 0)
    tex.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
  if (is_placed()) {
    D3D11_TEXTURE2D_DESC1 image = {tex.Width, tex.Height, tex.MipLevels, tex.ArraySize, tex.Format, tex.SampleDesc,
                                  tex.Usage, tex.BindFlags, 0, tex.MiscFlags, D3D11_TEXTURE_LAYOUT_UNDEFINED};
    return alias(D3D11_RESOURCE_DIMENSION_TEXTURE2D, &image) ? d3d11_resource_.get() : nullptr;
  }
  ID3D11Texture2D *texture = nullptr;
  if (FAILED(device_->d3d11_device()->CreateTexture2D(&tex, nullptr, &texture)))
    return nullptr;
  d3d11_resource_ = texture;
  texture->Release();
  return d3d11_resource_.get();
}

bool DxmtD3D12Resource::initialize_backing() {
  if (is_placed()) {
    if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      if (!desc_.Width || desc_.Width > UINT_MAX) return false;
      WMTBufferInfo info = {};
      info.length = desc_.Width;
      info.gpu_address = gpu_va_;
      info.memory.set(mapped_);
      info.options = heap_resource_options(heap_properties_);
      dxmt_buffer_ = new dxmt::Buffer(desc_.Width, device_->metal_device());
      auto allocation = dxmt_buffer_->importBuffer(metal_buffer_, info, metal_buffer_base_offset_);
      if (!allocation) return false;
      dxmt_buffer_->rename(std::move(allocation));
      D3D11_BUFFER_DESC desc = {};
      desc.ByteWidth = UINT(desc_.Width); desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE;
      if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
      desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
      using ImportBuffer = HRESULT (WINAPI *)(ID3D11Device *, const D3D11_BUFFER_DESC *, dxmt::Buffer *, ID3D11Buffer **);
      auto import_buffer = reinterpret_cast<ImportBuffer>(
          GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "DXMTCreateBufferFromAllocation"));
      if (!import_buffer || FAILED(import_buffer(device_->d3d11_device(), &desc, dxmt_buffer_.ptr(), &generic_buffer_)))
        return false;
      d3d11_resource_ = generic_buffer_;
    } else {
      WMTTextureInfo info = {};
      if (!texture_info_from_desc(device_->metal_device(), desc_, heap_resource_options(heap_properties_), info))
        return false;
      dxmt_texture_ = new dxmt::Texture(info, device_->metal_device());
      auto allocation = dxmt_texture_->importTexture(metal_texture_, metal_texture_id_);
      if (!allocation) return false;
      dxmt_texture_->rename(std::move(allocation));
    }
  }
  if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && !d3d11_resource_) {
    if (!desc_.Width || desc_.Width > UINT_MAX) return false;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = UINT(desc_.Width);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    if (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    HRESULT result;
#if defined(__i386__)
    if (heap_properties_.Type == D3D12_HEAP_TYPE_UPLOAD || heap_properties_.Type == D3D12_HEAP_TYPE_READBACK) {
      using Shared = HRESULT (WINAPI *)(ID3D11Device *, const D3D11_BUFFER_DESC *, ID3D11Buffer **);
      auto shared = reinterpret_cast<Shared>(GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "DXMTCreateSharedBuffer"));
      result = shared ? shared(device_->d3d11_device(), &desc, &generic_buffer_) : E_NOTIMPL;
    } else
#endif
      result = device_->d3d11_device()->CreateBuffer(&desc, nullptr, &generic_buffer_);
    if (FAILED(result)) return false;
    d3d11_resource_ = generic_buffer_;
  }
  ID3D11Resource *resource = d3d11_resource();
  if (!resource) return false;
  auto *common = GetResourceCommon(resource);
  if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    dxmt_buffer_ = common->buffer();
    if (!dxmt_buffer_) return false;
    auto *allocation = dxmt_buffer_->current();
    if (!allocation) return false;
    if (!is_placed())
      gpu_va_ = allocation->gpuAddress() + allocation->currentSuballocationOffset();
    mapped_ = allocation->mappedMemory(allocation->currentSuballocation());
    register_gpu_address();
  } else {
    dxmt_texture_ = common->texture();
    if (!dxmt_texture_ || !dxmt_texture_->current()) return false;
  }
  return true;
}

void DxmtD3D12Resource::update_buffer(ID3D11Buffer *buffer, UINT byte_width) {
  if (dxmt_buffer_) return;
  if (!buffer || cpu_data_.empty())
    return;
  device_->context()->UpdateSubresource(buffer, 0, nullptr, cpu_data_.data(), byte_width, 0);
}

ID3D11Buffer *DxmtD3D12Resource::buffer_for_bind(UINT bind) {
  if (dxmt_buffer_) {
    if (bind != D3D11_BIND_CONSTANT_BUFFER) return generic_buffer_;
    if (!constant_buffer_) {
      using CreateAlias = HRESULT (WINAPI *)(ID3D11Buffer *, const D3D11_BUFFER_DESC *, ID3D11Buffer **);
      auto create_alias = reinterpret_cast<CreateAlias>(
          GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "DXMTCreateBufferAlias"));
      D3D11_BUFFER_DESC desc = {};
      desc.ByteWidth = UINT(desc_.Width) & ~15u;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      if (!create_alias || FAILED(create_alias(generic_buffer_, &desc, &constant_buffer_))) return nullptr;
    }
    return constant_buffer_;
  }
  ID3D11Buffer **slot = bind == D3D11_BIND_VERTEX_BUFFER
                            ? &vertex_buffer_
                            : bind == D3D11_BIND_INDEX_BUFFER ? &index_buffer_
                                                               : bind == D3D11_BIND_CONSTANT_BUFFER
                                                                     ? &constant_buffer_
                                                                     : &generic_buffer_;
  if (*slot)
    return *slot;

  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = static_cast<UINT>(std::max<UINT64>(1, desc_.Width));
  if (bind == D3D11_BIND_CONSTANT_BUFFER)
    desc.ByteWidth = align_uint(desc.ByteWidth, 16);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = bind;

  ID3D11Buffer *buffer = nullptr;
  if (FAILED(device_->d3d11_device()->CreateBuffer(&desc, nullptr, &buffer)))
    return nullptr;
  *slot = buffer;
  update_buffer(buffer, desc.ByteWidth);
  return buffer;
}

/* Translates one DXIL stage with Metal Shader Converter and pulls its entry
 * point out of the resulting metallib.
 *
 * out_threadgroup_size, when given, receives the [numthreads] the shader
 * declared: Metal takes it at dispatch rather than baking it into the pipeline,
 * so the compute path cannot encode without it.
 *
 * Returns false only on a real failure; a stage the pipeline does not use is
 * not an error and leaves both outputs empty. */
static bool
convert_dxil_stage(
    WMT::Device metal_device, const D3D12_SHADER_BYTECODE &bytecode,
    const dxmt::IRVersionedRootSignatureDescriptorMirror *root_signature,
    WMT::Reference<WMT::Library> &out_library,
    WMT::Reference<WMT::Function> &out_function,
    WMTSize *out_threadgroup_size = nullptr
) {
  out_library = {};
  out_function = {};

  if (!bytecode.pShaderBytecode || !bytecode.BytecodeLength || !metal_device)
    return true;
  if (!WMTShaderConverterIsAvailable())
    return false;

  obj_handle_t converter_root_signature = NULL_OBJECT_HANDLE;
  if (root_signature) {
    WMTConstMemoryPointer descriptor;
    descriptor.set(root_signature);
    obj_handle_t error = NULL_OBJECT_HANDLE;
    converter_root_signature = WMTShaderConverterCreateRootSignature(descriptor, &error);
    if (!converter_root_signature) {
      char message[1024] = {};
      if (error) {
        WMTShaderConverterCopyErrorMessage(error, message, sizeof(message));
        WMTShaderConverterReleaseError(error);
      }
      Logger::err(std::string("D3D12 root signature conversion failed: ") + message);
      return false;
    }
  }

  /* Pin the emitted function name: the converter otherwise keeps whatever the
   * DXIL entry point was called, and nothing here can enumerate a library's
   * functions to find out. */
  static const char kEntryPointName[] = "shader_main";

  WMTShaderConverterInfo info = {};
  info.dxil.set(bytecode.pShaderBytecode);
  info.dxil_size = bytecode.BytecodeLength;
  info.rename_entry_point.set(kEntryPointName);
  info.root_signature = converter_root_signature;
  info.deployment_os = WMTShaderConverterOSDefault;

  obj_handle_t error = NULL_OBJECT_HANDLE;
  int converted = WMTShaderConverterConvert(&info, &error);

  if (converter_root_signature)
    WMTShaderConverterReleaseRootSignature(converter_root_signature);
  if (info.reflection) {
    if (GetEnvironmentVariableA("DXMT_D3D12_DIAGNOSTICS", nullptr, 0)) {
      uint64_t length = WMTShaderConverterCopyReflectionJSON(info.reflection, nullptr, 0);
      if (length && length < 128 * 1024) {
        std::vector<char> json(length + 1);
        WMTShaderConverterCopyReflectionJSON(info.reflection, json.data(), json.size());
        Logger::debug(std::string("D3D12 shader reflection: ") + json.data());
      }
    }
    uint32_t threadgroup_size[3] = {};
    if (out_threadgroup_size && WMTShaderConverterCopyComputeInfo(info.reflection, threadgroup_size))
      *out_threadgroup_size = {threadgroup_size[0], threadgroup_size[1], threadgroup_size[2]};
    WMTShaderConverterReleaseReflection(info.reflection);
  }

  if (converted != 0 || !info.metallib) {
    if (error)
      WMTShaderConverterReleaseError(error);
    return false;
  }

  WMT::Error library_error;
  out_library = metal_device.newLibrary(WMT::DispatchData(info.metallib), library_error);
  /* newLibrary retains the data, so the converter's reference is done with. */
  NSObject_release(info.metallib);
  if (!out_library)
    return false;

  /* The converter keeps the DXIL entry point name unless it is told to rename,
   * and it is the only function in the library. */
  out_function = out_library.newFunction(kEntryPointName);
  return bool(out_function);
}

/* Keeps the input layout, with its semantic strings, past the creation call. */
void DxmtD3D12PipelineState::capture_input_layout(const D3D12_INPUT_LAYOUT_DESC &layout) {
  semantic_names_.reserve(layout.NumElements);
  for (UINT i = 0; i < layout.NumElements; i++)
    semantic_names_.emplace_back(layout.pInputElementDescs[i].SemanticName
                                     ? layout.pInputElementDescs[i].SemanticName
                                     : "");

  input_elements_.reserve(layout.NumElements);
  for (UINT i = 0; i < layout.NumElements; i++) {
    const D3D12_INPUT_ELEMENT_DESC &source = layout.pInputElementDescs[i];
    input_elements_.push_back(dxmt::VertexInputElement{
        semantic_names_[i].c_str(), source.SemanticIndex, uint32_t(source.Format), source.InputSlot,
        source.AlignedByteOffset,
        source.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, source.InstanceDataStepRate
    });
  }
}

void DxmtD3D12PipelineState::capture_pipeline_info(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
  WMT::Device metal_device = device_->metal_device();

  /* Not a zeroed struct: several fields have to hold something Metal accepts
   * rather than nothing -- a zero max_tessellation_factor is a hard assertion
   * failure at pipeline creation even for a pipeline with no tessellation. */
  WMT::InitializeRenderPipelineInfo(metal_pipeline_info_);
  metal_pipeline_info_.vertex_function = vertex_function_.handle;
  metal_pipeline_info_.fragment_function = fragment_function_.handle;
  metal_pipeline_info_.rasterization_enabled = true;
  metal_pipeline_info_.alpha_to_coverage_enabled = desc.BlendState.AlphaToCoverageEnable;
  metal_pipeline_info_.raster_sample_count = uint8_t(std::max<UINT>(1, desc.SampleDesc.Count));
  metal_pipeline_info_.input_primitive_topology =
      WMTPrimitiveTopologyClass(desc.PrimitiveTopologyType);

  auto pixel_format = [&](DXGI_FORMAT format) {
    MTL_DXGI_FORMAT_DESC description;
    if (!metal_device || format == DXGI_FORMAT_UNKNOWN ||
        MTLQueryDXGIFormat(metal_device, format, description))
      return WMTPixelFormatInvalid;
    return description.PixelFormat;
  };

  for (UINT i = 0; i < 8; i++) {
    auto &color = metal_pipeline_info_.colors[i];
    color.pixel_format = i < desc.NumRenderTargets ? pixel_format(desc.RTVFormats[i]) : WMTPixelFormatInvalid;

    /* D3D12 keeps one blend description per target unless independent blending
     * is on, in which case each target uses its own. */
    const D3D12_RENDER_TARGET_BLEND_DESC &blend =
        desc.BlendState.RenderTarget[desc.BlendState.IndependentBlendEnable ? i : 0];
    dxmt::D3DRenderTargetBlend source = {};
    source.blend_enable = blend.BlendEnable;
    source.src_blend = blend.SrcBlend;
    source.dst_blend = blend.DestBlend;
    source.blend_op = blend.BlendOp;
    source.src_blend_alpha = blend.SrcBlendAlpha;
    source.dst_blend_alpha = blend.DestBlendAlpha;
    source.blend_op_alpha = blend.BlendOpAlpha;
    source.write_mask = blend.RenderTargetWriteMask;

    /* A factor Metal cannot express turns blending off rather than silently
     * substituting a different one. */
    if (!dxmt::colorAttachmentBlendFromD3D(source, color))
      color.blending_enabled = false;
  }

  WMTPixelFormat depth = pixel_format(desc.DSVFormat);
  metal_pipeline_info_.depth_pixel_format = depth;
  metal_pipeline_info_.stencil_pixel_format =
      desc.DepthStencilState.StencilEnable ? depth : WMTPixelFormatInvalid;

  dxmt::D3DDepthStencil depth_stencil = {};
  depth_stencil.depth_enable = desc.DepthStencilState.DepthEnable;
  depth_stencil.depth_write_all = desc.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
  depth_stencil.depth_func = desc.DepthStencilState.DepthFunc;
  depth_stencil.stencil_enable = desc.DepthStencilState.StencilEnable;
  depth_stencil.stencil_read_mask = desc.DepthStencilState.StencilReadMask;
  depth_stencil.stencil_write_mask = desc.DepthStencilState.StencilWriteMask;
  depth_stencil.front = {desc.DepthStencilState.FrontFace.StencilFailOp,
                         desc.DepthStencilState.FrontFace.StencilDepthFailOp,
                         desc.DepthStencilState.FrontFace.StencilPassOp,
                         desc.DepthStencilState.FrontFace.StencilFunc};
  depth_stencil.back = {desc.DepthStencilState.BackFace.StencilFailOp,
                        desc.DepthStencilState.BackFace.StencilDepthFailOp,
                        desc.DepthStencilState.BackFace.StencilPassOp,
                        desc.DepthStencilState.BackFace.StencilFunc};

  /* The depth-stencil state is a separate Metal object bound on the encoder,
   * not part of the pipeline; it is built here so an unmappable value is
   * caught at pipeline creation rather than mid-frame. */
  if (metal_device && dxmt::depthStencilFromD3D(depth_stencil, metal_depth_stencil_info_))
    metal_depth_stencil_ = metal_device.newDepthStencilState(metal_depth_stencil_info_);
}

WMT::RenderPipelineState
DxmtD3D12PipelineState::metal_pipeline(const uint32_t *strides, uint32_t stride_count) {
  std::lock_guard<std::mutex> lock(pipeline_mutex_);
  if (!has_metal_functions() || !strides)
    return {};

  uint64_t key = dxmt::vertexStrideCacheKey(input_elements_.data(), uint32_t(input_elements_.size()), strides,
                                            stride_count);

  auto cached = pipelines_by_stride_.find(key);
  if (cached != pipelines_by_stride_.end())
    return cached->second;

  dxmt::VertexDescriptorBuild build;
  bool built = dxmt::buildVertexDescriptor(
      input_elements_.data(), uint32_t(input_elements_.size()), shader_attributes_.data(),
      uint32_t(shader_attributes_.size()), strides, stride_count, dxmt::kVertexBufferBindPoint, build
  );
  if (!built)
    return {};

  WMTVertexDescriptorInfo vertex_descriptor = build.info();
  WMTRenderPipelineInfo info = metal_pipeline_info_;
  if (!build.attributes.empty())
    info.vertex_descriptor.set(&vertex_descriptor);

  WMT::Error error;
  auto pipeline = device_->metal_device().newRenderPipelineState(info, error);
  /* Cache the failure too: a pipeline Metal rejected for one stride set will be
   * rejected again, and retrying costs a compile every draw. */
  pipelines_by_stride_.emplace(key, pipeline);
  return pipeline;
}

/* Compute pipelines have no fixed-function state and no vertex descriptor, so
 * unlike the graphics path there is nothing to defer: the Metal pipeline is
 * built here and is the whole object. */
HRESULT DxmtD3D12PipelineState::init_compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc) {
  if (!desc)
    return E_INVALIDARG;

  dump_pipeline_shader("cs", desc->CS);

  WMT::Device metal_device = device_->metal_device();
  if (!metal_device)
    return E_FAIL;

  const dxmt::IRVersionedRootSignatureDescriptorMirror *root_signature = nullptr;
  if (auto *signature = static_cast<DxmtD3D12RootSignature *>(desc->pRootSignature))
    root_signature = signature->converter_descriptor();

  if (!convert_dxil_stage(
          metal_device, desc->CS, root_signature, compute_library_, compute_function_, &compute_threadgroup_size_
      )) {
    HRESULT hr = device_->d3d11_device()->CreateComputeShader(desc->CS.pShaderBytecode,
                                                            desc->CS.BytecodeLength, nullptr, cs_.put());
    if (SUCCEEDED(hr)) legacy_bindings_[3] = LegacyShaderBindingMask::reflect(cs_.get());
    if (d3d12_diagnostics()) {
      static std::atomic<unsigned> traced_compute_masks = 0;
      if (traced_compute_masks.fetch_add(1) < 128)
        Logger::warn(str::format("D3D12 compute shader=",
                                 dxmt::Sha1HashState::compute(desc->CS.pShaderBytecode,
                                                            desc->CS.BytecodeLength).string().substr(0, 8),
                                 " UAV mask=", legacy_compute_uav_mask()));
    }
    return hr;
  }
  if (!compute_function_)
    return E_INVALIDARG;
  /* A dispatch with a zero-sized threadgroup is a Metal validation failure, and
   * the reflection is the only place the size comes from. */
  if (!compute_threadgroup_size_.width || !compute_threadgroup_size_.height ||
      !compute_threadgroup_size_.depth)
    return E_INVALIDARG;

  WMT::Error error;
  compute_pipeline_ = metal_device.newComputePipelineState(compute_function_, error);
  return compute_pipeline_ ? S_OK : E_FAIL;
}

HRESULT DxmtD3D12PipelineState::init(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc) {
  if (!desc)
    return E_INVALIDARG;

  dump_pipeline_shader("vs", desc->VS);
  dump_pipeline_shader("ps", desc->PS);
  dump_pipeline_shader("gs", desc->GS);

  /* Translate first: a real D3D12 application supplies DXIL, which the d3d11
   * path below cannot compile at all, so the Metal result is what decides
   * whether the pipeline exists. */
  const dxmt::IRVersionedRootSignatureDescriptorMirror *root_signature = nullptr;
  if (auto *signature = static_cast<DxmtD3D12RootSignature *>(desc->pRootSignature))
    root_signature = signature->converter_descriptor();

  WMT::Device metal_device = device_->metal_device();
  // The native vertex/fragment encoder cannot execute a geometry stage.
  bool metal_ok = !desc->GS.BytecodeLength &&
                  convert_dxil_stage(metal_device, desc->VS, root_signature, vertex_library_, vertex_function_) &&
                  convert_dxil_stage(metal_device, desc->PS, root_signature, fragment_library_, fragment_function_);
  static const bool trace_pipelines = GetEnvironmentVariableA("DXMT_D3D12_PIPELINE_TRACE", nullptr, 0) != 0;
  static std::atomic<unsigned> traced_pipelines = 0;
  if (trace_pipelines && traced_pipelines.fetch_add(1) < 256)
    Logger::warn(str::format("D3D12 pipeline path=", metal_ok && vertex_function_ ? "metal" : "dxbc",
                             " targets=", desc->NumRenderTargets, " depth=", desc->DepthStencilState.DepthEnable,
                             " depthFormat=", UINT(desc->DSVFormat), " vsBytes=", desc->VS.BytecodeLength,
                             " psBytes=", desc->PS.BytecodeLength, " gsBytes=", desc->GS.BytecodeLength,
                             " hsBytes=", desc->HS.BytecodeLength, " dsBytes=", desc->DS.BytecodeLength));

  if (metal_ok && vertex_function_) {
    /* The converter numbers attributes by declaration order including system
     * values, so the numbering has to come from the function itself. */
    uint32_t attribute_count = vertex_function_.vertexAttributes(nullptr, 0);
    shader_attributes_.resize(attribute_count);
    if (attribute_count)
      vertex_function_.vertexAttributes(shader_attributes_.data(), attribute_count);

    capture_input_layout(desc->InputLayout);
    capture_pipeline_info(*desc);
  }

  sample_mask_ = desc->SampleMask;
  auto device = device_->d3d11_device();

  std::vector<D3D11_INPUT_ELEMENT_DESC> elems(desc->InputLayout.NumElements);
  for (UINT i = 0; i < desc->InputLayout.NumElements; i++) {
    const auto &src = desc->InputLayout.pInputElementDescs[i];
    elems[i] = {src.SemanticName,
                src.SemanticIndex,
                src.Format,
                src.InputSlot,
                src.AlignedByteOffset,
                src.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                    ? D3D11_INPUT_PER_INSTANCE_DATA
                    : D3D11_INPUT_PER_VERTEX_DATA,
                src.InstanceDataStepRate};
  }

  /* Everything below builds the d3d11 objects the current execution path draws
   * with. They cannot be built from DXIL, so once the Metal translation has
   * succeeded their failure is expected rather than fatal: the pipeline is
   * valid, it simply is not drawable until the command list moves to Metal. */
  auto shader_stage_failed = [&](HRESULT hr) { return FAILED(hr) && !metal_ok; };

  ID3D11InputLayout *input_layout = nullptr;
  HRESULT hr = S_OK;
  if (!elems.empty())
    hr = device->CreateInputLayout(elems.data(), static_cast<UINT>(elems.size()), desc->VS.pShaderBytecode,
                                    desc->VS.BytecodeLength, &input_layout);
  if (shader_stage_failed(hr))
    return hr;
  if (SUCCEEDED(hr) && input_layout) {
    input_layout_ = input_layout;
    input_layout->Release();
  }

  ID3D11VertexShader *vs = nullptr;
  if (desc->VS.BytecodeLength)
    hr = device->CreateVertexShader(desc->VS.pShaderBytecode, desc->VS.BytecodeLength, nullptr, &vs);
  else hr = S_OK;
  if (shader_stage_failed(hr))
    return hr;
  if (SUCCEEDED(hr) && vs) {
    vs_ = vs;
    vs->Release();
  }

  ID3D11PixelShader *ps = nullptr;
  if (desc->PS.BytecodeLength)
    hr = device->CreatePixelShader(desc->PS.pShaderBytecode, desc->PS.BytecodeLength, nullptr, &ps);
  else hr = S_OK;
  if (shader_stage_failed(hr))
    return hr;
  if (SUCCEEDED(hr) && ps) {
    ps_ = ps;
    ps->Release();
  }
  if (desc->GS.BytecodeLength) {
    hr = device->CreateGeometryShader(desc->GS.pShaderBytecode, desc->GS.BytecodeLength, nullptr, gs_.put());
    if (FAILED(hr)) return hr;
  }
  legacy_bindings_[0] = LegacyShaderBindingMask::reflect(vs_.get());
  legacy_bindings_[1] = LegacyShaderBindingMask::reflect(ps_.get());
  legacy_bindings_[2] = LegacyShaderBindingMask::reflect(gs_.get());
  if (d3d12_diagnostics() && ps_) {
    ComPtr<IMTLD3D11Shader> shader;
    if (SUCCEEDED(ps_->QueryInterface(__uuidof(IMTLD3D11Shader), reinterpret_cast<void **>(shader.put())))) {
      if (auto *managed = shader->GetManagedShader(); managed && managed->reflection().UAVSlotMask) {
        static std::atomic<unsigned> traced_uavs = 0;
        if (traced_uavs.fetch_add(1) < 32)
          Logger::warn(str::format("D3D12 pixel shader UAV mask=", managed->reflection().UAVSlotMask,
                                   " targets=", desc->NumRenderTargets, " bytes=", desc->PS.BytecodeLength));
      }
    }
  }

  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = static_cast<D3D11_FILL_MODE>(desc->RasterizerState.FillMode);
  rs.CullMode = static_cast<D3D11_CULL_MODE>(desc->RasterizerState.CullMode);
  rs.FrontCounterClockwise = desc->RasterizerState.FrontCounterClockwise;
  rs.DepthBias = desc->RasterizerState.DepthBias;
  rs.DepthBiasClamp = desc->RasterizerState.DepthBiasClamp;
  rs.SlopeScaledDepthBias = desc->RasterizerState.SlopeScaledDepthBias;
  rs.DepthClipEnable = desc->RasterizerState.DepthClipEnable;
  rs.ScissorEnable = TRUE;
  rs.MultisampleEnable = desc->RasterizerState.MultisampleEnable;
  rs.AntialiasedLineEnable = desc->RasterizerState.AntialiasedLineEnable;
  ID3D11RasterizerState *rasterizer = nullptr;
  hr = device->CreateRasterizerState(&rs, &rasterizer);
  if (FAILED(hr))
    return hr;
  rasterizer_ = rasterizer;
  rasterizer->Release();

  D3D11_BLEND_DESC blend = {};
  blend.AlphaToCoverageEnable = desc->BlendState.AlphaToCoverageEnable;
  blend.IndependentBlendEnable = desc->BlendState.IndependentBlendEnable;
  for (UINT i = 0; i < 8; i++) {
    const auto &src = desc->BlendState.RenderTarget[i];
    auto &dst = blend.RenderTarget[i];
    dst.BlendEnable = src.BlendEnable;
    dst.SrcBlend = static_cast<D3D11_BLEND>(src.SrcBlend ? src.SrcBlend : D3D12_BLEND_ONE);
    dst.DestBlend = static_cast<D3D11_BLEND>(src.DestBlend ? src.DestBlend : D3D12_BLEND_ZERO);
    dst.BlendOp = static_cast<D3D11_BLEND_OP>(src.BlendOp ? src.BlendOp : D3D12_BLEND_OP_ADD);
    dst.SrcBlendAlpha = static_cast<D3D11_BLEND>(src.SrcBlendAlpha ? src.SrcBlendAlpha : D3D12_BLEND_ONE);
    dst.DestBlendAlpha = static_cast<D3D11_BLEND>(src.DestBlendAlpha ? src.DestBlendAlpha : D3D12_BLEND_ZERO);
    dst.BlendOpAlpha = static_cast<D3D11_BLEND_OP>(src.BlendOpAlpha ? src.BlendOpAlpha : D3D12_BLEND_OP_ADD);
    dst.RenderTargetWriteMask = src.RenderTargetWriteMask;
  }
  ID3D11BlendState *blend_state = nullptr;
  hr = device->CreateBlendState(&blend, &blend_state);
  if (FAILED(hr))
    return hr;
  blend_ = blend_state;
  blend_state->Release();

  D3D11_DEPTH_STENCIL_DESC ds = {};
  ds.DepthEnable = desc->DepthStencilState.DepthEnable;
  ds.DepthWriteMask = static_cast<D3D11_DEPTH_WRITE_MASK>(desc->DepthStencilState.DepthWriteMask);
  ds.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(
      desc->DepthStencilState.DepthFunc ? desc->DepthStencilState.DepthFunc : D3D12_COMPARISON_FUNC_LESS);
  ds.StencilEnable = desc->DepthStencilState.StencilEnable;
  ds.StencilReadMask = desc->DepthStencilState.StencilReadMask;
  ds.StencilWriteMask = desc->DepthStencilState.StencilWriteMask;
  const auto &front = desc->DepthStencilState.FrontFace;
  const auto &back = desc->DepthStencilState.BackFace;
  ds.FrontFace = {D3D11_STENCIL_OP(front.StencilFailOp), D3D11_STENCIL_OP(front.StencilDepthFailOp),
                   D3D11_STENCIL_OP(front.StencilPassOp), D3D11_COMPARISON_FUNC(front.StencilFunc)};
  ds.BackFace = {D3D11_STENCIL_OP(back.StencilFailOp), D3D11_STENCIL_OP(back.StencilDepthFailOp),
                  D3D11_STENCIL_OP(back.StencilPassOp), D3D11_COMPARISON_FUNC(back.StencilFunc)};
  ID3D11DepthStencilState *depth_state = nullptr;
  hr = device->CreateDepthStencilState(&ds, &depth_state);
  if (FAILED(hr))
    return hr;
  depth_stencil_ = depth_state;
  depth_state->Release();


  return S_OK;
}

void DxmtD3D12PipelineState::apply(ID3D11DeviceContext *context, const FLOAT *blend_factor, UINT stencil_ref) {
  context->IASetInputLayout(input_layout_.get());
  context->VSSetShader(vs_.get(), nullptr, 0);
  context->PSSetShader(ps_.get(), nullptr, 0);
  context->GSSetShader(gs_.get(), nullptr, 0);
  context->RSSetState(rasterizer_.get());
  context->OMSetBlendState(blend_.get(), blend_factor, sample_mask_);
  context->OMSetDepthStencilState(depth_stencil_.get(), stencil_ref);
}

class DxmtD3D12GraphicsCommandList final : public ID3D12GraphicsCommandList,
                                           public D3D12ChildMethods,
                                           public UnknownRef {
  struct RecordedCommand;
public:
  DxmtD3D12GraphicsCommandList(DxmtD3D12Device *device, D3D12_COMMAND_LIST_TYPE type,
                               ID3D12PipelineState *initial_state)
      : D3D12ChildMethods(device), type_(type) {
    pipeline_ = static_cast<DxmtD3D12PipelineState *>(initial_state);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final {
    if (!ppvObject)
      return E_POINTER;
    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList)) {
      *ppvObject = static_cast<ID3D12GraphicsCommandList *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() final { return add_ref(); }
  ULONG STDMETHODCALLTYPE Release() final { return release_ref(); }
  DXMT_D3D12_OBJECT_OVERRIDES

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() final { return type_; }
  HRESULT STDMETHODCALLTYPE Close() final {
    closed_ = true;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *, ID3D12PipelineState *initial_state) final {
    commands_.clear();
    closed_ = false;
    pipeline_ = static_cast<DxmtD3D12PipelineState *>(initial_state);
    vbvs_ = {}; ibv_ = {}; root_descriptors_ = {}; compute_root_descriptors_ = {};
    graphics_root_arguments_.setLayout(nullptr); compute_root_arguments_.setLayout(nullptr);
    graphics_root_signature_ = nullptr; compute_root_signature_ = nullptr;
    graphics_root_resources_ = {}; compute_root_resources_ = {};
    resource_heap_ = nullptr; sampler_heap_ = nullptr;
    topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    viewport_ = {}; has_viewport_ = false;
    scissor_ = {}; has_scissor_ = false;
    render_target_ = {}; pending_depth_clear_ = false; pending_depth_value_ = 1;
    blend_factor_ = {1,1,1,1}; stencil_ref_ = 0;
    return S_OK;
  }
  void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pipeline_state) final {
    pipeline_ = static_cast<DxmtD3D12PipelineState *>(pipeline_state);
  }
  void STDMETHODCALLTYPE DrawInstanced(UINT vertex_count_per_instance, UINT instance_count,
                                       UINT start_vertex_location, UINT start_instance_location) final {
    CpuTimingScope recording_time(bridge_cpu_statistics().record_draw_ticks);
    /* A pipeline with Metal shaders behind it and a placed render target is
     * one this can encode itself; anything else still goes through d3d11. */
    if (record_metal_draw(vertex_count_per_instance, instance_count, start_vertex_location,
                          start_instance_location))
      return;

    auto pipeline = pipeline_;
    auto vbvs = vbvs_;
    auto bindings = legacy_root_bindings();
    auto blend_factor = blend_factor_;
    auto stencil_ref = stencil_ref_;
    record([=, this]() {
      CpuTimingScope drawing_time(bridge_cpu_statistics().legacy_draw_ticks);
      auto context = device_->context();
      if (pipeline)
        pipeline->apply(context, blend_factor.data(), stencil_ref);

      bind_legacy_graphics(bindings, pipeline.get());

      bind_legacy_vertex_buffers(vbvs);

      context->DrawInstanced(vertex_count_per_instance, instance_count, start_vertex_location,
                             start_instance_location);
    });
  }

  /* Returns true when the draw was recorded for Metal. Everything the encoding
   * needs is copied out here: the application may rebind and redraw before any
   * of this executes. */
  bool record_metal_draw(UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance,
                         bool indexed = false, INT base_vertex = 0) {
    auto *pipeline = pipeline_.get();
    if (!pipeline || !device_ || !device_->dxmt_device() || !render_target_.count)
      return false;

    std::array<uint32_t, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> strides = {};
    for (UINT i = 0; i < strides.size(); ++i) strides[i] = vbvs_[i].StrideInBytes;
    WMT::RenderPipelineState metal = pipeline->metal_pipeline(strides.data(), strides.size());
    if (!metal)
      return false;

    WMTPrimitiveType primitive;
    if (!primitive_type_from_d3d(topology_, primitive))
      return false;

    auto work = std::make_shared<GraphicsDrawWork>();
    work->pipeline = metal;
    work->pipeline_owner = pipeline_;
    work->depth_stencil = pipeline->metal_depth_stencil();
    work->primitive = primitive;
    work->vertex_start = start_vertex;
    work->vertex_count = vertex_count;
    work->instance_count = instance_count ? instance_count : 1;
    work->base_instance = start_instance;
    work->root_arguments.assign(
        static_cast<const uint8_t *>(graphics_root_arguments_.data()),
        static_cast<const uint8_t *>(graphics_root_arguments_.data()) + graphics_root_arguments_.size()
    );

    for (UINT i = 0; i < vbvs_.size(); ++i) {
      if (auto *vb = device_->resource_from_gpu_va(vbvs_[i].BufferLocation)) {
        if (WMT::Buffer buffer = vb->metal_buffer()) {
          uint64_t offset = vb->metal_buffer_offset() + vbvs_[i].BufferLocation - vb->GetGPUVirtualAddress();
          work->resident.push_back({WMT::Reference<WMT::Resource>(WMT::Resource(buffer.handle)), vb,
                                     WMTResourceUsageRead});
          work->vertex_buffers.push_back({buffer, offset, uint8_t(dxmt::kVertexBufferBindPoint + i)});
        }
      }
    }

    if (indexed) {
      DxmtD3D12Resource *ib = device_->resource_from_gpu_va(ibv_.BufferLocation);
      WMT::Buffer buffer = ib ? ib->metal_buffer() : WMT::Buffer();
      if (!buffer)
        return false;
      if (ibv_.Format != DXGI_FORMAT_R16_UINT && ibv_.Format != DXGI_FORMAT_R32_UINT)
        return false;
      uint64_t base = ib->GetGPUVirtualAddress();
      uint64_t view_offset = ibv_.BufferLocation > base ? ibv_.BufferLocation - base : 0;
      uint32_t index_size = ibv_.Format == DXGI_FORMAT_R16_UINT ? 2 : 4;

      work->indexed = true;
      work->index_buffer = buffer;
      /* D3D12 counts the first index from the view's start; Metal takes a byte
       * offset into the buffer, so the two are added here. */
      work->index_buffer_offset = ib->metal_buffer_offset() + view_offset + uint64_t(start_vertex) * index_size;
      work->resident.push_back({WMT::Reference<WMT::Resource>(WMT::Resource(buffer.handle)), ib,
                                 WMTResourceUsageRead});
      work->index_type = index_size == 2 ? WMTIndexTypeUInt16 : WMTIndexTypeUInt32;
      work->base_vertex = base_vertex;
    }

    work->viewport = viewport_;
    work->has_viewport = has_viewport_;
    const auto width = render_target_.width, height = render_target_.height;
    if (has_scissor_) {
      const auto left = std::clamp<int64_t>(scissor_.left, 0, width);
      const auto top = std::clamp<int64_t>(scissor_.top, 0, height);
      const auto right = std::clamp<int64_t>(scissor_.right, left, width);
      const auto bottom = std::clamp<int64_t>(scissor_.bottom, top, height);
      if (left == right || top == bottom) return true;
      work->scissor = WMTScissorRect{uint32_t(left), uint32_t(top), uint32_t(right-left), uint32_t(bottom-top)};
    } else work->scissor = WMTScissorRect{0, 0, width, height};

    collect_graphics_residency(*work);
    commands_.push_back(RecordedCommand{{}, false, nullptr, work, nullptr, nullptr, false, {}});
    return true;
  }

  static bool primitive_type_from_d3d(D3D12_PRIMITIVE_TOPOLOGY topology, WMTPrimitiveType &out) {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
      out = WMTPrimitiveTypePoint;
      return true;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
      out = WMTPrimitiveTypeLine;
      return true;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
      out = WMTPrimitiveTypeLineStrip;
      return true;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
      out = WMTPrimitiveTypeTriangle;
      return true;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
      out = WMTPrimitiveTypeTriangleStrip;
      return true;
    default:
      /* Adjacency and patch topologies need geometry or tessellation, which
       * this path does not set up yet. */
      return false;
    }
  }

  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT index_count_per_instance, UINT instance_count,
                                              UINT start_index_location, INT base_vertex_location,
                                              UINT start_instance_location) final {
    CpuTimingScope recording_time(bridge_cpu_statistics().record_draw_ticks);
    if (record_metal_draw(index_count_per_instance, instance_count, start_index_location,
                          start_instance_location, true, base_vertex_location))
      return;

    auto pipeline = pipeline_;
    auto vbvs = vbvs_;
    auto ibv = ibv_;
    auto bindings = legacy_root_bindings();
    auto blend_factor = blend_factor_;
    auto stencil_ref = stencil_ref_;
    record([=, this]() {
      CpuTimingScope drawing_time(bridge_cpu_statistics().legacy_draw_ticks);
      auto context = device_->context();
      if (pipeline)
        pipeline->apply(context, blend_factor.data(), stencil_ref);

      bind_legacy_graphics(bindings, pipeline.get());

      bind_legacy_vertex_buffers(vbvs);

      DxmtD3D12Resource *ib_res = device_->resource_from_gpu_va(ibv.BufferLocation);
      if (ib_res) {
        ID3D11Buffer *ib = ib_res->index_buffer();
        ib_res->update_buffer(ib, ibv.SizeInBytes);
        context->IASetIndexBuffer(ib, ibv.Format, UINT(ibv.BufferLocation - ib_res->GetGPUVirtualAddress()));
      }

      context->DrawIndexedInstanced(index_count_per_instance, instance_count,
                                    start_index_location, base_vertex_location, start_instance_location);
    });
  }
  void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) final {
    if (!x || !y || !z)
      return;

    auto *pipeline = pipeline_.get();
    if (!pipeline || !device_ || !device_->dxmt_device()) return;
    if (pipeline->legacy_compute_shader()) {
      auto owner = pipeline_;
      LegacyRootBindings bindings = {compute_root_signature_, compute_root_arguments_, compute_root_descriptors_, resource_heap_, sampler_heap_};
      record([=, this]() {
        device_->context()->CSSetShader(owner->legacy_compute_shader(), nullptr, 0);
        bind_legacy_graphics(bindings, owner.get(), true);
        device_->context()->Dispatch(x, y, z);
        device_->synchronize_compute_for_diagnostics();
      });
      return;
    }
    if (!pipeline->metal_compute_pipeline()) return;

    /* Everything the encoding needs is copied out here rather than read from
     * the command list later: the application is free to change root arguments
     * and rebind heaps for the next dispatch before this one is executed. */
    auto work = std::make_shared<ComputeDispatchWork>();
    work->pipeline = pipeline->metal_compute_pipeline();
    work->pipeline_owner = pipeline_;
    work->threadgroup_size = pipeline->compute_threadgroup_size();
    work->threadgroups = {x, y, z};
    work->root_arguments.assign(
        static_cast<const uint8_t *>(compute_root_arguments_.data()),
        static_cast<const uint8_t *>(compute_root_arguments_.data()) + compute_root_arguments_.size()
    );
    collect_compute_residency(*work);

    commands_.push_back(RecordedCommand{{}, false, work, nullptr, nullptr, nullptr, false, {}});
  }
  void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src,
                                          UINT64 src_offset, UINT64 num_bytes) final {
    auto *dst_res = static_cast<DxmtD3D12Resource *>(dst);
    auto *src_res = static_cast<DxmtD3D12Resource *>(src);
    if (!dst_res || !src_res || !num_bytes)
      return;

    /* Placed buffers have real Metal memory, so the copy is a blit rather than
     * a memcpy between shadows. */
    if (WMT::Buffer source = src_res->metal_buffer()) {
      if (WMT::Buffer destination = dst_res->metal_buffer()) {
        auto copy = std::make_shared<CopyWork>();
        copy->kind = CopyWork::Kind::BufferToBuffer;
        copy->src_buffer = source;
        copy->dst_buffer = destination;
        copy->src_resource = src_res;
        copy->dst_resource = dst_res;
        copy->src_offset = src_res->metal_buffer_offset() + src_offset;
        copy->dst_offset = dst_res->metal_buffer_offset() + dst_offset;
        copy->length = num_bytes;
        record_copy(copy);
        return;
      }
    }

    /* Buffers on this path live in a CPU shadow that is uploaded when they are
     * bound, so a region copy is a copy between shadows. Deferred like every
     * other command so it lands in submission order. */
    record([=]() {
      if (src_res->cpu_size() < src_offset + num_bytes)
        return;
      uint8_t *destination = dst_res->cpu_data_for_write(size_t(dst_offset + num_bytes));
      std::memcpy(destination + dst_offset, src_res->cpu_data() + src_offset, size_t(num_bytes));
    });
  }
  void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x,
                                           UINT dst_y, UINT dst_z, const D3D12_TEXTURE_COPY_LOCATION *src,
                                           const D3D12_BOX *box) final {
    if (!dst || !src)
      return;
    D3D12_TEXTURE_COPY_LOCATION dst_copy = *dst;
    D3D12_TEXTURE_COPY_LOCATION src_copy = *src;

    if (record_metal_texture_copy(dst_copy, dst_x, dst_y, dst_z, src_copy, box))
      return;

    const bool has_box = box != nullptr;
    const D3D12_BOX source_box = has_box ? *box : D3D12_BOX{};
    record([=]() {
      auto *dst_res = static_cast<DxmtD3D12Resource *>(dst_copy.pResource);
      auto *src_res = static_cast<DxmtD3D12Resource *>(src_copy.pResource);
      if (!dst_res || !src_res)
        return;
      const bool src_is_footprint = src_copy.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      const bool dst_is_footprint = dst_copy.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      if (src_is_footprint && !dst_is_footprint)
        copy_shadow_to_texture(src_res, src_copy.PlacedFootprint, has_box ? &source_box : nullptr, dst_res,
                               dst_copy.SubresourceIndex, dst_x, dst_y, dst_z);
      else if (!src_is_footprint && dst_is_footprint)
        copy_texture_to_shadow(src_res, src_copy.SubresourceIndex, has_box ? &source_box : nullptr, dst_res,
                               dst_copy.PlacedFootprint, dst_x, dst_y, dst_z);
      else if (!src_is_footprint && !dst_is_footprint) {
        ID3D11Resource *src11 = src_res->d3d11_resource(), *dst11 = dst_res->d3d11_resource();
        if (!src11 || !dst11)
          return;
        D3D11_BOX region = {source_box.left, source_box.top, source_box.front,
                            source_box.right, source_box.bottom, source_box.back};
        device_->context()->CopySubresourceRegion(dst11, dst_copy.SubresourceIndex, dst_x, dst_y, dst_z, src11,
                                                  src_copy.SubresourceIndex, has_box ? &region : nullptr);
      }
    });
  }

  /* The rows a buffer's shadow holds, into one subresource of a d3d11 texture at
   * the destination point; a source box picks the part of the footprint copied. */
  void copy_shadow_to_texture(DxmtD3D12Resource *src_res, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &placed,
                              const D3D12_BOX *box, DxmtD3D12Resource *dst_res, UINT subresource,
                              UINT dst_x, UINT dst_y, UINT dst_z) {
    ID3D11Resource *dst11 = dst_res->d3d11_resource();
    if (!dst11)
      return;
    const D3D12_SUBRESOURCE_FOOTPRINT &fp = placed.Footprint;
    D3D12_BOX part = box ? *box : D3D12_BOX{0, 0, 0, fp.Width, fp.Height, std::max(1u, fp.Depth)};
    const UINT64 offset = placed.Offset + UINT64(part.front) * fp.RowPitch * fp.Height +
                          UINT64(part.top) * fp.RowPitch + UINT64(part.left) * bytes_per_pixel(fp.Format);
    if (offset >= src_res->cpu_size())
      return;
    D3D11_BOX region = {dst_x, dst_y, dst_z, dst_x + part.right - part.left, dst_y + part.bottom - part.top,
                        dst_z + part.back - part.front};
    device_->context()->UpdateSubresource(dst11, subresource, &region, src_res->cpu_data() + offset, fp.RowPitch,
                                          fp.RowPitch * fp.Height);
  }

  /* d3d11 copies no texture into a buffer: the subresource goes through a staging
   * texture, is read back once the GPU has written it, and its rows land in the
   * buffer's shadow where the footprint puts them -- which is what Map then hands
   * the application. */
  void copy_texture_to_shadow(DxmtD3D12Resource *src_res, UINT subresource, const D3D12_BOX *box,
                              DxmtD3D12Resource *dst_res, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &placed,
                              UINT dst_x, UINT dst_y, UINT dst_z) {
    ComPtr<ID3D11Texture2D> source;
    ID3D11Resource *src11 = src_res->d3d11_resource();
    if (!src11 || FAILED(src11->QueryInterface(IID_PPV_ARGS(source.put()))))
      return;
    D3D11_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);
    const UINT mip = subresource % desc.MipLevels;
    D3D12_BOX part = box ? *box
                         : D3D12_BOX{0, 0, 0, std::max(1u, desc.Width >> mip), std::max(1u, desc.Height >> mip), 1};
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = part.right - part.left;
    staging_desc.Height = part.bottom - part.top;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = desc.Format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->d3d11_device()->CreateTexture2D(&staging_desc, nullptr, staging.put())))
      return;
    D3D11_BOX region = {part.left, part.top, 0, part.right, part.bottom, 1};
    device_->context()->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, src11, subresource, &region);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(device_->context()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return;
    const D3D12_SUBRESOURCE_FOOTPRINT &fp = placed.Footprint;
    const UINT texel = bytes_per_pixel(fp.Format);
    const size_t row_bytes = size_t(staging_desc.Width) * texel;
    const UINT64 first = placed.Offset + UINT64(dst_z) * fp.RowPitch * fp.Height + UINT64(dst_y) * fp.RowPitch +
                         UINT64(dst_x) * texel;
    uint8_t *rows = dst_res->cpu_data_for_write(size_t(first + UINT64(staging_desc.Height - 1) * fp.RowPitch + row_bytes));
    for (UINT y = 0; y < staging_desc.Height; y++)
      std::memcpy(rows + first + UINT64(y) * fp.RowPitch, static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                  std::min<size_t>(row_bytes, mapped.RowPitch));
    device_->context()->Unmap(staging.get(), 0);
  }

  /* The three copy shapes D3D12 expresses through one entry point, told apart
   * by each location's Type. Returns false when either side has no Metal
   * memory, which leaves the d3d11 path to it.
   *
   * Mip and array addressing is not split out of the subresource index yet, so
   * anything past subresource 0 is left alone rather than copied to the wrong
   * slice. */
  bool record_metal_texture_copy(const D3D12_TEXTURE_COPY_LOCATION &dst, UINT dst_x, UINT dst_y, UINT dst_z,
                                 const D3D12_TEXTURE_COPY_LOCATION &src, const D3D12_BOX *box) {
    auto *dst_res = static_cast<DxmtD3D12Resource *>(dst.pResource);
    auto *src_res = static_cast<DxmtD3D12Resource *>(src.pResource);
    if (!dst_res || !src_res)
      return false;

    const bool src_is_footprint = src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    const bool dst_is_footprint = dst.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

    auto copy = std::make_shared<CopyWork>();
    copy->dst_origin = {dst_x, dst_y, dst_z};

    /* A texture side addresses one subresource; Metal wants that as a level
     * and a slice. Planar formats are not handled -- Metal reaches their
     * planes through separate views rather than a slice index. */
    if (!src_is_footprint) {
      D3D12_RESOURCE_DESC desc = {};
      src_res->GetDesc(&desc);
      SubresourceLocation location = {};
      if (!decompose_subresource(desc, src.SubresourceIndex, location) || location.plane)
        return false;
      copy->src_level = location.mip;
      copy->src_slice = location.slice;
    }
    if (!dst_is_footprint) {
      D3D12_RESOURCE_DESC desc = {};
      dst_res->GetDesc(&desc);
      SubresourceLocation location = {};
      if (!decompose_subresource(desc, dst.SubresourceIndex, location) || location.plane)
        return false;
      copy->dst_level = location.mip;
      copy->dst_slice = location.slice;
    }

    if (src_is_footprint && !dst_is_footprint) {
      const D3D12_SUBRESOURCE_FOOTPRINT &footprint = src.PlacedFootprint.Footprint;
      copy->src_buffer = src_res->metal_buffer();
      copy->dst_texture = dst_res->metal_texture();
      if (!copy->src_buffer || !copy->dst_texture)
        return false;
      copy->kind = CopyWork::Kind::BufferToTexture;
      copy->src_offset = src_res->metal_buffer_offset() + src.PlacedFootprint.Offset;
      copy->bytes_per_row = footprint.RowPitch;
      copy->bytes_per_image = footprint.RowPitch * footprint.Height;
      copy->size = {footprint.Width, footprint.Height, std::max(1u, footprint.Depth)};
    } else if (!src_is_footprint && dst_is_footprint) {
      const D3D12_SUBRESOURCE_FOOTPRINT &footprint = dst.PlacedFootprint.Footprint;
      copy->src_texture = src_res->metal_texture();
      copy->dst_buffer = dst_res->metal_buffer();
      if (!copy->src_texture || !copy->dst_buffer)
        return false;
      copy->kind = CopyWork::Kind::TextureToBuffer;
      copy->dst_offset = dst_res->metal_buffer_offset() + dst.PlacedFootprint.Offset;
      copy->bytes_per_row = footprint.RowPitch;
      copy->bytes_per_image = footprint.RowPitch * footprint.Height;
      copy->size = {footprint.Width, footprint.Height, std::max(1u, footprint.Depth)};
      /* A texture source addresses the box, not the footprint. */
      if (box) {
        copy->src_origin = {box->left, box->top, box->front};
        copy->size = {box->right - box->left, box->bottom - box->top, std::max(1u, box->back - box->front)};
      }
    } else if (!src_is_footprint && !dst_is_footprint) {
      copy->src_texture = src_res->metal_texture();
      copy->dst_texture = dst_res->metal_texture();
      if (!copy->src_texture || !copy->dst_texture)
        return false;
      copy->kind = CopyWork::Kind::TextureToTexture;
      D3D12_RESOURCE_DESC desc = {};
      src_res->GetDesc(&desc);
      copy->size = mip_size(desc, copy->src_level);
      if (box) {
        copy->src_origin = {box->left, box->top, box->front};
        copy->size = {box->right - box->left, box->bottom - box->top, std::max(1u, box->back - box->front)};
      }
    } else {
      /* Buffer to buffer through this entry point is not a shape D3D12
       * defines; CopyBufferRegion is where that lives. */
      return false;
    }

    if (!copy->size.width || !copy->size.height || !copy->size.depth)
      return false;
    copy->src_resource = src_res;
    copy->dst_resource = dst_res;
    record_copy(copy);
    return true;
  }
  void STDMETHODCALLTYPE CopyResource(ID3D12Resource *dst, ID3D12Resource *src) final {
    auto *dst_res = static_cast<DxmtD3D12Resource *>(dst);
    auto *src_res = static_cast<DxmtD3D12Resource *>(src);
    if (!dst_res || !src_res)
      return;

    D3D12_RESOURCE_DESC src_desc = {};
    src_res->GetDesc(&src_desc);
    bool buffers = src_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;

    /* Whole-resource copy between placed resources: a blit, like the region
     * forms. Anything with mips or array slices is left to the path below,
     * which at least does not copy to the wrong slice. */
    if (buffers && src_res->metal_buffer() && dst_res->metal_buffer()) {
      auto copy = std::make_shared<CopyWork>();
      copy->kind = CopyWork::Kind::BufferToBuffer;
      copy->src_buffer = src_res->metal_buffer();
      copy->dst_buffer = dst_res->metal_buffer();
      copy->src_resource = src_res;
      copy->dst_resource = dst_res;
      copy->src_offset = src_res->metal_buffer_offset();
      copy->dst_offset = dst_res->metal_buffer_offset();
      copy->length = src_desc.Width;
      record_copy(copy);
      return;
    }
    if (!buffers && src_res->metal_texture() && dst_res->metal_texture()) {
      /* Whole resource means every subresource: one blit per mip and slice,
       * each with its own extent. */
      UINT count = subresource_count(src_desc);
      for (UINT i = 0; i < count; i++) {
        SubresourceLocation location = {};
        if (!decompose_subresource(src_desc, i, location) || location.plane)
          continue;
        auto copy = std::make_shared<CopyWork>();
        copy->kind = CopyWork::Kind::TextureToTexture;
        copy->src_texture = src_res->metal_texture();
        copy->dst_texture = dst_res->metal_texture();
        copy->src_resource = src_res;
        copy->dst_resource = dst_res;
        copy->src_level = location.mip;
        copy->src_slice = location.slice;
        copy->dst_level = location.mip;
        copy->dst_slice = location.slice;
        copy->size = mip_size(src_desc, location.mip);
        record_copy(copy);
      }
      return;
    }

    record([=]() {
      if (buffers) {
        size_t size = src_res->cpu_size();
        if (!size)
          return;
        std::memcpy(dst_res->cpu_data_for_write(size), src_res->cpu_data(), size);
        return;
      }
      ID3D11Resource *dst11 = dst_res->d3d11_resource();
      ID3D11Resource *src11 = src_res->d3d11_resource();
      if (dst11 && src11)
        device_->context()->CopyResource(dst11, src11);
    });
  }
  void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *,
                                   const D3D12_TILE_REGION_SIZE *, ID3D12Resource *, UINT64,
                                   D3D12_TILE_COPY_FLAGS) final {}
  void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *dst, UINT dst_subresource,
                                            ID3D12Resource *src, UINT src_subresource,
                                            DXGI_FORMAT format) final {
    auto *dst_res = static_cast<DxmtD3D12Resource *>(dst);
    auto *src_res = static_cast<DxmtD3D12Resource *>(src);
    if (!dst_res || !src_res)
      return;

    record([=]() {
      ID3D11Resource *dst11 = dst_res->d3d11_resource();
      ID3D11Resource *src11 = src_res->d3d11_resource();
      if (dst11 && src11)
        device_->context()->ResolveSubresource(dst11, dst_subresource, src11, src_subresource, format);
    });
  }
  void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) final {
    topology_ = topology;
    record_state([=]() { device_->context()->IASetPrimitiveTopology(topology); });
  }
  void STDMETHODCALLTYPE RSSetViewports(UINT count, const D3D12_VIEWPORT *viewports) final {
    if (count && viewports) {
      viewport_ = {viewports[0].TopLeftX,  viewports[0].TopLeftY, viewports[0].Width,
                   viewports[0].Height,    viewports[0].MinDepth, viewports[0].MaxDepth};
      has_viewport_ = true;
    }
    std::vector<D3D11_VIEWPORT> copied(count);
    for (UINT i = 0; i < count; i++) {
      copied[i] = {viewports[i].TopLeftX, viewports[i].TopLeftY, viewports[i].Width,
                   viewports[i].Height, viewports[i].MinDepth, viewports[i].MaxDepth};
    }
    record_state([=]() { device_->context()->RSSetViewports(count, copied.data()); });
  }
  void STDMETHODCALLTYPE RSSetScissorRects(UINT count, const D3D12_RECT *rects) final {
    has_scissor_ = count && rects;
    scissor_ = has_scissor_ ? rects[0] : D3D12_RECT{};
    std::vector<D3D11_RECT> copied(count);
    for (UINT i = 0; i < count; i++)
      copied[i] = {rects[i].left, rects[i].top, rects[i].right, rects[i].bottom};
    record_state([=]() { device_->context()->RSSetScissorRects(count, copied.data()); });
  }
  void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT factor[4]) final {
    std::copy(factor, factor + 4, blend_factor_.begin());
  }
  void STDMETHODCALLTYPE OMSetStencilRef(UINT value) final { stencil_ref_ = value; }
  void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *pipeline_state) final {
    pipeline_ = static_cast<DxmtD3D12PipelineState *>(pipeline_state);
  }
  /* Keep application barriers for both native and DXBC commands. D3D11
   * resource tracking alone does not preserve D3D12's explicit UAV handoff. */
  void STDMETHODCALLTYPE ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER *barriers) final {
    for (UINT i = 0; i < count && barriers; i++) {
      const D3D12_RESOURCE_BARRIER &barrier = barriers[i];
      dxmt::BarrierAction action = {};
      dxmt::BarrierResource resource = {};

      switch (barrier.Type) {
      case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION: {
        auto *target = static_cast<DxmtD3D12Resource *>(barrier.Transition.pResource);
        resource = barrier_resource(target);
        action = dxmt::translateTransitionBarrier(resource, barrier.Transition.StateBefore,
                                                  barrier.Transition.StateAfter);
        // UAV results must be visible before a later pass consumes them,
        // including committed resources whose Metal hazards are tracked.
        if ((barrier.Transition.StateBefore & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
            barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
          action.kind = dxmt::BarrierKind::EncoderSplit;
          action.needs_fence = true;
        }
        break;
      }
      case D3D12_RESOURCE_BARRIER_TYPE_UAV:
        resource = barrier_resource(static_cast<DxmtD3D12Resource *>(barrier.UAV.pResource));
        action = dxmt::translateUnorderedAccessBarrier(resource);
        break;
      case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
        if (d3d12_diagnostics()) {
          static std::atomic<unsigned> traced_aliases = 0;
          if (traced_aliases.fetch_add(1) < 24)
            Logger::warn(str::format("D3D12 aliasing barrier before=", barrier.Aliasing.pResourceBefore,
                                     " after=", barrier.Aliasing.pResourceAfter));
        }
        action = dxmt::translateAliasingBarrier();
        break;
      default:
        continue;
      }

      if (action.kind == dxmt::BarrierKind::None)
        continue;
      commands_.push_back(RecordedCommand{{}, false, nullptr, nullptr, nullptr, nullptr, true,
                                          action.scope ? action.scope : WMTBarrierScopeBuffers,
                                          barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING,
                                          action.kind == dxmt::BarrierKind::EncoderSplit});
    }
  }

  /* Placed resources are created with hazard tracking off -- see
   * heap_resource_options -- so Metal will not order them for us. */
  static dxmt::BarrierResource barrier_resource(DxmtD3D12Resource *resource) {
    if (!resource)
      return {false, false};
    D3D12_RESOURCE_DESC desc = {};
    resource->GetDesc(&desc);
    return {desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER, !resource->is_placed()};
  }
  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *) final {
    static std::atomic<unsigned> calls = 0;
    if (d3d12_diagnostics() && calls.fetch_add(1) < 8)
      ERR("D3D12: bundle execution is not implemented");
  }
  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap *const *heaps) final {
    /* D3D12 allows one heap of each type to be bound at a time, and the
     * converted shaders read them from two fixed buffer slots. */
    resource_heap_ = nullptr;
    sampler_heap_ = nullptr;
    for (UINT i = 0; i < count && heaps; i++) {
      auto *heap = static_cast<DxmtD3D12DescriptorHeap *>(heaps[i]);
      if (!heap)
        continue;
      D3D12_DESCRIPTOR_HEAP_DESC desc = {};
      heap->GetDesc(&desc);
      if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
        sampler_heap_ = heap;
      else
        resource_heap_ = heap;
    }
  }
  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *root_signature) final {
    bind_root_signature(root_signature, compute_root_signature_, compute_root_arguments_);
  }
  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *root_signature) final {
    bind_root_signature(root_signature, graphics_root_signature_, graphics_root_arguments_);
  }
  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT index,
                                                       D3D12_GPU_DESCRIPTOR_HANDLE handle) final {
    if (index < compute_root_descriptors_.size()) compute_root_descriptors_[index] = handle;
    compute_root_arguments_.setDescriptorTable(index, descriptor_table_address(handle));
  }
  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT index,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE handle) final {
    if (index < root_descriptors_.size())
      root_descriptors_[index] = handle;
    graphics_root_arguments_.setDescriptorTable(index, descriptor_table_address(handle));
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT index, UINT value, UINT offset) final {
    compute_root_arguments_.setConstants(index, offset, &value, 1);
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT offset) final {
    graphics_root_arguments_.setConstants(index, offset, &value, 1);
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT index, UINT count, const void *data,
                                                      UINT offset) final {
    compute_root_arguments_.setConstants(index, offset, data, count);
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT index, UINT count, const void *data,
                                                       UINT offset) final {
    graphics_root_arguments_.setConstants(index, offset, data, count);
  }
  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT index,
                                                          D3D12_GPU_VIRTUAL_ADDRESS address) final {
    compute_root_arguments_.setRootDescriptor(index, root_descriptor_address(address));
    track_root_resource(index, address);
  }
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) final {
    graphics_root_arguments_.setRootDescriptor(index, root_descriptor_address(address));
    track_graphics_root_resource(index, address);
  }
  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT index,
                                                          D3D12_GPU_VIRTUAL_ADDRESS address) final {
    compute_root_arguments_.setRootDescriptor(index, root_descriptor_address(address));
    track_root_resource(index, address);
  }
  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) final {
    graphics_root_arguments_.setRootDescriptor(index, root_descriptor_address(address));
    track_graphics_root_resource(index, address);
  }
  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) final {
    compute_root_arguments_.setRootDescriptor(index, root_descriptor_address(address));
    track_root_resource(index, address);
  }
  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT index,
                                                            D3D12_GPU_VIRTUAL_ADDRESS address) final {
    graphics_root_arguments_.setRootDescriptor(index, root_descriptor_address(address));
    track_graphics_root_resource(index, address);
  }
  void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view) final {
    ibv_ = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
  }
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT start_slot, UINT count,
                                            const D3D12_VERTEX_BUFFER_VIEW *views) final {
    if (start_slot >= vbvs_.size()) return;
    for (UINT i = 0; i < std::min<UINT>(count, vbvs_.size() - start_slot); ++i)
      vbvs_[start_slot + i] = views ? views[i] : D3D12_VERTEX_BUFFER_VIEW{};
  }
  void STDMETHODCALLTYPE SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *) final {}
  void STDMETHODCALLTYPE OMSetRenderTargets(UINT count,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *rtv,
                                            BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) final {
    std::vector<Descriptor *> rtvs;
    for (UINT i = 0; i < count; i++)
      rtvs.push_back(contiguous ? DxmtD3D12DescriptorHeap::from_cpu(rtv[0]) + i :
                                DxmtD3D12DescriptorHeap::from_cpu(rtv[i]));
    Descriptor *dsv_desc = dsv ? DxmtD3D12DescriptorHeap::from_cpu(*dsv) : nullptr;

    /* A placed render target has real Metal memory and is drawn into by our
     * own encoder; a committed one is still backed by d3d11 and goes through
     * the path below. */
    auto target = std::make_shared<RenderTargetState>();
    for (UINT i = 0; i < count && i < target->colors.size(); i++) {
      if (!rtvs[i] || !rtvs[i]->resource || !rtvs[i]->resource->metal_texture())
        continue;
      D3D12_RESOURCE_DESC desc = {};
      rtvs[i]->resource->GetDesc(&desc);
      target->colors[i].texture = rtvs[i]->resource->metal_texture();
      target->colors[i].resource = rtvs[i]->resource;
      target->count = i + 1;
      /* The pass extent is the first attachment's; D3D12 requires them to
       * match anyway. */
      if (!target->width) {
        target->width = uint32_t(desc.Width);
        target->height = desc.Height;
        target->sample_count = uint8_t(desc.SampleDesc.Count);
      }
    }
    if (dsv_desc && dsv_desc->resource) {
      target->depth_texture = dsv_desc->resource->metal_texture();
      target->depth_resource = dsv_desc->resource;
      if (!target->width) {
        D3D12_RESOURCE_DESC desc = {};
        dsv_desc->resource->GetDesc(&desc);
        target->width = UINT(desc.Width);
        target->height = desc.Height;
        target->sample_count = uint8_t(desc.SampleDesc.Count);
      }
    }
    /* A depth clear recorded before the targets were bound belongs to this
     * pass: D3D12 lets the two come in either order. */
    target->clear_depth = pending_depth_clear_;
    target->depth_value = pending_depth_value_;
    pending_depth_clear_ = false;
    render_target_ = *target;
    commands_.push_back(RecordedCommand{{}, false, nullptr, nullptr, nullptr, target, false, {}});

    std::vector<ComPtr<ID3D11RenderTargetView>> captured_views;
    for (auto *descriptor : rtvs)
      captured_views.emplace_back(descriptor ? descriptor->rtv.get() : nullptr);
    ComPtr<ID3D11DepthStencilView> captured_depth(dsv_desc ? dsv_desc->dsv.get() : nullptr);
    record([=]() {
      std::vector<ID3D11RenderTargetView *> views(captured_views.size());
      for (size_t i = 0; i < views.size(); i++) views[i] = captured_views[i].get();
      device_->context()->OMSetRenderTargets(UINT(views.size()), views.data(), captured_depth.get());
    });
  }
  void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                               D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                               UINT8 stencil, UINT, const D3D12_RECT *) final {
    Descriptor *desc = DxmtD3D12DescriptorHeap::from_cpu(dsv);

    if (desc && desc->resource && desc->dsv) {
      ComPtr<ID3D11DepthStencilView> view(desc->dsv);
      record([=]() { device_->context()->ClearDepthStencilView(view.get(), flags, depth, stencil); });
      return;
    }

    /* As for colour, a clear is a load action rather than a command. If this
     * depth buffer is already the bound one the pass restarts with the clear;
     * otherwise it is held until OMSetRenderTargets picks it up. */
    if (desc && desc->resource && desc->resource->metal_texture() && (flags & D3D12_CLEAR_FLAG_DEPTH)) {
      if (render_target_.depth_texture == desc->resource->metal_texture()) {
        auto target = std::make_shared<RenderTargetState>(render_target_);
        /* Only depth is being cleared; the colour attachments keep whatever is
         * already in them. */
        for (auto &color : target->colors)
          color.clear = false;
        target->clear_depth = true;
        target->depth_value = depth;
        render_target_ = *target;
        commands_.push_back(RecordedCommand{{}, false, nullptr, nullptr, nullptr, target, false, {}});
      } else {
        pending_depth_clear_ = true;
        pending_depth_value_ = depth;
      }
      return;
    }

    record([=]() {
      if (desc && desc->dsv)
        device_->context()->ClearDepthStencilView(desc->dsv.get(), flags, depth, stencil);
    });
  }
  void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                               const FLOAT color[4], UINT,
                                               const D3D12_RECT *) final {
    Descriptor *desc = DxmtD3D12DescriptorHeap::from_cpu(rtv);
    FLOAT clear[4] = {color[0], color[1], color[2], color[3]};

    if (desc && desc->resource && desc->rtv) {
      ComPtr<ID3D11RenderTargetView> view(desc->rtv);
      record([=]() { device_->context()->ClearRenderTargetView(view.get(), clear); });
      return;
    }

    /* Metal clears with a load action rather than a command, so this becomes
     * how the next pass into that target begins. */
    if (desc && desc->resource && desc->resource->metal_texture()) {
      WMT::Texture texture = desc->resource->metal_texture();
      auto target = std::make_shared<RenderTargetState>(render_target_);

      /* Which of the bound attachments this is. Clearing something that is not
       * bound makes it the only attachment, which is what D3D12 means by
       * clearing a view the pass does not use. */
      uint32_t slot = target->count;
      for (uint32_t i = 0; i < target->count; i++) {
        if (target->colors[i].texture == texture) {
          slot = i;
          break;
        }
      }
      if (slot >= target->colors.size())
        return;
      if (slot == target->count) {
        D3D12_RESOURCE_DESC resource_desc = {};
        desc->resource->GetDesc(&resource_desc);
        *target = RenderTargetState{};
        target->depth_texture = render_target_.depth_texture;
        target->width = uint32_t(resource_desc.Width);
        target->height = resource_desc.Height;
        target->sample_count = uint8_t(resource_desc.SampleDesc.Count);
        slot = 0;
        target->count = 1;
      }
      target->colors[slot].texture = texture;
      target->colors[slot].resource = desc->resource;
      target->colors[slot].clear = true;
      target->colors[slot].clear_color = {clear[0], clear[1], clear[2], clear[3]};
      render_target_ = *target;
      commands_.push_back(RecordedCommand{{}, false, nullptr, nullptr, nullptr, target, false, {}});
      return;
    }

    record([=]() {
      if (desc && desc->rtv)
        device_->context()->ClearRenderTargetView(desc->rtv.get(), clear);
    });
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                      ID3D12Resource *, const UINT values[4],
                                                      UINT rectangles, const D3D12_RECT *) final {
    auto *descriptor = DxmtD3D12DescriptorHeap::from_cpu(handle);
    if (!descriptor || !descriptor->uav || !values) return;
    if (rectangles) { ERR("D3D12: partial UINT UAV clears are not implemented"); return; }
    ComPtr<ID3D11UnorderedAccessView> view(descriptor->uav);
    std::array<UINT, 4> clear; std::copy_n(values, 4, clear.begin());
    record([=, this]() { device_->context()->ClearUnorderedAccessViewUint(view.get(), clear.data()); });
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                       ID3D12Resource *, const float values[4],
                                                       UINT rectangles, const D3D12_RECT *) final {
    auto *descriptor = DxmtD3D12DescriptorHeap::from_cpu(handle);
    if (!descriptor || !descriptor->uav || !values) return;
    if (rectangles) { ERR("D3D12: partial FLOAT UAV clears are not implemented"); return; }
    ComPtr<ID3D11UnorderedAccessView> view(descriptor->uav);
    std::array<float, 4> clear; std::copy_n(values, 4, clear.begin());
    record([=, this]() { device_->context()->ClearUnorderedAccessViewFloat(view.get(), clear.data()); });
  }
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *, const D3D12_DISCARD_REGION *) final {}
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) final {
    auto *h = static_cast<DxmtD3D12QueryHeap *>(heap);
    if (!h || type == D3D12_QUERY_TYPE_TIMESTAMP || !h->accepts(type, index)) return;
    ComPtr<DxmtD3D12QueryHeap> owner(h);
    record([=, this]() {
      if (auto *query = owner->query(index, DxmtD3D12Device::asynchronous_query_resolve_enabled()))
        device_->context()->Begin(query);
    });
  }
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) final {
    auto *h = static_cast<DxmtD3D12QueryHeap *>(heap);
    if (!h || !h->accepts(type, index)) return;
    ComPtr<DxmtD3D12QueryHeap> owner(h);
    record([=, this]() {
      if (auto *query = owner->query(index, type == D3D12_QUERY_TYPE_TIMESTAMP &&
                                          DxmtD3D12Device::asynchronous_query_resolve_enabled()))
        device_->context()->End(query);
    });
  }
  void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start, UINT count,
                                          ID3D12Resource *destination, UINT64 offset) final {
    auto *h = static_cast<DxmtD3D12QueryHeap *>(heap);
    auto *dst = static_cast<DxmtD3D12Resource *>(destination);
    if (!h || !dst || !count || !h->accepts(type, start) || count - 1 > UINT_MAX - start ||
        !h->accepts(type, start + count - 1) || dst->desc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        (offset & 7) || offset > UINT_MAX || offset > dst->desc().Width ||
        UINT64(count) * 8 > dst->desc().Width - offset || UINT64(count) * 8 > UINT_MAX - offset) return;
    ComPtr<DxmtD3D12QueryHeap> owner(h); ComPtr<DxmtD3D12Resource> target(dst);
    record([=, this]() {
      auto context = device_->context(); std::vector<UINT64> values(count);
      std::vector<ComPtr<ID3D11Query>> queries;
      queries.reserve(count);
      for (UINT i = 0; i < count; ++i) {
        auto *query = owner->query(start + i);
        if (!query) return;
        queries.emplace_back(query);
      }
      if (device_->defer_query_resolve(queries, type, target.get(), offset)) return;
      // Submit once before waiting. Repeated GetData calls with implicit
      // flushing otherwise flood the queue with empty command buffers.
      device_->submit_recorded_work();
      for (UINT i = 0; i < count; ++i) {
        HRESULT hr;
        while ((hr = context->GetData(queries[i].get(), &values[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) Sleep(1);
        if (FAILED(hr)) { ERR("D3D12: resolving query failed: ", hr); return; }
        if (type == D3D12_QUERY_TYPE_BINARY_OCCLUSION) values[i] = values[i] != 0;
      }
      if (auto *resource = target->d3d11_resource()) {
        // A full UpdateSubresource can rename a D3D11 buffer. D3D12 GPU
        // addresses and persistent mappings must keep their original backing.
        D3D11_BUFFER_DESC desc = {}; desc.ByteWidth = UINT(count * 8ull); desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial = {values.data(), 0, 0}; ComPtr<ID3D11Buffer> upload;
        HRESULT hr = device_->d3d11_device()->CreateBuffer(&desc, &initial, upload.put());
        if (FAILED(hr)) { ERR("D3D12: query result upload failed: ", hr); return; }
        context->CopySubresourceRegion(resource, 0, UINT(offset), 0, 0, upload.get(), 0, nullptr);
      }
    });
  }
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *, UINT64, D3D12_PREDICATION_OP) final {}
  void STDMETHODCALLTYPE SetMarker(UINT, const void *, UINT) final {}
  void STDMETHODCALLTYPE BeginEvent(UINT, const void *, UINT) final {}
  void STDMETHODCALLTYPE EndEvent() final {}
  void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *signature, UINT maximum,
                                         ID3D12Resource *arguments, UINT64 offset,
                                         ID3D12Resource *count_buffer, UINT64 count_offset) final {
    if (!signature || !arguments || !maximum) return;
    auto *sig = static_cast<DxmtD3D12CommandSignature *>(signature);
    auto type = sig->type(); UINT stride = sig->stride(), size = sig->argument_size();
    ComPtr<DxmtD3D12Resource> args(static_cast<DxmtD3D12Resource *>(arguments));
    ComPtr<DxmtD3D12Resource> counts(static_cast<DxmtD3D12Resource *>(count_buffer));
    if (args->desc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset > UINT_MAX || (offset & 3) ||
        offset > args->desc().Width || UINT64(maximum - 1) * stride + size > args->desc().Width - offset) return;
    if (counts && (counts->desc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
                   count_offset > UINT_MAX || (count_offset & 3) || count_offset > counts->desc().Width ||
                   counts->desc().Width - count_offset < 4)) return;
    auto pipeline = pipeline_; auto vbvs = vbvs_; auto ibv = ibv_;
    auto graphics = legacy_root_bindings();
    LegacyRootBindings compute = {compute_root_signature_, compute_root_arguments_, compute_root_descriptors_, resource_heap_, sampler_heap_};
    auto blend = blend_factor_; auto stencil = stencil_ref_;
    record([=, this]() {
      CpuTimingScope indirect_time(bridge_cpu_statistics().indirect_ticks);
      auto context = device_->context(); UINT count = maximum;
      ComPtr<ID3D11Buffer> buffer;
      if (!args->d3d11_resource() || FAILED(args->d3d11_resource()->QueryInterface(IID_PPV_ARGS(buffer.put())))) return;
      if (counts) {
        D3D11_BUFFER_DESC desc = {}; desc.ByteWidth = 4; desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;
        if (!counts->d3d11_resource() || FAILED(device_->d3d11_device()->CreateBuffer(&desc, nullptr, staging.put()))) return;
        D3D11_BOX box = {UINT(count_offset), 0, 0, UINT(count_offset + 4), 1, 1};
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, counts->d3d11_resource(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return;
        count = std::min(maximum, *static_cast<const UINT *>(mapped.pData)); context->Unmap(staging.get(), 0);
      }
      if (!count || !pipeline) return;
      if (type != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH && pipeline->has_metal_functions()) {
        ERR("D3D12: SM6 indirect drawing is not implemented"); return;
      }
      if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
        if (!pipeline->legacy_compute_shader()) { ERR("D3D12: SM6 indirect dispatch is not implemented"); return; }
        context->CSSetShader(pipeline->legacy_compute_shader(), nullptr, 0);
        bind_legacy_graphics(compute, pipeline.get(), true);
      } else {
        pipeline->apply(context, blend.data(), stencil);
        bind_legacy_graphics(graphics, pipeline.get()); bind_legacy_vertex_buffers(vbvs);
        if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED) {
          auto *resource = device_->resource_from_gpu_va(ibv.BufferLocation);
          if (!resource) return;
          auto index = resource->index_buffer(); resource->update_buffer(index, ibv.SizeInBytes);
          context->IASetIndexBuffer(index, ibv.Format, UINT(ibv.BufferLocation - resource->GetGPUVirtualAddress()));
        }
      }
      for (UINT i = 0; i < count; ++i) {
        UINT at = UINT(offset + UINT64(i) * stride);
        if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW) context->DrawInstancedIndirect(buffer.get(), at);
        else if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED) context->DrawIndexedInstancedIndirect(buffer.get(), at);
        else {
          context->DispatchIndirect(buffer.get(), at);
          device_->synchronize_compute_for_diagnostics();
        }
      }
    });
  }

  /* Native UAV barriers can stay inside a compute pass. Pass transitions and
   * DXBC barriers close pending work and order it on the GPU, without a CPU
   * completion wait. */
  void replay_immediate() { replay(commands_); }
  std::function<void()> capture_replay() {
    ComPtr<DxmtD3D12GraphicsCommandList> owner(this);
    return [owner = std::move(owner), commands = commands_]() mutable { owner->replay(commands); };
  }
  void replay(std::vector<RecordedCommand> &commands) {
    CpuTimingScope replay_time(bridge_cpu_statistics().replay_ticks);
    std::vector<RecordedCommand *> compute_batch;
    std::vector<GraphicsDrawWork *> draw_batch;
    std::vector<CopyWork *> copy_batch;
    RenderTargetState target;
    bool gpu_barrier_emitted = false;

    auto flush_compute = [&]() {
      if (!compute_batch.empty())
        encode_compute_batch(compute_batch);
      compute_batch.clear();
    };
    auto flush_copies = [&]() {
      if (!copy_batch.empty())
        encode_copy_batch(copy_batch);
      copy_batch.clear();
    };
    auto flush_draws = [&]() {
      if (!draw_batch.empty() || target.any_clear()) {
        encode_render_batch(target, draw_batch);
        /* One clear per pass: a second pass into the same targets loads what
         * the first left rather than clearing them again. Depth as well as
         * colour -- clearing depth again would let anything behind what was
         * already drawn pass the test. */
        target.clear_load_actions();
      }
      draw_batch.clear();
    };

    for (auto &command : commands) {
      if (!command.barrier && !command.state_only) gpu_barrier_emitted = false;
      if (command.dispatch) {
        flush_draws();
        flush_copies();
        compute_batch.push_back(&command);
      } else if (command.draw) {
        flush_compute();
        flush_copies();
        draw_batch.push_back(command.draw.get());
      } else if (command.copy) {
        flush_compute();
        flush_draws();
        copy_batch.push_back(command.copy.get());
      } else if (command.target) {
        /* Retargeting or clearing ends the pass the draws so far were going
         * into: a Metal pass has one attachment set and one load action.
         * Rebinding the same attachments with nothing to clear is a no-op, and
         * splitting the pass for it would throw away the batching. */
        if (!draw_batch.empty() || !target.same_attachments(*command.target) || command.target->any_clear())
          flush_draws();
        target = *command.target;
      } else if (command.barrier) {
        if (command.aliasing_barrier || command.encoder_split || compute_batch.empty()) {
          flush_compute();
          flush_draws();
          flush_copies();
          // A whole ResourceBarrier array often becomes consecutive records.
          // One marker orders all of them when no work occurs in between.
          if (!gpu_barrier_emitted) device_->encode_gpu_barrier();
          gpu_barrier_emitted = true;
        } else
          compute_batch.push_back(&command);
      } else if (command.d3d11) {
        if (!command.state_only) {
          flush_compute();
          flush_draws();
          flush_copies();
        }
        command.d3d11();
      }
    }
    flush_compute();
    flush_draws();
    flush_copies();
    // A D3D12 command list ends its encoders, not the submitted GPU batch.
    // ExecuteCommandLists commits the whole group after replaying every list.
    device_->submit_recorded_work(false);
  }

private:
  D3D12_COMMAND_LIST_TYPE type_;
  bool closed_ = false;
  ComPtr<DxmtD3D12PipelineState> pipeline_;
  std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vbvs_ = {};
  D3D12_INDEX_BUFFER_VIEW ibv_ = {};
  std::array<FLOAT, 4> blend_factor_ = {1, 1, 1, 1};
  UINT stencil_ref_ = 0;
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 64> root_descriptors_ = {};
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 64> compute_root_descriptors_ = {};
  /* The root signature owns the layout the argument buffer points at, so it has
   * to outlive it; graphics and compute keep separate sets, as in D3D12. */
  ComPtr<DxmtD3D12RootSignature> graphics_root_signature_;
  ComPtr<DxmtD3D12RootSignature> compute_root_signature_;
  dxmt::RootArgumentBuffer graphics_root_arguments_;
  dxmt::RootArgumentBuffer compute_root_arguments_;
  ComPtr<DxmtD3D12DescriptorHeap> resource_heap_;
  ComPtr<DxmtD3D12DescriptorHeap> sampler_heap_;

  /* Resources reached through a root CBV/SRV/UAV, kept per root parameter so a
   * rebind replaces rather than accumulates. Metal needs each one made
   * resident explicitly: the shader gets a raw address, which tells the driver
   * nothing about what memory is in use. */
  std::array<ComPtr<DxmtD3D12Resource>, 64> compute_root_resources_;
  std::array<ComPtr<DxmtD3D12Resource>, 64> graphics_root_resources_;

  /* The colour attachment a run of draws renders into, and whether the pass
   * starts by clearing it. D3D12 clears with a command; Metal clears with a
   * load action, so a clear immediately before a draw becomes part of the pass
   * rather than a pass of its own. */
  struct RenderTargetState {
    struct ColorAttachment {
      WMT::Texture texture;
      ComPtr<DxmtD3D12Resource> resource;
      bool clear = false;
      WMTClearColor clear_color = {};
    };
    /* D3D12 binds up to eight, and so does a Metal render pass. Deferred
     * renderers bind all of them at once for their G-buffer. */
    std::array<ColorAttachment, 8> colors;
    uint32_t count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t sample_count = 1;
    WMT::Texture depth_texture;
    ComPtr<DxmtD3D12Resource> depth_resource;
    bool clear_depth = false;
    float depth_value = 1.0f;

    bool
    same_attachments(const RenderTargetState &other) const {
      if (count != other.count || depth_texture != other.depth_texture)
        return false;
      for (uint32_t i = 0; i < count; i++)
        if (colors[i].texture != other.colors[i].texture)
          return false;
      return true;
    }

    bool
    any_clear() const {
      for (uint32_t i = 0; i < count; i++)
        if (colors[i].clear)
          return true;
      return clear_depth;
    }

    void
    clear_load_actions() {
      for (auto &color : colors)
        color.clear = false;
      clear_depth = false;
    }
  };

  /* Pipeline state the draw needs but D3D12 sets separately from the draw. */
  D3D12_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  WMTViewport viewport_ = {};
  bool has_viewport_ = false;
  D3D12_RECT scissor_ = {};
  bool has_scissor_ = false;
  RenderTargetState render_target_;
  bool pending_depth_clear_ = false;
  float pending_depth_value_ = 1.0f;

  struct GraphicsDrawWork {
    WMT::RenderPipelineState pipeline;
    WMT::DepthStencilState depth_stencil;
    WMTPrimitiveType primitive = WMTPrimitiveTypeTriangle;
    uint64_t vertex_start = 0;
    uint64_t vertex_count = 0;
    uint32_t instance_count = 1;
    uint32_t base_instance = 0;
    std::vector<uint8_t> root_arguments;
    std::vector<ResidentResource> resident;
    ComPtr<DxmtD3D12DescriptorHeap> resource_heap;
    ComPtr<DxmtD3D12DescriptorHeap> sampler_heap;
    ComPtr<DxmtD3D12PipelineState> pipeline_owner;
    WMT::Reference<WMT::Buffer> resource_heap_buffer;
    WMT::Reference<WMT::Buffer> sampler_heap_buffer;
    struct BoundVertexBuffer {
      WMT::Buffer buffer;
      uint64_t offset;
      uint8_t index;
    };
    std::vector<BoundVertexBuffer> vertex_buffers;
    /* Set only for an indexed draw. */
    WMT::Buffer index_buffer;
    uint64_t index_buffer_offset = 0;
    WMTIndexType index_type = WMTIndexTypeUInt16;
    int32_t base_vertex = 0;
    bool indexed = false;
    WMTViewport viewport = {};
    WMTScissorRect scissor = {};
    bool has_viewport = false;
  };

  /* A copy between placed resources, which have real Metal memory and so go
   * through a blit encoder rather than the d3d11 shadow-buffer path. */
  struct CopyWork {
    enum class Kind { BufferToBuffer, BufferToTexture, TextureToBuffer, TextureToTexture };
    Kind kind = Kind::BufferToBuffer;
    WMT::Buffer src_buffer;
    WMT::Buffer dst_buffer;
    WMT::Texture src_texture;
    WMT::Texture dst_texture;
    ComPtr<DxmtD3D12Resource> src_resource;
    ComPtr<DxmtD3D12Resource> dst_resource;
    uint64_t src_offset = 0;
    uint64_t dst_offset = 0;
    uint64_t length = 0;
    uint32_t bytes_per_row = 0;
    uint32_t bytes_per_image = 0;
    WMTSize size = {};
    WMTOrigin src_origin = {};
    WMTOrigin dst_origin = {};
    uint32_t src_slice = 0;
    uint32_t src_level = 0;
    uint32_t dst_slice = 0;
    uint32_t dst_level = 0;
  };

  struct ComputeDispatchWork {
    WMT::ComputePipelineState pipeline;
    WMTSize threadgroup_size = {};
    WMTSize threadgroups = {};
    std::vector<uint8_t> root_arguments;
    /* Buffers to make resident, with the usage each is reached with. */
    std::vector<ResidentResource> resident;
    ComPtr<DxmtD3D12DescriptorHeap> resource_heap;
    ComPtr<DxmtD3D12DescriptorHeap> sampler_heap;
    ComPtr<DxmtD3D12PipelineState> pipeline_owner;
    WMT::Reference<WMT::Buffer> resource_heap_buffer;
    WMT::Reference<WMT::Buffer> sampler_heap_buffer;
  };

  /* One recorded command. Exactly one of the three is set. Keeping dispatches
   * and barriers in the same list as the d3d11 work is what preserves the
   * order the application recorded them in. */
  struct RecordedCommand {
    std::function<void()> d3d11;
    /* Pure d3d11 state -- viewport, scissor, topology -- which a Metal draw
     * does not read, so running it does not have to end the pass the draws are
     * accumulating into. Ending it would put every state change between two
     * draws into a pass of its own. */
    bool state_only = false;
    std::shared_ptr<ComputeDispatchWork> dispatch;
    std::shared_ptr<GraphicsDrawWork> draw;
    std::shared_ptr<CopyWork> copy;
    /* Set by OMSetRenderTargets and by ClearRenderTargetView: both change what
     * the next pass renders into or how it begins. */
    std::shared_ptr<RenderTargetState> target;
    bool barrier = false;
    WMTBarrierScope barrier_scope = WMTBarrierScopeBuffers;
    bool aliasing_barrier = false;
    bool encoder_split = false;
  };
  std::vector<RecordedCommand> commands_;

  void record(std::function<void()> fn) {
    commands_.push_back(RecordedCommand{std::move(fn), false, nullptr, nullptr, nullptr, nullptr, false, {}});
  }
  void record_state(std::function<void()> fn) {
    commands_.push_back(RecordedCommand{std::move(fn), true, nullptr, nullptr, nullptr, nullptr, false, {}});
  }


  template <typename Work>
  void collect_residency(Work &work, const std::array<ComPtr<DxmtD3D12Resource>, 64> &roots);
  template <typename Work> void refresh_heap_residency(Work &work);
  void collect_compute_residency(ComputeDispatchWork &work);
  void encode_compute_batch(const std::vector<RecordedCommand *> &batch);
  void collect_graphics_residency(GraphicsDrawWork &work);
  void encode_copy_batch(const std::vector<CopyWork *> &copies);
  void record_copy(std::shared_ptr<CopyWork> copy) {
    commands_.push_back(RecordedCommand{{}, false, nullptr, nullptr, copy, nullptr, false, {}});
  }
  void encode_render_batch(const RenderTargetState &target, const std::vector<GraphicsDrawWork *> &draws);

  /* Root descriptors are plain addresses, so tracking the resource behind one
   * is the only way the dispatch can make it resident. */
  void track_root_resource(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    if (index >= compute_root_resources_.size())
      return;
    compute_root_resources_[index] = device_ ? device_->resource_from_gpu_va(address) : nullptr;
  }
  void track_graphics_root_resource(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    if (index >= graphics_root_resources_.size())
      return;
    graphics_root_resources_[index] = device_ ? device_->resource_from_gpu_va(address) : nullptr;
  }

  void bind_legacy_vertex_buffers(const decltype(vbvs_) &views) {
    std::array<ID3D11Buffer *, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> buffers = {};
    std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> strides = {}, offsets = {};
    for (UINT i = 0; i < views.size(); ++i) {
      if (auto *resource = device_->resource_from_gpu_va(views[i].BufferLocation)) {
        buffers[i] = resource->vertex_buffer();
        resource->update_buffer(buffers[i], views[i].SizeInBytes);
        strides[i] = views[i].StrideInBytes;
        offsets[i] = UINT(views[i].BufferLocation - resource->GetGPUVirtualAddress());
      }
    }
    device_->context()->IASetVertexBuffers(0, buffers.size(), buffers.data(), strides.data(), offsets.data());
  }

  struct LegacyRootBindings {
    ComPtr<DxmtD3D12RootSignature> signature;
    dxmt::RootArgumentBuffer arguments;
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 64> tables;
    ComPtr<DxmtD3D12DescriptorHeap> resource_heap;
    ComPtr<DxmtD3D12DescriptorHeap> sampler_heap;
  };

  LegacyRootBindings legacy_root_bindings() const {
    return {graphics_root_signature_, graphics_root_arguments_, root_descriptors_, resource_heap_, sampler_heap_};
  }

  /* DXBC stages run through the shared D3D11 core, with the D3D12 signature
   * deciding registers and stage visibility. Root arguments are captured per
   * draw; volatile table descriptors are resolved when the list executes. */
  void bind_legacy_graphics(const LegacyRootBindings &bindings, DxmtD3D12PipelineState *pipeline,
                            bool compute = false) {
    CpuTimingScope binding_time(bridge_cpu_statistics().legacy_bind_ticks);
    if (!bindings.signature || !device_->context1()) return;
    auto *context = device_->context1();
    const LegacyShaderBindingMask unknown = {UINT16_MAX, UINT16_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    auto stages = [&](UINT visibility, auto bind) {
      const UINT visibility_by_stage[] = {D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL,
                                          D3D12_SHADER_VISIBILITY_GEOMETRY};
      for (UINT stage = compute ? 3 : 0; stage < (compute ? 4 : 3); ++stage)
        if (compute || visibility == D3D12_SHADER_VISIBILITY_ALL || visibility == visibility_by_stage[stage])
          bind(stage, pipeline ? pipeline->legacy_bindings(stage) : unknown);
    };
    auto cbv_stages = [&](UINT slot, UINT visibility) {
      UINT used = 0;
      if (slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
        stages(visibility, [&](UINT stage, const auto &mask) {
          if (mask.cbvs & (1u << slot)) used |= 1u << stage;
        });
      return used;
    };
    auto bind_cbv = [&](UINT used, UINT slot, ID3D11Buffer *buffer, UINT first, UINT count) {
      if (used & 1) context->VSSetConstantBuffers1(slot, 1, &buffer, &first, &count);
      if (used & 2) context->PSSetConstantBuffers1(slot, 1, &buffer, &first, &count);
      if (used & 4) context->GSSetConstantBuffers1(slot, 1, &buffer, &first, &count);
      if (used & 8) context->CSSetConstantBuffers1(slot, 1, &buffer, &first, &count);
    };
    auto cbv = [&](UINT slot, UINT visibility, UINT64 address, UINT bytes) {
      const UINT used = cbv_stages(slot, visibility);
      if (!used) return;
      ID3D11Buffer *buffer = nullptr;
      UINT first = 0, count = 0;
      if (auto *resource = address ? device_->resource_from_gpu_va(address) : nullptr) {
        D3D12_RESOURCE_DESC desc = {};
        resource->GetDesc(&desc);
        UINT64 offset = address - resource->GetGPUVirtualAddress();
        UINT64 available = desc.Width > offset ? desc.Width - offset : 0;
        UINT size = UINT(std::min<UINT64>(available, bytes ? bytes : 65536));
        buffer = resource->constant_buffer();
        first = UINT(offset / 16);
        count = std::min(4096u, align_uint(size, 256) / 16);
      }
      static std::atomic<unsigned> missing_cbvs = 0;
      if (d3d12_diagnostics() && address && !buffer && missing_cbvs.fetch_add(1) < 24)
        Logger::warn(str::format("D3D12 unresolved CBV slot=", slot, " address=", address, " compute=", compute));
      bind_cbv(used, slot, buffer, first, count);
    };
    if (compute) {
      std::array<ID3D11UnorderedAccessView *, D3D11_PS_CS_UAV_REGISTER_COUNT> empty = {};
      context->CSSetUnorderedAccessViews(0, UINT(empty.size()), empty.data(), nullptr);
    }
    const auto &description = bindings.signature->description();
    for (UINT index = 0; index < description.parameters.size(); ++index) {
      const auto &parameter = description.parameters[index];
      const auto *layout = bindings.signature->layout().parameter(index);
      if (!layout || layout->offset + layout->size > bindings.arguments.size()) continue;
      const auto *argument = static_cast<const uint8_t *>(bindings.arguments.data()) + layout->offset;
      if (parameter.kind == dxmt::RootParameterKind::ConstantBufferView) {
        UINT64 address = 0;
        std::memcpy(&address, argument, sizeof(address));
        cbv(parameter.shader_register, parameter.shader_visibility, address, 0);
      } else if (parameter.kind == dxmt::RootParameterKind::Constants) {
        UINT used = cbv_stages(parameter.shader_register, parameter.shader_visibility);
        if (!used) continue;
        UINT first = 0, count = 0;
        ID3D11Buffer *buffer = device_->upload_root_constants(argument, parameter.num_32bit_values * 4, first, count);
        bind_cbv(used, parameter.shader_register, buffer, first, count);
      } else if (parameter.kind == dxmt::RootParameterKind::DescriptorTable) {
        Descriptor *table = index < bindings.tables.size() ? DxmtD3D12DescriptorHeap::from_gpu(bindings.tables[index]) : nullptr;
        UINT available = 0;
        if (table && table->owner) {
          D3D12_DESCRIPTOR_HEAP_DESC heap = {};
          table->owner->GetDesc(&heap);
          available = heap.NumDescriptors > table->index ? heap.NumDescriptors - table->index : 0;
        }
        UINT append = 0;
        for (UINT r = 0; r < parameter.range_count; ++r) {
          const auto &range = description.ranges[parameter.first_range + r];
          UINT offset = range.offset_in_descriptors_from_table_start;
          if (offset == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) offset = append;
          UINT limit = range.range_type == dxmt::DescriptorRangeType::ConstantBufferView ? 14 :
                       range.range_type == dxmt::DescriptorRangeType::Sampler ? 16 :
                       range.range_type == dxmt::DescriptorRangeType::UnorderedAccessView ? D3D11_PS_CS_UAV_REGISTER_COUNT : 128;
          UINT length = range.base_shader_register < limit ?
              std::min(range.descriptor_count, limit - range.base_shader_register) : 0;
          auto entry_at = [&](UINT i) {
            return table && offset < available && i < available - offset ? table + offset + i : nullptr;
          };
          if (length && range.range_type == dxmt::DescriptorRangeType::ShaderResourceView) {
            stages(parameter.shader_visibility, [&](UINT stage, const auto &mask) {
              if (!(mask.srvs_lo | mask.srvs_hi)) return;
              std::array<ComPtr<ID3D11ShaderResourceView>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> retained;
              std::array<ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views = {};
              UINT first = length, last = 0;
              for (UINT i = 0; i < length; ++i) if (mask.srv(range.base_shader_register + i)) {
                first = std::min(first, i); last = i + 1;
                if (auto *entry = entry_at(i); entry && entry->type == DescriptorType::Srv) {
                  retained[i] = entry->srv; views[i] = retained[i].get();
                }
              }
              if (first == length) return;
              UINT slot = range.base_shader_register + first, count = last - first;
              if (stage == 0) context->VSSetShaderResources(slot, count, views.data() + first);
              if (stage == 1) context->PSSetShaderResources(slot, count, views.data() + first);
              if (stage == 2) context->GSSetShaderResources(slot, count, views.data() + first);
              if (stage == 3) context->CSSetShaderResources(slot, count, views.data() + first);
            });
          } else if (length && range.range_type == dxmt::DescriptorRangeType::Sampler) {
            stages(parameter.shader_visibility, [&](UINT stage, const auto &mask) {
              if (!mask.samplers) return;
              std::array<ComPtr<ID3D11SamplerState>, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> retained;
              std::array<ID3D11SamplerState *, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers = {};
              UINT first = length, last = 0;
              for (UINT i = 0; i < length; ++i) if (mask.samplers & (1u << (range.base_shader_register + i))) {
                first = std::min(first, i); last = i + 1;
                if (auto *entry = entry_at(i); entry && entry->type == DescriptorType::Sampler) {
                  retained[i] = entry->sampler11; samplers[i] = retained[i].get();
                }
              }
              if (first == length) return;
              UINT slot = range.base_shader_register + first, count = last - first;
              if (stage == 0) context->VSSetSamplers(slot, count, samplers.data() + first);
              if (stage == 1) context->PSSetSamplers(slot, count, samplers.data() + first);
              if (stage == 2) context->GSSetSamplers(slot, count, samplers.data() + first);
              if (stage == 3) context->CSSetSamplers(slot, count, samplers.data() + first);
            });
          } else for (UINT i = 0; i < length; ++i) {
            UINT slot = range.base_shader_register + i;
            const auto &mask = pipeline ? pipeline->legacy_bindings(3) : unknown;
            if (range.range_type == dxmt::DescriptorRangeType::ConstantBufferView &&
                !cbv_stages(slot, parameter.shader_visibility)) continue;
            if (compute && range.range_type == dxmt::DescriptorRangeType::UnorderedAccessView &&
                !(mask.uavs & (uint64_t(1) << slot))) continue;
            Descriptor *entry = entry_at(i);
            if (range.range_type == dxmt::DescriptorRangeType::ConstantBufferView) {
              cbv(slot, parameter.shader_visibility, entry ? entry->cbv.BufferLocation : 0,
                  entry ? entry->cbv.SizeInBytes : 0);
            } else if (range.range_type == dxmt::DescriptorRangeType::UnorderedAccessView && compute) {
              ComPtr<ID3D11UnorderedAccessView> retained;
              if (entry && entry->type == DescriptorType::Uav) retained = entry->uav;
              ID3D11UnorderedAccessView *view = retained.get();
              UINT keep = UINT_MAX;
              context->CSSetUnorderedAccessViews(slot, 1, &view, &keep);
            }
          }
          append = range.descriptor_count > UINT_MAX - offset ? UINT_MAX : offset + range.descriptor_count;
        }
      }
    }
    const auto &samplers = bindings.signature->samplers();
    for (UINT i = 0; i < samplers.size(); ++i) {
      const auto &desc = description.static_samplers[i];
      if (desc.shader_register >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) continue;
      stages(desc.shader_visibility, [&](UINT stage, const auto &mask) {
        if (!(mask.samplers & (1u << desc.shader_register))) return;
        ID3D11SamplerState *sampler = samplers[i].get();
        if (stage == 0) context->VSSetSamplers(desc.shader_register, 1, &sampler);
        if (stage == 1) context->PSSetSamplers(desc.shader_register, 1, &sampler);
        if (stage == 2) context->GSSetSamplers(desc.shader_register, 1, &sampler);
        if (stage == 3) context->CSSetSamplers(desc.shader_register, 1, &sampler);
      });
    }
  }

  void bind_root_signature(ID3D12RootSignature *root_signature,
                           ComPtr<DxmtD3D12RootSignature> &slot,
                           dxmt::RootArgumentBuffer &arguments) {
    if (slot.get() == root_signature) return;
    slot = static_cast<DxmtD3D12RootSignature *>(root_signature);
    /* Changing the root signature invalidates every root argument, which is
     * what a fresh layout gives: setLayout zeroes the storage. */
    arguments.setLayout(slot ? &slot->layout() : nullptr);
  }

  /* A GPU descriptor handle is a descriptor's address in its heap's Metal
   * buffer, which is what a table root parameter stores. */
  static uint64_t descriptor_table_address(D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    Descriptor *descriptor = DxmtD3D12DescriptorHeap::from_gpu(handle);
    if (!descriptor || !descriptor->owner)
      return 0;
    return descriptor->owner->gpu_descriptor_address(descriptor->index);
  }

  /* Root CBV/SRV/UAV store a raw address. Committed resources still live in
   * d3d11 and their "addresses" are object identities, so only a placed
   * resource yields a real one. */
  uint64_t root_descriptor_address(D3D12_GPU_VIRTUAL_ADDRESS address) const {
    if (!device_ || !address)
      return 0;
    DxmtD3D12Resource *resource = device_->resource_from_gpu_va(address);
    return device_->buffer_address(resource, address);
  }
};

/* Descriptor-table resources are indirect Metal accesses. Retain the actual
 * typed view as well as the D3D12 resource used by DXMT's hazard tracking. */
template <typename Work>
void DxmtD3D12GraphicsCommandList::collect_residency(
    Work &work, const std::array<ComPtr<DxmtD3D12Resource>, 64> &roots) {
  auto add = [&](DxmtD3D12Resource *resource, WMTResourceUsage usage, WMT::Texture view = {}) {
    if (!resource) return;
    WMT::Resource object;
    if (view) object = WMT::Resource(view.handle);
    else if (auto buffer = resource->metal_buffer()) object = WMT::Resource(buffer.handle);
    else if (auto texture = resource->metal_texture()) object = WMT::Resource(texture.handle);
    if (!object) return;
    for (auto &existing : work.resident) {
      if (existing.object == object) {
        existing.usage = WMTResourceUsage(existing.usage | usage);
        return;
      }
    }
    work.resident.push_back({WMT::Reference<WMT::Resource>(object), resource, usage});
  };
  const auto both = WMTResourceUsage(WMTResourceUsageRead | WMTResourceUsageWrite);
  for (auto &resource : roots) add(resource.get(), both);
  work.resource_heap = resource_heap_;
  work.sampler_heap = sampler_heap_;
  if (resource_heap_)
    if (auto *heap = resource_heap_->gpu_heap()) work.resource_heap_buffer = heap->buffer();
  if (sampler_heap_)
    if (auto *heap = sampler_heap_->gpu_heap()) work.sampler_heap_buffer = heap->buffer();
}

/* Resolve volatile table resources at submission, after descriptor writes
 * made between recording and ExecuteCommandLists. Each execution owns its
 * residency snapshot, including when the command list is reset afterward. */
template <typename Work>
void DxmtD3D12GraphicsCommandList::refresh_heap_residency(Work &work) {
  if (!work.resource_heap) return;
  for (const auto &entry : work.resource_heap->residency()) {
    auto found = std::find_if(work.resident.begin(), work.resident.end(),
                              [&](const auto &r) { return r.object == entry.object; });
    if (found == work.resident.end()) work.resident.push_back(entry);
    else found->usage = WMTResourceUsage(found->usage | entry.usage);
  }
}

void DxmtD3D12GraphicsCommandList::collect_compute_residency(ComputeDispatchWork &work) {
  collect_residency(work, compute_root_resources_);
}

/* Records a run of dispatches, and the barriers between them, into the command
 * chunk d3d11 is also filling.
 *
 * The pass is opened and closed inside the one callback, so it cannot straddle
 * anything d3d11 emits. What it cannot do by itself is start while a d3d11
 * encoder is still open -- startComputePass asserts on that -- so the current
 * pass is closed first. */
void DxmtD3D12GraphicsCommandList::encode_compute_batch(const std::vector<RecordedCommand *> &batch) {
  dxmt::Device *dxmt_device = device_ ? device_->dxmt_device() : nullptr;
  if (!dxmt_device)
    return;

  /* Copied out of the command list: the callback runs later, and the recorded
   * commands are free to be reset by then. */
  struct Step {
    std::shared_ptr<ComputeDispatchWork> dispatch;
    WMTBarrierScope barrier_scope;
  };
  std::vector<Step> steps;
  steps.reserve(batch.size());
  for (auto *command : batch) {
    auto dispatch = command->dispatch ? std::make_shared<ComputeDispatchWork>(*command->dispatch) : nullptr;
    if (dispatch) refresh_heap_residency(*dispatch);
    steps.push_back({std::move(dispatch), command->barrier_scope});
  }
  if (steps.empty())
    return;

  /* Ends whatever pass d3d11 has open; the chunk itself stays and keeps the
   * ordering between its commands and the ones below. */
  IMTLD3D11DeviceContext *immediate = nullptr;
  if (SUCCEEDED(device_->context()->QueryInterface(__uuidof(IMTLD3D11DeviceContext), (void **)&immediate))) {
    immediate->PrepareFlush();
    immediate->Release();
  }

  dxmt_device->queue().CurrentChunk()->emitcc([steps = std::move(steps)](ArgumentEncodingContext &enc) {
    enc.startComputePass(0);

    for (auto &step : steps) {
      if (!step.dispatch) {
        auto &barrier = enc.encodeComputeCommand<wmtcmd_compute_memory_barrier>();
        barrier.type = WMTComputeCommandMemoryBarrier;
        barrier.scope = step.barrier_scope;
        continue;
      }
      const ComputeDispatchWork &work = *step.dispatch;

      auto &setpso = enc.encodeComputeCommand<wmtcmd_compute_setpso>();
      setpso.type = WMTComputeCommandSetPSO;
      setpso.pso = work.pipeline.handle;
      setpso.threadgroup_size = work.threadgroup_size;

      /* Metal Shader Converter's fixed binding ABI: 0 is the resource
       * descriptor heap, 1 the sampler heap, 2 the top-level argument
       * buffer.
       *
       * A classic descriptor table does not go through 0 or 1 -- its root
       * parameter holds the slot's absolute GPU address and the shader
       * dereferences that. These two are for SM 6.6 dynamic resources, where
       * ResourceDescriptorHeap[i] indexes the bound heap directly; nothing
       * here exercises that yet. */
      if (work.resource_heap_buffer) {
        auto &cmd = enc.encodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd.type = WMTComputeCommandSetBuffer;
        cmd.buffer = work.resource_heap_buffer;
        cmd.offset = 0;
        cmd.index = 0;
      }
      if (work.sampler_heap_buffer) {
        auto &cmd = enc.encodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd.type = WMTComputeCommandSetBuffer;
        cmd.buffer = work.sampler_heap_buffer;
        cmd.offset = 0;
        cmd.index = 1;
      }
      if (!work.root_arguments.empty()) {
        /* The root argument buffer is at most 64 dwords, so it goes inline
         * rather than through a buffer allocation. The copy is into the
         * chunk's own storage: the recorded vector does not outlive
         * encoding. */
        void *storage = enc.allocate_cpu_heap(work.root_arguments.size(), 16);
        memcpy(storage, work.root_arguments.data(), work.root_arguments.size());
        auto &cmd = enc.encodeComputeCommand<wmtcmd_compute_setbytes>();
        cmd.type = WMTComputeCommandSetBytes;
        cmd.bytes.set(storage);
        cmd.length = work.root_arguments.size();
        cmd.index = 2;
      }

      /* Classic root tables reach the heap through an address in the TLAB.
       * Binding an otherwise unused SM6.6 heap slot does not establish that
       * indirect residency. The table buffer itself must be declared. */
      for (const auto &buffer : {work.resource_heap_buffer, work.sampler_heap_buffer}) {
        if (!buffer) continue;
        auto &cmd = enc.encodeComputeCommand<wmtcmd_compute_useresource>();
        cmd.type = WMTComputeCommandUseResource;
        cmd.resource = WMT::Resource(buffer.handle);
        cmd.usage = WMTResourceUsageRead;
      }
      for (auto &resident : work.resident) {
        int access = (resident.usage & WMTResourceUsageWrite) ? ResourceAccess::Read | ResourceAccess::Write
                                                          : ResourceAccess::Read;
        resident.resource->track_access<PipelineStage::Compute>(enc, access);
        auto &cmd = enc.encodeComputeCommand<wmtcmd_compute_useresource>();
        cmd.type = WMTComputeCommandUseResource;
        cmd.resource = resident.object;
        cmd.usage = resident.usage;
      }

      /* D3D12 counts threadgroups, which is dispatchThreadgroups, not
       * dispatchThreads. */
      auto &dispatch = enc.encodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = work.threadgroups;
    }

    enc.endPass();
  });
  device_->synchronize_compute_for_diagnostics();
}

void DxmtD3D12GraphicsCommandList::collect_graphics_residency(GraphicsDrawWork &work) {
  collect_residency(work, graphics_root_resources_);
}

/* Records a run of draws into one Metal render pass.
 *
 * A pass in Metal fixes its attachments and its load action up front, so the
 * batching in replay() is what decides where a pass begins and ends: draws
 * accumulate, and retargeting, clearing, a dispatch or anything going to d3d11
 * closes the pass. */
void DxmtD3D12GraphicsCommandList::encode_render_batch(const RenderTargetState &target,
                                                       const std::vector<GraphicsDrawWork *> &draws) {
  dxmt::Device *dxmt_device = device_ ? device_->dxmt_device() : nullptr;
  if (!dxmt_device || (!target.count && !target.depth_texture) || (draws.empty() && !target.any_clear()))
    return;

  std::vector<GraphicsDrawWork> steps;
  steps.reserve(draws.size());
  for (auto *draw : draws) {
    steps.push_back(*draw);
    refresh_heap_residency(steps.back());
  }

  /* Ends whatever pass d3d11 has open; the chunk stays, so the ordering
   * between its commands and these holds. */
  IMTLD3D11DeviceContext *immediate = nullptr;
  if (SUCCEEDED(device_->context()->QueryInterface(__uuidof(IMTLD3D11DeviceContext), (void **)&immediate))) {
    immediate->PrepareFlush();
    immediate->Release();
  }

  /* The attachment is a texture DXMT did not allocate -- it lives in an MTLHeap
   * this device owns -- so it gets a view with no TextureAllocation behind it.
   * Valid as a colour attachment; residency for everything the shaders read is
   * handled below by useResource. */
  std::array<dxmt::TextureViewRef, 8> attachments;
  for (uint32_t i = 0; i < target.count; i++) {
    if (target.colors[i].texture)
      attachments[i] =
          dxmt::TextureViewRef(new dxmt::TextureView(WMT::Reference<WMT::Texture>(target.colors[i].texture), 0));
  }
  dxmt::TextureViewRef depth_attachment;
  if (target.depth_texture)
    depth_attachment = dxmt::TextureViewRef(
        new dxmt::TextureView(WMT::Reference<WMT::Texture>(target.depth_texture), 0)
    );

  dxmt_device->queue().CurrentChunk()->emitcc(
      [steps = std::move(steps), attachments = std::move(attachments),
       depth_attachment = std::move(depth_attachment), target](ArgumentEncodingContext &enc) mutable {
        auto *pass = static_cast<RenderEncoderData *>(enc.startRenderPass(0, 0, uint8_t(target.count), 0));
        pass->render_target_width = target.width;
        pass->render_target_height = target.height;
        pass->render_target_array_length = 1;
        pass->default_raster_sample_count = target.sample_count;
        for (uint32_t i = 0; i < target.count; i++) {
          if (!attachments[i])
            continue;
          pass->colors[i].attachment = std::move(attachments[i]);
          pass->colors[i].load_action = target.colors[i].clear ? WMTLoadActionClear : WMTLoadActionLoad;
          pass->colors[i].store_action = WMTStoreActionStore;
          pass->colors[i].clear_color = target.colors[i].clear_color;
          pass->colors[i].level = 0;
          pass->colors[i].slice = 0;
          pass->colors[i].depth_plane = 0;
          if (target.colors[i].resource)
            target.colors[i].resource->track_access<PipelineStage::Pixel>(enc, ResourceAccess::Write);
        }

        if (depth_attachment) {
          pass->depth.attachment = std::move(depth_attachment);
          pass->depth.load_action = target.clear_depth ? WMTLoadActionClear : WMTLoadActionLoad;
          pass->depth.store_action = WMTStoreActionStore;
          pass->depth.clear_depth = target.depth_value;
          pass->depth.level = 0;
          pass->depth.slice = 0;
          pass->depth.depth_plane = 0;
          if (target.depth_resource)
            target.depth_resource->track_access<PipelineStage::Pixel>(enc, ResourceAccess::Write);
        }

        for (auto &work : steps) {
          auto &setpso = enc.encodeRenderCommand<wmtcmd_render_setpso>();
          setpso.type = WMTRenderCommandSetPSO;
          setpso.pso = work.pipeline.handle;

          if (work.depth_stencil) {
            auto &dsso = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
            dsso.type = WMTRenderCommandSetDSSO;
            dsso.dsso = work.depth_stencil.handle;
          }

          if (work.has_viewport) {
            auto &viewport = enc.encodeRenderCommand<wmtcmd_render_setviewport>();
            viewport.type = WMTRenderCommandSetViewport;
            viewport.viewport = work.viewport;
          }
          auto &scissor = enc.encodeRenderCommand<wmtcmd_render_setscissorrect>();
          scissor.type = WMTRenderCommandSetScissorRect;
          scissor.scissor_rect = work.scissor;

          /* Metal Shader Converter's fixed binding ABI, as on the compute
           * side: 0 the resource heap, 1 the sampler heap, 2 the top-level
           * argument buffer -- which both stages read, so it is bound twice. */
          for (int fragment = 0; fragment < 2; fragment++) {
            auto bind = [&](WMT::Buffer buffer, uint64_t offset, uint8_t index) {
              auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setbuffer>();
              cmd.type = fragment ? WMTRenderCommandSetFragmentBuffer : WMTRenderCommandSetVertexBuffer;
              cmd.buffer = buffer;
              cmd.offset = offset;
              cmd.index = index;
            };
            if (work.resource_heap_buffer)
              bind(work.resource_heap_buffer, 0, 0);
            if (work.sampler_heap_buffer)
              bind(work.sampler_heap_buffer, 0, 1);
            if (!work.root_arguments.empty()) {
              void *storage = enc.allocate_cpu_heap(work.root_arguments.size(), 16);
              memcpy(storage, work.root_arguments.data(), work.root_arguments.size());
              auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setbytes>();
              cmd.type = fragment ? WMTRenderCommandSetFragmentBytes : WMTRenderCommandSetVertexBytes;
              cmd.bytes.set(storage);
              cmd.length = work.root_arguments.size();
              cmd.index = 2;
            }
          }

          for (auto &vertex_buffer : work.vertex_buffers) {
            auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setbuffer>();
            cmd.type = WMTRenderCommandSetVertexBuffer;
            cmd.buffer = vertex_buffer.buffer;
            cmd.offset = vertex_buffer.offset;
            cmd.index = vertex_buffer.index;
          }

          for (const auto &buffer : {work.resource_heap_buffer, work.sampler_heap_buffer}) {
            if (!buffer) continue;
            auto &cmd = enc.encodeRenderCommand<wmtcmd_render_useresource>();
            cmd.type = WMTRenderCommandUseResource;
            cmd.resource = WMT::Resource(buffer.handle);
            cmd.usage = WMTResourceUsageRead;
            cmd.stages = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
          }
          for (auto &resident : work.resident) {
            int access = (resident.usage & WMTResourceUsageWrite) ? ResourceAccess::Read | ResourceAccess::Write
                                                              : ResourceAccess::Read;
            resident.resource->track_access<PipelineStage::Vertex>(enc, access);
            resident.resource->track_access<PipelineStage::Pixel>(enc, access);
            auto &cmd = enc.encodeRenderCommand<wmtcmd_render_useresource>();
            cmd.type = WMTRenderCommandUseResource;
            cmd.resource = resident.object;
            cmd.usage = resident.usage;
            cmd.stages = WMTRenderStages(WMTRenderStageVertex | WMTRenderStageFragment);
          }

          if (work.indexed) {
            auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw_indexed>();
            draw.type = WMTRenderCommandDrawIndexed;
            draw.primitive_type = work.primitive;
            draw.index_type = work.index_type;
            draw.index_count = work.vertex_count;
            draw.index_buffer = work.index_buffer;
            draw.index_buffer_offset = work.index_buffer_offset;
            draw.instance_count = work.instance_count;
            draw.base_vertex = work.base_vertex;
            draw.base_instance = work.base_instance;
          } else {
            auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw>();
            draw.type = WMTRenderCommandDraw;
            draw.primitive_type = work.primitive;
            draw.vertex_start = work.vertex_start;
            draw.vertex_count = work.vertex_count;
            draw.instance_count = work.instance_count;
            draw.base_instance = work.base_instance;
          }
        }

        enc.endPass();
      }
  );
}

/* Records a run of copies into one Metal blit pass.
 *
 * Blits need no residency call: the encoder is given the resources directly
 * rather than an address a shader dereferences. */
void DxmtD3D12GraphicsCommandList::encode_copy_batch(const std::vector<CopyWork *> &copies) {
  dxmt::Device *dxmt_device = device_ ? device_->dxmt_device() : nullptr;
  if (!dxmt_device || copies.empty())
    return;

  for (auto *copy : copies) {
    if (copy->src_resource) copy->src_resource->order_cpu_write();
    if (copy->dst_resource) copy->dst_resource->order_cpu_write();
  }
  std::vector<CopyWork> steps;
  steps.reserve(copies.size());
  for (auto *copy : copies)
    steps.push_back(*copy);

  IMTLD3D11DeviceContext *immediate = nullptr;
  if (SUCCEEDED(device_->context()->QueryInterface(__uuidof(IMTLD3D11DeviceContext), (void **)&immediate))) {
    immediate->PrepareFlush();
    immediate->Release();
  }

  dxmt_device->queue().CurrentChunk()->emitcc([steps = std::move(steps)](ArgumentEncodingContext &enc) {
    enc.startBlitPass();

    for (auto &copy : steps) {
      if (copy.src_resource)
        copy.src_resource->track_access<PipelineStage::Compute>(enc, ResourceAccess::Read, copy.src_level, copy.src_slice);
      if (copy.dst_resource)
        copy.dst_resource->track_access<PipelineStage::Compute>(enc, ResourceAccess::Write, copy.dst_level, copy.dst_slice);
      switch (copy.kind) {
      case CopyWork::Kind::BufferToBuffer: {
        auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
        cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
        cmd.src = copy.src_buffer;
        cmd.src_offset = copy.src_offset;
        cmd.dst = copy.dst_buffer;
        cmd.dst_offset = copy.dst_offset;
        cmd.copy_length = copy.length;
        break;
      }
      case CopyWork::Kind::BufferToTexture: {
        auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        cmd.type = WMTBlitCommandCopyFromBufferToTexture;
        cmd.src = copy.src_buffer;
        cmd.src_offset = copy.src_offset;
        cmd.bytes_per_row = copy.bytes_per_row;
        cmd.bytes_per_image = copy.bytes_per_image;
        cmd.size = copy.size;
        cmd.dst = copy.dst_texture;
        cmd.slice = copy.dst_slice;
        cmd.level = copy.dst_level;
        cmd.origin = copy.dst_origin;
        break;
      }
      case CopyWork::Kind::TextureToBuffer: {
        auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
        cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
        cmd.src = copy.src_texture;
        cmd.slice = copy.src_slice;
        cmd.level = copy.src_level;
        cmd.origin = copy.src_origin;
        cmd.size = copy.size;
        cmd.dst = copy.dst_buffer;
        cmd.offset = copy.dst_offset;
        cmd.bytes_per_row = copy.bytes_per_row;
        cmd.bytes_per_image = copy.bytes_per_image;
        break;
      }
      case CopyWork::Kind::TextureToTexture: {
        auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
        cmd.type = WMTBlitCommandCopyFromTextureToTexture;
        cmd.src = copy.src_texture;
        cmd.src_slice = copy.src_slice;
        cmd.src_level = copy.src_level;
        cmd.src_origin = copy.src_origin;
        cmd.src_size = copy.size;
        cmd.dst = copy.dst_texture;
        cmd.dst_slice = copy.dst_slice;
        cmd.dst_level = copy.dst_level;
        cmd.dst_origin = copy.dst_origin;
        break;
      }
      }
    }

    enc.endPass();
  });
}

/* Every D3D12 application asks what the device can do before it does anything
 * else, and a great many treat a failure here as fatal -- returning E_NOTIMPL
 * meant nothing got past device creation.
 *
 * Where a capability is not certain, the conservative answer is the safe one:
 * a game told a tier is lower than it really is takes a slower path that
 * works, while one told a tier is higher takes a path this cannot run. Tiers
 * are raised here only once something tests them.
 *
 * An unknown feature is E_INVALIDARG, which is what a real driver returns --
 * that is how an application discovers a feature its runtime knows and the
 * driver does not. */
HRESULT
DxmtD3D12Device::CheckFeatureSupport(D3D12_FEATURE feature, void *data, UINT size) {
  if (!data)
    return E_INVALIDARG;

  /* The size is the application's declaration of which version of the struct
   * it compiled against, so it has to match exactly. */
  auto fits = [&](size_t want) { return size == want; };

  switch (feature) {
  case D3D12_FEATURE_D3D12_OPTIONS: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)))
      return E_INVALIDARG;
    auto *o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(data);
    *o = {};
    /* Metal has no fp64 on any Apple GPU, which is also why the DADD/DMUL
     * family is absent from the converter. */
    o->DoublePrecisionFloatShaderOps = FALSE;
    o->OutputMergerLogicOp = FALSE;
    o->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
    /* Sparse/tiled resources are unimplemented in the d3d11 core this shares,
     * where every tile entry point is a stub. */
    o->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
    /* Tier 2 rather than 3: descriptor heaps are sized on request and the
     * shader-visible heap is a Metal argument buffer, so tier 3 is likely --
     * but nothing here has tested unbounded UAV ranges yet. */
    o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_2;
    o->PSSpecifiedStencilRefSupported = FALSE;
    o->TypedUAVLoadAdditionalFormats = FALSE;
    /* Apple GPUs do have raster order groups, but nothing maps ROVs onto
     * them yet. */
    o->ROVsSupported = FALSE;
    o->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
    o->MaxGPUVirtualAddressBitsPerResource = 40;
    o->StandardSwizzle64KBSupported = FALSE;
    o->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
    o->CrossAdapterRowMajorTextureSupported = FALSE;
    o->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = FALSE;
    /* Metal heaps take buffers and textures alike, which is what tier 2 says. */
    o->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
    return S_OK;
  }
  case D3D12_FEATURE_ARCHITECTURE: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_ARCHITECTURE)))
      return E_INVALIDARG;
    auto *a = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(data);
    if (a->NodeIndex != 0)
      return E_INVALIDARG;
    /* Apple silicon: one physical pool of memory behind a tile-based GPU. */
    a->TileBasedRenderer = TRUE;
    a->UMA = TRUE;
    a->CacheCoherentUMA = TRUE;
    return S_OK;
  }
  case D3D12_FEATURE_ARCHITECTURE1: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1)))
      return E_INVALIDARG;
    auto *a = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1 *>(data);
    if (a->NodeIndex != 0)
      return E_INVALIDARG;
    a->TileBasedRenderer = TRUE;
    a->UMA = TRUE;
    a->CacheCoherentUMA = TRUE;
    a->IsolatedMMU = TRUE;
    return S_OK;
  }
  case D3D12_FEATURE_FEATURE_LEVELS: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)))
      return E_INVALIDARG;
    auto *f = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(data);
    if (!f->NumFeatureLevels || !f->pFeatureLevelsRequested)
      return E_INVALIDARG;
    /* The highest of those asked for that is at or below what D3D12CreateDevice
     * itself accepts, so the two can never disagree. */
    const D3D_FEATURE_LEVEL ceiling = D3D_FEATURE_LEVEL_11_1;
    bool any = false;
    D3D_FEATURE_LEVEL best = D3D_FEATURE_LEVEL_11_0;
    for (UINT i = 0; i < f->NumFeatureLevels; i++) {
      D3D_FEATURE_LEVEL want = f->pFeatureLevelsRequested[i];
      if (want <= ceiling && (!any || want > best)) {
        best = want;
        any = true;
      }
    }
    if (!any)
      return E_INVALIDARG;
    f->MaxSupportedFeatureLevel = best;
    return S_OK;
  }
  case D3D12_FEATURE_FORMAT_SUPPORT: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT)))
      return E_INVALIDARG;
    auto *f = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(data);
    f->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
    f->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
    /* The d3d11 device underneath already answers this against the same Metal
     * device, and its answer has been tested; the two enumerations share their
     * values bit for bit. */
    UINT support = 0;
    if (d3d11_device_ && SUCCEEDED(d3d11_device_->CheckFormatSupport(f->Format, &support)))
      f->Support1 = static_cast<D3D12_FORMAT_SUPPORT1>(support);
    return S_OK;
  }
  case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS)))
      return E_INVALIDARG;
    auto *m = static_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(data);
    m->NumQualityLevels = 0;
    UINT levels = 0;
    if (d3d11_device_ &&
        SUCCEEDED(d3d11_device_->CheckMultisampleQualityLevels(m->Format, m->SampleCount, &levels)))
      m->NumQualityLevels = levels;
    return S_OK;
  }
  case D3D12_FEATURE_FORMAT_INFO: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_FORMAT_INFO)))
      return E_INVALIDARG;
    auto *f = static_cast<D3D12_FEATURE_DATA_FORMAT_INFO *>(data);
    /* Planar formats are the only ones with more than one plane, and none of
     * them are supported here. */
    f->PlaneCount = 1;
    return S_OK;
  }
  case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT)))
      return E_INVALIDARG;
    auto *g = static_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *>(data);
    g->MaxGPUVirtualAddressBitsPerResource = 40;
    g->MaxGPUVirtualAddressBitsPerProcess = 40;
    return S_OK;
  }
  case D3D12_FEATURE_SHADER_MODEL: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_SHADER_MODEL)))
      return E_INVALIDARG;
    auto *m = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(data);
    /* Clamped down, never up: the caller passes the highest it can use and
     * gets back the highest both sides can. The Metal Shader Converter takes
     * DXIL through 6.6, which is where ResourceDescriptorHeap arrives. */
    const D3D_SHADER_MODEL ceiling = D3D_SHADER_MODEL_6_6;
    if (m->HighestShaderModel > ceiling)
      m->HighestShaderModel = ceiling;
    return S_OK;
  }
  case D3D12_FEATURE_ROOT_SIGNATURE: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE)))
      return E_INVALIDARG;
    auto *r = static_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(data);
    /* Same clamping contract as the shader model. Version 1.1 adds descriptor
     * volatility flags, which the parser reads. */
    if (r->HighestVersion > D3D_ROOT_SIGNATURE_VERSION_1_1)
      r->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    return S_OK;
  }
  case D3D12_FEATURE_SHADER_CACHE: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_SHADER_CACHE)))
      return E_INVALIDARG;
    auto *c = static_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(data);
    /* An in-process cache exists; nothing is written to disk, and claiming a
     * library the application could then load from would be wrong. */
    c->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO;
    return S_OK;
  }
  case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY)))
      return E_INVALIDARG;
    auto *q = static_cast<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY *>(data);
    /* Metal has no global-realtime queue class to hand out. */
    q->PriorityForTypeIsSupported =
        q->Priority != D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    return S_OK;
  }
  case D3D12_FEATURE_EXISTING_HEAPS: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS)))
      return E_INVALIDARG;
    static_cast<D3D12_FEATURE_DATA_EXISTING_HEAPS *>(data)->Supported = FALSE;
    return S_OK;
  }
  case D3D12_FEATURE_SERIALIZATION: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_SERIALIZATION)))
      return E_INVALIDARG;
    auto *z = static_cast<D3D12_FEATURE_DATA_SERIALIZATION *>(data);
    if (z->NodeIndex != 0)
      return E_INVALIDARG;
    z->HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
    return S_OK;
  }
  case D3D12_FEATURE_CROSS_NODE: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_CROSS_NODE)))
      return E_INVALIDARG;
    auto *c = static_cast<D3D12_FEATURE_DATA_CROSS_NODE *>(data);
    c->SharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
    c->AtomicShaderInstructions = FALSE;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS1: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1)))
      return E_INVALIDARG;
    auto *o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1 *>(data);
    *o = {};
    /* Metal's SIMD group width on Apple GPUs is 32, fixed. */
    o->WaveOps = TRUE;
    o->WaveLaneCountMin = 32;
    o->WaveLaneCountMax = 32;
    o->TotalLaneCount = 32;
    o->ExpandedComputeResourceStates = TRUE;
    o->Int64ShaderOps = FALSE;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS2: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2)))
      return E_INVALIDARG;
    auto *o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2 *>(data);
    o->DepthBoundsTestSupported = FALSE;
    o->ProgrammableSamplePositionsTier =
        D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS3: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3)))
      return E_INVALIDARG;
    auto *o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3 *>(data);
    *o = {};
    o->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS4: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4)))
      return E_INVALIDARG;
    auto *o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(data);
    *o = {};
    o->SharedResourceCompatibilityTier =
        D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
    /* Metal has half; nothing maps the D3D12 native-16-bit path to it yet. */
    o->Native16BitShaderOpsSupported = FALSE;
    return S_OK;
  }
  case D3D12_FEATURE_D3D12_OPTIONS5: {
    if (!fits(sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)))
      return E_INVALIDARG;
    auto *o = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(data);
    *o = {};
    o->SRVOnlyTiledResourceTier3 = FALSE;
    o->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
    /* Metal has ray tracing; DXR is not wired to it. */
    o->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    return S_OK;
  }
  default:
    /* A feature this does not know. E_INVALIDARG is the documented answer and
     * is how an application tells a missing feature from a broken driver. */
    return E_INVALIDARG;
  }
}

HRESULT DxmtD3D12Device::CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE type,
                                           ID3D12CommandAllocator *,
                                           ID3D12PipelineState *initial_state,
                                           REFIID riid, void **command_list) {
  if (!command_list)
    return E_POINTER;
  *command_list = nullptr;
  if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COPY &&
      type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
    return E_NOTIMPL;
  auto object = new DxmtD3D12GraphicsCommandList(this, type, initial_state);
  HRESULT hr = object->QueryInterface(riid, command_list);
  object->Release();
  return hr;
}

HRESULT DxmtD3D12Device::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES *heap_properties, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
    REFIID riid, void **resource) {
  if (!resource)
    return E_POINTER;
  *resource = nullptr;
  if (!desc)
    return E_INVALIDARG;
  D3D12_HEAP_PROPERTIES heap = heap_properties
                                   ? *heap_properties
                                   : D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_DEFAULT,
                                                           D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                                           D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  static std::atomic<unsigned> traced = 0;
  if (d3d12_diagnostics() && traced.fetch_add(1) < 40)
    Logger::warn(str::format("D3D12 committed dimension=", UINT(desc->Dimension), " format=", UINT(desc->Format),
                             " width=", desc->Width, " height=", desc->Height));
  auto object = new DxmtD3D12Resource(this, heap, *desc);
  if (!object->initialize_backing()) {
    object->Release();
    return E_FAIL;
  }
  HRESULT hr = object->QueryInterface(riid, resource);
  object->Release();
  return hr;
}

HRESULT DxmtD3D12Device::CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid, void **heap) {
  if (!heap)
    return E_POINTER;
  *heap = nullptr;
  if (!desc || !desc->SizeInBytes)
    return E_INVALIDARG;
  if (!metal_device_)
    return E_FAIL;

  WMTHeapInfo info = {};
  info.size = desc->SizeInBytes;
  info.options = heap_resource_options(desc->Properties);
  /* Placement, not automatic: D3D12 decides the offset of every resource, and
   * an automatic heap does not accept one. */
  info.type = WMTHeapTypePlacement;

  auto metal_heap = metal_device_.newHeap(info);
  if (!metal_heap)
    return E_OUTOFMEMORY;

  auto object = new DxmtD3D12Heap(this, *desc, std::move(metal_heap));
  HRESULT hr = object->QueryInterface(riid, heap);
  object->Release();
  return hr;
}

HRESULT DxmtD3D12Device::CreatePlacedResource(ID3D12Heap *heap, UINT64 heap_offset,
                                              const D3D12_RESOURCE_DESC *desc,
                                              D3D12_RESOURCE_STATES,
                                              const D3D12_CLEAR_VALUE *, REFIID riid,
                                              void **resource) {
  if (!resource)
    return E_POINTER;
  *resource = nullptr;
  if (!heap || !desc)
    return E_INVALIDARG;
  if (!metal_device_)
    return E_FAIL;

  /* Only heaps this device produced carry an MTLHeap; anything else would be a
   * blind cast. */
  ID3D12Heap *validated = nullptr;
  if (FAILED(heap->QueryInterface(__uuidof(ID3D12Heap), (void **)&validated)))
    return E_INVALIDARG;
  auto *dxmt_heap = static_cast<DxmtD3D12Heap *>(validated);
  validated->Release();
  WMT::Heap metal_heap = dxmt_heap->metal_heap();
  if (!metal_heap)
    return E_INVALIDARG;

  static std::atomic<unsigned> traced = 0;
  if (d3d12_diagnostics() && traced.fetch_add(1) < 40)
    Logger::warn(str::format("D3D12 placed dimension=", UINT(desc->Dimension), " format=", UINT(desc->Format),
                             " width=", desc->Width, " height=", desc->Height));
  DxmtD3D12Resource *object = nullptr;

  if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    WMTBufferInfo info = {};
    info.length = std::max<UINT64>(1, desc->Width);
    info.options = dxmt_heap->resource_options();
    info.memory.set(nullptr);
    info.gpu_address = 0;

    uint64_t buffer_offset = 0;
    WMT::Reference<WMT::Buffer> buffer;
#if defined(__i386__)
    /* MTLHeap contents can lie above 4GB. Keep CPU-visible placed buffers in
     * one Wine-addressable allocation per heap, preserving placement aliases
     * without truncating a host pointer or copying on Map/Unmap. */
    auto properties = heap_desc_properties(dxmt_heap);
    if (properties.Type == D3D12_HEAP_TYPE_UPLOAD || properties.Type == D3D12_HEAP_TYPE_READBACK) {
      buffer = dxmt_heap->cpu_buffer(metal_device_, heap_offset, info.length, info);
      buffer_offset = heap_offset;
    } else
#endif
      buffer = metal_heap.newBuffer(info, heap_offset);
    if (!buffer)
      return E_OUTOFMEMORY;

    object = new DxmtD3D12Resource(this, *desc, dxmt_heap, heap_offset, std::move(buffer), info.gpu_address,
                                   info.memory.get_accessible_or_null(), buffer_offset);
  } else {
    WMTTextureInfo info;
    if (!texture_info_from_desc(metal_device_, *desc, dxmt_heap->resource_options(), info))
      return E_INVALIDARG;

    auto texture = metal_heap.newTexture(info, heap_offset);
    if (!texture)
      return E_OUTOFMEMORY;

    object = new DxmtD3D12Resource(this, *desc, dxmt_heap, heap_offset, std::move(texture),
                                   info.gpu_resource_id);
  }

  if (!object->initialize_backing()) {
    object->Release();
    return E_FAIL;
  }
  HRESULT hr = object->QueryInterface(riid, resource);
  object->Release();
  return hr;
}

void DxmtD3D12Device::CreateRenderTargetView(ID3D12Resource *resource,
                                             const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  Descriptor *dst = DxmtD3D12DescriptorHeap::from_cpu(descriptor);
  if (!dst) return;
  dst->type = DescriptorType::Rtv;
  dst->resource = static_cast<DxmtD3D12Resource *>(resource);
  dst->rtv = nullptr;
  D3D11_RENDER_TARGET_VIEW_DESC view_desc = {};
  if (desc) {
    view_desc.Format = desc->Format;
    view_desc.ViewDimension = D3D11_RTV_DIMENSION(desc->ViewDimension);
    switch (desc->ViewDimension) {
    case D3D12_RTV_DIMENSION_BUFFER:
      view_desc.Buffer = {UINT(desc->Buffer.FirstElement), desc->Buffer.NumElements}; break;
    case D3D12_RTV_DIMENSION_TEXTURE1D:
      view_desc.Texture1D = {desc->Texture1D.MipSlice}; break;
    case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
      view_desc.Texture1DArray = {desc->Texture1DArray.MipSlice, desc->Texture1DArray.FirstArraySlice,
                                 desc->Texture1DArray.ArraySize}; break;
    case D3D12_RTV_DIMENSION_TEXTURE2D:
      view_desc.Texture2D = {desc->Texture2D.MipSlice}; break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
      view_desc.Texture2DArray = {desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice,
                                 desc->Texture2DArray.ArraySize}; break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
      view_desc.Texture2DMSArray = {desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize}; break;
    case D3D12_RTV_DIMENSION_TEXTURE3D:
      view_desc.Texture3D = {desc->Texture3D.MipSlice, desc->Texture3D.FirstWSlice, desc->Texture3D.WSize}; break;
    default: break;
    }
  }
  HRESULT hr = dst->resource ? d3d11_device_->CreateRenderTargetView(dst->resource->d3d11_resource(),
                                                                    desc ? &view_desc : nullptr, dst->rtv.put()) : E_INVALIDARG;
  if (FAILED(hr) && d3d12_diagnostics())
    Logger::warn(str::format("D3D12 RTV failed hr=", hr, " format=", desc ? UINT(desc->Format) : 0));
}

void DxmtD3D12Device::CreateDepthStencilView(ID3D12Resource *resource,
                                             const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  Descriptor *dst = DxmtD3D12DescriptorHeap::from_cpu(descriptor);
  if (!dst) return;
  dst->type = DescriptorType::Dsv;
  dst->resource = static_cast<DxmtD3D12Resource *>(resource);
  dst->dsv = nullptr;
  D3D11_DEPTH_STENCIL_VIEW_DESC view_desc = {};
  if (desc) {
    view_desc.Format = desc->Format;
    view_desc.Flags = UINT(desc->Flags);
    view_desc.ViewDimension = D3D11_DSV_DIMENSION(desc->ViewDimension);
    switch (desc->ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE1D:
      view_desc.Texture1D = {desc->Texture1D.MipSlice}; break;
    case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
      view_desc.Texture1DArray = {desc->Texture1DArray.MipSlice, desc->Texture1DArray.FirstArraySlice,
                                 desc->Texture1DArray.ArraySize}; break;
    case D3D12_DSV_DIMENSION_TEXTURE2D:
      view_desc.Texture2D = {desc->Texture2D.MipSlice}; break;
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
      view_desc.Texture2DArray = {desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice,
                                 desc->Texture2DArray.ArraySize}; break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
      view_desc.Texture2DMSArray = {desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize}; break;
    default: break;
    }
  }
  HRESULT hr = dst->resource ? d3d11_device_->CreateDepthStencilView(dst->resource->d3d11_resource(),
                                                                    desc ? &view_desc : nullptr, dst->dsv.put()) : E_INVALIDARG;
  if (FAILED(hr) && d3d12_diagnostics())
    Logger::warn(str::format("D3D12 DSV failed hr=", hr, " format=", desc ? UINT(desc->Format) : 0));
}

/* GPU address of a resource's buffer storage. A placed resource has real Metal
 * memory; a committed one is still backed by d3d11 and has none, so the caller
 * ends up writing a null descriptor rather than a wrong one. */
uint64_t DxmtD3D12Device::buffer_address(DxmtD3D12Resource *resource,
                                         D3D12_GPU_VIRTUAL_ADDRESS location) const {
  if (!resource || !resource->metal_buffer())
    return 0;
  uint64_t base = resource->GetGPUVirtualAddress();
  if (!base)
    return 0;
  /* location points somewhere inside the resource; keep the offset. */
  return location >= base ? base + (location - base) : base;
}

dxmt::DescriptorEntry DxmtD3D12Device::encode_resource_descriptor(DxmtD3D12Resource *resource) const {
  if (!resource)
    return dxmt::DescriptorEntry{};

  D3D12_RESOURCE_DESC desc = {};
  resource->GetDesc(&desc);

  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return dxmt::encodeBufferDescriptor(resource->GetGPUVirtualAddress(),
                                        static_cast<uint32_t>(desc.Width));

  if (!resource->metal_texture_id())
    return dxmt::DescriptorEntry{};
  return dxmt::encodeTextureDescriptor(resource->metal_texture_id(), 0.0f);
}

void DxmtD3D12Device::CreateShaderResourceView(ID3D12Resource *resource,
                                               const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  Descriptor *dst = DxmtD3D12DescriptorHeap::from_cpu(descriptor);
  if (!dst) return;
  dst->type = DescriptorType::Srv;
  dst->resource = static_cast<DxmtD3D12Resource *>(resource);
  dst->srv = nullptr;
  dst->texture_view = nullptr;
  D3D11_SHADER_RESOURCE_VIEW_DESC view_desc = {};
  if (desc) {
    view_desc.Format = desc->Format;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION(desc->ViewDimension);
    switch (desc->ViewDimension) {
    case D3D12_SRV_DIMENSION_BUFFER:
      view_desc.Buffer.FirstElement = UINT(desc->Buffer.FirstElement);
      view_desc.Buffer.NumElements = desc->Buffer.NumElements;
      if (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      }
      break;
    case D3D12_SRV_DIMENSION_TEXTURE1D:
      view_desc.Texture1D = {desc->Texture1D.MostDetailedMip, desc->Texture1D.MipLevels}; break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
      view_desc.Texture1DArray = {desc->Texture1DArray.MostDetailedMip, desc->Texture1DArray.MipLevels,
                                 desc->Texture1DArray.FirstArraySlice, desc->Texture1DArray.ArraySize}; break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
      view_desc.Texture2D = {desc->Texture2D.MostDetailedMip, desc->Texture2D.MipLevels}; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
      view_desc.Texture2DArray = {desc->Texture2DArray.MostDetailedMip, desc->Texture2DArray.MipLevels,
                                 desc->Texture2DArray.FirstArraySlice, desc->Texture2DArray.ArraySize}; break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
      view_desc.Texture2DMSArray = {desc->Texture2DMSArray.FirstArraySlice, desc->Texture2DMSArray.ArraySize}; break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
      view_desc.Texture3D = {desc->Texture3D.MostDetailedMip, desc->Texture3D.MipLevels}; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
      view_desc.TextureCube = {desc->TextureCube.MostDetailedMip, desc->TextureCube.MipLevels}; break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
      view_desc.TextureCubeArray = {desc->TextureCubeArray.MostDetailedMip, desc->TextureCubeArray.MipLevels,
                                    desc->TextureCubeArray.First2DArrayFace, desc->TextureCubeArray.NumCubes}; break;
    default: break;
    }
  }
  ComPtr<ID3D11Buffer> structured;
  ID3D11Resource *backing = dst->resource ? dst->resource->d3d11_resource() : nullptr;
  if (backing && desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER && desc->Buffer.StructureByteStride) {
    ComPtr<ID3D11Buffer> original;
    if (SUCCEEDED(backing->QueryInterface(IID_PPV_ARGS(original.put())))) {
      D3D11_BUFFER_DESC buffer_desc = {};
      original->GetDesc(&buffer_desc);
      buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      buffer_desc.StructureByteStride = desc->Buffer.StructureByteStride;
      using CreateAlias = HRESULT (WINAPI *)(ID3D11Buffer *, const D3D11_BUFFER_DESC *, ID3D11Buffer **);
      auto create_alias = reinterpret_cast<CreateAlias>(GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "DXMTCreateBufferAlias"));
      if (create_alias && SUCCEEDED(create_alias(original.get(), &buffer_desc, structured.put())))
        backing = structured.get();
    }
  }
  HRESULT hr = E_INVALIDARG;
  if (dst->resource)
    hr = d3d11_device_->CreateShaderResourceView(backing, desc ? &view_desc : nullptr,
                                                dst->srv.put());
  if (FAILED(hr) && d3d12_diagnostics())
    Logger::warn(str::format("D3D12 SRV failed hr=", hr, " format=", desc ? UINT(desc->Format) : 0,
                             " dimension=", desc ? UINT(desc->ViewDimension) : 0));
  auto entry = encode_resource_descriptor(dst->resource.get());
  if (dst->srv) {
    auto *view = static_cast<dxmt::D3D11ShaderResourceView *>(dst->srv.get());
    if (auto texture = view->texture()) {
      auto &typed = texture->view(dxmt::TextureViewKey(view->viewId()));
      dst->texture_view = WMT::Texture(typed.texture);
      float clamp = 0;
      if (desc) {
        switch (desc->ViewDimension) {
        case D3D12_SRV_DIMENSION_TEXTURE1D: clamp = desc->Texture1D.ResourceMinLODClamp; break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: clamp = desc->Texture1DArray.ResourceMinLODClamp; break;
        case D3D12_SRV_DIMENSION_TEXTURE2D: clamp = desc->Texture2D.ResourceMinLODClamp; break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: clamp = desc->Texture2DArray.ResourceMinLODClamp; break;
        case D3D12_SRV_DIMENSION_TEXTURE3D: clamp = desc->Texture3D.ResourceMinLODClamp; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE: clamp = desc->TextureCube.ResourceMinLODClamp; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: clamp = desc->TextureCubeArray.ResourceMinLODClamp; break;
        default: break;
        }
      }
      entry = dxmt::encodeTextureDescriptor(typed.gpuResourceID, clamp);
    } else if (auto buffer = view->buffer()) {
      auto slice = view->bufferSlice();
      entry = dxmt::encodeBufferDescriptor(buffer->current()->gpuAddress() +
                                           buffer->current()->currentSuballocationOffset() + slice.byteOffset,
                                           slice.byteLength);
    }
  }
  write_gpu_descriptor(dst, entry);
}

void DxmtD3D12Device::CreateUnorderedAccessView(ID3D12Resource *resource, ID3D12Resource *counter,
                                                const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  Descriptor *dst = DxmtD3D12DescriptorHeap::from_cpu(descriptor);
  if (!dst) return;
  dst->type = DescriptorType::Uav;
  dst->resource = static_cast<DxmtD3D12Resource *>(resource);
  dst->srv = nullptr; dst->uav = nullptr; dst->texture_view = nullptr;
  D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc = {};
  if (desc) {
    view_desc.Format = desc->Format;
    view_desc.ViewDimension = D3D11_UAV_DIMENSION(desc->ViewDimension);
    switch (desc->ViewDimension) {
    case D3D12_UAV_DIMENSION_BUFFER:
      view_desc.Buffer = {UINT(desc->Buffer.FirstElement), desc->Buffer.NumElements,
                          (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? D3D11_BUFFER_UAV_FLAG_RAW : 0u}; break;
    case D3D12_UAV_DIMENSION_TEXTURE1D: view_desc.Texture1D.MipSlice = desc->Texture1D.MipSlice; break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
      view_desc.Texture1DArray = {desc->Texture1DArray.MipSlice, desc->Texture1DArray.FirstArraySlice,
                                   desc->Texture1DArray.ArraySize}; break;
    case D3D12_UAV_DIMENSION_TEXTURE2D: view_desc.Texture2D.MipSlice = desc->Texture2D.MipSlice; break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
      view_desc.Texture2DArray = {desc->Texture2DArray.MipSlice, desc->Texture2DArray.FirstArraySlice,
                                   desc->Texture2DArray.ArraySize}; break;
    case D3D12_UAV_DIMENSION_TEXTURE3D:
      view_desc.Texture3D = {desc->Texture3D.MipSlice, desc->Texture3D.FirstWSlice, desc->Texture3D.WSize}; break;
    default: break;
    }
  }
  ID3D11Resource *backing = dst->resource ? dst->resource->d3d11_resource() : nullptr;
  ComPtr<ID3D11Buffer> structured;
  if (backing && desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER && desc->Buffer.StructureByteStride) {
    ComPtr<ID3D11Buffer> original;
    if (SUCCEEDED(backing->QueryInterface(IID_PPV_ARGS(original.put())))) {
      D3D11_BUFFER_DESC buffer_desc = {};
      original->GetDesc(&buffer_desc);
      buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
      buffer_desc.StructureByteStride = desc->Buffer.StructureByteStride;
      using CreateAlias = HRESULT (WINAPI *)(ID3D11Buffer *, const D3D11_BUFFER_DESC *, ID3D11Buffer **);
      auto alias = reinterpret_cast<CreateAlias>(GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "DXMTCreateBufferAlias"));
      if (alias && SUCCEEDED(alias(original.get(), &buffer_desc, structured.put()))) backing = structured.get();
    }
  }
  HRESULT hr = backing && !counter ? d3d11_device_->CreateUnorderedAccessView(backing, desc ? &view_desc : nullptr,
                                                                             dst->uav.put()) : E_INVALIDARG;
  if (FAILED(hr) && d3d12_diagnostics()) Logger::warn(str::format("D3D12 UAV failed hr=", hr));
  auto entry = encode_resource_descriptor(dst->resource.get());
  if (dst->uav) {
    auto *view = static_cast<dxmt::D3D11UnorderedAccessView *>(dst->uav.get());
    if (auto texture = view->texture()) {
      auto &typed = texture->view(dxmt::TextureViewKey(view->viewId()));
      dst->texture_view = WMT::Texture(typed.texture);
      entry = dxmt::encodeTextureDescriptor(typed.gpuResourceID, 0);
    } else if (auto buffer = view->buffer()) {
      auto slice = view->bufferSlice();
      entry = dxmt::encodeBufferDescriptor(buffer->current()->gpuAddress() +
                                           buffer->current()->currentSuballocationOffset() + slice.byteOffset,
                                           slice.byteLength);
    }
  }
  write_gpu_descriptor(dst, entry);
}

void DxmtD3D12Device::CreateSampler(const D3D12_SAMPLER_DESC *desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  Descriptor *dst = DxmtD3D12DescriptorHeap::from_cpu(descriptor);
  dst->type = DescriptorType::Sampler;
  dst->sampler = {};
  dst->sampler11 = nullptr;

  if (!desc || !metal_device_)
    return;

  D3D11_SAMPLER_DESC legacy = {};
  legacy.Filter = D3D11_FILTER(desc->Filter);
  legacy.AddressU = D3D11_TEXTURE_ADDRESS_MODE(desc->AddressU);
  legacy.AddressV = D3D11_TEXTURE_ADDRESS_MODE(desc->AddressV);
  legacy.AddressW = D3D11_TEXTURE_ADDRESS_MODE(desc->AddressW);
  legacy.MipLODBias = desc->MipLODBias;
  legacy.MaxAnisotropy = desc->MaxAnisotropy;
  legacy.ComparisonFunc = D3D11_COMPARISON_FUNC(desc->ComparisonFunc);
  std::copy(std::begin(desc->BorderColor), std::end(desc->BorderColor), std::begin(legacy.BorderColor));
  legacy.MinLOD = desc->MinLOD;
  legacy.MaxLOD = desc->MaxLOD;
  d3d11_device_->CreateSamplerState(&legacy, dst->sampler11.put());

  WMTSamplerInfo info = {};
  info.min_filter = (desc->Filter & kFilterMinLinear) ? WMTSamplerMinMagFilterLinear
                                                      : WMTSamplerMinMagFilterNearest;
  info.mag_filter = (desc->Filter & kFilterMagLinear) ? WMTSamplerMinMagFilterLinear
                                                      : WMTSamplerMinMagFilterNearest;
  info.mip_filter = (desc->Filter & kFilterMipLinear) ? WMTSamplerMipFilterLinear
                                                      : WMTSamplerMipFilterNearest;
  info.s_address_mode = sampler_address_mode(desc->AddressU);
  info.t_address_mode = sampler_address_mode(desc->AddressV);
  info.r_address_mode = sampler_address_mode(desc->AddressW);
  info.border_color = sampler_border_color(desc->BorderColor);
  info.compare_function = (desc->Filter & kFilterComparison)
                              ? static_cast<WMTCompareFunction>(std::min<UINT>(desc->ComparisonFunc
                                                                                   ? desc->ComparisonFunc - 1
                                                                                   : 0,
                                                                               WMTCompareFunctionAlways))
                              : WMTCompareFunctionNever;
  info.lod_min_clamp = desc->MinLOD;
  info.lod_max_clamp = desc->MaxLOD;
  info.max_anisotroy = (desc->Filter & kFilterAnisotropic) ? std::max<UINT>(1, desc->MaxAnisotropy) : 1;
  info.normalized_coords = true;
  info.lod_average = false;
  /* Required: the converter reads samplers out of an argument buffer, and a
   * sampler without this has no resource id to encode. */
  info.support_argument_buffers = true;

  dst->sampler = metal_device_.newSamplerState(info);
  if (!dst->sampler)
    return;

  /* D3D12 carries the LOD bias in the sampler; Metal carries it in the
   * descriptor, which is where the converter looks for it. */
  write_gpu_descriptor(dst, dxmt::encodeSamplerDescriptor(info.gpu_resource_id, desc->MipLODBias));
}

void DxmtD3D12Device::CopyDescriptorsSimple(UINT descriptor_count,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE src,
                                            D3D12_DESCRIPTOR_HEAP_TYPE) {
  Descriptor *to = DxmtD3D12DescriptorHeap::from_cpu(dst);
  const Descriptor *from = DxmtD3D12DescriptorHeap::from_cpu(src);
  if (!to || !from)
    return;

  /* Descriptors hold COM references, so this cannot be a memcpy: that would
   * copy the pointers without a reference and drop the destination's. */
  for (UINT i = 0; i < descriptor_count; i++) {
    to[i].assign_contents(from[i]);
    write_gpu_descriptor(&to[i], to[i].gpu_entry);
  }
}

void DxmtD3D12Device::CopyDescriptors(UINT dst_range_count,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE *dst_range_starts,
                                      const UINT *dst_range_sizes, UINT src_range_count,
                                      const D3D12_CPU_DESCRIPTOR_HANDLE *src_range_starts,
                                      const UINT *src_range_sizes,
                                      D3D12_DESCRIPTOR_HEAP_TYPE type) {
  if (!dst_range_starts || !src_range_starts)
    return;

  uint32_t segment_count =
      dxmt::planDescriptorCopy(dst_range_count, dst_range_sizes, src_range_count, src_range_sizes, nullptr, 0);
  if (!segment_count)
    return;

  std::vector<dxmt::DescriptorCopySegment> segments(segment_count);
  dxmt::planDescriptorCopy(dst_range_count, dst_range_sizes, src_range_count, src_range_sizes, segments.data(),
                           segment_count);

  for (const auto &segment : segments) {
    D3D12_CPU_DESCRIPTOR_HANDLE to = {dst_range_starts[segment.dst_range].ptr +
                                      segment.dst_offset * sizeof(Descriptor)};
    D3D12_CPU_DESCRIPTOR_HANDLE from = {src_range_starts[segment.src_range].ptr +
                                        segment.src_offset * sizeof(Descriptor)};
    CopyDescriptorsSimple(segment.count, to, from, type);
  }
}

HRESULT DxmtD3D12DXGIDevice::query_inner(IMTLDXGIDevice **inner) const {
  if (!inner)
    return E_POINTER;
  *inner = nullptr;
  return device_ && device_->d3d11_device()
             ? device_->d3d11_device()->QueryInterface(__uuidof(IMTLDXGIDevice),
                                                       reinterpret_cast<void **>(inner))
             : E_NOINTERFACE;
}

HRESULT DxmtD3D12DXGIDevice::SetPrivateData(REFGUID guid, UINT data_size,
                                            const void *data) {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return S_OK;
  hr = inner->SetPrivateData(guid, data_size, data);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::SetPrivateDataInterface(REFGUID guid,
                                                     const IUnknown *object) {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return S_OK;
  hr = inner->SetPrivateDataInterface(guid, object);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::GetPrivateData(REFGUID guid, UINT *data_size,
                                            void *data) {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return DXGI_ERROR_NOT_FOUND;
  hr = inner->GetPrivateData(guid, data_size, data);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::GetParent(REFIID riid, void **parent) {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return hr;
  hr = inner->GetParent(riid, parent);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::GetAdapter(IDXGIAdapter **adapter) {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return hr;
  hr = inner->GetAdapter(adapter);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::QueryResourceResidency(IUnknown *const *,
                                                    DXGI_RESIDENCY *residency,
                                                    UINT resource_count) {
  if (residency) {
    for (UINT i = 0; i < resource_count; i++)
      residency[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
  }
  return S_OK;
}

HRESULT DxmtD3D12DXGIDevice::SetGPUThreadPriority(INT priority) {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return S_OK;
  hr = inner->SetGPUThreadPriority(priority);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::GetGPUThreadPriority(INT *priority) {
  if (!priority)
    return E_POINTER;
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr)) {
    *priority = 0;
    return S_OK;
  }
  hr = inner->GetGPUThreadPriority(priority);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::SetMaximumFrameLatency(UINT max_latency) {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return S_OK;
  hr = inner->SetMaximumFrameLatency(max_latency);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::GetMaximumFrameLatency(UINT *max_latency) {
  if (!max_latency)
    return E_POINTER;
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr)) {
    *max_latency = 1;
    return S_OK;
  }
  hr = inner->GetMaximumFrameLatency(max_latency);
  inner->Release();
  return hr;
}

HRESULT DxmtD3D12DXGIDevice::ReclaimResources(UINT resource_count,
                                              IDXGIResource *const *,
                                              BOOL *discarded) {
  if (discarded) {
    for (UINT i = 0; i < resource_count; i++)
      discarded[i] = FALSE;
  }
  return S_OK;
}

WMT::Device DxmtD3D12DXGIDevice::GetMTLDevice() {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return {};
  WMT::Device device = inner->GetMTLDevice();
  inner->Release();
  return device;
}

D3DKMT_HANDLE DxmtD3D12DXGIDevice::GetLocalD3DKMT() {
  IMTLDXGIDevice *inner = nullptr;
  HRESULT hr = query_inner(&inner);
  if (FAILED(hr))
    return 0;
  D3DKMT_HANDLE handle = inner->GetLocalD3DKMT();
  inner->Release();
  return handle;
}

HRESULT DxmtD3D12DXGIDevice::CreateSwapChain(
    IDXGIFactory1 *factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
    IDXGISwapChain1 **swap_chain) {
  DxmtD3D12SwapChainFactory swapchain_factory(device_.get(), queue_.get());
  return swapchain_factory.CreateSwapChain(factory, hwnd, desc, fullscreen_desc, swap_chain);
}

/* Record GPU signal/wait positions through the shared D3D11 command chunk.
 * For CPU readback, publish the logical fence only after that GPU position
 * and the queue's actual result writes are complete. A separate worker keeps
 * that publication from blocking independent work on the physical queue. */
HRESULT DxmtD3D12CommandQueue::emit_signal(ID3D12Fence *fence, UINT64 value) {
  std::lock_guard<std::recursive_mutex> lock(device_->submission_mutex());
  static std::atomic<unsigned> traced = 0;
  if (d3d12_diagnostics() && traced.fetch_add(1) < 32)
    Logger::warn(str::format("D3D12 signal queue=", this, " type=", UINT(desc_.Type), " fence=", fence, " value=", value));
  auto *timeline_fence = static_cast<DxmtD3D12Fence *>(fence);
  if (!timeline_fence)
    return E_INVALIDARG;

  dxmt::TimelineFence *timeline = timeline_fence->timeline();
  dxmt::Device *dxmt_device = device_ ? device_->dxmt_device() : nullptr;
  if (!timeline || !dxmt_device)
    return E_FAIL;

  if (cpu_readback_.value || signal_worker_) {
    if (!completion_event_) completion_event_ = device_->metal_device().newSharedEvent();
    if (!completion_event_) return E_OUTOFMEMORY;
    if (!signal_worker_) signal_worker_ = std::make_unique<QueryResolveWorker>(L"dxmt-d12-signal");
    const uint64_t marker = ++completion_value_;
    dxmt_device->queue().CurrentChunk()->emitcc(
        [event = completion_event_, marker](ArgumentEncodingContext &enc) mutable {
          enc.signalEvent(WMT::Reference<WMT::Event>(event), marker);
        });
    device_->submit_recorded_work();
    // Keep signal publication ordered on this logical queue. The physical
    // GPU queue can execute independent work while CPU readback finishes.
    signal_worker_->submit([device = ComPtr<DxmtD3D12Device>(device_),
                             owner = ComPtr<DxmtD3D12Fence>(timeline_fence), value,
                             event = completion_event_, marker,
                             readback = cpu_readback_]() mutable {
      bool ready = event.waitUntilSignaledValue(marker, 60000);
      if (ready && readback.value)
        ready = readback.event.waitUntilSignaledValue(readback.value, 60000);
      if (!ready) device->fail_query_resolve();
      std::atomic_thread_fence(std::memory_order_release);
      owner->timeline()->signal(value);
    });
    return S_OK;
  }

  auto &queue = dxmt_device->queue();
  queue.CurrentChunk()->emitcc(
      [event = WMT::Reference<WMT::Event>(timeline->event()), value](ArgumentEncodingContext &enc) mutable {
        enc.signalEvent(std::move(event), value);
      }
  );
  /* A D3D12 queue signal is submitted work, not something the application has
   * to flush, so the chunk goes out now. This runs through the d3d11 immediate
   * context's own commit rather than the queue's, so its state checks still
   * apply — the chunk is shared with it. */
  device_->submit_recorded_work();
  return S_OK;
}

HRESULT DxmtD3D12CommandQueue::emit_wait(ID3D12Fence *fence, UINT64 value) {
  std::lock_guard<std::recursive_mutex> lock(device_->submission_mutex());
  static std::atomic<unsigned> traced = 0;
  if (d3d12_diagnostics() && traced.fetch_add(1) < 32)
    Logger::warn(str::format("D3D12 wait queue=", this, " type=", UINT(desc_.Type), " fence=", fence, " value=", value));
  auto *timeline_fence = static_cast<DxmtD3D12Fence *>(fence);
  if (!timeline_fence)
    return E_INVALIDARG;

  dxmt::TimelineFence *timeline = timeline_fence->timeline();
  dxmt::Device *dxmt_device = device_ ? device_->dxmt_device() : nullptr;
  if (!timeline || !dxmt_device)
    return E_FAIL;

  auto &queue = dxmt_device->queue();
  queue.CurrentChunk()->emitcc(
      [event = WMT::Reference<WMT::Event>(timeline->event()), value](ArgumentEncodingContext &enc) mutable {
        enc.waitEvent(std::move(event), value);
      }
  );
  return S_OK;
}

DxmtD3D12CommandQueue::DxmtD3D12CommandQueue(DxmtD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC &desc)
    : D3D12ChildMethods(device), desc_(desc) { device_->register_queue(this); }
DxmtD3D12CommandQueue::~DxmtD3D12CommandQueue() { device_->unregister_queue(this); }

void DxmtD3D12Device::register_queue(DxmtD3D12CommandQueue *queue) {
  std::lock_guard<std::recursive_mutex> lock(submission_mutex_);
  queues_.push_back(queue);
}
void DxmtD3D12Device::unregister_queue(DxmtD3D12CommandQueue *queue) {
  std::lock_guard<std::recursive_mutex> lock(submission_mutex_);
  queues_.erase(std::remove(queues_.begin(), queues_.end(), queue), queues_.end());
}

/* Logical queues share one encoder. A wait for a not-yet-submitted signal
 * must stay on its logical queue; putting that wait into the physical queue
 * would also block the producer's later signal. Ready queues continue to run. */
void DxmtD3D12Device::pump_queues() {
  std::lock_guard<std::recursive_mutex> lock(submission_mutex_);
  bool progressed;
  do {
    progressed = false;
    for (auto *queue : queues_) {
      while (!queue->pending_.empty()) {
        auto &front = queue->pending_.front();
        using Kind = DxmtD3D12CommandQueue::OperationKind;
        if (front.kind == Kind::Wait && !front.fence->scheduled(front.value) &&
            front.fence->GetCompletedValue() < front.value) break;
        auto operation = std::move(front);
        queue->pending_.pop_front();
        if (operation.kind == Kind::Work) operation.work();
        else if (operation.kind == Kind::Wait) queue->emit_wait(operation.fence.get(), operation.value);
        else {
          if (SUCCEEDED(queue->emit_signal(operation.fence.get(), operation.value)))
            operation.fence->mark_scheduled(operation.value);
          else fail_query_resolve();
        }
        progressed = true;
      }
    }
  } while (progressed);
}

HRESULT DxmtD3D12Fence::Signal(UINT64 value) {
  if (!timeline_.ptr()) return E_FAIL;
  value_ = value;
  timeline_->signal(value);
  if (device_) device_->pump_queues();
  return S_OK;
}
HRESULT DxmtD3D12CommandQueue::Signal(ID3D12Fence *fence, UINT64 value) {
  if (!fence) return E_INVALIDARG;
  std::lock_guard<std::recursive_mutex> lock(device_->submission_mutex());
  auto *local = static_cast<DxmtD3D12Fence *>(fence);
  if (pending_.empty()) {
    HRESULT hr = emit_signal(fence, value);
    if (FAILED(hr)) return hr;
    local->mark_scheduled(value);
  } else pending_.push_back({OperationKind::Signal, {}, local, value});
  device_->pump_queues();
  return S_OK;
}
HRESULT DxmtD3D12CommandQueue::Wait(ID3D12Fence *fence, UINT64 value) {
  if (!fence) return E_INVALIDARG;
  std::lock_guard<std::recursive_mutex> lock(device_->submission_mutex());
  auto *local = static_cast<DxmtD3D12Fence *>(fence);
  if (pending_.empty() && (local->scheduled(value) || local->GetCompletedValue() >= value))
    return emit_wait(fence, value);
  pending_.push_back({OperationKind::Wait, {}, local, value});
  device_->pump_queues();
  return S_OK;
}
void DxmtD3D12CommandQueue::submit(std::function<void()> work) {
  std::lock_guard<std::recursive_mutex> lock(device_->submission_mutex());
  if (pending_.empty()) work();
  else {
    pending_.push_back({OperationKind::Work, std::move(work), {}, 0});
    device_->pump_queues();
  }
}
void DxmtD3D12CommandQueue::ExecuteCommandLists(UINT count, ID3D12CommandList *const *lists) {
  CpuTimingScope execute_time(bridge_cpu_statistics().execute_ticks);
  std::lock_guard<std::recursive_mutex> lock(device_->submission_mutex());
  if (present_trace().enabled()) {
    bridge_cpu_statistics().execute_calls.fetch_add(1);
    bridge_cpu_statistics().command_lists.fetch_add(count);
  }
  if (!count || !lists) return;
  if (pending_.empty()) {
    LogicalQueueScope logical_queue(*device_.get(), this);
    for (UINT i = 0; i < count; ++i)
      if (auto *list = static_cast<DxmtD3D12GraphicsCommandList *>(lists[i])) list->replay_immediate();
    device_->submit_recorded_work();
  } else {
    // Retain immutable recordings for the entire API batch: the application
    // can Reset each list while its queue still waits for another producer.
    std::vector<std::function<void()>> recordings;
    recordings.reserve(count);
    for (UINT i = 0; i < count; ++i)
      if (auto *list = static_cast<DxmtD3D12GraphicsCommandList *>(lists[i]))
        recordings.push_back(list->capture_replay());
    pending_.push_back({OperationKind::Work,
        [this, recordings = std::move(recordings), device = ComPtr<DxmtD3D12Device>(device_)]() mutable {
          LogicalQueueScope logical_queue(*device.get(), this);
          for (auto &replay : recordings) replay();
          device->submit_recorded_work();
        }, {}, 0});
  }
  device_->pump_queues();
}

HRESULT DxmtD3D12SwapChainFactory::CreateSwapChain(
    IDXGIFactory1 *factory, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc, IDXGISwapChain1 **swap_chain) {
  if (!swap_chain)
    return E_POINTER;
  *swap_chain = nullptr;
  if (!desc)
    return E_INVALIDARG;
  if (desc->BufferCount < 2 || desc->BufferCount > 16 || desc->SampleDesc.Count != 1 ||
      (desc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && desc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD))
    return DXGI_ERROR_INVALID_CALL;

  /* The logical D3D12 ring owns its textures. The D3D11 presenter needs one
   * surface; Present copies the selected logical buffer into it on the GPU. */
  DXGI_SWAP_CHAIN_DESC1 presenter = *desc;
  presenter.BufferCount = 1;
  presenter.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  IDXGISwapChain1 *inner = nullptr;
  IMTLDXGIDevice *raw_dxgi_device = nullptr;
  HRESULT hr = device_->d3d11_device()->QueryInterface(__uuidof(IMTLDXGIDevice),
                                                       reinterpret_cast<void **>(&raw_dxgi_device));
  if (SUCCEEDED(hr)) {
    hr = raw_dxgi_device->CreateSwapChain(factory, hwnd, &presenter, fullscreen_desc, &inner);
    raw_dxgi_device->Release();
  } else {
    IMTLSwapChainFactory *raw_factory = nullptr;
    hr = device_->d3d11_device()->QueryInterface(__uuidof(IMTLSwapChainFactory),
                                                 reinterpret_cast<void **>(&raw_factory));
    if (SUCCEEDED(hr)) {
      hr = raw_factory->CreateSwapChain(factory, hwnd, &presenter, fullscreen_desc, &inner);
      raw_factory->Release();
    }
  }
  if (FAILED(hr))
    return hr;

  auto wrapper = new DxmtD3D12SwapChain(device_.get(), inner, *desc, queue_.get());
  inner->Release();
  hr = wrapper->initialize_buffers();
  if (FAILED(hr)) {
    wrapper->Release();
    return hr;
  }
  *swap_chain = static_cast<IDXGISwapChain1 *>(wrapper);
  return S_OK;
}

HRESULT DxmtD3D12SwapChain::GetDevice(REFIID riid, void **device) {
  return device_ ? device_->QueryInterface(riid, device) : E_NOINTERFACE;
}

HRESULT DxmtD3D12SwapChain::initialize_buffers() {
  DXGI_SWAP_CHAIN_DESC1 actual = {};
  HRESULT hr = inner_->GetDesc1(&actual);
  if (FAILED(hr)) return hr;
  desc1_.Width = actual.Width;
  desc1_.Height = actual.Height;
  desc1_.Format = actual.Format;
  std::vector<ComPtr<DxmtD3D12Resource>> buffers;
  D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                               D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc1_.Width;
  desc.Height = desc1_.Height;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.Format = desc1_.Format;
  desc.SampleDesc = desc1_.SampleDesc;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  for (UINT i = 0; i < desc1_.BufferCount; ++i) {
    ID3D12Resource *resource = nullptr;
    hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PRESENT,
                                           nullptr, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) return hr;
    buffers.emplace_back(static_cast<DxmtD3D12Resource *>(resource));
    resource->Release();
  }
  buffers_ = std::move(buffers);
  current_ = 0;
  return S_OK;
}

HRESULT DxmtD3D12SwapChain::Present1(UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS *params) {
  std::lock_guard<std::recursive_mutex> lock(device_->submission_mutex());
  if (flags & DXGI_PRESENT_TEST) return inner_->Present1(sync_interval, flags, params);
  if ((flags & DXGI_PRESENT_DO_NOT_WAIT) && queue_ && queue_->has_pending_work())
    return DXGI_ERROR_WAS_STILL_DRAWING;
  if (current_ >= buffers_.size()) return DXGI_ERROR_INVALID_CALL;
  struct PresentRequest {
    ComPtr<DxmtD3D12Device> device;
    ComPtr<IDXGISwapChain1> inner;
    ComPtr<DxmtD3D12Resource> buffer;
    std::vector<RECT> rects;
    RECT scroll_rect = {};
    POINT scroll_offset = {};
    bool has_scroll = false;
    HRESULT result = S_OK;
  };
  auto request = std::make_shared<PresentRequest>();
  request->device = device_;
  request->inner = inner_;
  request->buffer = buffers_[current_];
  if (params) {
    if (params->DirtyRectsCount && params->pDirtyRects)
      request->rects.assign(params->pDirtyRects, params->pDirtyRects + params->DirtyRectsCount);
    if (params->pScrollRect && params->pScrollOffset) {
      request->has_scroll = true;
      request->scroll_rect = *params->pScrollRect;
      request->scroll_offset = *params->pScrollOffset;
    }
  }
  auto present = [request, sync_interval, flags, identity = this]() {
    ComPtr<ID3D11Resource> target;
    auto &trace = present_trace();
    auto start = trace.begin();
    HRESULT hr = request->inner->GetBuffer(0, IID_PPV_ARGS(target.put()));
    if (SUCCEEDED(hr)) {
      auto *source = request->buffer->d3d11_resource();
      if (!source) hr = E_FAIL;
      else request->device->context()->CopyResource(target.get(), source);
    }
    if (SUCCEEDED(hr)) {
      DXGI_PRESENT_PARAMETERS parameters = {UINT(request->rects.size()), request->rects.data(),
        request->has_scroll ? &request->scroll_rect : nullptr,
        request->has_scroll ? &request->scroll_offset : nullptr};
      hr = request->inner->Present1(sync_interval, flags, &parameters);
    }
    request->result = hr;
    if (start) {
      dxmt::FrameStatistics stats;
      if (auto *device = request->device->dxmt_device()) {
        auto &queue = device->queue();
        auto frame = queue.CurrentFrameSeq();
        // CurrentFrameSeq is one-based and PresentBoundary has already
        // advanced/reset the current slot. Read the completed frame.
        if (frame >= 2) stats = queue.statistics.at(frame - 2);
      }
      using ForegroundWindow = HWND (WINAPI *)();
      static auto foreground_window = reinterpret_cast<ForegroundWindow>(
          GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow"));
      HWND game_window = nullptr;
      int foreground = -1;
      if (foreground_window && SUCCEEDED(request->inner->GetHwnd(&game_window)))
        foreground = foreground_window() == game_window;
      trace.end(identity, start, hr, stats, foreground, sync_interval);
    }
  };
  if (queue_) queue_->submit(std::move(present));
  else present();
  if (request->result == S_OK) current_ = (current_ + 1) % desc1_.BufferCount;
  return request->result;
}

HRESULT DxmtD3D12SwapChain::ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
  std::lock_guard<std::recursive_mutex> lock(device_->submission_mutex());
  if (queue_ && queue_->has_pending_work()) return DXGI_ERROR_INVALID_CALL;
  if (count && (count < 2 || count > 16)) return DXGI_ERROR_INVALID_CALL;
  HRESULT hr = inner_->ResizeBuffers(1, width, height, format, flags);
  if (FAILED(hr)) return hr;
  if (count) desc1_.BufferCount = count;
  desc1_.Flags = flags;
  return initialize_buffers();
}

HRESULT DxmtD3D12SwapChain::GetBuffer(UINT index, REFIID riid, void **surface) {
  if (!surface)
    return E_POINTER;
  *surface = nullptr;

  if (index >= buffers_.size()) return DXGI_ERROR_INVALID_CALL;
  return buffers_[index]->QueryInterface(riid, surface);
}

} // namespace
} // namespace dxmt::d3d12

namespace {

/* D3D12_ROOT_SIGNATURE_DESC into the core's own description, which is what the
 * serializer takes. Version 1.1 ranges and descriptors carry a flags word that
 * 1.0 does not; both go through here with `version` deciding the layout. */
bool
describe_root_signature(const D3D12_ROOT_PARAMETER1 *parameters, UINT parameter_count,
                        const D3D12_STATIC_SAMPLER_DESC *samplers, UINT sampler_count, UINT flags,
                        uint32_t version, bool versioned, const D3D12_ROOT_PARAMETER *parameters10,
                        dxmt::RootSignatureDescription &out) {
  out = dxmt::RootSignatureDescription{};
  out.version = version;
  out.flags = flags;

  for (UINT i = 0; i < parameter_count; i++) {
    D3D12_ROOT_PARAMETER_TYPE type =
        versioned ? parameters[i].ParameterType : parameters10[i].ParameterType;
    UINT visibility = versioned ? parameters[i].ShaderVisibility : parameters10[i].ShaderVisibility;

    dxmt::RootParameterInfo info = {};
    info.shader_visibility = visibility;

    switch (type) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
      info.kind = dxmt::RootParameterKind::DescriptorTable;
      info.first_range = uint32_t(out.ranges.size());
      UINT range_count = versioned ? parameters[i].DescriptorTable.NumDescriptorRanges
                                   : parameters10[i].DescriptorTable.NumDescriptorRanges;
      info.range_count = range_count;
      for (UINT r = 0; r < range_count; r++) {
        dxmt::RootDescriptorRange range = {};
        if (versioned) {
          const D3D12_DESCRIPTOR_RANGE1 &source = parameters[i].DescriptorTable.pDescriptorRanges[r];
          range.range_type = dxmt::DescriptorRangeType(source.RangeType);
          range.descriptor_count = source.NumDescriptors;
          range.base_shader_register = source.BaseShaderRegister;
          range.register_space = source.RegisterSpace;
          range.flags = source.Flags;
          range.offset_in_descriptors_from_table_start = source.OffsetInDescriptorsFromTableStart;
        } else {
          const D3D12_DESCRIPTOR_RANGE &source = parameters10[i].DescriptorTable.pDescriptorRanges[r];
          range.range_type = dxmt::DescriptorRangeType(source.RangeType);
          range.descriptor_count = source.NumDescriptors;
          range.base_shader_register = source.BaseShaderRegister;
          range.register_space = source.RegisterSpace;
          range.offset_in_descriptors_from_table_start = source.OffsetInDescriptorsFromTableStart;
        }
        out.ranges.push_back(range);
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
      info.kind = dxmt::RootParameterKind::Constants;
      info.shader_register =
          versioned ? parameters[i].Constants.ShaderRegister : parameters10[i].Constants.ShaderRegister;
      info.register_space =
          versioned ? parameters[i].Constants.RegisterSpace : parameters10[i].Constants.RegisterSpace;
      info.num_32bit_values =
          versioned ? parameters[i].Constants.Num32BitValues : parameters10[i].Constants.Num32BitValues;
      break;
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
      info.kind = type == D3D12_ROOT_PARAMETER_TYPE_CBV   ? dxmt::RootParameterKind::ConstantBufferView
                  : type == D3D12_ROOT_PARAMETER_TYPE_SRV ? dxmt::RootParameterKind::ShaderResourceView
                                                          : dxmt::RootParameterKind::UnorderedAccessView;
      info.shader_register =
          versioned ? parameters[i].Descriptor.ShaderRegister : parameters10[i].Descriptor.ShaderRegister;
      info.register_space =
          versioned ? parameters[i].Descriptor.RegisterSpace : parameters10[i].Descriptor.RegisterSpace;
      info.descriptor_flags = versioned ? parameters[i].Descriptor.Flags : D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
      break;
    default:
      return false;
    }
    out.parameters.push_back(info);
  }

  for (UINT i = 0; i < sampler_count; i++) {
    const D3D12_STATIC_SAMPLER_DESC &source = samplers[i];
    dxmt::StaticSamplerInfo sampler = {};
    sampler.filter = source.Filter;
    sampler.address_u = source.AddressU;
    sampler.address_v = source.AddressV;
    sampler.address_w = source.AddressW;
    sampler.mip_lod_bias = source.MipLODBias;
    sampler.max_anisotropy = source.MaxAnisotropy;
    sampler.comparison_func = source.ComparisonFunc;
    sampler.border_color = source.BorderColor;
    sampler.min_lod = source.MinLOD;
    sampler.max_lod = source.MaxLOD;
    sampler.shader_register = source.ShaderRegister;
    sampler.register_space = source.RegisterSpace;
    sampler.shader_visibility = source.ShaderVisibility;
    out.static_samplers.push_back(sampler);
  }
  return true;
}

HRESULT
serialize_into_blob(const dxmt::RootSignatureDescription &description, ID3DBlob **blob) {
  std::vector<uint8_t> bytes;
  if (!dxmt::serializeRootSignature(description, bytes))
    return E_INVALIDARG;
  *blob = new dxmt::d3d12::Blob(bytes.data(), bytes.size());
  return S_OK;
}

} // namespace

/* Produces the real serialized form -- a DXBC container with an RTS0 chunk --
 * which is what CreateRootSignature parses. It used to hand back a byte copy of
 * the descriptor struct, pointers and all, which the parser rightly refused. */
extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC *root_signature_desc, D3D_ROOT_SIGNATURE_VERSION,
    ID3DBlob **blob, ID3DBlob **error_blob) {
  if (error_blob)
    *error_blob = nullptr;
  if (!blob)
    return E_POINTER;
  *blob = nullptr;
  if (!root_signature_desc)
    return E_INVALIDARG;

  dxmt::RootSignatureDescription description;
  if (!describe_root_signature(nullptr, root_signature_desc->NumParameters,
                               root_signature_desc->pStaticSamplers,
                               root_signature_desc->NumStaticSamplers, root_signature_desc->Flags, 1,
                               false, root_signature_desc->pParameters, description))
    return E_INVALIDARG;
  return serialize_into_blob(description, blob);
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature_desc, ID3DBlob **blob,
    ID3DBlob **error_blob) {
  if (error_blob)
    *error_blob = nullptr;
  if (!blob)
    return E_POINTER;
  *blob = nullptr;
  if (!root_signature_desc)
    return E_INVALIDARG;

  dxmt::RootSignatureDescription description;
  if (root_signature_desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
    const D3D12_ROOT_SIGNATURE_DESC1 &desc = root_signature_desc->Desc_1_1;
    if (!describe_root_signature(desc.pParameters, desc.NumParameters, desc.pStaticSamplers,
                                 desc.NumStaticSamplers, desc.Flags, 2, true, nullptr, description))
      return E_INVALIDARG;
  } else {
    const D3D12_ROOT_SIGNATURE_DESC &desc = root_signature_desc->Desc_1_0;
    if (!describe_root_signature(nullptr, desc.NumParameters, desc.pStaticSamplers,
                                 desc.NumStaticSamplers, desc.Flags, 1, false, desc.pParameters,
                                 description))
      return E_INVALIDARG;
  }
  return serialize_into_blob(description, blob);
}

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID, void **debug) {
  if (debug)
    *debug = nullptr;
  return E_NOINTERFACE;
}

/* None of the classes D3D12GetInterface hands out exist here: no debug layer, as
 * D3D12GetDebugInterface says, and none of the Agility SDK's runtime objects. */
extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID, REFIID, void **object) {
  if (object)
    *object = nullptr;
  return E_NOINTERFACE;
}

/* Experimental features need Windows' developer mode; without it this is the answer. */
extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT, const IID *, void *, UINT *) {
  return E_NOINTERFACE;
}

/* Exported so that a game importing them loads; a game calling them is told no. */
static HRESULT create_root_signature_reader(const void *blob, SIZE_T size, REFIID iid, void **deserializer) {
  if (!deserializer) return E_POINTER;
  *deserializer = nullptr;
  auto *reader = new dxmt::d3d12::RootSignatureDeserializer;
  if (!reader->init(blob, size)) {
    reader->Release();
    return E_INVALIDARG;
  }
  HRESULT hr = reader->QueryInterface(iid, deserializer);
  reader->Release();
  return hr;
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void *blob, SIZE_T size, REFIID iid,
                                                               void **deserializer) {
  return create_root_signature_reader(blob, size, iid, deserializer);
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(const void *blob, SIZE_T size, REFIID iid,
                                                                        void **deserializer) {
  return create_root_signature_reader(blob, size, iid, deserializer);
}

extern "C" HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter,
                                            D3D_FEATURE_LEVEL minimum_feature_level,
                                            REFIID iid, void **device) {
  if (minimum_feature_level > D3D_FEATURE_LEVEL_11_1)
    return E_INVALIDARG;
  if (device)
    *device = nullptr;

  IDXGIAdapter *dxgi_adapter = nullptr;
  if (adapter) {
    HRESULT hr = adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter));
    if (FAILED(hr))
      return hr;
  }

  D3D_FEATURE_LEVEL requested[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  ID3D11Device *d3d11_device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  D3D_FEATURE_LEVEL chosen = {};
  HRESULT hr = D3D11CreateDevice(dxgi_adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN
                                                       : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, 0, requested, 2, D3D11_SDK_VERSION,
                                 &d3d11_device, &chosen, &context);
  if (dxgi_adapter)
    dxgi_adapter->Release();
  if (FAILED(hr))
    return hr;
  /* A probe for support: D3D12 answers S_FALSE when it could have created the
   * device. */
  if (!device) {
    if (context)
      context->Release();
    if (d3d11_device)
      d3d11_device->Release();
    return S_FALSE;
  }

  auto wrapper = new dxmt::d3d12::DxmtD3D12Device(d3d11_device, context);
  if (context)
    context->Release();
  if (d3d11_device)
    d3d11_device->Release();
  hr = wrapper->initialization_status();
  if (SUCCEEDED(hr))
    hr = wrapper->QueryInterface(iid, device);
  wrapper->Release();
  return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    DisableThreadLibraryCalls(instance);
  return TRUE;
}
