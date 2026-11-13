#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#import <Foundation/Foundation.h>

#define WINEMETAL_API
#include "../msc_thunks.h"
#include "msc_unix.h"

typedef int NTSTATUS;
#define STATUS_SUCCESS 0

/* Metal Shader Converter is an optional runtime dependency, so it is loaded by
 * dlopen and described by local declarations instead of its SDK headers. Only
 * the subset DXMT needs is bound; the enum values below are copied from
 * metal_irconverter.h and are part of that library's ABI. */

typedef struct IRObject IRObject;
typedef struct IRCompiler IRCompiler;
typedef struct IRError IRError;
typedef struct IRRootSignature IRRootSignature;
typedef struct IRMetalLibBinary IRMetalLibBinary;
typedef struct IRShaderReflection IRShaderReflection;

/* IRVersionedCSInfo from metal_irconverter.h. Mirrored rather than included so
 * this file still builds where the converter's headers are not installed; the
 * dylib is loaded by name at runtime for the same reason. */
#define IR_REFLECTION_VERSION_1_0 1
typedef struct IRVersionedCSInfo {
  int version;
  union {
    struct {
      uint32_t tg_size[3];
    } info_1_0;
  };
} IRVersionedCSInfo;

#define IR_BYTECODE_OWNERSHIP_NONE 0
#define IR_STAGE_IN_USE_METAL_VERTEX_FETCH 0
#define IR_STAGE_IN_USE_SEPARATE_FUNCTION 1

static struct {
  void *handle;

  IRObject *(*ObjectCreateFromDXIL)(const uint8_t *bytecode, size_t size, int ownership);
  void (*ObjectDestroy)(IRObject *object);
  int (*ObjectGetMetalIRShaderStage)(const IRObject *object);
  bool (*ObjectGetMetalLibBinary)(const IRObject *object, int stage, IRMetalLibBinary *lib);
  bool (*ObjectGetReflection)(const IRObject *object, int stage, IRShaderReflection *reflection);

  IRCompiler *(*CompilerCreate)(void);
  void (*CompilerDestroy)(IRCompiler *compiler);
  void (*CompilerSetEntryPointName)(IRCompiler *compiler, const char *name);
  void (*CompilerSetGlobalRootSignature)(IRCompiler *compiler, const IRRootSignature *root_signature);
  void (*CompilerSetCompatibilityFlags)(IRCompiler *compiler, int flags);
  void (*CompilerSetMinimumDeploymentTarget)(IRCompiler *compiler, int operating_system, const char *version);
  void (*CompilerSetMinimumGPUFamily)(IRCompiler *compiler, int family);
  void (*CompilerSetStageInGenerationMode)(IRCompiler *compiler, int mode);
  void (*CompilerIgnoreRootSignature)(IRCompiler *compiler, bool ignore);
  void (*CompilerIgnoreDebugInformation)(IRCompiler *compiler, bool ignore);
  IRObject *(*CompilerAllocCompileAndLink)(
      IRCompiler *compiler, const char *entry_point, const IRObject *input, IRError **error
  );

  IRMetalLibBinary *(*MetalLibBinaryCreate)(void);
  void (*MetalLibBinaryDestroy)(IRMetalLibBinary *lib);
  dispatch_data_t (*MetalLibGetBytecodeData)(const IRMetalLibBinary *lib);

  IRShaderReflection *(*ShaderReflectionCreate)(void);
  bool (*ShaderReflectionCopyComputeInfo)(const IRShaderReflection *reflection, int version, IRVersionedCSInfo *csinfo);
  bool (*ShaderReflectionReleaseComputeInfo)(IRVersionedCSInfo *csinfo);
  void (*ShaderReflectionDestroy)(IRShaderReflection *reflection);
  const char *(*ShaderReflectionCopyJSONString)(const IRShaderReflection *reflection);
  void (*ShaderReflectionReleaseString)(const char *serialized);

  IRRootSignature *(*RootSignatureCreateFromDescriptor)(const void *descriptor, IRError **error);
  void (*RootSignatureDestroy)(IRRootSignature *root_signature);

