/* D3D12 API contracts and real placed-buffer GPU copies. Not a game benchmark. */
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <fstream>
#include <iterator>
#include <string>

static int failures;
static ID3D12Device *device;
static UINT64 placed_test_offset;
static bool retire_placed_alias;
static bool retire_newer_alias;

static void expect(const char *name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}

template<class T> static void drop(T *&object) {
  if (object) object->Release();
  object = nullptr;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return desc;
}

static UINT64 unavailable_shared_event(UINT64) { return 0; }

static void fence_allocation_failure() {
  // Replace only this DLL's allocation import, then restore it immediately.
  // The real device, queues, and GPU work continue to use their normal events.
  auto *image = reinterpret_cast<unsigned char *>(GetModuleHandleA("d3d12.dll"));
  auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(image);
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(image + dos->e_lfanew);
  auto *imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(image +
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  ULONG_PTR *slot = nullptr;
  for (auto *entry = imports; entry->Name && !slot; ++entry) {
    if (!entry->OriginalFirstThunk) continue;
    auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(image + entry->OriginalFirstThunk);
    auto *addresses = reinterpret_cast<IMAGE_THUNK_DATA *>(image + entry->FirstThunk);
    for (UINT i = 0; names[i].u1.AddressOfData; ++i) {
      if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
      auto *name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(image + names[i].u1.AddressOfData);
      if (!std::strcmp(reinterpret_cast<char *>(name->Name), "MTLDevice_newSharedEvent")) {
        slot = reinterpret_cast<ULONG_PTR *>(&addresses[i].u1.Function);
        break;
      }
    }
  }
  expect("native shared-event allocator import located", slot != nullptr);
  if (!slot) return;
  DWORD protection = 0;
  bool writable = VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection) != FALSE;
  expect("test allocator import can be replaced", writable);
  if (!writable) return;
  ULONG_PTR original = *slot;
  *slot = reinterpret_cast<ULONG_PTR>(&unavailable_shared_event);
  ID3D12Fence *fence = reinterpret_cast<ID3D12Fence *>(ULONG_PTR(1));
  HRESULT hr = device->CreateFence(17, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  *slot = original;
  DWORD ignored = 0;
  VirtualProtect(slot, sizeof(*slot), protection, &ignored);
  std::printf("INFO failed event allocation: HRESULT %08lx fence %p\n", (unsigned long)hr, fence);
  expect("event allocation failure returns E_OUTOFMEMORY and clears the fence", hr == E_OUTOFMEMORY && !fence);
  if (SUCCEEDED(hr)) drop(fence);
  fence = nullptr;
  hr = device->CreateFence(17, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  expect("restored event allocator creates a real timeline with the initial value",
         SUCCEEDED(hr) && fence && fence->GetCompletedValue() == 17);
  drop(fence);
}

static void footprints() {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = 8;
  desc.DepthOrArraySize = desc.SampleDesc.Count = 1;
  desc.MipLevels = 3;
  desc.Format = DXGI_FORMAT_BC1_UNORM;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout[3] = {};
  UINT rows[3] = {};
  UINT64 bytes[3] = {}, total = 0;
  device->GetCopyableFootprints(&desc, 0, 3, 0, layout, rows, bytes, &total);
  expect("BC1 block rows and bytes", rows[0] == 2 && rows[1] == 1 && rows[2] == 1 &&
         bytes[0] == 16 && bytes[1] == 8 && bytes[2] == 8);
  expect("BC1 small mip footprints are block aligned", layout[1].Footprint.Width == 4 &&
         layout[2].Footprint.Width == 4 && layout[2].Footprint.Height == 4);
  expect("BC1 mip offsets and exact upload span", layout[0].Offset == 0 &&
         layout[1].Offset == 512 && layout[2].Offset == 1024 && total == 1032);

  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.MipLevels = 1;
  device->GetCopyableFootprints(&desc, 0, 1, 4096, layout, rows, bytes, &total);
  expect("RGBA16F uses eight bytes per texel", bytes[0] == 64 && rows[0] == 8 &&
         layout[0].Offset == 4096 && total == 1856);
  desc.Format = DXGI_FORMAT_R8_UNORM;
  device->GetCopyableFootprints(&desc, 0, 1, 0, layout, rows, bytes, &total);
  expect("R8 uses one byte per texel and no trailing row padding", bytes[0] == 8 && total == 1800);
  total = 0;
  device->GetCopyableFootprints(&desc, 1, 1, 0, nullptr, nullptr, nullptr, &total);
  expect("invalid subresource range reports UINT64_MAX", total == UINT64_MAX);

  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  desc.Width = 7; desc.Height = 5; desc.DepthOrArraySize = 3;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  device->GetCopyableFootprints(&desc, 0, 1, 0, layout, rows, bytes, &total);
  expect("3D upload span includes depth slices", rows[0] == 5 && bytes[0] == 28 && total == 3612);

  D3D12_RESOURCE_DESC pair[] = {buffer_desc(65536), buffer_desc(65536)};
  auto single = device->GetResourceAllocationInfo(0, 1, pair);
  auto group = device->GetResourceAllocationInfo(0, 2, pair);
  expect("allocation info reserves both nonoverlapping resources", single.SizeInBytes != UINT64_MAX &&
         group.SizeInBytes >= 2 * single.SizeInBytes && group.Alignment >= single.Alignment);
}

static void signatures() {
  D3D12_DESCRIPTOR_RANGE1 range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 3, 2,
      D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  D3D12_ROOT_PARAMETER1 params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[1].Descriptor = {5, 1, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants = {9, 4, 6};
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 2;
  sampler.RegisterSpace = 1;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1 = {3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  ID3DBlob *blob = nullptr;
  HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, nullptr);
  expect("serialize version 1.1 root signature", SUCCEEDED(hr) && blob);
  if (!blob) return;
  ID3D12RootSignature *root = nullptr;
  hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
  expect("create serialized root signature", SUCCEEDED(hr) && root);
  drop(root);

  ID3D12VersionedRootSignatureDeserializer *reader = nullptr;
  hr = D3D12CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&reader));
  expect("create versioned root signature deserializer", SUCCEEDED(hr) && reader);
  if (reader) {
    const auto *out = reader->GetUnconvertedRootSignatureDesc();
    bool ok = out && out->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 &&
              out->Desc_1_1.NumParameters == 3 && out->Desc_1_1.NumStaticSamplers == 1;
    if (ok) {
      const auto &p = out->Desc_1_1.pParameters[0];
      const auto &r = p.DescriptorTable.pDescriptorRanges[0];
      ok = p.ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL && r.NumDescriptors == 7 &&
           r.BaseShaderRegister == 3 && r.RegisterSpace == 2 && r.Flags == range.Flags &&
           out->Desc_1_1.pParameters[1].Descriptor.Flags == params[1].Descriptor.Flags &&
           out->Desc_1_1.pParameters[2].Constants.Num32BitValues == 6 &&
           out->Desc_1_1.pStaticSamplers[0].RegisterSpace == 1;
    }
    expect("root signature round trip preserves ranges, flags, constants, sampler", ok);
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *v10 = nullptr;
    hr = reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0, &v10);
    expect("root signature converts 1.1 to 1.0", SUCCEEDED(hr) && v10 &&
           v10->Version == D3D_ROOT_SIGNATURE_VERSION_1_0 && v10->Desc_1_0.NumParameters == 3 &&
           v10->Desc_1_0.pParameters[1].Descriptor.ShaderRegister == 5);
    drop(reader);
  }
  ID3D12RootSignatureDeserializer *legacy = nullptr;
  hr = D3D12CreateRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&legacy));
  expect("legacy root signature reader accepts version 1.1", SUCCEEDED(hr) && legacy &&
         legacy->GetRootSignatureDesc()->NumParameters == 3);
  drop(legacy);
  unsigned char invalid[16] = {};
  reader = nullptr;
  hr = D3D12CreateVersionedRootSignatureDeserializer(invalid, sizeof(invalid), IID_PPV_ARGS(&reader));
  expect("malformed root signature rejected and output cleared", FAILED(hr) && !reader);
  drop(blob);
}

static void queue_copy(D3D12_COMMAND_LIST_TYPE type, const char *name) {
  D3D12_COMMAND_QUEUE_DESC desc = {};
  desc.Type = type;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
  std::printf("INFO %s queue HRESULT %08lx\n", name, (unsigned long)hr);
  expect(name, SUCCEEDED(hr) && queue);
  if (!queue) return;
  hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, type, allocator, nullptr, IID_PPV_ARGS(&list));
  expect("queue allocator and matching command list", SUCCEEDED(hr) && list && list->GetType() == type);
  if (!list) { drop(allocator); drop(queue); return; }

  ID3D12Heap *upload_heap = nullptr, *readback_heap = nullptr;
  ID3D12Resource *upload = nullptr, *readback = nullptr;
  ID3D12Fence *fence = nullptr;
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  D3D12_HEAP_DESC heap = {};
  heap.SizeInBytes = 65536 + placed_test_offset;
  heap.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
  heap.Properties.CreationNodeMask = heap.Properties.VisibleNodeMask = 1;
  heap.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  hr = device->CreateHeap(&heap, IID_PPV_ARGS(&upload_heap));
  heap.Properties.Type = D3D12_HEAP_TYPE_READBACK;
  if (SUCCEEDED(hr)) hr = device->CreateHeap(&heap, IID_PPV_ARGS(&readback_heap));
  auto b = buffer_desc(4096);
  if (SUCCEEDED(hr)) hr = device->CreatePlacedResource(upload_heap, placed_test_offset, &b, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&upload));
  if (SUCCEEDED(hr)) hr = device->CreatePlacedResource(readback_heap, 0, &b, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, IID_PPV_ARGS(&readback));
  void *source = nullptr;
  if (SUCCEEDED(hr)) hr = upload->Map(0, nullptr, &source);
  if (SUCCEEDED(hr) && source) {
    for (unsigned i = 0; i < 1024; ++i) static_cast<uint32_t *>(source)[i] = 0x9e3779b9u * (i + 1);
    upload->Unmap(0, nullptr);
    list->CopyBufferRegion(readback, 0, upload, 0, 4096);
    hr = list->Close();
    if (SUCCEEDED(hr)) {
      ID3D12CommandList *lists[] = {list};
      queue->ExecuteCommandLists(1, lists);
      hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
      if (SUCCEEDED(hr)) hr = queue->Signal(fence, 1);
      if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, event);
      if (SUCCEEDED(hr) && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) hr = E_FAIL;
    }
  }
  bool copied = SUCCEEDED(hr) && source;
  void *result = nullptr;
  if (copied) copied = SUCCEEDED(readback->Map(0, nullptr, &result)) && result;
  if (copied) {
    for (unsigned i = 0; i < 1024; ++i)
      if (static_cast<uint32_t *>(result)[i] != 0x9e3779b9u * (i + 1)) {
        std::printf("INFO queue copy mismatch index %u actual %08x source %08x expected %08x\n", i,
                    static_cast<uint32_t *>(result)[i], static_cast<uint32_t *>(source)[i], 0x9e3779b9u * (i + 1));
        copied = false; break;
      }
    readback->Unmap(0, nullptr);
  }
  expect("placed-buffer GPU copy and fence completion", copied);
  CloseHandle(event);
  drop(fence); drop(readback); drop(upload); drop(readback_heap); drop(upload_heap);
  drop(list); drop(allocator); drop(queue);
}

