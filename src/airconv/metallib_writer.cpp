#include "metallib_writer.hpp"
#include "air_compat_text_writer.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/IR/PrintPasses.h"
#include "llvm/IR/TypeFinder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>

#ifdef __APPLE__
#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <mach/machine.h>
#include <spawn.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace llvm;

namespace dxmt::metallib {

template <typename T> std::string_view value(const T &value) {
  return std::string_view((const char *)(&value), sizeof(T));
};

#ifdef __APPLE__

// Spawn a process directly (no shell), wait for it, and optionally capture its
// stdout or stderr. The other output stream is discarded. Returns the exit
// code, or -1 if the process could not be spawned or terminated abnormally.
/* True on Apple silicon, whether or not this process is being translated. */
static bool hostIsAppleSilicon() {
  static const bool value = [] {
    int has_arm64 = 0;
    size_t size = sizeof(has_arm64);
    if (sysctlbyname("hw.optional.arm64", &has_arm64, &size, nullptr, 0) != 0)
      return false;
    return has_arm64 != 0;
  }();
  return value;
}

static int spawnAndWait(
    const char *const argv[], std::string *output_text,
    int capture_fd = STDOUT_FILENO) {
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa))
    return -1;

  int pipe_fd[2] = {-1, -1};
  bool ok = true;
  if (output_text) {
    ok = pipe(pipe_fd) == 0 &&
         !posix_spawn_file_actions_adddup2(&fa, pipe_fd[1], capture_fd) &&
         !posix_spawn_file_actions_addclose(&fa, pipe_fd[0]) &&
         !posix_spawn_file_actions_addclose(&fa, pipe_fd[1]);
  } else {
    ok = !posix_spawn_file_actions_addopen(
        &fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  }
  int discard_fd = capture_fd == STDOUT_FILENO ? STDERR_FILENO : STDOUT_FILENO;
  ok = ok && !posix_spawn_file_actions_addopen(
                 &fa, discard_fd, "/dev/null", O_WRONLY, 0);

  /* Apple's tools ship universal, and a spawn inherits the parent's
   * architecture preference -- so a translated process spawns the x86_64 slice
   * of air-as and pays the translation a second time, for a tool that has a
   * native slice sitting right beside it. Measured on an M4: 26.1 ms per spawn
   * translated against 8.6 ms native, and a shader compiles one spawn at a
   * time. Ask for arm64; a binary without that slice simply fails to spawn,
   * which is why the plain spawn is still there to fall back on. */
  posix_spawnattr_t attr;
  bool have_attr = false;
  if (hostIsAppleSilicon() && posix_spawnattr_init(&attr) == 0) {
    cpu_type_t preferred = CPU_TYPE_ARM64;
    size_t copied = 0;
    if (posix_spawnattr_setbinpref_np(&attr, 1, &preferred, &copied) == 0 &&
        copied == 1)
      have_attr = true;
    else
      posix_spawnattr_destroy(&attr);
  }

  pid_t pid = -1;
  if (ok) {
    // dylib-safe spelling of environ: air-as/xcrun need the real environment
    // (DEVELOPER_DIR, TMPDIR, ...).
    int rc = posix_spawn(
        &pid, argv[0], &fa, have_attr ? &attr : nullptr,
        const_cast<char *const *>(argv), *_NSGetEnviron());
    if (rc != 0 && have_attr)
      rc = posix_spawn(
          &pid, argv[0], &fa, nullptr, const_cast<char *const *>(argv),
          *_NSGetEnviron());
    ok = rc == 0;
  }
  if (have_attr)
    posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  if (pipe_fd[1] >= 0)
    close(pipe_fd[1]);

  if (!ok) {
    if (pipe_fd[0] >= 0)
      close(pipe_fd[0]);
    return -1;
  }

  if (output_text) {
    char buf[1024];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0)
      output_text->append(buf, n);
    close(pipe_fd[0]);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Returns path to air-as from Apple's Metal toolchain. Result is cached by the
// single caller, so the lookup (which may spawn xcrun) runs at most once.
static std::string findAirAs() {
  // 1) Explicit override via env var.
  if (const char *env = getenv("DXMT_AIR_AS")) {
    if (env[0] && access(env, X_OK) == 0)
      return env;
  }

  // 2) Ask xcrun (works when DEVELOPER_DIR / xcode-select are sane).
  {
    const char *const argv[] = {"/usr/bin/xcrun", "--find", "air-as", nullptr};
    std::string out;
    if (spawnAndWait(argv, &out) == 0) {
      if (size_t nl = out.find('\n'); nl != std::string::npos)
        out.resize(nl);
      if (!out.empty() && access(out.c_str(), X_OK) == 0)
        return out;
    }
  }

  // 3) Glob the cryptex MetalToolchain mount directly. CrossOver's sandbox
  //    often lacks a usable xcrun, but the cryptex mount is still readable.
  {
    glob_t g = {};
    std::string found;
    if (glob(
            "/var/run/com.apple.security.cryptexd/mnt/"
            "com.apple.MobileAsset.MetalToolchain-*/Metal.xctoolchain/usr/bin/"
            "air-as",
            0, nullptr, &g) == 0) {
      for (size_t i = 0; i < g.gl_pathc && found.empty(); i++) {
        if (access(g.gl_pathv[i], X_OK) == 0)
          found = g.gl_pathv[i];
      }
    }
    globfree(&g);
    if (!found.empty())
      return found;
  }

  return {};
}

// Compile LLVM IR text to AIR bitcode using Apple's air-as.
// Returns true and fills out_bc on success.
static bool compileWithAirAs(
    const llvm::Module &module,
    SmallVectorImpl<char> &out_bc, std::string *error) {

  static const std::string air_as = findAirAs();
  if (air_as.empty())
    return false;

  std::string ir_text, downgraded;
  {
    // An unnamed struct type prints as %N, spelled like a numbered value, and
    // the downgrade renames values by spelling: it took the type %0 for the
    // argument %0 it had cast. Named, the two cannot meet.
    // (Module::getIdentifiedStructTypes lists only the named ones.)
    llvm::TypeFinder types;
    types.run(module, false);
    for (llvm::StructType *type : types)
      if (!type->isLiteral() && !type->hasName())
        type->setName("dxmt.anon");

    // Print LLVM22-dialect textual IR, then downgrade it to the Apple
    // AIR LLVM dialect that air-as understands (opaque pointers, memory()
    // attributes, splat constants, typed function-ref metadata, ...).
    {
      raw_string_ostream ir_stream(ir_text);
      module.print(ir_stream, nullptr);
    }
    std::string derr;
    if (!dxmt::aircompat::DowngradeToAppleAIR(ir_text, downgraded, derr))
      return false;
  }

  // Write the IR to a unique temp file; derive the output name from it so both
  // are unique across concurrent shader compilations in this and other
  // processes.
  const char *tmpdir = getenv("TMPDIR");
  if (!tmpdir || !tmpdir[0])
    tmpdir = "/tmp";
  std::string ll_path = std::string(tmpdir) + "/dxmt_airconv_XXXXXX.ll";
  int fd = mkstemps(ll_path.data(), 3);
  if (fd < 0)
    return false;
  for (size_t off = 0; off < downgraded.size();) {
    ssize_t n = write(fd, downgraded.data() + off, downgraded.size() - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      unlink(ll_path.c_str());
      return false;
    }
    off += n;
  }
  close(fd);
  std::string bc_path = ll_path.substr(0, ll_path.size() - 3) + ".bc";

  // Run air-as to produce AIR-compatible bitcode. What it says goes back to
  // the caller when it refuses the module: its first diagnostic names the
  // line of IR it could not take.
  const char *const argv[] = {
      air_as.c_str(), "-o", bc_path.c_str(), ll_path.c_str(), nullptr};
  std::string diagnostics;
  int status = spawnAndWait(argv, &diagnostics, STDERR_FILENO);
  unlink(ll_path.c_str());
  if (status != 0) {
    unlink(bc_path.c_str());
    if (error) {
      auto line = diagnostics.find("error:");
      if (line == std::string::npos)
        line = 0;
      *error = diagnostics.substr(line, diagnostics.find('\n', line) - line);
    }
    return false;
  }

  // Read result.
  auto buf = MemoryBuffer::getFile(bc_path);
  unlink(bc_path.c_str());
  if (!buf)
    return false;

  auto &mb = *buf.get();
  out_bc.assign(mb.getBufferStart(), mb.getBufferEnd());
  return true;
}

#else // !__APPLE__

// air-as only exists on macOS; without it there is no way to produce bitcode
// that Apple's AGX compiler accepts from LLVM 22 output.
static bool compileWithAirAs(const llvm::Module &, SmallVectorImpl<char> &, std::string *) {
  return false;
}

#endif

struct InputAttribute {
  uint8_t attribute;
  std::string name;
  uint8_t type;
  // TODO: extend patch/control point here
};

bool MetallibWriter::Write(const llvm::Module &module, raw_ostream &OS, std::string *error) {

  SmallVector<char, 0> bitcode;
  SmallVector<char, 0> public_metadata;
  SmallVector<char, 0> private_metadata;
  SmallVector<char, 0> function_def;

  uint32_t fn_count = 0;

  // Version fields must match what the AIR module actually declares, otherwise
  // Apple's loader/AGX backend can reject the package.
  uint8_t airMaj = 2, airMin = 8, langMaj = 4, langMin = 0;
  uint8_t osMaj = 26, osMin = 0;
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

  // Produce AIR-compatible bitcode via Apple's air-as. There is no fallback:
  // LLVM 22 binary bitcode fed directly to Apple's Metal AGX reader is
  // rejected in the best case ("Failed to upgrade function bitcode") and
  // crashes MTLCompilerService (XPC_ERROR_CONNECTION_INTERRUPTED) in the
  // worst, so emitting it would poison the whole device. Failing here lets
  // the caller surface a clean shader-compilation error instead.
  if (!compileWithAirAs(module, bitcode, error))
    return false;

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
  return true;
}

} // namespace dxmt::metallib
