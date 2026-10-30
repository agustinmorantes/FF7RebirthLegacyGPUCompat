# Legacy GPU Compatibility for FF7 Rebirth

Run FINAL FANTASY VII REBIRTH (PC, Steam) on graphics cards without mesh shaders, such as the
NVIDIA GTX 10 series. The mod replaces mesh-shader work with compute and vertex shaders and
handles the game's graphics capability checks.

**Experimental, tested on a GTX 1070.** The author reports extended in-game use without issues
on that setup. Other GPUs remain untested.

## Start here

- [Installation](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Installation): download, first start, updates and uninstall.
- [Compatibility](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Compatibility): GPUs, optional mods and known limitations.
- [Troubleshooting](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Troubleshooting): installation checks, conversion errors and bug reports.
- [Build from source](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/wiki/Build-from-source): prerequisites, tests, conversion tools and release packaging.

The release includes the conversion tools. It converts shaders from your own installed copy
of the game on first start and after game updates; no game shaders are included in the download.

## Project links and credits

- [Downloads](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/releases)
- [Source and README](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat)
- [Technical explanation](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/blob/main/docs/how-it-works.md)
- [Bug reports](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/issues)

Thanks to frostbone25 and the [ShaderInjector contributors](https://github.com/frostbone25/ShaderInjector).
Their open-source project served as a reference for hooking D3D12 shader creation.

The project code is [MIT licensed](https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat/blob/main/LICENSE).
Bundled tools have their own license files. Unofficial fan project, not affiliated with or
endorsed by Square Enix.