static ID3D12Resource *placed(const D3D12_RESOURCE_DESC &desc, D3D12_HEAP_TYPE type,
                              D3D12_RESOURCE_STATES state) {
  auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = allocation.SizeInBytes + placed_test_offset;
  heap_desc.Properties.Type = type;
  heap_desc.Properties.CreationNodeMask = heap_desc.Properties.VisibleNodeMask = 1;
  ID3D12Heap *heap = nullptr;
  ID3D12Resource *resource = nullptr;
  HRESULT hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
  if (SUCCEEDED(hr)) hr = device->CreatePlacedResource(heap, placed_test_offset, &desc, state, nullptr, IID_PPV_ARGS(&resource));
  if (SUCCEEDED(hr) && retire_placed_alias && desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    ID3D12Resource *second = nullptr;
    hr = device->CreatePlacedResource(heap, placed_test_offset, &desc, state, nullptr, IID_PPV_ARGS(&second));
    expect("second placed buffer allocation for VA lifetime test", SUCCEEDED(hr) && second);
    if (SUCCEEDED(hr) && second) {
      std::printf("INFO alias GPU VAs %llx %llx; retiring %s resource\n",
                  (unsigned long long)resource->GetGPUVirtualAddress(),
                  (unsigned long long)second->GetGPUVirtualAddress(), retire_newer_alias ? "newer" : "older");
      expect("placed aliases reuse the GPU VA for this lifetime regression",
             resource->GetGPUVirtualAddress() == second->GetGPUVirtualAddress());
      if (retire_newer_alias) drop(second);
      else { drop(resource); resource = second; }
    }
  }
  drop(heap);
  if (FAILED(hr)) std::printf("INFO placed resource failed %08lx\n", (unsigned long)hr);
  return resource;
}

static ID3D12Resource *committed(const D3D12_RESOURCE_DESC &desc, D3D12_HEAP_TYPE type,
                                 D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = type;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  ID3D12Resource *resource = nullptr;
  HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                IID_PPV_ARGS(&resource));
  if (FAILED(hr)) std::printf("INFO committed resource failed %08lx\n", (unsigned long)hr);
  return resource;
}

static bool finish(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list) {
  if (FAILED(list->Close())) return false;
  ID3D12CommandList *lists[] = {list};
  LARGE_INTEGER submit_start, submit_end, submit_frequency;
  QueryPerformanceFrequency(&submit_frequency);
  QueryPerformanceCounter(&submit_start);
  queue->ExecuteCommandLists(1, lists);
  QueryPerformanceCounter(&submit_end);
  std::printf("TIMING command submission: %.6f ms CPU (not game FPS)\n",
              double(submit_end.QuadPart-submit_start.QuadPart)*1000/submit_frequency.QuadPart);
  ID3D12Fence *fence = nullptr;
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (SUCCEEDED(hr)) hr = queue->Signal(fence, 1);
  if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, event);
  bool done = SUCCEEDED(hr) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
  LARGE_INTEGER complete; QueryPerformanceCounter(&complete);
  std::printf("TIMING submitted work and GPU fence completion: %.6f ms (not game FPS)\n",
              double(complete.QuadPart-submit_start.QuadPart)*1000/submit_frequency.QuadPart);
  CloseHandle(event); drop(fence);
  return done;
}

