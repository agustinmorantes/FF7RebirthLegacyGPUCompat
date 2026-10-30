# Troubleshooting

## The game does not start, or still reports that DX12 is unsupported

1. Check that you downloaded the release asset and extracted it as shown in
   [Installation](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Installation).
2. Confirm `d3d12.dll`, `legacygpu.ini` and `legacygpu_tools` are directly in
   `End\Binaries\Win64`, rather than inside an extra folder.
3. Check that `d3d12.dll` belongs to this mod. Another feature-level spoof cannot use the same
   filename alongside it.
4. Test with other mods removed. If the problem remains, include `legacygpu.log` in a bug report.

Other GPUs without mesh shaders may still have unsupported features. See
[Compatibility](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Compatibility).

## Conversion window or startup delay

First-start conversion can take a few minutes. The window only appears after six seconds;
a quick check can complete without displaying it. Successful unchanged starts skip conversion.
The game's own shader compilation is a separate step afterward.

If conversion fails, the warning points to `legacygpu.log`. The game may continue with missing
geometry, and conversion is retried on the next start. Check that `legacygpu_tools` is complete
and include the log in your report. Avoid treating every skipped-shader message as a failure:
a small number of shaders are known limitations.

## Missing or flickering geometry

Keep `EmulateMeshShaders=1` in `legacygpu.ini`; setting it to 0 drops mesh draws. Leave the
other defaults in place while troubleshooting. Record the area, camera position and action
that reproduce the problem. Brief missing geometry after a sudden demand increase is a known
limitation; persistent missing geometry needs a report.

## Problems with optional mods

Test this mod alone, then reintroduce optional mods one at a time. Follow the upstream guides
linked from [Compatibility](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Compatibility).
XeFG availability depends on the GPU and OptiScaler setup.

## Reporting a bug

Open an [issue](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/issues) with:

- GPU, driver version, Windows version, game build and mod version.
- Game settings and a clear location or sequence of steps that reproduces the issue.
- Whether it happens with other mods removed; list any mods used when it occurs.
- `End\Binaries\Win64\legacygpu.log`, plus a screenshot or video when useful.

Diagnostics options are off by default and can affect performance. Enable them only for a
specific investigation; normal reports should start with the default configuration.
