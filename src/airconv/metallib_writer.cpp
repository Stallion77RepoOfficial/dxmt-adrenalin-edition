#include "metallib_writer.hpp"
#include "air_compat_text_writer.hpp"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/PrintPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <unistd.h>
#include <cstdarg>

using namespace llvm;

namespace dxmt::metallib {

template <typename T> std::string_view value(const T &value) {
  return std::string_view((const char *)(&value), sizeof(T));
};

static bool debugDumpEnabled() {
  return getenv("DXMT_AIR_DEBUG_DUMP") != nullptr;
}

// File-based diagnostics are only emitted when shader debug dumps are enabled.
static void airlog(const char *fmt, ...) {
  if (!debugDumpEnabled())
    return;

  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  // Try several locations; CrossOver's sandbox may block some of them.
  const char *home = getenv("HOME");
  std::string home_path = home ? std::string(home) + "/dxmt_airas.log" : "";
  const char *candidates[] = {
    "/tmp/dxmt_airas.log",
    home_path.empty() ? nullptr : home_path.c_str(),
    "/Users/Shared/dxmt_airas.log",
  };
  for (const char *path : candidates) {
    if (!path) continue;
    FILE *f = fopen(path, "a");
    if (f) {
      fprintf(f, "%s\n", msg);
      fclose(f);
    }
  }
}

// Returns path to air-as from Apple's Metal toolchain.
static std::string findAirAs() {
  // 1) Explicit override via env var.
  if (const char *env = getenv("DXMT_AIR_AS")) {
    airlog("findAirAs: DXMT_AIR_AS=%s", env);
    if (env[0] && access(env, X_OK) == 0)
      return env;
  }

  // 2) Ask xcrun (works when DEVELOPER_DIR / xcode-select are sane).
  {
    FILE *f = popen("/usr/bin/xcrun --find air-as 2>/dev/null", "r");
    airlog("findAirAs: popen(xcrun) -> %s", f ? "ok" : "NULL");
    if (f) {
      char buf[1024] = {};
      bool got = fgets(buf, sizeof(buf), f) != nullptr;
      pclose(f);
      airlog("findAirAs: xcrun result='%s'", got ? buf : "(empty)");
      if (got) {
        std::string s(buf);
        if (!s.empty() && s.back() == '\n') s.pop_back();
        if (!s.empty() && access(s.c_str(), X_OK) == 0)
          return s;
      }
    }
  }

  // 3) Glob the cryptex MetalToolchain mount directly. CrossOver's sandbox
  //    often lacks a usable xcrun, but the cryptex mount is still readable.
  {
    FILE *f = popen(
      "/bin/ls -1 "
      "/var/run/com.apple.security.cryptexd/mnt/"
      "com.apple.MobileAsset.MetalToolchain-*/Metal.xctoolchain/usr/bin/air-as "
      "2>/dev/null | /usr/bin/head -1",
      "r");
    airlog("findAirAs: popen(cryptex ls) -> %s", f ? "ok" : "NULL");
    if (f) {
      char buf[1024] = {};
      bool got = fgets(buf, sizeof(buf), f) != nullptr;
      pclose(f);
      airlog("findAirAs: cryptex result='%s'", got ? buf : "(empty)");
      if (got) {
        std::string s(buf);
        if (!s.empty() && s.back() == '\n') s.pop_back();
        if (!s.empty() && access(s.c_str(), X_OK) == 0)
          return s;
      }
    }
  }

  airlog("findAirAs: NOT FOUND; falling back to LLVM bitcode writer");
  return {};
}

// Compile LLVM IR text to AIR bitcode using Apple's air-as.
// Returns true and fills out_bc on success; falls back to WriteBitcodeToFile.
static bool compileWithAirAs(
    const llvm::Module &module,
    SmallVectorImpl<char> &out_bc) {

  airlog("compileWithAirAs: called");
  static const std::string air_as = findAirAs();
  if (air_as.empty()) return false;
  airlog("compileWithAirAs: using air-as=%s", air_as.c_str());

  // Unique temp names per call: same pid + multiple concurrent shader
  // compilations would otherwise clobber each other's files.
  static std::atomic<uint64_t> counter{0};
  uint64_t id = counter.fetch_add(1);
  char tmp_ll[256], tmp_bc[256];
  snprintf(tmp_ll, sizeof(tmp_ll), "/tmp/dxmt_airconv_%d_%llu.ll", (int)getpid(),
           (unsigned long long)id);
  snprintf(tmp_bc, sizeof(tmp_bc), "/tmp/dxmt_airconv_%d_%llu.bc", (int)getpid(),
           (unsigned long long)id);

  std::string ir_text, downgraded;
  {
    // Print LLVM22-dialect textual IR, then downgrade it to the Apple
    // AIR LLVM dialect that air-as understands (opaque pointers, memory()
    // attributes, splat constants, typed function-ref metadata, ...).
    {
      raw_string_ostream ir_stream(ir_text);
      module.print(ir_stream, nullptr);
    }
    std::string derr;
    if (!dxmt::aircompat::DowngradeToAppleAIR(ir_text, downgraded, derr)) {
      airlog("compileWithAirAs: downgrade FAILED: %s", derr.c_str());
      return false;
    }
    std::error_code ec;
    raw_fd_ostream ll_stream(tmp_ll, ec, sys::fs::OF_None);
    if (ec) return false;
    ll_stream << downgraded;
  }

  // Run air-as to produce AIR-compatible bitcode.
  std::string cmd = "'" + air_as + "' -o '" + tmp_bc + "' '" + tmp_ll + "'";
  if (debugDumpEnabled())
    cmd += " 2>>/tmp/dxmt_airas.log";
  else
    cmd += " 2>/dev/null";
  int ret = system(cmd.c_str());
  if (ret != 0) {
    airlog("compileWithAirAs: air-as FAILED ret=%d ll=%s", ret, tmp_ll);
    static std::atomic<int> saved{0};
    int n = saved.fetch_add(1);
    if (debugDumpEnabled() && n < 64) {
      system("/bin/mkdir -p /tmp/dxmt_fail 2>/dev/null");
      char p[256];
      snprintf(p, sizeof(p), "/tmp/dxmt_fail/shader_%d_%llu.air.ll", (int)getpid(),
               (unsigned long long)id);
      if (FILE *f = fopen(p, "w")) { fwrite(downgraded.data(), 1, downgraded.size(), f); fclose(f); }
    }
    remove(tmp_ll);
    remove(tmp_bc);
    return false;
  }
  remove(tmp_ll);

  // Read result.
  auto buf = MemoryBuffer::getFile(tmp_bc);
  remove(tmp_bc);
  if (!buf) {
    airlog("compileWithAirAs: air-as produced no output");
    return false;
  }

  auto &mb = *buf.get();
  out_bc.assign(mb.getBufferStart(), mb.getBufferEnd());
  airlog("compileWithAirAs: SUCCESS, %zu bytes", out_bc.size());

  {
    static std::atomic<int> okSaved{0};
    int n = okSaved.fetch_add(1);
    if (debugDumpEnabled() && n < 16) {
      system("/bin/mkdir -p /tmp/dxmt_ok 2>/dev/null");
      char p[256];
      snprintf(p, sizeof(p), "/tmp/dxmt_ok/ok_%d_%llu.air.ll", (int)getpid(),
               (unsigned long long)id);
      if (FILE *f = fopen(p, "w")) { fwrite(downgraded.data(), 1, downgraded.size(), f); fclose(f); }
      snprintf(p, sizeof(p), "/tmp/dxmt_ok/ok_%d_%llu.air", (int)getpid(),
               (unsigned long long)id);
      if (FILE *f = fopen(p, "wb")) { fwrite(out_bc.data(), 1, out_bc.size(), f); fclose(f); }
    }
  }
  return true;
}

struct InputAttribute {
  uint8_t attribute;
  std::string name;
  uint8_t type;
  // TODO: extend patch/control point here
};

void MetallibWriter::Write(const llvm::Module &module, raw_ostream &OS) {

  SmallVector<char, 0> bitcode;
  SmallVector<char, 0> public_metadata;
  SmallVector<char, 0> private_metadata;
  SmallVector<char, 0> function_def;

  uint32_t fn_count = 0;

  // Version fields must match what the AIR module actually declares, otherwise
  // Apple's loader/AGX backend can reject the package.
  uint8_t airMaj = 2, airMin = 6, langMaj = 3, langMin = 1;
  uint8_t osMaj = 14, osMin = 4;
  auto mdInt = [](const llvm::MDOperand &op, uint8_t fallback) -> uint8_t {
    if (auto *cam = dyn_cast_or_null<ConstantAsMetadata>(op.get()))
      if (auto *ci = dyn_cast<ConstantInt>(cam->getValue()))
        return (uint8_t)ci->getZExtValue();
    return fallback;
  };
  if (auto *v = module.getNamedMetadata("air.version")) {
    if (v->getNumOperands() && v->getOperand(0)->getNumOperands() >= 2) {
      auto *t = v->getOperand(0);
      airMaj = mdInt(t->getOperand(0), airMaj);
      airMin = mdInt(t->getOperand(1), airMin);
    }
  }
  if (auto *v = module.getNamedMetadata("air.language_version")) {
    if (v->getNumOperands() && v->getOperand(0)->getNumOperands() >= 3) {
      auto *t = v->getOperand(0);
      langMaj = mdInt(t->getOperand(1), langMaj);
      langMin = mdInt(t->getOperand(2), langMin);
    }
  }
  {
    std::string triple = module.getTargetTriple().str();
    size_t m = triple.find("macosx");
    if (m != std::string::npos) {
      int a = 0, b = 0;
      if (sscanf(triple.c_str() + m + 6, "%d.%d", &a, &b) >= 1) {
        osMaj = (uint8_t)a;
        osMin = (uint8_t)b;
      }
    }
  }

  airlog("MetallibWriter::Write called (new air-as build)");
  // Try to produce AIR-compatible bitcode via Apple's air-as.
  // This avoids the "Failed to upgrade function bitcode" error that occurs
  // when LLVM 22 binary bitcode is fed directly to Apple's Metal AGX reader.
  if (!compileWithAirAs(module, bitcode)) {
    raw_svector_ostream bitcode_stream(bitcode);
    WriteBitcodeToFile(module, bitcode_stream, false, nullptr, true);
  }

  auto hash =
    compute_sha256_hash((const uint8_t *)bitcode.data(), bitcode.size());

  raw_svector_ostream public_metadata_stream(public_metadata);
  raw_svector_ostream private_metadata_stream(private_metadata);

  {

    raw_svector_ostream function_def_stream(function_def);

    auto vertexFns = module.getNamedMetadata("air.vertex");
    if (vertexFns) {
      for (auto fn : vertexFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Vertex});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(MTLB_VERS_TAG{
          .airVersionMajor = airMaj,
          .airVersionMinor = airMin,
          .languageVersionMajor = langMaj,
          .languageVersionMinor = langMin,
        });
        while (fn->getNumOperands() > 3 && isa<MDTuple>(fn->getOperand(3).get())
        ) {
          auto maybe_patch_tuple = cast<MDTuple>(fn->getOperand(3).get());
          if (maybe_patch_tuple->getNumOperands() != 4)
            break;
          if (!isa<MDString>(maybe_patch_tuple->getOperand(0).get()))
            break;
          auto air_patch =
            cast<MDString>(maybe_patch_tuple->getOperand(0).get());
          if (air_patch->getString() != "air.patch")
            break;
          if (!isa<MDString>(maybe_patch_tuple->getOperand(1).get()))
            break;
          auto air_patch_value =
            cast<MDString>(maybe_patch_tuple->getOperand(1).get());
          if (air_patch_value->getString() == "triangle") {
            function_def_stream
              << value(MTLB_TESS_TAG{.patchType = 1, .controlPointCount = 0});
          } else {
            function_def_stream
              << value(MTLB_TESS_TAG{.patchType = 2, .controlPointCount = 0});
          }
          break;
        }
        function_def_stream << "ENDT";
        auto inputs = dyn_cast<MDTuple>(fn->getOperand(2).get());

        std::vector<InputAttribute> attributes;

        for (auto &input : inputs->operands()) {
          auto inputElement = dyn_cast<MDTuple>(input.get());
          auto inputKind =
            dyn_cast<MDString>(inputElement->getOperand(1))->getString();
          if (inputKind == "air.vertex_input") {
            uint32_t location =
              dyn_cast<ConstantInt>(
                dyn_cast<ConstantAsMetadata>(inputElement->getOperand(3))
                  ->getValue()
              )
                ->getValue()
                .getZExtValue();
            auto typeName =
              dyn_cast<MDString>(inputElement->getOperand(6))->getString();
            auto argName =
              dyn_cast<MDString>(inputElement->getOperand(8))->getString();
            attributes.push_back(InputAttribute{
              .attribute = (uint8_t)location,
              .name = (argName.str()),
              .type =
                (uint8_t)(typeName == "float4"  ? 0x06
                          : typeName == "uint4" ? 0x24
                                                : 0x20), // REFACTOR IT: hope it
                                                         // works in general?
            });
          }
        }
        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        if (attributes.size()) {
          // if no vertex attributes, then don't emit VATY, otherwise PSO
          // doesn't compile
          fn_public_metadata_stream << value(MTLBFourCC::VertexAttribute);
          auto lenOffset = fn_public_metadata_stream.tell();
          fn_public_metadata_stream << value((uint16_t)0);
          fn_public_metadata_stream << value((uint16_t)attributes.size());
          for (auto &vattr : attributes) {
            fn_public_metadata_stream << vattr.name << '\0';
            fn_public_metadata_stream << value(MTLB_VATY{
              .attribute = vattr.attribute,
              .__ = 0,
              .usage = 0,
              .active = 1,
            });
          }
          auto vatt_written = fn_public_metadata_stream.tell() - lenOffset;
          *(uint16_t *)(&fn_public_metadata[lenOffset]) = vatt_written - 2;
          fn_public_metadata_stream << value(MTLBFourCC::VertexAttributeType);
          fn_public_metadata_stream << value((uint16_t)(2 + attributes.size()));
          fn_public_metadata_stream << value((uint16_t)(attributes.size()));
          for (auto &vattr : attributes) {
            fn_public_metadata_stream << value(vattr.type);
          }
        }
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
    auto fragmentFns = module.getNamedMetadata("air.fragment");
    if (fragmentFns) {
      for (auto fn : fragmentFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Fragment});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(MTLB_VERS_TAG{
          .airVersionMajor = airMaj,
          .airVersionMinor = airMin,
          .languageVersionMajor = langMaj,
          .languageVersionMinor = langMin,
        });
        function_def_stream << "ENDT";
        public_metadata_stream << value(4);
        public_metadata_stream << "ENDT";
        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
    auto kernelFns = module.getNamedMetadata("air.kernel");
    if (kernelFns) {
      for (auto fn : kernelFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Kernel});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(MTLB_VERS_TAG{
          .airVersionMajor = airMaj,
          .airVersionMinor = airMin,
          .languageVersionMajor = langMaj,
          .languageVersionMinor = langMin,
        });
        function_def_stream << "ENDT";

        std::vector<InputAttribute> attributes;

        // auto inputs = dyn_cast<MDTuple>(fn->getOperand(2).get());
        // for (auto &input : inputs->operands()) {
        //   auto inputElement = dyn_cast<MDTuple>(input.get());
        //   auto inputKind =
        //     dyn_cast<MDString>(inputElement->getOperand(1))->getString();
        //   if (inputKind == "air.vertex_input") {
        //     uint32_t location =
        //       dyn_cast<ConstantInt>(
        //         dyn_cast<ConstantAsMetadata>(inputElement->getOperand(3))
        //           ->getValue()
        //       )
        //         ->getValue()
        //         .getZExtValue();
        //     auto typeName =
        //       dyn_cast<MDString>(inputElement->getOperand(6))->getString();
        //     auto argName =
        //       dyn_cast<MDString>(inputElement->getOperand(8))->getString();
        //     attributes.push_back(InputAttribute{
        //       .attribute = (uint8_t)location,
        //       .name = (argName.str()),
        //       .type =
        //         (uint8_t)(typeName == "float4"  ? 0x06
        //                   : typeName == "uint4" ? 0x24
        //                                         : 0x20), // REFACTOR IT: hope
        //                                         it
        //                                                  // works in general?
        //     });
        //   }
        // }
        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        // fn_public_metadata_stream << value(MTLBFourCC::VertexAttribute);
        // auto lenOffset = fn_public_metadata_stream.tell();
        // fn_public_metadata_stream << value((uint16_t)0);
        // fn_public_metadata_stream << value((uint16_t)attributes.size());
        // for (auto &vattr : attributes) {
        //   fn_public_metadata_stream << vattr.name << '\0';
        //   fn_public_metadata_stream << value(MTLB_VATY{
        //     .attribute = vattr.attribute,
        //     .__ = 0,
        //     .usage = 0,
        //     .active = 1,
        //   });
        // }
        // auto vatt_written = fn_public_metadata_stream.tell() - lenOffset;
        // *(uint16_t *)(&fn_public_metadata[lenOffset]) = vatt_written - 2;
        // fn_public_metadata_stream << value(MTLBFourCC::VertexAttributeType);
        // fn_public_metadata_stream << value((uint16_t)(2 +
        // attributes.size())); fn_public_metadata_stream <<
        // value((uint16_t)(attributes.size())); for (auto &vattr : attributes)
        // {
        //   fn_public_metadata_stream << value(vattr.type);
        // }
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }

    auto objectFns = module.getNamedMetadata("air.object");
    if (objectFns) {
      for (auto fn : objectFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Object});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(MTLB_VERS_TAG{
          .airVersionMajor = airMaj,
          .airVersionMinor = airMin,
          .languageVersionMajor = langMaj,
          .languageVersionMinor = langMin,
        });
        function_def_stream << "ENDT";

        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }

