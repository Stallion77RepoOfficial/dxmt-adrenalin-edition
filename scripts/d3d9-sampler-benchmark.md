# D3D9 sampler performance check

Measured on 27 October 2026 on an Apple M4 with 16 GB memory, using Goose HUB's
native ARM64 Wine and FEX to run the 32-bit test executable.

`D3D9Device::BindTexturesAndSamplers` previously constructed sampler descriptions
and searched the sampler-object map for all 20 slots on every draw. It now keeps
the resolved sampler for each slot and resolves it again when its sampler state
or depth-comparison mode changes. Texture and sampler bindings are still sent to
the D3D11 context, including after internal `StretchRect` operations.

## Controlled before/after result

The baseline was rebuilt from the local sources immediately before this change,
with the same compiler, flags and dependencies as the changed DLL. This comparison
does not use the repository's older Git HEAD as the baseline.

Each version ran four times in interleaved order. Each run submitted 60,000 draws
per workload, in batches of 512. Shader compilation and GPU readback are outside
the measured submission interval. Readback drains each batch and the test checks
actual pixel colours before measuring. Pipeline compilation is synchronous, so
an uncompiled pipeline cannot improve the result by skipping draws. `DXMT_FPS`
and the Metal HUD are disabled during these measurements.

| Sampler workload | Before, median µs/draw | After, median µs/draw | CPU time reduction |
| --- | ---: | ---: | ---: |
| Unchanged state | 2.755 | 1.695 | 38.5% |
| Slot 0 changes every 64 draws | 2.711 | 1.768 | 34.8% |
| Slot 0 changes every draw | 2.735 | 1.911 | 30.1% |

These are CPU submission measurements for this small D3D9 workload. They are not
CoD4 FPS measurements or a CrossOver comparison. Game logic, translation of the
game itself, GPU work, frame pacing, and different graphics backends can dominate
a real game's frame time.

Baseline DLL SHA-256, after removing Wine's builtin marker:
`427502ce143d65ebf9c430b6155fbe6946357e3e7d99486b6e51a08bb1334985`.
Changed DLL SHA-256:
`4c7eb9b7a2fdf5ce4c1913754263c519c1a40f29ef9579a51d0f80f3d2003a72`.

The raw measurements and per-run logs are in
`GooseHUB/Resources/Validation/2026-10-27/`. The table comes from
`sampler-comparison.json` there. The old test binaries and temporary game/prefix
copies were removed when the current build was packaged.

## Validation and reproduction

`d3d9-sampler-benchmark.c` verifies 13 pixel results: point/linear filtering,
restoring filtering, state-block application, sRGB decoding, changing between
colour and comparison textures without changing sampler state, bindings after
`StretchRect`, and vertex texture sampling. The blit uses default-pool surfaces
outside a BeginScene/EndScene pair. These checks passed for every measured run.
Release D3D9 DLLs compiled for x86, x64, and ARM64EC. Both x86 and 64-bit guests
passed the packaged-runtime checks, with the latter using the ARM64EC D3D9 DLL.

Build a console executable from the repository root:

```sh
i686-w64-mingw32-gcc -O2 -Wall -Wextra scripts/d3d9-sampler-benchmark.c \
  -o sampler-benchmark-x86.exe -ld3d9 -ld3dcompiler -luser32
```

Use `x86_64-w64-mingw32-gcc` to build the 64-bit guest version. Run it with an
isolated Wine prefix containing the DXMT DLL set and a matching `winemetal`
bridge. Use the packaged DLLs: raw build outputs may have a `Wine builtin DLL`
marker that causes Wine to substitute its own DLLs. For this test, set
`DXMT_CONFIG=d3d11.asyncPipelineCompilation=False`, `DXMT_FPS=0`,
`MTL_HUD_ENABLED=0`, and `WINEMSYNC=1`. Pass the desired draw count as the first
argument. Each `BENCH` output line contains a JSON measurement.

## CoD4 and CrossOver

The supplied `iw3sp.exe` is a 32-bit executable. The ARM64EC DLL path used by
64-bit games does not make its x86 D3D9 renderer native; the x86 path needs its own
measurements. MSync was active in the runtime logs. The old Goose HUB CoD4 session
contained 308,908 `OMSetRenderTargets: invalid render targets` warnings. A local
render-target compatibility fix was already present before this sampler change,
but the existing app packages did not contain the same DXMT DLLs as the current
release build. The new `Goose HUB performance.app` packages the current builds.
Its signature verification succeeded and its CLI reports Wine, DXMT, FEX,
Vulkan and MetalGoose available, with no inventory issues.

An isolated copy of the supplied game reached `bog_a` using the packaged runtime,
at 1280×720, 4× antialiasing, windowed, VSync off and no frame cap. The roughly
one-minute run logged 56 groups of 60 frames, including loading/menu frames and
scene draws, with zero invalid-render-target warnings. Two WineD3D OpenGL
`glClear` errors occurred during startup. This was a smoke test with diagnostic
timing enabled, not a repeatable game FPS benchmark. The user's original game
files, profiles and bottles were not edited.

### CrossOver CPU comparison

CrossOver 26.3 was tested in a separate `CoD4Perf` bottle under the local build
directory, with MSync and the same x86 benchmark executable. Its WineD3D backend
passed all 13 pixel checks. Four repeated runs gave these medians:

| Workload | Goose HUB after change, µs/draw | CrossOver WineD3D, µs/draw |
| --- | ---: | ---: |
| Unchanged state | 1.695 | 0.307 |
| Slot 0 changes every 64 draws | 1.768 | 0.306 |
| Slot 0 changes every draw | 1.911 | 0.400 |

There is still a substantial CPU submission gap in this x86 workload. These
different graphics backends do not have the same GPU costs, so these numbers
cannot establish a CoD4 frame-rate ratio. The DXMT native ARM64EC path also passed
the pixel checks; the 32-bit renderer preparation path and translation overhead
are the next profiling targets. FEX's local WOW64 and ARM64EC builds already use
Release `-O3 -DNDEBUG` with assertions disabled, so this is not explained by a
debug FEX build. Wine's native build uses `-O2` and the tested DXMT uses `-O3`.

The CrossOver DXVK attempt failed the first pixel check because MoltenVK could
not compile a fragment shader with overlapping texture/sampler bindings. It
therefore produced no valid performance measurement. CrossOver's existing Steam
bottle was not used or changed.

For a CrossOver comparison, use the same CoD4 build, scene or recorded demo,
resolution, antialiasing, texture settings and frame cap, with VSync and
MetalGoose disabled. Warm shader caches before collecting repeated frame-time
runs and record CrossOver's selected backend. CrossOver itself offers DXMT,
D3DMetal, DXVK and Wine graphics options; MSync's benefit depends on the
application ([CodeWeavers settings documentation](https://support.codeweavers.com/en_US/advanced-settings-in-crossover-mac-26)).
