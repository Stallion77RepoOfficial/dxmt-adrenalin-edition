#ifndef _WINEMETAL_MSC_THUNKS_H
#define _WINEMETAL_MSC_THUNKS_H

#include "msc_public.h"

/* Continues the numbering of __wine_unix_call_funcs in unix/winemetal_unix.c.
 * The last entry before these was _WMTEnableDeveloperHUD at 132; the codes are
 * an ABI, so append only. */
enum msc_unixcalls {
  unix_msc_is_available = 133,
  unix_msc_create_root_signature = 134,
  unix_msc_release_root_signature = 135,
  unix_msc_convert = 136,
  unix_msc_copy_reflection_json = 137,
  unix_msc_release_reflection = 138,
  unix_msc_copy_error_message = 139,
  unix_msc_release_error = 140,
  /* 141-147 are heap and vertex-attribute calls in winemetal_thunks.h. */
  unix_msc_copy_compute_info = 148,
};

struct unixcall_msc_is_available {
  uint64_t ret;
};

struct unixcall_msc_create_root_signature {
  struct WMTConstMemoryPointer desc;
  obj_handle_t ret_error;
  obj_handle_t ret;
};

struct unixcall_msc_convert {
  struct WMTMemoryPointer info; /* struct WMTShaderConverterInfo *, inout */
  obj_handle_t ret_error;
  int32_t ret;
  uint32_t padding;
};

struct unixcall_msc_copy_string {
  obj_handle_t handle;
  struct WMTMemoryPointer buffer;
  uint64_t buffer_size;
  uint64_t ret_size;
};

struct unixcall_msc_copy_compute_info {
  obj_handle_t handle;
  uint32_t ret_threadgroup_size[3];
  uint32_t ret; /* nonzero when the reflection had compute info */
};

STATIC_ASSERT(sizeof(unixcall_msc_create_root_signature) == 24);
STATIC_ASSERT(sizeof(unixcall_msc_copy_compute_info) == 24);
STATIC_ASSERT(sizeof(unixcall_msc_convert) == 24);
STATIC_ASSERT(sizeof(unixcall_msc_copy_string) == 32);

#endif