static std::vector<char> shader(const char *name) {
  std::ifstream file(name, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static void native_render(bool draw, const char *mode) {
  auto vs = shader("vs.dxil"), ps = shader("ps.dxil");
  bool single_draw = std::strstr(mode, "single") != nullptr;
  const UINT cbv_slot = std::strstr(mode, "slot13") ? 13 : 0;
  std::vector<char> gs;
  bool geometry_test = std::strstr(mode, "geometry") != nullptr;
  bool skin_cache = std::strstr(mode, "skin-cache") != nullptr;
  bool typed_vertex = skin_cache && std::strstr(mode, "typed") != nullptr;
  bool vertex_resource = std::strstr(mode, "vertex-resource") != nullptr;
  bool high_table = std::strstr(mode, "table128") != nullptr;
  bool wide_table = high_table || std::strstr(mode, "table64") != nullptr;
  const UINT table_size = high_table ? 128 : 64;
  bool stencil_test = std::strstr(mode, "stencil") != nullptr;
  bool depth_test = std::strstr(mode, "depth") != nullptr;
  bool wide_depth = std::strstr(mode, "depth32s8") != nullptr;
  bool prepass_test = std::strstr(mode, "prepass") != nullptr;
  bool depth_copy_test = std::strstr(mode, "depthcopy") != nullptr;
  bool clear_before_bind = std::strstr(mode, "unbound-clear") != nullptr;
  bool mrt_test = std::strstr(mode, "mrt") != nullptr;
  const UINT target_count = mrt_test ? 5 : 1;
  bool indexed_test = std::strstr(mode, "indexed") != nullptr;
  bool occlusion_test = std::strstr(mode, "occlusion") != nullptr;
  bool query_reuse = occlusion_test && std::strstr(mode, "reuse") != nullptr;
  bool binary_occlusion = std::strstr(mode, "binary-occlusion") != nullptr;
  bool scissor_test = std::strstr(mode, "scissor") != nullptr;
  bool stream_test = std::strstr(mode, "streams") != nullptr;
  bool structured_test = std::strstr(mode, "structured") != nullptr;
  bool cube_test = std::strstr(mode, "cube") != nullptr;
  bool bc1_test = std::strstr(mode, "bc1") != nullptr;
  bool texture_test = std::strstr(mode, "texture") != nullptr || cube_test;
  bool native_texture = std::strstr(mode, "texture-sm6") != nullptr;
  bool volatile_texture = std::strstr(mode, "volatile") != nullptr;
  bool indirect = std::strstr(mode, "indirect") != nullptr;
  bool indirect_count = std::strstr(mode, "indirect-count") != nullptr;
  bool indirect_empty = std::strstr(mode, "indirect-empty") != nullptr;
  bool dxbc = stencil_test || stream_test || structured_test || (texture_test && !native_texture) || std::strstr(mode, "dxbc") != nullptr;
  if (native_texture) ps = shader("ps-texture.dxil");
  if (dxbc) {
    auto text = shader("validation.hlsl");
    if (cbv_slot) {
      std::string source(text.begin(), text.end());
      auto slot = source.find("register(b0)");
      if (slot != std::string::npos) source.replace(slot, 12, "register(b13)");
      text.assign(source.begin(), source.end());
    }
    if (single_draw) {
      char extra[256];
      std::snprintf(extra, sizeof(extra), "\nfloat4 VSSingle(uint id : SV_VertexID) : SV_Position { float4 p=VSMain(id); p.x+=0.00000001*%u; return p; }\n", unsigned(GetTickCount64() & 65535) + 1);
      text.insert(text.end(), extra, extra + std::strlen(extra));
    }
    if (depth_test) {
      const char extra[] = "\nfloat4 VSDepth(uint id : SV_VertexID) : SV_Position { float4 p=VSMain(id); p.z=0.5; return p; }\n";
      text.insert(text.end(), extra, extra + sizeof(extra) - 1);
    }
    if (depth_copy_test) {
      const char extra[] = "\nTexture2D<float> copiedDepth : register(t0);\nfloat4 PSDepthCopy() : SV_Target { return copiedDepth.Load(int3(8,8,0))>0.4 ? tint : float4(0,1,0,1); }\n";
      text.insert(text.end(), extra, extra + sizeof(extra) - 1);
    }
    if (wide_table) {
      const char extra[] = "\nTexture2D<float4> lastTexture : register(t63); SamplerState lastSampler : register(s15);\nfloat4 PSWideTable() : SV_Target { return (inputTexture.Load(int3(0,0,0)) + lastTexture.SampleLevel(lastSampler,float2(0.5,0.5),0))*0.5; }\n";
      std::string declaration(extra);
      if (high_table) declaration.replace(declaration.find("t63"), 3, "t127");
      text.insert(text.end(), declaration.begin(), declaration.end());
    }
    if (mrt_test) {
      const char extra[] = "\nstruct GBuffer { float4 a:SV_Target0; float4 b:SV_Target1; float4 c:SV_Target2; float4 d:SV_Target3; float4 e:SV_Target4; };\nGBuffer PSMRT() { GBuffer o; o.a=tint; o.b=float4(0,1,0,1); o.c=float4(0,0,1,1); o.d=float4(1,1,0,1); o.e=float4(1,0,1,1); return o; }\n";
      text.insert(text.end(), extra, extra + sizeof(extra) - 1);
    }
    if (geometry_test) {
      const char extra[] = "\ncbuffer GeometryConstants : register(b1) { float4 geometryOffset; };\nstruct GeometryVertex { float4 p:SV_Position; };\nGeometryVertex VSGeometry(uint id:SV_VertexID) { GeometryVertex o; o.p=VSMain(id); o.p.x+=4; return o; }\n[maxvertexcount(3)] void GSMain(triangle GeometryVertex input[3], inout TriangleStream<GeometryVertex> outputStream) { for(uint i=0;i<3;++i) { GeometryVertex o; o.p=float4(input[i].p.x-geometryOffset.x,input[i].p.yzw); outputStream.Append(o); } }\n";
      text.insert(text.end(), extra, extra + sizeof(extra) - 1);
    }
    if (geometry_test && std::strstr(mode, "geometry-static")) {
      std::string source(text.begin(), text.end());
      auto offset = source.find("input[i].p.x-geometryOffset.x");
      if (offset != std::string::npos) source.replace(offset, std::strlen("input[i].p.x-geometryOffset.x"), "input[i].p.x-4");
      text.assign(source.begin(), source.end());
    }
    if (cube_test) {
      const char extra[] = "\nTextureCube<float4> inputCube : register(t0); SamplerState cubeSampler : register(s0);\nfloat4 PSCube() : SV_Target { return inputCube.SampleLevel(cubeSampler, float3(1,0,0), 0); }\n";
      text.insert(text.end(), extra, extra + sizeof(extra) - 1);
    }
    if (structured_test) {
      const char *extra = typed_vertex ?
          "\nBuffer<float4> inputData : register(t0);\nfloat4 PSStructured() : SV_Target { return inputData[1]; }\n" :
          "\nStructuredBuffer<float4> inputData : register(t0);\nfloat4 PSStructured() : SV_Target { return inputData[1]; }\n";
      text.insert(text.end(), extra, extra + std::strlen(extra));
    }
    if (vertex_resource) {
      const char extra[] = "\nfloat4 VSResource(uint id : SV_VertexID) : SV_Position { float4 p=VSMain(id); p.x+=4-inputData[1].r*4; return p; }\n";
      text.insert(text.end(), extra, extra + sizeof(extra) - 1);
    }
    if (stream_test) {
      const char extra[] = "\nstruct StreamOutput { float4 p : SV_Position; float4 c : COLOR0; };\nStreamOutput VSStream(float2 p : POSITION, float4 c : COLOR0) { StreamOutput o; o.p=float4(p,0,1); o.c=c; return o; }\nfloat4 PSStream(StreamOutput i) : SV_Target { return i.c; }\n";
      text.insert(text.end(), extra, extra + sizeof(extra) - 1);
    }
    for (bool vertex : {true, false}) {
      ID3DBlob *code = nullptr, *errors = nullptr;
      HRESULT hr = D3DCompile(text.data(), text.size(), "validation.hlsl", nullptr, nullptr,
                              vertex ? single_draw ? "VSSingle" : vertex_resource ? "VSResource" : geometry_test ? "VSGeometry" : depth_test ? "VSDepth" : stream_test ? "VSStream" : "VSMain" : vertex_resource ? "PSMain" : wide_table ? "PSWideTable" : depth_copy_test ? "PSDepthCopy" : mrt_test ? "PSMRT" : cube_test ? "PSCube" : stream_test ? "PSStream" : structured_test ? "PSStructured" : texture_test ? "PSTexture" : "PSMain", vertex ? "vs_5_0" : "ps_5_0", 0, 0, &code, &errors);
      if (errors) { std::printf("%s\n", static_cast<char *>(errors->GetBufferPointer())); drop(errors); }
      if (FAILED(hr) || !code) { expect("DXBC validation shader compilation", false); return; }
      auto &bytes = vertex ? vs : ps;
      bytes.assign(static_cast<char *>(code->GetBufferPointer()),
                    static_cast<char *>(code->GetBufferPointer()) + code->GetBufferSize());
      drop(code);
    }
    if (geometry_test) {
      ID3DBlob *code = nullptr, *errors = nullptr;
      HRESULT hr = D3DCompile(text.data(), text.size(), "geometry.hlsl", nullptr, nullptr,
                              "GSMain", "gs_5_0", 0, 0, &code, &errors);
      if (errors) { std::printf("%s\n", static_cast<char *>(errors->GetBufferPointer())); drop(errors); }
      expect("DXBC geometry shader compilation", SUCCEEDED(hr) && code);
      if (!code) return;
      ID3DBlob *assembly = nullptr;
      if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &assembly))) {
        std::printf("INFO geometry bytecode\n%.*s\n", int(assembly->GetBufferSize()), static_cast<char *>(assembly->GetBufferPointer()));
        drop(assembly);
      }
      gs.assign(static_cast<char *>(code->GetBufferPointer()), static_cast<char *>(code->GetBufferPointer()) + code->GetBufferSize());
      drop(code);
    }
  }
  if (vs.empty() || ps.empty()) { expect("SM6 shader fixtures available", false); return; }
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                                  IID_PPV_ARGS(&list));
  if (FAILED(hr)) { expect("native rendering command setup", false); return; }
  D3D12_RESOURCE_DESC texture_desc = {};
  texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture_desc.Width = texture_desc.Height = 16;
  texture_desc.DepthOrArraySize = texture_desc.MipLevels = texture_desc.SampleDesc.Count = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  auto create = dxbc && !std::strstr(mode, "placed") ? committed : placed;
  ID3D12Resource *target = create(texture_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  std::vector<ID3D12Resource *> targets(target_count, nullptr);
  targets[0] = target;
  for (UINT i = 1; i < target_count; ++i)
    targets[i] = create(texture_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  bool copy_cbv = std::strstr(mode, "copy-cbv") != nullptr;
  bool large_cbv = std::strstr(mode, "cbv-large") != nullptr;
  UINT cbv_offset = large_cbv ? 262144 : copy_cbv ? 256 : 0;
  auto upload_desc = buffer_desc(large_cbv ? cbv_offset + 256 : texture_test ? 2048 : copy_cbv || geometry_test ? 512 : 256), readback_desc = buffer_desc(4096 * target_count);
  ID3D12Resource *upload = create(upload_desc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *readback = create(readback_desc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!target || !upload || !readback) { expect("native rendering resources", false); return; }
  void *mapped = nullptr;
  hr = upload->Map(0, nullptr, &mapped);
  expect("upload buffer maps real CPU-visible storage", SUCCEEDED(hr) && mapped);
  if (FAILED(hr) || !mapped) return;
  if (SUCCEEDED(hr)) {
    const float red[] = {1, 0, 0, 1};
    std::memcpy(mapped, red, sizeof(red));
    if (geometry_test) {
      const float offset[] = {4,0,0,0};
      std::memcpy(static_cast<char *>(mapped) + 256, offset, sizeof(offset));
    }
    if (copy_cbv || large_cbv) {
      const float blue[] = {0, 0, 1, 1};
      std::memcpy(mapped, blue, sizeof(blue));
      std::memcpy(static_cast<char *>(mapped) + cbv_offset, red, sizeof(red));
    }
    if (indexed_test) {
      const uint16_t indices[] = {42,0,1,2};
      std::memcpy(static_cast<char *>(mapped) + 192, indices, sizeof(indices));
    }
    if (stream_test) {
      const float positions[] = {-1,-1, -1,3, 3,-1};
      const float colors[] = {1,0,0,1, 1,0,0,1, 1,0,0,1};
      std::memcpy(static_cast<char *>(mapped) + 64, positions, sizeof(positions));
      std::memcpy(static_cast<char *>(mapped) + 128, colors, sizeof(colors));
    }
    if (structured_test) {
      const float blue[] = {0, 0, 1, 1};
      std::memcpy(mapped, blue, sizeof(blue));
      std::memcpy(static_cast<char *>(mapped) + 16, red, sizeof(red));
      if (vertex_resource) std::memcpy(mapped, red, sizeof(red));
    }
    if (texture_test) {
      std::memset(mapped, 0, 2048);
      for (UINT y = 0; y < 4; ++y)
        for (UINT x = 0; x < 4; ++x)
          reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) + y * 256)[x] = 0xffff0000;
      for (UINT y = 0; y < 2; ++y)
        for (UINT x = 0; x < 2; ++x)
          reinterpret_cast<uint32_t *>(static_cast<char *>(mapped) + 1024 + y * 256)[x] = 0xff0000ff;
      if (bc1_test) {
        std::memset(mapped, 0, 2048);
        const uint16_t blue[] = {0x001f, 0x001f}, red_block[] = {0xf800, 0xf800};
        std::memcpy(mapped, blue, sizeof(blue));
        std::memcpy(static_cast<char *>(mapped) + 1024, red_block, sizeof(red_block));
      }
    }
    upload->Unmap(0, nullptr);
  }

  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0,
                                 D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  range.BaseShaderRegister = cbv_slot;
  if (texture_test || structured_test) range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  if (wide_table) range.NumDescriptors = table_size;
  D3D12_ROOT_PARAMETER parameter = {};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable = {1, &range};
  if (vertex_resource) parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  bool root_cbv = std::strstr(mode, "root-cbv") != nullptr;
  bool root_constants = std::strstr(mode, "root-constants") != nullptr;
  if (root_cbv) {
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor = {cbv_slot, 0};
  } else if (root_constants) {
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants = {cbv_slot, 0, 4};
  }
  D3D12_ROOT_SIGNATURE_DESC root_desc = {1, &parameter, 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  D3D12_ROOT_PARAMETER geometry_parameters[2] = {parameter, {}};
  D3D12_DESCRIPTOR_RANGE depth_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  D3D12_DESCRIPTOR_RANGE sampler_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  if (vertex_resource) {
    geometry_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    geometry_parameters[1].Descriptor = {0, 0};
    geometry_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_desc.NumParameters = 2;
    root_desc.pParameters = geometry_parameters;
  } else if (geometry_test) {
    geometry_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    geometry_parameters[1].Descriptor = {1, 0};
    geometry_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    root_desc.NumParameters = 2;
    root_desc.pParameters = geometry_parameters;
  } else if (wide_table) {
    geometry_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    geometry_parameters[1].DescriptorTable = {1, &sampler_range};
    geometry_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_desc.NumParameters = 2;
    root_desc.pParameters = geometry_parameters;
  } else if (depth_copy_test) {
    geometry_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    geometry_parameters[1].DescriptorTable = {1, &depth_range};
    geometry_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_desc.NumParameters = 2;
    root_desc.pParameters = geometry_parameters;
  }
  D3D12_STATIC_SAMPLER_DESC cube_sampler = {};
  cube_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  cube_sampler.AddressU = cube_sampler.AddressV = cube_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  cube_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  cube_sampler.MaxLOD = D3D12_FLOAT32_MAX;
  if (cube_test) { root_desc.NumStaticSamplers = 1; root_desc.pStaticSamplers = &cube_sampler; }
  ID3DBlob *blob = nullptr;
  ID3D12RootSignature *root = nullptr;
  hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr);
  if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&root));
  drop(blob);
  if (FAILED(hr)) { expect("native rendering root signature", false); return; }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc = {};
  D3D12_INPUT_ELEMENT_DESC stream_layout[] = {
      {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,2,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
      {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,7,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
  if (stream_test) pipeline_desc.InputLayout = {stream_layout, 2};
  pipeline_desc.pRootSignature = root;
  pipeline_desc.VS = {vs.data(), vs.size()};
  pipeline_desc.PS = {ps.data(), ps.size()};
  if (geometry_test) pipeline_desc.GS = {gs.data(), gs.size()};
  pipeline_desc.SampleMask = UINT_MAX;
  pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
  pipeline_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pipeline_desc.DepthStencilState.StencilReadMask = pipeline_desc.DepthStencilState.StencilWriteMask = 255;
  pipeline_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  pipeline_desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
  pipeline_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
  pipeline_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  if (stencil_test) {
    pipeline_desc.DepthStencilState.StencilEnable = TRUE;
    pipeline_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    pipeline_desc.DepthStencilState.StencilWriteMask = 0;
    pipeline_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
  }
  if (depth_test) {
    pipeline_desc.DepthStencilState.DepthEnable = TRUE;
    pipeline_desc.DepthStencilState.DepthWriteMask = prepass_test ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline_desc.DepthStencilState.DepthFunc = prepass_test ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pipeline_desc.DSVFormat = wide_depth ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
  }
  pipeline_desc.DepthStencilState.BackFace = pipeline_desc.DepthStencilState.FrontFace;
  auto &blend = pipeline_desc.BlendState.RenderTarget[0];
  blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
  blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline_desc.NumRenderTargets = target_count;
  for (UINT i = 0; i < target_count; ++i) pipeline_desc.RTVFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pipeline_desc.SampleDesc.Count = 1;
  ID3D12PipelineState *pipeline = nullptr;
  hr = device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline));
  std::printf("INFO %s graphics pipeline HRESULT %08lx\n", dxbc ? "DXBC" : "SM6", (unsigned long)hr);
  expect("graphics pipeline creation", SUCCEEDED(hr) && pipeline);
  if (!pipeline) return;
  ID3D12PipelineState *prepass = nullptr;
  if (prepass_test) {
    auto desc = pipeline_desc;
    desc.PS = {};
    desc.NumRenderTargets = 0;
    for (auto &format : desc.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&prepass));
    expect("depth-only prepass pipeline creation", SUCCEEDED(hr) && prepass);
    if (!prepass) return;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.NumDescriptors = target_count;
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  ID3D12DescriptorHeap *rtv_heap = nullptr, *resource_heap = nullptr, *sampler_heap = nullptr;
  hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 4096;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&resource_heap));
  if (FAILED(hr)) { expect("native rendering descriptor heaps", false); return; }
  if (wide_table) {
    auto desc = heap_desc;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; desc.NumDescriptors = 16;
    hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&sampler_heap));
    expect("dynamic sampler table allocation", SUCCEEDED(hr) && sampler_heap);
    if (!sampler_heap) return;
    D3D12_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    auto handle = sampler_heap->GetCPUDescriptorHandleForHeapStart();
    UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (UINT i = 0; i < 16; ++i) { device->CreateSampler(&sampler, handle); handle.ptr += step; }
  }
  ID3D12DescriptorHeap *cpu_heap = nullptr;
  if (copy_cbv) {
    auto cpu_desc = heap_desc;
    cpu_desc.NumDescriptors = 1;
    cpu_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device->CreateDescriptorHeap(&cpu_desc, IID_PPV_ARGS(&cpu_heap));
    expect("CPU descriptor heap creation", SUCCEEDED(hr) && cpu_heap);
  }
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs(target_count);
  UINT rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (UINT i = 0; i < target_count; ++i) {
    rtvs[i].ptr = rtv.ptr + i * rtv_increment;
    device->CreateRenderTargetView(targets[i], nullptr, rtvs[i]);
  }
  auto cpu = resource_heap->GetCPUDescriptorHandleForHeapStart();
  UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {upload->GetGPUVirtualAddress(), 256};
  for (UINT i = 0; i < 4096; ++i) {
    device->CreateConstantBufferView(&cbv, cpu);
    cpu.ptr += increment;
  }
  if (copy_cbv && cpu_heap) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC offset_cbv = {upload->GetGPUVirtualAddress() + cbv_offset, 256};
    auto source = cpu_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateConstantBufferView(&offset_cbv, source);
    device->CopyDescriptorsSimple(1, resource_heap->GetCPUDescriptorHandleForHeapStart(), source,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  ID3D12Resource *sampled_texture = nullptr, *old_texture = nullptr;
  if (texture_test) {
    auto sample_desc = texture_desc;
    sample_desc.Width = sample_desc.Height = 4;
    sample_desc.MipLevels = 2;
    if (cube_test) sample_desc.DepthOrArraySize = 6;
    sample_desc.Format = bc1_test ? DXGI_FORMAT_BC1_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    sample_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    sampled_texture = create(sample_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    expect("typeless texture allocation", sampled_texture != nullptr);
    if (!sampled_texture) return;
    for (UINT face = 0; face < (cube_test ? 6u : 1u); ++face) for (UINT mip = 0; mip < 2; ++mip) {
      D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
      from.pResource = upload;
      from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      from.PlacedFootprint.Offset = mip ? 1024 : 0;
      from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 4u >> mip, 4u >> mip, 1, 256};
      if (bc1_test) from.PlacedFootprint.Footprint = {DXGI_FORMAT_BC1_UNORM, 4, 4, 1, 256};
      to.pResource = sampled_texture;
      to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      to.SubresourceIndex = face * 2 + mip;
      list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    D3D12_RESOURCE_BARRIER sample_barrier = {};
    sample_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sample_barrier.Transition = {sampled_texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &sample_barrier);
    D3D12_SHADER_RESOURCE_VIEW_DESC sample_view = {};
    sample_view.Format = bc1_test ? DXGI_FORMAT_BC1_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    sample_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sample_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sample_view.Texture2D.MostDetailedMip = 1;
    sample_view.Texture2D.MipLevels = 1;
    if (cube_test) {
      sample_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      sample_view.TextureCube.MostDetailedMip = 1; sample_view.TextureCube.MipLevels = 1;
    }
    if (volatile_texture) old_texture = committed(sample_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    device->CreateShaderResourceView(volatile_texture ? old_texture : sampled_texture, &sample_view,
                                       resource_heap->GetCPUDescriptorHandleForHeapStart());
    if (wide_table) {
      auto handle = resource_heap->GetCPUDescriptorHandleForHeapStart();
      for (UINT i = 1; i < table_size; ++i) {
        handle.ptr += increment;
        device->CreateShaderResourceView(sampled_texture, &sample_view, handle);
      }
    }
  }
  ID3D12Resource *skin_output = nullptr;
  ID3D12RootSignature *skin_root = nullptr;
  ID3D12PipelineState *skin_pipeline = nullptr;
  if (structured_test) {
    D3D12_SHADER_RESOURCE_VIEW_DESC data_view = {};
    data_view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    data_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    data_view.Buffer.NumElements = 2;
    data_view.Buffer.StructureByteStride = 16;
    if (typed_vertex) {
      data_view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      data_view.Buffer.StructureByteStride = 0;
    }
    if (skin_cache) {
      auto desc = buffer_desc(256);
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      skin_output = create(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      const char *source = typed_vertex ?
          "RWBuffer<float4> output : register(u0); [numthreads(1,1,1)] void CSMain() { output[0]=float4(1,0,0,1); output[1]=float4(1,0,0,1); }" :
          "RWStructuredBuffer<float4> output : register(u0); [numthreads(1,1,1)] void CSMain() { output[0]=float4(1,0,0,1); output[1]=float4(1,0,0,1); }";
      ID3DBlob *code = nullptr;
      hr = D3DCompile(source, std::strlen(source), "skin-cache.hlsl", nullptr, nullptr,
                      "CSMain", "cs_5_0", 0, 0, &code, nullptr);
      D3D12_DESCRIPTOR_RANGE skin_range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
      D3D12_ROOT_PARAMETER skin_parameter = {};
      skin_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      skin_parameter.DescriptorTable = {1, &skin_range};
      D3D12_ROOT_SIGNATURE_DESC skin_desc = {1, &skin_parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      if (SUCCEEDED(hr)) hr = D3D12SerializeRootSignature(&skin_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr);
      if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&skin_root));
      drop(blob);
      D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {};
      compute_desc.pRootSignature = skin_root;
      if (code) compute_desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
      if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&skin_pipeline));
      drop(code);
      expect("skin cache producer setup", SUCCEEDED(hr) && skin_output && skin_pipeline);
      if (FAILED(hr) || !skin_output || !skin_pipeline) return;
      D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
      view.Format = data_view.Format;
      view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      view.Buffer.NumElements = 2;
      view.Buffer.StructureByteStride = data_view.Buffer.StructureByteStride;
      auto handle = resource_heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += increment;
      device->CreateUnorderedAccessView(skin_output, nullptr, &view, handle);
      list->SetPipelineState(skin_pipeline);
      list->SetComputeRootSignature(skin_root);
      list->SetDescriptorHeaps(1, &resource_heap);
      auto gpu_handle = resource_heap->GetGPUDescriptorHandleForHeapStart(); gpu_handle.ptr += increment;
      list->SetComputeRootDescriptorTable(0, gpu_handle);
      list->Dispatch(1,1,1);
      D3D12_RESOURCE_BARRIER barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition = {skin_output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
      list->ResourceBarrier(1, &barrier);
    }
    device->CreateShaderResourceView(skin_cache ? skin_output : upload, &data_view, resource_heap->GetCPUDescriptorHandleForHeapStart());
  }
  auto gpu = resource_heap->GetGPUDescriptorHandleForHeapStart();
  list->SetPipelineState(pipeline);
  list->SetGraphicsRootSignature(root);
  ID3D12DescriptorHeap *bound_heaps[] = {resource_heap, sampler_heap};
  list->SetDescriptorHeaps(wide_table ? 2 : 1, bound_heaps);
  if (wide_table) list->SetGraphicsRootDescriptorTable(1, sampler_heap->GetGPUDescriptorHandleForHeapStart());
  if (geometry_test) list->SetGraphicsRootConstantBufferView(1, upload->GetGPUVirtualAddress() + 256);
  if (vertex_resource) list->SetGraphicsRootConstantBufferView(1, upload->GetGPUVirtualAddress());
  if (root_cbv) list->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress() + cbv_offset);
  else if (root_constants) {
    const float red[] = {1, 0, 0, 1};
    list->SetGraphicsRoot32BitConstants(0, 4, red, 0);
  } else list->SetGraphicsRootDescriptorTable(0, gpu);
  ID3D12Resource *depth = nullptr;
  ID3D12Resource *copied_depth = nullptr;
  ID3D12DescriptorHeap *dsv_heap = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  if (stencil_test || depth_test) {
    auto depth_desc = texture_desc;
    depth_desc.Format = wide_depth ? DXGI_FORMAT_R32G8X24_TYPELESS : depth_test ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    if (depth_copy_test) depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    depth = create(depth_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    hr = device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&dsv_heap));
    expect("depth-stencil attachment allocation", SUCCEEDED(hr) && depth && dsv_heap);
    if (!depth || !dsv_heap) return;
    dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = pipeline_desc.DSVFormat;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depth, &view, dsv);
    if (depth_copy_test) {
      copied_depth = create(depth_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      expect("copied scene depth allocation", copied_depth != nullptr);
      if (!copied_depth) return;
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.Format = DXGI_FORMAT_R32_FLOAT;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      auto cpu = resource_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += increment;
      device->CreateShaderResourceView(copied_depth, &srv, cpu);
      auto gpu_depth = gpu; gpu_depth.ptr += increment;
      list->SetGraphicsRootDescriptorTable(1, gpu_depth);
    }
  }
  if (clear_before_bind && (stencil_test || depth_test))
    list->ClearDepthStencilView(dsv, depth_test ? D3D12_CLEAR_FLAG_DEPTH : D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.25f, 7, 0, nullptr);
  list->OMSetRenderTargets(target_count, &rtv, TRUE, stencil_test || depth_test ? &dsv : nullptr);
  if (stencil_test || depth_test) {
    if (!clear_before_bind) list->ClearDepthStencilView(dsv, depth_test ? D3D12_CLEAR_FLAG_DEPTH : D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.25f, 7, 0, nullptr);
    list->OMSetStencilRef(7);
  }
  D3D12_VIEWPORT viewport = {0, 0, 16, 16, 0, 1};
  D3D12_RECT scissor = {0, 0, scissor_test ? 8 : 16, 16};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  if (stream_test) {
    D3D12_VERTEX_BUFFER_VIEW positions = {upload->GetGPUVirtualAddress()+64, 24, 8};
    D3D12_VERTEX_BUFFER_VIEW colors = {upload->GetGPUVirtualAddress()+128, 48, 16};
    list->IASetVertexBuffers(2, 1, &positions);
    list->IASetVertexBuffers(7, 1, &colors);
  }
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  if (indexed_test) {
    D3D12_INDEX_BUFFER_VIEW indices = {upload->GetGPUVirtualAddress() + 192, 8, DXGI_FORMAT_R16_UINT};
    list->IASetIndexBuffer(&indices);
  }
  const float clear[] = {0, 0, 1, 1};
  for (auto handle : rtvs) list->ClearRenderTargetView(handle, clear, 0, nullptr);
  if (prepass_test) {
    list->OMSetRenderTargets(0, nullptr, TRUE, &dsv);
    list->SetPipelineState(prepass);
    list->DrawInstanced(3, 1, 0, 0);
    if (depth_copy_test) {
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition = {depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE};
      list->ResourceBarrier(1, &barrier);
      list->CopyResource(copied_depth, depth);
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_READ;
      list->ResourceBarrier(1, &barrier);
      barrier.Transition = {copied_depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
      list->ResourceBarrier(1, &barrier);
    }
    list->OMSetRenderTargets(target_count, &rtv, TRUE, &dsv);
    list->SetPipelineState(pipeline);
  }
  ID3D12QueryHeap *queries = nullptr;
  ID3D12Resource *query_result = nullptr;
  D3D12_QUERY_TYPE query_type = binary_occlusion ? D3D12_QUERY_TYPE_BINARY_OCCLUSION : D3D12_QUERY_TYPE_OCCLUSION;
  if (occlusion_test) {
    D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, 2, 0};
    hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&queries));
    query_result = committed(buffer_desc(query_reuse ? 64 : 32), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    void *data = nullptr;
    if (query_result && SUCCEEDED(query_result->Map(0, nullptr, &data))) {
      std::memset(data, 0xcd, query_reuse ? 64 : 32); query_result->Unmap(0, nullptr);
    }
    expect("occlusion query heap and readback", SUCCEEDED(hr) && queries && query_result);
    if (!queries || !query_result) return;
    list->BeginQuery(queries, query_type, 0);
  }
  LARGE_INTEGER start, end, frequency;
  QueryPerformanceFrequency(&frequency);
  /* PSO specialization may happen on the first draw; do not time that compile. */
  ID3D12CommandSignature *indirect_signature = nullptr;
  ID3D12Resource *indirect_args = nullptr, *indirect_counter = nullptr;
  if (indirect && draw) {
    D3D12_INDIRECT_ARGUMENT_DESC argument = {}; argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC desc = {32, 1, &argument, 0};
    hr = device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&indirect_signature));
    expect("indirect draw signature", SUCCEEDED(hr) && indirect_signature);
    indirect_args = committed(buffer_desc(256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *data = nullptr;
    if (indirect_args && SUCCEEDED(indirect_args->Map(0, nullptr, &data))) {
      std::memset(data, 0, 256);
      D3D12_DRAW_ARGUMENTS args = {3, 1, 0, 0}; std::memcpy(static_cast<char *>(data) + 16, &args, sizeof(args));
      indirect_args->Unmap(0, nullptr);
    }
    if (indirect_count || indirect_empty) {
      indirect_counter = committed(buffer_desc(256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      if (indirect_counter && SUCCEEDED(indirect_counter->Map(0, nullptr, &data))) {
        std::memset(data, 0, 256); static_cast<UINT *>(data)[1] = indirect_empty ? 0 : 1;
        indirect_counter->Unmap(0, nullptr);
      }
    }
    if (indirect_signature && indirect_args)
      list->ExecuteIndirect(indirect_signature, 1, indirect_args, 16, indirect_counter, 4);
  } else if (draw) {
    if (indexed_test) list->DrawIndexedInstanced(3, 1, 1, 0, 0);
    else list->DrawInstanced(3, 1, 0, 0);
  }
  QueryPerformanceCounter(&start);
  for (UINT i = 0; draw && !indirect && i < (single_draw ? 0u : 1024u); ++i) {
    if (indexed_test) list->DrawIndexedInstanced(3, 1, 1, 0, 0);
    else list->DrawInstanced(3, 1, 0, 0);
  }
  QueryPerformanceCounter(&end);
  if (occlusion_test) {
    list->EndQuery(queries, query_type, 0);
    list->BeginQuery(queries, query_type, 1);
    list->EndQuery(queries, query_type, 1);
    list->ResolveQueryData(queries, query_type, 0, 2, query_result, 8);
    if (query_reuse) {
      list->ResolveQueryData(queries, query_type, 0, 1, query_result, 32);
      list->BeginQuery(queries, query_type, 0);
      list->EndQuery(queries, query_type, 0);
      list->ResolveQueryData(queries, query_type, 0, 1, query_result, 40);
    }
  }
  std::printf("TIMING %u extra draws, 4096 descriptors: %.6f ms CPU recording (not game FPS)\n",
              draw && !indirect && !single_draw ? 1024u : 0u,
              double(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart);
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
  list->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = target;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 256};
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  for (UINT i = 1; i < target_count; ++i) {
    barrier.Transition.pResource = targets[i];
    list->ResourceBarrier(1, &barrier);
    src.pResource = targets[i];
    dst.PlacedFootprint.Offset = i * 4096;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  if (volatile_texture) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MostDetailedMip = view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(sampled_texture, &view, resource_heap->GetCPUDescriptorHandleForHeapStart());
  }
  bool ok = finish(queue, list);
  if (occlusion_test) {
    void *data = nullptr;
    bool valid = ok && SUCCEEDED(query_result->Map(0, nullptr, &data)) && data;
    auto *values = static_cast<UINT64 *>(data);
    if (valid) std::printf("INFO occlusion visible %llu empty %llu\n", (unsigned long long)values[1], (unsigned long long)values[2]);
    expect(binary_occlusion ? "binary occlusion resolves visible one and empty zero" : "occlusion counts visible samples and empty zero",
           valid && (binary_occlusion ? values[1] == 1 : values[1] > 0) && values[2] == 0);
    expect("occlusion resolve preserves surrounding bytes", valid && values[0] == 0xcdcdcdcdcdcdcdcdull && values[3] == values[0]);
    if (query_reuse) {
      expect("repeated resolve preserves the earlier visible query result", valid && values[4] == values[1]);
      expect("reusing a query index for an empty interval preserves both old and new results", valid && values[5] == 0 && values[1] > 0);
      expect("query reuse preserves trailing sentinel bytes", valid && values[6] == values[0] && values[7] == values[0]);
    }
    if (valid) query_result->Unmap(0, nullptr);
  }
  std::printf("INFO native rendering GPU complete %d\n", int(ok));
  void *pixels = nullptr;
  ok = ok && SUCCEEDED(readback->Map(0, nullptr, &pixels)) && pixels;
  uint32_t pixel = pixels ? *reinterpret_cast<uint32_t *>(static_cast<char *>(pixels) + 8 * 256 + 8 * 4) : 0;
  if (mrt_test) {
    const uint32_t expected[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xff00ffff, 0xffff00ff};
    for (UINT i = 0; i < target_count; ++i) {
      uint32_t actual = pixels ? *reinterpret_cast<uint32_t *>(static_cast<char *>(pixels) + i * 4096 + 8 * 256 + 8 * 4) : 0;
      std::printf("INFO G-buffer target %u actual %08x expected %08x\n", i, actual, expected[i]);
      expect("depth prepass and all G-buffer attachments preserve their shader outputs", ok && actual == expected[i]);
    }
  }
  std::printf("INFO native rendering center pixel %08x\n", pixel);
  std::printf("INFO shader path %s with %s resources\n", dxbc ? "DXBC" : "SM6", create == placed ? "placed" : "committed");
  if (indirect) {
    expect(indirect_empty ? "zero GPU count preserves the clear" : "GPU indirect arguments draw the triangle",
           ok && pixel == (indirect_empty ? 0xffff0000 : 0xff0000ff));
  } else if (scissor_test && pixels) {
    uint32_t left = *reinterpret_cast<uint32_t *>(static_cast<char *>(pixels) + 8*256+4*4);
    uint32_t right = *reinterpret_cast<uint32_t *>(static_cast<char *>(pixels) + 8*256+12*4);
    std::printf("INFO scissor pixels left %08x right %08x\n", left, right);
    expect("scissor clips the draw and preserves the cleared right half", ok && left == 0xff0000ff && right == 0xffff0000);
  } else expect(draw ? stencil_test ? "stencil clear, comparison ops, and dynamic reference preserve UI pixels" : stream_test ? "vertex inputs in slots 2 and 7 with separate offsets" : structured_test ? "structured SRV shares the original uploaded buffer" : texture_test ? "typed SRV samples mip 1 of a typeless uploaded texture" : copy_cbv ? "copied CPU descriptor preserves nonzero CBV offset" : root_cbv ? "draw with root CBV" : root_constants ? "draw with root constants" :
                "draw reads CBV through descriptor table and GPU texture readback" :
                "clear-only render pass and GPU texture readback", ok && pixel == (draw ? 0xff0000ff : 0xffff0000));
  if (pixels) readback->Unmap(0, nullptr);
  drop(skin_pipeline); drop(skin_root); drop(skin_output);
  drop(query_result); drop(queries); drop(indirect_signature); drop(indirect_args); drop(indirect_counter);
  drop(copied_depth); drop(depth); drop(dsv_heap); drop(old_texture); drop(sampled_texture); drop(cpu_heap); drop(sampler_heap); drop(resource_heap); drop(rtv_heap); drop(prepass); drop(pipeline); drop(root);
  for (UINT i = 1; i < target_count; ++i) drop(targets[i]);
  drop(readback); drop(upload); drop(target); drop(list); drop(allocator); drop(queue);
}

static void swapchain() {
  HWND window = CreateWindowExA(0, "STATIC", "D3D12 swapchain validation", WS_OVERLAPPEDWINDOW,
                                 30, 30, 200, 120, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  IDXGIFactory4 *factory = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  IDXGISwapChain1 *chain = nullptr;
  IDXGISwapChain3 *chain3 = nullptr;
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = 128; desc.Height = 64;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 3;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &chain);
  std::printf("INFO three-buffer swapchain HRESULT %08lx\n", (unsigned long)hr);
  expect("three-buffer flip-sequential chain creation", SUCCEEDED(hr) && chain);
  if (!chain) { drop(queue); drop(factory); DestroyWindow(window); return; }
  chain->QueryInterface(IID_PPV_ARGS(&chain3));
  ID3D12Resource *buffers[3] = {}, *again = nullptr;
  bool valid = true;
  for (UINT i = 0; i < 3; ++i) {
    hr = chain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
    valid &= SUCCEEDED(hr) && buffers[i];
  }
  expect("logical backbuffers are distinct resources", valid && buffers[0] != buffers[1] && buffers[1] != buffers[2]);
  hr = chain->GetBuffer(1, IID_PPV_ARGS(&again));
  expect("GetBuffer preserves COM resource identity", SUCCEEDED(hr) && again == buffers[1]);
  drop(again);
  DXGI_SWAP_CHAIN_DESC1 observed = {};
  hr = chain->GetDesc1(&observed);
  expect("logical descriptor preserves flip mode and count", SUCCEEDED(hr) && observed.BufferCount == 3 &&
         observed.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && observed.Width == 128 && observed.Height == 64);
  if (chain3) {
    UINT before = chain3->GetCurrentBackBufferIndex();
    hr = chain->Present(0, DXGI_PRESENT_TEST);
    expect("TEST present does not advance the ring", SUCCEEDED(hr) && chain3->GetCurrentBackBufferIndex() == before);
  }
  if (chain3) {
    ID3D12Fence *dependency = nullptr, *completed = nullptr;
    ID3D12CommandQueue *producer = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dependency));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed));
    D3D12_COMMAND_QUEUE_DESC copy_queue = {};
    copy_queue.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    device->CreateCommandQueue(&copy_queue, IID_PPV_ARGS(&producer));
    if (dependency && completed && producer) {
      UINT before = chain3->GetCurrentBackBufferIndex(), presents_before = 0, presents_waiting = 0, presents_after = 0;
      chain->GetLastPresentCount(&presents_before);
      queue->Wait(dependency, 1);
      hr = chain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
      expect("nonblocking Present reports a pending queue and keeps its buffer", hr == DXGI_ERROR_WAS_STILL_DRAWING &&
             chain3->GetCurrentBackBufferIndex() == before);
      hr = chain->Present(0, 0);
      chain->GetLastPresentCount(&presents_waiting);
      expect("presentation waits on its own logical queue without blocking the CPU", hr == S_OK &&
             presents_waiting == presents_before && chain3->GetCurrentBackBufferIndex() == (before+1)%3);
      queue->Signal(completed, 1);
      producer->Signal(dependency, 1);
      HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
      completed->SetEventOnCompletion(1, event);
      bool ready = WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
      chain->GetLastPresentCount(&presents_after);
      expect("producer signal submits the queued presentation", ready && presents_after == presents_before+1);
      CloseHandle(event);
    } else expect("queued presentation synchronization setup", false);
    drop(dependency); drop(completed); drop(producer);
  }
  for (auto &buffer : buffers) drop(buffer);
  hr = chain->ResizeBuffers(2, 64, 32, DXGI_FORMAT_UNKNOWN, 0);
  if (SUCCEEDED(hr)) hr = chain->GetBuffer(1, IID_PPV_ARGS(&again));
  D3D12_RESOURCE_DESC resized = {};
  if (again) resized = again->GetDesc();
  expect("resize recreates the complete logical ring", SUCCEEDED(hr) && again && resized.Width == 64 && resized.Height == 32);
  drop(again); drop(chain3); drop(chain); drop(queue); drop(factory); DestroyWindow(window);
}