  uint32_t (*ErrorGetCode)(const IRError *error);
  const void *(*ErrorGetPayload)(const IRError *error);
  void (*ErrorDestroy)(IRError *error);
} msc;

static pthread_once_t msc_load_once = PTHREAD_ONCE_INIT;

static void
msc_load(void) {
  const char *path = getenv("DXMT_METAL_IRCONVERTER_PATH");
  if (!path || !*path)
    path = "/usr/local/lib/libmetalirconverter.dylib";

  msc.handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
  if (!msc.handle)
    msc.handle = dlopen("libmetalirconverter.dylib", RTLD_LAZY | RTLD_LOCAL);
  if (!msc.handle)
    return;

#define BIND(field, symbol)                                                                                            \
  msc.field = dlsym(msc.handle, symbol);                                                                               \
  if (!msc.field)                                                                                                      \
    goto missing_symbol;

  BIND(ObjectCreateFromDXIL, "IRObjectCreateFromDXIL")
  BIND(ObjectDestroy, "IRObjectDestroy")
  BIND(ObjectGetMetalIRShaderStage, "IRObjectGetMetalIRShaderStage")
  BIND(ObjectGetMetalLibBinary, "IRObjectGetMetalLibBinary")
  BIND(ObjectGetReflection, "IRObjectGetReflection")
  BIND(CompilerCreate, "IRCompilerCreate")
  BIND(CompilerDestroy, "IRCompilerDestroy")
  BIND(CompilerSetEntryPointName, "IRCompilerSetEntryPointName")
  BIND(CompilerSetGlobalRootSignature, "IRCompilerSetGlobalRootSignature")
  BIND(CompilerSetCompatibilityFlags, "IRCompilerSetCompatibilityFlags")
  BIND(CompilerSetMinimumDeploymentTarget, "IRCompilerSetMinimumDeploymentTarget")
  BIND(CompilerSetMinimumGPUFamily, "IRCompilerSetMinimumGPUFamily")
  BIND(CompilerSetStageInGenerationMode, "IRCompilerSetStageInGenerationMode")
  BIND(CompilerIgnoreRootSignature, "IRCompilerIgnoreRootSignature")
  BIND(CompilerIgnoreDebugInformation, "IRCompilerIgnoreDebugInformation")
  BIND(CompilerAllocCompileAndLink, "IRCompilerAllocCompileAndLink")
  BIND(MetalLibBinaryCreate, "IRMetalLibBinaryCreate")
  BIND(MetalLibBinaryDestroy, "IRMetalLibBinaryDestroy")
  BIND(MetalLibGetBytecodeData, "IRMetalLibGetBytecodeData")
  BIND(ShaderReflectionCreate, "IRShaderReflectionCreate")
  BIND(ShaderReflectionCopyComputeInfo, "IRShaderReflectionCopyComputeInfo")
  BIND(ShaderReflectionReleaseComputeInfo, "IRShaderReflectionReleaseComputeInfo")
  BIND(ShaderReflectionDestroy, "IRShaderReflectionDestroy")
  BIND(ShaderReflectionCopyJSONString, "IRShaderReflectionCopyJSONString")
  BIND(ShaderReflectionReleaseString, "IRShaderReflectionReleaseString")
  BIND(RootSignatureCreateFromDescriptor, "IRRootSignatureCreateFromDescriptor")
  BIND(RootSignatureDestroy, "IRRootSignatureDestroy")
  BIND(ErrorGetCode, "IRErrorGetCode")
  BIND(ErrorGetPayload, "IRErrorGetPayload")
  BIND(ErrorDestroy, "IRErrorDestroy")

#undef BIND
  return;

missing_symbol:
  /* A partially bound library is worse than none: fail the availability check
   * so callers take their fallback path. */
  dlclose(msc.handle);
  memset(&msc, 0, sizeof(msc));
}

static bool
msc_available(void) {
  pthread_once(&msc_load_once, msc_load);
  return msc.handle != NULL;
}

