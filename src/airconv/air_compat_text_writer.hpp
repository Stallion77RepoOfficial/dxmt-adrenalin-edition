#pragma once

#include <string>

namespace dxmt::aircompat {

// Downgrade LLVM22-dialect textual IR (as produced by llvm::Module::print)
// into the Apple AIR LLVM dialect that Apple's `air-as` accepts.
//
// The transformation covers:
//   - opaque pointers          ptr addrspace(N)      -> <pointee> addrspace(N)*
//   - function-ref metadata    !{ptr @fn, ...}       -> !{<sig>* @fn, ...}
//   - memory() attributes      memory(argmem: read)  -> argmemonly readonly
//   - capture attributes       captures(none)        -> nocapture
//   - splat constants          splat (T C)           -> <T C, T C, ...>
//   - injected opaque structs  texture/sampler handle struct decls
//
// Returns true on success and fills `out`. On failure fills `err` and returns
// false; callers should fall back to the legacy bitcode writer.
bool DowngradeToAppleAIR(const std::string &in, std::string &out,
                         std::string &err);

} // namespace dxmt::aircompat
