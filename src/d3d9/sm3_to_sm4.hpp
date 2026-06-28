#pragma once
#include <cstdint>
#include <vector>

namespace dxmt::d3d9 {

// Converts SM3 (D3D9) shader bytecode to SM4 DXBC (D3D11-compatible).
// pFunction points to the D3D9 bytecode stream, dwCount is the number of DWORDs.
// Returns the DXBC container bytes, or empty vector on failure.
std::vector<uint8_t> ConvertSM3ToDXBC(const uint32_t *pFunction, size_t dwCount);

} // namespace dxmt::d3d9