static void future_queue_wait() {
  ID3D12CommandQueue *consumer = nullptr, *producer = nullptr;
  ID3D12CommandAllocator *allocator = nullptr, *next_allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12Fence *dependency = nullptr, *done = nullptr;
  auto desc = buffer_desc(256);
  ID3D12Resource *upload = committed(desc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *readback = committed(desc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_COMMAND_QUEUE_DESC q = {};
  HRESULT hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&consumer));
  q.Type = D3D12_COMMAND_LIST_TYPE_COPY;
  if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&producer));
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&next_allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
  if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dependency));
  if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
  expect("future queue wait setup", SUCCEEDED(hr) && upload && readback);
  if (FAILED(hr) || !upload || !readback) return;
  void *data = nullptr;
  hr = upload->Map(0, nullptr, &data);
  if (SUCCEEDED(hr)) { std::memset(data, 0x5a, 256); upload->Unmap(0, nullptr); }
  LARGE_INTEGER start, end, frequency;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);
  consumer->Wait(dependency, 1);
  QueryPerformanceCounter(&end);
  expect("GPU queue wait returns before its future producer signal", (end.QuadPart-start.QuadPart)*1000.0/frequency.QuadPart < 100);
  list->CopyBufferRegion(readback, 0, upload, 0, 256);
  list->Close();
  ID3D12CommandList *lists[] = {list};
  consumer->ExecuteCommandLists(1, lists);
  /* Reuse the list before the other queue signals; submitted commands must survive. */
  list->Reset(next_allocator, nullptr);
  list->Close();
  consumer->Signal(done, 1);
  producer->Signal(dependency, 1);
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  done->SetEventOnCompletion(1, event);
  bool completed = WaitForSingleObject(event, 2500) == WAIT_OBJECT_0;
  expect("later copy-queue signal unblocks the earlier direct-queue wait", completed);
  if (!completed) {
    /* Release a broken implementation's GPU wait so this test cannot leave a hung queue. */
    dependency->Signal(1);
    completed = WaitForSingleObject(event, 2500) == WAIT_OBJECT_0;
  }
  data = nullptr;
  bool copied = completed && SUCCEEDED(readback->Map(0, nullptr, &data)) && data;
  if (copied) {
    for (UINT i = 0; i < 256; ++i) copied &= static_cast<unsigned char *>(data)[i] == 0x5a;
    readback->Unmap(0, nullptr);
  }
  expect("submitted copy survives command-list Reset while a queue is waiting", copied);
  consumer->Wait(dependency, 2);
  consumer->Signal(done, 2);
  dependency->Signal(2);
  done->SetEventOnCompletion(2, event);
  expect("CPU fence signal also releases a waiting logical queue", WaitForSingleObject(event, 2500) == WAIT_OBJECT_0);
  CloseHandle(event);
  drop(list); drop(allocator); drop(next_allocator); drop(dependency); drop(done);
  drop(readback); drop(upload); drop(consumer); drop(producer);
}

