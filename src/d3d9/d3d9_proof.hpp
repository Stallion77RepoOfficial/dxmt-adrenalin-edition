#pragma once
// TEMP DIAGNOSTIC: unconditional file marker, independent of DXMT_LOG_PATH.
#include <cstdio>

namespace dxmt {
inline void d3d9_proof(const char *msg, long hr = 0) {
  if (FILE *f = fopen("dxmt_d3d9_PROOF.txt", "a")) {
    if (hr)
      fprintf(f, "%s hr=0x%08lx\n", msg, (unsigned long)hr);
    else
      fprintf(f, "%s\n", msg);
    fclose(f);
  }
}
} // namespace dxmt
