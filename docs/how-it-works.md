# How it works

## What stops the game on GPUs without mesh shaders

Measured on a GTX 1070 (Pascal). Other cards without mesh shaders hit at least the first two.

1. **Feature level.** The game's adapter probe creates the device at `D3D_FEATURE_LEVEL_12_2`
   and rejects the adapter when that fails ("DX12 is not supported on your system"). Pascal tops
   out at 12_1.
2. **Mesh shaders.** The game's renderer needs `MeshShaderTier` 1; with 0 it looks for shaders
   that aren't shipped. The shader library has about 4,100 mesh shaders: the open-world meshlet
   renderer (prepass, base pass and shadow variants), materials (particles, static meshes) and
   fullscreen passes. Pascal has no mesh shaders.
3. **Native 16-bit ops.** Eleven global compute shaders need native 16-bit arithmetic, which
   Pascal doesn't report.

Everything else the game uses (SM 6.6, wave ops, DXR 1.0, resource binding tier 3) is available
on Pascal.

## The proxy

`d3d12.dll` sits in `End\Binaries\Win64`, where the game's delay-loaded import finds it first. It
forwards every export to `System32\d3d12.dll`, which still honours the game's Agility SDK exports.
It then:

- creates the device at 12_1 when 12_2 is requested, and answers feature-level queries that
  include 12_2 with 12_2;
- reports `MeshShaderTier` 1;
- replaces shader bytecode by hash from `legacygpu_shaders\<hash>.cso` (the 16-bit fixes);
- turns each mesh-shader PSO into a vertex + pixel shader PSO built from the converted pair, plus
  a compute PSO;
- emulates each mesh dispatch (direct or `ExecuteIndirect`) as described below;
- hands the game a dummy object when a PSO still can't be created, and drops the work recorded
  with it, rather than crashing.

The feature-level spoof, the mesh shader tier and the dummy PSOs are always on: the game doesn't
start or crashes without them. `EmulateMeshShaders=0` drops mesh draws instead of emulating them,
which is useful only to measure what emulation costs.

## Shader conversion

The shader library is a closed, cooked set, so every shader is converted ahead of time and looked
up by its DXIL container hash. No compiler runs while the game renders. Releases ship no converted
shaders. Instead, before the device hooks load the shaders, `proxy/src/generate.cpp` checks
the size and write time of the pak and the converter scripts against `legacygpu_shaders\stamp.txt`.
On a mismatch, which happens on the first start and after a game update, it runs the bundled
`legacygpu_tools\build_shaders.py` with the bundled embeddable Python behind a progress window and
waits for it. This takes about 1.5 minutes on 12 threads for the full set. `build_shaders.py` keeps
the outputs it already has, deletes ones the game no longer uses, and rebuilds everything when the
converters' content hash (`converter.txt`) changes. It reads the SM6 shader archives from
`pakchunk0-WindowsNoEditor.pak` and runs:

- `tools/ms2cs.py` on every mesh shader. It edits the DXIL IR (from `dxc -dumpbin`) into a compute
  shader with the same body that writes vertices and indices to a scratch UAV instead of the mesh
  outputs, and generates an HLSL vertex shader that fetches those vertices back by `SV_VertexID`
  with the mesh shader's output signature. `tools/dxilasm` reassembles and signs the edited IR.
- `tools/fix16.py` on 16-bit compute shaders. Shaders whose only 16-bit use is `Buffer<half>`
  typed loads are rewritten to load float, which is identical because typed loads convert in
  hardware.

## Emulating a mesh dispatch

Each emulated call records:

1. The converted compute shader, dispatched with the mesh dispatch's group counts (for
   `ExecuteIndirect`, through a compute command signature with the same root constants).
2. Each group reserves its vertices and indices with one atomic add per counter, so output is
   compacted and the index count lands directly in `DRAW_INDEXED` arguments in the scratch header.
3. An indirect indexed draw of that output with the vertex + pixel shader PSO.

