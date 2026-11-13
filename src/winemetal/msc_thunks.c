#ifdef _WIN32
#define WINEMETAL_API __declspec(dllexport)
#else
#define WINEMETAL_API
#endif

#include "msc_thunks.h"
#include <wineunixlib.h>
#include "assert.h"

#ifdef NDEBUG
#define UNIX_CALL(code, params) WINE_UNIX_CALL(code, params)
#else
#define UNIX_CALL(code, params)                                                                                        \
  {                                                                                                                    \
    NTSTATUS status = WINE_UNIX_CALL(code, params);                                                                    \
    assert(!status && "unix call failed");                                                                             \
  }
#endif

WINEMETAL_API bool
WMTShaderConverterIsAvailable(void) {
  struct unixcall_msc_is_available params;
  params.ret = 0;
  UNIX_CALL(unix_msc_is_available, &params);
  return params.ret != 0;
}

WINEMETAL_API obj_handle_t
WMTShaderConverterCreateRootSignature(struct WMTConstMemoryPointer desc, obj_handle_t *err_out) {
  struct unixcall_msc_create_root_signature params;
  params.desc = desc;
  params.ret_error = NULL_OBJECT_HANDLE;
  params.ret = NULL_OBJECT_HANDLE;
  UNIX_CALL(unix_msc_create_root_signature, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret;
}

WINEMETAL_API void
WMTShaderConverterReleaseRootSignature(obj_handle_t root_signature) {
  UNIX_CALL(unix_msc_release_root_signature, &root_signature);
}

WINEMETAL_API int
WMTShaderConverterConvert(struct WMTShaderConverterInfo *info, obj_handle_t *err_out) {
  struct unixcall_msc_convert params;
  WMT_MEMPTR_SET(params.info, info);
  params.ret_error = NULL_OBJECT_HANDLE;
  params.ret = -1;
  UNIX_CALL(unix_msc_convert, &params);
  if (err_out)
    *err_out = params.ret_error;
  return params.ret;
}

WINEMETAL_API uint64_t
WMTShaderConverterCopyReflectionJSON(obj_handle_t reflection, char *buffer, uint64_t buffer_size) {
  struct unixcall_msc_copy_string params;
  params.handle = reflection;
  WMT_MEMPTR_SET(params.buffer, buffer);
  params.buffer_size = buffer_size;
  params.ret_size = 0;
  UNIX_CALL(unix_msc_copy_reflection_json, &params);
  return params.ret_size;
}

WINEMETAL_API bool
WMTShaderConverterCopyComputeInfo(obj_handle_t reflection, uint32_t threadgroup_size[3]) {
  struct unixcall_msc_copy_compute_info params;
  params.handle = reflection;
  params.ret_threadgroup_size[0] = 0;
  params.ret_threadgroup_size[1] = 0;
  params.ret_threadgroup_size[2] = 0;
  params.ret = 0;
  UNIX_CALL(unix_msc_copy_compute_info, &params);
  if (!params.ret)
    return false;
  threadgroup_size[0] = params.ret_threadgroup_size[0];
  threadgroup_size[1] = params.ret_threadgroup_size[1];
  threadgroup_size[2] = params.ret_threadgroup_size[2];
  return true;
}

WINEMETAL_API void
WMTShaderConverterReleaseReflection(obj_handle_t reflection) {
  UNIX_CALL(unix_msc_release_reflection, &reflection);
}

WINEMETAL_API uint64_t
WMTShaderConverterCopyErrorMessage(obj_handle_t error, char *buffer, uint64_t buffer_size) {
  struct unixcall_msc_copy_string params;
  params.handle = error;
  WMT_MEMPTR_SET(params.buffer, buffer);
  params.buffer_size = buffer_size;
  params.ret_size = 0;
  UNIX_CALL(unix_msc_copy_error_message, &params);
  return params.ret_size;
}

WINEMETAL_API void
WMTShaderConverterReleaseError(obj_handle_t error) {
  UNIX_CALL(unix_msc_release_error, &error);
}