static void compute_dispatch(const char *mode) {
  bool dxbc = std::strstr(mode, "dxbc") != nullptr;
  bool shared = std::strstr(mode, "groupshared") != nullptr;
  bool pingpong = std::strstr(mode, "pingpong") != nullptr;
  bool aliasing = std::strstr(mode, "aliasing") != nullptr;
  bool alias_same_resource = std::strstr(mode, "same-resource") != nullptr;
  bool alias_separated = std::strstr(mode, "separated") != nullptr;
  bool texture_compute = std::strstr(mode, "texpingpong") != nullptr;
  bool root_uav = std::strstr(mode, "root-uav") != nullptr;
  bool dual_output = std::strstr(mode, "dual-") != nullptr;
  bool unused_uav = std::strstr(mode, "unused-uav") != nullptr;
  bool null_unused_uav = std::strstr(mode, "null-unused") != nullptr;
  bool raw_output = std::strstr(mode, "dual-raw") != nullptr;
  bool float_output = std::strstr(mode, "dual-float") != nullptr;
  UINT data_offset = std::strstr(mode, "unaligned") ? 12 : std::strstr(mode, "offset") ? 256 : 0;
  auto cs = shader("cs.dxil");
  if (dxbc) {
    auto source = shader("validation.hlsl");
    if (dual_output) {
      const char *text = float_output ?
        "RWBuffer<float> output:register(u0); RWBuffer<float> second:register(u1);\n[numthreads(8,1,1)] void CSMain(uint3 id:SV_DispatchThreadID) { output[id.x]=float(id.x)*0.5-7.25; second[id.x]=float(id.x)*0.25+2.5; }\n" : raw_output ?
        "RWByteAddressBuffer output:register(u0); RWByteAddressBuffer second:register(u1);\n[numthreads(8,1,1)] void CSMain(uint3 id:SV_DispatchThreadID) { output.Store(id.x*4,id.x*17+42); second.Store(id.x*4,id.x*17+85); }\n" :
        "RWBuffer<uint> output:register(u0); RWBuffer<uint> second:register(u1);\n[numthreads(8,1,1)] void CSMain(uint3 id:SV_DispatchThreadID) { output[id.x]=id.x*17+42; second[id.x]=id.x*17+85; }\n";
      source.assign(text, text + std::strlen(text));
    }
    if (unused_uav) {
      const char text[] = "RWBuffer<uint> output:register(u0);\n"
          "[numthreads(8,1,1)] void CSMain(uint3 id:SV_DispatchThreadID) { output[id.x]=id.x*17+42; }\n";
      source.assign(text, text + sizeof(text) - 1);
    }
    if (texture_compute) {
      const char text[] = "RWTexture2D<uint> output : register(u0);\n[numthreads(8,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) { output[int2(id.x,0)] = id.x * 17 + 42; }\n";
      source.assign(text, text + sizeof(text) - 1);
    }
    if (shared) {
      const char text[] = "RWStructuredBuffer<uint> output : register(u0);\n"
          "groupshared uint scratch[8];\n"
          "[numthreads(8,1,1)] void CSMain(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID) {\n"
          "if(local.x == 0) scratch[1] = group.x * 8 * 17 + 42;\n"
          "GroupMemoryBarrierWithGroupSync(); output[id.x] = scratch[1] + local.x * 17; }\n";
      source.assign(text, text + sizeof(text) - 1);
    }
    if (aliasing) {
      const char text[] = "RWStructuredBuffer<uint> output:register(u0);\n"
          "[numthreads(8,1,1)] void CSMain(uint3 id:SV_DispatchThreadID) { uint value=id.x*17+42; "
          "[loop] for(uint i=0;i<16384;++i) value=value*1664525+1013904223; output[id.x]=value; }\n";
      source.assign(text, text + sizeof(text) - 1);
    }
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT result = D3DCompile(source.data(), source.size(), "validation.hlsl", nullptr, nullptr,
                                 "CSMain", "cs_5_0", 0, 0, &code, &errors);
    if (errors) { std::printf("%s\n", static_cast<char *>(errors->GetBufferPointer())); drop(errors); }
    expect("DXBC compute shader compilation", SUCCEEDED(result) && code);
    if (!code) return;
    cs.assign(static_cast<char *>(code->GetBufferPointer()),
              static_cast<char *>(code->GetBufferPointer()) + code->GetBufferSize());
    drop(code);
  }
  if (cs.empty()) { expect("compute shader fixture available", false); return; }
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, queue_desc.Type, allocator, nullptr, IID_PPV_ARGS(&list));
  expect("compute command setup", SUCCEEDED(hr) && list);
  if (!list) return;
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
                                 D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
  if (dual_output || unused_uav) range.NumDescriptors = 2;
  D3D12_ROOT_PARAMETER parameter = {};
  parameter.ParameterType = root_uav ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  if (root_uav) parameter.Descriptor = {0, 0};
  else parameter.DescriptorTable = {1, &range};
  D3D12_DESCRIPTOR_RANGE ping_ranges[] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
  if (pingpong) parameter.DescriptorTable = {2, ping_ranges};
  D3D12_ROOT_SIGNATURE_DESC root_desc = {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = nullptr;
  ID3D12RootSignature *root = nullptr;
  hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr);
  if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
  drop(blob);
  expect("compute root signature", SUCCEEDED(hr) && root);
  if (!root) return;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root;
  pso_desc.CS = {cs.data(), cs.size()};
  ID3D12PipelineState *pipeline = nullptr;
  hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pipeline));
  std::printf("INFO %s compute pipeline HRESULT %08lx\n", dxbc ? "DXBC" : "SM6", (unsigned long)hr);
  expect("compute pipeline creation", SUCCEEDED(hr) && pipeline);
  if (!pipeline) { drop(list); drop(allocator); drop(queue); drop(root); return; }
  auto output_desc = buffer_desc(256 + data_offset);
  output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (texture_compute) {
    output_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    output_desc.Width = 32; output_desc.Height = 1;
    output_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; output_desc.Format = DXGI_FORMAT_R32_UINT;
  }
  auto create = std::strstr(mode, "placed") ? placed : committed;
  ID3D12Resource *output = create(output_desc, D3D12_HEAP_TYPE_DEFAULT, dual_output || unused_uav ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource *alias_read = nullptr;
  if (aliasing) {
    drop(output);
    D3D12_HEAP_DESC alias_heap_desc = {};
    alias_heap_desc.SizeInBytes = 65536;
    alias_heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    alias_heap_desc.Properties.CreationNodeMask = alias_heap_desc.Properties.VisibleNodeMask = 1;
    ID3D12Heap *alias_heap = nullptr;
    hr = device->CreateHeap(&alias_heap_desc, IID_PPV_ARGS(&alias_heap));
    if (SUCCEEDED(hr)) hr = device->CreatePlacedResource(alias_heap, 0, &output_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&output));
    if (SUCCEEDED(hr)) hr = device->CreatePlacedResource(alias_heap, 0, &output_desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&alias_read));
    drop(alias_heap);
    expect("two buffer resources alias the same heap range", SUCCEEDED(hr) && output && alias_read);
    if (FAILED(hr) || !output || !alias_read) return;
  }
  ID3D12Resource *readback = committed(buffer_desc(256), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  expect("compute UAV and readback allocation", output && readback);
  if (!output || !readback) return;
  ID3D12Resource *zeros = nullptr;
  if (dual_output || aliasing || unused_uav) {
    zeros = committed(buffer_desc(256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *data = nullptr;
    expect("dual output initialization", zeros && SUCCEEDED(zeros->Map(0, nullptr, &data)) && data);
    if (!data) return;
    std::memset(data, 0, 256); zeros->Unmap(0, nullptr);
    if (aliasing) {
      D3D12_RESOURCE_BARRIER activate = {};
      activate.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
      activate.Aliasing = {nullptr, output};
      list->ResourceBarrier(1, &activate);
    }
    list->CopyBufferRegion(output, 0, zeros, 0, 256);
    D3D12_RESOURCE_BARRIER barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output,0,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1, &barrier);
  }
  ID3D12PipelineState *reader = nullptr;
  ID3D12Resource *secondary = nullptr;
  if (pingpong) {
    const char *source = texture_compute ?
        "Texture2D<uint> inputData : register(t0); RWTexture2D<uint> output : register(u0);\n[numthreads(8,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) { output[int2(id.x,0)]=inputData.Load(int3(id.x,0,0))+1; }\n" :
        "StructuredBuffer<uint> inputData : register(t0); RWStructuredBuffer<uint> output : register(u0);\n[numthreads(8,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) { output[id.x]=inputData[id.x]+1; }\n";
    ID3DBlob *code = nullptr;
    hr = D3DCompile(source, std::strlen(source), "pingpong.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &code, nullptr);
    if (SUCCEEDED(hr)) {
      pso_desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
      hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&reader));
    }
    drop(code);
    secondary = create(output_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (FAILED(hr) || !reader || !secondary) { expect("second compute pass setup", false); return; }
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, pingpong ? 4u : dual_output || unused_uav ? 2u : 1u,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ID3D12DescriptorHeap *heap = nullptr;
  hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
  if (FAILED(hr) || !heap) { expect("compute descriptor heap", false); return; }
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.FirstElement = data_offset / 4;
  uav.Buffer.NumElements = 32;
  uav.Buffer.StructureByteStride = 4;
  if (dual_output || unused_uav) {
    uav.Format = raw_output ? DXGI_FORMAT_R32_TYPELESS : float_output ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT;
    uav.Buffer.StructureByteStride = 0;
    uav.Buffer.Flags = raw_output ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
  }
  if (texture_compute) {
    uav = {}; uav.Format = DXGI_FORMAT_R32_UINT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  }
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
  UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (pingpong) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 32; srv.Buffer.StructureByteStride = 4;
    if (texture_compute) {
      srv = {}; srv.Format = DXGI_FORMAT_R32_UINT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    }
    device->CreateShaderResourceView(nullptr, &srv, cpu);
    auto input = cpu; input.ptr += 2 * increment;
    device->CreateShaderResourceView(aliasing && !alias_same_resource ? alias_read : output, &srv, input);
    auto next = cpu; next.ptr += 3 * increment; device->CreateUnorderedAccessView(secondary, nullptr, &uav, next);
    cpu.ptr += increment;
  }
  device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
  if (unused_uav) {
    auto next = cpu; next.ptr += increment;
    device->CreateUnorderedAccessView(null_unused_uav ? nullptr : output, nullptr, &uav, next);
  }
  if (dual_output) {
    auto next = cpu; next.ptr += increment;
    uav.Buffer.FirstElement = data_offset / 4 + 32;
    device->CreateUnorderedAccessView(output, nullptr, &uav, next);
  }
  list->SetPipelineState(pipeline);
  list->SetComputeRootSignature(root);
  list->SetDescriptorHeaps(1, &heap);
  if (root_uav) list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress() + data_offset);
  else list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  list->Dispatch(4, 1, 1);
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = output;
  list->ResourceBarrier(1, &barrier);
  if (pingpong) {
    if (aliasing && !alias_same_resource) {
      if (alias_separated) {
        bool completed = finish(queue, list);
        expect("writer completes before the separate alias consumer submission", completed);
        if (!completed) return;
        hr = allocator->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(allocator, nullptr);
        expect("alias consumer command-list reset", SUCCEEDED(hr));
        if (FAILED(hr)) return;
        list->SetComputeRootSignature(root);
        list->SetDescriptorHeaps(1, &heap);
      }
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
      barrier.Aliasing = {output, alias_read};
      list->ResourceBarrier(1, &barrier);
      barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    }
    list->SetPipelineState(reader);
    auto next = heap->GetGPUDescriptorHandleForHeapStart(); next.ptr += 2 * increment;
    list->SetComputeRootDescriptorTable(0, next);
    list->Dispatch(4, 1, 1);
    barrier.UAV.pResource = secondary; list->ResourceBarrier(1, &barrier);
  }
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {pingpong ? secondary : output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
  list->ResourceBarrier(1, &barrier);
  if (texture_compute) {
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = secondary; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT,32,1,1,256};
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
  } else list->CopyBufferRegion(readback, 0, pingpong ? secondary : output, data_offset, dual_output ? 256 : 128);
  bool done = finish(queue, list);
  expect("compute dispatch completes on GPU", done);
  void *data = nullptr;
  bool correct = done && SUCCEEDED(readback->Map(0, nullptr, &data)) && data;
  if (correct) {
    for (UINT i = 0; i < (dual_output ? 64u : 32u); ++i) {
      UINT expected = dual_output && i >= 32 ? (i-32)*17+85 : i*17+42+pingpong;
      if (aliasing) {
        expected = i*17+42;
        if (dxbc)
          for (UINT iteration = 0; iteration < 16384; ++iteration)
            expected = expected*1664525u+1013904223u;
        ++expected;
      }
      if (float_output) {
        float value = i >= 32 ? float(i-32)*0.25f+2.5f : float(i)*0.5f-7.25f;
        std::memcpy(&expected, &value, sizeof(expected));
      }
      if (static_cast<uint32_t *>(data)[i] != expected) {
        std::printf("INFO compute mismatch index %u actual %u expected %u\n", i,
                    static_cast<uint32_t *>(data)[i], expected);
        correct = false; break;
      }
    }
    readback->Unmap(0, nullptr);
  }
  expect(dual_output ? "both disjoint UAV ranges retain all 64 GPU writes" : "compute shader writes all 32 expected uint values", correct);
  drop(alias_read); drop(zeros); drop(reader); drop(secondary); drop(readback); drop(output); drop(heap); drop(pipeline); drop(root);
  drop(list); drop(allocator); drop(queue);
}