The vertex region stores records in blocks of 32, so the threads of a warp write contiguous,
aligned memory. Culled primitives (`SV_CullPrimitive`) are compacted away in the compute pass.
Other per-primitive attributes (`PRIMITIVEID`) go to a per-triangle record the vertex shader reads.
Vertex attributes proven to come from half-precision data are stored two per 32-bit word.

**Per-call commands.** The scratch rests in a read state (index buffer, shader resource, indirect
argument, copy source) between calls. Each pass is three transitions: read → copy dest, a copy of
the 128-byte header, → UAV, the dispatch, UAV → read, then the draw. On direct lists the scratch
starts from `COMMON` at the list's first emulated call and returns to `COMMON` at `Close`, so lists
can run in any order. With `OptimizeMeshRootBindings=1` the emulation's compute root signature and
arguments stay bound between calls; the game's own compute state is restored only before a
command that uses it (native dispatch or `ExecuteIndirect`, ray dispatch, compute root setters,
descriptor heap changes, bundles and meta commands).

**Scratch sizing.** The scratch buffer starts at `MeshScratchMinMiB`. Every call's actual demand is
read back once the GPU has finished it. The scratch grows when a call needs more than 70% of a
region and shrinks after `MeshScratchShrinkSeconds` of lower demand, up to `MeshScratchMaxMiB`.
Groups that don't fit are queued for another pass of the same call; a PSO that keeps overflowing
gets more passes recorded up front. Until feedback catches up, the first oversized call can lose
some geometry for a frame.

## Diagnostics

All off by default; they only add log output and, where noted, GPU queries.

- `MeshUsageDiagnostics`: scratch size, emulated calls, peak demand and resizes in the log, plus a
  dump per emulation PSO.
- `MemoryDiagnostics`: GPU memory usage and budget.
- `MeshPerformanceDiagnostics`: fence-verified GPU timestamps of sampled emulated calls
  (`MESHPROF`).
- `PassPerformanceDiagnostics`: every N seconds, GPU timestamps per render pass for about two
  frames (`PASSPROF`), with compute spans labelled by shader.
- `PassResourceDiagnostics`: with the pass profiler on, F12 captures one window that also logs
  render targets and barriers and dumps textures.

## Tried and removed

These were built, measured on the GTX 1070 and removed because they didn't pay for their
complexity. The game is GPU-bound in large emulated calls (prepass, shadow and base pass), not in
per-call overhead, so per-call savings don't show up in frame rate.

- **Draw batching** (`OptimizeMeshBatching`): ran compatible small calls' dispatches back to back
  in disjoint scratch partitions and drew them afterwards. In gameplay only about 100 calls run per
  frame and batches averaged 2 calls. A PresentMon A/B (2026-10-15) showed no FPS gain (36.8 on,
  36.5 off), and the partitions could lose geometry on a demand spike.
- **Header ring** (`OptimizeMeshHeaderRing`): 63 pre-reset header slots, so single-pass calls
  skipped the per-call header copy and one transition. Worth about 1 µs per call in a synthetic
  test; no gameplay difference between v5 (no ring) and v6 (ring and batching).
- **Barrier option** (`OptimizeMeshBarriers`): the three-transition path above is now the only one;
  the original four-transition path was removed.
- **Options for the always-on fixes** (`SpoofFeatureLevel`, `MeshShaderTier`, `SkipFailedPSOs`):
  the game can't run with any of them off.
- **Unused vertex output stripping and retry-pass scheduling** were evaluated and dropped before
  implementation: pixel shaders consume the outputs, and gameplay calls fit the scratch in one
  pass.

## Limitations

- Four debug mesh shaders with line topology aren't converted.
- Seven `ReflectionEnvironmentCS` compute shaders do real half arithmetic and aren't converted.
  Their PSOs are skipped.
- Rasterization order across groups is approximate, not the strict order of real mesh shaders.
- The game is GPU-bound under emulation. On a GTX 1070, emulation costs about a quarter of GPU
  time; disabling it (`EmulateMeshShaders=0`, which drops the geometry) runs about 35% faster,
  which bounds what further optimization can recover.
- A game update changes shader hashes. The proxy converts the changed shaders on the next start.
  An update that changes how the engine uses mesh shaders could still need converter changes.