    auto meshFns = module.getNamedMetadata("air.mesh");
    if (meshFns) {
      for (auto fn : meshFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Mesh});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(MTLB_VERS_TAG{
          .airVersionMajor = airMaj,
          .airVersionMinor = airMin,
          .languageVersionMajor = langMaj,
          .languageVersionMinor = langMin,
        });
        function_def_stream << "ENDT";

        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
  }

  MTLBHeader header;
  header.Magic = MTLB_Magic;
  header.FileSize =
    sizeof(MTLBHeader) + sizeof(uint32_t) /* fn count */ +
    sizeof(uint32_t) /* constant: function list size */ + function_def.size() +
    sizeof(MTLBFourCC::EndTag) /* extended header*/
    + public_metadata.size() + private_metadata.size() + bitcode.size();
  header.FunctionListOffset = sizeof(MTLBHeader);
  header.FunctionListSize = function_def.size() + 4;
  header.PublicMetadataOffset =
    header.FunctionListOffset + header.FunctionListSize +
    sizeof(uint32_t) // extra room for function count
    + sizeof(MTLBFourCC::EndTag);
  header.PublicMetadataSize = public_metadata.size();
  header.PrivateMetadataOffset =
    header.PublicMetadataOffset + header.PublicMetadataSize;
  header.PrivateMetadataSize = private_metadata.size();
  header.BitcodeOffset =
    header.PrivateMetadataOffset + header.PrivateMetadataSize;
  header.BitcodeSize = bitcode.size();

  header.Type = FileType::MTLBType_Executable; // executable
  header.Platform = Platform::MTLBPlatform_macOS;
  header.VersionMajor = 2;
  header.VersionMinor = 7;
  header.OS = OS::MTLBOS_macOS;
  header.OSVersionMajor = osMaj;
  header.OSVersionMinor = osMin;

  // write to stream
  OS << value(header);
  OS << value(fn_count);
  OS << value((uint32_t)header.FunctionListSize);
  OS << function_def;
  OS.write("ENDT", 4); // extend header
  OS << public_metadata;
  OS << private_metadata;
  OS << bitcode;
}

} // namespace dxmt::metallib