static uint64_t
msc_copy_string(struct unixcall_msc_copy_string *params, const char *str) {
  size_t length = strlen(str);
  char *buffer = params->buffer.ptr;

  if (buffer && params->buffer_size) {
    size_t copied = length < params->buffer_size - 1 ? length : params->buffer_size - 1;
    memcpy(buffer, str, copied);
    buffer[copied] = '\0';
  }
  return length;
}

NTSTATUS
_WMTShaderConverterIsAvailable(void *obj) {
  struct unixcall_msc_is_available *params = obj;
  params->ret = msc_available() ? 1 : 0;
  return STATUS_SUCCESS;
}

NTSTATUS
_WMTShaderConverterCreateRootSignature(void *obj) {
  struct unixcall_msc_create_root_signature *params = obj;
  IRError *error = NULL;

  params->ret = 0;
  params->ret_error = 0;

  if (!msc_available())
    return STATUS_SUCCESS;

  params->ret = (obj_handle_t)msc.RootSignatureCreateFromDescriptor(params->desc.ptr, &error);
  params->ret_error = (obj_handle_t)error;
  return STATUS_SUCCESS;
}

NTSTATUS
_WMTShaderConverterReleaseRootSignature(void *obj) {
  obj_handle_t *handle = obj;

  if (*handle && msc_available())
    msc.RootSignatureDestroy((IRRootSignature *)*handle);
  return STATUS_SUCCESS;
}

NTSTATUS
_WMTShaderConverterConvert(void *obj) {
  struct unixcall_msc_convert *params = obj;
  struct WMTShaderConverterInfo *info = params->info.ptr;
  IRObject *input = NULL;
  IRObject *output = NULL;
  IRCompiler *compiler = NULL;
  IRMetalLibBinary *lib = NULL;
  IRShaderReflection *reflection = NULL;
  IRError *error = NULL;
  dispatch_data_t bytecode;
  int stage;

  params->ret = -1;
  params->ret_error = 0;
  info->metallib = 0;
  info->reflection = 0;
  info->stage = WMTShaderConverterStageInvalid;

  if (!msc_available())
    return STATUS_SUCCESS;

  input = msc.ObjectCreateFromDXIL(info->dxil.ptr, info->dxil_size, IR_BYTECODE_OWNERSHIP_NONE);
  if (!input)
    return STATUS_SUCCESS;

  compiler = msc.CompilerCreate();
  if (!compiler)
    goto cleanup;

  if (info->rename_entry_point.ptr)
    msc.CompilerSetEntryPointName(compiler, info->rename_entry_point.ptr);
  if (info->root_signature)
    msc.CompilerSetGlobalRootSignature(compiler, (const IRRootSignature *)info->root_signature);
  if (info->compat_flags)
    msc.CompilerSetCompatibilityFlags(compiler, (int)info->compat_flags);
  if (info->min_gpu_family)
    msc.CompilerSetMinimumGPUFamily(compiler, (int)info->min_gpu_family);
  if (info->deployment_os != WMTShaderConverterOSDefault) {
    char version[32];
    snprintf(
        version, sizeof(version), "%u.%u.%u", info->deployment_major, info->deployment_minor, info->deployment_patch
    );
    msc.CompilerSetMinimumDeploymentTarget(compiler, info->deployment_os, version);
  }
  msc.CompilerSetStageInGenerationMode(
      compiler, info->flags & WMTShaderConverterFlagSeparateStageInFunction ? IR_STAGE_IN_USE_SEPARATE_FUNCTION
                                                                            : IR_STAGE_IN_USE_METAL_VERTEX_FETCH
  );
  if (info->flags & WMTShaderConverterFlagIgnoreEmbeddedRootSignature)
    msc.CompilerIgnoreRootSignature(compiler, true);
  if (info->flags & WMTShaderConverterFlagIgnoreDebugInformation)
    msc.CompilerIgnoreDebugInformation(compiler, true);

  output = msc.CompilerAllocCompileAndLink(compiler, info->entry_point.ptr, input, &error);
  if (!output) {
    params->ret_error = (obj_handle_t)error;
    goto cleanup;
  }

  stage = msc.ObjectGetMetalIRShaderStage(output);

  lib = msc.MetalLibBinaryCreate();
  if (!lib || !msc.ObjectGetMetalLibBinary(output, stage, lib))
    goto cleanup;

  /* The converter documents this pointer as owned by the metallib binary, so it
   * has to be retained before the binary is destroyed below. The PE side gives
   * it back with NSObject_release. */
  bytecode = msc.MetalLibGetBytecodeData(lib);
  if (!bytecode)
    goto cleanup;
  [(id)bytecode retain];

  reflection = msc.ShaderReflectionCreate();
  if (reflection && !msc.ObjectGetReflection(output, stage, reflection)) {
    msc.ShaderReflectionDestroy(reflection);
    reflection = NULL;
  }

  info->metallib = (obj_handle_t)bytecode;
  info->reflection = (obj_handle_t)reflection;
  /* WMTShaderConverterStage mirrors IRShaderStage one for one. */
  info->stage = (uint32_t)stage;
  params->ret = 0;

cleanup:
  if (lib)
    msc.MetalLibBinaryDestroy(lib);
  if (output)
    msc.ObjectDestroy(output);
  if (compiler)
    msc.CompilerDestroy(compiler);
  msc.ObjectDestroy(input);
  return STATUS_SUCCESS;
}

