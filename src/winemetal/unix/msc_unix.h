#ifndef _WINEMETAL_UNIX_MSC_UNIX_H
#define _WINEMETAL_UNIX_MSC_UNIX_H

/* Unix-side entry points for the Metal Shader Converter bridge. Registered in
 * __wine_unix_call_funcs by winemetal_unix.c; see msc_thunks.h for the codes. */

int _WMTShaderConverterIsAvailable(void *obj);
int _WMTShaderConverterCreateRootSignature(void *obj);
int _WMTShaderConverterReleaseRootSignature(void *obj);
int _WMTShaderConverterConvert(void *obj);
int _WMTShaderConverterCopyReflectionJSON(void *obj);
int _WMTShaderConverterCopyComputeInfo(void *obj);
int _WMTShaderConverterReleaseReflection(void *obj);
int _WMTShaderConverterCopyErrorMessage(void *obj);
int _WMTShaderConverterReleaseError(void *obj);

#endif