struct NullWait { ID3D12Fence *fence; HANDLE entered; HRESULT result; };
static DWORD WINAPI wait_without_event(void *arg) {
  auto *state = static_cast<NullWait *>(arg);
  SetEvent(state->entered);
  state->result = state->fence->SetEventOnCompletion(1, nullptr);
  return 0;
}
static void null_event_wait() {
  ID3D12Fence *fence = nullptr;
  HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (FAILED(hr)) { expect("CPU fence creation", false); return; }
  NullWait state = {fence, CreateEventA(nullptr, FALSE, FALSE, nullptr), E_FAIL};
  HANDLE worker = CreateThread(nullptr, 0, wait_without_event, &state, 0, nullptr);
  bool entered = worker && WaitForSingleObject(state.entered, 1000) == WAIT_OBJECT_0;
  expect("null-event completion waits before the fence is signaled",
         entered && WaitForSingleObject(worker, 100) == WAIT_TIMEOUT);
  hr = fence->Signal(1);
  bool completed = worker && WaitForSingleObject(worker, 2000) == WAIT_OBJECT_0;
  expect("CPU signal releases null-event completion", SUCCEEDED(hr) && completed && SUCCEEDED(state.result));
  CloseHandle(worker); CloseHandle(state.entered); drop(fence);
}

