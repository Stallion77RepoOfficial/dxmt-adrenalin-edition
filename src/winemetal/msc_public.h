#ifndef _WINEMETAL_MSC_PUBLIC_H
#define _WINEMETAL_MSC_PUBLIC_H

#include "winemetal.h"

/* Bridge to Apple's Metal Shader Converter (libmetalirconverter.dylib), which
 * translates DXIL into Metal IR. It lives on the unix side because it is a
 * native dylib, so every entry point here is a unix call.
 *
 * The library is an optional runtime dependency: call
 * WMTShaderConverterIsAvailable() once and fall back if it returns false. */

enum WMTShaderConverterStage : uint32_t {
  WMTShaderConverterStageInvalid = 0,
  WMTShaderConverterStageVertex = 1,
  WMTShaderConverterStageFragment = 2,
  WMTShaderConverterStageHull = 3,
  WMTShaderConverterStageDomain = 4,
  WMTShaderConverterStageMesh = 5,
  WMTShaderConverterStageAmplification = 6,
  WMTShaderConverterStageGeometry = 7,
  WMTShaderConverterStageCompute = 8,
  WMTShaderConverterStageClosestHit = 9,
  WMTShaderConverterStageIntersection = 10,
  WMTShaderConverterStageAnyHit = 11,
  WMTShaderConverterStageMiss = 12,
  WMTShaderConverterStageRayGeneration = 13,
  WMTShaderConverterStageCallable = 14,
  WMTShaderConverterStageStreamOut = 15,
  WMTShaderConverterStageStageIn = 16,
};

enum WMTShaderConverterFlag : uint32_t {
  /* Emit a separate visible stage-in function instead of relying on Metal
   * vertex fetch. Required for the geometry/tessellation emulation pipelines. */
  WMTShaderConverterFlagSeparateStageInFunction = 1u << 0,
  /* Ignore the root signature embedded in the DXIL and use the one passed in
   * WMTShaderConverterInfo::root_signature. */
  WMTShaderConverterFlagIgnoreEmbeddedRootSignature = 1u << 1,
  WMTShaderConverterFlagIgnoreDebugInformation = 1u << 2,
};

enum WMTShaderConverterOS : uint8_t {
  WMTShaderConverterOSDefault = 0xff,
  WMTShaderConverterOSMacOS = 0,
  WMTShaderConverterOSIOS = 1,
  WMTShaderConverterOSTvOS = 2,
  WMTShaderConverterOSIOSSimulator = 3,
};

struct WMTShaderConverterInfo {
  /* in */
  struct WMTConstMemoryPointer dxil;
  uint64_t dxil_size;
  /* NUL-terminated, optional. NULL compiles the DXIL's own entry point. */
  struct WMTConstMemoryPointer entry_point;
  /* NUL-terminated, optional. Renames the emitted Metal function. */
  struct WMTConstMemoryPointer rename_entry_point;
  /* 0 uses the root signature embedded in the DXIL. */
  obj_handle_t root_signature;
  /* IRCompatibilityFlags bits, passed through verbatim. */
  uint32_t compat_flags;
  /* WMTShaderConverterFlag bits. */
  uint32_t flags;
  /* IRGPUFamily, 0 leaves the converter's default in place. */
  uint32_t min_gpu_family;
  /* WMTShaderConverterOSDefault leaves the converter's default in place. */
  uint8_t deployment_os;
  uint8_t deployment_major;
  uint8_t deployment_minor;
  uint8_t deployment_patch;

  /* out */
  /* dispatch_data_t holding the metallib. Feed it straight to
   * MTLDevice_newLibrary, then NSObject_release it. The bytes never cross into
   * PE memory. */
  obj_handle_t metallib;
  /* IRShaderReflection. Release with WMTShaderConverterReleaseReflection. */
  obj_handle_t reflection;
  uint32_t stage; /* enum WMTShaderConverterStage */
  uint32_t padding;
};

STATIC_ASSERT(sizeof(WMTShaderConverterInfo) == 80);

WINEMETAL_API bool WMTShaderConverterIsAvailable(void);

/* Returns 0 on success. On failure returns a negative value and, when err_out
 * is non-NULL, stores an error handle to inspect with
 * WMTShaderConverterCopyErrorMessage. */
WINEMETAL_API int WMTShaderConverterConvert(struct WMTShaderConverterInfo *info, obj_handle_t *err_out);

/* desc points at an IRVersionedRootSignatureDescriptor. Its layout matches
 * D3D12_VERSIONED_ROOT_SIGNATURE_DESC, so a deserialized D3D12 root signature
 * can be passed through as-is.
 *
 * The descriptor's nested parameter and static-sampler arrays are followed by
 * raw pointer, which only works when the PE and unix sides share a pointer
 * width. D3D12 is 64-bit only, so this is not a restriction in practice; a
 * 32-bit caller must not use this entry point. */
WINEMETAL_API obj_handle_t WMTShaderConverterCreateRootSignature(
    struct WMTConstMemoryPointer desc, obj_handle_t *err_out
);
WINEMETAL_API void WMTShaderConverterReleaseRootSignature(obj_handle_t root_signature);

/* Writes at most buffer_size bytes (NUL-terminated) and returns the length the
 * JSON would need, excluding the terminator. Pass a NULL buffer to size it. */
WINEMETAL_API uint64_t WMTShaderConverterCopyReflectionJSON(obj_handle_t reflection, char *buffer, uint64_t buffer_size);

/* The [numthreads] the shader declared. Metal takes the threadgroup size at
 * dispatch rather than baking it into the pipeline the way D3D12 does, so a
 * compute dispatch cannot be encoded without it. Returns false when the
 * reflection is not for a compute stage. */
WINEMETAL_API bool WMTShaderConverterCopyComputeInfo(obj_handle_t reflection, uint32_t threadgroup_size[3]);

WINEMETAL_API void WMTShaderConverterReleaseReflection(obj_handle_t reflection);

WINEMETAL_API uint64_t WMTShaderConverterCopyErrorMessage(obj_handle_t error, char *buffer, uint64_t buffer_size);
WINEMETAL_API void WMTShaderConverterReleaseError(obj_handle_t error);

#endif