NTSTATUS
_WMTShaderConverterCopyReflectionJSON(void *obj) {
  struct unixcall_msc_copy_string *params = obj;
  const char *json;

  params->ret_size = 0;

  if (!params->handle || !msc_available())
    return STATUS_SUCCESS;

  json = msc.ShaderReflectionCopyJSONString((const IRShaderReflection *)params->handle);
  if (!json)
    return STATUS_SUCCESS;

  params->ret_size = msc_copy_string(params, json);
  msc.ShaderReflectionReleaseString(json);
  return STATUS_SUCCESS;
}

NTSTATUS
_WMTShaderConverterCopyComputeInfo(void *obj) {
  struct unixcall_msc_copy_compute_info *params = obj;
  IRVersionedCSInfo info;

  params->ret = 0;
  params->ret_threadgroup_size[0] = 0;
  params->ret_threadgroup_size[1] = 0;
  params->ret_threadgroup_size[2] = 0;

  if (!params->handle || !msc_available())
    return STATUS_SUCCESS;

  memset(&info, 0, sizeof(info));
  if (!msc.ShaderReflectionCopyComputeInfo(
          (const IRShaderReflection *)params->handle, IR_REFLECTION_VERSION_1_0, &info
      ))
    return STATUS_SUCCESS;

  params->ret_threadgroup_size[0] = info.info_1_0.tg_size[0];
  params->ret_threadgroup_size[1] = info.info_1_0.tg_size[1];
  params->ret_threadgroup_size[2] = info.info_1_0.tg_size[2];
  params->ret = 1;
  msc.ShaderReflectionReleaseComputeInfo(&info);
  return STATUS_SUCCESS;
}

NTSTATUS
_WMTShaderConverterReleaseReflection(void *obj) {
  obj_handle_t *handle = obj;

  if (*handle && msc_available())
    msc.ShaderReflectionDestroy((IRShaderReflection *)*handle);
  return STATUS_SUCCESS;
}

NTSTATUS
_WMTShaderConverterCopyErrorMessage(void *obj) {
  struct unixcall_msc_copy_string *params = obj;
  const IRError *error = (const IRError *)params->handle;
  const char *payload;
  char message[512];

  params->ret_size = 0;

  if (!error || !msc_available())
    return STATUS_SUCCESS;

  /* The converter documents the payload of a compilation error as a
   * NUL-terminated message; the code is always meaningful, so it is reported
   * either way. */
  payload = msc.ErrorGetPayload(error);
  snprintf(message, sizeof(message), "IRError %u: %s", msc.ErrorGetCode(error), payload ? payload : "(no payload)");

  params->ret_size = msc_copy_string(params, message);
  return STATUS_SUCCESS;
}

NTSTATUS
_WMTShaderConverterReleaseError(void *obj) {
  obj_handle_t *handle = obj;

  if (*handle && msc_available())
    msc.ErrorDestroy((IRError *)*handle);
  return STATUS_SUCCESS;
}