static void uav_clear(bool floating) {
  ID3D12CommandQueue *queue = nullptr; ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr; ID3D12DescriptorHeap *heap = nullptr;
  D3D12_COMMAND_QUEUE_DESC q = {};
  HRESULT hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, q.Type, allocator, nullptr, IID_PPV_ARGS(&list));
  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
  auto desc = buffer_desc(256); desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto *target = placed(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto *input = committed(buffer_desc(256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto *result = committed(buffer_desc(256), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  if (FAILED(hr) || !list || !heap || !target || !input || !result) { expect("UAV clear setup", false); return; }
  void *data = nullptr;
  if (SUCCEEDED(input->Map(0, nullptr, &data))) { std::memset(data, 0xcd, 256); input->Unmap(0, nullptr); }
  list->CopyBufferRegion(target, 0, input, 0, 256);
  D3D12_RESOURCE_BARRIER barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {target, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  list->ResourceBarrier(1, &barrier);
  D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
  view.Format = floating ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT;
  view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; view.Buffer.FirstElement = 4; view.Buffer.NumElements = 8;
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart(); auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
  device->CreateUnorderedAccessView(target, nullptr, &view, cpu);
  list->SetDescriptorHeaps(1, &heap);
  const UINT integers[4] = {0xdeadbeef, 0, 0, 0}; const float floats[4] = {1.25f, 0, 0, 0};
  if (floating) list->ClearUnorderedAccessViewFloat(gpu, cpu, target, floats, 0, nullptr);
  else list->ClearUnorderedAccessViewUint(gpu, cpu, target, integers, 0, nullptr);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; list->ResourceBarrier(1, &barrier);
  list->CopyBufferRegion(result, 0, target, 0, 256);
  bool ok = finish(queue, list) && SUCCEEDED(result->Map(0, nullptr, &data)) && data;
  if (ok) {
    for (UINT i = 0; i < 64; ++i)
      ok &= static_cast<UINT *>(data)[i] == (i >= 4 && i < 12 ? floating ? 0x3fa00000u : 0xdeadbeefu : 0xcdcdcdcdu);
    result->Unmap(0, nullptr);
  }
  expect(floating ? "FLOAT UAV clear preserves view range and surrounding bytes" : "UINT UAV clear preserves all 32 bits and surrounding bytes", ok);
  drop(result); drop(input); drop(target); drop(heap); drop(list); drop(allocator); drop(queue);
}

static void multi_list_queries() {
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *lists[3] = {};
  ID3D12Fence *gate = nullptr, *done = nullptr;
  ID3D12QueryHeap *queries = nullptr;
  D3D12_COMMAND_QUEUE_DESC desc = {};
  HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator));
  for (auto &list : lists)
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, desc.Type, allocator, nullptr, IID_PPV_ARGS(&list));
  if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
  if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
  D3D12_QUERY_HEAP_DESC query_desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
  if (SUCCEEDED(hr)) hr = device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries));
  expect("multi-list query and queue setup", SUCCEEDED(hr));
  if (FAILED(hr)) return;
  auto *input = committed(buffer_desc(256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto *first = committed(buffer_desc(256), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto *second = committed(buffer_desc(256), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto *output = committed(buffer_desc(256), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto *results = committed(buffer_desc(64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  expect("multi-list GPU buffers", input && first && second && output && results);
  if (!input || !first || !second || !output || !results) return;
  void *mapped = nullptr;
  if (SUCCEEDED(input->Map(0, nullptr, &mapped))) {
    for (UINT i = 0; i < 64; ++i) static_cast<UINT *>(mapped)[i] = i * 73 + 17;
    input->Unmap(0, nullptr);
  }
  if (SUCCEEDED(results->Map(0, nullptr, &mapped))) { std::memset(mapped, 0xcd, 64); results->Unmap(0, nullptr); }
  lists[0]->CopyBufferRegion(first, 0, input, 0, 256);
  lists[0]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  D3D12_RESOURCE_BARRIER barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {first, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
  lists[1]->ResourceBarrier(1, &barrier);
  lists[1]->CopyBufferRegion(second, 0, first, 0, 256);
  lists[1]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
  lists[1]->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, results, 8);
  barrier.Transition.pResource = second;
  lists[2]->ResourceBarrier(1, &barrier);
  lists[2]->CopyBufferRegion(output, 0, second, 0, 256);
  lists[2]->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  lists[2]->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, results, 32);
  for (auto *list : lists) expect("multi-list close", SUCCEEDED(list->Close()));
  expect("multi-list future wait", SUCCEEDED(queue->Wait(gate, 1)));
  ID3D12CommandList *batch[] = {lists[0], lists[1], lists[2]};
  queue->ExecuteCommandLists(3, batch);
  for (auto *list : lists) expect("recording reset before pending batch executes", SUCCEEDED(list->Reset(allocator, nullptr)));
  expect("multi-list completion signal queued", SUCCEEDED(queue->Signal(done, 1)));
  expect("CPU gate releases the captured batch", SUCCEEDED(gate->Signal(1)));
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  bool complete = SUCCEEDED(done->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
  CloseHandle(event);
  expect("captured multi-list batch and queries finish on GPU", complete);
  bool copied = complete && SUCCEEDED(output->Map(0, nullptr, &mapped)) && mapped;
  if (copied) {
    for (UINT i = 0; i < 64; ++i) copied &= static_cast<UINT *>(mapped)[i] == i * 73 + 17;
    output->Unmap(0, nullptr);
  }
  expect("cross-list GPU copies survive command-list Reset", copied);
  bool read = complete && SUCCEEDED(results->Map(0, nullptr, &mapped)) && mapped;
  auto *ticks = static_cast<UINT64 *>(mapped);
  expect("cross-list query resolve and index reuse preserve real GPU timestamps",
         read && ticks[1] && ticks[1] != 0xcdcdcdcdcdcdcdcdull && ticks[2] >= ticks[1] &&
         ticks[2] != 0xcdcdcdcdcdcdcdcdull && ticks[4] >= ticks[2] && ticks[4] != 0xcdcdcdcdcdcdcdcdull);
  expect("multi-list resolves preserve surrounding bytes", read && ticks[0] == 0xcdcdcdcdcdcdcdcdull && ticks[3] == ticks[0] && ticks[5] == ticks[0]);
  if (read) results->Unmap(0, nullptr);
  drop(results); drop(output); drop(second); drop(first); drop(input); drop(queries); drop(done); drop(gate);
  for (auto &list : lists) drop(list);
  drop(allocator); drop(queue);
}

static void query_heap_results(bool gpu_destination = false, bool mixed = false) {
  ID3D12CommandQueue *queue = nullptr; ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr; ID3D12QueryHeap *heap = nullptr;
  D3D12_COMMAND_QUEUE_DESC q = {}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  HRESULT hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
  if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, q.Type, allocator, nullptr, IID_PPV_ARGS(&list));
  D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
  if (SUCCEEDED(hr)) hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap));
  expect("timestamp query heap creation", SUCCEEDED(hr) && heap);
  if (!heap || !list) { drop(heap); drop(list); drop(allocator); drop(queue); return; }
  auto *input = committed(buffer_desc(256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto *output = committed(buffer_desc(256), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto *result = committed(buffer_desc(64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!input || !output || !result) { expect("query result buffers", false); return; }
  void *mapped = nullptr;
  if (SUCCEEDED(input->Map(0, nullptr, &mapped))) { std::memset(mapped, 0x5c, 256); input->Unmap(0, nullptr); }
  if (SUCCEEDED(result->Map(0, nullptr, &mapped))) { std::memset(mapped, 0xcd, 64); result->Unmap(0, nullptr); }
  ID3D12Resource *gpu_result = nullptr;
  ID3D12Resource *seed = nullptr;
  if (gpu_destination) {
    gpu_result = committed(buffer_desc(64), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    seed = committed(buffer_desc(64), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    expect("GPU-only query destination allocation", gpu_result && seed);
    if (!gpu_result || !seed) return;
    seed->Map(0, nullptr, &mapped); std::memset(mapped, 0xcd, 64); seed->Unmap(0, nullptr);
    list->CopyBufferRegion(gpu_result, 0, seed, 0, 64);
  }
  auto *destination = gpu_destination ? gpu_result : result;
  list->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  list->CopyBufferRegion(output, 0, input, 0, 256);
  list->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
  list->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, destination, 8);
  list->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, mixed ? result : destination, 32);
  list->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  list->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, destination, 40);
  if (gpu_destination) {
    D3D12_RESOURCE_BARRIER barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {gpu_result, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(result, 0, gpu_result, 0, mixed ? 32 : 64);
    if (mixed) list->CopyBufferRegion(result, 40, gpu_result, 40, 24);
  }
  bool done = finish(queue, list);
  bool read = done && SUCCEEDED(result->Map(0, nullptr, &mapped));
  auto *ticks = static_cast<UINT64 *>(mapped);
  if (read) std::printf("INFO GPU timestamps %llu %llu\n", (unsigned long long)ticks[1], (unsigned long long)ticks[2]);
  expect("query timestamps replace sentinel with nonzero GPU results", read && ticks[1] && ticks[2] >= ticks[1] &&
         ticks[1] != 0xcdcdcdcdcdcdcdcdull && ticks[2] != 0xcdcdcdcdcdcdcdcdull);
  expect("query resolve preserves surrounding bytes", read && ticks[0] == 0xcdcdcdcdcdcdcdcdull && ticks[3] == ticks[0]);
  expect("repeated timestamp resolve retains the original value", read && ticks[4] == ticks[1]);
  expect("timestamp index reuse retains both GPU results", read && ticks[5] >= ticks[1] && ticks[5] != 0xcdcdcdcdcdcdcdcdull);
  expect("timestamp reuse preserves trailing sentinel bytes", read && ticks[6] == ticks[0] && ticks[7] == ticks[0]);
  if (read) result->Unmap(0, nullptr);
  drop(seed); drop(gpu_result); drop(result); drop(output); drop(input); drop(heap); drop(list); drop(allocator); drop(queue);
}

static void readback_scheduling(const char *mode) {
  const bool overlap = std::strstr(mode, "overlap") != nullptr;
  const bool overwrite = std::strstr(mode, "overwrite") != nullptr;
  const bool cross_queue = std::strstr(mode, "cross-queue") != nullptr;
  const bool release_all = std::strstr(mode, "release") != nullptr;
  const bool placed_target = std::strstr(mode, "placed") != nullptr;
  SetEnvironmentVariableA("DXMT_D3D12_QUERY_READBACK_DELAY_MS", overlap ? "500" : "40");
  ID3D12CommandQueue *queues[2] = {};
  ID3D12CommandAllocator *allocators[2] = {};
  ID3D12GraphicsCommandList *lists[2] = {};
  ID3D12Fence *fences[2] = {};
  HANDLE events[2] = {};
  HRESULT hr = S_OK;
  D3D12_COMMAND_QUEUE_DESC q = {};
  for (UINT i = 0; i < 2; ++i) {
    if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queues[i]));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocators[i]));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, q.Type, allocators[i], nullptr, IID_PPV_ARGS(&lists[i]));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[i]));
    events[i] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  }
  ID3D12QueryHeap *heap = nullptr;
  D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1, 0};
  if (SUCCEEDED(hr)) hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap));
  auto *input = committed(buffer_desc(256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = [&](UINT64 bytes) {
    return placed_target ? placed(buffer_desc(bytes), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST)
                         : committed(buffer_desc(bytes), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  };
  auto *results = readback(64);
  auto *second = readback(64);
  auto *output = committed(buffer_desc(256), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  expect("readback scheduling setup", SUCCEEDED(hr) && input && results && second && output);
  if (FAILED(hr) || !input || !results || !second || !output) return;
  void *mapped = nullptr;
  input->Map(0, nullptr, &mapped); std::memset(mapped, 0xa5, 256); input->Unmap(0, nullptr);
  for (auto *resource : {results, second}) {
    resource->Map(0, nullptr, &mapped); std::memset(mapped, 0xcd, 64); resource->Unmap(0, nullptr);
  }
  lists[0]->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  lists[0]->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, results, 8);
  if (overwrite) lists[0]->CopyBufferRegion(results, 8, input, 0, 8);
  lists[0]->Close(); ID3D12CommandList *a[] = {lists[0]};
  queues[0]->ExecuteCommandLists(1, a);
  expect("query recording reset while readback is outstanding", SUCCEEDED(lists[0]->Reset(allocators[0], nullptr)));
  if (release_all) {
    drop(heap); drop(input); drop(results); drop(second); drop(output);
    for (auto &list : lists) drop(list);
    for (auto &allocator : allocators) drop(allocator);
    for (auto &fence : fences) drop(fence);
    for (auto &queue : queues) drop(queue);
    drop(device);
    Sleep(250);
    expect("releasing every application COM reference during delayed readback remains safe", true);
    for (auto event : events) CloseHandle(event);
    return;
  }
  expect("first queue signals after its pending CPU readback", SUCCEEDED(queues[0]->Signal(fences[0], 1)));
  expect("later fence values retain the queue signal order", SUCCEEDED(queues[0]->Signal(fences[0], 2)));
  expect("query completion notification registered", SUCCEEDED(fences[0]->SetEventOnCompletion(2, events[0])));
  if (cross_queue) expect("consumer waits for readback queue fence", SUCCEEDED(queues[1]->Wait(fences[0], 2)));
  lists[1]->CopyBufferRegion(output, 0, input, 0, 256);
  if (cross_queue) {
    lists[1]->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    lists[1]->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, second, 16);
  }
  lists[1]->Close(); ID3D12CommandList *b[] = {lists[1]};
  queues[1]->ExecuteCommandLists(1, b);
  expect("independent queue completion signal", SUCCEEDED(queues[1]->Signal(fences[1], 1)));
  fences[1]->SetEventOnCompletion(1, events[1]);
  if (overlap) {
    bool quick = WaitForSingleObject(events[1], 200) == WAIT_OBJECT_0;
    expect("unrelated GPU queue completes before a delayed query callback", quick);
    expect("readback queue fence remains pending until actual query bytes are written",
           WaitForSingleObject(events[0], 0) == WAIT_TIMEOUT && fences[0]->GetCompletedValue() < 2);
    if (!quick) WaitForSingleObject(events[1], 3000);
  } else expect("dependent GPU work and query completion finish", WaitForSingleObject(events[1], 3000) == WAIT_OBJECT_0);
  expect("query result fence eventually completes", WaitForSingleObject(events[0], 3000) == WAIT_OBJECT_0);
  expect("ordered signal values remain complete", fences[0]->GetCompletedValue() >= 2);
  output->Map(0, nullptr, &mapped);
  bool copied = true;
  for (UINT i = 0; i < 64; ++i) copied &= static_cast<UINT *>(mapped)[i] == 0xa5a5a5a5;
  output->Unmap(0, nullptr); expect("independent GPU buffer contents remain exact", copied);
  results->Map(0, nullptr, &mapped); auto *ticks = static_cast<UINT64 *>(mapped); UINT64 first = ticks[1];
  expect(overwrite ? "later GPU copy wins over earlier query write" : "readback contains an actual nonzero GPU timestamp",
         overwrite ? first == 0xa5a5a5a5a5a5a5a5ull : first && first != 0xcdcdcdcdcdcdcdcdull);
  expect("CPU readback preserves surrounding sentinel bytes", ticks[0] == 0xcdcdcdcdcdcdcdcdull && ticks[2] == ticks[0]);
  results->Unmap(0, nullptr);
  if (cross_queue) {
    second->Map(0, nullptr, &mapped); ticks = static_cast<UINT64 *>(mapped);
    expect("query index reuse across queued waits retains both results", ticks[2] >= first && ticks[2] != 0xcdcdcdcdcdcdcdcdull);
    expect("cross-queue readback keeps its own result offset", ticks[0] == 0xcdcdcdcdcdcdcdcdull && ticks[1] == ticks[0] && ticks[3] == ticks[0]);
    second->Unmap(0, nullptr);
  }
  drop(heap); drop(input); drop(results); drop(second); drop(output);
  for (auto &list : lists) drop(list);
  for (auto &allocator : allocators) drop(allocator);
  for (auto &fence : fences) drop(fence);
  for (auto &queue : queues) drop(queue);
  for (auto event : events) CloseHandle(event);
}

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  HMODULE module = GetModuleHandleA("d3d12.dll");
  auto create = GetProcAddress(module, "D3D12CreateDevice");
  auto create_ordinal = GetProcAddress(module, MAKEINTRESOURCEA(101));
  expect("CreateDevice name and Windows ordinal 101 match", create && create == create_ordinal);
  auto debug = GetProcAddress(module, "D3D12GetDebugInterface");
  expect("GetDebugInterface name and Windows ordinal 102 match",
         debug && debug == GetProcAddress(module, MAKEINTRESOURCEA(102)));
  if (create_ordinal) {
    auto probe = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(create_ordinal);
    expect("ordinal support probe returns S_FALSE",
           probe(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr) == S_FALSE);
  }
  if (argc > 1 && (!std::strcmp(argv[1], "--heap-offset") || std::strstr(argv[1], "suballocated")))
    placed_test_offset = 65536;
  if (argc > 1) {
    retire_placed_alias = std::strstr(argv[1], "retire-alias") != nullptr;
    retire_newer_alias = std::strstr(argv[1], "retire-alias-new") != nullptr;
  }
  HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(hr) || !device) {
    std::printf("FAIL D3D12CreateDevice %08lx\n", (unsigned long)hr);
    return 2;
  }
  char path[MAX_PATH] = {};
  GetModuleFileNameA(GetModuleHandleA("d3d12.dll"), path, sizeof(path));
  std::printf("INFO D3D12 module %s\n", path);
  footprints();
  signatures();
  queue_copy(D3D12_COMMAND_LIST_TYPE_DIRECT, "direct queue");
  queue_copy(D3D12_COMMAND_LIST_TYPE_COPY, "copy queue");
  queue_copy(D3D12_COMMAND_LIST_TYPE_COMPUTE, "compute queue");
  if (argc > 1) {
    if (std::strstr(argv[1], "--readback-")) readback_scheduling(argv[1]);
    else if (!std::strcmp(argv[1], "--fence-allocation")) fence_allocation_failure();
    else if (!std::strcmp(argv[1], "--queries")) query_heap_results();
    else if (!std::strcmp(argv[1], "--queries-default")) query_heap_results(true);
    else if (!std::strcmp(argv[1], "--queries-mixed")) query_heap_results(true, true);
    else if (!std::strcmp(argv[1], "--multi-list-queries")) multi_list_queries();
    else if (!std::strcmp(argv[1], "--uav-clear-uint")) uav_clear(false);
    else if (!std::strcmp(argv[1], "--uav-clear-float")) uav_clear(true);
    else if (!std::strcmp(argv[1], "--future-queue")) future_queue_wait();
    else if (!std::strcmp(argv[1], "--null-event")) null_event_wait();
    else if (!std::strcmp(argv[1], "--swapchain")) swapchain();
    else if (std::strstr(argv[1], "cold-single")) {
      native_render(true, "--dxbc-placed");
      native_render(true, "--dxbc-placed");
      native_render(true, argv[1]);
    }
    else if (!std::strcmp(argv[1], "--heap-offset")) std::printf("INFO placed source offset %llu validated on all queues\n", (unsigned long long)placed_test_offset);
    else if (std::strstr(argv[1], "compute")) compute_dispatch(argv[1]);
    else if (std::strstr(argv[1], "table64") || std::strstr(argv[1], "table128")) {
      for (UINT i = 0; i < 6; ++i) { std::printf("INFO table workload iteration %u\n", i); native_render(true, argv[1]); }
    } else native_render(std::strcmp(argv[1], "--clear") != 0, argv[1]);
  }
  drop(device);
  std::printf("%s D3D12 compatibility validation (%d failures)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
